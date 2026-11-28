// A sheet's detents (SheetProps.Detents, DetentKind): decoded, resolved
// to heights, and where a drag lets go settles, as iOS's and
// @gorhom/bottom-sheet's do: the detent nearest to where the sheet would
// coast to at its velocity, a fling past it going to the next one.
import { DetentKind } from "./ops.js";

/** decodeDetents reads Detents' bytes: u32 count, then per detent u8 DetentKind and f32 its value. */
export function decodeDetents(b) {
  if (!(b instanceof Uint8Array) || b.length < 4) return [];
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const n = v.getUint32(0, true);
  if (b.length !== 4 + 5 * n) return [];
  const out = [];
  for (let i = 0; i < n; i++) out.push({ kind: v.getUint8(4 + 5 * i), value: v.getFloat32(5 + 5 * i, true) });
  return out;
}

/**
 * resolveHeights is each detent's height in points, in their order: room
 * gives large's and medium's (the platform's), and fit's (the content's,
 * as the core's Fit measured it; large's until it has). None is taller
 * than large. No detents: large alone.
 */
export function resolveHeights(detents, room) {
  const { large, medium } = room;
  const ds = detents.length ? detents : [{ kind: DetentKind.Large, value: 0 }];
  return ds.map((d) => {
    let h = large;
    switch (d.kind) {
      case DetentKind.Medium: h = medium; break;
      case DetentKind.Fit: h = room.fit > 0 ? room.fit : large; break;
      case DetentKind.Height: h = d.value; break;
      case DetentKind.Fraction: h = large * d.value; break;
    }
    return Math.max(0, Math.min(h, large));
  });
}

/** shortest is the index of the shortest height (the first of equals). */
export function shortest(heights) {
  let at = 0;
  for (let i = 1; i < heights.length; i++) if (heights[i] < heights[at]) at = i;
  return at;
}

/** tallest is the index of the tallest height (the first of equals). */
export function tallest(heights) {
  let at = 0;
  for (let i = 1; i < heights.length; i++) if (heights[i] > heights[at]) at = i;
  return at;
}

/** fitsOnly reports whether a sheet's one detent is fit: one the core sizes to its content. */
export function fitsOnly(detents) { return detents.length === 1 && detents[0].kind === DetentKind.Fit; }

/** hasFit reports whether a sheet has a detent of its content's height. */
export function hasFit(detents) { return detents.some((d) => d.kind === DetentKind.Fit); }

// How far a drag coasts: its velocity (points a millisecond) for this
// long, as a fling's projection (UIScrollView's normal deceleration
// rate, roughly).
const coast = 120;

/**
 * settle is where a drag let go at height h (points of the sheet shown),
 * moving at velocity v (points a millisecond, up positive), comes to
 * rest: a detent's index, or -1 to dismiss (only when dismissible, past
 * half the shortest detent's height, or flung down from it).
 */
export function settle(heights, h, v, dismissible) {
  const to = h + v * coast;
  let at = 0, best = Infinity;
  for (let i = 0; i < heights.length; i++) {
    const d = Math.abs(heights[i] - to);
    if (d < best || (d === best && heights[i] < heights[at])) { best = d; at = i; }
  }
  if (dismissible) {
    const low = heights[shortest(heights)];
    if (to < low / 2 || (h < low && v < -0.5)) return -1;
  }
  return at;
}

/**
 * shownHeight is a sheet's height on the screen (SheetHeight), to two
 * decimals, from its rectangle r (as drawn: what slides it in or out
 * counts): a bottom sheet's is from the page's bottom (bottom) to its
 * top; a card's (a wide page's, a Window panel's), floating, is its own
 * height, as a desktop's sheet window's is.
 */
export function shownHeight(bottomSheet, r, bottom) {
  const h = bottomSheet ? bottom - r.top : r.height;
  return Math.max(0, Math.round(h * 100) / 100);
}

/**
 * dimming is how much a sheet shown h points tall dims what's behind it
 * (0 to 1), when it doesn't dim at undimmed's height or lower: it fades
 * in up to the next detent above that one (or by 1 point, with none), as
 * iOS's does. undimmed < 0: it always dims.
 */
export function dimming(heights, undimmed, h) {
  if (undimmed < 0 || undimmed >= heights.length) return 1;
  const low = heights[undimmed];
  if (h <= low) return 0;
  let next = Infinity;
  for (const x of heights) if (x > low && x < next) next = x;
  if (!isFinite(next)) return 1;
  return Math.min(1, (h - low) / (next - low));
}
