import assert from "node:assert/strict";
import test from "node:test";
import { buildControlModel, presentationModel, modelShapeKey, sparklinePoints } from "../src/model.js";

function tree(generation = 1, value = 2) {
  return { CONTENTS: { component: { FULL_PATH: "/device", BLIP_COMPONENT_ID: "fixture.device",
    BLIP_UI: { TOPIC: "Sensors", PRIMARY: "level", GAUGES: "level" }, BLIP_SCHEMA_GENERATION: generation,
    CONTENTS: {
      level: { TYPE: "i", VALUE: [value], ACCESS: 1, RANGE: [{ MIN: 0, MAX: 10 }] },
      secret: { TYPE: "s", ACCESS: 2, BLIP_READABLE: false, BLIP_WRITABLE: true },
      dynamic: { TYPE: "s", ACCESS: 3, BLIP_DYNAMIC: true, VALUE: ["hello"] },
      advanced: { TYPE: "i", VALUE: [123], ACCESS: 3 },
    } } } };
}

test("Simple includes declared main controls and dynamic controls; Advanced covers all", () => {
  const model = buildControlModel(tree());
  assert.deepEqual(presentationModel(model, "simple").components[0].controls.map(c => c.id), ["level", "dynamic"]);
  assert.equal(presentationModel(model, "advanced").index.size, 4);
  assert.equal(model.components[0].topic, "Sensors");
  assert.equal(model.index.get("/device/level").gauge, true);
  assert.equal(presentationModel(model, "advanced", "Other").components.length, 0);
});

test("Value polling preserves shape; new schema generation invalidates editors", () => {
  assert.equal(modelShapeKey(buildControlModel(tree(1, 2))), modelShapeKey(buildControlModel(tree(1, 7))));
  assert.notEqual(modelShapeKey(buildControlModel(tree(1))), modelShapeKey(buildControlModel(tree(2))));
});

test("Graph geometry handles constant samples and rejects non-finite values", () => {
  assert.equal(sparklinePoints([3, 3]), "0,24 120,24");
  assert.equal(sparklinePoints([0, 10]), "0,46 120,2");
  assert.equal(sparklinePoints([NaN, Infinity]), "");
});
