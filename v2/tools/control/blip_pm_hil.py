#!/usr/bin/env python3
"""Verify the pixel CPU lock and restore the original strip color."""

from __future__ import annotations

import argparse
import json
import time
from typing import Any

from blip_sleep_hil import boolean, control, integer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    arguments = parser.parse_args()

    port = arguments.port
    pm = "blip.power.manager"
    led = "blip.output.strip0"
    wifi = "blip.transport.wifi"
    original_red = integer(port, led, "red")
    original_enabled = boolean(port, led, "enabled")
    result: dict[str, Any] = {
        "port": port,
        "original_red": original_red,
        "original_led_enabled": original_enabled,
        "wifi_state_before": integer(port, wifi, "state"),
        "max_cpu_mhz": integer(port, pm, "max_cpu_mhz"),
        "min_cpu_mhz": integer(port, pm, "min_cpu_mhz"),
        "auto_light_sleep": boolean(port, pm, "auto_light_sleep"),
        "acquired_before": integer(port, pm, "acquired_frames"),
        "applied_before": integer(port, led, "applied_frames"),
        "failed_before": integer(port, led, "failed_frames"),
    }
    try:
        if not original_enabled:
            raise RuntimeError("strip must be enabled for the frame-lock test")
        control(port, "set", led, "red", f"i:{0 if original_red else 1}")
        time.sleep(0.2)
    except RuntimeError as error:
        result["error"] = str(error)
    finally:
        control(port, "set", led, "red", f"i:{original_red}")
        time.sleep(0.2)
        result.update({
            "restored_red": integer(port, led, "red"),
            "restored_led_enabled": boolean(port, led, "enabled"),
            "wifi_state_after": integer(port, wifi, "state"),
            "acquired_after": integer(port, pm, "acquired_frames"),
            "active_frame_locks": integer(port, pm, "active_frame_locks"),
            "release_errors": integer(port, pm, "release_errors"),
            "applied_after": integer(port, led, "applied_frames"),
            "failed_after": integer(port, led, "failed_frames"),
            "last_frame_us": integer(port, led, "last_frame_us"),
        })
        result["passed"] = ("error" not in result and
                            result["max_cpu_mhz"] == 160 and
                            result["min_cpu_mhz"] == 80 and
                            not result["auto_light_sleep"] and
                            result["acquired_after"] >= result["acquired_before"] + 2 and
                            result["applied_after"] >= result["applied_before"] + 2 and
                            result["failed_after"] == result["failed_before"] and
                            result["active_frame_locks"] == 0 and
                            result["release_errors"] == 0 and
                            result["restored_red"] == original_red and
                            result["restored_led_enabled"] == original_enabled and
                            result["wifi_state_after"] == result["wifi_state_before"])
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
