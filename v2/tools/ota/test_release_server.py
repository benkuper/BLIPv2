import copy
import json
import unittest
import os
import subprocess
import tempfile
import threading
import socket
from http.server import HTTPServer
from pathlib import Path
from urllib.request import urlopen
from urllib.error import HTTPError
from urllib.parse import urlencode
from release_server import ReleaseIndex, unique_object, release_url, handler, create_server

IDENTITY = {"project": "blip-v2", "board": "creators-ball-v2", "target": "esp32c6", "layout": "ota-8mb-v1",
            "profile": "minimal", "channel": "stable", "flash_bytes": 8388608, "features": 123, "api": 1}
ARTIFACT = {"code": 2, "version": "0.2.0-alpha", "bytes": 1000000, "url": "https://www.goldengeek.org/blip/releases/app.bin",
            "sha256": "0123456789abcdef" * 4, "minimum_other_code": 2000}
QUERY = {"schema": 1, **IDENTITY, "fw_code": 1, "fw_version": "0.1.0", "web_code": 2000}


class ReleaseServerTests(unittest.TestCase):
    def index(self, **changes):
        return ReleaseIndex({"schema": 1, "releases": [{**IDENTITY, "firmware": copy.deepcopy(ARTIFACT), "web": None, **changes}]})

    def test_exact_identity_and_independent_artifacts(self):
        index = self.index()
        response = index.answer(urlencode(QUERY))
        self.assertEqual(response["firmware"], ARTIFACT)
        self.assertIsNone(response["web"])
        for key, value in (("board", "another-c6"), ("features", 122), ("api", 2), ("flash_bytes", 4194304), ("channel", "beta")):
            with self.subTest(key=key):
                self.assertIsNone(index.answer(urlencode({**QUERY, key: value}))["firmware"])
        self.assertEqual(index.answer(urlencode({**QUERY, "fw_code": 3}))["firmware"], ARTIFACT)

    def test_invalid_duplicate_private_and_oversized_queries(self):
        index = self.index()
        for query in (urlencode(QUERY) + "&board=evil", urlencode(QUERY) + "&password=secret",
                      urlencode({**QUERY, "features": -1}), urlencode({**QUERY, "schema": 2}),
                      urlencode({**QUERY, "features": "01"}), urlencode({**QUERY, "api": "1e0"}),
                      urlencode({**QUERY, "fw_version": "x" * 32}), "x" * 1025):
            with self.subTest(query=query[:30]):
                with self.assertRaises(ValueError):
                    index.answer(query)

    def test_catalog_rejects_duplicate_keys_and_identity(self):
        with self.assertRaises(ValueError):
            json.loads('{"schema":1,"schema":2}', object_pairs_hook=unique_object)
        release = {**IDENTITY, "firmware": ARTIFACT, "web": None}
        with self.assertRaises(ValueError):
            ReleaseIndex({"schema": 1, "releases": [release, release]})
        for change in ({"bytes": 0}, {"code": True}, {"sha256": "x" * 64}, {"url": "ftp://example/app"}, {"extra": 1}):
            with self.assertRaises(ValueError):
                self.index(firmware={**ARTIFACT, **change})

    def test_transport_policy_matches_device(self):
        for url in ("ftp://example/app", "https://user:pass@example/app", "https://example/app#fragment",
                    "https:///app", "https://example:0/app", "https://example:99999/app", "https://example/ bad", "https://example/%zz"):
            with self.assertRaises(ValueError):
                release_url(url)
        self.assertEqual(release_url("https://127.0.0.1:8443/artifact%20one.bin"), "https://127.0.0.1:8443/artifact%20one.bin")
        self.assertEqual(release_url("http://127.0.0.1:8088/artifact.bin"), "http://127.0.0.1:8088/artifact.bin")
        self.index(firmware={**ARTIFACT, "url": "http://example/app"})

    def test_catalog_remains_responsive_while_artifact_reader_stalls(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = root / "app.bin"
            with payload.open("wb") as stream:
                stream.truncate(8 * 1024 * 1024)
            artifact = {**ARTIFACT, "bytes": payload.stat().st_size}
            catalog = root / "catalog.json"
            catalog.write_text(json.dumps({"schema": 1, "releases": [
                {**IDENTITY, "firmware": artifact, "web": None}]}), encoding="utf8")
            server = create_server(("127.0.0.1", 0), catalog, root)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            slow = socket.socket()
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.settimeout(2)
            try:
                slow.connect(server.server_address)
                slow.sendall(b"GET /blip/releases/app.bin HTTP/1.1\r\nHost: localhost\r\n\r\n")
                self.assertIn(b"200", slow.recv(1024))
                # Deliberately stop reading the multi-megabyte response. Its
                # worker cannot finish before this independent catalog request.
                url = f"http://127.0.0.1:{server.server_port}/blip/update?" + urlencode(QUERY)
                with urlopen(url, timeout=2) as response:
                    self.assertEqual(json.load(response)["firmware"], artifact)
            finally:
                slow.close()
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)

    def test_handler_files_errors_and_cpp_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.json"
            payload = b"fixture artifact"
            (root / "app.bin").write_bytes(payload)
            catalog.write_text(json.dumps({"schema": 1, "releases": [{**IDENTITY, "firmware": ARTIFACT, "web": None}]}), encoding="utf-8")
            server = HTTPServer(("127.0.0.1", 0), handler(catalog, root))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            try:
                with urlopen(base + "/blip/update?" + urlencode(QUERY), timeout=2) as response:
                    answer = response.read()
                    self.assertEqual(response.headers["Cache-Control"], "no-store")
                    self.assertEqual(json.loads(answer)["firmware"], ARTIFACT)
                cpp = os.environ.get("BLIP_RELEASE_CATALOG_TEST_EXE")
                if cpp:
                    fixture = root / "device-response.json"
                    fixture.write_bytes(answer)
                    subprocess.run([cpp, str(fixture)], check=True, timeout=5)
                    print("C++ device parser accepted the HTTP catalog response", flush=True)
                with urlopen(base + "/blip/releases/app.bin", timeout=2) as response:
                    self.assertEqual(response.read(), payload)
                for path, status in (("/blip/update?schema=1", 400), ("/blip/releases/unknown.bin", 404),
                                     ("/blip/releases/../catalog.json", 404), ("/blip/releases/app.bin?x=1", 404)):
                    with self.subTest(path=path), self.assertRaises(HTTPError) as error:
                        urlopen(base + path, timeout=2)
                    self.assertEqual(error.exception.code, status)
                    error.exception.close()
                catalog.write_text("invalid", encoding="utf-8")
                with self.assertRaises(HTTPError) as error:
                    urlopen(base + "/blip/update?" + urlencode(QUERY), timeout=2)
                self.assertEqual(error.exception.code, 503)
                error.exception.close()
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
