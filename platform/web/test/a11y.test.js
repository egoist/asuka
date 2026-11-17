// Screen readers' roles and states (src/a11y.js): ARIA's role for each
// ops.Role, the states RFC 0010's props give a view, A11yRange as
// ops.AppendRange writes it, and the views a view names by their
// elements' IDs.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as a11y from "../src/a11y.js";
import { Role, A11yChecked, A11yExpanded } from "../src/ops.js";

const fromHex = (s) => new Uint8Array(s.match(/../g).map((x) => parseInt(x, 16)));

/** element is what a11y.js reads and writes of one: its attributes. */
function element() {
  const attrs = new Map();
  return {
    attrs,
    id: "",
    dataset: {},
    tabIndex: -1,
    setAttribute(k, v) { attrs.set(k, String(v)); if (k === "tabindex") this.tabIndex = +v; },
    getAttribute(k) { return attrs.has(k) ? attrs.get(k) : null; },
    removeAttribute(k) { attrs.delete(k); },
    hasAttribute(k) { return attrs.has(k); },
  };
}

test("every ops.Role has ARIA's role, in the enum's order", () => {
  const want = {
    Button: "button", Header: "heading", Image: "img", Link: "link", Search: "searchbox",
    Checkbox: "checkbox", Radio: "radio", RadioGroup: "radiogroup", Switch: "switch", Tab: "tab", TabList: "tablist",
    TabPanel: "tabpanel", Slider: "slider", SpinButton: "spinbutton", ProgressBar: "progressbar", Meter: "meter",
    Menu: "menu", MenuBar: "menubar", MenuItem: "menuitem", MenuItemCheckbox: "menuitemcheckbox", MenuItemRadio: "menuitemradio",
    Listbox: "listbox", Option: "option", ComboBox: "combobox", Dialog: "dialog", AlertDialog: "alertdialog",
    Tooltip: "tooltip", Group: "group", Separator: "separator", Toolbar: "toolbar", Status: "status",
  };
  assert.equal(a11y.roles.length, Object.keys(Role).length);
  for (const [name, v] of Object.entries(Role)) {
    if (name === "None" || name === "Text") assert.equal(a11y.roles[v], null, name);
    else assert.equal(a11y.roles[v], want[name], name);
  }
});

test("a role, then its states: a checkable one unset reads unchecked; a tab or an option not selected says so", () => {
  const n = {}, el = element();
  a11y.setRole(n, el, Role.Checkbox);
  assert.equal(el.getAttribute("role"), "checkbox");
  assert.equal(el.getAttribute("aria-checked"), "false");
  a11y.setChecked(n, el, A11yChecked.Checked);
  assert.equal(el.getAttribute("aria-checked"), "true");
  a11y.setChecked(n, el, A11yChecked.Mixed);
  assert.equal(el.getAttribute("aria-checked"), "mixed");
  // Not checkable: None, none.
  a11y.setChecked(n, el, A11yChecked.None);
  a11y.setRole(n, el, Role.Group);
  assert.equal(el.getAttribute("aria-checked"), null);
  // A state set before its role: the role brings it.
  const o = {}, oel = element();
  a11y.setSelected(o, oel, false);
  assert.equal(oel.getAttribute("aria-selected"), null);
  a11y.setRole(o, oel, Role.Option);
  assert.equal(oel.getAttribute("aria-selected"), "false");
  a11y.setSelected(o, oel, true);
  assert.equal(oel.getAttribute("aria-selected"), "true");
  // A heading's level goes with its role; a button is a Tab stop.
  const hd = {}, hel = element();
  a11y.setRole(hd, hel, Role.Header);
  assert.equal(hel.getAttribute("aria-level"), "2");
  a11y.setRole(hd, hel, Role.Button);
  assert.equal(hel.getAttribute("aria-level"), null);
  assert.equal(hel.tabIndex, 0);
  // None: no role (but the host's own, a control's).
  a11y.setRole(hd, hel, Role.None);
  assert.equal(hel.getAttribute("role"), null);
  hel.setAttribute("role", "switch");
  hel.dataset.ownRole = "";
  a11y.setRole(hd, hel, Role.None);
  assert.equal(hel.getAttribute("role"), "switch");
});

test("expanded, a value, modal", () => {
  const el = element();
  assert.equal(a11y.ariaExpanded(A11yExpanded.None), null);
  a11y.setExpanded(el, A11yExpanded.Collapsed);
  assert.equal(el.getAttribute("aria-expanded"), "false");
  a11y.setExpanded(el, A11yExpanded.Expanded);
  assert.equal(el.getAttribute("aria-expanded"), "true");
  a11y.setExpanded(el, A11yExpanded.None);
  assert.equal(el.getAttribute("aria-expanded"), null);
  a11y.setValue(el, "40%");
  assert.equal(el.getAttribute("aria-valuetext"), "40%");
  a11y.setValue(el, "");
  assert.equal(el.getAttribute("aria-valuetext"), null);
  a11y.setModal(el, true);
  assert.equal(el.getAttribute("aria-modal"), "true");
  a11y.setModal(el, false);
  assert.equal(el.getAttribute("aria-modal"), null);
});

test("A11yRange reads ops.AppendRange; empty or short, none", () => {
  // ops.AppendRange(nil, ops.Range{Min: 0, Max: 100, Now: 40}) and {-1.5, 2.25, 0.125}.
  assert.deepEqual(a11y.decodeRange(fromHex("000000000000000000000000000059400000000000004440")), { min: 0, max: 100, now: 40 });
  const big = new Uint8Array(30);
  big.set(fromHex("000000000000f8bf0000000000000240000000000000c03f"), 6);
  assert.deepEqual(a11y.decodeRange(big.subarray(6)), { min: -1.5, max: 2.25, now: 0.125 });
  assert.equal(a11y.decodeRange(new Uint8Array(0)), null);
  assert.equal(a11y.decodeRange(new Uint8Array(23)), null);
  const el = element();
  a11y.setRange(el, fromHex("000000000000000000000000000059400000000000004440"));
  assert.deepEqual(["aria-valuemin", "aria-valuemax", "aria-valuenow"].map((k) => el.getAttribute(k)), ["0", "100", "40"]);
  a11y.setRange(el, new Uint8Array(0));
  assert.deepEqual(["aria-valuemin", "aria-valuemax", "aria-valuenow"].map((k) => el.getAttribute(k)), [null, null, null]);
});

test("controls, labelled and described by: the named view's element's ID, given once", () => {
  const label = { el: element() }, list = { el: element() }, own = { el: element() };
  own.el.id = "mine";
  const nodes = new Map([[7, label], [8, list], [9, own]]);
  const el = element();
  a11y.setRelation(nodes, el, a11y.relations.labelledBy, 7);
  assert.equal(el.getAttribute("aria-labelledby"), "asuka-v7");
  assert.equal(label.el.id, "asuka-v7");
  a11y.setRelation(nodes, el, a11y.relations.controls, 8);
  assert.equal(el.getAttribute("aria-controls"), "asuka-v8");
  // An element with an ID keeps it.
  a11y.setRelation(nodes, el, a11y.relations.describedBy, 9);
  assert.equal(el.getAttribute("aria-describedby"), "mine");
  // 0, or a view not made yet: none (the core sends it again once it's made).
  a11y.setRelation(nodes, el, a11y.relations.controls, 0);
  assert.equal(el.getAttribute("aria-controls"), null);
  a11y.setRelation(nodes, el, a11y.relations.labelledBy, 99);
  assert.equal(el.getAttribute("aria-labelledby"), null);
});
