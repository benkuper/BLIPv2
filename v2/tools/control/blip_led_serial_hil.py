#!/usr/bin/env python3
"""Exercise the Milestone 3 LED component over BLIP serial control."""

from __future__ import annotations

import argparse
import json
import time
from typing import Any

import blip_serial_control as control


COMPONENT = "blip.output.strip0"


def exchange(
    port: str, timeout: float, operation: str, component: str, parameter: str, values: list[str]
) -> dict[str, Any]:
    request_id = time.monotonic_ns() & 0xFFFFFFFF
    payload = control.encode_control(
        control.OPERATION_BY_NAME[operation],
        component,
        parameter,
        [control.parse_scalar(value) for value in values],
    )
    request = control.encode_envelope(control.KIND_REQUEST, request_id, payload)
    return control.exchange(port, 115200, timeout, request, request_id)


def value_of(response: dict[str, Any]) -> Any:
    values = response["values"]
    if len(values) != 1:
        raise AssertionError(f"expected one response value, received {len(values)}")
    return values[0]["value"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--phase", required=True, choices=("configure", "verify"))
    parser.add_argument("--pin", required=True, type=int)
    parser.add_argument("--protocol", type=int, choices=(0, 1), default=0)
    parser.add_argument("--pixels", type=int, default=8)
    parser.add_argument("--expect-antenna", type=int, choices=(0, 1, 2))
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()
    observations: dict[str, Any] = {"port": args.port, "phase": args.phase}

    def call(operation: str, parameter: str, *values: str) -> dict[str, Any]:
        return exchange(args.port, args.timeout, operation, COMPONENT, parameter, list(values))

    def get(parameter: str) -> Any:
        result = value_of(call("get", parameter))
        observations[parameter] = result
        return result

    try:
        if args.expect_antenna is not None:
            antenna = value_of(
                exchange(
                    args.port,
                    args.timeout,
                    "get",
                    "blip.transport.wifi",
                    "antenna",
                    [],
                )
            )
            observations["antenna"] = antenna
            if antenna != args.expect_antenna:
                raise AssertionError(
                    f"antenna mismatch: expected {args.expect_antenna}, received {antenna}"
                )

        if args.phase == "configure":
            if get("enabled"):
                call("set", "enabled", "false")
            for parameter, value in (
                ("pin", args.pin),
                ("protocol", args.protocol),
                ("pixels", args.pixels),
                ("brightness", 255),
                ("red", 64),
                ("green", 16),
                ("blue", 4),
                ("white", 0),
            ):
                call("set", parameter, str(value))
            call("set", "enabled", "true")
            time.sleep(0.1)
            expected = {
                "enabled": True,
                "pin": args.pin,
                "protocol": args.protocol,
                "pixels": args.pixels,
                "brightness": 255,
                "red": 64,
                "green": 16,
                "blue": 4,
                "white": 0,
            }
            for parameter, expected_value in expected.items():
                actual = get(parameter)
                if actual != expected_value:
                    raise AssertionError(
                        f"{parameter} mismatch: expected {expected_value}, received {actual}"
                    )
            if get("applied_frames") < 1 or get("failed_frames") != 0:
                raise AssertionError("LED frame counters show no successful frame or a failure")
            observations["last_frame_us"] = get("last_frame_us")
            observations["worker_stack_headroom"] = get("worker_stack_headroom")
            observations["backend"] = get("backend")
            try:
                call("set", "pin", str(args.pin + 1))
            except control.ProtocolError as error:
                if error.response is None:
                    raise
                observations["live_transport_change"] = {
                    "error_domain": error.response["error_domain"],
                    "error_code": error.response["error_code"],
                    "detail": error.response["detail"],
                }
                if (
                    error.response["error_code"] != "invalid_state"
                    or error.response["detail"] != "disable-before-pin-or-protocol-change"
                ):
                    raise AssertionError("live transport change returned the wrong typed error")
            else:
                raise AssertionError("live transport change unexpectedly succeeded")
        else:
            expected = {
                "enabled": True,
                "pin": args.pin,
                "protocol": args.protocol,
                "pixels": args.pixels,
                "brightness": 255,
                "red": 64,
                "green": 16,
                "blue": 4,
                "white": 0,
            }
            for parameter, expected_value in expected.items():
                actual = get(parameter)
                if actual != expected_value:
                    raise AssertionError(
                        f"persisted {parameter} mismatch: expected {expected_value}, received {actual}"
                    )
            call("action", "blackout")
            time.sleep(0.1)
            for parameter in ("red", "green", "blue", "white"):
                if get(parameter) != 0:
                    raise AssertionError(f"blackout did not clear {parameter}")
            call("set", "enabled", "false")
            if get("enabled"):
                raise AssertionError("failed to leave output disabled")
            observations["applied_frames"] = get("applied_frames")
            observations["failed_frames"] = get("failed_frames")

        print(json.dumps({"ok": True, "observations": observations}, sort_keys=True))
        return 0
    except (AssertionError, control.ProtocolError, ValueError) as error:
        print(
            json.dumps(
                {"ok": False, "observations": observations, "error": str(error)},
                sort_keys=True,
            )
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
