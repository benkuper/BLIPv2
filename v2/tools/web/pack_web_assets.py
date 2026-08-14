#!/usr/bin/env python3
"""Build, verify, and optionally upload a deterministic BLIP web bundle."""

from __future__ import annotations

import argparse
import binascii
import gzip
import json
import struct
import sys
import urllib.request
import zlib
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"BLWB"
FORMAT_VERSION = 1
HEADER_SIZE = 48
ENTRY_SIZE = 96
MAX_PATH_BYTES = 63
MAX_ASSETS = 24
MAX_BUNDLE_BYTES = 256 * 1024
HEADER = struct.Struct("<4sHHIHHIIIIII8s")
ENTRY = struct.Struct("<BBBBIIIII64s8s")

CONTENT_TYPES = {
    ".html": 1,
    ".css": 2,
    ".js": 3,
    ".json": 4,
    ".txt": 5,
    ".png": 6,
    ".ico": 7,
}
COMPRESSIBLE = {1, 2, 3, 4, 5}


@dataclass(frozen=True)
class Asset:
    path: str
    content_type: int
    encoding: int
    cache_policy: int
    original: bytes
    stored: bytes


def deterministic_gzip(value: bytes) -> bytes:
    compressor = zlib.compressobj(level=9, method=zlib.DEFLATED, wbits=-15)
    payload = compressor.compress(value) + compressor.flush()
    header = bytes.fromhex("1f8b08000000000002ff")
    trailer = struct.pack("<II", binascii.crc32(value) & 0xFFFFFFFF, len(value) & 0xFFFFFFFF)
    return header + payload + trailer


def source_files(root: Path) -> list[Path]:
    candidates = [root / "index.html", root / "styles.css"]
    candidates.extend(sorted((root / "src").rglob("*")))
    files = [path for path in candidates if path.is_file() and path.suffix in CONTENT_TYPES]
    if not files or root / "index.html" not in files:
        raise ValueError("web source must contain index.html")
    if len(files) > MAX_ASSETS:
        raise ValueError(f"web source exceeds {MAX_ASSETS} assets")
    return files


def load_assets(root: Path) -> list[Asset]:
    assets: list[Asset] = []
    for source in source_files(root):
        route = "/" + source.relative_to(root).as_posix()
        encoded_route = route.encode("ascii")
        if len(encoded_route) > MAX_PATH_BYTES or ".." in route or "//" in route:
            raise ValueError(f"invalid asset route: {route}")
        content_type = CONTENT_TYPES[source.suffix]
        original = source.read_bytes()
        encoding = 1 if content_type in COMPRESSIBLE else 0
        stored = deterministic_gzip(original) if encoding == 1 else original
        cache_policy = 0 if route == "/index.html" else 1
        assets.append(Asset(route, content_type, encoding, cache_policy, original, stored))
    return sorted(assets, key=lambda asset: asset.path)


def package_version(root: Path) -> int:
    package = json.loads((root / "package.json").read_text(encoding="utf-8"))
    parts = package["version"].split(".")
    if len(parts) != 3 or any(not part.isdigit() for part in parts):
        raise ValueError("package version must be numeric major.minor.patch")
    major, minor, patch = (int(part) for part in parts)
    if major > 4_294 or minor > 999 or patch > 999:
        raise ValueError("package version exceeds bundle encoding")
    value = major * 1_000_000 + minor * 1_000 + patch
    if value == 0:
        raise ValueError("bundle version must be nonzero")
    return value


def build_bundle(root: Path, version: int | None = None) -> bytes:
    assets = load_assets(root)
    manifest = bytearray()
    payload = bytearray()
    for asset in assets:
        path = asset.path.encode("ascii")
        manifest.extend(
            ENTRY.pack(
                len(path),
                asset.content_type,
                asset.encoding,
                asset.cache_policy,
                len(payload),
                len(asset.stored),
                len(asset.original),
                binascii.crc32(asset.stored) & 0xFFFFFFFF,
                0,
                path.ljust(64, b"\0"),
                b"\0" * 8,
            )
        )
        payload.extend(asset.stored)
    total_size = HEADER_SIZE + len(manifest) + len(payload)
    if total_size > MAX_BUNDLE_BYTES:
        raise ValueError(f"bundle exceeds {MAX_BUNDLE_BYTES} bytes")
    bundle_version = package_version(root) if version is None else version
    if not 0 < bundle_version <= 0xFFFFFFFF:
        raise ValueError("bundle version must fit uint32")
    header = HEADER.pack(
        MAGIC,
        FORMAT_VERSION,
        HEADER_SIZE,
        bundle_version,
        len(assets),
        0,
        len(manifest),
        len(payload),
        total_size,
        0,
        binascii.crc32(manifest) & 0xFFFFFFFF,
        binascii.crc32(payload) & 0xFFFFFFFF,
        b"\0" * 8,
    )
    bundle = bytearray(header + manifest + payload)
    bundle_crc = binascii.crc32(bundle) & 0xFFFFFFFF
    struct.pack_into("<I", bundle, 28, bundle_crc)
    verify_bundle(bytes(bundle), assets)
    return bytes(bundle)


def verify_bundle(bundle: bytes, expected_assets: list[Asset] | None = None) -> None:
    if len(bundle) < HEADER_SIZE or len(bundle) > MAX_BUNDLE_BYTES:
        raise ValueError("invalid bundle size")
    fields = HEADER.unpack_from(bundle)
    (
        magic,
        format_version,
        header_size,
        bundle_version,
        entry_count,
        flags,
        manifest_size,
        payload_size,
        total_size,
        bundle_crc,
        manifest_crc,
        payload_crc,
        reserved,
    ) = fields
    if (
        magic != MAGIC
        or format_version != FORMAT_VERSION
        or header_size != HEADER_SIZE
        or bundle_version == 0
        or not 0 < entry_count <= MAX_ASSETS
        or flags != 0
        or manifest_size != entry_count * ENTRY_SIZE
        or total_size != len(bundle)
        or total_size != HEADER_SIZE + manifest_size + payload_size
        or reserved != b"\0" * 8
    ):
        raise ValueError("invalid bundle header")
    zeroed = bytearray(bundle)
    zeroed[28:32] = b"\0" * 4
    if binascii.crc32(zeroed) & 0xFFFFFFFF != bundle_crc:
        raise ValueError("invalid bundle CRC")
    manifest = bundle[HEADER_SIZE : HEADER_SIZE + manifest_size]
    payload = bundle[HEADER_SIZE + manifest_size :]
    if binascii.crc32(manifest) & 0xFFFFFFFF != manifest_crc:
        raise ValueError("invalid manifest CRC")
    if binascii.crc32(payload) & 0xFFFFFFFF != payload_crc:
        raise ValueError("invalid payload CRC")
    prior = ""
    expected_offset = 0
    decoded: list[tuple[str, bytes]] = []
    for index in range(entry_count):
        entry = ENTRY.unpack_from(manifest, index * ENTRY_SIZE)
        path_size, content_type, encoding, cache_policy, offset, stored_size, original_size, crc = entry[:8]
        entry_reserved, path_field, tail_reserved = entry[8:]
        path = path_field[:path_size].decode("ascii")
        stored = payload[offset : offset + stored_size]
        if (
            not path.startswith("/")
            or path == "/"
            or path <= prior
            or offset != expected_offset
            or stored_size == 0
            or len(stored) != stored_size
            or content_type not in CONTENT_TYPES.values()
            or encoding not in (0, 1)
            or cache_policy not in (0, 1, 2)
            or entry_reserved != 0
            or path_field[path_size:] != b"\0" * (64 - path_size)
            or tail_reserved != b"\0" * 8
            or binascii.crc32(stored) & 0xFFFFFFFF != crc
        ):
            raise ValueError(f"invalid bundle entry {index}")
        original = gzip.decompress(stored) if encoding == 1 else stored
        if len(original) != original_size:
            raise ValueError(f"invalid original size for {path}")
        decoded.append((path, original))
        prior = path
        expected_offset += stored_size
    if expected_offset != payload_size or "/index.html" not in {path for path, _ in decoded}:
        raise ValueError("invalid payload layout")
    if expected_assets is not None:
        expected = [(asset.path, asset.original) for asset in expected_assets]
        if decoded != expected:
            raise ValueError("bundle does not reproduce its source assets")


def upload_bundle(device: str, bundle: bytes) -> None:
    endpoint = device.rstrip("/") + "/api/web-assets"
    request = urllib.request.Request(
        endpoint,
        data=bundle,
        method="PUT",
        headers={"Content-Type": "application/vnd.blip.web-bundle"},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        if response.status not in (200, 201, 204):
            raise RuntimeError(f"device rejected bundle with HTTP {response.status}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument("--output", type=Path)
    destination.add_argument("--check", type=Path)
    parser.add_argument("--bundle-version", type=int)
    parser.add_argument("--upload")
    args = parser.parse_args()

    root = args.source.resolve()
    bundle = build_bundle(root, args.bundle_version)
    if args.check is not None:
        committed = args.check.read_bytes()
        verify_bundle(committed)
        if committed != bundle:
            print(f"bundle drift: regenerate {args.check}", file=sys.stderr)
            return 1
        print(f"PASS web bundle: {len(bundle)} bytes")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(bundle)
        print(f"Wrote {args.output}: {len(bundle)} bytes")
    if args.upload:
        upload_bundle(args.upload, bundle)
        print(f"Uploaded web bundle to {args.upload}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
