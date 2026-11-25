// A double-click's second press on what its first press opened goes nowhere
// (src/doubleclick.js, secondPress); one on what was already there, or a press of
// its own, is a press.
import { test } from "node:test";
import assert from "node:assert/strict";
import { secondPress } from "../src/doubleclick.js";

const sheet = { name: "sheet" }, row = null;

test("the second press of a double-click on a sheet its first press opened goes nowhere", () => {
  const first = { t: 1000, x: 200, y: 300, open: new Set() };
  assert.equal(secondPress(first, { t: 1180, x: 201, y: 300 }, sheet), true);
});

test("a double-click on a list row (no sheet under it) is a double-click: both presses count", () => {
  const first = { t: 1000, x: 200, y: 300, open: new Set() };
  assert.equal(secondPress(first, { t: 1180, x: 200, y: 300 }, row), false);
});

test("a press on a sheet that was open already, a later press, or one elsewhere, is a press", () => {
  const first = { t: 1000, x: 200, y: 300, open: new Set([sheet]) };
  assert.equal(secondPress(first, { t: 1180, x: 200, y: 300 }, sheet), false);
  const opened = { t: 1000, x: 200, y: 300, open: new Set() };
  assert.equal(secondPress(opened, { t: 1600, x: 200, y: 300 }, sheet), false);
  assert.equal(secondPress(opened, { t: 1180, x: 260, y: 300 }, sheet), false);
  assert.equal(secondPress(null, { t: 1180, x: 200, y: 300 }, sheet), false);
});
