#!/usr/bin/env python3
"""Test a routerless ESP-NOW fleet over serial without changing the computer's Wi-Fi."""

from __future__ import annotations

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import blip_serial_control as control

FLEET = "blip.fleet"
PROBE = "blip.bootstrap"


def request(port: str, operation: str, component: str, name: str, *values):
    request_id = time.monotonic_ns() & 0xFFFFFFFF
    scalars = [control.Scalar(control.VALUE_BOOLEAN if isinstance(value, bool) else
                             control.VALUE_INTEGER if isinstance(value, int) else
                             control.VALUE_STRING, value) for value in values]
    payload = control.encode_control(control.OPERATION_BY_NAME[operation], component, name, scalars)
    frame = control.encode_envelope(control.KIND_REQUEST, request_id, payload)
    return control.exchange(port, 115200, 4.0, frame, request_id)


def get(port: str, name: str, component: str = FLEET):
    return request(port, "get", component, name)["values"][0]["value"]


def set_value(port: str, name: str, value, component: str = FLEET):
    return request(port, "set", component, name, value)


def snapshot(port: str):
    names = ("node_id", "leader_id", "is_leader", "synchronized", "active", "pending",
             "clock_samples", "offset_us", "uncertainty_us", "sample_age_us", "executed",
             "execution_failed", "execution_dropped", "late", "cancelled", "duplicates",
             "send_failed", "invalid_packets", "maximum_lateness_us", "last_execution_us",
             "network_stack_headroom", "executor_stack_headroom")
    return {name: get(port, name) for name in names}


def wait_pair(ports: list[str], leader_id: int, seconds: float = 30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        observations = [{name: get(port, name) for name in
                         ("leader_id", "synchronized", "active")} for port in ports]
        if all(value["leader_id"] == leader_id and value["synchronized"] and
               value["active"] for value in observations):
            return observations
        time.sleep(0.25)
    raise RuntimeError(f"fleet did not converge: {observations}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", nargs=2, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    report = {"schema_version": 1, "gate_id": "M5-7-FLEET-PAIR-HIL",
              "timestamp_utc": datetime.now(timezone.utc).isoformat(), "ports": args.ports,
              "checks": [], "passed": False,
              "limits": "Software dispatch timing on two routerless ESP-NOW nodes; no physical LED timing measurement, peer authentication, or large-fleet radio qualification."}
    originals = {}
    restore_errors = []
    try:
        for port in args.ports:
            originals[port] = {name: get(port, name) for name in ("enabled", "fleet_id", "channel")}
            originals[port]["probe_value"] = get(port, "probe_value", PROBE)
            originals[port]["low_latency_clients"] = get(port, "low_latency_clients", "blip.transport.wifi")
            originals[port]["autonomous_channel"] = get(port, "autonomous_channel", "blip.transport.wifi")
            set_value(port, "enabled", False)
        report["originals"] = originals
        nodes = {port: get(port, "node_id") for port in args.ports}
        leader = min(nodes, key=nodes.get)
        follower = next(port for port in args.ports if port != leader)
        report["leader_port"] = leader
        for port in args.ports:
            set_value(port, "fleet_id", 0xB11F0001)
            set_value(port, "channel", 1)
            set_value(port, "enabled", True)
        report["initial_convergence"] = wait_pair(args.ports, nodes[leader])
        print("Fleet elected and synchronized", flush=True)
        report["before"] = {port: snapshot(port) for port in args.ports}
        report["latency_leases"] = {port: get(port, "low_latency_clients", "blip.transport.wifi") for port in args.ports}
        report["routerless_radio"] = {port: {name: get(port, name, "blip.transport.wifi")
                                             for name in ("autonomous_channel", "state", "setup_ap_active")}
                                       for port in args.ports}
        if any(value["autonomous_channel"] != 1 or value["state"] != 6 or value["setup_ap_active"]
               for value in report["routerless_radio"].values()):
            raise RuntimeError("fleet did not enter routerless mode")
        if any(report["latency_leases"][port] != originals[port]["low_latency_clients"] + 1 for port in args.ports):
            raise RuntimeError("fleet did not acquire its Wi-Fi latency lease")
        report["checks"].append("two nodes elect the lowest station node and lock their clocks")

        for value in (31, 32, 33, 34):
            queued = request(leader, "action", FLEET, "schedule_write_integer",
                             PROBE, "probe_value", value, 1500)
            # Direct normal control stays usable while a scheduled command is pending.
            set_value(follower, "probe_value", -value, PROBE)
            time.sleep(1.8)
            observed = {port: get(port, "probe_value", PROBE) for port in args.ports}
            if set(observed.values()) != {value}:
                raise RuntimeError(f"scheduled write did not execute on both nodes: {observed}")
            report.setdefault("writes", []).append({"value": value, "sequence": queued["values"][0]["value"],
                                                     "observed": observed})
            print(f"Cue {value} executed on both nodes", flush=True)
        request(leader, "action", FLEET, "schedule_action", PROBE, "reset_probe", 1500)
        time.sleep(1.8)
        if any(get(port, "probe_value", PROBE) != 0 for port in args.ports):
            raise RuntimeError("scheduled action did not execute on both nodes")
        report["checks"].append("four integer cues and one action execute once on each node alongside direct control")

        request(leader, "action", FLEET, "schedule_write_integer", PROBE, "probe_value", 77, 10000)
        time.sleep(0.3)
        if get(follower, "pending") != 1:
            raise RuntimeError("follower did not queue the future cue")
        set_value(leader, "enabled", False)
        started = time.monotonic()
        deadline = started + 8
        while time.monotonic() < deadline:
            set_value(follower, "probe_value", 42, PROBE)
            if get(follower, "is_leader") and get(follower, "pending") == 0:
                break
        else:
            raise RuntimeError("follower did not recover after leader withdrawal")
        report["leader_recovery_seconds"] = time.monotonic() - started
        print("Leader loss recovered while direct control remained usable", flush=True)
        request(follower, "action", FLEET, "schedule_write_integer", PROBE, "probe_value", 43, 1500)
        time.sleep(1.8)
        if get(follower, "probe_value", PROBE) != 43:
            raise RuntimeError("replacement leader could not execute a cue")
        report["checks"].append("leader withdrawal cancels old cues; direct control and replacement leader cues still work")
        set_value(leader, "enabled", True)
        report["rejoin_convergence"] = wait_pair(args.ports, nodes[leader])
        # Wait beyond the abandoned cue deadline to prove it was cancelled.
        time.sleep(3)
        if get(follower, "probe_value", PROBE) != 43:
            raise RuntimeError("cancelled old-epoch cue executed after rejoin")
        report["after"] = {port: snapshot(port) for port in args.ports}
        for port in args.ports:
            before, after = report["before"][port], report["after"][port]
            expected = 6 if port == follower else 5
            if after["executed"] - before["executed"] != expected:
                raise RuntimeError(f"incorrect execution count on {port}")
            if after["execution_failed"] != before["execution_failed"] or after["execution_dropped"] != before["execution_dropped"]:
                raise RuntimeError(f"executor failed or dropped a command on {port}")
        report["checks"].append("returning original leader is elected and clocks relock; cancelled cue stays cancelled")
        report["passed"] = True
    except (OSError, control.ProtocolError, RuntimeError) as error:
        report["error"] = str(error)
        report["failure_observations"] = {}
        for port in args.ports:
            try:
                report["failure_observations"][port] = snapshot(port)
            except (OSError, control.ProtocolError) as diagnostic_error:
                report["failure_observations"][port] = {"error": str(diagnostic_error)}
    finally:
        for port, original in originals.items():
            for name, value, component in (("enabled", False, FLEET),
                                            ("probe_value", original.get("probe_value", 0), PROBE),
                                            ("fleet_id", original["fleet_id"], FLEET),
                                            ("channel", original["channel"], FLEET),
                                            ("enabled", original["enabled"], FLEET)):
                try:
                    set_value(port, name, value, component)
                except (OSError, control.ProtocolError) as error:
                    restore_errors.append(f"{port} {name}: {error}")
        report["restored"] = {}
        for port, original in originals.items():
            try:
                restored = {name: get(port, name) for name in ("enabled", "fleet_id", "channel")}
                restored["probe_value"] = get(port, "probe_value", PROBE)
                restored["low_latency_clients"] = get(port, "low_latency_clients", "blip.transport.wifi")
                restored["autonomous_channel"] = get(port, "autonomous_channel", "blip.transport.wifi")
                report["restored"][port] = restored
                if restored != original:
                    restore_errors.append(f"{port}: restored state differs from original")
            except (OSError, control.ProtocolError) as error:
                restore_errors.append(f"{port} restoration verification: {error}")
        report["restoration_errors"] = restore_errors
        report["passed"] = report["passed"] and not restore_errors
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "error": report.get("error"), "report": str(args.out)}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
