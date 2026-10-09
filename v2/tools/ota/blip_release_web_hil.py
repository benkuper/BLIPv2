"""Qualify a native web update against a public local catalog on existing Wi-Fi."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time
import urllib.request
from release_publish import firmware_metadata, web_metadata
from blip_release_identity_hil import snapshot

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "control"))
from blip_wasm_hil import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("port", "mac", "endpoint"): parser.add_argument("--" + name, required=True)
    for name in ("build", "flash-log", "bundle", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    identity, code, version = firmware_metadata(args.build / "blip-v2.bin")
    web_code, web_version = web_metadata(args.bundle)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {"passed": False, "board": identity["board"], "port": args.port,
              "mac": args.mac, "checks": [], "pc_network_changed": False,
              "firmware_sha256": sha(args.build / "blip-v2.bin"), "firmware_code": code,
              "firmware_version": version, "bundle_sha256": sha(args.bundle),
              "expected_web_code": web_code, "expected_web_version": web_version,
              "source_snapshot": snapshot(), "tool_sha256": sha(Path(__file__))}
    client = None; saved = {}
    updates = "blip.updates"
    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition: raise AssertionError(name)
    def get(name, component=updates): return client.get(name, component)
    try:
        raw = args.flash_log.read_bytes()
        text = raw.decode("utf-16" if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else "utf-8-sig")
        check("flashed MAC", args.mac.lower() in re.findall(r"mac:\s*([0-9a-f:]+)", text.lower()))
        report["flash_log_sha256"] = sha(args.flash_log)
        client = Client(args.port)
        deadline = time.monotonic() + 45
        while True:
            try:
                if get("state", "blip.transport.wifi") == 2: break
            except TimeoutError: pass
            if time.monotonic() >= deadline: raise TimeoutError("shared-network readiness")
            time.sleep(.5)
        host = get("ip_address", "blip.transport.wifi"); report["host"] = host
        check("installed board identity", get("board", "blip.ota") == identity["board"])
        report["boot_before"] = get("boot_sequence", "blip.diagnostics")
        report["heap_before"] = get("heap_free_internal", "blip.diagnostics")
        for key in ("endpoint", "interval_hours", "automatic_web", "automatic_firmware"):
            saved[key] = get(key)
        for key, value in {"endpoint": args.endpoint, "interval_hours": 0,
                           "automatic_web": False, "automatic_firmware": False}.items():
            client.request("set", updates, key, value)
        client.request("action", updates, "check")
        deadline = time.monotonic() + 30
        while get("state") == "checking":
            if time.monotonic() >= deadline: raise TimeoutError("catalog check")
            time.sleep(.5)
        check("public catalog HTTP 200", get("http_status") == 200)
        check("eligible web update", get("web_available"))
        check("published web version", get("web_candidate") == web_version)
        client.request("action", updates, "install_web")
        deadline = time.monotonic() + 150
        while True:
            state = get("state")
            if state != "downloading-web": break
            if time.monotonic() >= deadline: raise TimeoutError("native web installation")
            time.sleep(1)
        report["final_state"] = state; report["last_error"] = get("last_error")
        check("native web installed", state == "web-installed")
        time.sleep(.2)
        check("web update does not reboot", get("boot_sequence", "blip.diagnostics") == report["boot_before"])
        report["worker_stack_headroom"] = get("worker_stack_headroom")
        check("worker retains stack margin", report["worker_stack_headroom"] >= 768)
        check("installed web code", get("web_bundle_version", "blip.storage.files.internal") == web_code)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(f"http://{host}/api/web-assets", timeout=15) as response: assets = json.load(response)
        report["web_assets"] = assets
        check("HTTP serves installed bundle", assets["bundle_version"] == web_code and assets["bytes"] == args.bundle.stat().st_size)
        report["heap_after"] = get("heap_free_internal", "blip.diagnostics")
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if client:
            try:
                for key, value in saved.items(): client.request("set", updates, key, value)
                report["policy_restored"] = all(get(key) == value for key, value in saved.items())
                if not report["policy_restored"]: report["passed"] = False
            except Exception as error:
                report["policy_restore_error"] = str(error); report["passed"] = False
            client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"board": report["board"], "passed": report["passed"], "checks": len(report["checks"]),
                      "error": report.get("error"), "report": str(args.report)}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__": raise SystemExit(main())
