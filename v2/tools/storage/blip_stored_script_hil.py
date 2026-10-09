"""Qualify named script loading from automatic storage on an existing LAN."""
import argparse
from datetime import datetime, timezone
import hashlib
from http.client import HTTPConnection
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
sys.path.insert(0, str(ROOT / "v2/tools/ota"))
from blip_wasm_hil import Client
from blip_script_controls_hil import fixture, SerialCapture
from blip_release_identity_hil import snapshot
from release_publish import firmware_metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("device", "port", "mac", "expected-medium"):
        parser.add_argument("--" + name, required=True)
    for name in ("build", "flash-log", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--reset", action="store_true")
    args = parser.parse_args()
    origin = urlsplit(args.device)
    if origin.scheme != "http" or not origin.hostname or origin.path not in ("", "/") or origin.query:
        parser.error("device must be an HTTP origin")
    if not 1 <= args.cycles <= 100: parser.error("cycles must be 1..100")
    identity, code, version = firmware_metadata(args.build / "blip-v2.bin")
    raw = args.flash_log.read_bytes()
    log = raw.decode("utf-16" if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else "utf-8-sig")
    if args.mac.lower() not in re.findall(r"mac:\s*([0-9a-f:]+)", log.lower()):
        parser.error("flashed MAC differs")
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {"passed": False, "created_at": datetime.now(timezone.utc).isoformat(),
              "board": identity["board"], "firmware_code": code, "firmware_version": version,
              "mac": args.mac.lower(), "port": args.port, "checks": [],
              "pc_network_changed": False, "source_snapshot": snapshot(),
              "tool_sha256": sha(Path(__file__)), "flash_log_sha256": sha(args.flash_log),
              "artifacts": {name: sha(args.build / name) for name in
                            ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}}
    client = http = None
    received = bytearray()
    original_interval = None
    paths = []
    tag = f"stored_{time.monotonic_ns():x}"
    script = "scripts/" + tag + ".wasm"
    data = fixture()

    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition: raise AssertionError(name)

    def open_client():
        result = Client(args.port)
        result.connection = SerialCapture(result.connection, received)
        return result

    def request(method, path, body=None):
        http.request(method, "/api/files/" + path, body=body,
                     headers={"Content-Type": "application/octet-stream"})
        response = http.getresponse()
        return response.status, response.read(16386)

    def store(path, body):
        if path not in paths: paths.append(path)
        status, detail = request("PUT", path, body)
        if status != 201: report["last_file_error"] = {"status": status, "detail": detail.decode("utf-8", errors="replace")}
        check("store-" + path, status == 201)

    def load_result(path):
        token = client.action("load_file", path)[0]
        deadline = time.monotonic() + 60
        while True:
            try: return client.completion(token)
            except TimeoutError:
                if time.monotonic() >= deadline: raise
                # SD checksum validation runs on the worker, independently of
                # serial admission. Its file read can exceed a call's 2 s poll.

    def loaded(path=script):
        result = load_result(path)
        check("named-load", result["error"] == "none")
        check("module-loaded", client.get("state") == "loaded" and client.get("loaded_bytes") == len(data))

    def execute():
        check("copied-script-action", client.work("fire", True, -9223372036854775807, 1.5, "stored script")["error"] == "none")
        token = client.action("call0", "count")[0]
        check("loaded-file-executes", client.completion(token)["error"] == "none")
        result = client.action("result", token, 0)
        check("lossless-guest-result", result == [1, 1, 2147483648])

    try:
        client = open_client()
        check("board-identity", client.get("board", "blip.ota") == identity["board"])
        check("release-identity", client.get("release_code", "blip.ota") == code)
        check("network-identity", client.get("ip_address", "blip.transport.wifi") == origin.hostname)
        report["medium"] = client.get("preferred_medium", "blip.storage.files.internal")
        check("automatic-medium", report["medium"] == args.expected_medium)
        original_interval = client.get("interval_hours", "blip.updates")
        client.request("set", "blip.updates", "interval_hours", 0)
        http = HTTPConnection(origin.hostname, origin.port or 80, timeout=20)
        store(script, data)
        check("exact-stored-bytes", request("GET", script) == (200, data))
        loaded(); execute()
        client.request("set", "blip.wasm", "level", 42)
        check("edit-script-parameter", client.get("level") == 42)
        generation = client.get("generation")
        for path, expected in (("scripts/" + tag + "_missing.wasm", "not_found"),
                               ("scripts/" + tag + "_large.wasm", "capacity_exceeded"),
                               ("scripts/" + tag + "_empty.wasm", "capacity_exceeded")):
            if "_large" in path: store(path, bytes(16385))
            if "_empty" in path: store(path, b"")
            check("failed-lookup-or-size", load_result(path)["error"] == expected)
            check("early-failure-preserves-module", client.get("state") == "loaded" and
                  client.get("generation") == generation and client.get("level") == 42)
            execute()  # Also proves action admission reopened on early failure.
        for path in ("scripts/../bad", "playback/a.wasm", "scripts/", "scripts/a//b",
                     "scripts/a?b", "scripts/a\\b", "scripts/a\0b", "scripts/" + "x" * 89):
            response = client.receive(client.send("action", "blip.wasm", "load_file", path))
            check("reject-invalid-script-path", response["error_code"] != "none")
        # LittleFS has a 64-byte per-component limit, independent of the
        # 96-byte logical path bound. Include the physical .b0/.b1 suffix.
        long_path = "scripts/" + "x" * 40 + "/" + "y" * (47 - len(tag)) + tag
        check("maximum-logical-path", len(long_path) == 96)
        store(long_path, data); loaded(long_path); execute()
        for label, body in (("magic", b"invalid wasm"), ("signature", fixture(bad_signature=True))):
            bad = "scripts/" + tag + "_" + label + ".wasm"
            store(bad, body)
            check("reject-invalid-module-" + label, load_result(bad)["error"] != "none")
            check("no-partial-module", client.get("state") == "ready")
            loaded(); execute()
        heaps = []
        for cycle in range(args.cycles):
            loaded()
            check("replacement-resets-parameters", client.get("level") == 5 and client.get("note") == "hello")
            execute()
            heaps.append(client.get("heap_free_internal", "blip.diagnostics"))
        report["cycles"] = args.cycles
        report["heap_samples"] = heaps
        report["heap_range"] = [min(heaps), max(heaps)]
        # Repeated reads must release their generation before replacements.
        for cycle in range(6):
            store(script, data); loaded()
        if args.reset:
            client.connection.close(); client = None; http.close(); http = None
            restart = subprocess.run([sys.executable, "-m", "esptool", "--chip", identity["target"],
                "-p", args.port, "--before", "default-reset", "--after", "hard-reset", "run"],
                capture_output=True, text=True, timeout=45)
            check("stub-assisted-restart", restart.returncode == 0)
            deadline = time.monotonic() + 45
            while True:
                try:
                    client = open_client()
                    if client.get("ip_address", "blip.transport.wifi") == origin.hostname: break
                    client.connection.close(); client = None
                except Exception:
                    if client: client.connection.close()
                    client = None
                if time.monotonic() >= deadline: raise TimeoutError("post-reset readiness")
                time.sleep(.5)
            http = HTTPConnection(origin.hostname, origin.port or 80, timeout=20)
            check("stored-module-persists", request("GET", script) == (200, data))
            loaded(); execute()
        check("delete-stored-script", request("DELETE", script)[0] == 200)
        check("loaded-module-independent-of-file", client.get("state") == "loaded")
        execute()
        check("deleted-script-not-found", load_result(script)["error"] == "not_found")
        execute()
        report["worker_stack_headroom"] = client.get("worker_stack_headroom")
        check("worker-stack-margin", report["worker_stack_headroom"] >= 1024)
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if http:
            for path in paths:
                try: request("DELETE", path); request("DELETE", path)
                except Exception: pass
            http.close()
        if client is None and original_interval is not None:
            try: client = open_client()
            except Exception as error: report["cleanup_error"] = str(error); report["passed"] = False
        if client:
            try:
                client.work("unload")
                if original_interval is not None:
                    client.request("set", "blip.updates", "interval_hours", original_interval)
                    report["policy_restored"] = client.get("interval_hours", "blip.updates") == original_interval
                    if not report["policy_restored"]: report["passed"] = False
            except Exception as error: report["cleanup_error"] = str(error); report["passed"] = False
            client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        serial_log = args.report.with_suffix(".serial.log"); serial_log.write_bytes(received)
        report["serial_log_sha256"] = sha(serial_log)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "board": report["board"], "checks": len(report["checks"]),
                      "error": report.get("error"), "report": str(args.report)}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__": raise SystemExit(main())
