#!/usr/bin/env python3
"""Validate artifacts and compiler flags from one ESP-IDF bootstrap build."""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path


TARGETS = ("esp32", "esp32s3", "esp32c6")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def owned_commands(entries: list[dict[str, object]]) -> list[tuple[str, str]]:
    commands = []
    for entry in entries:
        source = str(entry.get("file", "")).replace("\\", "/")
        if "/v2/components/" in source or source.endswith("/v2/firmware/main/main.cpp"):
            if "command" in entry:
                commands.append((source, str(entry["command"])))
            else:
                arguments = entry.get("arguments")
                if isinstance(arguments, list):
                    commands.append((source, shlex.join(str(argument) for argument in arguments)))
    return commands


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=TARGETS)
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    require(build_dir.is_dir(), f"build directory does not exist: {build_dir}")

    for name in ("blip-v2.bin", "blip-v2.elf", "compile_commands.json", "sdkconfig"):
        require((build_dir / name).is_file(), f"missing build artifact: {name}")

    sdkconfig = (build_dir / "sdkconfig").read_text(encoding="utf-8")
    require(f'CONFIG_IDF_TARGET="{args.target}"' in sdkconfig, "configured target mismatch")
    require("CONFIG_IDF_INIT_VERSION=\"6.0.2\"" in sdkconfig, "configured ESP-IDF version mismatch")
    require("CONFIG_APP_REPRODUCIBLE_BUILD=y" in sdkconfig, "reproducible build is disabled")
    require("# CONFIG_COMPILER_CXX_EXCEPTIONS is not set" in sdkconfig, "exceptions are enabled")
    require("# CONFIG_COMPILER_CXX_RTTI is not set" in sdkconfig, "RTTI is enabled")
    require("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in sdkconfig,
            "OTA rollback is disabled")
    require("CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y" in sdkconfig,
            "4 MiB flash layout is not selected")

    image_size = (build_dir / "blip-v2.bin").stat().st_size
    require(image_size <= 0x190000, "application image exceeds an OTA slot")

    entries = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    commands = owned_commands(entries)
    require(commands, "no BLIP-owned sources are present in compile_commands.json")
    require(any(source.endswith("/v2/firmware/main/main.cpp") for source, _ in commands),
            "main.cpp is absent from compile_commands.json")
    for source, command in commands:
        for option in ("-std=gnu++20", "-fno-exceptions", "-fno-rtti", "-Werror"):
            require(option in command, f"{source} compile command lacks {option}")

    print(f"PASS firmware build contract: {args.target}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, json.JSONDecodeError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1) from error
