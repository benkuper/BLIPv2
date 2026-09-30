#!/usr/bin/env python3
"""Validate browser-installer contracts without requiring ESP toolchains."""

from __future__ import annotations

import importlib.util
import json
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

    print("PASS browser installer manifests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
