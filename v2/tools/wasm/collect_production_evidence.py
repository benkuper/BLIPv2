#!/usr/bin/env python3
"""Validate the Ball production worker report and bind it to build artifacts."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_hil import source_snapshot


def read_json(path):
    data = path.read_bytes()
    return json.loads(data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--size", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = read_json(args.report)
    assert report["board"] == "creators-ball-v2" and report["passed"]
    assert len(report["checks"]) >= 124 and all(item["passed"] for item in report["checks"])
    assert report["source_snapshot"] == source_snapshot(ROOT), "report source changed"
    for name, expected in report["artifacts"].items():
        assert digest(args.build / name) == expected, f"artifact changed: {name}"
    metrics = report["metrics"]
    assert metrics["pool_reserved"] == 81920 and metrics["pool_used"] == 0
    assert 0 < metrics["pool_peak"] <= metrics["pool_reserved"]
    assert metrics["worker_stack_headroom"] >= 2048 and metrics["supervisor_stack_headroom"] >= 1024
    coex = report["coexistence"]
    assert coex["passed"] and coex["settings_writes"] == 20 and coex["settings_restored"]
    assert len(coex["script_calls"]) == 20
    assert all(item["error"] == "budget_exceeded" and item["count"] == 0 for item in coex["script_calls"])
    assert coex["heap_minimum"] >= 8192
    assert coex["worker_state_after"] == "ready"
    assert coex["worker_stack_headroom_after"] >= 2048
    assert coex["supervisor_stack_headroom_after"] >= 1024
    assert report["host_network"]["independent_recovery_armed"]
    assert report["host_network"]["original_wifi_and_internet_restored"]
    assert report["host_network"]["temporary_profile_removed"]
    cache = (args.build / "CMakeCache.txt").read_text(encoding="utf-8")
    enabled = ("WASM", "BLE", "ESPNOW", "FLEET", "DDP", "ARTNET")
    assert all(f"BLIP_ENABLE_{name}:BOOL=ON" in cache for name in enabled)
    engine = ROOT / "build/wasm-deps/wamr"
    revision = subprocess.check_output(["git", "-C", str(engine), "rev-parse", "HEAD"], text=True).strip()
    assert revision == "25bd7eb63e828e4bd242cc9b38d260b4b31c6605"
    assert not subprocess.check_output(["git", "-C", str(engine), "status", "--porcelain"], text=True).strip()
    linked = read_json(args.size)
    baseline_evidence = read_json(ROOT / "docs/v2/evidence/fleet/2026-10-01-seven-board-build.json")
    baseline = next(item for item in baseline_evidence["boards"] if item["profile"] == "ball")
    app_bytes = (args.build / "blip-v2.bin").stat().st_size
    assert app_bytes <= baseline["partition_bytes"]
    evidence = {
        "schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "result": "one-board production worker and coexistence passed; full Gate D qualification open",
        "runtime": {"name": "WAMR", "commit": revision, "checkout_unmodified": True},
        "profile": {"board": "creators-ball-v2", "target": "esp32c6", "enabled_features": enabled,
                    "app_bytes": app_bytes, "app_partition_bytes": baseline["partition_bytes"]},
        "fixed_costs": {"engine_pool_bytes": 81920, "module_buffer_bytes": 16384,
                        "wasm_stack_bytes_inside_pool": 4096, "worker_native_stack_bytes": 8192,
                        "supervisor_native_stack_bytes": 4096, "worker_priority": 2,
                        "supervisor_priority": 6, "affinity": "none", "queue_capacity": 8,
                        "completion_capacity": 16},
        "linked_size": linked,
        "comparison": {"prior_ball_source_commit": baseline_evidence["source_commit"],
                       "prior_app_bytes": baseline["app_bytes"], "prior_app_sha256": baseline["sha256"],
                       "app_delta_bytes": app_bytes - baseline["app_bytes"],
                       "note": "Profile comparison includes the new Wi-Fi RAM and tick settings; not an isolated engine delta."},
        "trial": report,
        "limitations": [
            "Only the Ball C6 production profile is qualified here; six remaining board profiles are open.",
            "Native component task stop/restart and allocation-failure lifecycle qualification are open.",
            "Network and LED counters establish operation, not physical LED waveform timing or long-term RF performance.",
            "Deadline failure is enforced; low worker priority and SDK/cache work can delay completion beyond the requested deadline.",
            "BLE advertises and fleet is present; active BLE/fleet traffic was not injected in this trial.",
            "Modules are volatile; provider imports, checked string ABI, registry script declarations and generated SDK remain open."
        ]
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "checks": len(report["checks"]), "output": str(args.output)}))


if __name__ == "__main__":
    main()
