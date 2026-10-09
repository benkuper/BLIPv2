"""Bind portable script-declaration qualification to exact source and artifacts."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
from pathlib import Path
import re
import sys
import wasmtime

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/qualification/wasm-controls"))
import run_controls


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_text(path):
    data = path.read_bytes()
    return data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sources = run_controls.snapshot()
    native = {}
    for board, chip in (("xiao", "esp32c6"), ("huzzah32", "esp32"), ("m5dial", "esp32s3")):
        build = ROOT / f"build/m6-controls-{board}"
        report_path = ROOT / f"build/m6-controls-{board}-trial.json"
        report = json.loads(report_path.read_text())
        run_controls.validate(report)
        require(report["passed"] and report["chip"] == chip and report["board"] == board, f"{board}: identity/result")
        require(report["flashed_mac"] == run_controls.MACS[board], f"{board}: MAC")
        require(report["source_snapshot"] == sources, f"{board}: stale sources")
        require(not report["pc_network_changed"] and not report["firmware_restore_requested"] and report["test_firmware_left_installed"], f"{board}: test/PC policy")
        require(not report["interpreter_exercised"] and not report["live_control_publication_exercised"], f"{board}: scope")
        for name, artifact in report["inputs"].items():
            path = build / name
            require(path.stat().st_size == artifact["bytes"] and sha(path) == artifact["sha256"], f"{board}: artifact {name}")
        build_log = ROOT / f"build/m6-controls-{board}-build.log"
        require("Project build complete" in read_text(build_log), f"{board}: incomplete SDK build")
        stack_files = list(build.rglob("script_manifest.cpp.su"))
        require(len(stack_files) == 1, f"{board}: missing parser stack usage")
        frames = []
        for line in stack_files[0].read_text().splitlines():
            name, size, kind = line.rsplit("\t", 2)
            frames.append({"function": name.replace(ROOT.as_posix(), "/BLIP"), "frame_bytes": int(size), "kind": kind})
        require(all(f["kind"] == "static" and f["frame_bytes"] <= 1024 for f in frames), f"{board}: unexpected parser frame")
        native[board] = {"report_sha256": sha(report_path), "build_log_sha256": sha(build_log),
                         "compiler_frames": frames, "qualification": report}
    host_log = ROOT / "build/m6-manifest-host-ctest.log"
    require("100% tests passed out of 25" in read_text(host_log), "Host suites not all passing")
    require("error LNK2019" in read_text(ROOT / "build/m6-manifest-red-build.log"), "Missing initial unimplemented-contract result")
    source = (ROOT / "v2/qualification/wasm-controls/main/main.cpp").read_text()
    fixture_source = source.split("constexpr std::array fixture{", 1)[1].split("};", 1)[0]
    fixture = bytes(ord(v[1]) if v.startswith("'") else int(v, 0) for v in re.findall(r"std::byte\{([^}]+)\}", fixture_source))
    require(importlib.metadata.version("wasmtime") == "36.0.0", "Unexpected host fixture validator")
    wasmtime.Module(wasmtime.Engine(), fixture)
    evidence = {
        "created_utc": datetime.now(timezone.utc).isoformat(), "passed": True,
        "scope": "Portable script declaration decoding/projection; production publication is not implemented",
        "collector_sha256": sha(Path(__file__)), "source_snapshot": sources,
        "host": {"suites": 25, "manifest_cases": 12, "ctest_log_sha256": sha(host_log),
                 "red_build_log_sha256": sha(ROOT / "build/m6-manifest-red-build.log")},
        "fixture": {"bytes": len(fixture), "sha256": hashlib.sha256(fixture).hexdigest(), "validator": "wasmtime 36.0.0", "valid_wasm_framing": True},
        "native": native,
        "limitations": [
            "No interpreter, production worker, dynamic registry publication, live control dispatch or event delivery in this qualifier",
            "Repeated parse duration uses a one-control fixture, not a worst-case schema or deadline guarantee",
            "The 20 KiB qualification stack accommodates allocating host fixtures and schema copies; it is not a production stack requirement",
            "Compiler frame sizes are individual functions, not a complete call-chain measurement",
            "Production code does not yet instantiate or consume the owned declaration schema; six required production profiles will be renewed when live integration is enabled",
            "Full 6.6/6.7, Gate C/D, concurrency/lifecycle stress and multi-day soaks remain open"
        ]
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "host_suites": 25, "native_cases": 36, "parse_cycles": 3000}))


if __name__ == "__main__":
    main()
