// Table and Outline: a <table> with the grid role, and a tree (role=tree
// of treeitems), both virtualized as the List is (core-virtualized): a
// scrolling element as tall as all the rows (count × RowHeight), holding
// only the rows in view and a few more each side, each a recycled row
// view whose cell (a host-made view, AK_HOST_ID_BIT) the core binds the
// row into through ListBind. A row of plain text comes as its cell's
// RowData, which the host writes into the row's <td>s (or the tree row's
// text) itself; a row of views has them as its cell's children, laid out
// by the core across the row (a Table's at its columns' places, from the
// ColumnLayout this host reports; an Outline's after its indent and
// disclosure). The rows' order, the selection, the sort and what's
// expanded are the core's: the host shows what Selection, Sort and Levels
// say and reports the user's changes (RowsSelected, SortBy, Expand), and
// the user's columns (ColumnLayout). The keyboard's focus stays on the
// grid (or the tree), its active row its aria-activedescendant: rows come
// and go as they scroll. Pure parts (encodings, the selection, the rows
// in view, the tree's keys) are grid.js's.

import { Event, Events, Command, Reader, KindProp as P, Prop } from "./ops.js";
import { own } from "./views.js";
import { decodeActions } from "./actions.js";
import { withPicks } from "./menu.js";
import { symbolSVG } from "./media.js";
import { apple } from "./keys.js";
import * as g from "./grid.js";

const KindGridCell = 0xfff2;
const overscan = 8; // rows shown past each end of the view
const dragSlop = 5; // points a header moves before it's dragged, not clicked
const longPressMs = 500, pressSlop = 10; // a touch held on a row: its menu

const controls = "button, input, select, textarea, a[href], [contenteditable=true], [contenteditable=''], .asuka-pressable";

function div(cls) {
  const d = document.createElement("div");
  if (cls) d.className = cls;
  return d;
}

function span(cls) {
  const s = document.createElement("span");
  s.className = cls;
  return s;
}

const alignCSS = ["start", "center", "end"];

/** What a Table and an Outline share: the rows in view, their cells, the selection, the keyboard. */
class Grid {
  constructor(h, n) {
    this.h = h;
    this.n = n;
    this.ids = [];
    this.rh = 32;
    this.mode = g.Mode.Single;
    this.sel = new g.RowSelection();
    this.active = -1; // the keyboard's row
    this.activeId = null;
    this.shown = new Map(); // row -> row view
    this.free = [];
    this.pending = null; // a ListUpdate's IDs, until the core returns
    this.queued = false;
    this.tiling = false;
    this.rebind = false;
    this.focused = false;
    this.cells = new Set(); // every cell made
    const el = own(n, div("asuka-grid-view"));
    this.scroller = div("asuka-scroller asuka-grid-scroll");
    el.appendChild(this.scroller);
    let ticking = false;
    this.scroller.addEventListener("scroll", () => {
      if (ticking || this.tiling) return;
      ticking = true;
      requestAnimationFrame(() => {
        ticking = false;
        if (!n.dying) this.h.afterCore(() => this.tile());
      });
    }, { passive: true });
  }

  /** wire listens on the rows' container and the focus element (made by the kind). */
  wire() {
    const f = this.focusEl, b = this.body;
    f.tabIndex = 0;
    this.n.focusEl = f;
    f.addEventListener("focus", () => this.setFocused(true));
    f.addEventListener("blur", () => this.setFocused(false));
    f.addEventListener("keydown", (e) => this.key(e));
    b.addEventListener("pointerdown", (e) => this.down(e));
    b.addEventListener("dblclick", (e) => this.doubleClick(e));
    b.addEventListener("contextmenu", (e) => this.context(e));
    b.addEventListener("pointermove", (e) => { if (this.hold && Math.hypot(e.clientX - this.hold.x, e.clientY - this.hold.y) > pressSlop) this.cancelHold(); });
    b.addEventListener("pointerup", () => this.cancelHold());
    b.addEventListener("pointercancel", () => { this.cancelHold(); this.tap = null; });
    b.addEventListener("click", (e) => this.tapped(e));
    // A secondary click's contextmenu is its row's (context); one with no
    // click before it is the keyboard's (the context menu key), at the
    // active row.
    f.addEventListener("pointerdown", (e) => {
      if (e.button === 2 || (e.button === 0 && e.ctrlKey && apple())) this.secondaryAt = performance.now();
    }, true);
    f.addEventListener("contextmenu", (e) => {
      const mouse = this.secondaryAt && performance.now() - this.secondaryAt < 1500;
      this.secondaryAt = 0;
      if (e.defaultPrevented || mouse || e.pointerType === "touch") return;
      this.keyMenu(e);
    });
  }

  setFocused(on) {
    this.focused = on;
    this.n.el.classList.toggle("asuka-grid-focused", on);
    if (on && this.h.focus?.modality === "key") this.n.el.classList.add("asuka-grid-keys");
    if (on && this.active < 0 && this.ids.length) {
      // The keyboard starts at the first selected row, or the first.
      const f = this.sel.first();
      this.setActive(f >= 0 ? f : 0, false);
    }
  }

  // --- Props and commands ---

  setRowHeight(v) {
    if (!(v > 0) || v === this.rh) return;
    this.rh = v;
    this.n.el.style.setProperty("--asuka-row-height", v + "px");
    this.queue();
  }

  setMode(v) {
    this.mode = v;
    this.focusEl.setAttribute("aria-multiselectable", v === g.Mode.Multiple ? "true" : "false");
    for (const rv of this.shown.values()) this.paint(rv);
  }

  setSelection(v) {
    this.sel.set(g.decodeRuns(v));
    for (const rv of this.shown.values()) this.paint(rv);
  }

  command(cmd, args) {
    switch (cmd) {
      case Command.ListUpdate: this.update(args); return true;
      case Command.ScrollTo: {
        const r = new Reader(args);
        const i = r.u32(), animated = r.u8() !== 0;
        this.h.afterCore(() => { this.flush(); this.scrollToRow(i, animated); });
        return true;
      }
      case Command.ScrollToOffset: {
        const r = new Reader(args);
        const x = r.f32(), y = r.f32(), animated = r.u8() !== 0;
        this.h.afterCore(() => { this.flush(); this.scroller.scrollTo({ left: x, top: y, behavior: animated ? "smooth" : "instant" }); this.tile(); });
        return true;
      }
    }
    return false;
  }

  update(args) {
    const r = new Reader(args);
    const count = r.u32();
    const ids = new Array(count);
    for (let i = 0; i < count; i++) ids[i] = r.i64();
    this.pending = ids;
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

  /** flush takes a ListUpdate's IDs (and the Levels that came with them); the rows in view bind again. */
  flush() {
    if (this.pending) {
      this.ids = this.pending;
      this.pending = null;
      this.updated();
      this.rebind = true;
      // The keyboard's row stays on its item.
      if (this.activeId != null) {
        const i = this.ids.indexOf(this.activeId);
        this.active = i >= 0 ? i : Math.min(this.active, this.ids.length - 1);
      }
      if (this.active >= this.ids.length) this.active = this.ids.length - 1;
      this.activeId = this.active >= 0 ? this.ids[this.active] : null;
      // A Shift extends from there (the rows may have moved under the old anchor).
      if (this.sel.anchor >= 0) this.sel.anchor = this.active;
      if (this.countAttr) this.focusEl.setAttribute(this.countAttr, String(this.ids.length + this.headRows));
    }
    this.tile();
  }

  updated() {}
  get headRows() { return 0; }

  // --- Tiling ---

  /** headHeight is the room above the rows (a Table's header). */
  headHeight() { return 0; }

  viewport() { return Math.max((this.scroller.clientHeight || this.n.frame[3]) - this.headHeight(), 0); }

  tile() {
    const h = this.h;
    if (this.tiling || !h.core || h.inGo || this.n.dying) return;
    this.tiling = true;
    try {
      const count = this.ids.length, rh = this.rh;
      const [first, last] = g.visibleRange(this.scroller.scrollTop, this.viewport(), rh, count, overscan);
      for (const [r, rv] of this.shown) {
        if (r < first || r >= last) {
          this.shown.delete(r);
          rv.el.remove();
          rv.row = -1;
          this.free.push(rv);
        }
      }
      let prev = null;
      for (let r = first; r < last; r++) {
        let rv = this.shown.get(r);
        if (!rv) {
          rv = this.free.pop() || this.makeRow();
          this.shown.set(r, rv);
        }
        if (rv.row !== r || this.rebind) {
          rv.row = r;
          this.bind(rv, r);
        }
        const want = prev ? prev.el.nextSibling : this.top.nextSibling;
        if (rv.el !== want) this.body.insertBefore(rv.el, want);
        prev = rv;
      }
      this.rebind = false;
      this.spacer(this.top, first * rh);
      this.spacer(this.bottom, (count - last) * rh);
      this.activeDescendant();
    } finally {
      this.tiling = false;
    }
  }

  spacer(el, height) {
    el.style.height = height + "px";
    el.style.display = height > 0 ? "" : "none";
  }

  /** bind binds row r into rv's cell: the core sends its RowData, or its views, as it does. */
  bind(rv, r) {
    const c = rv.cell;
    c.item = this.ids[r];
    rv.el.id = `asuka-grid-${this.n.id}-${r}`;
    this.placed(rv, r);
    this.h.listBind(this.n.id, r, c.n.id);
    this.fill(rv);
    this.paint(rv);
  }

  /** placed sets what a row view shows of its place (its index for screen readers). */
  placed(rv, r) {}

  newCell(rv) {
    const n = this.h.hostNode(KindGridCell, cellImpl);
    const c = { n, rv, grid: this, item: null, data: null, menu: null };
    n.gridCell = c;
    // Keys typed in a row's views go on to the grid (keys.js).
    n.listCell = { l: { n: this.n } };
    this.cells.add(c);
    return c;
  }

  /** views reports whether rv's cell holds views (a row of views), not text. */
  views(rv) { return rv.cell.n.children.length > 0; }

  paint(rv) {
    const r = rv.row;
    if (r < 0) return;
    const selected = this.mode !== g.Mode.None && this.sel.has(r);
    rv.el.classList.toggle("asuka-selected", selected);
    rv.el.setAttribute("aria-selected", this.mode === g.Mode.None ? "false" : String(selected));
    rv.el.classList.toggle("asuka-active", r === this.active);
    rv.el.classList.toggle("asuka-odd", (r & 1) === 1);
  }

  activeDescendant() {
    const rv = this.active >= 0 ? this.shown.get(this.active) : null;
    if (rv) this.focusEl.setAttribute("aria-activedescendant", rv.el.id);
    else this.focusEl.removeAttribute("aria-activedescendant");
  }

  setActive(r, scroll) {
    const was = this.active;
    this.active = r;
    this.activeId = r >= 0 ? this.ids[r] : null;
    for (const i of [was, r]) {
      const rv = this.shown.get(i);
      if (rv) this.paint(rv);
    }
    if (scroll && r >= 0) this.scrollToRow(r, false);
    this.activeDescendant();
  }

  scrollToRow(r, animated) {
    if (r < 0 || r >= this.ids.length) return;
    const s = this.scroller;
    const y = g.scrollFor(r, s.scrollTop, this.viewport(), this.rh);
    if (Math.abs(y - s.scrollTop) > 0.5) s.scrollTo({ top: y, behavior: animated ? "smooth" : "instant" });
    this.tile();
  }

  repaint() { for (const rv of this.shown.values()) this.paint(rv); }

  // --- Reporting ---

  reportSelection() {
    this.repaint();
    if (this.n.listening(Event.RowsSelected)) this.h.dispatch(Events.rowsSelected(this.n.id, g.encodeRuns(this.sel.runs)));
  }

  open(r) {
    if (r >= 0 && r < this.ids.length && this.n.listening(Event.Open)) this.h.dispatch(Events.open(this.n.id, r));
  }

  // --- The pointer ---

  /** rowAt is the row view of this grid target is in (not a grid's inside a row's views), or null. */
  rowAt(target) {
    for (let el = target?.closest?.(".asuka-grid-row"); el; el = el.parentElement?.closest(".asuka-grid-row")) {
      const rv = el.asukaRow;
      if (rv && rv.row >= 0 && this.shown.get(rv.row) === rv) return rv;
    }
    return null;
  }

  /** inControl: target is a control in a row of views (a button, a field), which keeps its own presses. */
  inControl(rv, target) {
    const c = target.closest?.(controls);
    return !!c && rv.el.contains(c);
  }

  /**
   * down: a mouse's press on a row selects as the platform does (Shift
   * extends, ⌘ or Ctrl toggles) as it goes down; a secondary press (a
   * Mac's Control-click) selects the row unless it's selected, for its
   * menu. A touch selects as it taps (click: a touch that scrolls
   * selects nothing), and held, shows the row's menu.
   */
  down(e) {
    this.n.el.classList.remove("asuka-grid-keys");
    const rv = this.rowAt(e.target);
    if (!rv || !e.isPrimary || this.inControl(rv, e.target)) return;
    const mac = apple();
    if (e.pointerType === "mouse" && (e.button !== 0 || (mac && e.ctrlKey))) {
      if (e.button === 2 || e.button === 0) this.ensureSelected(rv.row);
      return;
    }
    if (this.pressedPart(rv, e)) return;
    this.focusEl.focus({ preventScroll: true });
    if (e.pointerType === "touch" || e.pointerType === "pen") {
      this.cancelHold();
      const x = e.clientX, y = e.clientY;
      this.tap = rv;
      this.hold = { x, y, t: setTimeout(() => { this.hold = null; this.tap = null; this.heldAt = performance.now(); this.menuAt(rv, x, y, true); }, longPressMs) };
      return;
    }
    const primary = mac ? e.metaKey : e.ctrlKey;
    if (this.sel.click(rv.row, e.shiftKey, primary, this.mode)) this.reportSelection();
    this.setActive(rv.row, false);
  }

  /** tapped: a touch's tap on a row selects it. */
  tapped(e) {
    const rv = this.tap;
    this.tap = null;
    if (!rv || this.rowAt(e.target) !== rv || this.inControl(rv, e.target)) return;
    if (this.sel.click(rv.row, false, false, this.mode)) this.reportSelection();
    this.setActive(rv.row, false);
  }

  /** pressedPart: a press on what the row draws besides its cell (an Outline's disclosure); true when it took it. */
  pressedPart() { return false; }

  cancelHold() {
    if (!this.hold) return;
    clearTimeout(this.hold.t);
    this.hold = null;
  }

  doubleClick(e) {
    const rv = this.rowAt(e.target);
    if (!rv || rv.row < 0 || this.inControl(rv, e.target) || e.target.closest?.(".asuka-tree-disclosure")) return;
    this.open(rv.row);
  }

  ensureSelected(r) {
    if (this.sel.ensure(r, this.mode)) this.reportSelection();
    this.setActive(r, false);
  }

  context(e) {
    const rv = this.rowAt(e.target);
    if (!rv || rv.row < 0 || e.defaultPrevented) return;
    // A view in the row with a menu of its own has it (interaction.js).
    const own = this.h.interaction.pressTarget(e.target);
    if (own && own.menu && rv.el.contains(own.el)) return;
    if (this.heldAt && performance.now() - this.heldAt < 1500) {
      // The browser's long press, after the host's own showed the menu.
      this.heldAt = 0;
      e.preventDefault();
      return;
    }
    this.cancelHold();
    this.tap = null;
    if (!rv.cell.menu?.length) {
      this.ensureSelected(rv.row);
      return;
    }
    e.preventDefault();
    this.menuAt(rv, e.clientX, e.clientY, false);
  }

  keyMenu(e) {
    const r = this.active >= 0 ? this.active : this.sel.first();
    if (r < 0) return;
    this.scrollToRow(r, false);
    const rv = this.shown.get(r);
    if (!rv) return;
    e.preventDefault();
    const b = rv.el.getBoundingClientRect(), s = this.scroller.getBoundingClientRect();
    const x = this.h.rtl ? Math.min(b.right, s.right) - 24 : Math.max(b.left, s.left) + 24;
    this.menuAt(rv, x, b.bottom, false);
  }

  /**
   * menuAt selects rv's row if it isn't, then shows its cell's Menu at x,
   * y: an item chosen is Action on the cell, its number depth first.
   */
  menuAt(rv, x, y, touch) {
    const r = rv.row;
    if (r < 0) return;
    this.ensureSelected(r);
    this.h.afterCore(() => {
      // The selection's change may have changed the menu (the core sent it again).
      const c = this.shown.get(r)?.cell;
      if (!c?.menu?.length) return;
      const id = c.n.id;
      this.h.menus.show(withPicks(c.menu, (a) => this.h.dispatch(Events.action(id, a.index))), x, y);
      const m = this.h.menus.popup;
      m?.addEventListener("toggle", (ev) => {
        if (ev.newState !== "closed") return;
        // The focus goes back to the grid, where the menu came from.
        const a = document.activeElement;
        if (!a || a === document.body || m.contains(a)) this.focusEl.focus({ preventScroll: true });
      });
      if (touch) navigator.vibrate?.(10);
    });
  }

  // --- The keyboard ---

  page() { return Math.max(1, Math.floor(this.viewport() / this.rh) - 1); }

  key(e) {
    if (e.target !== this.focusEl || e.isComposing) return;
    // The keyboard's row shows while the keyboard moves it.
    if (!["Shift", "Control", "Alt", "Meta"].includes(e.key)) this.n.el.classList.add("asuka-grid-keys");
    const count = this.ids.length;
    const mac = apple();
    const primary = mac ? e.metaKey : e.ctrlKey;
    const cur = this.active;
    let to = -2;
    switch (e.key) {
      case "ArrowDown": to = cur < 0 ? 0 : Math.min(cur + 1, count - 1); break;
      case "ArrowUp": to = cur < 0 ? 0 : Math.max(cur - 1, 0); break;
      case "Home": to = 0; break;
      case "End": to = count - 1; break;
      case "PageDown": to = cur < 0 ? 0 : Math.min(cur + this.page(), count - 1); break;
      case "PageUp": to = cur < 0 ? 0 : Math.max(cur - this.page(), 0); break;
      case "Enter": {
        if (e.altKey || e.ctrlKey || e.metaKey) return;
        const f = this.mode === g.Mode.None ? cur : this.sel.first() >= 0 ? this.sel.first() : cur;
        e.preventDefault();
        this.open(f);
        return;
      }
      case " ":
        if (e.altKey || e.ctrlKey || e.metaKey || cur < 0) return;
        e.preventDefault();
        if (this.sel.toggle(cur, this.mode)) this.reportSelection();
        return;
      case "F10":
        if (!e.shiftKey) return;
        this.keyMenu(e);
        return;
      case "a": case "A":
        if (!primary || e.altKey || e.shiftKey) return;
        if (this.mode !== g.Mode.Multiple) return;
        e.preventDefault();
        if (this.sel.all(count, this.mode)) this.reportSelection();
        return;
      default:
        if (this.treeKey(e)) e.preventDefault();
        return;
    }
    if (count <= 0 || e.altKey) return;
    e.preventDefault();
    this.moveTo(to, e.shiftKey, primary);
  }

  /** moveTo moves the keyboard's row to row, selecting as the platform does (Shift extends, the primary modifier moves the focus alone). */
  moveTo(row, shift, primary) {
    if (row < 0) return;
    if (this.sel.moved(row, shift, primary, this.mode)) this.reportSelection();
    this.setActive(row, true);
  }

  treeKey() { return false; }

  // --- Cells ---

  /** rowData: a cell's RowData came (as its row bound, or as its text changed). */
  rowData(c, d) {
    c.data = d;
    this.fill(c.rv);
  }

  viewsChanged(c) { this.fill(c.rv); }

  dispose() {
    for (const c of this.cells) this.h.nodes.delete(c.n.id);
    this.cells.clear();
  }
}

// --- Table ---

class Table extends Grid {
  constructor(h, n) {
    super(h, n);
    this.cols = [];
    this.layout = null; // grid.layoutColumns'
    this.sort = [];
    this.movable = false;
    this.sentLayout = null;
    const t = document.createElement("table");
    t.className = "asuka-table";
    t.setAttribute("role", "grid");
    t.setAttribute("aria-rowcount", "1");
    this.countAttr = "aria-rowcount";
    this.colgroup = document.createElement("colgroup");
    t.appendChild(this.colgroup);
    this.thead = document.createElement("thead");
    this.headRow = document.createElement("tr");
    this.headRow.setAttribute("role", "row");
    this.headRow.setAttribute("aria-rowindex", "1");
    this.thead.appendChild(this.headRow);
    t.appendChild(this.thead);
    this.body = document.createElement("tbody");
    t.appendChild(this.body);
    this.top = this.spacerRow();
    this.bottom = this.spacerRow();
    this.body.append(this.top, this.bottom);
    this.table = t;
    this.focusEl = t;
    this.scroller.appendChild(t);
    this.indicator = div("asuka-col-drop");
    this.indicator.hidden = true;
    this.scroller.appendChild(this.indicator);
    this.wire();
  }

  get headRows() { return 1; }

  spacerRow() {
    const tr = document.createElement("tr");
    tr.className = "asuka-grid-spacer";
    tr.setAttribute("aria-hidden", "true");
    const td = document.createElement("td");
    tr.appendChild(td);
    tr.style.display = "none";
    return tr;
  }

  headHeight() { return this.thead.offsetHeight || 0; }

  prop(p, v) {
    switch (p) {
      case P.Table.Columns: this.setColumns(g.decodeColumns(v)); break;
      case P.Table.Sort: this.sort = g.decodeSort(v); this.paintSort(); break;
      case P.Table.SelectionMode: this.setMode(v); break;
      case P.Table.Selection: this.setSelection(v); break;
      case P.Table.RowHeight: this.setRowHeight(v); break;
      case P.Table.ColumnsMovable: this.movable = !!v; this.n.el.classList.toggle("asuka-columns-movable", this.movable); break;
    }
  }

  setColumns(cols) {
    this.cols = cols;
    this.layout = g.layoutColumns(this.layout, cols);
    this.table.setAttribute("aria-colcount", String(cols.length));
    this.buildHead();
    this.relayout();
  }

  /** relayout applies the columns as shown: their widths, each row's cells in their order; the core hears of it. */
  relayout() {
    const { order, widths } = this.layout;
    const cols = this.colgroup;
    while (cols.children.length < order.length + 1) cols.appendChild(document.createElement("col"));
    while (cols.children.length > order.length + 1) cols.lastChild.remove();
    order.forEach((k, i) => { cols.children[i].style.width = widths[k] + "px"; });
    cols.children[order.length].style.width = ""; // the filler: what's left of the width
    this.sizeTable();
    for (const sp of [this.top, this.bottom]) sp.firstChild.colSpan = order.length + 1;
    for (const rv of [...this.shown.values(), ...this.free]) {
      rv.views.colSpan = order.length + 1;
      this.cellsFor(rv);
      for (const t of rv.tds) t.col = -1;
      if (rv.row >= 0) this.fill(rv);
    }
    this.h.afterCore(() => this.sendLayout());
  }

  sizeTable() {
    if (!this.layout) return;
    const { order, widths } = this.layout;
    let sum = 0;
    for (const k of order) sum += widths[k];
    this.sum = sum;
    this.table.style.width = Math.max(sum, this.scroller.clientWidth || this.n.frame[2]) + "px";
  }

  sendLayout() {
    if (!this.layout || this.n.dying) return;
    const b = g.encodeColumnLayout(this.layout.order, this.layout.widths);
    if (this.sentLayout && this.sentLayout.length === b.length && this.sentLayout.every((x, i) => x === b[i])) return;
    this.sentLayout = b;
    if (this.n.listening(Event.ColumnLayout)) this.h.dispatch(Events.columnLayout(this.n.id, b));
  }

  buildHead() {
    const row = this.headRow;
    row.textContent = "";
    this.ths = [];
    const { order } = this.layout;
    order.forEach((k, i) => {
      const c = this.cols[k];
      const th = document.createElement("th");
      th.setAttribute("role", "columnheader");
      th.setAttribute("aria-colindex", String(i + 1));
      th.style.textAlign = alignCSS[c.align] || "start";
      th.asukaColumn = k;
      const inner = span("asuka-th-inner");
      const title = span("asuka-th-title");
      title.textContent = c.title;
      inner.appendChild(title);
      const ind = span("asuka-sort-indicator");
      ind.setAttribute("aria-hidden", "true");
      inner.appendChild(ind);
      th.appendChild(inner);
      if (c.sortable) {
        th.classList.add("asuka-th-sortable");
        th.tabIndex = 0;
        th.addEventListener("keydown", (e) => {
          if ((e.key === "Enter" || e.key === " ") && !e.altKey && !e.ctrlKey && !e.metaKey) {
            e.preventDefault();
            this.sortBy(k);
          }
        });
      }
      th.addEventListener("pointerdown", (e) => this.headDown(e, th));
      if (c.resizable) {
        const grip = div("asuka-col-resize");
        grip.setAttribute("aria-hidden", "true");
        grip.addEventListener("pointerdown", (e) => this.resizeDown(e, k));
        grip.addEventListener("dblclick", (e) => e.stopPropagation());
        th.appendChild(grip);
      }
      row.appendChild(th);
      this.ths.push(th);
    });
    const filler = document.createElement("th");
    filler.className = "asuka-th-filler";
    filler.setAttribute("aria-hidden", "true");
    row.appendChild(filler);
    this.paintSort();
  }

  /** paintSort draws the primary sort key's indicator, as Sort says (and only as it says). */
  paintSort() {
    if (!this.ths) return;
    const key = this.sort[0];
    for (const th of this.ths) {
      const k = th.asukaColumn;
      const ind = th.querySelector(".asuka-sort-indicator");
      const on = key && key.column === k;
      if (on) th.setAttribute("aria-sort", key.ascending ? "ascending" : "descending");
      else th.removeAttribute("aria-sort");
      th.classList.toggle("asuka-sorted", !!on);
      const want = on ? (key.ascending ? "chevron.up" : "chevron.down") : "";
      if (ind.dataset.symbol !== want) {
        ind.dataset.symbol = want;
        ind.textContent = "";
        const s = want && symbolSVG(want, false);
        if (s) ind.appendChild(s);
      }
    }
  }

  sortBy(k) {
    if (!this.cols[k]?.sortable || !this.n.listening(Event.SortBy)) return;
    this.h.dispatch(Events.sortBy(this.n.id, k));
  }

  // A header pressed: a click sorts by it; dragged (ColumnsMovable), it moves.
  headDown(e, th) {
    if (e.button !== 0 || !e.isPrimary || e.target.closest(".asuka-col-resize")) return;
    // A click on a header leaves the keyboard's focus where it was (the rows).
    if (e.pointerType === "mouse") e.preventDefault();
    const k = th.asukaColumn;
    const from = this.layout.order.indexOf(k);
    const d = { id: e.pointerId, x0: e.clientX, from, to: from, moving: false };
    try { th.setPointerCapture(e.pointerId); } catch {}
    const move = (ev) => {
      if (ev.pointerId !== d.id || !this.movable) return;
      if (!d.moving && Math.abs(ev.clientX - d.x0) <= dragSlop) return;
      if (!d.moving) { d.moving = true; th.classList.add("asuka-th-dragging"); }
      d.to = this.dropAt(ev.clientX, from);
    };
    const up = (ev) => {
      if (ev.pointerId !== d.id) return;
      th.removeEventListener("pointermove", move);
      th.removeEventListener("pointerup", up);
      th.removeEventListener("pointercancel", up);
      th.classList.remove("asuka-th-dragging");
      this.indicator.hidden = true;
      if (ev.type === "pointercancel") return;
      if (d.moving) {
        if (d.to !== d.from) {
          this.layout.order = g.moveColumn(this.layout.order, d.from, d.to);
          this.buildHead();
          this.relayout();
        }
      } else if (document.elementFromPoint?.(ev.clientX, ev.clientY)?.closest?.("th") === th) {
        this.sortBy(k);
      }
    };
    th.addEventListener("pointermove", move);
    th.addEventListener("pointerup", up);
    th.addEventListener("pointercancel", up);
  }

  /** dropAt is where a header dragged to clientX would go, and shows it. */
  dropAt(x, from) {
    const rtl = this.h.rtl || getComputedStyle(this.table).direction === "rtl";
    const rects = this.ths.map((th) => th.getBoundingClientRect());
    // Edges in reading order, as distances toward the end.
    const edges = rects.map((r) => (rtl ? [-r.right, -r.left] : [r.left, r.right]));
    const to = g.dropIndex(edges, from, rtl ? -x : x);
    // The line where it goes: before the header it lands at (or after the last).
    const at = to > from ? to + 1 : to;
    const s = this.scroller.getBoundingClientRect();
    let px;
    if (at >= rects.length) px = rtl ? rects[rects.length - 1].left : rects[rects.length - 1].right;
    else px = rtl ? rects[at].right : rects[at].left;
    const ind = this.indicator;
    ind.hidden = false;
    ind.style.left = px - s.left + this.scroller.scrollLeft - 1 + "px";
    ind.style.top = this.scroller.scrollTop + "px";
    ind.style.height = this.headHeight() + "px";
    return to;
  }

  // A header's edge dragged: the column's width, kept within its bounds; the core hears of it at the end.
  resizeDown(e, k) {
    if (e.button !== 0 || !e.isPrimary) return;
    e.preventDefault();
    e.stopPropagation();
    const grip = e.currentTarget;
    try { grip.setPointerCapture(e.pointerId); } catch {}
    const rtl = getComputedStyle(this.table).direction === "rtl";
    const x0 = e.clientX, w0 = this.layout.widths[k], col = this.cols[k];
    const i = this.layout.order.indexOf(k);
    this.n.el.classList.add("asuka-resizing");
    const move = (ev) => {
      const w = g.clampWidth(w0 + (ev.clientX - x0) * (rtl ? -1 : 1), col);
      this.layout.widths[k] = w;
      this.colgroup.children[i].style.width = w + "px";
      this.sizeTable();
    };
    const up = () => {
      grip.removeEventListener("pointermove", move);
      grip.removeEventListener("pointerup", up);
      grip.removeEventListener("pointercancel", up);
      this.n.el.classList.remove("asuka-resizing");
      this.layout.user.set(col.id, this.layout.widths[k]);
      this.sendLayout();
    };
    grip.addEventListener("pointermove", move);
    grip.addEventListener("pointerup", up);
    grip.addEventListener("pointercancel", up);
  }

  resized() {
    this.sizeTable();
    this.queue();
  }

  // --- Rows ---

  makeRow() {
    const tr = document.createElement("tr");
    tr.className = "asuka-grid-row";
    tr.setAttribute("role", "row");
    const rv = { el: tr, row: -1, cell: null, tds: [], views: document.createElement("td"), filler: document.createElement("td"), viewMode: false };
    tr.asukaRow = rv;
    rv.cell = this.newCell(rv);
    // A row of views: one cell across the row, the core's view in it
    // (its children at their columns' places); a row of text keeps it
    // there out of sight.
    rv.views.className = "asuka-grid-views";
    rv.views.setAttribute("role", "gridcell");
    rv.views.colSpan = (this.layout?.order.length || 0) + 1;
    rv.views.hidden = true;
    rv.views.appendChild(rv.cell.n.el);
    rv.filler.className = "asuka-td-filler";
    rv.filler.setAttribute("aria-hidden", "true");
    tr.append(rv.filler, rv.views);
    this.cellsFor(rv);
    return rv;
  }

  /** cellsFor makes rv's <td>s, one per column shown, before its filler. */
  cellsFor(rv) {
    const n = this.layout?.order.length || 0;
    while (rv.tds.length < n) {
      const td = document.createElement("td");
      td.setAttribute("role", "gridcell");
      td.hidden = rv.viewMode;
      const icon = span("asuka-cell-icon");
      icon.hidden = true;
      const text = span("asuka-cell-text");
      td.append(icon, text);
      rv.tds.push({ td, icon, text, symbol: "", align: "", col: -1 });
      rv.el.insertBefore(td, rv.filler);
    }
    for (const t of rv.tds.splice(n)) t.td.remove();
  }

  placed(rv, r) { rv.el.setAttribute("aria-rowindex", String(r + 2)); }

  /** fill shows rv's cell: its RowData in the <td>s, or its views across the row. */
  fill(rv) {
    const views = this.views(rv);
    if (rv.viewMode !== views) {
      rv.viewMode = views;
      rv.el.classList.toggle("asuka-grid-viewrow", views);
      for (const t of rv.tds) t.td.hidden = views;
      rv.filler.hidden = views;
      rv.views.hidden = !views;
    }
    if (views) return;
    const d = rv.cell.data;
    const order = this.layout?.order || [];
    for (let i = 0; i < order.length && i < rv.tds.length; i++) {
      const k = order[i], t = rv.tds[i], col = this.cols[k];
      const align = alignCSS[col?.align] || "start";
      if (t.align !== align) { t.align = align; t.td.style.textAlign = align; }
      if (t.col !== i) { t.col = i; t.td.setAttribute("aria-colindex", String(i + 1)); }
      const s = d?.cells[k] ?? "";
      if (t.text.textContent !== s) t.text.textContent = s;
      const sym = k === 0 ? d?.symbol || "" : "";
      if (t.symbol !== sym) {
        t.symbol = sym;
        t.icon.textContent = "";
        const svg = sym && symbolSVG(sym, this.h.rtl);
        if (svg) t.icon.appendChild(svg);
        t.icon.hidden = !svg;
      }
    }
  }
}

// --- Outline ---

class Outline extends Grid {
  constructor(h, n) {
    super(h, n);
    this.levels = { depth: new Uint16Array(0), flags: new Uint8Array(0) };
    this.info = g.treeInfo(this.levels.depth);
    this.pendingLevels = null;
    const t = div("asuka-tree");
    t.setAttribute("role", "tree");
    this.countAttr = null;
    this.body = t;
    this.top = div("asuka-grid-spacer");
    this.bottom = div("asuka-grid-spacer");
    this.top.setAttribute("aria-hidden", "true");
    this.bottom.setAttribute("aria-hidden", "true");
    t.append(this.top, this.bottom);
    this.focusEl = t;
    this.scroller.appendChild(t);
    this.wire();
  }

  prop(p, v) {
    switch (p) {
      case P.Outline.Levels: this.pendingLevels = g.decodeLevels(v); this.queue(); break;
      case P.Outline.SelectionMode: this.setMode(v); break;
      case P.Outline.Selection: this.setSelection(v); break;
      case P.Outline.RowHeight: this.setRowHeight(v); break;
    }
  }

  flush() {
    if (this.pendingLevels && !this.pending) {
      // Levels alone: the same rows, expanded otherwise.
      this.levels = this.pendingLevels;
      this.pendingLevels = null;
      this.info = g.treeInfo(this.levels.depth);
      this.rebind = true;
    }
    super.flush();
  }

  updated() {
    if (this.pendingLevels) {
      this.levels = this.pendingLevels;
      this.pendingLevels = null;
      this.info = g.treeInfo(this.levels.depth);
    }
  }

  resized() { this.queue(); }

  depth(r) { return r < this.levels.depth.length ? this.levels.depth[r] : 0; }
  flags(r) { return r < this.levels.flags.length ? this.levels.flags[r] : 0; }

  makeRow() {
    const el = div("asuka-grid-row asuka-tree-row");
    el.setAttribute("role", "treeitem");
    const disc = span("asuka-tree-disclosure");
    disc.setAttribute("aria-hidden", "true");
    const chev = symbolSVG("chevron.right", false);
    if (chev) disc.appendChild(chev);
    const icon = span("asuka-cell-icon");
    icon.hidden = true;
    const text = span("asuka-tree-text");
    const views = span("asuka-tree-views");
    el.append(disc, icon, text, views);
    const rv = { el, row: -1, cell: null, disc, icon, text, views, symbol: "" };
    el.asukaRow = rv;
    rv.cell = this.newCell(rv);
    views.appendChild(rv.cell.n.el);
    return rv;
  }

  placed(rv, r) {
    const d = this.depth(r), f = this.flags(r);
    const el = rv.el;
    el.setAttribute("aria-level", String(d + 1));
    el.setAttribute("aria-setsize", String(this.info.size[r] || 1));
    el.setAttribute("aria-posinset", String(this.info.pos[r] || 1));
    if (f & 1) el.setAttribute("aria-expanded", (f & 2) ? "true" : "false");
    else el.removeAttribute("aria-expanded");
    el.style.paddingInlineStart = d * g.indent + "px";
    rv.disc.classList.toggle("asuka-expandable", (f & 1) !== 0);
    rv.disc.classList.toggle("asuka-expanded", (f & 2) !== 0);
  }

  fill(rv) {
    const views = this.views(rv);
    rv.el.classList.toggle("asuka-tree-viewrow", views);
    rv.views.hidden = !views;
    rv.text.hidden = views;
    if (views) {
      rv.icon.hidden = true;
      return;
    }
    const d = rv.cell.data;
    const s = d?.cells[0] ?? "";
    if (rv.text.textContent !== s) rv.text.textContent = s;
    const sym = d?.symbol || "";
    if (rv.symbol !== sym) {
      rv.symbol = sym;
      rv.icon.textContent = "";
      const svg = sym && symbolSVG(sym, this.h.rtl);
      if (svg) rv.icon.appendChild(svg);
    }
    rv.icon.hidden = !rv.icon.firstChild;
  }

  pressedPart(rv, e) {
    if (!e.target.closest?.(".asuka-tree-disclosure")) return false;
    const f = this.flags(rv.row);
    if (!(f & 1)) return false;
    e.preventDefault();
    this.focusEl.focus({ preventScroll: true });
    this.setActive(rv.row, false);
    this.expand(rv.row, (f & 2) ? 0 : 1);
    return true;
  }

  expand(r, on) {
    if (this.n.listening(Event.Expand)) this.h.dispatch(Events.expand(this.n.id, r, on));
  }

  treeKey(e) {
    if (e.key !== "ArrowRight" && e.key !== "ArrowLeft") return false;
    if (e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return false;
    const r = this.active;
    if (r < 0) return false;
    const rtl = getComputedStyle(this.body).direction === "rtl";
    const into = (e.key === "ArrowRight") !== rtl;
    const a = g.treeKey(into ? "in" : "out", r, this.levels, this.info.parent);
    if (!a) return true;
    if (a.expand != null) this.expand(r, a.expand);
    else this.moveTo(a.move, false, false);
    return true;
  }
}

// --- The cells ---

const cellImpl = {
  create(h, n) { n.el = div("asuka-grid-cell"); },
  common(h, n, p, v) {
    const c = n.gridCell;
    switch (p) {
      case Prop.RowData: c.grid.rowData(c, g.decodeRowData(v)); return true;
      case Prop.Menu: { const m = decodeActions(v); c.menu = m.length ? m : null; return true; }
      case Prop.MenuTrigger: return true; // a right click, a long press, the menu key: always
    }
    return false;
  },
  insert(h, p, c, i) {
    h.placeChild(p, c, i);
    p.gridCell.grid.viewsChanged(p.gridCell);
    return true;
  },
  remove(h, p, c) {
    if (c.el && c.el.parentNode === p.box) c.el.remove();
    p.gridCell.grid.viewsChanged(p.gridCell);
    return true;
  },
  frame(h, n, x, y, w, hh) {
    n.frame = [x, y, w, hh];
    n.placed = true;
    n.el.style.width = w + "px";
    n.el.style.height = hh + "px";
    return true;
  },
  measure() { return [0, 0]; },
};

function gridKind(Kind) {
  return {
    create(h, n) { n.grid = new Kind(h, n); },
    prop(h, n, p, v) { n.grid.prop(p, v); },
    frame(h, n, x, y, w, hh) {
      h.place(n, x, y, w, hh);
      n.grid.resized();
      return true;
    },
    command(h, n, cmd, args) { return n.grid.command(cmd, args); },
    label(h, n, v) {
      n.el.removeAttribute("aria-label");
      if (v) n.grid.focusEl.setAttribute("aria-label", v); else n.grid.focusEl.removeAttribute("aria-label");
    },
    destroy(h, n) {
      n.grid.dispose();
      return false;
    },
    measure() { return [0, 0]; },
  };
}

export const tableKind = gridKind(Table);
export const outlineKind = gridKind(Outline);
