// A Table's and an Outline's pure parts (src/grid.js): the props' and
// events' encodings, the rows in view, the columns as the user leaves
// them, the selection as clicks and keys change it, the tree's keys;
// and the pointer's shapes (src/cursor.js) and the undo keys (src/keys.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { Writer, Reader, Cursor } from "../src/ops.js";
import * as g from "../src/grid.js";
import { cursorCSS } from "../src/cursor.js";
import { undoKey, editable } from "../src/keys.js";

const bytes = (w) => w.buf.slice(0, w.n);

test("Columns decode: widths, bounds, flags and alignment", () => {
  const w = new Writer().u32(2);
  w.string("name").string("Name").f32(260).f32(120).f32(0).u8(3).u8(0);
  w.string("size").string("Size").f32(0).f32(0).f32(300).u8(2).u8(2);
  const cols = g.decodeColumns(bytes(w));
  assert.deepEqual(cols[0], { id: "name", title: "Name", width: 260, min: 120, max: Infinity, resizable: true, sortable: true, align: 0 });
  assert.deepEqual(cols[1], { id: "size", title: "Size", width: 0, min: g.leastWidth, max: 300, resizable: false, sortable: true, align: 2 });
  assert.deepEqual(g.decodeColumns(new Uint8Array(0)), []);
});

test("a column's width keeps to its bounds; one with none of its own takes the host's", () => {
  const cols = [{ id: "a", width: 0, min: 32, max: Infinity }, { id: "b", width: 500, min: 32, max: 200 }];
  const l = g.layoutColumns(null, cols);
  assert.deepEqual(l.order, [0, 1]);
  assert.deepEqual(l.widths, [g.defaultWidth, 200]);
  assert.equal(g.clampWidth(10, cols[0]), 32);
});

test("the user's order and widths stay while the columns' IDs do", () => {
  const cols = [{ id: "a", width: 100, min: 32, max: Infinity }, { id: "b", width: 100, min: 32, max: Infinity }, { id: "c", width: 100, min: 32, max: Infinity }];
  let l = g.layoutColumns(null, cols);
  l.order = g.moveColumn(l.order, 2, 0); // c first
  l.widths[1] = 150;
  l.user.set("b", 150);
  // The app sends its columns again (a title changed, b's width too): the user's stay.
  const again = cols.map((c) => ({ ...c, width: c.id === "b" ? 90 : c.width }));
  l = g.layoutColumns(l, again);
  assert.deepEqual(l.order, [2, 0, 1]);
  assert.deepEqual(l.widths, [100, 150, 100]);
  // Other IDs: the app's order; a width the user gave stays with its ID.
  l = g.layoutColumns(l, [cols[1], { id: "d", width: 80, min: 32, max: Infinity }]);
  assert.deepEqual(l.order, [0, 1]);
  assert.deepEqual(l.widths, [150, 80]);
});

test("ColumnLayout encodes the columns as shown, from the start", () => {
  const r = new Reader(g.encodeColumnLayout([2, 0], [100, 50, 75]));
  assert.equal(r.u32(), 2);
  assert.deepEqual([r.u32(), r.f32(), r.u32(), r.f32()], [2, 75, 0, 100]);
  assert.ok(r.atEnd);
});

test("a dragged header lands where the pointer is past a header's middle", () => {
  const edges = [[0, 100], [100, 200], [200, 300]];
  assert.equal(g.dropIndex(edges, 2, 40), 0);
  assert.equal(g.dropIndex(edges, 2, 120), 1);
  assert.equal(g.dropIndex(edges, 2, 160), 2); // its own place
  assert.equal(g.dropIndex(edges, 0, 260), 2);
  assert.equal(g.dropIndex(edges, 0, 20), 0);
  assert.deepEqual(g.moveColumn([0, 1, 2], 2, 0), [2, 0, 1]);
});

test("Sort and RowData decode", () => {
  const s = g.decodeSort(bytes(new Writer().u32(2).u32(1).u8(0).u32(0).u8(1)));
  assert.deepEqual(s, [{ column: 1, ascending: false }, { column: 0, ascending: true }]);
  const d = g.decodeRowData(bytes(new Writer().u32(2).string("readme.md").string("4 KB").string("doc")));
  assert.deepEqual(d, { cells: ["readme.md", "4 KB"], symbol: "doc" });
  assert.deepEqual(g.decodeRowData(bytes(new Writer().u32(1).string("Home").string(""))), { cells: ["Home"], symbol: "" });
});

test("runs: decoded sorted and merged, encoded back, cut at the count", () => {
  const b = bytes(new Writer().u32(3).u32(10).u32(2).u32(0).u32(3).u32(3).u32(1));
  assert.deepEqual(g.decodeRuns(b), [0, 4, 10, 12]);
  assert.deepEqual(g.decodeRuns(b, 11), [0, 4, 10, 11]);
  assert.deepEqual(g.decodeRuns(g.encodeRuns([0, 4, 10, 12])), [0, 4, 10, 12]);
  assert.ok(g.runsHas([0, 4, 10, 12], 3) && g.runsHas([0, 4, 10, 12], 11));
  assert.ok(!g.runsHas([0, 4, 10, 12], 4) && !g.runsHas([0, 4, 10, 12], 12) && !g.runsHas([], 0));
  assert.equal(g.runsCount([0, 4, 10, 12]), 6);
});

test("a click selects one row; Shift extends from the anchor; the primary modifier toggles", () => {
  const s = new g.RowSelection(), M = g.Mode.Multiple;
  assert.ok(s.click(3, false, false, M));
  assert.deepEqual(s.runs, [3, 4]);
  assert.ok(s.click(6, true, false, M));
  assert.deepEqual(s.runs, [3, 7]);
  assert.ok(s.click(1, true, false, M)); // from the same anchor, the other way
  assert.deepEqual(s.runs, [1, 4]);
  assert.ok(s.click(10, false, true, M)); // ⌘: adds, and anchors there
  assert.deepEqual(s.runs, [1, 4, 10, 11]);
  assert.ok(s.click(2, false, true, M)); // ⌘ on a selected row: removes it
  assert.deepEqual(s.runs, [1, 2, 3, 4, 10, 11]);
  assert.ok(s.click(12, true, true, M)); // ⌘⇧: a range added to what was selected at the anchor
  assert.deepEqual(s.runs, [1, 13]);
  assert.ok(!s.click(12, true, true, M)); // the same: no change to report
});

test("one row at most where one is selected; none where none is", () => {
  const s = new g.RowSelection();
  assert.ok(s.click(2, true, false, g.Mode.Single));
  assert.deepEqual(s.runs, [2, 3]);
  assert.ok(s.click(2, false, true, g.Mode.Single)); // the primary modifier deselects it
  assert.deepEqual(s.runs, []);
  assert.ok(!s.click(4, false, false, g.Mode.None));
  assert.ok(!s.moved(4, false, false, g.Mode.None));
  assert.deepEqual(s.runs, []);
});

test("keys: arrows select the row moved to, Shift extends, the primary modifier moves alone, Space toggles", () => {
  const s = new g.RowSelection(), M = g.Mode.Multiple;
  s.click(5, false, false, M);
  assert.ok(s.moved(6, true, false, M));
  assert.ok(s.moved(7, true, false, M));
  assert.deepEqual(s.runs, [5, 8]);
  assert.ok(!s.moved(9, false, true, M)); // focus alone
  assert.ok(s.toggle(9, M));
  assert.deepEqual(s.runs, [5, 8, 9, 10]);
  assert.ok(s.moved(2, false, false, M));
  assert.deepEqual(s.runs, [2, 3]);
  assert.ok(s.all(100, M));
  assert.deepEqual(s.runs, [0, 100]);
  assert.ok(!s.all(100, g.Mode.Single));
});

test("a right click selects its row unless it's selected", () => {
  const s = new g.RowSelection(), M = g.Mode.Multiple;
  s.set([2, 5]);
  assert.ok(!s.ensure(3, M));
  assert.deepEqual(s.runs, [2, 5]);
  assert.ok(s.ensure(9, M));
  assert.deepEqual(s.runs, [9, 10]);
});

test("the rows in view, and a few more each side", () => {
  assert.deepEqual(g.visibleRange(0, 320, 32, 100000, 4), [0, 14]);
  assert.deepEqual(g.visibleRange(32 * 50000 + 10, 320, 32, 100000, 4), [49996, 50015]);
  assert.deepEqual(g.visibleRange(32 * 99990, 320, 32, 100000, 4), [99986, 100000]);
  assert.deepEqual(g.visibleRange(0, 320, 32, 0, 4), [0, 0]);
  assert.equal(g.scrollFor(20, 0, 320, 32), 21 * 32 - 320);
  assert.equal(g.scrollFor(2, 320, 320, 32), 64);
  assert.equal(g.scrollFor(12, 320, 320, 32), 320);
});

// Home (expanded) > Docs (expanded) > a, b; Pictures; Shared (collapsed).
const levelsBytes = bytes(new Writer().u32(6).u16(0).u8(3).u16(1).u8(3).u16(2).u8(0).u16(2).u8(0).u16(1).u8(1).u16(0).u8(1));

test("Levels decode; each row's parent, place and siblings", () => {
  const l = g.decodeLevels(levelsBytes);
  assert.deepEqual([...l.depth], [0, 1, 2, 2, 1, 0]);
  assert.deepEqual([...l.flags], [3, 3, 0, 0, 1, 1]);
  const t = g.treeInfo(l.depth);
  assert.deepEqual([...t.parent], [-1, 0, 1, 1, 0, -1]);
  assert.deepEqual([...t.pos], [1, 1, 1, 2, 2, 2]);
  assert.deepEqual([...t.size], [2, 2, 2, 2, 2, 2]);
});

test("the tree's arrows: expand, into the first child, collapse, out to the parent", () => {
  const l = g.decodeLevels(levelsBytes), p = g.treeInfo(l.depth).parent;
  assert.deepEqual(g.treeKey("in", 5, l, p), { expand: 1 });
  assert.deepEqual(g.treeKey("in", 0, l, p), { move: 1 });
  assert.equal(g.treeKey("in", 2, l, p), null);
  assert.deepEqual(g.treeKey("out", 1, l, p), { expand: 0 });
  assert.deepEqual(g.treeKey("out", 3, l, p), { move: 1 });
  assert.equal(g.treeKey("out", 5, l, p), null);
  // Expanded with no children yet (they load): Right goes nowhere.
  const lazy = g.decodeLevels(bytes(new Writer().u32(2).u16(0).u8(3).u16(0).u8(0)));
  assert.equal(g.treeKey("in", 0, lazy, g.treeInfo(lazy.depth).parent), null);
});

test("Cursor's CSS", () => {
  assert.equal(cursorCSS(Cursor.Auto), "");
  assert.equal(cursorCSS(Cursor.Arrow), "default");
  assert.equal(cursorCSS(Cursor.Pointer), "pointer");
  assert.equal(cursorCSS(Cursor.ResizeHorizontal), "ew-resize");
  assert.equal(cursorCSS(Cursor.ResizeColumn), "col-resize");
  assert.equal(cursorCSS(Cursor.ResizeRow), "row-resize");
  assert.equal(cursorCSS(Cursor.ResizeAntiDiagonal), "nesw-resize");
  assert.equal(cursorCSS(Cursor.Hidden), "none");
  assert.equal(cursorCSS(Cursor.ZoomOut), "zoom-out");
  assert.equal(cursorCSS(99), "");
  assert.equal(Object.keys(Cursor).length, 24);
});

test("undo keys: ⌘Z and ⇧⌘Z on Apple's platforms; Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y elsewhere", () => {
  const k = (key, o = {}) => ({ key, code: "Key" + key.toUpperCase(), ctrlKey: false, metaKey: false, shiftKey: false, altKey: false, ...o });
  assert.equal(undoKey(k("z", { metaKey: true }), true), 0);
  assert.equal(undoKey(k("Z", { metaKey: true, shiftKey: true }), true), 1);
  assert.equal(undoKey(k("z", { ctrlKey: true }), true), -1);
  assert.equal(undoKey(k("y", { metaKey: true }), true), -1);
  assert.equal(undoKey(k("z", { ctrlKey: true }), false), 0);
  assert.equal(undoKey(k("z", { ctrlKey: true, shiftKey: true }), false), 1);
  assert.equal(undoKey(k("y", { ctrlKey: true }), false), 1);
  assert.equal(undoKey(k("z", { ctrlKey: true, altKey: true }), false), -1);
  assert.equal(undoKey(k("z"), false), -1);
  // A Russian layout's Z key (я): where a US keyboard has Z.
  assert.equal(undoKey({ key: "я", code: "KeyZ", ctrlKey: true }, false), 0);
});

test("what's editable undoes its own typing", () => {
  const el = (tag, attrs = {}, props = {}) => ({ nodeType: 1, tagName: tag, isContentEditable: false, readOnly: false, disabled: false, getAttribute: (n) => attrs[n] ?? null, ...props });
  assert.ok(editable(el("INPUT")));
  assert.ok(editable(el("INPUT", { type: "search" })));
  assert.ok(!editable(el("INPUT", { type: "checkbox" })));
  assert.ok(!editable(el("INPUT", {}, { readOnly: true })));
  assert.ok(editable(el("TEXTAREA")));
  assert.ok(editable(el("DIV", {}, { isContentEditable: true })));
  assert.ok(!editable(el("TABLE")));
  assert.ok(!editable(null));
});
