"""Collect checked-string native trials and seven production compatibility builds."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT / "v2/qualification/wasm-strings"))
from run_strings import PIN, snapshot, validate
from collect_profile_evidence import PROFILES, app_partition_bytes, read_text


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,required=True)
    args = parser.parse_args()
    dependency = ROOT / "build/wasm-deps/wamr"
    if subprocess.check_output(["git","-C",str(dependency),"rev-parse","HEAD"],text=True).strip() != PIN or \
       subprocess.check_output(["git","-C",str(dependency),"status","--porcelain"],text=True).strip():
        raise RuntimeError("Runtime checkout differs")
    sources = snapshot()
    fixture = (ROOT / "v2/qualification/wasm-strings/main/fixtures.hpp").read_text(encoding="utf-8")
    fixture_hash = re.search(r'strings_sha256\[\] = "([0-9a-f]+)"',fixture)[1]
    trials=[]
    for key in ("huzzah32","m5dial","xiao"):
        path = ROOT / f"build/m6-strings-{key}-trial.json"
        trial = json.loads(path.read_text(encoding="utf-8"))
        validate(trial)
        board,chip,_,mac = PROFILES[key]
        displayed=trial["flashed_mac"].split(":")
        if len(displayed)==8 and displayed[3:5]==["ff","fe"]:
            displayed=displayed[:3]+displayed[5:]
        if (trial["board"],trial["chip"],":".join(displayed)) != (board,chip,mac) or not trial["passed"]:
            raise RuntimeError(f"Unexpected native trial identity: {key}")
        if trial["source_snapshot"] != sources or trial["wamr_commit"] != PIN:
            raise RuntimeError("Native source/runtime changed")
        if trial["pc_network_changed"] or trial["firmware_restore_requested"] or not trial["test_firmware_left_installed"]:
            raise RuntimeError("Unexpected network/restore action")
        for name,info in trial["inputs"].items():
            artifact=ROOT / f"build/m6-strings-{key}" / name
            if artifact.stat().st_size!=info["bytes"] or sha(artifact)!=info["sha256"]:
                raise RuntimeError(f"Native artifact changed: {artifact}")
        if trial["flashed_app_sha256"] != trial["inputs"]["blip-wasm-strings.bin"]["sha256"]:
            raise RuntimeError("Flashed app hash differs")
        start=next(r for r in trial["records"] if r["type"]=="start")
        end=next(r for r in trial["records"] if r["type"]=="complete")
        if start["fixture_sha256"]!=fixture_hash or end["unloaded_pool_used"]!=0 or end["pool_peak"]>81920:
            raise RuntimeError("Fixture/pool accounting differs")
        trials.append({"profile":key,"mac":mac,"trial_sha256":sha(path),"trial":trial})
    baseline=json.loads((ROOT / "docs/v2/evidence/wasm/2026-10-07-production-profile-memory.json").read_text(encoding="utf-8"))
    builds=[]
    for key,(board,chip,selector,_) in PROFILES.items():
        directory=ROOT / f"build/m6-production-{key}"
        log=ROOT / f"build/m6-strings-production-{key}-build.log"
        if "Project build complete" not in read_text(log):
            raise RuntimeError(f"Production build not completed: {key}")
        cache=(directory / "CMakeCache.txt").read_text(encoding="utf-8")
        config=(directory / "sdkconfig").read_text(encoding="utf-8")
        if f'CONFIG_IDF_TARGET="{chip}"' not in config or "CONFIG_FREERTOS_HZ=1000" not in config:
            raise RuntimeError("Production target/tick configuration differs")
        for feature in ("WASM","ESPNOW","FLEET","ARTNET","DDP"):
            if f"BLIP_ENABLE_{feature}:BOOL=ON" not in cache:
                raise RuntimeError(f"Required production feature missing: {key}/{feature}")
        if selector and not re.search(rf"^{selector}:[^=]+=ON$",cache,re.M):
            raise RuntimeError(f"Production board selection differs: {key}")
        if "__wrap_uart_write_bytes" in (directory / "blip-v2.map").read_text(encoding="utf-8"):
            raise RuntimeError("UART diagnostic unexpectedly attached to production build")
        if chip=="esp32" and not all(f"{key}=y" in config for key in ("CONFIG_FREERTOS_UNICORE","CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY")):
            raise RuntimeError("ESP32 memory placement differs")
        image=directory / "blip-v2.bin"
        size=image.stat().st_size
        partition=app_partition_bytes(directory)
        if size>partition:
            raise RuntimeError("Production image exceeds partition")
        previous=next(b for b in baseline["boards"] if b["profile"]==key)
        artifacts={name:{"bytes":(directory/name).stat().st_size,"sha256":sha(directory/name)}
                   for name in ("blip-v2.bin","blip-v2.elf","blip-v2.map","sdkconfig")}
        nm=re.search(r"^CMAKE_NM:FILEPATH=(.+)$",cache,re.M)[1]
        symbols=subprocess.check_output([nm,"--demangle",str(directory / "blip-v2.elf")],text=True)
        if not all(f"WamrRuntime::{method}(" in symbols for method in ("read_memory","write_memory")):
            raise RuntimeError("Checked memory adapter was not linked")
        unused_strings=not any(f"::{method}(" in symbols for method in ("read_utf8","write_utf8"))
        if not unused_strings:
            raise RuntimeError("Production string binding footprint changed; update qualification scope")
        builds.append({"profile":key,"board":board,"chip":chip,"app_bytes":size,"app_partition_bytes":partition,
                       "prior_app_bytes":previous["app_bytes"],"app_delta_bytes":size-previous["app_bytes"],
                       "build_log_sha256":sha(log),"artifacts":artifacts,"checked_memory_adapter_linked":True,
                       "unused_string_helpers_removed":unused_strings,"flashed_in_this_trial":False})
    hostlog=ROOT / "build/m6-utf8-host-ctest.log"
    if "100% tests passed" not in read_text(hostlog) or "out of 23" not in read_text(hostlog):
        raise RuntimeError("Host suite incomplete")
    evidence={"schema_version":1,"recorded_utc":datetime.now(timezone.utc).isoformat(),"passed":True,
        "result":"checked UTF-8 ABI passed host and three-family native qualification; seven production compatibility builds passed",
        "string_abi_version":1,"maximum_string_bytes":256,"source_snapshot":sources,
        "runtime":{"commit":PIN,"checkout_unmodified":True},"fixture_sha256":fixture_hash,
        "native_checks":sum(next(r for r in t["trial"]["records"] if r["type"]=="complete")["checks"] for t in trials),
        "host_tests":23,"host_log_sha256":sha(hostlog),"native_trials":trials,"production_builds":builds,
        "host_sources":{name:sha(ROOT / "v2/tests/host" / name) for name in ("CMakeLists.txt","blip_wasm_tests.cpp","blip_wasm_utf8_tests.cpp")},
        "host_executables":{name:sha(ROOT / "build/host-m4/Release" / name) for name in ("blip_wasm_tests.exe","blip_wasm_utf8_tests.exe")},
        "limitations":[
            "String APIs are worker-confined runtime/service boundaries. Component-owned imports, registry declarations and generated SDK bindings follow in 6.5-6.7.",
            "Native string trials are isolated from production radio, LED and settings tasks. Compatibility images were compiled on all seven profiles but were not flashed by this string trial.",
            "Production string helpers are currently removed by linker garbage collection until bindings use them; app deltas include the checked memory adapter/vtable and build metadata.",
            "Maximum read/write times are observed isolated workloads, not hard real-time bounds. The 256-byte read scratch uses the existing worker stack; no persistent string buffer or heap reservation is added.",
            "Full Gate D, active BLE/fleet coexistence, cold/global OOM, long soaks, M5StickC serial bursts and physical Gate C timing remain open.",
        ]}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(evidence,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"output":str(args.output),"native_checks":evidence["native_checks"],"production_builds":len(builds),"passed":True}))


if __name__=="__main__":
    main()
