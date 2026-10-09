"""Measure real transport stacks through persisted settings and script controls.

Uses the existing LAN; never changes the PC network. Optional provisioning
reads the ignored local credentials file and never records its contents.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import sys
import time
import urllib.parse
import urllib.request

from websockets.sync.client import connect
from blip_led_network_hil import osc_message, decode_osc
from blip_script_controls_hil import fixture
from blip_wasm_hil import Client
from blip_wifi_provision import read_credentials

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/ota"))
from release_publish import firmware_metadata


def source_snapshot():
    paths = [p for p in (ROOT / "v2/components").rglob("*")
             if p.is_file() and p.suffix in (".cpp", ".hpp", ".h", ".c", ".txt")]
    paths += [Path(__file__), ROOT / "v2/tools/control/blip_wasm_hil.py",
              ROOT / "v2/tools/control/blip_script_controls_hil.py",
              ROOT / "v2/tools/control/blip_led_network_hil.py",
              ROOT / "v2/tools/control/blip_wifi_provision.py",
              ROOT / "v2/tools/control/blip_serial_control.py", ROOT / "v2/firmware/CMakeLists.txt"]
    return {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}


def nodes(tree):
    yield tree
    for child in tree.get("CONTENTS", {}).values():
        yield from nodes(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--flash-log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--credentials", type=Path)
    parser.add_argument("--cycles", type=int, default=20)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("cycles must be positive")
    identity, code, _ = firmware_metadata(args.build / "blip-v2.bin")
    if identity["board"] != args.board:
        parser.error("Build identity differs from expected board")
    raw_log = args.flash_log.read_bytes()
    log = raw_log.decode("utf-16" if raw_log.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
    macs = re.findall(r"MAC:\s*([0-9a-f:]+)", log, re.I)
    if not macs or macs[-1].lower() != args.mac.lower():
        parser.error("Flashed MAC differs from expected board")
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = {"board": args.board, "port": args.port, "mac": args.mac,
              "passed": False, "checks": [], "pc_network_changed": False,
              "source_snapshot": source_snapshot(), "checker_sha256": sha(Path(__file__)),
              "flash_log_sha256": sha(args.flash_log),
              "artifacts": {p: sha(args.build / p) for p in ("blip-v2.bin", "blip-v2.elf", "sdkconfig")}}
    client = None
    originals = {}
    serial_log = args.report.with_suffix(".serial.log")
    serial_log.parent.mkdir(parents=True, exist_ok=True)

    class CapturedSerial:
        def __init__(self, connection, log):
            self.connection, self.log = connection, log

        def __getattr__(self, name):
            return getattr(self.connection, name)

        def read(self, size):
            data = self.connection.read(size)
            self.log.write(data)
            self.log.flush()
            return data

    log = serial_log.open("wb")

    def check(name, condition, **detail):
        report["checks"].append({"name": name, "passed": bool(condition), **detail})
        if not condition:
            raise AssertionError(name)

    def sample():
        return {"serial": client.get("task_stack_headroom", "blip.transport.serial"),
                "udp": client.get("task_stack_headroom", "blip.oscquery"),
                "wifi": client.get("worker_stack_headroom", "blip.transport.wifi"),
                "http": client.get("http_stack_headroom", "blip.oscquery"),
                "heap_free": client.get("heap_free_internal", "blip.diagnostics"),
                "heap_largest": client.get("heap_largest_internal", "blip.diagnostics")}

    try:
        client = Client(args.port)
        client.connection = CapturedSerial(client.connection, log)
        report["stage"] = "initial-state"
        check("installed-board", client.get("board", "blip.ota") == args.board)
        check("installed-release", client.get("release_code", "blip.ota") == code)
        ip = client.get("ip_address", "blip.transport.wifi")
        report["ip"] = ip
        report["boot_before"] = client.get("boot_sequence", "blip.diagnostics")
        report["before"] = sample()
        client.action("cancel_all")
        check("initial-unload", client.work("unload")["error"] == "none")
        check("script-published", client.upload(fixture())["error"] == "none")
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        report["stage"] = "dynamic-http-tree"
        with opener.open(f"http://{ip}/?config=1", timeout=8) as response:
            tree = json.load(response)
        components = {n["BLIP_COMPONENT_ID"]: n for n in nodes(tree) if "BLIP_COMPONENT_ID" in n}
        controls = [("blip.device.identity", "name"), ("blip.output.strip0", "red"),
                    ("blip.transport.wifi", "boot_profile"), ("blip.wasm", "note")]
        for component, name in controls:
            report["stage"] = "read-original-" + component + "/" + name
            originals[component, name] = client.get(name, component)
        report["original_settings"] = [{"component": component, "name": name, "value": value}
            for (component, name), value in originals.items()]
        # Journal the values before the first persisted mutation, including a
        # native USB disconnect or a checker process interrupted by a panic.
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        fire = components["blip.wasm"]["CONTENTS"]["fire"]["FULL_PATH"]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, \
                connect(f"ws://{ip}/", open_timeout=5, close_timeout=2, proxy=None) as websocket:
            udp.settimeout(5)

            def exchange(kind, path, values):
                report["stage"] = kind + "-exchange-" + path
                packet = osc_message(path, values)
                if kind == "udp":
                    udp.sendto(packet, (ip, 9000))
                    packet, address = udp.recvfrom(1025)
                    check("udp-peer", address[0] == ip)
                else:
                    websocket.send(packet)
                    packet = websocket.recv(timeout=5)
                returned, values = decode_osc(packet)
                check(kind + "-response-path", returned == path)
                return values[1:] if kind == "udp" else values

            for kind in ("udp", "websocket"):
                for cycle in range(args.cycles):
                    values = ["Stack probe " + args.mac.replace(":", "")[-4:] + f" {cycle % 2}",
                              cycle % 256, originals["blip.transport.wifi", "boot_profile"],
                              "owned " + ("text" * 28)]
                    for (component, name), value in zip(controls, values):
                        path = components[component]["CONTENTS"][name]["FULL_PATH"]
                        check(kind + "-write-" + name, exchange(kind, path, [value]) == [value])
                        check(kind + "-read-" + name, exchange(kind, path, []) == [value])
                    tokens = exchange(kind, fire, [True, -9001, 2.5, "copy"])
                    check(kind + "-script-admitted", len(tokens) == 1)
                    done = client.completion(tokens[0])
                    check(kind + "-script-completed", done["error"] == "none", completion=done)
                    token = client.action("call0", "count")[0]
                    check(kind + "-result-completed", client.completion(token)["error"] == "none")
                    result = client.action("result", token, 0)
                    check(kind + "-typed-callback", (result[1] | result[2] << 32) == (-9001 & ((1 << 64) - 1)))
        if args.credentials:
            report["stage"] = "provisioning"
            ssid, password = read_credentials(args.credentials)
            # The documented helper supplies credentials; only the boolean
            # outcome is retained, never credential values or response bodies.
            check("serial-provision", client.request("action", "blip.transport.wifi", "provision", ssid, password) == [])
            request = urllib.request.Request(f"http://{ip}/provision",
                data=urllib.parse.urlencode({"ssid": ssid, "password": password}).encode(),
                headers={"Content-Type": "application/x-www-form-urlencoded"})
            with opener.open(request, timeout=12) as response:
                check("http-deferred-provision", response.status == 202)
            time.sleep(1.5)
            check("station-retained", client.get("state", "blip.transport.wifi") == 2)
        check("final-unload", client.work("unload")["error"] == "none")
        report["after"] = sample()
        report["boot_after"] = client.get("boot_sequence", "blip.diagnostics")
        check("no-unexpected-reboot", report["boot_before"] == report["boot_after"])
        for name in ("serial", "udp", "wifi", "http"):
            check(name + "-stack-margin", report["after"][name] >= 1024, bytes=report["after"][name])
        report["passed"] = True
    except Exception as error:
        # Do not stringify provisioning exceptions: a protocol response could
        # otherwise include a credential value. Other checks identify stages.
        report["error"] = type(error).__name__
    finally:
        if client:
            try:
                deadline = time.monotonic() + 35
                while True:
                    try:
                        if client is None:
                            client = Client(args.port)
                            client.connection = CapturedSerial(client.connection, log)
                        client.get("board", "blip.ota")
                        break
                    except (TimeoutError, OSError):
                        if client:
                            client.connection.close()
                        client = None
                        if time.monotonic() >= deadline:
                            raise TimeoutError("cleanup serial readiness")
                        time.sleep(.25)
                for (component, name), value in originals.items():
                    if component != "blip.wasm":
                        client.request("set", component, name, value)
                client.action("cancel_all")
                check("cleanup-unload", client.work("unload")["error"] == "none")
                check("settings-restored", all(client.get(name, component) == value
                    for (component, name), value in originals.items() if component != "blip.wasm"))
                report["restored"] = sample()
            except Exception as error:
                report["passed"] = False
                report["restore_error"] = type(error).__name__
            if client:
                client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        log.close()
        report["serial_log_sha256"] = sha(serial_log)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"board": args.board, "passed": report["passed"], "checks": len(report["checks"]),
                      "after": report.get("after"), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
