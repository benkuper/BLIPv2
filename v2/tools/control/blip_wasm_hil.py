#!/usr/bin/env python3
"""Exercise the production script worker without changing the PC network."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import threading
import time
import urllib.request
import zlib

import blip_serial_control as wire


class Client:
    """Keep the serial port open so bursts and active cancellation are observable."""
    def __init__(self, port: str):
        import serial
        self.connection = serial.Serial()
        self.connection.port = port
        self.connection.baudrate = 115200
        self.connection.timeout = 0.002
        self.connection.write_timeout = 2
        self.connection.dtr = self.connection.rts = False
        self.connection.open()
        quiet = time.monotonic()
        deadline = quiet + 3
        while time.monotonic() < deadline:
            if self.connection.read(128):
                quiet = time.monotonic()
            elif time.monotonic() - quiet >= 0.5:
                break
        self.connection.reset_input_buffer()
        self.next_id = 0
        self.pending = {}
        self.frame = bytearray()
        self.discarding = False

    def send(self, operation, component, name, *values):
        self.next_id += 1
        scalars = [wire.Scalar(wire.VALUE_STRING if isinstance(value, str) else
                   wire.VALUE_BOOLEAN if isinstance(value, bool) else wire.VALUE_INTEGER, value)
                   for value in values]
        payload = wire.encode_control(wire.OPERATION_BY_NAME[operation], component, name, scalars)
        self.connection.write(b"\0" + wire.cobs_encode(
            wire.encode_envelope(wire.KIND_REQUEST, self.next_id, payload)))
        return self.next_id

    def receive(self, request, timeout=2):
        deadline = time.monotonic() + timeout
        while request not in self.pending and time.monotonic() < deadline:
            for byte in self.connection.read(max(1, self.connection.in_waiting)):
                if byte:
                    if not self.discarding:
                        self.frame.append(byte)
                        if len(self.frame) > wire.MAX_SERIAL_FRAME_BYTES:
                            self.frame.clear()
                            self.discarding = True
                    continue
                if not self.discarding and self.frame:
                    try:
                        kind, identity, payload = wire.decode_envelope(wire.cobs_decode(bytes(self.frame)))
                        response = wire.decode_control(payload)
                        if kind in (wire.KIND_RESPONSE, wire.KIND_ERROR):
                            self.pending[identity] = response
                    except wire.ProtocolError:
                        pass  # Bounded console-text candidate between binary frames.
                self.frame.clear()
                self.discarding = False
        if request not in self.pending:
            raise TimeoutError(f"serial request {request} timed out")
        return self.pending.pop(request)

    def request(self, operation, component, name, *values):
        deadline = time.monotonic() + 2
        while True:
            response = self.receive(self.send(operation, component, name, *values))
            if not (operation == "get" or name in ("completion", "result")) or \
                    response["error_code"] != "resource_unavailable" or time.monotonic() >= deadline:
                break
            time.sleep(0.002)
        if response["error_code"] != "none":
            raise wire.ProtocolError(str(response), response)
        return [value["value"] for value in response["values"]]

    def get(self, name, component="blip.wasm"):
        return self.request("get", component, name)[0]

    def action(self, name, *values):
        return self.request("action", "blip.wasm", name, *values)

    def completion(self, token):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            result = self.action("completion", token)
            if result[0]:
                return {"error": wire.ERROR_CODE_NAME[result[1]], "count": result[2], "elapsed_us": result[3]}
        raise TimeoutError(f"worker token {token} timed out")

    def work(self, name, *values):
        return self.completion(self.action(name, *values)[0])

    def upload(self, data, crc=None):
        results = [self.work("upload_begin", len(data), zlib.crc32(data) if crc is None else crc)]
        for offset in range(0, len(data), 64):
            results.append(self.work("upload_chunk", offset, data[offset:offset + 64].hex()))
        if any(result["error"] != "none" for result in results):
            raise AssertionError(f"upload staging failed: {results}")
        return self.work("upload_commit")


def fixtures(root):
    text = (root / "v2/qualification/wasm-service/main/fixtures.hpp").read_text(encoding="utf-8")
    arrays = re.findall(r"std::array<std::byte, \d+> (\w+)\{\{(.*?)\}\};", text, re.S)
    return {name: bytes(int(value, 16) for value in re.findall(r"std::byte\{0x([0-9a-f]{2})\}", body))
            for name, body in arrays}


def run(client, modules, report, cycles):
    checks = report["checks"]

    def check(name, passed, **details):
        checks.append({"name": name, "passed": bool(passed), **details})
        if not passed:
            raise AssertionError(f"{name}: {details}")

    def load():
        result = client.upload(modules["workloads"])
        check("valid-load", result["error"] == "none" and client.get("state") == "loaded", **result)

    client.action("cancel_all")
    check("initial-unload", client.work("unload")["error"] == "none")
    check("worker-ready", client.get("state") == "ready")
    load()
    token = client.action("call_i32", "echo", 0xffffffff)[0]
    result = client.completion(token)
    bits = client.action("result", token, 0)
    check("i32-result-bits", result["error"] == "none" and bits == [0, 0xffffffff, 0], bits=bits, **result)
    for name, action, arguments, expected in (
        ("unreachable", "call0", ("trap",), "verification_failed"),
        ("out-of-bounds", "call0", ("invalid",), "verification_failed"),
        ("divide-zero", "call_i32", ("divide", 0), "verification_failed"),
        ("recursive-stack", "call0", ("recursion",), None),
        ("instruction-fuel", "call0", ("spin",), "budget_exceeded"),
    ):
        result = client.work(action, *arguments)
        state = client.get("state")
        check(name, result["error"] != "none" and (expected is None or result["error"] == expected)
              and result["count"] == 0 and state == "faulted", state=state, **result)
        load()
    for name in ("start_loop", "post_loop", "ctor_loop", "initialize_loop", "imported", "large_memory"):
        result = client.upload(modules[name])
        check(f"reject-{name}", result["error"] != "none" and client.get("state") == "ready", **result)
        load()
    result = client.upload(modules["workloads"], 0)
    check("crc-rejection", result["error"] == "verification_failed" and client.get("state") == "ready", **result)
    load()
    client.request("set", "blip.wasm", "instruction_budget", 1000000)
    client.request("set", "blip.wasm", "deadline_ms", 5)
    result = client.work("call0", "spin")
    check("wall-clock-deadline", result["error"] == "budget_exceeded" and result["count"] == 0
          and client.get("deadlines") > 0, **result)
    load()
    client.request("set", "blip.wasm", "deadline_ms", 50)
    call = client.send("action", "blip.wasm", "call0", "spin")
    # Let the worker enter guest code, then cancel through the same live port.
    response = client.receive(call)
    check("cancel-call-admitted", response["error_code"] == "none")
    token = response["values"][0]["value"]
    time.sleep(0.005)
    client.action("cancel_all")
    result = client.completion(token)
    check("active-cancel", result["error"] == "cancelled" and result["count"] == 0, **result)
    load()
    requests = [client.send("action", "blip.wasm", "call0", "spin") for _ in range(12)]
    cancel = client.send("action", "blip.wasm", "cancel_all")
    responses = [client.receive(request) for request in requests]
    accepted = [response["values"][0]["value"] for response in responses if response["error_code"] == "none"]
    rejected = [response["error_code"] for response in responses if response["error_code"] != "none"]
    check("queue-reject-new", accepted and "queue_full" in rejected and
          len(accepted) == len(set(accepted)) and client.receive(cancel)["error_code"] == "none",
          accepted=accepted, rejected=rejected)
    results = [client.completion(token) for token in accepted]
    check("queued-cancellation", any(result["error"] == "cancelled" for result in results)
          and all(result["count"] == 0 for result in results), results=results)
    load()
    running = client.action("call0", "spin")[0]
    time.sleep(0.005)
    barrier = client.action("unload")[0]
    stale = client.action("call_i32", "echo", 123)[0]
    results = [client.completion(token) for token in (running, barrier, stale)]
    check("stale-module-generation", [result["error"] for result in results] ==
          ["budget_exceeded", "none", "cancelled"], results=results)
    data = modules["workloads"]
    check("partial-upload-begin", client.work("upload_begin", len(data), zlib.crc32(data))["error"] == "none")
    check("partial-upload-chunk", client.work("upload_chunk", 0, data[:64].hex())["error"] == "none")
    client.action("cancel_all")
    deadline = time.monotonic() + 0.2
    while client.get("upload_received") and time.monotonic() < deadline:
        time.sleep(0.005)
    check("cancel-upload-snapshot", client.get("upload_received") == 0)
    check("cancel-upload-commit", client.work("upload_commit")["error"] == "invalid_state")
    load()
    client.request("set", "blip.wasm", "instruction_budget", 10000)
    client.request("set", "blip.wasm", "deadline_ms", 10)
    old = client.action("call_i32", "echo", 1)[0]
    check("expiry-baseline", client.completion(old)["error"] == "none")
    for value in range(17):
        check("completion-ring-call", client.work("call_i32", "echo", value)["error"] == "none")
    response = client.receive(client.send("action", "blip.wasm", "completion", old))
    check("completion-expired", response["error_code"] == "not_found", response=response["detail"])
    check("cycle-warmup", client.work("unload")["error"] == "none")
    before = client.get("heap_free_internal", "blip.diagnostics")
    for _ in range(cycles):
        load()
        check("cycle-call", client.work("call_i32", "echo", 42)["error"] == "none")
        check("cycle-unload", client.work("unload")["error"] == "none" and client.get("pool_used") == 0)
    after = client.get("heap_free_internal", "blip.diagnostics")
    check("non-growing-load-cycles", after >= before, before=before, after=after, cycles=cycles)
    report["metrics"] = {name: client.get(name) for name in (
        "state", "submitted", "completed", "failed", "rejected", "deadlines", "cancelled",
        "pool_reserved", "pool_used", "pool_peak", "worker_stack_headroom", "supervisor_stack_headroom")}
    report["heap"] = {name: client.get(name, "blip.diagnostics") for name in (
        "heap_free_internal", "heap_minimum_internal", "heap_largest_internal")}


def coexist(client, modules, report, ip, pixels):
    stop = threading.Event()
    network = {"ddp_sent": 0, "ddp_failures": [], "http_ok": 0, "http_failures": [], "http_max_ms": 0}

    def ddp():
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
            sequence = 0
            while not stop.is_set():
                sequence = sequence % 15 + 1
                rgb = bytes((4, 0, 0)) * pixels
                packet = bytes((0x41, sequence, 0x0b, 1)) + bytes(4) + len(rgb).to_bytes(2, "big") + rgb
                try:
                    connection.sendto(packet, (ip, 4048))
                    network["ddp_sent"] += 1
                except OSError as error:
                    network["ddp_failures"].append(str(error))
                    return
                stop.wait(1 / 60)

    def http():
        # Bypass proxy settings for this explicit local device address.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        while not stop.is_set():
            began = time.monotonic()
            try:
                with opener.open(f"http://{ip}/?HOST_INFO", timeout=2) as response:
                    info = json.loads(response.read(4096))
                    if response.status != 200 or "NAME" not in info:
                        raise ValueError("unexpected OSCQuery host info")
                network["http_ok"] += 1
                network["http_max_ms"] = max(network["http_max_ms"], (time.monotonic() - began) * 1000)
            except Exception as error:
                network["http_failures"].append(str(error))
            stop.wait(0.05)

    def metrics():
        return {"led_frames": client.get("applied_frames", "blip.output.strip0"),
                "led_failures": client.get("failed_frames", "blip.output.strip0"),
                "ddp_accepted": client.get("accepted_packets", "blip.input.ddp"),
                "ddp_output_rejections": client.get("output_rejections", "blip.input.ddp")}

    before = metrics()
    original_red = client.get("red", "blip.output.strip0")
    threads = [threading.Thread(target=ddp), threading.Thread(target=http)]
    for thread in threads:
        thread.start()
    calls = []
    settings_writes = 0
    try:
        client.request("set", "blip.wasm", "instruction_budget", 1000000)
        client.request("set", "blip.wasm", "deadline_ms", 50)
        for index in range(20):
            loaded = client.upload(modules["workloads"])
            if loaded["error"] != "none":
                raise AssertionError(f"coexistence load failed: {loaded}")
            token = client.action("call0", "spin")[0]
            # These control and persisted settings operations execute while
            # guest work is active; a higher-priority supervisor still cancels it.
            probe = client.get("probe_value", "blip.bootstrap")
            client.request("set", "blip.output.strip0", "red", 4 + index % 2)
            settings_writes += 1
            result = client.completion(token)
            result["probe"] = probe
            calls.append(result)
            if result["error"] != "budget_exceeded" or result["count"] != 0:
                raise AssertionError(f"coexistence deadline failed: {result}")
        client.request("set", "blip.output.strip0", "red", original_red)
        if client.get("red", "blip.output.strip0") != original_red:
            raise AssertionError("persisted settings control did not recover")
        if client.work("unload")["error"] != "none":
            raise AssertionError("coexistence unload failed")
        time.sleep(0.1)
    finally:
        stop.set()
        for thread in threads:
            thread.join(timeout=3)
    after = metrics()
    passed = after["led_frames"] > before["led_frames"] and \
        after["led_failures"] == before["led_failures"] and \
        after["ddp_accepted"] > before["ddp_accepted"] and \
        after["ddp_output_rejections"] == before["ddp_output_rejections"] and \
        network["http_ok"] > 0 and not network["http_failures"] and not network["ddp_failures"]
    report["coexistence"] = {"passed": passed, "ip": ip, "pixels": pixels, "ddp_rate_hz": 60,
        "before": before, "after": after, "network": network, "script_calls": calls,
        "settings_writes": settings_writes, "settings_restored": True,
        "worker_state_after": client.get("state"),
        "worker_stack_headroom_after": client.get("worker_stack_headroom"),
        "supervisor_stack_headroom_after": client.get("supervisor_stack_headroom"),
        "heap_free_after": client.get("heap_free_internal", "blip.diagnostics"),
        "heap_minimum": client.get("heap_minimum_internal", "blip.diagnostics")}
    report["checks"].append({"name": "led-network-settings-coexistence", "passed": passed})
    if not passed:
        raise AssertionError("LED/network/settings coexistence counters failed; inspect the report")


def source_snapshot(root):
    paths = [path for path in (root / "v2/components/blip_wasm").rglob("*")
             if path.is_file() and (path.suffix in (".c", ".cpp", ".hpp") or path.name == "CMakeLists.txt")]
    paths += [root / name for name in (
        "v2/firmware/CMakeLists.txt", "v2/firmware/main/CMakeLists.txt", "v2/firmware/main/main.cpp",
        "v2/firmware/sdkconfig.wasm.defaults", "v2/tools/control/blip_wasm_hil.py",
        "v2/tools/control/blip_serial_control.py", "v2/qualification/wasm-service/main/fixtures.hpp",
        "v2/tools/control/blip_wasm_network_hil.ps1", "v2/tools/wifi/blip_wifi_guard.ps1",
        "v2/tools/wifi/blip_wifi_recovery.ps1")]
    files = {path.relative_to(root).as_posix(): hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")).hexdigest() for path in sorted(paths)}
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    return {"sha256": hashlib.sha256(encoded).hexdigest(), "files": files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--ip", help="test HTTP and DDP on an already reachable device; PC Wi-Fi is unchanged")
    parser.add_argument("--pixels", type=int, default=36)
    args = parser.parse_args()
    if args.cycles < 1 or not 1 <= args.pixels <= 512:
        parser.error("cycles must be positive and pixels must be between 1 and 512")
    root = Path(__file__).resolve().parents[3]
    modules = fixtures(root)
    report = {"schema_version": 1, "board": args.board, "port": args.port, "passed": False,
              "checks": [], "fixture_sha256": hashlib.sha256(modules["workloads"]).hexdigest(),
              "source_snapshot": source_snapshot(root),
              "artifacts": {name: hashlib.sha256((args.build / name).read_bytes()).hexdigest()
                            for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}}
    client = None
    try:
        client = Client(args.port)
        run(client, modules, report, args.cycles)
        if args.ip:
            coexist(client, modules, report, args.ip, args.pixels)
        report["passed"] = True
    except Exception as error:
        report["failure"] = str(error)
    finally:
        if client:
            client.connection.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]),
                      "failure": report.get("failure"), "output": str(args.output)}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
