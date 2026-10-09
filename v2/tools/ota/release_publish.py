"""Package firmware/web artifacts and atomically update a local release index."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import sys
from urllib.parse import urlsplit
from release_server import ReleaseIndex, validate_release, release_url, ascii_text, load_index
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "web"))
from pack_web_assets import verify_bundle


def fixed_text(data, offset, size):
    field = data[offset:offset + size]
    if len(field) != size or b"\0" not in field:
        raise ValueError("invalid fixed image text")
    end = field.index(0)
    if any(field[end:]):
        raise ValueError("noncanonical image text padding")
    value = field[:end].decode("ascii")
    return ascii_text(value, size - 1)


def firmware_metadata(path):
    with path.open("rb") as stream:
        prefix = stream.read(492)
    size = path.stat().st_size
    if (len(prefix) != 492 or size > 8 * 1024 * 1024 or prefix[0] != 0xe9 or
            struct.unpack_from("<I", prefix, 32)[0] != 0xabcd5432 or prefix[288:296] != b"BLIPREL1"):
        raise ValueError("firmware lacks a BLIP release descriptor")
    schema, code, flash_bytes, features, api = struct.unpack_from("<5I", prefix, 296)
    if schema != 1 or not code or api != 1 or flash_bytes not in (4194304, 8388608):
        raise ValueError("unsupported embedded release identity")
    metadata = dict(project=fixed_text(prefix, 316, 32), board=fixed_text(prefix, 348, 64),
                    target=fixed_text(prefix, 412, 16), layout=fixed_text(prefix, 428, 32),
                    profile=fixed_text(prefix, 460, 32), flash_bytes=flash_bytes, features=features, api=api)
    version = fixed_text(prefix, 48, 32)
    chip = struct.unpack_from("<H", prefix, 12)[0]
    if (metadata["project"] != fixed_text(prefix, 80, 32) or metadata["project"] != "blip-v2" or
            {"esp32": 0, "esp32s3": 9, "esp32c6": 13}.get(metadata["target"]) != chip or
            metadata["layout"] != ("ota-8mb-v1" if flash_bytes == 8388608 else "ota-4mb-v1")):
        raise ValueError("image/embedded identity mismatch")
    maximum = 3145728 if flash_bytes == 8388608 else 1638400
    if size > maximum:
        raise ValueError("firmware exceeds BLIP app partition")
    return metadata, code, version


def web_metadata(path):
    if not 48 <= path.stat().st_size <= 256 * 1024:
        raise ValueError("invalid web bundle size")
    data = path.read_bytes()
    verify_bundle(data)
    count = struct.unpack_from("<H", data, 12)[0]
    for index in range(count):
        offset = 48 + index * 96
        length = data[offset]
        route = data[offset + 24:offset + 24 + length].decode("ascii")
        if not 0 < length <= 63 or ".." in route or "//" in route or not re.fullmatch(r"/[A-Za-z0-9/._-]+", route):
            raise ValueError("unsupported device web asset path")
    code = struct.unpack_from("<I", data, 8)[0]
    return code, f"{code // 1000000}.{code // 1000 % 1000}.{code % 1000}"


def artifact(path, code, version, base_url, minimum_other_code):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    suffix = ".bin" if path.suffix == ".bin" else ".bundle"
    filename = f"{code}-{digest}{suffix}"
    result = dict(code=code, version=version, bytes=path.stat().st_size,
                  url=base_url.rstrip("/") + "/" + filename, sha256=digest, minimum_other_code=minimum_other_code)
    return result, filename


def atomic_copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
            temporary = Path(stream.name)
            with source.open("rb") as input_stream:
                shutil.copyfileobj(input_stream, stream, length=4096)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def publish(firmware, web, base_url, channel, firmware_minimum_web, web_minimum_firmware, catalog, artifacts):
    release_url(base_url)
    if urlsplit(base_url).query or not urlsplit(base_url).path.rstrip("/").endswith("/blip/releases"):
        raise ValueError("base URL must end in /blip/releases with no query")
    ascii_text(channel, 15)
    identity, code, version = firmware_metadata(firmware)
    firmware_artifact, firmware_name = artifact(firmware, code, version, base_url, firmware_minimum_web)
    row = {**identity, "channel": channel, "firmware": firmware_artifact, "web": None}
    web_name = None
    if web is not None:
        web_code, web_version = web_metadata(web)
        row["web"], web_name = artifact(web, web_code, web_version, base_url, web_minimum_firmware)
    validate_release(row)
    existing = load_index(catalog).releases if catalog.exists() else {}
    key = ReleaseIndex.key(row)
    previous = existing.get(key)
    if previous:
        for kind in ("firmware", "web"):
            old, new = previous[kind], row[kind]
            if new is None:
                row[kind] = old  # Firmware-only publication retains independent web release.
            elif old is not None and (new["code"] < old["code"] or
                    (new["code"] == old["code"] and new != old)):
                raise ValueError("release code must increase when artifact metadata changes")
    existing[key] = row
    # Immutable names contain the complete digest. Verify copied bytes before
    # publishing the index, including a source that changed during packaging.
    for source, name, kind in ((firmware, firmware_name, "firmware"), (web, web_name, "web")):
        if source is None:
            continue
        destination = artifacts / name
        atomic_copy(source, destination)
        with destination.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if destination.stat().st_size != row[kind]["bytes"] or digest != row[kind]["sha256"]:
            raise ValueError("artifact changed while packaging")
    catalog.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps({"schema": 1, "releases": list(existing.values())}, indent=2) + "\n"
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=catalog.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(encoded.encode("utf-8")); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, catalog)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--web", type=Path)
    parser.add_argument("--base-url", default="http://www.goldengeek.org/blip/releases")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--firmware-minimum-web", type=int, default=0)
    parser.add_argument("--web-minimum-firmware", type=int, default=1)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    row = publish(args.firmware, args.web, args.base_url, args.channel, args.firmware_minimum_web,
                  args.web_minimum_firmware, args.catalog, args.artifacts)
    print(json.dumps(row, indent=2))


if __name__ == "__main__":
    main()
