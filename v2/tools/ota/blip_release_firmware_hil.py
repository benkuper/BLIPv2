"""Qualify a native firmware upgrade from a public catalog on the existing LAN."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time
import urllib.request
from serial import SerialException
from release_publish import firmware_metadata
from blip_release_identity_hil import snapshot

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "control"))
from blip_wasm_hil import Client


class SerialCapture:
    def __init__(self, connection, received):
        self.connection, self.received = connection, received

    def __getattr__(self, name):
        return getattr(self.connection, name)

    def read(self, count):
        data = self.connection.read(count)
        self.received.extend(data)
        return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("port", "mac", "endpoint"): parser.add_argument("--" + name, required=True)
    for name in ("build", "flash-log", "report"): parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    identity, code, version = firmware_metadata(args.build / "blip-v2.bin")
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {"passed": False, "board": identity["board"], "port": args.port, "mac": args.mac,
              "checks": [], "pc_network_changed": False, "expected_code": code, "expected_version": version,
              "firmware_sha256": sha(args.build / "blip-v2.bin"), "source_snapshot": snapshot(),
              "tool_sha256": sha(Path(__file__)), "initial_flash_log_sha256": sha(args.flash_log)}
    client = None; saved = {}; updates = "blip.updates"
    serial_received = bytearray()
    def open_client():
        opened = Client(args.port)
        opened.connection = SerialCapture(opened.connection, serial_received)
        return opened
    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition: raise AssertionError(name)
    def get(name, component=updates): return client.get(name, component)
    try:
        raw = args.flash_log.read_bytes()
        log = raw.decode("utf-16" if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else "utf-8-sig")
        check("initial flashed MAC", args.mac.lower() in re.findall(r"mac:\s*([0-9a-f:]+)", log.lower()))
        client = open_client()
        check("installed board identity", get("board", "blip.ota") == identity["board"])
        report["before_code"] = get("release_code", "blip.ota")
        check("candidate advances firmware release", code > report["before_code"])
        report["memory_before"] = {key: get(key, "blip.diagnostics")
            for key in ("heap_free_internal", "heap_largest_internal")}
        report["boot_before"] = get("boot_sequence", "blip.diagnostics")
        report["name_before"] = get("name", "blip.device.identity")
        report["web_before"] = get("web_bundle_version", "blip.storage.files.internal")
        for key in ("endpoint", "channel", "interval_hours", "automatic_web", "automatic_firmware"): saved[key] = get(key)
        report["original_policy"] = dict(saved)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
        for key, value in {"endpoint": args.endpoint, "channel": "stable", "interval_hours": 0,
                           "automatic_web": False, "automatic_firmware": False}.items():
            client.request("set", updates, key, value)
        client.request("action", updates, "check")
        deadline = time.monotonic() + 30
        while get("state") == "checking":
            if time.monotonic() >= deadline: raise TimeoutError("catalog check")
            time.sleep(.25)
        report["catalog_result"] = {"state": get("state"), "error": get("last_error"), "http": get("http_status")}
        check("public catalog HTTP 200", get("http_status") == 200)
        check("eligible firmware update", get("firmware_available"))
        check("published firmware version", get("firmware_candidate") == version)
        client.request("action", updates, "install_firmware")
        deadline = time.monotonic() + 210
        progress = []; confirmed = False
        report["download_progress"] = progress  # Retain partial progress on failure too.
        while time.monotonic() < deadline:
            try:
                if client is None: client = open_client()
                installed_code = get("release_code", "blip.ota")
                if installed_code == code:
                    if get("state", "blip.ota") == "confirmed": confirmed = True; break
                else:
                    state = get("state")
                    if state in ("error", "cancelled"): raise RuntimeError(f"{state}: {get('last_error')}")
                    received = get("received_bytes")
                    if not progress or received != progress[-1]: progress.append(received)
            except (TimeoutError, OSError, SerialException):
                # Native USB may disappear or keep a stale handle across OTA.
                # Reopen without toggling DTR/RTS or causing another reset.
                if client: client.connection.close()
                client = None
                report["serial_reconnects"] = report.get("serial_reconnects", 0) + 1
            time.sleep(.25)
        report["download_progress"] = progress
        check("website firmware boots and confirms", confirmed)
        report["boot_after"] = get("boot_sequence", "blip.diagnostics")
        check("upgrade has one software reboot", report["boot_after"] == report["boot_before"] + 1)
        check("installed firmware version", get("version", "blip.ota") == version)
        check("firmware upgrade preserves interface", get("web_bundle_version", "blip.storage.files.internal") == report["web_before"])
        check("firmware upgrade preserves device name", get("name", "blip.device.identity") == report["name_before"])
        deadline = time.monotonic() + 45
        while get("state", "blip.transport.wifi") != 2:
            if time.monotonic() >= deadline: raise TimeoutError("post-upgrade shared-network readiness")
            time.sleep(.5)
        host = get("ip_address", "blip.transport.wifi"); report["host"] = host
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(f"http://{host}/api/releases", timeout=15) as response: state = json.load(response)
        check("HTTP exposes installed firmware code", state["installed_firmware"] == code)
        report["passed"] = True
    except Exception as error: report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if client is None and saved:
            try: client = open_client()
            except (OSError, SerialException) as error: report["policy_restore_error"] = str(error)
        if client:
            try:
                for key, value in saved.items(): client.request("set", updates, key, value)
                report["policy_restored"] = all(get(key) == value for key, value in saved.items())
                if not report["policy_restored"]: report["passed"] = False
            except Exception as error: report["policy_restore_error"] = str(error); report["passed"] = False
            client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        serial_log = args.report.with_suffix(".serial.log")
        serial_log.write_bytes(serial_received)
        report["serial_log_sha256"] = sha(serial_log)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"board": report["board"], "passed": report["passed"], "checks": len(report["checks"]),
                      "error": report.get("error"), "report": str(args.report)}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__": raise SystemExit(main())
