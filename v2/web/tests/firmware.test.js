import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import test from "node:test";

import { inspectFirmware, uploadFirmware } from "../src/firmware.js";

function fixture(project = "blip-v2", version = "0.1.0") {
  const image = new Uint8Array(512);
  image[0] = 0xe9;
  new DataView(image.buffer).setUint32(32, 0xabcd5432, true);
  image.set(Buffer.from(`${version}\0`, "ascii"), 48);
  image.set(Buffer.from(`${project}\0`, "ascii"), 80);
  return image.buffer;
}

test("inspects ESP-IDF application metadata and digest", async () => {
  const source = fixture();
  const image = await inspectFirmware(source, webcrypto);
  assert.equal(image.project, "blip-v2");
  assert.equal(image.version, "0.1.0");
  assert.equal(image.size, 512);
  assert.equal(image.sha256, createHash("sha256").update(Buffer.from(source)).digest("hex"));
});

test("rejects invalid application images", async () => {
  await assert.rejects(() => inspectFirmware(new ArrayBuffer(512), webcrypto), /not an ESP-IDF/);
});

test("uploads with verified metadata headers", async () => {
  let request;
  const fetchImpl = async (url, init) => {
    request = { url: String(url), init };
    return { status: 202, text: async () => "accepted" };
  };
  const image = await uploadFirmware({
    file: { arrayBuffer: async () => fixture() },
    target: "esp32c6",
    baseUrl: "http://192.0.2.7/",
    fetchImpl,
    cryptoImpl: webcrypto,
  });
  assert.equal(image.project, "blip-v2");
  assert.equal(request.url, "http://192.0.2.7/api/firmware");
  assert.equal(request.init.headers["X-BLIP-Target"], "esp32c6");
  assert.equal(request.init.headers["X-BLIP-Project"], "blip-v2");
  assert.equal(request.init.headers["X-BLIP-SHA256"].length, 64);
});

test("rejects an unknown target before upload", async () => {
  await assert.rejects(
    () => uploadFirmware({ file: {}, target: "esp8266", baseUrl: "http://192.0.2.7/" }),
    /Select the connected device target/,
  );
});
