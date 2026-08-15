#!/usr/bin/env python3
"""Build merged factory images plus ESP Web Tools and ESP Launchpad manifests."""

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


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def load_build(build_dir: Path, expected_target: str) -> tuple[dict, str]:
    flasher = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    description = json.loads((build_dir / "project_description.json").read_text(encoding="utf-8"))
    if description.get("target") != expected_target:
        raise ValueError(f"{build_dir} is not an {expected_target} build")
    if description.get("project_name") != "blip-v2":
        raise ValueError(f"{build_dir} is not a BLIP V2 build")
    settings = flasher.get("flash_settings", {})
    if settings.get("flash_size") != "4MB" or settings.get("flash_mode") != "dio":
        raise ValueError(f"{build_dir} does not use the qualified 4MB DIO layout")
    required = list(flasher.get("flash_files", {}).values()) + ["storage.bin"]
    missing = [name for name in required if not (build_dir / name).is_file()]
    if missing:
        raise ValueError(f"{build_dir} is missing factory artifacts: {', '.join(missing)}")
    flash_files = {int(offset, 0): path for offset, path in flasher["flash_files"].items()}
    if flash_files.get(STORAGE_OFFSET) != "storage.bin":
        raise ValueError(f"{build_dir} does not flash storage.bin at {hex(STORAGE_OFFSET)}")
    return flasher, description["project_version"]


def merge_image(build_dir: Path, target: str, flasher: dict, output: Path) -> None:
    settings = flasher["flash_settings"]
    parts = sorted((int(offset, 0), path) for offset, path in flasher["flash_files"].items())
    if STORAGE_OFFSET not in {offset for offset, _ in parts}:
        parts.append((STORAGE_OFFSET, "storage.bin"))
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
        parser.add_argument(f"--{target}-build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--base-url", default="https://example.invalid/blip-v2")
    args = parser.parse_args()
    output = args.output.resolve()
    artifacts = output / "artifacts"
    if output.exists() and any(output.iterdir()):
        raise ValueError("--output must be absent or empty")
    artifacts.mkdir(parents=True)

    builds: list[dict] = []
    hashes: dict[str, dict] = {}
    version: str | None = None
    for target, family in TARGETS.items():
        build_dir = getattr(args, f"{target}_build").resolve()
        flasher, target_version = load_build(build_dir, target)
        if version is None:
            version = target_version
        elif version != target_version:
            raise ValueError("target builds have different project versions")
        filename = f"blip-v2-{target}-factory.bin"
        destination = artifacts / filename
        merge_image(build_dir, target, flasher, destination)
        if destination.stat().st_size > 4 * 1024 * 1024:
            raise ValueError(f"merged {target} image exceeds 4MB")
        builds.append(
            {
                "chipFamily": family,
                "improv": False,
                "parts": [{"path": f"artifacts/{filename}", "offset": 0}],
            }
        )
        hashes[target] = {
            "file": f"artifacts/{filename}",
            "bytes": destination.stat().st_size,
            "sha256": sha256(destination),
            "storage_offset": STORAGE_OFFSET,
            "source_parts": {
                offset: {"file": path, "sha256": sha256(build_dir / path)}
                for offset, path in flasher["flash_files"].items()
            },
        }

    assert version is not None
    manifest = {
        "name": "BLIP V2",
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
    launchpad = f'''esp_toml_version = 1.0
firmware_images_url = "{image_url}"
supported_apps = ["BLIP_V2"]

[BLIP_V2]
chipsets = ["ESP32", "ESP32-S3", "ESP32-C6"]
image.esp32 = "blip-v2-esp32-factory.bin"
image.esp32-s3 = "blip-v2-esp32s3-factory.bin"
image.esp32-c6 = "blip-v2-esp32c6-factory.bin"
description = "BLIP V2 factory image with A/B OTA and browser control"
console_baudrate = 115200
'''
    (output / "launchpad.toml").write_text(launchpad, encoding="utf-8")
    shutil.copyfile(Path(__file__).with_name("index.html"), output / "index.html")
    print(f"PASS browser installer: version={version} targets={len(TARGETS)} output={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
