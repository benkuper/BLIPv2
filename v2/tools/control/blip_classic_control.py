#!/usr/bin/env python3
"""BLIP COBS control over an opt-in Classic Bluetooth SPP connection."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any, Sequence

from blip_serial_control import (
    KIND_ERROR,
    KIND_REQUEST,
    KIND_RESPONSE,
    MAX_SERIAL_FRAME_BYTES,
    OPERATION_BY_NAME,
    ProtocolError,
    cobs_decode,
    cobs_encode,
    decode_control,
    decode_envelope,
    encode_control,
    encode_envelope,
    parse_scalar,
)


def exchange(address: str, channel: int, timeout: float, request: bytes,
             request_id: int) -> dict[str, Any]:
    if not hasattr(socket, "AF_BLUETOOTH") or not hasattr(socket, "BTPROTO_RFCOMM"):
        raise ProtocolError("this Python installation has no RFCOMM socket support")
    frame = cobs_encode(request)
    with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM,
                       socket.BTPROTO_RFCOMM) as connection:
        connection.settimeout(timeout)
        connection.connect((address, channel))
        connection.sendall(b"\x00" + frame)
        deadline = time.monotonic() + timeout
        received = bytearray()
        while time.monotonic() < deadline:
            connection.settimeout(max(0.1, deadline - time.monotonic()))
            chunk = connection.recv(128)
            if not chunk:
                raise ProtocolError("SPP connection closed before a response")
            for byte in chunk:
                if byte != 0:
                    if len(received) == MAX_SERIAL_FRAME_BYTES:
                        raise ProtocolError("SPP response exceeds protocol bound")
                    received.append(byte)
                    continue
                if not received:
                    continue
                kind, response_id, payload = decode_envelope(cobs_decode(bytes(received)))
                if response_id != request_id or kind not in (KIND_RESPONSE, KIND_ERROR):
                    raise ProtocolError("SPP response envelope mismatch")
                response = decode_control(payload)
                response["request_id"] = response_id
                response["ok"] = kind == KIND_RESPONSE and response["error_code"] == "none"
                if not response["ok"]:
                    raise ProtocolError(
                        f"{response['error_domain']}.{response['error_code']}: "
                        f"{response['detail']}", response
                    )
                return response
        raise ProtocolError("timed out waiting for an SPP response")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True, help="device Classic Bluetooth address")
    parser.add_argument("--channel", type=int, default=1, help="RFCOMM server channel")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("operation", choices=tuple(OPERATION_BY_NAME))
    parser.add_argument("component")
    parser.add_argument("control")
    parser.add_argument("values", nargs="*")
    arguments = parser.parse_args(argv)
    try:
        if not 1 <= arguments.channel <= 30 or arguments.timeout <= 0:
            raise ProtocolError("invalid RFCOMM channel or timeout")
        if arguments.operation == "get" and arguments.values:
            raise ProtocolError("get does not accept values")
        if arguments.operation == "set" and len(arguments.values) != 1:
            raise ProtocolError("set requires exactly one value")
        request_id = time.monotonic_ns() & 0xFFFFFFFF
        payload = encode_control(OPERATION_BY_NAME[arguments.operation],
                                 arguments.component, arguments.control,
                                 [parse_scalar(value) for value in arguments.values])
        request = encode_envelope(KIND_REQUEST, request_id, payload)
        response = exchange(arguments.address, arguments.channel, arguments.timeout,
                            request, request_id)
        print(json.dumps(response, ensure_ascii=False, sort_keys=True))
        return 0
    except (OSError, ProtocolError, ValueError, OverflowError) as error:
        if isinstance(error, ProtocolError) and error.response is not None:
            print(json.dumps(error.response, ensure_ascii=False, sort_keys=True), file=sys.stderr)
        else:
            print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True),
                  file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
