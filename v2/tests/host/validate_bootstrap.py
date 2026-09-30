#!/usr/bin/env python3
"""Validate the static work-package 1.1 build contracts without ESP-IDF."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
FIRMWARE = ROOT / "v2" / "firmware"
TARGETS = ("esp32", "esp32s3", "esp32c6")
IDF_VERSION = "6.0.2"
IDF_COMMIT = "7101770dc6db2667b3c477cc31365dd1acd6db4e"
IDF_IMAGE_DIGEST = "sha256:0d8c9773d48a327233f9c1d7c654ff0bcf133ae24503ea2e97a57cfe02b8cb67"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def validate_files() -> None:
    required = (
        ".clang-format",
        ".editorconfig",
        ".github/workflows/v2-ci.yml",
        "v2/cmake/blip_component.cmake",
        "v2/firmware/CMakeLists.txt",
        "v2/firmware/dependencies.lock.esp32",
        "v2/firmware/dependencies.lock.esp32s3",
        "v2/firmware/dependencies.lock.esp32c6",
        "v2/firmware/main/CMakeLists.txt",
        "v2/firmware/main/idf_component.yml",
        "v2/firmware/main/main.cpp",
        "v2/firmware/partitions.csv",
        "v2/firmware/sdkconfig.defaults",
        "v2/firmware/toolchain.lock.json",
        "v2/firmware/version.txt",
    )
    missing = [path for path in required if not (ROOT / path).is_file()]
    require(not missing, f"missing bootstrap files: {missing}")


def validate_toolchain_lock() -> None:
    lock = json.loads((FIRMWARE / "toolchain.lock.json").read_text(encoding="utf-8"))
    require(lock["schema_version"] == 1, "unexpected toolchain lock schema")
    require(lock["esp_idf"]["version"] == IDF_VERSION, "ESP-IDF version is not pinned")
    require(lock["esp_idf"]["git_commit"] == IDF_COMMIT, "ESP-IDF commit is not pinned")
    require(
        lock["esp_idf"]["container_digest"] == IDF_IMAGE_DIGEST,
        "ESP-IDF container digest is not pinned",
    )
    require(
        set(lock["component_locks"]) == set(TARGETS),
        "component locks do not cover every initial target",
    )
    for target, relative_path in lock["component_locks"].items():
        component_lock = (FIRMWARE / relative_path).read_text(encoding="utf-8")
        require(f"target: {target}" in component_lock, f"component lock target mismatch: {target}")
        require(f"version: {IDF_VERSION}" in component_lock, f"component lock IDF mismatch: {target}")


def validate_profiles() -> None:
    for target in TARGETS:
        profile_path = ROOT / "v2" / "profiles" / "targets" / f"{target}-generic.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        require(profile["idf_target"] == target, f"profile target mismatch: {target}")
        contract = profile["build_contract"]
        require(contract["esp_idf"] == "6.0.x", f"profile IDF family mismatch: {target}")
        require(contract["cpp_standard"] == "c++20", f"profile C++ mismatch: {target}")
        require(not contract["exceptions"], f"exceptions enabled in profile: {target}")
        require(not contract["rtti"], f"RTTI enabled in profile: {target}")
        require(contract["warnings_as_errors"], f"warnings not fatal in profile: {target}")


def validate_build_contract() -> None:
    root_cmake = read("v2/firmware/CMakeLists.txt")
    require(
        root_cmake.index("set(SDKCONFIG") < root_cmake.index("include($ENV{IDF_PATH}"),
        "SDKCONFIG must be selected before ESP-IDF project.cmake is included",
    )
    require("MINIMAL_BUILD ON" in root_cmake, "ESP-IDF minimal build is not enabled")
    require("dependencies.lock.${IDF_TARGET}" in root_cmake, "locks are not target-specific")

    component_cmake = read("v2/cmake/blip_component.cmake")
    for option in ("-std=gnu++20", "-fno-exceptions", "-fno-rtti", "-Werror"):
        require(option in component_cmake, f"missing owned-source option: {option}")

    sdkconfig = read("v2/firmware/sdkconfig.defaults")
    for option in (
        "CONFIG_APP_REPRODUCIBLE_BUILD=y",
        "CONFIG_COMPILER_CXX_EXCEPTIONS=n",
        "CONFIG_COMPILER_CXX_RTTI=n",
        "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y",
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
    ):
        require(option in sdkconfig, f"missing sdkconfig contract: {option}")

    partitions = []
    for line in read("v2/firmware/partitions.csv").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        require(len(fields) >= 5, f"malformed partition row: {line}")
        partitions.append((fields[0], fields[1], fields[2], int(fields[3], 0), int(fields[4], 0)))
    by_name = {partition[0]: partition for partition in partitions}
    require("factory" not in by_name, "A/B layout must not retain a factory app")
    require(by_name["nvs"][3:] == (0x9000, 0xF000),
            "NVS capacity must fit current V2 component settings")
    require(by_name["otadata"][3] == 0x18000 and by_name["phy_init"][3] == 0x1A000,
            "OTA/PHY offsets must leave the expanded NVS region intact")
    require(by_name["otadata"][4] == 0x2000, "OTA ledger must contain two sectors")
    require(by_name["ota_0"][4] == by_name["ota_1"][4] == 0x190000,
            "OTA slots must be equal 1.5625 MiB partitions")
    ordered = sorted(partitions, key=lambda partition: partition[3])
    for previous, current in zip(ordered, ordered[1:]):
        require(previous[3] + previous[4] <= current[3],
                f"overlapping partitions: {previous[0]} and {current[0]}")
    require(max(offset + size for _, _, _, offset, size in partitions) <= 0x400000,
            "partition layout exceeds 4 MiB")

    manifest = read("v2/firmware/main/idf_component.yml")
    require(f'version: "=={IDF_VERSION}"' in manifest, "component manifest does not pin ESP-IDF")
    for target in TARGETS:
        require(re.search(rf"^\s+- {target}$", manifest, re.MULTILINE) is not None, f"missing target: {target}")

    source = read("v2/firmware/main/main.cpp")
    require('extern "C" void app_main()' in source, "firmware entry point is missing")
    require("BLIP_V2_BOOTSTRAP_READY" in source, "boot readiness marker is missing")
    require("ESP_IDF_VERSION_VAL(6, 0, 2)" in source, "compile-time IDF guard is missing")


def validate_ci() -> None:
    workflow = read(".github/workflows/v2-ci.yml")
    require(IDF_IMAGE_DIGEST in workflow, "CI image does not use the locked digest")
    require("target: [esp32, esp32s3, esp32c6]" in workflow, "CI target matrix is incomplete")
    require("validate_firmware_build.py" in workflow, "CI does not validate build flags")
    require("Check reproducible application image" in workflow, "CI lacks reproducibility check")
    require("ctest --test-dir build/host" in workflow, "CI does not run C++ host tests")


def main() -> int:
    checks = (
        ("bootstrap files", validate_files),
        ("toolchain lock", validate_toolchain_lock),
        ("target profiles", validate_profiles),
        ("build contract", validate_build_contract),
        ("CI matrix", validate_ci),
    )
    for label, check in checks:
        check()
        print(f"PASS {label}")
    print(f"Bootstrap validation passed ({len(checks)} checks)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, json.JSONDecodeError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1) from error
