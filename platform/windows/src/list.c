// list.c: a List is core-virtualized (no Win32 list hosts arbitrary rows):
// a view whose rows the host places itself. As it scrolls, rows coming
// into view take a row view (a recycled one, or a new one) and its cells,
// host-made views the core binds rows into (asuka_list_bind); rows going
// out are kept for reuse. A row's height is its cells' (a grid's row holds
// Columns of them), from their frames; rows not yet bound are estimated.
// A section's header is a row of its own (one cell, bound with the index
// -1 - the section's); a plain or bare list's header stays at the top
// while its section's rows scroll under it.
#include "nav.h"

typedef struct {
	uint32_t start;
	int64_t id;
} lsection;

typedef struct {
	bool header;
	uint32_t section, first, count;
} lrow;

struct wh_cell {
	node *n;
	wh_list *l;
	int64_t item;
	float height;
	actions swipe, leading;
	bool header, selected;
	wh_rowview *rv;
};

struct wh_rowview {
	node *n;  // a host node of kind KIND_ROW
	wh_cell **cells;
	uint32_t ncells;
	int row;
	bool sep;
	float card;   // an inset grouped list's row: its card's height
	bool pinned;  // a pinned header's: drawn on the window's background
};

struct wh_list {
	node *n;
	int64_t *ids;
	uint32_t nids;
	wh_map heights;  // by item ID (or a header's section's): a float's bits + 1
	lrow *rows;
	uint32_t nrows, caprows;
	lsection *sections;
	uint32_t nsections;
	float *tops;  // each row's top, then the total height
	uint32_t ntops;
	wh_map shown;  // row index + 1 → its row view
	wh_rowview **free_rows;
	uint32_t nfree;
	uint32_t columns;
	float gap, width, est, offset;
	int appearance;
	float inset;
	bool bottom, stuck;
	float lead;
	bool selecting;
	int64_t *pending;
	uint32_t npending;
	lsection *pending_s;
	uint32_t npending_s;
	bool updated, tiling, queued, rebind, applied;
};

#define CARD_GAP 4

static bool height_of(wh_list *l, int64_t id, float *out) {
	void *v = wh_map_get(&l->heights, (uint64_t)id);
	if (!v) return false;
	uint32_t bits = (uint32_t)((uintptr_t)v - 1);
	memcpy(out, &bits, 4);
	return true;
}

static void set_height_of(wh_list *l, int64_t id, float h) {
	uint32_t bits;
	memcpy(&bits, &h, 4);
	wh_map_put(&l->heights, (uint64_t)id, (void *)((uintptr_t)bits + 1));
}

void wh_list_new(node *n) {
	wh_list *l = wh_alloc(sizeof *l);
	l->n = n, l->columns = 1, l->est = 44, l->stuck = true;
	n->list = l;
	n->hwnd = wh_make_view(WS_TABSTOP);
}

static void queue(wh_list *l);

void wh_list_set_prop(node *n, uint16_t p, const ak_value *v) {
	wh_list *l = n->list;
	switch (p) {
	case AK_PROP_LIST_REFRESHABLE:
		wh_set_refreshable(n, v->i != 0);
		return;
	case AK_PROP_LIST_REFRESHING:
		wh_set_refreshing(n, v->i != 0);
		return;
	case AK_PROP_LIST_COLUMNS: l->columns = v->i > 1 ? (uint32_t)v->i : 1; break;
	case AK_PROP_LIST_GAP: l->gap = (float)v->f; break;
	case AK_PROP_LIST_APPEARANCE: l->appearance = (int)v->i; break;
	case AK_PROP_LIST_INSET: l->inset = (float)v->f; break;
	case AK_PROP_LIST_ANCHOR:
		l->bottom = v->i == AK_SCROLL_ANCHOR_BOTTOM;
		l->stuck = true;
		break;
	case AK_PROP_LIST_SELECTING: {
		l->selecting = v->i != 0;
		uint32_t i = 0;
		void *rv;
		while (wh_map_next(&l->shown, &i, NULL, &rv)) InvalidateRect(((wh_rowview *)rv)->n->hwnd, NULL, FALSE);
		return;
	}
	}
	l->rebind = true;
	queue(l);
}

// wh_list_update takes the core's new item IDs and sections (u32 count,
// u64 each; u32 sections, u32 start and u64 ID each); rows bind again once
// the core has returned, so binding never re-enters a flush.
void wh_list_update(node *n, const uint8_t *args, size_t len) {
	wh_list *l = n->list;
	if (len < 4) return;
	uint32_t count = wh_u32(args);
	size_t o = 4;
	free(l->pending);
	l->pending = wh_alloc((count ? count : 1) * sizeof *l->pending);
	l->npending = 0;
	for (uint32_t i = 0; i < count && o + 8 <= len; i++, o += 8) l->pending[l->npending++] = (int64_t)wh_u64(args + o);
	free(l->pending_s);
	l->pending_s = NULL;
	l->npending_s = 0;
	if (o + 4 <= len) {
		uint32_t ns = wh_u32(args + o);
		o += 4;
		l->pending_s = wh_alloc((ns ? ns : 1) * sizeof *l->pending_s);
		for (uint32_t i = 0; i < ns && o + 12 <= len; i++, o += 12) l->pending_s[l->npending_s++] = (lsection){wh_u32(args + o), (int64_t)wh_u64(args + o + 4)};
	}
	l->updated = true;
	l->rebind = true;
	queue(l);
}

static void push_row(wh_list *l, lrow r) {
	if (l->nrows == l->caprows) {
		l->caprows = l->caprows ? l->caprows * 2 : 64;
		l->rows = wh_realloc(l->rows, l->caprows * sizeof *l->rows);
	}
	l->rows[l->nrows++] = r;
}

// make_rows lays the items and the sections' headers out in rows.
static void make_rows(wh_list *l) {
	l->nrows = 0;
#define ADD(sec, from, to) \
	for (uint32_t i = (from); i < (to); i += l->columns) push_row(l, (lrow){false, (sec), i, wh_mini((int)l->columns, (int)((to) - i))})
	if (!l->nsections) {
		ADD(0, 0, l->nids);
		return;
	}
	for (uint32_t s = 0; s < l->nsections; s++) {
		uint32_t end = s + 1 < l->nsections ? l->sections[s + 1].start : l->nids;
		push_row(l, (lrow){true, s, 0, 0});
		uint32_t from = wh_mini((int)l->sections[s].start, (int)l->nids), to = wh_mini(wh_maxi((int)end, (int)l->sections[s].start), (int)l->nids);
		ADD(s, from, to);
	}
#undef ADD
}

static bool grouped(const wh_list *l) { return l->appearance == AK_LIST_APPEARANCE_INSET_GROUPED && l->columns <= 1; }
static float pad(const wh_list *l) { return grouped(l) ? l->inset : 0; }
static bool sticky(const wh_list *l) { return l->nsections > 0 && l->appearance != AK_LIST_APPEARANCE_INSET_GROUPED; }

static int64_t row_key(wh_list *l, uint32_t r) {
	lrow row = l->rows[r];
	return row.header ? l->sections[row.section].id : l->ids[row.first];
}

// row_height is a row's height: its cells' tallest, estimated until
// they're bound (a grid's rows add the gap below, an inset grouped list's
// the space between cards, but not before a header or at the end).
static float row_height(wh_list *l, uint32_t r) {
	lrow row = l->rows[r];
	float height = 0, v;
	bool known = false;
	if (row.header) known = height_of(l, l->sections[row.section].id, &height);
	for (uint32_t i = 0; i < row.count; i++)
		if (height_of(l, l->ids[row.first + i], &v)) height = known ? wh_maxf(height, v) : v, known = true;
	if (!known) height = l->est;
	if (!row.header && r + 1 < l->nrows && !l->rows[r + 1].header) {
		if (l->columns > 1) height += l->gap;
		if (grouped(l)) height += CARD_GAP;
	}
	return height;
}

static float viewport(wh_list *l) {
	RECT r;
	GetClientRect(l->n->hwnd, &r);
	return wh_pt(l->n, r.bottom);
}

static void measure_tops(wh_list *l) {
	l->tops = wh_realloc(l->tops, (l->nrows + 1) * sizeof *l->tops);
	l->ntops = l->nrows + 1;
	float y = pad(l);
	for (uint32_t r = 0; r < l->nrows; r++) {
		l->tops[r] = y;
		y += row_height(l, r);
	}
	l->tops[l->nrows] = y + pad(l);
	// Anchored at the bottom, rows that don't fill it sit at its bottom.
	l->lead = 0;
	if (l->bottom && l->n->hwnd) {
		float view = viewport(l), total = l->tops[l->nrows];
		if (total < view) {
			l->lead = view - total;
			for (uint32_t i = 0; i < l->ntops; i++) l->tops[i] += l->lead;
		}
	}
}

// first_below is the first row whose bottom is past y.
static uint32_t first_below(wh_list *l, float y) {
	uint32_t lo = 0, hi = l->nrows;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (l->tops[mid + 1] > y) hi = mid;
		else lo = mid + 1;
	}
	return lo;
}

static uint32_t first_at(wh_list *l, float y) {
	uint32_t lo = 0, hi = l->nrows;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (l->tops[mid] >= y) hi = mid;
		else lo = mid + 1;
	}
	return lo;
}

// anchor_row is the row at the top of what the list shows, by its key,
// and how far its top is below the top of the view.
static bool anchor_row(wh_list *l, int64_t *key, float *dy) {
	if (!l->nrows || l->ntops != l->nrows + 1) return false;
	uint32_t r = first_below(l, l->offset);
	if (r >= l->nrows) return false;
	*key = row_key(l, r), *dy = l->tops[r] - l->offset;
	return true;
}

// pinned_row is the header row that stays at the top of a plain list:
// its section's, once its own place has scrolled above the top (-1).
static int pinned_row(wh_list *l) {
	if (!sticky(l) || !l->nrows) return -1;
	uint32_t r = first_below(l, l->offset);
	if (r >= l->nrows) return -1;
	uint32_t s = l->rows[r].section;
	for (int h = (int)r; h >= 0; h--)
		if (l->rows[h].header && l->rows[h].section == s) return l->tops[h] >= l->offset ? -1 : h;
	return -1;
}

static wh_rowview *shown_get(wh_list *l, uint32_t r) { return wh_map_get(&l->shown, (uint64_t)r + 1); }

static wh_rowview *take(wh_list *l) {
	if (l->nfree) return l->free_rows[--l->nfree];
	wh_rowview *rv = wh_alloc(sizeof *rv);
	rv->row = -1;
	node *rn = wh_node_new(wh_host_id(), KIND_ROW);
	rn->hwnd = wh_make_view(0);
	rn->row = rv;
	rn->k = l->n->k, rn->dir = l->n->dir;
	rv->n = rn;
	SetParent(rn->hwnd, l->n->hwnd);
	wh_node_add(rn);
	return rv;
}

void wh_list_set_direction(node *n, uint8_t d) {
	wh_list *l = n->list;
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&l->shown, &i, NULL, &v)) ((wh_rowview *)v)->n->dir = d;
	for (uint32_t j = 0; j < l->nfree; j++) l->free_rows[j]->n->dir = d;
}

static wh_cell *new_cell(wh_list *l, wh_rowview *rv) {
	node *n = wh_node_new(wh_host_id(), KIND_CELL);
	n->hwnd = wh_make_view(0);
	wh_cell *c = wh_alloc(sizeof *c);
	c->n = n, c->l = l, c->rv = rv;
	n->k = rv->n->k;
	n->cell = c;
	SetParent(n->hwnd, rv->n->hwnd);
	wh_node_add(n);
	return c;
}

// bind binds row r's items (or its section's header) into the row view's
// cells.
static void bind_row(wh_list *l, wh_rowview *rv, uint32_t r) {
	lrow row = l->rows[r];
	uint32_t k = row.header ? 1 : row.count;
	while (rv->ncells < k) {
		rv->cells = wh_realloc(rv->cells, (rv->ncells + 1) * sizeof *rv->cells);
		rv->cells[rv->ncells] = new_cell(l, rv);
		rv->ncells++;
	}
	for (uint32_t i = 0; i < rv->ncells; i++) ShowWindow(rv->cells[i]->n->hwnd, i >= k ? SW_HIDE : SW_SHOWNA);
	rv->sep = l->columns <= 1 && !grouped(l) && l->appearance != AK_LIST_APPEARANCE_BARE && !row.header && r + 1 < l->nrows && !l->rows[r + 1].header;
	for (uint32_t i = 0; i < k; i++) {
		wh_cell *c = rv->cells[i];
		c->header = row.header;
		int64_t index = (int64_t)row.first + i;
		if (row.header) {
			c->item = l->sections[row.section].id;
			index = -1 - (int64_t)row.section;
		} else {
			c->item = l->ids[index];
		}
		float v;
		if (height_of(l, c->item, &v)) c->height = v;
		wh_core_begin();
		asuka_list_bind(l->n->id, index, c->n->id);
		wh_core_end();
	}
	InvalidateRect(rv->n->hwnd, NULL, FALSE);
}

static void row_x(wh_list *l, float *x, float *w) {
	if (grouped(l)) *x = l->inset, *w = wh_maxf(l->width - 2 * l->inset, 0);
	else *x = 0, *w = l->width;
}

// place_cells lays out a row's cells: a grid's side by side, the first on
// the right, right to left.
static void place_cells(wh_list *l, wh_rowview *rv, uint32_t r) {
	UINT f = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
	if (l->columns <= 1 || l->rows[r].header) {
		float x, w;
		row_x(l, &x, &w);
		rv->card = 0;
		if (grouped(l) && !l->rows[r].header) {
			rv->card = row_height(l, r);
			if (r + 1 < l->nrows && !l->rows[r + 1].header) rv->card -= CARD_GAP;
		}
		for (uint32_t i = 0; i < rv->ncells; i++) SetWindowPos(rv->cells[i]->n->hwnd, NULL, 0, 0, wh_px(l->n, w), wh_px(l->n, rv->cells[i]->height), f);
		InvalidateRect(rv->n->hwnd, NULL, FALSE);
		return;
	}
	float cw = (l->width - l->gap * (float)(l->columns - 1)) / (float)l->columns;
	bool rtl = wh_is_rtl(l->n);
	for (uint32_t i = 0; i < rv->ncells; i++) {
		float x = (float)i * (cw + l->gap);
		if (rtl) x = l->width - x - cw;
		SetWindowPos(rv->cells[i]->n->hwnd, NULL, wh_px(l->n, x), 0, wh_px(l->n, x + cw) - wh_px(l->n, x), wh_px(l->n, rv->cells[i]->height), f);
	}
}

// place_rows moves the shown rows to their places below the scroll
// position, at once (one DeferWindowPos); a pinned header at the top,
// pushed up by the next header, over the rows.
static void place_rows(wh_list *l) {
	HDWP d = BeginDeferWindowPos((int)l->shown.len);
	HWND pinned = NULL;
	uint32_t i = 0;
	uint64_t key;
	void *v;
	while (wh_map_next(&l->shown, &i, &key, &v)) {
		uint32_t r = (uint32_t)key - 1;
		wh_rowview *rv = v;
		float y = l->tops[r] - l->offset, height = row_height(l, r), x, w;
		row_x(l, &x, &w);
		rv->pinned = false;
		if (l->rows[r].header && sticky(l) && y < 0) {
			y = 0;
			for (uint32_t n = r + 1; n < l->nrows; n++)
				if (l->rows[n].header) {
					y = wh_minf(y, l->tops[n] - l->offset - height);
					break;
				}
			rv->pinned = true;
			pinned = rv->n->hwnd;
		}
		int x0 = wh_px(l->n, x), y0 = wh_px(l->n, y);
		if (d) d = DeferWindowPos(d, rv->n->hwnd, NULL, x0, y0, wh_px(l->n, x + w) - x0, wh_px(l->n, y + height) - y0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
	}
	if (d) EndDeferWindowPos(d);
	if (pinned) SetWindowPos(pinned, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // over the rows that scroll under it
	i = 0;
	while (wh_map_next(&l->shown, &i, &key, &v)) place_cells(l, v, (uint32_t)key - 1);
}

// show_row gives row r a row view, bound if it's new to it; true if its
// height changed (an estimate made real).
static bool show_row(wh_list *l, uint32_t r) {
	wh_rowview *rv = shown_get(l, r);
	bool fresh = !rv;
	if (fresh) {
		rv = take(l);
		wh_map_put(&l->shown, (uint64_t)r + 1, rv);
	}
	if (!fresh && !l->rebind && rv->row == (int)r) return false;
	rv->row = (int)r;
	float was = row_height(l, r);
	bind_row(l, rv, r);
	return row_height(l, r) != was;
}

// tile_keeping shows the rows in view (and half a screen around them),
// binds the ones new to the view, and places them, keeping the row with
// key dy below the top (if keep). Rows above the top that change height
// as they bind don't move what's in view; a list anchored at its bottom
// and at its end stays there.
static void tile_keeping(wh_list *l, int64_t key, float dy, bool keep) {
	if (l->tiling || !l->n->hwnd) return;
	l->tiling = true;
	float before = l->offset;
	for (int pass = 0; pass < 4; pass++) {
		measure_tops(l);
		float total = l->tops[l->nrows], view = viewport(l);
		if (l->bottom && l->stuck) {
			l->offset = total - view;
		} else if (keep) {
			for (uint32_t r = 0; r < l->nrows; r++)
				if (row_key(l, r) == key) {
					l->offset = l->tops[r] - dy;
					break;
				}
		}
		l->offset = wh_clampf(l->offset, 0, wh_maxf(total - view, 0));
		float top = l->offset - view / 2, bottom = l->offset + view * 1.5f;
		uint32_t first = first_below(l, top), last = first_at(l, bottom);
		int pin = pinned_row(l);
		// Rows gone from view go back to the pool.
		uint32_t i = 0;
		uint64_t k;
		void *v;
		uint64_t gone[512];
		uint32_t ngone = 0;
		while (wh_map_next(&l->shown, &i, &k, &v)) {
			uint32_t r = (uint32_t)k - 1;
			if ((r < first || r >= last) && (int)r != pin && ngone < WH_LEN(gone)) gone[ngone++] = k;
		}
		for (uint32_t g = 0; g < ngone; g++) {
			wh_rowview *rv = wh_map_get(&l->shown, gone[g]);
			wh_map_del(&l->shown, gone[g]);
			ShowWindow(rv->n->hwnd, SW_HIDE);
			l->free_rows = wh_realloc(l->free_rows, (l->nfree + 1) * sizeof *l->free_rows);
			l->free_rows[l->nfree++] = rv;
		}
		bool changed = false;
		for (uint32_t r = first; r < last; r++) changed |= show_row(l, r);
		if (pin >= 0 && ((uint32_t)pin < first || (uint32_t)pin >= last)) changed |= show_row(l, (uint32_t)pin);
		l->rebind = false;
		place_rows(l);
		if (!changed) break;
	}
	wh_update_bars(l->n);
	l->tiling = false;
	if (l->offset != before) {
		ak_event e = {.kind = AK_EVENT_SCROLLED, .x = 0, .y = l->offset};
		wh_emit(l->n, &e);
	}
}

static void tile(wh_list *l) {
	int64_t key = 0;
	float dy = 0;
	bool ok = anchor_row(l, &key, &dy);
	tile_keeping(l, key, dy, ok);
}

// flush applies an update waiting, if any, and tiles. Rows added or
// removed above the ones in view keep those where they are, unless the
// list is at the edge it's anchored to.
static void flush(wh_list *l) {
	if (!l->updated) {
		tile(l);
		return;
	}
	l->updated = false;
	int64_t key = 0;
	float dy = 0;
	bool ok = anchor_row(l, &key, &dy);
	if ((l->bottom && l->stuck) || (!l->bottom && l->offset <= 0)) ok = false;
	free(l->ids);
	l->ids = l->pending, l->nids = l->npending;
	free(l->sections);
	l->sections = l->pending_s, l->nsections = l->npending_s;
	l->pending = NULL, l->pending_s = NULL, l->npending = l->npending_s = 0;
	// The heights of the items and sections still there.
	wh_map live = {0};
	float v;
	for (uint32_t i = 0; i < l->nids; i++)
		if (height_of(l, l->ids[i], &v)) {
			uint32_t bits;
			memcpy(&bits, &v, 4);
			wh_map_put(&live, (uint64_t)l->ids[i], (void *)((uintptr_t)bits + 1));
		}
	for (uint32_t i = 0; i < l->nsections; i++)
		if (height_of(l, l->sections[i].id, &v)) {
			uint32_t bits;
			memcpy(&bits, &v, 4);
			wh_map_put(&live, (uint64_t)l->sections[i].id, (void *)((uintptr_t)bits + 1));
		}
	free(l->heights.keys);
	free(l->heights.vals);
	l->heights = live;
	make_rows(l);
	tile_keeping(l, key, dy, ok);
	l->applied = l->applied || l->nids > 0;
}

static void flush_later(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (!n || !n->list) return;
	n->list->queued = false;
	if (n->hwnd) flush(n->list);
}

// queue tiles once the core has returned.
static void queue(wh_list *l) {
	if (l->queued) return;
	l->queued = true;
	wh_after_core(flush_later, (void *)(uintptr_t)l->n->id);
}

void wh_list_rebind(node *n) {
	n->list->rebind = true;
	queue(n->list);
}

// wh_cell_set_height takes a row's height from its cell's frame.
void wh_cell_set_height(node *n, float height) {
	wh_cell *c = n->cell;
	wh_list *l = c->l;
	c->height = height;
	float old;
	bool had = height_of(l, c->item, &old);
	if (had && old == height) return;
	if (!had && l->heights.len < 64 && !c->header) l->est = (l->est * (float)l->heights.len + height) / (float)(l->heights.len + 1);  // the estimate follows the first rows'
	set_height_of(l, c->item, height);
	if (!l->tiling) queue(l);
}

void wh_cell_set_prop(node *n, uint16_t p, const ak_value *v) {
	wh_cell *c = n->cell;
	if (p == AK_PROP_SWIPE_ACTIONS) actions_decode(&c->swipe, (const uint8_t *)v->s.p, v->s.len);
	else if (p == AK_PROP_LEADING_SWIPE_ACTIONS) actions_decode(&c->leading, (const uint8_t *)v->s.p, v->s.len);
	else if (p == AK_PROP_SELECTED) {
		c->selected = v->i != 0;
		InvalidateRect(c->rv->n->hwnd, NULL, FALSE);
	}
}

static bool row_selected(wh_rowview *rv) {
	for (uint32_t i = 0; i < rv->ncells; i++)
		if (rv->cells[i]->selected && !rv->cells[i]->header && rv->cells[i]->l->selecting) return true;
	return false;
}

// wh_paint_row draws a list row's separator (a hairline at its bottom,
// inset as the content is, but the last row's), an inset grouped list's
// row's card (Windows 11's settings cards), a selected row's fill.
void wh_paint_row(node *n, gfx g, float w, float h) {
	wh_rowview *rv = n->row;
	if (!rv) return;
	float k = wh_scale_of(n);
	if (rv->pinned) g_fill_rect(g, rgba_argb(H.theme.bg), 0, 0, w, h);
	if (row_selected(rv)) {
		// A selected row: the subtle fill and the accent's pill at its
		// start, as a WinUI list's selected item.
		g_fill_round_rect(g, rgba_argb(H.theme.subtle_hot), 0, 0, w, h, 4 * k);
		float pill = 3 * k, x = wh_is_rtl(n) ? w - pill : 0, ph = wh_minf(16 * k, h / 2);
		g_fill_round_rect(g, rgba_argb(H.theme.accent), x, (h - ph) / 2, pill, ph, pill / 2);
	}
	if (rv->card > 0) {
		float ch = wh_minf(rv->card * k, h);
		g_fill_round_rect(g, rgba_argb(H.theme.card), 0, 0, w, ch, 4 * k);
		g_stroke_round_rect(g, rgba_argb(H.theme.card_stroke), wh_maxf(k, 1), 0, 0, w, ch, 4 * k);
		return;
	}
	if (!rv->sep) return;
	float x = wh_is_rtl(n) ? 0 : 16 * k;  // inset at the start
	g_fill_rect(g, rgba_argb(H.theme.separator), x, h - wh_maxf(k, 1), w - 16 * k, wh_maxf(k, 1));
}

// wh_list_resized lays the rows out at the list's new width.
void wh_list_resized(node *n) {
	wh_list *l = n->list;
	if (n->frame[2] != l->width) {
		l->width = n->frame[2];
		l->rebind = true;  // the core lays the rows out at the new width
	}
	queue(l);
}

float wh_list_offset(node *n) { return n->list->offset; }
float wh_list_content(node *n) { return n->list->ntops ? n->list->tops[n->list->ntops - 1] : 0; }

// wh_list_scroll moves the list to an offset (clamped) and tiles; the
// user's scrolling moves a list anchored at its bottom off its end, or
// back.
void wh_list_scroll(node *n, float y) {
	wh_list *l = n->list;
	flush(l);
	if (!l->ntops) return;
	float total = l->tops[l->ntops - 1], view = viewport(l);
	y = wh_clampf(y, 0, wh_maxf(total - view, 0));
	if (l->bottom) l->stuck = y >= wh_maxf(total - view, 0) - 1;
	if (y == l->offset) return;
	l->offset = y;
	tile(l);
	wh_show_bars(n);
	wh_overlays_dirty();  // an anchor in a row moved
	ak_event e = {.kind = AK_EVENT_SCROLLED, .x = 0, .y = l->offset};
	wh_emit(n, &e);
}

// wh_list_scroll_to scrolls the least that shows an item (below a header
// pinned over it).
void wh_list_scroll_to(node *n, uint32_t index) {
	wh_list *l = n->list;
	flush(l);
	if (index >= l->nids) return;
	int row = -1;
	for (uint32_t r = 0; r < l->nrows && row < 0; r++)
		if (!l->rows[r].header && index >= l->rows[r].first && index < l->rows[r].first + l->rows[r].count) row = (int)r;
	if (row < 0) return;
	float top = l->tops[row], bottom = l->tops[row + 1];
	if (sticky(l))
		for (int r = row; r >= 0; r--)
			if (l->rows[r].header) {
				top -= row_height(l, (uint32_t)r);
				break;
			}
	float view = viewport(l), y = l->offset;
	if (top < y) y = top;
	else if (bottom > y + view) y = bottom - view;
	wh_list_scroll(n, y);
}

// wh_select_click toggles the selection of the row n is in, if it's in a
// selecting list; false: it isn't. Ctrl on a row of a list that isn't
// selecting begins selecting there (RowSelectBegin), as Ctrl-click
// selects many in Explorer, when the row listens.
bool wh_select_click(node *n) {
	for (node *p = n; p; p = p->parent) {
		wh_cell *c = p->cell;
		if (!c) continue;
		if (c->header) return false;
		if (!c->l->selecting) {
			if (GetKeyState(VK_CONTROL) >= 0 || !node_listening(c->n, AK_EVENT_ROW_SELECT_BEGIN)) return false;
			wh_emit(c->n, &(ak_event){.kind = AK_EVENT_ROW_SELECT_BEGIN});
			return true;
		}
		c->selected = !c->selected;
		InvalidateRect(c->rv->n->hwnd, NULL, FALSE);
		wh_emit_bool(c->n, AK_EVENT_ROW_SELECTED, c->selected);
		return true;
	}
	return false;
}

typedef struct {
	uint32_t id;
	actions all;
	uint32_t nleading;
} row_menu_job;

static void row_menu_now(void *ctx) {
	row_menu_job *j = ctx;
	node *n = wh_node(j->id);
	if (n && n->hwnd) {
		POINT p;
		GetCursorPos(&p);
		int64_t got = wh_track_actions(&j->all, wh_is_rtl(n), p, GetAncestor(n->hwnd, GA_ROOT));
		if (got >= 0 && (n = wh_node(j->id))) wh_emit_int(n, AK_EVENT_ACTION, got < j->nleading ? -1 - got : got - j->nleading);
	}
	actions_free(&j->all);
	free(j);
}

// wh_row_menu shows a row's swipe actions, leading then trailing, as its
// context menu (a mouse has no swipes); a leading one's Action is -1 - its
// index. False: it has none.
bool wh_row_menu(node *n) {
	wh_cell *c = n->cell;
	if (!c || c->l->selecting || !(c->leading.n + c->swipe.n)) return false;
	row_menu_job *j = wh_alloc(sizeof *j);
	j->id = c->n->id;
	j->nleading = c->leading.n;
	actions both = {wh_alloc((c->leading.n + c->swipe.n) * sizeof(action)), c->leading.n + c->swipe.n};
	memcpy(both.v, c->leading.v, c->leading.n * sizeof(action));
	memcpy(both.v + c->leading.n, c->swipe.v, c->swipe.n * sizeof(action));
	j->all = actions_flat(&both);
	free(both.v);
	wh_after_core(row_menu_now, j);
	return true;
}

// wh_list_release forgets the host's rows and cells: the list's window
// takes their windows with it.
// forget_host_node lets go of a row's or cell's node: out of the nodes,
// what's bound in it left without a parent (the core destroys it), freed
// once the message being handled returns. Its window goes with the
// list's.
static void forget_host_node(node *x) {
	for (uint32_t i = 0; i < x->nkids; i++)
		if (x->kids[i]->parent == x) x->kids[i]->parent = NULL;
	x->nkids = 0;
	x->cell = NULL;
	x->row = NULL;
	wh_map_del(&H.nodes, x->id);
	wh_free_node(x);
}

void wh_list_release(node *n) {
	wh_list *l = n->list;
	if (!l) return;
	uint32_t i = 0;
	void *v;
	wh_rowview **all = wh_alloc((l->shown.len + l->nfree + 1) * sizeof *all);
	uint32_t na = 0;
	while (wh_map_next(&l->shown, &i, NULL, &v)) all[na++] = v;
	for (uint32_t j = 0; j < l->nfree; j++) all[na++] = l->free_rows[j];
	for (uint32_t j = 0; j < na; j++) {
		wh_rowview *rv = all[j];
		for (uint32_t c = 0; c < rv->ncells; c++) {
			forget_host_node(rv->cells[c]->n);
			actions_free(&rv->cells[c]->swipe);
			actions_free(&rv->cells[c]->leading);
			free(rv->cells[c]);
		}
		free(rv->cells);
		forget_host_node(rv->n);
		free(rv);
	}
	free(all);
	free(l->shown.keys);
	free(l->shown.vals);
	free(l->free_rows);
	free(l->ids);
	free(l->heights.keys);
	free(l->heights.vals);
	free(l->rows);
	free(l->sections);
	free(l->tops);
	free(l->pending);
	free(l->pending_s);
	free(l);
	n->list = NULL;
}

// For the dump: the rows shown and what's in them.
void wh_list_dump(node *n, ak_buf *b, int depth, void (*walk)(ak_buf *, node *, int)) {
	wh_list *l = n->list;
	for (uint32_t r = 0; r < l->nrows; r++) {
		wh_rowview *rv = shown_get(l, r);
		if (!rv) continue;
		uint64_t k = (uint64_t)r + 1;
		for (uint32_t c = 0; c < rv->ncells; c++) {
			for (int d = 0; d < depth + 1; d++) ak_buf_put(b, "  ", 2);
			ak_buf_printf(b, "row %u:\n", (unsigned)(k - 1));
			node *cn = rv->cells[c]->n;
			for (uint32_t j = 0; j < cn->nkids; j++) walk(b, cn->kids[j], depth + 2);
		}
	}
}
