"""Collect bounded release-contract qualification; no device worker claims."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/wasm"))
from collect_profile_evidence import PROFILES, read_text, app_partition_bytes


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    host = ROOT / "build/m38-catalog-host-tests.log"
    server = ROOT / "build/m38-release-server-tests.log"
    require("100% tests passed out of 29" in read_text(host), "host qualification")
    require("Ran 5 tests" in read_text(server) and "\nOK" in read_text(server) and
            "C++ device parser accepted the HTTP catalog response" in read_text(server), "server/device qualification")
    builds = []
    for name, (board, target, _, _) in PROFILES.items():
        directory = ROOT / f"build/m6-production-{name}"
        log = ROOT / f"build/m38-catalog-{name}-build.log"
        compiled = directory / "esp-idf/blip_ota/CMakeFiles/__idf_blip_ota.dir/src/release_catalog.cpp.obj"
        require("Project build complete" in read_text(log) and compiled.is_file(), f"{name}: target parser build")
        size = (directory / "blip-v2.bin").stat().st_size
        require(size <= app_partition_bytes(directory), f"{name}: app partition")
        builds.append({"profile": name, "board": board, "target": target, "app_bytes": size,
                       "app_partition_bytes": app_partition_bytes(directory), "build_log_sha256": digest(log),
                       "compiled_parser_sha256": digest(compiled), "application_sha256": digest(directory / "blip-v2.bin")})
    sources = ["v2/components/blip_ota/include/blip/ota/release_catalog.hpp",
               "v2/components/blip_ota/src/release_catalog.cpp", "v2/components/blip_ota/CMakeLists.txt",
               "v2/tests/host/blip_release_catalog_tests.cpp", "v2/tests/host/CMakeLists.txt",
               "v2/tools/ota/release_server.py", "v2/tools/ota/test_release_server.py",
               "v2/tools/ota/collect_catalog_evidence.py"]
    result = {"recorded_at": datetime.now(timezone.utc).isoformat(), "contract": "release-catalog-v1",
              "passed": True, "host_suites": 29, "server_tests": 5, "cross_language_http_response_checked": True,
              "sources": {name: digest(ROOT / name) for name in sources}, "builds": builds,
              "test_logs": {str(path.relative_to(ROOT)).replace('\\', '/'): digest(path) for path in (host, server)},
              "device_https_worker_qualified": False, "device_installation_qualified": False,
              "goldengeek_catalog_deployed": False, "pc_network_changed": False,
              "remaining": ["embedded firmware identity", "native HTTPS checks/downloads", "update center",
                            "automatic policy", "release publication", "corruption/interruption/rollback HIL"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "profiles": len(builds), "host_suites": 29, "server_tests": 5}))


if __name__ == "__main__":
    main()
