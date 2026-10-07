"""Test the actual service/adapter (app/otadata only); optional image restore."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import serial

root = Path(__file__).resolve().parent
sys.path.insert(0, str(root.parent / "wasm"))
from run_benchmark import flash, verify_restore


def capture(port, report):
    device = serial.Serial()
    device.port = port
    device.baudrate = 115200
    device.timeout = 0.2
    device.dtr = False
    device.rts = False
    report["records"], report["console"] = [], []
    end = time.monotonic() + 35
    with device:
        while time.monotonic() < end:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if line:
                report["console"].append(line)
            if "SERVICE " in line:
                record = json.loads(line.split("SERVICE ", 1)[1])
                report["records"].append(record)
                print(json.dumps(record), flush=True)
                if record["type"] == "complete":
                    return
                if record["type"] in {"fatal", "supervisor-failure"}:
                    raise RuntimeError(f"Service qualification failed: {record}")
    raise RuntimeError("Service qualification did not finish:\n" + "\n".join(report["console"][-40:]))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--chip", choices=["esp32", "esp32s3", "esp32c6"], required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--restore", type=Path, help="Optional image to install after the trial; default leaves test firmware")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    names = ("blip-wasm-service.bin", "blip-wasm-service.elf", "blip-wasm-service.map", "sdkconfig")
    inputs = [(args.build, names + ("ota_data_initial.bin",))]
    if args.restore:
        inputs.append((args.restore, ("blip-v2.bin", "ota_data_initial.bin")))
    for directory, files in inputs:
        for name in files:
            if not (directory / name).is_file():
                parser.error(f"Missing input: {directory / name}")
    if (args.build / names[0]).stat().st_size > 0x190000:
        parser.error("Service image exceeds the smallest existing app partition")
    repo = root.parents[2]
    digest = hashlib.sha256()
    sources = list(root.rglob("*")) + list((repo / "v2/components/blip_wasm").rglob("*"))
    sources.append(root.parent / "wasm/run_benchmark.py")
    for source in sorted(p for p in sources if p.is_file() and "__pycache__" not in p.parts):
        digest.update(source.relative_to(repo).as_posix().encode())
        digest.update(b"\0")
        digest.update(source.read_bytes())
    report = {"board": args.board, "port": args.port, "chip": args.chip,
              "source_sha256": digest.hexdigest(), "pc_network_changed": False,
              "inputs": {name: {"bytes": (args.build / name).stat().st_size, "sha256": sha(args.build / name)} for name in names},
              "wamr_commit": "25bd7eb63e828e4bd242cc9b38d260b4b31c6605",
              "restore_requested": args.restore is not None, "restored": False}
    try:
        flash(args.port, args.chip, args.build, args.build / names[0])
        capture(args.port, report)
        starts = [r for r in report["records"] if r["type"] == "start"]
        complete = [r for r in report["records"] if r["type"] == "complete"]
        if len(starts) != 1 or starts[0]["chip"] != args.chip or len(complete) != 1:
            raise RuntimeError("Invalid/incomplete service identity or completion")
        if complete[0]["failures"] or complete[0]["checks"] < 350:
            raise RuntimeError(f"Service checks failed: {complete[0]}")
        report["passed"] = True
    finally:
        try:
            if args.restore:
                report["restore_image_sha256"] = flash(args.port, args.chip, args.restore, args.restore / "blip-v2.bin")
                report["restore_serial_check"] = verify_restore(args.port)
                report["restored"] = True
        finally:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Trial complete; {'restored supplied image' if args.restore else 'left test firmware installed'}; report: {args.report}", flush=True)


if __name__ == "__main__":
    main()
