// The style props' arithmetic, apart from the DOM: font variants,
// gradients' CSS, where a text is cut (Truncation's Head and Middle,
// which CSS has no ellipsis for) and the scale a shrinking text fits at
// (MinimumScale). The host gives each what it measured.

import { Reader, Truncation, GradientKind } from "./ops.js";

/** FontTraits' bits. */
export const Italic = 1, Digits = 2, SmallCaps = 4;

/** traitsCSS is FontTraits as CSS: [font-style, font-variant-numeric, font-variant-caps]. */
export function traitsCSS(t) {
  return [t & Italic ? "italic" : "", t & Digits ? "tabular-nums" : "", t & SmallCaps ? "small-caps" : ""];
}

/** shadowCSS is a TextShadow (or Shadow) as CSS's text-shadow, "" for none. */
export function shadowCSS(b, css) {
  if (!b || b.length < 20) return "";
  const r = new Reader(b);
  const c = r.u64(), blur = r.f32(), x = r.f32(), y = r.f32();
  return c ? `${x}px ${y}px ${blur}px ${css(c) || "transparent"}` : "";
}

const pct = (f) => +(f * 100).toFixed(3) + "%";

/**
 * gradientCSS is a Gradient as a CSS image, for a box w by h: linear
 * between its points (CSS's angle, the stops placed along the line it
 * draws), radial (a circle around its center, its radius a fraction of
 * the box's shorter side) or conic (from its angle, clockwise from the
 * top). An older encoding without a kind is linear. "" for none.
 */
export function gradientCSS(b, w, h, css) {
  if (!b || b.length < 4) return "";
  const r = new Reader(b);
  const count = r.u32();
  if (!count) return "";
  const stops = [];
  for (let i = 0; i < count; i++) stops.push([r.u64(), r.f32()]);
  const sx = r.f32(), sy = r.f32(), ex = r.f32(), ey = r.f32();
  let kind = GradientKind.Linear, cx = 0.5, cy = 0.5, radius = 0.5, angle = 0;
  if (r.p + 17 <= b.length) {
    kind = r.u8();
    cx = r.f32(); cy = r.f32(); radius = r.f32(); angle = r.f32();
  }
  const color = (c) => css(c) || "transparent";
  w = w || 1; h = h || 1;
  if (kind === GradientKind.Radial) {
    const R = Math.max(radius * Math.min(w, h), 0.001);
    const list = stops.map(([c, at]) => `${color(c)} ${pct(at)}`);
    return `radial-gradient(circle ${+R.toFixed(3)}px at ${pct(cx)} ${pct(cy)}, ${list.join(", ")})`;
  }
  if (kind === GradientKind.Conic) {
    const list = stops.map(([c, at]) => `${color(c)} ${pct(at)}`);
    return `conic-gradient(from ${+angle.toFixed(3)}deg at ${pct(cx)} ${pct(cy)}, ${list.join(", ")})`;
  }
  const dx = (ex - sx) * w, dy = (ey - sy) * h;
  const len = Math.hypot(dx, dy) || 1;
  const ux = dx / len, uy = dy / len;
  // CSS: 0deg points up, clockwise; the line goes through the center,
  // as long as the box's corners need.
  const a = Math.atan2(ux, -uy);
  const L = Math.abs(w * Math.sin(a)) + Math.abs(h * Math.cos(a)) || 1;
  const t = (px, py) => ((px * w - w / 2) * ux + (py * h - h / 2) * uy) / L + 0.5;
  const t0 = t(sx, sy), t1 = t(ex, ey);
  const list = stops.map(([c, at]) => `${color(c)} ${((t0 + at * (t1 - t0)) * 100).toFixed(3)}%`);
  return `linear-gradient(${(a * 180 / Math.PI).toFixed(3)}deg, ${list.join(", ")})`;
}

/** graphemes splits s into what a reader sees as characters, which a cut never splits. */
export function graphemes(s) {
  if (typeof Intl !== "undefined" && Intl.Segmenter) return Array.from(new Intl.Segmenter().segment(s), (g) => g.segment);
  return Array.from(s);
}

/** cutText is the text with keep of its characters kept and an ellipsis for the rest: at its start (Head) or its middle (Middle). */
export function cutText(chars, keep, mode) {
  if (keep >= chars.length) return chars.join("");
  if (mode === Truncation.Head) return "…" + chars.slice(chars.length - keep).join("");
  const front = Math.ceil(keep / 2), back = keep - front;
  return chars.slice(0, front).join("") + "…" + (back > 0 ? chars.slice(chars.length - back).join("") : "");
}

/**
 * truncate is what a line shows of s cut at its start or middle: the
 * most characters fits (a string's width test) holds with the ellipsis,
 * by bisection. All of it when it fits.
 */
export function truncate(s, mode, fits) {
  if (fits(s)) return s;
  const chars = graphemes(s);
  let lo = 0, hi = chars.length - 1; // lo always fits (an ellipsis alone, at worst)
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (fits(cutText(chars, mid, mode))) lo = mid; else hi = mid - 1;
  }
  return cutText(chars, lo, mode);
}

/**
 * fitScale is the scale a shrinking text is drawn at: the largest from
 * min to 1 at which fits (the text at a scale holds in its lines at its
 * width) holds, to a 1/256; min when none does, 1 when it's not to
 * shrink (min 0 or at least 1).
 */
export function fitScale(min, fits) {
  if (!(min > 0 && min < 1) || fits(1)) return 1;
  if (!fits(min)) return min;
  let lo = min, hi = 1;
  while (hi - lo > 1 / 256) {
    const mid = (lo + hi) / 2;
    if (fits(mid)) lo = mid; else hi = mid;
  }
  return lo;
}
