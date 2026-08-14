#!/usr/bin/env python3
"""Inspect and upload a BLIP V2 ESP-IDF application image."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import struct
import sys
from pathlib import Path
from urllib.parse import urlsplit


DESCRIPTOR_END = 288
APP_DESCRIPTOR_OFFSET = 32
APP_DESCRIPTOR_MAGIC = 0xABCD5432


def parse_fixed_text(data: bytes, offset: int, capacity: int, label: str) -> str:
    field = data[offset : offset + capacity]
    terminator = field.find(b"\0")
    if terminator <= 0:
        raise ValueError(f"invalid {label} in application descriptor")
    return field[:terminator].decode("ascii")


def inspect_image(path: Path) -> tuple[int, str, str, str]:
    size = path.stat().st_size
    if size < DESCRIPTOR_END:
        raise ValueError("image is too small for an ESP application descriptor")
    with path.open("rb") as source:
        prefix = source.read(DESCRIPTOR_END)
        if prefix[0] != 0xE9:
            raise ValueError("image does not have the ESP application magic")
        magic = struct.unpack_from("<I", prefix, APP_DESCRIPTOR_OFFSET)[0]
        if magic != APP_DESCRIPTOR_MAGIC:
            raise ValueError("image does not have an ESP application descriptor")
        version = parse_fixed_text(prefix, 48, 32, "version")
        project = parse_fixed_text(prefix, 80, 32, "project")
        digest = hashlib.sha256(prefix)
        while chunk := source.read(65536):
            digest.update(chunk)
    return size, digest.hexdigest(), project, version


def upload(
    device: str,
    image: Path,
    target: str,
    profile: str,
    interrupt_after: int | None = None,
    timeout_seconds: float = 300.0,
) -> None:
    size, digest, project, version = inspect_image(image)
    if interrupt_after is not None and not 0 < interrupt_after < size:
        raise ValueError("--interrupt-after must be greater than zero and smaller than the image")
    parsed = urlsplit(device)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        raise ValueError("device must be an http:// or https:// origin")
    connection_type = (
        http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
    )
    connection = connection_type(parsed.hostname, parsed.port, timeout=timeout_seconds)
    endpoint = f"{parsed.path.rstrip('/')}/api/firmware"
    connection.putrequest("PUT", endpoint)
    connection.putheader("Content-Type", "application/octet-stream")
    connection.putheader("Content-Length", str(size))
    connection.putheader("X-BLIP-SHA256", digest)
    connection.putheader("X-BLIP-Project", project)
    connection.putheader("X-BLIP-Version", version)
    connection.putheader("X-BLIP-Target", target)
    connection.putheader("X-BLIP-Profile", profile)
    connection.endheaders()
    sent = 0
    with image.open("rb") as source:
        while sent < size:
            requested = min(65536, size - sent)
            if interrupt_after is not None:
                requested = min(requested, interrupt_after - sent)
            chunk = source.read(requested)
            if not chunk:
                break
            connection.send(chunk)
            sent += len(chunk)
            if interrupt_after is not None and sent == interrupt_after:
                connection.close()
                print(
                    f"PASS OTA transport interrupted after {sent}/{size} bytes: "
                    f"{project} {version}, {target}/{profile}, sha256={digest}"
                )
                return
    response = connection.getresponse()
    body = response.read().decode("utf-8", errors="replace")
    connection.close()
    if response.status != 202:
        raise RuntimeError(f"device rejected update: HTTP {response.status}: {body}")
    print(
        f"PASS OTA accepted: {project} {version}, {target}/{profile}, "
        f"{size} bytes, sha256={digest}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--device", help="device origin, for example http://192.0.2.8")
    parser.add_argument("--target", required=True, choices=("esp32", "esp32s3", "esp32c6"))
    parser.add_argument("--profile", default="minimal")
    parser.add_argument("--inspect", action="store_true", help="validate and print metadata only")
    parser.add_argument(
        "--interrupt-after",
        type=int,
        help="close the connection after this many image bytes (HIL interruption test)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=300.0,
        help="HTTP response timeout in seconds (default: 300)",
    )
    args = parser.parse_args()
    if not args.inspect and args.device is None:
        parser.error("--device is required unless --inspect is used")
    try:
        size, digest, project, version = inspect_image(args.image)
        if args.inspect:
            print(
                f"{project} {version}, {args.target}/{args.profile}, "
                f"{size} bytes, sha256={digest}"
            )
        else:
            upload(
                args.device,
                args.image,
                args.target,
                args.profile,
                args.interrupt_after,
                args.timeout,
            )
        return 0
    except (OSError, UnicodeDecodeError, ValueError, RuntimeError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
