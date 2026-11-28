// table.c: Table, its TableRows, and Outline: rows of a List's (list.c),
// bound by the host into its cells and keyed in ListUpdate, in the
// platform's table and tree on a desktop and the web (NSTableView, a
// ListView in report view, a GtkColumnView, a <table> grid; NSOutlineView,
// a TreeView, a GtkTreeListModel's list, role tree).
//
// A Table's rows come from the for in it, each a TableRow: its cells'
// text (the fast path: the host's table draws it, from the cell's
// RowData), or its children, a view per column, each in a box of the
// core's at its column's place and width as the host shows them
// (ColumnLayout). The sort, the selection (rows' indices, which follow
// the items as they move) and the columns' widths and order are the
// host's to change and the app's to bind.
//
// An Outline's rows are what's shown of a tree of IDs: the roots, and
// each expanded item's children, depth first (the core reads them, as
// what they read changes); each row's depth and whether it expands go to
// the host in Levels, its text and symbol in its RowData (or its views,
// the row builder's). The host draws the disclosure and the indent, and
// reports the user's expanding with Expand.
//
// Phones have no table: there, the core makes the platform's List
// instead (the same rows, bound the same way), each row a TableRow it
// fills itself: a Table row's first cell over the others, an Outline
// row indented by its depth behind a disclosure.
#include "ui/list_int.h"

#include <stdlib.h>

// What the prelude's structs are, in types.asuka's field order.
static const ak_shape *const no_shapes[8];
static const uint8_t sort_key_kinds[] = {AK_K_REF, AK_K_BOOL};
static const char *const sort_key_names[] = {"column", "ascending"};
static const ak_shape shape_sort_key = {"SortKey", 2, sort_key_kinds, no_shapes, sort_key_names, 0, NULL, false, NULL};
static const uint8_t edges_kinds[] = {AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const ak_shape shape_edges = {"Edges", 6, edges_kinds, no_shapes, NULL, 0, NULL, false, NULL};
static const uint8_t font_kinds[] = {AK_K_INT, AK_K_FLOAT, AK_K_INT, AK_K_REF, AK_K_BOOL, AK_K_BOOL, AK_K_BOOL};
static const ak_shape shape_font = {"Font", 7, font_kinds, no_shapes, NULL, 0, NULL, false, NULL};

// TableColumn's fields.
enum { COL_ID, COL_TITLE, COL_WIDTH, COL_MIN, COL_MAX, COL_RESIZABLE, COL_SORTABLE, COL_ALIGN };

#define DEFAULT_WIDTH 100.0f   // a column's with none
#define OUTLINE_INDENT 16.0f   // an outline's indent per level, and its disclosure's room
#define OUTLINE_DISCLOSURE 20.0f
#define MAX_DEPTH 64           // an outline's deepest (a tree of IDs can loop)

// A set of strings, by hash: what's expanded, an outline's rows by ID.
typedef struct {
	uint32_t *slot;  // the entry's index + 1 (0: empty)
	uint32_t cap;
} keyset;

static uint32_t str_hash(const ak_str *s) {
	uint32_t h = (uint32_t)ak_val_hash((ak_val){.p = (void *)s}, AK_K_REF);
	return h ? h : 1;
}

// strset_build indexes the n strings at keys (none NULL).
static void strset_build(keyset *t, ak_str *const *keys, uint32_t n) {
	uint32_t need = 16;
	while (need < n * 2) need *= 2;
	if (t->cap < need) {
		ak_free(t->slot);
		t->slot = ak_alloc(need * sizeof(uint32_t));
		t->cap = need;
	} else {
		memset(t->slot, 0, t->cap * sizeof(uint32_t));
	}
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t j = str_hash(keys[i]) & (t->cap - 1);; j = (j + 1) & (t->cap - 1)) {
			if (!t->slot[j]) {
				t->slot[j] = i + 1;
				break;
			}
			if (ak_str_eq(keys[t->slot[j] - 1], keys[i])) break;  // a repeat: the first stays
		}
	}
}

// strset_find is the index of s among the keys, or -1.
static int64_t strset_find(const keyset *t, ak_str *const *keys, const ak_str *s) {
	if (!t->cap || !s) return -1;
	for (uint32_t j = str_hash(s) & (t->cap - 1);; j = (j + 1) & (t->cap - 1)) {
		if (!t->slot[j]) return -1;
		if (ak_str_eq(keys[t->slot[j] - 1], s)) return t->slot[j] - 1;
	}
}

typedef AK_VEC(ak_str *) str_vec;
typedef AK_VEC(uint32_t) u32_vec;

typedef struct ak_tbl {
	list_t *st;
	ak_view *e;
	bool outline;
	bool phone;  // a List stands in for it
	float row_h;  // RowHeight, as sent (0: not yet)
	ak_node sig;  // what the rows are made of changed: an outline's keys
	// A Table's columns as the host shows them (ColumnLayout): their
	// indices in columns, in order, and their widths.
	u32_vec shown;
	AK_VEC(float) shown_w;
	bool laid;
	ak_buf cols_sent, sort_sent, sel_sent;
	bool sort_known, sel_known;
	// The selection: a Table's rows' indices, sorted; an outline's rows
	// shown that are selected.
	u32_vec sel;
	bool sel_post;  // a remapped selection is to be reported
	// What the app hasn't bound: the sort, an outline's expanded and
	// selected IDs ([SortKey], [string], [string]), the user's.
	ak_arr *own_sort, *own_expanded, *own_sel;
	// An outline's rows shown: their IDs (rows), each one's depth and
	// flags (levels: u16, u8), as the last flattening made them (next_*)
	// and as sent.
	str_vec rows, next_rows;
	ak_buf levels, next_levels, levels_sent;
	bool levels_known;
	keyset row_set, open_set;
	ak_node lv;  // the levels changed: a phone's row reads its depth
} ak_tbl;

static ak_closure *closure_key(const ak_view *e, uint16_t key) {
	const ak_val *v = ak_style_get(e, key);
	return v ? v->p : NULL;
}

static const ak_arr *arr_key(const ak_view *e, uint16_t key) {
	const ak_val *v = ak_style_get(e, key);
	return v ? v->p : NULL;
}

static bool is_phone(ak_ui_root *r) {
	uint8_t p = r ? r->env.platform : AK_PLATFORM_UNKNOWN;
	if (p == AK_PLATFORM_UNKNOWN) {
		// Before the host's environment: the platform the core was built for.
#if defined(__ANDROID__)
		p = AK_PLATFORM_ANDROID;
#elif defined(__APPLE__) && defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
		p = AK_PLATFORM_IOS;
#elif defined(__APPLE__) && (defined(__ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__) || defined(__ENVIRONMENT_WATCH_OS_VERSION_MIN_REQUIRED__))
		p = AK_PLATFORM_IOS;
#endif
	}
	return p == AK_PLATFORM_IOS || p == AK_PLATFORM_ANDROID;
}

static void release_ctx(void *ctx) { ak_release(ctx); }

// post_call calls e's handler for ev with arg (taken) soon: from inside
// the items' effect, not then.
typedef struct {
	ak_view *e;
	uint16_t ev;
	ak_val arg;
} post_t;

static void posted(void *ctx) {
	post_t *p = ctx;
	if (!p->e->dead) {
		ak_batch_begin();
		ak_view_call(p->e, p->ev, &p->arg, 1);
		ak_batch_end();
		ak_ui_flush();
	}
	ak_release(p->arg.p);
	ak_release(p->e);
	ak_free(p);
}

static void post_call(ak_view *e, uint16_t ev, void *arg) {
	post_t *p = ak_alloc(sizeof *p);
	p->e = ak_retain(e);
	p->ev = ev;
	p->arg.p = arg;
	ak_post(posted, p);
}

// ---- Encoding ----

static void put_str(ak_buf *b, const ak_str *s) { ak_buf_str(b, s ? s->data : "", s ? s->len : 0); }

// send_if sends prop p's value b, unless it's what was sent (*sent,
// known), and keeps it as sent.
static void send_if(ak_view *e, ak_prop p, ak_buf *b, ak_buf *sent, bool *known) {
	if (*known && sent->len == b->len && (b->len == 0 || memcmp(sent->data, b->data, b->len) == 0)) return;
	sent->len = 0;
	ak_buf_put(sent, b->data, b->len);
	*known = true;
	if (e->real && !e->dead) ak_batch_set_bytes(e->r->batch, e->id, p, b->data, b->len);
}

// encode_runs encodes sorted rows as Selection's runs.
static void encode_runs(ak_buf *b, const u32_vec *rows) {
	uint32_t n = 0;
	size_t at = b->len;
	ak_buf_u32(b, 0);
	for (uint32_t i = 0; i < rows->len;) {
		uint32_t start = rows->data[i], len = 1;
		while (i + len < rows->len && rows->data[i + len] == start + len) len++;
		ak_buf_u32(b, start);
		ak_buf_u32(b, len);
		n++;
		i += len;
	}
	uint8_t le[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
	memcpy(b->data + at, le, 4);
}

// decode_runs reads Selection's runs into rows (sorted, below count).
static void decode_runs(ak_span s, int64_t count, u32_vec *rows) {
	rows->len = 0;
	ak_reader r = {(const uint8_t *)s.p, s.len, 0, false};
	uint32_t n = ak_read_u32(&r);
	int64_t last = -1;
	for (uint32_t i = 0; i < n && !r.err; i++) {
		uint32_t start = ak_read_u32(&r), len = ak_read_u32(&r);
		if (r.err) break;
		for (uint64_t j = start; j < (uint64_t)start + len && (int64_t)j < count; j++) {
			if ((int64_t)j <= last) continue;  // out of order: what's after
			ak_vec_push(rows, (uint32_t)j);
			last = (int64_t)j;
		}
	}
}

static int cmp_u32(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static void sort_unique(u32_vec *v) {
	if (v->len > 1) qsort(v->data, v->len, sizeof(uint32_t), cmp_u32);
	uint32_t n = 0;
	for (uint32_t i = 0; i < v->len; i++)
		if (!n || v->data[n - 1] != v->data[i]) v->data[n++] = v->data[i];
	v->len = n;
}

static ak_arr *int_array(const u32_vec *v) {
	ak_arr *a = ak_arr_new(AK_K_INT, NULL, v->len);
	for (uint32_t i = 0; i < v->len; i++) ak_arr_push(&a, (ak_val){.i = v->data[i]});
	return a;
}

static ak_arr *str_array(uint32_t cap) {
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, cap);
	a->h.flags = 1;  // strings
	return a;
}

// ---- Columns ----

static uint32_t ncols(const ak_tbl *t) {
	const ak_arr *c = arr_key(t->e, AK_KEY_TABLE_COLUMNS);
	return c ? c->len : 0;
}

static const ak_struct *col_at(const ak_tbl *t, uint32_t i) {
	const ak_arr *c = arr_key(t->e, AK_KEY_TABLE_COLUMNS);
	return c && i < c->len ? ak_arr_vals(c)[i].p : NULL;
}

static float col_width(const ak_struct *c) {
	float w = c ? (float)c->f[COL_WIDTH].f : 0;
	return w > 0 ? w : DEFAULT_WIDTH;
}

// column_place is column i's x from the row's start and width, as the
// host shows them (else the app's order and widths).
static bool column_place(const ak_tbl *t, uint32_t i, float *x, float *w) {
	float at = 0;
	if (t->laid) {
		for (uint32_t k = 0; k < t->shown.len; k++) {
			if (t->shown.data[k] == i) {
				*x = at, *w = t->shown_w.data[k];
				return true;
			}
			at += t->shown_w.data[k];
		}
		return false;  // hidden
	}
	uint32_t n = ncols(t);
	for (uint32_t k = 0; k < n; k++) {
		float cw = col_width(col_at(t, k));
		if (k == i) {
			*x = at, *w = cw;
			return true;
		}
		at += cw;
	}
	return false;
}

static float columns_width(const ak_tbl *t) {
	float w = 0;
	if (t->laid) {
		for (uint32_t k = 0; k < t->shown.len; k++) w += t->shown_w.data[k];
		return w;
	}
	uint32_t n = ncols(t);
	for (uint32_t k = 0; k < n; k++) w += col_width(col_at(t, k));
	return w;
}

static void send_columns(ak_tbl *t) {
	const ak_arr *cols = arr_key(t->e, AK_KEY_TABLE_COLUMNS);
	ak_buf b = {0};
	ak_buf_u32(&b, cols ? cols->len : 0);
	for (uint32_t i = 0; cols && i < cols->len; i++) {
		const ak_struct *c = ak_arr_vals(cols)[i].p;
		put_str(&b, c->f[COL_ID].p);
		put_str(&b, c->f[COL_TITLE].p);
		ak_buf_f32(&b, (float)c->f[COL_WIDTH].f);
		ak_buf_f32(&b, (float)c->f[COL_MIN].f);
		ak_buf_f32(&b, (float)c->f[COL_MAX].f);
		ak_buf_u8(&b, (uint8_t)((c->f[COL_RESIZABLE].i ? 1 : 0) | (c->f[COL_SORTABLE].i ? 2 : 0)));
		ak_buf_u8(&b, (uint8_t)c->f[COL_ALIGN].i);
	}
	bool known = t->cols_sent.len > 0;
	send_if(t->e, AK_PROP_TABLE_COLUMNS, &b, &t->cols_sent, &known);
	ak_buf_free(&b);
}

// col_index is the index of the column with ID id, or -1.
static int64_t col_index(const ak_tbl *t, const ak_str *id) {
	uint32_t n = ncols(t);
	for (uint32_t i = 0; i < n; i++)
		if (id && ak_str_eq(col_at(t, i)->f[COL_ID].p, id)) return i;
	return -1;
}

static const ak_arr *sort_now(const ak_tbl *t) {
	const ak_val *v = ak_style_get(t->e, AK_KEY_TABLE_SORT);
	return v ? v->p : t->own_sort;
}

static void send_sort(ak_tbl *t) {
	if (t->phone) return;
	const ak_arr *s = sort_now(t);
	ak_buf b = {0};
	uint32_t n = 0;
	ak_buf_u32(&b, 0);
	for (uint32_t i = 0; s && i < s->len; i++) {
		const ak_struct *k = ak_arr_vals(s)[i].p;
		int64_t c = col_index(t, k->f[0].p);
		if (c < 0) continue;
		ak_buf_u32(&b, (uint32_t)c);
		ak_buf_u8(&b, k->f[1].i ? 1 : 0);
		n++;
	}
	uint8_t le[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
	memcpy(b.data, le, 4);
	send_if(t->e, AK_PROP_TABLE_SORT, &b, &t->sort_sent, &t->sort_known);
	ak_buf_free(&b);
}

// The views of the bound rows' cells are laid out again: the columns
// moved, or changed width.
static void tcells_relayout(ak_tbl *t);

// ---- The selection ----

static void send_selection(ak_tbl *t) {
	if (t->phone) return;
	ak_buf b = {0};
	encode_runs(&b, &t->sel);
	send_if(t->e, t->outline ? AK_PROP_OUTLINE_SELECTION : AK_PROP_TABLE_SELECTION, &b, &t->sel_sent, &t->sel_known);
	ak_buf_free(&b);
}

static const ak_arr *outline_sel_ids(const ak_tbl *t) {
	const ak_val *v = ak_style_get(t->e, AK_KEY_OUTLINE_SELECTION);
	return v ? v->p : t->own_sel;
}

// outline_selection works out which shown rows are selected.
static void outline_selection(ak_tbl *t) {
	const ak_arr *ids = outline_sel_ids(t);
	t->sel.len = 0;
	for (uint32_t i = 0; ids && i < ids->len; i++) {
		int64_t at = strset_find(&t->row_set, t->rows.data, ak_arr_vals(ids)[i].p);
		if (at >= 0) ak_vec_push(&t->sel, (uint32_t)at);
	}
	sort_unique(&t->sel);
}

// user_selected hears the host's RowsSelected: the rows selected now.
static void user_selected(ak_tbl *t, ak_span s) {
	ak_view *e = t->e;
	decode_runs(s, ak_list_items_count(t->st), &t->sel);
	ak_buf b = {0};
	encode_runs(&b, &t->sel);
	t->sel_sent.len = 0;  // the host shows this
	ak_buf_put(&t->sel_sent, b.data, b.len);
	t->sel_known = true;
	ak_buf_free(&b);
	if (!t->outline) {
		ak_arr *rows = int_array(&t->sel);
		ak_val arg = {.p = rows};
		ak_view_call(e, AK_EV_TABLE_SELECTION_CHANGE, &arg, 1);
		ak_release(rows);
		return;
	}
	ak_arr *ids = str_array(t->sel.len);
	for (uint32_t i = 0; i < t->sel.len; i++) ak_arr_push(&ids, (ak_val){.p = ak_retain(t->rows.data[t->sel.data[i]])});
	ak_release(t->own_sel);
	t->own_sel = ak_retain(ids);
	ak_val arg = {.p = ids};
	ak_view_call(e, AK_EV_OUTLINE_SELECTION_CHANGE, &arg, 1);
	ak_release(ids);
}

// ---- The items, as they change ----

// A map of the new items' IDs to their indices (a Table's selection
// following them).
typedef struct {
	int64_t *ids;
	uint32_t *at;  // index + 1 (0: empty)
	uint32_t cap;
} idmap;

static idmap ids_scratch;  // main thread's

static uint32_t id_hash(int64_t id) {
	uint64_t x = (uint64_t)id * UINT64_C(0x9e3779b97f4a7c15);
	return (uint32_t)(x >> 32);
}

static void idmap_build(idmap *m, const int64_t *ids, uint32_t n) {
	uint32_t need = 16;
	while (need < n * 2) need *= 2;
	if (m->cap < need) {
		ak_free(m->ids);
		ak_free(m->at);
		m->ids = ak_alloc(need * sizeof(int64_t));
		m->at = ak_alloc(need * sizeof(uint32_t));
		m->cap = need;
	} else {
		memset(m->at, 0, m->cap * sizeof(uint32_t));
	}
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t j = id_hash(ids[i]) & (m->cap - 1);; j = (j + 1) & (m->cap - 1)) {
			if (!m->at[j]) {
				m->ids[j] = ids[i];
				m->at[j] = i + 1;
				break;
			}
			if (m->ids[j] == ids[i]) break;
		}
	}
}

static int64_t idmap_find(const idmap *m, int64_t id) {
	for (uint32_t j = id_hash(id) & (m->cap - 1);; j = (j + 1) & (m->cap - 1)) {
		if (!m->at[j]) return -1;
		if (m->ids[j] == id) return m->at[j] - 1;
	}
}

static void outline_commit(ak_tbl *t);

// ak_tbl_items hears the items' new IDs (st->order) before ListUpdate
// goes: an outline's levels go first; a Table's selection is the rows of
// the items it was, where they are now.
void ak_tbl_items(list_t *st) {
	ak_tbl *t = st->tbl;
	if (t->outline) {
		outline_commit(t);
		return;
	}
	if (!t->sel.len || !st->sent) return;
	idmap_build(&ids_scratch, st->order.data, st->order.len);
	u32_vec next = {0};
	for (uint32_t i = 0; i < t->sel.len; i++) {
		uint32_t s = t->sel.data[i];
		if (s >= st->sent_ids.len) continue;
		int64_t at = idmap_find(&ids_scratch, st->sent_ids.data[s]);
		if (at >= 0) ak_vec_push(&next, (uint32_t)at);
	}
	sort_unique(&next);
	bool same = next.len == t->sel.len && (next.len == 0 || memcmp(next.data, t->sel.data, next.len * sizeof(uint32_t)) == 0);
	if (!same) {
		ak_vec_free(&t->sel);
		t->sel = next;
		t->sel_post = true;
	} else {
		ak_vec_free(&next);
	}
}

// ak_tbl_items_sent follows ListUpdate: the selection of the rows as
// they are now (a Table's, reported to the app, which binds it).
void ak_tbl_items_sent(list_t *st) {
	ak_tbl *t = st->tbl;
	if (t->outline) {
		outline_selection(t);
		send_selection(t);
		return;
	}
	if (!t->sel_post) return;
	t->sel_post = false;
	send_selection(t);
	if (ak_view_handler(t->e, AK_EV_TABLE_SELECTION_CHANGE)) post_call(t->e, AK_EV_TABLE_SELECTION_CHANGE, int_array(&t->sel));
}

// ---- An outline's rows: the tree, flattened ----

static const ak_arr *expanded_now(const ak_tbl *t) {
	const ak_val *v = ak_style_get(t->e, AK_KEY_OUTLINE_EXPANDED);
	return v ? v->p : t->own_expanded;
}

typedef struct {
	ak_tbl *t;
	ak_arr *out;
	str_vec open;  // what's expanded, for the set
	ak_closure *children, *expandable;
	bool failed;
} flat_ctx;

// call_str calls f with id: what it gives (owned), NULL with an error
// pending.
static ak_val call_id(ak_closure *f, ak_str *id) {
	ak_val arg = {.p = id};
	return ak_call(f, &arg, 1);
}

static void visit(flat_ctx *fc, ak_str *id, uint32_t depth) {
	ak_tbl *t = fc->t;
	if (fc->failed || !id) return;
	ak_arr_push(&fc->out, (ak_val){.p = ak_retain(id)});
	ak_vec_push(&t->next_rows, id);
	bool can = false;
	ak_arr *kids = NULL;
	if (fc->expandable) {
		ak_val v = call_id(fc->expandable, id);
		if (ak_error_pending()) {
			fc->failed = true;
			return;
		}
		can = v.i != 0;
	}
	bool open = strset_find(&t->open_set, fc->open.data, id) >= 0;
	if ((open && (can || !fc->expandable)) || !fc->expandable) {
		if (fc->children) {
			ak_val v = call_id(fc->children, id);
			if (ak_error_pending()) {
				fc->failed = true;
				return;
			}
			kids = v.p;
		}
		if (!fc->expandable) can = kids && kids->len > 0;
	}
	bool shown = open && can;
	ak_buf_u16(&t->next_levels, (uint16_t)depth);
	ak_buf_u8(&t->next_levels, (uint8_t)((can ? 1 : 0) | (shown ? 2 : 0)));
	if (shown && kids && depth + 1 < MAX_DEPTH)
		for (uint32_t i = 0; i < kids->len && !fc->failed; i++) visit(fc, ak_arr_vals(kids)[i].p, depth + 1);
	ak_release(kids);
}

// outline_items is the outline's items' function (its List's): the IDs
// shown, depth first, their levels kept for outline_commit. It reads
// the keys (t->sig), the roots, what's expanded and, through the
// functions, what they read.
static ak_val outline_items(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_tbl *t = (ak_tbl *)c->fn;
	ak_signal_read(&t->sig);
	ak_view *e = t->e;
	flat_ctx fc = {t, str_array(16), {0}, closure_key(e, AK_KEY_OUTLINE_CHILDREN_OF), closure_key(e, AK_KEY_OUTLINE_EXPANDABLE), false};
	const ak_arr *open = expanded_now(t);
	for (uint32_t i = 0; open && i < open->len; i++)
		if (ak_arr_vals(open)[i].p) ak_vec_push(&fc.open, ak_arr_vals(open)[i].p);
	strset_build(&t->open_set, fc.open.data, fc.open.len);
	t->next_rows.len = 0;
	t->next_levels.len = 0;
	const ak_arr *roots = arr_key(e, AK_KEY_OUTLINE_ROOTS);
	for (uint32_t i = 0; roots && i < roots->len && !fc.failed; i++) visit(&fc, ak_arr_vals(roots)[i].p, 0);
	ak_vec_free(&fc.open);
	if (fc.failed) {
		ak_release(fc.out);
		t->next_rows.len = 0;
		return (ak_val){0};
	}
	return (ak_val){.p = fc.out};
}

// outline_commit takes the flattening's rows and levels (the items the
// list has now), and sends the levels if they changed, before
// ListUpdate.
static void outline_commit(ak_tbl *t) {
	str_vec rv = t->rows;
	t->rows = t->next_rows;
	t->next_rows = rv;
	t->next_rows.len = 0;
	ak_buf lb = t->levels;
	t->levels = t->next_levels;
	t->next_levels = lb;
	t->next_levels.len = 0;
	strset_build(&t->row_set, t->rows.data, t->rows.len);
	ak_signal_write(&t->lv);
	if (t->phone) return;
	ak_buf b = {0};
	ak_buf_u32(&b, t->rows.len);
	ak_buf_put(&b, t->levels.data, t->levels.len);
	send_if(t->e, AK_PROP_OUTLINE_LEVELS, &b, &t->levels_sent, &t->levels_known);
	ak_buf_free(&b);
}

// row_level is row i's depth and flags (1 expands, 2 expanded).
static void row_level(const ak_tbl *t, int64_t i, uint32_t *depth, uint8_t *flags) {
	*depth = 0, *flags = 0;
	if (i < 0 || (uint64_t)(i + 1) * 3 > t->levels.len) return;
	const uint8_t *p = t->levels.data + i * 3;
	*depth = (uint32_t)p[0] | (uint32_t)p[1] << 8;
	*flags = p[2];
}

static ak_val outline_key(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	ak_retain(a[0].p);
	return a[0];
}


// ---- TableRow ----

typedef struct trow {
	ak_buf sent;  // RowData as sent
	bool has_sent;
	ak_view *e;
	// A phone's: its views (lead: the disclosure and symbol before the
	// body: its first cell over the rest), and an outline row's table,
	// index and item accessors (retained).
	ak_view *lead, *icon, *body, *title, *detail, *rest, *chevron;
	uint32_t added;  // the app's children
	ak_tbl *t;
	void *index, *item;
} trow;

extern const ak_el_class ak_class_trow, ak_class_trow_phone;
static const ak_el_class cls_tcell;

static bool is_trow(const ak_view *v) { return v && (v->cls == &ak_class_trow || v->cls == &ak_class_trow_phone); }

// cell_of is the List cell v's row is in (NULL: none, yet).
static lcell *cell_of(const ak_view *v) {
	for (const ak_view *p = v; p; p = p->parent)
		if (p->cls == &ak_class_lcell) return p->impl;
	return NULL;
}

// row_of is the TableRow at the top of cell c's row, through fragments.
static ak_view *row_of(const lcell *c) {
	ak_view *v = c->el && c->el->kids.len ? c->el->kids.data[0] : NULL;
	while (v && !is_trow(v) && v->frag && v->kids.len) v = v->kids.data[0];
	return is_trow(v) ? v : NULL;
}

static ak_buf row_scratch;  // the main thread's

// trow_send sends a desktop row's RowData to its cell, if it changed.
static void trow_send(ak_view *v) {
	trow *tr = v->impl;
	lcell *c = cell_of(v);
	if (!tr || !c || c->header || row_of(c) != v || c->el->dead) return;
	const ak_arr *cells = arr_key(v, AK_KEY_TABLE_ROW_CELLS);
	const ak_val *icon = ak_style_get(v, AK_KEY_TABLE_ROW_ICON);
	ak_buf *b = &row_scratch;
	b->len = 0;
	ak_buf_u32(b, cells ? cells->len : 0);
	for (uint32_t i = 0; cells && i < cells->len; i++) put_str(b, ak_arr_vals(cells)[i].p);
	put_str(b, icon ? icon->p : NULL);
	if (tr->has_sent && tr->sent.len == b->len && memcmp(tr->sent.data, b->data, b->len) == 0) return;
	tr->sent.len = 0;
	ak_buf_put(&tr->sent, b->data, b->len);
	tr->has_sent = true;
	ak_batch_set_bytes(v->r->batch, c->el->id, AK_PROP_ROW_DATA, b->data, b->len);
}

static void trow_init(ak_view *e) {
	trow *tr = ak_alloc(sizeof *tr);
	tr->e = e;
	e->impl = tr;
}

static void trow_drop(ak_view *e) {
	trow *tr = e->impl;
	if (!tr) return;
	ak_buf_free(&tr->sent);
	ak_release(tr->index);
	ak_release(tr->item);
	ak_free(tr);
	e->impl = NULL;
}

static void trow_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(v), AK_UNUSED(prev);
	if (key == AK_KEY_TABLE_ROW_CELLS || key == AK_KEY_TABLE_ROW_ICON) trow_send(e);
}

static void trow_adjust(ak_view *e, ak_lstyle *ls) {
	AK_UNUSED(e);
	ls->width = ak_lpct(100);
	ls->height = ak_lpct(100);
}

// A row's views go each in a box of the core's at its column's place
// (cls_tcell), so the cell's children are its columns', one each, in the
// columns' order.
static void trow_add(ak_view *e, ak_view *child) {
	ak_view *w = ak_view_new(e->r, &cls_tcell, 0, true);
	w->finalized = true;
	w->variant = (int32_t)e->kids.len;  // its column
	ak_batch_set_bool(e->r->batch, w->id, AK_PROP_CLIP, true);
	ak_style_relayout(w);
	ak_view_append(w, child);
	ak_view_append(e, w);
	ak_release(w);
}

static void tcell_adjust(ak_view *e, ak_lstyle *ls) {
	lcell *c = cell_of(e);
	ak_tbl *t = c ? c->st->tbl : NULL;
	float x = 0, w = DEFAULT_WIDTH;
	bool shown = true;
	if (t) shown = column_place(t, (uint32_t)e->variant, &x, &w);
	if (!shown) w = 0;
	if (t && c->width > 0 && ak_view_rtl(t->e)) x = c->width - x - w;  // physical: the first column at the right
	ls->position = AK_LABSOLUTE;
	ls->inset = (ak_lrect){ak_lpt(0), AK_LAUTO, AK_LAUTO, ak_lpt(x)};
	ls->width = ak_lpt(w);
	ls->height = ak_lpct(100);
	ls->flex_direction = AK_LROW;
	ls->align_items = AK_LALIGN_CENTER;
}

static const ak_el_class cls_tcell = {.name = "TableCell", .kind = AK_KIND_VIEW, .flags = AK_ELF_CHILDREN, .adjust = tcell_adjust};

const ak_el_class ak_class_trow = {.name = "TableRow", .kind = AK_KIND_VIEW, .flags = AK_ELF_FLATTEN | AK_ELF_CHILDREN, .init = trow_init,
                                   .set = trow_set, .adjust = trow_adjust, .add = trow_add, .drop = trow_drop};

static void tcells_relayout(ak_tbl *t) {
	list_t *st = t->st;
	for (uint32_t i = 0; i < st->cells.len; i++) {
		lcell *c = st->cells.data[i];
		c->width = -1;
		ak_view *row = row_of(c);
		for (uint32_t k = 0; row && k < row->kids.len; k++)
			if (row->kids.data[k]->cls == &cls_tcell) ak_style_relayout(row->kids.data[k]);
		ak_style_relayout(c->el);
	}
}

// ---- A phone's rows ----

static ak_val edges(float top, float bottom, float start, float end) {
	ak_struct *s = ak_struct_new(&shape_edges);
	s->f[0].f = top, s->f[2].f = bottom, s->f[4].f = start, s->f[5].f = end;
	return (ak_val){.p = s};
}

static void set_owned(ak_view *v, uint16_t key, ak_val x) {
	ak_ui_set(v, key, x);
	ak_release_val(x, (ak_kind)ak_key_kinds[key]);
}

static void set_i(ak_view *v, uint16_t key, int64_t i) { ak_ui_set(v, key, (ak_val){.i = i}); }
static void set_f(ak_view *v, uint16_t key, double f) { ak_ui_set(v, key, (ak_val){.f = f}); }

static void add_kid(ak_view *p, ak_view *k) {
	ak_view_append(p, k);
	ak_release(k);
}

// new_under makes element el for row e's own views, under e's owner: not
// the binding's effect a key's change runs in, which the views would
// hold (and the effect the row).
typedef struct {
	uint16_t el;
	ak_view *out;
} new_ctx;

static void new_fn(void *ctx) {
	new_ctx *m = ctx;
	m->out = ak_ui_new(m->el);
}

static ak_view *new_under(ak_view *e, uint16_t el) {
	new_ctx m = {el, NULL};
	ak_ui_root *prev = ak_ui.cur;
	ak_ui.cur = e->r;
	if (e->owner && !ak_owner_disposed(e->owner)) ak_with_owner(e->owner, new_fn, &m);
	else new_fn(&m);
	ak_ui.cur = prev;
	return m.out;
}

static ak_view *text_view(ak_view *e, uint8_t style, ak_color color) {
	ak_view *t = new_under(e, AK_EL_TEXT);
	ak_struct *f = ak_struct_new(&shape_font);
	f->f[0].i = style;
	f->f[3].p = ak_str_empty();
	set_owned(t, AK_KEY_FONT, (ak_val){.p = f});
	if (color) set_i(t, AK_KEY_COLOR, (int64_t)color);
	set_i(t, AK_KEY_LINES, 1);
	return t;
}

static void put_text(ak_view *t, const ak_str *s) {
	ak_ui_set(t, AK_KEY_TEXT_TEXT, (ak_val){.p = (void *)(s ? s : ak_str_empty())});
	set_i(t, AK_KEY_HIDDEN, !s || s->len == 0);
}

static void phone_texts(trow *tr) {
	if (tr->title) return;
	tr->title = text_view(tr->e, AK_TEXT_STYLE_BODY, 0);
	ak_view_append(tr->body, tr->title);
	ak_release(tr->title);
}

// phone_detail shows the detail line under the title, or none: a row
// with none (an outline's) has no view for it, which, hidden, would
// still take its line (an empty text is a line tall on Android).
static void phone_detail(trow *tr, const ak_str *s) {
	if (!s || !s->len) {
		if (tr->detail) ak_view_remove(tr->body, tr->detail, true);
		tr->detail = NULL;
		return;
	}
	if (!tr->detail) {
		tr->detail = text_view(tr->e, AK_TEXT_STYLE_SUBHEADLINE, ak_semantic_color(AK_SEMANTIC_COLOR_SECONDARY_LABEL));
		ak_view_append(tr->body, tr->detail);
		ak_release(tr->detail);
	}
	put_text(tr->detail, s);
}

static void phone_icon(trow *tr, const ak_str *name) {
	bool none = !name || !name->len;
	if (!tr->icon) {
		if (none) return;
		tr->icon = new_under(tr->e, AK_EL_SYMBOL);
		set_i(tr->icon, AK_KEY_COLOR, (int64_t)ak_semantic_color(AK_SEMANTIC_COLOR_ACCENT));
		set_owned(tr->icon, AK_KEY_MARGIN, edges(0, 0, 0, 12));
		ak_view_append(tr->lead, tr->icon);  // after an outline's disclosure
		ak_release(tr->icon);
	}
	ak_ui_set(tr->icon, AK_KEY_SYMBOL_NAME, (ak_val){.p = (void *)(none ? ak_str_empty() : name)});
	set_i(tr->icon, AK_KEY_HIDDEN, none);
}

// phone_cells shows a phone's Table row's cells: the first, over the
// rest after one another, a dot between them.
static void phone_cells(trow *tr) {
	const ak_arr *cells = arr_key(tr->e, AK_KEY_TABLE_ROW_CELLS);
	if (!cells || !cells->len) {
		if (tr->title) put_text(tr->title, NULL), phone_detail(tr, NULL);
		return;
	}
	phone_texts(tr);
	put_text(tr->title, ak_arr_vals(cells)[0].p);
	ak_buf b = {0};
	for (uint32_t i = 1; i < cells->len; i++) {
		const ak_str *s = ak_arr_vals(cells)[i].p;
		if (!s || !s->len) continue;
		if (b.len) ak_buf_put(&b, " \xc2\xb7 ", 4);
		ak_buf_put(&b, s->data, s->len);
	}
	ak_str *d = ak_str_new((const char *)b.data, b.len);
	phone_detail(tr, d);
	ak_release(d);
	ak_buf_free(&b);
}

static void trow_phone_init(ak_view *e) {
	trow_init(e);
	trow *tr = e->impl;
	set_i(e, AK_KEY_DIRECTION, 1);  // .row
	set_i(e, AK_KEY_ALIGN, 2);      // .center
	set_owned(e, AK_KEY_PADDING, edges(11, 11, 16, 16));
	tr->lead = new_under(e, AK_EL_ROW);
	set_i(tr->lead, AK_KEY_ALIGN, 2);
	tr->body = new_under(e, AK_EL_COLUMN);
	set_f(tr->body, AK_KEY_GROW, 1);
	set_f(tr->body, AK_KEY_SHRINK, 1);
	set_f(tr->body, AK_KEY_GAP, 2);
	ak_view_append(e, tr->lead);
	ak_view_append(e, tr->body);
	ak_release(tr->lead);
	ak_release(tr->body);
	ak_view_listen(e, AK_EVENT_PRESS);
}

static void trow_phone_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(prev);
	trow *tr = e->impl;
	if (key == AK_KEY_TABLE_ROW_CELLS) phone_cells(tr);
	else if (key == AK_KEY_TABLE_ROW_ICON) phone_icon(tr, v.p);
}

// A phone's row's views: the first on the body's first line, the rest
// after one another on a line under it.
static void trow_phone_add(ak_view *e, ak_view *child) {
	trow *tr = e->impl;
	if (tr->added++ == 0 || tr->t) {
		ak_view_append(tr->body, child);
		return;
	}
	if (!tr->rest) {
		tr->rest = new_under(e, AK_EL_ROW);
		set_f(tr->rest, AK_KEY_GAP, 8);
		set_i(tr->rest, AK_KEY_WRAP, 1);
		add_kid(tr->body, tr->rest);
	}
	ak_view_append(tr->rest, child);
}

static void phone_pressed(ak_view *e);
static void run_menu(lcell *c, int64_t n);

static void trow_phone_event(ak_view *e, const ak_event *ev) {
	if (ev->kind == AK_EVENT_PRESS) {
		phone_pressed(e);
	} else if (ev->kind == AK_EVENT_ACTION) {
		lcell *c = cell_of(e);
		if (c) run_menu(c, ev->i);
	}
}

const ak_el_class ak_class_trow_phone = {.name = "TableRow", .kind = AK_KIND_VIEW, .flags = AK_ELF_CHILDREN, .init = trow_phone_init,
                                         .set = trow_phone_set, .event = trow_phone_event, .add = trow_phone_add, .drop = trow_drop};

// ---- An outline's rows ----

static int64_t trow_index(trow *tr, bool track) {
	if (!tr->index) return -1;
	if (!track) {
		void *tok = ak_untrack_begin();
		ak_val v = ak_vm_acc_get(tr->index);
		ak_untrack_end(tok);
		return v.i;
	}
	return ak_vm_acc_get(tr->index).i;
}

static ak_val level_padding(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	trow *tr = (trow *)c->fn;
	ak_signal_read(&tr->t->lv);
	uint32_t depth;
	uint8_t flags;
	row_level(tr->t, trow_index(tr, true), &depth, &flags);
	return edges(11, 11, 8 + OUTLINE_INDENT * (float)depth, 16);
}

static ak_val level_chevron(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	trow *tr = (trow *)c->fn;
	ak_signal_read(&tr->t->lv);
	uint32_t depth;
	uint8_t flags;
	row_level(tr->t, trow_index(tr, true), &depth, &flags);
	return (ak_val){.p = ak_str_c(!(flags & 1) ? "" : (flags & 2) ? "chevron.down" : "chevron.forward")};
}

static void expand_row(ak_tbl *t, int64_t i, bool on);

static ak_val chevron_pressed(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	trow *tr = (trow *)c->fn;
	int64_t i = trow_index(tr, false);
	uint32_t depth;
	uint8_t flags;
	row_level(tr->t, i, &depth, &flags);
	if (flags & 1) expand_row(tr->t, i, !(flags & 2));
	return (ak_val){0};
}

// item_text is what the outline's function (the key in its capture)
// gives the row's item: its text (its ID without one), or its symbol.
static ak_val item_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	trow *tr = (trow *)c->fn;
	ak_val id = ak_vm_acc_get(tr->item);
	ak_closure *f = closure_key(tr->t->e, (uint16_t)c->caps[0].i);
	ak_val v = {0};
	if (f && id.p) v = call_id(f, id.p);
	else if (c->caps[0].i == AK_KEY_OUTLINE_TEXT && id.p) v.p = ak_retain(id.p);
	ak_release(id.p);
	if (ak_error_pending()) return (ak_val){0};
	return v.p ? v : (ak_val){.p = ak_str_empty()};
}

static ak_val item_cells(ak_closure *c, ak_val *a, uint32_t n) {
	ak_val s = item_text(c, a, n);
	if (ak_error_pending()) return (ak_val){0};
	ak_arr *out = str_array(1);
	ak_arr_push(&out, s);
	return (ak_val){.p = out};
}

static const uint8_t int_cap[] = {AK_K_INT};

static ak_closure *row_fn(ak_invoke_fn f, trow *tr, int64_t cap) {
	ak_closure *c = ak_closure_new(f, tr, 1, int_cap);
	c->caps[0].i = cap;
	return c;
}

static void bind_row(ak_view *v, uint16_t key, ak_invoke_fn f, trow *tr, int64_t cap) {
	ak_closure *c = row_fn(f, tr, cap);
	ak_ui_bind(v, key, c);
	ak_release(c);
}

// outline_row is the outline's rows' function (its List's): an item's
// row, the builder's views, or a TableRow of its text and symbol; on a
// phone, indented behind a disclosure.
static ak_val outline_row(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_tbl *t = (ak_tbl *)c->fn;
	ak_closure *builder = closure_key(t->e, AK_KEY_OUTLINE_ROW);
	if (!t->phone && builder) return ak_call(builder, a, 1);
	ak_view *v = ak_ui_new(AK_EL_TABLE_ROW);
	trow *tr = v->impl;
	tr->t = t;
	tr->item = ak_retain(a[0].p);
	tr->index = ak_retain(a[1].p);
	if (t->phone) {
		ak_view *chev = ak_ui_new(AK_EL_VIEW);
		set_i(chev, AK_KEY_WIDTH, ak_length_pt(24));
		set_i(chev, AK_KEY_ALIGN, 2);
		ak_closure *press = ak_closure_new(chevron_pressed, tr, 0, NULL);
		ak_ui_on(chev, AK_EV_VIEW_PRESS, press);
		ak_release(press);
		ak_view *sym = ak_ui_new(AK_EL_SYMBOL);
		set_i(sym, AK_KEY_COLOR, (int64_t)ak_semantic_color(AK_SEMANTIC_COLOR_SECONDARY_LABEL));
		ak_closure *name = ak_closure_new(level_chevron, tr, 0, NULL);
		ak_ui_bind(sym, AK_KEY_SYMBOL_NAME, name);
		ak_release(name);
		ak_ui_add(chev, sym);
		ak_release(sym);
		tr->chevron = chev;
		add_kid(tr->lead, chev);
		ak_closure *pad = ak_closure_new(level_padding, tr, 0, NULL);
		ak_ui_bind(v, AK_KEY_PADDING, pad);
		ak_release(pad);
	}
	if (closure_key(t->e, AK_KEY_OUTLINE_ICON)) bind_row(v, AK_KEY_TABLE_ROW_ICON, item_text, tr, AK_KEY_OUTLINE_ICON);
	if (t->phone && builder) {
		ak_val k = ak_call(builder, a, 1);
		if (ak_error_pending()) {
			ak_release(v);
			return (ak_val){0};
		}
		if (k.p) {
			ak_ui_add(v, k.p);
			ak_release(k.p);
		}
	} else {
		bind_row(v, AK_KEY_TABLE_ROW_CELLS, item_cells, tr, AK_KEY_OUTLINE_TEXT);
	}
	return (ak_val){.p = v};
}

// ---- Events: the user's ----

static ak_str *row_id(const ak_tbl *t, int64_t i) { return i >= 0 && i < t->rows.len ? t->rows.data[i] : NULL; }

static void open_row(ak_tbl *t, int64_t i) {
	if (i < 0 || i >= ak_list_items_count(t->st)) return;
	if (!t->outline) {
		ak_val arg = {.i = i};
		ak_view_call(t->e, AK_EV_TABLE_OPEN, &arg, 1);
		return;
	}
	ak_str *id = row_id(t, i);
	if (!id) return;
	ak_val arg = {.p = ak_retain(id)};
	ak_view_call(t->e, AK_EV_OUTLINE_OPEN, &arg, 1);
	ak_release(arg.p);
}

// expand_row expands or collapses row i, as the user did: what's
// expanded with it, or without it.
static void expand_row(ak_tbl *t, int64_t i, bool on) {
	ak_str *id = row_id(t, i);
	if (!id) return;
	ak_retain(id);
	const ak_arr *open = expanded_now(t);
	ak_arr *next = str_array(open ? open->len + 1 : 1);
	bool had = false;
	for (uint32_t k = 0; open && k < open->len; k++) {
		ak_str *s = ak_arr_vals(open)[k].p;
		if (s && ak_str_eq(s, id)) {
			had = true;
			if (!on) continue;
		}
		ak_arr_push(&next, (ak_val){.p = ak_retain(s)});
	}
	if (on && !had) ak_arr_push(&next, (ak_val){.p = ak_retain(id)});
	ak_release(t->own_expanded);
	t->own_expanded = ak_retain(next);
	t->levels_known = false;  // sent again: the host shows what the app keeps
	ak_signal_write(&t->sig);
	ak_val args[2] = {{.p = id}, {.i = on}};
	ak_view_call(t->e, AK_EV_OUTLINE_EXPAND, args, 2);
	ak_val arg = {.p = next};
	if (!t->e->dead) ak_view_call(t->e, AK_EV_OUTLINE_EXPANDED_CHANGE, &arg, 1);
	ak_release(next);
	ak_release(id);
}

// sort_by hears a click on column i's header: that column first,
// ascending, or the other way when it's first already; the others after
// it as they were.
static void sort_by(ak_tbl *t, int64_t i) {
	const ak_struct *col = i >= 0 && i < UINT32_MAX ? col_at(t, (uint32_t)i) : NULL;
	if (!col || !col->f[COL_SORTABLE].i) return;
	ak_str *id = col->f[COL_ID].p;
	const ak_arr *cur = sort_now(t);
	ak_arr *next = ak_arr_new(AK_K_REF, &shape_sort_key, 4);
	ak_struct *first = ak_struct_new(&shape_sort_key);
	first->f[0].p = ak_retain(id);
	const ak_struct *was = cur && cur->len ? ak_arr_vals(cur)[0].p : NULL;
	first->f[1].i = was && ak_str_eq(was->f[0].p, id) ? !was->f[1].i : 1;
	ak_arr_push(&next, (ak_val){.p = first});
	for (uint32_t k = 0; cur && k < cur->len && next->len < 3; k++) {
		const ak_struct *s = ak_arr_vals(cur)[k].p;
		if (ak_str_eq(s->f[0].p, id)) continue;
		ak_arr_push(&next, (ak_val){.p = ak_retain((void *)s)});
	}
	ak_release(t->own_sort);
	t->own_sort = ak_retain(next);
	ak_val arg = {.p = next};
	ak_view_call(t->e, AK_EV_TABLE_SORT_CHANGE, &arg, 1);
	ak_release(next);
	// The host shows the sort the app has: sent again whatever it is.
	t->sort_known = false;
	if (!t->e->dead) send_sort(t);
}

// columns_laid hears ColumnLayout: the columns as shown, which the
// rows' views are placed at; a user's resize or move is reported.
static void columns_laid(ak_tbl *t, ak_span s) {
	ak_reader r = {(const uint8_t *)s.p, s.len, 0, false};
	uint32_t n = ak_read_u32(&r), nc = ncols(t);
	u32_vec shown = {0};
	AK_VEC(float) widths = {0};
	for (uint32_t i = 0; i < n && !r.err; i++) {
		uint32_t at = ak_read_u32(&r);
		float w = ak_read_f32(&r);
		if (r.err || at >= nc || !(w >= 0)) continue;
		ak_vec_push(&shown, at);
		ak_vec_push(&widths, w);
	}
	if (r.err) {
		ak_vec_free(&shown);
		ak_vec_free(&widths);
		return;
	}
	bool was = t->laid;
	u32_vec old = t->shown;
	struct {
		float *data;
		uint32_t len;
	} old_w = {t->shown_w.data, t->shown_w.len};
	t->shown = shown;
	t->shown_w.data = widths.data, t->shown_w.len = widths.len, t->shown_w.cap = widths.cap;
	t->laid = true;
	tcells_relayout(t);
	if (was) {
		// Resizes: each column whose width changed.
		for (uint32_t i = 0; i < t->shown.len && !t->e->dead; i++) {
			for (uint32_t k = 0; k < old.len; k++) {
				if (old.data[k] != t->shown.data[i] || old_w.data[k] == t->shown_w.data[i]) continue;
				const ak_struct *col = col_at(t, t->shown.data[i]);
				if (!col) break;
				ak_val args[2] = {{.p = col->f[COL_ID].p}, {.f = t->shown_w.data[i]}};
				ak_view_call(t->e, AK_EV_TABLE_COLUMN_RESIZE, args, 2);
				break;
			}
		}
		// A move: the column that went furthest.
		int64_t moved = -1, far = 0;
		for (uint32_t i = 0; i < t->shown.len; i++)
			for (uint32_t k = 0; k < old.len; k++)
				if (old.data[k] == t->shown.data[i]) {
					int64_t d = (int64_t)i - (int64_t)k;
					if (d < 0) d = -d;
					if (d > far) far = d, moved = i;
				}
		const ak_struct *col = moved >= 0 ? col_at(t, t->shown.data[moved]) : NULL;
		if (col && !t->e->dead) {
			ak_val args[2] = {{.p = col->f[COL_ID].p}, {.i = moved}};
			ak_view_call(t->e, AK_EV_TABLE_COLUMN_MOVE, args, 2);
		}
	}
	ak_vec_free(&old);
	ak_free(old_w.data);
}

// A phone's row pressed: an outline's that expands, without opening,
// expands or collapses; else it's selected and opened.
static void phone_pressed(ak_view *row) {
	lcell *c = cell_of(row);
	if (!c) return;
	ak_tbl *t = c->st->tbl;
	int64_t i = ak_lcell_index(c, false);
	ak_retain(t->e);
	if (t->outline) {
		uint32_t depth;
		uint8_t flags;
		row_level(t, i, &depth, &flags);
		if ((flags & 1) && !ak_view_handler(t->e, AK_EV_OUTLINE_OPEN)) {
			expand_row(t, i, !(flags & 2));
			ak_release(t->e);
			return;
		}
	}
	u32_vec one = {0};
	ak_vec_push(&one, (uint32_t)i);
	ak_buf b = {0};
	encode_runs(&b, &one);
	ak_vec_free(&one);
	user_selected(t, (ak_span){(const char *)b.data, (uint32_t)b.len});
	ak_buf_free(&b);
	if (!t->e->dead) open_row(t, i);
	ak_release(t->e);
}

// ---- The class ----

static float platform_row_height(uint8_t p) {
	switch (p) {
	case AK_PLATFORM_WINDOWS:
	case AK_PLATFORM_WEB:
		return 32;
	case AK_PLATFORM_LINUX: return 34;
	}
	return 24;
}

static void row_height_run(void *ctx) {
	ak_view *e = ctx;
	if (e->dead) return;
	ak_tbl *t = ((list_t *)e->impl)->tbl;
	ak_signal_read(&e->r->env_node);
	float own = ak_style_float(e, t->outline ? AK_KEY_OUTLINE_ROW_HEIGHT : AK_KEY_TABLE_ROW_HEIGHT, 0);
	float h = own > 0 ? own : platform_row_height(e->r->env.platform);
	if (h == t->row_h) return;
	t->row_h = h;
	ak_batch_set_float(e->r->batch, e->id, t->outline ? AK_PROP_OUTLINE_ROW_HEIGHT : AK_PROP_TABLE_ROW_HEIGHT, h);
	tcells_relayout(t);
}

static const ak_comp_info row_height_info = {.drop = release_ctx, .label = "Table.rowHeight"};

static void tbl_init(ak_view *e) {
	ak_list_init(e);
	list_t *st = e->impl;
	ak_tbl *t = ak_alloc(sizeof *t);
	t->st = st;
	t->e = e;
	t->outline = e->el == AK_EL_OUTLINE;
	t->phone = e->kind == AK_KIND_LIST;
	ak_signal_init(&t->sig);
	ak_signal_init(&t->lv);
	st->tbl = t;
}

static void tbl_drop(ak_view *e) {
	list_t *st = e->impl;
	ak_tbl *t = st ? st->tbl : NULL;
	if (t) {
		ak_signal_drop(&t->sig);
		ak_signal_drop(&t->lv);
		ak_vec_free(&t->shown);
		ak_vec_free(&t->shown_w);
		ak_buf_free(&t->cols_sent);
		ak_buf_free(&t->sort_sent);
		ak_buf_free(&t->sel_sent);
		ak_vec_free(&t->sel);
		ak_release(t->own_sort);
		ak_release(t->own_expanded);
		ak_release(t->own_sel);
		ak_vec_free(&t->rows);
		ak_vec_free(&t->next_rows);
		ak_buf_free(&t->levels);
		ak_buf_free(&t->next_levels);
		ak_buf_free(&t->levels_sent);
		ak_free(t->row_set.slot);
		ak_free(t->open_set.slot);
		ak_free(t);
		st->tbl = NULL;
	}
	ak_list_drop(e);
}

static void tbl_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(prev);
	ak_tbl *t = ((list_t *)e->impl)->tbl;
	ak_batch *b = e->r->batch;
	switch (key) {
	case AK_KEY_TABLE_COLUMNS: {
		if (t->phone) break;
		// What the host shows stays while it's of these columns.
		uint32_t n = ncols(t);
		for (uint32_t i = 0; i < t->shown.len; i++)
			if (t->shown.data[i] >= n) t->laid = false;
		if (e->finalized) send_columns(t), send_sort(t);
		tcells_relayout(t);
		break;
	}
	case AK_KEY_TABLE_SORT:
		if (e->finalized) send_sort(t);
		break;
	case AK_KEY_TABLE_SELECTION: {
		const ak_arr *rows = v.p;
		t->sel.len = 0;
		for (uint32_t i = 0; rows && i < rows->len; i++) {
			int64_t r = ak_arr_get(rows, i).i;
			if (r >= 0 && r < UINT32_MAX) ak_vec_push(&t->sel, (uint32_t)r);
		}
		sort_unique(&t->sel);
		if (e->finalized) send_selection(t);
		break;
	}
	case AK_KEY_OUTLINE_SELECTION:
		outline_selection(t);
		if (e->finalized) send_selection(t);
		break;
	case AK_KEY_TABLE_SELECTION_MODE:
	case AK_KEY_OUTLINE_SELECTION_MODE:
		if (!t->phone) ak_batch_set_enum(b, e->id, t->outline ? AK_PROP_OUTLINE_SELECTION_MODE : AK_PROP_TABLE_SELECTION_MODE, (int32_t)v.i);
		break;
	case AK_KEY_TABLE_COLUMNS_MOVABLE:
		if (!t->phone && (prev || v.i)) ak_batch_set_bool(b, e->id, AK_PROP_TABLE_COLUMNS_MOVABLE, v.i != 0);
		break;
	case AK_KEY_TABLE_ROW_HEIGHT:
	case AK_KEY_OUTLINE_ROW_HEIGHT:
		if (e->finalized && !t->phone) row_height_run(e);
		break;
	case AK_KEY_OUTLINE_ROOTS:
	case AK_KEY_OUTLINE_CHILDREN_OF:
	case AK_KEY_OUTLINE_EXPANDABLE:
	case AK_KEY_OUTLINE_EXPANDED:
		ak_signal_write(&t->sig);  // shown again
		break;
	}
}

static void tbl_start(void *ctx) {
	ak_view *e = ctx;
	ak_tbl *t = ((list_t *)e->impl)->tbl;
	if (!t->phone) {
		if (!t->outline) {
			send_columns(t);
			send_sort(t);
			ak_view_listen(e, AK_EVENT_SORT_BY);
			ak_view_listen(e, AK_EVENT_COLUMN_LAYOUT);
		} else {
			ak_view_listen(e, AK_EVENT_EXPAND);
		}
		ak_view_listen(e, AK_EVENT_ROWS_SELECTED);
		ak_view_listen(e, AK_EVENT_OPEN);
		send_selection(t);
		ak_effect_new(row_height_run, ak_retain(e), AK_EFFECT_RENDER, &row_height_info);
	}
	if (t->outline) {
		// Its rows: the tree's, flattened, keyed by their IDs.
		ak_closure *items = ak_closure_new(outline_items, t, 0, NULL), *key = ak_closure_new(outline_key, NULL, 0, NULL),
		           *row = ak_closure_new(outline_row, t, 0, NULL);
		ak_list_start(e, items, key, NULL, (ak_val){0}, row, AK_K_REF);
		ak_release(items), ak_release(key), ak_release(row);
	}
}

// tbl_finalize starts it under the owner it was made under (its rows',
// its effects'), whenever it's finalized.
static void tbl_finalize(ak_view *e) {
	if (e->owner && !ak_owner_disposed(e->owner)) ak_with_owner(e->owner, tbl_start, e);
	else tbl_start(e);
}

static void tbl_event(ak_view *e, const ak_event *ev) {
	ak_tbl *t = ((list_t *)e->impl)->tbl;
	switch (ev->kind) {
	case AK_EVENT_ROWS_SELECTED: user_selected(t, ev->s); break;
	case AK_EVENT_SORT_BY: sort_by(t, ev->i); break;
	case AK_EVENT_COLUMN_LAYOUT: columns_laid(t, ev->s); break;
	case AK_EVENT_OPEN: open_row(t, ev->i); break;
	case AK_EVENT_EXPAND:
		if (ev->x >= 0 && ev->x < 4294967296.0) expand_row(t, (int64_t)ev->x, ev->y != 0);
		break;
	default: ak_list_event(e, ev); break;
	}
}

static void tbl_add(ak_view *e, ak_view *child) {
	AK_UNUSED(child);
	ak_panic("ui: a %s's rows come from %s", ak_el_names[e->el], e->el == AK_EL_TABLE ? "a for in it" : "its roots and children");
}

static const ak_el_class cls_table = {.name = "Table", .kind = AK_KIND_TABLE, .flags = AK_ELF_CHILDREN, .init = tbl_init, .set = tbl_set,
                                      .finalize = tbl_finalize, .event = tbl_event, .adjust = ak_list_adjust, .add = tbl_add, .drop = tbl_drop};
static const ak_el_class cls_outline = {.name = "Outline", .kind = AK_KIND_OUTLINE, .flags = AK_ELF_CHILDREN, .init = tbl_init, .set = tbl_set,
                                        .finalize = tbl_finalize, .event = tbl_event, .adjust = ak_list_adjust, .add = tbl_add, .drop = tbl_drop};
// A phone's: its List.
static const ak_el_class cls_table_list = {.name = "Table", .kind = AK_KIND_LIST, .flags = AK_ELF_CHILDREN, .init = tbl_init, .set = tbl_set,
                                           .finalize = tbl_finalize, .event = tbl_event, .adjust = ak_list_adjust, .add = tbl_add, .drop = tbl_drop};
static const ak_el_class cls_outline_list = {.name = "Outline", .kind = AK_KIND_LIST, .flags = AK_ELF_CHILDREN, .init = tbl_init, .set = tbl_set,
                                             .finalize = tbl_finalize, .event = tbl_event, .adjust = ak_list_adjust, .add = tbl_add, .drop = tbl_drop};

const ak_el_class *ak_tbl_class_of(uint16_t el) {
	bool phone = is_phone(ak_ui_current());
	switch (el) {
	case AK_EL_TABLE: return phone ? &cls_table_list : &cls_table;
	case AK_EL_OUTLINE: return phone ? &cls_outline_list : &cls_outline;
	case AK_EL_TABLE_ROW: return phone ? &ak_class_trow_phone : &ak_class_trow;
	}
	return NULL;
}

bool ak_listlike(const ak_view *e) {
	return e && (e->el == AK_EL_LIST || e->el == AK_EL_TABLE || e->el == AK_EL_OUTLINE) && e->impl;
}

// ---- list.c's hooks ----

// run_menu runs item n of cell c's row's context menu.
static void run_menu(lcell *c, int64_t n) {
	const ak_enum *a = c->actions ? ak_find_action(c->actions, n) : NULL;
	if (!a || ak_enum_case(a) != 0 || a->f[3].i || !a->f[5].p) return;
	ak_closure *run = ak_retain(a->f[5].p);
	ak_val r = ak_ui_call_under(c->row.owner, run, NULL, 0);
	if (!ak_error_pending()) ak_release(r.p);
	else ak_ui_raise_pending(c->row.owner);
	ak_release(run);
}

// The view a row's menu goes on: the cell (the host's table shows it),
// or a phone's row.
static ak_view *menu_target(lcell *c) {
	if (!c->st->tbl->phone) return c->el;
	ak_view *v = row_of(c);
	return v && v->real ? v : NULL;
}

static void menu_run(void *ctx) {
	lcell *c = ctx;
	if (c->el->dead) return;
	ak_tbl *t = c->st->tbl;
	ak_closure *f = closure_key(t->e, t->outline ? AK_KEY_OUTLINE_MENU : AK_KEY_TABLE_MENU);
	ak_view *to = menu_target(c);
	if (!f || !to) return;
	ak_val arg = {0};
	if (t->outline) arg = ak_vm_cell_get(c->row.cell_item, true);
	else arg.i = ak_lcell_index(c, true);
	ak_val v = ak_call(f, &arg, 1);
	if (t->outline) ak_release(arg.p);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	ak_buf b = {0};
	ak_encode_actions(&b, v.p);
	bool same = c->has_sent && c->sent.len == b.len && memcmp(c->sent.data, b.data, b.len) == 0;
	ak_release(c->actions);
	c->actions = v.p;
	if (!same) {
		ak_buf_free(&c->sent);
		c->sent = b;
		c->has_sent = true;
		ak_batch_set_bytes(to->r->batch, to->id, AK_PROP_MENU, c->sent.data, c->sent.len);
		ak_batch_set_enum(to->r->batch, to->id, AK_PROP_MENU_TRIGGER, AK_MENU_TRIGGER_LONG_PRESS);
		ak_view_listen(to, AK_EVENT_ACTION);
	} else {
		ak_buf_free(&b);
	}
}

static const ak_comp_info menu_info = {.label = "Table.menu"};

void ak_tbl_cell_effects(lcell *c) {
	ak_tbl *t = c->st->tbl;
	if (closure_key(t->e, t->outline ? AK_KEY_OUTLINE_MENU : AK_KEY_TABLE_MENU)) ak_effect_new(menu_run, c, AK_EFFECT_RENDER, &menu_info);
}

void ak_tbl_bound(list_t *st, lcell *c) {
	ak_view *v = row_of(c);
	if (!v || v->cls != &ak_class_trow) return;
	trow *tr = v->impl;
	if (!tr->t) {
		// Its first cell: its views' boxes go at their columns, which they
		// couldn't know before.
		tr->t = st->tbl;
		for (uint32_t k = 0; k < v->kids.len; k++)
			if (v->kids.data[k]->cls == &cls_tcell) ak_style_relayout(v->kids.data[k]);
	}
	trow_send(v);
}

bool ak_tbl_cell_event(lcell *c, const ak_event *ev) {
	if (ev->kind != AK_EVENT_ACTION) return false;
	run_menu(c, ev->i);
	return true;
}

void ak_tbl_unbind(lcell *c) { AK_UNUSED(c); }

float ak_tbl_row_height(const list_t *st) { return st->tbl->phone ? 0 : st->tbl->row_h; }

float ak_tbl_cell_width(const list_t *st, lcell *c, float w) {
	const ak_tbl *t = st->tbl;
	if (t->phone) return w;
	if (!t->outline) {
		float cw = columns_width(t);
		return cw > w ? cw : w;
	}
	uint32_t depth;
	uint8_t flags;
	row_level(t, ak_lcell_index(c, false), &depth, &flags);
	float in = w - OUTLINE_DISCLOSURE - OUTLINE_INDENT * (float)depth;
	return in > 1 ? in : 1;
}

uint16_t ak_tbl_end_event(const list_t *st) { return st->tbl->outline ? 0 : AK_EV_TABLE_END_REACHED; }

