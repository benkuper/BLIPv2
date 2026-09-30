#!/usr/bin/env python3
"""Discover one BLIP Art-Net node and send bounded RGB ArtDmx frames."""

from __future__ import annotations

import argparse
import json
import socket
import time


ARTNET_ID = b"Art-Net\x00"
POLL = ARTNET_ID + b"\x00\x20\x00\x0e\x00\x00"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="node IP or directed broadcast address")
    parser.add_argument("--expect-ip", default="", help="required IP in the PollReply")
    parser.add_argument("--pixels", type=int, default=1)
    parser.add_argument("--red", type=int, default=32)
    parser.add_argument("--green", type=int, default=0)
    parser.add_argument("--blue", type=int, default=0)
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--broadcast", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.pixels <= 170 or not 1 <= args.frames <= 100000:
        parser.error("pixels must be 1..170 and frames must be 1..100000")
    if not all(0 <= value <= 255 for value in (args.red, args.green, args.blue)):
        parser.error("color channels must be 0..255")
    if not 0 < args.fps <= 120:
        parser.error("fps must be 0..120")

    data = bytes((args.red, args.green, args.blue)) * args.pixels
    if len(data) & 1:
        data += b"\x00"  # ArtDmx lengths must be even; mapping ignores the trailing channel.
    header = ARTNET_ID + b"\x00\x50\x00\x0e"
    started_at = time.monotonic()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.settimeout(4.0)
        if args.broadcast:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        connection.bind(("", 0))
        connection.sendto(POLL, (args.host, 6454))
        reply, source = connection.recvfrom(512)
        if len(reply) != 239 or reply[:10] != ARTNET_ID + b"\x00\x21":
            raise ValueError("invalid ArtPollReply")
        advertised_ip = socket.inet_ntoa(reply[10:14])
        if advertised_ip != source[0]:
            raise ValueError("ArtPollReply IP differs from its source")
        if args.expect_ip and advertised_ip != args.expect_ip:
            raise ValueError("ArtPollReply came from an unexpected node")
        node_name = reply[26:44].split(b"\x00", 1)[0].decode("ascii", errors="replace")
        connection.settimeout(None)
        for index in range(args.frames):
            sequence = index % 255 + 1
            packet = (header + bytes((sequence, 0)) + b"\x00\x00" +
                      len(data).to_bytes(2, "big") + data)
            connection.sendto(packet, (source[0], 6454))
            if index + 1 < args.frames:
                delay = started_at + (index + 1) / args.fps - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
    print(json.dumps({"node_ip": advertised_ip, "node_name": node_name,
                      "poll_reply_bytes": len(reply), "sent_dmx_frames": args.frames,
                      "elapsed_seconds": round(time.monotonic() - started_at, 3)},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
