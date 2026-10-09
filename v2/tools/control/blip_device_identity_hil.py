#!/usr/bin/env python3
"""Qualify saved identity and live mDNS on shared LAN, without changing PC Wi-Fi."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from datetime import datetime, timezone

from blip_wasm_hil import Client
from blip_discovery_probe import http_json, services, OSC, OSCQUERY

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/ota"))
from release_publish import firmware_metadata
from blip_release_identity_hil import snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--type", required=True)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--flash-log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    metadata, code, version = firmware_metadata(args.build / "blip-v2.bin")
    report = {"passed": False, "board": metadata["board"], "port": args.port,
              "device_type": args.type, "mac": args.mac, "checks": [],
              "created_at": datetime.now(timezone.utc).isoformat(),
              "pc_network_changed": False, "test_firmware_left_installed": True,
              "source_snapshot": snapshot(), "tool_sha256": sha(Path(__file__)),
              "firmware_sha256": sha(args.build / "blip-v2.bin"),
              "firmware_code": code, "firmware_version": version,
              "flash_log_sha256": sha(args.flash_log)}
    client = None
    original = None
    component = "blip.device.identity"
    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition: raise AssertionError(name)
    def ready(timeout=35):
        nonlocal client
        if client: client.connection.close()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                client = Client(args.port)
                client.get("name", component)
                host = client.get("ip_address", "blip.transport.wifi")
                if host not in ("0.0.0.0", "192.168.4.1"): return host
            except Exception:
                pass
            if client: client.connection.close(); client = None
            time.sleep(.3)
        raise TimeoutError("serial identity / shared LAN readiness")
    def verify(host, name):
        info = http_json(host, "/?HOST_INFO")
        check("HTTP identity matches saved name and type", info["NAME"] == name and info["DEVICE_TYPE"] == args.type)
        check("MAC identity matches flashed device", info["DEVICE_ID"].lower() == args.mac.lower())
        stem = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")[:50].rstrip("-") or "blip"
        expected = stem + "-" + args.mac.replace(":", "").lower() + ".local."
        found = services(host, timeout=12)
        check("both mDNS services include saved device name", set(found) == {OSC, OSCQUERY} and
              all(service["server"] == expected for service in found.values()))
        report.setdefault("discovery", []).append({"device_name": name, "services": found})
    try:
        raw = args.flash_log.read_bytes()
        text = raw.decode("utf-16" if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else "utf-8-sig")
        check("flashed device MAC", args.mac.lower() in re.findall(r"mac:\s*([0-9a-f:]+)", text.lower()))
        host = ready()
        original = client.get("name", component)
        report["original_name"] = original; report["host"] = host
        check("human-readable board type", client.get("type", component) == args.type)
        verify(host, original)
        renamed = "Stage Left " + args.mac.replace(":", "")[-4:]
        client.request("set", component, "name", renamed)
        check("saved rename readable over serial", client.get("name", component) == renamed)
        time.sleep(1.5)
        verify(host, renamed)
        for invalid in ("", "bad\nname", "x" * 64):
            response = client.receive(client.send("set", component, "name", invalid))
            check("invalid name rejected", response["error_code"] == "invalid_argument")
        response = client.receive(client.send("set", component, "type", "Other"))
        check("board type is read-only", response["error_code"] != "none")
        client.connection.close(); client = None
        reset = subprocess.run([sys.executable, "-m", "esptool", "--chip", metadata["target"],
            "-p", args.port, "run"], capture_output=True, text=True, timeout=20)
        check("stub-assisted reset", reset.returncode == 0)
        host = ready()
        check("name survives reboot", client.get("name", component) == renamed)
        verify(host, renamed)
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if original is not None:
            try:
                if client is None: ready()
                client.request("set", component, "name", original)
                check("original device name restored", client.get("name", component) == original)
            except Exception as error:
                report["passed"] = False; report["restore_error"] = str(error)
        if client: client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"board": report["board"], "passed": report["passed"], "checks": len(report["checks"]),
                      "error": report.get("error"), "report": str(args.report)}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__": raise SystemExit(main())
