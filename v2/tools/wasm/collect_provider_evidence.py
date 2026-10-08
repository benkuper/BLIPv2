"""Bind production providers, owner lifecycle and fixed reservations to exact artifacts."""
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
sys.path.insert(0, str(ROOT / "v2/qualification/wasm-imports"))
import run_imports
sys.path.insert(0, str(ROOT / "v2/qualification/wasm-worker"))
import run_worker
from blip_wasm_provider_hil import artifacts, snapshot
from collect_profile_evidence import PROFILES, app_partition_bytes, read_text
from collect_import_evidence import symbols, static_ram


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(read_text(path))


def require(condition, message):
    if not condition: raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sources = snapshot()
    require(subprocess.check_output(["git", "-C", str(ROOT / "build/wasm-deps/wamr"), "rev-parse", "HEAD"], text=True).strip()
            == run_imports.PIN, "runtime pin differs")
    require(not subprocess.check_output(["git", "-C", str(ROOT / "build/wasm-deps/wamr"), "status", "--porcelain"], text=True).strip(),
            "runtime checkout modified")
    production = []
    for key in ("ball", "club", "xiao", "huzzah32", "m5dial"):
        path = ROOT / f"build/m6-providers-final-{key}-trial.json"
        trial = read(path)
        require(trial["passed"] and all(item["passed"] for item in trial["checks"]) and trial["source_snapshot"] == sources,
                f"production trial/source differs: {key}")
        require(trial["board"] == PROFILES[key][0] and trial["reload_cycles"] == 100, f"production identity/reloads differ: {key}")
        require(trial["artifacts"] == artifacts(ROOT / f"build/m6-production-{key}"), f"production artifacts changed: {key}")
        require(not trial["pc_network_changed"] and trial["test_firmware_left_installed"] and trial["led_settings_restored"],
                f"production cleanup differs: {key}")
        require(trial["metrics"]["native_task_maximum_us"] <= 2000 and trial["metrics"]["buffer_reserved"] == 98304,
                f"production timing/reservation differs: {key}")
        flash_log = ROOT / f"build/m6-providers-final-{key}-flash.log"
        mac = re.findall(r"MAC:\s*([0-9a-f:]+)", read_text(flash_log), re.I)[-1].lower().split(":")
        if len(mac) == 8 and mac[3:5] == ["ff", "fe"]: mac = mac[:3] + mac[5:]
        require(":".join(mac) == PROFILES[key][3], f"flashed identity differs: {key}")
        production.append({"profile": key, "checks": len(trial["checks"]), "trial_sha256": sha(path),
                           "flash_log_sha256": sha(flash_log), "trial": trial})
    pair = next(item["trial"] for item in production if item["profile"] == "xiao")
    require(pair["peer"]["artifacts"] == artifacts(ROOT / "build/m6-production-huzzah32"), "pair peer artifacts changed")
    require(all(timing["native_task_maximum_us"] <= 2000 for timing in pair["pair_native_timing"].values()), "pair task budget exceeded")
    ball = production[0]["trial"]
    require(ball["validation_cycles"] == 500 and ball["validation_wall_overruns"], "Ball preemption reproduction missing")
    lifecycle = []
    for key in ("ball", "huzzah32"):
        path = ROOT / f"build/m6-provider-lifecycle-{key}-trial.json"
        trial = read(path)
        require(trial["passed"] and trial["source_snapshot"] == sources, f"lifecycle trial/source differs: {key}")
        require(trial["artifacts"] == artifacts(ROOT / f"build/m6-provider-lifecycle-{key}"), f"lifecycle artifacts changed: {key}")
        end = trial["records"][-1]
        require(end["type"] == "complete" and end["checks"] == 61 and not end["failures"] and end["owner_cycles"] == 4,
                f"lifecycle checks differ: {key}")
        lifecycle.append({"profile": key, "trial_sha256": sha(path), "trial": trial})
    native, workers = [], []
    for key in ("xiao", "huzzah32", "m5dial"):
        path = ROOT / f"build/m6-providers-imports-{key}-trial.json"
        trial = read(path); run_imports.validate(trial)
        require(trial["passed"] and trial["source_snapshot"] == run_imports.snapshot(), f"native trial/source differs: {key}")
        for name, info in trial["inputs"].items():
            require(sha(ROOT / f"build/m6-imports-{key}" / name) == info["sha256"], f"native artifact changed: {key}/{name}")
        native.append({"profile": key, "trial_sha256": sha(path), "trial": trial})
        path = ROOT / f"build/m6-reservation-worker-{key}-trial.json"
        trial = read(path); run_worker.validate(trial)
        require(trial["passed"] and trial["source_sha256"] == run_worker.source_digest(), f"worker trial/source differs: {key}")
        for name, info in trial["inputs"].items():
            require(sha(ROOT / f"build/m6-worker-{key}" / name) == info["sha256"], f"worker artifact changed: {key}/{name}")
        workers.append({"profile": key, "trial_sha256": sha(path), "trial": trial})
    prior = read(ROOT / "docs/v2/evidence/wasm/2026-10-08-native-import-bridge.json")
    builds = []
    for key, (board, chip, selector, _) in PROFILES.items():
        directory = ROOT / f"build/m6-production-{key}"
        cache, config = (directory / "CMakeCache.txt").read_text(), (directory / "sdkconfig").read_text()
        require(f'CONFIG_IDF_TARGET="{chip}"' in config and "CONFIG_FREERTOS_HZ=1000" in config, f"target/tick differs: {key}")
        require(all(f"{name}=y" in config for name in ("CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS",
            "CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64", "CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER")), f"task accounting differs: {key}")
        require(all(f"BLIP_ENABLE_{feature}:BOOL=ON" in cache for feature in ("WASM", "FLEET", "ESPNOW", "ARTNET", "DDP")), f"feature missing: {key}")
        require("BLIP_QUALIFY_WASM_PROVIDERS:BOOL=OFF" in cache, f"qualification unexpectedly enabled: {key}")
        if selector: require(re.search(rf"^{selector}:[^=]+=ON$", cache, re.M), f"board selector differs: {key}")
        if key == "ball": require("BLIP_ENABLE_BLE:BOOL=ON" in cache, "Ball BLE missing")
        if chip == "esp32s3": require("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y" in config, "S3 USB console missing")
        if chip == "esp32": require(all(f"{name}=y" in config for name in ("CONFIG_FREERTOS_UNICORE", "CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY")), "ESP32 memory placement differs")
        require((directory / "blip-v2.bin").stat().st_size <= app_partition_bytes(directory), f"app partition exceeded: {key}")
        linked = symbols(directory, "blip-v2.elf")
        require(all(name in linked for name in ("EspFleetComponent::invoke(", "EspRmtStripComponent::invoke(",
                "EspWasmComponent::bind_capabilities(", "WamrRuntime::raw_capability(")), f"production providers not linked: {key}")
        require(not any(name in linked for name in ("ProviderLifecycle", "__wrap_wasm_runtime_register_natives_raw", "__wrap_uart_write_bytes")), f"qualification code linked: {key}")
        log = ROOT / f"build/m6-providers-final-{key}-build.log"
        require("Project build complete" in read_text(log), f"production build incomplete: {key}")
        size = json.loads(subprocess.check_output([sys.executable, str(args.idf / "tools/idf_size.py"), "--format", "json2", str(directory / "blip-v2.map")], text=True))
        old = next(item for item in prior["production_builds"] if item["profile"] == key)
        builds.append({"profile": key, "board": board, "chip": chip, "app_bytes": (directory / "blip-v2.bin").stat().st_size,
            "app_partition_bytes": app_partition_bytes(directory), "static_ram_bytes": static_ram(size),
            "prior_bridge_app_bytes": old["app_bytes"], "prior_bridge_static_ram_bytes": old["static_ram_bytes"],
            "artifacts": artifacts(directory), "build_log_sha256": sha(log)})
    directory = ROOT / "build/m6-providers-no-wasm"
    require("BLIP_ENABLE_WASM:BOOL=OFF" in (directory / "CMakeCache.txt").read_text(), "disabled build contains WASM")
    require("WamrRuntime::" not in symbols(directory, "blip-v2.elf"), "WASM linked into disabled build")
    require("Project build complete" in read_text(ROOT / "build/m6-providers-no-wasm-final-build.log"), "disabled build incomplete")
    host = ROOT / "build/m6-providers-final-host-ctest.log"
    require("100% tests passed" in read_text(host) and "out of 24" in read_text(host), "host suites incomplete")
    evidence = {"schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": True,
        "result": "production LED/fleet guest providers with routerless copied cues, task-time accounting and safe owner retirement/restart",
        "source_snapshot": sources, "additional_sources": {name: sha(ROOT / name) for name in ("v2/firmware/sdkconfig.esp32s3.defaults",)},
        "runtime_commit": run_imports.PIN, "runtime_checkout_unmodified": True,
        "host_suites": 24, "host_log_sha256": sha(host), "provider_host_cases": 13,
        "production_checks": sum(item["checks"] for item in production), "production_trials": production,
        "owner_lifecycle_checks": 122, "owner_lifecycle_trials": lifecycle,
        "native_import_checks": sum(item["trial"]["records"][-1]["checks"] for item in native), "native_import_trials": native,
        "worker_checks": sum(item["trial"]["records"][-1]["checks"] for item in workers), "worker_trials": workers,
        "production_builds": builds, "wasm_disabled_artifacts": artifacts(directory), "collector_sha256": sha(Path(__file__)),
        "limitations": ["Frame completion is software evidence, not a physical color, current or synchronization measurement.",
            "Owner qualification joins an idle actual script worker before direct exclusive-worker callbacks and owner suspend/restart. Active-call/queued cancellation and startup fault injection are separate actual-worker trials; failed live provider teardown and saturated catalogs remain stress work.",
            "Suspend joins/releases owner tasks and disables provider admission. Automatic resume is explicitly unsupported; lifecycle restart uses start after quiescence. The script layer persists across module unload until clear/blackout/owner stop.",
            "Task time includes interrupts and meter overhead; wall time is reported separately. Native code remains trusted, bounded and nonblocking. Post-return checks and cooperative cancellation do not preempt a hung provider or roll back effects.",
            "Stop retains the fixed 96 KiB script reservation. Permanent retirement must release it after join. Runtime engine metrics alone do not describe these retained SDK buffers.",
            "M5Dial uses the primary native USB console and matching bootloader. It required stub-assisted esptool run after flashing; no-stub run was insufficient. USB console/framed replies share the driver; unattended installer reset behavior needs separate qualification.",
            "Tab and absent M5StickC are compile-only in this checkpoint. Large UART bursts, full Gate C/D, RF scale, allocation/fragmentation stress and multi-day soaks remain open.",
            "Generated script controls/SDK bindings, script persistence and later implementation milestones remain incomplete."]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: evidence[key] for key in ("passed", "production_checks", "owner_lifecycle_checks", "native_import_checks", "worker_checks")}))


if __name__ == "__main__":
    main()
