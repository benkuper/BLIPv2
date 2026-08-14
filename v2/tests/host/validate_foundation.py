#!/usr/bin/env python3
"""Offline semantic validation for the BLIP V2 Milestone 0 foundation."""

from __future__ import annotations

import hashlib
import json
import math
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[3]
FIXTURES = REPO / "v2" / "tests" / "fixtures" / "v1"
BASELINE_COMMIT = "e567eeb5f20ba022595fd5b89a77fb17ba59ca94"


class ValidationError(AssertionError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"invalid JSON {path.relative_to(REPO)}: {error}") from error


@dataclass
class Cursor:
    data: bytes
    offset: int = 0

    def take(self, size: int) -> bytes:
        require(size >= 0 and self.offset + size <= len(self.data), "truncated binary fixture")
        result = self.data[self.offset : self.offset + size]
        self.offset += size
        return result

    def byte(self) -> int:
        return self.take(1)[0]


def unpack_msgpack(cursor: Cursor) -> Any:
    marker = cursor.byte()
    if marker <= 0x7F:
        return marker
    if marker >= 0xE0:
        return marker - 256
    if 0xA0 <= marker <= 0xBF:
        return cursor.take(marker & 0x1F).decode("utf-8")
    if 0x90 <= marker <= 0x9F:
        return [unpack_msgpack(cursor) for _ in range(marker & 0x0F)]
    if 0x80 <= marker <= 0x8F:
        return {unpack_msgpack(cursor): unpack_msgpack(cursor) for _ in range(marker & 0x0F)}
    if marker == 0xC0:
        return None
    if marker == 0xC2:
        return False
    if marker == 0xC3:
        return True
    if marker == 0xCA:
        return struct.unpack(">f", cursor.take(4))[0]
    if marker == 0xCC:
        return cursor.byte()
    if marker == 0xCD:
        return struct.unpack(">H", cursor.take(2))[0]
    if marker == 0xD0:
        return struct.unpack(">b", cursor.take(1))[0]
    if marker == 0xD1:
        return struct.unpack(">h", cursor.take(2))[0]
    if marker == 0xD2:
        return struct.unpack(">i", cursor.take(4))[0]
    if marker == 0xD9:
        return cursor.take(cursor.byte()).decode("utf-8")
    if marker == 0xDA:
        return cursor.take(struct.unpack(">H", cursor.take(2))[0]).decode("utf-8")
    if marker == 0xDC:
        return [unpack_msgpack(cursor) for _ in range(struct.unpack(">H", cursor.take(2))[0])]
    if marker == 0xDE:
        return {unpack_msgpack(cursor): unpack_msgpack(cursor) for _ in range(struct.unpack(">H", cursor.take(2))[0])}
    raise ValidationError(f"unsupported MessagePack marker 0x{marker:02x}")


def semantically_equal(actual: Any, expected: Any, path: str = "$.") -> None:
    if isinstance(expected, float):
        require(isinstance(actual, (int, float)) and math.isclose(float(actual), expected, rel_tol=1e-6, abs_tol=1e-6), f"numeric mismatch at {path}: {actual!r} != {expected!r}")
    elif isinstance(expected, dict):
        require(isinstance(actual, dict), f"expected object at {path}")
        require(actual.keys() == expected.keys(), f"object keys differ at {path}")
        for key in expected:
            semantically_equal(actual[key], expected[key], f"{path}{key}.")
    elif isinstance(expected, list):
        require(isinstance(actual, list) and len(actual) == len(expected), f"array mismatch at {path}")
        for index, item in enumerate(expected):
            semantically_equal(actual[index], item, f"{path}[{index}].")
    else:
        require(type(actual) is type(expected) and actual == expected, f"value mismatch at {path}: {actual!r} != {expected!r}")


def read_osc_string(cursor: Cursor) -> str:
    end = cursor.data.find(b"\0", cursor.offset)
    require(end >= cursor.offset, "unterminated OSC string")
    raw = cursor.data[cursor.offset:end]
    consumed = (end - cursor.offset) + 1
    padded = (consumed + 3) & ~3
    cursor.take(padded)
    return raw.decode("utf-8")


def parse_osc(data: bytes) -> dict[str, Any]:
    cursor = Cursor(data)
    address = read_osc_string(cursor)
    tags = read_osc_string(cursor)
    require(tags.startswith(","), "OSC type tag string must start with comma")
    arguments: list[Any] = []
    for tag in tags[1:]:
        if tag == "s":
            arguments.append(read_osc_string(cursor))
        elif tag == "i":
            arguments.append(struct.unpack(">i", cursor.take(4))[0])
        elif tag == "f":
            arguments.append(struct.unpack(">f", cursor.take(4))[0])
        elif tag == "r":
            arguments.append(list(cursor.take(4)))
        elif tag == "T":
            arguments.append(True)
        elif tag == "F":
            arguments.append(False)
        else:
            raise ValidationError(f"unsupported OSC tag {tag!r}")
    require(cursor.offset == len(data), f"trailing OSC bytes for {address}")
    return {"address": address, "type_tags": tags[1:], "arguments": arguments}


def parse_espnow(data: bytes) -> dict[str, Any]:
    cursor = Cursor(data)
    packet_type = cursor.byte()
    if packet_type == 0:
        start_id = cursor.byte()
        end_id = cursor.byte()
        address = cursor.take(cursor.byte()).decode("utf-8")
        command = cursor.take(cursor.byte()).decode("utf-8")
        values: list[dict[str, Any]] = []
        while cursor.offset < len(data):
            kind = chr(cursor.byte())
            if kind == "b":
                value: Any = bool(cursor.byte())
            elif kind == "i":
                value = struct.unpack("<i", cursor.take(4))[0]
            elif kind == "f":
                value = struct.unpack("<f", cursor.take(4))[0]
            elif kind == "s":
                value = cursor.take(cursor.byte()).decode("utf-8")
            elif kind == "p":
                value = list(cursor.take(cursor.byte()))
            else:
                raise ValidationError(f"unknown V1 ESP-NOW value type {kind!r}")
            values.append({"type": kind, "value": value})
        return {"type": 0, "ids": [start_id, end_id], "address": address, "command": command, "values": values}
    if packet_type == 1:
        universe, start_channel = struct.unpack(">HH", cursor.take(4))
        remaining = cursor.take(len(data) - cursor.offset)
        require(len(remaining) > 0 and len(remaining) % 3 == 0, "invalid ESP-NOW RGB stream length")
        return {"type": 1, "universe": universe, "start_channel": start_channel, "rgb": [list(remaining[index : index + 3]) for index in range(0, len(remaining), 3)]}
    if packet_type in (2, 4):
        channel = cursor.byte()
        require(cursor.offset == len(data), "trailing pairing/wake bytes")
        return {"type": packet_type, "wifi_channel": channel}
    if packet_type == 3:
        require(cursor.offset == len(data), "trailing pairing response bytes")
        return {"type": 3}
    raise ValidationError(f"unknown V1 ESP-NOW packet type {packet_type}")


def parse_v1_serial(line: str) -> dict[str, Any]:
    text = line.rstrip("\n")
    split_index = text.find(" ")
    target = text if split_index < 0 else text[:split_index]
    dot_index = target.rfind(".")
    component = "root" if dot_index < 0 else target[:dot_index]
    command = target if dot_index < 0 else target[dot_index + 1 :]
    args = "" if split_index < 0 else text[split_index + 1 :]
    values: list[dict[str, Any]] = []
    for token in args.split(",") if args else []:
        numeric = bool(token) and all(char.isdigit() or char == "." for char in token) and token.count(".") <= 1
        if numeric:
            number = float(token)
            if "." not in token and number == int(number):
                values.append({"type": "int", "value": int(number)})
            else:
                values.append({"type": "float", "value": number})
        else:
            values.append({"type": "string", "value": token})
    return {"component": component, "command": command, "values": values}


def validate_manifest() -> dict[str, dict[str, Any]]:
    manifest = load_json(FIXTURES / "manifest.json")
    require(manifest["fixture_set_version"] == 1, "unexpected fixture set version")
    require(manifest["baseline"]["commit"] == BASELINE_COMMIT, "fixture baseline drift")
    entries: dict[str, dict[str, Any]] = {}
    for entry in manifest["fixtures"]:
        relative = entry["path"]
        require(relative not in entries, f"duplicate fixture path {relative}")
        candidate = (FIXTURES / relative).resolve()
        require(FIXTURES.resolve() in candidate.parents, f"fixture escapes root: {relative}")
        require(candidate.is_file(), f"missing fixture {relative}")
        data = candidate.read_bytes()
        require(len(data) == entry["size"], f"fixture size mismatch: {relative}")
        require(hashlib.sha256(data).hexdigest() == entry["sha256"], f"fixture hash mismatch: {relative}")
        require(entry["evidence"] == "source-derived", f"unexpected evidence class: {relative}")
        require(entry["source_paths"] and all(path.startswith(("src/", "configs/", "platformio")) for path in entry["source_paths"]), f"missing source provenance: {relative}")
        entries[relative] = entry
    require(len(entries) >= 20, "fixture set is unexpectedly small")
    return entries


def validate_settings(entries: dict[str, dict[str, Any]]) -> None:
    expected = load_json(FIXTURES / "settings" / "representative.json")
    cursor = Cursor((FIXTURES / "settings" / "representative.msgpack").read_bytes())
    actual = unpack_msgpack(cursor)
    require(cursor.offset == len(cursor.data), "trailing MessagePack settings bytes")
    semantically_equal(actual, expected)
    require(entries["settings/representative.msgpack"]["expected"]["equals_json"] == "settings/representative.json", "settings manifest relation missing")

    imported = load_json(FIXTURES / "settings" / "expected-import.json")
    require(imported["source_format"] == "blip-settings-v1", "unexpected settings import source")
    indexed = {(item["legacy_component_path"], item["field"]): item["value"] for item in imported["values"]}
    require(indexed[("settings", "propID")] == 7, "propID import expectation missing")
    require(indexed[("leds.strip1", "maxPower")] == 1600, "strip import expectation missing")
    require(indexed[("comm.osc", "remotePort")] == 10000, "OSC setting import expectation missing")


def validate_serial() -> None:
    cases = [json.loads(line) for line in (FIXTURES / "serial" / "messages.ndjson").read_text(encoding="utf-8").splitlines()]
    require(len(cases) >= 7, "serial fixture coverage missing")
    for case in cases:
        require(case["raw"].endswith("\n"), "serial record is not newline terminated")
        if case["direction"] == "input" and "component" in case:
            parsed = parse_v1_serial(case["raw"])
            semantically_equal(parsed, {key: case[key] for key in ("component", "command", "values")})


def validate_osc(entries: dict[str, dict[str, Any]]) -> None:
    index = load_json(FIXTURES / "osc" / "messages.json")
    for item in index:
        parsed = parse_osc((FIXTURES / item["path"]).read_bytes())
        semantically_equal(parsed, {key: item[key] for key in ("address", "type_tags", "arguments")})
        semantically_equal(parsed, entries[item["path"]]["expected"])
    require({item["address"] for item in index} >= {"/yo", "/ping", "/settings/save", "/leds/strip1/brightness"}, "OSC fixture coverage missing")


def validate_espnow(entries: dict[str, dict[str, Any]]) -> None:
    index = load_json(FIXTURES / "espnow" / "packets.json")
    require({item["type"] for item in index} == {0, 1, 2, 3, 4}, "ESP-NOW packet families incomplete")
    for item in index:
        parsed = parse_espnow((FIXTURES / item["path"]).read_bytes())
        expected = {key: value for key, value in item.items() if key != "path"}
        semantically_equal(parsed, expected)
        semantically_equal(parsed, entries[item["path"]]["expected"])
        require(entries[item["path"]]["size"] <= 250, f"ESP-NOW fixture too large: {item['path']}")


def validate_playback() -> None:
    metadata = load_json(FIXTURES / "playback" / "demo.meta")
    expected = load_json(FIXTURES / "playback" / "demo.expected.json")
    data = (FIXTURES / "playback" / "demo.colors").read_bytes()
    pixel_count = expected["pixel_count"]
    frame_size = pixel_count * 4
    require(len(data) % frame_size == 0, "partial V1 playback frame")
    frames = []
    for frame_start in range(0, len(data), frame_size):
        frame = []
        for pixel_start in range(frame_start, frame_start + frame_size, 4):
            alpha, red, green, blue = data[pixel_start : pixel_start + 4]
            frame.append([red, green, blue, alpha])
        frames.append(frame)
    require(frames == expected["decoded_rgba_frames"], "V1 playback ARGB decode mismatch")
    require(len(frames) == expected["frame_count"] == 3, "V1 playback frame count mismatch")
    require(metadata["fps"] == expected["fps"] == 30, "V1 playback FPS mismatch")
    require(all(set(script) == {"name", "start", "end"} for script in metadata["scripts"]), "V1 playback script metadata mismatch")


def validate_oscquery() -> None:
    tree = load_json(FIXTURES / "oscquery" / "creatorsballv2.json")
    require(tree["FULL_PATH"] == "" and tree["ACCESS"] == 0, "invalid OSCQuery root")
    node_paths: set[str] = set()
    value_paths: set[str] = set()

    def walk(current: dict[str, Any]) -> None:
        path = current["FULL_PATH"]
        require(path not in node_paths, f"duplicate OSCQuery node {path}")
        node_paths.add(path)
        require(current["ACCESS"] == 0 and isinstance(current["CONTENTS"], dict), f"invalid OSCQuery node {path}")
        for key, child in current["CONTENTS"].items():
            expected_path = f"{path}/{key}"
            require(child["FULL_PATH"] == expected_path, f"OSCQuery path mismatch at {expected_path}")
            if "CONTENTS" in child:
                walk(child)
            else:
                require(child["ACCESS"] in (1, 3), f"invalid OSCQuery access at {expected_path}")
                require(child["TYPE"], f"missing OSCQuery type at {expected_path}")
                if child["TYPE"] != "I":
                    require("VALUE" in child, f"missing OSCQuery value at {expected_path}")
                require(expected_path not in value_paths, f"duplicate OSCQuery value {expected_path}")
                value_paths.add(expected_path)

    walk(tree)
    required_nodes = {
        "/comm/serial",
        "/comm/osc",
        "/comm/espnow",
        "/comm/server",
        "/settings",
        "/leds/strip1/playbackLayer",
        "/leds/strip1/streamLayer",
        "/leds/strip1/scriptLayer",
        "/leds/strip1/systemLayer",
        "/leds/strip1/fx",
        "/wifi",
        "/battery",
        "/files",
        "/script",
        "/dmxReceiver",
        "/buttons/button1",
        "/ir",
        "/motion",
    }
    require(required_nodes <= node_paths, f"OSCQuery component coverage missing: {sorted(required_nodes - node_paths)}")
    require(len(value_paths) >= 120, f"OSCQuery parameter/action coverage unexpectedly small: {len(value_paths)}")

    host = load_json(FIXTURES / "oscquery" / "host-info.json")
    require(host["OSC_PORT"] == 9000 and host["OSC_TRANSPORT"] == "UDP", "OSCQuery host transport mismatch")
    require(host["EXTENSIONS"]["ACCESS"] and host["EXTENSIONS"]["VALUE"], "OSCQuery host extensions missing")


def validate_target_profiles() -> None:
    schema = load_json(REPO / "v2" / "profiles" / "target-profile.schema.json")
    require(schema["properties"]["schema_version"]["const"] == 1, "target schema version mismatch")
    profiles = sorted((REPO / "v2" / "profiles" / "targets").glob("*.json"))
    require(len(profiles) == 3, "expected exactly three initial target profiles")
    targets: set[str] = set()
    ids: set[str] = set()
    for path in profiles:
        profile = load_json(path)
        require(profile["schema_version"] == 1, f"profile schema mismatch: {path.name}")
        require(re.fullmatch(r"[a-z0-9][a-z0-9-]*", profile["id"]) is not None, f"invalid profile ID: {path.name}")
        require(profile["id"] not in ids, f"duplicate profile ID: {profile['id']}")
        ids.add(profile["id"])
        targets.add(profile["idf_target"])
        require(profile["status"] == "planning", f"unsupported evidence claim in {path.name}")
        require(profile["build_contract"] == {"esp_idf": "6.0.x", "cpp_standard": "c++20", "exceptions": False, "rtti": False, "warnings_as_errors": True}, f"build contract drift: {path.name}")
        require(profile["reference_board"] == {"required": True, "id": None, "evidence": "planned"}, f"unverified board claim: {path.name}")
        require(profile["pins"] == {"policy": "board-manifest-required", "assignments": {}}, f"generic pin assignment found: {path.name}")
        require(profile["gate_a"]["status"] == "planned", f"unsupported Gate A claim: {path.name}")
        require(set(profile["gate_a"]["required_evidence"]) == {"clean-build", "flash", "boot", "registry-self-test"}, f"Gate A evidence mismatch: {path.name}")
    require(targets == {"esp32", "esp32s3", "esp32c6"}, "initial target matrix mismatch")


def validate_documentation() -> None:
    required = [
        "docs/v2/README.md",
        "docs/v2/source-baseline.md",
        "docs/v2/v1-inventory.md",
        "docs/v2/compatibility.md",
        "docs/v2/quality-gates.md",
        "docs/v2/dependency-map.md",
        "docs/v2/adr/0001-component-lifecycle-and-registry.md",
        "docs/v2/adr/0002-resource-ownership.md",
        "docs/v2/adr/0003-threading-and-scheduling.md",
        "docs/v2/adr/0004-memory-errors-and-observability.md",
        "docs/v2/adr/0005-persistence-and-migrations.md",
        "docs/v2/adr/0006-v1-compatibility-boundary.md",
    ]
    for relative in required:
        path = REPO / relative
        require(path.is_file() and path.stat().st_size > 200, f"missing/empty planning document: {relative}")
        text = path.read_text(encoding="utf-8")
        if "/adr/000" in relative:
            require("Status: Accepted" in text, f"ADR not accepted: {relative}")
        if relative != "docs/v2/README.md":
            require(BASELINE_COMMIT in text or "ADR-" in text or "work package" in text or "Gate" in text, f"document lacks traceability: {relative}")

    markdown_files = list((REPO / "docs" / "v2").rglob("*.md")) + list((REPO / "v2").rglob("README.md"))
    link_pattern = re.compile(r"\[[^\]]+\]\(([^)]+)\)")
    for markdown in markdown_files:
        for target in link_pattern.findall(markdown.read_text(encoding="utf-8")):
            target = target.strip().strip("<>").split("#", 1)[0]
            if not target or re.match(r"^[a-z]+://", target) or target.startswith("mailto:"):
                continue
            require((markdown.parent / target).resolve().exists(), f"broken local link in {markdown.relative_to(REPO)}: {target}")

    planned_directories = [
        "v2/firmware",
        "v2/components/blip_core",
        "v2/components/blip_resources",
        "v2/components/blip_storage",
        "v2/components/blip_transport",
        "v2/components/blip_oscquery",
        "v2/components/blip_led",
        "v2/components/blip_wasm",
        "v2/components/blip_fleet",
        "v2/boards",
        "v2/profiles",
        "v2/web",
        "v2/tools/kitchen",
        "v2/tests/fixtures/v1",
    ]
    for relative in planned_directories:
        require((REPO / relative).is_dir(), f"planned repository path missing: {relative}")


def validate_generated_files() -> None:
    result = subprocess.run(
        [sys.executable, str(FIXTURES / "generate.py"), "--check"],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=False,
    )
    require(result.returncode == 0, f"fixture generator drift:\n{result.stdout}{result.stderr}")


def main() -> int:
    try:
        validate_generated_files()
        print("PASS generated fixture drift")
        entries = validate_manifest()
        print("PASS fixture manifest")
        validate_settings(entries)
        print("PASS V1 settings MessagePack")
        validate_serial()
        print("PASS V1 serial grammar")
        validate_osc(entries)
        print("PASS V1 OSC datagrams")
        validate_espnow(entries)
        print("PASS V1 ESP-NOW packets")
        validate_playback()
        print("PASS V1 playback pair")
        validate_oscquery()
        print("PASS V1 OSCQuery snapshots")
        validate_target_profiles()
        print("PASS target profiles")
        validate_documentation()
        print("PASS documentation and layout")
    except (ValidationError, KeyError, IndexError, TypeError, ValueError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    print("Foundation validation passed (10 checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
