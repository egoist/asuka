// Chips and date ranges (src/controls.js): the keys that remove an input
// chip and the symbol before a chip's title; a range's days as date
// inputs have them, in the device's time zone, what a change of either
// end makes of it and reports, and its inputs' bounds.
import { test } from "node:test";
import assert from "node:assert/strict";
import "../src/host.js"; // as on a page: the host's modules loaded in their order
import { chipRemoves, chipLead, dayString, dayStart, rangeChange, rangeBounds, settled, clampDay } from "../src/controls.js";

const key = (k, m = {}) => ({ key: k, ctrlKey: false, metaKey: false, altKey: false, isComposing: false, ...m });

test("Delete and Backspace remove a removable chip; nothing else does", () => {
  assert.equal(chipRemoves(key("Delete"), true), true);
  assert.equal(chipRemoves(key("Backspace"), true), true);
  assert.equal(chipRemoves(key("Backspace"), false), false); // not removable
  assert.equal(chipRemoves(key("Enter"), true), false);
  assert.equal(chipRemoves(key(" "), true), false);
  assert.equal(chipRemoves(key("Backspace", { metaKey: true }), true), false); // a shortcut
  assert.equal(chipRemoves(key("Delete", { ctrlKey: true }), true), false);
  assert.equal(chipRemoves(key("Backspace", { isComposing: true }), true), false);
});

test("a selected filter chip's checkmark takes its icon's place", () => {
  assert.equal(chipLead({ filter: false, selected: false, icon: "person" }), "person");
  assert.equal(chipLead({ filter: false, selected: true, icon: "person" }), "person"); // not a filter chip
  assert.equal(chipLead({ filter: true, selected: false, icon: "" }), "");
  assert.equal(chipLead({ filter: true, selected: true, icon: "" }), "checkmark");
  assert.equal(chipLead({ filter: true, selected: true, icon: "wifi" }), "checkmark");
});

test("days: an instant's, and a day's local midnight", () => {
  const noon = new Date(2026, 11, 3, 12, 30).getTime();
  assert.equal(dayString(noon), "2026-12-03");
  assert.equal(dayString(0), ""); // none chosen
  assert.equal(dayStart("2026-12-03"), new Date(2026, 11, 3).getTime());
  assert.equal(dayString(dayStart("2026-01-31")), "2026-01-31");
  assert.equal(dayStart(""), null);
  assert.equal(dayStart("2026-1-3"), null);
});

test("a change reports the range once both ends are chosen, local midnights, the first first", () => {
  const d = (s) => new Date(...s.split("-").map((x, i) => (i === 1 ? +x - 1 : +x))).getTime();
  assert.deepEqual(rangeChange("2026-12-03", "", "start"), { start: "2026-12-03", end: "", report: null });
  assert.deepEqual(rangeChange("", "2026-12-08", "end"), { start: "", end: "2026-12-08", report: null });
  assert.deepEqual(rangeChange("2026-12-03", "2026-12-08", "end"), { start: "2026-12-03", end: "2026-12-08", report: [d("2026-12-03"), d("2026-12-08")] });
  // One day: both ends on it.
  assert.deepEqual(rangeChange("2026-12-03", "2026-12-03", "end").report, [d("2026-12-03"), d("2026-12-03")]);
  // A start moved past the end takes the end with it.
  assert.deepEqual(rangeChange("2026-12-10", "2026-12-08", "start"), { start: "2026-12-10", end: "2026-12-10", report: [d("2026-12-10"), d("2026-12-10")] });
  // An end moved before the start takes the start with it.
  assert.deepEqual(rangeChange("2026-12-10", "2026-12-05", "end"), { start: "2026-12-05", end: "2026-12-05", report: [d("2026-12-05"), d("2026-12-05")] });
  // An end cleared: nothing to report.
  assert.equal(rangeChange("2026-12-03", "", "end").report, null);
});

test("a year being typed, or a day out of bounds, moves nothing and reports nothing until it's left", () => {
  // A date input says it changed at each digit of the year: 0002, 0020, 0202, 2026.
  assert.equal(settled("0002-12-10"), false);
  assert.equal(settled("0202-12-10"), false);
  assert.equal(settled("2026-12-10"), true);
  assert.equal(settled("202026-12-10"), false); // typed on past four digits
  assert.equal(settled("202026-12-10", "2026-11-28"), false);
  assert.equal(clampDay("202026-12-10", "2026-11-28", "2027-02-01"), "2027-02-01");
  assert.equal(clampDay("202026-12-10"), "");
  assert.equal(settled(""), true);
  assert.equal(settled("2026-11-27", "2026-11-28"), false);
  assert.equal(settled("2027-03-01", "", "2027-02-01"), false);
  // The end typed while the start's set: its half-typed year doesn't take the start back to year 2.
  assert.deepEqual(rangeChange("2026-12-10", "0002-12-14", "end"), { start: "2026-12-10", end: "0002-12-14", report: null, pending: true });
  assert.equal(rangeChange("2026-12-10", "2026-12-14", "end", "2026-11-28").report.length, 2);
  assert.equal(rangeChange("2026-11-01", "2026-12-14", "start", "2026-11-28").pending, true);
  // Left half typed or out of bounds: into them, or none.
  assert.equal(clampDay("0002-12-10", "2026-11-28"), "2026-11-28");
  assert.equal(clampDay("0002-12-10"), "");
  assert.equal(clampDay("2027-03-01", "", "2027-02-01"), "2027-02-01");
  assert.equal(clampDay("2026-12-10", "2026-11-28", "2027-02-01"), "2026-12-10");
  assert.equal(clampDay(""), "");
});

test("bounds: the picker's on both, the end no earlier than the start", () => {
  const min = new Date(2026, 10, 28, 9).getTime(), max = new Date(2027, 1, 1, 18).getTime();
  assert.deepEqual(rangeBounds("", 0, 0), { startMin: "", startMax: "", endMin: "", endMax: "" });
  assert.deepEqual(rangeBounds("", min, max), { startMin: "2026-11-28", startMax: "2027-02-01", endMin: "2026-11-28", endMax: "2027-02-01" });
  assert.deepEqual(rangeBounds("2026-12-03", min, 0), { startMin: "2026-11-28", startMax: "", endMin: "2026-12-03", endMax: "" });
  assert.equal(rangeBounds("2026-12-03", 0, 0).endMin, "2026-12-03");
});
