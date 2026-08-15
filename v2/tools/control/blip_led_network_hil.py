#!/usr/bin/env python3
"""Qualify OSCQuery, browser WebSocket, and UDP LED control on a device AP."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import urllib.request
import urllib.error
from typing import Any

from websockets.sync.client import connect


LED_PATH = "/blip/output/strip0"


def osc_string(value: str) -> bytes:
    encoded = value.encode("utf-8") + b"\0"
    return encoded + b"\0" * ((-len(encoded)) % 4)


def osc_message(address: str, values: list[Any]) -> bytes:
    tags = [","]
    payload = bytearray()
    for value in values:
        if isinstance(value, bool):
            tags.append("T" if value else "F")
        elif isinstance(value, int):
            tags.append("i")
            payload.extend(struct.pack(">i", value))
        elif isinstance(value, float):
            tags.append("f")
            payload.extend(struct.pack(">f", value))
        elif isinstance(value, str):
            tags.append("s")
            payload.extend(osc_string(value))
        else:
            raise TypeError(f"unsupported OSC value: {type(value).__name__}")
    return osc_string(address) + osc_string("".join(tags)) + payload


def read_osc_string(packet: bytes, offset: int) -> tuple[str, int]:
    end = packet.index(0, offset)
    value = packet[offset:end].decode("utf-8")
    return value, (end + 4) & ~3


def decode_osc(packet: bytes) -> tuple[str, list[Any]]:
    address, offset = read_osc_string(packet, 0)
    tags, offset = read_osc_string(packet, offset)
    if not tags.startswith(","):
        raise ValueError("OSC type tag string is missing")
    values: list[Any] = []
    for tag in tags[1:]:
        if tag == "i":
            values.append(struct.unpack_from(">i", packet, offset)[0])
            offset += 4
        elif tag == "f":
            values.append(struct.unpack_from(">f", packet, offset)[0])
            offset += 4
        elif tag == "s":
            value, offset = read_osc_string(packet, offset)
            values.append(value)
        elif tag == "T":
            values.append(True)
        elif tag == "F":
            values.append(False)
        else:
            raise ValueError(f"unsupported OSC response tag: {tag}")
    if offset != len(packet):
        raise ValueError("OSC response has trailing bytes")
    return address, values


def get(origin: str, query: str = "", accept: str = "application/json") -> tuple[bytes, str]:
    request = urllib.request.Request(origin + "/" + query, headers={"Accept": accept})
    with urllib.request.urlopen(request, timeout=5) as response:
        return response.read(), response.headers.get_content_type()


def find_path(value: Any, path: str) -> dict[str, Any] | None:
    if isinstance(value, dict):
        if value.get("FULL_PATH") == path:
            return value
        for child in value.values():
            found = find_path(child, path)
            if found is not None:
                return found
    elif isinstance(value, list):
        for child in value:
            found = find_path(child, path)
            if found is not None:
                return found
    return None


def assert_parameter(tree: dict[str, Any], parameter: str, expected: Any | None = None) -> Any:
    full_path = f"{LED_PATH}/{parameter}"
    node = find_path(tree, full_path)
    if node is None or node.get("BLIP_KIND") != "parameter":
        raise AssertionError(f"OSCQuery parameter missing: {full_path}")
    values = node.get("VALUE")
    actual = values[0] if isinstance(values, list) and len(values) == 1 else None
    if expected is not None and actual != expected:
        raise AssertionError(f"{full_path} expected {expected}, received {actual}")
    return actual


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", default="http://192.168.4.1")
    parser.add_argument("--expect-board")
    parser.add_argument("--expect-antenna")
    parser.add_argument("--expect-pin-count", type=int)
    args = parser.parse_args()
    observations: dict[str, Any] = {"origin": args.origin}
    try:
        body, content_type = get(args.origin, "", "text/html")
        if content_type != "text/html" or b"<html" not in body.lower():
            raise AssertionError("device root did not serve the browser shell")
        observations["browser_shell_bytes"] = len(body)

        body, _ = get(args.origin, "?HOST_INFO")
        host = json.loads(body)
        if host.get("OSC_PORT") != 9000 or host.get("OSC_TRANSPORT") != "UDP":
            raise AssertionError("OSCQuery host info has the wrong transport")
        observations["device_id"] = host.get("DEVICE_ID")

        body, _ = get(args.origin, "?config=1")
        tree = json.loads(body)
        assert_parameter(tree, "red")
        assert_parameter(tree, "green")
        pin_node = find_path(tree, f"{LED_PATH}/pin")
        selector = pin_node.get("BLIP_RESOURCE_SELECTOR") if pin_node else None
        if not isinstance(selector, dict) or selector.get("CLASS") != "gpio":
            raise AssertionError("LED pin is not advertised as a GPIO resource selector")
        observations["schema_led_present"] = True

        with urllib.request.urlopen(args.origin + "/api/resources", timeout=5) as response:
            resources = json.loads(response.read())
        if resources.get("schema_version") != 1 or not isinstance(resources.get("allocation_revision"), int):
            raise AssertionError("resource snapshot version or revision missing")
        board = resources.get("board", {})
        pins = resources.get("pins", [])
        if args.expect_board and board.get("id") != args.expect_board:
            raise AssertionError(f"board mismatch: {board.get('id')}")
        if args.expect_antenna and board.get("antenna") != args.expect_antenna:
            raise AssertionError(f"antenna mismatch: {board.get('antenna')}")
        if args.expect_pin_count is not None and len(pins) != args.expect_pin_count:
            raise AssertionError(f"pin count mismatch: {len(pins)}")
        led_pin = next((pin for pin in pins if any(owner.get("path") == "blip.output.strip0:pin" for owner in pin.get("owners", []))), None)
        if led_pin is None:
            raise AssertionError("LED pin lease missing from resource snapshot")
        if args.expect_antenna == "onboard":
            reserved = {pin.get("gpio"): pin.get("reason") for pin in pins if pin.get("state") == "reserved"}
            if reserved.get(3) != "onboard-antenna-rf-switch-power" or reserved.get(14) != "onboard-antenna-selected":
                raise AssertionError("onboard antenna GPIO reservations missing")
        observations["resource_snapshot"] = {
            "board": board.get("id"),
            "antenna": board.get("antenna"),
            "pins": len(pins),
            "revision": resources["allocation_revision"],
            "led_pin": led_pin.get("id"),
        }
        stale_request = urllib.request.Request(
            args.origin + "/api/resources/reassign",
            data=json.dumps({
                "schema_version": 1,
                "expected_revision": resources["allocation_revision"] + 1,
                "operation": "swap",
                "requester": "blip.output.strip0:pin",
                "previous_owner": "blip.output.strip0:pin",
                "target_resource": "gpio.1",
            }).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            urllib.request.urlopen(stale_request, timeout=5)
        except urllib.error.HTTPError as error:
            detail = error.read()
            if error.code != 409 or b"stale-revision" not in detail:
                raise AssertionError(f"stale reassignment returned HTTP {error.code}: {detail!r}") from error
        else:
            raise AssertionError("stale reassignment unexpectedly succeeded")
        observations["stale_reassignment_rejected"] = True

        ws_origin = args.origin.replace("http://", "ws://", 1).replace("https://", "wss://", 1)
        with connect(ws_origin + "/", open_timeout=5, close_timeout=2) as websocket:
            websocket.send(osc_message(f"{LED_PATH}/enabled", [True]))
            response = websocket.recv(timeout=5)
            if not isinstance(response, bytes):
                raise AssertionError(f"WebSocket returned text diagnostic: {response}")
            address, values = decode_osc(response)
            if address != f"{LED_PATH}/enabled" or values != [True]:
                raise AssertionError("WebSocket LED enable feedback mismatch")
            websocket.send(osc_message(f"{LED_PATH}/red", [11]))
            response = websocket.recv(timeout=5)
            if not isinstance(response, bytes):
                raise AssertionError(f"WebSocket returned text diagnostic: {response}")
            address, values = decode_osc(response)
            if address != f"{LED_PATH}/red" or values != [11]:
                raise AssertionError("WebSocket LED write feedback mismatch")
        observations["websocket_red"] = 11

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(5)
            udp.sendto(osc_message(f"{LED_PATH}/green", [22]), ("192.168.4.1", 9000))
            packet, _ = udp.recvfrom(4096)
        address, values = decode_osc(packet)
        if address != f"{LED_PATH}/green" or len(values) != 2 or values[1] != 22:
            raise AssertionError("UDP LED write feedback mismatch")
        observations["udp_green"] = 22
        observations["udp_feedback_device_id"] = values[0]

        body, _ = get(args.origin, "?config=1")
        updated = json.loads(body)
        assert_parameter(updated, "red", 11)
        assert_parameter(updated, "green", 22)
        assert_parameter(updated, "enabled", True)
        observations["shared_registry_readback"] = True

        with connect(ws_origin + "/", open_timeout=5, close_timeout=2) as websocket:
            websocket.send(osc_message(f"{LED_PATH}/enabled", [False]))
            response = websocket.recv(timeout=5)
            if not isinstance(response, bytes):
                raise AssertionError(f"WebSocket returned text diagnostic: {response}")
            address, values = decode_osc(response)
            if address != f"{LED_PATH}/enabled" or values != [False]:
                raise AssertionError("WebSocket LED disable feedback mismatch")
        observations["websocket_enabled_then_disabled"] = True
        print(json.dumps({"ok": True, "observations": observations}, sort_keys=True))
        return 0
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"ok": False, "observations": observations, "error": str(error)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
