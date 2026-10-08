"""Exercise production LED/fleet guest imports over serial; never change the PC network."""
import argparse
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import time

import wasmtime
from blip_wasm_hil import Client, source_snapshot as worker_snapshot

ROOT = Path(__file__).resolve().parents[3]
LED = "blip.output.strip0"
FLEET = "blip.fleet"


def snapshot():
    files = worker_snapshot(ROOT)["files"]
    for component in ("blip_core", "blip_led", "blip_fleet"):
        for path in (ROOT / "v2/components" / component).rglob("*"):
            if path.is_file() and (path.suffix in (".cpp", ".c", ".hpp", ".h") or path.name == "CMakeLists.txt"):
                files[path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    path = Path(__file__)
    files[path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    for path in (ROOT / "v2/qualification/wasm-providers").rglob("*"):
        if path.suffix in (".hpp", ".py"):
            files[path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    return files


def fixture():
    if version("wasmtime") != "36.0.0":
        raise RuntimeError("Fixture compilation requires host-only wasmtime==36.0.0")
    wat = r'''(module
    (import "blip.output.strip0.v1" "fill" (func $fill (param i32 i32 i32 i32 i32) (result i32)))
    (import "blip.output.strip0.v1" "clear" (func $clear (result i32)))
    (import "blip.output.strip0.v1" "frames" (func $frames (result i32)))
    (import "blip.output.strip0.v1" "pending" (func $pending (result i32)))
    (import "blip.fleet.v1" "ready" (func $ready (result i32)))
    (import "blip.fleet.v1" "time_us" (func $time (result i64)))
    (import "blip.fleet.v1" "leader" (func $leader (result i64)))
    (import "blip.fleet.v1" "schedule_i32" (func $write (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "blip.fleet.v1" "schedule_action" (func $action (param i32 i32 i32 i32 i32) (result i32)))
    (memory 1 1)
    (data (i32.const 0) "blip.output.strip0")
    (data (i32.const 64) "brightness") (data (i32.const 96) "blackout")
    (data (i32.const 128) "\e0\80\80") (data (i32.const 160) "blip.output.strip0\00evil")
    (func (export "fill") (param i32) (result i32) local.get 0 i32.const 0 i32.const 0 i32.const 0 i32.const 65535 call $fill)
    (func (export "clear") (result i32) call $clear)
    (func (export "frames") (result i32) call $frames)
    (func (export "pending") (result i32) call $pending)
    (func (export "ready") (result i32) call $ready)
    (func (export "time") (result i64) call $time)
    (func (export "leader") (result i64) call $leader)
    (func (export "write") (result i32) i32.const 0 i32.const 18 i32.const 64 i32.const 10 i32.const 73 i32.const 1000 call $write)
    (func (export "action") (result i32) i32.const 0 i32.const 18 i32.const 96 i32.const 8 i32.const 1000 call $action)
    (func (export "bad_utf8") (result i32) i32.const 128 i32.const 3 i32.const 64 i32.const 10 i32.const 73 i32.const 1000 call $write)
    (func (export "bad_nul") (result i32) i32.const 160 i32.const 23 i32.const 64 i32.const 10 i32.const 73 i32.const 1000 call $write)
    (func (export "bad_bounds") (result i32) i32.const -1 i32.const 18 i32.const 64 i32.const 10 i32.const 73 i32.const 1000 call $write)
    (func (export "erase") i32.const 0 i32.const 0 i32.const 110 memory.fill)
    (func (export "flood") (loop $again i32.const 1 i32.const 0 i32.const 0 i32.const 0 i32.const 65535 call $fill drop br $again))
    (func (export "trap") unreachable))'''
    return bytes(wasmtime.wat2wasm(wat))


class Trial:
    def __init__(self, client, report, module):
        self.client, self.report, self.module = client, report, module

    def check(self, name, passed, **details):
        self.report["checks"].append({"name": name, "passed": bool(passed), **details})
        if not passed:
            raise AssertionError(f"{name}: {details}")

    def load(self):
        result = self.client.upload(self.module)
        self.check("provider-module-load", result["error"] == "none", **result)

    def call(self, name, argument=None, retry_read=False):
        for attempt in range(4):
            token = self.client.action("call0", name)[0] if argument is None else self.client.action("call_i32", name, argument)[0]
            result = self.client.completion(token)
            if result["error"] != "resource_unavailable" or not retry_read or attempt == 3:
                break
            self.report.setdefault("read_admission_retries", 0)
            self.report["read_admission_retries"] += 1
            self.load()  # A failed native admission faults the module, including read queries.
        bits = self.client.action("result", token, 0) if result["error"] == "none" and result["count"] else None
        return result, bits

    def value(self, name, argument=None):
        result, bits = self.call(name, argument, name in ("frames", "pending", "ready", "time", "leader"))
        self.check(name, result["error"] == "none" and result["count"] == 1 and bits is not None, completion=result, bits=bits)
        return bits[1] | (bits[2] << 32)


def run_one(trial, cycles, validation_cycles=0):
    client = trial.client
    original = {key: client.get(key, LED) for key in ("enabled", "brightness", "red", "green", "blue", "white")}
    trial.report["original_led"] = original
    try:
        client.action("cancel_all")
        trial.check("initial-unload", client.work("unload")["error"] == "none")
        for key, value in dict(red=0, green=0, blue=0, white=0, brightness=24, enabled=True).items():
            client.request("set", LED, key, value)
        trial.load()
        for key in ("frames", "pending", "ready", "leader"):
            trial.value(key)
        before = client.get("applied_frames", LED)
        trial.check("fill-admitted", trial.value("fill", 16384) == 1)
        deadline = time.monotonic() + 2
        while client.get("applied_frames", LED) <= before and time.monotonic() < deadline:
            time.sleep(0.01)
        trial.check("script-render-completed", client.get("applied_frames", LED) > before)
        trial.check("fill-does-not-persist-base-color", all(client.get(key, LED) == 0 for key in ("red", "green", "blue", "white")))
        for name, argument in (("fill", 65536), ("bad_utf8", None), ("bad_nul", None), ("bad_bounds", None), ("trap", None)):
            scheduled = client.get("scheduled", FLEET)
            result, bits = trial.call(name, argument)
            expected = "verification_failed" if name == "trap" else "invalid_argument"
            trial.check(f"reject-{name}", result["error"] == expected and not result["count"] and bits is None and
                client.get("state") == "faulted" and client.get("scheduled", FLEET) == scheduled, completion=result)
            trial.load()
            trial.check("clear-after-fault", trial.value("clear") == 1)
        trial.report["validation_cycles"] = validation_cycles
        trial.report["validation_wall_overruns"] = []
        for index in range(validation_cycles):
            name = ("bad_utf8", "bad_nul", "bad_bounds")[index % 3]
            result, bits = trial.call(name)
            timing = {key: client.get(key) for key in ("native_last_us", "native_task_last_us")}
            trial.check("validation-status-under-radio-load", result["error"] == "invalid_argument" and
                        result["count"] == 0 and bits is None and timing["native_task_last_us"] <= 2000,
                        export=name, completion=result, **timing)
            if timing["native_last_us"] > 2000:
                trial.report["validation_wall_overruns"].append({"export": name, "completion": result, **timing})
            trial.load()
        client.request("set", "blip.wasm", "deadline_ms", 50)
        result, bits = trial.call("flood")
        trial.check("script-queue-overflow", result["error"] == "queue_full" and not result["count"] and bits is None and
            client.get("state") == "faulted", completion=result)
        trial.load(); trial.value("clear")
        client.request("set", "blip.wasm", "deadline_ms", 10)
        trial.check("unload-before-cycles", client.work("unload")["error"] == "none")
        idle = client.get("pool_used")
        for _ in range(cycles):
            trial.load(); trial.value("frames")
            trial.check("reload-unload", client.work("unload")["error"] == "none" and client.get("pool_used") == idle)
        trial.report["reload_cycles"] = cycles
        trial.report["metrics"] = {name: client.get(name) for name in (
            "native_calls", "native_failures", "native_maximum_us", "native_task_maximum_us", "buffer_reserved", "pool_reserved", "pool_used", "pool_peak",
            "worker_stack_headroom", "supervisor_stack_headroom")}
        trial.report["heap_free_internal"] = client.get("heap_free_internal", "blip.diagnostics")
        trial.report["heap_minimum_internal"] = client.get("heap_minimum_internal", "blip.diagnostics")
        trial.report["led_worker_headroom"] = client.get("worker_stack_headroom", LED)
        trial.check("bounded-worker-and-engine", trial.report["metrics"]["worker_stack_headroom"] >= 1024 and
            trial.report["metrics"]["pool_peak"] <= 81920 and trial.report["metrics"]["native_task_maximum_us"] <= 2000)
    finally:
        client.action("cancel_all")
        client.work("unload")
        client.request("action", LED, "blackout")
        for key, value in original.items():
            client.request("set", LED, key, value)
        trial.report["led_settings_restored"] = True


def artifacts(build):
    return {name: hashlib.sha256((build / name).read_bytes()).hexdigest()
            for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}


def run_pair(clients, report, module):
    """Admit real guest cues on a private routerless fleet, then retire memory."""
    originals = {}
    trials = {port: Trial(client, report, module) for port, client in clients.items()}
    check = next(iter(trials.values())).check
    try:
        for port, client in clients.items():
            originals[port] = {
                "fleet": {key: client.get(key, FLEET) for key in ("enabled", "fleet_id", "channel")},
                "led": {key: client.get(key, LED) for key in ("enabled", "brightness", "red", "green", "blue", "white")}}
            client.action("cancel_all"); client.work("unload")
            client.request("set", FLEET, "enabled", False)
        nodes = {port: client.get("node_id", FLEET) for port, client in clients.items()}
        leader = min(nodes, key=nodes.get)
        report["pair_nodes"], report["pair_leader"] = nodes, leader
        for client in clients.values():
            client.request("set", FLEET, "fleet_id", 0xB11F6501)
            client.request("set", FLEET, "channel", 1)
            client.request("set", FLEET, "enabled", True)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            states = {port: {key: client.get(key, FLEET) for key in ("active", "leader_id", "synchronized")}
                      for port, client in clients.items()}
            if all(state["active"] and state["synchronized"] and state["leader_id"] == nodes[leader]
                   for state in states.values()):
                break
            time.sleep(0.1)
        check("routerless-pair-consensus", all(state["active"] and state["synchronized"] and
              state["leader_id"] == nodes[leader] for state in states.values()), states=states)
        radio = {port: {key: client.get(key, "blip.transport.wifi") for key in
                       ("state", "autonomous_channel", "setup_ap_active")}
                 for port, client in clients.items()}
        check("pair-needs-no-router", all(state["state"] == 6 and state["autonomous_channel"] == 1 and
              not state["setup_ap_active"] for state in radio.values()), radio=radio)
        for port, trial in trials.items():
            trial.load()
            check("guest-fleet-ready", trial.value("ready") == 1, port=port)
            check("guest-fleet-leader", trial.value("leader") == nodes[leader], port=port)
            first, second = trial.value("time"), trial.value("time")
            check("guest-fleet-time-monotonic", 0 < first <= second, port=port, first=first, second=second)
            trial.client.work("unload")
        origin = trials[leader]
        for name, parameter, value in (("write", "brightness", 73), ("action", "red", 0)):
            before = {port: client.get("executed", FLEET) for port, client in clients.items()}
            failures = {port: client.get("execution_failed", FLEET) for port, client in clients.items()}
            for client in clients.values():
                client.request("set", LED, parameter, 24 if name == "write" else 32)
            origin.load()
            # No retries after a potentially effective native action.
            cue = origin.value(name)
            check("guest-cue-admitted", cue > 0, export=name, cue=cue)
            result, _ = origin.call("erase")
            check("queued-identifiers-overwritten", result["error"] == "none" and result["count"] == 0)
            check("origin-module-unloaded-before-cue", origin.client.work("unload")["error"] == "none")
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                observed = {port: client.get(parameter, LED) for port, client in clients.items()}
                executed = {port: client.get("executed", FLEET) for port, client in clients.items()}
                if all(observed[port] == value and executed[port] == before[port] + 1 for port in clients):
                    break
                time.sleep(0.02)
            check("copied-guest-cue-executes-on-both", all(observed[port] == value and
                  executed[port] == before[port] + 1 and client.get("execution_failed", FLEET) == failures[port]
                  for port, client in clients.items()), export=name, observed=observed, executed=executed)
    finally:
        errors = []
        for port, original in originals.items():
            client = clients[port]
            try:
                client.action("cancel_all"); client.work("unload")
                client.request("set", FLEET, "enabled", False)
                client.request("action", LED, "blackout")
                for key, value in original["led"].items(): client.request("set", LED, key, value)
                for key in ("fleet_id", "channel", "enabled"):
                    client.request("set", FLEET, key, original["fleet"][key])
                if any(client.get(key, LED) != value for key, value in original["led"].items()) or \
                   any(client.get(key, FLEET) != value for key, value in original["fleet"].items()):
                    errors.append(f"{port}: settings verification failed")
            except Exception as error:
                errors.append(f"{port}: {error}")
        report["pair_original_settings"] = originals
        check("pair-settings-restored", not errors, errors=errors)
    report["pair_native_timing"] = {port: {key: client.get(key) for key in
        ("native_calls", "native_failures", "native_maximum_us", "native_task_maximum_us")}
        for port, client in clients.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--validation-cycles", type=int, default=0)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--peer-port")
    parser.add_argument("--peer-board")
    parser.add_argument("--peer-build", type=Path)
    args = parser.parse_args()
    if args.peer_port and (not args.peer_board or not args.peer_build or args.peer_port == args.port):
        parser.error("a pair requires a distinct peer port, board and build")
    module = fixture()
    report = {"board": args.board, "port": args.port, "source_snapshot": snapshot(), "checks": [], "passed": False,
        "fixture_sha256": hashlib.sha256(module).hexdigest(), "pc_network_changed": False, "test_firmware_left_installed": True,
        "artifacts": artifacts(args.build)}
    client = Client(args.port)
    peer = None
    try:
        run_one(Trial(client, report, module), args.cycles, args.validation_cycles)
        if args.peer_port:
            peer = Client(args.peer_port)
            report["peer"] = {"port": args.peer_port, "board": args.peer_board, "artifacts": artifacts(args.peer_build)}
            run_pair({args.port: client, args.peer_port: peer}, report, module)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if peer: peer.connection.close()
        client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"board": args.board, "passed": True, "checks": len(report["checks"]), "metrics": report["metrics"]}))


if __name__ == "__main__":
    main()
