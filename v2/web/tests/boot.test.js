import assert from "node:assert/strict";
import test from "node:test";
import { startInterface } from "../src/boot.js";

function fixture() {
  const values = new Map(), elements = new Map();
  return {
    storage: { getItem: key => values.get(key) ?? null, setItem: (key, value) => values.set(key, value), removeItem: key => values.delete(key) },
    document: { querySelector: selector => {
      if (!elements.has(selector)) elements.set(selector, { addEventListener(name, callback) { this[name] = callback; } });
      return elements.get(selector);
    } },
    values,
  };
}

test("startup consumes imports sequentially and clears the reload budget on success", async () => {
  const options = fixture(); let active = 0;
  options.storage.setItem("blip.interface-startup.0.3.10", "2");
  const names = [];
  assert.equal(await startInterface({ ...options, loadModule: async name => {
    assert.equal(active++, 0); await new Promise(resolve => setImmediate(resolve)); names.push(name); active--;
  } }), true);
  assert.equal(names.at(-1), "app"); assert.ok(names.indexOf("files") < names.indexOf("client"));
  assert.equal(options.values.size, 0);
});

test("cached import failures get two reloads across documents and then an explicit retry", async () => {
  const options = fixture(); let reloads = 0; const waits = [];
  const loadModule = async () => { throw new TypeError("Failed to fetch dynamically imported module"); };
  const run = () => startInterface({ ...options, loadModule, reload: () => reloads++, wait: async ms => waits.push(ms) });
  await run(); await run(); await run();
  assert.equal(reloads, 2); assert.deepEqual(waits, [1000, 2000]);
  const button = options.document.querySelector("#connect-button");
  assert.equal(button.disabled, false); assert.equal(button.textContent, "Retry loading");
  let prevented = false; button.click({ preventDefault() { prevented = true; } });
  assert.equal(prevented, true); assert.equal(reloads, 3); assert.equal(options.values.size, 0);
  await run(); assert.equal(reloads, 4, "deliberate retry starts a fresh bounded attempt");
});

test("unavailable storage and invalid modules leave a usable recovery page", async () => {
  for (const error of [new TypeError("network"), new SyntaxError("invalid module")]) {
    const options = fixture(); let reloads = 0;
    if (error instanceof TypeError) options.storage = { getItem() { throw new Error("blocked storage"); } };
    await startInterface({ ...options, loadModule: async () => { throw error; }, reload: () => reloads++ });
    assert.equal(reloads, 0); assert.equal(options.document.querySelector("#notice").hidden, false);
    assert.equal(options.document.querySelector("#connection-status").textContent, "Loading error");
  }
});
