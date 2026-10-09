// On-device browser checks. Requires a local Playwright installation and browser.
import assert from "node:assert/strict";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { resolve, dirname } from "node:path";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { buildControlModel, presentationModel } from "../../web/src/model.js";

const options = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, i, args) =>
  i % 2 ? pairs : [...pairs, [value.replace(/^--/, ""), args[i + 1]]], []));
for (const key of ["device", "report", "playwright", "browser"]) if (!options[key]) throw new Error(`Missing --${key}`);
const pageUrl = options.device;
options.device = new URL(options.device).origin;
const { chromium } = await import(pathToFileURL(resolve(options.playwright)).href);
const report = { passed: false, device: options.device, checks: [], pc_network_changed: false,
  checker_sha256: createHash("sha256").update(await readFile(new URL(import.meta.url))).digest("hex") };
const sourcePaths = execFileSync("git", ["ls-files", "--cached", "--others", "--exclude-standard", "v2/components", "v2/firmware", "v2/web"], { encoding: "utf8" }).trim().split("\n");
report.source_snapshot = Object.fromEntries(await Promise.all(sourcePaths.map(async path => [path,
  createHash("sha256").update(await readFile(path)).digest("hex")])));
const browser = await chromium.launch({ executablePath: options.browser, headless: true });
report.browser_version = browser.version();
report.node_version = process.version;
report.playwright_version = JSON.parse(await readFile(resolve(dirname(options.playwright), "package.json"), "utf8")).version;
const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
const errors = []; page.on("pageerror", error => errors.push(error.message));
const requests = [];
page.on("requestfailed", request => requests.push({ url: request.url(), failure: request.failure()?.errorText }));
page.on("response", response => { if (response.status() >= 400) requests.push({ url: response.url(), status: response.status() }); });
const check = (name, condition) => { report.checks.push({ name, passed: Boolean(condition) }); assert.ok(condition, name); };
const tree = async () => (await page.request.get(`${options.device}/?config=1`)).json();
let loaded = false;
let originalName = null;
async function renameDevice(name) {
  await page.locator("#device-name-input").fill(name);
  await page.locator("#device-name-form button").click();
  await page.waitForFunction(expected => document.querySelector("#device-name").textContent === expected, name);
}
function moduleOperation(operation) {
  const code = `import sys; sys.path.insert(0,'v2/tools/control'); from blip_script_controls_hil import fixture; from blip_wasm_hil import Client; c=Client(${JSON.stringify(options.port)}); r=c.upload(fixture()) if '${operation}'=='load' else c.work('unload'); c.connection.close(); assert r['error']=='none',r`;
  execFileSync(options.python, ["-c", code], { timeout: 30000 });
}
try {
  const bundle = await readFile(options.bundle ?? "v2/components/blip_storage/factory_web.bundle");
  const assets = await (await page.request.get(`${options.device}/api/web-assets`)).json();
  report.web_assets = assets;
  check("installed-web-bundle", assets.bundle_version === bundle.readUInt32LE(8) && assets.bytes === bundle.length &&
    assets.crc32 === bundle.readUInt32LE(28).toString(16).padStart(8, "0"));
  await page.goto(pageUrl, { waitUntil: "networkidle", timeout: 30000 });
  await page.locator("#connection-status").filter({ hasText: /^Live$/ }).waitFor();
  const model = buildControlModel(await tree());
  const host = await (await page.request.get(`${options.device}/?HOST_INFO`)).json();
  originalName = host.NAME;
  check("device-name-and-type-visible", await page.locator("#device-name").innerText() === host.NAME &&
    (await page.locator("#device-meta").innerText()).includes(host.DEVICE_TYPE));
  check("name-editor-in-simple-mode", await page.locator("#device-name-form").isVisible());
  await renameDevice("Browser Stage " + host.DEVICE_ID.replaceAll(":", "").slice(-4));
  check("browser-renames-saved-identity", (await (await page.request.get(`${options.device}/?HOST_INFO`)).json()).NAME ===
    await page.locator("#device-name-input").inputValue());
  await renameDevice(originalName);
  report.component_count = model.components.length; report.control_count = model.index.size;
  check("simple-default", await page.locator("#simple-mode").getAttribute("aria-pressed") === "true");
  check("declared-primary-coverage", await page.locator(".control-row").count() === presentationModel(model, "simple").index.size);
  await page.waitForFunction(() => [...document.querySelectorAll(".sensor-trend polyline")].some(line => line.getAttribute("points")?.includes(" ")), { timeout: 15000 });
  check("actual-polled-graph", true);
  await page.screenshot({ path: `${options.report}.simple.png`, fullPage: true });
  await page.locator("#advanced-mode").click();
  check("name-editor-in-advanced-mode", await page.locator("#device-name-form").isVisible());
  check("advanced-all-controls", await page.locator(".control-row").count() === model.index.size);
  await page.locator("#advanced-mode").focus();
  await page.keyboard.press("Tab");
  check("keyboard-navigation", await page.evaluate(() => document.activeElement !== document.body));
  check("advanced-resources", await page.locator("#reservations").isVisible());
  await page.locator("#topics button").filter({ hasText: /^Lighting$/ }).click();
  check("topic-filter", await page.locator(".control-row").count() === presentationModel(model, "advanced", "Lighting").index.size);
  await page.locator("#topics button").filter({ hasText: /^All$/ }).click();
  await page.locator("#control-search").fill("brightness");
  check("search-filters", await page.locator(".control-row").count() > 0 && await page.locator(".control-row").count() < model.index.size);
  await page.locator("#control-search").fill("");
  await page.screenshot({ path: `${options.report}.advanced.png`, fullPage: true });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.locator("#simple-mode").click();
  await page.waitForFunction(() => [...document.querySelectorAll(".component-card")].every(card => Number(getComputedStyle(card).opacity) >= .99));
  check("mobile-no-horizontal-overflow", await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
  check("mobile-modes-accessible", await page.locator("#advanced-mode").isVisible());
  await page.screenshot({ path: `${options.report}.mobile.png`, fullPage: true });
  if (options.port && options.python) {
    moduleOperation("load"); loaded = true;
    const withScript = buildControlModel(await tree());
    const script = withScript.components.find(component => component.id === "blip.wasm");
    const note = script.controls.find(control => control.id === "note");
    const row = page.locator(".control-row").filter({ has: page.locator(`input[aria-label="${note.label}"]`) });
    await row.waitFor({ timeout: 15000 });
    check("dynamic-schema-auto-published", true);
    const input = row.locator("input"); await input.fill("unsent draft");
    await page.waitForTimeout(4000);
    check("poll-preserves-focused-draft", await input.inputValue() === "unsent draft");
    moduleOperation("unload"); loaded = false;
    await row.waitFor({ state: "detached", timeout: 15000 });
    check("dynamic-schema-auto-retired", true);
  }
  check("no-browser-exceptions", errors.length === 0);
  report.passed = true;
} catch (error) {
  report.error = error.stack; report.browser_errors = errors; report.failed_requests = requests;
  report.page_text = await page.locator("body").innerText();
  await page.screenshot({ path: `${options.report}.failure.png`, fullPage: true });
  throw error;
}
finally {
  if (originalName !== null) {
    try { await renameDevice(originalName); }
    catch (error) {
      report.passed = false; report.identity_restore_error = error.message;
      if (options.port && options.python) {
        try {
          const code = `import sys; sys.path.insert(0,'v2/tools/control'); from blip_wasm_hil import Client; c=Client(${JSON.stringify(options.port)}); c.request('set','blip.device.identity','name',${JSON.stringify(originalName)}); assert c.get('name','blip.device.identity')==${JSON.stringify(originalName)}; c.connection.close()`;
          execFileSync(options.python, ["-c", code], { timeout: 15000 });
          report.identity_restored_over_serial = true;
        } catch (restoreError) { report.serial_restore_error = restoreError.message; }
      }
    }
  }
  if (loaded) moduleOperation("unload");
  await mkdir(dirname(resolve(options.report)), { recursive: true });
  await writeFile(options.report, JSON.stringify(report, null, 2) + "\n");
  await browser.close();
}
