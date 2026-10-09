// Exercise the installed filesystem UI in a real headless browser, on the existing LAN.
import assert from "node:assert/strict";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { parseArgs } from "node:util";
import { pathToFileURL } from "node:url";
import { dirname } from "node:path";
import { createHash } from "node:crypto";

const { values: args } = parseArgs({ options: Object.fromEntries(
  ["device", "playwright", "browser", "fixture", "report"].map(name => [name, { type: "string" }])) });
for (const name of ["device", "playwright", "browser", "fixture", "report"]) assert.ok(args[name], `--${name} is required`);
const root = new URL(args.device);
assert.equal(root.protocol, "http:"); assert.equal(root.pathname, "/"); assert.equal(root.search, "");
const { chromium } = await import(pathToFileURL(args.playwright).href);
const fixture = await readFile(args.fixture);
const tag = "000_browser_" + Date.now().toString(16);
const hash = bytes => createHash("sha256").update(bytes).digest("hex");
const report = { passed: false, created_at: new Date().toISOString(), device: root.origin,
  pc_network_changed: false, fixture_sha256: hash(fixture), tool_sha256: hash(await readFile(new URL(import.meta.url))),
  checks: [], page_errors: [], responses: [], schema_responses: [], request_failures: [], screenshots: [] };
const paths = [];
let browser, page;
const check = (name, condition) => { report.checks.push({ name, passed: Boolean(condition) }); assert.ok(condition, name); };
// Out-of-browser probes must not leave a fifth idle HTTP session competing
// with the browser's bounded connection pool and live WebSocket.
const probe = (path, options = {}) => fetch(new URL(path, root), {
  ...options, headers: { Connection: "close" }, signal: AbortSignal.timeout(60000),
});
const responseJSON = async path => { const response = await probe(path); assert.ok(response.ok); return response.json(); };
try {
  report.host = await responseJSON("/?HOST_INFO"); report.web = await responseJSON("/api/web-assets");
  browser = await chromium.launch({ executablePath: args.browser, headless: true, args: ["--no-proxy-server"] });
  page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, acceptDownloads: true });
  await page.addInitScript(() => {
    window.__fileReadings = [];
    const original = window.fetch;
    window.fetch = function (...args) {
      const url = new URL(args[0], location.href);
      if (url.pathname === "/" && url.searchParams.has("config")) window.__fileReadings.push({
        time: performance.now(), fileBusy: document.querySelector("#file-center")?.getAttribute("aria-busy") === "true",
      });
      return Reflect.apply(original, this, args);
    };
  });
  page.setDefaultTimeout(60000);
  page.on("pageerror", error => report.page_errors.push(error.message));
  page.on("response", response => {
    if (response.url().includes("/api/files/")) report.responses.push({ url: response.url(), status: response.status() });
    if (new URL(response.url()).searchParams.has("config")) report.schema_responses.push({ status: response.status(), time: Date.now() });
  });
  page.on("requestfailed", request => report.request_failures.push({ url: request.url(), failure: request.failure() }));
  report.bootstrap_reloads = 0;
  for (;;) {
    try {
      await page.goto(root.href, { waitUntil: "domcontentloaded", timeout: 60000 });
      await page.locator("#connection-status").filter({ hasText: /^Live$/ }).waitFor({ timeout: 20000 }); break;
    }
    catch (error) {
      // No controls have been submitted yet. Record and bound reloads during
      // module/schema startup; never replay a file mutation or script action.
      if (report.bootstrap_reloads === 2) throw error;
      report.bootstrap_reloads++;
    }
  }
  check("simple-opens-by-default", await page.locator("#simple-mode").getAttribute("aria-pressed") === "true");
  check("file-browser-hidden-in-simple", await page.locator("#file-center").isHidden());
  await page.locator("#advanced-mode").click();
  const panel = page.locator("#file-center"); await panel.waitFor({ state: "visible" });
  await panel.locator("[data-file-refresh]").waitFor({ state: "visible" });
  await page.waitForFunction(() => document.querySelector("#file-center").getAttribute("aria-busy") === "false");
  for (const namespace of ["scripts", "playback", "sequences"]) {
    await panel.locator(`[data-file-namespace="${namespace}"]`).click();
    await page.waitForFunction(() => document.querySelector("#file-center").getAttribute("aria-busy") === "false");
    const name = namespace === "scripts" ? "show.wasm" : "show.bin";
    const bytes = namespace === "scripts" ? fixture : namespace === "playback" ?
      Buffer.from(Array.from({ length: 32769 }, (_, i) => (i * 37 + 11) % 251)) : Buffer.from([0, 1, 2, 253, 254, 255]);
    const path = `${namespace}/${tag}/${name}`; paths.push(path);
    await panel.locator("[data-file-input]").setInputFiles({ name, mimeType: "application/octet-stream", buffer: bytes });
    await panel.locator("[data-file-path]").fill(`${tag}/${name}`);
    await panel.getByRole("button", { name: "Upload file", exact: true }).click();
    await panel.locator("[data-file-status]").filter({ hasText: /^Saved on device$/ }).waitFor();
    const folder = panel.locator("li").filter({ has: page.locator("strong", { hasText: tag }) });
    for (let pages = 0; await folder.count() === 0; pages++) {
      assert.ok(pages < 32, "fixture folder must be reachable through bounded pages");
      await panel.locator("[data-file-more]").click();
      await page.waitForFunction(() => document.querySelector("#file-center").getAttribute("aria-busy") === "false");
    }
    await folder.getByRole("button", { name: "Open folder" }).click();
    const row = panel.locator("li").filter({ has: page.locator("strong", { hasText: name }) });
    await row.waitFor();
    check(`${namespace}-folder-navigation`, (await panel.locator("[data-file-breadcrumbs]").textContent()).includes(tag));
    const downloadEvent = page.waitForEvent("download");
    await row.getByRole("button", { name: "Download", exact: true }).click();
    const download = await downloadEvent;
    check(`${namespace}-download-exact-bytes`, (await readFile(await download.path())).equals(bytes));
    check(`${namespace}-download-name`, download.suggestedFilename() === name);
    await page.waitForFunction(() => document.querySelector("#file-center").getAttribute("aria-busy") === "false");
    if (namespace === "scripts") {
      await row.getByRole("button", { name: "Load script", exact: true }).click();
      await panel.locator("[data-file-status]").filter({ hasText: /^Script loaded/ }).waitFor();
      await page.locator(".control-row").filter({ has: page.locator(".control-label", { hasText: /^Script level$/ }) }).waitFor();
      check("stored-script-publishes-browser-controls", true);
      const shot = args.report.replace(/\.json$/, "-desktop.png");
      await panel.screenshot({ path: shot }); report.screenshots.push(shot);
      await page.setViewportSize({ width: 390, height: 844 });
      await panel.scrollIntoViewIfNeeded();
      check("mobile-has-no-horizontal-overflow", await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      const mobile = args.report.replace(/\.json$/, "-mobile.png");
      await panel.screenshot({ path: mobile }); report.screenshots.push(mobile);
      await page.setViewportSize({ width: 1440, height: 1000 });
    }
    await row.getByRole("button", { name: "Delete", exact: true }).click();
    await panel.locator("[data-file-status]").filter({ hasText: /^File deleted$/ }).waitFor();
    check(`${namespace}-deleted-row-hidden`, await panel.locator("li strong").count() === 0);
    const deleted = await probe("/api/files/" + path);
    check(`${namespace}-deleted-file-not-found`, deleted.status === 404); await deleted.arrayBuffer();
  }
  check("no-browser-script-errors", report.page_errors.length === 0);
  check("browser-file-requests-succeeded", report.responses.every(row => row.status < 400));
  report.schema_reads = await page.evaluate(() => window.__fileReadings);
  check("background-readings-paused-during-files", report.schema_reads.every(read => !read.fileBusy));
  report.passed = true;
} catch (error) { report.error = error.stack; if (error.cause) report.cause = error.cause.message; }
finally {
  if (page && !page.isClosed() && !report.schema_reads)
    report.schema_reads = await page.evaluate(() => window.__fileReadings ?? []).catch(() => []);
  if (page && !page.isClosed()) report.final_browser_state = await page.evaluate(() => ({
    visibility: document.visibilityState, readings: document.querySelector("#sample-status")?.textContent,
    notice: document.querySelector("#notice")?.textContent, connection: document.querySelector("#connection-status")?.textContent,
    fileBusy: document.querySelector("#file-center")?.getAttribute("aria-busy"),
    fileStatus: document.querySelector("[data-file-status]")?.textContent,
  })).catch(error => ({ error: error.message }));
  await browser?.close();
  for (const path of paths) {
    try {
      for (let i = 0; i < 2; i++) {
        const response = await probe("/api/files/" + path, { method: "DELETE" });
        await response.arrayBuffer(); if (!response.ok) throw new Error(`HTTP ${response.status}`);
      }
    } catch (error) { report.cleanup_error = error.message; report.passed = false; }
  }
  await mkdir(dirname(args.report), { recursive: true });
  await writeFile(args.report, JSON.stringify(report, null, 2) + "\n");
}
console.log(JSON.stringify({ passed: report.passed, checks: report.checks.length, error: report.error, report: args.report }));
process.exitCode = report.passed ? 0 : 1;
