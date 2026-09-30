#!/usr/bin/env python3
"""Check the LED calculated-current limit over serial and restore strip settings."""

from __future__ import annotations

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import blip_serial_control as control


COMPONENT = "blip.output.strip0"


def request(port: str, operation: str, name: str, value: int | bool | None = None) -> dict:
    request_id = time.monotonic_ns() & 0xFFFFFFFF
    scalar = (f"b:{str(value).lower()}" if isinstance(value, bool) else f"i:{value}")
    values = () if value is None else (control.parse_scalar(scalar),)
    payload = control.encode_control(control.OPERATION_BY_NAME[operation],
                                     COMPONENT, name, values)
    envelope = control.encode_envelope(control.KIND_REQUEST, request_id, payload)
    return control.exchange(port, 115200, 4.0, envelope, request_id)


def get_integer(port: str, name: str) -> int:
    response = request(port, "get", name)
    values = response["values"]
    if len(values) != 1 or values[0]["type"] != "integer":
        raise control.ProtocolError(f"{name} did not return one integer")
    return int(values[0]["value"])


def get_boolean(port: str, name: str) -> bool:
    response = request(port, "get", name)
    values = response["values"]
    if len(values) != 1 or values[0]["type"] != "boolean":
        raise control.ProtocolError(f"{name} did not return one boolean")
    return bool(values[0]["value"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--budget-ma", type=int, default=50)
    parser.add_argument("--red", type=int, default=255)
    parser.add_argument("--temporarily-enable", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.budget_ma <= 5000 or not 1 <= args.red <= 255:
        parser.error("budget must be 1..5000 mA and red must be 1..255")

    original = {name: get_integer(args.port, name)
                for name in ("power_budget_ma", "red", "pixels", "brightness",
                             "failed_frames", "applied_frames")}
    original["enabled"] = get_boolean(args.port, "enabled")
    if args.budget_ma < original["pixels"] or original["brightness"] == 0:
        parser.error("budget is below the modeled pixel idle current or brightness is zero")
    if not original["enabled"] and not args.temporarily_enable:
        parser.error("strip is disabled; pass --temporarily-enable to test and restore it")
    restoration_errors: list[str] = []
    observation: dict[str, int] = {}
    try:
        request(args.port, "set", "power_budget_ma", args.budget_ma)
        if not original["enabled"]:
            request(args.port, "set", "enabled", True)
        request(args.port, "set", "red", args.red)
        time.sleep(0.4)
        observation = {name: get_integer(args.port, name)
                       for name in ("power_budget_ma", "red", "estimated_current_ma",
                                    "power_scale_q16", "failed_frames", "applied_frames")}
        observation["enabled"] = get_boolean(args.port, "enabled")
    finally:
        for name in ("red", "enabled", "power_budget_ma"):
            try:
                request(args.port, "set", name, original[name])
            except (OSError, control.ProtocolError) as error:
                restoration_errors.append(f"{name}: {error}")
    restored = {name: get_integer(args.port, name)
                for name in ("power_budget_ma", "red")}
    restored["enabled"] = get_boolean(args.port, "enabled")
    passed = (observation["power_budget_ma"] == args.budget_ma and
              observation["red"] == args.red and
              observation["enabled"] and
              observation["estimated_current_ma"] <= args.budget_ma and
              observation["power_scale_q16"] < 65535 and
              observation["failed_frames"] == original["failed_frames"] and
              observation["applied_frames"] > original["applied_frames"] and
              restored["red"] == original["red"] and
              restored["enabled"] == original["enabled"] and
              restored["power_budget_ma"] == original["power_budget_ma"] and
              not restoration_errors)
    report = {
        "schema_version": 1,
        "gate_id": "M5-2-LED-POWER-HIL",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "port": args.port,
        "original": original,
        "limited_observation": observation,
        "restored": restored,
        "restoration_errors": restoration_errors,
        "passed": passed,
        "limit": "Calculated channel-current model; no physical current measurement."
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"out": str(args.out), "passed": passed,
                      "restored": restored}, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
