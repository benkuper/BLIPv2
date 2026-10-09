"""Check installed release identity over serial and device-served OSCQuery."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.request
from release_publish import firmware_metadata

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
from blip_wasm_hil import Client


def snapshot():
    names = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "--",
                                     "v2/components", "v2/firmware", "v2/tools/ota"], cwd=ROOT, text=True).splitlines()
    return {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sorted(set(names))
            if not Path(name).name.startswith("collect_") and Path(name).suffix != ".md"}


def find_component(node, name):
    if node.get("BLIP_COMPONENT_ID") == name:
        return node
    for child in node.get("CONTENTS", {}).values():
        result = find_component(child, name)
        if result is not None:
            return result
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--flash-log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    metadata, code, version = firmware_metadata(args.build / "blip-v2.bin")
    raw_log = args.flash_log.read_bytes()
    flash = raw_log.decode("utf-16" if raw_log.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
    macs = re.findall(r"MAC:\s*([0-9a-f:]+)", flash, re.I)
    if not macs or macs[-1].lower() != args.mac.lower():
        parser.error("Flashed MAC differs from expected board")
    expected = {key: metadata[key] for key in ("board", "target", "layout", "profile", "features", "flash_bytes")}
    expected.update(release_code=code, version=version)
    report = {"passed": False, "board": metadata["board"], "port": args.port, "checks": [],
              "source_snapshot": snapshot(), "pc_network_changed": False,
              "flash_log_sha256": hashlib.sha256(raw_log).hexdigest(),
              "artifacts": {name: hashlib.sha256((args.build / name).read_bytes()).hexdigest()
                            for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}}
    client = None
    try:
        client = Client(args.port)
        for name, value in expected.items():
            actual = client.get(name, "blip.ota")
            report["checks"].append({"name": "serial-" + name, "passed": actual == value, "actual": actual, "expected": value})
            if actual != value:
                raise AssertionError("Installed serial identity mismatch")
        request = urllib.request.Request(args.device.rstrip("/") + "/?config=1", headers={"Accept": "application/json"})
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(request, timeout=10) as response:
            tree = json.load(response)
        component = find_component(tree, "blip.ota")
        if component is None:
            raise AssertionError("OTA component missing from OSCQuery")
        for name, value in expected.items():
            actual = component["CONTENTS"][name]["VALUE"][0]
            report["checks"].append({"name": "oscquery-" + name, "passed": actual == value, "actual": actual, "expected": value})
            if actual != value:
                raise AssertionError("Installed OSCQuery identity mismatch")
        report["heap_free_internal"] = client.get("heap_free_internal", "blip.diagnostics")
        report["heap_largest_internal"] = client.get("heap_largest_internal", "blip.diagnostics")
        report["passed"] = True
    except Exception as error:
        report["failure"] = str(error)
    finally:
        if client:
            client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]), "failure": report.get("failure")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
