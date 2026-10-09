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
