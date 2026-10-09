const DESCRIPTOR_END = 288;
const APP_DESCRIPTOR_OFFSET = 32;
const APP_DESCRIPTOR_MAGIC = 0xabcd5432;
const TARGETS = new Set(["esp32", "esp32s3", "esp32c6"]);

const SHA256_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);
const rotateRight = (word, bits) => (word >>> bits) | (word << (32 - bits));
const hex = bytes => Array.from(bytes, value => value.toString(16).padStart(2, "0")).join("");

export async function sha256Hex(buffer, cryptoImpl = globalThis.crypto) {
  if (typeof cryptoImpl?.subtle?.digest === "function") {
    return hex(new Uint8Array(await cryptoImpl.subtle.digest("SHA-256", buffer)));
  }
  // Device HTTP origins have no SubtleCrypto. Keep the checksum available there
  // without a dependency, an extra copy of the image, or a long UI-blocking task.
  const state = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const words = new Uint32Array(64);
  function block(view, offset) {
    for (let i = 0; i < 16; ++i) words[i] = view.getUint32(offset + i * 4);
    for (let i = 16; i < 64; ++i) {
      const x = words[i - 15], y = words[i - 2];
      words[i] = words[i - 16] + (rotateRight(x, 7) ^ rotateRight(x, 18) ^ (x >>> 3)) +
        words[i - 7] + (rotateRight(y, 17) ^ rotateRight(y, 19) ^ (y >>> 10));
    }
    let [a, b, c, d, e, f, g, h] = state;
    for (let i = 0; i < 64; ++i) {
      const t1 = h + (rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25)) +
        ((e & f) ^ (~e & g)) + SHA256_K[i] + words[i];
      const t2 = (rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22)) +
        ((a & b) ^ (a & c) ^ (b & c));
      h = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    for (const [i, word] of [a, b, c, d, e, f, g, h].entries()) state[i] += word;
  }
  const view = new DataView(buffer);
  const fullBytes = buffer.byteLength - buffer.byteLength % 64;
  for (let offset = 0; offset < fullBytes; offset += 64) {
    block(view, offset);
    if ((offset + 64) % 65536 === 0) await new Promise(resolve => setTimeout(resolve, 0));
  }
  const tail = new Uint8Array(buffer.byteLength % 64 < 56 ? 64 : 128);
  tail.set(new Uint8Array(buffer, fullBytes));
  tail[buffer.byteLength % 64] = 0x80;
  const tailView = new DataView(tail.buffer);
  tailView.setUint32(tail.length - 8, Math.floor(buffer.byteLength / 0x20000000));
  tailView.setUint32(tail.length - 4, (buffer.byteLength * 8) >>> 0);
  for (let offset = 0; offset < tail.length; offset += 64) block(tailView, offset);
  return Array.from(state, word => word.toString(16).padStart(8, "0")).join("");
}

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
  const sha256 = await sha256Hex(buffer, cryptoImpl);
  return {
    buffer,
    size: buffer.byteLength,
    sha256,
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
