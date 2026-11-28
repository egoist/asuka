// table.c: a Table is a ListView in report view with owner data
// (LVS_OWNERDATA: the control asks for what it shows, LVN_GETDISPINFO,
// and holds no items), an Outline a TreeView. Their rows are a List's
// (the core's list.c): the host binds the rows the control asks for into
// cells it makes (asuka_list_bind), whose RowData is the row's text (a
// string a column and a symbol), which the control draws itself, or
// whose children are the row's views, a window over its row. Cells are
// a pool of slots, the least recently used rebound as rows scroll into
// view (the ones in view never), so scrolling allocates nothing once the
// pool is the view's size.
//
// Both are drawn in custom draw as Windows 11's lists are (WinUI's
// ListView and TreeView, Explorer's details view): a row's rounded
// highlight when the pointer is over it or it's selected, the accent's
// pill at its start when it's selected, the header as Explorer's (the
// sort's chevron at the top of its column's), the outline's chevrons
// and no lines, in Segoe UI Variable and Segoe Fluent Icons, light or
// dark. The controls keep their own keyboard (arrows, Home and End,
// pages; Shift and Ctrl selecting many) and screen readers' providers
// (UI Automation's table, grid and tree).
//
// What the core decides, they only show: the sort (Sort: a header's
// click is SortBy), the selection (Selection, set without a report; the
// user's is RowsSelected), an outline's expanded rows (Levels: the
// user's expanding is Expand, and the TreeView expands only as Levels
// says). Right to left, both controls are mirrored, as Windows mirrors
// them (a Table's first column at its right, an Outline's rows indented
// from the right, the scroll bar at the left), made again if the
// direction changes; what they draw is laid out on the screen's pixels
// (GDI+ doesn't follow a mirrored DC), and a row's views are placed by
// the core, physically.
#include "win.h"

enum {
	WM_TBL_REPORT = WM_APP + 40,  // the user's selection, reported once it's settled
	WM_TBL_LAYOUT,                // the columns as shown, after a resize or a move
	WM_TBL_SYNC,                  // rows of views placed over their rows
	WM_TBL_FLUSH,                 // rows waiting while a control drew
};

static int drawing;  // controls in custom draw: their rows wait

#define LEAD 4.0f        // the highlight's inset at the row's ends (points)
#define PAD 12.0f        // a cell's text from its column's edges
#define ICON 16.0f       // a row's symbol
#define INDENT 16.0f     // an outline's indent per level
#define DISCLOSURE 20.0f // its chevron's room
#define OUTLINE_LEAD 8.0f // an outline's rows' content from its start: after the selection's pill
#define HEADER_H 32.0f   // the header's height

// ---- Rows bound into cells ----

typedef struct {
	node *cell;      // KIND_TABLE_CELL
	int64_t row;     // the row bound (-1: none)
	int64_t item;    // its item's ID, as bound
	uint32_t stamp;  // when it was last asked for
	wchar_t *text;   // RowData's strings, each NUL-ended
	uint32_t ntext, captext;
	uint32_t *off;  // where each starts in text
	uint32_t nstr, capstr;
	wchar_t glyph;  // its symbol's (0: none)
	float fw, fh;   // its frame's size (points)
} tslot;

typedef struct {
	char *id;
	wchar_t *title;
	float width, min, max;  // the app's (points; 0: the host's)
	uint8_t flags, align;   // 1 resizable, 2 sortable; TextAlign
	bool user_sized;        // the user resized it: its width stays
} tcol;

// An index of IDs, rebuilt as they change: an item's row.
typedef struct {
	uint64_t *keys;
	uint32_t *vals;  // row + 1 (0: empty)
	uint32_t cap;
} idmap;

struct wh_table {
	node *n;
	bool outline;
	HWND ctl, header;
	int64_t *ids;  // the rows' items, as shown
	uint32_t nids;
	int64_t *pend;  // ListUpdate's, applied once the core has returned
	uint32_t npend;
	bool updated, queued;
	tslot **slots;
	uint32_t nslots, clock;
	uint32_t nviews;  // slots with a window (rows of views)
	int32_t mode;     // AK_SELECTION_MODE_*
	float row_h;
	HIMAGELIST il;  // its height is the rows' (report view)
	int hot;        // the row under the pointer (-1: none)
	bool tracking;
	bool setting;   // the host is changing the selection or the tree: no report
	bool report_posted;
	ak_buf sel_known;  // the selection as last applied or reported (runs)
	ak_buf sel_want;   // Selection, applied once the rows are
	bool sel_dirty;
	ak_buf scratch;
	rgba bg;
	// A Table's.
	tcol *cols;
	uint32_t ncols;
	int *shown;  // the display order from the start (model indices)
	bool user_order;
	int sort_col;  // -1: none
	bool sort_asc;
	bool movable;
	ak_buf layout_sent;
	int hot_header;
	// An Outline's.
	uint8_t *levels;  // per row: u16 depth, u8 flags (as Levels has them)
	uint8_t *pend_levels;
	uint32_t npend_levels;
	bool levels_new;
	HTREEITEM *items;  // each row's
	idmap rows;        // a row by its item's ID
	int64_t sel_item;  // the item selected (an outline's; -1 none)
	bool sel_has;
	bool building;  // its tree changing: no row bound, none drawn by its place
	int printing;   // drawing for WM_PRINTCLIENT: what's behind a row's views
	bool watch_layout;  // the user moving or resizing a column: reported once the header shows it
};

static tslot *slot_of(node *cell) { return cell->kind == KIND_TABLE_CELL ? cell->host_data : NULL; }
node *wh_table_of(node *cell) { return cell->table ? cell->table->n : NULL; }
static bool rtl(const wh_table *t) { return wh_is_rtl(t->n); }
static int px(const wh_table *t, float v) { return wh_px(t->n, v); }

static void idmap_build(idmap *m, const int64_t *ids, uint32_t n) {
	uint32_t cap = 16;
	while (cap < n * 2) cap *= 2;
	if (m->cap != cap) {
		free(m->keys);
		free(m->vals);
		m->keys = wh_alloc(cap * sizeof *m->keys);
		m->vals = wh_alloc(cap * sizeof *m->vals);
		m->cap = cap;
	} else {
		memset(m->vals, 0, cap * sizeof *m->vals);
	}
	for (uint32_t i = 0; i < n; i++) {
		uint64_t k = (uint64_t)ids[i];
		for (uint32_t j = (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 32) & (cap - 1);; j = (j + 1) & (cap - 1)) {
			if (!m->vals[j]) {
				m->keys[j] = k, m->vals[j] = i + 1;
				break;
			}
			if (m->keys[j] == k) break;
		}
	}
}

static int64_t idmap_get(const idmap *m, int64_t id) {
	if (!m->cap) return -1;
	uint64_t k = (uint64_t)id;
	for (uint32_t j = (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 32) & (m->cap - 1);; j = (j + 1) & (m->cap - 1)) {
		if (!m->vals[j]) return -1;
		if (m->keys[j] == k) return (int64_t)m->vals[j] - 1;
	}
}

static const wchar_t *slot_str(const tslot *s, uint32_t i) {
	return s && i < s->nstr ? s->text + s->off[i] : L"";
}

// ---- The pool ----

static int page_rows(wh_table *t) {
	RECT r;
	GetClientRect(t->ctl, &r);
	int h = px(t, t->row_h > 0 ? t->row_h : 32);
	return h > 0 ? r.bottom / h + 2 : 32;
}

static int64_t top_row(wh_table *t) {
	if (!t->outline) return ListView_GetTopIndex(t->ctl);
	HTREEITEM h = TreeView_GetFirstVisible(t->ctl);
	if (!h) return 0;
	TVITEMW it = {.mask = TVIF_PARAM, .hItem = h};
	TreeView_GetItem(t->ctl, &it);
	int64_t r = idmap_get(&t->rows, (int64_t)it.lParam);
	return r < 0 ? 0 : r;
}

static tslot *new_slot(wh_table *t) {
	tslot *s = wh_alloc(sizeof *s);
	node *c = wh_node_new(wh_host_id(), KIND_TABLE_CELL);
	c->k = t->n->k, c->dir = t->n->dir;
	c->host_data = s;
	c->table = t;
	s->cell = c;
	s->row = -1;
	wh_node_add(c);
	t->slots = wh_realloc(t->slots, (t->nslots + 1) * sizeof *t->slots);
	t->slots[t->nslots++] = s;
	return s;
}

static tslot *find_slot(wh_table *t, int64_t row) {
	for (uint32_t i = 0; i < t->nslots; i++)
		if (t->slots[i]->row == row) return t->slots[i];
	return NULL;
}

static void hide_views(tslot *s) {
	if (s->cell->hwnd) ShowWindow(s->cell->hwnd, SW_HIDE);
}

// bind_row is the slot row is bound into: one already, or the least recently
// used of those out of view (a new one while the pool is smaller than
// three views of rows), bound to it now. NULL while a batch is applied:
// the core can't be called then, and the row is drawn again after.
static void redraw_later(void *ctx);

static tslot *bind_row(wh_table *t, int64_t row) {
	if (row < 0 || row >= (int64_t)t->nids || t->building) return NULL;
	tslot *s = find_slot(t, row);
	if (s) {
		s->stamp = ++t->clock;
		return s;
	}
	if (H.applying) {
		wh_after_core(redraw_later, (void *)(uintptr_t)t->n->id);
		return NULL;
	}
	int64_t top = top_row(t), page = page_rows(t);
	uint32_t cap = (uint32_t)wh_maxi(48, (int)page * 3);
	if (t->nslots < cap) {
		s = new_slot(t);
	} else {
		for (uint32_t i = 0; i < t->nslots; i++) {
			tslot *c = t->slots[i];
			bool in_view = c->row >= top && c->row < top + page;
			if (in_view) continue;
			if (!s || c->stamp < s->stamp) s = c;
		}
		if (!s) s = new_slot(t);  // all in view: the view grew
	}
	s->row = row;
	s->item = t->ids[row];
	s->stamp = ++t->clock;
	s->nstr = 0, s->ntext = 0, s->glyph = 0;
	hide_views(s);
	wh_core_begin();
	asuka_list_bind(t->n->id, row, s->cell->id);  // its props come in the batch it applies
	wh_core_end();
	return s->row == row ? s : NULL;
}

static void redraw_later(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (n && n->table && n->table->ctl) InvalidateRect(n->table->ctl, NULL, FALSE);
}

static void invalidate_row(wh_table *t, int64_t row) {
	if (row < 0 || row >= (int64_t)t->nids || !t->ctl) return;
	if (t->building) {
		InvalidateRect(t->ctl, NULL, FALSE);
		return;
	}
	if (!t->outline) {
		ListView_RedrawItems(t->ctl, (int)row, (int)row);
		return;
	}
	RECT r;
	if (t->items && t->items[row] && TreeView_GetItemRect(t->ctl, t->items[row], &r, FALSE)) InvalidateRect(t->ctl, &r, FALSE);
}

// wh_table_row_data takes a bound cell's RowData: u32 count, u32-length
// strings, a u32-length symbol.
void wh_table_row_data(node *cell, const ak_value *v) {
	tslot *s = slot_of(cell);
	wh_table *t = cell->table;
	if (!s || !t) return;
	const uint8_t *b = (const uint8_t *)v->s.p;
	size_t len = v->s.len, o = 4;
	uint32_t count = len >= 4 ? wh_u32(b) : 0;
	s->nstr = 0, s->ntext = 0, s->glyph = 0;
	for (uint32_t i = 0; i <= count && o + 4 <= len; i++) {
		uint32_t l = wh_u32(b + o);
		o += 4;
		if (o + l > len) break;
		const char *p = (const char *)b + o;
		o += l;
		if (i == count) {
			// The symbol.
			char name[64];
			snprintf(name, sizeof name, "%.*s", (int)wh_mini((int)l, 63), p);
			s->glyph = l ? wh_directed_glyph(name, rtl(t)) : 0;
			if (l && !s->glyph) wh_warn_symbol(name);
			break;
		}
		int wn = l ? MultiByteToWideChar(CP_UTF8, 0, p, (int)l, NULL, 0) : 0;
		if (s->ntext + (uint32_t)wn + 1 > s->captext) {
			s->captext = (s->ntext + (uint32_t)wn + 1) * 2;
			s->text = wh_realloc(s->text, s->captext * sizeof *s->text);
		}
		if (s->nstr == s->capstr) {
			s->capstr = s->capstr ? s->capstr * 2 : 8;
			s->off = wh_realloc(s->off, s->capstr * sizeof *s->off);
		}
		s->off[s->nstr++] = s->ntext;
		if (wn) MultiByteToWideChar(CP_UTF8, 0, p, (int)l, s->text + s->ntext, wn);
		s->ntext += (uint32_t)wn;
		s->text[s->ntext++] = 0;
	}
	invalidate_row(t, s->row);
}

// ---- Rows of views: their cells' windows over the rows ----

void wh_table_cell_frame(node *cell, const float f[4]) {
	tslot *s = slot_of(cell);
	if (!s) return;
	s->fw = f[2], s->fh = f[3];
	if (cell->hwnd && cell->table && cell->table->ctl) PostMessageW(cell->table->ctl, WM_TBL_SYNC, 0, 0);
}

void wh_table_cell_window(node *cell) {
	wh_table *t = cell->table;
	if (!t || !t->ctl) return;
	cell->hwnd = wh_make_view(0);
	ShowWindow(cell->hwnd, SW_HIDE);
	SetParent(cell->hwnd, t->ctl);
	wh_bind(cell, cell->hwnd);
	t->nviews++;
	PostMessageW(t->ctl, WM_TBL_SYNC, 0, 0);
}

static int row_depth(const wh_table *t, int64_t row) {
	if (!t->levels || row < 0 || row >= (int64_t)t->nids) return 0;
	const uint8_t *l = t->levels + row * 3;
	return l[0] | l[1] << 8;
}

static uint8_t row_flags(const wh_table *t, int64_t row) {
	if (!t->levels || row < 0 || row >= (int64_t)t->nids) return 0;
	return t->levels[row * 3 + 2];
}

static bool row_rect(wh_table *t, int64_t row, RECT *r) {
	if (row < 0 || row >= (int64_t)t->nids) return false;
	if (!t->outline) return ListView_GetItemRect(t->ctl, (int)row, r, LVIR_BOUNDS) != 0;
	return t->items && t->items[row] && TreeView_GetItemRect(t->ctl, t->items[row], r, FALSE);
}

// sync_views places each row of views' cell over its row (an outline's
// after its disclosure and indent), the ones out of view hidden.
static void sync_views(wh_table *t) {
	if (!t->nviews || !t->ctl) return;
	RECT client;
	GetClientRect(t->ctl, &client);
	for (uint32_t i = 0; i < t->nslots; i++) {
		tslot *s = t->slots[i];
		HWND w = s->cell->hwnd;
		if (!w) continue;
		RECT r;
		if (s->row < 0 || !s->cell->nkids || !row_rect(t, s->row, &r) || r.bottom <= 0 || r.top >= client.bottom) {
			ShowWindow(w, SW_HIDE);
			continue;
		}
		int x = r.left, width = wh_maxi(px(t, s->fw), r.right - r.left);
		if (t->outline) {
			width = px(t, s->fw);
			x = px(t, OUTLINE_LEAD + DISCLOSURE + INDENT * (float)row_depth(t, s->row));  // from its start, mirrored or not
		}
		SetWindowPos(w, HWND_TOP, x, r.top, width, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
	}
}

// ---- Selection ----

static void put_run(ak_buf *b, uint32_t first, uint32_t len) {
	ak_buf_u32(b, first);
	ak_buf_u32(b, len);
}

// selected_runs encodes the rows the control has selected as runs.
static void selected_runs(wh_table *t, ak_buf *b) {
	b->len = 0;
	ak_buf_u32(b, 0);
	uint32_t runs = 0;
	if (t->mode == AK_SELECTION_MODE_NONE) return;
	if (t->outline) {
		HTREEITEM h = TreeView_GetSelection(t->ctl);
		if (h) {
			TVITEMW it = {.mask = TVIF_PARAM, .hItem = h};
			TreeView_GetItem(t->ctl, &it);
			int64_t r = idmap_get(&t->rows, (int64_t)it.lParam);
			if (r >= 0) put_run(b, (uint32_t)r, 1), runs = 1;
		}
	} else {
		int first = -1, last = -2;
		for (int i = ListView_GetNextItem(t->ctl, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(t->ctl, i, LVNI_SELECTED)) {
			if (i == last + 1) {
				last = i;
				continue;
			}
			if (first >= 0) put_run(b, (uint32_t)first, (uint32_t)(last - first + 1)), runs++;
			first = last = i;
		}
		if (first >= 0) put_run(b, (uint32_t)first, (uint32_t)(last - first + 1)), runs++;
	}
	memcpy(b->data, &runs, 4);
}

static bool same_buf(const ak_buf *a, const ak_buf *b) { return a->len == b->len && (!a->len || memcmp(a->data, b->data, a->len) == 0); }

// report_now reports the user's selection, if it changed.
static void report_now(wh_table *t) {
	t->report_posted = false;
	if (t->setting || !t->ctl) return;
	selected_runs(t, &t->scratch);
	if (same_buf(&t->scratch, &t->sel_known)) return;
	t->sel_known.len = 0;
	ak_buf_put(&t->sel_known, t->scratch.data, t->scratch.len);
	wh_emit_bytes(t->n, AK_EVENT_ROWS_SELECTED, t->scratch.data, t->scratch.len);
}

static void report_later(wh_table *t) {
	if (t->setting || t->report_posted) return;
	t->report_posted = true;
	PostMessageW(t->ctl, WM_TBL_REPORT, 0, 0);
}

// apply_selection selects exactly Selection's rows, reporting nothing.
static void apply_selection(wh_table *t) {
	t->sel_dirty = false;
	const uint8_t *b = t->sel_want.data;
	size_t len = t->sel_want.len;
	uint32_t runs = len >= 4 ? wh_u32(b) : 0;
	t->setting = true;
	if (t->outline) {
		HTREEITEM h = NULL;
		if (runs && len >= 12) {
			uint32_t r = wh_u32(b + 4);
			if (r < t->nids && t->items) h = t->items[r];
		}
		t->sel_has = h != NULL;
		t->sel_item = h ? t->ids[wh_u32(b + 4)] : -1;
		TreeView_SelectItem(t->ctl, h);
	} else {
		ListView_SetItemState(t->ctl, -1, 0, LVIS_SELECTED);
		if (runs == 1 && len >= 12 && wh_u32(b + 4) == 0 && wh_u32(b + 8) >= t->nids && t->nids) {
			ListView_SetItemState(t->ctl, -1, LVIS_SELECTED, LVIS_SELECTED);  // all of them
		} else {
			for (uint32_t k = 0; k < runs && 4 + k * 8 + 8 <= len; k++) {
				uint32_t first = wh_u32(b + 4 + k * 8), n = wh_u32(b + 8 + k * 8);
				for (uint32_t i = first; i < first + n && i < t->nids; i++) ListView_SetItemState(t->ctl, (int)i, LVIS_SELECTED, LVIS_SELECTED);
			}
		}
	}
	t->setting = false;
	selected_runs(t, &t->sel_known);
}

// ---- Columns ----

static void free_cols(tcol *c, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		free(c[i].id);
		free(c[i].title);
	}
	free(c);
}

static uint32_t decode_cols(const uint8_t *b, size_t len, tcol **out) {
	*out = NULL;
	if (len < 4) return 0;
	uint32_t n = wh_u32(b), got = 0;
	size_t o = 4;
	tcol *c = wh_alloc((n ? n : 1) * sizeof *c);
	for (uint32_t i = 0; i < n; i++) {
		if (o + 4 > len) break;
		uint32_t l = wh_u32(b + o);
		if (o + 4 + l + 4 > len) break;
		char *id = wh_strndup((const char *)b + o + 4, l);
		o += 4 + l;
		uint32_t tl = wh_u32(b + o);
		if (o + 4 + tl + 14 > len) {
			free(id);
			break;
		}
		c[got].id = id;
		c[got].title = wh_wide((const char *)b + o + 4, (int)tl);
		o += 4 + tl;
		c[got].width = wh_f32(b + o), c[got].min = wh_f32(b + o + 4), c[got].max = wh_f32(b + o + 8);
		c[got].flags = b[o + 12], c[got].align = b[o + 13];
		o += 14;
		got++;
	}
	*out = c;
	return got;
}

static float col_min(const tcol *c) { return c->min > 0 ? c->min : 32; }
static float col_width(const tcol *c) {
	float w = c->width > 0 ? c->width : 100;
	w = wh_maxf(w, col_min(c));
	return c->max > 0 ? wh_minf(w, c->max) : w;
}

static int col_fmt(const tcol *c, int model) {
	if (model == 0) return LVCFMT_LEFT;  // a ListView's first column is
	return c->align == AK_TEXT_ALIGN_CENTER ? LVCFMT_CENTER : c->align == AK_TEXT_ALIGN_END ? LVCFMT_RIGHT : LVCFMT_LEFT;
}

// apply_order shows the columns in display order shown (from the start:
// the left, or the right of a mirrored ListView, right to left).
static void apply_order(wh_table *t) {
	if (!t->ncols) return;
	int *o = wh_alloc(t->ncols * sizeof *o);
	for (uint32_t i = 0; i < t->ncols; i++) o[i] = t->shown[i];
	ListView_SetColumnOrderArray(t->ctl, (int)t->ncols, o);
	free(o);
	InvalidateRect(t->ctl, NULL, FALSE);
}

static void read_order(wh_table *t) {
	if (!t->ncols) return;
	int *o = wh_alloc(t->ncols * sizeof *o);
	if (ListView_GetColumnOrderArray(t->ctl, (int)t->ncols, o))
		for (uint32_t i = 0; i < t->ncols; i++) t->shown[i] = o[i];
	free(o);
}

// report_layout sends ColumnLayout, the columns as shown from the start
// and their widths, if it changed.
static void report_layout(wh_table *t) {
	if (t->outline || !t->ncols || !node_listening(t->n, AK_EVENT_COLUMN_LAYOUT)) return;  // sent once the core listens
	read_order(t);
	ak_buf *b = &t->scratch;
	b->len = 0;
	ak_buf_u32(b, t->ncols);
	for (uint32_t i = 0; i < t->ncols; i++) {
		ak_buf_u32(b, (uint32_t)t->shown[i]);
		float w = wh_pt(t->n, ListView_GetColumnWidth(t->ctl, t->shown[i]));
		uint32_t bits;
		memcpy(&bits, &w, 4);
		ak_buf_u32(b, bits);
	}
	if (same_buf(b, &t->layout_sent)) return;
	t->watch_layout = false;
	t->layout_sent.len = 0;
	ak_buf_put(&t->layout_sent, b->data, b->len);
	wh_emit_bytes(t->n, AK_EVENT_COLUMN_LAYOUT, b->data, b->len);
	PostMessageW(t->ctl, WM_TBL_SYNC, 0, 0);
}

static void apply_sort_arrows(wh_table *t) {
	if (!t->header) return;
	for (uint32_t i = 0; i < t->ncols; i++) {
		HDITEMW hi = {.mask = HDI_FORMAT};
		Header_GetItem(t->header, (int)i, &hi);
		hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
		if ((int)i == t->sort_col) hi.fmt |= t->sort_asc ? HDF_SORTUP : HDF_SORTDOWN;
		Header_SetItem(t->header, (int)i, &hi);
	}
	InvalidateRect(t->header, NULL, FALSE);
}

static void insert_cols(wh_table *t) {
	while (ListView_DeleteColumn(t->ctl, 0)) {
	}
	for (uint32_t i = 0; i < t->ncols; i++) {
		LVCOLUMNW c = {.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM};
		c.pszText = t->cols[i].title;
		c.cx = px(t, col_width(&t->cols[i]));
		c.fmt = col_fmt(&t->cols[i], (int)i);
		c.iSubItem = (int)i;
		ListView_InsertColumn(t->ctl, (int)i, &c);
	}
}

// set_columns takes Columns: the same IDs keep the user's order and
// widths (the app's titles, flags and bounds are taken); others are new
// columns, in the app's order at its widths.
static void set_columns(wh_table *t, const uint8_t *b, size_t len) {
	tcol *nc;
	uint32_t n = decode_cols(b, len, &nc);
	bool same = n == t->ncols;
	int *from = wh_alloc((n ? n : 1) * sizeof *from);  // each new column's old index
	for (uint32_t i = 0; i < n && same; i++) {
		from[i] = -1;
		for (uint32_t j = 0; j < t->ncols; j++)
			if (!strcmp(nc[i].id, t->cols[j].id)) from[i] = (int)j;
		if (from[i] < 0) same = false;
	}
	if (same) {
		bool moved = false;
		for (uint32_t i = 0; i < n; i++) moved |= from[i] != (int)i;
		int *widths = wh_alloc((n ? n : 1) * sizeof *widths);
		int *shown = wh_alloc((n ? n : 1) * sizeof *shown);
		for (uint32_t i = 0; i < n; i++) {
			tcol *o = &t->cols[from[i]];
			nc[i].user_sized = o->user_sized;
			widths[i] = o->user_sized ? ListView_GetColumnWidth(t->ctl, from[i]) : px(t, col_width(&nc[i]));
		}
		// The order shown, in the new model's indices.
		for (uint32_t d = 0; d < n; d++)
			for (uint32_t i = 0; i < n; i++)
				if (from[i] == t->shown[d]) shown[d] = (int)i;
		free_cols(t->cols, t->ncols);
		t->cols = nc;
		if (moved) insert_cols(t);
		for (uint32_t i = 0; i < n; i++) {
			LVCOLUMNW c = {.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH, .pszText = nc[i].title, .fmt = col_fmt(&nc[i], (int)i), .cx = widths[i]};
			ListView_SetColumn(t->ctl, (int)i, &c);
		}
		if (!t->user_order) for (uint32_t i = 0; i < n; i++) shown[i] = (int)i;
		memcpy(t->shown, shown, n * sizeof *shown);
		free(widths);
		free(shown);
	} else {
		free_cols(t->cols, t->ncols);
		t->cols = nc;
		t->ncols = n;
		free(t->shown);
		t->shown = wh_alloc((n ? n : 1) * sizeof *t->shown);
		for (uint32_t i = 0; i < n; i++) t->shown[i] = (int)i;
		t->user_order = false;
		insert_cols(t);
	}
	free(from);
	apply_order(t);
	apply_sort_arrows(t);
	report_layout(t);
}

static void set_sort(wh_table *t, const uint8_t *b, size_t len) {
	t->sort_col = -1;
	if (len >= 9 && wh_u32(b) > 0) {
		t->sort_col = (int)wh_u32(b + 4);
		t->sort_asc = b[8] != 0;
	}
	apply_sort_arrows(t);
}

// ---- Applying the rows ----

static void outline_apply(wh_table *t);

static void flush(wh_table *t) {
	if (!t->ctl) return;
	if (t->updated || t->levels_new) {
		if (t->outline) {
			outline_apply(t);
		} else {
			t->updated = false;
			free(t->ids);
			t->ids = t->pend, t->nids = t->npend;
			t->pend = NULL, t->npend = 0;
			// The cells whose rows hold other items now are bound again as
			// they're asked for.
			for (uint32_t i = 0; i < t->nslots; i++) {
				tslot *s = t->slots[i];
				if (s->row >= 0 && (s->row >= (int64_t)t->nids || t->ids[s->row] != s->item)) {
					s->row = -1;
					hide_views(s);
				}
			}
			t->setting = true;
			ListView_SetItemCountEx(t->ctl, (int)t->nids, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
			t->setting = false;
			InvalidateRect(t->ctl, NULL, FALSE);
			if (t->hot >= (int)t->nids) t->hot = -1;
		}
		selected_runs(t, &t->sel_known);
	}
	if (t->sel_dirty) apply_selection(t);
	sync_views(t);
}

static void flush_later(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (!n || !n->table || n->kind == KIND_TABLE_CELL) return;
	n->table->queued = false;
	if (drawing) {
		PostMessageW(n->table->ctl, WM_TBL_FLUSH, 0, 0);  // not while a control draws its rows
		return;
	}
	flush(n->table);
}

static void queue(wh_table *t) {
	if (t->queued) return;
	t->queued = true;
	wh_after_core(flush_later, (void *)(uintptr_t)t->n->id);
}

// wh_table_update takes ListUpdate's IDs (u32 count, u64 each; a list's
// sections after, which a table has none of), applied once the core has
// returned.
void wh_table_update(node *n, const uint8_t *args, size_t len) {
	wh_table *t = n->table;
	if (len < 4) return;
	uint32_t count = wh_u32(args);
	free(t->pend);
	t->pend = wh_alloc((count ? count : 1) * sizeof *t->pend);
	t->npend = 0;
	for (size_t o = 4; t->npend < count && o + 8 <= len; o += 8) t->pend[t->npend++] = (int64_t)wh_u64(args + o);
	t->updated = true;
	queue(t);
}

// ---- Drawing ----
//
// A row (or a header's column) is drawn into a bitmap of its size, in
// physical pixels, then copied to the control: right to left the control
// is mirrored (its first column at its right, its rows indented from the
// right, its scroll bar at its left, Windows' own), which GDI+ doesn't
// follow, so what's drawn is laid out for the screen and copied without
// being mirrored again.

static struct {
	HDC dc;
	HBITMAP bmp;
	HGDIOBJ old;
	int w, h;
} canvas;

// canvas_for is a memory DC of at least w by h pixels (one, grown, for
// every row: drawing allocates nothing as rows scroll).
static HDC canvas_for(int w, int h) {
	if (w > canvas.w || h > canvas.h || !canvas.dc) {
		if (canvas.dc) {
			SelectObject(canvas.dc, canvas.old);
			DeleteObject(canvas.bmp);
			DeleteDC(canvas.dc);
		}
		canvas.w = wh_maxi(w, canvas.w), canvas.h = wh_maxi(h, canvas.h);
		BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), canvas.w, -canvas.h, 1, 32, BI_RGB}};
		void *bits;
		canvas.dc = CreateCompatibleDC(NULL);
		canvas.bmp = CreateDIBSection(canvas.dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
		canvas.old = SelectObject(canvas.dc, canvas.bmp);
	}
	return canvas.dc;
}

// canvas_put copies the canvas's w by h pixels to r in dc, as they are
// (a mirrored DC's not mirrored again).
static void canvas_put(HDC dc, RECT r) {
	DWORD rop = SRCCOPY | ((GetLayout(dc) & LAYOUT_RTL) ? NOMIRRORBITMAP : 0);
	BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, canvas.dc, 0, 0, rop);
}

static bool mirrored(const wh_table *t) { return (GetWindowLongPtrW(t->ctl, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0; }

// physical is a span [a, b) of a row w pixels wide, as the control
// places it (mirrored or not), on the screen from the row's left.
static void physical(const wh_table *t, int w, int a, int b, int *x0, int *x1) {
	if (mirrored(t)) *x0 = w - b, *x1 = w - a;
	else *x0 = a, *x1 = b;
}

static HFONT header_font(wh_table *t) {
	font_spec s = wh_spec(AK_TEXT_STYLE_CAPTION, 0, 0, NULL);
	s.k = wh_scale_of(t->n);
	return wh_font(s);
}

static UINT align_format(wh_table *t, uint8_t align) {
	bool r = rtl(t);
	UINT f = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (r ? DT_RTLREADING : 0);
	if (align == AK_TEXT_ALIGN_CENTER) return f | DT_CENTER;
	if (align == AK_TEXT_ALIGN_END) return f | (r ? DT_LEFT : DT_RIGHT);
	return f | (r ? DT_RIGHT : DT_LEFT);
}

static rgba text_color(wh_table *t) { return IsWindowEnabled(t->ctl) ? H.theme.label : H.theme.disabled; }

// paint_highlight draws a row's highlight in a w by h canvas: the subtle
// fill under the pointer or selected, the accent's pill at the start of
// a selected one, a focus ring where the keyboard is.
static void paint_highlight(wh_table *t, HDC dc, int width, int height, bool sel, bool hot, bool focus) {
	if (!sel && !hot && !focus) return;
	float k = wh_scale_of(t->n);
	float x = LEAD * k, y = 2 * k, w = (float)width - 2 * LEAD * k, h = (float)height - 4 * k;
	if (w <= 0 || h <= 0) return;
	gfx g = g_new(dc);
	rgba fill = H.theme.subtle_hot;
	if (sel && hot) fill.a = (uint8_t)wh_mini(255, fill.a * 2);
	else if (sel) fill.a = (uint8_t)wh_mini(255, fill.a * 3 / 2);
	if (sel || hot) g_fill_round_rect(g, rgba_argb(fill), x, y, w, h, 4 * k);
	if (sel) {
		float pw = 3 * k, ph = wh_minf(16 * k, h / 2), px0 = rtl(t) ? x + w - pw : x;
		g_fill_round_rect(g, rgba_argb(H.theme.accent), px0, y + (h - ph) / 2, pw, ph, pw / 2);
	}
	if (focus) g_stroke_round_rect(g, rgba_argb(H.theme.label), 2 * k, x + k, y + k, w - 2 * k, h - 2 * k, 4 * k);
	g_close(g);
}

// views_follow has a row of views' cell drawn again as its row is: what
// it shows behind its views (its highlight) is the row's. Not while the
// cell draws the row behind it (WM_PRINTCLIENT).
static void views_follow(wh_table *t, tslot *s) {
	if (s && s->cell->hwnd && !t->printing && IsWindowVisible(s->cell->hwnd)) RedrawWindow(s->cell->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static void draw_glyph(wh_table *t, HDC dc, wchar_t glyph, RECT r, rgba c, float size) {
	HGDIOBJ old = SelectObject(dc, wh_icon_font(t->n, size));
	SetTextColor(dc, rgba_ref(c));
	DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old);
}

static bool focused_view(wh_table *t) { return GetFocus() == t->ctl && wh_keyboard_cues(t->ctl); }

// cell_text draws a cell's text in cr (the symbol first, at its start),
// in the canvas.
static void cell_text(wh_table *t, HDC dc, RECT cr, wchar_t glyph, rgba glyph_color, const wchar_t *text, uint8_t align) {
	if (glyph) {
		int icon = px(t, ICON);
		RECT ir = cr;
		if (rtl(t)) ir.left = ir.right - icon, cr.right -= icon + px(t, 8);
		else ir.right = ir.left + icon, cr.left += icon + px(t, 8);
		draw_glyph(t, dc, glyph, ir, glyph_color, 12);
	}
	if (!text || !*text || cr.right <= cr.left) return;
	HGDIOBJ old = SelectObject(dc, wh_node_font(t->n));
	SetTextColor(dc, rgba_ref(text_color(t)));
	DrawTextW(dc, text, -1, &cr, align_format(t, align));
	SelectObject(dc, old);
}

// draw_table_row draws a Table's row: its highlight, then each column's
// text (the first's after the row's symbol), where the header has it.
static void draw_table_row(wh_table *t, HDC dst, int row) {
	RECT r;
	if (!ListView_GetItemRect(t->ctl, row, &r, LVIR_BOUNDS)) return;
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	HDC dc = canvas_for(w, h);
	RECT all = {0, 0, w, h};
	FillRect(dc, &all, wh_brush(t->bg));
	UINT st = ListView_GetItemState(t->ctl, row, LVIS_SELECTED | LVIS_FOCUSED);
	bool sel = t->mode != AK_SELECTION_MODE_NONE && (st & LVIS_SELECTED);
	paint_highlight(t, dc, w, h, sel, row == t->hot, (st & LVIS_FOCUSED) && focused_view(t));
	tslot *s = bind_row(t, row);
	if (s) {
		views_follow(t, s);
		SetBkMode(dc, TRANSPARENT);
		for (uint32_t c = 0; c < t->ncols && c < s->nstr; c++) {
			RECT hr;
			if (!Header_GetItemRect(t->header, (int)c, &hr)) continue;
			int x0, x1;
			physical(t, w, hr.left, hr.right, &x0, &x1);
			RECT cr = {x0 + px(t, PAD), 0, x1 - px(t, PAD), h};
			cell_text(t, dc, cr, c == 0 ? s->glyph : 0, H.theme.secondary, slot_str(s, c), t->cols[c].align);
		}
	}
	canvas_put(dst, r);
}

// draw_header_item draws a column's header as Explorer's: its title (in
// the column's alignment), a hairline between it and the next, the
// subtle fill under the pointer or pressed, the sort's chevron at its top.
static void draw_header_item(wh_table *t, HDC dst, int col, RECT r, UINT state) {
	float k = wh_scale_of(t->n);
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	HDC dc = canvas_for(w, h);
	RECT all = {0, 0, w, h};
	FillRect(dc, &all, wh_brush(t->bg));
	gfx g = g_new(dc);
	bool hot = (state & CDIS_HOT) || col == t->hot_header, pressed = (state & CDIS_SELECTED) != 0;
	bool sortable = col >= 0 && (uint32_t)col < t->ncols && (t->cols[col].flags & 2);
	if ((hot || pressed) && sortable) {
		rgba f = pressed ? H.theme.subtle_pressed : H.theme.subtle_hot;
		g_fill_round_rect(g, rgba_argb(f), 2 * k, 3 * k, (float)w - 4 * k, (float)h - 6 * k, 4 * k);
	}
	// The divider at its end, as the order shows it.
	float line = wh_maxf(1, floorf(k));
	g_fill_rect(g, rgba_argb(H.theme.separator), rtl(t) ? 0 : (float)w - line, 8 * k, line, (float)h - 16 * k);
	g_fill_rect(g, rgba_argb(H.theme.separator), 0, (float)h - line, (float)w, line);
	g_close(g);
	if (col >= 0 && (uint32_t)col < t->ncols) {
		SetBkMode(dc, TRANSPARENT);
		if (col == t->sort_col) {
			RECT ar = {0, 0, w, px(t, 10)};
			draw_glyph(t, dc, t->sort_asc ? 0xE70E : 0xE70D, ar, H.theme.secondary, 8);
		}
		HGDIOBJ old = SelectObject(dc, header_font(t));
		SetTextColor(dc, rgba_ref(IsWindowEnabled(t->ctl) ? H.theme.secondary : H.theme.disabled));
		RECT tr = {px(t, PAD), 0, w - px(t, PAD), h};
		DrawTextW(dc, t->cols[col].title, -1, &tr, align_format(t, t->cols[col].align));
		SelectObject(dc, old);
	}
	canvas_put(dst, r);
}

// bottom_line draws the header's hairline along its bottom, from x0 to x1.
static void bottom_line(wh_table *t, HDC dc, int x0, int x1, int bottom) {
	int line = (int)wh_maxf(1, floorf(wh_scale_of(t->n)));
	RECT l = {x0, bottom - line, x1, bottom};
	FillRect(dc, &l, wh_brush(rgba_over(H.theme.separator, t->bg)));
}

static LRESULT header_custom_draw(wh_table *t, NMCUSTOMDRAW *cd) {
	switch (cd->dwDrawStage) {
	case CDDS_PREPAINT: {
		// A column moved shows once the header has its new order, which
		// the drag's own loop sets after it ends: reported then.
		if (t->watch_layout) PostMessageW(t->ctl, WM_TBL_LAYOUT, 0, 0);
		RECT r;
		GetClientRect(t->header, &r);
		FillRect(cd->hdc, &r, wh_brush(t->bg));
		bottom_line(t, cd->hdc, 0, r.right, r.bottom);
		return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
	}
	case CDDS_ITEMPREPAINT:
		draw_header_item(t, cd->hdc, (int)cd->dwItemSpec, cd->rc, cd->uItemState);
		return CDRF_SKIPDEFAULT;
	case CDDS_POSTPAINT: {
		// Past the last column: what's behind, as the theme's filler isn't.
		RECT r, it;
		GetClientRect(t->header, &r);
		int right = 0;
		for (uint32_t i = 0; i < t->ncols; i++)
			if (Header_GetItemRect(t->header, (int)i, &it)) right = wh_maxi(right, it.right);
		r.left = right;
		if (r.left >= r.right) return CDRF_DODEFAULT;
		FillRect(cd->hdc, &r, wh_brush(t->bg));
		bottom_line(t, cd->hdc, r.left, r.right, r.bottom);
		return CDRF_DODEFAULT;
	}
	}
	return CDRF_DODEFAULT;
}

// draw_outline_row draws an Outline's row: its highlight, its chevron
// (as Levels says: down expanded, forward collapsed, none for a leaf)
// after its depth's indent, its symbol and its text.
static void draw_outline_row(wh_table *t, HDC dst, HTREEITEM item, int64_t id) {
	RECT r, client;
	if (!TreeView_GetItemRect(t->ctl, item, &r, FALSE)) return;
	GetClientRect(t->ctl, &client);
	r.left = 0, r.right = client.right;
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	HDC dc = canvas_for(w, h);
	RECT all = {0, 0, w, h};
	FillRect(dc, &all, wh_brush(t->bg));
	int64_t row = idmap_get(&t->rows, id);
	if (row >= 0) {
		bool sel = t->mode != AK_SELECTION_MODE_NONE && TreeView_GetSelection(t->ctl) == item;
		bool focus = TreeView_GetSelection(t->ctl) == item && focused_view(t);
		paint_highlight(t, dc, w, h, sel, row == t->hot, focus);
		uint8_t flags = row_flags(t, row);
		int at = px(t, OUTLINE_LEAD + INDENT * (float)row_depth(t, row)), dw = px(t, DISCLOSURE);
		int x0, x1;
		physical(t, w, at, at + dw, &x0, &x1);
		RECT dr = {x0, 0, x1, h};
		SetBkMode(dc, TRANSPARENT);
		if (flags & 1) draw_glyph(t, dc, (flags & 2) ? 0xE70D : (rtl(t) ? 0xE76B : 0xE76C), dr, H.theme.secondary, 8);
		physical(t, w, at + dw, w - px(t, PAD), &x0, &x1);
		RECT cr = {x0, 0, x1, h};
		tslot *s = bind_row(t, row);
		if (s) {
			views_follow(t, s);
			cell_text(t, dc, cr, s->glyph, H.theme.accent_text, slot_str(s, 0), AK_TEXT_ALIGN_START);
		}
	}
	canvas_put(dst, r);
}

// fill_below paints what's below the last row as what's behind the
// table: no column lines there, as a themed ListView draws.
static void fill_below(wh_table *t, HDC dc) {
	RECT c, r;
	GetClientRect(t->ctl, &c);
	int last = ListView_GetTopIndex(t->ctl) + ListView_GetCountPerPage(t->ctl);
	if (last >= (int)t->nids) last = (int)t->nids - 1;
	if (last >= 0 && ListView_GetItemRect(t->ctl, last, &r, LVIR_BOUNDS)) c.top = r.bottom;
	else if (t->header) {
		RECT h;
		GetWindowRect(t->header, &h);
		c.top = h.bottom - h.top;
	}
	if (c.top < c.bottom) FillRect(dc, &c, wh_brush(t->bg));
}

static LRESULT custom_draw(wh_table *t, NMCUSTOMDRAW *cd) {
	switch (cd->dwDrawStage) {
	case CDDS_PREPAINT:
		return CDRF_NOTIFYITEMDRAW | (t->outline ? 0 : CDRF_NOTIFYPOSTPAINT);
	case CDDS_POSTPAINT:
		fill_below(t, cd->hdc);
		return CDRF_DODEFAULT;
	case CDDS_ITEMPREPAINT:
		drawing++;
		if (t->outline) draw_outline_row(t, cd->hdc, (HTREEITEM)cd->dwItemSpec, (int64_t)cd->lItemlParam);
		else draw_table_row(t, cd->hdc, (int)cd->dwItemSpec);
		drawing--;
		return CDRF_SKIPDEFAULT;
	}
	return CDRF_DODEFAULT;
}

// ---- The rows' menus ----

typedef struct {
	uint32_t cell;
	POINT at;
	bool rtl;
} menu_job;

static void menu_now(void *ctx) {
	menu_job *j = ctx;
	node *c = wh_node(j->cell);
	if (c && c->menu.n && c->table && c->table->ctl) {
		int64_t got = wh_track_actions(&c->menu, j->rtl, j->at, GetAncestor(c->table->ctl, GA_ROOT));
		if (got >= 0 && (c = wh_node(j->cell))) wh_emit_int(c, AK_EVENT_ACTION, got);
	}
	free(j);
}

// row_menu shows row's menu (its cell's Menu) at screen point at, the row
// selected first if it isn't, as Explorer's context menus do.
static void row_menu(wh_table *t, int64_t row, POINT at) {
	if (row < 0 || row >= (int64_t)t->nids) return;
	if (t->mode != AK_SELECTION_MODE_NONE) {
		if (t->outline) {
			if (TreeView_GetSelection(t->ctl) != t->items[row]) TreeView_SelectItem(t->ctl, t->items[row]);
		} else if (!(ListView_GetItemState(t->ctl, (int)row, LVIS_SELECTED) & LVIS_SELECTED)) {
			ListView_SetItemState(t->ctl, -1, 0, LVIS_SELECTED);
			ListView_SetItemState(t->ctl, (int)row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		}
		report_now(t);
	}
	tslot *s = bind_row(t, row);
	if (!s || !s->cell->menu.n) return;
	menu_job *j = wh_alloc(sizeof *j);
	j->cell = s->cell->id, j->at = at, j->rtl = rtl(t);
	wh_after_core(menu_now, j);
}

static void key_menu(wh_table *t) {
	int64_t row = -1;
	if (t->outline) {
		HTREEITEM h = TreeView_GetSelection(t->ctl);
		for (uint32_t i = 0; h && i < t->nids; i++)
			if (t->items[i] == h) row = i;
	} else {
		row = ListView_GetNextItem(t->ctl, -1, LVNI_FOCUSED);
		if (row < 0) row = ListView_GetNextItem(t->ctl, -1, LVNI_SELECTED);
	}
	RECT r;
	if (row < 0 || !row_rect(t, row, &r)) return;
	POINT p = {r.left + px(t, PAD), r.bottom};  // its start, mirrored or not
	ClientToScreen(t->ctl, &p);
	row_menu(t, row, p);
}

static void open_row(wh_table *t, int64_t row) {
	if (row < 0 || row >= (int64_t)t->nids) return;
	report_now(t);
	wh_emit_int(t->n, AK_EVENT_OPEN, row);
}

static int64_t first_selected(wh_table *t) {
	if (t->outline) {
		HTREEITEM h = TreeView_GetSelection(t->ctl);
		if (!h) return -1;
		TVITEMW it = {.mask = TVIF_PARAM, .hItem = h};
		TreeView_GetItem(t->ctl, &it);
		return idmap_get(&t->rows, (int64_t)it.lParam);
	}
	int r = ListView_GetNextItem(t->ctl, -1, LVNI_SELECTED);
	return r >= 0 ? r : ListView_GetNextItem(t->ctl, -1, LVNI_FOCUSED);
}

static void set_hot(wh_table *t, int row) {
	if (row == t->hot) return;
	int was = t->hot;
	t->hot = row;
	invalidate_row(t, was);
	invalidate_row(t, row);
}

// ---- The ListView's and the TreeView's own messages ----

static int64_t outline_row_at(wh_table *t, POINT client, HTREEITEM *out) {
	TVHITTESTINFO hi = {.pt = client};
	HTREEITEM h = TreeView_HitTest(t->ctl, &hi);
	if (out) *out = h;
	if (!h || !(hi.flags & (TVHT_ONITEM | TVHT_ONITEMINDENT | TVHT_ONITEMBUTTON | TVHT_ONITEMRIGHT))) return -1;
	TVITEMW it = {.mask = TVIF_PARAM, .hItem = h};
	TreeView_GetItem(t->ctl, &it);
	return idmap_get(&t->rows, (int64_t)it.lParam);
}

static int64_t row_at(wh_table *t, POINT client) {
	if (t->outline) return outline_row_at(t, client, NULL);
	LVHITTESTINFO hi = {.pt = client};
	int i = ListView_HitTest(t->ctl, &hi);
	return i >= 0 && (hi.flags & LVHT_ONITEM) ? i : -1;
}

// on_disclosure reports whether client point p is on an outline row's
// chevron (where it draws it), and the row.
static bool on_disclosure(wh_table *t, POINT p, int64_t *row) {
	*row = outline_row_at(t, p, NULL);
	if (*row < 0 || !(row_flags(t, *row) & 1)) return false;
	RECT client;
	GetClientRect(t->ctl, &client);
	int at = px(t, OUTLINE_LEAD + INDENT * (float)row_depth(t, *row)), dw = px(t, DISCLOSURE);
	return p.x >= at && p.x < at + dw;  // from its start, mirrored or not
}

static void expand(wh_table *t, int64_t row, bool on) {
	if (row < 0 || row >= (int64_t)t->nids) return;
	ak_event e = {.kind = AK_EVENT_EXPAND, .x = (double)row, .y = on ? 1 : 0};
	wh_emit(t->n, &e);
}

static wh_table *table_of_ctl(HWND hwnd) {
	node *n = wh_node_of(hwnd);
	return n && n->table && n->kind != KIND_TABLE_CELL && n->table->ctl == hwnd ? n->table : NULL;
}

// header_message is the header's: its height (Explorer's), and no
// resizing a column that doesn't resize.
static LRESULT header_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	wh_table *t = table_of_ctl(GetParent(hwnd));
	if (!t) return wh_def_subclass(hwnd, m, wp, lp);
	switch (m) {
	case HDM_LAYOUT: {
		LRESULT r = wh_def_subclass(hwnd, m, wp, lp);
		HDLAYOUT *l = (HDLAYOUT *)lp;
		int h = px(t, HEADER_H);
		l->pwpos->cy = h;
		l->prc->top = l->pwpos->y + h;
		return r;
	}
	case WM_SETCURSOR: {
		HDHITTESTINFO hi = {0};
		GetCursorPos(&hi.pt);
		ScreenToClient(hwnd, &hi.pt);
		int i = (int)SendMessageW(hwnd, HDM_HITTEST, 0, (LPARAM)&hi);
		if (i >= 0 && (uint32_t)i < t->ncols && (hi.flags & (HHT_ONDIVIDER | HHT_ONDIVOPEN)) && !(t->cols[i].flags & 1)) {
			SetCursor(LoadCursorW(NULL, IDC_ARROW));
			return TRUE;
		}
		break;
	}
	case WM_MOUSEMOVE: {
		HDHITTESTINFO hi = {.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}};
		int i = (int)SendMessageW(hwnd, HDM_HITTEST, 0, (LPARAM)&hi);
		if (i != t->hot_header) {
			t->hot_header = i;
			InvalidateRect(hwnd, NULL, FALSE);
			TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
			TrackMouseEvent(&tme);
		}
		break;
	}
	case WM_MOUSELEAVE:
		t->hot_header = -1;
		InvalidateRect(hwnd, NULL, FALSE);
		break;
	}
	return wh_def_subclass(hwnd, m, wp, lp);
}

// fit_column sizes a column to what it shows (a divider's double click):
// its title's width and the rows in view's (with the first's symbol), its
// bounds kept. A ListView with owner data would ask for every row's text.
static void fit_column(wh_table *t, int c) {
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, header_font(t));
	int w = wh_text_extent(dc, t->cols[c].title, (int)wcslen(t->cols[c].title)).cx;
	SelectObject(dc, wh_node_font(t->n));
	int top = ListView_GetTopIndex(t->ctl), page = ListView_GetCountPerPage(t->ctl);
	for (int r = top; r <= top + page && r < (int)t->nids; r++) {
		tslot *s = bind_row(t, r);
		if (!s || (uint32_t)c >= s->nstr) continue;
		const wchar_t *str = slot_str(s, (uint32_t)c);
		int tw = wh_text_extent(dc, str, (int)wcslen(str)).cx + (c == 0 && s->glyph ? px(t, ICON + 8) : 0);
		w = wh_maxi(w, tw);
	}
	SelectObject(dc, old);
	ListView_SetColumnWidth(t->ctl, c, w + 2 * px(t, PAD));  // its bounds: HDN_ITEMCHANGING's
}

// header_notify is what the header tells its ListView: its drawing, the
// columns' bounds kept while the user resizes, the layout reported after.
static bool header_notify(wh_table *t, NMHDR *h, LRESULT *out) {
	switch (h->code) {
	case NM_CUSTOMDRAW:
		*out = header_custom_draw(t, (NMCUSTOMDRAW *)h);
		return true;
	case HDN_BEGINTRACKW:
	case HDN_DIVIDERDBLCLICKW: {
		NMHEADERW *nh = (NMHEADERW *)h;
		if (nh->iItem < 0 || (uint32_t)nh->iItem >= t->ncols) return false;
		if (!(t->cols[nh->iItem].flags & 1)) {
			*out = TRUE;  // it doesn't resize
			return true;
		}
		t->cols[nh->iItem].user_sized = true;
		t->watch_layout = true;
		PostMessageW(t->ctl, WM_TBL_LAYOUT, 0, 0);
		if (h->code == HDN_DIVIDERDBLCLICKW) {
			fit_column(t, nh->iItem);  // the rows shown's, not the ListView's every row
			*out = TRUE;
			return true;
		}
		return false;
	}
	case HDN_ITEMCHANGINGW: {
		NMHEADERW *nh = (NMHEADERW *)h;
		if (nh->pitem && (nh->pitem->mask & HDI_WIDTH) && nh->iItem >= 0 && (uint32_t)nh->iItem < t->ncols) {
			tcol *c = &t->cols[nh->iItem];
			int lo = px(t, col_min(c)), hi = c->max > 0 ? px(t, c->max) : 1 << 30;
			nh->pitem->cxy = wh_maxi(lo, wh_mini(hi, nh->pitem->cxy));
		}
		return false;
	}
	case HDN_ENDTRACKW:
	case HDN_TRACKW:
		PostMessageW(t->ctl, WM_TBL_LAYOUT, 0, 0);
		return false;
	case HDN_ENDDRAG:
		if (!t->movable) {
			*out = TRUE;
			return true;
		}
		t->user_order = true;
		t->watch_layout = true;  // the order's set once this returns
		PostMessageW(t->ctl, WM_TBL_LAYOUT, 0, 0);
		return false;
	}
	return false;
}

static LRESULT table_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	wh_table *t = table_of_ctl(hwnd);
	if (!t) return wh_def_subclass(hwnd, m, wp, lp);
	switch (m) {
	case WM_NOTIFY: {
		NMHDR *h = (NMHDR *)lp;
		LRESULT r = 0;
		if (h->hwndFrom == t->header && header_notify(t, h, &r)) return r;
		break;
	}
	case WM_TBL_REPORT:
		report_now(t);
		return 0;
	case WM_TBL_LAYOUT:
		report_layout(t);
		return 0;
	case WM_TBL_FLUSH:
		flush(t);
		return 0;
	case WM_TBL_SYNC: {
		MSG next;
		while (PeekMessageW(&next, hwnd, WM_TBL_SYNC, WM_TBL_SYNC, PM_REMOVE)) {
		}
		sync_views(t);
		return 0;
	}
	case WM_GETDLGCODE: {
		LRESULT r = wh_def_subclass(hwnd, m, wp, lp);
		MSG *msg = (MSG *)lp;
		if (msg && msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN) r |= DLGC_WANTALLKEYS;
		return r | DLGC_WANTARROWS;
	}
	case WM_KEYDOWN:
		if (wp == VK_RETURN) {
			open_row(t, first_selected(t));
			return 0;
		}
		if (t->outline && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_ADD || wp == VK_SUBTRACT)) {
			int64_t row = first_selected(t);
			if (row < 0) break;
			uint8_t f = row_flags(t, row);
			bool right = wp == VK_RIGHT || wp == VK_ADD;
			if (rtl(t) && (wp == VK_LEFT || wp == VK_RIGHT)) right = !right;  // they go the way they point
			if (right && (f & 1) && !(f & 2)) {
				expand(t, row, true);
				return 0;
			}
			if (!right && (f & 2)) {
				expand(t, row, false);
				return 0;
			}
			if (wp == VK_ADD || wp == VK_SUBTRACT) return 0;
			// To the first child or the parent: the TreeView's own.
			if (rtl(t)) wp = VK_LEFT + VK_RIGHT - wp;
		}
		break;
	case WM_CONTEXTMENU:
		if (lp == -1) {
			key_menu(t);  // Shift+F10, the menu key
		} else {
			POINT s = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, c = s;
			ScreenToClient(hwnd, &c);
			row_menu(t, row_at(t, c), s);
		}
		return 0;
	case WM_MOUSEMOVE: {
		POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		set_hot(t, (int)row_at(t, p));
		if (!t->tracking) {
			TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
			TrackMouseEvent(&tme);
			t->tracking = true;
		}
		break;
	}
	case WM_MOUSELEAVE:
		t->tracking = false;
		set_hot(t, -1);
		break;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (t->outline) {
			int64_t row;
			POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			if (on_disclosure(t, p, &row)) {
				SetFocus(hwnd);
				expand(t, row, !(row_flags(t, row) & 2));
				return 0;
			}
		}
		break;
	case WM_VSCROLL: case WM_HSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_SIZE: {
		LRESULT r = wh_def_subclass(hwnd, m, wp, lp);
		sync_views(t);
		return r;
	}
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		InvalidateRect(hwnd, NULL, FALSE);  // its focus ring
		break;
	case WM_PRINTCLIENT: {
		t->printing++;
		LRESULT r = wh_def_subclass(hwnd, m, wp, lp);
		t->printing--;
		return r;
	}
	}
	LRESULT r = wh_def_subclass(hwnd, m, wp, lp);
	if (m == WM_KEYDOWN && t->nviews) sync_views(t);
	return r;
}

// ---- Notifications to the table's view ----

static void copy_text(wchar_t *to, int cap, const wchar_t *s) {
	if (!to || cap <= 0) return;
	int n = (int)wcslen(s);
	if (n >= cap) n = cap - 1;
	memcpy(to, s, (size_t)n * sizeof *to);
	to[n] = 0;
}

static LRESULT table_notify(wh_table *t, NMHDR *hdr, LPARAM lp, bool *handled) {
	switch (hdr->code) {
	case NM_CUSTOMDRAW:
		*handled = true;
		return custom_draw(t, (NMCUSTOMDRAW *)lp);
	case LVN_GETDISPINFOW: {
		NMLVDISPINFOW *d = (NMLVDISPINFOW *)lp;
		*handled = true;
		if (d->item.mask & LVIF_TEXT) {
			tslot *s = bind_row(t, d->item.iItem);
			copy_text(d->item.pszText, d->item.cchTextMax, slot_str(s, (uint32_t)d->item.iSubItem));
		}
		return 0;
	}
	case LVN_ODCACHEHINT: {
		NMLVCACHEHINT *c = (NMLVCACHEHINT *)lp;
		*handled = true;
		for (int i = c->iFrom; i <= c->iTo && i - c->iFrom < 512; i++) bind_row(t, i);
		return 0;
	}
	case LVN_ODFINDITEMW:
		*handled = true;
		return -1;
	case LVN_ITEMCHANGED: {
		NMLISTVIEW *l = (NMLISTVIEW *)lp;
		if ((l->uChanged & LVIF_STATE) && ((l->uOldState ^ l->uNewState) & LVIS_SELECTED)) report_later(t);
		break;
	}
	case LVN_ODSTATECHANGED: {
		NMLVODSTATECHANGE *o = (NMLVODSTATECHANGE *)lp;
		if ((o->uOldState ^ o->uNewState) & LVIS_SELECTED) report_later(t);
		break;
	}
	case LVN_COLUMNCLICK: {
		NMLISTVIEW *l = (NMLISTVIEW *)lp;
		*handled = true;
		if (l->iSubItem >= 0 && (uint32_t)l->iSubItem < t->ncols && (t->cols[l->iSubItem].flags & 2)) wh_emit_int(t->n, AK_EVENT_SORT_BY, l->iSubItem);
		return 0;
	}
	case NM_DBLCLK: {
		NMITEMACTIVATE *a = (NMITEMACTIVATE *)lp;
		*handled = true;
		if (a->iItem >= 0) open_row(t, a->iItem);
		return 0;
	}
	case LVN_ENDSCROLL:
		PostMessageW(t->ctl, WM_TBL_SYNC, 0, 0);
		break;
	}
	return 0;
}

static LRESULT outline_notify(wh_table *t, NMHDR *hdr, LPARAM lp, bool *handled) {
	switch (hdr->code) {
	case NM_CUSTOMDRAW:
		*handled = true;
		return custom_draw(t, (NMCUSTOMDRAW *)lp);
	case TVN_GETDISPINFOW: {
		NMTVDISPINFOW *d = (NMTVDISPINFOW *)lp;
		*handled = true;
		if (d->item.mask & TVIF_TEXT) {
			tslot *s = bind_row(t, idmap_get(&t->rows, (int64_t)d->item.lParam));
			copy_text(d->item.pszText, d->item.cchTextMax, slot_str(s, 0));
		}
		return 0;
	}
	case TVN_ITEMEXPANDINGW: {
		NMTREEVIEWW *v = (NMTREEVIEWW *)lp;
		*handled = true;
		if (t->setting) return FALSE;  // Levels', applied
		int64_t row = idmap_get(&t->rows, (int64_t)v->itemNew.lParam);
		uint8_t f = row_flags(t, row);
		bool on = (v->action & TVE_EXPAND) != 0;
		if ((v->action & TVE_TOGGLE) == TVE_TOGGLE) on = !(f & 2);
		if ((f & 1) && on != ((f & 2) != 0)) expand(t, row, on);
		return TRUE;  // as Levels says, which comes back
	}
	case TVN_SELCHANGEDW:
		if (!t->setting) {
			NMTREEVIEWW *v = (NMTREEVIEWW *)lp;
			t->sel_has = v->itemNew.hItem != NULL;
			t->sel_item = (int64_t)v->itemNew.lParam;
			report_later(t);
		}
		break;
	case NM_RCLICK: {
		*handled = true;
		POINT s;
		GetCursorPos(&s);
		POINT c = s;
		ScreenToClient(t->ctl, &c);
		row_menu(t, row_at(t, c), s);
		return TRUE;
	}
	case NM_DBLCLK: {
		*handled = true;
		POINT c;
		GetCursorPos(&c);
		ScreenToClient(t->ctl, &c);
		int64_t row = row_at(t, c);
		if (row >= 0) open_row(t, row);
		return 0;  // and the TreeView's own: it expands or collapses (Expand)
	}
	}
	return 0;
}

LRESULT wh_table_notify(node *n, NMHDR *hdr, LPARAM lp, bool *handled) {
	wh_table *t = n->table;
	*handled = false;
	if (!t || hdr->hwndFrom != t->ctl) return 0;
	return t->outline ? outline_notify(t, hdr, lp, handled) : table_notify(t, hdr, lp, handled);
}

// ---- An outline's tree, from Levels and the IDs ----

static void set_children(wh_table *t, HTREEITEM h, uint8_t flags) {
	TVITEMW it = {.mask = TVIF_CHILDREN, .hItem = h, .cChildren = (flags & 1) ? 1 : 0};
	TreeView_SetItem(t->ctl, &it);
}

static HTREEITEM insert(wh_table *t, HTREEITEM parent, HTREEITEM after, int64_t id, uint8_t flags) {
	TVINSERTSTRUCTW is = {.hParent = parent, .hInsertAfter = after};
	is.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM;
	is.item.pszText = LPSTR_TEXTCALLBACKW;
	is.item.cChildren = (flags & 1) ? 1 : 0;
	is.item.lParam = (LPARAM)id;
	return TreeView_InsertItem(t->ctl, &is);
}

static int ldepth(const uint8_t *lv, uint32_t i) { return lv[i * 3] | lv[i * 3 + 1] << 8; }

// rebuild makes the tree again from the rows: when what changed isn't
// rows coming and going in one place (an expand, a collapse).
static void rebuild(wh_table *t, const int64_t *ids, const uint8_t *lv, uint32_t n, HTREEITEM *out) {
	TreeView_DeleteAllItems(t->ctl);
	HTREEITEM stack[64] = {0};
	for (uint32_t i = 0; i < n; i++) {
		int d = wh_mini(ldepth(lv, i), 63);
		while (d > 0 && !stack[d - 1]) d--;  // no parent: under the nearest there is
		out[i] = insert(t, d ? stack[d - 1] : TVI_ROOT, TVI_LAST, ids[i], lv[i * 3 + 2]);
		stack[d] = out[i];
		for (int k = d + 1; k < 64; k++) stack[k] = NULL;
	}
}

// apply_expanded expands the rows Levels says are, whose children follow
// them, and collapses the others (t->setting: not reported).
static void apply_expanded(wh_table *t) {
	const uint8_t *lv = t->levels;
	for (uint32_t i = 0; lv && i < t->nids; i++) {
		bool open = (lv[i * 3 + 2] & 2) && i + 1 < t->nids && ldepth(lv, i + 1) > ldepth(lv, i);
		bool is = (TreeView_GetItemState(t->ctl, t->items[i], TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
		if (open && !is) TreeView_Expand(t->ctl, t->items[i], TVE_EXPAND);
		else if (!open && is && TreeView_GetChild(t->ctl, t->items[i])) TreeView_Expand(t->ctl, t->items[i], TVE_COLLAPSE);
	}
}

// outline_apply takes the new rows (IDs and Levels): what's the same at
// the start and the end is kept, the rows between deleted and inserted
// (an expand inserts its children, a collapse deletes them); anything
// else makes the tree again. Then each row's flags, and the rows whose
// children show are expanded.
static void outline_apply(wh_table *t) {
	const int64_t *nids = t->updated ? t->pend : t->ids;
	uint32_t nn = t->updated ? t->npend : t->nids;
	const uint8_t *nlv = t->levels_new ? t->pend_levels : t->levels;
	uint32_t nlv_n = t->levels_new ? t->npend_levels : t->nids;
	uint8_t *lv = wh_alloc((nn ? nn : 1) * 3);
	for (uint32_t i = 0; i < nn && nlv; i++)
		if (i < nlv_n) memcpy(lv + i * 3, nlv + i * 3, 3);
	HTREEITEM *items = wh_alloc((nn ? nn : 1) * sizeof *items);
	uint32_t on = t->nids;
	const int64_t *oids = t->ids;
	const uint8_t *olv = t->levels;
	t->setting = true;
	t->building = true;
	SendMessageW(t->ctl, WM_SETREDRAW, FALSE, 0);
	HTREEITEM top = TreeView_GetFirstVisible(t->ctl);
	int64_t top_id = -1;
	if (top) {
		TVITEMW it = {.mask = TVIF_PARAM, .hItem = top};
		TreeView_GetItem(t->ctl, &it);
		top_id = (int64_t)it.lParam;
	}
	uint32_t p = 0, s = 0;
	while (p < on && p < nn && oids[p] == nids[p] && ldepth(olv, p) == ldepth(lv, p)) p++;
	while (s < on - p && s < nn - p && oids[on - 1 - s] == nids[nn - 1 - s] && ldepth(olv, on - 1 - s) == ldepth(lv, nn - 1 - s)) s++;
	bool clean = t->items != NULL;
	// Deleting the old rows between: each subtree's root (its descendants
	// go with it), none reaching past them.
	int del = -1;
	for (uint32_t i = p; i < on - s && clean; i++) {
		if (del >= 0 && ldepth(olv, i) > del) continue;
		del = ldepth(olv, i);
	}
	if (clean && del >= 0 && s > 0 && ldepth(olv, on - s) > del) clean = false;
	// The rows after keep their parents.
	if (clean && s > 0) {
		int ostack[64], nstack[64];
		for (int k = 0; k < 64; k++) ostack[k] = nstack[k] = -1;
		for (uint32_t i = 0; i < on - s; i++) {
			int d = wh_mini(ldepth(olv, i), 63);
			ostack[d] = (int)i;
			for (int k = d + 1; k < 64; k++) ostack[k] = -1;
		}
		for (uint32_t i = 0; i < nn - s; i++) {
			int d = wh_mini(ldepth(lv, i), 63);
			nstack[d] = (int)i;
			for (int k = d + 1; k < 64; k++) nstack[k] = -1;
		}
		int least = 64;
		for (uint32_t j = 0; j < s && clean; j++) {
			int d = wh_mini(ldepth(lv, nn - s + j), 63);
			if (d > least) continue;  // under a row after: kept with it
			least = d;
			if (d == 0) continue;
			int op = ostack[d - 1], np = nstack[d - 1];
			if (op < 0 || np < 0 || op != np || (uint32_t)op >= p) clean = false;
		}
	}
	if (clean) {
		del = -1;
		for (uint32_t i = p; i < on - s; i++) {
			if (del >= 0 && ldepth(olv, i) > del) continue;
			del = ldepth(olv, i);
			TreeView_DeleteItem(t->ctl, t->items[i]);
		}
		memcpy(items, t->items, p * sizeof *items);
		for (uint32_t j = 0; j < s; j++) items[nn - s + j] = t->items[on - s + j];
		for (uint32_t i = p; i < nn - s; i++) {
			int d = ldepth(lv, i);
			HTREEITEM parent = TVI_ROOT, after = TVI_FIRST;
			for (int64_t k = (int64_t)i - 1; k >= 0; k--) {
				int kd = ldepth(lv, (uint32_t)k);
				if (kd == d && after == TVI_FIRST) after = items[k];
				if (kd < d) {
					if (kd == d - 1) parent = items[k];
					break;
				}
			}
			if (d > 0 && parent == TVI_ROOT) {
				clean = false;  // no parent: Levels that make no tree
				break;
			}
			items[i] = insert(t, parent, after, nids[i], lv[i * 3 + 2]);
		}
	}
	if (!clean) rebuild(t, nids, lv, nn, items);
	// The rows kept: their flags.
	for (uint32_t i = 0; i < nn; i++) {
		bool kept = clean && (i < p || i >= nn - s);
		uint32_t oi = i < p ? i : i - nn + on;
		if (kept && olv && olv[oi * 3 + 2] == lv[i * 3 + 2]) continue;
		if (kept) set_children(t, items[i], lv[i * 3 + 2]);
	}
	free(t->items);
	t->items = items;
	if (t->updated) {
		free(t->ids);
		t->ids = t->pend, t->nids = t->npend;
		t->pend = NULL, t->npend = 0;
	}
	free(t->levels);
	t->levels = lv;
	free(t->pend_levels);
	t->pend_levels = NULL, t->npend_levels = 0;
	t->updated = t->levels_new = false;
	idmap_build(&t->rows, t->ids, t->nids);
	t->building = false;
	apply_expanded(t);
	// The rows' cells: those whose rows hold other items are bound again.
	for (uint32_t i = 0; i < t->nslots; i++) {
		tslot *sl = t->slots[i];
		if (sl->row < 0) continue;
		int64_t r = idmap_get(&t->rows, sl->item);
		if (r != sl->row) {
			sl->row = -1;
			hide_views(sl);
		}
	}
	// What was selected and at the top, still.
	int64_t sr = t->sel_has ? idmap_get(&t->rows, t->sel_item) : -1;
	HTREEITEM want = sr >= 0 ? items[sr] : NULL;
	if (TreeView_GetSelection(t->ctl) != want) TreeView_SelectItem(t->ctl, want);
	t->sel_has = want != NULL;
	int64_t tr = top_id >= 0 ? idmap_get(&t->rows, top_id) : -1;
	if (tr >= 0) TreeView_SelectSetFirstVisible(t->ctl, items[tr]);
	t->setting = false;
	SendMessageW(t->ctl, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(t->ctl, NULL, FALSE);
	if (t->hot >= (int)t->nids) t->hot = -1;
}

// ---- Props ----

static void set_row_height(wh_table *t, float h) {
	t->row_h = h > 0 ? h : 32;
	int ph = wh_maxi(px(t, t->row_h), 1);
	if (t->outline) {
		TreeView_SetItemHeight(t->ctl, ph);
	} else {
		// A report view's rows are its small images' height.
		HIMAGELIST il = ImageList_Create(1, ph, ILC_COLOR32, 1, 0);
		ListView_SetImageList(t->ctl, il, LVSIL_SMALL);
		if (t->il) ImageList_Destroy(t->il);
		t->il = il;
	}
	PostMessageW(t->ctl, WM_TBL_SYNC, 0, 0);
}

static void set_mode(wh_table *t, int32_t mode) {
	t->mode = mode;
	if (!t->outline) {
		LONG_PTR st = GetWindowLongPtrW(t->ctl, GWL_STYLE);
		st = mode == AK_SELECTION_MODE_MULTIPLE ? st & ~(LONG_PTR)LVS_SINGLESEL : st | LVS_SINGLESEL;
		SetWindowLongPtrW(t->ctl, GWL_STYLE, st);
		if (mode == AK_SELECTION_MODE_NONE) {
			t->setting = true;
			ListView_SetItemState(t->ctl, -1, 0, LVIS_SELECTED);
			t->setting = false;
		}
	}
	InvalidateRect(t->ctl, NULL, FALSE);
}

void wh_table_set_prop(node *n, uint16_t p, const ak_value *v) {
	wh_table *t = n->table;
	if (!t) return;
	const uint8_t *b = (const uint8_t *)v->s.p;
	size_t len = v->s.len;
	if (t->outline) {
		switch (p) {
		case AK_PROP_OUTLINE_LEVELS:
			free(t->pend_levels);
			t->npend_levels = len >= 4 ? wh_u32(b) : 0;
			if (4 + (size_t)t->npend_levels * 3 > len) t->npend_levels = (uint32_t)((len - 4) / 3);
			t->pend_levels = wh_alloc((t->npend_levels ? t->npend_levels : 1) * 3);
			if (t->npend_levels) memcpy(t->pend_levels, b + 4, (size_t)t->npend_levels * 3);
			t->levels_new = true;
			queue(t);
			break;
		case AK_PROP_OUTLINE_SELECTION_MODE: set_mode(t, (int32_t)v->i); break;
		case AK_PROP_OUTLINE_SELECTION:
			t->sel_want.len = 0;
			ak_buf_put(&t->sel_want, b, len);
			t->sel_dirty = true;
			queue(t);
			break;
		case AK_PROP_OUTLINE_ROW_HEIGHT: set_row_height(t, (float)v->f); break;
		}
		return;
	}
	switch (p) {
	case AK_PROP_TABLE_COLUMNS: set_columns(t, b, len); break;
	case AK_PROP_TABLE_SORT: set_sort(t, b, len); break;
	case AK_PROP_TABLE_SELECTION_MODE: set_mode(t, (int32_t)v->i); break;
	case AK_PROP_TABLE_SELECTION:
		t->sel_want.len = 0;
		ak_buf_put(&t->sel_want, b, len);
		t->sel_dirty = true;
		queue(t);
		break;
	case AK_PROP_TABLE_ROW_HEIGHT: set_row_height(t, (float)v->f); break;
	case AK_PROP_TABLE_COLUMNS_MOVABLE:
		t->movable = v->i != 0;
		ListView_SetExtendedListViewStyleEx(t->ctl, LVS_EX_HEADERDRAGDROP, t->movable ? LVS_EX_HEADERDRAGDROP : 0);
		break;
	}
}

// ---- Making it ----

// wh_table_listen is a Listen for one of the table's events: the columns
// as first shown go once the core listens for them.
void wh_table_listen(node *n, int e) {
	if (e == AK_EVENT_COLUMN_LAYOUT && n->table && n->table->ctl) PostMessageW(n->table->ctl, WM_TBL_LAYOUT, 0, 0);
}

void wh_table_theme(node *n) {
	wh_table *t = n->table;
	if (!t || !t->ctl) return;
	t->bg = wh_backdrop(n);
	wh_allow_dark(t->ctl, H.dark);
	SetWindowTheme(t->ctl, H.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);  // its scroll bars
	if (t->outline) {
		TreeView_SetBkColor(t->ctl, rgba_ref(t->bg));
		TreeView_SetTextColor(t->ctl, rgba_ref(H.theme.label));
	} else {
		ListView_SetBkColor(t->ctl, rgba_ref(t->bg));
		ListView_SetTextBkColor(t->ctl, rgba_ref(t->bg));
		ListView_SetTextColor(t->ctl, rgba_ref(H.theme.label));
		if (t->header) {
			wh_allow_dark(t->header, H.dark);
			SetWindowTheme(t->header, H.dark ? L"DarkMode_ItemsView" : L"ItemsView", NULL);
			InvalidateRect(t->header, NULL, FALSE);
		}
	}
	InvalidateRect(t->ctl, NULL, FALSE);
}

// make_ctl makes the table's control in its view: mirrored right to left.
static void make_ctl(wh_table *t) {
	node *n = t->n;
	DWORD ex = wh_is_rtl(n) ? WS_EX_LAYOUTRTL : 0;
	if (t->outline) {
		wh_make_control_in(n, WC_TREEVIEWW, WS_TABSTOP | WS_CLIPCHILDREN | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT | TVS_SHOWSELALWAYS | TVS_NOTOOLTIPS | TVS_NONEVENHEIGHT, ex);
		TreeView_SetExtendedStyle(n->ctl, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
		TreeView_SetIndent(n->ctl, wh_px(n, INDENT));
	} else {
		DWORD single = t->mode == AK_SELECTION_MODE_MULTIPLE ? 0 : LVS_SINGLESEL;
		wh_make_control_in(n, WC_LISTVIEWW, WS_TABSTOP | WS_CLIPCHILDREN | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | single, ex);
		DWORD st = LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | (t->movable ? LVS_EX_HEADERDRAGDROP : 0);
		ListView_SetExtendedListViewStyleEx(n->ctl, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP, st);
		t->header = ListView_GetHeader(n->ctl);
		if (t->header) {
			SetWindowLongPtrW(t->header, GWL_STYLE, GetWindowLongPtrW(t->header, GWL_STYLE) | HDS_HOTTRACK);
			SendMessageW(t->header, WM_SETFONT, (WPARAM)header_font(t), FALSE);
			wh_subclass(t->header, header_message);
		}
	}
	t->ctl = n->ctl;
	wh_subclass(t->ctl, table_message);
}

void wh_table_new(node *n, bool outline) {
	wh_table *t = wh_alloc(sizeof *t);
	t->n = n, t->outline = outline;
	t->hot = t->hot_header = -1, t->sort_col = -1, t->sel_item = -1;
	t->row_h = 32;
	n->table = t;
	n->hwnd = wh_make_view(0);
	make_ctl(t);
	set_row_height(t, 32);
}

// remake makes the control again, as it was (its columns as shown, its
// rows, its selection, its tree), mirrored or not as the direction now is.
static void remake(wh_table *t) {
	node *n = t->n;
	HWND old = t->ctl;
	bool focused = GetFocus() == old;
	int *widths = wh_alloc((t->ncols ? t->ncols : 1) * sizeof *widths);
	for (uint32_t i = 0; i < t->ncols; i++) widths[i] = ListView_GetColumnWidth(old, (int)i);
	for (uint32_t i = 0; i < t->nslots; i++)
		if (t->slots[i]->cell->hwnd) {
			ShowWindow(t->slots[i]->cell->hwnd, SW_HIDE);
			SetParent(t->slots[i]->cell->hwnd, H.park);
		}
	if (t->header) wh_unsubclass(t->header);
	wh_unsubclass(old);
	wh_unbind(old);
	t->header = NULL;
	n->ctl = NULL;
	DestroyWindow(old);
	make_ctl(t);
	wh_bind(n, n->ctl);
	t->setting = true;
	set_row_height(t, t->row_h);
	wh_table_theme(n);
	RECT r;
	GetClientRect(n->hwnd, &r);
	SetWindowPos(t->ctl, NULL, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
	if (t->outline) {
		free(t->items);
		t->items = wh_alloc((t->nids ? t->nids : 1) * sizeof *t->items);
		if (t->levels) rebuild(t, t->ids, t->levels, t->nids, t->items);
		apply_expanded(t);
		int64_t sr = t->sel_has ? idmap_get(&t->rows, t->sel_item) : -1;
		if (sr >= 0) TreeView_SelectItem(t->ctl, t->items[sr]);
	} else {
		insert_cols(t);
		for (uint32_t i = 0; i < t->ncols; i++) ListView_SetColumnWidth(t->ctl, (int)i, widths[i]);
		apply_order(t);
		apply_sort_arrows(t);
		ListView_SetItemCountEx(t->ctl, (int)t->nids, LVSICF_NOINVALIDATEALL);
		t->sel_want.len = 0;
		ak_buf_put(&t->sel_want, t->sel_known.data, t->sel_known.len);
		apply_selection(t);
	}
	t->setting = false;
	free(widths);
	for (uint32_t i = 0; i < t->nslots; i++)
		if (t->slots[i]->cell->hwnd) SetParent(t->slots[i]->cell->hwnd, t->ctl);
	if (focused) SetFocus(t->ctl);
	sync_views(t);
}

// wh_table_direction follows the table's direction: right to left its
// control is mirrored (made again).
void wh_table_direction(node *n) {
	wh_table *t = n->table;
	if (!t || !t->ctl) return;
	if (mirrored(t) != wh_is_rtl(n)) remake(t);
	InvalidateRect(t->ctl, NULL, FALSE);
}

// wh_table_rescale follows its window's scale: its fonts, its rows' and
// header's heights, its columns' widths (as last shown, in points).
void wh_table_rescale(node *n) {
	wh_table *t = n->table;
	if (!t || !t->ctl) return;
	SendMessageW(t->ctl, WM_SETFONT, (WPARAM)wh_node_font(n), TRUE);
	if (t->outline) TreeView_SetIndent(t->ctl, px(t, INDENT));
	if (t->header) SendMessageW(t->header, WM_SETFONT, (WPARAM)header_font(t), TRUE);
	set_row_height(t, t->row_h);
	const uint8_t *b = t->layout_sent.data;
	uint32_t count = t->layout_sent.len >= 4 ? wh_u32(b) : 0;
	for (uint32_t i = 0; i < count && 4 + i * 8 + 8 <= t->layout_sent.len; i++) {
		uint32_t c = wh_u32(b + 4 + i * 8);
		if (c < t->ncols) ListView_SetColumnWidth(t->ctl, (int)c, px(t, wh_f32(b + 8 + i * 8)));
	}
	InvalidateRect(t->ctl, NULL, FALSE);
}

void wh_table_resized(node *n) {
	wh_table *t = n->table;
	if (!t || !t->ctl) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	SetWindowPos(t->ctl, NULL, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	rgba bg = wh_backdrop(n);
	if (memcmp(&bg, &t->bg, sizeof bg)) wh_table_theme(n);
	sync_views(t);
}

// wh_table_release lets go of the cells (their windows go with the
// control's); what's bound in them is left without a parent, which the
// core destroys.
void wh_table_release(node *n) {
	wh_table *t = n->table;
	if (!t) return;
	for (uint32_t i = 0; i < t->nslots; i++) {
		tslot *s = t->slots[i];
		node *c = s->cell;
		for (uint32_t k = 0; k < c->nkids; k++)
			if (c->kids[k]->parent == c) c->kids[k]->parent = NULL;
		c->nkids = 0;
		c->host_data = NULL;
		c->table = NULL;
		wh_map_del(&H.nodes, c->id);
		wh_free_node(c);
		free(s->text);
		free(s->off);
		free(s);
	}
	free(t->slots);
	if (t->header) wh_unsubclass(t->header);
	if (t->ctl) wh_unsubclass(t->ctl);
	if (t->il) ImageList_Destroy(t->il);
	free(t->ids);
	free(t->pend);
	free_cols(t->cols, t->ncols);
	free(t->shown);
	free(t->levels);
	free(t->pend_levels);
	free(t->items);
	free(t->rows.keys);
	free(t->rows.vals);
	ak_buf_free(&t->sel_known);
	ak_buf_free(&t->sel_want);
	ak_buf_free(&t->scratch);
	ak_buf_free(&t->layout_sent);
	free(t);
	n->table = NULL;
}

// ---- For the automation ----

static wh_table *shown_table(bool outline) {
	wh_table *best = NULL;
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (n->table && n->kind != KIND_TABLE_CELL && n->table->outline == outline && wh_on_screen(n) && (!best || n->id > best->n->id)) best = n->table;
	}
	return best;
}

bool wh_table_row_point(int64_t row, POINT *p) {
	wh_table *t = shown_table(false);
	if (!t || row < 0 || row >= (int64_t)t->nids) return false;
	ListView_EnsureVisible(t->ctl, (int)row, FALSE);
	RECT r;
	if (!ListView_GetItemRect(t->ctl, (int)row, &r, LVIR_BOUNDS)) return false;
	RECT c;
	GetClientRect(t->ctl, &c);
	*p = (POINT){wh_mini(r.left + px(t, 40), c.right / 2), (r.top + r.bottom) / 2};
	ClientToScreen(t->ctl, p);
	return true;
}

bool wh_table_header_point(const wchar_t *title, POINT *p) {
	wh_table *t = shown_table(false);
	if (!t || !t->header) return false;
	for (uint32_t i = 0; i < t->ncols; i++) {
		RECT r;
		if (wcscmp(t->cols[i].title, title) || !Header_GetItemRect(t->header, (int)i, &r)) continue;
		*p = (POINT){(r.left + r.right) / 2, (r.top + r.bottom) / 2};
		ClientToScreen(t->header, p);
		return true;
	}
	return false;
}

bool wh_outline_point(const wchar_t *text, bool disclosure, POINT *p) {
	wh_table *t = shown_table(true);
	if (!t) return false;
	for (uint32_t i = 0; i < t->nids; i++) {
		tslot *s = bind_row(t, i);
		if (!s || wcscmp(slot_str(s, 0), text)) continue;
		TreeView_EnsureVisible(t->ctl, t->items[i]);
		RECT r, c;
		if (!TreeView_GetItemRect(t->ctl, t->items[i], &r, FALSE)) return false;
		GetClientRect(t->ctl, &c);
		int at = px(t, OUTLINE_LEAD + INDENT * (float)row_depth(t, i) + (disclosure ? DISCLOSURE / 2 : DISCLOSURE + ICON + 20));
		*p = (POINT){at, (r.top + r.bottom) / 2};
		ClientToScreen(t->ctl, p);
		return true;
	}
	return false;
}

static void quote(ak_buf *b, const wchar_t *s) {
	int n;
	char *u = wh_utf8(s, -1, &n);
	ak_fmt_quote(b, ak_strn(u, (size_t)n));
	free(u);
}

static void indent(ak_buf *b, int depth) {
	for (int d = 0; d < depth; d++) ak_buf_put(b, "  ", 2);
}

// wh_table_dump describes what the control shows, as its own messages
// read it back (LVM_GETITEMTEXT, TVM_GETITEM: the text it asks for): a
// table's header (the sort's arrow, the header's own HDF_SORT*), the rows
// in view, the selection, how many cells are bound; an outline's rows.
void wh_table_dump(node *n, ak_buf *b, int depth, void (*walk)(ak_buf *, node *, int)) {
	wh_table *t = n->table;
	if (!t || !t->ctl) return;
	indent(b, depth + 1);
	uint32_t bound = 0;
	for (uint32_t i = 0; i < t->nslots; i++) bound += t->slots[i]->row >= 0;
	ak_buf_printf(b, "rows=%u cells=%u bound=%u", (unsigned)t->nids, (unsigned)t->nslots, (unsigned)bound);
	selected_runs(t, &t->scratch);
	ak_buf_put(b, " selected=[", 11);
	uint32_t runs = wh_u32(t->scratch.data);
	for (uint32_t k = 0; k < runs; k++)
		ak_buf_printf(b, "%s%u+%u", k ? " " : "", (unsigned)wh_u32(t->scratch.data + 4 + k * 8), (unsigned)wh_u32(t->scratch.data + 8 + k * 8));
	ak_buf_put(b, "]\n", 2);
	wchar_t buf[512];
	if (!t->outline) {
		indent(b, depth + 1);
		ak_buf_put(b, "header:", 7);
		read_order(t);
		for (uint32_t d = 0; d < t->ncols; d++) {
			int c = t->shown[d];
			HDITEMW hi = {.mask = HDI_FORMAT | HDI_WIDTH};
			Header_GetItem(t->header, c, &hi);
			ak_buf_u8(b, ' ');
			quote(b, t->cols[c].title);
			ak_buf_printf(b, "%s:%.0f", (hi.fmt & HDF_SORTUP) ? "^" : (hi.fmt & HDF_SORTDOWN) ? "v" : "", (double)wh_pt(n, hi.cxy));
		}
		ak_buf_u8(b, '\n');
		// Where the columns' headers are on the screen (pixels): what a
		// test drags.
		indent(b, depth + 1);
		ak_buf_put(b, "header at:", 10);
		for (uint32_t d = 0; d < t->ncols; d++) {
			RECT r;
			if (!Header_GetItemRect(t->header, t->shown[d], &r)) continue;
			MapWindowPoints(t->header, NULL, (POINT *)&r, 2);
			ak_buf_printf(b, " %ld-%ld", (long)wh_mini(r.left, r.right), (long)wh_maxi(r.left, r.right));
			if (d + 1 == t->ncols) ak_buf_printf(b, " y=%ld", (long)(r.top + r.bottom) / 2);
		}
		ak_buf_u8(b, '\n');
		int top = ListView_GetTopIndex(t->ctl), page = ListView_GetCountPerPage(t->ctl);
		for (int r = top; r < top + page && r < (int)t->nids; r++) {
			indent(b, depth + 1);
			bool sel = ListView_GetItemState(t->ctl, r, LVIS_SELECTED) & LVIS_SELECTED;
			ak_buf_printf(b, "row %d%s:", r, sel ? " selected" : "");
			for (uint32_t c = 0; c < t->ncols; c++) {
				buf[0] = 0;
				ListView_GetItemText(t->ctl, r, (int)c, buf, (int)WH_LEN(buf));
				ak_buf_u8(b, ' ');
				quote(b, buf);
			}
			ak_buf_u8(b, '\n');
		}
	} else {
		HTREEITEM selh = TreeView_GetSelection(t->ctl);
		for (uint32_t r = 0; r < t->nids; r++) {
			TVITEMW it = {.mask = TVIF_TEXT | TVIF_STATE, .hItem = t->items[r], .pszText = buf, .cchTextMax = (int)WH_LEN(buf), .stateMask = TVIS_EXPANDED};
			buf[0] = 0;
			TreeView_GetItem(t->ctl, &it);
			uint8_t f = row_flags(t, r);
			indent(b, depth + 1 + row_depth(t, r));
			ak_buf_put(b, (f & 1) ? ((f & 2) ? "v " : "> ") : "  ", 2);
			quote(b, buf);
			if (t->items[r] == selh && t->mode != AK_SELECTION_MODE_NONE) ak_buf_put(b, " selected", 9);
			ak_buf_u8(b, '\n');
		}
	}
	for (uint32_t i = 0; i < t->nslots; i++) {
		node *c = t->slots[i]->cell;
		if (!c->nkids || t->slots[i]->row < 0) continue;
		indent(b, depth + 1);
		ak_buf_printf(b, "row %lld:\n", (long long)t->slots[i]->row);
		for (uint32_t k = 0; k < c->nkids; k++) walk(b, c->kids[k], depth + 2);
	}
}
