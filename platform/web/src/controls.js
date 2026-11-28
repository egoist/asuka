// Chips, date ranges and disclosures, as the page's own elements:
//
//   - Chip: a pill of the host's look, its <button> the chip (aria-pressed
//     for a filter chip, which shows a checkmark while selected), with a
//     leading symbol (Icon); a removable (input) chip has a remove button
//     after the title, named "Remove" and the title, out of the Tab order
//     (Delete or Backspace on the chip removes it too). The two buttons
//     are siblings in the chip's box, not one in the other: a button
//     inside a button is neither valid HTML nor read as two.
//   - DateRangePicker: two <input type=date>, an en dash between, each
//     named; the end's min is the start, and a start moved past the end
//     takes the end with it (an end before the start, the start).
//   - Disclosure: a <details> of only its <summary>: the browser's
//     keyboard (Space, Enter) and its expanded state for screen readers;
//     its chevron at the end turns as it opens. What it discloses is the
//     core's, laid out under it.

import { Kind, KindProp as P, Event, Events } from "./ops.js";
import { own } from "./views.js";
import { symbolSVG } from "./media.js";
import * as text from "./text.js";

const finite = (f) => f === f && f !== Infinity && f !== -Infinity;

/** unconstrained is a measure's constraint of a control as wide as its content. */
const unconstrained = { width: NaN, height: NaN, availW: Infinity, availH: Infinity };

/** fitWidth is a control's width: its own, no wider than the space it has (as views.js's). */
function fitWidth(c, natural) {
  if (c.width === c.width) return c.width;
  if (c.availW === -Infinity) return natural;
  if (finite(c.availW)) return Math.min(natural, Math.max(c.availW, 0));
  return natural;
}

// --- Chip ---

/** chipRemoves says a key pressed on a removable chip removes it: Delete or Backspace, no modifier. */
export function chipRemoves(e, removable) {
  if (!removable || e.ctrlKey || e.metaKey || e.altKey || e.isComposing) return false;
  return e.key === "Delete" || e.key === "Backspace";
}

/** chipLead is the symbol before a chip's title: a selected filter chip's checkmark, else its icon. */
export function chipLead(n) { return n.filter && n.selected ? "checkmark" : n.icon; }

const chip = {
  create(h, n) {
    const el = own(n, document.createElement("div"));
    el.className = "asuka-chip";
    const b = document.createElement("button");
    b.type = "button";
    b.className = "asuka-chip-main asuka-control";
    n.lead = document.createElement("span");
    n.lead.className = "asuka-chip-lead";
    n.lead.hidden = true;
    n.label = document.createElement("span");
    n.label.className = "asuka-chip-title";
    b.append(n.lead, n.label);
    el.appendChild(b);
    n.button = n.focusEl = b;
    n.title = "";
    n.icon = "";
    n.leadName = "";
    n.filter = false;
    n.selected = false;
    n.removable = false;
    b.addEventListener("click", (e) => {
      e.stopPropagation();
      if (n.filter) {
        // Shown at once; the core sends Selected back if the app keeps it.
        n.selected = !n.selected;
        chipState(h, n);
        h.emit(n, Event.Toggled, (id) => Events.toggled(id, n.selected));
        return;
      }
      h.emit(n, Event.Press, Events.press);
    });
    b.addEventListener("keydown", (e) => {
      if (!chipRemoves(e, n.removable) || n.disabled) return;
      e.preventDefault();
      e.stopPropagation();
      h.emit(n, Event.Remove, Events.remove);
    });
    text.applyFont(h, n, el);
  },
  prop(h, n, p, v) {
    switch (p) {
      case P.Chip.Title:
        n.title = v;
        n.label.textContent = v;
        n.label.hidden = !v;
        removeLabel(h, n);
        break;
      case P.Chip.Icon:
        n.icon = v;
        break;
      case P.Chip.Filter:
        n.filter = !!v;
        break;
      case P.Chip.Selected:
        n.selected = !!v;
        break;
      case P.Chip.Removable:
        n.removable = !!v;
        removeButton(h, n);
        break;
    }
    chipState(h, n);
  },
  // Its Press, Toggled and Remove are its buttons': the box isn't one.
  listen(h, n, e) { return e === Event.Press || e === Event.Toggled || e === Event.Remove; },
  color(h, n) { n.el.style.setProperty("--tint", h.css(n.color) || "var(--asuka-accent)"); },
  disabled(h, n) {
    n.button.disabled = n.disabled;
    if (n.remove) n.remove.disabled = n.disabled;
    n.el.classList.toggle("asuka-chip-disabled", n.disabled);
  },
  label(h, n, v) {
    // The chip's name is its button's.
    n.el.removeAttribute("aria-label");
    if (v) n.button.setAttribute("aria-label", v); else n.button.removeAttribute("aria-label");
    removeLabel(h, n);
  },
  direction(h, n) { n.leadName = ""; chipState(h, n); },
  measure(h, n, c) {
    const [w, hh] = h.measureElement(n, n.el, unconstrained);
    return [fitWidth(c, w), hh];
  },
};

/** chipState shows a chip's state: pressed or not, its leading symbol. */
function chipState(h, n) {
  const b = n.button;
  if (n.filter) b.setAttribute("aria-pressed", String(n.selected));
  else b.removeAttribute("aria-pressed");
  n.el.classList.toggle("asuka-chip-selected", n.filter && n.selected);
  const lead = chipLead(n);
  if (lead !== n.leadName) {
    n.leadName = lead;
    n.lead.textContent = "";
    const s = lead ? symbolSVG(lead, h.rtl) : null;
    if (s) n.lead.appendChild(s);
    n.lead.hidden = !s;
  }
}

/** removeButton gives a removable chip its remove button at its end, or takes it away. */
function removeButton(h, n) {
  if (!n.removable) {
    n.remove?.remove();
    n.remove = null;
    n.el.classList.remove("asuka-chip-removable");
    return;
  }
  if (n.remove) return;
  const r = document.createElement("button");
  r.type = "button";
  r.className = "asuka-chip-remove";
  r.tabIndex = -1; // the chip is the Tab stop; Delete removes it from there
  r.disabled = n.disabled;
  const s = symbolSVG("xmark.circle.fill", false);
  if (s) r.appendChild(s);
  r.addEventListener("click", (e) => {
    e.stopPropagation();
    h.emit(n, Event.Remove, Events.remove);
  });
  n.remove = r;
  n.el.appendChild(r);
  n.el.classList.add("asuka-chip-removable");
  removeLabel(h, n);
}

/** removeLabel names the remove button: the word and what it removes (the chip's name). */
function removeLabel(h, n) {
  if (!n.remove) return;
  const what = n.button.getAttribute("aria-label") || n.title;
  const word = h.nav?.word ? h.nav.word("remove") : "Remove";
  n.remove.setAttribute("aria-label", what ? `${word} ${what}` : word);
}

// --- DateRangePicker ---

const pad = (x, k = 2) => String(x).padStart(k, "0");

/** dayString is the day an instant is in, as a date input has it (YYYY-MM-DD), in the device's time zone; "" for 0. */
export function dayString(ms) {
  if (!ms) return "";
  const d = new Date(ms);
  return `${pad(d.getFullYear(), 4)}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

/** dayStart is the local midnight a date input's day (YYYY-MM-DD) starts at, in Unix milliseconds (null: none). */
export function dayStart(s) {
  const m = /^(\d{4,})-(\d{2})-(\d{2})$/.exec(s || "");
  if (!m) return null;
  const d = new Date(0);
  d.setFullYear(+m[1], +m[2] - 1, +m[3]);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

/**
 * settled says a date input's day is one the user has finished choosing:
 * none, or a day within min and max (date input days). A date input says
 * it changed at each digit of a year typed (0002, 0020, 0202, 2026, and
 * on to six digits): a year of other than four digits is one being typed.
 */
export function settled(v, min = "", max = "") {
  if (!v) return true;
  if (!/^[1-9]\d{3}-\d{2}-\d{2}$/.test(v)) return false;
  return (!min || v >= min) && (!max || v <= max);
}

/** clampDay is a day left unsettled as the user leaves its input: into min and max, or none. */
export function clampDay(v, min = "", max = "") {
  const d = dayStart(v);
  if (d == null) return "";
  if (min && d < dayStart(min)) return min;
  if (max && d > dayStart(max)) return max;
  return settled(v) ? v : "";
}

/**
 * rangeChange is what a change to one of a range's inputs makes of it:
 * the days both show ({ start, end }, as date inputs have them), and
 * report, the range to report ([first, last] local midnights) once both
 * ends are chosen. which is the input the user changed ("start" or
 * "end"): a start past the end takes the end with it; an end before the
 * start, the start. While either's being typed (not settled within min
 * and max), nothing moves and nothing's reported: pending.
 */
export function rangeChange(start, end, which, min = "", max = "") {
  if (!settled(start, min, max) || !settled(end, min, max)) return { start, end, report: null, pending: true };
  const s = dayStart(start), e = dayStart(end);
  if (s != null && e != null && s > e) {
    if (which === "start") end = start;
    else start = end;
  }
  const a = dayStart(start), b = dayStart(end);
  return { start, end, report: a != null && b != null ? [a, b] : null };
}

/** rangeBounds are a range's inputs' min and max: the picker's bounds, the end's no earlier than the start. */
export function rangeBounds(start, minMs, maxMs) {
  const min = dayString(minMs), max = dayString(maxMs);
  const endMin = start && (!min || start > min) ? start : min;
  return { startMin: min, startMax: max, endMin, endMax: max };
}

const dateRange = {
  create(h, n) {
    const el = own(n, document.createElement("div"));
    el.className = "asuka-date-range";
    el.setAttribute("role", "group");
    const mk = (word) => {
      const i = document.createElement("input");
      i.type = "date";
      i.className = "asuka-date asuka-control";
      i.setAttribute("aria-label", h.nav?.word ? h.nav.word(word) : word);
      text.applyFont(h, n, i);
      return i;
    };
    n.start = mk("startDate");
    n.end = mk("endDate");
    const dash = document.createElement("span");
    dash.className = "asuka-date-dash";
    dash.setAttribute("aria-hidden", "true");
    dash.textContent = "–";
    el.append(n.start, dash, n.end);
    n.focusEl = n.start;
    n.startMs = n.endMs = n.minMs = n.maxMs = 0;
    const changed = (which) => {
      const b = rangeBounds("", n.minMs, n.maxMs);
      const r = rangeChange(n.start.value, n.end.value, which, b.startMin, b.startMax);
      if (r.pending) return;
      // Only the other input's moved: setting one's value as it's typed in starts its typing over.
      if (n.start.value !== r.start) n.start.value = r.start;
      if (n.end.value !== r.end) n.end.value = r.end;
      rangeLimits(n);
      if (!r.report) return;
      [n.startMs, n.endMs] = r.report;
      h.emit(n, Event.RangeChanged, (id) => Events.rangeChanged(id, r.report[0], r.report[1]));
    };
    // A day left half typed, or out of bounds, is put in them as the user leaves it.
    const left = (i, which) => {
      const b = rangeBounds("", n.minMs, n.maxMs);
      if (settled(i.value, b.startMin, b.startMax)) return;
      i.value = clampDay(i.value, b.startMin, b.startMax);
      changed(which);
    };
    n.start.addEventListener("change", () => changed("start"));
    n.end.addEventListener("change", () => changed("end"));
    n.start.addEventListener("blur", () => left(n.start, "start"));
    n.end.addEventListener("blur", () => left(n.end, "end"));
  },
  prop(h, n, p, v) {
    switch (p) {
      case P.DateRangePicker.Start:
        n.startMs = v;
        if (n.start.value !== dayString(v)) n.start.value = dayString(v);
        break;
      case P.DateRangePicker.End:
        n.endMs = v;
        if (n.end.value !== dayString(v)) n.end.value = dayString(v);
        break;
      case P.DateRangePicker.Min: n.minMs = v; break;
      case P.DateRangePicker.Max: n.maxMs = v; break;
      case P.DateRangePicker.Inline: break; // the web's date inputs open their own calendar
    }
    rangeLimits(n);
  },
  font(h, n) { text.applyFont(h, n, n.start); text.applyFont(h, n, n.end); },
  disabled(h, n) { n.start.disabled = n.end.disabled = n.disabled; },
  measure(h, n, c) {
    const [w, hh] = h.measureElement(n, n.el, unconstrained);
    return [fitWidth(c, w), hh];
  },
};

function rangeLimits(n) {
  const b = rangeBounds(n.start.value, n.minMs, n.maxMs);
  // Only what changed: an input's attribute set as the user types a day in
  // it (Min, an instant, comes again each time the app's laid out) starts
  // the day's digits over.
  const set = (i, k, v) => {
    if ((i.getAttribute(k) || "") === v) return;
    if (v) i.setAttribute(k, v); else i.removeAttribute(k);
  };
  set(n.start, "min", b.startMin);
  set(n.start, "max", b.startMax);
  set(n.end, "min", b.endMin);
  set(n.end, "max", b.endMax);
}

// --- Disclosure ---

const disclosure = {
  create(h, n) {
    const d = own(n, document.createElement("details"));
    d.className = "asuka-disclosure";
    const s = document.createElement("summary");
    s.className = "asuka-disclosure-header asuka-control";
    n.label = document.createElement("span");
    n.label.className = "asuka-disclosure-title";
    const chev = document.createElement("span");
    chev.className = "asuka-disclosure-chevron";
    const svg = symbolSVG("chevron.right", false);
    if (svg) chev.appendChild(svg);
    s.append(n.label, chev);
    d.appendChild(s);
    n.summary = n.focusEl = s;
    n.expanded = false;
    // The user's press (Space, Enter): the browser opens or closes it, then says so.
    d.addEventListener("toggle", () => {
      if (d.open === n.expanded) return; // the core's Expanded
      n.expanded = d.open;
      h.emit(n, Event.Toggled, (id) => Events.toggled(id, d.open));
    });
    text.applyFont(h, n, s);
  },
  prop(h, n, p, v) {
    if (p === P.Disclosure.Title) n.label.textContent = v;
    if (p === P.Disclosure.Expanded) {
      n.expanded = !!v;
      n.el.open = n.expanded;
    }
  },
  listen(h, n, e) { return e === Event.Toggled || e === Event.Press; },
  color(h, n) { n.el.style.setProperty("--tint", h.css(n.color) || ""); },
  disabled(h, n) {
    // A summary can't be disabled: it leaves the Tab order and takes no presses.
    n.el.classList.toggle("asuka-disabled", n.disabled);
    n.summary.tabIndex = n.disabled ? -1 : 0;
    if (n.disabled) n.summary.setAttribute("aria-disabled", "true"); else n.summary.removeAttribute("aria-disabled");
  },
  label(h, n, v) {
    n.el.removeAttribute("aria-label");
    if (v) n.summary.setAttribute("aria-label", v); else n.summary.removeAttribute("aria-label");
  },
  measure(h, n, c) {
    // As wide as it's laid out; as tall as its row.
    const [w, hh] = h.measureElement(n, n.el, unconstrained);
    if (c.width === c.width) return [c.width, hh];
    if (finite(c.availW)) return [Math.max(c.availW, 0), hh];
    return [w, hh];
  },
};

export const controlKinds = {
  [Kind.Chip]: chip,
  [Kind.DateRangePicker]: dateRange,
  [Kind.Disclosure]: disclosure,
};
