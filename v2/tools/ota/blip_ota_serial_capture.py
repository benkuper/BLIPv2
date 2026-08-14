#!/usr/bin/env python3
"""Capture an OTA device serial log without toggling its reset lines."""

from __future__ import annotations

import argparse
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--reset",
        action="store_true",
        help="pulse RTS after opening so native USB consoles are reset and released",
    )
    args = parser.parse_args()

    try:
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required: python -m pip install pyserial") from error

    connection = serial.Serial()
    connection.port = args.port
    connection.baudrate = args.baud
    connection.timeout = 0.2
    connection.dtr = False
    connection.rts = False
    connection.open()
    if args.reset:
        connection.dtr = False
        connection.rts = True
        time.sleep(0.1)
        connection.rts = False
    deadline = time.monotonic() + args.seconds
    try:
        with args.output.open("wb") as output:
            while time.monotonic() < deadline:
                chunk = connection.read(4096)
                if chunk:
                    output.write(chunk)
                    output.flush()
    finally:
        connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
