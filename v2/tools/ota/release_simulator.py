"""Run the public release handler locally on the PC's existing network."""
import argparse
from http.server import HTTPServer
import json
from pathlib import Path
from release_server import handler, load_index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path("build/release-simulator"))
    parser.add_argument("--listen", default="127.0.0.1", help="Existing LAN IP for device tests; never changes Wi-Fi")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    catalog = args.directory / "releases.json"
    if not catalog.exists(): catalog.write_text(json.dumps({"schema": 1, "releases": []}) + "\n", encoding="utf-8")
    artifacts = args.directory / "artifacts"
    artifacts.mkdir(exist_ok=True)
    load_index(catalog)
    print(f"Public local endpoint: http://{args.listen}:{args.port}/blip/update", flush=True)
    if args.prepare_only: return
    server = HTTPServer((args.listen, args.port), handler(catalog, artifacts))
    server.serve_forever()


if __name__ == "__main__": main()
