"""Flash and capture opt-in actual-owner lifecycle checks; leave test firmware installed."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import serial

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_provider_hil import artifacts, snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("port", "chip", "board"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    cache = (args.build / "CMakeCache.txt").read_text()
    config = (args.build / "sdkconfig").read_text()
    if "BLIP_QUALIFY_WASM_PROVIDERS:BOOL=ON" not in cache or f'CONFIG_IDF_TARGET="{args.chip}"' not in config:
        parser.error("build must opt in to provider qualification and match the chip")
    report = {"board": args.board, "chip": args.chip, "port": args.port, "source_snapshot": snapshot(),
              "artifacts": artifacts(args.build), "records": [], "console": [], "passed": False,
              "pc_network_changed": False, "test_firmware_left_installed": True}
    try:
        flashed = subprocess.run([sys.executable, "-m", "esptool", "--chip", args.chip, "--port", args.port,
            "--baud", "460800", "write-flash", "0x18000", str(args.build / "ota_data_initial.bin"),
            "0x20000", str(args.build / "blip-v2.bin")], capture_output=True, text=True, timeout=90)
        if flashed.returncode: raise RuntimeError(flashed.stdout + flashed.stderr)
        report["flashed_mac"] = re.findall(r"MAC:\s*([0-9a-f:]+)", flashed.stdout, re.I)[-1].lower()
        device = serial.Serial()
        device.port, device.baudrate, device.timeout = args.port, 115200, 0.2
        device.dtr = device.rts = False
        deadline = time.monotonic() + 30
        with device:
            while time.monotonic() < deadline:
                line = device.readline().decode(errors="replace").strip()
                if not line: continue
                report["console"].append(line)
                if "PROVIDERS " in line:
                    record = json.loads(line.split("PROVIDERS ", 1)[1])
                    report["records"].append(record)
                    print(json.dumps(record), flush=True)
                    if record["type"] == "failure": raise RuntimeError(str(record))
                    if record["type"] == "complete": break
                if "Guru Meditation" in line or "panic'ed" in line:
                    raise RuntimeError(line)
        starts = [record for record in report["records"] if record["type"] == "start"]
        ends = [record for record in report["records"] if record["type"] == "complete"]
        if len(starts) != 1 or len(ends) != 1 or starts[0]["chip"] != args.chip or starts[0]["idf"] != "v6.0.2":
            raise RuntimeError("missing lifecycle identity/completion")
        end = ends[0]
        if end["failures"] or end["checks"] < 50 or end["owner_cycles"] != 4 or end["worker_headroom"] < 1024:
            raise RuntimeError(f"lifecycle checks failed: {end}")
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
