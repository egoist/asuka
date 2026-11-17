// The keyboard's focus on views (src/focus.js): FocusChange's values as
// the focus moves (2 from the keyboard, 1 from a press, 0 lost; a field's
// its own), Tab round a focus trap's stops, a character typed to keyText,
// NoFocusOnPress, Focusable's Tab stop, and a Focus that waits until its
// view can take it.
import { test } from "node:test";
import assert from "node:assert/strict";
import { Focus, cycle, typed, focusValue, waitBatches } from "../src/focus.js";
import { Kind, Event } from "../src/ops.js";

test("cycle goes round, both ways; from outside, the first or the last", () => {
  const l = ["a", "b", "c"];
  assert.equal(cycle(l, "a", false), "b");
  assert.equal(cycle(l, "c", false), "a");
  assert.equal(cycle(l, "a", true), "c");
  assert.equal(cycle(l, "x", false), "a");
  assert.equal(cycle(l, "x", true), "c");
  assert.equal(cycle([], "x", false), null);
});

test("typed: one character, no shortcut", () => {
  const ev = (key, m = {}) => ({ key, ctrlKey: false, metaKey: false, altKey: false, isComposing: false, keyCode: 0, ...m });
  assert.equal(typed(ev("b")), "b");
  assert.equal(typed(ev("B", { shiftKey: true })), "B");
  assert.equal(typed(ev(" ")), " ");
  assert.equal(typed(ev("é")), "é");
  assert.equal(typed(ev("😀")), "😀");
  assert.equal(typed(ev("Enter")), "");
  assert.equal(typed(ev("ArrowDown")), "");
  assert.equal(typed(ev("c", { metaKey: true })), "");
  assert.equal(typed(ev("c", { ctrlKey: true })), "");
  assert.equal(typed(ev("a", { isComposing: true })), "");
  assert.equal(focusValue(true), 2);
  assert.equal(focusValue(false), 1);
});

// --- A fake page ---

function element(parent, tag = "div") {
  const attrs = new Map();
  const classes = new Set();
  const el = {
    tagName: tag.toUpperCase(),
    nodeType: 1,
    parentElement: parent,
    isConnected: true,
    focusable: true,
    classes,
    classList: { toggle(c, on) { on ? classes.add(c) : classes.delete(c); }, contains: (c) => classes.has(c) },
    setAttribute(k, v) { attrs.set(k, String(v)); },
    getAttribute(k) { return attrs.has(k) ? attrs.get(k) : null; },
    hasAttribute(k) { return attrs.has(k); },
    removeAttribute(k) { attrs.delete(k); },
    matches: () => false,
    closest: () => null,
    contains(x) { for (; x; x = x.parentElement) if (x === el) return true; return false; },
    focus() { if (el.focusable && el.isConnected) globalThis.document.activeElement = el; },
    blur() { if (globalThis.document.activeElement === el) globalThis.document.activeElement = null; },
  };
  Object.defineProperty(el, "tabIndex", { get: () => (attrs.has("tabindex") ? +attrs.get("tabindex") : -1), set: (v) => attrs.set("tabindex", String(v)) });
  return el;
}

function page() {
  globalThis.document = { activeElement: null, hasFocus: () => true };
  const container = element(null);
  container.addEventListener = () => {};
  const sent = [];
  const later = [];
  const h = {
    container,
    nodes: new Map(),
    dispatch: (b) => sent.push(decode(b)),
    afterCore: (f) => later.push(f),
  };
  let next = 1;
  const node = (kind = Kind.View, parentEl = container) => {
    const n = { id: next++, kind, el: element(parentEl), listens: new Set(), listening(e) { return this.listens.has(e); } };
    n.el.asukaNode = n;
    h.nodes.set(n.id, n);
    return n;
  };
  const f = new Focus(h);
  h.focus = f;
  const run = () => { for (const g of later.splice(0)) g(); };
  return { h, f, node, sent, run, container };
}

function decode(b) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const e = { id: v.getUint32(0, true), kind: v.getUint16(4, true) };
  if (e.kind === Event.FocusChange) e.v = v.getUint32(6, true);
  if (e.kind === Event.KeyText) e.s = new TextDecoder().decode(b.subarray(10));
  return e;
}

const FC = (id, v) => ({ id, kind: Event.FocusChange, v });

test("FocusChange as the focus moves: from a press 1, from the keyboard 2, lost 0; a field's are its own", () => {
  const { f, node, sent } = page();
  const a = node(), b = node(), field = node(Kind.TextField);
  const inner = element(a.el); // a control in a's element: a's focus
  f.modality = "pointer";
  f.focusIn({ target: a.el });
  assert.deepEqual(sent.splice(0), [FC(a.id, 1)]);
  // Within the same view: nothing.
  f.focusOut({ target: a.el, relatedTarget: inner });
  f.focusIn({ target: inner });
  assert.deepEqual(sent.splice(0), []);
  // To b, from the keyboard.
  f.modality = "key";
  f.focusOut({ target: inner, relatedTarget: b.el });
  f.focusIn({ target: b.el });
  assert.deepEqual(sent.splice(0), [FC(a.id, 0), FC(b.id, 2)]);
  // A press with no :focus-visible: 1; with it (CSS's heuristics): 2.
  f.modality = "";
  f.focusIn({ target: a.el });
  assert.deepEqual(sent.splice(0), [FC(b.id, 0), FC(a.id, 1)]);
  b.el.matches = (s) => s === ":focus-visible";
  f.focusIn({ target: b.el });
  assert.deepEqual(sent.splice(0), [FC(a.id, 0), FC(b.id, 2)]);
  // To a field: b loses it; the field says nothing (its Focus and Blur).
  f.focusOut({ target: b.el, relatedTarget: field.el });
  f.focusIn({ target: field.el });
  assert.deepEqual(sent.splice(0), [FC(b.id, 0)]);
  // Back to a view, then to nothing.
  f.focusIn({ target: a.el });
  f.focusOut({ target: a.el, relatedTarget: null });
  assert.deepEqual(sent.splice(0), [FC(a.id, 1), FC(a.id, 0)]);
  // The window going to the background: kept, unreported.
  f.focusIn({ target: a.el });
  sent.splice(0);
  globalThis.document.hasFocus = () => false;
  globalThis.document.activeElement = a.el;
  f.focusOut({ target: a.el, relatedTarget: null });
  f.focusIn({ target: a.el });
  assert.deepEqual(sent.splice(0), []);
  globalThis.document.hasFocus = () => true;
  // A view going: its focus goes with it, unreported.
  a.dying = true;
  f.forget(a);
  f.focusIn({ target: b.el });
  assert.deepEqual(sent.splice(0), [FC(b.id, 2)]);
  // The host's own moves (an overlay brought to the top): unreported.
  f.quiet++;
  f.focusOut({ target: b.el, relatedTarget: null });
  f.quiet--;
  assert.deepEqual(sent.splice(0), []);
});

test("Focusable: a Tab stop, unless off and not pressable; NoFocusRing; NoFocusOnPress", () => {
  const { f, node } = page();
  const a = node();
  f.setFocusable(a, true);
  assert.equal(a.el.getAttribute("tabindex"), "0");
  f.setFocusable(a, false);
  assert.equal(a.el.getAttribute("tabindex"), null);
  // A pressable view stays one.
  a.listens.add(Event.Press);
  a.el.setAttribute("tabindex", "0");
  f.setFocusable(a, false);
  assert.equal(a.el.getAttribute("tabindex"), "0");
  f.setNoRing(a, true);
  assert.ok(a.el.classes.has("asuka-no-ring"));
  f.setNoRing(a, false);
  assert.ok(!a.el.classes.has("asuka-no-ring"));

  // A mouse's press on it doesn't move the focus; on a control in it, it does.
  const b = node();
  b.noFocusOnPress = true;
  b.el.closest = () => b.el;
  const control = element(b.el, "button");
  control.closest = () => control;
  let prevented = 0;
  f.mouseDown({ button: 0, target: b.el, preventDefault: () => prevented++ });
  assert.equal(prevented, 1);
  f.mouseDown({ button: 0, target: control, preventDefault: () => prevented++ });
  assert.equal(prevented, 1);
  b.noFocusOnPress = false;
  f.mouseDown({ button: 0, target: b.el, preventDefault: () => prevented++ });
  assert.equal(prevented, 1);
});

test("Tab goes round a focus trap's stops; outside one, the browser's", () => {
  const { f, node } = page();
  const trap = node();
  trap.trap = true;
  const one = node(Kind.View, trap.el), two = node(Kind.View, trap.el), outside = node();
  for (const n of [one, two]) { n.el.setAttribute("tabindex", "0"); n.el.getClientRects = () => [{}]; }
  trap.el.querySelectorAll = () => [one.el, two.el];
  trap.el.matches = () => false;
  for (const n of [one, two]) n.el.matches = (s) => s.includes("[tabindex]");
  const key = (shiftKey) => {
    const e = { key: "Tab", shiftKey, ctrlKey: false, metaKey: false, altKey: false, defaultPrevented: false, prevented: 0 };
    e.preventDefault = () => { e.prevented++; e.defaultPrevented = true; };
    f.keyDown(e);
    return e;
  };
  one.el.focus();
  let e = key(false);
  assert.equal(e.prevented, 1);
  assert.equal(document.activeElement, two.el);
  key(false);
  assert.equal(document.activeElement, one.el);
  key(true);
  assert.equal(document.activeElement, two.el);
  // Outside it: the browser moves it.
  outside.el.focus();
  e = key(false);
  assert.equal(e.prevented, 0);
  assert.equal(document.activeElement, outside.el);
});

test("a character typed goes to the focused view's keyText, unless taken or in a field", () => {
  const { f, node, sent } = page();
  const list = node();
  list.listens.add(Event.KeyText);
  const ev = (key, target, m = {}) => ({ key, target, ctrlKey: false, metaKey: false, altKey: false, isComposing: false, keyCode: 0, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }, ...m });
  f.keyDown(ev("b", list.el));
  assert.deepEqual(sent.splice(0), [{ id: list.id, kind: Event.KeyText, s: "b" }]);
  // Space: typed, and not the page's scroll.
  const sp = ev(" ", list.el);
  f.keyDown(sp);
  assert.deepEqual(sent.splice(0), [{ id: list.id, kind: Event.KeyText, s: " " }]);
  assert.ok(sp.defaultPrevented);
  // Taken (a view's keys, a press): not.
  f.keyDown(ev("c", list.el, { defaultPrevented: true }));
  // A shortcut, a named key: not.
  f.keyDown(ev("c", list.el, { metaKey: true }));
  f.keyDown(ev("ArrowDown", list.el));
  // In a field: the field's.
  const input = element(list.el, "input");
  input.closest = () => input;
  f.keyDown(ev("d", input));
  // Not listening: not.
  const other = node();
  f.keyDown(ev("e", other.el));
  assert.deepEqual(sent.splice(0), []);
});

test("a Focus on a view that can't take it yet waits for a batch after which it can", () => {
  const { f, node, run } = page();
  const a = node();
  a.el.focusable = false; // in an overlay hidden until its anchor is known
  f.focus(a, a.el);
  assert.equal(document.activeElement, null);
  f.batched();
  run();
  assert.equal(document.activeElement, null, "still hidden");
  a.el.focusable = true;
  f.batched();
  run();
  assert.equal(document.activeElement, a.el);
  assert.equal(f.pending, null);
  // A Blur, or the view going, drops it.
  const b = node();
  b.el.focusable = false;
  f.focus(b, b.el);
  f.blur(b, b.el);
  b.el.focusable = true;
  f.batched();
  run();
  assert.equal(document.activeElement, a.el);
  f.focus(b, b.el); // focusable now: at once
  assert.equal(document.activeElement, b.el);
  // Not past a few batches, nor the focus going elsewhere.
  const c = node();
  c.el.focusable = false;
  f.focus(c, c.el);
  for (let i = 0; i < waitBatches; i++) { f.batched(); run(); }
  assert.ok(f.pending);
  f.batched();
  run();
  assert.equal(f.pending, null);
  f.focus(c, c.el);
  f.focusIn({ target: a.el });
  assert.equal(f.pending, null);
});
