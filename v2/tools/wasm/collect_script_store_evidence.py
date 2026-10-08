"""Bind script storage qualification to exact sources, images and production builds."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/qualification/script-controls"))
import run_controls
from collect_profile_evidence import PROFILES, app_partition_bytes, read_text
from collect_import_evidence import symbols, static_ram
from check_control_transports import sources as transport_sources
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_provider_hil import snapshot as provider_sources


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(read_text(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    dependency = ROOT / "build/wasm-deps/wamr"
    pin = "25bd7eb63e828e4bd242cc9b38d260b4b31c6605"
    require(subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip() == pin, "runtime pin")
    require(not subprocess.check_output(["git", "-C", str(dependency), "status", "--porcelain"], text=True).strip(), "modified runtime checkout")
    native_sources = run_controls.snapshot()
    native = []
    for board, chip in run_controls.TARGETS.items():
        path = ROOT / f"build/m6-script-store-{board}-trial.json"
        trial = read(path); run_controls.validate(trial)
        require(trial["passed"] and trial["chip"] == chip and trial["board"] == board, f"{board}: identity/result")
        require(trial["source_snapshot"] == native_sources and trial["flashed_mac"] == run_controls.MACS[board], f"{board}: source/MAC")
        require(not trial["pc_network_changed"] and not trial["firmware_restore_requested"] and not trial["production_script_owner_exercised"], f"{board}: qualifier scope")
        require(trial["interpreter_exercised"] and trial["concurrent_schema_retirement_exercised"] and trial["test_firmware_left_installed"], f"{board}: qualifier policy")
        build = ROOT / f"build/m6-script-store-{board}"
        for name, info in trial["inputs"].items():
            require((build / name).stat().st_size == info["bytes"] and sha(build / name) == info["sha256"], f"{board}: artifact {name}")
        log = ROOT / f"build/m6-script-store-{board}-build.log"
        require("Project build complete" in read_text(log), f"{board}: build")
        frames = []
        for filename, limit in (("script_controls.cpp.su", 512), ("wamr_runtime.cpp.su", 3072)):
            paths = list(build.rglob(filename)); require(len(paths) == 1, f"{board}: compiler frames {filename}")
            for line in paths[0].read_text().splitlines():
                function, size, kind = line.rsplit("\t", 2)
                if "blip::wasm::" not in function:
                    continue
                frame = {"function": function.replace(ROOT.as_posix(), "/BLIP"), "bytes": int(size), "kind": kind}
                require(kind == "static" and int(size) <= limit, f"{board}: worker function frame")
                frames.append(frame)
        linked = symbols(build, "blip-script-controls.elf")
        require("ScriptControlStore::publish(" in linked and "WamrRuntime::immutable_i32_global(" in linked, f"{board}: actual store/engine linked")
        native.append({"board": board, "chip": chip, "flashed_mac": trial["flashed_mac"], "records": trial["records"],
            "inputs": trial["inputs"], "report_sha256": sha(path), "build_log_sha256": sha(log), "compiler_frames": frames,
            "shared_case_passes": 126, "engine_checks": 32, "flash_log_sha256": hashlib.sha256(trial["flash_log"].encode()).hexdigest()})
    previous_path = ROOT / "docs/v2/evidence/wasm/2026-10-08-dynamic-registry-interfaces.json"
    previous = read(previous_path)
    builds = []
    for key, (board, chip, selector, _) in PROFILES.items():
        build = ROOT / f"build/m6-production-{key}"
        suffix = "ball" if key == "ball" else f"production-{key}"
        log = ROOT / f"build/m6-script-store-{suffix}-build.log"
        require("Project build complete" in read_text(log), f"{key}: production build")
        config = (build / "sdkconfig").read_text(); cache = (build / "CMakeCache.txt").read_text()
        require(f'CONFIG_IDF_TARGET="{chip}"' in config, f"{key}: target")
        for feature in ("WASM", "ESPNOW", "FLEET", "DDP", "ARTNET"):
            require(f"BLIP_ENABLE_{feature}:BOOL=ON" in cache, f"{key}: feature {feature}")
        if selector:
            require(re.search(rf"^{selector}:[^=]+=ON$", cache, re.M), f"{key}: board")
        if key == "ball":
            require("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y" in config and 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-8mb.csv"' in config, "Ball 8 MiB layout")
            flash = read(build / "flasher_args.json")
            require(flash["flash_settings"]["flash_size"] == "8MB" and "0x620000" in flash["flash_files"], "Ball generated flash arguments")
        linked = symbols(build, "blip-v2.elf")
        require("WamrRuntime::immutable_i32_global(" in linked and "ScriptControlStore::" not in linked, f"{key}: production checkpoint boundary")
        image = (build / "blip-v2.bin").stat().st_size; partition = app_partition_bytes(build)
        require(image <= partition, f"{key}: application capacity")
        layout = json.loads(subprocess.check_output([sys.executable, str(args.idf / "tools/idf_size.py"), "--format", "json2", str(build / "blip-v2.map")], text=True))
        ram = static_ram(layout); old = next(item for item in previous["production_builds"] if item["profile"] == key)
        builds.append({"profile": key, "board": board, "chip": chip, "app_bytes": image, "app_partition_bytes": partition,
            "static_ram_bytes": ram, "app_delta_from_registry_bytes": image - old["app_bytes"], "static_ram_delta_from_registry_bytes": ram - old["static_ram_bytes"],
            "build_log_sha256": sha(log), "artifacts": {name: sha(build / name) for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}})
    off = ROOT / "build/m6-providers-no-wasm"
    off_log = ROOT / "build/m6-script-store-no-wasm-build.log"
    require("Project build complete" in read_text(off_log) and "BLIP_ENABLE_WASM:BOOL=OFF" in (off / "CMakeCache.txt").read_text(), "disabled build")
    require("WamrRuntime" not in symbols(off, "blip-v2.elf"), "disabled runtime linked")
    transport_path = ROOT / "build/m6-script-store-production-ball-transports.json"
    transport = read(transport_path)
    provider_path = ROOT / "build/m6-script-store-production-ball-trial.json"
    provider = read(provider_path)
    ball = next(b for b in builds if b["profile"] == "ball")
    require(transport["passed"] and len(transport["checks"]) == 18 and all(c["passed"] for c in transport["checks"]), "Ball transports")
    require(provider["passed"] and len(provider["checks"]) == 95 and all(c["passed"] for c in provider["checks"]), "Ball providers")
    require(transport["source_snapshot"] == transport_sources() and provider["source_snapshot"] == provider_sources(), "production trial sources")
    require(transport["artifacts"] == provider["artifacts"] == ball["artifacts"], "production trial artifacts")
    require(transport["flashed_mac"] == PROFILES["ball"][3] and not transport["guest_defined_controls_exercised"], "Ball identity/scope")
    require(not transport["pc_network_changed"] and not provider["pc_network_changed"] and transport["test_firmware_left_installed"] and provider["test_firmware_left_installed"], "Ball network/firmware policy")
    require(min(transport["stack_headroom"].values()) >= 1024 and provider["metrics"]["worker_stack_headroom"] >= 1024, "Ball transport/worker headroom")
    host = ROOT / "build/m6-script-store-host-ctest.log"
    red = ROOT / "build/m6-script-store-red-build.log"
    require("100% tests passed out of 27" in read_text(host), "host suites")
    require("LNK2019" in read_text(red) and "ScriptControlStore" in read_text(red), "initial unimplemented API failure")
    initial = ROOT / "build/m6-script-store-xiao-initial-trial.json"
    require(not read(initial)["passed"], "initial native fixture failure")
    output = {"schema_version": 1, "work_package": "6.6", "checkpoint": "owned-script-store",
        "created_at": datetime.now(timezone.utc).isoformat(), "repository_head_before_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "wamr_commit": pin, "source_snapshot": native_sources, "native_trials": native, "production_builds": builds,
        "evidence_collector_sha256": hashlib.sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
        "native_check_passes": 474, "host_suites_passed": 27, "host_log_sha256": sha(host), "initial_host_link_failure_sha256": sha(red),
        "initial_native_trial_sha256": sha(initial), "initial_native_failure": "Test reader used sched_yield at higher priority than its owner; bounded sleeps and an independent supervisor fixed the fixture.",
        "previous_checkpoint_sha256": sha(previous_path), "no_wasm_build_log_sha256": sha(off_log),
        "ball_production": {"transport_trial": transport, "provider_report_sha256": sha(provider_path), "provider_checks": 95, "provider_metrics": provider["metrics"]},
        "production_script_store_linked": False, "guest_defined_controls_in_production_exercised": False,
        "full_work_package_complete": False, "full_plan_complete": False,
        "remaining": ["Production worker ownership and load/unload/fault retirement", "Supervised dynamic action admission and completion",
            "Guest parameter/event imports and owned event delivery", "External generation tokens, lossless integer encoding and automatic web schema refresh",
            "Concurrent production transports/reloads and cold-start failure qualification", "Generated bindings/examples and remaining 6.7 surfaces",
            "Full Gate C/D, remaining milestones 7-9 and all documented physical/soak gates"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "native_checks": 474, "host_suites": 27, "production_profiles": len(builds), "ball_regression_checks": 113}))


if __name__ == "__main__":
    main()
