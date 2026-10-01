#!/usr/bin/env python3
"""Build generic or board-specific factory images and browser manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


TARGETS = {
    "esp32": "ESP32",
    "esp32s3": "ESP32-S3",
    "esp32c6": "ESP32-C6",
}
STORAGE_OFFSET = 0x340000
BOARD_PROFILES = {
    "creators-ball-v2": ("esp32c6", "8MB", 0x620000),
    "seeed-xiao-esp32c6-chip-antenna": ("esp32c6", "4MB", 0x360000),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def load_build(build_dir: Path, expected_target: str, flash_size: str = "4MB",
               storage_offset: int = STORAGE_OFFSET, board_id: str | None = None) -> tuple[dict, str]:
    flasher = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    description = json.loads((build_dir / "project_description.json").read_text(encoding="utf-8"))
    if description.get("target") != expected_target:
        raise ValueError(f"{build_dir} is not an {expected_target} build")
    if description.get("project_name") != "blip-v2":
        raise ValueError(f"{build_dir} is not a BLIP V2 build")
    settings = flasher.get("flash_settings", {})
    if settings.get("flash_size") != flash_size or settings.get("flash_mode") != "dio":
        raise ValueError(f"{build_dir} does not use the expected {flash_size} DIO layout")
    required = list(flasher.get("flash_files", {}).values()) + ["storage.bin"]
    missing = [name for name in required if not (build_dir / name).is_file()]
    if missing:
        raise ValueError(f"{build_dir} is missing factory artifacts: {', '.join(missing)}")
    flash_files = {int(offset, 0): path for offset, path in flasher["flash_files"].items()}
    if flash_files.get(storage_offset) != "storage.bin":
        raise ValueError(f"{build_dir} does not flash storage.bin at {hex(storage_offset)}")
    if board_id is not None:
        cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
        config = (build_dir / "sdkconfig").read_text(encoding="utf-8")
        if "BLIP_ENABLE_BLE:BOOL=ON" not in cache:
            raise ValueError(f"{build_dir} is not a BLE build")
        ball = "BLIP_BOARD_CREATORS_BALL_V2:UNINITIALIZED=ON" in cache or \
               "BLIP_BOARD_CREATORS_BALL_V2:BOOL=ON" in cache
        if ball != (board_id == "creators-ball-v2"):
            raise ValueError(f"{build_dir} does not match board {board_id}")
        expected_partition = ("partitions-8mb.csv" if ball else "partitions-ble-4mb.csv")
        if f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{expected_partition}"' not in config:
            raise ValueError(f"{build_dir} does not use {expected_partition}")
    return flasher, description["project_version"]


def merge_image(build_dir: Path, target: str, flasher: dict, output: Path) -> None:
    settings = flasher["flash_settings"]
    parts = sorted((int(offset, 0), path) for offset, path in flasher["flash_files"].items())
    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        target,
        "merge-bin",
        "-o",
        str(output.resolve()),
        "--flash-mode",
        settings["flash_mode"],
        "--flash-freq",
        settings["flash_freq"],
        "--flash-size",
        settings["flash_size"],
    ]
    for offset, path in parts:
        command.extend((hex(offset), path))
    subprocess.run(command, cwd=build_dir, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for target in TARGETS:
        parser.add_argument(f"--{target}-build", type=Path)
    parser.add_argument("--board-id", choices=BOARD_PROFILES)
    parser.add_argument("--board-build", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--base-url", default="https://example.invalid/blip-v2")
    args = parser.parse_args()
    generic_builds = [getattr(args, f"{target}_build") for target in TARGETS]
    if args.board_id is not None or args.board_build is not None:
        if args.board_id is None or args.board_build is None or any(generic_builds):
            parser.error("--board-id and --board-build must be used together, without target builds")
        target, flash_size, storage_offset = BOARD_PROFILES[args.board_id]
        build_specs = [(target, args.board_build, flash_size, storage_offset, args.board_id)]
    else:
        if any(build is None for build in generic_builds):
            parser.error("all three target builds are required for a generic installer")
        build_specs = [(target, getattr(args, f"{target}_build"), "4MB", STORAGE_OFFSET, None)
                       for target in TARGETS]
    output = args.output.resolve()
    artifacts = output / "artifacts"
    if output.exists() and any(output.iterdir()):
        raise ValueError("--output must be absent or empty")
    artifacts.mkdir(parents=True)

    builds: list[dict] = []
    hashes: dict[str, dict] = {}
    version: str | None = None
    for target, build_path, flash_size, storage_offset, board_id in build_specs:
        build_dir = build_path.resolve()
        flasher, target_version = load_build(build_dir, target, flash_size, storage_offset,
                                            board_id)
        if version is None:
            version = target_version
        elif version != target_version:
            raise ValueError("target builds have different project versions")
        filename = f"blip-v2-{board_id + '-ble' if board_id else target}-factory.bin"
        destination = artifacts / filename
        merge_image(build_dir, target, flasher, destination)
        flash_bytes = int(flash_size[:-2]) * 1024 * 1024
        if destination.stat().st_size > flash_bytes:
            raise ValueError(f"merged {target} image exceeds {flash_size}")
        builds.append(
            {
                "chipFamily": TARGETS[target],
                "improv": False,
                "parts": [{"path": f"artifacts/{filename}", "offset": 0}],
            }
        )
        hashes[target] = {
            "file": f"artifacts/{filename}",
            "bytes": destination.stat().st_size,
            "sha256": sha256(destination),
            "storage_offset": storage_offset,
            "flash_bytes": flash_bytes,
            "source_parts": {
                offset: {"file": path, "sha256": sha256(build_dir / path)}
                for offset, path in flasher["flash_files"].items()
            },
        }

    assert version is not None
    manifest = {
        "name": f"BLIP V2 {args.board_id} BLE" if args.board_id else "BLIP V2",
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": builds,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (output / "hashes.json").write_text(
        json.dumps({"format": 1, "version": version, "targets": hashes}, indent=2, sort_keys=True)
        + "\n",
        encoding="utf-8",
    )
    image_url = args.base_url.rstrip("/") + "/artifacts"
    chipsets = ", ".join(f'"{TARGETS[target]}"' for target, *_ in build_specs)
    image_lines = "\n".join(
        f'image.{TARGETS[target].lower()} = "{build["parts"][0]["path"].split("/")[-1]}"'
        for (target, *_), build in zip(build_specs, builds)
    )
    launchpad = f'''esp_toml_version = 1.0
firmware_images_url = "{image_url}"
supported_apps = ["BLIP_V2"]

[BLIP_V2]
chipsets = [{chipsets}]
{image_lines}
description = "BLIP V2 factory image with A/B OTA and browser control"
console_baudrate = 115200
'''
    (output / "launchpad.toml").write_text(launchpad, encoding="utf-8")
    shutil.copyfile(Path(__file__).with_name("index.html"), output / "index.html")
    print(f"PASS browser installer: version={version} targets={len(build_specs)} output={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
