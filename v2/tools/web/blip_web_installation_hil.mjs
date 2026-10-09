// Factory-empty first run and manual OTA from a real, plain-HTTP device page.
import assert from "node:assert/strict";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const options = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, index, values) =>
  index % 2 === 0 ? [...pairs, [value.slice(2), values[index + 1]]] : pairs, []));
for (const key of ["device", "bundle", "build", "flash-log", "mac", "port", "python", "browser", "playwright", "report"])
  assert(options[key], `Missing --${key}`);
if (options["existing-ui"] !== "true") assert(options["preloaded-image"], "Missing --preloaded-image");
const digest = bytes => createHash("sha256").update(bytes).digest("hex");
const report = { passed: false, checks: [], pc_network_changed: false, device: options.device,
  mac: options.mac, port: options.port, checker_sha256: digest(await readFile(new URL(import.meta.url))) };
await mkdir(dirname(options.report), { recursive: true });
const sourcePaths = execFileSync("git", ["ls-files", "--cached", "--others", "--exclude-standard", "--", "v2/components", "v2/firmware", "v2/web"], { encoding: "utf8" }).trim().split("\n");
report.source_snapshot = Object.fromEntries(await Promise.all([...new Set(sourcePaths)].sort().map(async path => [path, digest(await readFile(path))])));
const check = (name, passed) => { report.checks.push({ name, passed: Boolean(passed) }); assert(passed, name); };
function serialStatus() {
  const code = `import sys,json;sys.path.insert(0,'v2/tools/control');from blip_wasm_hil import Client;c=Client(${JSON.stringify(options.port)});print(json.dumps({'boot':c.get('boot_sequence','blip.diagnostics'),'board':c.get('board','blip.ota'),'ota':c.get('state','blip.ota'),'web':c.get('web_bundle_version','blip.storage.files.internal')}));c.connection.close()`;
  return JSON.parse(execFileSync(options.python, ["-c", code], { timeout: 15000, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }));
}
const { chromium } = await import(pathToFileURL(resolve(options.playwright)).href);
const browser = await chromium.launch({ executablePath: options.browser, headless: true });
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
const firstRunResponses = [];
page.on("response", response => {
  if (new URL(response.url()).pathname === "/api/releases/first-run") firstRunResponses.push(response.status());
});
let firmwareResponse;
page.on("response", async response => {
  if (new URL(response.url()).pathname === "/api/firmware") firmwareResponse = response.status();
});
try {
  const image = await readFile(resolve(options.build, "blip-v2.bin"));
  report.firmware_sha256 = digest(image);
  report.mode = options["existing-ui"] === "true" ? "existing-ui-manual-ota" : "empty-filesystem-first-run";
  if (options["existing-ui"] !== "true") {
    const preloadedImage = await readFile(options["preloaded-image"]);
    report.preloaded_firmware_sha256 = digest(preloadedImage);
    check("preloaded-and-empty-filesystem-use-identical-app", image.equals(preloadedImage));
  }
  const flashBytes = await readFile(options["flash-log"]);
  report.flash_log_sha256 = digest(flashBytes);
  const flashLog = flashBytes.toString(flashBytes[0] === 255 && flashBytes[1] === 254 ? "utf16le" : "utf8");
  check("flashed-MAC", [...flashLog.toLowerCase().matchAll(/mac:\s*([0-9a-f:]+)/g)]
    .some(match => match[1] === options.mac.toLowerCase()));
  const readyDeadline = Date.now() + 45000;
  while (!report.initial && Date.now() < readyDeadline) {
    try { report.initial = serialStatus(); }
    catch { await new Promise(resolve => setTimeout(resolve, 1000)); }
  }
  check("application-ready-over-serial", Boolean(report.initial));
  if (options["existing-ui"] !== "true") check("empty-filesystem-has-no-full-interface", report.initial.web === 0);
  const response = await page.goto(options.device, { waitUntil: "domcontentloaded", timeout: 30000 });
  if (options["existing-ui"] !== "true") {
    const firstRunHtml = await response.text();
    check("firmware-serves-first-run-page", firstRunHtml.includes("Welcome to BLIP"));
    check("first-run-has-network-setup-and-retry", firstRunHtml.includes('href="/setup"') && firstRunHtml.includes('id="retry"'));
  }
  await page.locator("#connection-status").filter({ hasText: /^Live$/ }).waitFor({ timeout: 120000 });
  if (options["existing-ui"] !== "true") check("first-run-automatically-requests-install", firstRunResponses.includes(202));
  const bundle = await readFile(options.bundle);
  const assets = await (await page.request.get(`${options.device}/api/web-assets`)).json();
  report.web_assets = assets;
  check("downloaded-interface-matches-published-bundle", assets.bundle_version === bundle.readUInt32LE(8) &&
    assets.bytes === bundle.length && assets.crc32 === bundle.readUInt32LE(28).toString(16).padStart(8, "0"));
  report.installed = serialStatus();
  check("interface-loading-does-not-reboot", report.installed.boot === report.initial.boot);
  await page.screenshot({ path: options.report.replace(/\.json$/, ".installed.png"), fullPage: true });
  check("real-device-origin-has-no-SubtleCrypto", await page.evaluate(() => !globalThis.crypto?.subtle));
  const browserDigest = await page.evaluate(async data => {
    const { inspectFirmware } = await import("/src/firmware.js");
    return (await inspectFirmware(Uint8Array.from(atob(data), char => char.charCodeAt(0)).buffer)).sha256;
  }, image.toString("base64"));
  check("HTTP-browser-checks-full-real-firmware-image", browserDigest === report.firmware_sha256);
  await page.locator("#advanced-mode").click();
  const target = { 0: "esp32", 9: "esp32s3", 13: "esp32c6" }[image.readUInt16LE(12)];
  check("known-firmware-target", Boolean(target));
  if (options["existing-ui"] === "true") {
    const text = (offset, bytes) => image.subarray(offset, offset + bytes).toString("ascii").split("\0")[0];
    for (const [label, offset] of [["board", 348], ["layout", 428], ["flash-size", 304], ["features", 308],
      ["api", 312], ["profile", 460], ["target", 412], ["chip-header", 12], ["marker", 288], ["version", 48], ["short", -1]]) {
      const wrong = Buffer.from(image.subarray(0, offset < 0 ? 491 : 512));
      if (offset >= 0) wrong[offset] ^= 1;
      const rejected = await page.request.put(`${options.device}/api/firmware`, {
        data: wrong, headers: { "Content-Type": "application/octet-stream", "X-BLIP-SHA256": digest(wrong),
          "X-BLIP-Project": text(80, 32), "X-BLIP-Version": text(48, 32), "X-BLIP-Target": target,
          "X-BLIP-Profile": text(460, 32) },
      });
      check(`manual-upload-rejects-${label}`, rejected.status() === 400);
    }
    const afterRejections = serialStatus();
    check("rejected-images-preserve-running-firmware", afterRejections.boot === report.installed.boot && afterRejections.ota === "confirmed");
  }
  await page.locator("#firmware-target").selectOption(target);
  await page.locator("#firmware-file").setInputFiles(resolve(options.build, "blip-v2.bin"));
  await page.locator("#firmware-button").click();
  await page.waitForFunction(() => document.querySelector("#firmware-status").textContent.includes("accepted"), null, { timeout: 180000 });
  check("HTTP-browser-upload-accepted-by-device", firmwareResponse === 202);
  const deadline = Date.now() + 90000;
  let final;
  while (Date.now() < deadline) {
    try { final = serialStatus(); if (final.boot > report.installed.boot && final.ota === "confirmed") break; }
    catch { /* Reboot may temporarily interrupt serial readiness. */ }
    await new Promise(resolve => setTimeout(resolve, 1000));
  }
  report.final = final;
  check("manual-OTA-boots-and-confirms", final?.boot > report.installed.boot && final.ota === "confirmed");
  check("manual-OTA-preserves-installed-interface", final.web === assets.bundle_version);
  await page.goto(options.device, { waitUntil: "domcontentloaded", timeout: 30000 });
  await page.locator("#connection-status").filter({ hasText: /^Live$/ }).waitFor({ timeout: 30000 });
  check("interface-works-after-manual-OTA", true);
  report.passed = true;
} catch (error) { report.error = String(error.stack ?? error); }
finally {
  await browser.close();
  await mkdir(dirname(options.report), { recursive: true });
  await writeFile(options.report, JSON.stringify(report, null, 2) + "\n");
}
console.log(JSON.stringify({ passed: report.passed, checks: report.checks.length, error: report.error, report: options.report }));
process.exitCode = report.passed ? 0 : 1;
