// A TextField's suggestions (src/combobox.js): when the list shows (after
// typing, not on focus alone; Dropdown's button whether or not), the
// keyboard's model (Down, Up, Enter, Escape, Tab, Alt+Down), and where
// the list goes (under the field, over it near the keyboard).
import { test } from "node:test";
import assert from "node:assert/strict";
import "../src/host.js"; // as on a page: the host's modules loaded in their order
import { ComboModel, placeList, maxListHeight } from "../src/combobox.js";

const cities = ["Paris", "Prague", "Parma"];

test("the list shows after typing, not on focus alone, and closes on a choice until the next", () => {
  const m = new ComboModel();
  m.setItems(cities);
  assert.equal(m.open, false); // not focused
  m.focus();
  assert.equal(m.open, false); // focus alone
  m.input();
  assert.equal(m.open, true);
  m.setItems([]);
  assert.equal(m.open, false); // empty: hidden
  m.setItems(cities);
  assert.equal(m.open, true); // back, in place
  assert.deepEqual(m.key("ArrowDown"), { handled: true });
  assert.deepEqual(m.key("Enter"), { handled: true, choose: 0 });
  assert.equal(m.open, false);
  m.setItems(["Paris"]); // the core's, after the choice
  assert.equal(m.open, false);
  m.input();
  assert.equal(m.open, true);
  m.blur();
  assert.equal(m.open, false);
  m.focus();
  assert.equal(m.open, false);
});

test("Down and Up move round the options; from the field, closed, they open it at an end", () => {
  const m = new ComboModel();
  m.setItems(cities);
  m.focus();
  assert.deepEqual(m.key("ArrowDown"), { handled: true });
  assert.equal(m.open, true);
  assert.equal(m.active, 0);
  m.key("ArrowDown");
  m.key("ArrowDown");
  assert.equal(m.active, 2);
  m.key("ArrowDown");
  assert.equal(m.active, 0); // round
  m.key("ArrowUp");
  assert.equal(m.active, 2);
  assert.deepEqual(m.key("Escape"), { handled: true });
  assert.equal(m.open, false);
  assert.equal(m.active, -1);
  // Escape again is the field's (the page's back, a blur).
  assert.deepEqual(m.key("Escape"), { handled: false });
  m.key("ArrowUp");
  assert.equal(m.open, true);
  assert.equal(m.active, 2);
  // Tab closes it and moves on.
  assert.deepEqual(m.key("Tab"), { handled: false });
  assert.equal(m.open, false);
  // Alt+Down opens it without an option; Alt+Up closes it.
  m.key("ArrowDown", true);
  assert.equal(m.open, true);
  assert.equal(m.active, -1);
  m.key("ArrowDown", true);
  assert.equal(m.active, -1);
  assert.deepEqual(m.key("ArrowUp", true), { handled: true });
  assert.equal(m.open, false);
  // No suggestions: the arrows are the field's.
  m.setItems([]);
  assert.deepEqual(m.key("ArrowDown"), { handled: false });
});

test("Enter without an option is the field's return; typing takes the arrows off the list", () => {
  const m = new ComboModel();
  m.setItems(cities);
  m.focus();
  m.input();
  assert.deepEqual(m.key("Enter"), { handled: false });
  assert.equal(m.open, false);
  m.input();
  m.key("ArrowDown");
  m.key("ArrowDown");
  assert.equal(m.active, 1);
  m.input();
  assert.equal(m.active, -1);
  // Fewer suggestions: an option past them is let go.
  m.key("ArrowUp");
  assert.equal(m.active, 2);
  m.setItems(["Paris"]);
  assert.equal(m.active, -1);
});

test("Dropdown's button opens all of them, typed or not, and closes them", () => {
  const m = new ComboModel();
  m.setItems(cities);
  m.focus();
  m.toggle();
  assert.equal(m.open, true);
  assert.equal(m.active, -1);
  m.toggle();
  assert.equal(m.open, false);
  // Unfocused (a click on the button focuses the field first).
  m.blur();
  m.toggle();
  assert.equal(m.open, true);
  // None to show: it stays closed.
  m.close();
  m.setItems([]);
  m.toggle();
  assert.equal(m.open, false);
});

test("the list goes under the field, over it when the room's above (a phone's keyboard)", () => {
  const view = { top: 0, height: 800 };
  const under = placeList({ top: 100, bottom: 136 }, view, 120);
  assert.deepEqual(under, { top: 140, maxHeight: 120, up: false });
  // Taller than the most: it scrolls.
  assert.equal(placeList({ top: 100, bottom: 136 }, view, 1000).maxHeight, maxListHeight);
  // Near the bottom (the keyboard up: the visual viewport's short).
  const over = placeList({ top: 300, bottom: 336 }, { top: 0, height: 400 }, 200);
  assert.equal(over.up, true);
  assert.equal(over.maxHeight, 200);
  assert.equal(over.top, 300 - 4 - 200);
  // No more room above than under: under, as tall as there's room for.
  const low = placeList({ top: 40, bottom: 76 }, { top: 0, height: 150 }, 200);
  assert.equal(low.up, false);
  assert.equal(low.maxHeight, 150 - 76 - 4 - 8);
});
