#!/usr/bin/env python3
"""Verify unicast ESP-NOW control resumes after a routerless fleet lease."""

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import blip_fleet_hil as fleet

PEER = "blip.transport.espnow"


def wait_for(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.2)
    raise RuntimeError("radio mode transition timed out")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", nargs=2, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    originals = {}
    restoration_errors = []
    report = {"schema_version": 1, "timestamp_utc": datetime.now(timezone.utc).isoformat(),
              "ports": args.ports, "passed": False}
    try:
        for port in args.ports:
            originals[port] = {component: {name: fleet.get(port, name, component) for name in names}
                               for component, names in ((PEER, ("enabled", "peer_mac")),
                                                        (fleet.FLEET, ("enabled", "fleet_id", "channel")))}
            if originals[port][fleet.FLEET]["enabled"]:
                raise RuntimeError("run this gate with fleet initially disabled")
        nodes = {port: fleet.get(port, "node_id") for port in args.ports}
        for index, port in enumerate(args.ports):
            other = args.ports[1 - index]
            address = ":".join(f"{byte:02x}" for byte in nodes[other].to_bytes(6, "big"))
            fleet.set_value(port, "peer_mac", address, PEER)
            fleet.set_value(port, "enabled", True, PEER)
        wait_for(lambda: all(fleet.get(port, "active", PEER) for port in args.ports))
        expected = fleet.get(args.ports[1], "probe_value", fleet.PROBE)

        def remote_read():
            return fleet.request(args.ports[0], "action", PEER, "remote_read", fleet.PROBE,
                                 "probe_value")["values"][0]["value"]

        report["before_remote_read"] = remote_read()
        if report["before_remote_read"] != expected:
            raise RuntimeError("initial peer readback mismatch")
        for port in args.ports:
            fleet.set_value(port, "fleet_id", 0xB11F0002)
            fleet.set_value(port, "channel", 1)
            fleet.set_value(port, "enabled", True)
        fleet.wait_pair(args.ports, min(nodes.values()))
        report["during"] = {port: {"fleet_active": fleet.get(port, "active"),
                                  "peer_active": fleet.get(port, "active", PEER),
                                  "autonomous_channel": fleet.get(port, "autonomous_channel", "blip.transport.wifi")}
                            for port in args.ports}
        if any(not state["fleet_active"] or state["peer_active"] or state["autonomous_channel"] != 1
               for state in report["during"].values()):
            raise RuntimeError("fleet and peer control did not transition as expected")
        for port in args.ports:
            fleet.set_value(port, "enabled", False)
        wait_for(lambda: all(fleet.get(port, "active", PEER) for port in args.ports))
        report["after_remote_read"] = remote_read()
        if report["after_remote_read"] != expected:
            raise RuntimeError("peer readback did not resume after fleet")
        report["passed"] = True
    except (OSError, fleet.control.ProtocolError, RuntimeError) as error:
        report["error"] = str(error)
    finally:
        for port, saved in originals.items():
            try:
                fleet.set_value(port, "enabled", False)
                for component, names in ((fleet.FLEET, ("fleet_id", "channel", "enabled")),
                                         (PEER, ("peer_mac", "enabled"))):
                    for name in names:
                        fleet.set_value(port, name, saved[component][name], component)
                for component, values in saved.items():
                    for name, value in values.items():
                        if fleet.get(port, name, component) != value:
                            restoration_errors.append(f"{port} {component}.{name}: mismatch")
            except (OSError, fleet.control.ProtocolError) as error:
                restoration_errors.append(f"{port}: {error}")
        report["restoration_errors"] = restoration_errors
        report["passed"] = report["passed"] and not restoration_errors
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
