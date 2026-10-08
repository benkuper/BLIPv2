"""Bind the leased registry interface checkpoint to source, builds and trials."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/qualification/dynamic-controls"))
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
    require(subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip() ==
            "25bd7eb63e828e4bd242cc9b38d260b4b31c6605", "runtime pin")
    require(not subprocess.check_output(["git", "-C", str(dependency), "status", "--porcelain"], text=True).strip(), "modified runtime checkout")
    native_sources = run_controls.snapshot()
    native = []
    for board, chip in run_controls.TARGETS.items():
        path = ROOT / f"build/m6-dynamic-controls-{board}-trial.json"
        trial = read(path)
        run_controls.validate(trial)
        require(trial["passed"] and trial["chip"] == chip and trial["board"] == board, f"{board}: identity/result")
        require(trial["source_snapshot"] == native_sources and trial["flashed_mac"] == run_controls.MACS[board], f"{board}: source/MAC")
        require(not any(trial[key] for key in ("pc_network_changed", "firmware_restore_requested", "interpreter_exercised",
                "production_script_owner_exercised", "concurrent_schema_replacement_exercised")), f"{board}: qualifier scope")
        require(trial["test_firmware_left_installed"], f"{board}: qualifier policy")
        build = ROOT / f"build/m6-dynamic-controls-{board}"
        for name, info in trial["inputs"].items():
            require((build / name).stat().st_size == info["bytes"] and sha(build / name) == info["sha256"], f"{board}: artifact {name}")
        log = ROOT / f"build/m6-dynamic-controls-{board}-build.log"
        require("Project build complete" in read_text(log), f"{board}: build")
        frames = []
        for filename in ("oscquery.cpp.su", "legacy_osc.cpp.su", "blip_dynamic_controls_tests.cpp.su"):
            paths = list(build.rglob(filename))
            require(len(paths) == 1, f"{board}: compiler frames {filename}")
            for line in paths[0].read_text().splitlines():
                function, size, kind = line.rsplit("\t", 2)
                if "blip::" not in function:
                    continue
                frame = {"function": function.replace(ROOT.as_posix(), "/BLIP"), "bytes": int(size), "kind": kind}
                require(kind == "static" and int(size) <= 3072, f"{board}: portable core frame")
                frames.append(frame)
        native.append({"board": board, "chip": chip, "flashed_mac": trial["flashed_mac"], "records": trial["records"],
                       "inputs": trial["inputs"], "report_sha256": sha(path), "build_log_sha256": sha(log),
                       "shared_case_passes": sum(line.startswith("PASS ") for line in trial["console"]),
                       "compiler_frames": frames, "flash_log_sha256": hashlib.sha256(trial["flash_log"].encode()).hexdigest()})
    previous = read(ROOT / "docs/v2/evidence/wasm/2026-10-08-production-providers.json")
    builds = []
    size_tool = args.idf / "tools/idf_size.py"
    for key, (board, chip, selector, _) in PROFILES.items():
        build = ROOT / f"build/m6-production-{key}"
        log = ROOT / f"build/m6-dynamic-schema-production-{key}-build.log"
        require("Project build complete" in read_text(log), f"{key}: production build")
        config = (build / "sdkconfig").read_text()
        cache = (build / "CMakeCache.txt").read_text()
        require(f'CONFIG_IDF_TARGET="{chip}"' in config, f"{key}: target")
        for feature in ("WASM", "ESPNOW", "FLEET", "DDP", "ARTNET"):
            require(f"BLIP_ENABLE_{feature}:BOOL=ON" in cache, f"{key}: feature {feature}")
        if selector:
            require(re.search(rf"^{selector}:[^=]+=ON$", cache, re.M), f"{key}: board")
        if key == "ball":
            require("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y" in config and 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-8mb.csv"' in config,
                    "Ball is not using its 8 MiB layout")
            flash = read(build / "flasher_args.json")
            require(flash["flash_settings"]["flash_size"] == "8MB" and "0x620000" in flash["flash_files"], "Ball flash arguments")
        image_bytes = (build / "blip-v2.bin").stat().st_size
        partition_bytes = app_partition_bytes(build)
        require(image_bytes <= partition_bytes, f"{key}: app capacity")
        linked = symbols(build, "blip-v2.elf")
        require("WamrRuntime::raw_capability(" in linked and "DynamicSchemaLease" in linked, f"{key}: worker/schema linked")
        layout = json.loads(subprocess.check_output([sys.executable, str(size_tool), "--format", "json2", str(build / "blip-v2.map")], text=True))
        old = next(item for item in previous["production_builds"] if item["profile"] == key)
        ram = static_ram(layout)
        builds.append({"profile": key, "board": board, "chip": chip, "app_bytes": image_bytes, "app_partition_bytes": partition_bytes,
                       "static_ram_bytes": ram, "app_delta_from_providers_bytes": image_bytes - old["app_bytes"],
                       "static_ram_delta_from_providers_bytes": ram - old["static_ram_bytes"], "build_log_sha256": sha(log),
                       "artifacts": {name: sha(build / name) for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}})
    off = ROOT / "build/m6-providers-no-wasm"
    require("Project build complete" in read_text(ROOT / "build/m6-dynamic-schema-no-wasm-build.log"), "disabled build")
    require("BLIP_ENABLE_WASM:BOOL=OFF" in (off / "CMakeCache.txt").read_text(), "disabled config")
    require("WamrRuntime" not in symbols(off, "blip-v2.elf"), "disabled runtime linked")
    production = []
    for key in ("ball", "club", "m5dial"):
        path = ROOT / f"build/m6-dynamic-schema-production-{key}-transports.json"
        trial = read(path)
        require(trial["passed"] and len(trial["checks"]) == 18 and all(c["passed"] for c in trial["checks"]), f"{key}: transports")
        require(trial["source_snapshot"] == transport_sources(), f"{key}: transport source")
        require(trial["artifacts"] == next(b["artifacts"] for b in builds if b["profile"] == key), f"{key}: transport artifacts")
        require(trial["flashed_mac"] == PROFILES[key][3] and not trial["pc_network_changed"] and
                not trial["guest_defined_controls_exercised"] and trial["test_firmware_left_installed"], f"{key}: transport scope/identity")
        require(min(trial["stack_headroom"].values()) >= 1024, f"{key}: transport stack margin")
        provider = read(ROOT / f"build/m6-dynamic-schema-production-{key}-trial.json")
        require(provider["passed"] and len(provider["checks"]) == 95 and all(c["passed"] for c in provider["checks"]), f"{key}: providers")
        require(provider["source_snapshot"] == provider_sources() and provider["artifacts"] == trial["artifacts"], f"{key}: provider input")
        production.append({"profile": key, "transport_trial": trial, "transport_report_sha256": sha(path),
                           "provider_report_sha256": sha(ROOT / f"build/m6-dynamic-schema-production-{key}-trial.json"),
                           "provider_checks": 95, "provider_metrics": provider["metrics"]})
    host_log = ROOT / "build/m6-dynamic-schema-host-ctest.log"
    require("100% tests passed out of 26" in read_text(host_log), "host suites")
    require("FAIL" in read_text(ROOT / "build/m6-dynamic-schema-red-test.log"), "initial dispatcher failure")
    web = read(ROOT / "build/m6-dynamic-schema-web-model.json")
    tree_path = ROOT / "build/m6-dynamic-schema-tree.json"
    normalized_sha = lambda p: hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    require(web["passed"] and web["controls"] == 4 and web["schema_generation"] == 1 and web["tree_sha256"] == sha(tree_path), "web fixture")
    require(web["checker_sha256"] == normalized_sha(ROOT / "v2/tools/wasm/check_dynamic_schema_web.mjs") and
            web["model_sha256"] == normalized_sha(ROOT / "v2/web/src/model.js"), "web sources")
    web_tests = ROOT / "build/m6-dynamic-schema-web-tests.log"
    require("pass 21" in read_text(web_tests) and "fail 0" in read_text(web_tests), "web tests")
    evidence = {"recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": True,
        "scope": "Leased dynamic registry/control/OSCQuery interfaces; production script owner and guest controls remain pending",
        "collector_sha256": sha(Path(__file__)), "native_source_snapshot": native_sources,
        "host": {"suites": 26, "dynamic_cases": 6, "ctest_log_sha256": sha(host_log),
                 "initial_failing_dispatcher_log_sha256": sha(ROOT / "build/m6-dynamic-schema-red-test.log")},
        "web_model": web, "web_tests": {"passed": 21, "log_sha256": sha(web_tests)},
        "native": native, "native_case_passes": 378, "production_builds": builds,
        "production_trials": production, "production_checks": 339,
        "wasm_disabled_artifacts": {name: sha(off / name) for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")},
        "limitations": [
            "Native schema owner is a single-threaded surrogate with a pin counter; this is not concurrent publication/retirement evidence",
            "Production has no script schema owner, value store, supervised action callback bridge or event delivery yet",
            "The web check feeds generated C++ JSON to the existing model; no browser automation or live guest-defined UI is claimed",
            "Explicit control generation tokens reject stale dynamic dispatch; existing external wire requests without tokens resolve the current generation",
            "Existing OSC/browser numeric limits still apply; lossless full i64 script transport encoding remains needed",
            "Schema source owners must keep metadata alive through leases, forbid collisions and recheck retired tokens at admission",
            "Production transport checks use existing static string controls and full trees, not a maximum-size live script schema",
            "The 20 KiB qualifier stack supports the allocating shared test fixtures; compiler frames are individual functions rather than full call chains",
            "Four other production profiles are compile-only in this checkpoint; full 6.6/6.7, concurrency/lifecycle stress, Gate C/D and soaks remain open"
        ]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "host_suites": 26, "native_case_passes": 378, "production_checks": 339, "production_profiles": 7}))


if __name__ == "__main__":
    main()
