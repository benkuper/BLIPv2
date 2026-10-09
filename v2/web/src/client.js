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
    // Window.fetch requires the Window receiver in browsers. Calling a stored
    // native fetch as this.fetchImpl otherwise binds it to DeviceClient.
    this.fetchImpl = fetchImpl.bind(globalThis);
    this.WebSocketImpl = WebSocketImpl;
    this.socket = null;
  }

  async load(includeConfig = true) {
    const hostUrl = new URL(this.root);
    hostUrl.search = "HOST_INFO";
    const treeUrl = new URL(this.root);
    treeUrl.search = includeConfig ? "config=1" : "config=0";
    const resourceUrl = new URL("/api/resources", this.root);
    // Consume each response before opening the next. Small boards share their
    // TCP buffers with BLE, storage and the update downloader.
    const host = await boundedJson(await this.fetchImpl(hostUrl, { cache: "no-store" }), MAX_HOST_INFO_BYTES);
    const tree = await boundedJson(await this.fetchImpl(treeUrl, { cache: "no-store" }), MAX_TREE_BYTES);
    const resources = await boundedJson(await this.fetchImpl(resourceUrl, { cache: "no-store" }), MAX_RESOURCE_BYTES);
    return {
      host, tree, resources,
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

  async loadTree() {
    const url = new URL(this.root); url.search = "config=1";
    return boundedJson(await this.fetchImpl(url, { cache: "no-store" }), MAX_TREE_BYTES);
  }

  async loadReleases() {
    return boundedJson(await this.fetchImpl(new URL("/api/releases", this.root), { cache: "no-store" }), 4096);
  }

  async release(action) {
    if (!["check", "firmware", "web", "cancel"].includes(action)) throw new TypeError("Unknown update action");
    const response = await this.fetchImpl(new URL(`/api/releases/${action}`, this.root), { method: "POST", cache: "no-store" });
    if (response.status !== 202) throw new Error(`Update request failed (HTTP ${response.status}): ${(await response.text()).slice(0, 256)}`);
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
