"""Validate and collect M5StickC UART loss localization, without marking the burst gate passed."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/qualification/serial-burst"))
from run_burst import wire, source_snapshot


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(trial, build):
    if not trial["diagnostic_completed"] or trial["source_snapshot"] != source_snapshot(ROOT):
        raise RuntimeError("Incomplete diagnostic or changed production sources")
    if trial["board"] != "m5stack-m5stickc" or trial["pc_network_changed"] or trial["firmware_restore_requested"]:
        raise RuntimeError("Unexpected board/network/restore action")
    if trial["queue_overload_qualification"] or trial["subsequent_probe_value"] != 0:
        raise RuntimeError("Unexpected gate claim or unresponsive board")
    for name, info in trial["inputs"].items():
        artifact = build / name
        if sha(artifact) != info["sha256"] or artifact.stat().st_size != info["bytes"]:
            raise RuntimeError(f"Changed diagnostic artifact: {artifact}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=Path("build/m6-m5stickc-untraced-burst.json"))
    parser.add_argument("--baseline-build", type=Path, default=Path("build/m6-production-m5stickc"))
    parser.add_argument("--traced", type=Path, default=Path("build/m6-m5stickc-uart-trace-trial.json"))
    parser.add_argument("--traced-build", type=Path, default=Path("build/m6-m5stickc-uart-trace"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    traced = json.loads(args.traced.read_text(encoding="utf-8"))
    verify(baseline, args.baseline_build)
    verify(traced, args.traced_build)
    if baseline["trace_enabled"] or not traced["trace_enabled"] or traced["flashed_mac"] != "94:b9:7e:8b:b7:84":
        raise RuntimeError("Unexpected trace mode/device identity")
    for name, expected in traced["diagnostic_sha256"].items():
        if sha(ROOT / "v2/qualification/serial-burst" / name) != expected:
            raise RuntimeError(f"Trace diagnostic source changed: {name}")
    if sha(ROOT / "v2/qualification/serial-burst/run_burst.py") != baseline["diagnostic_sha256"]["run_burst.py"]:
        raise RuntimeError("Baseline runner changed")
    matches = []
    for trial in traced["trials"]:
        if not trial["trace_end_seen"]:
            raise RuntimeError("Trace dump did not finish")
        records = {r["request_id"]: r for r in trial["driver_traces"]}
        if not all(request in records for request in trial["requests"]):
            raise RuntimeError("Burst pre-driver trace is incomplete")
        if not all(records[request]["pre_driver_valid"] and records[request]["written"] == records[request]["encoded_bytes"] + 2 for request in trial["requests"]):
            raise RuntimeError("The burst was invalid before UART admission")
        for candidate in trial["invalid_candidates"]:
            damaged = bytes.fromhex(candidate["encoded_hex"])
            for repaired in candidate["one_byte_reconstructions"]:
                data = bytes.fromhex(repaired["encoded_hex"])
                kind, request, payload = wire.decode_envelope(wire.cobs_decode(data))
                if wire.decode_control(payload) != repaired["response"] or zlib.crc32(data) != repaired["frame_crc32"]:
                    raise RuntimeError("Offline reconstruction CRC/payload differs")
                before = records.get(request)
                if repaired["matches_valid_pre_driver_frame"]:
                    if not before or not before["pre_driver_valid"] or before["frame_crc32"] != zlib.crc32(data) or before["encoded_bytes"] != len(data) or before["written"] != len(data) + 2:
                        raise RuntimeError("Reconstruction does not match valid UART input")
                    if not any(data[:i["offset"]] + data[i["offset"] + 1:] == damaged and data[i["offset"]] == i["byte"] for i in repaired["insertions"]):
                        raise RuntimeError("Recorded deletion does not produce the raw candidate")
                    matches.append({"trial": trial["trial"], "request_id": request, "damaged_encoded_hex": candidate["encoded_hex"],
                                    "reconstructed_encoded_hex": repaired["encoded_hex"], "insertions": repaired["insertions"], "pre_driver_trace": before})
    if len(matches) < 1 or not any(t["errors"] for t in baseline["trials"]):
        raise RuntimeError("Loss localization was not reproduced")
    evidence = {"schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "result": "M5StickC replies validated immediately before UART were accepted in full by the driver but reached pyserial with missing bytes",
        "localization_passed": True, "serial_burst_gate_passed": False, "root_cause_status": "Open beyond the UART input boundary",
        "firmware_restore_requested": False, "test_firmware_left_installed": True,
        "matched_missing_byte_frames": matches, "baseline_report_sha256": sha(args.baseline), "baseline": baseline,
        "traced_report_sha256": sha(args.traced), "traced": traced,
        "limitations": [
            "A driver return count proves admission, not delivery on the physical wire. The UART driver/ISR, electrical path, USB bridge, Windows serial driver and pyserial remain possible locations.",
            "Some malformed candidates have more than one missing byte and are not reconstructed by the one-byte diagnostic. Console text between delimiters is also recorded as an invalid COBS candidate; it is expected and is not a damaged binary reply.",
            "The trace changes CPU/stack use; instrumentation still reproduced the loss in two of three trials, with the third healthy.",
            "Offline reconstructions are diagnostic inference checked against both envelope and pre-driver frame CRCs. They were never accepted by the control client.",
            "The diagnostic does not establish queue-overload acceptance or close the M5StickC's two omitted production HIL checks.",
        ]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "matched_frames": len(matches), "localization_passed": True, "serial_burst_gate_passed": False}))


if __name__ == "__main__":
    main()
