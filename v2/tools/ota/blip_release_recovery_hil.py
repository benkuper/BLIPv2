"""Inject native-update faults on an existing LAN; optional real browser observer."""
import argparse
from copy import deepcopy
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from http.client import HTTPConnection
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from urllib.parse import urlsplit
from release_publish import firmware_metadata, web_metadata
from blip_release_identity_hil import snapshot

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "control"))
from blip_wasm_hil import Client


class SerialCapture:
    def __init__(self, connection, received): self.connection = connection; self.received = received
    def __getattr__(self, name): return getattr(self.connection, name)
    def read(self, count):
        data = self.connection.read(count); self.received.extend(data); return data


class FaultServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, catalog, firmware, web):
        super().__init__(address, FaultHandler)
        self.catalog = catalog; self.firmware = firmware; self.web = web
        self.mode = "valid"; self.artifact_started = threading.Event()
        self.sent = 0; self.requests = []
        self.artifact_pause_seconds = 0
        self.artifact_pauses = 0

    def select(self, mode):
        self.mode = mode; self.artifact_started.clear(); self.sent = 0


class FaultHandler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass

    def do_GET(self):
        server = self.server; mode = server.mode; path = urlsplit(self.path).path
        server.requests.append({"path": path, "mode": mode})
        body = None; status = 200; declared = None
        if path == "/blip/update":
            catalog = deepcopy(server.catalog)
            if mode == "catalog-identity": catalog["board"] = "different-board"
            if mode == "web-crc":
                damaged = bytearray(server.web); damaged[-1] ^= 1
                catalog["web"]["sha256"] = hashlib.sha256(damaged).hexdigest()
            body = json.dumps(catalog, separators=(",", ":")).encode()
            if mode == "catalog-malformed": body = b'{"schema":'
            if mode == "catalog-oversized": body = b" " * 4097
            if mode == "catalog-redirect": status = 302
        elif path in ("/blip/releases/firmware.bin", "/blip/releases/web.bundle"):
            firmware = path.endswith(".bin")
            body = server.firmware if firmware else server.web
            kind = "firmware" if firmware else "web"
            if mode == kind + "-identity":
                damaged = bytearray(body); damaged[348 if firmware else 8] ^= 1; body = bytes(damaged)
            if mode in (kind + "-sha", "web-crc"):
                damaged = bytearray(body); damaged[-1] ^= 1; body = bytes(damaged)
            declared = len(body)
            if mode == kind + "-truncated": body = body[:8192]
            server.artifact_started.set()
        else: status = 404; body = b"missing"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body) if declared is None else declared))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Connection", "close")
        if status == 302: self.send_header("Location", "/blip/update")
        self.end_headers()
        try:
            for offset in range(0, len(body), 512):
                if mode.endswith("-stalled") and offset == 4096:
                    time.sleep(40)
                if mode == "web-browser" and offset == 4096 and server.artifact_pause_seconds:
                    server.artifact_pauses += 1
                    time.sleep(server.artifact_pause_seconds)
                self.wfile.write(body[offset:offset + 512]); self.wfile.flush()
                if path.startswith("/blip/releases/"):
                    server.sent = offset + len(body[offset:offset + 512])
                    if mode.endswith(("-cancel", "-reset")) or mode == "web-browser": time.sleep(.02)
        except (BrokenPipeError, ConnectionResetError, OSError): pass
        self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("port", "mac", "listen"): parser.add_argument("--" + name, required=True)
    parser.add_argument("--server-port", type=int, default=8091)
    for name in ("build", "initial-build", "flash-log", "bundle", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--playwright", type=Path)
    parser.add_argument("--browser", type=Path)
    parser.add_argument("--skip-faults", action="store_true", help="Isolate the successful browser/native-update trial")
    parser.add_argument("--hold-http-sessions", type=int, default=0, choices=range(4), help="Exercise the remaining HTTP session capacity alongside the browser")
    parser.add_argument("--artifact-pause-seconds", type=float, default=0, help="Pause the successful web response once after 4 KiB to exercise transient read timeouts (0..20)")
    parser.add_argument("--require-script-controls", action="store_true", help="Require the standard production script fixture to stay loaded through the successful web update")
    parser.add_argument("--require-ble", action="store_true", help="Require the NimBLE transport to remain active through the successful web update")
    parser.add_argument("--sample-dma", action="store_true", help="Record DMA heap metrics exposed by the installed firmware")
    args = parser.parse_args()
    if not 0 <= args.artifact_pause_seconds <= 20:
        parser.error("artifact pause must be 0..20 seconds")
    if bool(args.playwright) != bool(args.browser): parser.error("browser and playwright must be supplied together")
    identity, code, version = firmware_metadata(args.build / "blip-v2.bin")
    initial_identity, initial_code, _ = firmware_metadata(args.initial_build / "blip-v2.bin")
    web_code, web_version = web_metadata(args.bundle)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {"passed": False, "board": identity["board"], "port": args.port, "mac": args.mac,
              "checks": [], "trials": [], "pc_network_changed": False, "source_snapshot": snapshot(),
              "tool_sha256": sha(Path(__file__)), "firmware_sha256": sha(args.build / "blip-v2.bin"),
              "initial_firmware_sha256": sha(args.initial_build / "blip-v2.bin"),
              "bundle_sha256": sha(args.bundle), "flash_log_sha256": sha(args.flash_log)}
    base = f"http://{args.listen}:{args.server_port}"
    def artifact(data, release_code, release_version, path):
        return {"code": release_code, "version": release_version, "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(), "url": base + "/blip/releases/" + path,
                "minimum_other_code": 0 if path.endswith(".bin") else 1}
    fw = (args.build / "blip-v2.bin").read_bytes(); web = args.bundle.read_bytes()
    catalog = {"schema": 1, **identity, "channel": "stable", "firmware": artifact(fw, code, version, "firmware.bin"),
               "web": artifact(web, web_code, web_version, "web.bundle")}
    server = FaultServer((args.listen, args.server_port), catalog, fw, web)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    client = None; saved = {}; observer = None; observer_stop_sent = False; holds = []; updates = "blip.updates"
    serial_received = bytearray()
    def connect():
        value = Client(args.port)
        value.connection = SerialCapture(value.connection, serial_received)
        return value
    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition: raise AssertionError(name)
    def get(name, component=updates): return client.get(name, component)
    def settled(timeout=30):
        deadline = time.monotonic() + timeout
        next_sample = 0
        while (state := get("state")) in ("checking", "downloading-web", "downloading-firmware", "restarting"):
            if time.monotonic() >= deadline: raise TimeoutError("worker did not settle")
            if time.monotonic() >= next_sample:
                report.setdefault("worker_samples", []).append({"state": state,
                    "received": get("received_bytes"),
                    "heap": get("heap_free_internal", "blip.diagnostics"),
                    "largest": get("heap_largest_internal", "blip.diagnostics")})
                if args.sample_dma:
                    report["worker_samples"][-1].update({
                        "dma_heap": get("heap_free_dma", "blip.diagnostics"),
                        "dma_largest": get("heap_largest_dma", "blip.diagnostics")})
                next_sample = time.monotonic() + .5
            time.sleep(.1)
        # Allow the worker to finish releasing transaction resources.
        time.sleep(.1)
        return get("state")
    def healthy(label, expected_web):
        check(label + " preserves boot", get("boot_sequence", "blip.diagnostics") == report["boot"])
        check(label + " preserves reset cause", get("reset_cause", "blip.diagnostics") == report["reset_cause"])
        check(label + " preserves firmware", get("release_code", "blip.ota") == initial_code and get("state", "blip.ota") in ("confirmed", "failed", "idle"))
        check(label + " preserves interface", get("web_bundle_version", "blip.storage.files.internal") == expected_web)
        check(label + " preserves storage medium", get("preferred_medium", "blip.storage.files.internal") == report["initial_medium"])
    def catalog_check(mode):
        server.select(mode); client.request("action", updates, "check")
        state = settled()
        trial = {"mode": mode, "state": state, "error": get("last_error"), "http": get("http_status"),
                 "heap": get("heap_free_internal", "blip.diagnostics")}
        report.setdefault("catalog_trials", []).append(trial)
        print(json.dumps(trial), flush=True)
        return state
    def reset_and_reconnect():
        nonlocal client
        client.connection.close(); client = None
        completed = subprocess.run([sys.executable, "-m", "esptool", "--chip", identity["target"], "-p", args.port, "run"], capture_output=True, text=True)
        report.setdefault("reset_logs", []).append(completed.stdout + completed.stderr)
        check("stub-assisted reset command", completed.returncode == 0)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            try:
                client = connect()
                if get("state", "blip.transport.wifi") == 2: return
            except (TimeoutError, OSError): pass
            if client: client.connection.close(); client = None
            time.sleep(.5)
        raise TimeoutError("post-reset application readiness")
    try:
        raw = args.flash_log.read_bytes(); text = raw.decode("utf-16" if raw.startswith(b'\xff\xfe') else "utf-8-sig")
        check("flashed MAC", args.mac.lower() in re.findall(r"mac:\s*([0-9a-f:]+)", text.lower()))
        check("same initial and candidate hardware identity", initial_identity == identity)
        client = connect()
        check("installed board", get("board", "blip.ota") == identity["board"])
        check("installed initial code", get("release_code", "blip.ota") == initial_code)
        if not args.skip_faults: check("newer firmware candidate", code > initial_code)
        report["boot"] = get("boot_sequence", "blip.diagnostics")
        report["reset_cause"] = get("reset_cause", "blip.diagnostics")
        report["initial_web"] = get("web_bundle_version", "blip.storage.files.internal")
        report["initial_name"] = get("name", "blip.device.identity")
        report["initial_medium"] = get("preferred_medium", "blip.storage.files.internal")
        check("newer web candidate", web_code > report["initial_web"])
        deadline = time.monotonic() + 45
        while get("state", "blip.transport.wifi") != 2:
            if time.monotonic() >= deadline: raise TimeoutError("shared-network readiness")
            time.sleep(.5)
        report["host"] = get("ip_address", "blip.transport.wifi")
        for key in ("endpoint", "channel", "interval_hours", "automatic_web", "automatic_firmware"): saved[key] = get(key)
        report["original_policy"] = dict(saved)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
        for key, value in {"endpoint": base + "/blip/update", "channel": "stable", "interval_hours": 0,
                           "automatic_web": False, "automatic_firmware": False}.items(): client.request("set", updates, key, value)
        for mode in (() if args.skip_faults else ("catalog-malformed", "catalog-oversized", "catalog-identity", "catalog-redirect")):
            state = catalog_check(mode)
            check(mode + " rejected", state == "error")
            check(mode + " clears eligibility", not get("web_available") and not get("firmware_available"))
            healthy(mode, report["initial_web"])
        for kind in (() if args.skip_faults else ("web", "firmware")):
            faults = ("identity", "sha", "truncated", "crc", "stalled", "cancel", "reset") if kind == "web" else ("identity", "sha", "truncated", "stalled", "cancel", "reset")
            for fault in faults:
                mode = kind + "-" + fault
                check(mode + " catalog valid", catalog_check(mode) == "updates-available")
                client.request("action", updates, "install_" + kind)
                transfer_started = time.monotonic()
                if fault in ("cancel", "reset"):
                    deadline = time.monotonic() + 20
                    while get("received_bytes") < 2048:
                        if get("state") not in ("downloading-web", "downloading-firmware") or time.monotonic() >= deadline:
                            raise TimeoutError(f"{mode} transfer readiness: {get('state')}: {get('last_error')}")
                        time.sleep(.05)
                    if fault == "cancel":
                        started = time.monotonic(); client.request("action", updates, "cancel")
                        state = settled(10)
                        report.setdefault("cancel_seconds", {})[kind] = time.monotonic() - started
                    else:
                        old_boot = report["boot"]; reset_and_reconnect()
                        report["boot"] = get("boot_sequence", "blip.diagnostics")
                        report["reset_cause"] = get("reset_cause", "blip.diagnostics")
                        # esptool's hardware reset/download stub may clear RTC;
                        # the retained sequence can legitimately restart at one.
                        report.setdefault("commanded_resets", []).append({"mode": mode,
                            "boot_before": old_boot, "boot_after": report["boot"]})
                        check(mode + " returns to confirmed firmware", get("state", "blip.ota") == "confirmed")
                        state = get("state")
                else: state = settled(150)
                if fault == "stalled":
                    elapsed = time.monotonic() - transfer_started
                    check(mode + " hits bounded idle deadline", 25 <= elapsed <= 40 and get("http_read_retries") >= 4)
                report["trials"].append({"mode": mode, "state": state, "received": get("received_bytes"),
                    "error": get("last_error"), "worker_stack_headroom": get("worker_stack_headroom"),
                    "heap": get("heap_free_internal", "blip.diagnostics")})
                check(mode + " rejected or cancelled", state == ("cancelled" if fault == "cancel" else "not-checked" if fault == "reset" else "error"))
                healthy(mode, report["initial_web"])
                print(json.dumps({"trial": mode, "state": state, "passed": True}), flush=True)
        if not args.skip_faults:
            check("redirect is never followed", sum(request["mode"] == "catalog-redirect" for request in server.requests) == 1)
            report["fault_matrix_passed"] = True
        if args.browser:
            ready = args.report.with_suffix(".browser-ready.json"); ready.unlink(missing_ok=True)
            browser_report = args.report.with_suffix(".browser.json")
            observer = subprocess.Popen(["node", "v2/tools/web/blip_web_update_observer.mjs", "--device", "http://" + report["host"],
                "--playwright", str(args.playwright), "--browser", str(args.browser), "--ready", str(ready), "--report", str(browser_report)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 45
            while not ready.exists():
                if observer.poll() is not None or time.monotonic() >= deadline: raise TimeoutError("browser observer readiness")
                time.sleep(.1)
        for _ in range(args.hold_http_sessions):
            connection = HTTPConnection(report["host"], timeout=15)
            connection.request("GET", "/api/releases")
            response = connection.getresponse(); response.read()
            check("held HTTP session accepted", response.status == 200)
            holds.append(connection)
        report["held_http_sessions"] = len(holds)
        if args.require_script_controls:
            report["script_before_web"] = {name: get(name, "blip.wasm") for name in ("state", "control_text_reserved", "level", "note")}
            check("script loaded before update", report["script_before_web"]["state"] == "loaded" and
                  report["script_before_web"]["control_text_reserved"] == 640)
        if args.require_ble:
            check("BLE active before update", get("active", "blip.transport.ble"))
        check("successful web catalog", catalog_check("web-browser") == "updates-available")
        server.artifact_pause_seconds = args.artifact_pause_seconds
        client.request("action", updates, "install_web")
        final_state = settled(150)
        report["successful_web_result"] = {"state": final_state, "error": get("last_error"),
            "received": get("received_bytes"), "expected": get("expected_bytes"),
            "worker_stack_headroom": get("worker_stack_headroom"), "http_read_retries": get("http_read_retries")}
        check("successful web installation", final_state == "web-installed")
        report["artifact_pause_seconds"] = args.artifact_pause_seconds
        report["artifact_pauses"] = server.artifact_pauses
        if args.artifact_pause_seconds:
            check("transient artifact pause exercised", server.artifact_pauses == 1)
            if args.artifact_pause_seconds >= 15:
                check("transient HTTP read timeout recovered", get("http_read_retries") > 0)
        healthy("successful web update", web_code)
        if args.require_script_controls:
            check("loaded script and controls retained", report["script_before_web"] ==
                  {name: get(name, "blip.wasm") for name in report["script_before_web"]})
            token = client.action("call0", "armed")[0]
            check("loaded script executes after web update", client.completion(token)["error"] == "none" and
                  client.action("result", token, 0) in ([0, 0, 0], [0, 1, 0]))
        if args.require_ble:
            check("BLE retained after update", get("active", "blip.transport.ble"))
        if observer:
            time.sleep(12)  # Include the UI's normal 10-second installed-version poll.
            observer_stop_sent = True
            output, errors = observer.communicate("stop\n", timeout=20)
            observer_code = observer.returncode; observer = None
            report["browser_output"] = output; report["browser_errors"] = errors
            browser_result = json.loads(browser_report.read_text())
            report["browser_report_sha256"] = sha(browser_report)
            check("browser remains live during web update", observer_code == 0 and browser_result["passed"])
        report["server_requests"] = server.requests
        check("retains device name", get("name", "blip.device.identity") == report["initial_name"])
        report["passed"] = True
    except Exception as error: report["error"] = f"{type(error).__name__}: {error}"
    finally:
        for connection in holds: connection.close()
        if serial_received:
            console = args.report.with_suffix(".serial.bin")
            console.write_bytes(serial_received)
            report["serial_capture_sha256"] = sha(console)
        report["server_requests"] = server.requests
        if observer:
            try: report["browser_output"], report["browser_errors"] = observer.communicate(None if observer_stop_sent else "stop\n", timeout=20)
            except subprocess.TimeoutExpired:
                observer.kill(); observer.communicate(); report["browser_cleanup_error"] = "observer exit timeout"
            except Exception as error: report["browser_cleanup_error"] = str(error)
        if client:
            try:
                client.request("action", updates, "cancel"); settled(10)
                for key, value in saved.items(): client.request("set", updates, key, value)
                report["policy_restored"] = all(get(key) == value for key, value in saved.items())
                if not report["policy_restored"]: report["passed"] = False
            except Exception as error: report["policy_restore_error"] = str(error); report["passed"] = False
            client.connection.close()
        server.shutdown(); server.server_close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"board": report["board"], "passed": report["passed"], "checks": len(report["checks"]),
                      "error": report.get("error"), "report": str(args.report)}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__": raise SystemExit(main())
