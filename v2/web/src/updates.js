export function webVersion(code) {
  if (!Number.isSafeInteger(code) || code < 0 || code > 0xffffffff) return "Unavailable";
  return `${Math.floor(code / 1000000)}.${Math.floor(code / 1000) % 1000}.${code % 1000}`;
}

export function releaseSummary(state) {
  const labels = {
    "not-checked": "Ready when you are", checking: "Checking for updates…",
    current: "Everything is up to date", unpublished: "No release published for this device yet",
    "updates-available": "An update is ready", "downloading-web": "Downloading the interface…",
    "downloading-firmware": "Downloading firmware…", "web-installed": "Interface installed",
    restarting: "Firmware installed. Your BLIP is restarting…", cancelled: "Update cancelled",
    deferred: "Update check deferred", error: "The update could not finish",
  };
  return labels[state] ?? "Checking device status";
}

export class UpdatePanel {
  constructor({ document, root, onError, reload = () => location.reload() }) {
    this.root = root; this.onError = onError; this.reload = reload; this.client = null;
    this.status = root.querySelector("[data-update-status]");
    this.progress = root.querySelector("progress"); this.version = null;
    for (const button of root.querySelectorAll("[data-update-action]")) button.addEventListener("click", async () => {
      button.disabled = true;
      try { await this.client.release(button.dataset.updateAction); await this.refresh(); }
      catch (error) { this.status.textContent = error.message; this.onError?.(error); }
    });
  }
  connect(client) { this.client = client; this.version = null; this.refresh(); }
  async refresh() {
    clearTimeout(this.timer);
    const client = this.client;
    if (!client) return;
    try {
      const state = await client.loadReleases();
      if (client !== this.client) return;
      this.status.textContent = releaseSummary(state.state) + (state.error ? ` · ${state.error.replaceAll("-", " ")}` : "");
      this.root.querySelector("[data-firmware-version]").textContent = state.firmware_version || `Release ${state.installed_firmware}`;
      this.root.querySelector("[data-web-version]").textContent = webVersion(state.installed_web);
      this.root.querySelector("[data-firmware-candidate]").textContent = state.firmware_candidate ? `Published ${state.firmware_candidate}` : "No published release";
      this.root.querySelector("[data-web-candidate]").textContent = state.web_candidate ? `Published ${state.web_candidate}` : "No published release";
      for (const button of this.root.querySelectorAll("[data-update-action]")) {
        const action = button.dataset.updateAction;
        button.disabled = action === "cancel" ? !state.busy : state.busy ||
          (action === "firmware" && !state.firmware_available) || (action === "web" && !state.web_available);
      }
      this.progress.hidden = !state.busy;
      if (state.expected) { this.progress.max = state.expected; this.progress.value = state.received; }
      else this.progress.removeAttribute("value");
      if (this.version !== null && state.state === "web-installed" && this.version !== state.installed_web) { this.reload(); return; }
      this.version = state.installed_web;
      this.timer = setTimeout(() => this.refresh(), state.busy ? 1000 : 10000);
    } catch {
      if (client !== this.client) return;
      this.status.textContent = "Update status unavailable. Retrying shortly.";
      this.timer = setTimeout(() => this.refresh(), 5000);
    }
  }
}
