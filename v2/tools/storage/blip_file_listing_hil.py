"""Qualify bounded logical directory pages on a BLIP's existing LAN."""
import argparse
from datetime import datetime, timezone
import hashlib
from http.client import HTTPConnection
import json
from pathlib import Path
import re
import sys
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "v2/tools/control"))
sys.path.insert(0, str(ROOT / "v2/tools/ota"))
from blip_wasm_hil import Client
from blip_release_identity_hil import snapshot
from release_publish import firmware_metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("device", "port", "mac", "expected-medium"):
        parser.add_argument("--" + key, required=True)
    for key in ("build", "flash-log", "report"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    url = urlsplit(args.device)
    if url.scheme != "http" or not url.hostname or url.path not in ("", "/") or url.query:
        parser.error("device must be an HTTP origin")
    identity, code, version = firmware_metadata(args.build / "blip-v2.bin")
    raw = args.flash_log.read_bytes()
    log = raw.decode("utf-16" if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else "utf-8-sig")
    if args.mac.lower() not in re.findall(r"mac:\s*([0-9a-f:]+)", log.lower()):
        parser.error("flashed MAC differs")
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {"passed": False, "created_at": datetime.now(timezone.utc).isoformat(), "checks": [],
              "board": identity["board"], "port": args.port, "mac": args.mac, "firmware_code": code,
              "firmware_version": version, "pc_network_changed": False, "source_snapshot": snapshot(),
              "tool_sha256": sha(Path(__file__)), "flash_log_sha256": sha(args.flash_log),
              "artifacts": {name: sha(args.build / name) for name in ("blip-v2.bin", "blip-v2.elf", "blip-v2.map", "sdkconfig")}}
    client = http = None
    original_interval = None
    paths = []
    tag = f"zzlist_{time.monotonic_ns():x}"
    def check(name, condition):
        report["checks"].append({"name": name, "passed": bool(condition)})
        if not condition: raise AssertionError(name)
    def request(method, path, body=None):
        http.request(method, "/api/files/" + path, body=body, headers={"Content-Type": "application/octet-stream"})
        response = http.getresponse()
        return response.status, response.read(131073)
    def store(path, data=b"payload"):
        paths.append(path)
        check("store-" + path, request("PUT", path, data)[0] == 201)
    def listing(directory, after=""):
        status, raw = request("GET", directory + "/" + ("?after=" + after if after else ""))
        check("listing-http", status == 200 and len(raw) < 4096)
        page = json.loads(raw)
        check("listing-schema", page["schema"] == 1 and page["directory"] == directory and len(page["files"]) <= 4)
        check("cursor-advances", not page["cursor"] or page["cursor"] > after)
        check("bounded-page-progress", not page["more"] or bool(page["cursor"]))
        for entry in page["files"]:
            check("logical-child-only", entry["path"].startswith(directory + "/") and
                  "/" not in entry["path"][len(directory) + 1:] and not entry["path"].endswith((".b0", ".b1")))
        return page
    def all_files(directory):
        cursor = ""; result = []; page_count = 0
        while True:
            page = listing(directory, cursor); result.extend(page["files"]); page_count += 1
            if not page["more"]: break
            check("bounded-fixture-pages", page_count < 20)
            cursor = page["cursor"]
        names = [entry["path"] for entry in result]
        check("sorted-unique-pages", names == sorted(set(names)))
        return {entry["path"]: entry for entry in result}, page_count
    try:
        client = Client(args.port)
        check("board-identity", client.get("board", "blip.ota") == identity["board"])
        check("release-identity", client.get("release_code", "blip.ota") == code)
        check("network-identity", client.get("ip_address", "blip.transport.wifi") == url.hostname)
        report["medium"] = client.get("preferred_medium", "blip.storage.files.internal")
        check("automatic-medium", report["medium"] == args.expected_medium)
        original_interval = client.get("interval_hours", "blip.updates")
        client.request("set", "blip.updates", "interval_hours", 0)
        http = HTTPConnection(url.hostname, url.port or 80, timeout=60)
        report["namespaces"] = []
        for namespace in ("scripts", "playback", "sequences"):
            directory = namespace + "/" + tag
            expected = {directory + "/" + name for name in ("a", "b", "c", "d", "e", "f", "g", "h", "i")}
            for path in sorted(expected): store(path)
            store(directory + "/folder/child", b"nested")
            store(directory + "/folder", b"same name")
            store(directory + "/empty", b"")
            store(directory + "/deleted", b"old")
            check("delete-before-list", request("DELETE", directory + "/deleted")[0] == 200)
            entries, count = all_files(directory)
            check("all-files-across-pages", set(entries) == expected | {directory + "/folder", directory + "/empty"} and count >= 3)
            check("combined-folder-and-file", entries[directory + "/folder"]["directory"] and entries[directory + "/folder"]["file"])
            check("medium-attribution", all(entry["external"] == (args.expected_medium != "internal") and not entry["unreadable"] for entry in entries.values()))
            children, _ = all_files(directory + "/folder")
            check("nested-folder", set(children) == {directory + "/folder/child"})
            check("empty-file-not-deleted", request("GET", directory + "/empty") == (200, b""))
            check("delete-combined-file", request("DELETE", directory + "/folder")[0] == 200)
            entries, _ = all_files(directory)
            check("deletion-keeps-folder", entries[directory + "/folder"]["directory"] and not entries[directory + "/folder"]["file"])
            check("deleted-file-hidden", directory + "/deleted" not in entries)
            check("checked-download", request("GET", directory + "/a") == (200, b"payload"))
            check("replace-listed-file", request("PUT", directory + "/a", b"replaced")[0] == 201)
            check("checked-replacement", request("GET", directory + "/a") == (200, b"replaced"))
            report["namespaces"].append({"name": namespace, "pages": count, "items": len(entries)})
        check("missing-folder-empty", listing("scripts/" + tag + "_missing")["files"] == [])
        for path in ("web/", "scripts/../", "scripts/?after=a/b", "scripts/?after=a&x=1", "scripts/?bad=x", "scripts/?after=a%2Fb"):
            check("reject-directory-cursor-" + path, request("GET", path)[0] == 400)
        report["http_stack_headroom"] = client.get("http_stack_headroom", "blip.oscquery")
        check("http-stack-headroom", report["http_stack_headroom"] >= 1024)
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if http:
            for path in paths:
                try: request("DELETE", path); request("DELETE", path)
                except Exception as error: report["cleanup_error"] = str(error); report["passed"] = False
            http.close()
        if client:
            if original_interval is not None:
                try: client.request("set", "blip.updates", "interval_hours", original_interval)
                except Exception as error: report["policy_restore_error"] = str(error); report["passed"] = False
            client.connection.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]), "report": str(args.report), "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__": raise SystemExit(main())
