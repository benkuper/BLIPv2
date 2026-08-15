const DESCRIPTOR_END = 288;
const APP_DESCRIPTOR_OFFSET = 32;
const APP_DESCRIPTOR_MAGIC = 0xabcd5432;
const TARGETS = new Set(["esp32", "esp32s3", "esp32c6"]);

function fixedAscii(bytes, offset, capacity, label) {
  const field = bytes.subarray(offset, offset + capacity);
  const terminator = field.indexOf(0);
  if (terminator <= 0) throw new TypeError(`Invalid ${label} in application descriptor`);
  const value = new TextDecoder("ascii", { fatal: true }).decode(field.subarray(0, terminator));
  if (!/^[\x20-\x7e]+$/.test(value)) throw new TypeError(`Invalid ${label} in application descriptor`);
  return value;
}

export async function inspectFirmware(source, cryptoImpl = globalThis.crypto) {
  const buffer = source instanceof ArrayBuffer ? source : await source.arrayBuffer();
  if (buffer.byteLength < DESCRIPTOR_END) throw new TypeError("Firmware image is too small");
  const bytes = new Uint8Array(buffer);
  const view = new DataView(buffer);
  if (bytes[0] !== 0xe9 || view.getUint32(APP_DESCRIPTOR_OFFSET, true) !== APP_DESCRIPTOR_MAGIC) {
    throw new TypeError("File is not an ESP-IDF application image");
  }
  const digest = new Uint8Array(await cryptoImpl.subtle.digest("SHA-256", buffer));
  return {
    buffer,
    size: buffer.byteLength,
    sha256: Array.from(digest, (value) => value.toString(16).padStart(2, "0")).join(""),
    version: fixedAscii(bytes, 48, 32, "version"),
    project: fixedAscii(bytes, 80, 32, "project"),
  };
}

export async function uploadFirmware({ file, target, baseUrl, fetchImpl = globalThis.fetch, cryptoImpl }) {
  if (!TARGETS.has(target)) throw new TypeError("Select the connected device target");
  const image = await inspectFirmware(file, cryptoImpl);
  if (image.project !== "blip-v2") throw new TypeError(`Unexpected firmware project: ${image.project}`);
  const endpoint = new URL("/api/firmware", baseUrl);
  const response = await fetchImpl(endpoint, {
    method: "PUT",
    cache: "no-store",
    headers: {
      "Content-Type": "application/octet-stream",
      "X-BLIP-SHA256": image.sha256,
      "X-BLIP-Project": image.project,
      "X-BLIP-Version": image.version,
      "X-BLIP-Target": target,
      "X-BLIP-Profile": "minimal",
    },
    body: image.buffer,
  });
  const detail = await response.text();
  if (response.status !== 202) {
    throw new Error(`Device rejected firmware (HTTP ${response.status}): ${detail}`);
  }
  return image;
}
