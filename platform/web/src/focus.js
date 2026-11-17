// The keyboard's focus on views (RFC 0010), as the browser moves it:
//
//   - Focusable makes a view's element a Tab stop (tabindex 0); a press
//     with a mouse focuses it, as the browser focuses what has a tabindex,
//     unless NoFocusOnPress (its mousedown's default prevented).
//   - FocusChange goes to the core for every view (not a TextField: its
//     Focus and Blur) whose element takes or loses the focus: 0 lost, 2
//     from the keyboard (CSS's :focus-visible, or the last input a key's),
//     else 1. The core follows it with the focused and focusVisible states.
//   - NoFocusRing: the view's element draws no outline (style.css).
//   - FocusTrap: while the focus is inside the view, Tab and Shift+Tab go
//     round the Tab stops inside it.
//   - KeyText: a character typed at a view listening for it that no view's
//     Keys took (keys.js, ahead of this) and that doesn't press it.
//   - Focus on a view that can't take the focus yet (in an overlay shown
//     once its anchor is known, a layer not yet shown) waits for a batch
//     after which it can: a few at most, and not past the user's moving
//     the focus or pressing.

import { Event, Events, Kind } from "./ops.js";

/** tabbable is the selector of what Tab stops at, before the checks of what shows. */
const tabbable = "a[href], button, input, select, textarea, [tabindex], [contenteditable=true], [contenteditable='']";

/** cycle is the element after (before, backward) cur in list, round its ends; with cur not in it, the first (last). */
export function cycle(list, cur, backward) {
  if (!list.length) return null;
  const at = list.indexOf(cur);
  if (at < 0) return backward ? list[list.length - 1] : list[0];
  return list[(at + (backward ? -1 : 1) + list.length) % list.length];
}

/** typed is the text a keydown types for KeyText: one character, no shortcut's modifier held; "" for none. */
export function typed(e) {
  if (e.isComposing || e.keyCode === 229 || e.ctrlKey || e.metaKey || e.altKey) return "";
  const k = e.key || "";
  const one = k.length === 1 || (k.length === 2 && k.codePointAt(0) > 0xffff);
  return one ? k : "";
}

/** waitBatches is how many batches a Focus waits for its view to take it. */
export const waitBatches = 8;

/** focusValue is FocusChange's value for a view taking the focus: 2 from the keyboard, else 1. */
export function focusValue(visible) { return visible ? 2 : 1; }

export class Focus {
  constructor(h) {
    this.h = h;
    this.cur = null; // the node whose element has the focus (as last reported)
    this.modality = ""; // "key" or "pointer": the last input
    this.pending = null; // { n, el }: a Focus waiting until it can
    this.quiet = 0; // moves of the host's own (an overlay brought to the top) aren't reported
    const c = h.container;
    c.addEventListener("focusin", (e) => this.focusIn(e));
    c.addEventListener("focusout", (e) => this.focusOut(e));
    c.addEventListener("mousedown", (e) => this.mouseDown(e));
    c.addEventListener("keydown", (e) => this.keyDown(e));
    if (typeof addEventListener === "function") {
      addEventListener("keydown", (e) => { if (!["Shift", "Control", "Alt", "Meta"].includes(e.key)) this.modality = "key"; }, true);
      addEventListener("pointerdown", () => { this.modality = "pointer"; this.pending = null; }, true);
    }
  }

  // --- Props ---

  /** setFocusable takes Focusable: a Tab stop, or not (unless it's one as a pressable view, a button or adjustable). */
  setFocusable(n, on) {
    n.focusable = !!on;
    const el = n.el;
    if (!el || n.focusEl) return; // a control: a Tab stop of its own
    if (on) {
      if (!el.hasAttribute("tabindex") || el.tabIndex < 0) el.tabIndex = 0;
      return;
    }
    const pressable = n.listening(Event.Press) || n.menu || n.adjustable || el.getAttribute("role") === "button";
    if (!pressable) el.removeAttribute("tabindex");
  }

  /** setNoRing takes NoFocusRing: its element (and its control's) draws no focus outline. */
  setNoRing(n, on) {
    n.el?.classList.toggle("asuka-no-ring", !!on);
    n.focusEl?.classList.toggle("asuka-no-ring", !!on);
  }

  // --- Commands ---

  /** focus is the core's Focus: now if el can take it, else once it can (pending). */
  focus(n, el) {
    this.pending = null;
    if (!el) return;
    if (this.tryFocus(el)) return;
    this.pending = { n, el, batches: 0 };
  }

  blur(n, el) {
    if (this.pending?.n === n) this.pending = null;
    el?.blur();
  }

  tryFocus(el) {
    if (!el.isConnected) return false;
    el.focus({ preventScroll: true, focusVisible: this.modality === "key" });
    return document.activeElement === el;
  }

  /** batched: a batch was applied; a Focus waiting goes now if it can. */
  batched() {
    const p = this.pending;
    if (!p) return;
    if (p.n.dying || ++p.batches > waitBatches) { this.pending = null; return; }
    this.h.afterCore(() => { if (this.pending === p && this.tryFocus(p.el)) this.pending = null; });
  }

  // --- Reporting ---

  /** nodeOf is the view whose element el is, or is in. */
  nodeOf(el) {
    const c = this.h.container;
    for (; el && el !== c; el = el.parentElement) if (el.asukaNode) return el.asukaNode;
    return null;
  }

  visible(el) {
    if (this.modality === "key") return true;
    try { return el.matches(":focus-visible"); } catch { return false; }
  }

  focusIn(e) {
    if (this.quiet) return;
    if (this.pending && e.target !== this.pending.el) this.pending = null; // the focus went elsewhere
    const n = this.nodeOf(e.target);
    if (n === this.cur) return;
    this.lost();
    if (!n || n.kind === Kind.TextField) return;
    this.cur = n;
    this.h.dispatch(Events.focusChange(n.id, focusValue(this.visible(e.target))));
  }

  focusOut(e) {
    if (this.quiet) return;
    // The page's window going to the background keeps its focus where it is.
    if (!document.hasFocus() && document.activeElement === e.target) return;
    const next = e.relatedTarget ? this.nodeOf(e.relatedTarget) : null;
    if (next && next === this.cur) return;
    this.lost();
  }

  lost() {
    const n = this.cur;
    this.cur = null;
    if (n && !n.dying && this.h.nodes.get(n.id) === n) this.h.dispatch(Events.focusChange(n.id, 0));
  }

  /** forget: a view is going; its focus goes with it, unreported. */
  forget(n) {
    if (this.cur === n) this.cur = null;
    if (this.pending?.n === n) this.pending = null;
  }

  // --- Pointers and keys ---

  mouseDown(e) {
    if (e.button !== 0) return;
    const t = e.target?.nodeType === 1 ? e.target : null;
    const f = t?.closest?.(tabbable);
    if (!f || !this.h.container.contains(f)) return;
    const n = this.nodeOf(f);
    if (n?.noFocusOnPress && (f === n.el || f === n.focusEl)) e.preventDefault();
  }

  keyDown(e) {
    if (e.defaultPrevented) return;
    if (e.key === "Tab" && !e.ctrlKey && !e.metaKey && !e.altKey) {
      this.tab(e);
      return;
    }
    const s = typed(e);
    if (!s) return;
    const t = e.target;
    if (t?.closest?.("input, textarea, select, [contenteditable=true], [contenteditable='']")) return;
    const n = this.nodeOf(t);
    if (!n || !n.listening(Event.KeyText)) return;
    if (s === " ") e.preventDefault(); // not the page's scroll
    this.h.dispatch(Events.keyText(n.id, s));
  }

  /** trapOf is the innermost view with FocusTrap around el, or null. */
  trapOf(el) {
    const c = this.h.container;
    for (; el && el !== c; el = el.parentElement) {
      const n = el.asukaNode;
      if (n?.trap && n.el === el) return n;
    }
    return null;
  }

  /** stops are the Tab stops in el, in the page's order: shown, enabled, not taken out of the order. */
  stops(el) {
    const out = [];
    const all = [el, ...el.querySelectorAll(tabbable)];
    for (const x of all) {
      if (!x.matches(tabbable) || x.disabled || x.tabIndex < 0 || x.hidden) continue;
      if (x.closest("[inert], .asuka-hidden, .asuka-exiting")) continue;
      if (!x.getClientRects().length) continue;
      if (x.type === "radio" && x.name && !x.checked && el.querySelector(`input[type=radio][name="${CSS.escape(x.name)}"]:checked`)) continue;
      out.push(x);
    }
    return out;
  }

  tab(e) {
    const a = document.activeElement;
    const trap = this.trapOf(a);
    if (!trap) return;
    e.preventDefault();
    const next = cycle(this.stops(trap.el), a, e.shiftKey);
    if (next && next !== a) next.focus({ preventScroll: false, focusVisible: true });
  }
}
