"""Qualify automatic bulk files on an already provisioned BLIP; never changes PC Wi-Fi."""
import argparse
from datetime import datetime, timezone
import hashlib
from http.client import HTTPConnection
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
sys.path.insert(0, str(ROOT / "v2/tools/ota"))
from blip_wasm_hil import Client
from blip_release_identity_hil import snapshot
from release_publish import firmware_metadata


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--flash-log", type=Path, required=True)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--expected-medium", choices=("internal", "sd-spi", "sd-mmc", "spi-nor"), required=True)
    parser.add_argument("--reset", action="store_true", help="Default stub-assisted esptool run to qualify persistence")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    url = urlsplit(args.device)
    if url.scheme != "http" or not url.hostname or url.path not in ("", "/") or url.query:
        parser.error("device must be an HTTP origin on its existing network")
    log = args.flash_log.read_bytes()
    text = log.decode("utf-16" if log.startswith((b'\xff\xfe', b'\xfe\xff')) else "utf-8-sig")
    macs = re.findall(r"MAC:\s*([0-9a-f:]+)", text, re.I)
    if not macs or macs[-1].lower() != args.mac.lower(): parser.error("Flashed MAC differs")
    identity, code, version = firmware_metadata(args.build / "blip-v2.bin")
    report = {"passed": False, "created_at": datetime.now(timezone.utc).isoformat(),
              "board": identity["board"], "port": args.port, "mac": args.mac.lower(),
              "firmware_code": code, "firmware_version": version, "checks": [],
              "pc_network_changed": False, "test_firmware_left_installed": True,
              "source_snapshot": snapshot(), "tool_sha256": sha(Path(__file__)),
              "flash_log_sha256": sha(args.flash_log),
              "artifacts": {name: sha(args.build / name) for name in
                  ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}}
    http = None
    serial = None
    original_interval = None
    paths = []
    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition: raise RuntimeError(name)
    # Use one connection, rather than exhausting the device with HTTP sessions.
    def request(method, path, data=None):
        http.request(method, path, body=data, headers={"Content-Type": "application/octet-stream"})
        response = http.getresponse()
        body = response.read(AutomaticBound)
        return response.status, body
    def metrics():
        return {name: serial.get(name, "blip.diagnostics") for name in
                ("heap_free_internal", "heap_largest_internal", "heap_minimum_internal")}
    AutomaticBound = 16 * 1024 * 1024 + 1
    try:
        serial = Client(args.port)
        check("serial-board-identity", serial.get("board", "blip.ota") == identity["board"])
        check("serial-network-identity", serial.get("ip_address", "blip.transport.wifi") == url.hostname)
        report["medium"] = serial.get("preferred_medium", "blip.storage.files.internal")
        report["external_state"] = serial.get("external_state", "blip.storage.files.internal")
        check("automatic-medium-selection", report["medium"] == args.expected_medium)
        report["heap_before"] = metrics()
        original_interval = serial.get("interval_hours", "blip.updates")
        serial.request("set", "blip.updates", "interval_hours", 0)
        report["periodic_checks_temporarily_disabled"] = True
        http = HTTPConnection(url.hostname, url.port or 80, timeout=15)
        fixtures = {}
        tag = f"_blip_hil_{time.monotonic_ns():x}"
        for namespace, size in (("scripts", 16385), ("playback", 32769), ("sequences", 8193)):
            path = f"/api/files/{namespace}/{tag}.bin"
            paths.append(path)
            data = bytes((i * 37 + 11) % 251 for i in range(size))
            fixtures[path] = data
            check(f"{namespace}-upload", request("PUT", path, data)[0] == 201)
            status, returned = request("GET", path)
            check(f"{namespace}-exact-read", status == 200 and returned == data)
        script_path = paths[0]
        for cycle in range(8):
            changed = bytes([cycle]) + fixtures[script_path][1:]
            check(f"replace-{cycle}", request("PUT", script_path, changed)[0] == 201)
            check(f"read-generation-{cycle}", request("GET", script_path) == (200, changed))
            fixtures[script_path] = changed
        # A dropped upload must retain the previous complete generation.
        http.close()
        with socket.create_connection((url.hostname, url.port or 80), timeout=5) as interrupted:
            body = fixtures[script_path]
            header = f"PUT {script_path} HTTP/1.1\r\nHost: {url.hostname}\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n"
            interrupted.sendall(header.encode("ascii") + body[:1024])
        http = HTTPConnection(url.hostname, url.port or 80, timeout=15)
        check("interrupted-upload-keeps-previous", request("GET", script_path) == (200, fixtures[script_path]))
        for bad in ("scripts/../escape", "scripts/a%2fb", "scripts/a?x=1", "server/assets.bundle", "scripts/a\\b"):
            check(f"reject-{bad}", request("PUT", "/api/files/" + bad, b"bad")[0] == 400)
        # Closing downloads must release their reader leases, including errors.
        for cycle in range(6):
            temporary = HTTPConnection(url.hostname, url.port or 80, timeout=15)
            temporary.request("GET", paths[1])
            response = temporary.getresponse()
            check(f"download-admission-{cycle}", response.status == 200)
            response.read(97)
            temporary.close()
        check("reader-leases-released", request("PUT", paths[1], fixtures[paths[1]])[0] == 201)
        if args.reset:
            serial.connection.close(); serial = None
            http.close(); http = None
            reset = subprocess.run([sys.executable, "-m", "esptool", "--chip", identity["target"],
                "-p", args.port, "--before", "default-reset", "--after", "hard-reset", "run"],
                capture_output=True, text=True, timeout=45)
            check("stub-assisted-restart", reset.returncode == 0)
            deadline = time.monotonic() + 45
            while True:
                try:
                    serial = Client(args.port)
                    if serial.get("ip_address", "blip.transport.wifi") == url.hostname: break
                    serial.connection.close(); serial = None
                except Exception:
                    if serial: serial.connection.close()
                    serial = None
                if time.monotonic() >= deadline: raise TimeoutError("Application did not reconnect after restart")
                time.sleep(.5)
            http = HTTPConnection(url.hostname, url.port or 80, timeout=15)
            for path, data in fixtures.items():
                check("reboot-persistence-" + path.split("/")[3], request("GET", path) == (200, data))
        for path in paths:
            check("delete-" + path.split("/")[3], request("DELETE", path)[0] == 200)
            check("deleted-not-empty-" + path.split("/")[3], request("GET", path)[0] == 404)
            check("empty-file-" + path.split("/")[3], request("PUT", path, b"")[0] == 201 and request("GET", path) == (200, b""))
        report["heap_after"] = metrics()
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if http:
            for path in paths:
                try:
                    # Two tombstones reclaim both test payload generations.
                    request("DELETE", path); request("DELETE", path)
                except Exception: pass
            http.close()
        if serial is None and original_interval is not None:
            try: serial = Client(args.port)
            except Exception as error: report["policy_restore_error"] = str(error); report["passed"] = False
        if serial:
            if original_interval is not None:
                try: serial.request("set", "blip.updates", "interval_hours", original_interval)
                except Exception as error: report["policy_restore_error"] = str(error); report["passed"] = False
            serial.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]),
                      "report": str(args.report), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__": raise SystemExit(main())
