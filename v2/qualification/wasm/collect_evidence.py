"""Bind validated hardware results to archived images and build-size evidence."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from run_benchmark import validate


def read_json(path):
    data = path.read_bytes()
    return json.loads(data.decode("utf-16" if data.startswith(b"\xff\xfe") else "utf-8"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, action="append", required=True)
    parser.add_argument("--images", type=Path, default=Path("build/m6-wasm-images"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = {"schema_version": 1, "measured_at_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Standalone interpreter benchmark; production Gate D not qualified",
              "runtimes": {
                  "wamr": {"release": "WAMR-2.4.5", "commit": "25bd7eb63e828e4bd242cc9b38d260b4b31c6605"},
                  "wasm3": {"release": "v0.9.0", "commit": "0cd38327f0c721e75172f4f1eeb55854dc0517af"}},
              "boards": []}
    for path in args.report:
        report = read_json(path)
        if not report["restored"] or not report.get("restore_serial_check", {}).get("ok"):
            raise SystemExit(f"Restoration not verified: {path}")
        costs = {}
        for run in report["runs"]:
            validate(run["records"])
            start = next(r for r in run["records"] if r["type"] == "start")
            artifact = args.images / report["chip"] / start["engine"]
            if sha256(artifact / "blip-wasm-benchmark.bin") != run["image_sha256"]:
                raise SystemExit(f"Image does not match hardware evidence: {artifact}")
            size = read_json(artifact / "size.json")
            costs[start["engine"]] = {
                "app_bytes": (artifact / "blip-wasm-benchmark.bin").stat().st_size,
                "elf_sha256": sha256(artifact / "blip-wasm-benchmark.elf"),
                "map_sha256": sha256(artifact / "blip-wasm-benchmark.map"),
                "sdkconfig_sha256": sha256(artifact / "sdkconfig"),
                "size": size}
            del run["console"]
        if set(costs) != {"baseline", "wamr", "wasm3"}:
            raise SystemExit(f"Engine comparison incomplete: {path}")
        baseline = costs["baseline"]
        for cost in costs.values():
            cost["app_delta_from_baseline_bytes"] = cost["app_bytes"] - baseline["app_bytes"]
            base_regions = {r["name"]: r["used"] for r in baseline["size"]["layout"]}
            cost["region_deltas_from_baseline_bytes"] = {
                r["name"]: r["used"] - base_regions.get(r["name"], 0)
                for r in cost["size"]["layout"]}
        report["build_costs"] = costs
        output["boards"].append(report)
    if {b["chip"] for b in output["boards"]} != {"esp32", "esp32s3", "esp32c6"}:
        raise SystemExit("Three chip families are required")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(f"Bound nine hardware runs to their images: {args.output}")


if __name__ == "__main__":
    main()
