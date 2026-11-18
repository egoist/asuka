// control.c: control flow in views: Show, For (keyed,
// with moved rows), Switch, Dynamic; and what a component's body uses:
// contexts (provide and use) and tasks.
//
// Each is a fragment whose render effect builds what shows. A branch or
// a row is built under an owner of its own, a root that sees the
// contexts and catchers around it, disposed when it goes: its effects
// stop, and its views are destroyed with it.
#include "ui/ui_int.h"

#include <stdio.h>
#include <string.h>

static void raise_in_run(void) {
	void *err = ak_error_take();
	if (ak_is_cancel(err)) ak_release(err);
	else ak_raise(err);  // delivered once the run returns
}

// root_under makes an owner like ak_root_new's under parent: a branch's
// or a row's, made while the effect that shows it runs but held by what
// made the effect (an owner holds its parent: made under the effect's
// own, it would keep the effect from ever being dropped).
static void new_root(void *ctx) { *(ak_owner **)ctx = ak_root_new(); }

ak_owner *ak_root_under(ak_owner *parent) {
	ak_owner *o = NULL;
	if (parent && !ak_owner_disposed(parent)) ak_with_owner(parent, new_root, &o);
	else o = ak_root_new();
	return o;
}

static ak_owner *creator(void) {
	ak_owner *o = ak_current_owner();
	if (o) ak_owner_retain(o);
	return o;
}

// ---- Branches: Show and Switch ----

typedef struct {
	ak_view *f;  // the fragment, retained
	ak_closure *sel, *a, *b;  // Show: cond, then, otherwise; Switch: selector, build
	bool is_switch;
	bool has;      // a branch was chosen
	int64_t state;
	ak_view *shown;  // f's kid (f holds it)
	ak_owner *branch;
	ak_owner *owner;  // what made it, retained
	ak_path *path;
	ak_comp_info info;
} branch_t;

typedef struct {
	branch_t *s;
	ak_closure *build;
	ak_val arg;
	uint32_t nargs;
} branch_build;

static void build_branch(void *ctx) {
	branch_build *bb = ctx;
	branch_t *s = bb->s;
	ak_view *v = ak_ui_build(s->f->r, bb->build, &bb->arg, bb->nargs);
	if (v) {
		s->shown = v;
		ak_view_append(s->f, v);
		ak_release(v);
	}
}

static void clear_branch(branch_t *s) {
	if (s->shown) {
		ak_view_remove(s->f, s->shown, true);
		s->shown = NULL;
	}
	if (s->branch) {
		ak_owner *o = s->branch;
		s->branch = NULL;
		ak_owner_dispose(o);
	}
}

static void branch_run(void *ctx) {
	branch_t *s = ctx;
	ak_val v = ak_call(s->sel, NULL, 0);
	if (ak_error_pending()) {
		raise_in_run();
		return;
	}
	int64_t state = s->is_switch ? v.i : (v.i != 0);
	if (s->has && state == s->state) return;
	s->has = true;
	s->state = state;
	clear_branch(s);
	ak_closure *build = s->is_switch ? s->a : state ? s->a : s->b;
	if (!build) return;
	char seg[32];
	int n = s->is_switch ? snprintf(seg, sizeof seg, "?%lld", (long long)state) : snprintf(seg, sizeof seg, "%s", state ? "?then" : "?else");
	ak_path *prev = ak_path_push(s->path, seg, (size_t)n);
	s->branch = ak_root_under(s->owner);
	branch_build bb = {s, build, {.i = state}, s->is_switch ? 1 : 0};
	ak_with_owner(s->branch, build_branch, &bb);
	ak_path_pop(prev);
}

static void branch_drop(void *ctx) {
	branch_t *s = ctx;
	clear_branch(s);
	ak_release(s->f);
	ak_release(s->sel);
	ak_release(s->a);
	ak_release(s->b);
	ak_path_release(s->path);
	if (s->owner) ak_owner_release(s->owner);
	ak_free(s);
}

// branch_describe names an if or a match in a view as its source says
// it: "Card's if at card.asuka:12".
static void branch_describe(void *ctx, char *buf, size_t n) {
	branch_t *s = ctx;
	ak_vm_where(s->sel, s->is_switch ? "match" : "if", buf, n);
}

static const void *branch_site(void *ctx) { return ak_vm_where_site(((branch_t *)ctx)->sel); }

static ak_view *branch_new(ak_closure *sel, ak_closure *a, ak_closure *b, bool is_switch, const char *label) {
	ak_view *f = ak_frag_new();
	branch_t *s = ak_alloc(sizeof *s);
	s->f = ak_retain(f);
	s->sel = ak_retain(sel);
	s->a = ak_retain(a);
	s->b = ak_retain(b);
	s->is_switch = is_switch;
	s->owner = creator();
	s->path = ak_path_capture();
	s->info = (ak_comp_info){.drop = branch_drop, .label = label, .describe = branch_describe, .site = branch_site};
	ak_effect_new(branch_run, s, AK_EFFECT_RENDER, &s->info);
	return f;
}

ak_view *ak_ui_show(ak_closure *cond, ak_closure *then, ak_closure *otherwise) { return branch_new(cond, then, otherwise, false, "Show"); }

ak_view *ak_ui_switch(ak_closure *selector, ak_closure *build) { return branch_new(selector, build, NULL, true, "Switch"); }

// ---- Dynamic ----

typedef struct {
	ak_view *f;
	ak_closure *build;
	ak_view *shown;
	ak_owner *owner;  // what made it, retained
	ak_owner *built;  // what the last build made
	ak_path *path;
	ak_comp_info info;
	ak_val out;
} dyn_t;

static void dyn_build(void *ctx) {
	dyn_t *d = ctx;
	ak_ui_root *pr = ak_ui.cur;
	ak_ui.cur = d->f->r;
	d->out = ak_call(d->build, NULL, 0);
	ak_ui.cur = pr;
}

static void dyn_clear(dyn_t *d) {
	if (d->shown) {
		ak_view_remove(d->f, d->shown, true);
		d->shown = NULL;
	}
	if (d->built) {
		ak_owner *o = d->built;
		d->built = NULL;
		ak_owner_dispose(o);
	}
}

// dyn_run builds again, tracked: everything build reads (and what the
// components it calls read) makes it build again. What a build makes is
// under an owner of its own, disposed as it builds again.
static void dyn_run(void *ctx) {
	dyn_t *d = ctx;
	dyn_clear(d);
	ak_path *prev = ak_path_push(d->path, "~", 1);
	d->built = ak_root_under(d->owner);
	d->out = (ak_val){0};
	ak_with_owner_tracked(d->built, dyn_build, d);
	ak_path_pop(prev);
	if (ak_error_pending()) {
		raise_in_run();
		return;
	}
	ak_view *v = d->out.p;
	if (v) {
		ak_view_finalize(v);
		d->shown = v;
		ak_view_append(d->f, v);
		ak_release(v);
	}
}

static void dyn_drop(void *ctx) {
	dyn_t *d = ctx;
	dyn_clear(d);
	ak_release(d->f);
	ak_release(d->build);
	ak_path_release(d->path);
	if (d->owner) ak_owner_release(d->owner);
	ak_free(d);
}

ak_view *ak_ui_dynamic(ak_closure *build) {
	ak_view *f = ak_frag_new();
	dyn_t *d = ak_alloc(sizeof *d);
	d->f = ak_retain(f);
	d->build = ak_retain(build);
	d->owner = creator();
	d->path = ak_path_capture();
	d->info = (ak_comp_info){.drop = dyn_drop, .label = "Dynamic"};
	ak_effect_new(dyn_run, d, AK_EFFECT_RENDER, &d->info);
	return f;
}

// ---- For ----

typedef struct {
	ak_view *f;
	ak_closure *items, *key, *row;
	ak_kind key_kind;
	AK_VEC(ak_row) rows, next;
	uint32_t *table;  // old rows by key: index + 1, 0 empty
	uint32_t table_cap;
	AK_VEC(uint8_t) taken;
	AK_VEC(int64_t) tails, prev;
	AK_VEC(uint8_t) stay;
	AK_VEC(ak_val) keys;  // the new items' keys
	ak_owner *owner;      // what made it, retained: its rows' owners' parent
	ak_path *path;
	ak_comp_info info;
	bool over_path;       // its items are a path's (src), not items()'s
	ak_fsrc src;
} for_t;

static void key_seg(ak_buf *b, ak_val k, ak_kind kind) {
	ak_buf_u8(b, '[');
	if (kind == AK_K_INT || kind == AK_K_BOOL) ak_buf_printf(b, "%lld", (long long)k.i);
	else if (kind == AK_K_FLOAT) ak_buf_printf(b, "%g", k.f);
	else if (k.p && ((ak_obj *)k.p)->tag == AK_TAG_STR) ak_buf_put(b, ((ak_str *)k.p)->data, ((ak_str *)k.p)->len);
	else ak_buf_printf(b, "#%016llx", (unsigned long long)ak_val_hash(k, kind));
	ak_buf_u8(b, ']');
}

typedef struct {
	ak_ui_root *r;
	ak_closure *row;
	ak_val args[2];
	ak_view *out;
} row_build;

static void build_row(void *ctx) {
	row_build *rb = ctx;
	rb->out = ak_ui_build(rb->r, rb->row, rb->args, 2);
}

// ak_row_make makes a row of item at index i: its cells, its owner, its
// view (owned by the row; NULL if it built nothing).
void ak_row_make(ak_row *rw, ak_ui_root *r, ak_closure *row, ak_path *path, ak_owner *parent, ak_val item, ak_kind item_kind, int64_t i, ak_kind key_kind) {
	ak_retain_val(item, item_kind);
	rw->cell_item = ak_vm_cell_new(item, item_kind, NULL).p;
	rw->cell_index = ak_vm_cell_new((ak_val){.i = i}, AK_K_INT, NULL).p;
	rw->owner = ak_root_under(parent);
	ak_buf seg = {0};
	key_seg(&seg, rw->key, key_kind);
	ak_path *prev = ak_path_push(path, (const char *)seg.data, seg.len);
	ak_buf_free(&seg);
	row_build rb = {r, row, {ak_vm_acc_cell(rw->cell_item), ak_vm_acc_cell(rw->cell_index)}, NULL};
	ak_with_owner(rw->owner, build_row, &rb);
	ak_release(rb.args[0].p);
	ak_release(rb.args[1].p);
	ak_path_pop(prev);
	rw->el = rb.out;
}

void ak_row_make_at(ak_row *rw, ak_ui_root *r, ak_closure *row, ak_path *path, ak_owner *parent, void *acc, ak_effect *eff, ak_val item, ak_kind item_kind, int64_t i, ak_kind key_kind) {
	rw->cell_item = NULL;
	rw->cell_index = ak_vm_cell_new((ak_val){.i = i}, AK_K_INT, NULL).p;
	rw->item_acc = ak_vm_acc_row(acc, rw->cell_index, item_kind, eff).p;
	ak_vm_acc_row_set(rw->item_acc, item, item_kind);
	rw->owner = ak_root_under(parent);
	ak_buf seg = {0};
	key_seg(&seg, rw->key, key_kind);
	ak_path *prev = ak_path_push(path, (const char *)seg.data, seg.len);
	ak_buf_free(&seg);
	row_build rb = {r, row, {{.p = ak_retain(rw->item_acc)}, ak_vm_acc_cell(rw->cell_index)}, NULL};
	ak_with_owner(rw->owner, build_row, &rb);
	ak_release(rb.args[0].p);
	ak_release(rb.args[1].p);
	ak_path_pop(prev);
	rw->el = rb.out;
}

void ak_row_move(ak_row *rw, ak_val item, ak_kind item_kind, int64_t i) {
	ak_vm_acc_row_set(rw->item_acc, item, item_kind);
	ak_vm_cell_set(rw->cell_index, (ak_val){.i = i});
}

// ---- A for over a path's items ----

void ak_fsrc_init(ak_fsrc *s, void *acc, ak_val key) {
	memset(s, 0, sizeof *s);
	s->acc = ak_retain(acc);
	ak_obj *k = key.p;
	if (k && k->tag == AK_TAG_STR) {
		// A path of the item's fields: 'f' and a field's index, each.
		ak_str *f = key.p;
		s->by_field = true;
		s->nf = (uint16_t)(f->len / 3);
		s->fields = ak_alloc(sizeof(uint16_t) * (s->nf ? s->nf : 1));
		for (uint16_t j = 0; j < s->nf; j++) s->fields[j] = (uint16_t)((uint8_t)f->data[3 * j + 1] | (uint8_t)f->data[3 * j + 2] << 8);
	} else if (k) {
		static const uint8_t at[1] = {'I'};  // what the key reads of each item: a column's
		ak_val zero = {.i = 0};
		s->key = ak_retain(key.p);
		s->cursor = ak_vm_acc_path(acc, at, 1, &zero, AK_K_REF).p;
	}
}

void ak_fsrc_free(ak_fsrc *s) {
	ak_release(s->acc);
	ak_release(s->key);
	ak_release(s->cursor);
	ak_free(s->fields);
	memset(s, 0, sizeof *s);
}

int64_t ak_fsrc_follow(ak_fsrc *s) {
	static const uint8_t len[1] = {'n'};
	ak_val n = ak_vm_acc_getp(s->acc, len, 1, NULL);
	if (ak_error_pending()) return -1;
	if (s->by_field) ak_vm_acc_keys(s->acc, s->fields, s->nf);
	if (ak_error_pending()) return -1;
	return n.i;
}

ak_arr *ak_fsrc_items(ak_fsrc *s) {
	void *t = ak_untrack_begin();
	ak_val v = ak_vm_acc_get(s->acc);
	ak_untrack_end(t);
	return v.p;
}

ak_val ak_fsrc_key(ak_fsrc *s, ak_arr *items, int64_t i, ak_kind kk) {
	if (s->by_field) {
		ak_val v = items->data[i];
		for (uint16_t j = 0; j < s->nf; j++) {
			ak_struct *st = v.p;
			if (!st) break;
			v = st->f[s->fields[j]];
		}
		ak_retain_val(v, kk);
		return v;
	}
	if (!s->key) return (ak_val){.i = i};
	ak_vm_acc_seek(s->cursor, i);
	ak_val args[2] = {{.p = s->cursor}, {.i = i}};  // (item's accessor, index)
	return ak_call(s->key, args, 2);
}

void ak_row_update(ak_row *rw, ak_val item, ak_kind item_kind, int64_t i) {
	ak_retain_val(item, item_kind);
	ak_vm_cell_set(rw->cell_item, item);
	ak_vm_cell_set(rw->cell_index, (ak_val){.i = i});
}

void ak_row_free(ak_row *rw, ak_kind key_kind) {
	// Gone first: what its owner runs as it's disposed (its cleanups, its
	// tasks' defers) reads its own item, as its for last placed it, not
	// what's at its old index now.
	if (rw->item_acc) ak_vm_acc_row_gone(rw->item_acc);
	ak_owner_dispose(rw->owner);
	rw->owner = NULL;
	ak_release(rw->cell_item);
	ak_release(rw->cell_index);
	if (rw->item_acc) {
		ak_release(rw->item_acc);
		rw->item_acc = NULL;
	}
	ak_release_val(rw->key, key_kind);
}

static ak_val key_of(for_t *s, ak_val item, int64_t i) {
	if (!s->key) return (ak_val){.i = i};
	ak_val args[2] = {item, {.i = i}};  // the key's function takes (item, index)
	return ak_call(s->key, args, 2);
}

static uint32_t find_old(for_t *s, ak_val k, uint32_t hash) {
	uint32_t cap = s->table_cap;
	for (uint32_t j = hash & (cap - 1);; j = (j + 1) & (cap - 1)) {
		uint32_t x = s->table[j];
		if (!x) return UINT32_MAX;
		ak_row *rw = &s->rows.data[x - 1];
		if (!s->taken.data[x - 1] && rw->hash == hash && ak_val_eq(rw->key, k, s->key_kind)) return x - 1;
	}
}

// lis marks the rows of next (with old >= 0) on a longest increasing
// subsequence of old positions: they stay put, the others move.
static void lis(for_t *s) {
	uint32_t n = s->next.len;
	s->stay.len = 0;
	ak_vec_reserve(&s->stay, n);
	s->stay.len = n;
	if (n) memset(s->stay.data, 0, n);  // an empty list has no data yet
	s->prev.len = 0;
	ak_vec_reserve(&s->prev, n);
	s->prev.len = n;
	s->tails.len = 0;
	for (uint32_t i = 0; i < n; i++) {
		int64_t old = s->next.data[i].old;
		if (old < 0) continue;
		uint32_t lo = 0, hi = s->tails.len;
		while (lo < hi) {
			uint32_t mid = (lo + hi) / 2;
			if (s->next.data[s->tails.data[mid]].old < old) lo = mid + 1;
			else hi = mid;
		}
		s->prev.data[i] = lo > 0 ? s->tails.data[lo - 1] : -1;
		if (lo == s->tails.len) ak_vec_push(&s->tails, (int64_t)i);
		else s->tails.data[lo] = i;
	}
	if (s->tails.len)
		for (int64_t i = s->tails.data[s->tails.len - 1]; i >= 0; i = s->prev.data[i]) s->stay.data[i] = 1;
}

// item_keys computes the items' keys into s->keys: false when one threw
// (none kept, the rows left as they are).
static bool item_keys(for_t *s, ak_arr *list) {
	uint32_t n = list ? list->len : 0;
	s->keys.len = 0;
	ak_vec_reserve(&s->keys, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_val k = s->over_path ? ak_fsrc_key(&s->src, list, i, s->key_kind) : key_of(s, list->data[i], i);
		if (ak_error_pending()) {
			for (uint32_t j = 0; j < s->keys.len; j++) ak_release_val(s->keys.data[j], s->key_kind);
			s->keys.len = 0;
			return false;
		}
		s->keys.data[s->keys.len++] = k;
	}
	return true;
}

// reconcile makes the rows list's, keyed by s->keys.
static void reconcile(for_t *s, ak_arr *list) {
	ak_view *f = s->f;
	ak_ui_root *r = f->r;
	ak_kind ik = list ? ak_arr_kind(list) : AK_K_INT;
	uint32_t n = list ? list->len : 0;
	// The old rows, by key.
	uint32_t need = 16;
	while (need < s->rows.len * 2) need *= 2;
	if (s->table_cap < need) {
		ak_free(s->table);
		s->table = ak_alloc(need * sizeof(uint32_t));
		s->table_cap = need;
	} else {
		memset(s->table, 0, s->table_cap * sizeof(uint32_t));
	}
	s->taken.len = 0;
	ak_vec_reserve(&s->taken, s->rows.len);
	s->taken.len = s->rows.len;
	if (s->rows.len) memset(s->taken.data, 0, s->rows.len);
	for (uint32_t i = 0; i < s->rows.len; i++) {
		ak_row *rw = &s->rows.data[i];
		rw->old = i;
		uint32_t j = rw->hash & (s->table_cap - 1);
		while (s->table[j]) j = (j + 1) & (s->table_cap - 1);
		s->table[j] = i + 1;
	}
	s->next.len = 0;
	ak_vec_reserve(&s->next, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_val it = list->data[i];
		ak_val k = s->keys.data[i];
		uint32_t hash = (uint32_t)ak_val_hash(k, s->key_kind);
		uint32_t o = find_old(s, k, hash);
		if (o != UINT32_MAX) {
			s->taken.data[o] = 1;
			ak_row rw = s->rows.data[o];
			ak_release_val(k, s->key_kind);
			if (s->over_path) ak_row_move(&rw, it, ik, i);
			else ak_row_update(&rw, it, ik, i);
			ak_vec_push(&s->next, rw);
			continue;
		}
		ak_row rw = {.key = k, .hash = hash, .old = -1};
		if (s->over_path) ak_row_make_at(&rw, r, s->row, s->path, s->owner, s->src.acc, ak_current_effect(), it, ik, i, s->key_kind);
		else ak_row_make(&rw, r, s->row, s->path, s->owner, it, ik, i, s->key_kind);
		ak_vec_push(&s->next, rw);
	}
	s->keys.len = 0;  // released, or the new rows'

	// Removed rows.
	for (uint32_t i = 0; i < s->rows.len; i++) {
		if (s->taken.data[i]) continue;
		ak_row *rw = &s->rows.data[i];
		if (rw->el) ak_view_remove(f, rw->el, true);
		ak_row_free(rw, s->key_kind);
	}
	// Kept rows on the longest increasing run of old positions stay put;
	// the others move.
	lis(s);
	for (uint32_t i = 0; i < s->next.len; i++) {
		ak_row *rw = &s->next.data[i];
		if (rw->old >= 0 && !s->stay.data[i] && rw->el) ak_view_detach_native(rw->el);
	}
	// The fragment's children, in the new order: the kept rows' views
	// keep their references, the new ones' pass to it.
	f->kids.len = 0;
	ak_vec_reserve(&f->kids, s->next.len);
	for (uint32_t i = 0; i < s->next.len; i++) {
		ak_row *rw = &s->next.data[i];
		if (!rw->el) continue;
		f->kids.data[f->kids.len++] = rw->el;
		if (rw->old < 0) {
			rw->el->parent = f;
			ak_view_inherit_dir(f, rw->el);
		}
	}
	ak_view_mark_kids(f);
	for (uint32_t i = 0; i < s->next.len; i++) {
		ak_row *rw = &s->next.data[i];
		if (rw->el && (rw->old < 0 || !s->stay.data[i])) ak_view_attach_native(rw->el);
	}
	// next is the rows now.
	ak_vec_free(&s->rows);
	s->rows.data = s->next.data, s->rows.len = s->next.len, s->rows.cap = s->next.cap;
	s->next.data = NULL, s->next.len = s->next.cap = 0;
}

typedef struct {
	for_t *s;
	ak_arr *list;
} rec_ctx;

static void reconcile_fn(void *ctx) {
	rec_ctx *c = ctx;
	if (c->s->over_path || item_keys(c->s, c->list)) reconcile(c->s, c->list);
}

// for_path_run follows the order of a path's items: the length, and the
// keys (a field path's signal, or what a key's function reads of each
// item), not the items; then reconciles, untracked.
static void for_path_run(for_t *s) {
	if (ak_fsrc_follow(&s->src) < 0) {
		raise_in_run();
		return;
	}
	ak_arr *list = ak_fsrc_items(&s->src);
	if (ak_error_pending()) {
		raise_in_run();
		return;
	}
	if (!item_keys(s, list)) {
		ak_release(list);
		raise_in_run();
		return;
	}
	rec_ctx c = {s, list};
	ak_untrack(reconcile_fn, &c);
	ak_release(list);
	if (ak_error_pending()) raise_in_run();
}

static void for_run(void *ctx) {
	for_t *s = ctx;
	if (s->over_path) {
		for_path_run(s);
		return;
	}
	ak_val v = ak_call(s->items, NULL, 0);
	if (ak_error_pending()) {
		raise_in_run();
		return;
	}
	rec_ctx c = {s, v.p};
	ak_untrack(reconcile_fn, &c);
	ak_release(v.p);
	if (ak_error_pending()) raise_in_run();
}

static void for_drop(void *ctx) {
	for_t *s = ctx;
	for (uint32_t i = 0; i < s->rows.len; i++) ak_row_free(&s->rows.data[i], s->key_kind);
	ak_vec_free(&s->rows);
	ak_vec_free(&s->next);
	ak_vec_free(&s->taken);
	ak_vec_free(&s->tails);
	ak_vec_free(&s->prev);
	ak_vec_free(&s->stay);
	ak_vec_free(&s->keys);
	ak_free(s->table);
	if (s->over_path) ak_fsrc_free(&s->src);
	ak_release(s->f);
	ak_release(s->items);
	ak_release(s->key);
	ak_release(s->row);
	ak_path_release(s->path);
	if (s->owner) ak_owner_release(s->owner);
	ak_free(s);
}

static ak_view *for_new(ak_closure *items, ak_closure *key, void *acc, ak_val pkey, ak_closure *row, ak_kind key_kind) {
	ak_view *f = ak_frag_new();
	for_t *s = ak_alloc(sizeof *s);
	s->f = ak_retain(f);
	s->items = ak_retain(items);
	s->key = ak_retain(key);
	s->row = ak_retain(row);
	s->key_kind = key || pkey.p ? key_kind : AK_K_INT;
	if (acc) {
		s->over_path = true;
		ak_fsrc_init(&s->src, acc, pkey);
	}
	s->owner = creator();
	s->path = ak_path_capture();
	s->info = (ak_comp_info){.drop = for_drop, .label = "For"};
	ak_effect_new(for_run, s, AK_EFFECT_RENDER, &s->info);
	return f;
}

ak_view *ak_ui_for(ak_closure *items, ak_closure *key, ak_closure *row, ak_kind key_kind) { return for_new(items, key, NULL, (ak_val){0}, row, key_kind); }

ak_view *ak_ui_for_path(void *acc, ak_val key, ak_closure *row, ak_kind key_kind) { return for_new(NULL, NULL, acc, key, row, key_kind); }

// ---- Contexts ----

// Context keys are names, interned: the name's string is the key's
// address.
static AK_VEC(ak_str *) keys;

static const void *key_addr(ak_str *name) {
	for (uint32_t i = 0; i < keys.len; i++)
		if (ak_str_eq(keys.data[i], name)) return keys.data[i];
	ak_str *k = ak_str_new(name->data, name->len);
	ak_make_immortal(k);
	ak_vec_push(&keys, k);
	return k;
}

static void release_ctx(void *p) { ak_release(p); }

void ak_ui_provide(ak_str *key, ak_val value) {
	ak_owner *o = ak_current_owner();
	if (!o) ak_panic("ui: provide outside a component");
	ak_owner_set_context(o, key_addr(key), ak_retain(value.p));
	ak_on_cleanup(release_ctx, value.p);
}

ak_val ak_ui_use(ak_str *key) {
	void *v = NULL;
	if (!ak_context_lookup(key_addr(key), &v)) return (ak_val){0};
	return (ak_val){.p = ak_retain(v)};
}

// ---- Tasks ----

typedef struct {
	ak_closure *c;
	bool started;
	ak_comp_info info;
} task_t;

static void task_run(void *ctx) {
	task_t *t = ctx;
	if (t->started) return;
	t->started = true;
	void *tok = ak_untrack_begin();
	ak_val v = ak_call(t->c, NULL, 0);
	ak_untrack_end(tok);
	if (ak_error_pending()) {
		raise_in_run();
		return;
	}
	ak_release(v.p);  // the task's future: it runs on its own, owned here
}

static void task_drop(void *ctx) {
	task_t *t = ctx;
	ak_release(t->c);
	ak_free(t);
}

// ak_ui_task starts c in a user effect: once the component is built (at
// the end of the batch that builds it), owned by the effect's owner, so
// disposing the component cancels the task.
void ak_ui_task(ak_closure *c) {
	task_t *t = ak_alloc(sizeof *t);
	t->c = ak_retain(c);
	t->info = (ak_comp_info){.drop = task_drop, .label = "task"};
	ak_effect_new(task_run, t, AK_EFFECT_USER, &t->info);
}

// ---- Loaders ----
//
// let x = load f(…) is a loader: an effect calls the thunk, tracked, so
// it runs again when what the arguments read changes. A user effect:
// it first runs once the component is built (the thunk reads the
// component's values, set after the load is made). The thunk starts
// the call as a task under the effect's owner; the next run disposes that
// owner first, which cancels the last task.
//
// The loader holds what it shows (RFC 0014): it's pending only before
// its first answer; a run after that keeps the case it's in (ready with
// the last value, or failed) until it answers, and an answer equal to
// the last changes nothing. Two signals: node (the state, value and
// error: what a match and .value follow), written only when an answer
// changes them, and busy (running: whether a run is in flight, what
// .loading follows), written as a run starts and ends.

typedef struct ak_loader {
	ak_opaque h;
	ak_node node;    // state, value and error
	ak_node busy;    // running
	ak_node again;   // written by reload, read by each run
	uint8_t state;   // AK_LOAD_*
	uint8_t kind;    // the value's kind, once one came
	bool running;    // a run is in flight: fut is its task
	ak_val value;    // the latest value (kept while it runs again, and once it fails)
	void *error;     // when failed
	ak_future *fut;  // the run's task, while it's in flight
	ak_effect *eff;  // borrowed: NULL once disposed
	ak_closure *thunk;
	// Diagnostics (ak_diag_enabled): the load whose answer made the
	// content this one is in, until its first run says what it read
	// (load-waterfall); the timer of a hold, while it runs holding the
	// last answer (long-hold).
	struct ak_loader *cause;
	uint64_t hold;
} ak_loader;

// A hold this long, with nothing reading .loading, is said (long-hold):
// time the user waits, on asuka test's virtual clock a test's.
#define AK_LONG_HOLD_NS (2 * 1000000000LL)

// The load whose answer is being written, while what it shows is made.
static ak_loader *answering;

enum { AK_LOAD_PENDING, AK_LOAD_READY, AK_LOAD_FAILED };

typedef struct {
	ak_loader *l;   // retained
	ak_future *f;   // retained
} loader_ticket;

static void hold_end(ak_loader *l);
static void loader_describe(void *ctx, char *buf, size_t n);
static const void *loader_site(void *ctx);

static void loader_drop(ak_opaque *o) {
	ak_loader *l = (ak_loader *)o;
	hold_end(l);
	if (l->cause) ak_release(l->cause);
	ak_signal_drop(&l->node);
	ak_signal_drop(&l->busy);
	ak_signal_drop(&l->again);
	ak_release_val(l->value, (ak_kind)l->kind);
	if (l->error) ak_release(l->error);
	if (l->fut) ak_release(l->fut);
	if (l->thunk) ak_release(l->thunk);
}

static void loader_running(ak_loader *l, bool on) {
	if (l->running == on) return;
	l->running = on;
	ak_signal_write(&l->busy);
}

// loader_answer takes the run's value (borrowed): node is written only
// when the state or the value changes.
static void loader_answer(ak_loader *l, ak_val v, ak_kind k) {
	if (l->state == AK_LOAD_READY && l->kind == k && ak_val_eq(l->value, v, k)) return;
	ak_retain_val(v, k);
	ak_release_val(l->value, (ak_kind)l->kind);
	l->kind = (uint8_t)k;
	l->value = v;
	if (l->error) ak_release(l->error), l->error = NULL;
	l->state = AK_LOAD_READY;
	ak_signal_write(&l->node);
}

// loader_fail takes err: the error is news, unless it's the one shown.
static void loader_fail(ak_loader *l, void *err) {
	if (l->state == AK_LOAD_FAILED && l->error && ak_val_eq((ak_val){.p = l->error}, (ak_val){.p = err}, AK_K_REF)) {
		ak_release(err);
		return;
	}
	if (l->error) ak_release(l->error);
	l->error = err;
	l->state = AK_LOAD_FAILED;
	ak_signal_write(&l->node);
}

// loader_settle shows a run's answer: its value, or its error.
static void loader_settle(ak_loader *l, ak_future *f) {
	loader_running(l, false);
	if (f->state == AK_FUT_DONE) loader_answer(l, f->result, (ak_kind)f->kind);
	else if (!ak_is_cancel(f->error)) loader_fail(l, ak_retain(f->error));
}

// waterfall checks a load's first run, made in what another's answer
// showed (l->cause): its arguments, read now, don't read that answer, so
// it could have started beside it (load-waterfall).
static void waterfall(ak_loader *l) {
	ak_loader *cause = l->cause;
	l->cause = NULL;
	if (l->eff && cause->eff && !ak_comp_reads(l->eff, &cause->node) && ak_diag_first(loader_site(l), "load-waterfall")) {
		char mine[512], theirs[512], msg[1536];
		loader_describe(l, mine, sizeof mine);
		loader_describe(cause, theirs, sizeof theirs);
		snprintf(msg, sizeof msg, "%s waited for %s, but doesn't use its answer: it could have run at the same time; start it beside it, and pass the result down", mine, theirs);
		const char *where[2] = {mine, theirs};
		ak_diag("load-waterfall", msg, where, 2);
	}
	ak_release(cause);
}

// hold_fire: a run has held the last answer for AK_LONG_HOLD_NS. Nothing
// reading .loading (busy), nothing says so (long-hold).
static void hold_fire(void *ctx) {
	ak_loader *l = ctx;
	l->hold = 0;
	if (!l->running || !l->eff || ak_signal_watched(&l->busy) || !ak_diag_first(loader_site(l), "long-hold")) return;
	char me[512], msg[1536], name[64] = "it";
	loader_describe(l, me, sizeof me);
	const char *of = strstr(me, "load of ");
	if (of) {
		of += strlen("load of ");
		size_t k = strcspn(of, " ");
		snprintf(name, sizeof name, "%.*s", (int)k, of);
	}
	snprintf(msg, sizeof msg, "%s has shown its last answer for %lld s while loading the next, with nothing saying so: read %s.loading (dim it, show a ring), or show a spinner in its place", me, AK_LONG_HOLD_NS / 1000000000LL, name);
	const char *where[1] = {me};
	ak_diag("long-hold", msg, where, 1);
}

// hold_start times a run that holds the last answer; hold_end stops it
// (the run answered, or the load went). The timer holds no reference:
// the load stops it as it goes.
static void hold_start(ak_loader *l) { l->hold = ak_after(AK_LONG_HOLD_NS, hold_fire, l); }

static void hold_end(ak_loader *l) {
	if (!l->hold) return;
	ak_timer_cancel(l->hold);
	l->hold = 0;
}

static void loader_done(void *ctx) {
	loader_ticket *t = ctx;
	ak_loader *l = t->l;
	ak_future *f = t->f;
	if (l->fut == f) {  // else a later run replaced it, or it went
		ak_release(l->fut);
		l->fut = NULL;
		hold_end(l);
		if (AK_UNLIKELY(ak_diag_enabled)) {
			// What the answer shows is made in this batch: a load made
			// there waited for this one (load-waterfall).
			ak_loader *prev = answering;
			answering = l;
			ak_batch_begin();
			loader_settle(l, f);
			ak_batch_end();
			answering = prev;
		} else {
			loader_settle(l, f);
		}
	}
	ak_release(f);
	ak_release(l);
	ak_free(t);
}

static void loader_run_fn(void *ctx) {
	ak_loader *l = ctx;
	if (l->fut) ak_release(l->fut), l->fut = NULL;  // cancelled with the owner
	ak_signal_read(&l->again);
	ak_val v = ak_call(l->thunk, NULL, 0);
	if (AK_UNLIKELY(l->cause)) waterfall(l);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		loader_running(l, false);
		if (ak_is_cancel(err)) ak_release(err);
		else loader_fail(l, err);
		return;
	}
	ak_future *f = v.p;
	l->fut = ak_retain(f);
	loader_running(l, true);
	if (AK_UNLIKELY(ak_diag_enabled) && l->state != AK_LOAD_PENDING && !l->hold) hold_start(l);
	loader_ticket *t = ak_alloc(sizeof *t);
	t->l = ak_retain(l);
	t->f = f;  // the call's reference
	ak_future_wait(f, loader_done, t);
}

static void loader_effect_drop(void *ctx) {
	ak_loader *l = ctx;
	l->eff = NULL;
	hold_end(l);
	if (l->fut) ak_release(l->fut), l->fut = NULL;
	// No run is in flight once its owner went: none will answer.
	loader_running(l, false);
	// It never runs again: its thunk goes now, not with the loader, which
	// what the thunk holds may hold.
	ak_release(l->thunk);
	l->thunk = NULL;
	ak_release(l);
}

// loader_describe names a load as its source says it: "Profile's load of
// user at profile.asuka:8".
static void loader_describe(void *ctx, char *buf, size_t n) {
	ak_loader *l = ctx;
	if (l->thunk) ak_vm_where(l->thunk, NULL, buf, n);
	else snprintf(buf, n, "a load whose owner went");
}

static const void *loader_site(void *ctx) {
	ak_loader *l = ctx;
	return l->thunk ? ak_vm_where_site(l->thunk) : (const void *)l;
}

static const ak_comp_info loader_info = {.drop = loader_effect_drop, .label = "load", .describe = loader_describe, .site = loader_site};

ak_val ak_ui_load(ak_closure *thunk) {
	ak_loader *l = ak_opaque_new(AK_TAG_OPAQUE, sizeof *l, loader_drop);
	l->thunk = ak_retain(thunk);
	l->state = AK_LOAD_PENDING;
	if (AK_UNLIKELY(ak_diag_enabled) && answering && answering != l) l->cause = ak_retain(answering);
	l->eff = ak_effect_new(loader_run_fn, ak_retain(l), AK_EFFECT_USER, &loader_info);
	return (ak_val){.p = l};
}

static ak_loader *loader_arg(ak_val v, const char *name) {
	ak_opaque *o = v.p;
	if (!o || o->h.tag != AK_TAG_OPAQUE || o->drop != loader_drop) ak_panic("%s: not a loader", name);
	return (ak_loader *)o;
}

int64_t ak_ui_load_state(ak_val v) {
	ak_loader *l = loader_arg(v, "load.state");
	ak_signal_read(&l->node);
	return l->state;
}

// Whether a run is in flight: busy alone, so what reads it doesn't
// follow the value, nor a match it.
bool ak_ui_load_loading(ak_val v) {
	ak_loader *l = loader_arg(v, "load.loading");
	ak_signal_read(&l->busy);
	return l->running;
}

// The latest value, boxed when a scalar; none before the first.
ak_val ak_ui_load_value(ak_val v) {
	ak_loader *l = loader_arg(v, "load.value");
	ak_signal_read(&l->node);
	if (l->kind == AK_K_VOID) return (ak_val){0};
	if (l->kind == AK_K_REF) return (ak_val){.p = ak_retain(l->value.p)};
	return (ak_val){.p = ak_box_new((ak_kind)l->kind, l->value)};
}

ak_val ak_ui_load_error(ak_val v) {
	ak_loader *l = loader_arg(v, "load.error");
	ak_signal_read(&l->node);
	return (ak_val){.p = l->error ? ak_retain(l->error) : NULL};
}

void ak_ui_load_reload(ak_val v) {
	ak_loader *l = loader_arg(v, "load.reload");
	if (l->eff) ak_signal_write(&l->again);
}
