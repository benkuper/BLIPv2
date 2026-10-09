"""Flash generated factory files and capture actual update-worker lifecycle checks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import serial

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_hil import source_snapshot
sys.path.insert(0, str(ROOT / "v2/tools/ota"))
from release_publish import firmware_metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("port", "chip", "board", "mac"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    identity, code, version = firmware_metadata(build / "blip-v2.bin")
    if identity["board"] != args.board or identity["target"] != args.chip:
        parser.error("firmware identity differs from requested board or chip")
    cache = (build / "CMakeCache.txt").read_text()
    config = (build / "sdkconfig").read_text()
    if "BLIP_QUALIFY_RELEASE_WORKER:BOOL=ON" not in cache or f'CONFIG_IDF_TARGET="{args.chip}"' not in config:
        parser.error("build must opt in to release-worker qualification and match the chip")
    flash = json.loads((build / "flasher_args.json").read_text())
    if flash["extra_esptool_args"]["chip"] != args.chip:
        parser.error("generated flash target differs from requested chip")
    names = set(flash["flash_files"].values()) | {"blip-v2.elf", "sdkconfig", "CMakeCache.txt", "flasher_args.json"}
    report = {"board": args.board, "chip": args.chip, "port": args.port, "mac": args.mac.lower(),
        "source_snapshot": source_snapshot(ROOT), "firmware_identity": identity, "release_code": code, "version": version,
        "checker_sha256": hashlib.sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
        "qualification_sha256": hashlib.sha256(Path(__file__).with_name("lifecycle.hpp").read_bytes().replace(b"\r\n", b"\n")).hexdigest(),
        "artifacts": {name: hashlib.sha256((build / name).read_bytes()).hexdigest() for name in sorted(names)},
        "records": [], "console": [], "passed": False, "pc_network_changed": False,
        "test_firmware_left_installed": False}
    base = [sys.executable, "-m", "esptool", "--chip", args.chip, "--port", args.port]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    try:
        identity = subprocess.run([*base, "read-mac"], capture_output=True, text=True, timeout=30)
        report["identity_log"] = identity.stdout + identity.stderr
        if identity.returncode:
            raise RuntimeError("connected device identity unavailable; see identity_log")
        macs = re.findall(r"MAC:\s*([0-9a-f:]+)", report["identity_log"], re.I)
        if identity.returncode or not macs or macs[-1].lower() != args.mac.lower():
            raise RuntimeError("connected MAC differs from expected device before flash")
        command = [*base, "--baud", "460800", "write-flash", *flash["write_flash_args"]]
        for offset, name in flash["flash_files"].items():
            command.extend([offset, str(build / name)])
        flashed = subprocess.run(command, capture_output=True, text=True, timeout=150)
        log = args.report.with_suffix(".flash.log")
        log.write_text(flashed.stdout + flashed.stderr, encoding="utf8")
        report["flash_log_sha256"] = hashlib.sha256(log.read_bytes()).hexdigest()
        macs = re.findall(r"MAC:\s*([0-9a-f:]+)", flashed.stdout, re.I)
        if flashed.returncode or not macs or macs[-1].lower() != args.mac.lower():
            raise RuntimeError("flash failed or flashed MAC differs from expected device")
        report["test_firmware_left_installed"] = True
        # The connected M5Dial requires the normal stub-assisted run command.
        if args.chip == "esp32s3":
            reset = subprocess.run([*base, "run"], capture_output=True, text=True, timeout=30)
            report["run_log"] = reset.stdout + reset.stderr
            if reset.returncode:
                raise RuntimeError("stub-assisted application start failed")
        device = serial.Serial()
        device.port, device.baudrate, device.timeout = args.port, 115200, .2
        device.dtr = device.rts = False
        deadline = time.monotonic() + 20
        while True:
            try:
                device.open()
                break
            except serial.SerialException:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.2)
        deadline = time.monotonic() + 120
        pending_line = bytearray()
        with device:
            while time.monotonic() < deadline:
                pending_line.extend(device.readline())
                if len(pending_line) > 4096:
                    raise RuntimeError("oversized qualification console line")
                if not pending_line.endswith(b"\n"):
                    continue
                line = pending_line.decode("utf8", errors="replace").strip()
                pending_line.clear()
                if not line:
                    continue
                report["console"].append(line)
                match = re.search(r"RELEASE_WORKER (\{.*\})", line)
                if match:
                    record = json.loads(match[1])
                    report["records"].append(record)
                    print(json.dumps(record), flush=True)
                    if record["type"] == "failure":
                        raise RuntimeError(str(record))
                    if record["type"] == "complete":
                        break
                if "Guru Meditation" in line or "panic'ed" in line or "stack overflow" in line:
                    raise RuntimeError(line)
        starts = [r for r in report["records"] if r["type"] == "start"]
        ends = [r for r in report["records"] if r["type"] == "complete"]
        cycles = [r["cycle"] for r in report["records"] if r["type"] == "cycle"]
        if len(starts) != 1 or len(ends) != 1 or cycles != list(range(1, 31)):
            raise RuntimeError("missing lifecycle identity, cycles or completion")
        start, end = starts[0], ends[0]
        if start["chip"] != args.chip or start["idf"] != "v6.0.2" or start["stack_bytes"] != 6144:
            raise RuntimeError("unexpected lifecycle target, SDK or stack reservation")
        if end["checks"] != 220 or end["cycles"] != 30 or end["queued_cancellations"] != 3 or min(end["worker_headroom"], end["main_headroom"]) < 1024:
            raise RuntimeError(f"lifecycle checks or stack margin failed: {end}")
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"board": args.board, "passed": report["passed"], "error": report.get("error")}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
