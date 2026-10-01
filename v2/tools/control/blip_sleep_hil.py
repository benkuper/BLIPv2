#!/usr/bin/env python3
"""Qualify HUZZAH32 timer wake over serial, restoring Wi-Fi and LEDs afterward."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


CLIENT = Path(__file__).with_name("blip_serial_control.py")


def control(port: str, operation: str, component: str, name: str, *values: str) -> dict[str, Any]:
    command = [sys.executable, str(CLIENT), "--port", port, "--timeout", "5",
               operation, component, name, *values]
    result = subprocess.run(command, capture_output=True, text=True, timeout=12, check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    reply = json.loads(result.stdout)
    if not reply.get("ok"):
        raise RuntimeError(str(reply))
    return reply


def integer(port: str, component: str, name: str) -> int:
    return int(control(port, "get", component, name)["values"][0]["value"])


def boolean(port: str, component: str, name: str) -> bool:
    return bool(control(port, "get", component, name)["values"][0]["value"])


def set_enabled(port: str, component: str, value: bool) -> None:
    control(port, "set", component, "enabled", f"b:{str(value).lower()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--duration-ms", type=int, default=2000)
    arguments = parser.parse_args()
    if not 1000 <= arguments.duration_ms <= 30000:
        parser.error("duration must be between 1000 and 30000 ms")

    wifi = "blip.transport.wifi"
    led = "blip.output.strip0"
    sleep = "blip.power.sleep"
    original = {wifi: boolean(arguments.port, wifi, "enabled"),
                led: boolean(arguments.port, led, "enabled")}
    result: dict[str, Any] = {"port": arguments.port,
                              "duration_ms": arguments.duration_ms,
                              "original": original,
                              "restoration_errors": []}
    try:
        before = integer(arguments.port, sleep, "completed")
        set_enabled(arguments.port, led, False)
        set_enabled(arguments.port, wifi, False)
        control(arguments.port, "action", sleep, "light_sleep", f"i:{arguments.duration_ms}")
        # Opening the serial client during light sleep can time out before the
        # timer wakes the chip. Do not send any control frames in that interval.
        time.sleep(arguments.duration_ms / 1000 + 0.5)
        deadline = time.monotonic() + arguments.duration_ms / 1000 + 8
        while time.monotonic() < deadline:
            try:
                if integer(arguments.port, sleep, "completed") > before:
                    break
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            time.sleep(0.25)
        result["observation"] = {
            "state": integer(arguments.port, sleep, "state"),
            "completed_before": before,
            "completed_after": integer(arguments.port, sleep, "completed"),
            "last_sleep_us": integer(arguments.port, sleep, "last_sleep_us"),
            "wake_cause_mask": integer(arguments.port, sleep, "wake_cause"),
            "last_error": integer(arguments.port, sleep, "last_error"),
        }
        observation = result["observation"]
        result["passed"] = (observation["state"] == 3 and
                            observation["completed_after"] == before + 1 and
                            observation["last_error"] == 0 and
                            observation["last_sleep_us"] >= arguments.duration_ms * 800 and
                            (observation["wake_cause_mask"] & (1 << 4)) != 0)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        result["error"] = str(error)
    finally:
        # A failed probe may still be in its bounded sleep window.
        for component in (wifi, led):
            for attempt in range(12):
                try:
                    set_enabled(arguments.port, component, original[component])
                    break
                except (RuntimeError, subprocess.TimeoutExpired) as error:
                    if attempt == 11:
                        result["restoration_errors"].append(f"{component}: {error}")
                    time.sleep(3)
        result["restored"] = {component: boolean(arguments.port, component, "enabled")
                              for component in (wifi, led)}
        result["passed"] = (result.get("passed", False) and
                            result["restored"] == original and
                            not result["restoration_errors"])
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
