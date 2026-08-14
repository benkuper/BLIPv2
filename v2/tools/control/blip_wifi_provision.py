#!/usr/bin/env python3
"""Provision BLIP Wi-Fi over serial from a local two-line credentials file."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import blip_serial_control as control


def read_credentials(path: Path) -> tuple[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if len(lines) != 2 or not lines[0]:
        raise ValueError("credentials file must contain SSID then password on exactly two lines")
    if len(lines[0].encode("utf-8")) > 32:
        raise ValueError("SSID exceeds 32 UTF-8 bytes")
    if any(ord(character) < 0x20 or ord(character) > 0x7E for character in lines[1]):
        raise ValueError("password must contain printable ASCII characters only")
    password_size = len(lines[1])
    if password_size != 0 and not 8 <= password_size <= 63:
        raise ValueError("password must contain 8-63 ASCII characters (or be empty)")
    return lines[0], lines[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM8")
    parser.add_argument("--credentials", required=True, type=Path)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    try:
        ssid, password = read_credentials(args.credentials)
        values = [
            control.Scalar(control.VALUE_STRING, ssid),
            control.Scalar(control.VALUE_STRING, password),
        ]
        request_id = time.monotonic_ns() & 0xFFFFFFFF
        payload = control.encode_control(
            control.OPERATION_BY_NAME["action"],
            "blip.transport.wifi",
            "provision",
            values,
        )
        request = control.encode_envelope(control.KIND_REQUEST, request_id, payload)
        response = control.exchange(args.port, args.baud, args.timeout, request, request_id)
        print(json.dumps({"ok": response["ok"], "provisioned_ssid": ssid}, sort_keys=True))
        return 0
    except (OSError, UnicodeDecodeError, ValueError, control.ProtocolError) as error:
        if isinstance(error, control.ProtocolError) and error.response is not None:
            print(json.dumps(error.response, ensure_ascii=False, sort_keys=True))
        else:
            print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
