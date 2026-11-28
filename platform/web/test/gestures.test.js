// The host's gesture arithmetic (src/gestures.js): double taps' time and
// reach, when a drag is a pan, a zoom keeping the point under the fingers,
// and where a lifted row goes among a list's rows.
import { test } from "node:test";
import assert from "node:assert/strict";
import { Writer } from "../src/ops.js";
import {
  isSecondTap, doubleTime, panDecision, zoomScroll, rubberZoom, clampZoom, doubleTapZoom,
  reorderTarget, shiftOf, moveRow, moveID, decodeReorder,
} from "../src/gestures.js";

test("a second tap soon and near the first is a double tap; a later or farther one isn't", () => {
  const first = { t: 1000, x: 100, y: 100 };
  assert.equal(isSecondTap(first, { t: 1250, x: 110, y: 120 }, "touch"), true);
  assert.equal(isSecondTap(first, { t: 1000 + doubleTime("touch") + 1, x: 100, y: 100 }, "touch"), false);
  assert.equal(isSecondTap(first, { t: 1100, x: 160, y: 100 }, "touch"), false);
  // A mouse's double click: the desktop's time, a few points.
  assert.equal(isSecondTap(first, { t: 1450, x: 103, y: 102 }, "mouse"), true);
  assert.equal(isSecondTap(first, { t: 1100, x: 120, y: 100 }, "mouse"), false);
  assert.equal(isSecondTap(null, { t: 1100, x: 100, y: 100 }, "mouse"), false);
});

test("a pan begins past its distance; along one axis, a drag first along the other isn't one", () => {
  assert.equal(panDecision(8, 0, 0, 10), 0);
  assert.equal(panDecision(12, 3, 0, 10), 1);
  assert.equal(panDecision(3, 12, 0, 10), 1);
  // Horizontal: a vertical drag goes to the scroll.
  assert.equal(panDecision(3, 20, 1, 16), -1);
  assert.equal(panDecision(20, 3, 1, 16), 1);
  assert.equal(panDecision(10, 10, 1, 16), 0); // not yet past 16
  // Vertical: a sideways drag isn't one.
  assert.equal(panDecision(20, 3, 2, 10), -1);
  assert.equal(panDecision(-2, -20, 2, 10), 1);
});

test("zooming keeps the content under the point where it is", () => {
  // At zoom 1, scrolled to 0, the point 100, 50 is content 100, 50; at 2 it's at 200, 100: scrolled 100, 50 keeps it under.
  assert.deepEqual(zoomScroll(0, 0, 100, 50, 1, 2), [100, 50]);
  const [sx, sy] = zoomScroll(30, 40, 80, 60, 2, 3);
  // The content point (30 + 80) / 2 = 55 is at 55 * 3 - sx = 80 after.
  assert.ok(Math.abs(55 * 3 - sx - 80) < 1e-9);
  assert.ok(Math.abs(50 * 3 - sy - 60) < 1e-9);
});

test("a pinch gives way past the bounds, and settles within them; a double tap zooms in to twice min, or back out", () => {
  assert.equal(rubberZoom(2, 1, 4), 2);
  const over = rubberZoom(8, 1, 4);
  assert.ok(over > 4 && over < 8);
  const under = rubberZoom(0.5, 1, 4);
  assert.ok(under < 1 && under > 0.5);
  assert.equal(clampZoom(over, 1, 4), 4);
  assert.equal(clampZoom(under, 1, 4), 1);
  assert.equal(doubleTapZoom(1, 1, 4), 2);
  assert.equal(doubleTapZoom(1, 1, 1.5), 1.5);
  assert.equal(doubleTapZoom(3, 1, 4), 1);
  assert.equal(doubleTapZoom(0.5, 0.5, 4), 1);
});

// Five rows of 44 points.
const tops = [0, 44, 88, 132, 176, 220];

test("a lifted row goes past the middles it's taken past, and the others make way", () => {
  // Row 1 held where it was: it stays.
  assert.equal(reorderTarget(tops, 1, 66), 1);
  // Its middle below row 3's middle: row 3.
  assert.equal(reorderTarget(tops, 1, 160), 3);
  assert.equal(reorderTarget(tops, 1, 150), 2);
  // Above row 0's middle: the top.
  assert.equal(reorderTarget(tops, 3, 10), 0);
  // Not above the first header (sections: row 0 is one).
  assert.equal(reorderTarget(tops, 3, 10, 1), 1);
  // Rows between make way: down for a row taken up, up for one taken down.
  assert.deepEqual([0, 1, 2, 3, 4].map((r) => shiftOf(r, 3, 1, 44)), [0, 44, 44, 0, 0]);
  assert.deepEqual([0, 1, 2, 3, 4].map((r) => shiftOf(r, 1, 3, 44)), [0, 0, -44, -44, 0]);
});

test("a row moved: its item's indices before and after, and the IDs in the new order", () => {
  const ids = [10, 20, 30, 40];
  const rows = ids.map((_, i) => ({ header: false, section: 0, first: i, count: 1 }));
  const m = moveRow(rows, ids, [], 0, 2);
  assert.deepEqual(m.ids, [20, 30, 10, 40]);
  assert.equal(m.from, 0);
  assert.equal(m.to, 2);
  assert.equal(moveRow(rows, ids, [], 1, 1), null);
  assert.deepEqual(moveID(ids, 3, 1), [10, 40, 20, 30]);
  assert.deepEqual(ids, [10, 20, 30, 40]); // a copy
});

test("with sections, a row taken under another header joins its section", () => {
  // Section A (id 100): items 1, 2; section B (id 200): items 3, 4.
  const ids = [1, 2, 3, 4];
  const sections = [{ start: 0, id: 100 }, { start: 2, id: 200 }];
  const rows = [
    { header: true, section: 0, first: 0, count: 1 },
    { header: false, section: 0, first: 0, count: 1 },
    { header: false, section: 0, first: 1, count: 1 },
    { header: true, section: 1, first: 0, count: 1 },
    { header: false, section: 1, first: 2, count: 1 },
    { header: false, section: 1, first: 3, count: 1 },
  ];
  // Item 1 (row 1) to row 4's place: after B's header and item 3.
  const m = moveRow(rows, ids, sections, 1, 4);
  assert.deepEqual(m.ids, [2, 3, 1, 4]);
  assert.deepEqual(m.sections, [{ start: 0, id: 100 }, { start: 1, id: 200 }]);
  assert.equal(m.from, 0);
  assert.equal(m.to, 2);
  // Item 2 (row 2) just under B's header (row 3's place): its index stays 1, its section isn't the same: not a move the core hears of.
  assert.equal(moveRow(rows, ids, sections, 2, 3), null);
  // A header doesn't move.
  assert.equal(moveRow(rows, ids, sections, 3, 5), null);
});

test("Reorder's bytes: the move actions' names; empty: rows don't move", () => {
  const b = new Writer().u32(2).string("Nach oben").string("Nach unten").done();
  assert.deepEqual(decodeReorder(b), { up: "Nach oben", down: "Nach unten" });
  assert.equal(decodeReorder(new Uint8Array(0)), null);
});
