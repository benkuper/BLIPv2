"""Flash standalone benchmarks and restore the supplied BLIP image on exit.

Only the selected app and OTA selection are written. No PC network changes,
bootloader/partition writes, NVS or storage erases are performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import serial


def flash(port, chip, build, image):
    command = [sys.executable, "-m", "esptool", "--chip", chip, "--port", port,
               "--baud", "460800", "write-flash", "0x18000",
               str(build / "ota_data_initial.bin"), "0x20000", str(image)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return hashlib.sha256(image.read_bytes()).hexdigest()


def capture(port, run):
    device = serial.Serial()
    device.port = port
    device.baudrate = 115200
    device.timeout = 0.2
    device.dtr = False
    device.rts = False
    records, lines = [], []
    run["records"], run["console"] = records, lines
    end = time.monotonic() + 30
    with device:
        while time.monotonic() < end:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if line:
                lines.append(line)
            if "BENCH " in line:
                record = json.loads(line.split("BENCH ", 1)[1])
                records.append(record)
                print(json.dumps(record), flush=True)
                if record["type"] == "done":
                    return records, lines
    raise RuntimeError("Benchmark did not complete:\n" + "\n".join(lines[-40:]))


def verify_restore(port):
    tool = Path(__file__).resolve().parents[2] / "tools/control/blip_serial_control.py"
    end = time.monotonic() + 12
    while time.monotonic() < end:
        result = subprocess.run([sys.executable, str(tool), "--port", port,
                                 "get", "blip.bootstrap", "probe_value"],
                                capture_output=True, text=True, timeout=6)
        if result.returncode == 0:
            return json.loads(result.stdout)
        time.sleep(0.5)
    raise RuntimeError(f"BLIP firmware did not respond after restore on {port}")


def validate(records):
    kinds = {r["type"] for r in records}
    required = {"start", "load", "timing", "memory_initialization", "memory_growth", "memory", "done"}
    if not required <= kinds:
        raise RuntimeError(f"Missing benchmark output: {required - kinds}")
    for r in records:
        if r.get("failures", 0) or r.get("valid") is False or r.get("rejected") is False or r.get("zero_initialized") is False:
            raise RuntimeError(f"Benchmark validation failed: {r}")
        if r["type"] == "fault" and not (r["trapped"] and r["recovered"]):
            raise RuntimeError(f"Fault handling failed: {r}")
    start = next(r for r in records if r["type"] == "start")
    names = {r["name"] for r in records if r["type"] == "timing"}
    if names != {"noop", "integer", "float", "pixels", "host_calls"}:
        raise RuntimeError("Timing workload set incomplete")
    if start["engine"] != "baseline":
        faults = {r["name"] for r in records if r["type"] == "fault"}
        if faults != {"trap", "invalid", "divide", "recursion"}:
            raise RuntimeError("Fault workload set incomplete")
    if start["engine"] != "baseline" and "infinite_loop" not in kinds:
        raise RuntimeError("Infinite-loop trial missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--chip", choices=["esp32", "esp32s3", "esp32c6"], required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--build", type=Path, action="append", required=True)
    parser.add_argument("--restore", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    # Verify all inputs before the first mutation.
    for build in args.build + [args.restore]:
        for name in ["ota_data_initial.bin", "blip-v2.bin" if build == args.restore else "blip-wasm-benchmark.bin"]:
            if not (build / name).is_file():
                parser.error(f"Missing image: {build / name}")
    report = {"board": args.board, "port": args.port, "chip": args.chip,
              "pc_network_changed": False, "runs": [], "restored": False}
    digest = hashlib.sha256()
    source_root = Path(__file__).resolve().parent
    for source in sorted(p for p in source_root.rglob("*") if p.is_file() and "__pycache__" not in p.parts):
        digest.update(source.relative_to(source_root).as_posix().encode())
        digest.update(b"\0")
        digest.update(source.read_bytes())
    report["harness_sha256"] = digest.hexdigest()
    try:
        for build in args.build:
            digest = flash(args.port, args.chip, build, build / "blip-wasm-benchmark.bin")
            run = {"build": build.as_posix(), "image_sha256": digest}
            report["runs"].append(run)
            records, _ = capture(args.port, run)
            validate(records)
            if next(r for r in records if r["type"] == "start")["chip"] != args.chip:
                raise RuntimeError("Benchmark chip identity mismatch")
    finally:
        try:
            report["restore_image_sha256"] = flash(args.port, args.chip, args.restore, args.restore / "blip-v2.bin")
            report["restored"] = True
            report["restore_serial_check"] = verify_restore(args.port)
        finally:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Restored BLIP firmware; report: {args.report}", flush=True)


if __name__ == "__main__":
    main()
