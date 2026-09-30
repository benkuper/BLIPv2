#!/usr/bin/env python3
"""Send bounded E1.31/sACN RGB frames to a BLIP node on shared Wi-Fi."""

from __future__ import annotations

import argparse
import json
import socket
import time
import uuid


ACN_ID = b"ASC-E1.17\x00\x00\x00"
PORT = 5568


def packet(universe: int, source_id: bytes, sequence: int,
           priority: int, options: int, channels: bytes) -> bytes:
    output = bytearray(126 + len(channels))
    output[0:2] = (16).to_bytes(2, "big")
    output[4:16] = ACN_ID
    output[16:18] = (0x7000 | (len(output) - 16)).to_bytes(2, "big")
    output[18:22] = (4).to_bytes(4, "big")
    output[22:38] = source_id
    output[38:40] = (0x7000 | (len(output) - 38)).to_bytes(2, "big")
    output[40:44] = (2).to_bytes(4, "big")
    output[44:52] = b"BLIP HIL"
    output[108] = priority
    output[111] = sequence
    output[112] = options
    output[113:115] = universe.to_bytes(2, "big")
    output[115:117] = (0x7000 | (len(output) - 115)).to_bytes(2, "big")
    output[117:119] = b"\x02\xa1"
    output[121:123] = (1).to_bytes(2, "big")
    output[123:125] = (1 + len(channels)).to_bytes(2, "big")
    output[126:] = channels
    return bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="device IP for unicast")
    parser.add_argument("--universe", type=int, default=1)
    parser.add_argument("--multicast", action="store_true",
                        help="send to the universe's 239.255.x.y group")
    parser.add_argument("--pixels", type=int, default=1)
    parser.add_argument("--red", type=int, default=32)
    parser.add_argument("--green", type=int, default=0)
    parser.add_argument("--blue", type=int, default=0)
    parser.add_argument("--priority", type=int, default=100)
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--terminate", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.universe <= 63999:
        parser.error("universe must be 1..63999")
    if not 1 <= args.pixels <= 170 or not 1 <= args.frames <= 100000:
        parser.error("pixels must be 1..170 and frames must be 1..100000")
    if not all(0 <= value <= 255 for value in (args.red, args.green, args.blue)):
        parser.error("color channels must be 0..255")
    if not 0 <= args.priority <= 200 or not 0 < args.fps <= 120:
        parser.error("priority must be 0..200 and fps must be 0..120")

    source_id = uuid.uuid4().bytes
    channels = bytes((args.red, args.green, args.blue)) * args.pixels
    destination = (f"239.255.{args.universe >> 8}.{args.universe & 255}"
                   if args.multicast else args.host)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        if args.multicast:
            connection.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        started_at = time.monotonic()
        for index in range(args.frames):
            connection.sendto(packet(args.universe, source_id, index & 255,
                                     args.priority, 0, channels), (destination, PORT))
            if index + 1 < args.frames:
                delay = started_at + (index + 1) / args.fps - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
        if args.terminate:
            connection.sendto(packet(args.universe, source_id, args.frames & 255,
                                     args.priority, 0x40, channels), (destination, PORT))
    print(json.dumps({"destination": destination, "port": PORT,
                      "universe": args.universe, "sent_data_frames": args.frames,
                      "sent_termination": args.terminate,
                      "elapsed_seconds": round(time.monotonic() - started_at, 3)},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
