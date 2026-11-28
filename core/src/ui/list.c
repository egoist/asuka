// list.c: List: a for backed by the platform's recycling
// list (UICollectionView, RecyclerView). Rows are made only for the
// cells on screen, when the host binds them (ListBind, from its data
// source, on the main thread); a recycled cell rebinds its row to another
// item, which updates only the props that differ.
//
// Changes to the items send ListUpdate: an ID per item (stable while its
// key lasts), which the host diffs. Bound rows whose item changed are
// updated in place. Row heights come from the core's layout of each row
// at the list's width (a grid's column's): a cell's SetFrame carries its
// row's height.
//
// Sections: rows next to each other with the same section key (the
// list's section(index)) are one, headed by header(key)'s view, a row of
// its own that the host binds at an index of -1 - the section's; their
// IDs go with ListUpdate. Swipe actions (swipeActions(index),
// leadingSwipeActions(index)) and whether a row is selected
// (selected(index)) are followed in each bound row's effects; the host's
// Action, RowSelected and RowSelectBegin come to its cell.
#include "ui/ui_int.h"
#include "ui/ui_ext.h"

// A bound cell: the host's view, and the row built in it.
typedef struct lcell {
	ak_view *el;  // retained
	ak_row row;   // its key, cells, owner (its view is el's kid); a header's: its section's key's cell
	float width;  // as last laid out
	bool header;  // a section's header
	int8_t sel;   // 1: the host shows it selected; 0 not; -1 unknown
	ak_arr *actions, *leading;  // its swipe actions, as last sent (retained)
	ak_buf sent, sent_l;
	bool has_sent, has_sent_l;
	struct list_t *st;
} lcell;

// A run of rows with the same section key.
typedef struct {
	uint32_t start;
	ak_str *key;  // retained
	int64_t id;
} lsection;

// A key's sections' IDs, in the order they come.
typedef struct {
	ak_str *key;  // retained
	AK_VEC(int64_t) ids;
} lsecids;
typedef AK_VEC(lsecids) lsecids_vec;

// An item's ID, by key.
typedef struct {
	ak_val key;
	uint32_t hash;  // 0: empty
	int64_t id;
	uint32_t index;
} lid;

typedef struct list_t {
	ak_view *e;  // the List
	ak_closure *items, *key, *row;
	ak_kind key_kind;
	ak_owner *owner;   // what the List was made under: its rows' owners' parent
	ak_arr *list;      // the items, retained
	lid *ids, *next_ids;
	uint32_t ids_cap, next_cap;
	int64_t next;      // the last ID made
	AK_VEC(lcell *) cells;  // bound, in the order they were
	AK_VEC(int64_t) order;  // the items' IDs, in order (scratch)
	// What the last ListUpdate said: the IDs, and the sections' starts
	// and IDs (sections: whether it had them). One that would say the
	// same isn't sent (a change inside items, a filter that keeps them):
	// a host diffing 10,000 IDs for nothing costs it as much as the core.
	AK_VEC(int64_t) sent_ids, sent_secs;
	bool sent, sent_sections;
	int64_t end_at;    // the length the end was last reached at
	ak_val start_key;  // the first item the start was last reached at
	bool started;
	bool refresh_shown;
	float inset;  // an inset grouped list's, each side
	int columns;
	ak_path *path;
	ak_comp_info info;
	AK_VEC(lsection) sections;
	lsecids_vec secids, next_secids;
	float row_lead, row_trail;  // the room the host's selection control takes from the rows
	// Over a path (rfcs/0013): its items are src's, and it holds no
	// array (one held would be copied by each change in place of the
	// state): n is their number, and keys their keys, computed as its
	// effect runs (what a key's function reads followed).
	bool over_path;
	ak_fsrc src;
	ak_effect *eff;  // its effect: what its rows' items follow (borrowed)
	int64_t n;
	ak_kind ik;
	AK_VEC(ak_val) keys;
	// Moving rows (on move): the Reorder prop as last sent.
	ak_buf reorder_sent;
	bool reorder_known;
	bool has_sections;  // the last update's items had sections
} list_t;

// ---- The class ----

static void list_adjust(ak_view *e, ak_lstyle *ls) {
	// A List fills the space it's given: with no height and no grow, it
	// grows.
	AK_UNUSED(e);
	if (ak_llength_is_auto(ls->height) && ls->flex_grow == 0) ls->flex_grow = 1;
}

static void list_init(ak_view *e) {
	list_t *st = ak_alloc(sizeof *st);
	e->impl = st;
	st->e = e;
	st->end_at = -1;
	st->columns = 1;
	ak_vec_push(&e->r->lists, (ak_view *)ak_retain(e));
	ak_style_relayout(e);  // its adjust
}

static void free_ids(lid *t, uint32_t cap, ak_kind k) {
	for (uint32_t i = 0; t && i < cap; i++)
		if (t[i].hash) ak_release_val(t[i].key, k);
}

static void free_sections(list_t *st) {
	for (uint32_t i = 0; i < st->sections.len; i++) ak_release(st->sections.data[i].key);
	st->sections.len = 0;
}

static void free_secids(lsecids_vec *s) {
	for (uint32_t i = 0; i < s->len; i++) {
		ak_release(s->data[i].key);
		ak_vec_free(&s->data[i].ids);
	}
	s->len = 0;
}

static void list_drop(ak_view *e) {
	list_t *st = e->impl;
	if (!st) return;
	free_sections(st);
	ak_vec_free(&st->sections);
	free_secids(&st->secids);
	free_secids(&st->next_secids);
	ak_vec_free(&st->secids);
	ak_vec_free(&st->next_secids);
	ak_buf_free(&st->reorder_sent);
	ak_free(st);
}

static int64_t key_int(const ak_view *e, uint16_t key, int64_t def) {
	const ak_val *v = ak_style_get(e, key);
	return v ? v->i : def;
}

// ---- Moving rows ----

static void release_ctx(void *ctx);

// The screen readers' actions that move a row, by language: what a
// platform doesn't name for an app (UIKit's and Android's lists have no
// such action of their own).
static const struct {
	const char *lang, *up, *down;
} move_names[] = {
	{"en", "Move up", "Move down"},
	{"ar", "\u0646\u0642\u0644 \u0644\u0623\u0639\u0644\u0649", "\u0646\u0642\u0644 \u0644\u0623\u0633\u0641\u0644"},
	{"da", "Flyt op", "Flyt ned"},
	{"de", "Nach oben", "Nach unten"},
	{"es", "Subir", "Bajar"},
	{"fi", "Siirr\u00e4 yl\u00f6s", "Siirr\u00e4 alas"},
	{"fr", "D\u00e9placer vers le haut", "D\u00e9placer vers le bas"},
	{"he", "\u05d4\u05d6\u05d6 \u05dc\u05de\u05e2\u05dc\u05d4", "\u05d4\u05d6\u05d6 \u05dc\u05de\u05d8\u05d4"},
	{"hi", "\u090a\u092a\u0930 \u0932\u0947 \u091c\u093e\u090f\u0901", "\u0928\u0940\u091a\u0947 \u0932\u0947 \u091c\u093e\u090f\u0901"},
	{"id", "Pindahkan ke atas", "Pindahkan ke bawah"},
	{"it", "Sposta su", "Sposta gi\u00f9"},
	{"ja", "\u4e0a\u306b\u79fb\u52d5", "\u4e0b\u306b\u79fb\u52d5"},
	{"ko", "\uc704\ub85c \uc774\ub3d9", "\uc544\ub798\ub85c \uc774\ub3d9"},
	{"nb", "Flytt opp", "Flytt ned"},
	{"nl", "Omhoog", "Omlaag"},
	{"pl", "Przenie\u015b w g\u00f3r\u0119", "Przenie\u015b w d\u00f3\u0142"},
	{"pt", "Mover para cima", "Mover para baixo"},
	{"ru", "\u041f\u0435\u0440\u0435\u043c\u0435\u0441\u0442\u0438\u0442\u044c \u0432\u0432\u0435\u0440\u0445", "\u041f\u0435\u0440\u0435\u043c\u0435\u0441\u0442\u0438\u0442\u044c \u0432\u043d\u0438\u0437"},
	{"sv", "Flytta upp", "Flytta ned"},
	{"th", "\u0e22\u0e49\u0e32\u0e22\u0e02\u0e36\u0e49\u0e19", "\u0e22\u0e49\u0e32\u0e22\u0e25\u0e07"},
	{"tr", "Yukar\u0131 ta\u015f\u0131", "A\u015fa\u011f\u0131 ta\u015f\u0131"},
	{"uk", "\u041f\u0435\u0440\u0435\u043c\u0456\u0441\u0442\u0438\u0442\u0438 \u0432\u0433\u043e\u0440\u0443", "\u041f\u0435\u0440\u0435\u043c\u0456\u0441\u0442\u0438\u0442\u0438 \u0432\u043d\u0438\u0437"},
	{"vi", "Di chuy\u1ec3n l\u00ean", "Di chuy\u1ec3n xu\u1ed1ng"},
	{"zh", "\u4e0a\u79fb", "\u4e0b\u79fb"},
};

// move_names_of are the move actions' names in language tag's language
// (its first subtag's), else English's.
static uint32_t move_names_of(const ak_str *tag) {
	uint32_t n = 0;
	while (tag && n < tag->len && tag->data[n] != '-' && tag->data[n] != '_') n++;
	for (uint32_t i = 0; i < AK_LEN(move_names); i++) {
		const char *l = move_names[i].lang;
		if (strlen(l) != n) continue;
		bool same = true;
		for (uint32_t j = 0; j < n && same; j++) same = (char)(tag->data[j] | 0x20) == l[j];
		if (same) return i;
	}
	return 0;
}

static bool reorder_on(const ak_view *e) {
	const ak_val *v = ak_style_get(e, AK_KEY_LIST_REORDERABLE);
	return ak_view_handler((ak_view *)e, AK_EV_LIST_MOVE) && (!v || v->i);
}

// reorder_send sends whether the rows move, with the move actions' names
// in the app's language (read tracked: a reorder effect follows it), if
// that changed.
static void reorder_send(ak_view *e) {
	list_t *st = e->impl;
	ak_buf b = {0};
	if (reorder_on(e)) {
		ak_str *tag = ak_ui_locale();
		uint32_t i = move_names_of(tag);
		ak_release(tag);
		ak_buf_u32(&b, 2);
		ak_buf_str(&b, move_names[i].up, strlen(move_names[i].up));
		ak_buf_str(&b, move_names[i].down, strlen(move_names[i].down));
	}
	bool same = st->reorder_sent.len == b.len && (!b.len || memcmp(st->reorder_sent.data, b.data, b.len) == 0);
	if (!(same && (st->reorder_known || !b.len))) {
		ak_batch_set_bytes(e->r->batch, e->id, AK_PROP_LIST_REORDER, b.data, b.len);
		st->reorder_sent.len = 0;
		ak_buf_put(&st->reorder_sent, b.data, b.len);
		st->reorder_known = true;
	}
	ak_buf_free(&b);
}

static void reorder_run(void *ctx) {
	ak_view *e = ctx;
	if (!e->dead) reorder_send(e);
}

static const ak_comp_info reorder_info = {.drop = release_ctx, .label = "List.move"};

static void list_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	list_t *st = e->impl;
	ak_batch *b = e->r->batch;
	switch (key) {
	case AK_KEY_LIST_REFRESHING: ak_refresh_set(e, &st->refresh_shown, AK_PROP_LIST_REFRESHING, v); break;
	case AK_KEY_LIST_COLUMNS:
		st->columns = v.i > 1 ? (int)v.i : 1;
		if (e->finalized && (prev || v.i > 1)) ak_batch_set_int(b, e->id, AK_PROP_LIST_COLUMNS, v.i);
		break;
	case AK_KEY_LIST_APPEARANCE:
	case AK_KEY_LIST_ANCHOR:
		if (e->finalized) ak_batch_set_enum(b, e->id, key == AK_KEY_LIST_ANCHOR ? AK_PROP_LIST_ANCHOR : AK_PROP_LIST_APPEARANCE, (int32_t)v.i);
		break;
	case AK_KEY_LIST_EDGE_EFFECT:
		if (prev || v.i) ak_batch_set_enum(b, e->id, AK_PROP_LIST_EDGE_EFFECT, (int32_t)v.i);
		break;
	case AK_KEY_LIST_REORDERABLE:
		if (e->finalized) reorder_send(e);
		break;
	case AK_KEY_LIST_SELECTING:
		if (prev || v.i) ak_batch_set_bool(b, e->id, AK_PROP_LIST_SELECTING, v.i != 0);
		ak_view_listen(e, AK_EVENT_ROW_INSET);
		break;
	}
}

static void list_styled(ak_view *e, uint16_t key) {
	list_t *st = e->impl;
	if (key == AK_KEY_GAP && e->finalized && st->columns > 1) ak_batch_set_float(e->r->batch, e->id, AK_PROP_LIST_GAP, ak_style_float(e, AK_KEY_GAP, 0));
}

// list_inset is an inset grouped list's rows' inset from each side: the
// platform's margins (UIKit's layout margins, Material's window margins,
// System Settings' grouped forms).
static float list_inset(const ak_environment *env) {
	bool wide = env->width_class != AK_SIZE_CLASS_COMPACT;
	switch (env->platform) {
	case AK_PLATFORM_IOS:
		if (wide) return 20;
		break;
	case AK_PLATFORM_ANDROID:
		if (wide) return 24;
		break;
	case AK_PLATFORM_MACOS:
		return 20;
	}
	return 16;
}

static void inset_run(void *ctx) {
	ak_view *e = ctx;
	list_t *st = e->impl;
	ak_signal_read(&e->r->env_node);
	float in = list_inset(&e->r->env);
	if (in == st->inset) return;
	st->inset = in;  // the rows are laid out again at the new width
	ak_batch_set_float(e->r->batch, e->id, AK_PROP_LIST_INSET, in);
}

static void release_ctx(void *ctx) { ak_release(ctx); }

static int64_t count(const list_t *st);
static bool same_as_sent(list_t *st, bool sections);
static void send_order(list_t *st, bool sections);

// moved hears the user's move of a row, which the host made already: the
// IDs it has are in the new order, and the app's handler moves the
// items, whose update then sends nothing. If they don't follow (their
// effect isn't to run), the host is sent the order they have back.
static void moved(ak_view *e, int64_t from, int64_t to) {
	list_t *st = e->impl;
	int64_t n = count(st);
	if (!reorder_on(e) || from < 0 || to < 0 || from >= n || to >= n || from == to) return;
	if (st->sent && !st->sent_sections && st->sent_ids.len == (uint64_t)n) {
		// What the host shows now: the ID at from, at to.
		int64_t *ids = st->sent_ids.data, id = ids[from];
		if (from < to) memmove(ids + from, ids + from + 1, (size_t)(to - from) * sizeof(int64_t));
		else memmove(ids + to + 1, ids + to, (size_t)(from - to) * sizeof(int64_t));
		ids[to] = id;
	} else {
		st->sent = false;  // its sections are the host's own now: sent whole
	}
	ak_retain(e);
	ak_val args[2] = {{.i = from}, {.i = to}};
	ak_view_call(e, AK_EV_LIST_MOVE, args, 2);
	if (!e->dead && e->impl == st && st->eff && !ak_effect_stale(st->eff) && !same_as_sent(st, st->has_sections)) send_order(st, st->has_sections);
	ak_release(e);
}

static const ak_comp_info inset_info = {.drop = release_ctx, .label = "List.inset"};

static void list_finalize(ak_view *e) {
	list_t *st = e->impl;
	ak_batch *b = e->r->batch;
	int64_t appearance = key_int(e, AK_KEY_LIST_APPEARANCE, 0), anchor = key_int(e, AK_KEY_LIST_ANCHOR, 0);
	if (st->columns > 1) {
		// A grid: plain, read from its top (Android's grid can't stack
		// from its end).
		ak_batch_set_int(b, e->id, AK_PROP_LIST_COLUMNS, st->columns);
		float gap = ak_style_float(e, AK_KEY_GAP, 0);
		if (gap > 0) ak_batch_set_float(b, e->id, AK_PROP_LIST_GAP, gap);
		appearance = anchor = 0;
	}
	if (anchor != AK_SCROLL_ANCHOR_TOP) ak_batch_set_enum(b, e->id, AK_PROP_LIST_ANCHOR, (int32_t)anchor);
	if (appearance != AK_LIST_APPEARANCE_PLAIN) ak_batch_set_enum(b, e->id, AK_PROP_LIST_APPEARANCE, (int32_t)appearance);
	if (appearance == AK_LIST_APPEARANCE_INSET_GROUPED) {
		ak_effect_new(inset_run, ak_retain(e), AK_EFFECT_RENDER, &inset_info);
	}
	if (ak_view_handler(e, AK_EV_LIST_REFRESH)) ak_batch_set_bool(b, e->id, AK_PROP_LIST_REFRESHABLE, true);
	if (ak_view_handler(e, AK_EV_LIST_MOVE)) {
		ak_view_listen(e, AK_EVENT_MOVED);
		ak_effect_new(reorder_run, ak_retain(e), AK_EFFECT_RENDER, &reorder_info);
	}
}

static void list_event(ak_view *e, const ak_event *ev) {
	list_t *st = e->impl;
	if (ev->kind == AK_EVENT_ROW_INSET) {
		// The rows are laid out again at the width they have.
		st->row_lead = ev->x > 0 ? (float)ev->x : 0;
		st->row_trail = ev->y > 0 ? (float)ev->y : 0;
		for (uint32_t i = 0; i < st->cells.len; i++) st->cells.data[i]->width = -1;
		return;
	}
	if (ev->kind == AK_EVENT_MOVED) {
		// Indices, as doubles: what isn't one (a host's bad event) moves nothing.
		bool ok = ev->x >= 0 && ev->y >= 0 && ev->x < 4294967296.0 && ev->y < 4294967296.0;
		if (ok) moved(e, (int64_t)ev->x, (int64_t)ev->y);
	} else if (ev->kind == AK_EVENT_SCROLLED) {
		ak_val args[2] = {{.f = (float)ev->x}, {.f = (float)ev->y}};
		ak_view_call(e, AK_EV_LIST_SCROLL, args, 2);
	} else if (ev->kind == AK_EVENT_REFRESH) {
		ak_refresh_event(e, &st->refresh_shown, AK_PROP_LIST_REFRESHING, AK_KEY_LIST_REFRESHING, AK_EV_LIST_REFRESH);
	}
}

static void list_add(ak_view *e, ak_view *child) {
	AK_UNUSED(e), AK_UNUSED(child);
	ak_panic("ui: a List's rows come from a for in it");
}

const ak_el_class ak_class_list = {.name = "List", .kind = AK_KIND_LIST, .flags = AK_ELF_CHILDREN, .init = list_init, .set = list_set,
                                   .finalize = list_finalize, .event = list_event, .adjust = list_adjust, .add = list_add,
                                   .styled = list_styled, .drop = list_drop};

// ---- IDs ----

// count is the number of items.
static int64_t count(const list_t *st) {
	if (st->over_path) return st->n;
	return st->list ? st->list->len : 0;
}

// key_at is item i's key, of a list over a path: its items read now.
static ak_val key_at(list_t *st, int64_t i) {
	ak_arr *items = ak_fsrc_items(&st->src);
	if (ak_error_pending() || !items || i >= items->len) {
		ak_release(items);
		return (ak_val){.i = i};
	}
	ak_val k = ak_fsrc_key(&st->src, items, i, st->key_kind);
	ak_release(items);
	return k;
}

static ak_val key_of(list_t *st, ak_val item, int64_t i) {
	if (!st->key) return (ak_val){.i = i};
	ak_val args[2] = {item, {.i = i}};  // the key's function takes (item, index)
	return ak_call(st->key, args, 2);
}

static lid *find_id(lid *t, uint32_t cap, ak_val k, uint32_t hash, ak_kind kind) {
	for (uint32_t i = hash & (cap - 1);; i = (i + 1) & (cap - 1)) {
		if (!t[i].hash) return &t[i];
		if (t[i].hash == hash && ak_val_eq(t[i].key, k, kind)) return &t[i];
	}
}

static uint32_t hash_of(ak_val k, ak_kind kind) {
	uint32_t h = (uint32_t)ak_val_hash(k, kind);
	return h ? h : 1;
}

static ak_closure *closure_key(const ak_view *e, uint16_t key) {
	const ak_val *v = ak_style_get(e, key);
	return v ? v->p : NULL;
}

// make_sections splits the n items into runs of the same section key,
// each with an ID it keeps while its key does; a key that comes back
// after another run is told apart by which time it comes, and keeps its
// ID too. It reports whether the list has sections.
static bool make_sections(list_t *st, uint32_t n) {
	ak_closure *section = closure_key(st->e, AK_KEY_LIST_SECTION);
	if (!section || !closure_key(st->e, AK_KEY_LIST_HEADER)) return false;
	free_sections(st);
	free_secids(&st->next_secids);
	for (uint32_t i = 0; i < n; i++) {
		ak_val arg = {.i = i};
		ak_val kv = ak_call(section, &arg, 1);
		if (ak_error_pending()) {
			ak_ui_raise_pending(st->owner);
			continue;
		}
		ak_str *k = kv.p ? kv.p : ak_str_empty();
		if (st->sections.len && ak_str_eq(st->sections.data[st->sections.len - 1].key, k)) {
			ak_release(k);
			continue;
		}
		lsecids *now = NULL, *old = NULL;
		for (uint32_t j = 0; j < st->next_secids.len; j++)
			if (ak_str_eq(st->next_secids.data[j].key, k)) now = &st->next_secids.data[j];
		for (uint32_t j = 0; j < st->secids.len; j++)
			if (ak_str_eq(st->secids.data[j].key, k)) old = &st->secids.data[j];
		if (!now) {
			ak_vec_push(&st->next_secids, ((lsecids){ak_retain(k), {0}}));
			now = &st->next_secids.data[st->next_secids.len - 1];
		}
		uint32_t nth = now->ids.len;
		int64_t id = old && nth < old->ids.len ? old->ids.data[nth] : ++st->next;
		ak_vec_push(&now->ids, id);
		ak_vec_push(&st->sections, ((lsection){i, k, id}));
	}
	lsecids_vec t = st->secids;
	st->secids = st->next_secids;
	st->next_secids = t;
	return true;
}

// same_as_sent reports whether the IDs in st->order and the sections are
// what the last ListUpdate said; if not, they're kept as what's sent now.
static bool same_as_sent(list_t *st, bool sections) {
	uint32_t n = st->order.len, ns = sections ? st->sections.len * 2 : 0;
	bool same = st->sent && st->sent_sections == sections && st->sent_ids.len == n && st->sent_secs.len == ns &&
		(n == 0 || memcmp(st->sent_ids.data, st->order.data, n * sizeof(int64_t)) == 0);
	for (uint32_t i = 0; same && i < ns / 2; i++)
		same = st->sent_secs.data[2 * i] == st->sections.data[i].start && st->sent_secs.data[2 * i + 1] == st->sections.data[i].id;
	if (same) return true;
	st->sent = true;
	st->sent_sections = sections;
	st->sent_ids.len = 0;
	ak_vec_reserve(&st->sent_ids, n);
	if (n) memcpy(st->sent_ids.data, st->order.data, n * sizeof(int64_t));
	st->sent_ids.len = n;
	st->sent_secs.len = 0;
	ak_vec_reserve(&st->sent_secs, ns);
	for (uint32_t i = 0; i < ns / 2; i++) {
		st->sent_secs.data[2 * i] = st->sections.data[i].start;
		st->sent_secs.data[2 * i + 1] = st->sections.data[i].id;
	}
	st->sent_secs.len = ns;
	return false;
}

// send_order sends ListUpdate: the items' IDs in st->order, and the
// sections' starts and IDs.
static void send_order(list_t *st, bool sections) {
	ak_view *e = st->e;
	uint32_t n = st->order.len;
	ak_buf *b = ak_batch_command_begin(e->r->batch, e->id, AK_CMD_LIST_UPDATE);
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) ak_buf_u64(b, (uint64_t)st->order.data[i]);
	if (sections) {
		ak_buf_u32(b, st->sections.len);
		for (uint32_t i = 0; i < st->sections.len; i++) {
			ak_buf_u32(b, st->sections.data[i].start);
			ak_buf_u64(b, (uint64_t)st->sections.data[i].id);
		}
	}
	ak_batch_command_end(e->r->batch);
}

// update sends the new item IDs (the host diffs them), unless they're
// what it was sent last, and updates the rows that are bound.
static void update(list_t *st, ak_arr *list) {
	uint32_t n = list ? list->len : 0;
	ak_kind ik = list ? ak_arr_kind(list) : AK_K_INT;
	uint32_t need = 16;
	while (need < n * 2) need *= 2;
	if (st->next_cap < need) {
		ak_free(st->next_ids);
		st->next_ids = ak_alloc(need * sizeof(lid));
		st->next_cap = need;
	} else {
		memset(st->next_ids, 0, st->next_cap * sizeof(lid));
	}
	// The IDs first: a key that throws leaves the list as it was.
	st->order.len = 0;
	ak_vec_reserve(&st->order, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_val k;
		if (st->over_path) {
			k = st->keys.data[i];  // taken
			st->keys.data[i] = (ak_val){0};
		} else {
			k = key_of(st, ak_arr_vals(list)[i], i);
		}
		if (ak_error_pending()) {
			free_ids(st->next_ids, st->next_cap, st->key_kind);
			memset(st->next_ids, 0, st->next_cap * sizeof(lid));
			return;
		}
		uint32_t h = hash_of(k, st->key_kind);
		lid *old = st->ids ? find_id(st->ids, st->ids_cap, k, h, st->key_kind) : NULL;
		int64_t id = old && old->hash ? old->id : ++st->next;
		lid *slot = find_id(st->next_ids, st->next_cap, k, h, st->key_kind);
		if (slot->hash) {
			// A duplicate key gets its own ID.
			id = ++st->next;
			ak_release_val(k, st->key_kind);
		} else {
			*slot = (lid){k, h, id, i};
		}
		st->order.data[st->order.len++] = id;
	}
	if (st->over_path) {
		st->n = n;
		st->ik = ik;
	} else {
		ak_retain(list);
		ak_release(st->list);
		st->list = list;  // section(i) may read it
	}
	bool sections = make_sections(st, n);
	st->has_sections = sections;
	if (!same_as_sent(st, sections)) send_order(st, sections);
	free_ids(st->ids, st->ids_cap, st->key_kind);
	lid *t = st->ids;
	uint32_t c = st->ids_cap;
	st->ids = st->next_ids, st->ids_cap = st->next_cap;
	st->next_ids = t, st->next_cap = c;
	if (st->next_ids) memset(st->next_ids, 0, st->next_cap * sizeof(lid));
	for (uint32_t i = 0; i < st->cells.len; i++) {
		lcell *c = st->cells.data[i];
		if (c->header) continue;  // a section's ID keeps its key; the host rebinds the rest
		lid *x = find_id(st->ids, st->ids_cap, c->row.key, c->row.hash, st->key_kind);
		if (!x->hash) continue;
		if (st->over_path) ak_row_move(&c->row, ak_arr_vals(list)[x->index], ik, x->index);
		else ak_row_update(&c->row, ak_arr_vals(list)[x->index], ik, x->index);
	}
}

typedef struct {
	list_t *st;
	ak_arr *list;
} update_ctx;

static void update_fn(void *ctx) {
	update_ctx *u = ctx;
	update(u->st, u->list);
}

static void free_keys(list_t *st) {
	for (uint32_t i = 0; i < st->keys.len; i++) ak_release_val(st->keys.data[i], st->key_kind);
	st->keys.len = 0;
}

// path_run follows the order of a path's items (the length, the keys),
// not the items, then updates, untracked.
static void path_run(list_t *st) {
	ak_arr *list = NULL;
	if (ak_fsrc_follow(&st->src) < 0 || (list = ak_fsrc_items(&st->src), ak_error_pending())) {
		ak_release(list);
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	uint32_t n = list ? list->len : 0;
	free_keys(st);
	ak_vec_reserve(&st->keys, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_val k = ak_fsrc_key(&st->src, list, i, st->key_kind);
		if (ak_error_pending()) {
			free_keys(st);
			ak_release(list);
			ak_raise(ak_error_take());
			return;
		}
		st->keys.data[st->keys.len++] = k;
	}
	update_ctx u = {st, list};
	ak_untrack(update_fn, &u);
	free_keys(st);
	ak_release(list);
}

static void items_run(void *ctx) {
	list_t *st = ctx;
	if (st->over_path) {
		path_run(st);
		return;
	}
	ak_val v = ak_call(st->items, NULL, 0);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	update_ctx u = {st, v.p};
	ak_untrack(update_fn, &u);
	ak_release(v.p);
}

static void unbind_cell(list_t *st, uint32_t i);

// The List's rows go with what owns it: their owners, and their views,
// which the hosts keep by ID until they're destroyed (the cells are the
// host's, and go with the list's view).
static void items_drop(void *ctx) {
	list_t *st = ctx;
	while (st->cells.len) unbind_cell(st, st->cells.len - 1);
	ak_vec_free(&st->cells);
	ak_vec_free(&st->order);
	ak_vec_free(&st->sent_ids);
	ak_vec_free(&st->sent_secs);
	st->sent = false;
	free_ids(st->ids, st->ids_cap, st->key_kind);
	ak_free(st->ids);
	ak_free(st->next_ids);
	st->ids = st->next_ids = NULL;
	st->ids_cap = st->next_cap = 0;
	ak_release_val(st->start_key, st->key_kind);
	st->started = false;
	ak_release(st->list);
	st->list = NULL;
	if (st->over_path) {
		free_keys(st);
		ak_vec_free(&st->keys);
		ak_fsrc_free(&st->src);
		st->over_path = false;
	}
	ak_release(st->items);
	ak_release(st->key);
	ak_release(st->row);
	st->items = st->key = st->row = NULL;
	if (st->owner) ak_owner_release(st->owner);
	st->owner = NULL;
	ak_path_release(st->path);
	st->path = NULL;
	ak_release(st->e);
}

static void lazy_for(ak_view *v, ak_closure *items, ak_closure *key, void *acc, ak_val pkey, ak_closure *row, ak_kind key_kind) {
	if (v->el != AK_EL_LIST) ak_panic("ui: %s doesn't build its rows lazily", v->el ? ak_el_names[v->el] : v->cls->name);
	list_t *st = v->impl;
	if (st->items || st->over_path) ak_panic("ui: a List has one for");
	ak_retain(v);  // the effect's
	st->items = ak_retain(items);
	st->key = ak_retain(key);
	st->row = ak_retain(row);
	st->key_kind = key || pkey.p ? key_kind : AK_K_INT;
	if (acc) {
		st->over_path = true;
		ak_fsrc_init(&st->src, acc, pkey);
	}
	st->owner = ak_current_owner();
	if (st->owner) ak_owner_retain(st->owner);
	st->path = ak_path_capture();
	st->info = (ak_comp_info){.drop = items_drop, .label = "List"};
	st->eff = ak_effect_new(items_run, st, AK_EFFECT_RENDER, &st->info);
}

void ak_ui_lazy_for(ak_view *v, ak_closure *items, ak_closure *key, ak_closure *row, ak_kind key_kind) { lazy_for(v, items, key, NULL, (ak_val){0}, row, key_kind); }

void ak_ui_lazy_for_path(ak_view *v, void *acc, ak_val key, ak_closure *row, ak_kind key_kind) { lazy_for(v, NULL, NULL, acc, key, row, key_kind); }

// ---- From the host ----

int64_t ak_list_count(ak_view *e) { return count(e->impl); }

static void post_handler(void *ctx) {
	void **p = ctx;
	ak_view *e = p[0];
	uint16_t ev = (uint16_t)(uintptr_t)p[1];
	ak_free(p);
	if (!e->dead) {
		ak_batch_begin();
		ak_view_call(e, ev, NULL, 0);
		ak_batch_end();
		ak_ui_flush();
	}
	ak_release(e);
}

// post calls e's handler for ev soon: rows bind while the host lays out,
// not then.
static void post(ak_view *e, uint16_t ev) {
	void **p = ak_alloc(2 * sizeof(void *));
	p[0] = ak_retain(e);
	p[1] = (void *)(uintptr_t)ev;
	ak_post(post_handler, p);
}

// reached posts endReached when row i is near the end, once per length.
static void reached(list_t *st, int64_t i) {
	int64_t n = count(st);
	if (!ak_view_handler(st->e, AK_EV_LIST_END_REACHED) || i < n - 3 || n == st->end_at) return;
	st->end_at = n;
	post(st->e, AK_EV_LIST_END_REACHED);
}

// reached_start posts startReached when row i is near the start, once
// per first item.
static void reached_start(list_t *st, int64_t i) {
	if (!ak_view_handler(st->e, AK_EV_LIST_START_REACHED) || i > 2 || count(st) == 0) return;
	ak_val k = st->over_path ? key_at(st, 0) : key_of(st, ak_arr_vals(st->list)[0], 0);
	if (ak_error_pending()) {
		ak_ui_raise_pending(st->owner);
		return;
	}
	if (st->started && ak_val_eq(k, st->start_key, st->key_kind)) {
		ak_release_val(k, st->key_kind);
		return;
	}
	ak_release_val(st->start_key, st->key_kind);
	st->started = true;
	st->start_key = k;
	post(st->e, AK_EV_LIST_START_REACHED);
}

static int32_t find_cell(list_t *st, ak_id cell) {
	for (uint32_t i = 0; i < st->cells.len; i++)
		if (st->cells.data[i]->el->id == cell) return (int32_t)i;
	return -1;
}

typedef struct {
	list_t *st;
	lcell *c;
	ak_val item;
	ak_kind ik;
	int64_t i;
} make_ctx;

static void make_row(void *ctx) {
	make_ctx *m = ctx;
	lcell *c = m->c;
	if (m->st->over_path) ak_row_make_at(&c->row, m->st->e->r, m->st->row, m->st->path, m->st->owner, m->st->src.acc, m->st->eff, m->item, m->ik, m->i, m->st->key_kind);
	else ak_row_make(&c->row, m->st->e->r, m->st->row, m->st->path, m->st->owner, m->item, m->ik, m->i, m->st->key_kind);
	if (c->row.el) {
		ak_view_append(c->el, c->row.el);
		ak_release(c->row.el);  // the cell holds it
	}
}

// ---- Cells: swipe actions, selection ----

extern const ak_el_class ak_class_lcell;

// encode_swipe encodes a cell's swipe actions, flat: a swipe shows
// neither children nor a disabled look, so each item's number is its
// place.
static void encode_swipe(ak_buf *b, const ak_arr *as) {
	ak_buf_u32(b, as ? as->len : 0);
	for (uint32_t i = 0; as && i < as->len; i++) {
		const ak_enum *a = ak_arr_vals(as)[i].p;
		uint16_t c = a ? ak_enum_case(a) : 0;
		const ak_str *title = a ? a->f[0].p : NULL, *icon = a && c <= 1 ? a->f[1].p : NULL;
		ak_buf_u8(b, a && c == 0 ? (uint8_t)a->f[2].i : 0);
		ak_buf_str(b, title ? title->data : "", title ? title->len : 0);
		ak_buf_str(b, icon ? icon->data : "", icon ? icon->len : 0);
		ak_buf_u8(b, 0);
	}
}

static int64_t cell_index(lcell *c, bool track) { return ak_vm_cell_get(c->row.cell_index, track).i; }

// swipe_set sends one of a cell's swipe actions (v, taken), if it
// changed.
static void swipe_set(lcell *c, ak_arr *v, ak_arr **as, ak_buf *sent, bool *has, ak_prop p) {
	ak_release(*as);
	*as = v;
	ak_buf b = {0};
	encode_swipe(&b, *as);
	if (*has && b.len == sent->len && memcmp(b.data, sent->data, b.len) == 0) {
		ak_buf_free(&b);
		return;
	}
	ak_buf_free(sent);
	*sent = b;
	*has = true;
	ak_batch_set_bytes(c->el->r->batch, c->el->id, p, b.data, b.len);
}

// swipe_send sends the swipe actions f gives row i, if they changed.
static void swipe_send(lcell *c, ak_closure *f, ak_arr **as, ak_buf *sent, bool *has, ak_prop p, int64_t i) {
	ak_val arg = {.i = i};
	ak_val v = ak_call(f, &arg, 1);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	swipe_set(c, v.p, as, sent, has, p);
}

static void swipe_run(void *ctx) {
	lcell *c = ctx;
	if (c->el->dead) return;
	int64_t i = cell_index(c, true);
	ak_closure *trail = closure_key(c->st->e, AK_KEY_LIST_SWIPE_ACTIONS), *lead = closure_key(c->st->e, AK_KEY_LIST_LEADING_SWIPE_ACTIONS);
	if (trail) swipe_send(c, trail, &c->actions, &c->sent, &c->has_sent, AK_PROP_SWIPE_ACTIONS, i);
	if (lead) swipe_send(c, lead, &c->leading, &c->sent_l, &c->has_sent_l, AK_PROP_LEADING_SWIPE_ACTIONS, i);
}

// show_selected sends whether cell c's row is selected, if the host shows
// otherwise.
static void show_selected(lcell *c, bool v) {
	int8_t s = v ? 1 : 0;
	if (c->sel == s) return;
	c->sel = s;
	ak_batch_set_bool(c->el->r->batch, c->el->id, AK_PROP_SELECTED, v);
}

static bool is_selected(lcell *c, int64_t i) {
	ak_closure *f = closure_key(c->st->e, AK_KEY_LIST_SELECTED);
	ak_val arg = {.i = i};
	ak_val v = ak_call(f, &arg, 1);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return c->sel == 1;
	}
	return v.i != 0;
}

static void selected_run(void *ctx) {
	lcell *c = ctx;
	if (c->el->dead) return;
	show_selected(c, is_selected(c, cell_index(c, true)));
}

static const ak_comp_info swipe_info = {.label = "List.swipe"};
static const ak_comp_info selected_info = {.label = "List.selected"};

static void cell_effects(void *ctx) {
	lcell *c = ctx;
	ak_view *e = c->st->e;
	if (closure_key(e, AK_KEY_LIST_SWIPE_ACTIONS) || closure_key(e, AK_KEY_LIST_LEADING_SWIPE_ACTIONS)) {
		ak_view_listen(c->el, AK_EVENT_ACTION);
		ak_effect_new(swipe_run, c, AK_EFFECT_RENDER, &swipe_info);
	}
	if (closure_key(e, AK_KEY_LIST_SELECTED)) ak_effect_new(selected_run, c, AK_EFFECT_RENDER, &selected_info);
	if (closure_key(e, AK_KEY_LIST_SELECTED) || ak_view_handler(e, AK_EV_LIST_SELECT)) ak_view_listen(c->el, AK_EVENT_ROW_SELECTED);
	if (ak_view_handler(e, AK_EV_LIST_SELECT_BEGIN)) ak_view_listen(c->el, AK_EVENT_ROW_SELECT_BEGIN);
}

// ---- A row's own swipe actions and selection ----
//
// The container at the top of a row (View, Row, Column, SafeArea) may
// say them itself: its swipeActions, leadingSwipeActions and selected,
// its on select(on) and on selectBegin, rather than the List's of the
// row's index.

static bool is_container(const ak_view *v) { return v->el >= AK_EL_VIEW && v->el <= AK_EL_SAFE_AREA; }

// row_view is the container at the top of cell c's row, through
// fragments; NULL when its row has none.
static ak_view *row_view(lcell *c) {
	ak_view *v = c->el && c->el->kids.len ? c->el->kids.data[0] : NULL;
	while (v && !is_container(v) && v->frag && v->kids.len) v = v->kids.data[0];
	return v && is_container(v) ? v : NULL;
}

static void row_sync(lcell *c) {
	ak_view *v = row_view(c);
	if (!v || c->el->dead) return;
	const ak_val *t = ak_style_get(v, ak_ext_key(v, AK_KEY_VIEW_SWIPE_ACTIONS));
	const ak_val *l = ak_style_get(v, ak_ext_key(v, AK_KEY_VIEW_LEADING_SWIPE_ACTIONS));
	const ak_val *s = ak_style_get(v, ak_ext_key(v, AK_KEY_VIEW_SELECTED));
	if (t) swipe_set(c, t->p ? ak_retain(t->p) : NULL, &c->actions, &c->sent, &c->has_sent, AK_PROP_SWIPE_ACTIONS);
	if (l) swipe_set(c, l->p ? ak_retain(l->p) : NULL, &c->leading, &c->sent_l, &c->has_sent_l, AK_PROP_LEADING_SWIPE_ACTIONS);
	if (t || l) ak_view_listen(c->el, AK_EVENT_ACTION);
	if (s) show_selected(c, s->i != 0);
	if (s || ak_view_handler(v, ak_ext_ev(v, AK_EV_VIEW_SELECT))) ak_view_listen(c->el, AK_EVENT_ROW_SELECTED);
	if (ak_view_handler(v, ak_ext_ev(v, AK_EV_VIEW_SELECT_BEGIN))) ak_view_listen(c->el, AK_EVENT_ROW_SELECT_BEGIN);
}

// ak_list_row_keys hears of a container's swipe actions or selected
// changing: when it's at the top of a List's row, the row's cell sends
// them.
void ak_list_row_keys(ak_view *v) {
	for (ak_view *p = v; p; p = p->parent) {
		if (p->cls == &ak_class_lcell) {
			if (p->impl && !((lcell *)p->impl)->header && row_view(p->impl) == v) row_sync(p->impl);
			return;
		}
	}
}

static void run_swipe(lcell *c, const ak_arr *as, int64_t k) {
	if (!as || k < 0 || k >= as->len) return;
	const ak_enum *a = ak_arr_vals(as)[k].p;
	if (!a || ak_enum_case(a) != 0 || !a->f[5].p) return;  // a swipe shows each action enabled
	ak_closure *run = ak_retain(a->f[5].p);
	ak_val r = ak_ui_call_under(c->row.owner, run, NULL, 0);
	if (!ak_error_pending()) ak_release(r.p);
	else ak_ui_raise_pending(c->row.owner);
	ak_release(run);
}

static void lcell_event(ak_view *el, const ak_event *ev) {
	lcell *c = el->impl;
	if (!c || c->header) return;
	list_t *st = c->st;
	int64_t i = cell_index(c, false);
	switch (ev->kind) {
	case AK_EVENT_ACTION: {
		ak_arr *as = ak_retain(ev->i < 0 ? c->leading : c->actions);
		run_swipe(c, as, ev->i < 0 ? -1 - ev->i : ev->i);
		ak_release(as);
		ak_val args[2] = {{.i = i}, {.i = ev->i}};
		if (!st->e->dead) ak_view_call(st->e, AK_EV_LIST_ACTION, args, 2);
		break;
	}
	case AK_EVENT_ROW_SELECTED: {
		// The host shows it as the user left it; what the app doesn't take
		// goes back.
		c->sel = ev->b ? 1 : 0;
		ak_view *rv = row_view(c);
		if (rv) {
			ak_retain(rv);
			ak_val on = {.i = ev->b};
			ak_view_call(rv, ak_ext_ev(rv, AK_EV_VIEW_SELECT), &on, 1);
			const ak_val *s = el->dead || rv->dead ? NULL : ak_style_get(rv, ak_ext_key(rv, AK_KEY_VIEW_SELECTED));
			if (s) show_selected(c, s->i != 0);
			ak_release(rv);
		}
		ak_val args[2] = {{.i = i}, {.i = ev->b}};
		ak_view_call(st->e, AK_EV_LIST_SELECT, args, 2);
		if (!el->dead && closure_key(st->e, AK_KEY_LIST_SELECTED)) {
			void *tok = ak_untrack_begin();
			bool v = is_selected(c, i);
			ak_untrack_end(tok);
			if (ak_error_pending()) ak_ui_raise_pending(c->row.owner);
			show_selected(c, v);
		}
		break;
	}
	case AK_EVENT_ROW_SELECT_BEGIN: {
		ak_view *rv = row_view(c);
		if (rv) ak_view_call(rv, ak_ext_ev(rv, AK_EV_VIEW_SELECT_BEGIN), NULL, 0);
		ak_val arg = {.i = i};
		ak_view_call(st->e, AK_EV_LIST_SELECT_BEGIN, &arg, 1);
		break;
	}
	}
}

static void lcell_adjust(ak_view *e, ak_lstyle *ls) {
	AK_UNUSED(e);
	ls->width = ak_lpct(100);
}

const ak_el_class ak_class_lcell = {.name = "Cell", .kind = AK_KIND_VIEW, .flags = AK_ELF_CHILDREN, .event = lcell_event, .adjust = lcell_adjust};

// new_cell makes the element of a cell the host made.
static lcell *new_cell(list_t *st, ak_id cell) {
	ak_view *e = st->e;
	ak_ui_root *r = e->r;
	lcell *c = ak_alloc(sizeof *c);
	c->st = st;
	ak_view *el = ak_view_new(r, &ak_class_lcell, 0, false);
	el->real = true;
	el->cell = true;
	el->id = cell;
	el->up = e;
	el->dir = e->dir;
	el->impl = c;
	ak_view_put_id(el);
	ak_style_relayout(el);  // a layout root: its direction (and as wide as the row it's laid out at)
	if (el->dir != AK_LAYOUT_DIRECTION_INHERIT) ak_batch_set_enum(r->batch, cell, AK_PROP_LAYOUT_DIRECTION, el->dir);
	el->finalized = true;
	c->el = el;
	c->row.old = -1;
	ak_vec_push(&st->cells, c);
	return c;
}

static int32_t find_cell(list_t *st, ak_id cell);

// bind_header binds section s's header into cell.
static void bind_header(list_t *st, int64_t s, ak_id cell) {
	ak_closure *header = closure_key(st->e, AK_KEY_LIST_HEADER);
	if (!header || s >= st->sections.len) return;
	lsection *sec = &st->sections.data[s];
	int32_t ci = find_cell(st, cell);
	if (ci >= 0) {
		lcell *c = st->cells.data[ci];
		if (c->header) {
			ak_retain(sec->key);
			ak_vm_cell_set(c->row.cell_item, (ak_val){.p = sec->key});
			return;
		}
		// A cell a row was in (a desktop host reuses its cells for both):
		// none of the row's state stays with it.
		ak_batch *b = st->e->r->batch;
		if (c->sel > 0) ak_batch_set_bool(b, cell, AK_PROP_SELECTED, false);
		uint8_t none[4] = {0};
		if (c->has_sent && c->sent.len > 4) ak_batch_set_bytes(b, cell, AK_PROP_SWIPE_ACTIONS, none, 4);
		if (c->has_sent_l && c->sent_l.len > 4) ak_batch_set_bytes(b, cell, AK_PROP_LEADING_SWIPE_ACTIONS, none, 4);
		unbind_cell(st, (uint32_t)ci);
	}
	ak_ui_root *r = st->e->r;
	lcell *c = new_cell(st, cell);
	c->header = true;
	ak_retain(sec->key);
	c->row.cell_item = ak_vm_cell_new((ak_val){.p = sec->key}, AK_K_REF, NULL).p;
	c->row.owner = ak_root_under(st->owner);
	ak_val acc = ak_vm_acc_cell(c->row.cell_item);
	ak_ui_root *prev = ak_ui.cur;
	ak_ui.cur = r;
	ak_view *v = ak_build_under(r, c->row.owner, header, &acc, 1);
	ak_ui.cur = prev;
	ak_release(acc.p);
	if (v) {
		ak_view_append(c->el, v);
		ak_release(v);
	}
}

// cell_id_ok reports whether cell can name a cell of the list: an ID the
// host made (AK_HOST_ID_BIT: never 0, never one of the core's) that no
// view has, but one of the list's own cells, which a bind recycles. A
// host that binds any other is wrong, and the bind is dropped: two views under one
// ID would leave the ID table naming a view already freed.
static bool cell_id_ok(list_t *st, ak_id cell) {
	if (!(cell & AK_HOST_ID_BIT)) return false;
	ak_view *v = ak_view_of(cell);
	return !v || find_cell(st, cell) >= 0;
}

static void list_bind(list_t *st, ak_view *e, int64_t i, ak_id cell, ak_arr *items);

void ak_list_bind(ak_view *e, int64_t i, ak_id cell) {
	list_t *st = e->impl;
	if (!cell_id_ok(st, cell)) return;
	if (i < 0) {
		bind_header(st, -1 - i, cell);
		return;
	}
	if (i >= count(st)) return;
	if (!st->over_path) {
		list_bind(st, e, i, cell, st->list);
		return;
	}
	// Over a path: its items now, read for this.
	ak_arr *items = ak_fsrc_items(&st->src);
	if (ak_error_pending()) {
		ak_ui_raise_pending(st->owner);
	} else if (items && i < items->len) {
		list_bind(st, e, i, cell, items);
	}
	ak_release(items);
}

static void list_bind(list_t *st, ak_view *e, int64_t i, ak_id cell, ak_arr *items) {
	ak_val item = ak_arr_vals(items)[i];
	ak_kind ik = ak_arr_kind(items);
	reached(st, i);
	reached_start(st, i);
	int32_t ci = find_cell(st, cell);
	bool was_header = ci >= 0 && st->cells.data[ci]->header;
	if (was_header) {
		unbind_cell(st, (uint32_t)ci);
		ci = -1;
	}
	if (ci >= 0) {
		// A recycled cell: its row takes the item, which updates only the
		// props that differ.
		lcell *c = st->cells.data[ci];
		ak_val k = st->over_path ? ak_fsrc_key(&st->src, items, i, st->key_kind) : key_of(st, item, i);
		if (ak_error_pending()) {
			ak_ui_raise_pending(st->owner);
			return;
		}
		ak_release_val(c->row.key, st->key_kind);
		c->row.key = k;
		c->row.hash = hash_of(k, st->key_kind);
		if (st->over_path) ak_row_move(&c->row, item, ik, i);
		else ak_row_update(&c->row, item, ik, i);
		return;
	}
	ak_val k = st->over_path ? ak_fsrc_key(&st->src, items, i, st->key_kind) : key_of(st, item, i);
	if (ak_error_pending()) {
		ak_ui_raise_pending(st->owner);
		return;
	}
	// The cell's element: the host made its view.
	ak_ui_root *r = e->r;
	lcell *c = new_cell(st, cell);
	if (was_header) c->sel = -1;  // the host's state is a header's: unknown, sent at once
	c->row.key = k;
	c->row.hash = hash_of(k, st->key_kind);
	// Under the List's owner: it sees what the List sees.
	make_ctx m = {st, c, item, ik, i};
	ak_ui_root *prev = ak_ui.cur;
	ak_ui.cur = r;
	make_row(&m);
	if (c->row.owner) ak_with_owner(c->row.owner, cell_effects, c);
	row_sync(c);
	ak_ui.cur = prev;
}

static void unbind_cell(list_t *st, uint32_t i) {
	lcell *c = st->cells.data[i];
	memmove(st->cells.data + i, st->cells.data + i + 1, (st->cells.len - i - 1) * sizeof(lcell *));
	st->cells.len--;
	ak_row_free(&c->row, st->key_kind);
	ak_view *el = c->el;
	while (el->kids.len) ak_view_remove(el, el->kids.data[el->kids.len - 1], true);
	el->dead = true;  // the host's view: not destroyed by the core
	el->impl = NULL;
	if (ak_view_of(el->id) == el) ak_view_drop_id(el->id);
	ak_release(el);
	ak_release(c->actions);
	ak_release(c->leading);
	ak_buf_free(&c->sent);
	ak_buf_free(&c->sent_l);
	ak_free(c);
}

void ak_list_unbind(ak_view *e, ak_id cell) {
	list_t *st = e->impl;
	int32_t i = find_cell(st, cell);
	if (i >= 0) unbind_cell(st, (uint32_t)i);
}

// ak_list_bound is the last row the host has bound (-1: none) and how
// many rows it has bound: where the shown rows end.
int64_t ak_list_bound(ak_view *e, uint32_t *n) {
	list_t *st = e->impl;
	int64_t last = -1;
	uint32_t rows = 0;
	for (uint32_t i = 0; st && i < st->cells.len; i++) {
		lcell *c = st->cells.data[i];
		if (c->header) continue;
		int64_t at = cell_index(c, false);
		if (at > last) last = at;
		rows++;
	}
	if (n) *n = rows;
	return last;
}

void ak_list_each_cell(ak_view *e, void (*f)(ak_view *c, void *ctx), void *ctx) {
	list_t *st = e->impl;
	for (uint32_t i = 0; st && i < st->cells.len; i++) f(st->cells.data[i]->el, ctx);
}

// ak_list_layout lays out the bound rows at the list's width (an inset
// grouped list's less its insets, a grid's cells at a column's); each
// cell's frame tells the host the row's height.
void ak_list_layout(ak_view *e, ak_lpass *p) {
	list_t *st = e->impl;
	if (!st || !st->cells.len) return;
	float w = ak_lnode_frame(e->box).w - 2 * st->inset - st->row_lead - st->row_trail;
	if (st->columns > 1) {
		float gap = ak_style_float(e, AK_KEY_GAP, 0);
		w = (ak_lnode_frame(e->box).w - gap * (float)(st->columns - 1)) / (float)st->columns;
	}
	if (w <= 0) return;
	for (uint32_t i = 0; i < st->cells.len; i++) {
		lcell *c = st->cells.data[i];
		if (ak_lnode_dirty(c->el->box) || c->width != w) {
			c->width = w;
			ak_lnode_layout(c->el->box, (ak_lsize){w, AK_LMAX_CONTENT}, p);
		}
	}
}
