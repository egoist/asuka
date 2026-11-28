// Scroll and List. A Scroll is the browser's scrolling element, its one
// child the content (the content's frame is its size). A List is
// core-virtualized, as on the desktop hosts: a scrolling element whose
// rows the host places itself. As it scrolls, rows coming into view take
// a row view (a recycled one, or a new one) and its cells, host-made
// views the core binds rows into through ListBind; rows going out are
// kept for reuse. A row's height is its cells' (a grid's row holds
// Columns of them), from their frames; rows not yet bound are estimated.

import { Event, Events, Command, Reader, KindProp as P, Prop } from "./ops.js";
import { own } from "./views.js";
import { decodeActions } from "./host.js";
import { apple } from "./keys.js";
import { Zoom } from "./zoom.js";
import { reorderTarget, shiftOf, moveRow, moveID, decodeReorder } from "./gestures.js";

const longPressMs = 500; // a touch held this long on a row begins selecting (Android's), or lifts it (Reorder)
const pressSlop = 10; // points a touch moves before it's a scroll, not a long press
const dragSlop = 5; // points the pointer moves on a row before it lifts it (a desktop's drag threshold)
const edgeZone = 48; // points from the list's top or bottom where a lifted row scrolls it
const edgeSpeed = 14; // points a frame it scrolls at most
const settleMs = 220; // how long rows take to settle after a move

const KindRow = 0xfff0, KindCell = 0xfff1;
const cardGap = 0; // an inset grouped list's rows touch, as one card

// --- Scroll ---

export const scrollKind = {
  create(h, n) {
    const el = own(n, document.createElement("div"));
    el.className = "asuka-scroller asuka-vertical";
    n.horizontal = false;
    n.paging = false;
    wireScroll(h, n, el);
  },
  prop(h, n, p, v) {
    switch (p) {
      case P.Scroll.Horizontal:
        n.horizontal = !!v;
        n.el.classList.toggle("asuka-horizontal", !!v);
        n.el.classList.toggle("asuka-vertical", !v);
        break;
      case P.Scroll.Paging:
        n.paging = !!v;
        n.el.classList.toggle("asuka-paging", !!v);
        pages(n);
        break;
      case P.Scroll.Refreshable:
        refreshable(h, n, !!v);
        break;
      case P.Scroll.Refreshing:
        refreshing(n, !!v);
        break;
      case P.Scroll.MinZoom:
      case P.Scroll.MaxZoom:
        if (p === P.Scroll.MinZoom) n.minZoom = v; else n.maxZoom = v;
        if (!n.zoom && (n.maxZoom ?? 1) <= (n.minZoom ?? 1)) break;
        n.zoom ??= new Zoom(h, n);
        n.zoom.bounds(n.minZoom ?? 1, n.maxZoom ?? 1);
        break;
    }
  },
  insert(h, p, c, i) {
    h.placeChild(p, c, i);
    return true;
  },
  childFrame(h, p, c, x, y, w, hh) {
    // The content's frame is at zoom 1, inside the zoom's layer: the scroll extent follows it scaled.
    h.place(c, x, y, w, hh);
    if (p.zoom?.on) p.zoom.sized();
    return true;
  },
  frame(h, n, x, y, w, hh) {
    h.place(n, x, y, w, hh);
    pages(n);
    return true;
  },
  command(h, n, cmd, args) {
    if (cmd === Command.ScrollToOffset) {
      const r = new Reader(args);
      const x = r.f32(), y = r.f32(), animated = r.u8() !== 0;
      n.el.scrollTo({ left: x, top: y, behavior: animated ? "smooth" : "instant" });
      return true;
    }
    if (cmd === Command.ZoomTo) {
      const r = new Reader(args);
      const z = r.f32(), animated = r.u8() !== 0;
      n.zoom?.zoomTo(z, animated);
      return true;
    }
    return false;
  },
  measure() { return [0, 0]; },
};

/** wireScroll reports a scrolling element's offset (Scrolled), and a paging one's page (Paged). */
function wireScroll(h, n, el) {
  let queued = false;
  el.addEventListener("scroll", () => {
    if (n.list) n.list.scrolled();
    if (queued || !n.listening(Event.Scrolled)) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      h.dispatch(Events.scrolled(n.id, el.scrollLeft, el.scrollTop));
    });
  }, { passive: true });
  el.addEventListener("scrollend", () => {
    if (!n.paging || !n.listening(Event.Paged)) return;
    const size = n.horizontal ? el.clientWidth : el.clientHeight;
    const page = Math.round((n.horizontal ? el.scrollLeft : el.scrollTop) / (size || 1));
    if (page !== n.page) {
      n.page = page;
      h.dispatch(Events.paged(n.id, page));
    }
  });
}

/** pages puts a snap point at each page of a paging Scroll. */
function pages(n) {
  n.snaps?.forEach((s) => s.remove());
  n.snaps = null;
  if (!n.paging) return;
  const content = n.children[0];
  const size = n.horizontal ? n.frame[2] : n.frame[3];
  if (!content || size <= 0) return;
  const total = n.horizontal ? content.frame[2] : content.frame[3];
  const count = Math.max(1, Math.round(total / size));
  n.snaps = [];
  for (let i = 0; i < count; i++) {
    const s = document.createElement("div");
    s.className = "asuka-snap";
    s.style[n.horizontal ? "left" : "top"] = i * size + "px";
    s.style.width = (n.horizontal ? size : 1) + "px";
    s.style.height = (n.horizontal ? 1 : size) + "px";
    n.el.appendChild(s);
    n.snaps.push(s);
  }
}

// Pull to refresh: a touch pulling the content down from its top, past
// the threshold, sends Refresh; the indicator shows while Refreshing.
function refreshable(h, n, on) {
  if (!on) {
    n.refresher?.remove();
    n.refresher = null;
    return;
  }
  if (n.refresher) return;
  const r = document.createElement("div");
  r.className = "asuka-refresher";
  r.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-dasharray="42 100"/></svg>';
  n.refresher = r;
  n.el.appendChild(r);
  let y0 = null, pull = 0;
  n.el.addEventListener("touchstart", (e) => { y0 = n.el.scrollTop <= 0 ? e.touches[0].clientY : null; pull = 0; }, { passive: true });
  n.el.addEventListener("touchmove", (e) => {
    if (y0 == null || n.refreshing) return;
    pull = Math.max(0, e.touches[0].clientY - y0);
    r.style.opacity = String(Math.min(pull / 70, 1));
    r.style.transform = `translateY(${Math.min(pull, 90) / 2}px) rotate(${pull * 3}deg)`;
  }, { passive: true });
  n.el.addEventListener("touchend", () => {
    if (y0 == null) return;
    y0 = null;
    if (pull > 70 && !n.refreshing) h.emit(n, Event.Refresh, Events.refresh);
    if (!n.refreshing) { r.style.opacity = ""; r.style.transform = ""; }
  });
}

function refreshing(n, on) {
  n.refreshing = on;
  if (!n.refresher) return;
  n.refresher.classList.toggle("asuka-refreshing", on);
  if (!on) { n.refresher.style.opacity = ""; n.refresher.style.transform = ""; }
}

// --- List ---

export const listKind = {
  create(h, n) {
    const el = own(n, document.createElement("div"));
    el.className = "asuka-scroller asuka-vertical asuka-list";
    el.tabIndex = -1;
    n.list = new List(h, n);
    wireScroll(h, n, el);
  },
  prop(h, n, p, v) { n.list.prop(p, v); },
  frame(h, n, x, y, w, hh) {
    h.place(n, x, y, w, hh);
    n.list.resized();
    return true;
  },
  background(h, n) {
    n.el.style.backgroundColor = h.css(n.background);
    return true;
  },
  command(h, n, cmd, args) {
    const l = n.list;
    switch (cmd) {
      case Command.ListUpdate: l.update(args); return true;
      case Command.ScrollTo: {
        const r = new Reader(args);
        l.scrollToItem(r.u32(), r.u8() !== 0);
        return true;
      }
      case Command.ScrollToOffset: {
        const r = new Reader(args);
        r.f32();
        const y = r.f32(), animated = r.u8() !== 0;
        h.afterCore(() => { l.flush(); l.scrollTo(y, animated); });
        return true;
      }
    }
    return false;
  },
  destroy(h, n) {
    n.list.dispose();
    return false;
  },
  measure() { return [0, 0]; },
};

class List {
  constructor(h, n) {
    this.h = h;
    this.n = n;
    this.ids = [];
    this.sections = [];
    this.rows = []; // { header, section, first, count }
    this.tops = [];
    this.heights = new Map(); // by item ID, or a header's by its section's ID
    this.shown = new Map(); // row index -> row view
    this.free = [];
    this.columns = 1;
    this.gap = 0;
    this.appearance = 0;
    this.inset = 16;
    this.bottom = false;
    this.stuck = true;
    this.lead = 0;
    this.selecting = false;
    this.est = 44;
    this.width = 0;
    this.pending = null;
    this.queued = false;
    this.tiling = false;
    this.rebind = false;
    this.spacer = document.createElement("div");
    this.spacer.className = "asuka-list-spacer";
    n.el.appendChild(this.spacer);
    this.reorder = null; // Reorder's action names ({up, down}): its rows move
    this.drag = null; // a row's drag: before it lifts, and while it's lifted
    this.settling = 0;
  }

  prop(p, v) {
    switch (p) {
      case P.List.Columns: this.columns = Math.max(v, 1); break;
      case P.List.Gap: this.gap = v; break;
      case P.List.Appearance:
        this.appearance = v;
        this.n.el.dataset.appearance = ["plain", "grouped", "bare"][v] || "plain";
        break;
      case P.List.Inset: this.inset = v; break;
      case P.List.Anchor: this.bottom = v === 1; this.stuck = true; break;
      case P.List.Selecting:
        this.selecting = !!v;
        this.n.el.classList.toggle("asuka-selecting", this.selecting);
        // The selection control's room, at the rows' leading edge.
        this.h.afterCore(() => this.h.dispatch(Events.rowInset(this.n.id, this.selecting ? 40 : 0, 0)));
        for (const rv of this.shown.values()) this.paintRow(rv);
        return;
      case P.List.Reorder: this.setReorder(decodeReorder(v)); return;
      case P.List.Refreshable: refreshable(this.h, this.n, !!v); return;
      case P.List.Refreshing: refreshing(this.n, !!v); return;
    }
    this.rebind = true;
    this.queue();
  }

  /** update takes the core's item IDs and sections; rows bind again once the core has returned. */
  update(args) {
    const r = new Reader(args);
    const count = r.u32();
    const ids = new Array(count);
    for (let i = 0; i < count; i++) ids[i] = r.i64();
    const secs = [];
    if (!r.atEnd) {
      const ns = r.u32();
      for (let i = 0; i < ns; i++) secs.push({ start: r.u32(), id: r.i64() });
    }
    this.pending = { ids, secs };
    this.rebind = true;
    this.queue();
  }

  queue() {
    if (this.queued) return;
    this.queued = true;
    this.h.afterCore(() => {
      this.queued = false;
      if (!this.n.dying) this.flush();
    });
  }

  get offset() { return this.n.el.scrollTop; }

  viewport() { return this.n.el.clientHeight || this.n.frame[3]; }

  flush() {
    if (!this.pending) { this.tile(); return; }
    if (this.drag) this.endDrag(false); // the items changed under it
    const anchor = this.anchorRow();
    let keep = anchor;
    if ((this.bottom && this.stuck) || (!this.bottom && this.offset <= 0)) keep = null;
    const { ids, secs } = this.pending;
    this.pending = null;
    this.ids = ids;
    this.sections = secs;
    const live = new Map();
    for (const id of ids) if (this.heights.has(id)) live.set(id, this.heights.get(id));
    for (const s of secs) if (this.heights.has(s.id)) live.set(s.id, this.heights.get(s.id));
    this.heights = live;
    this.makeRows();
    this.tileKeeping(keep);
  }

  makeRows() {
    const rows = [];
    const add = (sec, from, to) => {
      for (let i = from; i < to; i += this.columns) rows.push({ header: false, section: sec, first: i, count: Math.min(this.columns, to - i) });
    };
    if (!this.sections.length) add(0, 0, this.ids.length);
    else {
      this.sections.forEach((s, k) => {
        const end = k + 1 < this.sections.length ? this.sections[k + 1].start : this.ids.length;
        rows.push({ header: true, section: k, first: 0, count: 1 });
        add(k, Math.min(s.start, this.ids.length), Math.min(Math.max(end, s.start), this.ids.length));
      });
    }
    this.rows = rows;
  }

  grouped() { return this.appearance === 1 && this.columns <= 1; }
  sticky() { return this.sections.length > 0 && this.appearance !== 1; }
  pad() { return this.grouped() ? this.inset : 0; }

  rowKey(r) {
    const row = this.rows[r];
    return row.header ? this.sections[row.section].id : this.ids[row.first];
  }

  rowHeight(r) {
    const row = this.rows[r];
    let height = 0, known = false;
    if (row.header) {
      const v = this.heights.get(this.sections[row.section].id);
      if (v != null) { height = v; known = true; }
    } else {
      for (let i = 0; i < row.count; i++) {
        const v = this.heights.get(this.ids[row.first + i]);
        if (v != null) { height = Math.max(height, v); known = true; }
      }
    }
    if (!known) height = this.est;
    const next = this.rows[r + 1];
    if (!row.header && next && !next.header) {
      if (this.columns > 1) height += this.gap;
      if (this.grouped()) height += cardGap;
    }
    return height;
  }

  measureTops() {
    const tops = this.tops;
    tops.length = 0;
    let y = this.pad();
    for (let r = 0; r < this.rows.length; r++) {
      tops.push(y);
      y += this.rowHeight(r);
    }
    tops.push(y + this.pad());
    this.lead = 0;
    if (this.bottom) {
      const view = this.viewport(), total = tops[tops.length - 1];
      if (total < view) {
        this.lead = view - total;
        for (let i = 0; i < tops.length; i++) tops[i] += this.lead;
      }
    }
  }

  search(f) {
    let lo = 0, hi = this.rows.length;
    while (lo < hi) {
      const m = (lo + hi) >> 1;
      if (f(m)) hi = m; else lo = m + 1;
    }
    return lo;
  }

  /** anchorRow is the row at the top of what shows, and how far below the top its top is. */
  anchorRow() {
    if (!this.rows.length || this.tops.length !== this.rows.length + 1) return null;
    const off = this.offset;
    const r = this.search((r) => this.tops[r + 1] > off);
    if (r >= this.rows.length) return null;
    return { key: this.rowKey(r), dy: this.tops[r] - off };
  }

  atEnd() {
    const t = this.tops;
    return !t.length || this.offset + this.viewport() >= t[t.length - 1] - 1;
  }

  pinnedRow() {
    if (!this.sticky() || !this.rows.length) return -1;
    const off = this.offset;
    const r = this.search((r) => this.tops[r + 1] > off);
    if (r >= this.rows.length) return -1;
    const s = this.rows[r].section;
    for (let k = r; k >= 0; k--) {
      if (this.rows[k].header && this.rows[k].section === s) return this.tops[k] >= off ? -1 : k;
    }
    return -1;
  }

  scrolled() {
    if (this.tiling || this.setting) return;
    if (this.bottom) this.stuck = this.atEnd();
    if (this.tickQueued) return;
    this.tickQueued = true;
    requestAnimationFrame(() => {
      this.tickQueued = false;
      if (!this.n.dying) this.h.afterCore(() => this.tile());
    });
  }

  tile() { this.tileKeeping(this.anchorRow()); }

  /**
   * tileKeeping shows the rows in view (and half a screen around them),
   * binds those new to the view, and places them, keeping the row at the
   * top where it was as rows above it change height (estimates made
   * real); a list anchored at its bottom and at its end stays there.
   */
  tileKeeping(keep) {
    const h = this.h;
    if (this.tiling || !h.core || h.inGo) return;
    this.tiling = true;
    try {
      for (let pass = 0; pass < 4; pass++) {
        this.measureTops();
        const total = this.tops[this.tops.length - 1];
        const view = this.viewport();
        this.spacer.style.height = total + "px";
        let off = this.offset;
        if (this.bottom && this.stuck) off = total - view;
        else if (keep) {
          for (let r = 0; r < this.rows.length; r++) {
            if (this.rowKey(r) === keep.key) { off = this.tops[r] - keep.dy; break; }
          }
        }
        off = Math.min(Math.max(off, 0), Math.max(total - view, 0));
        if (Math.abs(off - this.offset) > 0.5) {
          this.setting = true;
          this.n.el.scrollTop = off;
          this.setting = false;
        }
        const top = off - view / 2, bottom = off + view * 1.5;
        const first = this.search((r) => this.tops[r + 1] > top);
        const last = this.search((r) => this.tops[r] >= bottom);
        const pin = this.pinnedRow();
        const lifted = this.drag?.lifted ? this.drag.rv : null;
        for (const [r, rv] of this.shown) {
          if ((r < first || r >= last) && r !== pin && rv !== lifted) {
            this.shown.delete(r);
            rv.el.hidden = true;
            this.free.push(rv);
          }
        }
        let changed = false;
        const show = (r) => {
          let rv = this.shown.get(r);
          const fresh = !rv;
          if (fresh) { rv = this.take(); this.shown.set(r, rv); }
          if (fresh || this.rebind || rv.row !== r) {
            rv.row = r;
            rv.el.hidden = false; // laid out as it binds: what's in it is measured
            const was = this.rowHeight(r);
            this.bind(rv, r);
            if (this.rowHeight(r) !== was) changed = true;
          }
        };
        for (let r = first; r < last; r++) show(r);
        if (pin >= 0 && (pin < first || pin >= last)) show(pin);
        this.rebind = false;
        this.placeRows();
        if (!changed) break;
      }
    } finally {
      this.tiling = false;
    }
  }

  placeRows() {
    const off = this.offset;
    const [x, w] = this.rowX();
    const d = this.drag?.lifted ? this.drag : null;
    for (const [r, rv] of this.shown) {
      let y = this.tops[r];
      const height = this.rowHeight(r);
      rv.pinned = false;
      if (this.rows[r].header && this.sticky() && y < off) {
        y = off;
        for (let k = r + 1; k < this.rows.length; k++) {
          if (this.rows[k].header) { y = Math.min(y, this.tops[k] - height); break; }
        }
        rv.pinned = true;
      }
      // A lifted row follows the pointer; the others make way for it
      // (translate, which eases: the place a row is bound at doesn't).
      let shift = 0;
      if (d && rv === d.rv) y = d.top;
      else if (d && !rv.pinned) shift = shiftOf(r, d.from, d.to, d.size);
      rv.y = y + shift;
      const s = rv.el.style;
      s.transform = `translateY(${y}px)`;
      s.translate = shift ? `0 ${shift}px` : "";
      s.left = x + "px";
      s.width = w + "px";
      s.height = height + "px";
      rv.el.hidden = false;
      rv.el.classList.toggle("asuka-pinned", rv.pinned);
      this.placeCells(rv, r);
      this.paintRow(rv);
    }
  }

  rowX() {
    return this.grouped() ? [this.inset, Math.max(this.width - 2 * this.inset, 0)] : [0, this.width];
  }

  take() {
    const rv = this.free.pop();
    if (rv) return rv;
    const el = document.createElement("div");
    el.className = "asuka-row";
    el.setAttribute("role", "listitem");
    this.n.el.appendChild(el);
    const row = { el, cells: [], row: -1, pinned: false, y: 0 };
    el.addEventListener("click", (e) => this.rowClicked(row, e));
    el.addEventListener("contextmenu", (e) => this.rowMenu(row, e));
    el.addEventListener("keydown", (e) => this.rowKeyDown(row, e));
    // A touch held on a row begins selecting, as Android's long press
    // (where rows move, it lifts the row: see dragDown).
    let hold = null;
    const cancel = () => { clearTimeout(hold?.t); hold = null; };
    el.addEventListener("pointerdown", (e) => {
      cancel();
      this.dragDown(row, e);
      if (e.pointerType !== "touch" || this.selecting || row.header || this.movable()) return;
      hold = { x: e.clientX, y: e.clientY, t: setTimeout(() => {
        hold = null;
        const c = this.cellAt(row, e.target);
        if (c && this.beginSelecting(c)) row.swallowClick = true;
      }, longPressMs) };
    });
    el.addEventListener("pointermove", (e) => { if (hold && Math.hypot(e.clientX - hold.x, e.clientY - hold.y) > pressSlop) cancel(); });
    el.addEventListener("pointerup", cancel);
    el.addEventListener("pointercancel", cancel);
    return row;
  }

  bind(rv, r) {
    const row = this.rows[r];
    const k = row.header ? 1 : row.count;
    while (rv.cells.length < k) rv.cells.push(this.newCell(rv));
    rv.cells.forEach((c, i) => { c.n.el.hidden = i >= k; });
    rv.header = row.header;
    rv.el.classList.toggle("asuka-header", row.header);
    this.edges(rv, r);
    for (let i = 0; i < k; i++) {
      const c = rv.cells[i];
      c.header = row.header;
      let index = row.first + i;
      if (row.header) { c.item = this.sections[row.section].id; index = -1 - row.section; }
      else c.item = this.ids[index];
      const known = this.heights.get(c.item);
      if (known != null) c.height = known;
      this.h.listBind(this.n.id, index, c.n.id);
    }
  }

  /** edges are what a row shows of its place: a separator under it, a grouped card's first and last; where rows move, its place for screen readers, and the keyboard's focus. */
  edges(rv, r) {
    const row = this.rows[r];
    rv.sep = this.columns <= 1 && this.appearance !== 2 && !row.header && r < this.rows.length - 1 && !this.rows[r + 1].header;
    rv.first = !row.header && (r === 0 || this.rows[r - 1].header);
    rv.last = !row.header && (r === this.rows.length - 1 || this.rows[r + 1].header);
    const el = rv.el;
    if (this.movable() && !row.header) {
      el.tabIndex = 0;
      el.setAttribute("aria-posinset", String(row.first + 1));
      el.setAttribute("aria-setsize", String(this.ids.length));
      el.setAttribute("aria-keyshortcuts", "Alt+ArrowUp Alt+ArrowDown");
    } else if (el.hasAttribute("aria-posinset")) {
      el.removeAttribute("tabindex");
      el.removeAttribute("aria-posinset");
      el.removeAttribute("aria-setsize");
      el.removeAttribute("aria-keyshortcuts");
    }
  }

  newCell(rv) {
    const l = this;
    const n = this.h.hostNode(KindCell, cellImpl);
    const c = { n, l, rv, item: 0, height: 0, header: false, selected: false, swipe: [], leading: [] };
    n.cell = {
      prop(h, node, p, v) {
        if (p === Prop.SwipeActions || p === Prop.LeadingSwipeActions) {
          const a = decodeActions(v);
          if (p === Prop.SwipeActions) c.swipe = a; else c.leading = a;
        } else {
          c.selected = !!v;
          l.paintRow(rv);
        }
      },
    };
    n.listCell = c;
    rv.el.appendChild(n.el);
    return c;
  }

  /** setHeight takes a row's height from its cell's frame. */
  setHeight(c, height) {
    c.height = height;
    const old = this.heights.get(c.item);
    if (old === height) return;
    if (old == null && this.heights.size < 64 && !c.header) {
      this.est = (this.est * this.heights.size + height) / (this.heights.size + 1);
    }
    this.heights.set(c.item, height);
    if (!this.tiling) this.queue();
  }

  placeCells(rv, r) {
    if (this.columns <= 1 || this.rows[r].header) {
      const [, w] = this.rowX();
      for (const c of rv.cells) {
        const s = c.n.el.style;
        s.left = "0px";
        s.top = "0px";
        s.width = w + "px";
        s.height = c.height + "px";
      }
      return;
    }
    const cw = (this.width - this.gap * (this.columns - 1)) / this.columns;
    const rtl = getComputedStyle(this.n.el).direction === "rtl";
    rv.cells.forEach((c, i) => {
      let x = i * (cw + this.gap);
      if (rtl) x = this.width - x - cw;
      const s = c.n.el.style;
      s.left = x + "px";
      s.top = "0px";
      s.width = cw + "px";
      s.height = c.height + "px";
    });
  }

  paintRow(rv) {
    const el = rv.el;
    el.classList.toggle("asuka-sep", !!rv.sep);
    el.classList.toggle("asuka-first", !!rv.first);
    el.classList.toggle("asuka-last", !!rv.last);
    const selected = this.selecting && rv.cells.some((c) => c.selected && !c.header && !c.n.el.hidden);
    el.classList.toggle("asuka-selected", selected);
    el.setAttribute("aria-selected", this.selecting ? String(selected) : "false");
  }

  rowClicked(rv, e) {
    if (rv.swallowClick) {
      // The click a long press that began selecting ends with.
      rv.swallowClick = false;
      e.stopPropagation();
      e.preventDefault();
      return;
    }
    if (rv.header) return;
    const c = this.cellAt(rv, e.target);
    if (!c) return;
    if (!this.selecting) {
      // ⌘-click (Ctrl-click off Apple's) begins selecting at the row, as
      // a desktop list's: when its row asks to hear of it.
      if ((apple() ? e.metaKey : e.ctrlKey) && this.beginSelecting(c)) {
        e.stopPropagation();
        e.preventDefault();
      }
      return;
    }
    e.stopPropagation();
    this.h.dispatch(Events.rowSelected(c.n.id, !c.selected));
  }

  /** cellAt is the row's cell target is in, else its first. */
  cellAt(rv, target) {
    return rv.cells.find((c) => c.n.el.contains(target)) || rv.cells[0];
  }

  /**
   * beginSelecting tells c's row that the user began selecting there
   * (RowSelectBegin), if it listens, as the other hosts do: the app's on
   * selectBegin turns selecting on (and may choose the row). False when
   * it doesn't listen.
   */
  beginSelecting(c) {
    if (!c.n.listening(Event.RowSelectBegin)) return false;
    this.h.dispatch(Events.rowSelectBegin(c.n.id));
    return true;
  }

  /**
   * rowMenu: a row's swipe actions are its context menu on the web, as
   * on the desktop hosts, and so are its moves (Reorder's actions) where
   * rows move: the web's screen readers have no custom actions.
   */
  rowMenu(rv, e) {
    if (this.drag && (this.drag.lifted || this.drag.type === "touch")) {
      // A touch held on a row lifts it: not the browser's long press menu.
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const c = rv.cells.find((c) => c.n.el.contains(e.target));
    if (!c || e.defaultPrevented) return;
    const moves = this.moveItems(rv);
    if (!c.swipe.length && !c.leading.length && !moves.length) return;
    // A view in the row with a menu of its own, or a long press, has its own.
    if (moves.length && !c.swipe.length && !c.leading.length) {
      const own = this.h.interaction.pressTarget(e.target) || this.h.interaction.gestureTarget(e.target, [Event.LongPress]);
      if (own && (own.menu || own.listening(Event.LongPress))) return;
    }
    e.preventDefault();
    // Flat: a swipe's actions have no children, and none is disabled.
    const items = [...moves, ...c.leading.map((a, i) => ({ ...a, flags: 0, items: [], pick: () => this.h.dispatch(Events.action(c.n.id, -1 - i)) })),
      ...c.swipe.map((a, i) => ({ ...a, flags: 0, items: [], pick: () => this.h.dispatch(Events.action(c.n.id, i)) }))];
    this.h.menus.show(items, e.clientX, e.clientY);
  }

  // --- Moving rows (Reorder) ---

  setReorder(names) {
    if (!names && this.drag) this.endDrag(false);
    const was = !!this.reorder;
    this.reorder = names;
    if (!!names !== was) {
      // While a row is lifted, a touch's move is its, not a scroll.
      if (names) this.n.el.addEventListener("touchmove", this.touchMove ??= (e) => { if (this.drag?.lifted) e.preventDefault(); }, { passive: false });
      else if (this.touchMove) this.n.el.removeEventListener("touchmove", this.touchMove);
      if (!this.dragListening) {
        this.dragListening = true;
        const el = this.n.el;
        el.addEventListener("pointermove", (e) => this.dragMove(e));
        el.addEventListener("pointerup", (e) => this.dragUp(e, true));
        el.addEventListener("pointercancel", (e) => this.dragUp(e, false));
      }
    }
    for (const [r, rv] of this.shown) this.edges(rv, r);
  }

  /** movable says its rows move: Reorder, in a list (a grid's cells don't). */
  movable() { return !!this.reorder && this.columns <= 1; }

  /** moveItems are a row's moves, as a menu's items: up one place, down one, where it can go. */
  moveItems(rv) {
    if (!this.movable() || rv.header || rv.row < 0) return [];
    const i = this.rows[rv.row].first;
    const out = [];
    if (i > 0) out.push({ role: 0, title: this.reorder.up, icon: "arrow.up", flags: 0, items: [], pick: () => this.moveBy(rv, -1) });
    if (i < this.ids.length - 1) out.push({ role: 0, title: this.reorder.down, icon: "arrow.down", flags: 0, items: [], pick: () => this.moveBy(rv, 1) });
    return out;
  }

  /** rowKeyDown: Alt (⌥) and the up or down arrow moves the focused row one place. */
  rowKeyDown(rv, e) {
    if (!e.altKey || e.ctrlKey || e.metaKey || e.shiftKey || (e.key !== "ArrowUp" && e.key !== "ArrowDown")) return;
    if (!this.movable() || rv.header) return;
    e.preventDefault();
    e.stopPropagation();
    this.moveBy(rv, e.key === "ArrowUp" ? -1 : 1);
  }

  /** moveBy moves a row one place up (-1) or down (1), as a screen reader's action or the keyboard's, and says where it is. */
  moveBy(rv, by) {
    if (this.drag || rv.header || rv.row < 0 || !this.shown.has(rv.row) || this.shown.get(rv.row) !== rv) return;
    const from = this.rows[rv.row].first, to = from + by;
    if (to < 0 || to >= this.ids.length) return;
    this.reordered(moveID(this.ids, from, to), this.sections, this.positions());
    this.h.dispatch(Events.moved(this.n.id, from, to));
    this.scrollToItem(to, true);
    const text = (rv.el.innerText || "").trim().split("\n")[0];
    this.h.announce(`${text ? text + ", " : ""}${to + 1}/${this.ids.length}`);
  }

  /** dragDown: a pointer down on a row may lift it, as it moves past the drag threshold, or a touch held for a long press. */
  dragDown(rv, e) {
    if (!this.movable() || rv.header || this.drag || !e.isPrimary || (e.pointerType === "mouse" && e.button !== 0)) return;
    // A field keeps its own drags (selecting text), a dragged view its drag and drop, a pan its pan.
    if (e.target.closest?.("input, textarea, select, [contenteditable=true], [draggable=true]")) return;
    const pan = this.h.interaction.gestureTarget(e.target, [Event.Pan]);
    if (pan && rv.el.contains(pan.el)) return;
    const d = { rv, id: e.pointerId, type: e.pointerType, x0: e.clientX, y0: e.clientY, x: e.clientX, y: e.clientY, lifted: false, moved: false, timer: 0, raf: 0, target: e.target };
    this.drag = d;
    if (d.type === "touch") d.timer = setTimeout(() => { if (this.drag === d) this.lift(); }, longPressMs);
  }

  dragMove(e) {
    const d = this.drag;
    if (!d || d.id !== e.pointerId) return;
    d.x = e.clientX;
    d.y = e.clientY;
    const far = Math.hypot(d.x - d.x0, d.y - d.y0);
    if (!d.lifted) {
      if (d.type === "touch") {
        if (far > pressSlop) { clearTimeout(d.timer); this.drag = null; } // a scroll
        return;
      }
      if (far <= dragSlop) return;
      this.lift();
    }
    if (far > pressSlop) d.moved = true;
    this.dragTo();
  }

  /** lift lifts the row being dragged: it follows the pointer, above the others. */
  lift() {
    const d = this.drag;
    if (!d || d.lifted || !this.shown.has(d.rv.row)) { this.drag = null; return; }
    clearTimeout(d.timer);
    d.lifted = true;
    this.h.interaction.takeTouch();
    try { d.rv.el.setPointerCapture(d.id); } catch {}
    const r = d.rv.row;
    d.from = d.to = r;
    d.size = this.tops[r + 1] - this.tops[r];
    const top = this.n.el.getBoundingClientRect().top;
    d.grab = d.y0 - top + this.offset - this.tops[r];
    d.top = this.tops[r];
    clearTimeout(this.settling);
    this.n.el.classList.remove("asuka-settling");
    d.rv.el.classList.remove("asuka-dropping");
    d.rv.el.classList.add("asuka-lifted");
    this.n.el.classList.add("asuka-reordering");
    d.key = (e) => { if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); this.endDrag(false); } };
    addEventListener("keydown", d.key, true);
    const edge = () => {
      if (this.drag !== d) return;
      this.edgeScroll(d);
      d.raf = requestAnimationFrame(edge);
    };
    d.raf = requestAnimationFrame(edge);
    this.dragTo();
  }

  /** dragTo places the lifted row under the pointer, and the others where it would go. */
  dragTo() {
    const d = this.drag;
    if (!d?.lifted) return;
    const top = this.n.el.getBoundingClientRect().top;
    const total = this.tops[this.tops.length - 1];
    d.top = Math.min(Math.max(d.y - top + this.offset - d.grab, 0), Math.max(total - d.size, 0));
    const to = reorderTarget(this.tops, d.from, d.top + d.size / 2, this.sections.length ? 1 : 0);
    if (to !== d.to) d.to = to;
    this.placeRows();
  }

  /** edgeScroll scrolls the list while a lifted row is held near its top or bottom, the nearer the faster. */
  edgeScroll(d) {
    const el = this.n.el, r = el.getBoundingClientRect();
    const zone = Math.min(edgeZone, r.height / 4);
    let v = 0;
    if (d.y < r.top + zone) v = -(r.top + zone - d.y) / zone;
    else if (d.y > r.bottom - zone) v = (d.y - (r.bottom - zone)) / zone;
    if (!v) return;
    const was = el.scrollTop;
    el.scrollTop = was + Math.max(-1.5, Math.min(1.5, v)) * edgeSpeed;
    if (el.scrollTop !== was) this.dragTo();
  }

  dragUp(e, dropped) {
    const d = this.drag;
    if (!d || d.id !== e.pointerId) return;
    if (!d.lifted) {
      clearTimeout(d.timer);
      this.drag = null;
      return;
    }
    // The click a drag ends with presses nothing.
    const swallow = (ev) => { ev.stopPropagation(); ev.preventDefault(); };
    addEventListener("click", swallow, true);
    setTimeout(() => removeEventListener("click", swallow, true), 0);
    if (dropped && !d.moved && d.type === "touch" && !this.selecting) {
      // Held and let go where it was: a long press, which begins selecting.
      this.endDrag(false);
      this.beginSelecting(this.cellAt(d.rv, d.target));
      return;
    }
    this.endDrag(dropped);
  }

  /** endDrag lets the lifted row go: where it was dropped (reported with Moved), or back where it was. */
  endDrag(dropped) {
    const d = this.drag;
    if (!d) return;
    this.drag = null;
    clearTimeout(d.timer);
    cancelAnimationFrame(d.raf);
    if (!d.lifted) return;
    removeEventListener("keydown", d.key, true);
    try { d.rv.el.releasePointerCapture(d.id); } catch {}
    const before = this.positions();
    d.rv.el.classList.remove("asuka-lifted");
    d.rv.el.classList.add("asuka-dropping");
    this.n.el.classList.remove("asuka-reordering");
    const m = dropped && d.to !== d.from ? moveRow(this.rows, this.ids, this.sections, d.from, d.to) : null;
    if (m) {
      this.reordered(m.ids, this.sections.length ? m.sections : this.sections, before);
      this.h.dispatch(Events.moved(this.n.id, m.from, m.to));
    } else {
      this.placeRows();
      this.settle(before);
    }
  }

  /** positions are where each row shows now. */
  positions() {
    const out = new Map();
    for (const rv of this.shown.values()) out.set(rv, rv.y);
    return out;
  }

  /**
   * reordered takes the items in their new order, which the user made:
   * each row view keeps its item (none is bound again), at its new place,
   * and they settle there from where they were (before).
   */
  reordered(ids, sections, before) {
    const shown = this.shown;
    this.ids = ids;
    this.sections = sections;
    this.makeRows();
    const items = new Map(), heads = new Map();
    this.rows.forEach((row, r) => {
      if (row.header) heads.set(this.sections[row.section].id, r);
      else items.set(this.ids[row.first], r);
    });
    this.shown = new Map();
    for (const rv of shown.values()) {
      const c = rv.cells[0];
      const r = c ? (rv.header ? heads.get(c.item) : items.get(c.item)) : undefined;
      if (r == null) { rv.el.hidden = true; this.free.push(rv); continue; }
      rv.row = r;
      this.edges(rv, r);
      this.shown.set(r, rv);
    }
    this.measureTops();
    this.placeRows();
    this.settle(before);
    this.queue(); // rows come into view where others left
  }

  /** settle eases rows from where they showed (before) to where they are. */
  settle(before) {
    const el = this.n.el;
    el.classList.remove("asuka-settling");
    let any = false;
    for (const [rv, y] of before) {
      if (rv.el.hidden || y == null) continue;
      const dy = y - rv.y;
      if (Math.abs(dy) < 0.5) continue;
      rv.el.style.translate = `0 ${dy}px`;
      any = true;
    }
    clearTimeout(this.settling);
    const done = () => {
      el.classList.remove("asuka-settling");
      for (const rv of before.keys()) rv.el.classList.remove("asuka-dropping");
    };
    if (!any) { done(); return; }
    void el.offsetHeight; // from there, with no transition
    el.classList.add("asuka-settling");
    for (const rv of before.keys()) rv.el.style.translate = "";
    this.settling = setTimeout(done, settleMs + 30);
  }

  resized() {
    const w = this.n.frame[2];
    if (w !== this.width) {
      this.width = w;
      this.rebind = true;
    }
    this.queue();
  }

  scrollTo(y, animated) {
    if (!this.tops.length) return;
    const total = this.tops[this.tops.length - 1];
    y = Math.min(Math.max(y, 0), Math.max(total - this.viewport(), 0));
    if (this.bottom) this.stuck = y >= Math.max(total - this.viewport(), 0) - 1;
    this.n.el.scrollTo({ top: y, behavior: animated ? "smooth" : "instant" });
    this.tile();
  }

  scrollToItem(index, animated) {
    this.h.afterCore(() => {
      this.flush();
      for (let r = 0; r < this.rows.length; r++) {
        const row = this.rows[r];
        if (!row.header && index >= row.first && index < row.first + row.count) {
          const view = this.viewport();
          const top = this.tops[r], bottom = this.tops[r + 1];
          let y = this.offset;
          if (top < y) y = top;
          else if (bottom > y + view) y = bottom - view;
          this.scrollTo(y, animated);
          return;
        }
      }
    });
  }

  dispose() {
    for (const rv of [...this.shown.values(), ...this.free]) {
      for (const c of rv.cells) this.h.nodes.delete(c.n.id);
    }
  }
}

const cellImpl = {
  create(h, n) {
    const el = document.createElement("div");
    el.className = "asuka-cell";
    n.el = el;
  },
  frame(h, n, x, y, w, hh) {
    n.frame = [x, y, w, hh];
    n.placed = true;
    n.listCell.l.setHeight(n.listCell, hh);
    n.el.style.height = hh + "px";
    return true;
  },
  measure() { return [0, 0]; },
};
