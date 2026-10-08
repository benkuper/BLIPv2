// Feed the exact C++ surrogate OSCQuery tree to the existing generic web model.
// This checks model integration; it does not claim browser or guest execution.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { buildControlModel } from "../../web/src/model.js";

const [treePath, reportPath] = process.argv.slice(2);
if (!treePath || !reportPath) throw new Error("Usage: node check_dynamic_schema_web.mjs TREE REPORT");
const treeBytes = readFileSync(treePath);
// PowerShell 5 redirects stdout as UTF-16; accept it without changing the bytes
// used by the artifact hash.
const treeText = treeBytes[0] === 0xff && treeBytes[1] === 0xfe
  ? treeBytes.subarray(2).toString("utf16le") : treeBytes.toString("utf8").replace(/^\uFEFF/, "");
const tree = JSON.parse(treeText);
assert.equal(tree.CONTENTS.test.CONTENTS.script.BLIP_SCHEMA_GENERATION, 1);
const model = buildControlModel(tree);
const script = model.components.find(({ id }) => id === "test.script");
assert.ok(script);
assert.deepEqual(script.controls.map(({ id, kind, editor }) => [id, kind, editor]), [
  ["level", "parameter", "number"], ["note", "parameter", "text"],
  ["fire", "action", "action"], ["changed", "event", "readonly"],
]);
const level = model.index.get("/test/script/level");
assert.equal(level.label, "Intensity"); assert.equal(level.value, 5); assert.equal(level.unit, "%");
assert.deepEqual(level.range, { values: [], minimum: 0, maximum: 100, step: 1 });
assert.equal(model.index.get("/test/script/note").value, "hello");
assert.deepEqual(model.index.get("/test/script/fire").fields.map(({ id, type }) => [id, type]),
  [["armed", "boolean"], ["text", "string"]]);
assert.deepEqual(model.index.get("/test/script/changed").fields.map(({ id, type }) => [id, type]),
  [["value", "integer"], ["text", "string"]]);
const sha = (data) => createHash("sha256").update(data).digest("hex");
const normalizedSha = (path) => sha(readFileSync(path).toString("utf8").replace(/\r\n/g, "\n"));
const report = {
  passed: true, controls: 4, schema_generation: 1, tree_sha256: sha(treeBytes),
  checker_sha256: normalizedSha(fileURLToPath(import.meta.url)),
  model_sha256: normalizedSha(fileURLToPath(new URL("../../web/src/model.js", import.meta.url))),
  browser_exercised: false, script_worker_exercised: false,
  control_editors: script.controls.map(({ id, kind, editor }) => ({ id, kind, editor })),
};
writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify({ passed: true, controls: 4, schema_generation: 1 }));
