"""Qualify live production script declarations without changing the PC network."""
import argparse
import hashlib
import json
from importlib.metadata import version
from pathlib import Path
import re
import struct
import time
import urllib.request

import wasmtime
from blip_wasm_hil import Client, source_snapshot

ROOT = Path(__file__).resolve().parents[3]


class SerialCapture:
    def __init__(self, connection, received): self.connection = connection; self.received = received
    def __getattr__(self, name): return getattr(self.connection, name)
    def read(self, count):
        data = self.connection.read(count); self.received.extend(data); return data


def u32(value):
    result = bytearray()
    while True:
        result.append((value & 127) | (128 if value > 127 else 0))
        value >>= 7
        if not value:
            return bytes(result)


def text(value):
    encoded = value.encode()
    return u32(len(encoded)) + encoded


def fixture(bad_signature=False, collision=False, loop=False):
    if version("wasmtime") != "36.0.0":
        raise RuntimeError("Fixture compilation requires host-only wasmtime==36.0.0")
    declaration = bytearray(b"BCM1" + u32(4))
    declaration += b"\0" + text("state" if collision else "level") + text("Script level")
    declaration += b"\1\2" + text("") + struct.pack("<q", 5) + b"\1" + struct.pack("<ddd", 0, 100, 1)
    declaration += b"\0" + text("note") + text("Script note") + b"\3\2" + text("") + text("hello") + b"\0"
    declaration += b"\1" + text("fire") + text("Run script action") + text("on_fire") + u32(4)
    for name, kind in (("armed", 0), ("count", 1), ("rate", 2), ("note", 3)):
        declaration += text(name) + bytes([kind])
    declaration += b"\2" + text("changed") + text("Script changed") + u32(1) + text("count") + b"\1"
    body = text("blip.controls.v1") + declaration
    callback = "unreachable" if bad_signature else """i32.const 0 local.get 0 i32.store
        i32.const 8 local.get 1 i64.store i32.const 16 local.get 2 f64.store
        i32.const 24 local.get 3 i32.store i32.const 28 local.get 4 i32.store"""
    if loop:
        callback = "(loop $again br $again)"
    signature = "" if bad_signature else "(param i32 i64 f64 i32 i32)"
    wat = f'''(module (memory 1 1)
      (global (export "blip_controls_buffer_v1") i32 (i32.const 1024))
      (func (export "on_fire") {signature} {callback})
      (func (export "armed") (result i32) i32.const 0 i32.load)
      (func (export "count") (result i64) i32.const 8 i64.load)
      (func (export "rate") (result f64) i32.const 16 f64.load)
      (func (export "length") (result i32) i32.const 28 i32.load)
      (func (export "text_word") (result i32) i32.const 24 i32.load i32.load)
      (func (export "trap") unreachable))'''
    module = bytes(wasmtime.wat2wasm(wat)) + b"\0" + u32(len(body)) + body
    wasmtime.Module.validate(wasmtime.Engine(), module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--flash-log", type=Path, required=True)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--cycles", type=int, default=20)
    args = parser.parse_args()
    raw_log = args.flash_log.read_bytes()
    log = raw_log.decode("utf-16" if raw_log.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
    macs = re.findall(r"MAC:\s*([0-9a-f:]+)", log, re.I)
    if not macs or macs[-1].lower() != args.mac.lower():
        parser.error("Flashed MAC differs from expected board")
    report = {"board": args.board, "port": args.port, "passed": False, "checks": [],
              "pc_network_changed": False, "production_script_owner_exercised": True,
              "source_snapshot": source_snapshot(ROOT),
              "checker_sha256": hashlib.sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
              "flashed_mac": args.mac.lower(), "flash_log_sha256": hashlib.sha256(raw_log).hexdigest(),
              "artifacts": {name: hashlib.sha256((args.build / name).read_bytes()).hexdigest()
                            for name in ("blip-v2.bin", "blip-v2.elf", "sdkconfig")}}
    client = None
    serial_received = bytearray()

    def check(name, condition, **details):
        report["checks"].append({"name": name, "passed": bool(condition), **details})
        if not condition:
            raise AssertionError(f"{name}: {details}")

    def result(name):
        token = client.action("call0", name)[0]
        done = client.completion(token)
        check("export-" + name, done["error"] == "none", completion=done)
        value = client.action("result", token, 0)
        return value[1] | (value[2] << 32)

    try:
        client = Client(args.port)
        client.connection = SerialCapture(client.connection, serial_received)
        client.action("cancel_all")
        check("initial-unload", client.work("unload")["error"] == "none")
        time.sleep(.05)
        check("empty-schema-has-no-text-reservation", client.get("control_text_reserved") == 0)
        valid = fixture()
        loaded = client.upload(valid)
        check("publish", loaded["error"] == "none", completion=loaded)
        check("schema-sized-text-reservation", client.get("control_text_reserved") == 640)
        check("default-value", client.get("level") == 5 and client.get("note") == "hello")
        client.request("set", "blip.wasm", "level", 42)
        client.request("set", "blip.wasm", "note", "owned note")
        check("dynamic-write-read", client.get("level") == 42 and client.get("note") == "owned note")
        # The generic client supports i64/string/boolean; encode the number
        # explicitly so every callback field is exercised over real serial.
        import blip_serial_control as wire
        client.next_id += 1
        scalars = [wire.Scalar(wire.VALUE_BOOLEAN, True), wire.Scalar(wire.VALUE_INTEGER, -9007199254740993),
                   wire.Scalar(wire.VALUE_NUMBER, 2.5), wire.Scalar(wire.VALUE_STRING, "copy")]
        payload = wire.encode_control(wire.OPERATION_BY_NAME["action"], "blip.wasm", "fire", scalars)
        client.connection.write(b"\0" + wire.cobs_encode(wire.encode_envelope(wire.KIND_REQUEST, client.next_id, payload)))
        response = client.receive(client.next_id)
        check("action-admitted", response["error_code"] == "none", response=response)
        done = client.completion(response["values"][0]["value"])
        check("action-completed", done["error"] == "none", completion=done)
        check("copied-typed-callback", result("armed") == 1 and result("count") == ((-9007199254740993) & ((1 << 64) - 1))
              and result("rate") == int.from_bytes(struct.pack("<d", 2.5), "little")
              and result("length") == 4 and result("text_word") == int.from_bytes(b"copy", "little"))
        ip = client.get("ip_address", "blip.transport.wifi")
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def tree_controls():
            with opener.open(f"http://{ip}/?config=1", timeout=8) as response:
                tree = json.load(response)
            def find(node):
                if node.get("BLIP_COMPONENT_ID") == "blip.wasm":
                    return node["CONTENTS"]
                for child in node.get("CONTENTS", {}).values():
                    found = find(child)
                    if found is not None:
                        return found
            return find(tree)

        controls = tree_controls()
        check("live-oscquery-schema", all(name in controls for name in ("level", "note", "fire", "changed"))
              and controls["level"]["VALUE"] == [42] and controls["note"]["VALUE"] == ["owned note"])
        check("unload", client.work("unload")["error"] == "none")
        check("retired-schema-hidden", "level" not in tree_controls())
        for name, module in (("bad-signature", fixture(bad_signature=True)), ("reserved-id", fixture(collision=True))):
            done = client.upload(module)
            check(name, done["error"] != "none" and client.get("state") == "ready", completion=done)
            check(name + "-hidden", "level" not in tree_controls())
        check("reload-after-failure", client.upload(valid)["error"] == "none")
        done = client.work("call0", "trap")
        check("fault", done["error"] != "none")
        check("fault-schema-hidden", "level" not in tree_controls())
        check("loop-load", client.upload(fixture(loop=True))["error"] == "none")
        client.request("set", "blip.wasm", "instruction_budget", 1000000)
        client.request("set", "blip.wasm", "deadline_ms", 50)
        sent = [client.send("action", "blip.wasm", "fire", True, -5, 2.0, "loop") for _ in range(8)]
        cancel = client.send("action", "blip.wasm", "cancel_all")
        admitted = []
        for request in sent:
            response = client.receive(request)
            if response["error_code"] == "none":
                admitted.append(response["values"][0]["value"])
            else:
                check("bounded-action-refusal", response["error_code"] in ("queue_full", "invalid_state", "not_found", "resource_unavailable"), response=response)
        check("cancel-admitted", client.receive(cancel)["error_code"] == "none")
        check("action-tickets-ordered", bool(admitted) and admitted == sorted(set(admitted)), tickets=admitted)
        for token in admitted:
            done = client.completion(token)
            check("cancelled-action-completed", done["error"] in ("cancelled", "budget_exceeded", "verification_failed"), completion=done)
        client.request("set", "blip.wasm", "instruction_budget", 10000)
        client.request("set", "blip.wasm", "deadline_ms", 10)
        check("reload-before-unload-barrier", client.upload(valid)["error"] == "none")
        unload = client.send("action", "blip.wasm", "unload")
        old_action = client.send("action", "blip.wasm", "fire", True, 1, 1.0, "old")
        unloaded = client.receive(unload)
        check("unload-barrier-admitted", unloaded["error_code"] == "none")
        rejected = client.receive(old_action)
        check("old-action-admission-closed", rejected["error_code"] in ("invalid_state", "not_found"), response=rejected)
        check("unload-barrier-completed", client.completion(unloaded["values"][0]["value"])["error"] == "none")
        report["heap_samples"] = []
        for cycle in range(args.cycles):
            check(f"reload-{cycle}", client.upload(valid)["error"] == "none")
            check(f"defaults-{cycle}", client.get("level") == 5 and client.get("note") == "hello")
            check(f"retire-{cycle}", client.work("unload")["error"] == "none")
            # An in-flight schema reader may still own the retired generation.
            # The worker releases text after that lease drains.
            release_deadline = time.monotonic() + 3
            while client.get("control_text_reserved") != 0 and time.monotonic() < release_deadline:
                time.sleep(.05)
            check(f"retired-text-released-{cycle}", client.get("control_text_reserved") == 0)
            report["heap_samples"].append(client.get("heap_free_internal", "blip.diagnostics"))
        report["worker_stack_headroom"] = client.get("worker_stack_headroom")
        check("worker-stack-reserve", report["worker_stack_headroom"] >= 1639)
        check("warmed-heap-stable", max(report["heap_samples"][2:]) - min(report["heap_samples"][2:]) <= 1024)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if client:
            client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        serial_log = args.report.with_suffix(".serial.bin")
        serial_log.write_bytes(serial_received)
        report["serial_capture_sha256"] = hashlib.sha256(serial_received).hexdigest()
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
