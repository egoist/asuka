// What screen readers read of a view: its role (ARIA's), and the states
// and relations RFC 0010's props give it: checked, expanded, selected, a
// value and its range, modal, and the views it controls, is labelled or
// described by (their elements' IDs: each view's element gets one when
// another names it).

import { Role, A11yChecked, A11yExpanded } from "./ops.js";

/** roles are ARIA's, by ops.Role (null: none of its own). */
export const roles = [
  null, "button", "heading", "img", "link", null, "searchbox",
  "checkbox", "radio", "radiogroup", "switch", "tab", "tablist", "tabpanel", "slider", "spinbutton",
  "progressbar", "meter", "menu", "menubar", "menuitem", "menuitemcheckbox", "menuitemradio",
  "listbox", "option", "combobox", "dialog", "alertdialog", "tooltip", "group", "separator", "toolbar", "status",
];

const checkable = new Set([Role.Checkbox, Role.Radio, Role.Switch, Role.MenuItemCheckbox, Role.MenuItemRadio]);
const selectable = new Set([Role.Tab, Role.Option]);

/** ariaChecked is aria-checked for a view's A11yChecked with its role: unset reads as unchecked on a checkable role (null: none). */
export function ariaChecked(role, checked) {
  switch (checked) {
    case A11yChecked.Unchecked: return "false";
    case A11yChecked.Checked: return "true";
    case A11yChecked.Mixed: return "mixed";
  }
  return checkable.has(role) ? "false" : null;
}

/** ariaExpanded is aria-expanded for A11yExpanded (null: not expandable). */
export function ariaExpanded(v) {
  return v === A11yExpanded.Collapsed ? "false" : v === A11yExpanded.Expanded ? "true" : null;
}

/** ariaSelected is aria-selected: a tab or an option not selected says so; other roles only when selected. */
export function ariaSelected(role, selected) {
  if (selected) return "true";
  return selectable.has(role) ? "false" : null;
}

/** decodeRange reads A11yRange (ops.AppendRange): f64 min, max, now, little endian; null when empty or short. */
export function decodeRange(b) {
  if (!b || b.length < 24) return null;
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  return { min: v.getFloat64(0, true), max: v.getFloat64(8, true), now: v.getFloat64(16, true) };
}

function attr(el, name, v) {
  if (v === null || v === undefined || v === "") el.removeAttribute(name);
  else el.setAttribute(name, v);
}

/** a11yState returns n's state, made on first use. */
function state(n) {
  return n.a11y ??= { role: 0, checked: 0, selected: false };
}

/** setRole gives el (n's) its role, and what follows the role: a heading's level, a button's tab stop, the states a role reads. */
export function setRole(n, el, r) {
  const role = roles[r] || null;
  if (role) el.setAttribute("role", role);
  else if (el.dataset?.ownRole === undefined) el.removeAttribute("role");
  if (r === Role.Header) el.setAttribute("aria-level", "2");
  else if (state(n).role === Role.Header) el.removeAttribute("aria-level");
  if (r === Role.Button && !el.hasAttribute("tabindex")) el.tabIndex = 0;
  state(n).role = r;
  applyChecked(n, el);
}

function applyChecked(n, el) {
  const s = state(n);
  attr(el, "aria-checked", ariaChecked(s.role, s.checked));
  attr(el, "aria-selected", ariaSelected(s.role, s.selected));
}

/** setChecked takes A11yChecked. */
export function setChecked(n, el, v) {
  state(n).checked = v;
  applyChecked(n, el);
}

/** setSelected takes A11ySelected. */
export function setSelected(n, el, v) {
  state(n).selected = !!v;
  applyChecked(n, el);
}

export function setExpanded(el, v) { attr(el, "aria-expanded", ariaExpanded(v)); }

export function setValue(el, v) { attr(el, "aria-valuetext", v); }

export function setRange(el, b) {
  const r = decodeRange(b);
  attr(el, "aria-valuemin", r ? String(r.min) : null);
  attr(el, "aria-valuemax", r ? String(r.max) : null);
  attr(el, "aria-valuenow", r ? String(r.now) : null);
}

export function setModal(el, v) { attr(el, "aria-modal", v ? "true" : null); }

/** elementID is the ID of the element of the view whose node ID is id, given it if it has none (null: no such view yet). */
export function elementID(nodes, id) {
  const t = id ? nodes.get(id) : null;
  const el = t?.el;
  if (!el) return null;
  if (!el.id) el.id = "asuka-v" + id;
  return el.id;
}

/** relation names, by the props that set them. */
export const relations = { controls: "aria-controls", labelledBy: "aria-labelledby", describedBy: "aria-describedby" };

/** setRelation points el's aria-controls, -labelledby or -describedby at the view id (0: none). */
export function setRelation(nodes, el, name, id) {
  attr(el, name, elementID(nodes, id));
}
