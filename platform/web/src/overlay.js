// An Overlay (RFC 0010) in a browser: a layer the core fills (a menu, a
// listbox, a dialog of std/base's) over a layout root: the main window's
// root view, a Sheet's, a Window's or a Popover's. Its element is in the
// top layer (a manual popover, position: fixed) over that root's element,
// as big as it, inside the root's own dialog where it has one (outside a
// modal <dialog> everything is inert); its children are placed at their
// frames in the root's coordinates. Present brings it over the others
// (shown again: the last shown is on top).
//
// It takes no presses of its own: presses go through it to what's under
// it, but its children's, unless it's Modal; then it takes every press in
// its root, and the rest of the root is inert (no presses, hidden from
// screen readers). A press (pointerdown, in the capture phase: before it
// reaches its target) that begins in its root but on none of its views is
// reported to it as OutsidePress, in the root's coordinates.
//
// It's sized by the host, which reports its Environment (its root's), and
// it reports where its Anchor view is (AnchorFrame): after each batch, on
// a scroll of anything (a capture-phase listener) and a resize, at most
// once a frame, never the same twice. Escape is the core's (the root's
// Keys); the web's back doesn't come to it.

import { Kind, KindProp as P, Event, Events, Command } from "./ops.js";

const nav = new Set([Kind.Sheet, Kind.Window, Kind.Popover]);

/** encodeAnchorFrame is AnchorFrame's payload (ops.AppendAnchorFrame): f32 x, y, w, h, then u8 visible. */
export function encodeAnchorFrame(x, y, w, h, visible) {
  const b = new Uint8Array(17);
  const v = new DataView(b.buffer);
  v.setFloat32(0, x, true);
  v.setFloat32(4, y, true);
  v.setFloat32(8, w, true);
  v.setFloat32(12, h, true);
  b[16] = visible ? 1 : 0;
  return b;
}

/** sameBytes reports whether a and b hold the same bytes. */
export function sameBytes(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

/**
 * shows reports whether any of rectangle r ({left, top, right, bottom})
 * shows within each of clips (the root's, and each clipping view's
 * around it): a rectangle of no size shows where its point is in them.
 */
export function shows(r, clips) {
  let { left, top, right, bottom } = r;
  for (const c of clips) {
    left = Math.max(left, c.left);
    top = Math.max(top, c.top);
    right = Math.min(right, c.right);
    bottom = Math.min(bottom, c.bottom);
  }
  const flat = r.right - r.left <= 0 || r.bottom - r.top <= 0;
  return flat ? right >= left && bottom >= top : right > left && bottom > top;
}

/** isOpen reports whether a popover element shows. */
function isOpen(el) {
  try { return el.matches(":popover-open"); } catch { return false; }
}

/** stacked is overlays in the order they show: the last presented on top. */
export function stacked(list) {
  return [...list].sort((a, b) => a.ov.seq - b.ov.seq);
}

export const overlayKind = {
  create(h, n) {
    const el = document.createElement("div");
    el.className = "asuka-overlay";
    n.el = n.box = el;
    n.ov = { root: 0, anchor: 0, modal: false, seq: 0, presented: false, sent: null };
  },
  prop(h, n, p, v) {
    switch (p) {
      case P.Overlay.Root:
        n.ov.root = v;
        if (n.ov.presented) h.overlays.attach(n);
        break;
      case P.Overlay.Anchor:
        n.ov.anchor = v;
        n.ov.sent = null; // reported again
        h.overlays.changed();
        break;
      case P.Overlay.Modal:
        n.ov.modal = !!v;
        n.el.classList.toggle("asuka-overlay-modal", n.ov.modal);
        h.overlays.updateModal();
        break;
    }
  },
  frame() { return true; }, // its root's area, the host's
  command(h, n, cmd) {
    if (cmd !== Command.Present) return false;
    h.overlays.present(n);
    return true;
  },
  destroy(h, n) {
    h.overlays.remove(n);
    return false; // its element goes as a view's
  },
  measure() { return [0, 0]; },
};

export class Overlays {
  constructor(h) {
    this.h = h;
    this.list = []; // shown overlays
    this.seq = 0;
    this.frame = 0; // a requestAnimationFrame waiting
    this.lastShown = null;
    if (typeof addEventListener === "function") {
      addEventListener("pointerdown", (e) => this.pointerDown(e), true);
      addEventListener("scroll", () => this.changed(), true);
      addEventListener("resize", () => this.layout());
    }
  }

  /** rootOf is the node an overlay is over: its Root, or the main root. */
  rootOf(n) {
    const r = this.h.nodes.get(n.ov.root);
    return r && !r.dying ? r : this.h.root;
  }

  /** hostOf is the element an overlay's element goes in: its root's dialog (a sheet's, a window's, a popover's), else the app's. */
  hostOf(n) {
    const r = this.rootOf(n);
    if (r && nav.has(r.kind) && r.dialog?.isConnected) return r.dialog;
    return this.h.container;
  }

  /** rootElement is the element whose area an overlay covers. */
  rootElement(n) {
    const r = this.rootOf(n);
    return r?.el?.isConnected ? r.el : this.h.container;
  }

  present(n) {
    if (!n.ov.presented) {
      n.ov.presented = true;
      this.list.push(n);
    }
    n.ov.seq = ++this.seq;
    this.attach(n, true);
    this.place(n);
    this.updateModal();
    this.changed();
  }

  /** attach puts n's element in its host and shows it; raise: over the others (shown again, unless it's the last shown). */
  attach(n, raise) {
    const el = n.el, host = this.hostOf(n);
    const moved = el.parentNode !== host;
    const open = isOpen(el);
    if (!moved && open && (!raise || this.lastShown === n)) return;
    // Shown again (moved, or brought over the others): its focus kept, unreported.
    const a = document.activeElement;
    const had = a && el.contains(a);
    this.h.focus.quiet++;
    try {
      if (open) el.hidePopover();
      if (moved) host.appendChild(el);
      if (el.showPopover) {
        if (!el.popover) el.popover = "manual";
        el.showPopover();
      }
      this.lastShown = n;
      if (had && a.isConnected) a.focus({ preventScroll: true });
    } finally {
      this.h.focus.quiet--;
    }
  }

  remove(n) {
    this.list = this.list.filter((x) => x !== n);
    n.ov.presented = false;
    if (this.lastShown === n) this.lastShown = null;
    if (isOpen(n.el)) {
      this.h.focus.quiet++;
      try { n.el.hidePopover(); } finally { this.h.focus.quiet--; }
    }
    this.updateModal();
  }

  /** place puts n's element over its root, and reports its Environment. */
  place(n) {
    if (!n.ov.presented) return;
    const rootEl = this.rootElement(n);
    const r = rootEl.getBoundingClientRect();
    const s = n.el.style;
    s.left = r.left + "px";
    s.top = r.top + "px";
    s.width = r.width + "px";
    s.height = r.height + "px";
    this.h.nav.report(n, r.width, r.height, rootEl);
  }

  /** layout places every overlay again (the window resized, a root moved), then their anchors. */
  layout() {
    for (const n of this.list) {
      if (n.ov.presented && n.el.parentNode !== this.hostOf(n)) this.attach(n);
      this.place(n);
    }
    this.anchors();
  }

  /** batched: a batch was applied: hosts that came (a popover's), then anchors, now and on the next frame. */
  batched() {
    if (!this.list.length) return;
    this.h.afterCore(() => {
      for (const n of this.list) if (n.el.parentNode !== this.hostOf(n)) { this.attach(n); this.place(n); }
      this.anchors();
    });
    this.changed();
  }

  /** changed asks for the anchors' frames on the next frame (at most once a frame). */
  changed() {
    if (this.frame || !this.list.length || typeof requestAnimationFrame !== "function") return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      this.layout();
    });
  }

  /** anchors reports each overlay's anchor's frame, where it changed. */
  anchors() {
    for (const n of this.list) {
      if (!n.ov.anchor || !n.listening(Event.AnchorFrame)) continue;
      const a = this.h.nodes.get(n.ov.anchor);
      if (!a?.el) continue;
      const b = this.anchorFrame(n, a);
      if (sameBytes(b, n.ov.sent)) continue;
      n.ov.sent = b;
      this.h.dispatch(Events.anchorFrame(n.id, b));
    }
  }

  /** anchorFrame is AnchorFrame for anchor a of overlay n: a's frame in n's root, and whether any of it shows. */
  anchorFrame(n, a) {
    const rootEl = this.rootElement(n);
    const root = rootEl.getBoundingClientRect();
    const el = a.el;
    const r = el.getBoundingClientRect();
    let visible = el.isConnected && el.getClientRects().length > 0 && !el.closest(".asuka-hidden, .asuka-hidden-page, .asuka-covered, [hidden]");
    if (visible) {
      const clips = [root];
      for (let p = el.parentElement; p && p !== rootEl && p !== this.h.container; p = p.parentElement) {
        const cs = getComputedStyle(p);
        if (cs.overflowX !== "visible" || cs.overflowY !== "visible") clips.push(p.getBoundingClientRect());
      }
      visible = shows(r, clips);
    }
    return encodeAnchorFrame(r.left - root.left, r.top - root.top, r.width, r.height, visible);
  }

  /** updateModal makes inert what's under the topmost modal overlay over each root: the root and the overlays under it. */
  updateModal() {
    const want = new Set();
    const byRoot = new Map();
    for (const n of stacked(this.list)) {
      const r = this.rootOf(n);
      if (!byRoot.has(r)) byRoot.set(r, []);
      byRoot.get(r).push(n);
    }
    for (const [r, ns] of byRoot) {
      let top = -1;
      ns.forEach((n, i) => { if (n.ov.modal && !n.hiddenInert) top = i; });
      if (top < 0) continue;
      if (r) want.add(r);
      for (let i = 0; i < top; i++) want.add(ns[i]);
    }
    for (const n of this.h.nodes.values()) {
      if (!!n.modalInert !== want.has(n)) this.h.setModalInert(n, want.has(n));
    }
  }

  /** pointerDown reports a press to each overlay it began outside of, in its root. */
  pointerDown(e) {
    if (!this.list.length) return;
    const t = e.target?.nodeType === 1 ? e.target : null;
    if (!t) return;
    for (const n of stacked(this.list).reverse()) {
      if (!n.listening(Event.OutsidePress) || n.dying) continue;
      if (t !== n.el && n.el.contains(t)) continue; // on one of its views
      if (t !== n.el && this.rootAt(t) !== this.rootOf(n)) continue; // in another root
      const r = this.rootElement(n).getBoundingClientRect();
      this.h.dispatch(Events.outsidePress(n.id, e.clientX - r.left, e.clientY - r.top));
    }
  }

  /** rootAt is the layout root el is in: an overlay's root, a sheet's, window's or popover's node, else the main root (null: not the app's). */
  rootAt(el) {
    const c = this.h.container;
    for (; el && el !== c; el = el.parentElement) {
      const n = el.asukaNode;
      if (n?.kind === Kind.Overlay && n.ov) return this.rootOf(n);
      if (n && nav.has(n.kind) && !n.parent) return n;
      for (const d of this.h.nav.dialogs) if (d.dialog === el) return d;
    }
    return el === c || this.h.options.fullPage ? this.h.root : null;
  }
}
