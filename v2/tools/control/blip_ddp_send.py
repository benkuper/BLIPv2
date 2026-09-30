#!/usr/bin/env python3
"""Send bounded RGB8 DDP frames to a BLIP pixel output."""

from __future__ import annotations

import argparse
import json
import socket
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=4048)
    parser.add_argument("--pixels", type=int, required=True)
    parser.add_argument("--lit-pixels", type=int, default=1)
    parser.add_argument("--red", type=int, default=255)
    parser.add_argument("--green", type=int, default=0)
    parser.add_argument("--blue", type=int, default=0)
    parser.add_argument("--frames", type=int, default=1)
    parser.add_argument("--fps", type=float, default=30.0)
    args = parser.parse_args()
    if not 1 <= args.pixels <= 480 or not 0 <= args.lit_pixels <= args.pixels:
        parser.error("pixels must be 1..480 and lit-pixels must fit within pixels")
    if not all(0 <= value <= 255 for value in (args.red, args.green, args.blue)):
        parser.error("color channels must be 0..255")
    if not 1 <= args.frames <= 100000 or not 0 < args.fps <= 120:
        parser.error("frames must be 1..100000 and fps must be 0..120")

    data = bytes((args.red, args.green, args.blue)) * args.lit_pixels
    data += bytes(3 * (args.pixels - args.lit_pixels))
    prefix = bytes((0x41, 0, 0x0B, 1)) + bytes(4) + len(data).to_bytes(2, "big")
    destination = (args.host, args.port)
    started_at = time.monotonic()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        for index in range(args.frames):
            sequence = index % 15 + 1
            packet = prefix[:1] + bytes((sequence,)) + prefix[2:] + data
            connection.sendto(packet, destination)
            if index + 1 < args.frames:
                delay = started_at + (index + 1) / args.fps - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
    print(json.dumps({"sent_frames": args.frames,
                      "elapsed_seconds": round(time.monotonic() - started_at, 3),
                      "host": args.host, "port": args.port}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
