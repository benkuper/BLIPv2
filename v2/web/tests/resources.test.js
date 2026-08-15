import assert from "node:assert/strict";
import test from "node:test";

import { buildReassignment, filterPins, normalizeResourceSnapshot, pinOutcome } from "../src/resources.js";

function snapshot() {
  return normalizeResourceSnapshot({
    schema_version: 1,
    allocation_revision: 7,
    board: { id: "fixture", target: "esp32c6", antenna: "onboard" },
    pins: [
      { id: "gpio.1", label: "D1", gpio: 1, capabilities: 0x42, electrical: "3.3V", selectable: true, state: "free", reason: "", bus: "", owners: [], configured_owners: [] },
      { id: "gpio.2", label: "D2", gpio: 2, capabilities: 0x42, electrical: "3.3V", selectable: true, state: "exclusive", reason: "", bus: "", owners: [{ path: "motor.left:pin", role: "Motor", mode: "exclusive", optional: true, reboot_required: true }], configured_owners: ["motor.left:pin"] },
      { id: "gpio.3", label: "RF switch", gpio: 3, capabilities: 2, electrical: "RF", selectable: false, state: "reserved", reason: "onboard-antenna-rf-switch-power", bus: "", owners: [{ path: "system.radio", role: "antenna", mode: "reserved" }], configured_owners: [] },
      { id: "gpio.4", label: "Input only", gpio: 4, capabilities: 1, electrical: "input", selectable: true, state: "free", reason: "", bus: "", owners: [], configured_owners: [] },
      { id: "gpio.5", label: "I2C SDA", gpio: 5, capabilities: 0x63, electrical: "open drain", selectable: true, state: "shared", reason: "", bus: "i2c.0", owners: [{ path: "sensor.a:bus", role: "0x28", mode: "bus-member", member_key: 40 }, { path: "sensor.b:bus", role: "0x29", mode: "bus-member", member_key: 41 }], configured_owners: ["sensor.a:bus", "sensor.b:bus"] },
    ],
  });
}

const control = {
  id: "pin",
  path: "/output/pixel-strip/pin",
  resourceOwner: "output.pixel-strip:pin",
  resourceSelector: { requiredCapabilities: 0x42, supportsSwap: true, supportsMove: true },
};

test("all board pins remain visible with distinct outcomes and reasons", () => {
  const value = snapshot();
  assert.equal(value.board.antenna, "onboard");
  assert.deepEqual(value.pins.map((pin) => pinOutcome(pin, control).state), ["free", "conflict", "reserved", "incompatible", "shared"]);
  assert.match(pinOutcome(value.pins[2], control).reason, /onboard-antenna/);
  assert.equal(filterPins(value, "sensor.b")[0].id, "gpio.5");
});

test("conflicts require explicit revisioned swap or optional move", () => {
  const value = snapshot();
  const pin = value.pins[1];
  assert.deepEqual(buildReassignment({ snapshot: value, control, pin, operation: "swap" }), {
    schema_version: 1,
    expected_revision: 7,
    operation: "swap",
    requester: "output.pixel-strip:pin",
    previous_owner: "motor.left:pin",
    target_resource: "gpio.2",
  });
  assert.equal(buildReassignment({ snapshot: value, control, pin, operation: "unassign-and-move" }).operation, "unassign-and-move");
  pin.owners[0].optional = false;
  assert.throws(() => buildReassignment({ snapshot: value, control, pin, operation: "unassign-and-move" }), /required/);
});

test("duplicate pins and malformed snapshots fail closed", () => {
  const raw = { schema_version: 1, allocation_revision: 1, board: {}, pins: [{ id: "gpio.1", gpio: 1 }, { id: "gpio.1", gpio: 1 }] };
  assert.throws(() => normalizeResourceSnapshot(raw), /Invalid resource pin/);
  assert.throws(() => normalizeResourceSnapshot({}), /Unsupported/);
});
