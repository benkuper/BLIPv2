import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import test from "node:test";

import { inspectFirmware, uploadFirmware, sha256Hex, FirmwarePanel } from "../src/firmware.js";

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

test("plain-HTTP checksum matches SHA-256 vectors and block/padding boundaries", async () => {
  const encode = text => new TextEncoder().encode(text).buffer;
  assert.equal(await sha256Hex(encode(""), null), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert.equal(await sha256Hex(encode("abc"), {}), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  for (const size of [1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 65535, 65536, 65537, 2_000_003]) {
    const bytes = Uint8Array.from({ length: size }, (_, i) => (i * 197 + (i >>> 8)) & 255);
    assert.equal(await sha256Hex(bytes.buffer, {}), createHash("sha256").update(bytes).digest("hex"), `length ${size}`);
  }
});

test("plain-HTTP hashing yields while preparing a firmware-sized image", async () => {
  let timerRan = false;
  const timer = new Promise(resolve => setTimeout(() => { timerRan = true; resolve(); }, 0));
  await sha256Hex(new ArrayBuffer(1_000_000), {});
  assert.equal(timerRan, true);
  await timer;
});

test("uploads over HTTP without SubtleCrypto and preserves the verified checksum", async () => {
  const source = fixture();
  let request;
  const image = await uploadFirmware({
    file: { arrayBuffer: async () => source }, target: "esp32c6", baseUrl: "http://192.0.2.7/",
    cryptoImpl: {}, fetchImpl: async (url, init) => {
      request = init;
      return { status: 202, text: async () => "accepted" };
    },
  });
  const expected = createHash("sha256").update(Buffer.from(source)).digest("hex");
  assert.equal(image.sha256, expected);
  assert.equal(request.headers["X-BLIP-SHA256"], expected);
  assert.equal(request.body, source);
});

test("manual firmware owns transfer admission from validation through response consumption", async () => {
  const elements = new Map(["file", "target", "button", "status"].map(key => ["#firmware-" + key,
    { files: [{}], value: "esp32c6", addEventListener() {} }]));
  const root = { querySelector: key => elements.get(key), setAttribute(key, value) { this[key] = value; } };
  let done, calls = 0, rejected = false;
  const panel = new FirmwarePanel({ root, baseUrl: () => "http://192.0.2.7/", isTransferring: () => rejected,
    upload: options => {
      calls++; assert.equal(panel.busy, true); assert.equal(root["aria-busy"], "true");
      assert.equal(options.target, "esp32c6"); assert.equal(options.baseUrl, "http://192.0.2.7/");
      return new Promise(resolve => { done = resolve; });
    } });
  const pending = panel.submit();
  assert.equal(panel.button.disabled, true); assert.equal(panel.file.disabled, true);
  await panel.submit(); assert.equal(calls, 1, "a second click cannot repeat the upload");
  done({ project: "blip-v2", version: "test" }); await pending;
  assert.equal(panel.busy, false); assert.equal(root["aria-busy"], "false");
  assert.match(panel.status.textContent, /accepted/);
  rejected = true; await panel.submit(); assert.equal(calls, 1); assert.match(panel.status.textContent, /Wait/);
  rejected = false; panel.upload = async () => { calls++; throw new TypeError("connection lost"); };
  await panel.submit(); assert.equal(calls, 2, "uncertain writes must not be replayed");
  assert.equal(panel.busy, false); assert.equal(panel.button.disabled, false);
  assert.match(panel.status.textContent, /connection lost/);
});
