// list.c: a List is core-virtualized, as on macOS and Windows: a
// GtkScrolledWindow around a viewport whose child is a view of the host's
// as tall as all the rows, in which the host places the rows itself. As
// it scrolls, rows coming into view take a row view (a recycled one, or
// a new one) and its cells, host-made views the core binds rows into
// (asuka_list_bind); rows going out are kept for reuse. A row's height is
// its cells' (a grid's row holds Columns of them), from their frames; rows
// not yet bound are estimated.
//
// It looks as libadwaita's lists do: a plain list's rows on the view's
// background, a hairline between them; an inset grouped list's in
// .boxed-list cards (a section's each) on the window's background; a bare
// list's on nothing. With sections, each section's header is a row of its
// own (one cell, bound with index -1 - the section's); a plain or bare
// list's header stays at the top while its section's rows scroll under it.
//
// A row's swipe actions are its context menu (a secondary click, a long
// press on a touchscreen), leading then trailing, as on the other desktop
// hosts. A selecting list's click selects or deselects a row
// (RowSelected), highlighted. F5 refreshes a refreshable list on screen,
// a spinner over its top meanwhile. With Reorder, its rows move (Moving
// rows, below).
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

bool lx_refresh_from(node *from);
void lx_popup_at(node *n, GMenuModel *model, double x, double y);
void lx_set_a11y_string(gpointer w, int prop, const char *s);

typedef struct list_t list_t;

// A section as the core sent it.
typedef struct {
	uint32_t start;
	int64_t id;
} lsection;

typedef AK_VEC(int64_t) i64_vec;
typedef AK_VEC(lsection) lsec_vec;

// A row the list shows: a section's header, or items.
typedef struct {
	bool header;
	int section, first, count;
} lrow;

// The host's views in a list: a row's view, a cell (a row is bound
// into), an inset grouped list's card.
enum { PART_ROW = 1, PART_CELL, PART_CARD };

typedef struct row_view row_view;

typedef struct {
	int tag;  // PART_CELL
	node *n;
	list_t *l;
	row_view *rv;
	int64_t item;
	float height;
	action_vec swipe, leading;
	bool header, selected, menu;
	GSimpleAction *moves[2];  // its screen readers' Move up and Move down (Reorder)
} cell;

struct row_view {
	int tag;  // PART_ROW
	node *n;
	list_t *l;
	AK_VEC(cell *) cells;
	int row;
	bool pinned;
	// Moving rows: how far below its place it shows (its transform), easing
	// from shift_from to shift_to since shift_t0; lifted (dragged, or
	// landing); snap: new to the view, it goes where it's to be at once.
	float shift, shift_from, shift_to;
	double shift_t0;
	bool easing, lifted, snap;
};

// A row being dragged (Reorder): armed by a press on it, lifted past the
// drag threshold (a long press on a touchscreen).
typedef struct {
	bool armed, lifted, touch;
	int row;      // its row
	int from, to;  // its item's index, and where it'd be dropped
	int slot;     // its place among the other rows, there
	float grab;   // the pointer below its top
	float h;      // its height
	double x0, y0, vy;  // the press, and the pointer now, in the list's view
	double tick_t;
	row_view *rv;
	GtkGesture *gesture;
} drag_t;

typedef struct {
	int tag;  // PART_CARD
	list_t *l;
} list_card;

// Heights by item ID (or a header's by its section's ID): an open
// addressed table.
typedef struct {
	int64_t key;
	float v;
	bool used;
} hslot;

struct list_t {
	node *n;
	GtkWidget *sw, *vp;
	GtkAdjustment *vadj;
	node *doc;  // the view the rows are in, as tall as they are
	i64_vec ids;
	hslot *heights;
	uint32_t hcap, hlen;
	AK_VEC(lrow) rows;
	AK_VEC(float) tops;  // each row's top, then the total height
	float total;
	lsec_vec sections;
	AK_VEC(row_view *) shown;  // the rows shown, each with its row
	AK_VEC(row_view *) free_;
	AK_VEC(node *) cards;
	int columns;
	float gap, width, est;
	int32_t appearance;
	float inset;
	bool bottom, stuck;
	float lead;
	int settling;
	AdwAnimation *anim;
	bool selecting;
	i64_vec pending;
	lsec_vec pending_s;
	bool updated, tiling, queued, rebind, applied, dead;
	double reported;
	bool refreshable, refreshing;
	GtkWidget *spinner;
	GtkEventController *shortcut;
	// Moving rows (Reorder).
	bool reorder, reorder_wired;
	bool moved;  // the host moved an item: a ListUpdate in its order is nothing to do
	char *move_names[2];
	GtkEventController *reorder_ctrls[3];
	drag_t drag;
	guint shift_tick, scroll_tick;
	GtkEventController *escape;  // Escape, on the window, while a row's lifted
	GtkWidget *escape_on;
};

// ---- Heights ----

static float *hget(list_t *l, int64_t key) {
	if (!l->hcap) return NULL;
	for (uint32_t i = (uint32_t)((uint64_t)key * 0x9e3779b97f4a7c15ull >> 32) & (l->hcap - 1);; i = (i + 1) & (l->hcap - 1)) {
		if (!l->heights[i].used) return NULL;
		if (l->heights[i].key == key) return &l->heights[i].v;
	}
}

static void hput(list_t *l, int64_t key, float v);

static void hgrow(list_t *l) {
	uint32_t old = l->hcap;
	hslot *t = l->heights;
	l->hcap = old ? old * 2 : 256;
	l->heights = calloc(l->hcap, sizeof *l->heights);
	if (!l->heights) abort();
	l->hlen = 0;
	for (uint32_t i = 0; i < old; i++)
		if (t[i].used) hput(l, t[i].key, t[i].v);
	free(t);
}

static void hput(list_t *l, int64_t key, float v) {
	float *p = hget(l, key);
	if (p) {
		*p = v;
		return;
	}
	if ((l->hlen + 1) * 2 > l->hcap) hgrow(l);
	for (uint32_t i = (uint32_t)((uint64_t)key * 0x9e3779b97f4a7c15ull >> 32) & (l->hcap - 1);; i = (i + 1) & (l->hcap - 1))
		if (!l->heights[i].used) {
			l->heights[i] = (hslot){key, v, true};
			l->hlen++;
			return;
		}
}

// ---- Parts ----

static void part_marker(node *n) { (void)n; }  // marks a list's own views: theirs, freed with it

static int part_tag(node *n) { return n->ext_free == part_marker && n->ext ? *(int *)n->ext : 0; }

static list_t *list_of(node *n) { return n->kind == AK_KIND_LIST ? (list_t *)n->list : NULL; }

// host_node makes a view of the host's own, a child of parent's.
static node *host_node(node *parent) {
	node *n = calloc(1, sizeof *n);
	if (!n) abort();
	n->id = AK_HOST_ID_BIT | ++H.next_cell;
	n->kind = AK_KIND_VIEW;
	n->opacity = 1;
	n->selected = -1;
	n->widget = lx_view_new();
	lx_view_set_node(n->widget, n);
	n->parent = parent;
	if (parent) {
		n->dir = parent->dir;
		n->rtl = parent->rtl;
	}
	lx_add_node(n);
	return n;
}

static float offset(list_t *l) { return (float)gtk_adjustment_get_value(l->vadj); }

static float view_h(list_t *l) { return l->n->placed ? l->n->frame[3] : (float)gtk_adjustment_get_page_size(l->vadj); }

static float end_offset(list_t *l) { return fmaxf(l->total - view_h(l), 0); }

static bool at_end(list_t *l) { return !l->tops.len || offset(l) + view_h(l) >= l->tops.data[l->tops.len - 1] - 1; }

static bool grouped(list_t *l) { return l->appearance == AK_LIST_APPEARANCE_INSET_GROUPED && l->columns <= 1; }

static bool sticky(list_t *l) { return l->sections.len > 0 && l->appearance != AK_LIST_APPEARANCE_INSET_GROUPED; }

static float pad(list_t *l) { return grouped(l) ? l->inset : 0; }

static int64_t row_key(list_t *l, int r) {
	lrow *row = &l->rows.data[r];
	return row->header ? l->sections.data[row->section].id : l->ids.data[row->first];
}

static float gap_below(list_t *l, int r) {
	if (l->columns > 1 && !l->rows.data[r].header && (uint32_t)r + 1 < l->rows.len && !l->rows.data[r + 1].header) return l->gap;
	return 0;
}

// row_height is a row's height: its cells' tallest, estimated until they
// are bound.
static float row_height(list_t *l, int r) {
	lrow *row = &l->rows.data[r];
	float height = 0;
	bool known = false;
	if (row->header) {
		float *v = hget(l, l->sections.data[row->section].id);
		if (v) {
			height = *v;
			known = true;
		}
	}
	for (int i = 0; i < row->count; i++) {
		float *v = hget(l, l->ids.data[row->first + i]);
		if (v) {
			height = fmaxf(height, *v);
			known = true;
		}
	}
	if (!known) height = l->est;
	return height + gap_below(l, r);
}

static void measure_tops(list_t *l) {
	l->tops.len = 0;
	float y = pad(l);
	for (uint32_t r = 0; r < l->rows.len; r++) {
		ak_vec_push(&l->tops, y);
		y += row_height(l, (int)r);
	}
	ak_vec_push(&l->tops, y + pad(l));
	// Anchored at the bottom, rows that don't fill it sit at its bottom.
	l->lead = 0;
	if (l->bottom) {
		float vh = view_h(l), total = l->tops.data[l->tops.len - 1];
		if (total < vh) {
			l->lead = vh - total;
			for (uint32_t i = 0; i < l->tops.len; i++) l->tops.data[i] += l->lead;
		}
	}
}

// configure sets the scroll position and range.
static void configure(list_t *l, float y) {
	double vh = view_h(l);
	l->settling++;
	gtk_adjustment_configure(l->vadj, y, 0, fmax(l->total, vh), vh * 0.1, vh * 0.9, vh);
	l->settling--;
}

// set_total makes the rows' view as tall as the rows.
static void set_total(list_t *l, float total) {
	float f[4] = {0, 0, l->width, total};
	if (l->doc->placed && !memcmp(l->doc->frame, f, sizeof f)) return;
	memcpy(l->doc->frame, f, sizeof f);
	l->doc->placed = true;
	l->total = total;
	gtk_widget_set_size_request(l->doc->widget, -1, (int)ceilf(total));
	configure(l, fminf(offset(l), fmaxf(total - view_h(l), 0)));
}

static void on_anim_done(AdwAnimation *a, gpointer data) {
	list_t *l = data;
	if (l->anim != a) return;
	l->anim = NULL;
	l->settling--;
	if (l->bottom && !l->dead) l->stuck = at_end(l);
	g_object_unref(a);
}

// set_offset scrolls the list itself (not the user): a list anchored at
// its bottom stays stuck to its end, or not, as it was, until it's done.
static void set_offset(list_t *l, float y, bool animated) {
	y = fminf(fmaxf(y, 0), end_offset(l));
	if (l->anim) adw_animation_skip(l->anim);  // done: where it was going
	float from = offset(l);
	if (from == y) return;
	if (!animated || !adw_timed_animation_new || !gtk_widget_get_mapped(l->n->widget)) {
		configure(l, y);
		return;
	}
	AdwAnimationTarget *target = adw_property_animation_target_new((GObject *)l->vadj, "value");
	AdwAnimation *a = adw_timed_animation_new(l->n->widget, from, y, 250, target);
	adw_timed_animation_set_easing((AdwTimedAnimation *)a, ADW_EASE_OUT_CUBIC);
	l->anim = a;
	l->settling++;
	g_signal_connect(a, "done", G_CALLBACK(on_anim_done), l);
	adw_animation_play(a);
}

static int search_rows(list_t *l, float y, bool by_bottom) {
	// The first row whose bottom (by_bottom) or top is past y.
	int lo = 0, hi = (int)l->rows.len;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		float v = by_bottom ? l->tops.data[mid + 1] : l->tops.data[mid];
		if (by_bottom ? v > y : v >= y)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

// anchor_row is the row at the top of what the list shows, by its key,
// and how far its top is below the top of the view.
static bool anchor_row(list_t *l, int64_t *key, float *dy) {
	if (!l->rows.len || l->tops.len != l->rows.len + 1) return false;
	float y = offset(l);
	int r = search_rows(l, y, true);
	if (r >= (int)l->rows.len) return false;
	*key = row_key(l, r);
	*dy = l->tops.data[r] - y;
	return true;
}

static void restore(list_t *l, int64_t key, float dy) {
	for (uint32_t r = 0; r < l->rows.len; r++)
		if (row_key(l, (int)r) == key) {
			float y = l->tops.data[r] - dy;
			if (y != offset(l)) configure(l, fminf(fmaxf(y, 0), end_offset(l)));
			return;
		}
}

// pinned_row is the header row that stays at the top of a plain list:
// its section's, once its own place has scrolled above y (-1: none).
static int pinned_row(list_t *l, float y) {
	if (!sticky(l) || !l->rows.len) return -1;
	int r = search_rows(l, y, true);
	if (r >= (int)l->rows.len) return -1;
	int s = l->rows.data[r].section;
	for (int h = r; h >= 0; h--)
		if (l->rows.data[h].header && l->rows.data[h].section == s) return l->tops.data[h] >= y ? -1 : h;
	return -1;
}

static row_view *shown_at(list_t *l, int r) {
	for (uint32_t i = 0; i < l->shown.len; i++)
		if (l->shown.data[i]->row == r) return l->shown.data[i];
	return NULL;
}

// ---- Cells ----

static void select_row(cell *c) {
	c->selected = !c->selected;
	lx_queue_draw(c->n);
	lx_emit(c->n, (ak_event){.kind = AK_EVENT_ROW_SELECTED, .b = c->selected});
}

// begins is whether a press on c with state held begins selecting
// there: Ctrl on a row of a list that isn't selecting, as a GNOME list's
// Ctrl-click selects many, when the row listens (RowSelectBegin).
static bool begins(cell *c, GdkModifierType state) {
	return !c->l->selecting && !c->header && (state & GDK_CONTROL_MASK) && listening(c->n, AK_EVENT_ROW_SELECT_BEGIN);
}

static void on_cell_pressed(GtkGestureClick *g, int count, double x, double y, gpointer data) {
	(void)count;
	(void)x;
	(void)y;
	cell *c = data;
	// A row that moves takes the focus, as a GTK list's row does (for
	// Alt+Up and Alt+Down), unless it's in a view of the row's already.
	if (c->l->reorder && !c->header && gtk_widget_get_focusable(c->n->widget)) {
		GtkRoot *root = gtk_widget_get_root(c->n->widget);
		GtkWidget *f = root ? gtk_window_get_focus((GtkWindow *)root) : NULL;
		if (!f || (f != c->n->widget && !gtk_widget_is_ancestor(f, c->n->widget))) gtk_widget_grab_focus(c->n->widget);
	}
	if ((c->l->selecting && !c->header) || begins(c, gtk_event_controller_get_current_event_state((GtkEventController *)g)))
		gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void on_cell_released(GtkGestureClick *g, int count, double x, double y, gpointer data) {
	(void)count;
	(void)x;
	(void)y;
	cell *c = data;
	if (c->l->selecting && !c->header) {
		lx_enter_begin();
		select_row(c);
		lx_enter_end();
	} else if (begins(c, gtk_event_controller_get_current_event_state((GtkEventController *)g))) {
		lx_enter_begin();
		lx_emit(c->n, (ak_event){.kind = AK_EVENT_ROW_SELECT_BEGIN});
		lx_enter_end();
	}
}

static void wire_moves(cell *c);
static void update_moves(cell *c);
static void drag_layout(list_t *l);
static void unshift(list_t *l, row_view *rv);

static cell *new_cell(list_t *l, row_view *rv) {
	node *n = host_node(rv->n);
	cell *c = calloc(1, sizeof *c);
	if (!c) abort();
	c->tag = PART_CELL;
	c->n = n;
	c->l = l;
	c->rv = rv;
	n->ext = c;
	n->ext_free = part_marker;
	gtk_widget_set_parent(n->widget, rv->n->widget);
	// A selecting list's click selects the row: taken before the row's own
	// views see it.
	GtkGesture *g = gtk_gesture_click_new();
	gtk_gesture_single_set_button((GtkGestureSingle *)g, 1);
	gtk_event_controller_set_propagation_phase((GtkEventController *)g, GTK_PHASE_CAPTURE);
	g_signal_connect(g, "pressed", G_CALLBACK(on_cell_pressed), c);
	g_signal_connect(g, "released", G_CALLBACK(on_cell_released), c);
	gtk_widget_add_controller(n->widget, (GtkEventController *)g);
	if (l->reorder) wire_moves(c);
	return c;
}

static row_view *take(list_t *l) {
	if (l->free_.len) {
		row_view *rv = l->free_.data[--l->free_.len];
		gtk_widget_set_visible(rv->n->widget, 1);
		rv->snap = true;
		return rv;
	}
	row_view *rv = calloc(1, sizeof *rv);
	if (!rv) abort();
	rv->tag = PART_ROW;
	rv->l = l;
	rv->row = -1;
	rv->snap = true;
	rv->n = host_node(l->doc);
	rv->n->ext = rv;
	rv->n->ext_free = part_marker;
	gtk_widget_insert_before(rv->n->widget, l->doc->widget, NULL);  // last: over the cards
	return rv;
}

// bind binds row r's items (or its section's header) into the row view's
// cells.
static void bind(list_t *l, row_view *rv, int r) {
	lrow *row = &l->rows.data[r];
	int k = row->header ? 1 : row->count;
	while ((int)rv->cells.len < k) ak_vec_push(&rv->cells, new_cell(l, rv));
	for (uint32_t i = 0; i < rv->cells.len; i++) gtk_widget_set_visible(rv->cells.data[i]->n->widget, (int)i < k);
	for (int i = 0; i < k; i++) {
		cell *c = rv->cells.data[i];
		c->header = row->header;
		int64_t index = row->first + i;
		if (row->header) {
			c->item = l->sections.data[row->section].id;
			index = -1 - row->section;
		} else {
			c->item = l->ids.data[index];
		}
		float *v = hget(l, c->item);
		if (v) c->height = *v;
		lx_enter_begin();
		asuka_list_bind(l->n->id, index, c->n->id);
		lx_enter_end();
		lx_queue_draw(c->n);
		update_moves(c);
	}
	lx_queue_draw(rv->n);
}

// set_host_frame sets a host node's frame, which its parent's view places
// it at; it reports whether it changed.
static bool set_host_frame(node *n, float x, float y, float w, float h) {
	float f[4] = {x, y, w, h};
	if (n->placed && !memcmp(n->frame, f, sizeof f)) return false;
	bool resized = n->frame[2] != w || n->frame[3] != h;
	memcpy(n->frame, f, sizeof f);
	n->placed = true;
	if (resized) lx_queue_draw(n);
	return true;
}

static void row_x(list_t *l, float *x, float *w) {
	if (grouped(l)) {
		*x = l->inset;
		*w = fmaxf(l->width - 2 * l->inset, 0);
		return;
	}
	*x = 0;
	*w = l->width;
}

// place puts a row view at its row's top (a pinned header at the top of
// what shows, pushed up by the next header) and lays out its cells.
static bool place(list_t *l, row_view *rv, int r) {
	float height = row_height(l, r), x, w;
	row_x(l, &x, &w);
	float y = l->tops.data[r];
	lrow *row = &l->rows.data[r];
	bool pinned = false;
	if (row->header && sticky(l)) {
		float off = offset(l);
		if (y < off) {
			y = off;
			for (uint32_t k = (uint32_t)r + 1; k < l->rows.len; k++)
				if (l->rows.data[k].header) {
					y = fminf(y, l->tops.data[k] - height);
					break;
				}
			pinned = true;
		}
	}
	if (pinned && gtk_widget_get_last_child(l->doc->widget) != rv->n->widget) gtk_widget_insert_before(rv->n->widget, l->doc->widget, NULL);  // over the rows under it
	if (pinned != rv->pinned) {
		rv->pinned = pinned;
		lx_queue_draw(rv->n);
	}
	bool moved = set_host_frame(rv->n, x, y, w, height);
	bool cells = false;
	if (l->columns <= 1 || row->header) {
		for (uint32_t i = 0; i < rv->cells.len; i++) cells |= set_host_frame(rv->cells.data[i]->n, 0, 0, w, rv->cells.data[i]->height);
	} else {
		float cw = (l->width - l->gap * (float)(l->columns - 1)) / (float)l->columns;
		for (uint32_t i = 0; i < rv->cells.len; i++) {
			int col = l->n->rtl ? l->columns - 1 - (int)i : (int)i;
			cells |= set_host_frame(rv->cells.data[i]->n, (float)col * (cw + l->gap), 0, cw, rv->cells.data[i]->height);
		}
	}
	if (cells) gtk_widget_queue_allocate(rv->n->widget);
	return moved;
}

// place_cards puts an inset grouped list's cards behind its rows, a
// section's each, from its first item row to its last.
static bool place_cards(list_t *l) {
	uint32_t boxes = grouped(l) && l->ids.len ? (l->sections.len ? l->sections.len : 1) : 0;
	bool moved = false;
	while (l->cards.len < boxes) {
		node *c = host_node(l->doc);
		list_card *lc = calloc(1, sizeof *lc);
		if (!lc) abort();
		lc->tag = PART_CARD;
		lc->l = l;
		c->ext = lc;
		c->ext_free = part_marker;
		gtk_widget_insert_after(c->widget, l->doc->widget, NULL);  // first: under the rows
		ak_vec_push(&l->cards, c);
	}
	for (uint32_t i = 0; i < l->cards.len; i++) {
		node *c = l->cards.data[i];
		if (i >= boxes) {
			gtk_widget_set_visible(c->widget, 0);
			continue;
		}
		int first = -1, last = -1;
		if (!l->sections.len) {
			first = 0;
			last = (int)l->rows.len - 1;
		} else {
			for (uint32_t r = 0; r < l->rows.len; r++) {
				if (l->rows.data[r].section == (int)i && !l->rows.data[r].header) {
					if (first < 0) first = (int)r;
					last = (int)r;
				} else if (first >= 0) {
					break;
				}
			}
		}
		if (first < 0 || last < first) {
			gtk_widget_set_visible(c->widget, 0);
			continue;
		}
		gtk_widget_set_visible(c->widget, 1);
		float x, w;
		row_x(l, &x, &w);
		if (set_host_frame(c, x, l->tops.data[first], w, l->tops.data[last] + row_height(l, last) - gap_below(l, last) - l->tops.data[first])) moved = true;
		lx_queue_draw(c);  // what of it shows follows the scroll
	}
	return moved;
}

// tile_keeping shows the rows in view (and half a screen around them),
// binds the ones new to the view, and places them, keeping the row with
// key dy below the top (if ok); hold_end leaves a list at its end where
// it is, for a scroll to its new end to animate.
static void tile_keeping(list_t *l, int64_t key, float dy, bool ok, bool hold_end) {
	if (l->tiling || !H.started || l->dead || !l->n->placed || l->width <= 0) return;
	l->tiling = true;
	for (int pass = 0; pass < 4; pass++) {
		measure_tops(l);
		set_total(l, l->tops.data[l->tops.len - 1]);
		if (l->bottom && l->stuck) {
			if (!l->settling && !hold_end) configure(l, end_offset(l));
		} else if (ok) {
			restore(l, key, dy);
		}
		float y = offset(l), vh = view_h(l), top = y - vh / 2, bottom = y + vh * 1.5f;
		int first = search_rows(l, top, true), last = search_rows(l, bottom, false);
		int pin = pinned_row(l, y);
		// Rows gone from view go back to the pool.
		for (uint32_t i = 0; i < l->shown.len;) {
			row_view *rv = l->shown.data[i];
			if ((rv->row < first || rv->row >= last) && rv->row != pin && rv != l->drag.rv) {  // the lifted row stays
				l->shown.data[i] = l->shown.data[--l->shown.len];
				unshift(l, rv);
				gtk_widget_set_visible(rv->n->widget, 0);
				rv->row = -1;
				ak_vec_push(&l->free_, rv);
				continue;
			}
			i++;
		}
		bool changed = false;
		for (int r = first; r <= last; r++) {
			int at = r;
			if (r == last) {
				if (pin < 0 || (pin >= first && pin < last)) break;
				at = pin;
			}
			row_view *rv = shown_at(l, at);
			bool fresh = !rv;
			if (fresh) {
				rv = take(l);
				rv->row = at;
				ak_vec_push(&l->shown, rv);
			}
			if (fresh || l->rebind) {
				float before = row_height(l, at);
				bind(l, rv, at);
				if (row_height(l, at) != before) changed = true;
			}
		}
		l->rebind = false;
		if (!changed) break;
	}
	bool moved = false;
	for (uint32_t i = 0; i < l->shown.len; i++) moved |= place(l, l->shown.data[i], l->shown.data[i]->row);
	moved |= place_cards(l);
	if (moved) gtk_widget_queue_allocate(l->doc->widget);
	l->tiling = false;
	if (l->drag.lifted) drag_layout(l);
}

static void tile(list_t *l) {
	int64_t key = 0;
	float dy = 0;
	bool ok = anchor_row(l, &key, &dy);
	tile_keeping(l, key, dy, ok, false);
}

// make_rows lays the items and the sections' headers out in rows.
static void make_rows(list_t *l) {
	l->rows.len = 0;
#define ADD(sec, from, to)                                                        \
	for (int i_ = (from); i_ < (to); i_ += l->columns) {                          \
		lrow r_ = {false, (sec), i_, l->columns < (to) - i_ ? l->columns : (to) - i_}; \
		ak_vec_push(&l->rows, r_);                                                \
	}
	int n = (int)l->ids.len;
	if (!l->sections.len) {
		ADD(0, 0, n);
		return;
	}
	for (uint32_t s = 0; s < l->sections.len; s++) {
		int end = s + 1 < l->sections.len ? (int)l->sections.data[s + 1].start : n;
		lrow h = {true, (int)s, 0, 0};
		ak_vec_push(&l->rows, h);
		int start = (int)l->sections.data[s].start < n ? (int)l->sections.data[s].start : n;
		int stop = end > start ? end : start;
		if (stop > n) stop = n;
		ADD((int)s, start, stop);
	}
#undef ADD
}

static bool take_update(list_t *l);

// flush applies an update waiting, if any, and tiles. Rows added or
// removed above the ones in view keep those where they are, unless the
// list is at the edge it's anchored to.
static void flush(list_t *l) {
	if (l->n->frame[2] != l->width) {
		l->width = l->n->frame[2];
		l->rebind = true;  // the core lays the rows out at the new width
	}
	if (!l->updated) {
		tile(l);
		return;
	}
	l->updated = false;
	if (take_update(l)) {
		// The order the host has, or its move undone, eased: nothing bound.
		tile(l);
		return;
	}
	l->rebind = true;
	int64_t key = 0;
	float dy = 0;
	bool ok = anchor_row(l, &key, &dy);
	if ((l->bottom && l->stuck) || (!l->bottom && offset(l) <= 0.5f)) ok = false;
	bool appended = l->applied && l->pending.len > l->ids.len && (!l->ids.len || l->pending.data[l->pending.len - 1] != l->ids.data[l->ids.len - 1]);
	ak_vec_free(&l->ids);
	l->ids = l->pending;
	memset(&l->pending, 0, sizeof l->pending);
	ak_vec_free(&l->sections);
	l->sections = l->pending_s;
	memset(&l->pending_s, 0, sizeof l->pending_s);
	// The heights of the items that are still there.
	hslot *old = l->heights;
	uint32_t ocap = l->hcap;
	l->heights = NULL;
	l->hcap = l->hlen = 0;
	list_t tmp = {.heights = old, .hcap = ocap};
	for (uint32_t i = 0; i < l->ids.len; i++) {
		float *v = hget(&tmp, l->ids.data[i]);
		if (v) hput(l, l->ids.data[i], *v);
	}
	for (uint32_t i = 0; i < l->sections.len; i++) {
		float *v = hget(&tmp, l->sections.data[i].id);
		if (v) hput(l, l->sections.data[i].id, *v);
	}
	free(old);
	make_rows(l);
	// New rows at the end of a chat at its end scroll into view.
	bool animate = appended && l->bottom && l->stuck && gtk_widget_get_mapped(l->n->widget);
	tile_keeping(l, key, dy, ok, animate);
	l->applied = l->applied || l->ids.len > 0;
	if (animate) set_offset(l, end_offset(l), true);
}

static void flush_later(void *ctx) {
	node *n = node_get((ak_id)(uintptr_t)ctx);
	list_t *l = n ? list_of(n) : NULL;
	if (!l) return;
	l->queued = false;
	if (!l->dead) flush(l);
}

// queue tiles once the core has returned.
static void queue(list_t *l) {
	if (l->queued) return;
	l->queued = true;
	lx_after_core(flush_later, (void *)(uintptr_t)l->n->id);
}

static void on_scrolled(GtkAdjustment *a, gpointer data) {
	(void)a;
	list_t *l = data;
	if (l->dead) return;
	double y = gtk_adjustment_get_value(l->vadj);
	if (y != l->reported) {
		l->reported = y;
		lx_emit(l->n, (ak_event){.kind = AK_EVENT_SCROLLED, .y = y});
	}
	if (l->tiling) return;
	if (l->bottom && !l->settling) l->stuck = at_end(l);
	if (H.depth > 0) {
		queue(l);
		return;
	}
	tile(l);
}

static void free_list(node *n);

static bool list_create(node *n) {
	if (n->kind != AK_KIND_LIST) return false;
	list_t *l = calloc(1, sizeof *l);
	if (!l) abort();
	l->n = n;
	l->columns = 1;
	l->est = 44;
	l->stuck = true;
	l->reported = -1;
	n->list = (list_state *)l;
	l->sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy((GtkScrolledWindow *)l->sw, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	l->vp = gtk_viewport_new(NULL, NULL);
	gtk_viewport_set_scroll_to_focus((GtkViewport *)l->vp, 0);  // rows come and go as it scrolls
	gtk_scrolled_window_set_child((GtkScrolledWindow *)l->sw, l->vp);
	lx_set_inner(n, l->sw);
	l->vadj = gtk_scrolled_window_get_vadjustment((GtkScrolledWindow *)l->sw);
	g_signal_connect(l->vadj, "value-changed", G_CALLBACK(on_scrolled), l);
	l->doc = host_node(n);
	gtk_viewport_set_child((GtkViewport *)l->vp, l->doc->widget);
	return true;
}

// ---- Refreshing ----

static void set_refreshing(list_t *l, bool on) {
	if (l->refreshing == on) return;
	l->refreshing = on;
	if (on && !l->spinner) {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_add_css_class(box, "card");
		GtkWidget *sp;
		if (adw_spinner_new) {
			sp = adw_spinner_new();
		} else {
			sp = gtk_spinner_new();
			gtk_spinner_set_spinning((GtkSpinner *)sp, 1);
		}
		gtk_widget_set_size_request(sp, 24, 24);
		gtk_widget_set_margin_top(sp, 8);
		gtk_widget_set_margin_bottom(sp, 8);
		gtk_widget_set_margin_start(sp, 8);
		gtk_widget_set_margin_end(sp, 8);
		gtk_box_append((GtkBox *)box, sp);
		gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
		gtk_widget_set_valign(box, GTK_ALIGN_START);
		gtk_widget_set_margin_top(box, 12);
		gtk_widget_set_can_target(box, 0);
		gtk_widget_set_parent(box, l->n->widget);  // after the scrolled window: over it
		l->spinner = box;
	}
	if (l->spinner) {
		gtk_widget_set_visible(l->spinner, on);
		if (!adw_spinner_new) gtk_spinner_set_spinning((GtkSpinner *)gtk_widget_get_first_child(l->spinner), on);
	}
}

static gboolean on_refresh_key(GtkWidget *w, GVariant *args, gpointer data) {
	(void)w;
	(void)args;
	node *n = node_get((ak_id)(uintptr_t)data);
	return n && list_of(n) && !list_of(n)->dead && lx_refresh_from(n);
}

static void set_refreshable(list_t *l, bool on) {
	l->refreshable = on;
	if (!on) {
		set_refreshing(l, false);
		return;
	}
	if (l->shortcut) return;
	GtkEventController *sc = gtk_shortcut_controller_new();
	gtk_shortcut_controller_set_scope((GtkShortcutController *)sc, GTK_SHORTCUT_SCOPE_GLOBAL);
	static const char *const keys[] = {"F5", "<Control>r"};
	for (size_t i = 0; i < AK_LEN(keys); i++)
		gtk_shortcut_controller_add_shortcut((GtkShortcutController *)sc,
			gtk_shortcut_new(gtk_shortcut_trigger_parse_string(keys[i]), gtk_callback_action_new(on_refresh_key, (gpointer)(uintptr_t)l->n->id, NULL)));
	gtk_widget_add_controller(l->n->widget, sc);
	l->shortcut = sc;
}

bool lx_list_refreshable(node *n) {
	list_t *l = list_of(n);
	return l && l->refreshable;
}

// lx_list_refresh starts a refresh, as the user asked for one.
bool lx_list_refresh(node *n) {
	list_t *l = list_of(n);
	if (!l || l->dead || !l->refreshable || l->refreshing || !listening(n, AK_EVENT_REFRESH) || !gtk_widget_get_mapped(n->widget)) return false;
	set_refreshing(l, true);  // the core's handler says when it's done
	lx_emit(n, (ak_event){.kind = AK_EVENT_REFRESH});
	return true;
}

// ---- Props, commands, frames ----

static void set_selecting(list_t *l, bool on) {
	if (on == l->selecting) return;
	l->selecting = on;
	for (uint32_t i = 0; i < l->shown.len; i++)
		for (uint32_t k = 0; k < l->shown.data[i]->cells.len; k++) lx_queue_draw(l->shown.data[i]->cells.data[k]->n);
}

static void set_reorder(list_t *l, ak_span b);

static bool list_prop(node *n, ak_prop p, const ak_value *v) {
	list_t *l = list_of(n);
	if (!l) return false;
	switch (p) {
	case AK_PROP_LIST_REFRESHABLE:
		set_refreshable(l, v->i != 0);
		return true;
	case AK_PROP_LIST_REFRESHING:
		set_refreshing(l, v->i != 0);
		return true;
	case AK_PROP_LIST_COLUMNS:
		l->columns = v->i > 1 ? (int)v->i : 1;
		break;
	case AK_PROP_LIST_GAP:
		l->gap = (float)v->f;
		break;
	case AK_PROP_LIST_APPEARANCE:
		l->appearance = (int32_t)v->i;
		lx_queue_draw(n);
		break;
	case AK_PROP_LIST_INSET:
		l->inset = (float)v->f;
		break;
	case AK_PROP_LIST_ANCHOR:
		l->bottom = v->i == AK_SCROLL_ANCHOR_BOTTOM;
		l->stuck = true;
		break;
	case AK_PROP_LIST_SELECTING:
		set_selecting(l, v->i != 0);
		return true;
	case AK_PROP_LIST_REORDER:
		set_reorder(l, v->s);
		return true;
	default:
		return false;
	}
	l->rebind = true;
	queue(l);
	return true;
}

static void cancel_drag(list_t *l);

// update takes the core's new item IDs and sections: u32 count, i64 each,
// then u32 sections, u32 start and i64 ID each. Rows bind again once the
// core has returned (flush), unless the host's moves explain it.
static void update(list_t *l, ak_span args) {
	if (l->drag.armed) cancel_drag(l);  // its rows change
	ak_reader r = {(const uint8_t *)args.p, args.len, 0, false};
	uint32_t count = ak_read_u32(&r);
	if (r.err) return;
	l->pending.len = 0;
	for (uint32_t i = 0; i < count && r.off + 8 <= r.len; i++) ak_vec_push(&l->pending, (int64_t)ak_read_u64(&r));
	l->pending_s.len = 0;
	if (r.off + 4 <= r.len) {
		uint32_t ns = ak_read_u32(&r);
		for (uint32_t i = 0; i < ns && r.off + 12 <= r.len; i++) {
			lsection s;
			s.start = ak_read_u32(&r);
			s.id = (int64_t)ak_read_u64(&r);
			ak_vec_push(&l->pending_s, s);
		}
	}
	l->updated = true;
	queue(l);
}

typedef struct {
	ak_id id;
	int index;
	bool animated;
	double y;
} scroll_req;

// scroll_to scrolls the least that shows an item (below a header pinned
// over it).
static void scroll_to(list_t *l, int index, bool animated) {
	flush(l);
	if (index < 0 || (uint32_t)index >= l->ids.len || l->tops.len != l->rows.len + 1) return;
	int row = -1;
	for (uint32_t r = 0; r < l->rows.len; r++) {
		lrow *x = &l->rows.data[r];
		if (!x->header && index >= x->first && index < x->first + x->count) {
			row = (int)r;
			break;
		}
	}
	if (row < 0) return;
	float top = l->tops.data[row], bottom = l->tops.data[row + 1];
	if (sticky(l))
		for (int r = row; r >= 0; r--)
			if (l->rows.data[r].header) {
				top -= row_height(l, r);
				break;
			}
	float y = offset(l), vh = view_h(l);
	if (top < y)
		y = top;
	else if (bottom > y + vh)
		y = bottom - vh;
	if (l->bottom) l->stuck = (uint32_t)index == l->ids.len - 1;
	set_offset(l, y, animated);
}

static void scroll_index_later(void *ctx) {
	scroll_req *s = ctx;
	node *n = node_get(s->id);
	list_t *l = n ? list_of(n) : NULL;
	if (l && !l->dead) scroll_to(l, s->index, s->animated);
	free(s);
}

static void scroll_offset_later(void *ctx) {
	scroll_req *s = ctx;
	node *n = node_get(s->id);
	list_t *l = n ? list_of(n) : NULL;
	if (l && !l->dead) {
		flush(l);
		float end = end_offset(l);
		float to = fminf(fmaxf((float)s->y, 0), end);
		if (l->bottom) l->stuck = to >= end - 1;
		set_offset(l, to, s->animated);
	}
	free(s);
}

static bool list_command(node *n, ak_cmd c, ak_span args) {
	list_t *l = list_of(n);
	if (!l) return false;
	switch (c) {
	case AK_CMD_LIST_UPDATE:
		update(l, args);
		return true;
	case AK_CMD_SCROLL_TO: {
		ak_reader r = {(const uint8_t *)args.p, args.len, 0, false};
		scroll_req *s = calloc(1, sizeof *s);
		if (!s) abort();
		s->id = n->id;
		s->index = (int)ak_read_u32(&r);
		s->animated = ak_read_u8(&r) != 0;
		// Once the core has returned: binding rows calls into it.
		lx_after_core(scroll_index_later, s);
		return true;
	}
	}
	return false;
}

static bool list_scroll_to(node *n, double x, double y, bool animated) {
	(void)x;
	list_t *l = list_of(n);
	if (!l) return false;
	scroll_req *s = calloc(1, sizeof *s);
	if (!s) abort();
	s->id = n->id;
	s->y = y;
	s->animated = animated;
	lx_after_core(scroll_offset_later, s);
	return true;
}

// list_frame takes a cell's frame as its row's height; a list's new size
// tiles it again.
static bool list_frame(node *n, const float f[4]) {
	if (part_tag(n) == PART_CELL) {
		cell *c = n->ext;
		list_t *l = c->l;
		c->height = f[3];
		float *old = hget(l, c->item);
		if (old && *old == f[3]) return true;
		if (!old && l->hlen < 64 && !c->header) l->est = (l->est * (float)l->hlen + f[3]) / (float)(l->hlen + 1);  // the estimate follows the first rows'
		hput(l, c->item, f[3]);
		if (!l->tiling) queue(l);
		return true;
	}
	list_t *l = list_of(n);
	if (l && (f[2] != n->frame[2] || f[3] != n->frame[3] || !n->placed)) {
		l->rebind = l->rebind || f[2] != l->width;
		queue(l);
	}
	return false;
}

// ---- Rows' menus ----

typedef struct {
	ak_id id;
	int64_t code;
} row_act;

static void on_row_action(GSimpleAction *a, GVariant *p, gpointer data) {
	(void)a;
	(void)p;
	row_act *r = data;
	node *n = node_get(r->id);
	if (n) lx_emit(n, (ak_event){.kind = AK_EVENT_ACTION, .i = r->code});
}

// row_menu shows a row's swipe actions, leading then trailing, as its
// context menu at (x, y) in its cell; a leading one's Action is -1 - its
// index. False: it has none.
static bool row_menu(cell *c, double x, double y) {
	if (c->header || c->l->selecting || !(c->leading.len + c->swipe.len) || !c->n->widget) return false;
	action_vec all = {0};
	for (uint32_t i = 0; i < c->leading.len; i++) {
		action a = {.role = c->leading.data[i].role, .title = c->leading.data[i].title, .icon = c->leading.data[i].icon, .index = (int64_t)all.len};
		ak_vec_push(&all, a);
	}
	for (uint32_t i = 0; i < c->swipe.len; i++) {
		action a = {.role = c->swipe.data[i].role, .title = c->swipe.data[i].title, .icon = c->swipe.data[i].icon, .index = (int64_t)all.len};
		ak_vec_push(&all, a);
	}
	int nl = (int)c->leading.len;
	GSimpleActionGroup *group = g_simple_action_group_new();
	for (uint32_t i = 0; i < all.len; i++) {
		char name[32];
		snprintf(name, sizeof name, "item%u", i);
		GSimpleAction *a = g_simple_action_new(name, NULL);
		row_act *r = malloc(sizeof *r);
		if (!r) abort();
		r->id = c->n->id;
		r->code = (int)i < nl ? -1 - (int64_t)i : (int64_t)i - nl;
		g_signal_connect_data(a, "activate", G_CALLBACK(on_row_action), r, (GClosureNotify)(void (*)(void))free, (GConnectFlags)0);
		g_action_map_add_action((GActionMap *)group, (GAction *)a);
		g_object_unref(a);
	}
	gtk_widget_insert_action_group(c->n->widget, "asukarow", (GActionGroup *)group);
	g_object_unref(group);
	GMenuModel *model = lx_menu_model(&all, "asukarow.item");
	ak_vec_free(&all);  // the titles are the cell's
	lx_popup_at(c->n, model, x, y);
	g_object_unref(model);
	return true;
}

static void on_row_secondary(GtkGestureClick *g, int count, double x, double y, gpointer data) {
	(void)count;
	if (row_menu(data, x, y)) gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void on_row_long(GtkGestureLongPress *g, double x, double y, gpointer data) {
	cell *c = data;
	if (c->l->reorder && !c->header) return;  // it lifts the row; let go where it was, the menu shows
	if (row_menu(c, x, y)) gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
}

// listen_row_menu gives a cell its context menu: a secondary click, or a
// long press on a touchscreen.
static void listen_row_menu(cell *c) {
	if (c->menu) return;
	c->menu = true;
	GtkGesture *r = gtk_gesture_click_new();
	gtk_gesture_single_set_button((GtkGestureSingle *)r, 3);
	g_signal_connect(r, "pressed", G_CALLBACK(on_row_secondary), c);
	gtk_widget_add_controller(c->n->widget, (GtkEventController *)r);
	GtkGesture *g = gtk_gesture_long_press_new();
	gtk_gesture_single_set_touch_only((GtkGestureSingle *)g, 1);
	g_signal_connect(g, "pressed", G_CALLBACK(on_row_long), c);
	gtk_widget_add_controller(c->n->widget, (GtkEventController *)g);
}

// cell_prop takes a cell's props: its swipe actions and whether it's
// selected.
static bool cell_prop(node *n, ak_prop p, const ak_value *v) {
	if (part_tag(n) != PART_CELL) return false;
	cell *c = n->ext;
	switch (p) {
	case AK_PROP_SWIPE_ACTIONS:
		actions_decode(&c->swipe, v->s, 0);
		break;
	case AK_PROP_LEADING_SWIPE_ACTIONS:
		actions_decode(&c->leading, v->s, 0);
		break;
	case AK_PROP_SELECTED:
		c->selected = v->i != 0;
		lx_queue_draw(n);
		break;
	default:
		return false;
	}
	if (c->swipe.len + c->leading.len) listen_row_menu(c);
	return true;
}

static bool cell_listen(node *n, ak_event_kind e) {
	if (part_tag(n) != PART_CELL) return false;
	if (e == AK_EVENT_ACTION) {
		listen_row_menu(n->ext);
		return true;
	}
	return e == AK_EVENT_ROW_SELECTED || e == AK_EVENT_ROW_SELECT_BEGIN;  // the cell's selection click, and Ctrl's
}

// ---- Moving rows (Reorder) ----
//
// A drag of a row with the mouse (a touchpad's), past the desktop's drag
// threshold, lifts it (a long press on a touchscreen): it follows the
// pointer up and down on a card, raised over the others, which make way,
// easing to where they'd be with it dropped there; near the list's top or
// bottom edge the list scrolls. Dropping it moves the item in the host's
// IDs and its row view with its cells (nothing is bound again: the core's
// ListUpdate in that order changes nothing), the rows easing to their new
// places, and sends Moved; Escape puts it back. A focused row moves with
// Alt+Up and Alt+Down, and a screen reader has the two as its actions (its
// "action" group, as a11y.c gives a view's), each a place.

#define ESCAPE_KEY 0xff1b
#define KEY_UP_ 0xff52
#define KEY_DOWN_ 0xff54
#define KEY_KP_UP_ 0xff97
#define KEY_KP_DOWN_ 0xff99

static int item_count(list_t *l) { return (int)l->ids.len; }

static float row_top(list_t *l, int r) { return l->tops.data[r]; }

static float row_h(list_t *l, int r) { return l->tops.data[r + 1] - l->tops.data[r]; }

// cell_index is a cell's item's index (-1: a header's).
static int cell_index(cell *c) {
	row_view *rv = c->rv;
	list_t *l = c->l;
	if (c->header || !rv || rv->row < 0 || (uint32_t)rv->row >= l->rows.len) return -1;
	for (uint32_t i = 0; i < rv->cells.len; i++)
		if (rv->cells.data[i] == c) return l->rows.data[rv->row].first + (int)i;
	return -1;
}

// update_moves enables a cell's move actions where they apply: no Move up
// for the first item, no Move down for the last, neither for a header.
static void update_moves(cell *c) {
	if (!c->moves[0]) return;
	int i = cell_index(c);
	g_simple_action_set_enabled(c->moves[0], i > 0);
	g_simple_action_set_enabled(c->moves[1], i >= 0 && i + 1 < item_count(c->l));
}

static void update_all_moves(list_t *l) {
	for (uint32_t i = 0; i < l->shown.len; i++)
		for (uint32_t k = 0; k < l->shown.data[i]->cells.len; k++) update_moves(l->shown.data[i]->cells.data[k]);
}

static void move_by_one(list_t *l, int from, int to);

typedef struct {
	ak_id cell;
	int by;
} move_act;

static void on_move_action(GSimpleAction *a, GVariant *p, gpointer data) {
	(void)a;
	(void)p;
	move_act *m = data;
	node *n = node_get(m->cell);
	if (!n || part_tag(n) != PART_CELL) return;
	cell *c = n->ext;
	int i = cell_index(c);
	if (i < 0 || c->l->dead || !c->l->reorder) return;
	move_by_one(c->l, i, i + m->by);
}

// wire_moves gives a cell the move actions (and makes it a Tab stop, for
// Alt+Up and Alt+Down), or takes them away.
static void wire_moves(cell *c) {
	list_t *l = c->l;
	GtkWidget *w = c->n->widget;
	if (!l->reorder) {
		if (c->moves[0]) {
			gtk_widget_insert_action_group(w, "action", NULL);
			c->moves[0] = c->moves[1] = NULL;
			gtk_widget_set_focusable(w, 0);
			gtk_accessible_reset_property((GtkAccessible *)w, (GtkAccessibleProperty)3);  // KEY_SHORTCUTS
		}
		return;
	}
	char names[2][128];
	for (int i = 0; i < 2; i++) lx_action_name(l->move_names[i], (unsigned)i, names[i], sizeof names[i]);
	if (!strcmp(names[0], names[1])) strcat(names[1], "-2");
	GSimpleActionGroup *group = g_simple_action_group_new();
	for (int i = 0; i < 2; i++) {
		GSimpleAction *a = g_simple_action_new(names[i], NULL);
		move_act *m = malloc(sizeof *m);
		if (!m) abort();
		m->cell = c->n->id;
		m->by = i ? 1 : -1;
		g_signal_connect_data(a, "activate", G_CALLBACK(on_move_action), m, (GClosureNotify)(void (*)(void))free, (GConnectFlags)0);
		g_action_map_add_action((GActionMap *)group, (GAction *)a);
		c->moves[i] = a;  // the group holds it
		g_object_unref(a);
	}
	gtk_widget_insert_action_group(w, "action", (GActionGroup *)group);  // the cell holds it
	g_object_unref(group);
	gtk_widget_set_focusable(w, 1);
	gtk_widget_set_focus_on_click(w, 0);
	gtk_widget_add_css_class(w, "asuka-focus-ring");
	lx_set_a11y_string(w, 3, "Alt+Up Alt+Down");  // KEY_SHORTCUTS
	update_moves(c);
}

static void each_cell(list_t *l, void (*f)(cell *c)) {
	for (uint32_t i = 0; i < l->shown.len; i++)
		for (uint32_t k = 0; k < l->shown.data[i]->cells.len; k++) f(l->shown.data[i]->cells.data[k]);
	for (uint32_t i = 0; i < l->free_.len; i++)
		for (uint32_t k = 0; k < l->free_.data[i]->cells.len; k++) f(l->free_.data[i]->cells.data[k]);
}

// ---- Shifts: rows eased to where they go ----

static void set_shift(list_t *l, row_view *rv, float dy) {
	rv->shift = dy;
	transform_t t = {.set = dy != 0, .ty = dy, .sx = 1, .sy = 1};
	if (!memcmp(&t, &rv->n->transform, sizeof t)) return;
	rv->n->transform = t;
	gtk_widget_queue_allocate(l->doc->widget);
}

static gboolean shift_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data);

// ease_to eases a row from where it shows to dy below its place, over a
// fifth of a second (at once with Reduce Motion).
static void land(list_t *l, row_view *rv);

static void ease_to(list_t *l, row_view *rv, float dy) {
	if (rv->snap || H.env.reduce_motion || !gtk_widget_get_mapped(l->n->widget) || (rv->shift == dy && !rv->easing)) {
		rv->snap = rv->easing = false;
		rv->shift_from = rv->shift_to = dy;
		set_shift(l, rv, dy);
		if (dy == 0) land(l, rv);
		return;
	}
	if (rv->easing && rv->shift_to == dy) return;
	rv->shift_from = rv->shift;
	rv->shift_to = dy;
	rv->shift_t0 = lx_now();
	rv->easing = true;
	if (!l->shift_tick) l->shift_tick = gtk_widget_add_tick_callback(l->n->widget, shift_tick, l, NULL);
}

static void land(list_t *l, row_view *rv) {
	if (rv->lifted && rv != l->drag.rv) {
		rv->lifted = false;
		lx_queue_draw(rv->n);
	}
	(void)l;
}

static gboolean shift_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
	(void)w;
	(void)clock;
	list_t *l = data;
	double now = lx_now();
	bool more = false;
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		if (!rv->easing) continue;
		double t = fmin((now - rv->shift_t0) / 0.2, 1), e = 1 - pow(1 - t, 3);
		set_shift(l, rv, (float)(rv->shift_from + (rv->shift_to - rv->shift_from) * e));
		if (t >= 1) {
			rv->easing = false;
			if (rv->shift_to == 0) land(l, rv);
		} else {
			more = true;
		}
	}
	if (!more) l->shift_tick = 0;
	return more;
}

static void stop_shifts(list_t *l) {
	if (l->shift_tick) gtk_widget_remove_tick_callback(l->n->widget, l->shift_tick);
	l->shift_tick = 0;
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		rv->easing = false;
		rv->lifted = false;
		set_shift(l, rv, 0);
	}
}

// unshift takes a row view's shift off as it goes back to the pool.
static void unshift(list_t *l, row_view *rv) {
	rv->easing = rv->lifted = false;
	rv->shift_from = rv->shift_to = 0;
	set_shift(l, rv, 0);
}

// ---- Committing a move ----

// new_index is where an item at i is once the item at from is at to.
static int new_index(int i, int from, int to) {
	if (i == from) return to;
	if (from < to && i > from && i <= to) return i - 1;
	if (to < from && i >= to && i < from) return i + 1;
	return i;
}

static int section_of(list_t *l, int i) {
	int s = 0;
	for (uint32_t k = 0; k < l->sections.len; k++)
		if ((int)l->sections.data[k].start <= i) s = (int)k;
	return s;
}

// row_of_item is the row an item is in, in a list of one column.
static int row_of_item(list_t *l, int i) { return l->sections.len ? i + section_of(l, i) + 1 : i; }

typedef struct {
	row_view *rv;
	float y;  // where it showed: its place and shift
	bool header;
	int64_t key;
	int index;  // its item's, or its section's
} shown_row;

static GtkWidget *focus_in(list_t *l) {
	GtkRoot *root = gtk_widget_get_root(l->n->widget);
	GtkWidget *f = root ? gtk_window_get_focus((GtkWindow *)root) : NULL;
	return f && gtk_widget_is_ancestor(f, l->n->widget) ? f : NULL;
}

static void place_all(list_t *l);
static void reassign_cells(list_t *l, int from, int to);

// commit moves the item at from to to in the host's IDs (sections, if
// any, given in starts: their new starts) and the row views with their
// cells, unbound, which ease from where they showed to their new places.
static void commit(list_t *l, int from, int to, const uint32_t *starts) {
	GtkWidget *focus = focus_in(l);
	AK_VEC(shown_row) was = {0};
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		lrow *row = &l->rows.data[rv->row];
		shown_row s = {rv, rv->n->frame[1] + rv->shift, row->header, 0, row->header ? row->section : row->first};
		ak_vec_push(&was, s);
	}
	int64_t id = l->ids.data[from];
	if (from < to)
		memmove(l->ids.data + from, l->ids.data + from + 1, (size_t)(to - from) * sizeof *l->ids.data);
	else
		memmove(l->ids.data + to + 1, l->ids.data + to, (size_t)(from - to) * sizeof *l->ids.data);
	l->ids.data[to] = id;
	if (starts)
		for (uint32_t s = 0; s < l->sections.len; s++) l->sections.data[s].start = starts[s];
	make_rows(l);
	measure_tops(l);
	set_total(l, l->tops.data[l->tops.len - 1]);
	if (l->columns <= 1) {
		// Each row view goes to the row its item (or header) is in now.
		for (uint32_t i = 0; i < was.len; i++) {
			shown_row *s = &was.data[i];
			int r = s->header ? (int)l->sections.data[s->index].start + s->index : row_of_item(l, new_index(s->index, from, to));
			s->rv->row = r < (int)l->rows.len ? r : -1;
		}
		// One that's lost its row (none should) goes back to the pool.
		for (uint32_t i = 0; i < l->shown.len;) {
			row_view *rv = l->shown.data[i];
			if (rv->row < 0) {
				l->shown.data[i] = l->shown.data[--l->shown.len];
				unshift(l, rv);
				gtk_widget_set_visible(rv->n->widget, 0);
				ak_vec_push(&l->free_, rv);
				continue;
			}
			i++;
		}
	} else {
		reassign_cells(l, from, to);
	}
	l->moved = true;
	tile_keeping(l, 0, 0, false, false);
	place_all(l);
	// From where they showed, to their places.
	for (uint32_t i = 0; i < was.len; i++) {
		row_view *rv = was.data[i].rv;
		if (rv->row < 0) continue;
		bool kept = false;
		for (uint32_t k = 0; k < l->shown.len && !kept; k++) kept = l->shown.data[k] == rv;
		if (!kept) continue;
		float d = was.data[i].y - rv->n->frame[1];
		rv->snap = false;
		rv->easing = false;
		set_shift(l, rv, d);
		ease_to(l, rv, 0);
	}
	ak_vec_free(&was);
	update_all_moves(l);
	if (focus && !focus_in(l) && gtk_widget_get_mapped(focus)) gtk_widget_grab_focus(focus);
}

static void place_all(list_t *l) {
	bool moved = false;
	for (uint32_t i = 0; i < l->shown.len; i++) moved |= place(l, l->shown.data[i], l->shown.data[i]->row);
	moved |= place_cards(l);
	if (moved) gtk_widget_queue_allocate(l->doc->widget);
}

// reassign_cells puts a grid's cells (its rows hold more than one) in the
// rows their items are in now, keeping each item's cell; a slot whose item
// had none shown binds one left over.
static void reassign_cells(list_t *l, int from, int to) {
	(void)from;
	(void)to;
	AK_VEC(cell *) pool = {0};
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		for (uint32_t k = 0; k < rv->cells.len; k++)
			if (gtk_widget_get_visible(rv->cells.data[k]->n->widget)) ak_vec_push(&pool, rv->cells.data[k]);
	}
	// Each shown row's slots take the cells of their items.
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		if (rv->row < 0 || (uint32_t)rv->row >= l->rows.len) continue;
		lrow *row = &l->rows.data[rv->row];
		int k = row->header ? 1 : row->count;
		for (int slot = 0; slot < k; slot++) {
			bool header = row->header;
			int64_t key = header ? l->sections.data[row->section].id : l->ids.data[row->first + slot];
			cell *got = NULL;
			for (uint32_t p = 0; p < pool.len; p++)
				if (pool.data[p]->header == header && pool.data[p]->item == key) {
					got = pool.data[p];
					pool.data[p] = pool.data[--pool.len];
					break;
				}
			if (!got) continue;
			if (got->rv != rv) {
				// Out of its row view's cells, into this one's.
				row_view *o = got->rv;
				for (uint32_t q = 0; q < o->cells.len; q++)
					if (o->cells.data[q] == got) {
						memmove(o->cells.data + q, o->cells.data + q + 1, (o->cells.len - q - 1) * sizeof *o->cells.data);
						o->cells.len--;
						break;
					}
				gtk_widget_unparent(got->n->widget);
				gtk_widget_set_parent(got->n->widget, rv->n->widget);
				got->n->parent = rv->n;
				got->rv = rv;
				ak_vec_push(&rv->cells, got);
			}
			// At its slot.
			for (uint32_t q = 0; q < rv->cells.len; q++)
				if (rv->cells.data[q] == got && (int)q != slot && (uint32_t)slot < rv->cells.len) {
					cell *t = rv->cells.data[slot];
					rv->cells.data[slot] = got;
					rv->cells.data[q] = t;
					break;
				}
		}
	}
	ak_vec_free(&pool);
	// The slots left: a cell bound to their items.
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		if (rv->row < 0 || (uint32_t)rv->row >= l->rows.len) continue;
		lrow *row = &l->rows.data[rv->row];
		int k = row->header ? 1 : row->count;
		while ((int)rv->cells.len < k) ak_vec_push(&rv->cells, new_cell(l, rv));
		for (int slot = 0; slot < k; slot++) {
			cell *c = rv->cells.data[slot];
			int64_t key = row->header ? l->sections.data[row->section].id : l->ids.data[row->first + slot];
			gtk_widget_set_visible(c->n->widget, 1);
			if (c->header == row->header && c->item == key) continue;
			c->header = row->header;
			c->item = key;
			float *v = hget(l, key);
			if (v) c->height = *v;
			lx_enter_begin();
			asuka_list_bind(l->n->id, row->header ? -1 - row->section : row->first + slot, c->n->id);
			lx_enter_end();
		}
		for (uint32_t q = (uint32_t)k; q < rv->cells.len; q++) gtk_widget_set_visible(rv->cells.data[q]->n->widget, 0);
		gtk_widget_queue_allocate(rv->n->widget);
	}
}

// emit_moved reports a move the host made.
static void emit_moved(list_t *l, int from, int to) { lx_emit(l->n, (ak_event){.kind = AK_EVENT_MOVED, .x = from, .y = to}); }

// move_by_one moves an item a place (a key's, a screen reader's), showing
// it.
static void move_by_one(list_t *l, int from, int to) {
	if (from < 0 || to < 0 || from >= item_count(l) || to >= item_count(l) || from == to || l->drag.lifted) return;
	flush(l);
	commit(l, from, to, NULL);
	emit_moved(l, from, to);
	if (!l->dead) scroll_to(l, to, true);
}

// ---- Dragging ----

static void stop_autoscroll(list_t *l) {
	if (l->scroll_tick) gtk_widget_remove_tick_callback(l->n->widget, l->scroll_tick);
	l->scroll_tick = 0;
}

static void end_drag(list_t *l) {
	stop_autoscroll(l);
	if (l->escape) {
		gtk_widget_remove_controller(l->escape_on, l->escape);
		g_object_unref(l->escape_on);
		l->escape = NULL;
		l->escape_on = NULL;
	}
	gtk_widget_set_cursor_from_name(l->n->widget, NULL);
	row_view *rv = l->drag.rv;
	l->drag = (drag_t){0};
	if (rv && !rv->easing && rv->shift == 0) land(l, rv);
}

// drag_top is where the lifted row's top is: under the pointer, as it was
// when it was lifted, within the rows.
static float drag_top(list_t *l) {
	float y = (float)l->drag.vy - l->n->inset[0] + offset(l) - l->drag.grab;
	float lo = l->tops.data[0], hi = l->tops.data[l->rows.len] - l->drag.h;
	return fminf(fmaxf(y, lo), fmaxf(hi, lo));
}

// drag_slot is where the lifted row would go: the place among the other
// rows (0: before the first) whose middle it's above; and the item index
// that is (*to).
static int drag_slot(list_t *l, float center, int *to) {
	int r0 = l->drag.row, n = (int)l->rows.len;
	float h0 = l->drag.h;
	// The others' middles where they'd be without it, ascending; its own
	// place between theirs.
	int lo = 0, hi = n;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		float key = mid == r0 ? row_top(l, r0) : row_top(l, mid) - (mid > r0 ? h0 : 0) + row_h(l, mid) / 2;
		if (key < center)
			lo = mid + 1;
		else
			hi = mid;
	}
	int p = lo - (lo > r0 ? 1 : 0);  // among the others
	if (p < 1 && l->sections.len) p = 1;  // below the first header
	if (p >= n - 1) {
		*to = item_count(l) - 1;
		return n - 1;
	}
	int rp = p < r0 ? p : p + 1;  // the row it'd be before
	lrow *row = &l->rows.data[rp];
	int before = row->header ? (int)fminf((float)l->sections.data[row->section].start, (float)item_count(l)) : row->first;
	*to = before - (rp > r0 ? 1 : 0);
	return p;
}

// drag_layout places the lifted row under the pointer and eases the
// others to where they'd be with it dropped there.
static void drag_layout(list_t *l) {
	if (!l->drag.lifted || l->tops.len != l->rows.len + 1) return;
	int r0 = l->drag.row;
	float h0 = l->drag.h, top = drag_top(l);
	int to = 0;
	int p = drag_slot(l, top + h0 / 2, &to);
	l->drag.slot = p;
	l->drag.to = to;
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		int r = rv->row;
		if (rv == l->drag.rv) {
			rv->easing = false;
			rv->snap = false;
			set_shift(l, rv, top - row_top(l, r0));
			continue;
		}
		if (rv->pinned) {
			ease_to(l, rv, 0);
			continue;
		}
		int q = r < r0 ? r : r - 1;  // its place among the others
		float y = row_top(l, r) - (r > r0 ? h0 : 0) + (q >= p ? h0 : 0);
		ease_to(l, rv, y - row_top(l, r));
	}
}

static gboolean autoscroll_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
	(void)w;
	(void)clock;
	list_t *l = data;
	double now = lx_now(), dt = fmin(now - l->drag.tick_t, 0.05);
	l->drag.tick_t = now;
	float y = (float)l->drag.vy - l->n->inset[0], vh = view_h(l), edge = fminf(48, vh / 4);
	float speed = 0;
	if (y < edge) speed = -(edge - y) / edge;
	if (y > vh - edge) speed = (y - (vh - edge)) / edge;
	speed = fminf(fmaxf(speed, -1), 1);
	if (speed != 0) {
		float to = fminf(fmaxf(offset(l) + speed * 1200 * (float)dt, 0), end_offset(l));
		if (to != offset(l)) {
			configure(l, to);  // tiles (on_scrolled)
			drag_layout(l);
		}
	}
	return 1;
}

static gboolean on_drag_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, gpointer data);

static guint drag_threshold(void) {
	int t = 8;
	GtkSettings *s = gtk_settings_get_default();
	if (s) g_object_get(s, "gtk-dnd-drag-threshold", &t, NULL);
	return t > 0 ? (guint)t : 8;
}

// lift lifts the armed row: it's raised, on a card, and follows the
// pointer; Escape puts it back.
static void lift(list_t *l) {
	row_view *rv = shown_at(l, l->drag.row);
	if (!rv) {
		l->drag.armed = false;
		return;
	}
	l->drag.lifted = true;
	l->drag.rv = rv;
	l->drag.h = row_h(l, l->drag.row);
	l->drag.from = l->rows.data[l->drag.row].first;
	l->drag.to = l->drag.from;
	rv->lifted = true;
	gtk_widget_insert_before(rv->n->widget, l->doc->widget, NULL);  // over the others
	lx_queue_draw(rv->n);
	gtk_widget_set_cursor_from_name(l->n->widget, "grabbing");
	GtkRoot *root = gtk_widget_get_root(l->n->widget);
	if (root) {
		GtkEventController *k = gtk_event_controller_key_new();
		gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
		g_signal_connect(k, "key-pressed", G_CALLBACK(on_drag_key), l);
		gtk_widget_add_controller((GtkWidget *)root, k);
		l->escape = k;
		l->escape_on = g_object_ref((GtkWidget *)root);
	}
	l->drag.tick_t = lx_now();
	l->scroll_tick = gtk_widget_add_tick_callback(l->n->widget, autoscroll_tick, l, NULL);
	drag_layout(l);
}

// put_back eases every row back to its place (Escape, a drop where it
// was).
static void put_back(list_t *l) {
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = l->shown.data[i];
		rv->snap = false;
		ease_to(l, rv, 0);
	}
	end_drag(l);
}

static void cancel_drag(list_t *l) {
	if (!l->drag.armed) return;
	if (l->drag.gesture) gtk_gesture_set_state(l->drag.gesture, GTK_EVENT_SEQUENCE_DENIED);
	if (l->drag.lifted)
		put_back(l);
	else
		end_drag(l);
}

static gboolean on_drag_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
	(void)k;
	(void)keycode;
	(void)state;
	list_t *l = data;
	if (keyval != ESCAPE_KEY || !l->drag.lifted) return 0;
	cancel_drag(l);
	return 1;
}

// drop moves the lifted row's item where it was dropped.
static void drop(list_t *l) {
	int from = l->drag.from, to = l->drag.to, p = l->drag.slot, r0 = l->drag.row;
	if (from == to || from < 0 || to < 0 || to >= item_count(l)) {
		put_back(l);
		return;
	}
	// With sections: their starts with it at its place among the rows.
	uint32_t *starts = NULL;
	if (l->sections.len) {
		starts = calloc(l->sections.len, sizeof *starts);
		if (!starts) abort();
		int items = 0, n = (int)l->rows.len;
		for (int q = 0, k = 0; q < n; q++) {
			// The rows in their new order: the others, it at p.
			int r;
			if (q == p)
				r = r0;
			else {
				r = k < r0 ? k : k + 1;
				k++;
			}
			lrow *row = &l->rows.data[r];
			if (row->header)
				starts[row->section] = (uint32_t)items;
			else
				items++;
		}
	}
	row_view *rv = l->drag.rv;
	end_drag(l);
	if (rv) rv->lifted = true;  // till it's landed
	commit(l, from, to, starts);
	free(starts);
	emit_moved(l, from, to);
}

static void on_reorder_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	list_t *l = data;
	if (l->drag.armed || !l->reorder || l->selecting || l->columns > 1 || l->dead || l->tops.len != l->rows.len + 1) return;
	float dy = (float)y - l->n->inset[0] + offset(l);
	int r = search_rows(l, dy, true);
	if (r >= (int)l->rows.len || row_top(l, r) > dy || l->rows.data[r].header || !shown_at(l, r)) return;
	GdkDevice *dev = gtk_gesture_get_device((GtkGesture *)g);
	l->drag = (drag_t){
		.armed = true,
		.row = r,
		.grab = dy - row_top(l, r),
		.x0 = x,
		.y0 = y,
		.vy = y,
		.touch = dev && gdk_device_get_source(dev) == GDK_SOURCE_TOUCHSCREEN,
		.gesture = (GtkGesture *)g,
	};
}

static void on_reorder_update(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	list_t *l = data;
	if (!l->drag.armed || l->drag.gesture != (GtkGesture *)g) return;
	l->drag.vy = l->drag.y0 + dy;
	if (!l->drag.lifted) {
		if (hypot(dx, dy) < drag_threshold()) return;
		if (l->drag.touch) {
			// A touch that moves before it's held scrolls.
			end_drag(l);
			return;
		}
		gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
		lift(l);
		return;
	}
	drag_layout(l);
}

static bool row_menu(cell *c, double x, double y);

static void on_reorder_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	(void)dx;
	(void)dy;
	list_t *l = data;
	if (!l->drag.armed || l->drag.gesture != (GtkGesture *)g) return;
	if (!l->drag.lifted) {
		end_drag(l);
		return;
	}
	bool held = l->drag.touch && hypot(dx, dy) < drag_threshold();
	row_view *rv = l->drag.rv;
	double x0 = l->drag.x0;
	drop(l);
	// A long press let go where it was: the row's menu (its swipe actions).
	if (held && rv && rv->cells.len) row_menu(rv->cells.data[0], x0, rv->cells.data[0]->height / 2);
}

static void on_reorder_cancel(GtkGesture *g, gpointer seq, gpointer data) {
	(void)seq;
	list_t *l = data;
	if (l->drag.armed && l->drag.gesture == g) cancel_drag(l);
}

// on_reorder_long lifts a touchscreen's held row.
static void on_reorder_long(GtkGestureLongPress *g, double x, double y, gpointer data) {
	(void)g;
	(void)x;
	(void)y;
	list_t *l = data;
	if (!l->drag.armed || l->drag.lifted || !l->drag.touch) return;
	if (l->drag.gesture) gtk_gesture_set_state(l->drag.gesture, GTK_EVENT_SEQUENCE_CLAIMED);
	lift(l);
}

// focused_cell is the cell the keyboard's focus is in.
static cell *focused_cell(list_t *l) {
	for (GtkWidget *w = focus_in(l); w && w != l->n->widget; w = gtk_widget_get_parent(w)) {
		node *n = lx_view_node(w);
		if (n && part_tag(n) == PART_CELL && ((cell *)n->ext)->l == l) return n->ext;
	}
	return NULL;
}

static gboolean on_reorder_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
	(void)k;
	(void)keycode;
	list_t *l = data;
	if (!l->reorder || !(state & GDK_ALT_MASK) || (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK))) return 0;
	int by = keyval == KEY_UP_ || keyval == KEY_KP_UP_ ? -1 : keyval == KEY_DOWN_ || keyval == KEY_KP_DOWN_ ? 1 : 0;
	if (!by) return 0;
	cell *c = focused_cell(l);
	int i = c ? cell_index(c) : -1;
	if (i < 0) return 0;
	move_by_one(l, i, i + by);
	return 1;
}

// set_reorder takes Reorder: u32 count (2), then the move actions' names
// (empty: rows don't move).
static void set_reorder(list_t *l, ak_span b) {
	ak_str_vec ss = {0};
	if (b.len) ak_decode_strings((const uint8_t *)b.p, b.len, &ss);
	bool on = ss.len >= 2;
	for (int i = 0; i < 2; i++) {
		free(l->move_names[i]);
		l->move_names[i] = on ? str_dup(ss.data[i]) : NULL;
	}
	ak_vec_free(&ss);
	if (!on && l->drag.armed) cancel_drag(l);
	l->reorder = on;
	if (on && !l->reorder_wired) {
		l->reorder_wired = true;
		GtkGesture *g = gtk_gesture_drag_new();
		gtk_gesture_single_set_button((GtkGestureSingle *)g, 1);
		g_signal_connect(g, "drag-begin", G_CALLBACK(on_reorder_begin), l);
		g_signal_connect(g, "drag-update", G_CALLBACK(on_reorder_update), l);
		g_signal_connect(g, "drag-end", G_CALLBACK(on_reorder_end), l);
		g_signal_connect(g, "cancel", G_CALLBACK(on_reorder_cancel), l);
		gtk_widget_add_controller(l->n->widget, (GtkEventController *)g);
		GtkGesture *lp = gtk_gesture_long_press_new();
		gtk_gesture_single_set_touch_only((GtkGestureSingle *)lp, 1);
		g_signal_connect(lp, "pressed", G_CALLBACK(on_reorder_long), l);
		gtk_widget_add_controller(l->n->widget, (GtkEventController *)lp);
		GtkEventController *k = gtk_event_controller_key_new();
		g_signal_connect(k, "key-pressed", G_CALLBACK(on_reorder_key), l);
		gtk_widget_add_controller(l->n->widget, k);
		l->reorder_ctrls[0] = (GtkEventController *)g;
		l->reorder_ctrls[1] = (GtkEventController *)lp;
		l->reorder_ctrls[2] = k;
	}
	each_cell(l, wire_moves);
}

// one_move is whether b is a with one item moved (from, to).
static bool one_move(const int64_t *a, const int64_t *b, uint32_t n, int *from, int *to) {
	uint32_t lo = 0, hi = n;
	while (lo < n && a[lo] == b[lo]) lo++;
	if (lo == n) return false;
	while (hi > lo && a[hi - 1] == b[hi - 1]) hi--;
	uint32_t last = hi - 1;
	// a[lo] went to last: b is a[lo+1..last], a[lo].
	if (b[last] == a[lo] && !memcmp(b + lo, a + lo + 1, (last - lo) * sizeof *a)) {
		*from = (int)lo;
		*to = (int)last;
		return true;
	}
	if (b[lo] == a[last] && !memcmp(b + lo + 1, a + lo, (last - lo) * sizeof *a)) {
		*from = (int)last;
		*to = (int)lo;
		return true;
	}
	return false;
}

static bool same_sections(const lsec_vec *a, const lsec_vec *b) {
	if (a->len != b->len) return false;
	for (uint32_t i = 0; i < a->len; i++)
		if (a->data[i].start != b->data[i].start || a->data[i].id != b->data[i].id) return false;
	return true;
}

// take_update takes a ListUpdate the host's moves explain: the order it
// has (after a move of the user's: nothing to do), or it with one item
// moved (the app didn't take the move: it moves back, eased). False:
// it's another update.
static bool take_update(list_t *l) {
	bool moved = l->moved;
	l->moved = false;
	if (!l->applied || l->pending.len != l->ids.len || !same_sections(&l->pending_s, &l->sections)) return false;
	int from = 0, to = 0;
	if (!memcmp(l->pending.data, l->ids.data, l->ids.len * sizeof *l->ids.data)) return moved;
	if (!moved || l->columns > 1 || !one_move(l->ids.data, l->pending.data, l->ids.len, &from, &to)) return false;
	if (l->drag.armed) cancel_drag(l);
	commit(l, from, to, NULL);
	l->moved = false;
	return true;
}

static void free_reorder(list_t *l) {
	if (l->drag.armed) end_drag(l);
	stop_shifts(l);
	for (int i = 0; i < 3; i++)
		if (l->reorder_ctrls[i]) g_signal_handlers_disconnect_matched(l->reorder_ctrls[i], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, l);
	for (int i = 0; i < 2; i++) free(l->move_names[i]);
}

// ---- Drawing ----

static GdkRGBA card_shade(void) { return H.dark ? (GdkRGBA){0, 0, 6.0f / 255, 0.36f} : (GdkRGBA){0, 0, 6.0f / 255, 0.07f}; }

static GskRoundedRect rounded(graphene_rect_t r, float radius);

// draw_lifted draws a lifted row on a card raised over the others (as
// libadwaita's dragged rows): its rows' background, rounded, and a soft
// shadow.
static void draw_lifted(list_t *l, GtkSnapshot *snap, const graphene_rect_t *b) {
	GskRoundedRect rr = rounded(*b, grouped(l) ? 12 : 6);
	GdkRGBA sh[2] = {{0, 0, 6.0f / 255, H.dark ? 0.5f : 0.18f}, {0, 0, 6.0f / 255, H.dark ? 0.3f : 0.1f}};
	gtk_snapshot_append_outset_shadow(snap, &rr, &sh[0], 0, 4, 0, 12);
	gtk_snapshot_append_outset_shadow(snap, &rr, &sh[1], 0, 1, 1, 3);
	GdkRGBA c = grouped(l) ? H.theme.card : l->appearance == AK_LIST_APPEARANCE_BARE ? H.theme.bg : H.theme.view;
	gtk_snapshot_push_rounded_clip(snap, &rr);
	gtk_snapshot_append_color(snap, &c, b);
	gtk_snapshot_pop(snap);
}

static void draw_row(row_view *rv, GtkSnapshot *snap, const graphene_rect_t *b) {
	list_t *l = rv->l;
	int r = rv->row;
	if (r < 0 || (uint32_t)r >= l->rows.len) return;
	if (rv->lifted) {
		draw_lifted(l, snap, b);
		return;
	}
	if (rv->pinned) {
		GdkRGBA c = l->appearance == AK_LIST_APPEARANCE_BARE ? H.theme.bg : H.theme.view;
		gtk_snapshot_append_color(snap, &c, b);
	}
	if (l->appearance == AK_LIST_APPEARANCE_BARE || l->columns > 1 || l->rows.data[r].header || (uint32_t)r + 1 >= l->rows.len || l->rows.data[r + 1].header) return;
	GdkRGBA c = H.theme.fg;
	c.alpha *= 0.15f;
	if (grouped(l)) c = card_shade();
	graphene_rect_t line = {{0, b->size.height - 1}, {b->size.width, 1}};
	gtk_snapshot_append_color(snap, &c, &line);
}

static GskRoundedRect rounded(graphene_rect_t r, float radius) {
	GskRoundedRect rr = {.bounds = r};
	for (int i = 0; i < 4; i++) rr.corner[i] = (graphene_size_t){radius, radius};
	return rr;
}

// draw_cell highlights a selected row of a selecting list: the accent at a
// quarter, as libadwaita's selected rows; in a card, its first and last
// rows rounded as the card is.
static void draw_cell(cell *c, GtkSnapshot *snap, const graphene_rect_t *b) {
	list_t *l = c->l;
	if (!l->selecting || !c->selected || c->header) return;
	GdkRGBA col = H.theme.accent_bg;
	col.alpha *= 0.25f;
	GskRoundedRect rr = rounded(*b, 0);
	if (grouped(l)) {
		int r = c->rv->row;
		if (r >= 0 && (uint32_t)r < l->rows.len) {
			graphene_size_t radius = {12, 12};
			if (r == 0 || l->rows.data[r - 1].header) rr.corner[0] = rr.corner[1] = radius;
			if ((uint32_t)r + 1 >= l->rows.len || l->rows.data[r + 1].header) rr.corner[2] = rr.corner[3] = radius;
		}
	} else if (l->columns > 1 || l->appearance == AK_LIST_APPEARANCE_BARE) {
		rr = rounded(*b, 6);
	}
	gtk_snapshot_push_rounded_clip(snap, &rr);
	gtk_snapshot_append_color(snap, &col, b);
	gtk_snapshot_pop(snap);
}

// draw_card draws an inset grouped list's card: libadwaita's .card (12
// points round, its hairline and soft shadows), the part of it around what
// shows.
static void draw_card(list_card *lc, node *n, GtkSnapshot *snap, const graphene_rect_t *b) {
	list_t *l = lc->l;
	const float margin = 64;
	float top = offset(l) - n->frame[1] - margin, bottom = offset(l) + view_h(l) - n->frame[1] + margin;
	graphene_rect_t r = *b;
	if (top > 0) r.origin.y = top;
	r.size.height = (bottom < b->size.height ? bottom : b->size.height) - r.origin.y;
	if (r.size.height <= 0) return;
	GskRoundedRect rr = rounded(r, 12);
	static const float shadows[3][4] = {{0, 0, 1, 0}, {0, 1, 1, 3}, {0, 2, 2, 6}};  // dx, dy, spread, blur
	float alphas[3] = {0.03f, 0.07f, 0.03f};
	if (H.dark) {
		alphas[0] = 0.09f;
		alphas[1] = 0.09f;
		alphas[2] = 0.06f;
	}
	for (int i = 0; i < 3; i++) {
		GdkRGBA c = {0, 0, 6.0f / 255, alphas[i]};
		gtk_snapshot_append_outset_shadow(snap, &rr, &c, shadows[i][0], shadows[i][1], shadows[i][2], shadows[i][3]);
	}
	GdkRGBA c = H.theme.card;
	gtk_snapshot_push_rounded_clip(snap, &rr);
	gtk_snapshot_append_color(snap, &c, &r);
	gtk_snapshot_pop(snap);
}

static bool list_draw(node *n, GtkSnapshot *snap, const graphene_rect_t *b) {
	list_t *l = list_of(n);
	if (l) {
		if (l->appearance == AK_LIST_APPEARANCE_PLAIN && !n->background && !n->gradient.len && !n->material) {
			GdkRGBA c = H.theme.view;
			gtk_snapshot_append_color(snap, &c, b);
		}
		return true;
	}
	switch (part_tag(n)) {
	case PART_ROW:
		draw_row(n->ext, snap, b);
		return true;
	case PART_CELL:
		draw_cell(n->ext, snap, b);
		return true;
	case PART_CARD:
		draw_card(n->ext, n, snap, b);
		return true;
	}
	return false;
}

// ---- Letting go ----

void lx_each_node(void (*f)(node *n, void *ctx), void *ctx);
void lx_forget_node(node *n);

// forget lets go of a view of the list's own: out of the nodes, its
// widget let go; the core's views in it, its own Destroy takes.
static void forget(node *x) {
	for (uint32_t i = 0; i < x->children.len; i++) x->children.data[i]->parent = NULL;
	x->children.len = 0;
	if (x->ext_free == part_marker) {
		int tag = part_tag(x);
		if (tag == PART_CELL) {
			cell *c = x->ext;
			actions_free(&c->swipe);
			actions_free(&c->leading);
		}
		if (tag == PART_ROW) ak_vec_free(&((row_view *)x->ext)->cells);
		free(x->ext);
		x->ext = NULL;
		x->ext_free = NULL;
	}
	lx_forget_node(x);
}

// free_list lets go of a list's rows, cells and cards as it goes.
static void free_list(node *n) {
	list_t *l = list_of(n);
	if (!l || l->dead) return;
	l->dead = true;
	free_reorder(l);
	if (l->anim) adw_animation_skip(l->anim);
	g_signal_handlers_disconnect_matched(l->vadj, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, l);
	AK_VEC(row_view *) rows = {0};
	for (uint32_t i = 0; i < l->free_.len; i++) ak_vec_push(&rows, l->free_.data[i]);
	for (uint32_t i = 0; i < l->shown.len; i++) ak_vec_push(&rows, l->shown.data[i]);
	for (uint32_t i = 0; i < rows.len; i++) {
		row_view *rv = rows.data[i];
		for (uint32_t k = 0; k < rv->cells.len; k++) {
			node *cn = rv->cells.data[k]->n;
			forget(cn);
		}
		forget(rv->n);
	}
	ak_vec_free(&rows);
	for (uint32_t i = 0; i < l->cards.len; i++) forget(l->cards.data[i]);
	gtk_viewport_set_child((GtkViewport *)l->vp, NULL);
	forget(l->doc);
	ak_vec_free(&l->shown);
	ak_vec_free(&l->free_);
	ak_vec_free(&l->cards);
	ak_vec_free(&l->ids);
	ak_vec_free(&l->pending);
	ak_vec_free(&l->sections);
	ak_vec_free(&l->pending_s);
	ak_vec_free(&l->rows);
	ak_vec_free(&l->tops);
	free(l->heights);
	// The list_t itself stays until the node's memory goes (a signal of
	// its widgets' may still name it): list_free_mem.
}

void lx_release_list(node *n);
void lx_release_list(node *n) {
	if (n->kind == AK_KIND_LIST && n->list) {
		free_list(n);
		free(n->list);
		n->list = NULL;
	}
}

static void list_release(node *n) {
	if (list_of(n)) free_list(n);
}

// ---- The dump ----

static int by_row(const void *a, const void *b) { return (*(row_view *const *)a)->row - (*(row_view *const *)b)->row; }

static void list_dump(node *n, int depth, ak_buf *b) {
	list_t *l = list_of(n);
	if (!l) return;
	row_view **rows = malloc((l->shown.len ? l->shown.len : 1) * sizeof *rows);
	if (!rows) abort();
	memcpy(rows, l->shown.data, l->shown.len * sizeof *rows);
	if (l->shown.len > 1) qsort(rows, l->shown.len, sizeof *rows, by_row);
	for (uint32_t i = 0; i < l->shown.len; i++) {
		row_view *rv = rows[i];
		float *f = rv->n->frame;
		for (int k = 0; k <= depth; k++) ak_buf_put(b, "  ", 2);
		ak_buf_printf(b, "row %d [%.0f,%.0f %.0fx%.0f]%s%s", rv->row, f[0], f[1], f[2], f[3], rv->pinned ? " pinned" : "", rv->lifted ? " lifted" : "");
		if (rv->shift != 0) ak_buf_printf(b, " shifted %.0f", rv->shift);
		ak_buf_u8(b, '\n');
		for (uint32_t k = 0; k < rv->cells.len; k++)
			if (gtk_widget_get_visible(rv->cells.data[k]->n->widget)) lx_dump_node(rv->cells.data[k]->n, depth + 2, b);
	}
	free(rows);
}

const area lx_area_list = {
	.name = "list",
	.create = list_create,
	.prop = list_prop,
	.common = cell_prop,
	.command = list_command,
	.frame = list_frame,
	.release = list_release,
	.draw = list_draw,
	.listen = cell_listen,
	.scroll_to = list_scroll_to,
	.dump = list_dump,
};
