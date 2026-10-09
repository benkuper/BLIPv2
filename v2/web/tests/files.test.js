import assert from "node:assert/strict";
import test from "node:test";
import { filePath, normalizeFilePage, loadStoredScript, MAX_FILE_BYTES, FilePanel } from "../src/files.js";
import { DeviceClient } from "../src/client.js";
import { encodeOscMessage } from "../src/osc.js";

class Socket {
  constructor() { this.readyState = 1; this.listeners = {}; this.sent = []; }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  send(data) { this.sent.push(data); }
  close() { this.readyState = 3; this.listeners.close?.(); }
  reply(address, values) { this.listeners.message?.({ data: encodeOscMessage(address, values) }); }
}
const entry = path => ({ path, directory: false, file: true, external: true, unreadable: false });
const page = (files = [], cursor = "", more = false) => ({ schema: 1, directory: "scripts", files, cursor, more });
const values = (...items) => items.map(value => ({ type: typeof value === "boolean" ? "boolean" : "integer", value }));

test("file pages accept deletion-only cursors, nested folders and combined file/folder names", () => {
  assert.equal(filePath("scripts/shows/main.wasm"), "scripts/shows/main.wasm");
  assert.equal(filePath("playback", true), "playback");
  for (const path of ["web/a", "scripts", "scripts/../a", "scripts/a..b", "scripts/./a", "scripts/a//b", "scripts/a b", "scripts/a?x", "scripts/" + "a".repeat(89)])
    assert.throws(() => filePath(path));
  assert.equal(normalizeFilePage(page([], "deleted", true), "scripts").more, true);
  const both = { ...entry("scripts/show"), directory: true };
  assert.equal(normalizeFilePage(page([both], "show"), "scripts").files[0].directory, true);
  for (const bad of [page([], "", true), page([], "a", true), page([entry("scripts/z")], "b"),
    page([entry("playback/z")], "z"), page([entry("scripts/sub/z")], "z"),
    page([entry("scripts/b"), entry("scripts/b")], "b"), page([entry("scripts/c"), entry("scripts/b")], "c")])
    assert.throws(() => normalizeFilePage(bad, "scripts", "a"));
});

test("file transfers stay on the selected device, pass cancellation, and bound downloads", async () => {
  const calls = [], abort = new AbortController();
  const client = new DeviceClient({ baseUrl: "http://192.0.2.8", WebSocketImpl: Socket,
    fetchImpl: async (url, options) => { calls.push({ url, options });
      if (url.pathname.endsWith("/")) return new Response(JSON.stringify(page([entry("scripts/z")], "z")));
      return new Response(new Uint8Array([1, 2, 3]));
    } });
  const result = await client.listFiles("scripts", "a", abort.signal);
  assert.equal(result.files[0].path, "scripts/z");
  assert.equal(calls[0].url.href, "http://192.0.2.8/api/files/scripts/?after=a");
  const body = new Blob([new Uint8Array([1, 2])]);
  await client.writeFile("scripts/a", body, abort.signal);
  assert.equal(calls[1].options.body, body); assert.equal(calls[1].options.signal, abort.signal);
  assert.deepEqual(new Uint8Array(await (await client.readFile("scripts/a")).arrayBuffer()), new Uint8Array([1, 2, 3]));
  await client.deleteFile("scripts/a"); assert.equal(calls.at(-1).options.method, "DELETE");
  await assert.rejects(() => client.writeFile("scripts/a", { size: MAX_FILE_BYTES + 1 }), /16 MiB/);
  await assert.rejects(() => client.listFiles("scripts", "../x"), /cursor/);
  let cancelled = false, reads = 0;
  client.fetchImpl = async () => ({ ok: true, body: new ReadableStream({
    pull(controller) { reads++; controller.enqueue(new Uint8Array(MAX_FILE_BYTES)); },
    cancel() { cancelled = true; },
  }) });
  await assert.rejects(() => client.readFile("scripts/a"), /exceeds/);
  assert.equal(cancelled, true); assert.ok(reads <= 3);
});

test("action promises resolve matching replies, reject disconnects, and never retry uncertain actions", async t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const client = new DeviceClient({ WebSocketImpl: Socket });
  let observed = 0; client.open({ onMessage() { observed++; } });
  const socket = client.socket;
  const pending = client.request("/load", []);
  await assert.rejects(() => client.request("/load"), /pending/);
  assert.throws(() => client.send("/load"), /pending/);
  socket.reply("/other", []); socket.reply("/load", values(8));
  assert.equal((await pending)[0].value, 8); assert.equal(observed, 2);
  const uncertain = client.request("/load", [], { timeout: 100 });
  const rejected = assert.rejects(uncertain, /may have run/);
  t.mock.timers.tick(100); await rejected;
  socket.reply("/load", values(9));
  await assert.rejects(() => client.request("/load"), /reconnect/);
  assert.equal(socket.sent.length, 2);
  client.open();
  const disconnected = client.request("/load", []);
  const closed = assert.rejects(disconnected, /not retried/); client.socket.close(); await closed;
  t.mock.timers.tick(500); assert.equal(client.socket.sent.length, 0);
  client.close();
});

test("stored script loading waits for bounded completion and exposes device errors", async () => {
  let polls = 0, clock = 0, admissions = 0;
  const client = { request: async path => {
    if (path === "/load") { admissions++; return values(42); }
    return values(++polls > 2, 0, 0, 100);
  } };
  const paths = { load: "/load", completion: "/done" };
  assert.equal(await loadStoredScript(client, paths, "scripts/a", { now: () => clock, wait: async ms => { clock += ms; } }), 42);
  assert.equal(admissions, 1); assert.equal(polls, 3);
  client.request = async path => path === "/load" ? values(43) : values(true, 22, 0, 0);
  await assert.rejects(() => loadStoredScript(client, paths, "scripts/missing"), /not found/);
  const abort = new AbortController(); abort.abort();
  await assert.rejects(() => loadStoredScript(client, paths, "scripts/a", { signal: abort.signal }), { name: "AbortError" });
  client.request = async path => path === "/load" ? values(44) : values(false, 0, 0, 0);
  clock = 0;
  await assert.rejects(() => loadStoredScript(client, paths, "scripts/a", { now: () => clock, wait: async () => { clock += 60000; } }), /still running/);
});

test("file navigation rejects stale device responses and keeps transfers exclusive", async () => {
  // Minimal DOM surface: real rendering and transfers are exercised by browser HIL.
  class Element {
    constructor() { this.children = []; this.listeners = {}; this.dataset = {}; this.value = ""; }
    querySelector(selector) { return this.elements[selector]; }
    querySelectorAll() { return []; }
    addEventListener(name, callback) { this.listeners[name] = callback; }
    append(...children) { this.children.push(...children); }
    replaceChildren() { this.children = []; }
    setAttribute() {}
  }
  const root = new Element(); root.elements = {};
  for (const key of ["[data-file-status]", "[data-file-rows]", "form", "[data-file-input]", "[data-file-path]", "[data-file-cancel]", "[data-file-refresh]", "[data-file-more]", "[data-file-breadcrumbs]", "[data-file-medium]"])
    root.elements[key] = new Element();
  const panel = new FilePanel({ document: { createElement: () => new Element() }, root });
  let resolve;
  panel.connect({ listFiles: () => new Promise(done => { resolve = done; }) });
  const pending = panel.run(() => panel.page(true));
  assert.equal(panel.busy, true);
  panel.navigate("playback"); assert.equal(panel.directory, "scripts");
  panel.connect({ listFiles: async () => page([entry("scripts/new")], "new") });
  resolve(page([entry("scripts/old")], "old")); await pending;
  assert.equal(panel.entries.length, 0, "old device response is discarded");
  await panel.run(() => panel.page(true)); assert.equal(panel.entries[0].path, "scripts/new");
  const model = { components: [{ id: "blip.wasm", controls: [
    { id: "load_file", path: "/load" }, { id: "completion", path: "/done" }, { id: "level", value: 5 },
  ] }] };
  panel.setModel(model);
  const stable = panel.rows.children[0]; model.components[0].controls[2].value = 42;
  panel.setModel(model); assert.equal(panel.rows.children[0], stable, "unrelated schema/value refresh preserves file buttons");
  panel.isUpdating = () => true;
  let called = false; await panel.run(async () => { called = true; }); assert.equal(called, false);
});

test("read retries discard partial downloads, stay bounded and never repeat writes", async () => {
  let calls = 0, cancelled = 0;
  const client = new DeviceClient({ WebSocketImpl: Socket, fetchImpl: async () => {
    if (++calls > 1) return new Response(new Uint8Array([7, 8]));
    let reads = 0;
    return { ok: true, body: { getReader: () => ({
      read: async () => { if (++reads === 1) return { done: false, value: new Uint8Array([99]) }; throw new TypeError("network error"); },
      cancel: async () => { cancelled++; }, releaseLock() {},
    }) } };
  } });
  assert.deepEqual(new Uint8Array(await (await client.readFile("scripts/a")).arrayBuffer()), new Uint8Array([7, 8]));
  assert.equal(calls, 2); assert.equal(cancelled, 1);
  calls = 0; client.fetchImpl = async () => { calls++; throw new TypeError("fetch failed"); };
  await assert.rejects(() => client.loadTree(), /fetch failed/); assert.equal(calls, 3);
  calls = 0;
  client.fetchImpl = async () => { calls++; throw new TypeError("fetch failed"); };
  await assert.rejects(() => client.loadTree(() => calls === 0), { name: "AbortError" });
  assert.equal(calls, 1, "a newly admitted transfer suppresses a queued heavy-read retry");
  calls = 0;
  await assert.rejects(() => client.writeFile("scripts/a", new Blob(["x"])), /fetch failed/);
  assert.equal(calls, 1, "an uncertain PUT must not be replayed");
  calls = 0;
  await assert.rejects(() => client.deleteFile("scripts/a"), /fetch failed/); assert.equal(calls, 1);
  const abort = new AbortController(); calls = 0;
  const pending = client.readFile("scripts/a", abort.signal);
  await new Promise(resolve => setImmediate(resolve)); abort.abort();
  await assert.rejects(pending, { name: "AbortError" }); assert.equal(calls, 1, "cancel stops the retry wait");
  calls = 0; client.fetchImpl = async () => { calls++; return new Response("missing", { status: 404 }); };
  await assert.rejects(() => client.readFile("scripts/a"), /404/); assert.equal(calls, 1);
});

test("file writes and release admissions consume their replies before returning", async () => {
  let pending = false, consumed = 0;
  const client = new DeviceClient({ WebSocketImpl: Socket, fetchImpl: async (url, options) => {
    assert.equal(pending, false, "the preceding response still holds a device connection");
    assert.notEqual(options.method, "GET"); pending = true;
    return { ok: true, status: url.pathname.startsWith("/api/releases/") ? 202 : 201,
      text: async () => { await new Promise(resolve => setImmediate(resolve)); pending = false; consumed++; return "accepted"; } };
  } });
  await client.writeFile("scripts/a", new Blob(["x"]));
  await client.deleteFile("scripts/a"); await client.release("check");
  assert.equal(pending, false); assert.equal(consumed, 3);
});
