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
