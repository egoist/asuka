// What a Table and an Outline (table.js) work out without the page: their
// props' encodings (Columns, Sort, Selection's runs, RowData, Levels) and
// their events' (ColumnLayout, RowsSelected), the rows in view, the
// columns as the user left them, the selection as clicks and keys change
// it (the platform's own: Shift extends from the anchor, the primary
// modifier toggles), and where the tree's keys go (the WAI-ARIA tree
// pattern). No DOM here: node --test runs it.

import { Reader, Writer } from "./ops.js";

/** The width a column with none of its own has, and the least one is resized to. */
export const defaultWidth = 100, leastWidth = 32;

/** Outline's indentation per level and its disclosure's room (the core lays a row's views out after them). */
export const indent = 16, disclosure = 20;

/**
 * decodeColumns reads a Table's Columns: [{ id, title, width, min, max,
 * resizable, sortable, align }], width the app's (0: none), min and max
 * the bounds a resize keeps to (max Infinity: none).
 */
export function decodeColumns(b) {
  const out = [];
  if (!b || b.length < 4) return out;
  const r = new Reader(b);
  const n = r.u32();
  for (let i = 0; i < n && r.p < b.length; i++) {
    const id = r.string(), title = r.string();
    const width = r.f32(), min = r.f32(), max = r.f32();
    const flags = r.u8(), align = r.u8();
    if (r.p > b.length) break;
    out.push({ id, title, width, min: min > 0 ? min : leastWidth, max: max > 0 ? max : Infinity, resizable: (flags & 1) !== 0, sortable: (flags & 2) !== 0, align });
  }
  return out;
}

/** clampWidth is w kept within a column's min and max. */
export function clampWidth(w, col) {
  return Math.min(Math.max(w, col.min), Math.max(col.max, col.min));
}

/** decodeSort reads a Table's Sort: [{ column, ascending }], the first the primary. */
export function decodeSort(b) {
  const out = [];
  if (!b || b.length < 4) return out;
  const r = new Reader(b);
  const n = r.u32();
  for (let i = 0; i < n && r.p + 5 <= b.length; i++) out.push({ column: r.u32(), ascending: r.u8() !== 0 });
  return out;
}

/**
 * layoutColumns is the columns as shown: { order: model indices in the
 * order shown, widths: by model index }. While cols have the IDs the
 * last layout (prev, with the IDs it was of) had, the user's order and
 * the widths the user gave stay; otherwise the app's order, and its
 * widths (a column the user resized keeps that width while its ID does).
 */
export function layoutColumns(prev, cols) {
  const ids = cols.map((c) => c.id);
  const widths = cols.map((c, i) => {
    const user = prev?.user?.get(c.id);
    return clampWidth(user ?? (c.width > 0 ? c.width : defaultWidth), c);
  });
  let order = cols.map((_, i) => i);
  if (prev && prev.ids.length === ids.length && prev.ids.every((id) => ids.includes(id))) {
    order = prev.order.map((k) => ids.indexOf(prev.ids[k]));
  }
  const user = new Map();
  for (const [id, w] of prev?.user || []) if (ids.includes(id)) user.set(id, w);
  return { ids, order, widths, user };
}

/** encodeColumnLayout is ColumnLayout's bytes: u32 count, per column shown (from the start) u32 its model index and f32 its width. */
export function encodeColumnLayout(order, widths) {
  const w = new Writer().u32(order.length);
  for (const k of order) w.u32(k).f32(widths[k]);
  return w.buf.slice(0, w.n);
}

/** moveColumn is order with the column shown at from moved to be shown at to. */
export function moveColumn(order, from, to) {
  const out = order.slice();
  const [k] = out.splice(from, 1);
  out.splice(to, 0, k);
  return out;
}

/** dropIndex is where a header dragged to x goes among headers whose edges are given ([start, end] each, in display order, x growing toward the end). */
export function dropIndex(edges, from, x) {
  let to = 0;
  for (let i = 0; i < edges.length; i++) {
    const mid = (edges[i][0] + edges[i][1]) / 2;
    if (x > mid) to = i + 1;
  }
  return to > from ? to - 1 : to;
}

/**
 * decodeRowData reads a cell's RowData: { cells: its strings (a Table's,
 * one per column of Columns; an Outline's one), symbol: the symbol its
 * first cell shows before its text ("": none) }.
 */
export function decodeRowData(b) {
  const cells = [];
  if (!b || b.length < 4) return { cells, symbol: "" };
  const r = new Reader(b);
  const n = r.u32();
  for (let i = 0; i < n && r.p + 4 <= b.length; i++) cells.push(r.string());
  const symbol = r.p + 4 <= b.length ? r.string() : "";
  return { cells, symbol };
}

/** decodeLevels reads an Outline's Levels: { depth: Uint16Array, flags: Uint8Array } (flags 1 expandable, 2 expanded). */
export function decodeLevels(b) {
  if (!b || b.length < 4) return { depth: new Uint16Array(0), flags: new Uint8Array(0) };
  const r = new Reader(b);
  const n = Math.min(r.u32(), Math.floor((b.length - 4) / 3));
  const depth = new Uint16Array(n), flags = new Uint8Array(n);
  for (let i = 0; i < n; i++) { depth[i] = r.u16(); flags[i] = r.u8(); }
  return { depth, flags };
}

/**
 * treeInfo is each row's place among its siblings, from the rows' depths
 * (depth first): parent (the nearest row before it one level up; -1 for a
 * root), pos (1-based among its siblings: aria-posinset) and size (how
 * many siblings: aria-setsize).
 */
export function treeInfo(depth) {
  const n = depth.length;
  const parent = new Int32Array(n), pos = new Int32Array(n), size = new Int32Array(n);
  const last = []; // per depth, the last row at it (its siblings' counter by parent)
  const counts = new Map(); // parent row (-1: the roots) -> siblings so far
  for (let i = 0; i < n; i++) {
    const d = depth[i];
    last.length = Math.min(last.length, d + 1);
    let p = -1;
    for (let k = Math.min(d, last.length) - 1; k >= 0; k--) {
      if (last[k] != null) { p = last[k]; break; }
    }
    parent[i] = p;
    const c = (counts.get(p) || 0) + 1;
    counts.set(p, c);
    pos[i] = c;
    last[d] = i;
  }
  for (let i = 0; i < n; i++) size[i] = counts.get(parent[i]);
  return { parent, pos, size };
}

/**
 * treeKey is what an arrow key does at row (the WAI-ARIA tree pattern),
 * key "in" (Right, Left right to left) or "out": { expand: 1 or 0 } to
 * ask the core to expand or collapse the row, { move: row } to move to
 * its first child or its parent, or null.
 */
export function treeKey(key, row, levels, parent) {
  const n = levels.flags.length;
  if (row < 0 || row >= n) return null;
  const f = levels.flags[row];
  if (key === "in") {
    if ((f & 1) && !(f & 2)) return { expand: 1 };
    if ((f & 2) && row + 1 < n && parent[row + 1] === row) return { move: row + 1 };
    return null;
  }
  if (f & 2) return { expand: 0 };
  return parent[row] >= 0 ? { move: parent[row] } : null;
}

/**
 * visibleRange is the rows [first, last) to show of count rows rowHeight
 * high, scrolled by offset in a viewport viewport high, with overscan
 * rows more each side.
 */
export function visibleRange(offset, viewport, rowHeight, count, overscan) {
  if (count <= 0 || rowHeight <= 0) return [0, 0];
  const first = Math.max(0, Math.floor(offset / rowHeight) - overscan);
  const last = Math.min(count, Math.ceil((offset + Math.max(viewport, 0)) / rowHeight) + overscan);
  return [Math.min(first, last), last];
}

/** scrollFor is the offset that shows row (keeping offset where it's already in view). */
export function scrollFor(row, offset, viewport, rowHeight) {
  const top = row * rowHeight, bottom = top + rowHeight;
  if (top < offset) return top;
  if (bottom > offset + viewport) return Math.max(0, bottom - viewport);
  return offset;
}

// --- Selection: sorted runs ---

/** decodeRuns reads Selection's (and RowsSelected's) runs: [first, end, first, end, ...], sorted and merged, below count when given. */
export function decodeRuns(b, count = Infinity) {
  const out = [];
  if (!b || b.length < 4) return out;
  const r = new Reader(b);
  const n = r.u32();
  for (let i = 0; i < n && r.p + 8 <= b.length; i++) {
    const first = r.u32(), len = r.u32();
    const end = Math.min(first + len, count);
    if (end > first) out.push(first, end);
  }
  return normalize(out);
}

/** encodeRuns is runs' bytes: u32 count, per run u32 first row, u32 length. */
export function encodeRuns(runs) {
  const w = new Writer().u32(runs.length / 2);
  for (let i = 0; i < runs.length; i += 2) w.u32(runs[i]).u32(runs[i + 1] - runs[i]);
  return w.buf.slice(0, w.n);
}

/** normalize sorts runs ([first, end) pairs) and merges those that touch. */
export function normalize(runs) {
  const pairs = [];
  for (let i = 0; i < runs.length; i += 2) if (runs[i + 1] > runs[i]) pairs.push([runs[i], runs[i + 1]]);
  pairs.sort((a, b) => a[0] - b[0]);
  const out = [];
  for (const [a, b] of pairs) {
    if (out.length && a <= out[out.length - 1]) out[out.length - 1] = Math.max(out[out.length - 1], b);
    else out.push(a, b);
  }
  return out;
}

export function runsHas(runs, i) {
  let lo = 0, hi = runs.length / 2;
  while (lo < hi) {
    const m = (lo + hi) >> 1;
    if (runs[2 * m + 1] <= i) lo = m + 1;
    else hi = m;
  }
  return lo < runs.length / 2 && runs[2 * lo] <= i;
}

export function runsCount(runs) {
  let n = 0;
  for (let i = 0; i < runs.length; i += 2) n += runs[i + 1] - runs[i];
  return n;
}

function without(runs, i) {
  const out = [];
  for (let k = 0; k < runs.length; k += 2) {
    const a = runs[k], b = runs[k + 1];
    if (i < a || i >= b) { out.push(a, b); continue; }
    if (a < i) out.push(a, i);
    if (i + 1 < b) out.push(i + 1, b);
  }
  return out;
}

const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

/** SelectionMode: Single, Multiple, None. */
export const Mode = Object.freeze({ Single: 0, Multiple: 1, None: 2 });

/**
 * RowSelection is the rows selected, as runs, and the anchor a Shift
 * extends from. Its changes say whether the selection changed (to be
 * reported); set (the app's) doesn't move the anchor unless it's gone.
 */
export class RowSelection {
  constructor() {
    this.runs = [];
    this.anchor = -1;
    this.base = []; // the selection as the anchor was set: what a primary+Shift click adds a range to
  }

  has(i) { return runsHas(this.runs, i); }
  get count() { return runsCount(this.runs); }
  first() { return this.runs.length ? this.runs[0] : -1; }

  set(runs) {
    this.runs = runs;
    if (this.anchor >= 0 && !this.has(this.anchor) && !runs.length) this.anchor = -1;
  }

  change(runs) {
    if (same(runs, this.runs)) return false;
    this.runs = runs;
    return true;
  }

  setAnchor(i) { this.anchor = i; this.base = this.runs; }

  /** click: a click on row with shift and primary (⌘, Ctrl) held, in mode. */
  click(row, shift, primary, mode) {
    if (mode === Mode.None || row < 0) return false;
    if (mode === Mode.Single) {
      const r = primary && this.has(row) ? [] : [row, row + 1];
      this.anchor = row;
      return this.change(r);
    }
    if (shift) {
      const a = this.anchor >= 0 ? this.anchor : row;
      const range = [Math.min(a, row), Math.max(a, row) + 1];
      return this.change(normalize(primary ? [...this.base, ...range] : range));
    }
    let changed;
    if (primary) changed = this.change(this.has(row) ? without(this.runs, row) : normalize([...this.runs, row, row + 1]));
    else changed = this.change([row, row + 1]);
    this.setAnchor(row);
    return changed;
  }

  /** moved: the keyboard moved the active row to row (an arrow, Home, a page), Shift extending, the primary modifier moving the focus alone. */
  moved(row, shift, primary, mode) {
    if (mode === Mode.None || row < 0) return false;
    if (mode === Mode.Multiple && shift) {
      const a = this.anchor >= 0 ? this.anchor : row;
      if (this.anchor < 0) this.setAnchor(row);
      return this.change([Math.min(a, row), Math.max(a, row) + 1]);
    }
    if (primary && mode === Mode.Multiple) return false;
    const changed = this.change([row, row + 1]);
    this.setAnchor(row);
    return changed;
  }

  /** toggle: Space at row: toggles it where several are selected, selects it alone where one is. */
  toggle(row, mode) {
    if (mode === Mode.None || row < 0) return false;
    if (mode === Mode.Single) {
      this.anchor = row;
      return this.change([row, row + 1]);
    }
    const changed = this.change(this.has(row) ? without(this.runs, row) : normalize([...this.runs, row, row + 1]));
    this.setAnchor(row);
    return changed;
  }

  /** all: the primary modifier and A, where several are selected. */
  all(count, mode) {
    if (mode !== Mode.Multiple || count <= 0) return false;
    return this.change([0, count]);
  }

  /** ensure: a right click on row selects it, unless it's selected already. */
  ensure(row, mode) {
    if (mode === Mode.None || row < 0 || this.has(row)) return false;
    const changed = this.change([row, row + 1]);
    this.setAnchor(row);
    return changed;
  }
}
