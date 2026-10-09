// Observe a device-hosted UI while another process exercises native updates.
import assert from "node:assert/strict";
import { readFile, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const args = Object.fromEntries(process.argv.slice(2).reduce((all, item, i, values) =>
  i % 2 ? all : [...all, [item.slice(2), values[i + 1]]], []));
for (const key of ["device", "playwright", "browser", "ready", "report"]) assert(args[key], key);
const { chromium } = await import(pathToFileURL(resolve(args.playwright)).href);
const browser = await chromium.launch({ executablePath: args.browser, headless: true });
const page = await browser.newPage();
const report = { passed: false, pc_network_changed: false, errors: [], samples: [],
  checker_sha256: createHash("sha256").update(await readFile(new URL(import.meta.url))).digest("hex") };
page.on("pageerror", error => report.errors.push(String(error)));
let navigations = 0;
page.on("framenavigated", frame => { if (frame === page.mainFrame()) ++navigations; });
let stop = false;
process.stdin.setEncoding("utf8"); process.stdin.on("data", () => { stop = true; });
try {
  await page.goto(args.device, { waitUntil: "domcontentloaded", timeout: 30000 });
  await page.locator("#connection-status").filter({ hasText: /^Live$/ }).waitFor({ timeout: 30000 });
  await writeFile(args.ready, JSON.stringify({ ready: true }) + "\n");
  const deadline = Date.now() + 180000;
  while (!stop && Date.now() < deadline) {
    try {
      report.samples.push({ ...(await page.evaluate(() => ({ time: Date.now(),
        status: document.querySelector("#connection-status")?.textContent,
        name: document.querySelector("#device-name")?.textContent }))), navigations });
    } catch (error) {
      if (!String(error).includes("Execution context was destroyed")) throw error;
      report.samples.push({ time: Date.now(), navigating: true, navigations });
    }
    await new Promise(resolve => setTimeout(resolve, 500));
  }
  // Installing new assets intentionally refreshes the app. Allow that single
  // bounded transition, while requiring live control outside it and at exit.
  const loading = report.samples.filter(sample => sample.status !== "Live");
  let loadingSince = null, longestTransition = 0;
  for (const sample of report.samples) {
    if (sample.status === "Live") loadingSince = null;
    else {
      loadingSince ??= sample.time;
      longestTransition = Math.max(longestTransition, sample.time - loadingSince);
    }
  }
  report.navigations = navigations;
  report.longest_transition_ms = longestTransition;
  report.passed = stop && report.samples.length >= 4 && report.errors.length === 0 && navigations <= 2 &&
    report.samples.at(-1)?.status === "Live" && report.samples.filter(sample => sample.status === "Live").every(sample => sample.name) &&
    loading.every(sample => sample.status === "Reconnecting" ||
      (sample.navigations >= 2 && (sample.navigating || sample.status === "Connecting" || sample.status === "Offline"))) &&
    longestTransition <= 5000;
} catch (error) { report.errors.push(String(error.stack ?? error)); }
finally {
  await browser.close();
  await writeFile(args.report, JSON.stringify(report, null, 2) + "\n");
}
console.log(JSON.stringify({ passed: report.passed, samples: report.samples.length, errors: report.errors }));
process.exitCode = report.passed ? 0 : 1;
