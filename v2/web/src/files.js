export const MAX_FILE_BYTES = 16 * 1024 * 1024;
const NAMESPACES = ["scripts", "playback", "sequences"];
const SCRIPT_ERRORS = ["none", "invalid argument", "invalid state", "capacity exceeded", "duplicate ID",
  "missing dependency", "dependency cycle", "validation failed", "start failed", "suspend failed",
  "resume failed", "stop failed", "serialization overflow", "resource unavailable", "resource conflict",
  "resource reserved", "queue full", "queue faulted", "budget exceeded", "cancelled", "wrong task context",
  "recursive dispatch", "not found", "I/O failed", "storage full", "corrupt data", "incompatible version",
  "verification failed", "generation exhausted"];

export function filePath(path, directory = false) {
  if (typeof path !== "string" || !path || path.length > 96 || path.includes("..") || !/^[A-Za-z0-9_./-]+$/.test(path))
    throw new TypeError("Use a path of at most 96 letters, digits, dots, dashes, underscores and slashes");
  const parts = path.split("/");
  if (!NAMESPACES.includes(parts[0]) || (!directory && parts.length < 2) || parts.some(part => !part || part === "." || part === ".."))
    throw new TypeError("Choose a file inside Scripts, Playback or Sequences");
  return path;
}

export function normalizeFilePage(page, directory, after = "") {
  filePath(directory, true);
  if (!page || page.schema !== 1 || page.directory !== directory || typeof page.more !== "boolean" ||
      typeof page.cursor !== "string" || !Array.isArray(page.files) || page.files.length > 4 ||
      (page.cursor && (!/^[A-Za-z0-9_.-]+$/.test(page.cursor) || page.cursor === "." || page.cursor.includes("..") || page.cursor.length > 96)) ||
      (page.cursor && page.cursor <= after) || (page.more && !page.cursor)) throw new TypeError("Invalid file listing");
  let previous = after;
  for (const entry of page.files) {
    filePath(entry?.path);
    const name = entry.path.slice(directory.length + 1);
    if (!entry.path.startsWith(directory + "/") || !name || name.includes("/") || name <= previous || name > page.cursor ||
        ["directory", "file", "external", "unreadable"].some(key => typeof entry[key] !== "boolean") ||
        (!entry.directory && !entry.file)) throw new TypeError("Invalid file entry");
    previous = name;
  }
  return page;
}

function abortError() { return new DOMException("Cancelled", "AbortError"); }
function delay(ms, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) { reject(abortError()); return; }
    const finish = () => { signal?.removeEventListener("abort", cancel); resolve(); };
    const timer = setTimeout(finish, ms);
    const cancel = () => { clearTimeout(timer); signal.removeEventListener("abort", cancel); reject(abortError()); };
    signal?.addEventListener("abort", cancel, { once: true });
  });
}

export async function loadStoredScript(client, paths, path, { signal, wait = delay, now = Date.now } = {}) {
  filePath(path);
  if (!path.startsWith("scripts/")) throw new TypeError("Choose a stored script");
  if (signal?.aborted) throw abortError();
  const reply = await client.request(paths.load, [{ type: "string", value: path }]);
  const token = reply[0]?.value;
  if (reply.length !== 1 || !Number.isInteger(token) || token <= 0 || token > 0x7fffffff) throw new Error("Invalid script work ID");
  const deadline = now() + 60000;
  for (;;) {
    if (signal?.aborted) throw abortError();
    const values = await client.request(paths.completion, [{ type: "integer", value: token }]);
    if (values.length !== 4 || typeof values[0]?.value !== "boolean" ||
        !Number.isInteger(values[1]?.value) || values[1].value < 0 || values[1].value >= SCRIPT_ERRORS.length ||
        !Number.isInteger(values[2]?.value) || !Number.isInteger(values[3]?.value))
      throw new Error("Invalid script completion reply");
    if (values[0].value) {
      if (values[1].value !== 0) throw new Error(`Script could not load: ${SCRIPT_ERRORS[values[1].value]}`);
      return token;
    }
    if (now() >= deadline) throw new Error("Script is still running. Check its state before loading again.");
    await wait(250, signal);
  }
}

export class FilePanel {
  constructor({ document, root, isUpdating = () => false, onLoaded = () => {} }) {
    this.document = document; this.root = root; this.isUpdating = isUpdating; this.onLoaded = onLoaded;
    this.client = null; this.busy = false; this.visible = false; this.directory = "scripts"; this.cursor = "";
    this.more = false; this.entries = []; this.loaded = false;
    this.status = root.querySelector("[data-file-status]"); this.rows = root.querySelector("[data-file-rows]");
    this.form = root.querySelector("form"); this.input = root.querySelector("[data-file-input]");
    this.path = root.querySelector("[data-file-path]"); this.cancel = root.querySelector("[data-file-cancel]");
    for (const button of root.querySelectorAll("[data-file-namespace]"))
      button.addEventListener("click", () => this.navigate(button.dataset.fileNamespace));
    root.querySelector("[data-file-refresh]").addEventListener("click", () => this.navigate(this.directory));
    root.querySelector("[data-file-more]").addEventListener("click", () => this.run(() => this.page(false)));
    this.cancel.addEventListener("click", () => this.abort?.abort());
    this.input.addEventListener("change", () => { if (this.input.files?.[0]) this.path.value = this.input.files[0].name; });
    this.form.addEventListener("submit", event => {
      event.preventDefault();
      this.run(async () => {
        const file = this.input.files?.[0]; if (!file) throw new Error("Choose a file first");
        const path = filePath(this.directory + "/" + this.path.value.trim());
        this.status.textContent = "Saving file…";
        await this.client.writeFile(path, file, this.abort.signal);
        await this.page(true); this.status.textContent = "Saved on device"; this.form.reset();
      });
    });
    this.render();
  }
  connect(client) {
    this.abort?.abort(); this.client = client; this.entries = []; this.loaded = false; this.cursor = ""; this.more = false;
    this.render(); if (this.visible && !this.busy) this.navigate(this.directory);
  }
  setModel(model) {
    const controls = model.components.find(c => c.id === "blip.wasm")?.controls ?? [];
    const paths = { load: controls.find(c => c.id === "load_file")?.path, completion: controls.find(c => c.id === "completion")?.path };
    const changed = paths.load !== this.paths?.load || paths.completion !== this.paths?.completion;
    this.paths = paths;
    const medium = model.components.find(c => c.id === "blip.storage.files.internal")?.controls.find(c => c.id === "preferred_medium")?.value;
    this.root.querySelector("[data-file-medium]").textContent = medium && medium !== "internal" ?
      `External storage · ${medium} · internal fallback` : "Internal storage";
    if (changed) this.render();
  }
  setVisible(visible) { this.visible = visible; if (visible && this.client && !this.loaded && !this.busy) this.navigate(this.directory); }
  navigate(directory) {
    if (this.busy) return;
    filePath(directory, true); this.directory = directory; this.loaded = false;
    this.run(() => this.page(true));
  }
  async page(reset) {
    const cursor = reset ? "" : this.cursor;
    const page = await this.client.listFiles(this.directory, cursor, this.abort.signal);
    if (this.abort.signal.aborted) throw abortError();
    this.cursor = page.cursor; this.more = page.more; this.loaded = true;
    this.entries = reset ? page.files : [...this.entries, ...page.files];
    this.status.textContent = this.more ? "More files available" : `${this.entries.length} items`;
    this.render();
  }
  async run(work, cancellable = true) {
    if (this.busy) return;
    if (!this.client) { this.status.textContent = "Connect to a device first"; return; }
    if (this.isUpdating()) { this.status.textContent = "Wait for the device update to finish"; return; }
    const client = this.client;
    this.busy = true; this.abort = new AbortController(); this.render(); this.cancel.hidden = !cancellable;
    try { await work(); }
    catch (error) { if (client === this.client) this.status.textContent = error.name === "AbortError" ? "Cancelled. Refresh to check the saved files." : error.message; }
    finally {
      this.busy = false; this.cancel.hidden = true;
      if (client !== this.client) { this.loaded = false; if (this.visible) this.navigate(this.directory); }
      else this.render();
    }
  }
  button(label, action, className = "secondary-button") {
    const button = this.document.createElement("button"); button.type = "button"; button.className = className;
    button.textContent = label; button.disabled = this.busy; button.addEventListener("click", action); return button;
  }
  render() {
    for (const button of this.root.querySelectorAll("button")) if (button !== this.cancel) button.disabled = this.busy;
    for (const input of this.root.querySelectorAll("input")) input.disabled = this.busy;
    this.root.setAttribute("aria-busy", String(this.busy));
    for (const button of this.root.querySelectorAll("[data-file-namespace]"))
      button.setAttribute("aria-pressed", String(this.directory.split("/")[0] === button.dataset.fileNamespace));
    const crumbs = this.root.querySelector("[data-file-breadcrumbs]"); crumbs.replaceChildren();
    const parts = this.directory.split("/");
    for (let i = 0; i < parts.length; i++) {
      const directory = parts.slice(0, i + 1).join("/");
      crumbs.append(this.button(parts[i], () => this.navigate(directory)));
    }
    this.rows.replaceChildren();
    for (const entry of this.entries) {
      const row = this.document.createElement("li"), details = this.document.createElement("div"), name = this.document.createElement("strong"), meta = this.document.createElement("small");
      name.textContent = entry.path.slice(this.directory.length + 1);
      meta.textContent = entry.unreadable ? "Metadata unreadable · replace or delete this file" :
        [entry.directory ? "Folder" : "", entry.file ? (entry.external ? "External storage" : "Internal storage") : ""].filter(Boolean).join(" · ");
      details.append(name, meta); row.append(details);
      const actions = this.document.createElement("div"); actions.className = "file-actions";
      if (entry.directory) actions.append(this.button("Open folder", () => this.navigate(entry.path)));
      if (entry.file) {
        actions.append(this.button("Download", () => this.run(async () => {
          this.status.textContent = "Checking and downloading file…";
          const blob = await this.client.readFile(entry.path, this.abort.signal);
          if (this.abort.signal.aborted) throw abortError();
          const url = URL.createObjectURL(blob), anchor = this.document.createElement("a");
          anchor.href = url; anchor.download = name.textContent; anchor.click();
          setTimeout(() => URL.revokeObjectURL(url), 1000); this.status.textContent = "Download ready";
        })));
        if (entry.path.startsWith("scripts/") && this.paths?.load && this.paths?.completion)
          actions.append(this.button("Load script", () => this.run(async () => {
            this.status.textContent = "Checking and loading script…";
            await loadStoredScript(this.client, this.paths, entry.path);
            this.status.textContent = "Script loaded · controls are refreshing"; this.onLoaded();
          }, false), "apply-button"));
        actions.append(this.button("Delete", () => this.run(async () => {
          await this.client.deleteFile(entry.path, this.abort.signal); await this.page(true);
          this.status.textContent = "File deleted";
        })));
      }
      row.append(actions); this.rows.append(row);
    }
    if (!this.entries.length) { const empty = this.document.createElement("li"); empty.className = "file-empty";
      empty.textContent = this.loaded ? "No files on this page. Upload a file or continue browsing." : "Refresh to browse files"; this.rows.append(empty); }
    this.root.querySelector("[data-file-more]").hidden = !this.more;
  }
}
