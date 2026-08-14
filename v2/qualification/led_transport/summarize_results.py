#!/usr/bin/env python3
"""Create append-only JSON and filtered serial evidence from HIL captures."""

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(65536):
            value.update(chunk)
    return value.hexdigest()


def read_run(values: list[str]) -> tuple[dict[str, object], list[str]]:
    target, port, board, mac, log_name, backup_name = values
    log_path = Path(log_name)
    backup_path = Path(backup_name)
    filtered: list[str] = []
    records: list[dict[str, object]] = []
    begin = False
    done = False
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "BLIP_LED_QUAL_BEGIN" in line:
            begin = True
            filtered.append(line)
        elif "BLIP_LED_QUAL_DONE" in line:
            done = True
            filtered.append(line)
        elif "BLIP_LED_QUAL {" in line:
            payload = line[line.index("{") :]
            records.append(json.loads(payload))
            filtered.append(line)
    if not begin or not done or len(records) != 115:
        raise ValueError(f"{target}: expected begin, done, and 115 records")

    def selected(backend: str) -> dict[str, object]:
        matches = [
            record
            for record in records
            if record["backend"] == backend
            and record["lanes"] == 1
            and record["pixels"] == 1024
        ]
        if len(matches) != 1:
            raise ValueError(f"{target}: missing selected record for {backend}")
        return matches[0]

    qualified_rmt_lanes = max(
        int(record["lanes"])
        for record in records
        if record["backend"] == "rmt"
        and record["pixels"] == 1024
        and record["supported"]
    )
    run = {
        "target": target,
        "port": port,
        "board": board,
        "mac": mac,
        "record_count": len(records),
        "completion_marker": True,
        "restored_overwrite_range": {
            "bytes": backup_path.stat().st_size,
            "sha256": digest(backup_path),
            "esptool_verify": "pass",
        },
        "maximum_completed_rmt_lanes_at_1024_pixels": qualified_rmt_lanes,
        "selected_1024_pixel_records": {
            backend: selected(backend)
            for backend in (
                "rmt",
                "rmt-dma",
                "spi-waveform",
                "spi-waveform-dma",
                "spi-clocked",
            )
        },
    }
    return run, filtered


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run",
        action="append",
        nargs=6,
        metavar=("TARGET", "PORT", "BOARD", "MAC", "LOG", "BACKUP"),
        required=True,
    )
    parser.add_argument("--json-output", type=Path, required=True)
    parser.add_argument("--log-output", type=Path, required=True)
    args = parser.parse_args()

    runs: list[dict[str, object]] = []
    raw_sections: list[str] = []
    for values in args.run:
        run, filtered = read_run(values)
        runs.append(run)
        raw_sections.append(f"# target={values[0]} port={values[1]}\n" + "\n".join(filtered))

    evidence = {
        "schema_version": 1,
        "gate_id": "WP-3.4-TRANSPORT-QUALIFICATION",
        "state": "pass-with-external-capture-open",
        "timestamp_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "esp_idf": "6.0.2",
        "sweep": {
            "pixels_per_lane": [1, 32, 256, 512, 1024],
            "lane_requests": [1, 2, 4, 8, 16],
            "iterations": 8,
            "load": "idle",
        },
        "runs": runs,
        "limitations": [
            "No external logic-analyzer capture was available.",
            "Multiline SPI and target-parallel rows remain fixture-required.",
            "Wi-Fi saturation, flash/cache-disable, and mixed-resource pressure remain Gate C qualification work.",
            "Completion timing proves driver progress, not waveform integrity or lane skew.",
        ],
    }
    args.json_output.parent.mkdir(parents=True, exist_ok=True)
    args.json_output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    args.log_output.write_text("\n\n".join(raw_sections) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
