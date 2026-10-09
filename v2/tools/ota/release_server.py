"""Public HTTP release handler, with optional TLS, for catalogs and artifacts."""
from __future__ import annotations
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import ssl
import time
from urllib.parse import parse_qsl, urlsplit, unquote

QUERY_FIELDS = {"schema", "project", "board", "target", "layout", "profile", "channel",
                "flash_bytes", "features", "api", "fw_code", "fw_version", "web_code"}
IDENTITY_FIELDS = QUERY_FIELDS - {"schema", "fw_code", "fw_version", "web_code"}
NUMBERS = {"flash_bytes", "features", "api", "fw_code", "web_code", "schema"}
TEXT_CAPACITY = {"project": 31, "board": 63, "target": 15, "layout": 31, "profile": 31, "channel": 15, "fw_version": 31}
ARTIFACT_FIELDS = {"code", "version", "bytes", "url", "sha256", "minimum_other_code"}


def uint32(value):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise ValueError("invalid unsigned field")
    return value


def ascii_text(value, maximum):
    if not isinstance(value, str) or not 0 < len(value) <= maximum or any(not 32 <= ord(c) < 127 for c in value):
        raise ValueError("invalid bounded text")
    return value


def release_url(value):
    ascii_text(value, 319)
    parts = urlsplit(value)
    if (parts.scheme not in ("http", "https") or not parts.hostname or parts.username is not None or parts.password is not None
            or parts.fragment or any(c in value for c in " @#\\") or re.search(r"%(?![0-9a-fA-F]{2})", value)):
        raise ValueError("invalid HTTP(S) artifact URL")
    if parts.port is not None and not 1 <= parts.port <= 65535:
        raise ValueError("invalid HTTP(S) port")
    if len(parts.hostname) > 253 or any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label)
                                      for label in parts.hostname.split(".")):
        raise ValueError("invalid HTTP(S) hostname")
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate catalog field")
        result[key] = value
    return result


def query_identity(query):
    if len(query) > 1024 or re.search(r"%(?![0-9a-fA-F]{2})", query):
        raise ValueError("update query too large")
    pairs = parse_qsl(query, keep_blank_values=True, strict_parsing=True, encoding="utf-8", errors="strict", max_num_fields=13)
    values = unique_object(pairs)
    if values.keys() != QUERY_FIELDS:
        raise ValueError("incomplete update query")
    for key in NUMBERS:
        if not re.fullmatch(r"0|[1-9][0-9]*", values[key]):
            raise ValueError("invalid unsigned query")
        values[key] = uint32(int(values[key]))
    for key, maximum in TEXT_CAPACITY.items():
        ascii_text(values[key], maximum)
    if values["schema"] != 1 or not values["api"] or not values["flash_bytes"]:
        raise ValueError("unsupported query schema/identity")
    return values


def validate_release(release):
    if not isinstance(release, dict) or release.keys() != IDENTITY_FIELDS | {"firmware", "web"}:
        raise ValueError("invalid release fields")
    for key in IDENTITY_FIELDS:
        if key in NUMBERS:
            uint32(release[key])
        else:
            ascii_text(release[key], TEXT_CAPACITY[key])
    if not release["api"] or not release["flash_bytes"]:
        raise ValueError("invalid release API/flash")
    for kind in ("firmware", "web"):
        artifact = release[kind]
        if artifact is None:
            continue
        if not isinstance(artifact, dict) or artifact.keys() != ARTIFACT_FIELDS:
            raise ValueError("invalid artifact fields")
        for key in ("code", "bytes", "minimum_other_code"):
            uint32(artifact[key])
        if not artifact["code"] or not (492 if kind == "firmware" else 48) <= artifact["bytes"] <= (8 * 1024 * 1024 if kind == "firmware" else 256 * 1024):
            raise ValueError("invalid artifact code/size")
        ascii_text(artifact["version"], 31)
        release_url(artifact["url"])
        if not isinstance(artifact["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]) or artifact["sha256"] == "0" * 64:
            raise ValueError("invalid artifact SHA-256")
    encoded = json.dumps({"schema": 1, **release}, separators=(",", ":"), ensure_ascii=True).encode()
    if len(encoded) > 4096:
        raise ValueError("release exceeds device catalog bound")


class ReleaseIndex:
    def __init__(self, source):
        if not isinstance(source, dict) or source.keys() != {"schema", "releases"} or type(source["schema"]) is not int or source["schema"] != 1 or not isinstance(source["releases"], list):
            raise ValueError("invalid release index")
        self.releases = {}
        self.artifact_paths = set()
        for release in source["releases"]:
            validate_release(release)
            key = self.key(release)
            if key in self.releases:
                raise ValueError("duplicate release identity")
            self.releases[key] = release
            for kind in ("firmware", "web"):
                if release[kind] is not None:
                    self.artifact_paths.add(urlsplit(release[kind]["url"]).path)

    @staticmethod
    def key(values):
        return tuple(values[key] for key in sorted(IDENTITY_FIELDS))

    def answer(self, query):
        values = query_identity(query)
        release = self.releases.get(self.key(values))
        result = {"schema": 1, **{key: values[key] for key in sorted(IDENTITY_FIELDS)}, "firmware": None, "web": None}
        if release is not None:
            result.update(firmware=release["firmware"], web=release["web"])
        return result


def load_index(path):
    with path.open("rb") as stream:
        source = stream.read(4 * 1024 * 1024 + 1)
    if len(source) > 4 * 1024 * 1024:
        raise ValueError("release index too large")
    return ReleaseIndex(json.loads(source, object_pairs_hook=unique_object))


def handler(index_path, artifact_root, artifact_bytes_per_second=0):
    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            self.request.settimeout(10)
            self.response_started = False
            super().setup()

        def send_response(self, code, message=None):
            self.response_started = True
            super().send_response(code, message)

        def log_message(self, *_):
            pass  # Do not log query values or credentials supplied by a caller.

        def json_response(self, status, value):
            body = json.dumps(value, separators=(",", ":"), ensure_ascii=True).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            try:
                index = load_index(index_path)
                url = urlsplit(self.path)
                if url.path in ("/blip/update", "/blip/update/"):
                    try:
                        answer = index.answer(url.query)
                    except ValueError:
                        self.json_response(400, {"error": "invalid update query"})
                        return
                    self.json_response(200, answer)
                    return
                if artifact_root is None or url.query or url.path not in index.artifact_paths or not url.path.startswith("/blip/releases/"):
                    self.json_response(404, {"error": "not found"})
                    return
                relative = unquote(url.path[len("/blip/releases/"):], errors="strict")
                if any(not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", part) or part in (".", "..") for part in relative.split("/")):
                    self.json_response(404, {"error": "not found"})
                    return
                root = artifact_root.resolve()
                file = (root / relative).resolve()
                if not file.is_relative_to(root) or not file.is_file():
                    self.json_response(404, {"error": "not found"})
                    return
                with file.open("rb") as stream:
                    # Firmware flash writes and TCP recovery can outlast the
                    # short request-header timeout. Match the device's bounded
                    # transfer lifetime while keeping each write bounded.
                    self.connection.settimeout(60)
                    deadline = time.monotonic() + 600
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(file.stat().st_size))
                    self.end_headers()
                    while block := stream.read(4096):
                        if time.monotonic() >= deadline:
                            self.close_connection = True
                            return
                        self.wfile.write(block)
                        if artifact_bytes_per_second:
                            self.wfile.flush()
                            time.sleep(len(block) / artifact_bytes_per_second)
            except (OSError, ValueError, json.JSONDecodeError):
                if self.response_started:
                    self.close_connection = True
                else:
                    self.json_response(503, {"error": "release catalog unavailable"})
    return Handler


def create_server(address, index_path, artifact_root, context=None, *, artifact_bytes_per_second=0):
    if artifact_bytes_per_second < 0:
        raise ValueError("artifact rate cannot be negative")
    # Flash writes can make a device consume its artifact slowly. Other devices
    # must still be able to check their catalogs and start downloads.
    class ReleaseServer(ThreadingHTTPServer):
        def get_request(self):
            connection, peer = super().get_request()
            connection.settimeout(10)
            if context is None: return connection, peer
            try:
                return context.wrap_socket(connection, server_side=True), peer
            except Exception:
                connection.close()
                raise
    return ReleaseServer(address, handler(index_path, artifact_root, artifact_bytes_per_second))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--cert", type=Path, help="Optional TLS certificate; requires --key")
    parser.add_argument("--key", type=Path, help="Optional TLS server key; requires --cert")
    args = parser.parse_args()
    load_index(args.catalog)
    if bool(args.cert) != bool(args.key): parser.error("--cert and --key must be supplied together")
    context = None
    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
    server = create_server((args.listen, args.port), args.catalog, args.artifacts, context)
    print(f"Release endpoint listening on {args.listen}:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
