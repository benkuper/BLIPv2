const MAX_PACKET_BYTES = 1024;
const MAX_ADDRESS_BYTES = 128;
const MAX_STRING_BYTES = 256;
const MAX_ARGUMENTS = 8;
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });

function align4(value) {
  return (value + 3) & ~3;
}

function encodedString(value, maximum, label) {
  if (typeof value !== "string" || value.includes("\0")) {
    throw new TypeError(`${label} must be a NUL-free string`);
  }
  const bytes = encoder.encode(value);
  if (bytes.length > maximum) {
    throw new RangeError(`${label} exceeds its protocol bound`);
  }
  return bytes;
}

function valueTag(value) {
  switch (value.type) {
    case "boolean":
      return value.value ? "T" : "F";
    case "integer":
      return "i";
    case "number":
      return "f";
    case "string":
      return "s";
    case "rgba":
      return "r";
    case "midi":
      return "m";
    case "timetag":
      return "t";
    default:
      throw new TypeError(`Unsupported OSC value type ${value.type}`);
  }
}

function valueSize(value) {
  switch (value.type) {
    case "boolean":
      return 0;
    case "integer":
    case "number":
    case "rgba":
    case "midi":
      return 4;
    case "timetag":
      return 8;
    case "string":
      return align4(encodedString(value.value, MAX_STRING_BYTES, "OSC string").length + 1);
    default:
      valueTag(value);
      return 0;
  }
}

function writeString(bytes, offset, valueBytes) {
  bytes.set(valueBytes, offset);
  return offset + align4(valueBytes.length + 1);
}

function checkWord(value, label) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError(`${label} must be an unsigned 32-bit integer`);
  }
}

export function encodeOscMessage(address, values = []) {
  const addressBytes = encodedString(address, MAX_ADDRESS_BYTES, "OSC address");
  if (!address.startsWith("/") || values.length > MAX_ARGUMENTS) {
    throw new RangeError("Invalid OSC address or argument count");
  }
  const tags = `,${values.map(valueTag).join("")}`;
  const tagBytes = encodedString(tags, MAX_ARGUMENTS + 1, "OSC type tag");
  const size =
    align4(addressBytes.length + 1) +
    align4(tagBytes.length + 1) +
    values.reduce((total, value) => total + valueSize(value), 0);
  if (size > MAX_PACKET_BYTES) throw new RangeError("OSC packet exceeds 1024 bytes");

  const buffer = new ArrayBuffer(size);
  const bytes = new Uint8Array(buffer);
  const view = new DataView(buffer);
  let offset = writeString(bytes, 0, addressBytes);
  offset = writeString(bytes, offset, tagBytes);
  for (const value of values) {
    switch (value.type) {
      case "boolean":
        break;
      case "integer":
        if (!Number.isInteger(value.value) || value.value < -2147483648 || value.value > 2147483647) {
          throw new RangeError("OSC integer must fit int32");
        }
        view.setInt32(offset, value.value, false);
        offset += 4;
        break;
      case "number":
        if (!Number.isFinite(value.value)) throw new RangeError("OSC number must be finite");
        view.setFloat32(offset, value.value, false);
        offset += 4;
        break;
      case "string": {
        const valueBytes = encodedString(value.value, MAX_STRING_BYTES, "OSC string");
        offset = writeString(bytes, offset, valueBytes);
        break;
      }
      case "rgba":
      case "midi":
        checkWord(value.value, `OSC ${value.type}`);
        view.setUint32(offset, value.value, false);
        offset += 4;
        break;
      case "timetag":
        if (typeof value.value !== "bigint" || value.value < 0n || value.value > 0xffffffffffffffffn) {
          throw new RangeError("OSC timetag must fit uint64");
        }
        view.setBigUint64(offset, value.value, false);
        offset += 8;
        break;
      default:
        throw new TypeError(`Unsupported OSC value type ${value.type}`);
    }
  }
  return buffer;
}

function asBytes(packet) {
  if (packet instanceof ArrayBuffer) return new Uint8Array(packet);
  if (ArrayBuffer.isView(packet)) {
    return new Uint8Array(packet.buffer, packet.byteOffset, packet.byteLength);
  }
  throw new TypeError("OSC packet must be an ArrayBuffer or typed array");
}

function readString(bytes, start, maximum, label) {
  const endLimit = Math.min(bytes.length, start + maximum + 1);
  let end = start;
  while (end < endLimit && bytes[end] !== 0) end += 1;
  if (end >= endLimit || bytes[end] !== 0) {
    throw new TypeError(`Invalid ${label}`);
  }
  const next = start + align4(end - start + 1);
  if (next > bytes.length || bytes.slice(end + 1, next).some((value) => value !== 0)) {
    throw new TypeError(`Invalid ${label} padding`);
  }
  return { value: decoder.decode(bytes.subarray(start, end)), next };
}

function requireBytes(bytes, offset, count) {
  if (offset + count > bytes.length) throw new RangeError("Truncated OSC packet");
}

export function decodeOscMessage(packet) {
  const bytes = asBytes(packet);
  if (bytes.length === 0 || bytes.length > MAX_PACKET_BYTES || bytes.length % 4 !== 0) {
    throw new RangeError("Invalid OSC packet length");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const addressResult = readString(bytes, 0, MAX_ADDRESS_BYTES, "OSC address");
  if (!addressResult.value.startsWith("/")) throw new TypeError("Invalid OSC address");
  const tagsResult = readString(bytes, addressResult.next, MAX_ARGUMENTS + 1, "OSC type tag");
  if (!tagsResult.value.startsWith(",")) throw new TypeError("Invalid OSC type tag");
  const tags = tagsResult.value.slice(1).split("");
  if (tags.length > MAX_ARGUMENTS) throw new RangeError("Too many OSC arguments");
  let offset = tagsResult.next;
  const values = [];
  for (const tag of tags) {
    switch (tag) {
      case "T":
      case "F":
        values.push({ type: "boolean", value: tag === "T" });
        break;
      case "i":
        requireBytes(bytes, offset, 4);
        values.push({ type: "integer", value: view.getInt32(offset, false) });
        offset += 4;
        break;
      case "f": {
        requireBytes(bytes, offset, 4);
        const value = view.getFloat32(offset, false);
        if (!Number.isFinite(value)) throw new TypeError("Non-finite OSC number");
        values.push({ type: "number", value });
        offset += 4;
        break;
      }
      case "s": {
        const result = readString(bytes, offset, MAX_STRING_BYTES, "OSC string");
        values.push({ type: "string", value: result.value });
        offset = result.next;
        break;
      }
      case "r":
      case "m":
        requireBytes(bytes, offset, 4);
        values.push({ type: tag === "r" ? "rgba" : "midi", value: view.getUint32(offset, false) });
        offset += 4;
        break;
      case "t":
        requireBytes(bytes, offset, 8);
        values.push({ type: "timetag", value: view.getBigUint64(offset, false) });
        offset += 8;
        break;
      default:
        throw new TypeError(`Unsupported OSC tag ${tag}`);
    }
  }
  if (offset !== bytes.length) throw new TypeError("Trailing OSC bytes");
  return { address: addressResult.value, values };
}
