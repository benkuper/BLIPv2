#!/usr/bin/env python3
"""BLIP envelope control over the optional NimBLE GATT service."""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from typing import Any, Sequence

from blip_serial_control import (
    KIND_ERROR,
    KIND_REQUEST,
    KIND_RESPONSE,
    MAX_ENVELOPE_BYTES,
    OPERATION_BY_NAME,
    ProtocolError,
    decode_control,
    decode_envelope,
    encode_control,
    encode_envelope,
    parse_scalar,
)


SERVICE_UUID = "6a6a83c0-64cf-43f4-8a3a-8bd713d24300"
RX_UUID = "6a6a83c0-64cf-43f4-8a3a-8bd713d24301"
TX_UUID = "6a6a83c0-64cf-43f4-8a3a-8bd713d24302"
CHUNK_HEADER_BYTES = 4
MAX_CHUNK_BYTES = 244


def split_frame(frame: bytes, frame_id: int, mtu: int) -> list[bytes]:
    capacity = min(MAX_CHUNK_BYTES, mtu - 3) - CHUNK_HEADER_BYTES
    if not 0 < len(frame) <= MAX_ENVELOPE_BYTES or capacity <= 0:
        raise ProtocolError("BLE MTU or envelope size is invalid")
    return [
        bytes(((1 if offset == 0 else 0) | (2 if offset + capacity >= len(frame) else 0),
               frame_id, offset & 0xFF, offset >> 8)) + frame[offset : offset + capacity]
        for offset in range(0, len(frame), capacity)
    ]


def append_chunk(buffer: bytearray, chunk: bytes, frame_id: int) -> bool:
    if len(chunk) <= CHUNK_HEADER_BYTES or len(chunk) > MAX_CHUNK_BYTES:
        raise ProtocolError("invalid BLE response chunk size")
    flags, received_id = chunk[:2]
    offset = int.from_bytes(chunk[2:4], "little")
    if (flags & ~3 or received_id != frame_id or offset != len(buffer)
            or bool(flags & 1) != (offset == 0)
            or len(buffer) + len(chunk) - CHUNK_HEADER_BYTES > MAX_ENVELOPE_BYTES):
        raise ProtocolError("invalid BLE response chunk sequence")
    buffer.extend(chunk[CHUNK_HEADER_BYTES:])
    return bool(flags & 2)


async def exchange(address: str | None, timeout: float, request: bytes,
                   request_id: int) -> dict[str, Any]:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise ProtocolError("bleak is required: python -m pip install bleak") from error

    target = address or await BleakScanner.find_device_by_name("BLIP-BLE", timeout=timeout)
    if target is None:
        raise ProtocolError("BLIP-BLE advertisement not found")
    chunks: asyncio.Queue[bytes] = asyncio.Queue()
    async with BleakClient(target, timeout=timeout) as client:
        if client.services.get_service(SERVICE_UUID) is None:
            raise ProtocolError("BLIP GATT service not found")
        await client.start_notify(TX_UUID, lambda _, data: chunks.put_nowait(bytes(data)))
        frame_id = request_id & 0xFF
        for chunk in split_frame(request, frame_id, client.mtu_size):
            await client.write_gatt_char(RX_UUID, chunk, response=True)
        response_bytes = bytearray()
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError("timed out waiting for BLE response")
            chunk = await asyncio.wait_for(chunks.get(), remaining)
            if append_chunk(response_bytes, chunk, frame_id):
                break
        kind, response_id, payload = decode_envelope(bytes(response_bytes))
        if response_id != request_id or kind not in (KIND_RESPONSE, KIND_ERROR):
            raise ProtocolError("BLE response envelope mismatch")
        response = decode_control(payload)
        response["request_id"] = response_id
        response["ok"] = kind == KIND_RESPONSE and response["error_code"] == "none"
        if not response["ok"]:
            raise ProtocolError(
                f"{response['error_domain']}.{response['error_code']}: {response['detail']}",
                response,
            )
        return response


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="BLE address; scan for BLIP-BLE if omitted")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("operation", choices=tuple(OPERATION_BY_NAME))
    parser.add_argument("component")
    parser.add_argument("control")
    parser.add_argument("values", nargs="*")
    arguments = parser.parse_args(argv)
    try:
        if arguments.operation == "get" and arguments.values:
            raise ProtocolError("get does not accept values")
        if arguments.operation == "set" and len(arguments.values) != 1:
            raise ProtocolError("set requires exactly one value")
        request_id = time.monotonic_ns() & 0xFFFFFFFF
        payload = encode_control(OPERATION_BY_NAME[arguments.operation],
                                 arguments.component, arguments.control,
                                 [parse_scalar(value) for value in arguments.values])
        request = encode_envelope(KIND_REQUEST, request_id, payload)
        response = asyncio.run(exchange(arguments.address, arguments.timeout,
                                        request, request_id))
        print(json.dumps(response, ensure_ascii=False, sort_keys=True))
        return 0
    except (ProtocolError, ValueError, OverflowError, TimeoutError) as error:
        if isinstance(error, ProtocolError) and error.response is not None:
            print(json.dumps(error.response, ensure_ascii=False, sort_keys=True), file=sys.stderr)
        else:
            print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
