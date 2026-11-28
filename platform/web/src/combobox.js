// A TextField's Suggestions: a WAI-ARIA 1.2 combobox. The field's input
// is the combobox (aria-autocomplete="list", aria-expanded,
// aria-controls, aria-activedescendant) and its list a role="listbox" of
// role="option"s under it, in the top layer (a manual popover), so a
// scroller's edge doesn't cut it; the keyboard's focus stays in the
// field. Not a <datalist>: browsers filter it again (the core has) and
// never say an option was chosen.
//
// The list shows while the field has the focus, the Suggestions aren't
// empty and the user has typed since it took the focus or since a choice
// (not on focus alone); Dropdown's button (a chevron at the field's end)
// opens it whether or not they typed, and closes it. Down and Up move
// through it (from the field, closed: open it at the first or the last),
// Enter chooses, Escape and Tab close it; a press on an option chooses
// it. Choosing puts its text in the field, the cursor at its end, and
// reports Suggestion, not TextChanged.

import { Event, Events } from "./ops.js";
import { symbolSVG } from "./media.js";

/** ComboModel is a combobox's state and keyboard (no page): what it shows, what a key does. */
export class ComboModel {
  constructor() {
    this.items = [];
    this.focused = false;
    this.typed = false; // the user typed since the focus or a choice
    this.forced = false; // opened by Dropdown's button or an arrow key
    this.active = -1; // the option the arrows are on (aria-activedescendant)
  }

  /** open says the list shows. */
  get open() { return this.focused && this.items.length > 0 && (this.typed || this.forced); }

  /** setItems takes new Suggestions; the arrows stay on their option while it's there. */
  setItems(items) {
    this.items = items;
    if (this.active >= items.length) this.active = -1;
  }

  focus() { this.focused = true; this.close(); }
  blur() { this.focused = false; this.close(); }

  /** input: the user typed, which shows the list (and takes the arrows off it). */
  input() { this.typed = true; this.forced = false; this.active = -1; }

  close() { this.typed = false; this.forced = false; this.active = -1; }

  /** toggle is Dropdown's button: the list, all its Suggestions, opened or closed. */
  toggle() {
    if (this.open) { this.close(); return; }
    this.focused = true;
    this.forced = this.items.length > 0;
    this.active = -1;
  }

  /**
   * key is what a key pressed in the field does: { handled } (its default
   * and the field's own use of it prevented), and choose, an option's
   * index, when it chooses one.
   */
  key(k, alt = false) {
    const n = this.items.length;
    switch (k) {
      case "ArrowDown":
        if (!n) return { handled: false };
        if (!this.open) {
          this.forced = true;
          this.active = alt ? -1 : 0;
        } else if (!alt) {
          this.active = (this.active + 1) % n;
        }
        return { handled: true };
      case "ArrowUp":
        if (!n) return { handled: false };
        if (alt) {
          if (this.open) { this.close(); return { handled: true }; }
          return { handled: false };
        }
        if (!this.open) this.forced = true;
        this.active = this.active <= 0 ? n - 1 : this.active - 1;
        return { handled: true };
      case "Enter":
        if (this.open && this.active >= 0) {
          const i = this.active;
          this.close();
          return { handled: true, choose: i };
        }
        if (this.open) this.close(); // and the field's return, as without a list
        return { handled: false };
      case "Escape":
        if (!this.open) return { handled: false };
        this.close();
        return { handled: true };
      case "Tab":
        this.close();
        return { handled: false };
    }
    return { handled: false };
  }
}

/** gap is the room between the field and its list, and the list and the window's edge. */
const gap = 4, edge = 8;
/** maxListHeight is the most a list is tall before it scrolls: about seven options. */
export const maxListHeight = 280;

/**
 * placeList is where a list content tall goes for a field at anchor (a
 * rect) in a window view ({ top, height }): under it, or over it when
 * there's more room there and not enough under (a phone's keyboard).
 */
export function placeList(anchor, view, content) {
  const below = view.top + view.height - anchor.bottom - gap - edge;
  const above = anchor.top - view.top - gap - edge;
  const want = Math.min(content, maxListHeight);
  const up = below < want && above > below;
  const height = Math.max(Math.min(want, up ? above : below), 0);
  return { top: up ? anchor.top - gap - height : anchor.bottom + gap, maxHeight: height, up };
}

let ids = 0;

/** Combobox is a field's suggestions in the page: its input's ARIA, its list, its Dropdown button. */
export class Combobox {
  constructor(h, n) {
    this.h = h;
    this.n = n;
    this.model = new ComboModel();
    this.id = "asuka-combo-" + (++ids);
    this.shown = null; // the items the list's options are of
    this.list = null;
    this.button = null;
    this.placing = null; // the listeners that keep it under the field while it shows
    this.attach(n.input);
  }

  /** attach makes input the combobox (the field's input, made again when it changes). */
  attach(input) {
    if (input.asukaCombo === this) return;
    input.asukaCombo = this;
    input.setAttribute("role", "combobox");
    input.setAttribute("aria-autocomplete", "list");
    input.setAttribute("aria-controls", this.id);
    input.setAttribute("aria-expanded", "false");
    input.setAttribute("autocomplete", input.getAttribute("autocomplete") || "off");
    input.addEventListener("focus", () => { this.model.focus(); this.render(); });
    input.addEventListener("blur", () => { this.model.blur(); this.render(); });
    if (this.button) input.after(this.button);
    this.labelChanged();
  }

  setItems(items) {
    this.model.setItems(items);
    this.render();
  }

  /** setDropdown gives the field Dropdown's button, at its end, or takes it away. */
  setDropdown(on) {
    if (!on) {
      this.button?.remove();
      this.button = null;
      this.n.el.classList.remove("asuka-combo-dropdown");
      return;
    }
    if (this.button) return;
    const b = document.createElement("button");
    b.type = "button";
    b.className = "asuka-combo-button";
    b.tabIndex = -1; // the field is the Tab stop; Alt+Down opens it from there
    b.setAttribute("aria-controls", this.id);
    b.setAttribute("aria-expanded", "false");
    const s = symbolSVG("chevron.down", false);
    if (s) b.appendChild(s);
    // The field keeps the keyboard's focus.
    b.addEventListener("pointerdown", (e) => e.preventDefault());
    b.addEventListener("mousedown", (e) => e.preventDefault());
    b.addEventListener("click", (e) => {
      e.stopPropagation();
      const input = this.n.input;
      if (input.disabled) return;
      if (document.activeElement !== input) input.focus({ preventScroll: true });
      this.model.toggle();
      this.render();
    });
    this.button = b;
    this.n.input.after(b);
    this.n.el.classList.add("asuka-combo-dropdown");
    this.labelChanged();
  }

  /** labelChanged names the list and the button as the field is named (its label, else its placeholder). */
  labelChanged() {
    const input = this.n.input;
    const name = input.getAttribute("aria-label") || input.placeholder || "";
    if (this.list) {
      if (name) this.list.setAttribute("aria-label", name); else this.list.removeAttribute("aria-label");
    }
    if (this.button) {
      if (name) this.button.setAttribute("aria-label", name); else this.button.removeAttribute("aria-label");
    }
  }

  disabled(on) {
    if (this.button) this.button.disabled = on;
    if (on && this.model.open) { this.model.close(); this.render(); }
  }

  /** input: the user typed in the field. */
  input() {
    this.model.input();
    this.render();
  }

  /** keydown is a key in the field; true: the combobox took it (the field doesn't submit). */
  keydown(e) {
    if (e.isComposing || e.keyCode === 229) return false;
    const r = this.model.key(e.key, e.altKey);
    if (r.handled) {
      e.preventDefault();
      e.stopPropagation();
    }
    if (r.choose != null) this.choose(r.choose);
    else this.render();
    return r.handled;
  }

  /** choose puts option i's text in the field and reports it (not as typing). */
  choose(i) {
    const text = this.model.items[i];
    if (text == null) return;
    this.model.close();
    const input = this.n.input;
    input.value = text;
    this.n.text = text;
    try { input.setSelectionRange(text.length, text.length); } catch {}
    this.render();
    this.h.emit(this.n, Event.Suggestion, (id) => Events.suggestion(id, i));
  }

  /** render shows the model: the input's state, the list, the button's. */
  render() {
    const m = this.model, input = this.n.input;
    const open = m.open;
    input.setAttribute("aria-expanded", String(open));
    this.button?.setAttribute("aria-expanded", String(open));
    if (!open) {
      input.removeAttribute("aria-activedescendant");
      this.hide();
      return;
    }
    const l = this.listEl();
    if (this.shown !== m.items) this.fill(l, m.items);
    const opts = l.children;
    for (let i = 0; i < opts.length; i++) opts[i].setAttribute("aria-selected", String(i === m.active));
    if (m.active >= 0) {
      input.setAttribute("aria-activedescendant", opts[m.active].id);
      opts[m.active].scrollIntoView?.({ block: "nearest" });
    } else {
      input.removeAttribute("aria-activedescendant");
    }
    if (!l.classList.contains("asuka-combo-open")) this.show(l);
    this.place();
  }

  listEl() {
    if (this.list) return this.list;
    const l = document.createElement("div");
    l.id = this.id;
    l.className = "asuka-combo-list";
    l.setAttribute("role", "listbox");
    l.tabIndex = -1;
    l.popover = "manual";
    // A press in the list leaves the focus in the field.
    l.addEventListener("pointerdown", (e) => e.preventDefault());
    l.addEventListener("mousedown", (e) => e.preventDefault());
    l.addEventListener("click", (e) => {
      e.stopPropagation();
      const o = e.target.closest?.("[role=option]");
      if (o && l.contains(o)) this.choose(Number(o.dataset.index));
    });
    this.list = l;
    this.labelChanged();
    return l;
  }

  fill(l, items) {
    l.textContent = "";
    items.forEach((t, i) => {
      const o = document.createElement("div");
      o.id = `${this.id}-${i}`;
      o.className = "asuka-combo-option";
      o.setAttribute("role", "option");
      o.dataset.index = String(i);
      o.textContent = t;
      l.appendChild(o);
    });
    this.shown = items;
  }

  show(l) {
    // In the field's box: inside a modal dialog's, which leaves what's
    // outside it inert; the top layer takes it out of the box's clip.
    if (l.parentNode !== this.n.el) this.n.el.appendChild(l);
    l.dir = this.h.rtl ? "rtl" : "ltr";
    l.style.font = getComputedStyle(this.n.input).font;
    l.classList.add("asuka-combo-open");
    try { l.showPopover(); } catch {}
    if (!this.placing) {
      const f = () => this.place();
      this.placing = f;
      addEventListener("scroll", f, true);
      addEventListener("resize", f);
      visualViewport?.addEventListener("resize", f);
    }
  }

  hide() {
    const l = this.list;
    if (!l || !l.classList.contains("asuka-combo-open")) return;
    l.classList.remove("asuka-combo-open");
    try { l.hidePopover(); } catch {}
    if (this.placing) {
      removeEventListener("scroll", this.placing, true);
      removeEventListener("resize", this.placing);
      visualViewport?.removeEventListener("resize", this.placing);
      this.placing = null;
    }
  }

  /** place puts the list under the field (over it near the keyboard), as wide. */
  place() {
    const l = this.list;
    if (!l || !l.classList.contains("asuka-combo-open")) return;
    const a = this.n.el.getBoundingClientRect();
    const vv = globalThis.visualViewport;
    const view = vv ? { top: vv.offsetTop, height: vv.height } : { top: 0, height: innerHeight };
    const s = l.style;
    s.maxHeight = "none";
    const content = l.scrollHeight;
    const p = placeList(a, view, content);
    s.left = a.left + "px";
    s.width = a.width + "px";
    s.top = p.top + "px";
    s.maxHeight = p.maxHeight + "px";
    l.classList.toggle("asuka-combo-up", p.up);
  }

  destroy() {
    this.hide();
    this.list?.remove();
  }
}
