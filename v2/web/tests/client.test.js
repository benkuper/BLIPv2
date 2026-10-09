import assert from "node:assert/strict";
import test from "node:test";

import { DeviceClient, deviceUrlFromLocation } from "../src/client.js";

class ClosedSocket {
  constructor() {
    this.readyState = 0;
  }
  addEventListener() {}
  close() {}
}

test("release checks and independent installations use bounded same-device endpoints", async () => {
  const calls = [];
  const client = new DeviceClient({ baseUrl: "http://192.0.2.8", WebSocketImpl: ClosedSocket,
    fetchImpl: async (url, options) => {
      calls.push([url.href, options.method ?? "GET"]);
      return options.method === "POST" ? new Response('{"accepted":true}', { status: 202 }) : response({ state: "current" });
    } });
  assert.equal((await client.loadReleases()).state, "current");
  await client.release("check"); await client.release("web"); await client.release("firmware"); await client.release("cancel");
  await assert.rejects(() => client.release("erase"), /Unknown update action/);
  assert.deepEqual(calls.map(([url]) => new URL(url).pathname), ["/api/releases", "/api/releases/check", "/api/releases/web", "/api/releases/firmware", "/api/releases/cancel"]);
  const oversized = new DeviceClient({ baseUrl: "http://192.0.2.8", WebSocketImpl: ClosedSocket,
    fetchImpl: async () => new Response("x".repeat(4097)) });
  await assert.rejects(() => oversized.loadReleases(), /too large/);
});

test("native fetch keeps its global receiver for polling and connection", async () => {
  const client = new DeviceClient({ baseUrl: "http://192.0.2.8", WebSocketImpl: ClosedSocket,
    fetchImpl: function () { assert.equal(this, globalThis); return Promise.resolve(response({ CONTENTS: {} })); } });
  await client.load();
  await client.loadTree();
});

function response(value) {
  return new Response(JSON.stringify(value), {
    status: 200,
    headers: { "content-type": "application/json" },
  });
}

test("client requests host info and the selected registry view", async () => {
  const requested = [];
  const fetchImpl = async (url) => {
    requested.push(url.href);
    if (url.pathname === "/api/resources") {
      return response({ schema_version: 1, allocation_revision: 1, board: {}, pins: [] });
    }
    return response(url.search === "?HOST_INFO" ? { NAME: "Fixture" } : { CONTENTS: {} });
  };
  const client = new DeviceClient({
    baseUrl: "http://192.0.2.8/setup?ignored=1",
    fetchImpl,
    WebSocketImpl: ClosedSocket,
  });
  const result = await client.load(false);
  assert.equal(result.host.NAME, "Fixture");
  assert.deepEqual(requested, [
    "http://192.0.2.8/?HOST_INFO",
    "http://192.0.2.8/?config=0",
    "http://192.0.2.8/api/resources",
  ]);
  assert.throws(() => client.send("/test"), /offline/);
});

test("initial connection consumes each JSON body before opening another request", async () => {
  let pending = false;
  const client = new DeviceClient({ baseUrl: "http://192.0.2.8", WebSocketImpl: ClosedSocket,
    fetchImpl: async () => {
      assert.equal(pending, false, "response body still occupies device TCP buffers");
      pending = true;
      return { ok: true, headers: new Headers(), text: async () => {
        await new Promise(resolve => setTimeout(resolve, 1)); pending = false; return "{}";
      } };
    } });
  await client.load();
  assert.equal(pending, false);
});

test("device query parameter supports a separately hosted shell", () => {
  assert.equal(
    deviceUrlFromLocation({ href: "http://localhost:8080/?device=http%3A%2F%2F192.0.2.9" }),
    "http://192.0.2.9",
  );
});

test("non-HTTP device URLs are rejected", () => {
  assert.throws(
    () =>
      new DeviceClient({
        baseUrl: "file:///device",
        fetchImpl: async () => response({}),
        WebSocketImpl: ClosedSocket,
      }),
    /HTTP or HTTPS/,
  );
});

test("oversized schema responses are rejected before parsing", async () => {
  const fetchImpl = async (url) => {
    if (url.search === "?HOST_INFO") return response({ NAME: "Fixture" });
    if (url.pathname === "/api/resources") {
      return response({ schema_version: 1, allocation_revision: 1, board: {}, pins: [] });
    }
    return new Response("{}", {
      status: 200,
      headers: { "content-length": String(256 * 1024 + 1) },
    });
  };
  const client = new DeviceClient({
    baseUrl: "http://192.0.2.8",
    fetchImpl,
    WebSocketImpl: ClosedSocket,
  });
  await assert.rejects(() => client.load(), /too large/);
});
