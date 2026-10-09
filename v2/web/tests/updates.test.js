import assert from "node:assert/strict";
import test from "node:test";
import { UpdatePanel, webVersion, releaseSummary } from "../src/updates.js";

test("release labels keep unpublished distinct from current and preserve independent web codes", () => {
  assert.equal(webVersion(2001), "0.2.1");
  assert.equal(webVersion(1002003), "1.2.3");
  assert.equal(webVersion(-1), "Unavailable");
  assert.notEqual(releaseSummary("unpublished"), releaseSummary("current"));
  assert.match(releaseSummary("restarting"), /restarting/);
});

function panelFixture() {
  const elements = new Map();
  const buttons = ["check", "firmware", "web", "cancel"].map(action => ({
    dataset: { updateAction: action }, listeners: {},
    addEventListener(name, callback) { this.listeners[name] = callback; },
  }));
  const root = {
    querySelector(selector) {
      if (!elements.has(selector)) elements.set(selector, { removeAttribute() {} });
      return elements.get(selector);
    },
    querySelectorAll() { return buttons; },
  };
  const panel = new UpdatePanel({ root, reload() {} });
  return { panel, buttons };
}

test("updates pause background readings during admission and temporary status failures, then resume", async t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const { panel, buttons } = panelFixture();
  let state = { busy: false, state: "updates-available", installed_web: 3007, web_available: true };
  let admitted;
  panel.client = { loadReleases: async () => state, release: () => new Promise(resolve => { admitted = resolve; }) };
  await panel.refresh(); assert.equal(panel.busy, false);
  const clicked = buttons.find(button => button.dataset.updateAction === "web").listeners.click();
  assert.equal(panel.busy, true, "pause before awaiting admission");
  state = { ...state, busy: true, state: "downloading-web" }; admitted(); await clicked;
  assert.equal(panel.busy, true);
  panel.client.loadReleases = async () => { throw new Error("temporary outage"); };
  await panel.refresh(); assert.equal(panel.busy, true, "status failure must not restart large reads");
  panel.client.loadReleases = async () => ({ ...state, busy: false, state: "cancelled" });
  await panel.refresh(); assert.equal(panel.busy, false);
  panel.client.release = async () => { throw new Error("admission rejected"); };
  await buttons.find(button => button.dataset.updateAction === "web").listeners.click();
  assert.equal(panel.busy, false, "rejected admission restores readings");
});

test("an older status response cannot undo a newer update pause", async t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const { panel } = panelFixture();
  let oldResponse;
  panel.client = { loadReleases: () => new Promise(resolve => { oldResponse = resolve; }) };
  const old = panel.refresh();
  panel.client.loadReleases = async () => ({ busy: true, state: "downloading-web", installed_web: 3007 });
  await panel.refresh(); assert.equal(panel.busy, true);
  oldResponse({ busy: false, state: "current", installed_web: 3007 }); await old;
  assert.equal(panel.busy, true);
  panel.connect({ loadReleases: async () => ({ busy: false, state: "current", installed_web: 3007 }) });
  assert.equal(panel.busy, false, "a different device starts with its own status");
});

test("another transfer pauses status requests and blocks update admission without replay", async t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const { panel, buttons } = panelFixture(); let transferring = true, reads = 0, mutations = 0;
  panel.isTransferring = () => transferring;
  panel.client = { loadReleases: async () => { reads++; return { busy: false, state: "current", installed_web: 3010 }; },
    release: async () => { mutations++; } };
  await panel.refresh(); assert.equal(reads, 0);
  await buttons[0].listeners.click(); assert.equal(mutations, 0); assert.match(panel.status.textContent, /Wait/);
  transferring = false; await panel.refresh(); assert.equal(reads, 1); assert.equal(mutations, 0);
});

test("an in-flight status reply cannot reload the page during a newer manual transfer", async t => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const { panel } = panelFixture(); let transferring = false, resolve, reloads = 0;
  panel.version = 3010; panel.isTransferring = () => transferring; panel.reload = () => reloads++;
  panel.client = { loadReleases: () => new Promise(done => { resolve = done; }) };
  const pending = panel.refresh(); transferring = true;
  resolve({ busy: false, state: "web-installed", installed_web: 3011 }); await pending;
  assert.equal(reloads, 0); assert.equal(panel.version, 3010, "defer the version transition until transfer admission is released");
  transferring = false;
  panel.client.loadReleases = async () => ({ busy: false, state: "web-installed", installed_web: 3011 });
  await panel.refresh(); assert.equal(reloads, 1);
});
