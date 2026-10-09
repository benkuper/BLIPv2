"""Validate actual native imports on three families and six required production builds."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/qualification/wasm-imports"))
from run_imports import PIN, snapshot, validate
from collect_profile_evidence import PROFILES, app_partition_bytes, read_text


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symbols(build, name):
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    nm = re.search(r"^CMAKE_NM:FILEPATH=(.+)$", cache, re.M)[1]
    return subprocess.check_output([nm, "--demangle", str(build / name)], text=True)


def static_ram(size):
    return sum(part["size"] for region in size["layout"] if region["name"] in ("DRAM", "DIRAM")
               for name, part in region["parts"].items() if name in (".data", ".bss", ".noinit"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    dependency = ROOT / "build/wasm-deps/wamr"
    if subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip() != PIN or \
       subprocess.check_output(["git", "-C", str(dependency), "status", "--porcelain"], text=True).strip():
        raise RuntimeError("Runtime checkout differs")
    sources = snapshot()
    fixture = (ROOT / "v2/qualification/wasm-imports/main/fixtures.hpp").read_text(encoding="utf-8")
    fixture_hash = re.search(r'valid_sha256\[\] = "([0-9a-f]+)"', fixture)[1]
    native = []
    for key in ("huzzah32", "m5dial", "xiao"):
        trial_path = ROOT / f"build/m6-imports-{key}-trial.json"
        trial = json.loads(trial_path.read_text(encoding="utf-8"))
        validate(trial)
        if not trial["passed"] or trial["source_snapshot"] != sources or trial["wamr_commit"] != PIN:
            raise RuntimeError(f"Native source/runtime trial differs: {key}")
        board, chip, _, mac = PROFILES[key]
        displayed = trial["flashed_mac"].split(":")
        if len(displayed) == 8 and displayed[3:5] == ["ff", "fe"]:
            displayed = displayed[:3] + displayed[5:]
        if (trial["board"], trial["chip"], ":".join(displayed)) != (board, chip, mac):
            raise RuntimeError(f"Native board identity differs: {key}")
        if trial["pc_network_changed"] or trial["firmware_restore_requested"] or not trial["test_firmware_left_installed"]:
            raise RuntimeError("Unexpected network change or firmware restoration")
        directory = ROOT / f"build/m6-imports-{key}"
        log = ROOT / f"build/m6-imports-{key}-build.log"
        if "Project build complete" not in read_text(log):
            raise RuntimeError(f"Native build incomplete: {key}")
        for name, info in trial["inputs"].items():
            artifact = directory / name
            if artifact.stat().st_size != info["bytes"] or sha(artifact) != info["sha256"]:
                raise RuntimeError(f"Native artifact changed: {key}/{name}")
        if trial["flashed_app_sha256"] != trial["inputs"]["blip-wasm-imports.bin"]["sha256"]:
            raise RuntimeError("Flashed native image differs")
        linked = symbols(directory, "blip-wasm-imports.elf")
        for name in ("CapabilityRegistry::bind(", "CapabilityRegistry::invoke(", "WamrRuntime::raw_capability(",
                     "WamrNativeBridge::dispatch(", "__wrap_wasm_runtime_register_natives_raw"):
            if name not in linked:
                raise RuntimeError(f"Native bridge/fault injection not linked: {key}/{name}")
        start, end = trial["records"][0], trial["records"][-1]
        if start["fixture_sha256"] != fixture_hash or end["native_calls"] < 600 or end["native_failures"] != 10:
            raise RuntimeError("Fixture/native dispatch evidence differs")
        if end["native_maximum_us"] <= 2000:
            raise RuntimeError("Injected callback overrun was not observed")
        config = (directory / "sdkconfig").read_text()
        if f'CONFIG_IDF_TARGET="{chip}"' not in config or "CONFIG_FREERTOS_HZ=1000" not in config:
            raise RuntimeError(f"Native target/tick differs: {key}")
        if chip == "esp32" and not all(f"{name}=y" in config for name in (
            "CONFIG_FREERTOS_UNICORE", "CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY")):
            raise RuntimeError("Native ESP32 placement differs")
        native.append({"profile": key, "checks": end["checks"], "metrics": end,
            "trial_sha256": sha(trial_path), "build_log_sha256": sha(log), "trial": trial})
    previous = json.loads((ROOT / "docs/v2/evidence/wasm/2026-10-08-component-provider-contract.json").read_text())
    builds = []
    size_tool = args.idf / "tools/idf_size.py"
    for key, (board, chip, selector, _) in PROFILES.items():
        directory = ROOT / f"build/m6-production-{key}"
        log = ROOT / f"build/m6-imports-production-{key}-build.log"
        if "Project build complete" not in read_text(log):
            raise RuntimeError(f"Production build incomplete: {key}")
        cache = (directory / "CMakeCache.txt").read_text()
        config = (directory / "sdkconfig").read_text()
        if f'CONFIG_IDF_TARGET="{chip}"' not in config or "CONFIG_FREERTOS_HZ=1000" not in config:
            raise RuntimeError(f"Target/tick differs: {key}")
        for feature in ("WASM", "ESPNOW", "FLEET", "ARTNET", "DDP"):
            if f"BLIP_ENABLE_{feature}:BOOL=ON" not in cache:
                raise RuntimeError(f"Feature missing: {key}/{feature}")
        if key == "ball" and "BLIP_ENABLE_BLE:BOOL=ON" not in cache:
            raise RuntimeError("Ball BLE differs")
        if selector and not re.search(rf"^{selector}:[^=]+=ON$", cache, re.M):
            raise RuntimeError(f"Board selector differs: {key}")
        if chip == "esp32" and not all(f"{name}=y" in config for name in (
            "CONFIG_FREERTOS_UNICORE", "CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY")):
            raise RuntimeError("ESP32 placement differs")
        image_size = (directory / "blip-v2.bin").stat().st_size
        partition = app_partition_bytes(directory)
        if image_size > partition:
            raise RuntimeError("App partition exceeded")
        linked = symbols(directory, "blip-v2.elf")
        for name in ("blip::core::valid_wasm_descriptor(", "WamrRuntime::raw_capability(", "CapabilityRegistry::check_import("):
            if name not in linked:
                raise RuntimeError(f"Production bridge not linked: {key}/{name}")
        mapped = (directory / "blip-v2.map").read_text()
        if "__wrap_uart_write_bytes" in mapped or "__wrap_wasm_runtime_register_natives_raw" in mapped:
            raise RuntimeError("Qualification wrapper unexpectedly linked")
        linked_size = json.loads(subprocess.check_output([
            sys.executable, str(size_tool), "--format", "json2", str(directory / "blip-v2.map")], text=True))
        old = next(b for b in previous["production_builds"] if b["profile"] == key)
        builds.append({"profile": key, "board": board, "chip": chip, "app_bytes": image_size,
            "app_partition_bytes": partition, "prior_provider_contract_app_bytes": old["app_bytes"],
            "app_delta_bytes": image_size - old["app_bytes"], "linked_size": linked_size,
            "static_ram_bytes": static_ram(linked_size), "prior_provider_contract_static_ram_bytes": old["static_ram_bytes"],
            "static_ram_delta_bytes": static_ram(linked_size) - old["static_ram_bytes"], "build_log_sha256": sha(log),
            "native_bridge_linked": True, "production_catalog_configured": False, "flashed_in_this_trial": False,
            "artifacts": {name: {"bytes": (directory / name).stat().st_size, "sha256": sha(directory / name)}
                for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}})
    host_log = ROOT / "build/m6-imports-host-ctest.log"
    if "100% tests passed" not in read_text(host_log) or "out of 24" not in read_text(host_log):
        raise RuntimeError("Host suite incomplete")
    red_logs = [ROOT / "build/m6-imports-host-red.log", ROOT / "build/m6-imports-policy-host-red.log"]
    for log, symbol in zip(red_logs, ("check_import", "passive_module")):
        if "LNK2019" not in read_text(log) or symbol not in read_text(log):
            raise RuntimeError("Tests-first evidence missing")
    evidence = {"schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": True,
        "result": "actual guest-to-native capability bridge qualified on ESP32, S3 and C6; production providers pending",
        "source_snapshot": sources, "runtime": {"commit": PIN, "checkout_unmodified": True},
        "fixture_sha256": fixture_hash, "host_tests": 24, "provider_host_cases": 12,
        "host_log_sha256": sha(host_log), "tests_first_logs": {p.name: sha(p) for p in red_logs},
        "host_sources": {name: sha(ROOT / "v2/tests/host" / name) for name in (
            "CMakeLists.txt", "blip_wasm_capability_tests.cpp", "test_harness.hpp")},
        "host_executable_sha256": sha(ROOT / "build/host-m4/Release/blip_wasm_capability_tests.exe"),
        "native_checks": sum(t["checks"] for t in native), "native_trials": native,
        "production_builds": builds, "size_tool_sha256": sha(size_tool), "collector_sha256": sha(Path(__file__)),
        "native_imports_exercised": True, "limitations": [
            "Native qualification uses two synthetic component-owned providers, eleven registered functions and 32 imported function slots. It does not exercise a fully occupied eight-provider/32-function native registration catalog.",
            "Six required production images were built but not flashed. No production catalog is configured yet, so they still refuse nonempty imports. LED/fleet providers and worker-before-provider teardown remain the next 6.5 checkpoint.",
            "Trusted native callbacks cannot be forcibly preempted. Elapsed time is checked after return; cancellation is cooperative and does not roll back completed effects. The largest reported callback deliberately delays about 10 ms to test overrun faults.",
            "Native callback cancellation uses the main task to call request_cancel; production supervisor/provider coexistence, radio/render stress, allocator exhaustion, fragmentation, soaks and full Gate D remain open.",
            "The registration table and nodes use the existing fixed 80 KiB engine budget without SDK heap fallback. Production pool/module/native task reservations are unchanged; production runtime heap was not remeasured.",
            "M5StickC was absent from serial inventory and was compiled only. Its large serial burst recovery and physical Gate C timing remain open.",
            "Generated SDK bindings, script controls and the remaining implementation plan are incomplete."]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"native_checks": evidence["native_checks"], "production_builds": len(builds), "passed": True}))


if __name__ == "__main__":
    main()
