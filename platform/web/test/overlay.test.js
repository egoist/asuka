// An Overlay (src/overlay.js): AnchorFrame as ops.AppendAnchorFrame
// writes it, whether an anchor shows within what clips it, the overlays'
// order, presses outside an overlay (modal and not), what's inert under a
// modal one, and an anchor's frame reported once per change.
import { test } from "node:test";
import assert from "node:assert/strict";
import { encodeAnchorFrame, sameBytes, shows, stacked, Overlays, overlayKind } from "../src/overlay.js";
import { Kind, KindProp, Event } from "../src/ops.js";

const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");

test("AnchorFrame is ops.AppendAnchorFrame's", () => {
  // ops.AppendAnchorFrame(nil, ops.AnchorFrame{1.5, -2, 30, 40.25, true}) and {10, 20, 0, 0, false}.
  assert.equal(hex(encodeAnchorFrame(1.5, -2, 30, 40.25, true)), "0000c03f000000c00000f0410000214201");
  assert.equal(hex(encodeAnchorFrame(10, 20, 0, 0, false)), "000020410000a041000000000000000000");
  assert.ok(sameBytes(encodeAnchorFrame(1, 2, 3, 4, true), encodeAnchorFrame(1, 2, 3, 4, true)));
  assert.ok(!sameBytes(encodeAnchorFrame(1, 2, 3, 4, true), encodeAnchorFrame(1, 2, 3, 4, false)));
  // Unchanged in f32: the same (a fraction past f32's isn't a change).
  assert.ok(sameBytes(encodeAnchorFrame(0.1, 0, 0, 0, true), encodeAnchorFrame(0.1000000001, 0, 0, 0, true)));
  assert.ok(!sameBytes(encodeAnchorFrame(1, 2, 3, 4, true), null));
});

test("an anchor shows while any of it is within each clip", () => {
  const R = (left, top, right, bottom) => ({ left, top, right, bottom });
  const root = R(0, 0, 400, 800);
  assert.ok(shows(R(10, 10, 50, 50), [root]));
  assert.ok(shows(R(-20, 10, 1, 50), [root]), "partly in");
  assert.ok(!shows(R(-20, 10, 0, 50), [root]), "touching isn't in");
  assert.ok(!shows(R(10, 900, 50, 950), [root]), "below the window");
  // A scroll view around it: scrolled out of its view.
  const scroller = R(0, 100, 400, 220);
  assert.ok(shows(R(10, 150, 50, 190), [root, scroller]));
  assert.ok(!shows(R(10, 40, 50, 90), [root, scroller]));
  // A rectangle of no size: its point.
  assert.ok(shows(R(10, 150, 10, 150), [root, scroller]));
  assert.ok(!shows(R(10, 90, 10, 90), [root, scroller]));
});

test("overlays show in the order they were presented", () => {
  const a = { ov: { seq: 3 } }, b = { ov: { seq: 1 } }, c = { ov: { seq: 2 } };
  assert.deepEqual(stacked([a, b, c]), [b, c, a]);
});

// --- A fake page ---

/** element is a DOM element under parent: what overlay.js reads of one. */
function element(parent, rect = { left: 0, top: 0, width: 400, height: 800 }) {
  const el = {
    parentElement: parent,
    parentNode: parent,
    nodeType: 1,
    isConnected: true,
    rect,
    children: [],
    style: {},
    classes: new Set(),
    classList: { toggle(c, on) { on ? el.classes.add(c) : el.classes.delete(c); }, contains: (c) => el.classes.has(c) },
    contains(x) { for (; x; x = x.parentElement) if (x === el) return true; return false; },
    getBoundingClientRect() {
      const r = el.rect;
      return { left: r.left, top: r.top, width: r.width, height: r.height, right: r.left + r.width, bottom: r.top + r.height };
    },
    getClientRects() { return [el.rect]; },
    closest() { return null; },
    matches() { return false; },
    appendChild(c) { c.parentElement = c.parentNode = el; el.children.push(c); },
  };
  return el;
}

function page() {
  globalThis.document = { activeElement: null, createElement: () => element(null) };
  globalThis.getComputedStyle ??= () => ({ overflowX: "visible", overflowY: "visible" });
  const container = element(null);
  const sent = [];
  const h = {
    container,
    options: { fullPage: true },
    nodes: new Map(),
    nav: { dialogs: [], report() {} },
    focus: { quiet: 0 },
    dispatch: (b) => sent.push(decode(b)),
    afterCore: (f) => f(),
    setModalInert(n, on) { n.modalInert = on; },
  };
  let next = 1;
  const node = (kind, parentEl = container, parent = null) => {
    const n = { id: next++, kind, el: element(parentEl), parent, listens: new Set(), listening(e) { return this.listens.has(e); } };
    n.el.asukaNode = n;
    if (parentEl) parentEl.children.push(n.el);
    h.nodes.set(n.id, n);
    return n;
  };
  h.root = node(Kind.View);
  h.overlays = new Overlays(h);
  const overlay = (root = h.root.id) => {
    const n = { id: next++, kind: Kind.Overlay, parent: null, listens: new Set([Event.OutsidePress, Event.AnchorFrame]), listening(e) { return this.listens.has(e); } };
    overlayKind.create(h, n);
    // A popover shows while it's in the page (in the top layer).
    n.el.matches = () => !!n.el.parentElement;
    n.el.showPopover = () => {};
    n.el.hidePopover = () => {};
    n.el.asukaNode = n;
    h.nodes.set(n.id, n);
    overlayKind.prop(h, n, KindProp.Overlay.Root, root);
    return n;
  };
  return { h, node, overlay, sent, container };
}

function decode(b) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const e = { id: v.getUint32(0, true), kind: v.getUint16(4, true) };
  if (e.kind === Event.OutsidePress) { e.x = v.getFloat64(6, true); e.y = v.getFloat64(14, true); }
  if (e.kind === Event.AnchorFrame) e.bytes = hex(b.subarray(10));
  return e;
}

/** child puts a view of the core's in overlay o, at a frame of its root's. */
function child(o, rect) {
  const el = element(o.el, rect);
  el.asukaNode = { id: 1000 + o.el.children.length, kind: Kind.View };
  o.el.appendChild(el);
  return el;
}

test("a press outside an overlay's views, in its root, is reported to it, in the root's coordinates", () => {
  const { h, node, overlay, sent } = page();
  h.root.el.rect = { left: 20, top: 30, width: 400, height: 800 };
  const button = node(Kind.Button, h.root.el);
  const o = overlay();
  h.overlays.present(o);
  assert.equal(o.el.parentElement, h.container, "over the main window: in the app's element");
  assert.deepEqual([o.el.style.left, o.el.style.top, o.el.style.width, o.el.style.height], ["20px", "30px", "400px", "800px"]);
  const item = child(o, { left: 40, top: 60, width: 100, height: 40 });
  const inner = element(item);

  // On what's under it: reported, before it reaches it (the press goes on).
  h.overlays.pointerDown({ target: button.el, clientX: 120, clientY: 230 });
  assert.deepEqual(sent.splice(0), [{ id: o.id, kind: Event.OutsidePress, x: 100, y: 200 }]);
  // On its views: not.
  h.overlays.pointerDown({ target: inner, clientX: 50, clientY: 70 });
  assert.deepEqual(sent.splice(0), []);
  // Not listening: not.
  o.listens.delete(Event.OutsidePress);
  h.overlays.pointerDown({ target: button.el, clientX: 120, clientY: 230 });
  assert.deepEqual(sent.splice(0), []);
  o.listens.add(Event.OutsidePress);

  // Modal: a press on its own layer (what's behind gets none) is reported.
  overlayKind.prop(h, o, KindProp.Overlay.Modal, true);
  assert.ok(o.el.classes.has("asuka-overlay-modal"), "it takes the presses");
  h.overlays.pointerDown({ target: o.el, clientX: 25, clientY: 35 });
  assert.deepEqual(sent.splice(0), [{ id: o.id, kind: Event.OutsidePress, x: 5, y: 5 }]);
});

test("a press in another root isn't outside an overlay over this one", () => {
  const { h, node, overlay, sent, container } = page();
  const dialog = element(container);
  const sheet = node(Kind.Sheet, dialog);
  sheet.dialog = dialog;
  h.nav.dialogs.push(sheet);
  const inSheet = node(Kind.View, sheet.el, sheet);
  const main = overlay();
  const over = overlay(sheet.id);
  h.overlays.present(main);
  h.overlays.present(over);
  assert.equal(over.el.parentElement, dialog, "over a sheet: in its dialog");
  h.overlays.pointerDown({ target: inSheet.el, clientX: 10, clientY: 10 });
  assert.deepEqual(sent.splice(0).map((e) => e.id), [over.id], "the sheet's overlay only");
  // In the main overlay's views: outside the sheet's overlay? A different root: no.
  const item = child(main, { left: 0, top: 0, width: 10, height: 10 });
  h.overlays.pointerDown({ target: item, clientX: 5, clientY: 5 });
  assert.deepEqual(sent.splice(0), []);
});

test("under the topmost modal overlay, its root and the overlays under it are inert", () => {
  const { h, overlay } = page();
  const a = overlay(), b = overlay(), c = overlay();
  for (const o of [a, b, c]) h.overlays.present(o);
  assert.ok(!h.root.modalInert);
  overlayKind.prop(h, b, KindProp.Overlay.Modal, true);
  assert.deepEqual([h.root.modalInert, a.modalInert, b.modalInert, c.modalInert].map(Boolean), [true, true, false, false]);
  // Presented again, a is over b: not inert.
  h.overlays.present(a);
  assert.deepEqual([h.root.modalInert, a.modalInert, b.modalInert, c.modalInert].map(Boolean), [true, false, false, false]);
  // Hidden (waiting for its anchor), a modal one blocks nothing yet.
  b.hiddenInert = true;
  h.overlays.updateModal();
  assert.ok(!h.root.modalInert);
  b.hiddenInert = false;
  h.overlays.updateModal();
  assert.ok(h.root.modalInert);
  // Gone: nothing is.
  h.overlays.remove(b);
  assert.deepEqual([h.root.modalInert, a.modalInert, c.modalInert].map(Boolean), [false, false, false]);
});

test("an anchor's frame, in the root's coordinates, reported when it changes", () => {
  const { h, node, overlay, sent } = page();
  h.root.el.rect = { left: 0, top: 50, width: 400, height: 700 };
  const anchor = node(Kind.View, h.root.el);
  anchor.el.rect = { left: 10, top: 80, width: 100, height: 40 };
  const o = overlay();
  overlayKind.prop(h, o, KindProp.Overlay.Anchor, anchor.id);
  h.overlays.present(o);
  h.overlays.anchors();
  assert.deepEqual(sent.splice(0), [{ id: o.id, kind: Event.AnchorFrame, bytes: hex(encodeAnchorFrame(10, 30, 100, 40, true)) }]);
  h.overlays.anchors();
  assert.deepEqual(sent.splice(0), [], "unchanged: not again");
  // Scrolled out of the window.
  anchor.el.rect = { left: 10, top: -100, width: 100, height: 40 };
  h.overlays.anchors();
  assert.deepEqual(sent.splice(0), [{ id: o.id, kind: Event.AnchorFrame, bytes: hex(encodeAnchorFrame(10, -150, 100, 40, false)) }]);
  // A new anchor (even the same view): reported again.
  overlayKind.prop(h, o, KindProp.Overlay.Anchor, anchor.id);
  h.overlays.anchors();
  assert.equal(sent.splice(0).length, 1);
  // None: nothing.
  overlayKind.prop(h, o, KindProp.Overlay.Anchor, 0);
  h.overlays.anchors();
  assert.deepEqual(sent.splice(0), []);
});
