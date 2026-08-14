import assert from "node:assert/strict";
import test from "node:test";

import {
  buildControlModel,
  coerceControlValue,
  describeHost,
  filterControlModel,
} from "../src/model.js";

function leaf(path, description, extra) {
  return { DESCRIPTION: description, FULL_PATH: path, ACCESS: 3, ...extra };
}

function component(path, id, description, contents) {
  return {
    DESCRIPTION: description,
    FULL_PATH: path,
    ACCESS: 0,
    BLIP_KIND: "component",
    BLIP_COMPONENT_ID: id,
    BLIP_SCHEMA_VERSION: 2,
    BLIP_DISABLE_POLICY: "live",
    CONTENTS: contents,
  };
}

function registryTree() {
  return {
    DESCRIPTION: "Root",
    FULL_PATH: "",
    ACCESS: 0,
    BLIP_KIND: "root",
    CONTENTS: {
      lights: component("/lights", "demo.lights", "Stage lights", {
        brightness: leaf("/lights/brightness", "Brightness", {
          TYPE: "f",
          VALUE: [0.65],
          RANGE: [{ MIN: 0, MAX: 1 }],
          BLIP_KIND: "parameter",
          BLIP_PERSISTED: true,
          BLIP_READABLE: true,
          BLIP_WRITABLE: true,
          BLIP_STEP: 0.01,
          BLIP_UNIT: "ratio",
        }),
        enabled: leaf("/lights/enabled", "Enabled", {
          TYPE: "T",
          VALUE: [true],
          BLIP_KIND: "parameter",
          BLIP_READABLE: true,
          BLIP_WRITABLE: true,
        }),
        palette: leaf("/lights/palette", "Palette", {
          TYPE: "s",
          VALUE: ["Warm"],
          RANGE: [{ VALS: ["Warm", "Cool"] }],
          BLIP_KIND: "parameter",
          BLIP_READABLE: true,
          BLIP_WRITABLE: true,
        }),
        token: leaf("/lights/token", "Token", {
          TYPE: "s",
          BLIP_KIND: "parameter",
          BLIP_READABLE: false,
          BLIP_WRITABLE: true,
        }),
        calibrate: leaf("/lights/calibrate", "Calibrate", {
          TYPE: "if",
          BLIP_KIND: "action",
          BLIP_FIELDS: [
            { ID: "channel", TYPE: "i", REQUIRED: true },
            { ID: "gain", TYPE: "f", REQUIRED: true },
          ],
        }),
        changed: leaf("/lights/changed", "changed", {
          ACCESS: 1,
          TYPE: "f",
          BLIP_KIND: "event",
          BLIP_FIELDS: [{ ID: "value", TYPE: "f", REQUIRED: true }],
        }),
      }),
      sensors: component("/sensors", "demo.sensors", "Sensors", {
        temperature: leaf("/sensors/temperature", "Temperature", {
          ACCESS: 1,
          TYPE: "f",
          VALUE: [21.5],
          BLIP_KIND: "parameter",
          BLIP_READABLE: true,
          BLIP_WRITABLE: false,
          BLIP_UNIT: "°C",
        }),
      }),
    },
  };
}

test("registry nodes become generic component controls", () => {
  const model = buildControlModel(registryTree());
  assert.equal(model.components.length, 2);
  assert.equal(model.components[0].id, "demo.lights");
  assert.deepEqual(
    model.components[0].controls.map(({ id, editor }) => [id, editor]),
    [
      ["brightness", "number"],
      ["enabled", "checkbox"],
      ["palette", "select"],
      ["token", "password"],
      ["calibrate", "action"],
      ["changed", "readonly"],
    ],
  );
  assert.equal(model.index.get("/lights/calibrate").fields[0].id, "channel");
  assert.equal(model.index.get("/lights/brightness").range.step, 0.01);
});

test("a new registry component needs no component-specific UI code", () => {
  const tree = registryTree();
  tree.CONTENTS.motion = component("/motion", "demo.motion", "Motion", {
    sensitivity: leaf("/motion/sensitivity", "Sensitivity", {
      TYPE: "i",
      VALUE: [4],
      RANGE: [{ MIN: 0, MAX: 10 }],
      BLIP_KIND: "parameter",
      BLIP_READABLE: true,
      BLIP_WRITABLE: true,
    }),
  });
  const model = buildControlModel(tree);
  assert.equal(model.components.length, 3);
  assert.equal(model.index.get("/motion/sensitivity").editor, "number");
});

test("legacy OSCQuery leaves retain useful fallback behavior", () => {
  const tree = registryTree();
  tree.CONTENTS.legacy = {
    DESCRIPTION: "Legacy",
    FULL_PATH: "/legacy",
    ACCESS: 0,
    CONTENTS: {
      fire: leaf("/legacy/fire", "Fire", { TYPE: "I" }),
      mode: leaf("/legacy/mode", "Mode", {
        TYPE: "s",
        VALUE: ["auto"],
        RANGE: [{ VALS: ["auto", "manual"] }],
      }),
    },
  };
  const model = buildControlModel(tree);
  assert.equal(model.index.get("/legacy/fire").kind, "action");
  assert.equal(model.index.get("/legacy/mode").editor, "select");
});

test("search, host identity, and typed input are deterministic", () => {
  const model = buildControlModel(registryTree());
  const filtered = filterControlModel(model, "temperature");
  assert.equal(filtered.length, 1);
  assert.equal(filtered[0].id, "demo.sensors");
  assert.deepEqual(describeHost({ NAME: "Fixture", DEVICE_ID: "AA", VERSION: "2.0" }), {
    name: "Fixture",
    id: "AA",
    type: "BLIP",
    version: "2.0",
  });
  assert.deepEqual(coerceControlValue("integer", "12"), { type: "integer", value: 12 });
  assert.deepEqual(coerceControlValue("boolean", false), { type: "boolean", value: false });
  assert.throws(() => coerceControlValue("integer", "1.5"), /does not match/);
});

test("malformed and duplicate paths fail closed", () => {
  assert.throws(() => buildControlModel({}), /CONTENTS/);
  const tree = registryTree();
  tree.CONTENTS.sensors.CONTENTS.temperature.FULL_PATH = "/lights/brightness";
  assert.throws(() => buildControlModel(tree), /Duplicate OSC path/);
});
