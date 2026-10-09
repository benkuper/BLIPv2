#!/usr/bin/env python3
"""Check broadcast fleet election, cues and recovery on a connected serial group."""

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import blip_fleet_hil as fleet


def consensus(ports, leader_id, seconds=45):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        states = {port: {name: fleet.get(port, name) for name in ("active", "leader_id", "synchronized")}
                  for port in ports}
        if all(state["active"] and state["leader_id"] == leader_id and state["synchronized"]
               for state in states.values()):
            return states
    raise RuntimeError(f"group consensus timed out: {states}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", nargs="+", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if len(args.ports) < 2 or len(set(args.ports)) != len(args.ports):
        parser.error("provide at least two distinct ports")
    originals = {}
    restoration_errors = []
    report = {"schema_version": 1, "timestamp_utc": datetime.now(timezone.utc).isoformat(),
              "ports": args.ports, "passed": False,
              "limits": "Single-hop routerless radio group; software control checks, no physical output timing or delivery guarantee."}
    try:
        for port in args.ports:
            original = {name: fleet.get(port, name) for name in ("enabled", "fleet_id", "channel")}
            original["probe_value"] = fleet.get(port, "probe_value", fleet.PROBE)
            originals[port] = original
            fleet.set_value(port, "enabled", False)
        nodes = {port: fleet.get(port, "node_id") for port in args.ports}
        report["nodes"] = nodes
        ordered = sorted(args.ports, key=nodes.get)
        leader, replacement = ordered[:2]
        report["leader_port"], report["replacement_port"] = leader, replacement
        report["before_executed"] = {port: fleet.get(port, "executed") for port in args.ports}
        for port in args.ports:
            fleet.set_value(port, "fleet_id", 0xB11F0003)
            fleet.set_value(port, "channel", 1)
            fleet.set_value(port, "enabled", True)
        report["initial_consensus"] = consensus(args.ports, nodes[leader])
        report["routerless"] = {port: {name: fleet.get(port, name, "blip.transport.wifi")
                                      for name in ("state", "autonomous_channel", "low_latency_clients", "setup_ap_active")}
                                 for port in args.ports}
        if any(state["state"] != 6 or state["setup_ap_active"] or state["autonomous_channel"] != 1 or state["low_latency_clients"] < 1
               for state in report["routerless"].values()):
            raise RuntimeError("some nodes still depend on router association")
        print(f"All {len(args.ports)} routerless nodes elected {leader} and locked clocks", flush=True)
        fleet.request(leader, "action", fleet.FLEET, "schedule_write_integer",
                      fleet.PROBE, "probe_value", 61, 2500)
        fleet.set_value(replacement, "probe_value", -61, fleet.PROBE)
        time.sleep(2.8)
        report["first_cue"] = {port: fleet.get(port, "probe_value", fleet.PROBE) for port in args.ports}
        if set(report["first_cue"].values()) != {61}:
            raise RuntimeError("first cue did not execute across the group")
        print("First cue executed on every node; direct serial control also worked", flush=True)
        fleet.request(leader, "action", fleet.FLEET, "schedule_write_integer",
                      fleet.PROBE, "probe_value", 77, 30000)
        time.sleep(1)
        report["queued"] = {port: fleet.get(port, "pending") for port in args.ports}
        if set(report["queued"].values()) != {1}:
            raise RuntimeError("future cue was not received across the group")
        fleet.set_value(leader, "enabled", False)
        remaining = [port for port in args.ports if port != leader]
        started = time.monotonic()
        report["replacement_consensus"] = consensus(remaining, nodes[replacement])
        report["observed_recovery_seconds"] = time.monotonic() - started
        if any(fleet.get(port, "pending") != 0 for port in remaining):
            raise RuntimeError("leader loss did not cancel pending work")
        print(f"Remaining nodes recovered with {replacement} as leader", flush=True)
        fleet.request(replacement, "action", fleet.FLEET, "schedule_write_integer",
                      fleet.PROBE, "probe_value", 62, 2500)
        time.sleep(2.8)
        report["replacement_cue"] = {port: fleet.get(port, "probe_value", fleet.PROBE) for port in remaining}
        if set(report["replacement_cue"].values()) != {62}:
            raise RuntimeError("replacement leader cue did not execute across the group")
        fleet.set_value(leader, "enabled", True)
        report["rejoin_consensus"] = consensus(args.ports, nodes[leader])
        names = ("executed", "execution_failed", "execution_dropped", "late", "duplicates",
                 "maximum_lateness_us", "network_stack_headroom", "executor_stack_headroom")
        report["after"] = {port: {name: fleet.get(port, name) for name in names} for port in args.ports}
        for port, state in report["after"].items():
            if min(state["network_stack_headroom"], state["executor_stack_headroom"]) < 1024:
                raise RuntimeError(f"fleet stack reserve below 1024 bytes on {port}")
            expected = 1 if port == leader else 2
            if state["executed"] - report["before_executed"][port] != expected or any(
                state[name] != 0 for name in ("execution_failed", "execution_dropped", "late")):
                raise RuntimeError(f"incorrect execution counters on {port}")
        report["passed"] = True
    except (OSError, fleet.control.ProtocolError, RuntimeError) as error:
        report["error"] = str(error)
    finally:
        report["restored"] = {}
        for port, saved in originals.items():
            try:
                fleet.set_value(port, "enabled", False)
                fleet.set_value(port, "probe_value", saved["probe_value"], fleet.PROBE)
                for name in ("fleet_id", "channel", "enabled"):
                    fleet.set_value(port, name, saved[name])
                restored = {name: fleet.get(port, name) for name in ("enabled", "fleet_id", "channel")}
                restored["probe_value"] = fleet.get(port, "probe_value", fleet.PROBE)
                report["restored"][port] = restored
                if restored != saved:
                    restoration_errors.append(f"{port}: setting restoration mismatch")
                if not saved["enabled"] and (fleet.get(port, "autonomous_channel", "blip.transport.wifi") != 0 or
                                              fleet.get(port, "low_latency_clients", "blip.transport.wifi") != 0):
                    restoration_errors.append(f"{port}: radio lease not released")
            except (OSError, fleet.control.ProtocolError) as error:
                restoration_errors.append(f"{port}: {error}")
        report["restoration_errors"] = restoration_errors
        report["passed"] = report["passed"] and not restoration_errors
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "error": report.get("error"), "report": str(args.out)}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
