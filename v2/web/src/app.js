import { DeviceClient, deviceUrlFromLocation } from "./client.js";
import { buildControlModel, describeHost } from "./model.js";
import { ControlView } from "./view.js";

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
  onError: showError,
});

async function connect() {
  connectButton.disabled = true;
  connectButton.textContent = "Connecting";
  notice.hidden = true;
  setConnection("connecting", "Connecting");
  try {
    client?.close();
    client = new DeviceClient({ baseUrl: deviceUrlFromLocation() });
    const { host, tree } = await client.load(showConfig.checked);
    const identity = describeHost(host);
    const model = buildControlModel(tree);
    deviceName.textContent = identity.name;
    deviceMeta.textContent = `${identity.type} · ${identity.id} · firmware ${identity.version}`;
    view.setModel(model);
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
view.setModel({ components: [], index: new Map() });

if (location.protocol === "http:" || location.protocol === "https:") connect();
