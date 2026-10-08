"""Qualify the native provider import bridge against actual guest memory; leave testing firmware installed."""
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
PIN = "25bd7eb63e828e4bd242cc9b38d260b4b31c6605"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot():
    files = [p for directory in (HERE, ROOT / "v2/components/blip_wasm", ROOT / "v2/components/blip_core")
             for p in directory.rglob("*") if p.is_file() and p.suffix in (".py", ".hpp", ".cpp", ".c", ".h", ".txt")]
    files.extend(ROOT / name for name in ("v2/cmake/blip_component.cmake",
        "v2/qualification/wasm-worker/sdkconfig.defaults", "v2/qualification/wasm-worker/sdkconfig.defaults.esp32c6",
        "v2/qualification/wasm-worker/sdkconfig.defaults.esp32s3", "v2/firmware/sdkconfig.wasm.esp32.defaults", "v2/firmware/partitions.csv"))
    return {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest() for p in sorted(files)}


def validate(report):
    records = report["records"]
    starts = [r for r in records if r["type"] == "start"]
    ends = [r for r in records if r["type"] == "complete"]
    if len(starts) != 1 or len(ends) != 1 or starts[0]["chip"] != report["chip"]:
        raise RuntimeError("Missing capability identity/completion")
    start, end = starts[0], ends[0]
    if start["worker_stack_bytes"] != 8192 or start["idf"] != "v6.0.2":
        raise RuntimeError("Unexpected native stack/SDK")
    if not end["native_imports_exercised"] or end["engine_cycles"] != 20:
        raise RuntimeError("Native import/reinitialization scope missing")
    if not (0 < end["idle_pool_used"] < 8192) or end["pool_peak"] > 81920:
        raise RuntimeError("Registration/pool bounds exceeded")
    if end["failures"] or end["checks"] < 700 or any(r["type"] in ("failure", "fatal") for r in records):
        raise RuntimeError(f"Portable provider qualification failed: {end}")
    if end["reload_cycles"] != 100 or end["heap_baseline"] != end["heap_min"] or end["heap_min"] != end["heap_max"] or end["worker_headroom"] < 1024:
        raise RuntimeError("Heap stability, reload count or stack margin failed")


def capture(port, report):
    device = serial.Serial()
    device.port, device.baudrate, device.timeout = port, 115200, 0.2
    device.dtr = device.rts = False
    report["records"], report["console"] = [], []
    until = time.monotonic() + 30
    with device:
        while time.monotonic() < until:
            line = device.readline().decode(errors="replace").strip()
            if not line:
                continue
            report["console"].append(line)
            if "IMPORTS " in line:
                record = json.loads(line.split("IMPORTS ",1)[1])
                report["records"].append(record)
                print(json.dumps(record), flush=True)
                if record["type"] == "complete":
                    return
                if record["type"] == "fatal":
                    raise RuntimeError(str(record))
            if "Guru Meditation" in line or "panic'ed" in line:
                raise RuntimeError(f"Device panic: {line}")
    raise RuntimeError("Capability qualifier did not finish:\n" + "\n".join(report["console"][-20:]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--chip", choices=["esp32", "esp32s3", "esp32c6"], required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    names = ("blip-wasm-imports.bin", "blip-wasm-imports.elf", "blip-wasm-imports.map", "sdkconfig", "ota_data_initial.bin")
    for name in names:
        if not (args.build / name).is_file():
            parser.error(f"Missing input: {args.build / name}")
    if (args.build / names[0]).stat().st_size > 0x190000:
        parser.error("Capability qualifier exceeds the smallest existing app partition")
    dependency = ROOT / "build/wasm-deps/wamr"
    if subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip() != PIN or \
       subprocess.check_output(["git", "-C", str(dependency), "status", "--porcelain"], text=True).strip():
        parser.error("WAMR checkout must remain pinned and unmodified")
    report = {"board": args.board, "chip": args.chip, "port": args.port, "source_snapshot": snapshot(), "wamr_commit": PIN,
              "pc_network_changed": False, "firmware_restore_requested": False, "test_firmware_left_installed": True, "passed": False,
              "inputs": {name: {"bytes": (args.build / name).stat().st_size, "sha256": sha(args.build / name)} for name in names}}
    try:
        command = [sys.executable,"-m","esptool","--chip",args.chip,"--port",args.port,"--baud","460800",
                   "write-flash","0x18000",str(args.build / "ota_data_initial.bin"),"0x20000",str(args.build / names[0])]
        flashed = subprocess.run(command,capture_output=True,text=True,timeout=90)
        if flashed.returncode:
            raise RuntimeError(flashed.stdout + flashed.stderr)
        macs = re.findall(r"MAC:\s*([0-9a-f:]+)",flashed.stdout,re.I)
        if not macs:
            raise RuntimeError("esptool did not report the flashed board MAC")
        report["flashed_mac"] = macs[-1].lower()
        report["flashed_app_sha256"] = sha(args.build / names[0])
        if args.chip == "esp32s3":
            run = subprocess.run([sys.executable,"-m","esptool","--chip",args.chip,"--port",args.port,"run"], capture_output=True,text=True,timeout=15)
            if run.returncode:
                raise RuntimeError(run.stdout + run.stderr)
            report["native_usb_run_reset"] = True
        capture(args.port, report)
        validate(report)
        report["passed"] = True
    except Exception as exc:
        report["error"] = str(exc)
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report,indent=2) + "\n",encoding="utf-8")


if __name__ == "__main__":
    main()
