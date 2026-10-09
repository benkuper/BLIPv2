"""Qualify embedded release metadata, actual images, packaging and three boards."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from release_publish import firmware_metadata, publish
from blip_release_identity_hil import snapshot

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/wasm"))
from collect_profile_evidence import PROFILES, read_text, app_partition_bytes
from collect_import_evidence import static_ram
from blip_wasm_hil import source_snapshot as wasm_snapshot


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    cpp = ROOT / "build/host/Debug/blip_release_image_tests.exe"
    source = snapshot()
    builds = []
    for name, (board, target, _, _) in PROFILES.items():
        directory = ROOT / f"build/m6-production-{name}"
        image = directory / "blip-v2.bin"
        metadata, code, version = firmware_metadata(image)
        log = ROOT / f"build/m38-image-{name}-build.log"
        require("Project build complete" in read_text(log), f"{name}: build")
        cache = (directory / "CMakeCache.txt").read_text()
        config = (directory / "sdkconfig").read_text()
        expected_features = sum(1 << bit for bit, feature in enumerate(("WASM", "BLE", "CLASSIC_BT", "ESPNOW", "FLEET", "ARTNET", "DDP", "E131"))
                                if re.search(rf"^BLIP_ENABLE_{feature}:BOOL=ON$", cache, re.M))
        require(metadata["board"] == board and metadata["target"] == target and metadata["features"] == expected_features,
                f"{name}: independent board/feature identity")
        require(code == 1 and version == "0.1.0" and metadata["profile"] == "minimal", f"{name}: sequence/version/profile")
        flash_bytes = 8388608 if name == "ball" else 4194304
        layout = "ota-8mb-v1" if name == "ball" else "ota-4mb-v1"
        require(metadata["flash_bytes"] == flash_bytes and metadata["layout"] == layout, f"{name}: layout")
        require(("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y" if name == "ball" else "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y") in config, f"{name}: configured flash")
        command = [str(cpp), str(image), board, target, layout, "minimal", str(flash_bytes), str(expected_features), str(code), version]
        require(subprocess.run(command, capture_output=True).returncode == 0, f"{name}: C++ actual binary validation")
        for index, bad in ((2, "wrong-board"), (4, "wrong-layout"), (5, "wrong-profile"), (6, str(flash_bytes ^ 4194304)),
                           (7, str(expected_features ^ 1)), (8, "2"), (9, "9.9.9")):
            mismatch = command.copy(); mismatch[index] = bad
            require(subprocess.run(mismatch, capture_output=True).returncode == 1, f"{name}: C++ mismatch rejection")
        output = ROOT / "build/m38-image-packaged"
        row = publish(image, ROOT / "v2/components/blip_storage/factory_web.bundle", "https://www.goldengeek.org/blip/releases",
                      "stable", 0, 1, output / "releases.json", output / "artifacts")
        require(row["firmware"]["sha256"] == digest(image), f"{name}: packaged digest")
        size = image.stat().st_size
        require(size <= app_partition_bytes(directory), f"{name}: app capacity")
        size_data = json.loads(subprocess.check_output([sys.executable, str(args.idf / "tools/idf_size.py"), "--format", "json2", str(directory / "blip-v2.map")], text=True))
        builds.append({"profile": name, "identity": metadata, "release_code": code, "version": version,
                       "app_bytes": size, "static_ram_bytes": static_ram(size_data), "app_partition_bytes": app_partition_bytes(directory),
                       "cpp_binary_checks": 8, "build_log_sha256": digest(log),
                       "artifacts": {file: digest(directory / file) for file in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}})
    trials = []
    for name in ("ball", "huzzah32", "m5dial"):
        path = ROOT / f"build/m38-image-{name}-identity.json"
        trial = json.loads(path.read_text(encoding="utf-8"))
        require(trial["passed"] and len(trial["checks"]) == 16 and all(check["passed"] for check in trial["checks"]), f"{name}: HIL")
        require(trial["source_snapshot"] == source and not trial["pc_network_changed"], f"{name}: source/network")
        build = next(item for item in builds if item["profile"] == name)
        require(trial["artifacts"] == build["artifacts"], f"{name}: HIL image")
        controls_path = ROOT / f"build/m38-image-{name}-controls.json"
        controls = json.loads(controls_path.read_text(encoding="utf-8"))
        require(controls["passed"] and len(controls["checks"]) == 99 and all(check["passed"] for check in controls["checks"]), f"{name}: script regression")
        require(set(controls["artifacts"]) == {"blip-v2.bin", "blip-v2.elf", "sdkconfig"} and
                all(value == build["artifacts"][file] for file, value in controls["artifacts"].items()) and
                controls["source_snapshot"] == wasm_snapshot(ROOT), f"{name}: regression source/image")
        require(not controls["pc_network_changed"] and controls["flashed_mac"] == PROFILES[name][3], f"{name}: regression board/network")
        samples = controls["heap_samples"][2:]
        require(len(samples) >= 18 and max(samples) - min(samples) <= 1024 and controls["worker_stack_headroom"] >= 1639, f"{name}: regression reserves")
        trials.append({"profile": name, "report_sha256": digest(path), "checks": 16,
                       "flash_log_sha256": trial["flash_log_sha256"], "heap_free_internal": trial["heap_free_internal"],
                       "heap_largest_internal": trial["heap_largest_internal"], "script_regression_sha256": digest(controls_path),
                       "script_regression_checks": 99, "warm_heap_minimum": min(samples), "warm_heap_maximum": max(samples),
                       "worker_stack_headroom": controls["worker_stack_headroom"]})
    logs = {name: ROOT / f"build/{name}.log" for name in ("m38-image-host-tests", "m38-release-publish-tests", "m38-image-server-tests")}
    require("100% tests passed out of 30" in read_text(logs["m38-image-host-tests"]), "host suites")
    for name, count in (("m38-release-publish-tests", 4), ("m38-image-server-tests", 5)):
        require(f"Ran {count} tests" in read_text(logs[name]) and "\nOK" in read_text(logs[name]), name)
    no_wasm = ROOT / "build/m6-providers-no-wasm"
    no_wasm_log = ROOT / "build/m38-image-no-wasm-build.log"
    require("Project build complete" in read_text(no_wasm_log) and "BLIP_ENABLE_WASM:BOOL=OFF" in (no_wasm / "CMakeCache.txt").read_text(), "WASM-disabled build")
    result = {"recorded_at": datetime.now(timezone.utc).isoformat(), "passed": True, "source_snapshot": source,
              "builds": builds, "trials": trials, "host_suites": 30, "python_tests": 9,
              "cpp_actual_binary_checks": 56, "serial_oscquery_hil_checks": 48,
              "production_script_regression_checks": 297,
              "no_wasm_build": {"log_sha256": digest(no_wasm_log), "application_sha256": digest(no_wasm / "blip-v2.bin")},
              "test_logs": {name: digest(path) for name, path in logs.items()},
              "published_index_sha256": digest(ROOT / "build/m38-image-packaged/releases.json"),
              "goldengeek_catalog_deployed": False, "native_https_worker_qualified": False,
              "remaining": ["native HTTPS worker/download", "update center", "automatic policy", "remote adverse-path HIL"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "profiles": 7, "binary_checks": 56, "hardware_checks": 48}))


if __name__ == "__main__":
    main()
