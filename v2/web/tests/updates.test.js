import assert from "node:assert/strict";
import test from "node:test";
import { webVersion, releaseSummary } from "../src/updates.js";

test("release labels keep unpublished distinct from current and preserve independent web codes", () => {
  assert.equal(webVersion(2001), "0.2.1");
  assert.equal(webVersion(1002003), "1.2.3");
  assert.equal(webVersion(-1), "Unavailable");
  assert.notEqual(releaseSummary("unpublished"), releaseSummary("current"));
  assert.match(releaseSummary("restarting"), /restarting/);
});
