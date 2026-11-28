// A sheet's detents (src/sheet.js): Detents' bytes decoded, resolved to
// heights, where a drag let go settles, and how much a sheet that
// doesn't dim at a detent dims what's behind it.
import { test } from "node:test";
import assert from "node:assert/strict";
import { decodeDetents, resolveHeights, shortest, tallest, fitsOnly, hasFit, settle, dimming, shownHeight } from "../src/sheet.js";
import { DetentKind as K } from "../src/ops.js";

/** bytes encodes detents as the core sends them: u32 count, then u8 kind and f32 value each. */
function bytes(...ds) {
  const b = new Uint8Array(4 + 5 * ds.length);
  const v = new DataView(b.buffer);
  v.setUint32(0, ds.length, true);
  ds.forEach(([k, x], i) => {
    v.setUint8(4 + 5 * i, k);
    v.setFloat32(5 + 5 * i, x, true);
  });
  return b;
}

test("Detents decode in the app's order", () => {
  assert.deepEqual(decodeDetents(bytes([K.Height, 120], [K.Medium, 0], [K.Fraction, 0.25], [K.Large, 0])), [
    { kind: K.Height, value: 120 },
    { kind: K.Medium, value: 0 },
    { kind: K.Fraction, value: 0.25 },
    { kind: K.Large, value: 0 },
  ]);
  // None (large alone), or bytes that aren't well formed.
  assert.deepEqual(decodeDetents(bytes()), []);
  assert.deepEqual(decodeDetents(new Uint8Array([2, 0, 0, 0, 1])), []);
  assert.deepEqual(decodeDetents(undefined), []);
});

test("resolved to heights, none taller than large", () => {
  const room = { large: 800, medium: 422, fit: 0 };
  const ds = decodeDetents(bytes([K.Height, 120], [K.Medium, 0], [K.Fraction, 0.25], [K.Large, 0], [K.Height, 5000], [K.Fit, 0]));
  // Fit, before the core measured the content: large.
  assert.deepEqual(resolveHeights(ds, room), [120, 422, 200, 800, 800, 800]);
  assert.deepEqual(resolveHeights(ds, { ...room, fit: 310 }).slice(-1), [310]);
  // No detents: large alone.
  assert.deepEqual(resolveHeights([], room), [800]);
  assert.equal(shortest([422, 120, 800]), 1);
  assert.equal(tallest([422, 120, 800]), 2);
  assert.ok(fitsOnly(decodeDetents(bytes([K.Fit, 0]))));
  assert.ok(!fitsOnly(decodeDetents(bytes([K.Fit, 0], [K.Large, 0]))));
  assert.ok(hasFit(decodeDetents(bytes([K.Fit, 0], [K.Large, 0]))));
});

test("a drag settles at the detent nearest where it coasts to", () => {
  const hs = [120, 422, 800]; // any order works
  // Let go still: the nearest.
  assert.equal(settle(hs, 300, 0, true), 1);
  assert.equal(settle(hs, 700, 0, true), 2);
  assert.equal(settle(hs, 200, 0, true), 0);
  // A fling up from the small one goes past the middle one's halfway.
  assert.equal(settle(hs, 200, 2, true), 1);
  assert.equal(settle(hs, 430, 4, true), 2);
  // A fling down from medium: small.
  assert.equal(settle(hs, 400, -2, true), 0);
  // Dragged well below the shortest, or flung down from under it: dismissed.
  assert.equal(settle(hs, 50, 0, true), -1);
  assert.equal(settle(hs, 110, -1, true), -1);
  // Unless it can't be dismissed: it goes back to the shortest.
  assert.equal(settle(hs, 50, 0, false), 0);
  assert.equal(settle(hs, 110, -1, false), 0);
  // Unordered detents settle by height, not by index.
  assert.equal(settle([800, 120, 422], 380, 0, true), 2);
});

test("a detent that doesn't dim: what's behind dims above it, fading in up to the next", () => {
  const hs = [120, 422, 800];
  // Modal at every detent.
  assert.equal(dimming(hs, -1, 120), 1);
  // Undimmed up to medium: not dimmed at it, nor below.
  assert.equal(dimming(hs, 1, 120), 0);
  assert.equal(dimming(hs, 1, 422), 0);
  // Halfway to large: half.
  assert.equal(dimming(hs, 1, 611), 0.5);
  assert.equal(dimming(hs, 1, 800), 1);
  // Undimmed at the tallest: never dimmed.
  assert.equal(dimming(hs, 2, 800), 0);
});

test("its height on the screen: a bottom sheet's from the page's bottom, a card's its own", () => {
  // A bottom sheet 422 tall on an 844 page, and sliding in (its top lower).
  assert.equal(shownHeight(true, { top: 422, height: 422 }, 844), 422);
  assert.equal(shownHeight(true, { top: 600.123, height: 422 }, 844), 243.88);
  // A card centered on a wide page: its height, wherever it floats.
  assert.equal(shownHeight(false, { top: 340, height: 120 }, 800), 120);
  // Below the page: nothing shown.
  assert.equal(shownHeight(true, { top: 900, height: 422 }, 844), 0);
});
