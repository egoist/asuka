// The style props' arithmetic (src/textstyle.js): font variants,
// gradients' CSS, cutting a line at its start or middle, and the scale a
// shrinking text fits at.
import { test } from "node:test";
import assert from "node:assert/strict";
import { traitsCSS, shadowCSS, gradientCSS, truncate, cutText, graphemes, fitScale } from "../src/textstyle.js";
import { Truncation, GradientKind } from "../src/ops.js";

const css = (c) => "#" + (c >>> 0).toString(16).padStart(8, "0");

/** gradient encodes a Gradient as the core does: stops, the line, then (unless old) its kind, center, radius and angle. */
function gradient(stops, line, rest) {
  const n = 4 + stops.length * 12 + 16 + (rest ? 17 : 0);
  const b = new Uint8Array(n), v = new DataView(b.buffer);
  let p = 0;
  v.setUint32(p, stops.length, true); p += 4;
  for (const [c, at] of stops) {
    v.setUint32(p, c, true); v.setUint32(p + 4, 2, true); p += 8; // an RGBA color: kind 2 in the high word
    v.setFloat32(p, at, true); p += 4;
  }
  for (const f of line) { v.setFloat32(p, f, true); p += 4; }
  if (rest) {
    b[p++] = rest[0];
    for (const f of rest.slice(1)) { v.setFloat32(p, f, true); p += 4; }
  }
  return b;
}

const stops = [[0xff0000ff, 0], [0x0000ffff, 1]];

test("font traits as CSS", () => {
  assert.deepEqual(traitsCSS(0), ["", "", ""]);
  assert.deepEqual(traitsCSS(1 | 2 | 4), ["italic", "tabular-nums", "small-caps"]);
  assert.deepEqual(traitsCSS(2), ["", "tabular-nums", ""]);
});

test("a text shadow as CSS", () => {
  const b = new Uint8Array(20), v = new DataView(b.buffer);
  assert.equal(shadowCSS(b, css), ""); // color 0: none
  v.setUint32(0, 0x00000080, true); v.setUint32(4, 2, true);
  v.setFloat32(8, 4, true); v.setFloat32(12, 1, true); v.setFloat32(16, 2, true);
  assert.equal(shadowCSS(b, css), "1px 2px 4px #00000080");
  assert.equal(shadowCSS(new Uint8Array(0), css), "");
});

test("a linear gradient, with or without a kind", () => {
  const old = gradientCSS(gradient(stops, [0, 0, 0, 1]), 100, 50, css);
  assert.match(old, /^linear-gradient\(180\.000deg, #ff0000ff 0\.000%, #0000ffff 100\.000%\)$/);
  assert.equal(gradientCSS(gradient(stops, [0, 0, 0, 1], [GradientKind.Linear, 0.5, 0.5, 0.5, 0]), 100, 50, css), old);
  assert.equal(gradientCSS(gradient([], [0, 0, 0, 1]), 100, 50, css), "");
  assert.equal(gradientCSS(null, 100, 50, css), "");
});

test("a radial gradient: a circle, its radius a fraction of the shorter side", () => {
  const g = gradient(stops, [0, 0, 0, 1], [GradientKind.Radial, 0.25, 0.75, 0.6, 0]);
  assert.equal(gradientCSS(g, 200, 100, css), "radial-gradient(circle 60px at 25% 75%, #ff0000ff 0%, #0000ffff 100%)");
  assert.equal(gradientCSS(g, 50, 300, css), "radial-gradient(circle 30px at 25% 75%, #ff0000ff 0%, #0000ffff 100%)");
});

test("a conic gradient: from its angle, clockwise from the top", () => {
  const g = gradient([[0xff0000ff, 0], [0x00ff00ff, 0.5], [0xff0000ff, 1]], [0, 0, 0, 1], [GradientKind.Conic, 0.5, 0.5, 0.5, 90]);
  assert.equal(gradientCSS(g, 100, 100, css), "conic-gradient(from 90deg at 50% 50%, #ff0000ff 0%, #00ff00ff 50%, #ff0000ff 100%)");
});

// Each character 10 wide, the ellipsis too.
const fitsIn = (w) => (s) => graphemes(s).length * 10 <= w;

test("a line cut at its start keeps its end", () => {
  const s = "A long title";
  assert.equal(truncate(s, Truncation.Head, fitsIn(1000)), s);
  assert.equal(truncate(s, Truncation.Head, fitsIn(60)), "…title");
  assert.equal(truncate(s, Truncation.Head, fitsIn(5)), "…");
});

test("a line cut in its middle keeps both ends, the start's the longer", () => {
  assert.equal(truncate("A long title", Truncation.Middle, fitsIn(60)), "A l…le");
  assert.equal(truncate("A long title", Truncation.Middle, fitsIn(70)), "A l…tle");
  assert.equal(cutText(graphemes("abcdef"), 1, Truncation.Middle), "a…");
});

test("a cut never splits a character", () => {
  const s = "👨‍👩‍👧‍👦👍🏽abc";
  assert.equal(truncate(s, Truncation.Middle, fitsIn(30)), "👨‍👩‍👧‍👦…c");
});

test("the scale a shrinking text fits at", () => {
  // It fits at its own size, or at scales up to 0.5.
  assert.equal(fitScale(0.25, () => true), 1);
  const s = fitScale(0.25, (k) => k <= 0.5);
  assert.ok(s <= 0.5 && s > 0.5 - 1 / 128, String(s));
  // It can't shrink enough: its minimum.
  assert.equal(fitScale(0.75, (k) => k <= 0.5), 0.75);
  // 0 and 1: it doesn't shrink, whatever fits says.
  assert.equal(fitScale(0, () => false), 1);
  assert.equal(fitScale(1, () => false), 1);
  // Few measurements: one for its size, one for its minimum, eight between.
  let probes = 0;
  fitScale(0.25, (k) => { probes++; return k <= 0.6; });
  assert.ok(probes <= 11, String(probes));
});
