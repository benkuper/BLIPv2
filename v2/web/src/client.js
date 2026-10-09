import { decodeOscMessage, encodeOscMessage } from "./osc.js";
import { filePath, normalizeFilePage, MAX_FILE_BYTES } from "./files.js";

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

async function retryRead(work, signal, permitted = () => true) {
  for (let attempt = 0; ; attempt++) {
    if (!permitted()) throw new DOMException("Readings paused", "AbortError");
    try { return await work(); }
    catch (error) {
      if (attempt === 2 || signal?.aborted || !(error instanceof TypeError || error.name === "NetworkError")) throw error;
      await new Promise((resolve, reject) => {
        const finish = () => { signal?.removeEventListener("abort", cancel); resolve(); };
        const timer = setTimeout(finish, 250 * (attempt + 1));
        const cancel = () => { clearTimeout(timer); signal.removeEventListener("abort", cancel); reject(signal.reason); };
        signal?.addEventListener("abort", cancel, { once: true });
      });
    }
  }
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
    this.pending = new Map(); this.blocked = new Set();
  }

  async load(includeConfig = true) {
    const hostUrl = new URL(this.root);
    hostUrl.search = "HOST_INFO";
    const treeUrl = new URL(this.root);
    treeUrl.search = includeConfig ? "config=1" : "config=0";
    const resourceUrl = new URL("/api/resources", this.root);
    // Consume each response before opening the next. Small boards share their
    // TCP buffers with BLE, storage and the update downloader.
    const host = await this.readJson(hostUrl, MAX_HOST_INFO_BYTES);
    const tree = await this.readJson(treeUrl, MAX_TREE_BYTES);
    const resources = await this.readJson(resourceUrl, MAX_RESOURCE_BYTES);
    return {
      host, tree, resources,
    };
  }

  readJson(url, maximum, signal, permitted) {
    return retryRead(async () => boundedJson(await this.fetchImpl(url, { cache: "no-store", signal }), maximum), signal, permitted);
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

  async loadTree(permitted) {
    const url = new URL(this.root); url.search = "config=1";
    return this.readJson(url, MAX_TREE_BYTES, undefined, permitted);
  }

  async loadReleases() {
    return this.readJson(new URL("/api/releases", this.root), 4096);
  }

  async release(action) {
    if (!["check", "firmware", "web", "cancel"].includes(action)) throw new TypeError("Unknown update action");
    const response = await this.fetchImpl(new URL(`/api/releases/${action}`, this.root), { method: "POST", cache: "no-store" });
    if (response.status !== 202) throw new Error(`Update request failed (HTTP ${response.status}): ${(await response.text()).slice(0, 256)}`);
    await response.text();
  }

  async listFiles(directory, after = "", signal) {
    filePath(directory, true);
    if (after && (!/^[A-Za-z0-9_.-]+$/.test(after) || after === "." || after.includes("..") || after.length > 96))
      throw new TypeError("Invalid file cursor");
    const url = new URL(`/api/files/${directory}/`, this.root);
    if (after) url.searchParams.set("after", after);
    return normalizeFilePage(await this.readJson(url, 4096, signal), directory, after);
  }

  async writeFile(path, file, signal) {
    filePath(path);
    if (!Number.isSafeInteger(file?.size) || file.size < 0 || file.size > MAX_FILE_BYTES)
      throw new RangeError("Files must be at most 16 MiB");
    await this.fileRequest("PUT", path, file, signal);
  }

  async deleteFile(path, signal) { await this.fileRequest("DELETE", path, undefined, signal); }

  async fileRequest(method, path, body, signal) {
    filePath(path);
    const response = await this.fetchImpl(new URL(`/api/files/${path}`, this.root), {
      method, cache: "no-store", headers: { "Content-Type": "application/octet-stream" }, body, signal,
    });
    if (!response.ok) throw new Error(`File request failed (HTTP ${response.status}): ${(await response.text()).slice(0, 160)}`);
    // Finish short mutation replies before opening another request. Leaving
    // their bodies unread retains a browser connection in the device's pool.
    if (method !== "GET") {
      const result = await response.text();
      if (new TextEncoder().encode(result).length > 256) throw new RangeError("File reply is too large");
    }
    return response;
  }

  async readFile(path, signal) {
    filePath(path);
    return retryRead(() => this.readFileOnce(path, signal), signal);
  }

  async readFileOnce(path, signal) {
    const response = await this.fileRequest("GET", path, undefined, signal);
    const reader = response.body.getReader(), chunks = [];
    let size = 0, finished = false;
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) { finished = true; break; }
        size += value.byteLength;
        if (size > MAX_FILE_BYTES) throw new RangeError("File download exceeds 16 MiB");
        chunks.push(value);
      }
      return new Blob(chunks, { type: "application/octet-stream" });
    } finally { if (!finished) await reader.cancel().catch(() => {}); reader.releaseLock(); }
  }

  open({ onMessage, onState, onError } = {}) {
    this.close();
    const generation = this.socketGeneration;
    let failures = 0;
    const url = new URL(this.root);
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    const connect = () => {
      if (this.socketGeneration !== generation) return;
      const socket = new this.WebSocketImpl(url);
      socket.binaryType = "arraybuffer";
      socket.addEventListener("open", () => {
        if (this.socket === socket) {
          failures = 0; onState?.("online");
          const heartbeat = () => {
            if (this.socket !== socket || socket.readyState !== 1) return;
            // Keep live control newer than idle HTTP sessions in the device's
            // bounded LRU pool. Text diagnostics are acknowledged without
            // changing parameters or entering the script worker.
            socket.send('{"COMMAND":"BLIP_PING"}');
            this.heartbeatTimer = setTimeout(heartbeat, 1500);
          };
          this.heartbeatTimer = setTimeout(heartbeat, 1500);
        }
      });
      socket.addEventListener("close", () => {
        if (this.socket === socket) {
          clearTimeout(this.heartbeatTimer);
          this.socket = null;
          this.rejectRequests(new Error("Device disconnected; the action was not retried"));
          this.blocked.clear();
          onState?.("reconnecting");
          this.reconnectTimer = setTimeout(connect, Math.min(10000, 500 * 2 ** Math.min(failures++, 5)));
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
          const message = decodeOscMessage(event.data);
          const pending = this.pending.get(message.address);
          if (pending) { this.pending.delete(message.address); clearTimeout(pending.timer); pending.resolve(message.values); }
          onMessage?.(message);
        } catch (error) {
          this.rejectRequests(error, true);
          onError?.(error);
        }
      });
      this.socket = socket;
    };
    connect();
  }

  send(path, values = []) {
    if (this.pending.has(path) || this.blocked.has(path)) throw new Error("This action is pending or needs a reconnect");
    if (this.socket === null || this.socket.readyState !== 1) {
      throw new Error("Device control socket is offline");
    }
    this.socket.send(encodeOscMessage(path, values));
  }

  request(path, values = [], { timeout = 10000 } = {}) {
    if (!Number.isFinite(timeout) || timeout < 1 || timeout > 60000) return Promise.reject(new TypeError("Invalid action timeout"));
    if (this.pending.has(path) || this.blocked.has(path) || this.pending.size >= 8)
      return Promise.reject(new Error("This action is pending or needs a reconnect"));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(path); this.blocked.add(path);
        reject(new Error("Action reply timed out. Reconnect before trying again; the action may have run."));
      }, timeout);
      // Register before send: a synchronous transport can reply immediately.
      this.pending.set(path, { resolve, reject, timer });
      try {
        if (this.socket?.readyState !== 1) throw new Error("Device control socket is offline");
        this.socket.send(encodeOscMessage(path, values));
      } catch (error) { this.pending.delete(path); clearTimeout(timer); reject(error); }
    });
  }

  rejectRequests(error, block = false) {
    for (const [path, pending] of this.pending) {
      clearTimeout(pending.timer); if (block) this.blocked.add(path); pending.reject(error);
    }
    this.pending.clear();
  }

  close() {
    this.rejectRequests(new Error("Device connection closed; the action was not retried")); this.blocked.clear();
    this.socketGeneration = (this.socketGeneration ?? 0) + 1;
    clearTimeout(this.reconnectTimer);
    clearTimeout(this.heartbeatTimer);
    const socket = this.socket;
    this.socket = null;
    socket?.close();
  }
}

export function deviceUrlFromLocation(locationLike = globalThis.location) {
  const page = new URL(locationLike.href);
  return page.searchParams.get("device") || page.origin;
}
