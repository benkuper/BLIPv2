"""Compress the small firmware-resident first-run page deterministically."""
import argparse
from pathlib import Path
from pack_web_assets import deterministic_gzip

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = args.source.read_bytes()
compressed = deterministic_gzip(source)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(compressed)
print(f"First-run page: {len(source)} bytes -> {len(compressed)} bytes")
