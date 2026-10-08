"""Qualify the owned store and passive global query; leave test firmware installed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import serial

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MACS = {"xiao": "10:51:db:1a:a6:68", "huzzah32": "30:ae:a4:f2:d1:84", "m5dial": "b0:81:84:96:83:74"}
TARGETS = {"xiao": "esp32c6", "huzzah32": "esp32", "m5dial": "esp32s3"}
ARTIFACTS = ("blip-script-controls.bin", "blip-script-controls.elf", "blip-script-controls.map", "sdkconfig",
             "ota_data_initial.bin", "bootloader/bootloader.bin", "partition_table/partition-table.bin", "flasher_args.json")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    files = {p for folder in (HERE, ROOT / "v2/components/blip_core", ROOT / "v2/components/blip_wasm") for p in folder.rglob("*")
             if p.is_file() and p.suffix in (".py", ".hpp", ".cpp", ".c", ".txt")}
    files.update(ROOT / name for name in ("v2/tests/host/blip_script_controls_tests.cpp", "v2/tests/host/test_harness.hpp",
        "v2/cmake/blip_component.cmake", "v2/qualification/wasm-worker/sdkconfig.defaults",
        "v2/qualification/wasm-worker/sdkconfig.defaults.esp32c6", "v2/qualification/wasm-worker/sdkconfig.defaults.esp32s3",
        "v2/firmware/partitions.csv"))
    return {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest() for p in sorted(files)}


def validate(report):
    starts = [r for r in report["records"] if r["type"] == "start"]
    ends = [r for r in report["records"] if r["type"] == "complete"]
    if len(starts) != 1 or len(ends) != 1 or any(r["type"] in ("fatal", "failure") for r in report["records"]):
        raise RuntimeError("Missing store identity/completion or a failed check")
    start, end = starts[0], ends[0]
    if start["chip"] != report["chip"] or start["idf"] != "v6.0.2" or start["stack_bytes"] != 24576:
        raise RuntimeError("Unexpected store target/SDK/stack")
    if not 3464 < start["store_bytes"] <= 12288 or start["message_bytes"] > 704 or start["value_bytes"] > 152:
        raise RuntimeError("Unexpected owner/message/value storage cost")
    header = (HERE / "main/fixtures.hpp").read_text()
    fixture = re.search(r'valid_sha256\[\] = "([0-9a-f]{64})";', header)
    if not fixture or start["fixture_sha256"] != fixture[1]:
        raise RuntimeError("Fixture hash differs from the generated source")
    if end["cases"] != 6 or end["repeat_cycles"] != 20 or end["case_passes"] != 126 or end["failures"] or end["stack_headroom"] < 1024:
        raise RuntimeError("Shared cases/cycles/stack margin failed")
    if end["engine_checks"] != 32 or end["engine_cycles"] != 20 or not 0 < end["pool_peak"] <= 98304:
        raise RuntimeError("Real-engine cases/pool failed")
    for prefix in ("", "engine_"):
        if end[prefix + "heap_baseline"] != end[prefix + "heap_min"] or end[prefix + "heap_min"] != end[prefix + "heap_max"]:
            raise RuntimeError("Heap changed across repeated store/engine cases")
    if sum(line.startswith("PASS ") for line in report["console"]) != 126:
        raise RuntimeError("Shared cases did not all pass on this target")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--board", choices=list(MACS), required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    chip = TARGETS[args.board]
    for name in ARTIFACTS:
        if not (args.build / name).is_file():
            parser.error(f"Missing input: {args.build / name}")
    if (args.build / ARTIFACTS[0]).stat().st_size > 0x190000:
        parser.error("Qualifier exceeds the smallest existing app partition")
    if not re.search(r"^IDF_TARGET:.*=" + re.escape(chip) + r"$", (args.build / "CMakeCache.txt").read_text(), re.M):
        parser.error("Build target differs from requested board")
    flash_arguments = json.loads((args.build / "flasher_args.json").read_text())
    boot = "0x1000" if chip == "esp32" else "0x0"
    expected_files = {boot: "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                      "0x18000": "ota_data_initial.bin", "0x20000": ARTIFACTS[0]}
    if flash_arguments["flash_files"] != expected_files or flash_arguments["extra_esptool_args"]["chip"] != chip:
        parser.error("Unexpected generated flash layout/target")
    report = {"board": args.board, "chip": chip, "port": args.port, "source_snapshot": snapshot(),
        "inputs": {name: {"bytes": (args.build / name).stat().st_size, "sha256": sha(args.build / name)} for name in ARTIFACTS},
        "pc_network_changed": False, "firmware_restore_requested": False, "test_firmware_left_installed": True,
        "interpreter_exercised": True, "production_script_owner_exercised": False,
        "concurrent_schema_retirement_exercised": True, "records": [], "console": [], "passed": False}
    try:
        command = [sys.executable, "-m", "esptool", "--chip", chip, "--port", args.port, "--baud", "460800",
                   "write-flash", *flash_arguments["write_flash_args"]]
        for offset, filename in expected_files.items():
            command.extend([offset, str(args.build / filename)])
        flashed = subprocess.run(command, capture_output=True, text=True, timeout=90)
        report["flash_log"] = flashed.stdout + flashed.stderr
        if flashed.returncode:
            raise RuntimeError(report["flash_log"])
        macs = re.findall(r"MAC:\s*([0-9a-f:]+)", flashed.stdout, re.I)
        if not macs or macs[-1].lower() != MACS[args.board]:
            raise RuntimeError("Flashed MAC differs from the test board identity")
        report["flashed_mac"] = macs[-1].lower()
        if chip == "esp32s3":
            run = subprocess.run([sys.executable, "-m", "esptool", "--chip", chip, "--port", args.port, "run"],
                                 capture_output=True, text=True, timeout=15)
            report["run_log"] = run.stdout + run.stderr
            if run.returncode:
                raise RuntimeError(report["run_log"])
        device = serial.Serial(); device.port, device.baudrate, device.timeout = args.port, 115200, .2
        device.dtr = device.rts = False
        until = time.monotonic() + 30
        with device:
            while time.monotonic() < until:
                line = device.readline().decode(errors="replace").strip()
                if not line:
                    continue
                report["console"].append(line)
                if "SCRIPTSTORE " in line:
                    record = json.loads(line.split("SCRIPTSTORE ", 1)[1]); report["records"].append(record)
                    print(json.dumps(record), flush=True)
                    if record["type"] == "complete":
                        break
                    if record["type"] in ("failure", "fatal"):
                        raise RuntimeError(str(record))
                if "Guru Meditation" in line or "panic'ed" in line:
                    raise RuntimeError(line)
            else:
                raise RuntimeError("Store qualifier timed out: " + "\n".join(report["console"][-12:]))
        validate(report); report["passed"] = True
    except Exception as exc:
        report["error"] = str(exc)
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
