"""Qualify production native worker tasks; leave the test image installed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import serial

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
PIN = "25bd7eb63e828e4bd242cc9b38d260b4b31c6605"


def source_digest():
    files = [p for directory in (ROOT, REPO / "v2/components/blip_wasm", REPO / "v2/components/blip_core")
             for p in directory.rglob("*") if p.is_file() and "__pycache__" not in p.parts and p.name != "README.md"]
    files.extend(REPO / name for name in (
        "v2/qualification/wasm-service/main/fixtures.hpp", "v2/cmake/blip_component.cmake",
        "v2/firmware/sdkconfig.wasm.esp32.defaults", "v2/firmware/partitions.csv"))
    digest = hashlib.sha256()
    for path in sorted(files):
        digest.update(path.relative_to(REPO).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def capture(port, report):
    device = serial.Serial()
    device.port, device.baudrate, device.timeout = port, 115200, 0.2
    device.dtr = device.rts = False
    report["records"], report["console"] = [], []
    until = time.monotonic() + 45
    with device:
        while time.monotonic() < until:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            report["console"].append(line)
            if "WORKER " in line:
                record = json.loads(line.split("WORKER ", 1)[1])
                report["records"].append(record)
                print(json.dumps(record), flush=True)
                if record["type"] == "complete":
                    return
                if record["type"] == "fatal":
                    raise RuntimeError(f"Fatal qualification failure: {record}")
            if "Guru Meditation" in line or "panic'ed" in line:
                raise RuntimeError(f"Device panic: {line}")
    raise RuntimeError("Native worker qualification did not complete:\n" + "\n".join(report["console"][-30:]))


def validate(report):
    records = report["records"]
    starts = [r for r in records if r["type"] == "start"]
    ends = [r for r in records if r["type"] == "complete"]
    injections = [r for r in records if r["type"] == "injection"]
    cycles = [r for r in records if r["type"] == "cycle"]
    stops = [r for r in records if r["type"] == "active-stop"]
    expected_faults = {"engine-pool", "module-buffer", "supervisor-task", "pthread-argument", "pthread-record", "worker-task"}
    if report["chip"] == "esp32":
        expected_faults.add("linear-arena")
    if len(starts) != 1 or starts[0]["chip"] != report["chip"] or len(ends) != 1:
        raise RuntimeError("Incomplete native worker identity/completion")
    start = starts[0]
    if start["engine"] != "wamr-2.4.5-fast-metered" or start["worker_stack_bytes"] != 8192 or start["supervisor_stack_bytes"] != 4096:
        raise RuntimeError("Unexpected production runtime or task configuration")
    if (start["engine_pool_bytes"], start["linear_bytes"]) != ((16384, 65536) if report["chip"] == "esp32" else (81920, 0)):
        raise RuntimeError("Unexpected production memory configuration")
    end = ends[0]
    if end["failures"] or end["checks"] < 900 or any(r["type"] == "failure" for r in records):
        raise RuntimeError(f"Native qualification checks failed: {end}")
    if len(injections) != len(expected_faults) or {r["name"] for r in injections} != expected_faults:
        raise RuntimeError("Missing allocation/task failure stage")
    if end["allocation_faults"] != len(expected_faults) or end["backend_faults"] != 1:
        raise RuntimeError("Failure stage totals differ")
    for r in injections:
        if not r["recovered"] or r["hits"] != 1 or any(r[f"{key}_before"] != r[f"{key}_after"] for key in ("dram", "iram", "tasks")):
            raise RuntimeError(f"Failure cleanup/recovery failed: {r}")
    if len(stops) != 1 or not stops[0]["observed_active"] or stops[0]["cancelled"] != 9 or stops[0]["queued"] != 8:
        raise RuntimeError("Active invocation and queued stop case missing")
    deadlines = [r for r in records if r["type"] == "deadline"]
    if len(deadlines) != 1 or deadlines[0]["error"] != 18 or deadlines[0]["counter_after"] != deadlines[0]["counter_before"] + 1 or deadlines[0]["elapsed_us"] >= 100000:
        raise RuntimeError("Actual supervisor deadline cancellation missing")
    if len(cycles) != 20 or [r["cycle"] for r in cycles] != list(range(1, 21)) or end["start_stop_cycles"] != 20:
        raise RuntimeError("Missing restart cycles")
    if end["heap_baseline"] != end["heap_min"] or end["heap_min"] != end["heap_max"]:
        raise RuntimeError("Native restart heap grew")
    if any(r["dram"] != end["heap_baseline"] or r["iram"] != end["iram_after"] or r["tasks"] != end["tasks_after"] for r in cycles):
        raise RuntimeError("Native restart heap/task count changed")
    if end["maximum_stop_us"] >= 1000000 or min(end["minimum_worker_headroom"], end["minimum_supervisor_headroom"]) <= 1024:
        raise RuntimeError("Stop latency or native stack margin failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--chip", choices=["esp32", "esp32s3", "esp32c6"], required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    names = ("blip-wasm-worker.bin", "blip-wasm-worker.elf", "blip-wasm-worker.map", "sdkconfig", "ota_data_initial.bin")
    for name in names:
        if not (args.build / name).is_file():
            parser.error(f"Missing input: {args.build / name}")
    if (args.build / names[0]).stat().st_size > 0x190000:
        parser.error("Qualification app exceeds the smallest existing app partition")
    dependency = REPO / "build/wasm-deps/wamr"
    revision = subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(dependency), "status", "--porcelain"], text=True).strip()
    if revision != PIN or dirty:
        parser.error("WAMR must be the pinned, unmodified checkout")
    report = {"board": args.board, "port": args.port, "chip": args.chip, "source_sha256": source_digest(),
              "pc_network_changed": False, "restore_requested": False, "test_firmware_left_installed": True,
              "wamr_commit": revision, "passed": False,
              "inputs": {name: {"bytes": (args.build / name).stat().st_size,
                                "sha256": hashlib.sha256((args.build / name).read_bytes()).hexdigest()} for name in names}}
    try:
        command = [sys.executable, "-m", "esptool", "--chip", args.chip, "--port", args.port,
                   "--baud", "460800", "write-flash", "0x18000", str(args.build / "ota_data_initial.bin"),
                   "0x20000", str(args.build / "blip-wasm-worker.bin")]
        flashed = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if flashed.returncode:
            raise RuntimeError(flashed.stdout + flashed.stderr)
        macs = re.findall(r"MAC:\s*([0-9a-f:]+)", flashed.stdout, re.I)
        if not macs:
            raise RuntimeError("esptool did not report the flashed device MAC")
        report["mac"] = macs[-1].lower()
        # Native USB reset can leave the S3 in download mode. Explicitly start
        # the installed image before opening the non-resetting capture port.
        if args.chip == "esp32s3":
            run = subprocess.run([sys.executable, "-m", "esptool", "--chip", args.chip, "--port", args.port, "run"],
                                 capture_output=True, text=True, timeout=15)
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
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Native qualification passed; test firmware remains installed; report: {args.report}", flush=True)


if __name__ == "__main__":
    main()
