"""Bind the three service qualifications to the current sources and images."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, action="append", required=True)
    parser.add_argument("--build", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if len(args.report) != 3 or len(args.build) != 3:
        parser.error("Supply three paired --report/--build arguments")
    repo = Path(__file__).resolve().parents[3]
    root = repo / "v2/qualification/wasm-service"
    digest = hashlib.sha256()
    sources = list(root.rglob("*")) + list((repo / "v2/components/blip_wasm").rglob("*"))
    sources.append(root.parent / "wasm/run_benchmark.py")
    for source in sorted(p for p in sources if p.is_file() and "__pycache__" not in p.parts):
        digest.update(source.relative_to(repo).as_posix().encode())
        digest.update(b"\0")
        digest.update(source.read_bytes())
    source_hash = digest.hexdigest()
    fixture_hash = re.search(r'workloads_sha256\[\] = "([a-f0-9]{64})"', (root / "main/fixtures.hpp").read_text(encoding="utf-8")).group(1)
    checkout = repo / "build/wasm-deps/wamr"
    commit = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
    if commit != "25bd7eb63e828e4bd242cc9b38d260b4b31c6605" or subprocess.check_output(["git", "-C", str(checkout), "status", "--porcelain"], text=True).strip():
        raise SystemExit("Wrong or modified WAMR checkout")
    result = {"schema_version": 1, "measured_at_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Actual portable service and WAMR adapter; standalone tasks, production integration/Gate D open",
              "source_sha256": source_hash, "boards": []}
    for report_path, build in zip(args.report, args.build):
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if not report.get("passed") or report["source_sha256"] != source_hash or report["wamr_commit"] != commit or report["pc_network_changed"]:
            raise SystemExit(f"Unverified or mismatched trial: {report_path}")
        if report["restore_requested"] and not (report["restored"] and report.get("restore_serial_check", {}).get("ok")):
            raise SystemExit(f"Requested restore not verified: {report_path}")
        if any(r["type"] not in {"start", "complete"} for r in report["records"]):
            raise SystemExit(f"Failure/repeated trial in output: {report_path}")
        starts = [r for r in report["records"] if r["type"] == "start"]
        completions = [r for r in report["records"] if r["type"] == "complete"]
        if len(starts) != 1 or len(completions) != 1 or starts[0]["chip"] != report["chip"] or starts[0]["fixture_sha256"] != fixture_hash:
            raise SystemExit(f"Incomplete/mismatched output: {report_path}")
        done = completions[0]
        if done["checks"] < 390 or done["failures"] or done["load_unload_cycles"] != 100 or done["start_stop_cycles"] != 20 or done["cancel_requests"] < 1:
            raise SystemExit(f"Failed/incomplete checks: {report_path}")
        if done["heap_after_warmup"] != done["heap_min"] or done["heap_min"] != done["heap_max"]:
            raise SystemExit(f"Growing heap: {report_path}")
        for name, metadata in report["inputs"].items():
            path = build / name
            if sha(path) != metadata["sha256"] or path.stat().st_size != metadata["bytes"]:
                raise SystemExit(f"Build input mismatch: {path}")
        report["build"] = build.as_posix()
        del report["console"]
        result["boards"].append(report)
    if {b["chip"] for b in result["boards"]} != {"esp32", "esp32s3", "esp32c6"}:
        raise SystemExit("Three distinct chip families required")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated and bound three service trials: {args.output}")


if __name__ == "__main__":
    main()
