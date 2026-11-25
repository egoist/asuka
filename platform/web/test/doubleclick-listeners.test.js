// The page's listeners in src/doubleclick.js, on a fake window: the second
// press of a double-click on a sheet its first press opened is swallowed
// whole, before the page's own listeners; a click from the keyboard is its
// own; a key held through the press and a second finger don't let it out;
// a press long after starts afresh; and a window hidden at the first press
// wasn't open then.
import { test } from "node:test";
import assert from "node:assert/strict";

const listeners = [];
globalThis.addEventListener = (type, fn) => listeners.push({ type, fn });
globalThis.Element = class Element {
  constructor(layer) { this.layer = layer; }
  closest() { return this.layer; }
};
// The dialogs and windows in the page; querySelectorAll finds them as the
// selector says, leaving out a hidden one if it asks for that
let open = [];
globalThis.document = {
  querySelectorAll: (sel) => open.filter((l) => !(l.hidden && sel.includes(":not([hidden])"))),
};
await import("../src/doubleclick.js");

// What the page's own listeners see: those on the press's element, which
// any stop at the window keeps from it, and one on the window after
// doubleclick.js's (an outside press's, nav.js), which only an immediate
// stop does
const reached = [], onWindow = [];
for (const type of ["pointerdown", "pointerup", "mousedown", "mouseup", "click", "dblclick", "keydown"])
  globalThis.addEventListener(type, (e) => onWindow.push(e.type));

let now = 1000;
function fire(type, target, more = {}) {
  const e = { type, target, timeStamp: now, clientX: 100, clientY: 100, isPrimary: true, detail: 1, ...more,
    stopped: false, prevented: false };
  e.stopImmediatePropagation = () => { e.stopped = e.halted = true; };
  e.stopPropagation = () => { e.halted = true; };
  e.preventDefault = () => { e.prevented = true; };
  for (const l of listeners) {
    if (l.type !== type) continue;
    l.fn(e);
    if (e.stopped) break;
  }
  if (!e.halted) reached.push(type);
  return e;
}
// One press and its release; the click, then on a double-click's second
// press the dblclick
function press(target, { detail = 1, dbl = false, ...more } = {}) {
  for (const t of ["pointerdown", "mousedown", "pointerup", "mouseup"]) fire(t, target, more);
  fire("click", target, { ...more, detail });
  if (dbl) fire("dblclick", target, more);
}

const row = new Element(null);
const sheet = { name: "sheet" };
const onSheet = new Element(sheet);

test("a double-click whose first press opens a sheet: its second press, all of it, never reaches the page", () => {
  open = [];
  reached.length = 0;
  press(row);
  assert.deepEqual(reached, ["pointerdown", "mousedown", "pointerup", "mouseup", "click"]);
  open = [sheet]; // the first press opened it
  reached.length = 0;
  now += 120;
  onWindow.length = 0;
  press(onSheet, { detail: 2, dbl: true });
  assert.deepEqual(reached, []);
  assert.deepEqual(onWindow, []);
});

test("a press that ends with no dblclick leaves a click from the keyboard (Enter on a button) its own", () => {
  open = [];
  now += 1000;
  press(row);
  open = [sheet];
  now += 120;
  reached.length = 0;
  for (const t of ["pointerdown", "mousedown", "pointerup", "mouseup"]) fire(t, onSheet); // dragged off: no click, no dblclick
  assert.deepEqual(reached, []);
  fire("keydown", onSheet);
  fire("click", onSheet, { detail: 0 });
  assert.deepEqual(reached, ["keydown", "click"]);
});

test("a key pressed while the swallowed press is held lets none of that press through", () => {
  open = [];
  now += 1000;
  press(row);
  open = [sheet];
  now += 120;
  reached.length = 0;
  fire("pointerdown", onSheet);
  fire("mousedown", onSheet);
  fire("keydown", onSheet, { key: "Shift" });
  fire("pointerup", onSheet);
  fire("mouseup", onSheet);
  fire("click", onSheet, { detail: 2 });
  fire("dblclick", onSheet);
  assert.deepEqual(reached, ["keydown"]);
});

test("a second finger landing while the swallowed press is held is its own, and leaves that press swallowed", () => {
  open = [];
  now += 1000;
  press(row);
  open = [sheet];
  now += 120;
  reached.length = 0;
  fire("pointerdown", onSheet);
  fire("pointerdown", onSheet, { isPrimary: false, clientX: 300 });
  fire("pointerup", onSheet, { isPrimary: false, clientX: 300 });
  fire("pointerup", onSheet);
  fire("click", onSheet, { detail: 2 });
  assert.deepEqual(reached, ["pointerdown", "pointerup"]);
});

test("the next press after a swallowed one starts afresh", () => {
  now += 1000;
  reached.length = 0;
  press(onSheet); // a press of its own on the sheet, long after
  assert.deepEqual(reached, ["pointerdown", "mousedown", "pointerup", "mouseup", "click"]);
});

test("a window hidden at the first press isn't open then: the second press on it, shown by the first, goes nowhere", () => {
  const win = { name: "window", hidden: true };
  open = [win];
  now += 1000;
  press(row);
  win.hidden = false; // the first press showed it
  now += 120;
  reached.length = 0;
  press(new Element(win), { detail: 2, dbl: true });
  assert.deepEqual(reached, []);
});
