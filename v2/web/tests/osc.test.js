import assert from "node:assert/strict";
import test from "node:test";

import { decodeOscMessage, encodeOscMessage } from "../src/osc.js";

test("OSC scalar values round-trip through the browser codec", () => {
  const packet = encodeOscMessage("/demo/control", [
    { type: "boolean", value: true },
    { type: "boolean", value: false },
    { type: "integer", value: -42 },
    { type: "number", value: 1.25 },
    { type: "string", value: "héllo" },
    { type: "string", value: "" },
  ]);
  assert.equal(packet.byteLength % 4, 0);
  assert.deepEqual(decodeOscMessage(packet), {
    address: "/demo/control",
    values: [
      { type: "boolean", value: true },
      { type: "boolean", value: false },
      { type: "integer", value: -42 },
      { type: "number", value: 1.25 },
      { type: "string", value: "héllo" },
      { type: "string", value: "" },
    ],
  });
});

test("OSC fixed-width compatibility types round-trip", () => {
  const packet = encodeOscMessage("/types", [
    { type: "rgba", value: 0x11223344 },
    { type: "midi", value: 0x01903c7f },
    { type: "timetag", value: 0x0102030405060708n },
  ]);
  assert.deepEqual(decodeOscMessage(packet).values, [
    { type: "rgba", value: 0x11223344 },
    { type: "midi", value: 0x01903c7f },
    { type: "timetag", value: 0x0102030405060708n },
  ]);
});

test("zero-argument actions produce a valid impulse packet", () => {
  const packet = new Uint8Array(encodeOscMessage("/settings/save"));
  assert.deepEqual([...packet], [
    47, 115, 101, 116, 116, 105, 110, 103, 115, 47, 115, 97, 118, 101, 0, 0,
    44, 0, 0, 0,
  ]);
  assert.deepEqual(decodeOscMessage(packet), { address: "/settings/save", values: [] });
});

test("malformed and oversized OSC input fails closed", () => {
  assert.throws(() => encodeOscMessage("not-a-path"), /Invalid OSC/);
  assert.throws(
    () => encodeOscMessage("/bad", [{ type: "number", value: Number.NaN }]),
    /finite/,
  );
  assert.throws(() => encodeOscMessage("/bad", Array(9).fill({ type: "boolean", value: true })), /count/);
  const valid = new Uint8Array(encodeOscMessage("/x"));
  valid[3] = 1;
  assert.throws(() => decodeOscMessage(valid), /padding/);
  assert.throws(() => decodeOscMessage(new Uint8Array(1028)), /length/);
});
