#!/usr/bin/env python3
"""Generate deterministic BLIP V1 compatibility fixtures using stdlib only."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any, Iterable


BASELINE_COMMIT = "e567eeb5f20ba022595fd5b89a77fb17ba59ca94"
FIXTURE_DIR = Path(__file__).resolve().parent


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def ndjson_bytes(values: Iterable[dict[str, Any]]) -> bytes:
    return ("".join(json.dumps(value, separators=(",", ":")) + "\n" for value in values)).encode("utf-8")


def pack_msgpack(value: Any) -> bytes:
    """Encode the subset used by ArduinoJson settings fixtures."""
    if value is None:
        return b"\xc0"
    if value is False:
        return b"\xc2"
    if value is True:
        return b"\xc3"
    if isinstance(value, int):
        if 0 <= value <= 0x7F:
            return bytes([value])
        if -32 <= value < 0:
            return bytes([value & 0xFF])
        if 0 <= value <= 0xFF:
            return b"\xcc" + struct.pack(">B", value)
        if 0 <= value <= 0xFFFF:
            return b"\xcd" + struct.pack(">H", value)
        if -0x80 <= value < 0:
            return b"\xd0" + struct.pack(">b", value)
        if -0x8000 <= value < -0x80:
            return b"\xd1" + struct.pack(">h", value)
        return b"\xd2" + struct.pack(">i", value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("fixture floats must be finite")
        return b"\xca" + struct.pack(">f", value)
    if isinstance(value, str):
        encoded = value.encode("utf-8")
        size = len(encoded)
        if size < 32:
            return bytes([0xA0 | size]) + encoded
        if size <= 0xFF:
            return b"\xd9" + bytes([size]) + encoded
        return b"\xda" + struct.pack(">H", size) + encoded
    if isinstance(value, list):
        size = len(value)
        prefix = bytes([0x90 | size]) if size < 16 else b"\xdc" + struct.pack(">H", size)
        return prefix + b"".join(pack_msgpack(item) for item in value)
    if isinstance(value, dict):
        size = len(value)
        prefix = bytes([0x80 | size]) if size < 16 else b"\xde" + struct.pack(">H", size)
        return prefix + b"".join(pack_msgpack(key) + pack_msgpack(item) for key, item in value.items())
    raise TypeError(f"unsupported MessagePack fixture type: {type(value)!r}")


def osc_string(value: str) -> bytes:
    raw = value.encode("utf-8") + b"\0"
    return raw + (b"\0" * ((-len(raw)) % 4))


def osc_message(address: str, arguments: list[tuple[str, Any]]) -> bytes:
    tags = "," + "".join(tag for tag, _ in arguments)
    payload = bytearray(osc_string(address) + osc_string(tags))
    for tag, value in arguments:
        if tag == "s":
            payload.extend(osc_string(value))
        elif tag == "i":
            payload.extend(struct.pack(">i", value))
        elif tag == "f":
            payload.extend(struct.pack(">f", value))
        elif tag == "r":
            payload.extend(bytes(value))
        elif tag in ("T", "F"):
            continue
        else:
            raise ValueError(f"unsupported OSC fixture tag {tag!r}")
    return bytes(payload)


def title_case(name: str) -> str:
    if not name:
        return ""
    result = name[0].upper()
    was_special = name[0].isupper() or name[0].isdigit()
    for char in name[1:]:
        special = char.isupper() or char.isdigit()
        if special and not was_special:
            result += " "
        result += char
        was_special = special
    return result


def parameter(
    path: str,
    name: str,
    type_name: str,
    value: Any,
    *,
    read_only: bool = False,
    ranges: list[dict[str, Any]] | None = None,
    enum_values: list[str] | None = None,
) -> dict[str, Any]:
    shown_type = type_name
    shown_value = value
    result: dict[str, Any] = {
        "DESCRIPTION": title_case(name),
        "ACCESS": 1 if read_only else 3,
        "TYPE": shown_type,
        "FULL_PATH": f"{path}/{name}",
        "VALUE": shown_value if isinstance(shown_value, list) else [shown_value],
    }
    if type_name == "b":
        result["TYPE"] = "T" if value else "F"
    if enum_values is not None:
        result["TYPE"] = "s"
        result["VALUE"] = [enum_values[int(value)]]
        result["RANGE"] = [{"VALS": enum_values}]
    elif ranges is not None:
        result["RANGE"] = ranges
    return result


def action(path: str, name: str) -> dict[str, Any]:
    return {
        "DESCRIPTION": title_case(name),
        "ACCESS": 3,
        "TYPE": "I",
        "FULL_PATH": f"{path}/{name}",
    }


def node(name: str, path: str, contents: dict[str, Any]) -> dict[str, Any]:
    return {
        "DESCRIPTION": title_case(name),
        "FULL_PATH": path,
        "ACCESS": 0,
        "CONTENTS": contents,
    }


def enabled(path: str, value: bool = True) -> dict[str, Any]:
    return parameter(path, "enabled", "b", value)


def representative_settings() -> dict[str, Any]:
    return {
        "components": {
            "comm": {
                "enabled": True,
                "components": {
                    "serial": {"enabled": True, "sendFeedback": True},
                    "osc": {
                        "enabled": True,
                        "remoteHost": "192.0.2.10",
                        "remotePort": 10000,
                        "sendFeedback": True,
                        "isAlive": True,
                    },
                    "espnow": {
                        "enabled": False,
                        "pairingMode": False,
                        "longRange": False,
                        "optimalRange": False,
                        "channel": 6,
                        "autoPairing": True,
                        "pairOnAnyData": True,
                        "sendFeedback": False,
                    },
                    "server": {
                        "enabled": True,
                        "sendFeedback": True,
                        "sendDebugLogs": False,
                        "suspendUpdatesDuringUpload": True,
                        "suppressFeedbackDuringUpload": True,
                    },
                },
            },
            "settings": {
                "enabled": True,
                "propID": 7,
                "deviceName": "Fixture BLIP",
                "wakeUpButton": 19,
                "wakeUpState": True,
            },
            "leds": {
                "enabled": True,
                "count": 1,
                "components": {
                    "strip1": {
                        "enabled": True,
                        "updateRate": 60,
                        "dataPin": 3,
                        "enPin": 21,
                        "clkPin": 2,
                        "invertStrip": False,
                        "multiLedMode": 0,
                        "maxPower": 1600,
                        "components": {
                            "playbackLayer": {"enabled": True},
                            "streamLayer": {
                                "enabled": True,
                                "universe": 0,
                                "startChannel": 1,
                                "use16Bits": False,
                                "includeAlpha": False,
                                "clearOnNoReception": True,
                                "noReceptionTime": 1.0,
                            },
                            "scriptLayer": {"enabled": True},
                            "systemLayer": {"enabled": True},
                            "fx": {
                                "enabled": False,
                                "staticOffset": 0.0,
                                "offsetSpeed": 0.0,
                                "isolationSpeed": 0.0,
                                "isolationSmoothing": 0.0,
                                "isolationAxis": 0,
                                "swapOnFlip": False,
                                "showCalibration": False,
                            },
                        },
                    }
                },
            },
            "wifi": {
                "enabled": True,
                "mode": 0,
                "ssid": "fixture-network",
                "pass": "not-a-real-secret",
                "manualIP": "",
                "manualGateway": "",
                "channelScanMode": True,
                "txPower": 2,
                "wifiProtocol": 2,
            },
            "battery": {
                "enabled": True,
                "updateRate": 5,
                "feedbackRate": 0.5,
                "batteryPin": 6,
                "chargePin": 14,
                "chargeLedIntensity": 0.01,
                "rawMin": 222,
                "rawMax": 335,
                "lowBatteryThreshold": 3.5,
                "shutdownChargeNoSignal": 0,
                "shutdownChargeSignalTimeout": 0,
            },
            "files": {
                "enabled": True,
                "sdSCK": 8,
                "sdMiso": 5,
                "sdMosi": 4,
                "sdCS": 15,
            },
            "script": {
                "enabled": True,
                "updateRate": 50,
                "scriptAtLaunch": "",
            },
            "dmxReceiver": {"enabled": True, "updateRate": 60},
            "buttons": {
                "enabled": True,
                "count": 1,
                "components": {
                    "button1": {
                        "enabled": True,
                        "pin": 19,
                        "mode": 2,
                        "inverted": False,
                        "canShutDown": True,
                    }
                },
            },
            "ir": {
                "enabled": True,
                "pin1": 18,
                "pin2": 20,
                "value": 0.0,
                "keepValueOnReboot": False,
            },
            "motion": {
                "updateRate": 100,
                "connected": False,
                "sendLevel": 1,
                "orientationSendRate": 50,
                "sdaPin": 0,
                "sclPin": 1,
                "intPin": 7,
                "orientationXOffset": 0.0,
                "flatThresholds": [0.8, 2.0, 0.0],
                "accelThresholds": [0.8, 2.0, 4.0],
                "diffThreshold": 8.0,
                "semiFlatThreshold": 2.0,
                "loftieThreshold": 12.0,
                "singleThreshold": 25.0,
                "angleOffset": 0.0,
                "xOnCalibration": 0.0,
            },
        }
    }


def flatten_settings(value: dict[str, Any], path: str = "") -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for key, item in value.items():
        if key == "components":
            for component_name, component in item.items():
                child_path = f"{path}.{component_name}" if path else component_name
                records.extend(flatten_settings(component, child_path))
        else:
            records.append({"legacy_component_path": path or "root", "field": key, "value": item})
    return records


def oscquery_tree() -> dict[str, Any]:
    normal_range = [{"MIN": 0, "MAX": 1}]
    root_contents: dict[str, Any] = {
        name: action("", name)
        for name in ("shutdown", "restart", "standby", "switchToWifi", "switchToESPNow")
    }

    comm_contents: dict[str, Any] = {"enabled": enabled("/comm")}
    comm_contents["serial"] = node(
        "serial",
        "/comm/serial",
        {
            "enabled": enabled("/comm/serial"),
            "sendFeedback": parameter("/comm/serial", "sendFeedback", "b", True),
        },
    )
    comm_contents["osc"] = node(
        "osc",
        "/comm/osc",
        {
            "enabled": enabled("/comm/osc"),
            "remoteHost": parameter("/comm/osc", "remoteHost", "s", "192.0.2.10"),
            "remotePort": parameter("/comm/osc", "remotePort", "i", 10000),
            "sendFeedback": parameter("/comm/osc", "sendFeedback", "b", True),
            "isAlive": parameter("/comm/osc", "isAlive", "b", True),
        },
    )
    comm_contents["espnow"] = node(
        "espnow",
        "/comm/espnow",
        {
            "enabled": enabled("/comm/espnow", False),
            "pairingMode": parameter("/comm/espnow", "pairingMode", "b", False),
            "longRange": parameter("/comm/espnow", "longRange", "b", False),
            "optimalRange": parameter("/comm/espnow", "optimalRange", "b", False),
            "channel": parameter("/comm/espnow", "channel", "i", 6),
            "autoPairing": parameter("/comm/espnow", "autoPairing", "b", True),
            "pairOnAnyData": parameter("/comm/espnow", "pairOnAnyData", "b", True),
            "sendFeedback": parameter("/comm/espnow", "sendFeedback", "b", False),
        },
    )
    comm_contents["server"] = node(
        "server",
        "/comm/server",
        {
            "enabled": enabled("/comm/server"),
            "sendFeedback": parameter("/comm/server", "sendFeedback", "b", True),
            "sendDebugLogs": parameter("/comm/server", "sendDebugLogs", "b", False),
            "suspendUpdatesDuringUpload": parameter("/comm/server", "suspendUpdatesDuringUpload", "b", True),
            "suppressFeedbackDuringUpload": parameter("/comm/server", "suppressFeedbackDuringUpload", "b", True),
        },
    )
    root_contents["comm"] = node("comm", "/comm", comm_contents)

    settings_contents: dict[str, Any] = {
        "saveSettings": action("/settings", "saveSettings"),
        "clearSettings": action("/settings", "clearSettings"),
        "factoryReset": action("/settings", "factoryReset"),
        "enabled": enabled("/settings"),
        "propID": parameter("/settings", "propID", "i", 7),
        "deviceName": parameter("/settings", "deviceName", "s", "Fixture BLIP"),
        "deviceType": parameter("/settings", "deviceType", "s", "Creators Ball V2", read_only=True),
        "firmwareVersion": parameter("/settings", "firmwareVersion", "s", "1.2.0", read_only=True),
        "wakeUpButton": parameter("/settings", "wakeUpButton", "i", 19),
        "wakeUpState": parameter("/settings", "wakeUpState", "b", True),
    }
    root_contents["settings"] = node("settings", "/settings", settings_contents)

    playback_path = "/leds/strip1/playbackLayer"
    playback = node(
        "playbackLayer",
        playback_path,
        {
            "enabled": enabled(playback_path),
            "blendMode": parameter(playback_path, "blendMode", "i", 0),
            "idMode": parameter(playback_path, "idMode", "b", False),
            "loop": parameter(playback_path, "loop", "b", False),
        },
    )
    stream_path = "/leds/strip1/streamLayer"
    stream = node(
        "streamLayer",
        stream_path,
        {
            "enabled": enabled(stream_path),
            "blendMode": parameter(stream_path, "blendMode", "i", 0),
            "universe": parameter(stream_path, "universe", "i", 0),
            "startChannel": parameter(stream_path, "startChannel", "i", 1),
            "use16Bits": parameter(stream_path, "use16Bits", "b", False),
            "includeAlpha": parameter(stream_path, "includeAlpha", "b", False),
            "clearOnNoReception": parameter(stream_path, "clearOnNoReception", "b", True),
            "noReceptionTime": parameter(stream_path, "noReceptionTime", "f", 1.0),
        },
    )
    script_layer_path = "/leds/strip1/scriptLayer"
    script_layer = node(
        "scriptLayer",
        script_layer_path,
        {
            "enabled": enabled(script_layer_path),
            "blendMode": parameter(script_layer_path, "blendMode", "i", 0),
        },
    )
    system_path = "/leds/strip1/systemLayer"
    system_layer = node(
        "systemLayer",
        system_path,
        {
            "enabled": enabled(system_path),
            "blendMode": parameter(system_path, "blendMode", "i", 4),
            "showBattery": parameter(system_path, "showBattery", "b", False),
            "espSyncColor": parameter(system_path, "espSyncColor", "r", [0.0, 1.0, 1.0, 1.0]),
        },
    )
    fx_path = "/leds/strip1/fx"
    fx = node(
        "fx",
        fx_path,
        {
            "enabled": enabled(fx_path, False),
            "staticOffset": parameter(fx_path, "staticOffset", "f", 0.0),
            "offsetSpeed": parameter(fx_path, "offsetSpeed", "f", 0.0),
            "isolationSpeed": parameter(fx_path, "isolationSpeed", "f", 0.0),
            "isolationSmoothing": parameter(fx_path, "isolationSmoothing", "f", 0.0),
            "isolationAxis": parameter(
                fx_path,
                "isolationAxis",
                "i",
                0,
                enum_values=["Projected Angle", "Yaw", "Pitch", "Roll"],
            ),
            "swapOnFlip": parameter(fx_path, "swapOnFlip", "b", False),
            "showCalibration": parameter(fx_path, "showCalibration", "b", False),
        },
    )
    strip_path = "/leds/strip1"
    strip = node(
        "strip1",
        strip_path,
        {
            "enabled": enabled(strip_path),
            "updateRate": parameter(strip_path, "updateRate", "i", 60),
            "count": parameter(strip_path, "count", "i", 36),
            "dataPin": parameter(strip_path, "dataPin", "i", 3),
            "enPin": parameter(strip_path, "enPin", "i", 21),
            "clkPin": parameter(strip_path, "clkPin", "i", 2),
            "brightness": parameter(strip_path, "brightness", "f", 0.5, ranges=[{"MIN": 0, "MAX": 2}]),
            "invertStrip": parameter(strip_path, "invertStrip", "b", False),
            "multiLedMode": parameter(
                strip_path,
                "multiLedMode",
                "i",
                0,
                enum_values=["Full Color", "Single Color", "Two Colors"],
            ),
            "maxPower": parameter(strip_path, "maxPower", "i", 1600),
            "colorCorrection": parameter(strip_path, "colorCorrection", "b", True),
            "playbackLayer": playback,
            "streamLayer": stream,
            "scriptLayer": script_layer,
            "systemLayer": system_layer,
            "fx": fx,
        },
    )
    root_contents["leds"] = node(
        "leds",
        "/leds",
        {
            "enabled": enabled("/leds"),
            "count": parameter("/leds", "count", "i", 1),
            "strip1": strip,
        },
    )

    root_contents["wifi"] = node(
        "wifi",
        "/wifi",
        {
            "enabled": enabled("/wifi"),
            "mode": parameter("/wifi", "mode", "i", 0, enum_values=["Wifi", "AP", "Wifi+AP"]),
            "ssid": parameter("/wifi", "ssid", "s", "fixture-network"),
            "pass": parameter("/wifi", "pass", "s", "not-a-real-secret"),
            "manualIP": parameter("/wifi", "manualIP", "s", ""),
            "manualGateway": parameter("/wifi", "manualGateway", "s", ""),
            "channelScanMode": parameter("/wifi", "channelScanMode", "b", True),
            "txPower": parameter("/wifi", "txPower", "i", 2, enum_values=["15dBm", "17dBm", "19.5dBm", "20.5dBm"]),
            "wifiProtocol": parameter("/wifi", "wifiProtocol", "i", 2, enum_values=["11B", "11BG", "11BGN", "AX"]),
            "signal": parameter("/wifi", "signal", "f", 0.75),
        },
    )

    root_contents["battery"] = node(
        "battery",
        "/battery",
        {
            "enabled": enabled("/battery"),
            "updateRate": parameter("/battery", "updateRate", "i", 5),
            "feedbackRate": parameter("/battery", "feedbackRate", "f", 0.5),
            "batteryPin": parameter("/battery", "batteryPin", "i", 6),
            "chargePin": parameter("/battery", "chargePin", "i", 14),
            "chargeLedIntensity": parameter("/battery", "chargeLedIntensity", "f", 0.01, ranges=normal_range),
            "rawMin": parameter("/battery", "rawMin", "i", 222),
            "rawMax": parameter("/battery", "rawMax", "i", 335),
            "lowBatteryThreshold": parameter("/battery", "lowBatteryThreshold", "f", 3.5),
            "batteryLevel": parameter("/battery", "batteryLevel", "f", 0.72, read_only=True, ranges=normal_range),
            "voltage": parameter("/battery", "voltage", "f", 3.95, read_only=True, ranges=[{"MIN": 3.3, "MAX": 4.2}]),
            "charging": parameter("/battery", "charging", "b", False, read_only=True),
            "shutdownChargeNoSignal": parameter("/battery", "shutdownChargeNoSignal", "i", 0),
            "shutdownChargeSignalTimeout": parameter("/battery", "shutdownChargeSignalTimeout", "i", 0),
        },
    )

    root_contents["files"] = node(
        "files",
        "/files",
        {
            "enabled": enabled("/files"),
            "sdSCK": parameter("/files", "sdSCK", "i", 8),
            "sdMiso": parameter("/files", "sdMiso", "i", 5),
            "sdMosi": parameter("/files", "sdMosi", "i", 4),
            "sdCS": parameter("/files", "sdCS", "i", 15),
        },
    )

    root_contents["script"] = node(
        "script",
        "/script",
        {
            "enabled": enabled("/script"),
            "updateRate": parameter("/script", "updateRate", "i", 50),
            "scriptAtLaunch": parameter("/script", "scriptAtLaunch", "s", ""),
            "universe": parameter("/script", "universe", "i", 0),
            "startChannel": parameter("/script", "startChannel", "i", 1),
        },
    )

    root_contents["dmxReceiver"] = node(
        "dmxReceiver",
        "/dmxReceiver",
        {
            "enabled": enabled("/dmxReceiver"),
            "updateRate": parameter("/dmxReceiver", "updateRate", "i", 60),
        },
    )

    button_modes = [
        "Digital Input",
        "Digital Input Pullup",
        "Digital Input Pulldown",
        "Analog Input",
        "Digital Output",
        "Analog Output",
        "Digital Oscillator",
        "Analog Oscillator",
        "Touch",
    ]
    button_path = "/buttons/button1"
    root_contents["buttons"] = node(
        "buttons",
        "/buttons",
        {
            "enabled": enabled("/buttons"),
            "count": parameter("/buttons", "count", "i", 1),
            "button1": node(
                "button1",
                button_path,
                {
                    "enabled": enabled(button_path),
                    "pin": parameter(button_path, "pin", "i", 19),
                    "mode": parameter(button_path, "mode", "i", 2, enum_values=button_modes),
                    "inverted": parameter(button_path, "inverted", "b", False),
                    "value": parameter(button_path, "value", "f", 0.0, ranges=normal_range),
                    "multiPressCount": parameter(button_path, "multiPressCount", "i", 0, read_only=True),
                    "longPress": parameter(button_path, "longPress", "b", False, read_only=True),
                    "veryLongPress": parameter(button_path, "veryLongPress", "b", False, read_only=True),
                    "canShutDown": parameter(button_path, "canShutDown", "b", True),
                },
            ),
        },
    )

    root_contents["ir"] = node(
        "ir",
        "/ir",
        {
            "enabled": enabled("/ir"),
            "pin1": parameter("/ir", "pin1", "i", 18),
            "pin2": parameter("/ir", "pin2", "i", 20),
            "value": parameter("/ir", "value", "f", 0.0, ranges=normal_range),
            "keepValueOnReboot": parameter("/ir", "keepValueOnReboot", "b", False),
        },
    )

    motion_path = "/motion"
    root_contents["motion"] = node(
        "motion",
        motion_path,
        {
            "enabled": enabled(motion_path, False),
            "updateRate": parameter(motion_path, "updateRate", "i", 100),
            "connected": parameter(motion_path, "connected", "b", False),
            "sendLevel": parameter(motion_path, "sendLevel", "i", 1, enum_values=["None", "Orientation", "All"]),
            "orientationSendRate": parameter(motion_path, "orientationSendRate", "i", 50),
            "sdaPin": parameter(motion_path, "sdaPin", "i", 0),
            "sclPin": parameter(motion_path, "sclPin", "i", 1),
            "intPin": parameter(motion_path, "intPin", "i", 7),
            "throwState": parameter(motion_path, "throwState", "i", 0, enum_values=["None", "Flat", "Single", "Double", "Flat Front", "Loftie"]),
            "orientation": parameter(
                motion_path,
                "orientation",
                "fff",
                [0.0, 0.0, 0.0],
                read_only=True,
                ranges=[{"MIN": -180, "MAX": 180}, {"MIN": -90, "MAX": 90}, {"MIN": -180, "MAX": 180}],
            ),
            "accel": parameter(motion_path, "accel", "fff", [0.0, 0.0, 1.0], read_only=True),
            "gyro": parameter(motion_path, "gyro", "fff", [0.0, 0.0, 0.0], read_only=True),
            "linearAccel": parameter(motion_path, "linearAccel", "fff", [0.0, 0.0, 0.0], read_only=True),
            "projectedAngle": parameter(motion_path, "projectedAngle", "f", 0.0, read_only=True),
            "spinCount": parameter(motion_path, "spinCount", "i", 0, read_only=True),
            "spin": parameter(motion_path, "spin", "f", 0.0, read_only=True),
            "activity": parameter(motion_path, "activity", "f", 0.0, read_only=True),
            "orientationXOffset": parameter(motion_path, "orientationXOffset", "f", 0.0),
            "flatThresholds": parameter(motion_path, "flatThresholds", "fff", [0.8, 2.0, 0.0]),
            "accelThresholds": parameter(motion_path, "accelThresholds", "fff", [0.8, 2.0, 4.0]),
            "diffThreshold": parameter(motion_path, "diffThreshold", "f", 8.0),
            "semiFlatThreshold": parameter(motion_path, "semiFlatThreshold", "f", 2.0),
            "loftieThreshold": parameter(motion_path, "loftieThreshold", "f", 12.0),
            "singleThreshold": parameter(motion_path, "singleThreshold", "f", 25.0),
            "angleOffset": parameter(motion_path, "angleOffset", "f", 0.0),
            "xOnCalibration": parameter(motion_path, "xOnCalibration", "f", 0.0),
        },
    )

    return node("root", "", root_contents)


def host_info() -> dict[str, Any]:
    return {
        "EXTENSIONS": {
            "ACCESS": True,
            "CLIPMODE": False,
            "CRITICAL": False,
            "RANGE": True,
            "TAGS": False,
            "TYPE": True,
            "UNIT": False,
            "VALUE": True,
            "LISTEN": True,
            "PATH_ADDED": True,
            "PATH_REMOVED": True,
            "PATH_RENAMED": True,
            "PATH_CHANGED": False,
        },
        "NAME": "Fixture BLIP",
        "VERSION": "1.2.0",
        "DEVICE_TYPE": "Creators Ball V2",
        "DEVICE_ID": "02:00:00:00:00:07",
        "OSC_PORT": 9000,
        "OSC_TRANSPORT": "UDP",
    }


def espnow_message(start_id: int, end_id: int, address: str, command: str, values: list[tuple[str, Any]]) -> bytes:
    address_bytes = address.encode("utf-8")
    command_bytes = command.encode("utf-8")
    packet = bytearray([0, start_id, end_id, len(address_bytes)])
    packet.extend(address_bytes)
    packet.append(len(command_bytes))
    packet.extend(command_bytes)
    for kind, value in values:
        packet.extend(kind.encode("ascii"))
        if kind == "b":
            packet.append(1 if value else 0)
        elif kind == "i":
            packet.extend(struct.pack("<i", value))
        elif kind == "f":
            packet.extend(struct.pack("<f", value))
        elif kind == "s":
            encoded = value.encode("utf-8")
            packet.append(len(encoded))
            packet.extend(encoded)
        elif kind == "p":
            packet.append(len(value))
            packet.extend(value)
        else:
            raise ValueError(f"unsupported ESP-NOW fixture type {kind!r}")
    if len(packet) > 250:
        raise ValueError("V1 ESP-NOW fixture exceeds 250 bytes")
    return bytes(packet)


def generated_files() -> tuple[dict[str, bytes], dict[str, dict[str, Any]]]:
    files: dict[str, bytes] = {}
    metadata: dict[str, dict[str, Any]] = {}

    def add(path: str, data: bytes, format_name: str, sources: list[str], expected: Any) -> None:
        files[path] = data
        metadata[path] = {"format": format_name, "source_paths": sources, "expected": expected}

    settings = representative_settings()
    settings_sources = [
        "src/Common/Settings.cpp",
        "src/Component/Component.cpp",
        "src/Component/components/settings/SettingsComponent.cpp",
    ]
    add("settings/representative.json", json_bytes(settings), "json", settings_sources, {"nvs_namespace": "blip", "nvs_key": "settings"})
    add("settings/representative.msgpack", pack_msgpack(settings), "messagepack", settings_sources, {"equals_json": "settings/representative.json"})
    expected_import = {"schema_version": 1, "source_format": "blip-settings-v1", "values": flatten_settings(settings)}
    add("settings/expected-import.json", json_bytes(expected_import), "json", settings_sources, {"record_count": len(expected_import["values"])})

    query_sources = ["src/Component/Component.cpp", "src/Component/components/communication/server/WebServerComponent.cpp", "src/RootComponent.cpp"]
    tree = oscquery_tree()
    add("oscquery/creatorsballv2.json", json_bytes(tree), "oscquery-json", query_sources, {"profile": "creatorsballv2", "root_path": ""})
    add("oscquery/host-info.json", json_bytes(host_info()), "oscquery-host-info-json", query_sources, {"osc_port": 9000, "transport": "UDP"})

    serial_cases = [
        {"direction": "input", "raw": "yo\n", "special": "discovery"},
        {"direction": "output", "raw": 'wassup 02:00:00:00:00:07 "Creators Ball V2" "Fixture BLIP" "1.2.0"\n', "special": "discovery"},
        {"direction": "input", "raw": "settings.save\n", "component": "settings", "command": "save", "values": []},
        {"direction": "input", "raw": "leds.strip1.brightness 0.5\n", "component": "leds.strip1", "command": "brightness", "values": [{"type": "float", "value": 0.5}]},
        {"direction": "input", "raw": "leds.strip1.playbackLayer.play demo,1.25\n", "component": "leds.strip1.playbackLayer", "command": "play", "values": [{"type": "string", "value": "demo"}, {"type": "float", "value": 1.25}]},
        {"direction": "input", "raw": "script.setParam gain,-2\n", "component": "script", "command": "setParam", "values": [{"type": "string", "value": "gain"}, {"type": "string", "value": "-2"}], "note": "negative token is a V1 string"},
        {"direction": "output", "raw": "/leds_strip1.brightness 0.500000\n", "source": "/leds_strip1", "command": "brightness", "values": ["0.500000"]},
    ]
    add("serial/messages.ndjson", ndjson_bytes(serial_cases), "ndjson", ["src/Component/components/communication/serial/SerialComponent.cpp", "src/Common/StringHelpers.cpp"], {"records": len(serial_cases)})

    osc_cases = [
        ("osc/yo.osc", "/yo", [("s", "192.0.2.10")]),
        ("osc/brightness.osc", "/leds/strip1/brightness", [("f", 0.5)]),
        ("osc/save.osc", "/settings/save", []),
        ("osc/ping.osc", "/ping", [("s", "192.0.2.10")]),
    ]
    osc_index: list[dict[str, Any]] = []
    for path, address, args in osc_cases:
        add(path, osc_message(address, args), "osc-message", ["src/Component/components/communication/osc/OSCComponent.cpp"], {"address": address, "type_tags": "".join(tag for tag, _ in args), "arguments": [value for _, value in args]})
        osc_index.append({"path": path, "address": address, "type_tags": "".join(tag for tag, _ in args), "arguments": [value for _, value in args]})
    add("osc/messages.json", json_bytes(osc_index), "json", ["src/Component/components/communication/osc/OSCComponent.cpp"], {"records": len(osc_index)})

    esp_sources = ["src/Component/components/communication/espnow/ESPNowComponent.cpp", "src/Common/var.h"]
    esp_packets: list[tuple[str, bytes, dict[str, Any]]] = [
        ("espnow/message-float.bin", espnow_message(7, 7, "/leds/strip1", "brightness", [("f", 0.5)]), {"type": 0, "ids": [7, 7], "address": "/leds/strip1", "command": "brightness", "values": [{"type": "f", "value": 0.5}]}),
        ("espnow/message-typed.bin", espnow_message(255, 255, "/script", "setParam", [("s", "gain"), ("f", 0.25), ("i", -2), ("b", True), ("p", bytes([1, 2, 255]))]), {"type": 0, "ids": [255, 255], "address": "/script", "command": "setParam", "values": [{"type": "s", "value": "gain"}, {"type": "f", "value": 0.25}, {"type": "i", "value": -2}, {"type": "b", "value": True}, {"type": "p", "value": [1, 2, 255]}]}),
        ("espnow/stream.bin", bytes([1, 0, 2, 0, 1, 255, 0, 0, 0, 255, 0, 0, 0, 255]), {"type": 1, "universe": 2, "start_channel": 1, "rgb": [[255, 0, 0], [0, 255, 0], [0, 0, 255]]}),
        ("espnow/pairing-request.bin", bytes([2, 6]), {"type": 2, "wifi_channel": 6}),
        ("espnow/pairing-response.bin", bytes([3]), {"type": 3}),
        ("espnow/wake.bin", bytes([4, 6]), {"type": 4, "wifi_channel": 6}),
    ]
    esp_index: list[dict[str, Any]] = []
    for path, packet, expected in esp_packets:
        add(path, packet, "blip-espnow-v1", esp_sources, expected)
        esp_index.append({"path": path, **expected})
    add("espnow/packets.json", json_bytes(esp_index), "json", esp_sources, {"records": len(esp_index)})

    playback_meta = {
        "fps": 30,
        "group": 1,
        "id": 2,
        "groupColor": [1.0, 0.25, 0.0],
        "scripts": [{"name": "intro", "start": 0.0, "end": 0.1}],
    }
    rgba_frames = [
        [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 255, 128]],
        [[255, 255, 0, 255], [0, 255, 255, 255], [255, 0, 255, 255], [0, 0, 0, 0]],
        [[16, 32, 48, 64], [64, 48, 32, 16], [1, 2, 3, 4], [250, 125, 0, 200]],
    ]
    colors = bytes(channel for frame in rgba_frames for rgba in frame for channel in (rgba[3], rgba[0], rgba[1], rgba[2]))
    playback_sources = ["src/Component/components/ledstrip/Layer/layers/playback/LedStripPlaybackLayer.cpp", "src/Common/color.h"]
    add("playback/demo.meta", json_bytes(playback_meta), "blip-playback-v1-meta-json", playback_sources, {"color_file": "playback/demo.colors", "pixel_count": 4})
    add("playback/demo.colors", colors, "blip-playback-v1-argb8", playback_sources, {"pixel_count": 4, "frame_count": 3, "frame_size": 16, "byte_order": "ARGB"})
    decoded = {"pixel_count": 4, "frame_count": 3, "fps": 30, "source_byte_order": "ARGB", "decoded_rgba_frames": rgba_frames}
    add("playback/demo.expected.json", json_bytes(decoded), "json", playback_sources, {"equals_decoded": "playback/demo.colors"})

    return files, metadata


def build_manifest(files: dict[str, bytes], metadata: dict[str, dict[str, Any]]) -> bytes:
    fixtures = []
    for path in sorted(files):
        fixtures.append(
            {
                "path": path,
                "sha256": hashlib.sha256(files[path]).hexdigest(),
                "size": len(files[path]),
                "evidence": "source-derived",
                **metadata[path],
            }
        )
    manifest = {
        "fixture_set_version": 1,
        "baseline": {
            "repository": "https://github.com/Golden-Geek/BLIP.git",
            "commit": BASELINE_COMMIT,
            "firmware_version": "1.2.0",
            "representative_profile": "creatorsballv2",
        },
        "generated_by": "v2/tests/fixtures/v1/generate.py",
        "fixtures": fixtures,
    }
    return json_bytes(manifest)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if checked-in files differ; write nothing")
    args = parser.parse_args()

    files, metadata = generated_files()
    files["manifest.json"] = build_manifest(files, metadata)
    mismatches: list[str] = []

    for relative_path, expected in files.items():
        destination = FIXTURE_DIR / relative_path
        if args.check:
            if not destination.exists():
                mismatches.append(f"missing: {relative_path}")
            elif destination.read_bytes() != expected:
                mismatches.append(f"changed: {relative_path}")
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(expected)

    if mismatches:
        print("fixture generation check failed:", file=sys.stderr)
        for mismatch in mismatches:
            print(f"  {mismatch}", file=sys.stderr)
        return 1

    verb = "verified" if args.check else "generated"
    print(f"{verb} {len(files) - 1} fixtures and manifest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
