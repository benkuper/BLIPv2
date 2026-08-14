#!/usr/bin/env python3
"""Compare the production V1 importer output with every golden expected value."""

from __future__ import annotations

import argparse
import json
import math
import subprocess
from pathlib import Path
from typing import Any


MAPPINGS = {
    "comm": "blip.transport",
    "comm.serial": "blip.transport.serial",
    "comm.osc": "blip.osc",
    "comm.espnow": "blip.transport.espnow",
    "comm.server": "blip.oscquery",
    "settings": "blip.system.settings",
    "leds": "blip.led",
    "leds.strip1": "blip.led.strip.1",
    "leds.strip1.playbackLayer": "blip.led.strip.1.layer.playback",
    "leds.strip1.streamLayer": "blip.led.strip.1.layer.stream",
    "leds.strip1.scriptLayer": "blip.led.strip.1.layer.script",
    "leds.strip1.systemLayer": "blip.led.strip.1.layer.system",
    "leds.strip1.fx": "blip.led.strip.1.fx",
    "wifi": "blip.transport.wifi",
    "battery": "blip.power.battery",
    "files": "blip.storage.files",
    "script": "blip.wasm",
    "dmxReceiver": "blip.transport.dmx.receiver",
    "buttons": "blip.input.buttons",
    "buttons.button1": "blip.input.button.1",
    "ir": "blip.input.ir",
    "motion": "blip.sensor.motion",
}


def equal_value(actual: Any, expected: Any) -> bool:
    if isinstance(expected, float):
        return isinstance(actual, (int, float)) and math.isclose(
            float(actual), expected, rel_tol=1e-6, abs_tol=1e-7
        )
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(
            equal_value(a, e) for a, e in zip(actual, expected, strict=True)
        )
    return type(actual) is type(expected) and actual == expected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--expected", type=Path, required=True)
    args = parser.parse_args()

    completed = subprocess.run(
        [str(args.tool), str(args.source)], check=True, capture_output=True, text=True
    )
    actual = json.loads(completed.stdout)
    expected = json.loads(args.expected.read_text(encoding="utf-8"))
    expected_values = expected["values"]
    if actual["setting_count"] != len(expected_values) or actual["component_count"] != 22:
        raise AssertionError("import summary mismatch")
    if actual["source_sha256"] != "bd9bed78d610868b60ef820ef5f4471aab024331ebcf7fa86c154eed9344192e":
        raise AssertionError("source SHA-256 mismatch")
    if len(actual["values"]) != len(expected_values):
        raise AssertionError("imported value count mismatch")
    for index, (got, wanted) in enumerate(
        zip(actual["values"], expected_values, strict=True)
    ):
        path = wanted["legacy_component_path"]
        if got["legacy_component_path"] != path:
            raise AssertionError(f"record {index}: legacy path mismatch")
        if got["component_id"] != MAPPINGS[path]:
            raise AssertionError(f"record {index}: V2 component mapping mismatch")
        if got["field"] != wanted["field"]:
            raise AssertionError(f"record {index}: field mismatch")
        if not equal_value(got["value"], wanted["value"]):
            raise AssertionError(
                f"record {index}: value mismatch: {got['value']!r} != {wanted['value']!r}"
            )
    print(f"PASS legacy settings import: {len(expected_values)} values, 22 components")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
