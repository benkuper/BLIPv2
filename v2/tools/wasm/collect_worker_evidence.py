"""Collect native worker lifecycle evidence for the three supported chip families."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/qualification/wasm-worker"))
from run_worker import PIN, source_digest, validate

BOARDS = {
    "huzzah32": ("adafruit-huzzah32", "esp32", "30:ae:a4:f2:d1:84"),
    "m5dial": ("m5stack-m5dial", "esp32s3", "b0:81:84:96:83:74"),
    "xiao": ("seeed-xiao-esp32c6-chip-antenna", "esp32c6", "10:51:db:1a:a6:68"),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-prefix", type=Path, default=Path("build/m6-worker"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    expected_source = source_digest()
    dependency = ROOT / "build/wasm-deps/wamr"
    revision = subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(dependency), "status", "--porcelain"], text=True).strip()
    if revision != PIN or dirty:
        raise RuntimeError("WAMR pin/cleanliness differs")
    trials = []
    for key, (board, chip, mac) in BOARDS.items():
        build = Path(f"{args.build_prefix}-{key}")
        trial_path = Path(f"{build}-trial.json")
        trial = json.loads(trial_path.read_text(encoding="utf-8"))
        validate(trial)
        if not trial["passed"] or trial["source_sha256"] != expected_source or trial["wamr_commit"] != revision:
            raise RuntimeError(f"Trial source/runtime differs: {key}")
        displayed = trial["mac"].split(":")
        if len(displayed) == 8 and displayed[3:5] == ["ff", "fe"]:
            displayed = displayed[:3] + displayed[5:]
        if (trial["board"], trial["chip"], ":".join(displayed)) != (board, chip, mac):
            raise RuntimeError(f"Unexpected physical device: {key}")
        if trial["pc_network_changed"] or trial["restore_requested"] or not trial["test_firmware_left_installed"]:
            raise RuntimeError("Unexpected network/restore action")
        for name, expected in trial["inputs"].items():
            artifact = build / name
            if artifact.stat().st_size != expected["bytes"] or sha(artifact) != expected["sha256"]:
                raise RuntimeError(f"Artifact differs: {artifact}")
        config = (build / "sdkconfig").read_text(encoding="utf-8-sig")
        if f"CONFIG_IDF_TARGET=\"{chip}\"" not in config or "CONFIG_FREERTOS_HZ=1000" not in config or "CONFIG_SPIRAM=y" in config:
            raise RuntimeError(f"Unexpected qualification configuration: {key}")
        if chip == "esp32" and not all(f"{setting}=y" in config for setting in ("CONFIG_FREERTOS_UNICORE", "CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY")):
            raise RuntimeError("ESP32 production memory settings missing")
        trials.append({"profile": key, "mac": mac, "trial_sha256": sha(trial_path), "trial": trial})
    evidence = {
        "schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": True,
        "result": "production native worker lifecycle and injected startup failure cleanup passed on three chip families; full Gate D remains open",
        "source_sha256": expected_source, "runtime": {"commit": revision, "checkout_unmodified": True},
        "checks": sum(next(r for r in t["trial"]["records"] if r["type"] == "complete")["checks"] for t in trials),
        "firmware_restore_requested": False, "trials": trials,
        "limitations": [
            "A minimal ESP-IDF application links the production component/adapter; it does not exercise production Wi-Fi, BLE, fleet, lighting or settings tasks concurrently.",
            "Failures are injected once at the real buffer/SDK allocation or task creation boundary after warmup; global heap exhaustion, cold low-memory lazy mutex/TLS allocation and fragmented heap qualification remain open.",
            "The backend initialization failure is explicit test-runtime rejection before engine startup; it does not establish recovery from every internal WAMR allocation stage.",
            "Twenty native task restart cycles per family are a bounded lifecycle trial, not a long soak or a real-time shutdown guarantee.",
            "The M5StickC serial burst loss, active BLE/fleet coexistence and physical LED timing remain separate open gaps.",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "checks": evidence["checks"], "families": len(trials), "passed": True}))


if __name__ == "__main__":
    main()
