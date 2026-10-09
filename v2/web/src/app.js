import { DeviceClient, deviceUrlFromLocation } from "./client.js";
import { buildControlModel, describeHost, presentationModel, modelShapeKey } from "./model.js";
import { normalizeResourceSnapshot } from "./resources.js";
import { ControlView, ReservationView } from "./view.js";
import { uploadFirmware } from "./firmware.js";
import { UpdatePanel } from "./updates.js";

const controlsRoot = document.querySelector("#controls");
const emptyTemplate = document.querySelector("#empty-state-template");
const connectButton = document.querySelector("#connect-button");
const connectionDot = document.querySelector("#connection-dot");
const connectionStatus = document.querySelector("#connection-status");
const deviceName = document.querySelector("#device-name");
const deviceMeta = document.querySelector("#device-meta");
const deviceNameForm = document.querySelector("#device-name-form");
const deviceNameInput = document.querySelector("#device-name-input");
const deviceNameStatus = document.querySelector("#device-name-status");
const notice = document.querySelector("#notice");
const search = document.querySelector("#control-search");
const simpleButton = document.querySelector("#simple-mode");
const advancedButton = document.querySelector("#advanced-mode");
const topicsRoot = document.querySelector("#topics");
const sampleStatus = document.querySelector("#sample-status");
const firmwareFile = document.querySelector("#firmware-file");
const firmwareTarget = document.querySelector("#firmware-target");
const firmwareButton = document.querySelector("#firmware-button");
const firmwareStatus = document.querySelector("#firmware-status");
const reservationsRoot = document.querySelector("#reservations");
const reservationSearch = document.querySelector("#reservation-search");

let client = null;
let mode = "simple", topic = "All", model = { components: [], index: new Map() };
let shape = "", refreshTimer, refreshFailures = 0;
let nameControl = null;
const updatePanel = new UpdatePanel({ document, root: document.querySelector("#update-center") });

function present() {
  nameControl = model.components.find(component => component.id === "blip.device.identity")?.controls.find(control => control.id === "name") ?? null;
  deviceNameForm.hidden = nameControl === null;
  view.setMode(mode);
  view.setModel(presentationModel(model, mode, topic));
  simpleButton.setAttribute("aria-pressed", String(mode === "simple"));
  advancedButton.setAttribute("aria-pressed", String(mode === "advanced"));
  document.querySelector("#mode-description").textContent = mode === "simple" ? "Main controls & live readings" : "All parameters, organized by component";
  document.querySelector("#controls-heading").textContent = mode === "simple" ? "At a glance" : "Device configuration";
  for (const panel of document.querySelectorAll(".advanced-only")) panel.hidden = mode !== "advanced";
  topicsRoot.hidden = mode !== "advanced";
  topicsRoot.replaceChildren();
  for (const label of ["All", ...new Set(model.components.map(component => component.topic))]) {
    const button = document.createElement("button"); button.type = "button";
    button.textContent = label; button.setAttribute("aria-pressed", String(topic === label));
    button.addEventListener("click", () => { topic = label; present(); }); topicsRoot.append(button);
  }
}

function receive(message) {
  const control = model.index.get(message.address);
  if (control?.kind === "parameter" && message.values.length === 1) control.value = message.values[0].value;
  if (control === nameControl && typeof control.value === "string") {
    deviceName.textContent = control.value;
    if (document.activeElement !== deviceNameInput) deviceNameInput.value = control.value;
    if (deviceNameInput.value === control.value) deviceNameStatus.textContent = "Saved on device";
  }
  view.applyMessage(message);
}

function scheduleRefresh(current) {
  clearTimeout(refreshTimer);
  refreshTimer = setTimeout(async () => {
    if (client !== current) return;
    if (document.hidden) { scheduleRefresh(current); return; }
    try {
      const updated = buildControlModel(await current.loadTree());
      if (client !== current) return;
      const nextShape = modelShapeKey(updated);
      if (nextShape !== shape) {
        model = updated; shape = nextShape;
        if (!model.components.some(component => component.topic === topic)) topic = "All";
        present();
      } else {
        for (const control of updated.index.values()) {
          if (control.readable && control.kind === "parameter" && control.value !== undefined)
            receive({ address: control.path, values: [{ type: control.type, value: control.value }] });
        }
      }
      refreshFailures = 0;
      sampleStatus.textContent = `Readings updated ${new Date().toLocaleTimeString()} · every 3 seconds`;
    } catch {
      if (client !== current) return;
      refreshFailures++;
      sampleStatus.textContent = "Readings unavailable · retrying";
    }
    if (client === current) scheduleRefresh(current);
  }, Math.min(30000, 3000 * 2 ** Math.min(refreshFailures, 4)));
}

function setConnection(state, label) {
  connectionDot.dataset.state = state;
  connectionStatus.textContent = label;
}

function showError(error) {
  notice.textContent = error instanceof Error ? error.message : String(error);
  notice.hidden = false;
  setConnection("error", "Connection error");
}

const view = new ControlView({
  document,
  root: controlsRoot,
  emptyTemplate,
  onSend: (control, values) => client.send(control.path, values),
  onReassign: async (request) => {
    try {
      const snapshot = normalizeResourceSnapshot(await client.reassign(request));
      reservationView.setSnapshot(snapshot);
      return snapshot;
    } catch (error) {
      if (error?.code === "stale-revision") await connect();
      throw error;
    }
  },
  onError: showError,
});
const reservationView = new ReservationView({
  document,
  root: reservationsRoot,
  onNavigate: (owner) => {
    const entry = [...view.rows.values()].find(({ control }) => control.resourceOwner === owner);
    entry?.row.scrollIntoView?.({ behavior: "smooth", block: "center" });
    entry?.editor.querySelector("input, select, button")?.focus();
  },
});

async function connect() {
  connectButton.disabled = true;
  connectButton.textContent = "Connecting";
  notice.hidden = true;
  setConnection("connecting", "Connecting");
  try {
    client?.close();
    client = new DeviceClient({ baseUrl: deviceUrlFromLocation() });
    clearTimeout(refreshTimer);
    const current = client;
    const { host, tree, resources } = await current.load(true);
    const identity = describeHost(host);
    model = buildControlModel(tree); shape = modelShapeKey(model);
    nameControl = model.components.find(component => component.id === "blip.device.identity")?.controls.find(control => control.id === "name") ?? null;
    deviceNameForm.hidden = nameControl === null;
    deviceNameInput.value = nameControl?.value ?? identity.name;
    deviceNameStatus.textContent = "";
    const resourceSnapshot = normalizeResourceSnapshot(resources);
    deviceName.textContent = identity.name;
    deviceMeta.textContent = `${identity.type} · ${identity.id} · firmware ${identity.version}`;
    view.setResources(resourceSnapshot);
    reservationView.setSnapshot(resourceSnapshot);
    present();
    current.open({
      onMessage: receive,
      onState: (state) => {
        setConnection(state, state === "online" ? "Live" : "Reconnecting");
        if (state === "online") notice.hidden = true;
        connectButton.textContent = state === "online" ? "Reconnect" : "Connect";
      },
      onError: showError,
    });
    refreshFailures = 0;
    sampleStatus.textContent = "Live readings · sampled every 3 seconds";
    scheduleRefresh(current);
    updatePanel.connect(current);
  } catch (error) {
    showError(error);
    connectButton.textContent = "Retry";
  } finally {
    connectButton.disabled = false;
  }
}

connectButton.addEventListener("click", connect);
deviceNameForm.addEventListener("submit", event => {
  event.preventDefault();
  if (!nameControl) return;
  const name = deviceNameInput.value.trim();
  if (!name || new TextEncoder().encode(name).length > 63 || /[\u0000-\u001f\u007f-\u009f\u2028\u2029]/u.test(name)) {
    deviceNameStatus.textContent = "Use a name of 1–63 UTF-8 bytes without control characters.";
    return;
  }
  try {
    client.send(nameControl.path, [{ type: "string", value: name }]);
    deviceNameInput.value = name;
    deviceNameStatus.textContent = "Saving…";
  } catch (error) { deviceNameStatus.textContent = error.message; }
});
simpleButton.addEventListener("click", () => { mode = "simple"; topic = "All"; present(); });
advancedButton.addEventListener("click", () => { mode = "advanced"; topic = "All"; present(); });
search.addEventListener("input", () => view.setFilter(search.value));
reservationSearch.addEventListener("input", () => reservationView.setFilter(reservationSearch.value));
view.setModel({ components: [], index: new Map() });
present();

firmwareButton.addEventListener("click", async () => {
  const file = firmwareFile.files?.[0];
  if (!file) {
    firmwareStatus.textContent = "Choose a BLIP application image first.";
    return;
  }
  firmwareButton.disabled = true;
  firmwareStatus.textContent = "Validating and uploading firmware…";
  try {
    const image = await uploadFirmware({
      file,
      target: firmwareTarget.value,
      baseUrl: deviceUrlFromLocation(),
    });
    firmwareStatus.textContent = `${image.project} ${image.version} accepted. The device is restarting.`;
  } catch (error) {
    firmwareStatus.textContent = error instanceof Error ? error.message : String(error);
  } finally {
    firmwareButton.disabled = false;
  }
});

if (location.protocol === "http:" || location.protocol === "https:") connect();
