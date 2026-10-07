"""Record raw production UART burst loss; optionally correlate an opt-in driver trace."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_hil import Client, fixtures, source_snapshot
import blip_serial_control as wire


def reconstruct(candidate):
    """Diagnostic inference only: never admit a repaired frame to the client."""
    found = {}
    for offset in range(len(candidate) + 1):
        for value in range(256):
            repaired = candidate[:offset] + bytes([value]) + candidate[offset:]
            try:
                kind, request, payload = wire.decode_envelope(wire.cobs_decode(repaired))
                response = wire.decode_control(payload)
            except wire.ProtocolError:
                continue
            if kind not in (wire.KIND_ERROR, wire.KIND_RESPONSE):
                continue
            key = repaired.hex()
            entry = found.setdefault(key, {"encoded_hex": key, "request_id": request,
                                          "frame_crc32": zlib.crc32(repaired), "response": response, "insertions": []})
            entry["insertions"].append({"offset": offset, "byte": value})
    return list(found.values())


def trial(client, raw, number, trace):
    if client.upload(fixtures(ROOT)["workloads"])["error"] != "none":
        raise RuntimeError("Workload upload failed")
    client.request("set", "blip.wasm", "instruction_budget", 1000000)
    client.request("set", "blip.wasm", "deadline_ms", 50)
    raw.clear()
    requests = [client.send("action", "blip.wasm", "call0", "spin") for _ in range(12)]
    requests.append(client.send("action", "blip.wasm", "cancel_all"))
    item = {"trial": number, "requests": requests, "responses": [], "errors": []}
    for request in requests:
        try:
            item["responses"].append({"request_id": request, "response": client.receive(request)})
        except TimeoutError as exc:
            item["errors"].append({"request_id": request, "error": str(exc)})
    burst_raw = bytes(raw)
    if trace:
        # Dump only after the tested replies were sent. Further traffic cannot
        # alter the pre-driver ring already recorded for those requests.
        probe_id = client.send("get", "blip.bootstrap", "probe_value")
        probe = client.receive(probe_id)
        item["probe_request_id"] = probe_id
        item["probe_response"] = probe
        until = time.monotonic() + 5
        while b"UARTTRACE_END\n" not in raw and time.monotonic() < until:
            client.connection.read(max(1, client.connection.in_waiting))
        item["trace_end_seen"] = b"UARTTRACE_END\n" in raw
        traces, invalid_lines = [], []
        for matched in re.finditer(rb"UARTTRACE (\{[^\r\n]*\})", raw):
            try:
                traces.append(json.loads(matched[1]))
            except json.JSONDecodeError:
                invalid_lines.append(matched[1].hex())
        item["driver_traces"] = traces
        item["invalid_trace_lines"] = invalid_lines
    decoded, invalid = [], []
    for candidate in burst_raw.split(b"\0"):
        if not candidate:
            continue
        try:
            kind, identity, payload = wire.decode_envelope(wire.cobs_decode(candidate))
            decoded.append({"kind": kind, "request_id": identity, "encoded_hex": candidate.hex(),
                            "frame_crc32": zlib.crc32(candidate), "response": wire.decode_control(payload)})
        except wire.ProtocolError as exc:
            invalid.append({"encoded_hex": candidate.hex(), "error": str(exc), "one_byte_reconstructions": reconstruct(candidate)})
    item.update(raw_bytes=len(burst_raw), raw_sha256=hashlib.sha256(burst_raw).hexdigest(),
                independently_decoded=decoded, invalid_candidates=invalid)
    if trace:
        trace_by_id = {r["request_id"]: r for r in item["driver_traces"]}
        for entry in invalid:
            for reconstruction in entry["one_byte_reconstructions"]:
                before = trace_by_id.get(reconstruction["request_id"])
                reconstruction["driver_trace"] = before
                reconstruction["matches_valid_pre_driver_frame"] = bool(before and before["pre_driver_valid"] and
                    before["frame_crc32"] == reconstruction["frame_crc32"] and
                    before["encoded_bytes"] == len(bytes.fromhex(reconstruction["encoded_hex"])) and
                    before["written"] == before["encoded_bytes"] + 2)
    time.sleep(0.1)
    client.action("cancel_all")
    client.work("unload")
    return item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--chip", choices=["esp32"], default="esp32")
    parser.add_argument("--board", required=True)
    parser.add_argument("--trials", type=int, choices=range(1, 11), default=3)
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--flash", action="store_true", help="Flash app/otadata via ROM at 115200; no restore")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    inputs = ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig", "ota_data_initial.bin")
    for name in inputs:
        if not (args.build / name).is_file():
            parser.error(f"Missing build input: {args.build / name}")
    config = (args.build / "sdkconfig").read_text(encoding="utf-8-sig")
    if 'CONFIG_IDF_TARGET="esp32"' not in config:
        parser.error("This diagnostic targets an original ESP32 UART board")
    cache = (args.build / "CMakeCache.txt").read_text(encoding="utf-8-sig")
    selectors = {"m5stack-m5stickc": "BLIP_BOARD_M5STICKC", "adafruit-huzzah32": "BLIP_BOARD_ADAFRUIT_HUZZAH32"}
    if args.board not in selectors or not re.search(rf"^{selectors[args.board]}:[^=]+=ON$", cache, re.M):
        parser.error("Build does not select the requested UART board")
    if not re.search(r"^BLIP_ENABLE_WASM:[^=]+=ON$", cache, re.M):
        parser.error("Burst diagnostic requires the production WASM worker")
    if args.trace and "__wrap_uart_write_bytes" not in (args.build / "blip-v2.map").read_text(encoding="utf-8-sig"):
        parser.error("Requested UART trace is absent from this executable")
    report = {"board": args.board, "port": args.port, "chip": args.chip,
              "pc_network_changed": False, "firmware_restore_requested": False,
              "source_snapshot": source_snapshot(ROOT), "trace_enabled": args.trace,
              "source_commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
              "diagnostic_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in Path(__file__).parent.iterdir() if path.is_file() and path.suffix in (".cpp", ".cmake", ".py")},
              "inputs": {name: {"bytes": (args.build / name).stat().st_size,
                                "sha256": hashlib.sha256((args.build / name).read_bytes()).hexdigest()} for name in inputs},
              "trials": [], "queue_overload_qualification": False}
    client = None
    try:
        if args.flash:
            command = [sys.executable, "-m", "esptool", "--chip", args.chip, "--port", args.port, "--no-stub",
                       "--baud", "115200", "write-flash", "0x18000", str(args.build / "ota_data_initial.bin"),
                       "0x20000", str(args.build / "blip-v2.bin")]
            flashed = subprocess.run(command, capture_output=True, text=True, timeout=240)
            if flashed.returncode:
                raise RuntimeError(flashed.stdout + flashed.stderr)
            macs = re.findall(r"MAC:\s*([0-9a-f:]+)", flashed.stdout, re.I)
            if not macs:
                raise RuntimeError("No flashed device identity")
            report["flashed_mac"] = macs[-1].lower()
        client = Client(args.port)
        raw = bytearray()
        underlying = client.connection.read
        def record_read(size=1):
            data = underlying(size)
            raw.extend(data)
            return data
        client.connection.read = record_read
        for number in range(1, args.trials + 1):
            item = trial(client, raw, number, args.trace)
            report["trials"].append(item)
            print(json.dumps({"trial": number, "responses": len(item["responses"]), "timeouts": len(item["errors"]),
                "damaged_frames": len(item["invalid_candidates"]), "pre_driver_matches": sum(
                    r.get("matches_valid_pre_driver_frame", False) for e in item["invalid_candidates"] for r in e["one_byte_reconstructions"])}), flush=True)
        report["subsequent_probe_value"] = client.get("probe_value", "blip.bootstrap")
        report["diagnostic_completed"] = True
    except Exception as exc:
        report["error"] = str(exc)
        raise
    finally:
        try:
            if client:
                client.action("cancel_all")
                client.work("unload")
        finally:
            if client:
                client.connection.close()
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
