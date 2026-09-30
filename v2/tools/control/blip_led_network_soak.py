#!/usr/bin/env python3
"""Run a bounded three-protocol LED network soak while sampling serial metrics."""

from __future__ import annotations

import argparse
import ipaddress
import json
import socket
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import blip_serial_control as control
from blip_e131_send import packet as e131_packet


METRICS = (
    ("blip.diagnostics", "heap_free_internal"),
    ("blip.diagnostics", "heap_minimum_internal"),
    ("blip.diagnostics", "heap_largest_internal"),
    ("blip.output.strip0", "applied_frames"),
    ("blip.output.strip0", "failed_frames"),
    ("blip.output.strip0", "last_frame_us"),
    ("blip.output.strip0", "worker_stack_headroom"),
    ("blip.input.artnet", "accepted_packets"),
    ("blip.input.artnet", "rejected_packets"),
    ("blip.input.ddp", "accepted_packets"),
    ("blip.input.ddp", "rejected_packets"),
    ("blip.input.ddp", "worker_stack_headroom"),
    ("blip.input.e131", "accepted_packets"),
    ("blip.input.e131", "rejected_packets"),
    ("blip.input.e131", "worker_stack_headroom"),
)


def read_metric(port: str, component: str, name: str) -> int:
    request_id = time.monotonic_ns() & 0xFFFFFFFF
    payload = control.encode_control(control.OPERATION_BY_NAME["get"], component, name, ())
    request = control.encode_envelope(control.KIND_REQUEST, request_id, payload)
    response = control.exchange(port, 115200, 4.0, request, request_id)
    values = response["values"]
    if len(values) != 1 or values[0]["type"] != "integer":
        raise control.ProtocolError(f"{component}.{name} did not return one integer")
    return int(values[0]["value"])


def sample(port: str, start: float) -> dict[str, object]:
    values = {}
    for component, name in METRICS:
        values[f"{component}.{name}"] = read_metric(port, component, name)
    return {"elapsed_seconds": round(time.monotonic() - start, 3), "values": values}


def send_frames(host: str, pixels: int, fps: float, stop: threading.Event,
                counts: dict[str, int], errors: list[str]) -> None:
    color = bytes((8, 0, 0)) * pixels
    art_data = color + (b"\x00" if len(color) & 1 else b"")
    art_header = b"Art-Net\x00\x00\x50\x00\x0e"
    ddp_prefix = bytes((0x41, 0, 0x0B, 1)) + bytes(4) + len(color).to_bytes(2, "big")
    source_id = uuid.uuid4().bytes
    ports = {"artnet": 6454, "ddp": 4048, "e131": 5568}
    period = 1.0 / fps
    due = {name: time.monotonic() for name in ports}
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
            while not stop.is_set():
                now = time.monotonic()
                for name, port in ports.items():
                    if now < due[name]:
                        continue
                    index = counts[name]
                    if name == "artnet":
                        sequence = index % 255 + 1
                        frame = (art_header + bytes((sequence, 0)) + b"\x00\x00" +
                                 len(art_data).to_bytes(2, "big") + art_data)
                    elif name == "ddp":
                        sequence = index % 15 + 1
                        frame = ddp_prefix[:1] + bytes((sequence,)) + ddp_prefix[2:] + color
                    else:
                        frame = e131_packet(1, source_id, index & 255, 100, 0, color)
                    connection.sendto(frame, (host, port))
                    counts[name] += 1
                    due[name] = now + period
                delay = min(due.values()) - time.monotonic()
                stop.wait(max(0.001, min(delay, 0.01)))
    except OSError as error:
        errors.append(str(error))
        stop.set()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="board station IPv4 address")
    parser.add_argument("--port", required=True, help="board control serial port")
    parser.add_argument("--pixels", type=int, default=36)
    parser.add_argument("--fps", type=float, default=20.0,
                        help="frames per second per protocol")
    parser.add_argument("--duration", type=int, default=180, help="seconds of traffic")
    parser.add_argument("--sample-seconds", type=int, default=30)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        ipaddress.IPv4Address(args.host)
    except ipaddress.AddressValueError as error:
        parser.error(str(error))
    if not 1 <= args.pixels <= 170 or not 1 <= args.fps <= 60:
        parser.error("pixels must be 1..170 and fps must be 1..60")
    if not 30 <= args.duration <= 1800 or not 20 <= args.sample_seconds <= args.duration:
        parser.error("duration must be 30..1800 and sample-seconds must be 20..duration")

    started_utc = datetime.now(timezone.utc).isoformat()
    baseline = sample(args.port, time.monotonic())
    baseline["elapsed_seconds"] = 0.0
    counts = {"artnet": 0, "ddp": 0, "e131": 0}
    errors: list[str] = []
    stop = threading.Event()
    sender = threading.Thread(target=send_frames,
                              args=(args.host, args.pixels, args.fps, stop, counts, errors),
                              daemon=True)
    started = time.monotonic()
    sender.start()
    samples = [baseline]
    try:
        while time.monotonic() - started < args.duration and not stop.is_set():
            remaining = args.duration - (time.monotonic() - started)
            stop.wait(min(args.sample_seconds, max(0, remaining)))
            if not stop.is_set():
                try:
                    samples.append(sample(args.port, started))
                except (OSError, control.ProtocolError) as error:
                    errors.append(f"serial sample failed: {error}")
                    break
    finally:
        stop.set()
        sender.join(timeout=5)
    traffic_duration = time.monotonic() - started
    try:
        samples.append(sample(args.port, started))
    except (OSError, control.ProtocolError) as error:
        errors.append(f"final serial sample failed: {error}")
    report = {
        "schema_version": 1,
        "started_at_utc": started_utc,
        "host": args.host,
        "serial_port": args.port,
        "pixels": args.pixels,
        "fps_per_protocol": args.fps,
        "requested_duration_seconds": args.duration,
        "traffic_duration_seconds": round(traffic_duration, 3),
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "sent_packets": counts,
        "sender_errors": errors,
        "samples": samples,
        "limits": "Serial and transport counters do not establish waveform integrity or a Gate C pass."
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"out": str(args.out), "sent_packets": counts,
                      "samples": len(samples), "sender_errors": errors}, sort_keys=True))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
