import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from release_publish import firmware_metadata, web_metadata, publish
from release_server import load_index

ROOT = Path(__file__).resolve().parents[3]


def firmware(code=2):
    data = bytearray(512)
    data[0] = 0xe9
    struct.pack_into("<H", data, 12, 13)
    struct.pack_into("<I", data, 32, 0xabcd5432)
    data[48:53] = b"0.1.0"
    data[80:87] = b"blip-v2"
    data[288:296] = b"BLIPREL1"
    struct.pack_into("<5I", data, 296, 1, code, 8388608, 123, 1)
    for offset, value in ((316, b"blip-v2"), (348, b"creators-ball-v2"), (412, b"esp32c6"),
                          (428, b"ota-8mb-v1"), (460, b"minimal")):
        data[offset:offset + len(value)] = value
    return data


class ReleasePublishTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.image = self.root / "app.bin"
        self.image.write_bytes(firmware())
        self.web = ROOT / "v2/components/blip_storage/factory_web.bundle"
        self.catalog = self.root / "index.json"
        self.artifacts = self.root / "files"

    def tearDown(self):
        self.temporary.cleanup()

    def publish(self, **changes):
        arguments = dict(firmware=self.image, web=self.web, base_url="https://www.goldengeek.org/blip/releases",
                         channel="stable", firmware_minimum_web=0, web_minimum_firmware=1,
                         catalog=self.catalog, artifacts=self.artifacts)
        arguments.update(changes)
        return publish(**arguments)

    def test_embedded_identity_and_native_web_version(self):
        identity, code, version = firmware_metadata(self.image)
        self.assertEqual((identity["board"], identity["flash_bytes"], identity["features"], code, version),
                         ("creators-ball-v2", 8388608, 123, 2, "0.1.0"))
        self.assertEqual(web_metadata(self.web), (2000, "0.2.0"))
        for offset in (0, 12, 32, 80, 288, 296, 312, 428):
            with self.subTest(offset=offset):
                data = firmware(); data[offset] ^= 1
                self.image.write_bytes(data)
                with self.assertRaises(ValueError):
                    firmware_metadata(self.image)

    def test_immutable_payloads_and_index_publication(self):
        row = self.publish()
        self.assertEqual(next(iter(load_index(self.catalog).releases.values())), row)
        for kind in ("firmware", "web"):
            path = self.artifacts / row[kind]["url"].rsplit("/", 1)[1]
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), row[kind]["sha256"])
            self.assertEqual(path.stat().st_size, row[kind]["bytes"])
        before = self.catalog.read_bytes()
        self.assertEqual(self.publish(), row)
        self.assertEqual(self.catalog.read_bytes(), before)
        newer = firmware(3); self.image.write_bytes(newer)
        self.assertEqual(self.publish(web=None)["web"], row["web"])
        self.assertEqual(next(iter(load_index(self.catalog).releases.values()))["firmware"]["code"], 3)

    def test_bad_inputs_and_same_code_changes_preserve_index(self):
        self.publish()
        previous = self.catalog.read_bytes()
        for changes in ({"base_url": "http://example/blip/releases"}, {"base_url": "https://example/elsewhere"},
                        {"firmware_minimum_web": -1}, {"web_minimum_firmware": -1}, {"channel": ""}):
            with self.assertRaises(ValueError):
                self.publish(**changes)
            self.assertEqual(self.catalog.read_bytes(), previous)
        changed = firmware(); changed[-1] = 1; self.image.write_bytes(changed)
        with self.assertRaises(ValueError):
            self.publish()
        self.assertEqual(self.catalog.read_bytes(), previous)
        self.image.write_bytes(firmware(1))
        with self.assertRaises(ValueError):
            self.publish()
        self.assertEqual(self.catalog.read_bytes(), previous)

    def test_corrupt_web_never_publishes(self):
        path = self.root / "bad.bundle"
        data = bytearray(self.web.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
        with self.assertRaises(ValueError):
            self.publish(web=path)
        self.assertFalse(self.catalog.exists())
        self.assertFalse(self.artifacts.exists())


if __name__ == "__main__":
    unittest.main()
