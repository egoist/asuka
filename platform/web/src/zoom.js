// A zoomable Scroll (MaxZoom more than MinZoom): its content, which the
// core lays out at zoom 1, is scaled in a layer of the host's (a CSS
// scale from its top left, which the core's frames, set on the content
// inside it, never undo), and a sizer makes the scroll extent the scaled
// content's. Two fingers pinch it around their middle, ctrl (⌘ on a Mac)
// and the wheel, a trackpad's pinch as browsers send it, zoom it around
// the pointer, and a double tap (interaction.js finds it) zooms in to
// twice MinZoom there or back out. The browser's own touch handling is
// off on it (touch-action: none: the page's pinch zoom would take the
// fingers), so one finger scrolls it here, flung as it's let go. Zoomed
// reports the user's zooming; ZoomTo's isn't.

import { Event, Events } from "./ops.js";
import { zoomScroll, rubberZoom, clampZoom, doubleTapZoom } from "./gestures.js";
import { apple } from "./keys.js";

const slop = 10; // points a finger moves before it scrolls
const animMs = 250;
const decel = 0.998; // a fling's speed kept per ms (UIScrollView's normal deceleration)

export class Zoom {
  constructor(h, n) {
    this.h = h;
    this.n = n;
    this.min = 1;
    this.max = 1;
    this.z = 1;
    this.on = false;
    this.layer = null;
    this.sizer = null;
    this.touches = new Map(); // pointerId -> {x, y}
    this.pinch = null;
    this.drag = null; // one finger scrolling
    this.fling = 0;
    this.anim = 0;
    const el = n.el;
    el.addEventListener("pointerdown", (e) => this.down(e));
    el.addEventListener("pointermove", (e) => this.move(e));
    el.addEventListener("pointerup", (e) => this.up(e));
    el.addEventListener("pointercancel", (e) => this.up(e));
    el.addEventListener("wheel", (e) => this.wheel(e), { passive: false });
  }

  /** bounds takes MinZoom and MaxZoom: it zooms when max is more than min; the shown zoom is kept within them, silently. */
  bounds(min, max) {
    this.min = min > 0 ? min : 1;
    this.max = max > 0 ? max : 1;
    const on = this.max > this.min;
    if (on && !this.on) this.enable();
    else if (!on && this.on) this.disable();
    if (on) this.apply(clampZoom(this.z, this.min, this.max));
  }

  enable() {
    const n = this.n, el = n.el;
    this.on = true;
    this.z = 1;
    this.sizer = document.createElement("div");
    this.sizer.className = "asuka-zoom-sizer";
    this.layer = document.createElement("div");
    this.layer.className = "asuka-zoom-layer";
    el.prepend(this.sizer, this.layer);
    for (const c of n.children) if (c.el && c.el.parentNode === el) this.layer.appendChild(c.el);
    n.box = this.layer;
    el.classList.add("asuka-zoomable");
  }

  disable() {
    const n = this.n, el = n.el;
    this.stop();
    for (const c of n.children) if (c.el && c.el.parentNode === this.layer) el.insertBefore(c.el, this.layer);
    this.layer.remove();
    this.sizer.remove();
    this.layer = this.sizer = null;
    n.box = el;
    el.classList.remove("asuka-zoomable");
    this.on = false;
    this.z = 1;
  }

  /** apply shows zoom z: the layer's scale, and the scroll extent of the content scaled. */
  apply(z) {
    this.z = z;
    if (!this.layer) return;
    this.layer.style.transform = z === 1 ? "" : `scale(${z})`;
    this.sized();
  }

  /** sized makes the scroll extent the scaled content's (as its frame changes too). */
  sized() {
    if (!this.sizer) return;
    const f = this.n.children[0]?.frame;
    const w = f ? f[0] + f[2] : 0, hh = f ? f[1] + f[3] : 0;
    this.sizer.style.width = Math.max(w * this.z, 1) + "px";
    this.sizer.style.height = Math.max(hh * this.z, 1) + "px";
  }

  /** zoomAt zooms to z keeping the content at px, py (from the scroller's top left) where it is; the user's is reported. */
  zoomAt(z, px, py, user) {
    const el = this.n.el;
    const [sx, sy] = zoomScroll(el.scrollLeft, el.scrollTop, px, py, this.z, z);
    this.apply(z);
    el.scrollLeft = sx;
    el.scrollTop = sy;
    if (user) this.h.emit(this.n, Event.Zoomed, (id) => Events.zoomed(id, z));
  }

  /** animate zooms to z around px, py over a moment (at once: not animated). */
  animate(z, px, py, user, animated = true) {
    cancelAnimationFrame(this.anim);
    this.anim = 0;
    const z0 = this.z;
    if (!animated || z0 === z) { this.zoomAt(z, px, py, user); return; }
    const t0 = performance.now();
    const step = (t) => {
      const k = Math.min((t - t0) / animMs, 1);
      const e = 1 - Math.pow(1 - k, 3);
      this.zoomAt(k >= 1 ? z : z0 + (z - z0) * e, px, py, user);
      this.anim = k < 1 ? requestAnimationFrame(step) : 0;
    };
    this.anim = requestAnimationFrame(step);
  }

  /** zoomTo is ZoomTo's: around the middle of what it shows, not reported. */
  zoomTo(z, animated) {
    if (!this.on) return;
    const el = this.n.el;
    this.stop();
    this.animate(clampZoom(z, this.min, this.max), el.clientWidth / 2, el.clientHeight / 2, false, animated);
  }

  /** doubleTap zooms in at the point (client coordinates) a double tap fell, or back out. */
  doubleTap(x, y) {
    const r = this.n.el.getBoundingClientRect();
    this.stop();
    this.animate(doubleTapZoom(this.z, this.min, this.max), x - r.left, y - r.top, true);
  }

  stop() {
    cancelAnimationFrame(this.fling);
    cancelAnimationFrame(this.anim);
    this.fling = this.anim = 0;
  }

  // --- The wheel: ctrl (⌘ on a Mac) and the wheel, a trackpad's pinch ---

  wheel(e) {
    if (!this.on || !(e.ctrlKey || (apple() && e.metaKey))) return;
    // A view inside that pinches itself takes its own.
    const inner = this.h.interaction.gestureTarget(e.target, [Event.Pinch]);
    if (inner && inner !== this.n && this.n.el.contains(inner.el)) return;
    e.preventDefault();
    e.stopPropagation();
    this.stop();
    const dy = e.deltaY * (e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 100 : 1);
    const z = clampZoom(this.z * Math.exp(-dy / 100), this.min, this.max);
    if (z === this.z) return;
    const r = this.n.el.getBoundingClientRect();
    this.zoomAt(z, e.clientX - r.left, e.clientY - r.top, true);
  }

  // --- Fingers: one scrolls, two pinch ---

  down(e) {
    if (!this.on || e.pointerType !== "touch") return;
    this.stop();
    this.touches.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (this.touches.size === 1) {
      this.drag = { id: e.pointerId, x0: e.clientX, y0: e.clientY, x: e.clientX, y: e.clientY, moved: false, samples: [[e.timeStamp, e.clientX, e.clientY]] };
    } else if (this.touches.size === 2) {
      this.drag = null;
      this.h.interaction.takeTouch();
      const [a, b] = this.touches.values();
      const r = this.n.el.getBoundingClientRect(), el = this.n.el;
      const mx = (a.x + b.x) / 2 - r.left, my = (a.y + b.y) / 2 - r.top;
      this.pinch = { d0: Math.hypot(b.x - a.x, b.y - a.y) || 1, z0: this.z, cx: (el.scrollLeft + mx) / this.z, cy: (el.scrollTop + my) / this.z, px: mx, py: my };
    }
  }

  move(e) {
    const t = this.touches.get(e.pointerId);
    if (!t) return;
    t.x = e.clientX;
    t.y = e.clientY;
    const el = this.n.el;
    const p = this.pinch;
    if (p) {
      if (this.touches.size < 2) return;
      const [a, b] = this.touches.values();
      const r = el.getBoundingClientRect();
      const z = rubberZoom(p.z0 * Math.hypot(b.x - a.x, b.y - a.y) / p.d0, this.min, this.max);
      p.px = (a.x + b.x) / 2 - r.left;
      p.py = (a.y + b.y) / 2 - r.top;
      // The content point that was under the fingers' middle stays under it, as it zooms and moves.
      this.apply(z);
      el.scrollLeft = p.cx * z - p.px;
      el.scrollTop = p.cy * z - p.py;
      this.h.emit(this.n, Event.Zoomed, (id) => Events.zoomed(id, z));
      return;
    }
    const d = this.drag;
    if (!d || d.id !== e.pointerId) return;
    if (!d.moved) {
      if (Math.hypot(e.clientX - d.x0, e.clientY - d.y0) <= slop) return;
      d.moved = true;
    } else {
      el.scrollLeft -= e.clientX - d.x;
      el.scrollTop -= e.clientY - d.y;
    }
    d.x = e.clientX;
    d.y = e.clientY;
    d.samples.push([e.timeStamp, e.clientX, e.clientY]);
    if (d.samples.length > 6) d.samples.shift();
  }

  up(e) {
    if (!this.touches.delete(e.pointerId)) return;
    const p = this.pinch;
    if (p) {
      if (this.touches.size >= 2) return;
      this.pinch = null;
      // Settled back within MinZoom and MaxZoom.
      const z = clampZoom(this.z, this.min, this.max);
      if (z !== this.z) this.animate(z, p.px, p.py, true);
      // The finger left scrolls on.
      for (const [id, t] of this.touches) this.drag = { id, x0: t.x, y0: t.y, x: t.x, y: t.y, moved: true, samples: [[e.timeStamp, t.x, t.y]] };
      return;
    }
    const d = this.drag;
    if (!d || d.id !== e.pointerId) return;
    this.drag = null;
    if (d.moved && e.type === "pointerup") this.flingFrom(d.samples);
  }

  flingFrom(samples) {
    if (samples.length < 2) return;
    const [t0, x0, y0] = samples[0], [t1, x1, y1] = samples[samples.length - 1];
    const dt = t1 - t0;
    if (dt <= 0) return;
    let vx = (x1 - x0) / dt, vy = (y1 - y0) / dt; // points per ms
    const el = this.n.el;
    let last = performance.now();
    const step = (t) => {
      const ms = Math.max(t - last, 0);
      last = t;
      el.scrollLeft -= vx * ms;
      el.scrollTop -= vy * ms;
      const k = Math.pow(decel, ms);
      vx *= k;
      vy *= k;
      this.fling = Math.hypot(vx, vy) > 0.02 ? requestAnimationFrame(step) : 0;
    };
    this.fling = requestAnimationFrame(step);
  }
}
