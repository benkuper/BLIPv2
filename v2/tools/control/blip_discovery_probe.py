#!/usr/bin/env python3
"""Verify BLIP discovery on shared Wi-Fi without changing the PC's network."""
from __future__ import annotations

import argparse
import ipaddress
import json
import socket
import time
import urllib.request
from pathlib import Path

from zeroconf import IPVersion, ServiceBrowser, ServiceStateChange, Zeroconf
import blip_serial_control as control

OSC = "_osc._udp.local."
OSCQUERY = "_oscjson._tcp.local."


def serial_request(port, operation, component, name, *values):
    request_id = time.monotonic_ns() & 0xffffffff
    scalars = [control.Scalar(control.VALUE_BOOLEAN, value) for value in values]
    payload = control.encode_control(control.OPERATION_BY_NAME[operation], component, name, scalars)
    request = control.encode_envelope(control.KIND_REQUEST, request_id, payload)
    return control.exchange(port, 115200, 5, request, request_id)


def http_json(host, path):
    request = urllib.request.Request(f"http://{host}{path}", headers={"Accept": "application/json"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=8) as response:
        assert response.headers.get_content_type() == "application/json"
        return json.load(response)


def services(host, timeout=12, keep_open=None):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((host, 5353))
        interface = route.getsockname()[0]
    seen = set()
    def change(zeroconf, service_type, name, state_change):
        if state_change != ServiceStateChange.Removed:
            seen.add((service_type, name))
    found = {}
    zc = Zeroconf(interfaces=[interface], ip_version=IPVersion.V4Only)
    browser = ServiceBrowser(zc, [OSC, OSCQUERY], handlers=[change])
    try:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and len(found) < 2:
            for service_type, name in tuple(seen):
                info = zc.get_service_info(service_type, name, timeout=500)
                if info and host in info.parsed_addresses():
                    found[service_type] = {"name": name, "server": info.server,
                        "port": info.port, "addresses": info.parsed_addresses(),
                        "txt": {key.decode(): value.decode() if value is not None else None
                                for key, value in info.properties.items()}}
            time.sleep(0.05)
    finally:
        if keep_open is not None:
            keep_open.append((browser, zc))
        else:
            browser.cancel()
            zc.close()
    return found


def receive_node(udp, host):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        udp.settimeout(max(0.01, deadline - time.monotonic()))
        reply, source = udp.recvfrom(512)
        if source[0] == host:
            return reply, source
    raise TimeoutError("expected node did not reply")


def osc_ping(host, port, device_id):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.sendto(b"/ping\0\0\0,\0\0\0", (host, port))
        reply, source = receive_node(udp, host)
        identifier = device_id.encode() + b"\0"
        identifier += bytes((-len(identifier)) % 4)
        assert source[1] == port and reply == b"/pong\0\0\0,s\0\0" + identifier
    return {"port": port, "device_id": device_id}


def artnet(host, broadcast=None):
    poll = b"Art-Net\0\x00\x20\x00\x0e\x02\x00"
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.settimeout(3)
        if broadcast:
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        udp.sendto(poll, (broadcast or host, 6454))
        reply, source = receive_node(udp, host)
        assert source[0] == host and len(reply) == 239 and reply[:10] == b"Art-Net\0\x00\x21"
        assert socket.inet_ntoa(reply[10:14]) == host
        assert int.from_bytes(reply[14:16], "little") == 6454
        assert reply[172:175] == b"\0\x01\x80" and reply[211] & 0x0d == 0x0d
        # A targeted poll outside this receiver's universe must remain unanswered.
        udp.sendto(poll[:12] + b"\x20\0\x00\x02\x00\x01", (host, 6454))
        deadline = time.monotonic() + 1.4
        try:
            while time.monotonic() < deadline:
                udp.settimeout(max(0.01, deadline - time.monotonic()))
                _, peer = udp.recvfrom(512)
                if peer[0] == host:
                    raise AssertionError("Art-Net replied to an unrelated targeted poll")
        except socket.timeout:
            pass
    return {"port_name": reply[26:44].split(b"\0")[0].decode(),
            "universe": reply[18] << 8 | reply[19] << 4 | reply[190],
            "status2": reply[211], "mac": reply[201:207].hex(":")}


def ddp(host, broadcast=None):
    results = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.settimeout(3)
        if broadcast:
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        for name, identifier in (("status", 251), ("config", 250)):
            udp.sendto(bytes((0x42, 0, 0, identifier)) + bytes(6),
                       (broadcast if broadcast and identifier == 251 else host, 4048))
            reply, source = receive_node(udp, host)
            assert source[0] == host and reply[:4] == bytes((0x45, 0, 0, identifier))
            assert reply[4:8] == bytes(4) and len(reply) == 10 + int.from_bytes(reply[8:10], "big")
            results[name] = json.loads(reply[10:])[name]
        assert results["status"]["man"] == "BLIP" and not results["status"]["ntp"]
        assert results["config"]["num_chan"] == results["config"]["ports"][0]["l"] * 3
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="device IPv4; can be read from --port")
    parser.add_argument("--port", help="serial port, required for --cycles")
    parser.add_argument("--cycles", type=int, default=0, help="Wi-Fi disable/re-enable cycles; restore enabled state")
    parser.add_argument("--no-lighting", action="store_true", help="build excludes Art-Net and DDP")
    parser.add_argument("--broadcast-address", help="also check native discovery via the subnet's directed broadcast")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if not args.host and not args.port:
        parser.error("--host or --port is required")
    if args.cycles < 0 or args.cycles > 5 or (args.cycles and not args.port):
        parser.error("cycles must be 0..5 and require --port")
    host = args.host or serial_request(args.port, "get", "blip.transport.wifi", "ip_address")["values"][0]["value"]
    ipaddress.IPv4Address(host)
    if args.broadcast_address:
        ipaddress.IPv4Address(args.broadcast_address)
    report = {"host": host, "passed": False, "cycles": []}
    wifi = "blip.transport.wifi"
    original = None
    discovery_clients = []
    try:
        report["phase"] = "host_info"
        info = http_json(host, "/?HOST_INFO")
        report["host_info"] = info
        report["phase"] = "mdns"
        found = services(host, keep_open=discovery_clients)
        report["services"] = found
        assert set(found) == {OSC, OSCQUERY}, f"missing services: {found}"
        assert found[OSC]["port"] == info["OSC_PORT"] == 9000
        assert found[OSCQUERY]["port"] == 80 and found[OSC]["server"] == found[OSCQUERY]["server"]
        assert found[OSCQUERY]["txt"].get("path") == "/"
        suffix = info["DEVICE_ID"].replace(":", "").lower()
        assert found[OSC]["server"] == f"blip-{suffix}.local."
        report["phase"] = "osc_udp"
        report["osc"] = osc_ping(host, found[OSC]["port"], info["DEVICE_ID"])
        report["phase"] = "oscquery_tree"
        assert "CONTENTS" in http_json(host, "/")
        report.update(services=found, host_info=info)
        if not args.no_lighting:
            report["phase"] = "artnet"
            report["artnet"] = artnet(host)
            report["phase"] = "ddp"
            report["ddp"] = ddp(host)
            if args.broadcast_address:
                report["phase"] = "broadcast"
                report["broadcast"] = {"artnet": artnet(host, args.broadcast_address),
                                       "ddp": ddp(host, args.broadcast_address)}
        if args.cycles:
            report["phase"] = "wifi_cycles"
            original = serial_request(args.port, "get", wifi, "enabled")["values"][0]["value"]
            assert original, "Wi-Fi must start enabled"
            for _ in range(args.cycles):
                report["phase"] = "wifi_disable"
                serial_request(args.port, "set", wifi, "enabled", False)
                assert not services(host, timeout=3), "services still respond after Wi-Fi disable"
                serial_request(args.port, "set", wifi, "enabled", True)
                report["phase"] = "wifi_reconnect"
                deadline = time.monotonic() + 20
                while serial_request(args.port, "get", wifi, "state")["values"][0]["value"] != 2:
                    assert time.monotonic() < deadline, "Wi-Fi did not reconnect"
                    time.sleep(0.2)
                host = serial_request(args.port, "get", wifi, "ip_address")["values"][0]["value"]
                report["phase"] = "mdns_readvertise"
                found = services(host, timeout=20, keep_open=discovery_clients)
                assert set(found) == {OSC, OSCQUERY}, f"services did not return: {found}"
                report["phase"] = "oscquery_after_reconnect"
                assert "CONTENTS" in http_json(host, "/")
                report["cycles"].append({"host": host, "withdrawn": True, "readvertised": True})
        report["passed"] = True
        report["phase"] = "complete"
    except Exception as error:
        report["error"] = str(error)
    finally:
        for browser, zc in discovery_clients:
            browser.cancel()
            zc.close()
        if original is not None:
            try:
                serial_request(args.port, "set", wifi, "enabled", original)
                report["wifi_enabled_restored"] = serial_request(
                    args.port, "get", wifi, "enabled")["values"][0]["value"] == original
                if not report["wifi_enabled_restored"]:
                    report["passed"] = False
            except Exception as error:
                report["passed"] = False
                report["restore_error"] = str(error)
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
