"""Bind Simple/Advanced browser trials to device bundles and firmware images."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/wasm"))
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from collect_profile_evidence import PROFILES, app_partition_bytes, read_text
from collect_import_evidence import static_ram, symbols
from blip_wasm_hil import source_snapshot


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    paths = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard",
                                     "v2/components", "v2/firmware", "v2/web"], text=True).splitlines()
    snapshot = {path: sha(ROOT / path) for path in paths}
    checker = ROOT / "v2/tools/web/blip_web_modes_hil.mjs"
    bundle = ROOT / "v2/components/blip_storage/factory_web.bundle"
    bundle_bytes = bundle.read_bytes()
    builds = []
    for key, (board, chip, selector, _) in PROFILES.items():
        build = ROOT / f"build/m6-production-{key}"
        log = ROOT / f"build/m37-{key}-qualified-build.log"
        require("Project build complete" in read_text(log), f"{key}: build")
        config = (build / "sdkconfig").read_text()
        cache = (build / "CMakeCache.txt").read_text()
        require(f'CONFIG_IDF_TARGET="{chip}"' in config and "BLIP_ENABLE_WASM:BOOL=ON" in cache, f"{key}: profile")
        if selector:
            require(re.search(rf"^{selector}:[^=]+=ON$", cache, re.M), f"{key}: board")
        if key == "ball":
            require("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y" in config and 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-8mb.csv"' in config, "Ball 8 MiB")
        layout = json.loads(subprocess.check_output([sys.executable, str(args.idf / "tools/idf_size.py"), "--format", "json2", str(build / "blip-v2.map")], text=True))
        size = (build / "blip-v2.bin").stat().st_size
        require(size <= app_partition_bytes(build), f"{key}: app capacity")
        builds.append({"profile": key, "board": board, "target": chip, "app_bytes": size,
                       "static_ram_bytes": static_ram(layout), "build_log_sha256": sha(log),
                       "artifacts": {name: sha(build / name) for name in ("blip-v2.bin", "blip-v2.elf", "sdkconfig")}})
    trials = []
    for key in ("ball", "huzzah32", "m5dial"):
        path = ROOT / f"build/m37-{key}-qualified-browser-final.json"
        report = read(path)
        require(report["passed"] and len(report["checks"]) == 15 and all(c["passed"] for c in report["checks"]), f"{key}: browser")
        require(report["source_snapshot"] == snapshot and report["checker_sha256"] == sha(checker), f"{key}: browser sources")
        require(not report["pc_network_changed"] and report["playwright_version"] == "1.62.1", f"{key}: environment")
        assets = report["web_assets"]
        require(assets["bundle_version"] == 2000 and assets["bytes"] == len(bundle_bytes) and assets["crc32"] == f'{int.from_bytes(bundle_bytes[28:32], "little"):08x}', f"{key}: installed bundle")
        controls_path = ROOT / f"build/m37-{key}-qualified-controls.json"
        controls = read(controls_path)
        build = next(item for item in builds if item["profile"] == key)
        require(controls["passed"] and controls["checks"] and all(c["passed"] for c in controls["checks"]), f"{key}: control regression")
        require(controls["source_snapshot"] == source_snapshot(ROOT) and controls["artifacts"] == build["artifacts"], f"{key}: flashed image")
        log = ROOT / f"build/m37-{key}-qualified-flash.log"
        require(controls["flashed_mac"] == PROFILES[key][3] and controls["flash_log_sha256"] == sha(log), f"{key}: board flash")
        require(controls["worker_stack_headroom"] >= 1639 and not controls["pc_network_changed"], f"{key}: reserve/policy")
        screenshots = {mode: sha(Path(str(path) + f".{mode}.png")) for mode in ("simple", "advanced", "mobile")}
        trials.append({"profile": key, "browser_report_sha256": sha(path), "browser_trial": report,
                       "screenshots_sha256": screenshots, "controls_report_sha256": sha(controls_path),
                       "control_checks": len(controls["checks"]), "warmed_heap_min": min(controls["heap_samples"][2:]),
                       "warmed_heap_max": max(controls["heap_samples"][2:]), "worker_stack_headroom": controls["worker_stack_headroom"]})
    host = ROOT / "build/m37-host-ctest.log"
    web = ROOT / "build/m37-web-tests.log"
    require("100% tests passed out of 28" in read_text(host) and "tests 25" in read_text(web) and "fail 0" in read_text(web), "host/web tests")
    off = ROOT / "build/m6-providers-no-wasm"
    off_log = ROOT / "build/m37-no-wasm-qualified-build.log"
    require("Project build complete" in read_text(off_log) and "WamrRuntime" not in symbols(off, "blip-v2.elf"), "disabled build")
    failures = []
    for filename, reason in (
        ("m37-ball-browser-debug.json", "AP provisioning form intercepted the home page; app delegation now owns root and recovery remains at /setup."),
        ("m37-ball-direct-browser.json", "Parallel browser ESM requests exceeded four HTTP sessions and evicted responses; startup now loads modules sequentially."),
        ("m37-ball-sequential-browser.json", "Normal browser Accept header exceeded the 128-byte reader, selecting OSCQuery JSON; reader is now bounded at 256 bytes."),
        ("m37-ball-direct-sequential-browser.json", "Stored Window.fetch had a DeviceClient receiver and failed browser brand checks; native fetch is now bound to globalThis."),
    ):
        path = ROOT / "build" / filename
        require(not read(path)["passed"], f"{filename}: failure history")
        failures.append({"report_sha256": sha(path), "reason": reason})
    evidence = {"schema_version": 1, "work_package": "3.7", "checkpoint": "simple-advanced-browser-foundation",
                "created_at": datetime.now(timezone.utc).isoformat(), "repository_head_before_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "web_bundle_sha256": sha(bundle), "web_bundle_version": "0.2.0", "web_bundle_bytes": len(bundle_bytes),
                "source_snapshot": snapshot, "production_builds": builds, "browser_trials": trials,
                "browser_checks_passed": 45, "host_suites_passed": 28, "web_tests_passed": 25,
                "host_log_sha256": sha(host), "web_log_sha256": sha(web), "no_wasm_build_log_sha256": sha(off_log),
                "failed_trials": failures, "collector_sha256": sha(Path(__file__)), "full_work_package_complete": False,
                "remaining": ["complete diagnostic inspection", "lossless i64 and schema-bound transport requests in 6.6", "firmware/web self-updates in 3.8/3.9", "Gate B/C/D and endurance"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"browser_checks": 45, "boards": 3, "builds": len(builds)}))


if __name__ == "__main__":
    main()
