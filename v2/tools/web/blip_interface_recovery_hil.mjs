// Inject browser-side asset failures against the installed UI on the existing LAN.
// The manual firmware trial sends an invalid identity prefix: no app slot is opened.
import assert from "node:assert/strict";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { parseArgs } from "node:util";
import { dirname } from "node:path";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";

const { values: args } = parseArgs({ options: Object.fromEntries(
  ["device", "playwright", "browser", "target", "report"].map(name => [name, { type: "string" }])) });
for (const name of ["device", "playwright", "browser", "target", "report"]) assert.ok(args[name], `--${name} is required`);
const root = new URL(args.device); assert.equal(root.protocol, "http:"); assert.equal(root.pathname, "/");
assert.ok(["esp32", "esp32s3", "esp32c6"].includes(args.target));
const { chromium } = await import(pathToFileURL(args.playwright).href);
const sha = bytes => createHash("sha256").update(bytes).digest("hex");
const report = { passed: false, created_at: new Date().toISOString(), device: root.origin,
  pc_network_changed: false, tool_sha256: sha(await readFile(new URL(import.meta.url))), checks: [], scenarios: [] };
const check = (name, condition) => { report.checks.push({ name, passed: Boolean(condition) }); assert.ok(condition, name); };
let browser, context, manualPage;
try {
  browser = await chromium.launch({ executablePath: args.browser, headless: true, args: ["--no-proxy-server"] });
  for (const [asset, permanent] of [["boot", false], ["files", false], ["files", true]]) {
    context = await browser.newContext();
    const page = await context.newPage(); page.setDefaultTimeout(60000);
    const scenario = { asset, permanent, failed_requests: 0, documents: 0, page_errors: [] };
    report.scenarios.push(scenario);
    page.on("pageerror", error => scenario.page_errors.push(error.message));
    page.on("framenavigated", frame => { if (frame === page.mainFrame()) scenario.documents++; });
    let fail = true;
    await page.route(root.origin + `/src/${asset}.js`, async route => {
      if (fail && (permanent || scenario.failed_requests === 0)) { scenario.failed_requests++; await route.abort("failed"); }
      else await route.continue();
    });
    // The application owns recovery. No harness reload is allowed here.
    await page.goto(root.href, { waitUntil: "domcontentloaded" });
    if (asset === "boot") {
      await page.locator("#startup-recovery a").waitFor();
      await page.waitForTimeout(2500);
      check("entry-fetch-failure-retains-static-reload", scenario.failed_requests === 1 && scenario.documents === 1);
      await page.locator("#startup-recovery a").click();
    } else if (permanent) {
      await page.locator("#connect-button").filter({ hasText: /^Retry loading$/ }).waitFor();
      await page.waitForFunction(() => !document.querySelector("#connect-button").disabled);
      check(`${asset}-persistent-failure-stops-after-two-reloads`, scenario.failed_requests === 3 && scenario.documents === 3);
      await page.waitForTimeout(2500);
      check(`${asset}-persistent-failure-stays-on-recovery-page`, scenario.documents === 3);
      fail = false; await page.locator("#connect-button").click();
    }
    await page.locator("#connection-status").filter({ hasText: /^Live$/ }).waitFor();
    const recovery = asset === "boot" || permanent ? "explicit" : "automatic";
    check(`${asset}-${recovery}-recovery-connects`, true);
    check(`${asset}-${recovery}-recovery-document-count`, scenario.documents === (permanent ? 4 : 2));
    check(`${asset}-${recovery}-success-clears-budget`, await page.evaluate(() => sessionStorage.getItem("blip.interface-startup.0.3.10")) === null);
    check(`${asset}-${recovery}-no-script-errors`, scenario.page_errors.length === 0);
    console.log(JSON.stringify({ scenario: `${asset}-${recovery}`, passed: true }));
    await context.close(); context = null;
  }

  context = await browser.newContext();
  const page = await context.newPage(); page.setDefaultTimeout(60000);
  manualPage = page;
  let manualDocuments = 0, statusRequests = 0, heldStatus, resolveStatus;
  page.on("framenavigated", frame => { if (frame === page.mainFrame()) manualDocuments++; });
  const oldStatus = new Promise(resolve => { resolveStatus = resolve; });
  await page.addInitScript(() => {
    window.__transferTraffic = [];
    const original = window.fetch;
    window.fetch = function (...args) {
      const url = new URL(args[0], location.href);
      window.__transferTraffic.push({ path: url.pathname + url.search, method: args[1]?.method ?? "GET",
        manualBusy: document.querySelector("#firmware-center")?.getAttribute("aria-busy") === "true" });
      return Reflect.apply(original, this, args);
    };
  });
  await page.goto(root.href, { waitUntil: "domcontentloaded" });
  await page.locator("#connection-status").filter({ hasText: /^Live$/ }).waitFor();
  // Wait for a real status/version before intercepting another request. On a
  // constrained board the second fetch may be a retry of its initial status.
  await page.waitForFunction(() => !document.querySelector('[data-update-action="check"]').disabled);
  report.initial_update_status_ready = true;
  await page.route(root.origin + "/api/releases", async route => {
    if (++statusRequests === 1) { heldStatus = route; resolveStatus(); }
    else await route.continue();
  });
  await page.locator("#advanced-mode").click();
  await page.waitForFunction(() => document.querySelector("#file-center").getAttribute("aria-busy") === "false");
  // Hold a poll begun before upload admission, then deliver a stale version
  // transition while the upload is pending. It must not reload the page.
  await Promise.race([oldStatus, page.waitForTimeout(30000).then(() => { throw new Error("status poll admission timed out"); })]);
  const fake = Buffer.alloc(1024); fake[0] = 0xe9; fake.writeUInt32LE(0xabcd5432, 32);
  fake.write("0.1.0\0", 48, "ascii"); fake.write("blip-v2\0", 80, "ascii");
  report.rejected_image_sha256 = sha(fake);
  let held, resolveHeld;
  const intercepted = new Promise(resolve => { resolveHeld = resolve; });
  await page.route(root.origin + "/api/firmware", async route => { held = route; resolveHeld(); });
  await page.locator("#firmware-target").selectOption(args.target);
  await page.locator("#firmware-file").setInputFiles({ name: "invalid-identity.bin", mimeType: "application/octet-stream", buffer: fake });
  await page.locator("#firmware-button").click();
  await Promise.race([intercepted, page.waitForTimeout(60000).then(() => { throw new Error("manual upload admission timed out"); })]);
  check("manual-upload-pauses-before-http-admission", await page.locator("#firmware-center").getAttribute("aria-busy") === "true");
  check("manual-upload-disables-inputs", await page.locator("#firmware-file").isDisabled() && await page.locator("#firmware-target").isDisabled());
  await heldStatus.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({
    busy: false, state: "web-installed", installed_web: 999999, installed_firmware: 35,
  }) });
  await page.waitForTimeout(250);
  check("stale-status-cannot-reload-during-manual-upload", manualDocuments === 1 && await page.locator("#firmware-center").getAttribute("aria-busy") === "true");
  await page.locator("[data-file-refresh]").click();
  check("manual-upload-blocks-file-operation", (await page.locator("[data-file-status]").textContent()).includes("Wait"));
  await page.locator('[data-update-action="check"]').click();
  check("manual-upload-blocks-release-admission", (await page.locator("[data-update-status]").textContent()).includes("Wait"));
  await page.locator("#connect-button").click();
  check("manual-upload-blocks-reconnect-read-burst", (await page.locator("#notice").textContent()).includes("Wait"));
  await page.waitForTimeout(7000);
  const traffic = await page.evaluate(() => window.__transferTraffic);
  check("manual-upload-pauses-heavy-and-status-reads", traffic.filter(row => row.manualBusy).every(row => row.path === "/api/firmware"));
  check("manual-upload-submits-once", traffic.filter(row => row.path === "/api/firmware").length === 1);
  await held.continue();
  await page.locator("#firmware-status").filter({ hasText: /Device rejected firmware \(HTTP 400\)/ }).waitFor();
  check("device-rejects-invalid-prefix", true);
  check("manual-failure-releases-admission", await page.locator("#firmware-center").getAttribute("aria-busy") === "false" && await page.locator("#firmware-button").isEnabled());
  const readCount = traffic.filter(row => row.path.startsWith("/?config=")).length;
  await page.waitForFunction(count => window.__transferTraffic.filter(row => row.path.startsWith("/?config=")).length > count, readCount);
  check("manual-failure-resumes-readings", true);
  await page.locator("[data-file-refresh]").click();
  await page.waitForFunction(() => document.querySelector("#file-center").getAttribute("aria-busy") === "false");
  check("manual-failure-resumes-files", !(await page.locator("[data-file-status]").textContent()).includes("Wait"));
  report.manual_traffic = await page.evaluate(() => window.__transferTraffic);
  check("manual-failure-does-not-replay-upload", report.manual_traffic.filter(row => row.path === "/api/firmware").length === 1);
  check("discarded-status-does-not-reload-after-upload", manualDocuments === 1);
  report.manual_documents = manualDocuments;
  report.passed = true;
} catch (error) { report.error = error.stack; }
finally {
  if (manualPage && !manualPage.isClosed()) {
    report.manual_traffic = await manualPage.evaluate(() => window.__transferTraffic ?? []).catch(() => []);
    report.manual_final_state = await manualPage.evaluate(() => ({
      firmware: document.querySelector("#firmware-status")?.textContent,
      busy: document.querySelector("#firmware-center")?.getAttribute("aria-busy"),
      readings: document.querySelector("#sample-status")?.textContent, visibility: document.visibilityState,
    })).catch(error => ({ error: error.message }));
  }
  await context?.close(); await browser?.close();
  await mkdir(dirname(args.report), { recursive: true });
  await writeFile(args.report, JSON.stringify(report, null, 2) + "\n");
}
console.log(JSON.stringify({ passed: report.passed, checks: report.checks.length, error: report.error, report: args.report }));
process.exitCode = report.passed ? 0 : 1;
