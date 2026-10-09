#!/usr/bin/env python3
"""Validate the six required production worker profiles and guarded traffic."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_hil import source_snapshot

PROFILES = {
    "huzzah32": ("adafruit-huzzah32", "esp32", "BLIP_BOARD_ADAFRUIT_HUZZAH32", "30:ae:a4:f2:d1:84"),
    "club": ("creators-club", "esp32", "BLIP_BOARD_CREATORS_CLUB", "30:ae:a4:f3:a3:88"),
    "m5stickc": ("m5stack-m5stickc", "esp32", "BLIP_BOARD_M5STICKC", "94:b9:7e:8b:b7:84"),
    "m5dial": ("m5stack-m5dial", "esp32s3", None, "b0:81:84:96:83:74"),
    "ball": ("creators-ball-v2", "esp32c6", "BLIP_BOARD_CREATORS_BALL_V2", "08:92:72:f6:4d:dc"),
    "xiao": ("seeed-xiao-esp32c6-chip-antenna", "esp32c6", None, "10:51:db:1a:a6:68"),
}


def read_text(path):
    data = path.read_bytes()
    return data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")


def read_json(path):
    return json.loads(read_text(path))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_trial(report, build, snapshot):
    assert report["passed"] and report["checks"] and all(check["passed"] for check in report["checks"])
    assert report["source_snapshot"] == snapshot, "trial source changed"
    for name, expected in report["artifacts"].items():
        assert sha(build / name) == expected, f"artifact changed: {build / name}"


def app_partition_bytes(build):
    data = (build / "partition_table/partition-table.bin").read_bytes()
    for offset in range(0, len(data) - 31, 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack_from("<HBBLL16sL", data, offset)
        if magic == 0x50aa and kind == 0 and subtype == 0x10:
            assert address == 0x20000
            return size
    raise AssertionError("missing OTA0 partition")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-prefix", default="build/m6-production-")
    parser.add_argument("--coexistence", nargs="+", choices=PROFILES, default=["huzzah32", "club"])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    snapshot = source_snapshot(ROOT)
    engine = ROOT / "build/wasm-deps/wamr"
    revision = subprocess.check_output(["git", "-C", str(engine), "rev-parse", "HEAD"], text=True).strip()
    assert revision == "25bd7eb63e828e4bd242cc9b38d260b4b31c6605"
    assert not subprocess.check_output(["git", "-C", str(engine), "status", "--porcelain"], text=True).strip()
    baseline = read_json(ROOT / "docs/v2/evidence/fleet/2026-10-01-seven-board-build.json")
    boards = []
    for profile, (board, target, selector, mac) in PROFILES.items():
        build = ROOT / (args.build_prefix + profile)
        trial_path = Path(str(build) + "-hil.json")
        trial = read_json(trial_path)
        validate_trial(trial, build, snapshot)
        omitted = ["queue-reject-new", "overflow-completions"] if profile == "m5stickc" else []
        assert trial["omitted_checks"] == omitted
        assert trial["board"] == board and trial["cycles"] >= 20 and len(trial["checks"]) >= 161 - len(omitted)
        steady = next(check for check in trial["checks"] if check["name"] == "non-growing-load-cycles")
        assert steady["cycles"] >= 20 and steady["after"] >= steady["before"]
        metrics = trial["metrics"]
        assert metrics["state"] == "ready" and metrics["pool_reserved"] == 81920 and metrics["pool_used"] == 0
        assert 0 < metrics["pool_peak"] <= 81920
        assert metrics["worker_stack_headroom"] >= 2048 and metrics["supervisor_stack_headroom"] >= 1024
        cache = dict(re.findall(r"^([A-Za-z0-9_]+):[^=\r\n]+=([^\r\n]+)$",
                               read_text(build / "CMakeCache.txt").replace("\r\n", "\n"), re.M))
        assert cache["IDF_TARGET"] == target
        enabled = ("WASM", "ESPNOW", "FLEET", "DDP", "ARTNET")
        assert all(cache[f"BLIP_ENABLE_{feature}"] == "ON" for feature in enabled)
        assert cache["BLIP_ENABLE_BLE"] == ("ON" if profile == "ball" else "OFF")
        if selector:
            assert cache[selector] == "ON"
        config = read_text(build / "sdkconfig")
        assert "CONFIG_FREERTOS_HZ=1000" in config
        if target == "esp32":
            assert "CONFIG_FREERTOS_UNICORE=y" in config and "CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY=y" in config
        flash_path = Path(str(build) + "-flash.log")
        flash = read_text(flash_path)
        assert flash.count("Hash of data verified.") >= 2, f"flash verification missing: {profile}"
        displayed_mac = re.search(r"\bMAC:\s+([0-9a-f:]+)", flash, re.I).group(1).lower()
        octets = displayed_mac.split(":")
        if len(octets) == 8 and octets[3:5] == ["ff", "fe"]:
            octets = octets[:3] + octets[5:]
        assert ":".join(octets) == mac, f"unexpected board identity: {displayed_mac}"
        app_bytes = (build / "blip-v2.bin").stat().st_size
        partition_bytes = app_partition_bytes(build)
        assert app_bytes <= partition_bytes
        prior = next(item for item in baseline["boards"] if item["profile"] == profile)
        assert prior["partition_bytes"] == partition_bytes
        item = {"profile": profile, "board": board, "target": target, "mac": mac,
                "enabled_features": list(enabled) + (["BLE"] if profile == "ball" else []),
                "app_bytes": app_bytes, "app_partition_bytes": partition_bytes,
                "prior_app_bytes": prior["app_bytes"], "app_delta_bytes": app_bytes - prior["app_bytes"],
                "linked_size": read_json(Path(str(build) + "-size.json")),
                "engine_dram_bytes": 16384 if target == "esp32" else 81920,
                "linear_iram_bytes": 65536 if target == "esp32" else 0,
                "single_core": "CONFIG_FREERTOS_UNICORE=y" in config,
                "flash_verified": True, "flash_displayed_mac": displayed_mac, "flash_log_sha256": sha(flash_path),
                "serial_trial_sha256": sha(trial_path), "serial_trial": trial}
        if profile in args.coexistence:
            network_path = Path(str(build) + "-network-hil.json")
            network = read_json(network_path)
            validate_trial(network, build, snapshot)
            assert network["board"] == board and network["mode"] == "coexistence"
            coex = network["coexistence"]
            assert coex["passed"] and coex["settings_writes"] == 20 and coex["settings_restored"]
            assert len(coex["script_calls"]) == 20
            assert all(call["error"] == "budget_exceeded" and call["count"] == 0 for call in coex["script_calls"])
            assert coex["heap_minimum"] >= 8192 and coex["worker_state_after"] == "ready"
            assert coex["worker_stack_headroom_after"] >= 2048 and coex["supervisor_stack_headroom_after"] >= 1024
            guard = network["host_network"]
            assert guard["independent_recovery_armed"] and guard["original_wifi_and_internet_restored"]
            assert guard["temporary_profile_removed"] and guard["recovery_delay_seconds"] == 45
            item.update(coexistence_trial_sha256=sha(network_path), coexistence_trial=network)
        boards.append(item)
    evidence = {"schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": True,
        "result": "six required production profiles passed their declared memory/policy checks; M5StickC large UART burst remains unresolved; full Gate D open",
        "runtime": {"name": "WAMR fast interpreter", "commit": revision, "checkout_unmodified": True},
        "source_snapshot": snapshot, "prior_profile_source_commit": baseline["source_commit"],
        "firmware_restore_requested": False, "boards": boards,
        "limitations": [
            "Native component task stop/restart and allocation-failure qualification remain open.",
            "The M5StickC omits two large UART-burst checks after repeated damaged replies; the same queue-overload policy passes on the other two required ESP32 profiles. See the separate serial-burst evidence.",
            "Traffic trials are brief operational checks, not lossless UDP or a network latency/soak guarantee.",
            "A first combined HUZZAH32 serial/traffic run had three HTTP timeouts; the separate traffic trial must pass and long-run latency remains open.",
            "Deadline cancellation and late-result rejection are enforced; SDK/cache work can delay completion.",
            "Active fleet/BLE traffic and physical LED timing were not qualified here.",
            "App deltas include single-core/IRAM and Wi-Fi profile changes; XIAO also adds network lighting relative to its prior profile.",
            "Peak usage for separate ESP32 arenas sums individual peaks and is a conservative bound.",
            "Provider imports, checked string ABI, script registry declarations, persistent modules and SDK remain open."
        ]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "boards": len(boards), "serial_checks": sum(len(board["serial_trial"]["checks"]) for board in boards),
                      "traffic_trials": len(args.coexistence), "output": str(args.output)}))


if __name__ == "__main__":
    main()
