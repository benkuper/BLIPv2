#!/usr/bin/env python3
"""Validate deterministic, build-time resource allocation manifests."""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
FIXTURES = ROOT / "v2" / "tests" / "fixtures" / "v2" / "resources"
ID_PATTERN = re.compile(r"^[a-z][a-z0-9._-]+$")
PUBLIC_ID_PATTERN = re.compile(r"^[a-z][a-z0-9_-]*$")
RESOURCE_CLASSES = {
    "gpio", "rmt", "spi", "i2c", "uart", "timer", "dma", "internal_memory", "psram", "radio"
}
OWNERSHIP_MODES = {"exclusive", "shared-read", "bus-member", "multiplexed"}
PIN_CAPABILITIES = {"input", "output", "adc", "pwm", "interrupt", "open-drain", "rmt"}


class ManifestError(ValueError):
    pass


@dataclass
class Claim:
    feature: str
    ownership: str
    amount: int
    member_key: int | None
    incompatible_features: set[str] = field(default_factory=set)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)


def require_keys(value: dict[str, Any], required: set[str], allowed: set[str], context: str) -> None:
    missing = sorted(required - value.keys())
    extra = sorted(value.keys() - allowed)
    require(not missing, f"{context}: missing keys {missing}")
    require(not extra, f"{context}: unknown keys {extra}")


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"{path}: invalid JSON: {error}") from error
    require(isinstance(document, dict), f"{path}: root must be an object")
    return document


def validate_resources(document: dict[str, Any], source: str) -> tuple[dict, dict]:
    resources: dict[tuple[str, str], dict[str, Any]] = {}
    claims: dict[tuple[str, str], list[Claim]] = {}
    for index, resource in enumerate(document["resources"]):
        context = f"{source}: resource[{index}]"
        require(isinstance(resource, dict), f"{context}: must be an object")
        require_keys(resource, {"class", "id", "capacity", "capabilities"},
                     {"class", "id", "capacity", "capabilities", "reserved_for"}, context)
        require(resource["class"] in RESOURCE_CLASSES, f"{context}: unknown class")
        require(isinstance(resource["id"], str) and ID_PATTERN.fullmatch(resource["id"]) is not None,
                f"{context}: invalid id")
        require(isinstance(resource["capacity"], int) and resource["capacity"] > 0,
                f"{context}: capacity must be positive")
        require(isinstance(resource["capabilities"], list) and
                all(isinstance(item, str) for item in resource["capabilities"]),
                f"{context}: capabilities must be strings")
        require(len(set(resource["capabilities"])) == len(resource["capabilities"]),
                f"{context}: duplicate capability")
        key = (resource["class"], resource["id"])
        require(key not in resources, f"{context}: duplicate resource {resource['id']}")
        resources[key] = resource
        claims[key] = []
    return resources, claims


def allocate_request(feature_id: str, request: dict[str, Any], resources: dict, claims: dict,
                     context: str) -> None:
    required = {"class", "logical_name", "ownership", "alternatives", "amount", "capabilities"}
    allowed = required | {"member_key", "incompatible_features", "live_reacquire"}
    require_keys(request, required, allowed, context)
    require(request["class"] in RESOURCE_CLASSES, f"{context}: unknown class")
    require(isinstance(request["logical_name"], str) and
            PUBLIC_ID_PATTERN.fullmatch(request["logical_name"]) is not None,
            f"{context}: invalid logical_name")
    require(request["ownership"] in OWNERSHIP_MODES, f"{context}: unknown ownership")
    require(isinstance(request["alternatives"], list) and request["alternatives"],
            f"{context}: alternatives must be non-empty")
    require(isinstance(request["amount"], int) and request["amount"] > 0,
            f"{context}: amount must be positive")
    require(isinstance(request["capabilities"], list), f"{context}: capabilities must be an array")
    required_capabilities = set(request["capabilities"])
    incompatible = set(request.get("incompatible_features", []))

    candidates: list[tuple[str, str]] = []
    blockers: set[str] = set()
    for alternative in sorted(request["alternatives"]):
        key = (request["class"], alternative)
        resource = resources.get(key)
        if resource is None or not required_capabilities.issubset(resource["capabilities"]):
            continue
        reserved_for = resource.get("reserved_for")
        if reserved_for is not None and reserved_for != feature_id:
            blockers.add(reserved_for)
            continue
        active = claims[key]
        compatible = sum(claim.amount for claim in active) + request["amount"] <= resource["capacity"]
        for claim in active:
            mode_compatible = (
                request["ownership"] == claim.ownership == "shared-read"
                or request["ownership"] == claim.ownership == "multiplexed"
                or (request["ownership"] == claim.ownership == "bus-member"
                    and request.get("member_key") != claim.member_key)
            )
            feature_conflict = claim.feature in incompatible or feature_id in claim.incompatible_features
            if not mode_compatible or feature_conflict:
                compatible = False
                blockers.add(claim.feature)
        if compatible:
            candidates.append(key)

    require(candidates,
            f"{context}: cannot acquire {sorted(request['alternatives'])}; "
            f"blocked by {sorted(blockers) if blockers else 'availability/capability'}")
    selected = min(candidates, key=lambda item: item[1])
    claims[selected].append(Claim(feature_id, request["ownership"], request["amount"],
                                  request.get("member_key"), incompatible))


def validate_and_allocate(document: dict[str, Any], source: str) -> None:
    require_keys(document, {"schema_version", "resources", "features"},
                 {"schema_version", "resources", "features"}, source)
    require(document["schema_version"] == 1, f"{source}: unsupported schema_version")
    require(isinstance(document["resources"], list), f"{source}: resources must be an array")
    require(isinstance(document["features"], list), f"{source}: features must be an array")
    resources, claims = validate_resources(document, source)

    features = document["features"]
    feature_ids = [feature.get("id") for feature in features if isinstance(feature, dict)]
    require(len(feature_ids) == len(features) and len(set(feature_ids)) == len(feature_ids),
            f"{source}: feature IDs must be unique")
    for feature in sorted(features, key=lambda item: item["id"]):
        feature_id = feature["id"]
        context = f"{source}: feature '{feature_id}'"
        require_keys(feature, {"id", "requests"}, {"id", "requests"}, context)
        require(isinstance(feature_id, str) and ID_PATTERN.fullmatch(feature_id) is not None,
                f"{context}: invalid id")
        require(isinstance(feature["requests"], list), f"{context}: requests must be an array")
        logical_names: set[str] = set()
        for request in sorted(feature["requests"], key=lambda item: item.get("logical_name", "")):
            logical_name = request.get("logical_name", "?")
            require(logical_name not in logical_names, f"{context}: duplicate request '{logical_name}'")
            logical_names.add(logical_name)
            allocate_request(feature_id, request, resources, claims,
                             f"{context} request '{logical_name}'")


def check_fixture(name: str, should_pass: bool, expected_text: str = "") -> None:
    path = FIXTURES / name
    try:
        validate_and_allocate(load_manifest(path), path.as_posix())
    except ManifestError as error:
        require(not should_pass, f"{name}: unexpected conflict: {error}")
        require(expected_text in str(error), f"{name}: missing diagnostic text {expected_text!r}")
        print(f"PASS expected conflict: {name}: {error}")
        return
    require(should_pass, f"{name}: expected a conflict")
    print(f"PASS resource manifest: {name}")


def validate_board(path: Path) -> None:
    document = load_manifest(path)
    require_keys(document, {"schema_version", "id", "target", "sources", "pins"},
                 {"schema_version", "id", "target", "sources", "pins", "antenna", "buses"}, path.as_posix())
    require(document["schema_version"] == 1, f"{path}: unsupported board schema")
    require(document["target"] in {"esp32", "esp32s3", "esp32c6"}, f"{path}: invalid target")
    require(isinstance(document["sources"], list) and document["sources"] and
            all(isinstance(source, str) and source.startswith("https://")
                for source in document["sources"]), f"{path}: authoritative sources required")
    require(isinstance(document["pins"], list) and document["pins"], f"{path}: pins required")
    ids: set[str] = set()
    gpios: set[int] = set()
    by_gpio: dict[int, dict[str, Any]] = {}
    for index, pin in enumerate(document["pins"]):
        context = f"{path}: pin[{index}]"
        require(isinstance(pin, dict), f"{context}: must be an object")
        require_keys(pin, {"id", "label", "gpio", "capabilities", "electrical"},
                     {"id", "label", "gpio", "capabilities", "electrical", "reserved_for",
                      "reason", "bus", "selectable"}, context)
        require(isinstance(pin["id"], str) and ID_PATTERN.fullmatch(pin["id"]),
                f"{context}: invalid id")
        require(pin["id"] not in ids, f"{context}: duplicate id")
        require(isinstance(pin["gpio"], int) and pin["gpio"] >= 0, f"{context}: invalid gpio")
        require(pin["gpio"] not in gpios, f"{context}: duplicate gpio")
        require(isinstance(pin["capabilities"], list) and
                set(pin["capabilities"]).issubset(PIN_CAPABILITIES),
                f"{context}: invalid capability")
        if pin.get("selectable") is False:
            require(bool(pin.get("reserved_for")) and bool(pin.get("reason")),
                    f"{context}: unavailable pin needs owner and reason")
        ids.add(pin["id"])
        gpios.add(pin["gpio"])
        by_gpio[pin["gpio"]] = pin
    if document["target"] == "esp32c6":
        require(document.get("antenna") == "onboard", f"{path}: reference C6 must use onboard antenna")
        require(by_gpio[3].get("reason") == "onboard-antenna-rf-switch-power",
                f"{path}: GPIO3 must reserve RF switch power")
        require(by_gpio[14].get("reason") == "onboard-antenna-selected",
                f"{path}: GPIO14 must reserve onboard antenna selection")
    print(f"PASS board manifest: {path.name} ({len(ids)} pins)")


def main() -> int:
    if len(sys.argv) > 1:
        for argument in sys.argv[1:]:
            path = Path(argument)
            validate_and_allocate(load_manifest(path), path.as_posix())
            print(f"PASS resource manifest: {path}")
        return 0

    minimal = ROOT / "v2" / "profiles" / "features" / "minimal.json"
    validate_and_allocate(load_manifest(minimal), minimal.as_posix())
    print("PASS resource manifest: minimal profile")
    check_fixture("valid.json", True)
    check_fixture("conflict-exclusive.json", False, "feature.button")
    check_fixture("conflict-reserved.json", False, "system.flash")
    for board in sorted((ROOT / "v2" / "boards").glob("*.json")):
        validate_board(board)
    print("Resource and board manifest validation passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ManifestError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1) from error
