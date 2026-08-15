import { DeviceClient, deviceUrlFromLocation } from "./client.js";
import { buildControlModel, describeHost } from "./model.js";
import { normalizeResourceSnapshot } from "./resources.js";
import { ControlView, ReservationView } from "./view.js";
import { uploadFirmware } from "./firmware.js";

const controlsRoot = document.querySelector("#controls");
const emptyTemplate = document.querySelector("#empty-state-template");
const connectButton = document.querySelector("#connect-button");
const connectionDot = document.querySelector("#connection-dot");
const connectionStatus = document.querySelector("#connection-status");
const deviceName = document.querySelector("#device-name");
const deviceMeta = document.querySelector("#device-meta");
const notice = document.querySelector("#notice");
const search = document.querySelector("#control-search");
const showConfig = document.querySelector("#show-config");
const firmwareFile = document.querySelector("#firmware-file");
const firmwareTarget = document.querySelector("#firmware-target");
const firmwareButton = document.querySelector("#firmware-button");
const firmwareStatus = document.querySelector("#firmware-status");
const reservationsRoot = document.querySelector("#reservations");
const reservationSearch = document.querySelector("#reservation-search");

let client = null;

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
    const { host, tree, resources } = await client.load(showConfig.checked);
    const identity = describeHost(host);
    const model = buildControlModel(tree);
    const resourceSnapshot = normalizeResourceSnapshot(resources);
    deviceName.textContent = identity.name;
    deviceMeta.textContent = `${identity.type} · ${identity.id} · firmware ${identity.version}`;
    view.setModel(model);
    view.setResources(resourceSnapshot);
    reservationView.setSnapshot(resourceSnapshot);
    client.open({
      onMessage: (message) => view.applyMessage(message),
      onState: (state) => {
        setConnection(state, state === "online" ? "Live" : "Offline");
        connectButton.textContent = state === "online" ? "Reconnect" : "Connect";
      },
      onError: showError,
    });
  } catch (error) {
    showError(error);
    connectButton.textContent = "Retry";
  } finally {
    connectButton.disabled = false;
  }
}

connectButton.addEventListener("click", connect);
showConfig.addEventListener("change", connect);
search.addEventListener("input", () => view.setFilter(search.value));
reservationSearch.addEventListener("input", () => reservationView.setFilter(reservationSearch.value));
view.setModel({ components: [], index: new Map() });

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
