#!/usr/bin/env python3
"""BLIP envelope v1 control client for UART and native USB serial ports."""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from typing import Any, Sequence


MAGIC = b"BLIP"
ENVELOPE_VERSION = 1
ENVELOPE_HEADER_BYTES = 24
MAX_ENVELOPE_PAYLOAD_BYTES = 512
MAX_ENVELOPE_BYTES = ENVELOPE_HEADER_BYTES + MAX_ENVELOPE_PAYLOAD_BYTES
MAX_SERIAL_FRAME_BYTES = MAX_ENVELOPE_BYTES + 4
CONTROL_VERSION = 1
CONTROL_HEADER_BYTES = 12
MAX_VALUES = 4

KIND_REQUEST = 1
KIND_RESPONSE = 2
KIND_ERROR = 4
PAYLOAD_CONTROL = 1

OPERATION_BY_NAME = {"get": 1, "set": 2, "action": 3}
OPERATION_NAME = {value: key for key, value in OPERATION_BY_NAME.items()}
VALUE_BOOLEAN = 0
VALUE_INTEGER = 1
VALUE_NUMBER = 2
VALUE_STRING = 3
VALUE_TYPE_NAME = {
    VALUE_BOOLEAN: "boolean",
    VALUE_INTEGER: "integer",
    VALUE_NUMBER: "number",
    VALUE_STRING: "string",
}
ERROR_DOMAIN_NAME = {
    0: "none",
    1: "registry",
    2: "lifecycle",
    3: "descriptor",
    4: "resource",
    5: "scheduler",
    6: "event",
    7: "storage",
    8: "diagnostics",
    9: "control",
    10: "transport",
}
ERROR_CODE_NAME = {
    0: "none",
    1: "invalid_argument",
    2: "invalid_state",
    3: "capacity_exceeded",
    4: "duplicate_id",
    5: "missing_dependency",
    6: "dependency_cycle",
    7: "validation_failed",
    8: "start_failed",
    9: "suspend_failed",
    10: "resume_failed",
    11: "stop_failed",
    12: "serialization_overflow",
    13: "resource_unavailable",
    14: "resource_conflict",
    15: "resource_reserved",
    16: "queue_full",
    17: "queue_faulted",
    18: "budget_exceeded",
    19: "cancelled",
    20: "wrong_task_context",
    21: "recursive_dispatch",
    22: "not_found",
    23: "io_failed",
    24: "storage_full",
    25: "corrupt_data",
    26: "incompatible_version",
    27: "verification_failed",
    28: "generation_exhausted",
}


class ProtocolError(RuntimeError):
    """A local framing error or a structured error returned by the device."""

    def __init__(self, message: str, response: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.response = response


@dataclass(frozen=True)
class Scalar:
    value_type: int
    value: Any


def cobs_encode(data: bytes) -> bytes:
    if not data or len(data) > MAX_ENVELOPE_BYTES:
        raise ProtocolError("invalid COBS input size")
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    output.append(0)
    if len(output) > MAX_SERIAL_FRAME_BYTES:
        raise ProtocolError("COBS frame exceeds protocol bound")
    return bytes(output)


def cobs_decode(frame: bytes) -> bytes:
    if not frame or len(frame) > MAX_SERIAL_FRAME_BYTES or frame.endswith(b"\x00"):
        raise ProtocolError("invalid delimited COBS frame")
    output = bytearray()
    offset = 0
    while offset < len(frame):
        code = frame[offset]
        offset += 1
        if code == 0 or code - 1 > len(frame) - offset:
            raise ProtocolError("invalid COBS code")
        output.extend(frame[offset : offset + code - 1])
        offset += code - 1
        if code != 0xFF and offset < len(frame):
            output.append(0)
    if not output or len(output) > MAX_ENVELOPE_BYTES:
        raise ProtocolError("decoded envelope exceeds protocol bound")
    return bytes(output)


def encode_envelope(kind: int, request_id: int, payload: bytes) -> bytes:
    if kind not in (KIND_REQUEST, KIND_RESPONSE, 3, KIND_ERROR):
        raise ProtocolError("invalid envelope kind")
    if len(payload) > MAX_ENVELOPE_PAYLOAD_BYTES:
        raise ProtocolError("envelope payload is too large")
    header = struct.pack(
        "<4sBBBBIHHII",
        MAGIC,
        ENVELOPE_VERSION,
        ENVELOPE_HEADER_BYTES,
        kind,
        0,
        request_id & 0xFFFFFFFF,
        PAYLOAD_CONTROL,
        len(payload),
        0,
        0,
    )
    envelope = bytearray(header + payload)
    struct.pack_into("<I", envelope, 20, zlib.crc32(envelope) & 0xFFFFFFFF)
    return bytes(envelope)


def decode_envelope(envelope: bytes) -> tuple[int, int, bytes]:
    if len(envelope) < ENVELOPE_HEADER_BYTES:
        raise ProtocolError("truncated envelope")
    magic, version, header_size, kind, flags, request_id, payload_type, payload_size, reserved, crc = (
        struct.unpack_from("<4sBBBBIHHII", envelope)
    )
    if (
        magic != MAGIC
        or version != ENVELOPE_VERSION
        or header_size != ENVELOPE_HEADER_BYTES
        or kind not in (KIND_REQUEST, KIND_RESPONSE, 3, KIND_ERROR)
        or flags != 0
        or payload_type != PAYLOAD_CONTROL
        or reserved != 0
        or payload_size > MAX_ENVELOPE_PAYLOAD_BYTES
        or len(envelope) != ENVELOPE_HEADER_BYTES + payload_size
    ):
        raise ProtocolError("invalid envelope header")
    verified = bytearray(envelope)
    struct.pack_into("<I", verified, 20, 0)
    if zlib.crc32(verified) & 0xFFFFFFFF != crc:
        raise ProtocolError("envelope CRC mismatch")
    return kind, request_id, envelope[ENVELOPE_HEADER_BYTES:]


def _validate_identifier(value: str, maximum: int, require_dot: bool) -> bytes:
    encoded = value.encode("ascii")
    allowed = "abcdefghijklmnopqrstuvwxyz0123456789_-."
    if (
        not encoded
        or len(encoded) > maximum
        or (require_dot and "." not in value)
        or any(character not in allowed for character in value)
    ):
        raise ProtocolError(f"invalid protocol identifier: {value!r}")
    return encoded


def encode_control(operation: int, component: str, control: str, values: Sequence[Scalar]) -> bytes:
    if operation not in OPERATION_NAME or len(values) > MAX_VALUES:
        raise ProtocolError("invalid control request")
    component_bytes = _validate_identifier(component, 63, True)
    control_bytes = _validate_identifier(control, 31, False)
    payload = bytearray(
        struct.pack(
            "<6BH4B",
            CONTROL_VERSION,
            operation,
            len(values),
            0,
            0,
            0,
            0,
            len(component_bytes),
            len(control_bytes),
            0,
            0,
        )
    )
    payload.extend(component_bytes)
    payload.extend(control_bytes)
    for scalar in values:
        if scalar.value_type == VALUE_BOOLEAN:
            encoded = bytes((1 if scalar.value else 0,))
        elif scalar.value_type == VALUE_INTEGER:
            encoded = struct.pack("<q", scalar.value)
        elif scalar.value_type == VALUE_NUMBER:
            if not math.isfinite(scalar.value):
                raise ProtocolError("numbers must be finite")
            encoded = struct.pack("<d", scalar.value)
        elif scalar.value_type == VALUE_STRING:
            encoded = scalar.value.encode("utf-8")
            if len(encoded) > 128:
                raise ProtocolError("string value exceeds 128 bytes")
        else:
            raise ProtocolError("invalid scalar type")
        payload.extend(struct.pack("<BBH", scalar.value_type, 0, len(encoded)))
        payload.extend(encoded)
    if len(payload) > MAX_ENVELOPE_PAYLOAD_BYTES:
        raise ProtocolError("control payload is too large")
    return bytes(payload)


def decode_control(payload: bytes) -> dict[str, Any]:
    if len(payload) < CONTROL_HEADER_BYTES:
        raise ProtocolError("truncated control message")
    version, operation, count, reserved0, domain, reserved1, code, component_size, control_size, detail_size, reserved2 = (
        struct.unpack_from("<6BH4B", payload)
    )
    if (
        version != CONTROL_VERSION
        or operation not in OPERATION_NAME
        or count > MAX_VALUES
        or reserved0 != 0
        or reserved1 != 0
        or reserved2 != 0
        or domain not in ERROR_DOMAIN_NAME
        or code not in ERROR_CODE_NAME
        or (code == 0) != (domain == 0)
        or component_size > 63
        or control_size > 31
        or detail_size > 63
    ):
        raise ProtocolError("invalid control header")
    offset = CONTROL_HEADER_BYTES
    text_size = component_size + control_size + detail_size
    if text_size > len(payload) - offset:
        raise ProtocolError("truncated control text")
    try:
        component = payload[offset : offset + component_size].decode("ascii")
        offset += component_size
        control = payload[offset : offset + control_size].decode("ascii")
        offset += control_size
        detail = payload[offset : offset + detail_size].decode("ascii")
        offset += detail_size
    except UnicodeDecodeError as error:
        raise ProtocolError("invalid control text") from error
    if any(ord(character) < 0x20 or ord(character) > 0x7E for character in detail):
        raise ProtocolError("invalid control error detail")
    _validate_identifier(component, 63, True)
    _validate_identifier(control, 31, False)
    values: list[dict[str, Any]] = []
    for _ in range(count):
        if len(payload) - offset < 4:
            raise ProtocolError("truncated scalar header")
        value_type, reserved, size = struct.unpack_from("<BBH", payload, offset)
        offset += 4
        if reserved != 0 or value_type not in VALUE_TYPE_NAME or size > len(payload) - offset:
            raise ProtocolError("invalid scalar header")
        encoded = payload[offset : offset + size]
        offset += size
        if value_type == VALUE_BOOLEAN:
            if size != 1 or encoded[0] > 1:
                raise ProtocolError("invalid boolean scalar")
            value: Any = encoded[0] == 1
        elif value_type == VALUE_INTEGER:
            if size != 8:
                raise ProtocolError("invalid integer scalar")
            value = struct.unpack("<q", encoded)[0]
        elif value_type == VALUE_NUMBER:
            if size != 8:
                raise ProtocolError("invalid number scalar")
            value = struct.unpack("<d", encoded)[0]
            if not math.isfinite(value):
                raise ProtocolError("non-finite number scalar")
        else:
            if size > 128:
                raise ProtocolError("string scalar exceeds protocol bound")
            try:
                value = encoded.decode("utf-8")
            except UnicodeDecodeError as error:
                raise ProtocolError("invalid UTF-8 string scalar") from error
        values.append({"type": VALUE_TYPE_NAME[value_type], "value": value})
    if offset != len(payload):
        raise ProtocolError("trailing control data")
    return {
        "operation": OPERATION_NAME[operation],
        "component": component,
        "control": control,
        "error_domain": ERROR_DOMAIN_NAME[domain],
        "error_code": ERROR_CODE_NAME[code],
        "detail": detail,
        "values": values,
    }


def parse_scalar(token: str) -> Scalar:
    if token.startswith("s:"):
        return Scalar(VALUE_STRING, token[2:])
    if token.startswith("i:"):
        return Scalar(VALUE_INTEGER, int(token[2:], 0))
    if token.startswith("n:"):
        return Scalar(VALUE_NUMBER, float(token[2:]))
    if token.startswith("b:"):
        value = token[2:].lower()
        if value not in ("true", "false"):
            raise ProtocolError("boolean values must be b:true or b:false")
        return Scalar(VALUE_BOOLEAN, value == "true")
    lowered = token.lower()
    if lowered in ("true", "false"):
        return Scalar(VALUE_BOOLEAN, lowered == "true")
    try:
        return Scalar(VALUE_INTEGER, int(token, 0))
    except ValueError:
        pass
    try:
        return Scalar(VALUE_NUMBER, float(token))
    except ValueError:
        return Scalar(VALUE_STRING, token)


def exchange(port: str, baud: int, timeout: float, request: bytes, request_id: int) -> dict[str, Any]:
    try:
        import serial  # type: ignore[import-untyped]
    except ImportError as error:
        raise ProtocolError("pyserial is required: python -m pip install pyserial") from error

    frame = cobs_encode(request)
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = baud
    connection.timeout = 0.05
    connection.write_timeout = timeout
    # Configure modem lines before opening. Setting them after opening can pulse EN/BOOT on
    # common ESP USB bridges and mix an entire boot log into the binary response window.
    connection.dtr = False
    connection.rts = False
    connection.open()
    try:
        # A newly flashed/rebooted target shares boot logs with the transport. Wait for a quiet
        # interval so text emitted before the transport starts cannot prefix the first COBS frame.
        quiet_since = time.monotonic()
        settle_deadline = quiet_since + min(timeout, 2.0)
        while time.monotonic() < settle_deadline:
            if connection.read(128):
                quiet_since = time.monotonic()
            elif time.monotonic() - quiet_since >= 0.5:
                break
        connection.reset_input_buffer()
        connection.write(b"\x00" + frame)
        connection.flush()
        deadline = time.monotonic() + timeout
        received = bytearray()
        discarding = False
        last_decode_error: ProtocolError | None = None
        while time.monotonic() < deadline:
            byte = connection.read(1)
            if not byte:
                continue
            if byte != b"\x00":
                if discarding:
                    continue
                if len(received) == MAX_SERIAL_FRAME_BYTES:
                    received.clear()
                    discarding = True
                    last_decode_error = ProtocolError("serial response exceeds protocol bound")
                    continue
                received.extend(byte)
                continue
            if discarding:
                received.clear()
                discarding = False
                continue
            if not received:
                continue
            try:
                kind, response_id, payload = decode_envelope(cobs_decode(bytes(received)))
            except ProtocolError as error:
                # A synchronized response starts after a zero delimiter, so console text
                # accumulated before it is discarded as a separate candidate.
                last_decode_error = error
                received.clear()
                continue
            if response_id != request_id:
                raise ProtocolError("serial response request ID mismatch")
            response = decode_control(payload)
            response["request_id"] = response_id
            response["ok"] = kind == KIND_RESPONSE and response["error_code"] == "none"
            if kind not in (KIND_RESPONSE, KIND_ERROR):
                raise ProtocolError("device returned an invalid envelope kind")
            if not response["ok"]:
                raise ProtocolError(
                    f"{response['error_domain']}.{response['error_code']}: {response['detail']}",
                    response,
                )
            return response
    finally:
        connection.close()
    if last_decode_error is not None:
        raise ProtocolError(f"no valid BLIP response ({last_decode_error})")
    raise ProtocolError(f"timed out waiting for a BLIP response on {port}")


def self_test() -> None:
    values = [
        Scalar(VALUE_BOOLEAN, True),
        Scalar(VALUE_INTEGER, -42),
        Scalar(VALUE_NUMBER, 3.5),
        Scalar(VALUE_STRING, "zero\x00inside"),
    ]
    payload = encode_control(OPERATION_BY_NAME["action"], "blip.self_test", "apply", values)
    envelope = encode_envelope(KIND_REQUEST, 0x1234ABCD, payload)
    assert cobs_decode(cobs_encode(envelope)[:-1]) == envelope
    kind, request_id, decoded_payload = decode_envelope(envelope)
    assert kind == KIND_REQUEST and request_id == 0x1234ABCD
    decoded = decode_control(decoded_payload)
    assert [item["value"] for item in decoded["values"]] == [True, -42, 3.5, "zero\x00inside"]
    corrupt = bytearray(envelope)
    corrupt[-1] ^= 1
    try:
        decode_envelope(bytes(corrupt))
    except ProtocolError:
        pass
    else:
        raise AssertionError("corrupt envelope was accepted")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port, for example COM8 or /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--self-test", action="store_true", help="test codecs without hardware")
    parser.add_argument("operation", nargs="?", choices=tuple(OPERATION_BY_NAME))
    parser.add_argument("component", nargs="?")
    parser.add_argument("control", nargs="?")
    parser.add_argument("values", nargs="*")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        if arguments.self_test:
            self_test()
            print(json.dumps({"ok": True, "self_test": "passed"}, sort_keys=True))
            return 0
        if not arguments.port or not arguments.operation or not arguments.component or not arguments.control:
            raise ProtocolError("--port, operation, component, and control are required")
        if arguments.operation == "get" and arguments.values:
            raise ProtocolError("get does not accept values")
        if arguments.operation == "set" and len(arguments.values) != 1:
            raise ProtocolError("set requires exactly one value")
        values = [parse_scalar(value) for value in arguments.values]
        operation = OPERATION_BY_NAME[arguments.operation]
        request_id = time.monotonic_ns() & 0xFFFFFFFF
        payload = encode_control(operation, arguments.component, arguments.control, values)
        request = encode_envelope(KIND_REQUEST, request_id, payload)
        response = exchange(arguments.port, arguments.baud, arguments.timeout, request, request_id)
        print(json.dumps(response, ensure_ascii=False, sort_keys=True))
        return 0
    except (ProtocolError, OverflowError, ValueError) as error:
        if isinstance(error, ProtocolError) and error.response is not None:
            print(json.dumps(error.response, ensure_ascii=False, sort_keys=True), file=sys.stderr)
        else:
            print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
