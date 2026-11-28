// The arithmetic of the host's gestures, apart from the DOM: when a second
// tap is a double tap, when a drag is a pan (PanAxis, PanMinDistance),
// how a zoom keeps the point under the fingers, and where a row a drag
// lifted goes among a list's rows (Reorder). interaction.js, zoom.js and
// list.js use it.

import { Reader } from "./ops.js";

/** doubleTime is how soon, in ms, a second tap follows the first to make a double tap: a desktop's double-click time (macOS's and Windows' default), a touch screen's (Android's DOUBLE_TAP_TIMEOUT). */
export function doubleTime(type) { return type === "touch" ? 300 : 500; }

/** doubleReach is how far, in points, a second tap may fall from the first: a mouse's double-click rectangle, a finger's slop. */
export function doubleReach(type) { return type === "touch" ? 40 : 6; }

/** isSecondTap says tap p ({t, x, y}, its press) is the second of a double tap after last ({t: its release, x, y}), with a pointer of type. */
export function isSecondTap(last, p, type) {
  return !!last && p.t - last.t <= doubleTime(type) && Math.hypot(p.x - last.x, p.y - last.y) <= doubleReach(type);
}

/**
 * panDecision is what a drag that has moved dx, dy is to a Pan along
 * axis (PanAxis: 0 both, 1 horizontal, 2 vertical) that begins past min
 * points: 0 not yet decided, 1 a pan, -1 not a pan (it moved first along
 * the other axis: what's around it, a scroll, has it).
 */
export function panDecision(dx, dy, axis, min) {
  if (Math.hypot(dx, dy) <= min) return 0;
  const ax = Math.abs(dx), ay = Math.abs(dy);
  if (axis === 1 && ay > ax) return -1;
  if (axis === 2 && ax > ay) return -1;
  return 1;
}

/** zoomScroll is the scroll offset that keeps the point at px, py (from the scroller's top left) on the same content as zoom goes from z0 to z1, given the offset sx, sy at z0. */
export function zoomScroll(sx, sy, px, py, z0, z1) {
  const k = z1 / z0;
  return [(sx + px) * k - px, (sy + py) * k - py];
}

/** rubberZoom is a pinch's zoom z kept near min and max: past them it gives way less and less, as UIScrollView's bounces. */
export function rubberZoom(z, min, max) {
  if (z > max) return max * Math.pow(z / max, 0.35);
  if (z < min) return min * Math.pow(z / min, 0.35);
  return z;
}

/** clampZoom is z within min and max. */
export function clampZoom(z, min, max) { return Math.min(Math.max(z, min), max); }

/** doubleTapZoom is where a double tap takes a zoom: in to twice min (max at most), or back out to min when it's zoomed in. */
export function doubleTapZoom(z, min, max) {
  return z > min + 0.01 ? min : Math.min(2 * min, max);
}

/**
 * reorderTarget is the row a lifted row (at index from) goes to, given
 * each row's top and bottom as they were laid out (tops, its length the
 * rows' and one more) and the lifted row's middle, y: rows above whose
 * middle it's above make way down, rows below whose middle it's below
 * make way up. first is the first row it may go to (1 with sections: not
 * above the first header).
 */
export function reorderTarget(tops, from, y, first = 0) {
  let to = from;
  for (let r = Math.max(first, 0); r < from; r++) {
    if (y < (tops[r] + tops[r + 1]) / 2) { to = r; break; }
  }
  if (to === from) {
    for (let r = tops.length - 2; r > from; r--) {
      if (y > (tops[r] + tops[r + 1]) / 2) { to = r; break; }
    }
  }
  return to;
}

/** shiftOf is how far row r moves to make way for a row lifted from from and held at to, of height size. */
export function shiftOf(r, from, to, size) {
  if (to < from && r >= to && r < from) return size;
  if (to > from && r > from && r <= to) return -size;
  return 0;
}

/**
 * moveRow moves a list's row: rows ({header, section, first}: a header,
 * or an item row of one item, first its index) the list's, ids its items'
 * IDs, sections its [{start, id}]; the row at from goes to to. It's the
 * IDs and sections after, and the moved item's index before and after
 * ({ids, sections, from, to}), or null when it isn't a move (a header, or
 * the item's index and section don't change).
 */
export function moveRow(rows, ids, sections, from, to) {
  const row = rows[from];
  if (!row || row.header || from === to) return null;
  const order = rows.slice();
  order.splice(from, 1);
  order.splice(to, 0, row);
  const nids = [], nsecs = [];
  let at = -1, sec = -1;
  for (const r of order) {
    if (r.header) {
      nsecs.push({ start: nids.length, id: sections[r.section].id });
      continue;
    }
    if (r === row) { at = nids.length; sec = nsecs.length - 1; }
    nids.push(ids[r.first]);
  }
  if (at === row.first) return null; // its index the same: under another header, or nowhere
  return { ids: nids, sections: nsecs, from: row.first, to: at, section: sec };
}

/** moveID moves the ID at from to to (a move by its flat index, as the core's), in a copy; sections keep their starts. */
export function moveID(ids, from, to) {
  const out = ids.slice();
  const [id] = out.splice(from, 1);
  out.splice(to, 0, id);
  return out;
}

/** decodeReorder reads a List's Reorder: its move actions' names ({up, down}), or null when its rows don't move. */
export function decodeReorder(b) {
  if (!b || b.length < 4) return null;
  const r = new Reader(b);
  const n = r.u32();
  const names = [];
  for (let i = 0; i < n && r.p + 4 <= b.length; i++) names.push(r.string());
  return { up: names[0] || "Move up", down: names[1] || "Move down" };
}
