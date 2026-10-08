"""Validate live production controls against exact firmware and source snapshots."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_hil import source_snapshot
from blip_wasm_provider_hil import snapshot as provider_snapshot
from collect_profile_evidence import PROFILES, app_partition_bytes, read_text
from collect_import_evidence import static_ram, symbols


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    snapshot = source_snapshot(ROOT)
    checker = ROOT / "v2/tools/control/blip_script_controls_hil.py"
    checker_sha = hashlib.sha256(checker.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    dependency = ROOT / "build/wasm-deps/wamr"
    pin = subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip()
    require(pin == "25bd7eb63e828e4bd242cc9b38d260b4b31c6605" and
            not subprocess.check_output(["git", "-C", str(dependency), "status", "--porcelain"], text=True).strip(), "runtime pin")
    builds = []
    for profile, (board, chip, selector, _) in PROFILES.items():
        build = ROOT / f"build/m6-production-{profile}"
        log = ROOT / f"build/m6-live-controls-{profile}-build.log"
        require("Project build complete" in read_text(log), f"{profile}: build")
        config = (build / "sdkconfig").read_text()
        cache = (build / "CMakeCache.txt").read_text()
        require(f'CONFIG_IDF_TARGET="{chip}"' in config and "BLIP_ENABLE_WASM:BOOL=ON" in cache, f"{profile}: profile")
        if selector:
            require(re.search(rf"^{selector}:[^=]+=ON$", cache, re.M), f"{profile}: board")
        if profile == "ball":
            require("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y" in config and 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-8mb.csv"' in config, "Ball flash layout")
        linked = symbols(build, "blip-v2.elf")
        require("ScriptControlStore::publish(" in linked and "copy_script_action_arguments(" in linked, f"{profile}: linked owner")
        layout = json.loads(subprocess.check_output([sys.executable, str(args.idf / "tools/idf_size.py"), "--format", "json2", str(build / "blip-v2.map")], text=True))
        size = (build / "blip-v2.bin").stat().st_size
        partition = app_partition_bytes(build)
        require(size <= partition, f"{profile}: image capacity")
        builds.append({"profile": profile, "board": board, "chip": chip, "app_bytes": size,
                       "app_partition_bytes": partition, "static_ram_bytes": static_ram(layout),
                       "build_log_sha256": sha(log), "artifacts": {name: sha(build / name) for name in
                           ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}})
    trials = []
    for profile in ("ball", "huzzah32", "m5dial"):
        path = ROOT / f"build/m6-live-controls-{profile}-reserved-pass.json"
        trial = json.loads(path.read_text())
        build = next(item for item in builds if item["profile"] == profile)
        require(trial["passed"] and trial["checks"] and all(c["passed"] for c in trial["checks"]), f"{profile}: controls")
        require(trial["source_snapshot"] == snapshot and trial["checker_sha256"] == checker_sha, f"{profile}: sources")
        require(trial["board"] == PROFILES[profile][0] and trial["flashed_mac"] == PROFILES[profile][3], f"{profile}: identity")
        require(all(value == build["artifacts"][name] for name, value in trial["artifacts"].items()), f"{profile}: image")
        log = ROOT / f"build/m6-live-controls-{profile}-reserved-flash.log"
        require(trial["flash_log_sha256"] == sha(log), f"{profile}: flash")
        require(not trial["pc_network_changed"] and trial["production_script_owner_exercised"], f"{profile}: scope")
        samples = trial["heap_samples"][2:]
        require(len(samples) >= 18 and max(samples) - min(samples) <= 1024 and trial["worker_stack_headroom"] >= 1639, f"{profile}: repeat reserve")
        trials.append({"profile": profile, "report_sha256": sha(path), "trial": trial})
    regressions = []
    for profile in ("ball", "m5dial"):
        path = ROOT / f"build/m6-live-controls-{profile}-reserved-providers.json"
        report = json.loads(path.read_text())
        build = next(item for item in builds if item["profile"] == profile)
        require(report["passed"] and len(report["checks"]) == 95 and all(c["passed"] for c in report["checks"]), f"{profile}: providers")
        require(report["source_snapshot"] == provider_snapshot() and report["artifacts"] == build["artifacts"], f"{profile}: provider sources")
        require(report["metrics"]["worker_stack_headroom"] >= 1639 and not report["pc_network_changed"], f"{profile}: provider reserve")
        regressions.append({"profile": profile, "report_sha256": sha(path), "checks": 95, "metrics": report["metrics"]})
    failures = []
    for filename, explanation in (
        ("m6-live-controls-ball-trial.json", "Full fixed upload buffer left insufficient combined BLE/WASM HTTP heap; replaced with bounded upload-sized storage."),
        ("m6-live-controls-m5dial-full-qualified.json", "Aligned pool allocation after Wi-Fi startup failed; lifecycle owner now reserves the pool before service startup."),
        ("m6-live-controls-m5dial-reserved-qualified.json", "HIL request preceded application readiness after native USB download mode; captured ready boot and reran."),
    ):
        path = ROOT / "build" / filename
        report = json.loads(path.read_text())
        require(not report["passed"], f"{filename}: failure history")
        failures.append({"report_sha256": sha(path), "error": report.get("error"), "explanation": explanation})
    host = ROOT / "build/m6-live-controls-host-ctest.log"
    require("100% tests passed out of 28" in read_text(host), "host suites")
    off = ROOT / "build/m6-providers-no-wasm"
    off_log = ROOT / "build/m6-live-controls-no-wasm-build.log"
    require("Project build complete" in read_text(off_log) and "WamrRuntime" not in symbols(off, "blip-v2.elf"), "WASM disabled")
    output = {"schema_version": 1, "work_package": "6.6", "checkpoint": "production-live-controls",
              "created_at": datetime.now(timezone.utc).isoformat(), "repository_head_before_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
              "source_snapshot": snapshot, "wamr_commit": pin, "production_builds": builds,
              "live_control_trials": trials, "provider_regressions": regressions, "failed_trials": failures,
              "host_suites_passed": 28, "host_log_sha256": sha(host), "no_wasm_build_log_sha256": sha(off_log),
              "evidence_collector_sha256": hashlib.sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
              "full_work_package_complete": False, "full_plan_complete": False,
              "remaining": ["guest parameter/event imports", "copied external event delivery", "transport schema tokens and lossless i64", "web schema refresh and browser HIL", "SDK", "Gate C/D and endurance"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps({"builds": len(builds), "live_boards": len(trials), "provider_checks": 190}))


if __name__ == "__main__":
    main()
