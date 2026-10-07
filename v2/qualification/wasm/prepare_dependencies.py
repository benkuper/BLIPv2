"""Fetch immutable benchmark inputs into ignored build output."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3] / "build/wasm-deps"
dependencies = [
    ("wamr", "https://github.com/wasm-micro-runtime/wasm-micro-runtime.git", "WAMR-2.4.5", "25bd7eb63e828e4bd242cc9b38d260b4b31c6605"),
    ("wasm3", "https://github.com/wasm3/wasm3.git", "v0.9.0", "0cd38327f0c721e75172f4f1eeb55854dc0517af"),
]
for name, url, tag, commit in dependencies:
    path = root / name
    if not path.exists():
        subprocess.run(["git", "clone", "--depth", "1", "--branch", tag, url, str(path)], check=True)
    actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != commit:
        raise SystemExit(f"Wrong {name} checkout: {actual}; expected {commit}")
    if subprocess.check_output(["git", "-C", str(path), "status", "--porcelain"], text=True).strip():
        raise SystemExit(f"Benchmark dependency must be unmodified: {path}")
    print(f"{name}: {tag} {actual}")
