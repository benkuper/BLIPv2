import { decodeOscMessage, encodeOscMessage } from "./osc.js";

const MAX_HOST_INFO_BYTES = 16 * 1024;
const MAX_TREE_BYTES = 256 * 1024;
const MAX_RESOURCE_BYTES = 128 * 1024;

function deviceRoot(value) {
  const fallback = globalThis.location?.href ?? "http://localhost/";
  const url = new URL(value || fallback, fallback);
  if (url.protocol !== "http:" && url.protocol !== "https:") {
    throw new TypeError("Device URL must use HTTP or HTTPS");
  }
  url.pathname = "/";
  url.search = "";
  url.hash = "";
  return url;
}

async function boundedJson(response, maximum) {
  if (!response.ok) throw new Error(`Device returned HTTP ${response.status}`);
  const declared = Number(response.headers.get("content-length"));
  if (Number.isFinite(declared) && declared > maximum) {
    throw new RangeError("Device schema response is too large");
  }
  const source = await response.text();
  if (new TextEncoder().encode(source).length > maximum) {
    throw new RangeError("Device schema response is too large");
  }
  return JSON.parse(source);
}

export class DeviceClient {
  constructor({ baseUrl, fetchImpl = globalThis.fetch, WebSocketImpl = globalThis.WebSocket } = {}) {
    if (typeof fetchImpl !== "function" || typeof WebSocketImpl !== "function") {
      throw new TypeError("Browser fetch and WebSocket support are required");
    }
    this.root = deviceRoot(baseUrl);
    this.fetchImpl = fetchImpl;
    this.WebSocketImpl = WebSocketImpl;
    this.socket = null;
  }

  async load(includeConfig = true) {
    const hostUrl = new URL(this.root);
    hostUrl.search = "HOST_INFO";
    const treeUrl = new URL(this.root);
    treeUrl.search = includeConfig ? "config=1" : "config=0";
    const resourceUrl = new URL("/api/resources", this.root);
    const [hostResponse, treeResponse, resourceResponse] = await Promise.all([
      this.fetchImpl(hostUrl, { cache: "no-store" }),
      this.fetchImpl(treeUrl, { cache: "no-store" }),
      this.fetchImpl(resourceUrl, { cache: "no-store" }),
    ]);
    return {
      host: await boundedJson(hostResponse, MAX_HOST_INFO_BYTES),
      tree: await boundedJson(treeResponse, MAX_TREE_BYTES),
      resources: await boundedJson(resourceResponse, MAX_RESOURCE_BYTES),
    };
  }

  async reassign(request) {
    const response = await this.fetchImpl(new URL("/api/resources/reassign", this.root), {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(request),
    });
    if (response.status === 409) {
      const error = new Error("Pin reservations changed; review the refreshed owners");
      error.code = "stale-revision";
      throw error;
    }
    if (!response.ok) throw new Error(`Device rejected reassignment (HTTP ${response.status})`);
    return boundedJson(response, MAX_RESOURCE_BYTES);
  }

  open({ onMessage, onState, onError } = {}) {
    this.close();
    const url = new URL(this.root);
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    const socket = new this.WebSocketImpl(url);
    socket.binaryType = "arraybuffer";
    socket.addEventListener("open", () => {
      if (this.socket === socket) onState?.("online");
    });
    socket.addEventListener("close", () => {
      if (this.socket === socket) {
        this.socket = null;
        onState?.("offline");
      }
    });
    socket.addEventListener("error", () => {
      if (this.socket === socket) onError?.(new Error("WebSocket connection failed"));
    });
    socket.addEventListener("message", (event) => {
      if (this.socket !== socket) return;
      try {
        if (typeof event.data === "string") {
          const diagnostic = JSON.parse(event.data);
          if (diagnostic.ok === false) throw new Error(diagnostic.error || "Device rejected control");
          return;
        }
        onMessage?.(decodeOscMessage(event.data));
      } catch (error) {
        onError?.(error);
      }
    });
    this.socket = socket;
  }

  send(path, values = []) {
    if (this.socket === null || this.socket.readyState !== 1) {
      throw new Error("Device control socket is offline");
    }
    this.socket.send(encodeOscMessage(path, values));
  }

  close() {
    if (this.socket !== null) {
      this.socket.close();
      this.socket = null;
    }
  }
}

export function deviceUrlFromLocation(locationLike = globalThis.location) {
  const page = new URL(locationLike.href);
  return page.searchParams.get("device") || page.origin;
}
