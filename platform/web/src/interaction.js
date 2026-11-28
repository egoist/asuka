// Interaction: presses, long presses, pressed and hover states, pans,
// pinches, rotations and swipes, drag and drop, and the screen reader's
// adjusting. One set of pointer listeners on the app's container finds
// the view each pointer is for: the innermost one that's pressable (or a
// control, which keeps its own presses), as UIKit's and Android's views
// behave.

import { Event, Events, Writer, gesture, Reader } from "./ops.js";
import * as fs from "./fs.js";
import { isSecondTap, doubleTime, panDecision } from "./gestures.js";

const slop = 10; // points a touch moves before it's a drag, not a tap
const longPressMs = 500;
const tapDelayMs = 150; // UIScrollView's delay before a touch in it shows pressed
const pressedMinMs = 64; // a quick tap still shows its pressed look
const menuClickMs = 500; // how long a menu waits for its release's click

const controls = "button, input, textarea, select, a[href], [contenteditable=true], .asuka-control";

const pressEvents = new Set([Event.Press, Event.LongPress, Event.Pressed]);
const gestureEvents = new Set([Event.Pan, Event.Pinch, Event.Rotate, Event.Swipe]);

export class Interaction {
  constructor(h) {
    this.h = h;
    this.press = null; // the primary pointer's press
    this.pointers = new Map(); // pointerId -> {x, y} (for pinches)
    this.gesture = null; // a pan, pinch or rotation under way
    this.wheel = null; // a trackpad's pinch (ctrl + wheel)
    this.dragSource = null;
    this.tapping = null; // the primary pointer's tap under way, on a view that hears of double taps
    this.lastTap = null; // the tap before it: { n, t, x, y, type, pending: its Press, waiting for a second tap }
    const c = h.container;
    c.addEventListener("pointerdown", (e) => this.down(e));
    c.addEventListener("pointermove", (e) => this.move(e));
    c.addEventListener("pointerup", (e) => this.up(e));
    c.addEventListener("pointercancel", (e) => this.cancel(e));
    c.addEventListener("mousedown", (e) => this.mouseDown(e));
    c.addEventListener("contextmenu", (e) => this.context(e));
    c.addEventListener("keydown", (e) => this.key(e));
    c.addEventListener("wheel", (e) => this.wheelPinch(e), { passive: false });
    c.addEventListener("scroll", () => this.scrolled(), true);
    c.addEventListener("dragstart", (e) => this.dragStart(e));
    c.addEventListener("dragend", (e) => this.dragEnd(e));
    c.addEventListener("dragover", (e) => this.dragOver(e));
    c.addEventListener("dragleave", (e) => this.dragLeave(e));
    c.addEventListener("drop", (e) => this.drop(e));
    window.addEventListener("blur", () => this.cancelAll());
  }

  /** listen makes n report what the core listens for. */
  listen(n, e) {
    if (!n.el) return;
    if (pressEvents.has(e)) this.pressable(n);
    else if (gestureEvents.has(e)) {
      n.el.classList.add("asuka-gesture");
      this.panConfig(n);
    } else if (e === Event.Hover) this.hoverable(n);
    else if (e === Event.HoverMove) this.hoverMoves(n, true);
  }

  unlisten(n, e) {
    if (e === Event.HoverMove) this.hoverMoves(n, false);
    if (gestureEvents.has(e)) this.panConfig(n);
    if (e === Event.Hover && n.hover) {
      n.el.removeEventListener("pointerenter", n.hover.enter);
      n.el.removeEventListener("pointerleave", n.hover.leave);
      if (n.hovered) this.h.dispatch(Events.hover(n.id, false));
      n.hover = null;
      n.hovered = false;
    }
    if (pressEvents.has(e) && !n.listening(Event.Press) && !n.listening(Event.LongPress) && !n.listening(Event.Pressed) && !n.menu) {
      n.el.classList.remove("asuka-pressable");
    }
  }

  /** pressable marks n as a view that takes presses: a button for screen readers and the keyboard, if it fires one. */
  pressable(n) {
    n.el.classList.add("asuka-pressable");
    if (n.listening(Event.Press) || n.menu) {
      if (!n.el.hasAttribute("role")) n.el.setAttribute("role", "button");
      if (!n.el.hasAttribute("tabindex")) n.el.tabIndex = 0;
    }
  }

  hoverable(n) {
    if (n.hover) return;
    n.hover = {
      enter: (e) => { if (e.pointerType !== "touch" && !n.hovered) { n.hovered = true; this.h.dispatch(Events.hover(n.id, true)); } },
      leave: () => { if (n.hovered) { n.hovered = false; this.h.dispatch(Events.hover(n.id, false)); } },
    };
    n.el.addEventListener("pointerenter", n.hover.enter);
    n.el.addEventListener("pointerleave", n.hover.leave);
    if (n.el.matches(":hover")) n.hover.enter({ pointerType: "mouse" });
  }

  /** hoverMoves reports where the pointer is over n (HoverMove) as it moves, while the core listens; a touch doesn't hover. */
  hoverMoves(n, on) {
    if (!on) {
      if (n.hoverMove) n.el.removeEventListener("pointermove", n.hoverMove);
      n.hoverMove = null;
      return;
    }
    if (n.hoverMove) return;
    n.hoverMove = (e) => {
      if (e.pointerType === "touch") return;
      const r = n.el.getBoundingClientRect();
      this.h.dispatch(Events.hoverMove(n.id, e.clientX - r.left, e.clientY - r.top));
    };
    n.el.addEventListener("pointermove", n.hoverMove);
  }

  /**
   * panConfig follows a view's Pan props: along one axis (PanAxis), a
   * touch's drag along the other is the browser's, a scroll (touch-action
   * pan-y, pan-x); with the scroll (PanSimultaneous), the browser scrolls
   * as the pan follows the touch, which then comes as touch events (the
   * browser's scroll cancels the touch's pointer).
   */
  panConfig(n) {
    if (!n.el) return;
    const pans = n.listening(Event.Pan) || n.listening(Event.Swipe);
    n.el.style.touchAction = !pans ? "" : n.panSimultaneous ? "pan-x pan-y" : n.panAxis === 1 ? "pan-y" : n.panAxis === 2 ? "pan-x" : "";
    if (pans && n.panSimultaneous) this.touchPans(n);
    else if (n.touchPan) {
      for (const [t, f] of Object.entries(n.touchPan)) n.el.removeEventListener(t, f);
      n.touchPan = null;
    }
  }

  /** touchPans follows a simultaneous Pan's touches with touch events, which keep coming while the browser scrolls. */
  touchPans(n) {
    if (n.touchPan) return;
    let g = null;
    const find = (e) => {
      for (const t of e.changedTouches) if (t.identifier === g.id) return t;
      return null;
    };
    const end = (e, state) => {
      if (!g) return;
      const t = find(e);
      if (!t && e.touches.length) return;
      const was = g;
      g = null;
      if (was.began) {
        this.pan(was, t || was.last, state);
        if (state === 2) this.swipe(was, t || was.last);
      }
    };
    n.touchPan = {
      touchstart: (e) => {
        if (e.touches.length !== 1) { end(e, 3); return; }
        const t = e.changedTouches[0];
        g = this.panGesture(n, t.identifier, t.clientX, t.clientY, e.timeStamp);
        g.last = t;
      },
      touchmove: (e) => {
        const t = g && find(e);
        if (!t) return;
        g.last = t;
        if (!this.panStep(g, t, e.timeStamp)) g = null;
      },
      touchend: (e) => end(e, 2),
      touchcancel: (e) => end(e, 3),
    };
    for (const [t, f] of Object.entries(n.touchPan)) n.el.addEventListener(t, f, { passive: true });
  }

  /** panGesture is a pan that may begin on n, its pointer (or touch) id down at x, y. */
  panGesture(n, id, x, y, t) {
    const min = n.panMinDistance > 0 ? n.panMinDistance : slop;
    return { n, kind: "pan", x0: x, y0: y, t0: t, began: false, samples: [[t, x, y]], id, min };
  }

  /** panStep follows a pan's pointer to pt (clientX, clientY): it begins past its distance, along its axis; false when the drag isn't a pan. */
  panStep(g, pt, t) {
    g.samples.push([t, pt.clientX, pt.clientY]);
    if (g.samples.length > 6) g.samples.shift();
    if (g.began) {
      this.pan(g, pt, 1);
      return true;
    }
    const d = panDecision(pt.clientX - g.x0, pt.clientY - g.y0, g.n.panAxis | 0, g.min);
    if (d < 0) return false;
    if (d > 0) {
      g.began = true;
      if (!g.n.panSimultaneous && pt.pointerId != null) {
        try { g.n.el.setPointerCapture(pt.pointerId); } catch {}
      }
      this.pan(g, pt, 0);
    }
    return true;
  }

  /** takeTouch: something of the host's own (a zoom's pinch, a row lifted) took the pointer: no press, no tap. */
  takeTouch() {
    this.endPress();
    if (this.tapping?.second) this.resumePress();
    this.tapping = null;
    const g = this.gesture;
    if (g?.kind === "pan") {
      this.gesture = null;
      if (g.began) this.pan(g, { clientX: g.x0, clientY: g.y0 }, 3);
    }
  }

  // --- Double taps ---

  /** doubleTarget is the innermost view at el that hears of double taps (or a zoomable Scroll, which zooms), or null where a control takes the press. */
  doubleTarget(el) {
    for (; el && el !== this.h.container; el = el.parentElement) {
      if (el.matches(controls)) return null;
      const n = el.asukaNode;
      if (n && (n.listening(Event.DoubleTap) || n.zoom?.on)) return n;
    }
    return null;
  }

  tapDown(e) {
    const n = this.doubleTarget(e.target);
    const last = this.lastTap;
    const second = !!(n && last && last.n === n && isSecondTap(last, { t: e.timeStamp, x: e.clientX, y: e.clientY }, e.pointerType));
    // A second tap holds the first's Press until it's known to be a double tap.
    if (second) clearTimeout(last.pending?.timer);
    else this.lastTap = null;
    this.tapping = n ? { n, id: e.pointerId, x0: e.clientX, y0: e.clientY, t0: e.timeStamp, type: e.pointerType, second, moved: false } : null;
  }

  /** mouseDown: a mouse's click count is the system's (its double-click time and distance): the second click of a double is the one it says. */
  mouseDown(e) {
    const tp = this.tapping, last = this.lastTap;
    if (!tp || tp.type !== "mouse" || e.button !== 0) return;
    const second = !!(last && last.n === tp.n && e.detail >= 2);
    if (second === tp.second) return;
    tp.second = second;
    if (second) clearTimeout(last.pending?.timer);
    else {
      this.resumePress();
      this.lastTap = null;
    }
  }

  /** tapUp ends the tap under way: what it was ({n, second}), or null when it wasn't a tap; a second tap is a double tap. */
  tapUp(e) {
    const tp = this.tapping;
    if (!tp || tp.id !== e.pointerId) return null;
    this.tapping = null;
    if (tp.moved || e.timeStamp - tp.t0 > longPressMs) {
      if (tp.second) this.resumePress();
      this.lastTap = null;
      return null;
    }
    if (tp.second) {
      if (this.lastTap) this.lastTap.pending = null; // a double tap: no Press
      this.lastTap = null;
      this.doubleTapped(tp.n, tp.x0, tp.y0);
      return { n: tp.n, second: true };
    }
    this.lastTap = { n: tp.n, t: e.timeStamp, x: e.clientX, y: e.clientY, type: tp.type, pending: null };
    return { n: tp.n, second: false };
  }

  tapMoved(e) {
    const tp = this.tapping;
    if (tp && tp.id === e.pointerId && !tp.moved && Math.hypot(e.clientX - tp.x0, e.clientY - tp.y0) > slop) tp.moved = true;
  }

  /** resumePress sends the first tap's Press that waited for a second tap that didn't make a double tap. */
  resumePress() {
    const p = this.lastTap?.pending;
    if (!p) return;
    this.lastTap.pending = null;
    clearTimeout(p.timer);
    this.h.emit(p.n, Event.Press, Events.press);
  }

  doubleTapped(n, x, y) {
    if (n.listening(Event.DoubleTap)) {
      const r = n.el.getBoundingClientRect();
      this.h.dispatch(Events.doubleTap(n.id, x - r.left, y - r.top));
    } else if (n.zoom?.on) n.zoom.doubleTap(x, y);
  }

  /** firePress presses n, at once, or once a second tap can't come when n hears of double taps too (none for a double tap's taps). */
  firePress(n, tap) {
    const last = this.lastTap;
    if (tap && tap.n === n && n.listening(Event.DoubleTap)) {
      if (tap.second || !last) return;
      const pending = { n, timer: 0 };
      pending.timer = setTimeout(() => {
        if (last.pending !== pending) return;
        last.pending = null;
        this.h.emit(n, Event.Press, Events.press);
      }, doubleTime(last.type));
      last.pending = pending;
      return;
    }
    this.h.emit(n, Event.Press, Events.press);
  }

  /** forget: a view is going; what it was in leaves, so what follows it doesn't stay stuck. */
  forget(n) {
    if (n === this.press?.n) this.endPress(false);
    if (n.hovered) { n.hovered = false; this.h.dispatch(Events.hover(n.id, false)); }
    if (this.gesture?.n === n) this.gesture = null;
    if (this.tapping?.n === n) this.tapping = null;
    if (this.lastTap?.n === n) {
      clearTimeout(this.lastTap.pending?.timer);
      this.lastTap = null;
    }
  }

  // --- Finding the view a pointer is for ---

  nodeOf(el) {
    for (; el && el !== this.h.container; el = el.parentElement) {
      if (el.asukaNode) return el.asukaNode;
    }
    return null;
  }

  /** pressTarget is the innermost pressable view at el, or null where a control (a button, a field) takes the press. */
  pressTarget(el) {
    for (; el && el !== this.h.container; el = el.parentElement) {
      if (el.matches(controls)) return null;
      const n = el.asukaNode;
      if (n && (n.listening(Event.Press) || n.listening(Event.LongPress) || n.listening(Event.Pressed) || n.menu)) return n;
    }
    return null;
  }

  gestureTarget(el, kinds) {
    for (; el && el !== this.h.container; el = el.parentElement) {
      const n = el.asukaNode;
      if (n && kinds.some((k) => n.listening(k))) return n;
    }
    return null;
  }

  inScroll(el) {
    for (; el && el !== this.h.container; el = el.parentElement) {
      if (el.classList.contains("asuka-scroller")) return true;
    }
    return false;
  }

  // --- Pointers ---

  down(e) {
    this.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (this.pointers.size === 2) this.twoFingers(e);
    if (!e.isPrimary || (e.pointerType === "mouse" && e.button !== 0)) return;
    this.pressPoint(e);
    this.tapDown(e);
    const n = this.pressTarget(e.target);
    const g = this.gestureTarget(e.target, [Event.Pan, Event.Swipe]);
    // A touch's pan with the scroll comes as touch events (touchPans).
    if (g && !(g.panSimultaneous && e.pointerType === "touch")) this.gesture = this.panGesture(g, e.pointerId, e.clientX, e.clientY, e.timeStamp);
    if (!n) return;
    const p = { n, id: e.pointerId, x0: e.clientX, y0: e.clientY, t0: performance.now(), shown: false, timer: 0, long: 0, inside: true, type: e.pointerType };
    this.press = p;
    if (n.listening(Event.Pressed)) {
      if (this.inScroll(n.el.parentElement)) p.timer = setTimeout(() => this.showPressed(p, true), tapDelayMs);
      else this.showPressed(p, true);
    }
    if (n.listening(Event.LongPress) || (n.menu && n.menuTrigger === 1)) {
      p.long = setTimeout(() => this.longPress(p), longPressMs);
    }
  }

  /** pressPoint tells the innermost view listening for it where a press began on it, in points from its top left: before its Pressed and Press. */
  pressPoint(e) {
    const n = this.gestureTarget(e.target, [Event.PressPoint]);
    if (!n) return;
    const r = n.el.getBoundingClientRect();
    this.h.dispatch(Events.pressPoint(n.id, e.clientX - r.left, e.clientY - r.top));
  }

  showPressed(p, on) {
    if (on === p.shown) return;
    p.shown = on;
    p.shownAt = performance.now();
    this.h.dispatch(Events.pressed(p.n.id, on));
  }

  longPress(p) {
    if (this.press !== p) return;
    this.endPress(false);
    const n = p.n;
    if (n.menu && n.menuTrigger === 1) this.h.menus.open(n, p.x0, p.y0);
    else this.h.emit(n, Event.LongPress, Events.longPress);
  }

  move(e) {
    if (this.pointers.has(e.pointerId)) this.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    this.tapMoved(e);
    const g = this.gesture;
    if (g && g.kind === "two") { this.twoMoved(e); return; }
    const p = this.press;
    if (p && p.id === e.pointerId) {
      const moved = Math.hypot(e.clientX - p.x0, e.clientY - p.y0) > slop;
      if (moved && (this.inScroll(p.n.el.parentElement) || g)) {
        this.endPress(false); // a scroll or a pan takes the touch
      } else if (moved) {
        clearTimeout(p.long);
        p.long = 0;
        const r = p.n.el.getBoundingClientRect();
        const m = p.type === "touch" ? 20 : 0;
        p.inside = e.clientX >= r.left - m && e.clientX <= r.right + m && e.clientY >= r.top - m && e.clientY <= r.bottom + m;
        if (p.n.listening(Event.Pressed) && !p.timer) this.showPressed(p, p.inside);
      }
    }
    if (g && g.kind === "pan" && g.id === e.pointerId && !this.panStep(g, e, e.timeStamp)) this.gesture = null;
  }

  pan(g, e, state) {
    if (!g.n.listening(Event.Pan)) return;
    const r = g.n.el.getBoundingClientRect();
    const [vx, vy] = velocity(g.samples);
    const s = gesture();
    Object.assign(s, { state, x: e.clientX - r.left, y: e.clientY - r.top, translationX: e.clientX - g.x0, translationY: e.clientY - g.y0, velocityX: vx, velocityY: vy });
    this.h.dispatch(Events.pan(g.n.id, s));
  }

  up(e) {
    this.pointers.delete(e.pointerId);
    const tap = this.tapUp(e);
    const g = this.gesture;
    if (g && g.kind === "two") {
      if (this.pointers.size < 2) this.twoEnded(2);
      return;
    }
    if (g && g.kind === "pan" && g.id === e.pointerId) {
      this.gesture = null;
      if (g.began) {
        this.pan(g, e, 2);
        this.swipe(g, e);
      }
    }
    const p = this.press;
    if (!p || p.id !== e.pointerId) return;
    this.press = null;
    clearTimeout(p.long);
    const fire = p.inside;
    if (p.timer) {
      // Released before the tap delay: shown pressed for a moment anyway.
      clearTimeout(p.timer);
      if (fire && p.n.listening(Event.Pressed)) {
        this.showPressed(p, true);
        setTimeout(() => this.showPressed(p, false), pressedMinMs);
      }
    } else if (p.shown) {
      const left = pressedMinMs - (performance.now() - p.shownAt);
      if (left > 0) setTimeout(() => this.showPressed(p, false), left);
      else this.showPressed(p, false);
    }
    if (!fire) return;
    if (p.n.menu && p.n.menuTrigger !== 1) {
      // On the click that follows the release, as a Button's menu: the
      // browser's light dismiss comes before it, at the release's mouseup
      // (after a tap, in a later task than the touchend), and would close
      // a menu shown earlier. The timer opens it anyway after menuClickMs
      // if no click comes, so the listener never waits for a later,
      // unrelated click.
      const n = p.n, x = e.clientX, y = e.clientY;
      const open = () => {
        clearTimeout(t);
        removeEventListener("click", open, true);
        this.h.menus.open(n, x, y);
      };
      const t = setTimeout(open, menuClickMs);
      addEventListener("click", open, true);
    } else this.firePress(p.n, tap);
  }

  swipe(g, e) {
    if (!g.n.listening(Event.Swipe)) return;
    const dx = e.clientX - g.x0, dy = e.clientY - g.y0;
    const [vx, vy] = velocity(g.samples);
    let dir = 0;
    if (Math.abs(dx) > Math.abs(dy) && Math.abs(dx) > 30 && Math.abs(vx) > 300) dir = dx > 0 ? 1 : 2;
    else if (Math.abs(dy) > 30 && Math.abs(vy) > 300) dir = dy > 0 ? 8 : 4;
    if (dir) this.h.dispatch(Events.swipe(g.n.id, dir));
  }

  cancel(e) {
    this.pointers.delete(e.pointerId);
    if (this.tapping?.id === e.pointerId) {
      if (this.tapping.second) this.resumePress();
      this.tapping = null;
    }
    if (this.press?.id === e.pointerId) this.endPress(false);
    const g = this.gesture;
    if (g?.kind === "pan" && g.id === e.pointerId) {
      this.gesture = null;
      if (g.began) this.pan(g, e, 3);
    }
    if (g?.kind === "two") this.twoEnded(3);
  }

  cancelAll() {
    if (this.press) this.endPress(false);
    this.tapping = null;
    this.pointers.clear();
  }

  scrolled() {
    // A scroll takes the touch: the press doesn't fire.
    if (this.press && this.press.type !== "mouse") this.endPress(false);
    if (this.tapping && this.tapping.type !== "mouse") this.tapping.moved = true;
  }

  endPress() {
    const p = this.press;
    if (!p) return;
    this.press = null;
    clearTimeout(p.timer);
    clearTimeout(p.long);
    if (p.shown) this.showPressed(p, false);
  }

  context(e) {
    const n = this.pressTarget(e.target) || this.gestureTarget(e.target, [Event.LongPress]);
    if (!n) return;
    if (n.menu && n.menuTrigger === 1) {
      e.preventDefault();
      this.endPress();
      this.h.menus.open(n, e.clientX, e.clientY);
    } else if (n.listening(Event.LongPress) && e.pointerType !== "touch") {
      // A secondary click is a long press with a mouse, as on a Mac.
      e.preventDefault();
      this.endPress();
      this.h.emit(n, Event.LongPress, Events.longPress);
    } else if (n.listening(Event.LongPress)) {
      e.preventDefault(); // the long press timer has it
    }
  }

  key(e) {
    const n = e.target.asukaNode;
    if (!n || e.target !== n.el) return;
    if (n.adjustable && (e.key === "ArrowUp" || e.key === "ArrowRight" || e.key === "ArrowDown" || e.key === "ArrowLeft")) {
      e.preventDefault();
      const up = e.key === "ArrowUp" || e.key === "ArrowRight";
      this.h.dispatch(Events.adjust(n.id, up ? 1 : -1));
      return;
    }
    if ((e.key === "Enter" || e.key === " ") && (n.listening(Event.Press) || n.menu)) {
      e.preventDefault();
      if (n.menu && n.menuTrigger !== 1) {
        const r = n.el.getBoundingClientRect();
        this.h.menus.open(n, r.left, r.bottom);
      } else {
        this.h.emit(n, Event.Press, Events.press);
      }
    }
  }

  // --- Pinches and rotations ---

  twoFingers(e) {
    if (this.tapping?.second) this.resumePress();
    this.tapping = null;
    const n = this.gestureTarget(e.target, [Event.Pinch, Event.Rotate]);
    if (!n) return;
    // A zoomable Scroll inside it pinches itself (zoom.js).
    const z = e.target.closest?.(".asuka-zoomable");
    if (z && z !== n.el && n.el.contains(z)) return;
    this.endPress();
    if (this.gesture?.kind === "pan" && this.gesture.began) this.pan(this.gesture, e, 3);
    const [a, b] = [...this.pointers.values()];
    this.gesture = { kind: "two", n, d0: Math.hypot(b.x - a.x, b.y - a.y) || 1, a0: Math.atan2(b.y - a.y, b.x - a.x), began: false, scale: 1, rot: 0 };
  }

  twoMoved() {
    const g = this.gesture;
    if (this.pointers.size < 2) return;
    const [a, b] = [...this.pointers.values()];
    g.scale = Math.hypot(b.x - a.x, b.y - a.y) / g.d0;
    g.rot = Math.atan2(b.y - a.y, b.x - a.x) - g.a0;
    g.cx = (a.x + b.x) / 2;
    g.cy = (a.y + b.y) / 2;
    this.two(g, g.began ? 1 : 0);
    g.began = true;
  }

  two(g, state) {
    const r = g.n.el.getBoundingClientRect();
    const s = gesture();
    Object.assign(s, { state, x: (g.cx ?? r.left) - r.left, y: (g.cy ?? r.top) - r.top, scale: g.scale, rotation: g.rot });
    if (g.n.listening(Event.Pinch)) this.h.dispatch(Events.pinch(g.n.id, s));
    if (g.n.listening(Event.Rotate)) this.h.dispatch(Events.rotate(g.n.id, s));
  }

  twoEnded(state) {
    const g = this.gesture;
    this.gesture = null;
    if (g?.began) this.two(g, state);
  }

  /** wheelPinch: a trackpad's pinch comes as a wheel with ctrl; it pinches the view, not the page. */
  wheelPinch(e) {
    if (!e.ctrlKey) return;
    const n = this.gestureTarget(e.target, [Event.Pinch]);
    if (!n) return;
    e.preventDefault();
    let w = this.wheel;
    if (!w || w.n !== n) {
      w = this.wheel = { kind: "two", n, scale: 1, rot: 0, began: false };
    }
    w.scale *= Math.exp(-e.deltaY / 100);
    w.cx = e.clientX;
    w.cy = e.clientY;
    this.two(w, w.began ? 1 : 0);
    w.began = true;
    clearTimeout(w.timer);
    w.timer = setTimeout(() => { this.two(w, 2); this.wheel = null; }, 150);
  }

  // --- A screen reader's ---

  setActions(n, b) {
    // The web has no custom actions for screen readers: they stay the
    // view's context menu's, which the core gives it too.
    n.a11yActions = b;
  }

  setAdjustable(n, on) {
    n.adjustable = !!on;
    if (on) {
      n.el.setAttribute("role", "slider");
      if (!n.el.hasAttribute("tabindex")) n.el.tabIndex = 0;
    }
  }

  // --- Drag and drop ---

  dragStart(e) {
    const n = this.nodeOf(e.target);
    if (!n || !n.dragItems) return;
    this.endPress();
    const dt = e.dataTransfer;
    let onlyValues = true;
    for (const it of n.dragItems) {
      if (it.value) continue;
      onlyValues = false;
      if (it.kind === 1) {
        dt.setData("text/uri-list", it.text);
        dt.setData("text/plain", it.text);
      } else if (it.kind === 0) {
        dt.setData("text/plain", it.text);
      } else if (it.path) {
        const f = fs.fileOf(it.path, it.name, it.mime);
        if (f && dt.items) dt.items.add(f);
      }
    }
    dt.setData("application/x-asuka-source", String(n.id));
    for (const it of n.dragItems) if (it.mime) dt.setData("application/x-asuka-type." + it.mime.toLowerCase(), "1");
    dt.effectAllowed = onlyValues ? "move" : "copyMove";
    const r = n.el.getBoundingClientRect();
    dt.setDragImage(n.el, e.clientX - r.left, e.clientY - r.top);
    this.dragSource = n;
    this.h.dispatch(Events.dragging(n.id, 0));
  }

  dragEnd(e) {
    const n = this.dragSource;
    this.dragSource = null;
    if (n) this.h.dispatch(Events.dragging(n.id, e.dataTransfer.dropEffect !== "none" ? 1 : 2));
  }

  /** dropTarget is the innermost view under the drag that takes it. */
  dropTarget(e) {
    const types = dragTypes(e.dataTransfer);
    for (let el = e.target; el && el !== this.h.container; el = el.parentElement) {
      const n = el.asukaNode;
      if (n && n.dropAccept && types.some((t) => matchMIME(n.dropAccept, t))) return n;
    }
    return null;
  }

  dragOver(e) {
    const n = this.dropTarget(e);
    if (this.over && this.over !== n) this.overState(this.over, 3, e);
    if (!n) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = e.dataTransfer.types.includes("application/x-asuka-source") ? "move" : "copy";
    this.overState(n, this.over === n ? 1 : 0, e);
    this.over = n;
  }

  overState(n, state, e) {
    const r = n.el.getBoundingClientRect();
    const g = gesture();
    Object.assign(g, { state, x: e.clientX - r.left, y: e.clientY - r.top });
    this.h.emit(n, Event.DragOver, (id) => Events.dragOver(id, g));
    if (state === 3) this.over = null;
  }

  dragLeave(e) {
    if (this.over && !this.over.el.contains(e.relatedTarget)) this.overState(this.over, 3, e);
  }

  async drop(e) {
    const n = this.dropTarget(e);
    if (!n) return;
    e.preventDefault();
    this.overState(n, 3, e);
    const r = n.el.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    const dt = e.dataTransfer;
    const source = Number(dt.getData("application/x-asuka-source")) || 0;
    const items = [];
    if (!source) {
      for (const f of [...dt.files]) {
        if (!matchMIME(n.dropAccept, f.type || "application/octet-stream")) continue;
        items.push({ kind: shareKind(f.type), text: "", path: await fs.saveUpload(f), name: f.name, mime: f.type || "application/octet-stream", size: f.size });
      }
      const uri = dt.getData("text/uri-list");
      if (uri && matchMIME(n.dropAccept, "text/uri-list")) {
        for (const u of uri.split(/\r?\n/)) if (u && !u.startsWith("#")) items.push({ kind: 1, text: u, path: "", name: "", mime: "text/uri-list", size: -1 });
      } else {
        const t = dt.getData("text/plain");
        if (t && matchMIME(n.dropAccept, "text/plain")) items.push({ kind: 0, text: t, path: "", name: "", mime: "text/plain", size: -1 });
      }
    }
    const w = new Writer().f32(x).f32(y).u32(source).u8(source ? 0 : 1);
    appendItems(w, items);
    this.h.emit(n, Event.Drop, (id) => Events.drop(id, w.done()));
  }
}

/** appendItems writes items as a Shared event carries them. */
export function appendItems(w, items) {
  w.u32(items.length);
  for (const it of items) w.u8(it.kind).string(it.text || "").string(it.path || "").string(it.name || "").string(it.mime || "").i64(it.size ?? -1);
  return w;
}

/** decodeDragItems reads a view's DragItems. */
export function decodeDragItems(b) {
  if (!b || b.length < 4) return null;
  const r = new Reader(b);
  const n = r.u32();
  const out = [];
  for (let i = 0; i < n && !r.atEnd; i++) out.push({ kind: r.u8(), text: r.string(), path: r.string(), name: r.string(), mime: r.string(), value: (r.u8() & 1) !== 0 });
  return out;
}

export function shareKind(mime) {
  if (/^image\//.test(mime)) return 2;
  if (/^video\//.test(mime)) return 3;
  if (/^audio\//.test(mime)) return 4;
  return 5;
}

function dragTypes(dt) {
  const out = [];
  for (const t of dt.types) {
    if (t === "Files") {
      for (const it of dt.items || []) if (it.kind === "file") out.push(it.type || "application/octet-stream");
      if (!dt.items) out.push("application/octet-stream");
    } else if (t.startsWith("application/x-asuka-type.")) {
      out.push(t.slice("application/x-asuka-type.".length));
    } else if (t !== "application/x-asuka-source") {
      out.push(t);
    }
  }
  return out;
}

/** matchMIME is ops.MatchMIME: exact, type/*, * / * or *, without regard to case or parameters. */
export function matchMIME(patterns, mime) {
  const m = mime.split(";")[0].trim().toLowerCase();
  for (let p of patterns) {
    p = p.split(";")[0].trim().toLowerCase();
    if (p === "*" || p === "*/*") return true;
    if (p.endsWith("/*") ? m.startsWith(p.slice(0, -1)) && m.length > p.length - 1 : p === m) return true;
  }
  return false;
}

/** velocity is a pointer's, in points per second, from its last samples. */
function velocity(samples) {
  if (samples.length < 2) return [0, 0];
  const [t0, x0, y0] = samples[0];
  const [t1, x1, y1] = samples[samples.length - 1];
  const dt = (t1 - t0) / 1000;
  if (dt <= 0) return [0, 0];
  return [(x1 - x0) / dt, (y1 - y0) / dt];
}
