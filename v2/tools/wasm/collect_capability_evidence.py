"""Validate portable provider qualification and six required production compatibility builds."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/qualification/wasm-capabilities"))
from run_capabilities import PIN, snapshot, validate
from collect_profile_evidence import PROFILES, app_partition_bytes, read_text


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symbols(build):
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    nm = re.search(r"^CMAKE_NM:FILEPATH=(.+)$", cache, re.M)[1]
    elf = "blip-wasm-capabilities.elf" if "capabilities" in build.name else "blip-v2.elf"
    return subprocess.check_output([nm, "--demangle", str(build / elf)], text=True)


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
    trial_path = ROOT / "build/m6-capabilities-xiao-trial.json"
    trial = json.loads(trial_path.read_text(encoding="utf-8"))
    validate(trial)
    if not trial["passed"] or trial["source_snapshot"] != sources or trial["wamr_commit"] != PIN:
        raise RuntimeError("Native source/runtime trial differs")
    board, chip, _, mac = PROFILES["xiao"]
    displayed = trial["flashed_mac"].split(":")
    if len(displayed) == 8 and displayed[3:5] == ["ff", "fe"]:
        displayed = displayed[:3] + displayed[5:]
    if (trial["board"], trial["chip"], ":".join(displayed)) != (board, chip, mac):
        raise RuntimeError("Native board identity differs")
    if trial["pc_network_changed"] or trial["firmware_restore_requested"] or not trial["test_firmware_left_installed"]:
        raise RuntimeError("Unexpected network or firmware restoration")
    native_build = ROOT / "build/m6-capabilities-xiao"
    if "Project build complete" not in read_text(ROOT / "build/m6-capabilities-xiao-build.log"):
        raise RuntimeError("Native build incomplete")
    for name, info in trial["inputs"].items():
        artifact = native_build / name
        if artifact.stat().st_size != info["bytes"] or sha(artifact) != info["sha256"]:
            raise RuntimeError(f"Native artifact changed: {name}")
    if trial["flashed_app_sha256"] != trial["inputs"]["blip-wasm-capabilities.bin"]["sha256"]:
        raise RuntimeError("Flashed native image differs")
    native_symbols = symbols(native_build)
    if not all(f"CapabilityRegistry::{name}(" in native_symbols for name in ("bind", "invoke")):
        raise RuntimeError("Native catalog not linked")
    start, end = trial["records"][0], trial["records"][-1]
    fixture = (ROOT / "v2/qualification/wasm-strings/main/fixtures.hpp").read_text(encoding="utf-8")
    fixture_hash = re.search(r'strings_sha256\[\] = "([0-9a-f]+)"', fixture)[1]
    if start["fixture_sha256"] != fixture_hash or end["pool_peak"] > 81920:
        raise RuntimeError("Fixture or memory pool differs")
    previous = json.loads((ROOT / "docs/v2/evidence/wasm/2026-10-07-checked-utf8.json").read_text())
    memory = json.loads((ROOT / "docs/v2/evidence/wasm/2026-10-07-production-profile-memory.json").read_text())
    builds = []
    size_tool = args.idf / "tools/idf_size.py"
    for key, (board, chip, selector, _) in PROFILES.items():
        directory = ROOT / f"build/m6-production-{key}"
        log = ROOT / f"build/m6-capabilities-production-{key}-build.log"
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
        linked = symbols(directory)
        if "blip::core::valid_wasm_descriptor(" not in linked or "CapabilityRegistry::invoke(" in linked:
            raise RuntimeError("Production provider scope changed")
        if "__wrap_uart_write_bytes" in (directory / "blip-v2.map").read_text():
            raise RuntimeError("UART diagnostic unexpectedly linked")
        linked_size = json.loads(subprocess.check_output([
            sys.executable, str(size_tool), "--format", "json2", str(directory / "blip-v2.map")], text=True))
        old = next(b for b in previous["production_builds"] if b["profile"] == key)
        old_memory = next(b for b in memory["boards"] if b["profile"] == key)
        builds.append({"profile": key, "board": board, "chip": chip, "app_bytes": image_size,
            "app_partition_bytes": partition, "prior_utf8_app_bytes": old["app_bytes"], "app_delta_bytes": image_size - old["app_bytes"],
            "linked_size": linked_size, "static_ram_bytes": static_ram(linked_size),
            "prior_memory_matrix_static_ram_bytes": static_ram(old_memory["linked_size"]),
            "static_ram_delta_vs_memory_matrix_bytes": static_ram(linked_size) - static_ram(old_memory["linked_size"]),
            "build_log_sha256": sha(log), "provider_validator_linked": True, "unused_catalog_removed": True,
            "flashed_in_this_trial": False, "artifacts": {name: {"bytes": (directory / name).stat().st_size,
            "sha256": sha(directory / name)} for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}})
    host_log = ROOT / "build/m6-capabilities-host-ctest.log"
    if "100% tests passed" not in read_text(host_log) or "out of 24" not in read_text(host_log):
        raise RuntimeError("Host suite incomplete")
    red_log = ROOT / "build/m6-capabilities-host-red.log"
    if not all(name in read_text(red_log) for name in ("LNK2019", "CapabilityRegistry", "valid_wasm_descriptor")):
        raise RuntimeError("Tests-first evidence missing")
    evidence = {"schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": True,
        "result": "portable component-owned provider contract qualified; native import bridge remains pending",
        "source_snapshot": sources, "runtime": {"commit": PIN, "checkout_unmodified": True},
        "fixture_sha256": fixture_hash, "host_tests": 24, "provider_host_cases": 10,
        "host_log_sha256": sha(host_log), "tests_first_log_sha256": sha(red_log),
        "host_sources": {name: sha(ROOT / "v2/tests/host" / name) for name in (
            "CMakeLists.txt", "blip_wasm_capability_tests.cpp", "test_harness.hpp")},
        "host_executable_sha256": sha(ROOT / "build/host-m4/Release/blip_wasm_capability_tests.exe"),
        "native_checks": end["checks"], "native_trial_sha256": sha(trial_path), "native_trial": trial,
        "production_builds": builds, "size_tool_sha256": sha(size_tool), "native_imports_exercised": False,
        "limitations": ["Guest imports remain rejected. Native registration, trampoline, import allowlist/signature checks and production-owned providers are the next 6.5 checkpoint.",
            "Native dispatch used real WAMR guest memory on XIAO C6 with synthetic component-owned providers; no radio, LED, settings or production provider coexistence was exercised.",
            "Six required production images were built but not flashed. They link the metadata validator; the unused provider catalog is removed by linker garbage collection.",
            "Callback time bounds are declarations of trusted native work. This portable checkpoint does not enforce elapsed time or prove a bound under load.",
            "Static RAM deltas use the earlier production memory matrix because the UTF-8 checkpoint did not record linked RAM. Production pool/module/native task reservations are unchanged.",
            "Full Gate C/D, soaks, M5StickC serial burst recovery, SDK generation and script controls remain open."]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"native_checks": end["checks"], "production_builds": len(builds), "passed": True}))


if __name__ == "__main__":
    main()
