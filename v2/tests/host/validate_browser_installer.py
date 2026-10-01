#!/usr/bin/env python3
"""Validate browser-installer contracts without requiring ESP toolchains."""

from __future__ import annotations

import importlib.util
import json
import shutil
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BUILDER_PATH = ROOT / "v2" / "installer" / "build_browser_installer.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    spec = importlib.util.spec_from_file_location("blip_browser_installer", BUILDER_PATH)
    require(spec is not None and spec.loader is not None, "installer builder is not importable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    require(
        module.TARGETS
        == {"esp32": "ESP32", "esp32s3": "ESP32-S3", "esp32c6": "ESP32-C6"},
        "browser manifest target matrix drifted",
    )
    require(module.STORAGE_OFFSET == 0x340000, "factory storage offset drifted")

    firmware_cmake = (ROOT / "v2" / "firmware" / "CMakeLists.txt").read_text(encoding="utf-8")
    require(
        "littlefs_create_partition_image(storage" in firmware_cmake
        and "FLASH_IN_PROJECT" in firmware_cmake,
        "factory flash no longer includes initialized LittleFS",
    )
    installer_html = (ROOT / "v2" / "installer" / "index.html").read_text(encoding="utf-8")
    require('manifest="manifest.json"' in installer_html, "installer page does not use its manifest")

    with tempfile.TemporaryDirectory(prefix="blip-installer-") as temporary:
        base = Path(temporary)
        arguments: list[str] = []
        for target in module.TARGETS:
            build = base / target
            build.mkdir()
            flash_files = {
                "0x0" if target != "esp32" else "0x1000": "bootloader.bin",
                "0x8000": "partition-table.bin",
                "0x18000": "ota-data.bin",
                "0x20000": "blip-v2.bin",
                "0x340000": "storage.bin",
            }
            (build / "flasher_args.json").write_text(
                json.dumps(
                    {
                        "flash_settings": {
                            "flash_mode": "dio",
                            "flash_size": "4MB",
                            "flash_freq": "40m" if target == "esp32" else "80m",
                        },
                        "flash_files": flash_files,
                    }
                ),
                encoding="utf-8",
            )
            (build / "project_description.json").write_text(
                json.dumps(
                    {"target": target, "project_name": "blip-v2", "project_version": "0.1.0"}
                ),
                encoding="utf-8",
            )
            for filename in flash_files.values():
                (build / filename).write_bytes(f"{target}:{filename}".encode())
            arguments.extend((f"--{target}-build", str(build)))

        def fake_merge(build_dir: Path, target: str, flasher: dict, output: Path) -> None:
            del build_dir, flasher
            output.write_bytes((target + "-merged").encode())

        module.merge_image = fake_merge
        output = base / "output"
        previous = sys.argv
        sys.argv = [
            str(BUILDER_PATH),
            *arguments,
            "--output",
            str(output),
            "--base-url",
            "https://example.test/v2",
        ]
        try:
            require(module.main() == 0, "installer builder failed")
        finally:
            sys.argv = previous
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        require(manifest["version"] == "0.1.0", "manifest version mismatch")
        require(len(manifest["builds"]) == 3, "manifest does not cover all targets")
        require(
            all(build["parts"][0]["offset"] == 0 for build in manifest["builds"]),
            "merged image offset drifted",
        )
        require(
            manifest["new_install_improv_wait_time"] == 0,
            "manifest falsely waits for Improv",
        )
        launchpad = (output / "launchpad.toml").read_text(encoding="utf-8")
        require(
            "ESP32-C6" in launchpad and "https://example.test/v2/artifacts" in launchpad,
            "Launchpad config mismatch",
        )
        hashes = json.loads((output / "hashes.json").read_text(encoding="utf-8"))
        require(set(hashes["targets"]) == set(module.TARGETS), "provenance target matrix mismatch")

        ball_build = base / "ball"
        shutil.copytree(base / "esp32c6", ball_build)
        ball_flasher = json.loads((ball_build / "flasher_args.json").read_text(encoding="utf-8"))
        ball_flasher["flash_settings"]["flash_size"] = "8MB"
        ball_flasher["flash_files"]["0x620000"] = ball_flasher["flash_files"].pop("0x340000")
        (ball_build / "flasher_args.json").write_text(json.dumps(ball_flasher), encoding="utf-8")
        (ball_build / "CMakeCache.txt").write_text(
            "BLIP_ENABLE_BLE:BOOL=ON\nBLIP_BOARD_CREATORS_BALL_V2:UNINITIALIZED=ON\n",
            encoding="utf-8",
        )
        (ball_build / "sdkconfig").write_text(
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-8mb.csv"\n',
            encoding="utf-8",
        )
        ball_output = base / "ball-output"
        sys.argv = [str(BUILDER_PATH), "--board-id", "creators-ball-v2",
                    "--board-build", str(ball_build), "--output", str(ball_output)]
        try:
            require(module.main() == 0, "Ball BLE installer builder failed")
        finally:
            sys.argv = previous
        ball_manifest = json.loads((ball_output / "manifest.json").read_text(encoding="utf-8"))
        ball_hashes = json.loads((ball_output / "hashes.json").read_text(encoding="utf-8"))
        require(len(ball_manifest["builds"]) == 1, "Ball manifest must contain one board build")
        require(ball_manifest["builds"][0]["chipFamily"] == "ESP32-C6", "Ball target drifted")
        require(ball_hashes["targets"]["esp32c6"]["storage_offset"] == 0x620000,
                "Ball storage offset drifted")
        require(ball_hashes["targets"]["esp32c6"]["flash_bytes"] == 8 * 1024 * 1024,
                "Ball flash size drifted")

        xiao_build = base / "xiao"
        shutil.copytree(base / "esp32c6", xiao_build)
        xiao_flasher = json.loads((xiao_build / "flasher_args.json").read_text(encoding="utf-8"))
        xiao_flasher["flash_files"]["0x360000"] = xiao_flasher["flash_files"].pop("0x340000")
        (xiao_build / "flasher_args.json").write_text(json.dumps(xiao_flasher), encoding="utf-8")
        (xiao_build / "CMakeCache.txt").write_text("BLIP_ENABLE_BLE:BOOL=ON\n",
                                                  encoding="utf-8")
        (xiao_build / "sdkconfig").write_text(
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-ble-4mb.csv"\n',
            encoding="utf-8",
        )
        xiao_output = base / "xiao-output"
        sys.argv = [str(BUILDER_PATH), "--board-id", "seeed-xiao-esp32c6-chip-antenna",
                    "--board-build", str(xiao_build), "--output", str(xiao_output)]
        try:
            require(module.main() == 0, "XIAO BLE installer builder failed")
        finally:
            sys.argv = previous
        xiao_hashes = json.loads((xiao_output / "hashes.json").read_text(encoding="utf-8"))
        require(xiao_hashes["targets"]["esp32c6"]["storage_offset"] == 0x360000,
                "XIAO storage offset drifted")
        try:
            module.load_build(ball_build, "esp32c6", "4MB", 0x360000,
                              "seeed-xiao-esp32c6-chip-antenna")
        except ValueError:
            pass
        else:
            raise AssertionError("Ball build was accepted as XIAO BLE")

    print("PASS browser installer manifests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
