// react.c: the VM's state, on the signal graph (core/src/signal): cells,
// memos, accessors, effects, cleanups, and tasks tied to owners.
#include "vm/vm_int.h"
#include "signal/signal.h"
#include "vm/keep.h"
#include "vm/path.h"

#include <stdio.h>
#include <string.h>

// ---- Cells ----
//
// A state's cell holds its value. A state whose initializer reads what
// changes is a follow (rfcs/0012): a cell whose value a memo of its
// initializer replaces when what it read changes (core/src/signal's
// ak_follow). Reads and writes of a cell check which it is (CELL_FOLLOW,
// one branch); a follow's readers follow its ak_follow, not the cell's
// node.
//
// A cell's readers follow paths below it (rfcs/0013, vm/path.h): its
// own node is the root, read whole; paths, the nodes below it, made as
// they're read. A follow's paths are read whole: its readers follow its
// ak_follow, whose value its initializer replaces.

typedef struct {
	ak_opaque h;
	ak_node node;
	ak_val v;
	uint8_t kind;
	// Its name in its key (Picker.selected#…: selected), which an effect
	// writing it notes for the panic of a loop (ak_signal_note_write).
	uint16_t name_off, name_len;
	ak_str *key;  // retained; NULL for a cell made without one
	ak_pnode *paths;  // the paths read below it (NULL: none)
} cell;

enum {
	CELL_KEPT = 1,    // kept across reloads (keep.c)
	CELL_FOLLOW = 2,  // a follow: the cell is a follow's first field
	CELL_WIDE = 4,    // a write of it ran more than AK_WIDE_FANOUT (ak_diag_enabled)
};

// A write that runs more than this many bindings and lets is wide; the
// second wide write of a state is reported (wide-fanout).
#define AK_WIDE_FANOUT 1000

typedef struct follow follow;
struct follow {
	cell c;
	ak_follow *f;        // NULL once the graph let go of it
	ak_closure *update;  // the initializer's thunk; NULL once settled or its owner went
	ak_comp_info info;
	ak_box *selfbox;     // a scalar's current value as T?, reused while only it holds it
	bool self;           // the thunk takes the state's current value (T?)
	bool has;            // computed or set once
	bool kept;           // its value is one kept across a reload, which its first update keeps
};

static void cell_drop(ak_opaque *o) {
	cell *c = (cell *)o;
	if (o->h.flags & CELL_KEPT) ak_keep_forget(c);  // a keyed cell (keep.c)
	if (o->h.flags & CELL_FOLLOW) {
		ak_release(((follow *)c)->update);
		ak_release(((follow *)c)->selfbox);
	}
	ak_release(c->key);
	ak_path_free(&c->paths);
	ak_signal_drop(&c->node);
	ak_release_val(c->v, (ak_kind)c->kind);
}

static inline bool is_follow(const cell *c) { return (c->h.h.flags & CELL_FOLLOW) != 0; }

static ak_val follow_get(follow *w, bool track);

// cell_name keeps key as c's name: what's between its last . and its #.
static void cell_name(cell *c, ak_str *key) {
	if (!key) return;
	c->key = ak_retain(key);
	const char *s = key->data;
	size_t n = strcspn(s, "#"), start = 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] == '.') start = i + 1;
	c->name_off = (uint16_t)(start < 0xFFFF ? start : 0);
	c->name_len = (uint16_t)(n - c->name_off < 0xFFFF ? n - c->name_off : 0);
}

// cell_wrote notes, for an effect writing c, what it wrote.
static inline void cell_wrote(const cell *c) {
	if (c->key) ak_signal_note_write(c->key->data + c->name_off, c->name_len);
}

// A cell kept across reloads is looked up and stored through these
// (dev.c sets them while the dev loop runs; NULL otherwise).
bool (*ak_vm_keep_find)(ak_str *key, ak_kind k, ak_val *out);
void (*ak_vm_keep_add)(ak_str *key, void *cell);

ak_val ak_vm_cell_new(ak_val v, ak_kind k, ak_str *keep_key) {
	cell *c = ak_opaque_new(AK_TAG_CELL, sizeof(cell), cell_drop);
	ak_signal_born(&c->node);
	c->kind = (uint8_t)k;
	c->v = v;
	cell_name(c, keep_key);
	if (keep_key && ak_vm_keep_find) {
		ak_val kept;
		if (ak_vm_keep_find(keep_key, k, &kept)) {
			ak_release_val(c->v, k);
			c->v = kept;
		}
		ak_vm_keep_add(keep_key, c);
	}
	return (ak_val){.p = c};
}

ak_kind ak_vm_cell_kind(void *p) { return (ak_kind)((cell *)p)->kind; }

// follow_update runs the initializer, given the state's current value
// when it asks for it (none the first time), and takes what it returns
// unless that's equal to the value now.
static bool follow_update(void *ctx) {
	follow *w = ctx;
	if (!w->update) return false;  // its owner went (follow_gone)
	ak_kind k = (ak_kind)w->c.kind;
	ak_val arg = {0}, v;
	if (w->self) {
		// T?: a reference is its own optional; a scalar, boxed, in a box
		// the follow keeps while nothing else holds it (no allocation
		// each time it runs).
		if (w->has && k == AK_K_REF) arg = w->c.v;
		else if (w->has) {
			ak_box *b = w->selfbox;
			if (b && b->h.rc == 1) {
				b->v = w->c.v;
			} else {
				ak_release(b);
				b = w->selfbox = ak_box_new(k, w->c.v);
			}
			arg.p = b;
		}
		v = ak_call(w->update, &arg, 1);
	} else {
		v = ak_call(w->update, NULL, 0);
	}
	if (w->kept) {
		// The first run, which linked what the initializer reads: the
		// value kept across the reload stands (a write: an error the
		// initializer raised is gone with what it returned).
		w->kept = false;
		if (ak_error_pending()) ak_release(ak_error_take());
		else ak_release_val(v, k);
		w->has = true;
		return true;
	}
	if (ak_error_pending()) {
		ak_raise(ak_error_take());  // the follow holds it, as a memo does
		return true;
	}
	if (w->has && ak_val_eq(w->c.v, v, k)) {
		ak_release_val(v, k);
		return false;
	}
	ak_val old = w->c.v;
	w->c.v = v;
	w->has = true;
	ak_release_val(old, k);
	return true;
}

static void follow_comp_drop(void *ctx) {
	follow *w = ctx;
	w->f = NULL;
	ak_release(w);  // the graph's reference
}

// follow_gone runs as the owner the follow was made under is disposed:
// it never runs its initializer again, and lets go of it now (it may
// hold the component's env, which holds the follow).
static void follow_gone(void *ctx) {
	follow *w = ctx;
	ak_closure *c = w->update;
	w->update = NULL;
	ak_release(c);
	ak_release(w);
}

// follow_settle: an initializer that ran and read nothing (a parameter
// whose caller passed a constant) never runs again; the follow is a
// plain state from then on, and lets go of it.
static inline void follow_settle(follow *w) {
	if (w->update && w->f && ak_follow_settled(w->f)) {
		ak_closure *c = w->update;
		w->update = NULL;
		ak_release(c);
	}
}

// follow_store takes v as the follow's value (a write): brought up to
// date first, so that a write after what it follows changed stands.
static void follow_store(follow *w, ak_val v, bool always) {
	ak_kind k = (ak_kind)w->c.kind;
	if (w->f) ak_follow_refresh(w->f);
	follow_settle(w);
	if (!always && w->has && ak_val_eq(w->c.v, v, k)) {
		ak_release_val(v, k);
		return;
	}
	ak_val old = w->c.v;
	w->c.v = v;
	w->has = true;
	ak_release_val(old, k);
	cell_wrote(&w->c);
	if (w->f) ak_follow_set(w->f);
}

// follow_describe names a state that follows as its source says it:
// "Edit's state at edit.asuka:3".
static void follow_describe(void *ctx, char *buf, size_t n) {
	follow *w = ctx;
	ak_vm_where(w->update, w->update && w->update->invoke == ak_vm_closure_invoke ? NULL : "a prop's caller's value", buf, n);
}

ak_val ak_vm_follow_new(ak_closure *update, ak_kind k, ak_str *keep_key, bool self) {
	follow *w = ak_opaque_new(AK_TAG_CELL, sizeof(follow), cell_drop);
	w->c.h.h.flags |= CELL_FOLLOW;
	w->c.kind = (uint8_t)k;
	cell_name(&w->c, keep_key);
	w->update = ak_retain(update);
	w->self = self;
	w->info = (ak_comp_info){.drop = follow_comp_drop, .label = update->invoke == ak_vm_closure_invoke ? ak_func_name(update->fn) : "a prop's caller's value", .describe = follow_describe};
	ak_retain(w);  // the graph's, until the computation is dropped
	w->f = ak_follow_new(follow_update, w, &w->info);
	if (ak_current_owner()) ak_on_cleanup(follow_gone, ak_retain(w));
	if (keep_key && ak_vm_keep_find) {
		// A value kept across a reload is a write: it stands until what
		// the initializer reads changes. The initializer can't run yet
		// (it reads its component's env, whole only once the component's
		// function has made every member), so the value waits for its
		// first run, at the first read, as a write would after it.
		ak_val kept;
		if (ak_vm_keep_find(keep_key, k, &kept)) {
			w->c.v = kept;
			w->kept = true;
		}
		ak_vm_keep_add(keep_key, w);
	}
	return (ak_val){.p = w};
}

// follow_get reads a follow, brought up to date: tracked (a binding
// follows it) or not. Its initializer's error is thrown in the reader.
static ak_val follow_get(follow *w, bool track) {
	if (w->f) {
		bool ok = track ? ak_follow_read(w->f) : (ak_follow_refresh(w->f), ak_memo_error(w->f) == NULL);
		follow_settle(w);
		if (!ok) {
			ak_throw(ak_retain(ak_memo_error(w->f)));
			return (ak_val){0};
		}
	}
	ak_retain_val(w->c.v, (ak_kind)w->c.kind);
	return w->c.v;
}

ak_val ak_vm_cell_get(void *p, bool track) {
	cell *c = p;
	if (AK_UNLIKELY(is_follow(c))) return follow_get((follow *)c, track);
	if (track) ak_signal_read(&c->node);
	ak_retain_val(c->v, (ak_kind)c->kind);
	return c->v;
}

static void cell_put(void *p, ak_val v);
static void cell_putp(void *p, ak_val v, const uint8_t *path, uint32_t n, const ak_val *args);

static void cell_set(void *p, ak_val v) {
	cell *c = p;
	if (AK_UNLIKELY(is_follow(c))) {
		follow_store((follow *)c, v, false);
		return;
	}
	if (ak_val_eq(c->v, v, (ak_kind)c->kind)) {
		ak_release_val(v, (ak_kind)c->kind);
		return;
	}
	ak_val old = c->v;
	c->v = v;
	cell_wrote(c);
	if (c->paths) ak_path_set(&c->paths, &c->node, old, v, (ak_kind)c->kind);
	else ak_signal_write(&c->node);
	ak_release_val(old, (ak_kind)c->kind);
}

ak_val ak_vm_cell_take(void *p) {
	cell *c = p;
	if (AK_UNLIKELY(is_follow(c)) && ((follow *)c)->f) {
		// Changed in place: from its current value.
		ak_follow_refresh(((follow *)c)->f);
		follow_settle((follow *)c);
	}
	ak_val v = c->v;
	if (c->kind == AK_K_REF) c->v.p = NULL;
	return v;
}

static void cell_put(void *p, ak_val v) {
	cell *c = p;
	if (AK_UNLIKELY(is_follow(c))) {
		// What take moved out, back: the follow is up to date already.
		follow *w = (follow *)c;
		ak_release_val(c->v, (ak_kind)c->kind);
		c->v = v;
		w->has = true;
		cell_wrote(c);
		if (w->f) ak_follow_set(w->f);
		return;
	}
	ak_release_val(c->v, (ak_kind)c->kind);
	c->v = v;
	cell_wrote(c);
	if (c->paths) {
		static const uint8_t all[1] = {AK_PS_ALL};
		ak_path_put(&c->paths, &c->node, v, (ak_kind)c->kind, all, 1, NULL);
	} else {
		ak_signal_write(&c->node);
	}
}

ak_val ak_vm_cell_path(void *p, const uint8_t *path, uint32_t n, const ak_val *args, bool len) {
	cell *c = p;
	ak_val out = {0};
	if (AK_UNLIKELY(is_follow(c))) {
		// Read whole (its ak_follow), and the path in its value.
		ak_val v = follow_get((follow *)c, true);
		if (ak_error_pending()) return out;
		ak_path_read(NULL, NULL, v, (ak_kind)c->kind, path, n, args, len, false, &out);
		ak_release_val(v, (ak_kind)c->kind);
		return out;
	}
	ak_path_read(&c->paths, &c->node, c->v, (ak_kind)c->kind, path, n, args, len, ak_tracking(), &out);
	return out;
}

static void cell_putp(void *p, ak_val v, const uint8_t *path, uint32_t n, const ak_val *args) {
	cell *c = p;
	if (AK_UNLIKELY(is_follow(c))) {
		cell_put(p, v);
		return;
	}
	ak_release_val(c->v, (ak_kind)c->kind);
	c->v = v;
	cell_wrote(c);
	ak_path_put(&c->paths, &c->node, v, (ak_kind)c->kind, path, n, args);
}

// ---- Wide writes (wide-fanout) ----
//
// With ak_diag_enabled, a write of a state counts the effects it queues
// (ak_signal_enqueued's growth across it): one that runs more than
// AK_WIDE_FANOUT bindings and lets is wide, and the second wide write of
// a state is reported once, with where it's written and the sites of the
// readers that ran most.

typedef struct {
	const void *site;
	char name[512];
	uint32_t n;
} reader_tally;

typedef struct {
	reader_tally top[4];
	uint32_t ntop;
} tally;

static void tally_reader(void *ctx, const ak_comp *c) {
	tally *t = ctx;
	const void *site = ak_comp_site(c);
	for (uint32_t i = 0; i < t->ntop; i++)
		if (t->top[i].site == site) {
			t->top[i].n++;
			return;
		}
	if (t->ntop < 4) {
		reader_tally *r = &t->top[t->ntop++];
		r->site = site;
		r->n = 1;
		ak_comp_name(c, r->name, sizeof r->name);
	}
}

static void wide_write(cell *c, uint64_t ran) {
	if (!(c->h.h.flags & CELL_WIDE)) {
		c->h.h.flags |= CELL_WIDE;  // once is a list's first build, or a reset
		return;
	}
	char here[512];
	ak_vm_here(here, sizeof here);
	if (!ak_diag_first(c->key ? (const void *)c->key : (const void *)c, "wide-fanout")) return;
	tally t = {0};
	ak_signal_each_reader(&c->node, tally_reader, &t);
	char what[64];
	if (c->key) snprintf(what, sizeof what, "%.*s", (int)c->name_len, c->key->data + c->name_off);
	else snprintf(what, sizeof what, "a state");
	char msg[1024];
	snprintf(msg, sizeof msg, "a write of %s ran %llu bindings and lets, again: what reads %s whole runs again on every change; read what each one shows by its path, or a set's contains", what, (unsigned long long)ran, what);
	char lines[4][528];
	const char *where[5] = {here};
	int nwhere = 1;
	for (uint32_t i = 0; i < t.ntop; i++) {
		snprintf(lines[i], sizeof lines[i], "%s (%u)", t.top[i].name, t.top[i].n);
		where[nwhere++] = lines[i];
	}
	ak_diag("wide-fanout", msg, where, nwhere);
}

static AK_NOINLINE void diag_set(cell *c, int how, ak_val v, const uint8_t *path, uint32_t n, const ak_val *args) {
	uint64_t before = ak_signal_enqueued();
	if (how == 0) cell_set(c, v);
	else if (how == 1) cell_put(c, v);
	else cell_putp(c, v, path, n, args);
	uint64_t ran = ak_signal_enqueued() - before;
	if (ran > AK_WIDE_FANOUT) wide_write(c, ran);
}

void ak_vm_cell_set(void *p, ak_val v) {
	if (AK_UNLIKELY(ak_diag_enabled)) diag_set(p, 0, v, NULL, 0, NULL);
	else cell_set(p, v);
}

void ak_vm_cell_put(void *p, ak_val v) {
	if (AK_UNLIKELY(ak_diag_enabled)) diag_set(p, 1, v, NULL, 0, NULL);
	else cell_put(p, v);
}

void ak_vm_cell_putp(void *p, ak_val v, const uint8_t *path, uint32_t n, const ak_val *args) {
	if (AK_UNLIKELY(ak_diag_enabled)) diag_set(p, 2, v, path, n, args);
	else cell_putp(p, v, path, n, args);
}

void ak_vm_cell_keys(void *p, const uint8_t *path, uint32_t n, const ak_val *args, const uint16_t *fields, uint16_t nf) {
	cell *c = p;
	if (AK_UNLIKELY(is_follow(c))) {
		ak_val v = follow_get((follow *)c, true);
		ak_release_val(v, (ak_kind)c->kind);
		return;
	}
	if (ak_tracking()) ak_path_keys(&c->paths, c->v, (ak_kind)c->kind, path, n, args, fields, nf);
}

uint32_t ak_vm_cell_nodes(void *p) { return ak_path_nodes(((cell *)p)->paths); }

// ak_vm_cell_read_by reports whether c reads the cell, whole or by a
// path below it.
bool ak_vm_cell_read_by(void *p, const ak_comp *c) {
	cell *x = p;
	return ak_comp_reads(c, &x->node) || ak_path_read_by(x->paths, c);
}

// ---- Errors between the VM and the graph ----
//
// The VM unwinds with a pending error (ak_throw); the graph delivers an
// error a computation raised to a catcher (ak_raise). Where VM code runs
// as a computation, an error it leaves pending is raised.

static void raise_pending(void) {
	if (!ak_error_pending()) return;
	void *err = ak_error_take();
	if (ak_is_cancel(err)) {
		ak_release(err);
		return;
	}
	if (ak_failed()) {
		ak_release(err);  // raised already (a memo's error, read)
		return;
	}
	ak_raise(err);
}

static const char *label_of(ak_closure *c) {
	return c->invoke == ak_vm_closure_invoke ? ak_func_name(c->fn) : "closure";
}

// ak_vm_where names the site of the program a closure is, as its source
// says it: "Picker's effect at picker.asuka:12", from its function's name
// (Picker/effect: its owner, then what it is) and the line it's made at.
// what, when set, says what it is instead ("text binding"). Nothing is
// allocated: it's made when it's said.
void ak_vm_where(const ak_closure *c, const char *what, char *buf, size_t n) {
	if (!c || c->invoke != ak_vm_closure_invoke) {
		snprintf(buf, n, "%s", what ? what : "a closure of the runtime's");
		return;
	}
	const ak_func *f = c->fn;
	const char *name = f->name->data;
	size_t owner = strcspn(name, "/");
	if (!what) {
		const char *last = strrchr(name, '/');
		what = last ? last + 1 : name;
	}
	uint32_t line = f->nlines ? f->lines[0].line : 0;
	if (name[owner]) snprintf(buf, n, "%.*s's %s at %s:%u", (int)owner, name, what, f->file->data, line);
	else snprintf(buf, n, "%s at %s:%u", what, f->file->data, line);
}

// ak_vm_where_site is what a site's reports are kept once by: its
// closure's function, one per place in the source.
const void *ak_vm_where_site(const ak_closure *c) {
	return c && c->invoke == ak_vm_closure_invoke ? c->fn : (const void *)c;
}

// ---- Memos ----

typedef struct {
	ak_opaque h;
	ak_memo *m;
	ak_closure *c;
	ak_val v;
	uint8_t kind;
	bool has;
	ak_comp_info info;
} memo;

static bool memo_update(void *ctx) {
	memo *m = ctx;
	if (!m->c) return false;  // its owner went (memo_gone)
	ak_val v = ak_call(m->c, NULL, 0);
	if (ak_error_pending()) {
		ak_raise(ak_error_take());  // the memo holds it (ak_memo_error)
		return true;
	}
	if (m->has && ak_val_eq(m->v, v, (ak_kind)m->kind)) {
		ak_release_val(v, (ak_kind)m->kind);
		return false;
	}
	ak_release_val(m->v, (ak_kind)m->kind);
	m->v = v;
	m->has = true;
	return true;
}

static void memo_comp_drop(void *ctx) {
	memo *m = ctx;
	m->m = NULL;
	ak_release(m);  // the graph's reference
}

// memo_gone runs as the owner the memo was made under is disposed (the
// component or store it's in goes): it never runs again, and its closure
// and value go now, not with the memo, which they may hold (a store's
// memo, whose closure holds the store), or which may outlive the
// computation's drop (a view it computed retains the computation's
// owner). Read after, it throws.
static void memo_gone(void *ctx) {
	memo *m = ctx;
	ak_closure *c = m->c;
	m->c = NULL;
	ak_release(c);
	if (m->has) {
		ak_val v = m->v;
		m->v = (ak_val){0};
		m->has = false;
		ak_release_val(v, (ak_kind)m->kind);
	}
	ak_release(m);
}

static void memo_drop(ak_opaque *o) {
	memo *m = (memo *)o;
	ak_release_val(m->v, (ak_kind)m->kind);
	ak_release(m->c);
}

// memo_describe names a let as its source says it: "Counter's let total
// at app.asuka:4".
static void memo_describe(void *ctx, char *buf, size_t n) {
	memo *m = ctx;
	if (m->c) ak_vm_where(m->c, NULL, buf, n);
	else snprintf(buf, n, "a let whose owner went");
}

static const void *memo_site(void *ctx) {
	memo *m = ctx;
	return m->c ? ak_vm_where_site(m->c) : (const void *)m;
}

ak_val ak_vm_memo_new(ak_closure *c, ak_kind k) {
	memo *m = ak_opaque_new(AK_TAG_MEMO, sizeof(memo), memo_drop);
	m->kind = (uint8_t)k;
	m->c = ak_retain(c);
	m->info = (ak_comp_info){.drop = memo_comp_drop, .label = label_of(c), .describe = memo_describe, .site = memo_site};
	ak_retain(m);  // the graph's, until the computation is dropped
	m->m = ak_memo_new(memo_update, m, &m->info);
	if (ak_current_owner()) ak_on_cleanup(memo_gone, ak_retain(m));
	return (ak_val){.p = m};
}

ak_val ak_vm_memo_get(void *p) {
	memo *m = p;
	if (m->m && !ak_memo_read(m->m)) {
		// It failed: reading it throws its error in the VM too.
		ak_throw(ak_retain(ak_memo_error(m->m)));
		return (ak_val){0};
	}
	if (!m->has) {
		// Disposed before it ever computed.
		ak_throw_msg("read of a disposed memo (%s)", m->info.label);
		return (ak_val){0};
	}
	ak_retain_val(m->v, (ak_kind)m->kind);
	return m->v;
}

// ---- Accessors ----

enum { ACC_VALUE, ACC_FUNC, ACC_CELL, ACC_MEMO, ACC_MODEL, ACC_PATH };

typedef struct {
	ak_opaque h;
	uint8_t mode, kind;
	ak_val v;  // the value, or the closure, cell, memo or path
} acc;

// An accessor of a path (rfcs/0013, ACCP): what's at the path below a
// cell, or below what another accessor reads (one that isn't a path's:
// read whole, then the path in its value). Its arguments are values,
// but for those that are cells, read as it's read: a row's index, so a
// row that moves reads its item at its new place.
// A row of a for over a path: what its accessor needs to read its item
// where the for put it. Between a write that moves or removes items and
// the for's next run (the rest of a handler that wrote), a row's index
// is the for's last; an untracked read then reads the element as the
// for last placed it (as a row's item cell held it before rfcs/0013),
// as one does once the row is gone. Tracked reads run after the for
// (bindings are made after it), so they read the place.
typedef struct {
	ak_effect *eff;  // the for's, NULL once the row is gone (rows go before their for's effect)
	ak_val last;     // the element, as the for last placed it
	uint8_t ik;
	void *parent;    // the row (its accessor) this row's for is in, if any: retained
} rowinfo;

typedef struct {
	void *root;      // a cell, or an accessor: retained
	uint8_t *steps;  // the path, spelled (vm/path.h)
	uint32_t n, nargs;
	ak_val *args;
	uint8_t *kinds;  // each argument's kind; AK_K_VOID: a cell of an int
	void *row;       // a row's item (its accessor: this one, or retained), or a path below one
	uint32_t row_n, row_a;  // the steps and arguments up to the row's item
	rowinfo ri;      // a row's item's own
	bool own_row;    // row is this accessor: not retained
} apath;

// apath_free lets go of what ap holds (it's its accessor's: one
// allocation, the accessor, then ap, its arguments, its kinds and its
// steps).
static void apath_free(apath *p) {
	if (p->row && !p->own_row) ak_release(p->row);
	ak_release_val(p->ri.last, (ak_kind)p->ri.ik);
	ak_release(p->ri.parent);
	ak_release(p->root);
	for (uint32_t i = 0; i < p->nargs; i++) {
		if (p->kinds[i] == AK_K_VOID) ak_release(p->args[i].p);
		else ak_release_val(p->args[i], (ak_kind)p->kinds[i]);
	}
}

static void acc_drop(ak_opaque *o) {
	acc *a = (acc *)o;
	if (a->mode == ACC_VALUE) ak_release_val(a->v, (ak_kind)a->kind);
	else if (a->mode == ACC_PATH) apath_free(a->v.p);
	else ak_release(a->v.p);
}

static acc *acc_new(uint8_t mode, ak_kind k, ak_val v) {
	acc *a = ak_opaque_new(AK_TAG_ACC, sizeof(acc), acc_drop);
	a->mode = mode;
	a->kind = (uint8_t)k;
	a->v = v;
	return a;
}

ak_val ak_vm_acc_value(ak_val v, ak_kind k) { return (ak_val){.p = acc_new(ACC_VALUE, k, v)}; }
ak_val ak_vm_acc_func(ak_closure *c, ak_kind k) { return (ak_val){.p = acc_new(ACC_FUNC, k, (ak_val){.p = ak_retain(c)})}; }

ak_val ak_vm_acc_cell(void *p) {
	ak_obj *o = p;
	if (o->tag == AK_TAG_MEMO) return (ak_val){.p = acc_new(ACC_MEMO, (ak_kind)((memo *)p)->kind, (ak_val){.p = ak_retain(p)})};
	if (o->tag == AK_TAG_ACC) return (ak_val){.p = ak_retain(p)};
	return (ak_val){.p = acc_new(ACC_CELL, (ak_kind)((cell *)p)->kind, (ak_val){.p = ak_retain(p)})};
}

bool ak_vm_dbg_peek(void *p, ak_val *v, ak_kind *k) {
	const ak_obj *o = p;
	switch (o->tag) {
	case AK_TAG_CELL: {
		const cell *c = p;
		if (is_follow(c) && !((const follow *)c)->has) return false;
		*v = c->v;
		*k = (ak_kind)c->kind;
		return true;
	}
	case AK_TAG_MEMO: {
		const memo *m = p;
		if (!m->has) return false;
		*v = m->v;
		*k = (ak_kind)m->kind;
		return true;
	}
	case AK_TAG_ACC: {
		const acc *a = p;
		if (a->mode == ACC_VALUE) {
			*v = a->v;
			*k = (ak_kind)a->kind;
			return true;
		}
		if (a->mode == ACC_CELL || a->mode == ACC_MEMO) return ak_vm_dbg_peek(a->v.p, v, k);
		return false;
	}
	default:
		return false;
	}
}

bool ak_vm_dbg_read(void *p, ak_val *v, ak_kind *k) {
	const ak_obj *o = p;
	void *tok = ak_untrack_begin();
	switch (o->tag) {
	case AK_TAG_CELL:
		*k = (ak_kind)((const cell *)p)->kind;
		*v = ak_vm_cell_get(p, false);
		break;
	case AK_TAG_MEMO:
		*k = (ak_kind)((const memo *)p)->kind;
		*v = ak_vm_memo_get(p);
		break;
	case AK_TAG_ACC:
		*k = (ak_kind)((const acc *)p)->kind;
		*v = ak_vm_acc_get(p);
		break;
	default:
		ak_untrack_end(tok);
		return false;
	}
	ak_untrack_end(tok);
	return !ak_error_pending();
}

static ak_val model_get(void *m);
static ak_val apath_read(apath *ap, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args);

ak_val ak_vm_acc_get(void *p) {
	acc *a = p;
	switch (a->mode) {
	case ACC_PATH:
		return apath_read(a->v.p, (ak_kind)a->kind, NULL, 0, NULL);
	case ACC_VALUE:
		ak_retain_val(a->v, (ak_kind)a->kind);
		return a->v;
	case ACC_FUNC:
		return ak_call(a->v.p, NULL, 0);
	case ACC_CELL:
		return ak_vm_cell_get(a->v.p, true);
	case ACC_MODEL:
		return model_get(a->v.p);
	default:
		return ak_vm_memo_get(a->v.p);
	}
}

// ---- Models: a prop its component assigns ----
//
// A prop with its change event (open, openChange) that its component
// assigns is read through a model, an accessor of the component's: bound
// by the caller (bind open: s), it reads the caller's accessor, and the
// component's assignment emits openChange, whose bind writes s; unbound,
// it's a follow of the caller's accessor (rfcs/0012): the caller's value
// until the component assigns it, and the caller's again when that
// changes. A caller's constant settles it: a state of the component's.

typedef struct {
	ak_opaque h;
	acc *inner;  // the caller's accessor
	void *own;   // unbound: the follow of it
	uint8_t kind;
	bool bound;
} model;

static void model_drop(ak_opaque *o) {
	model *m = (model *)o;
	ak_release(m->own);
	ak_release(m->inner);
}

static ak_val model_get(void *p) {
	model *m = p;
	if (m->bound) return ak_vm_acc_get(m->inner);
	return ak_vm_cell_get(m->own, true);
}

// The follow's initializer: the caller's accessor, read.
static const uint8_t model_read_caps[1] = {AK_K_REF};
static ak_val model_read(ak_closure *c, ak_val *args, uint32_t nargs) {
	AK_UNUSED(args);
	AK_UNUSED(nargs);
	return ak_vm_acc_get(c->caps[0].p);
}

ak_val ak_vm_model_new(void *inner, bool bound) {
	acc *in = inner;
	if (!in || ((ak_obj *)in)->tag != AK_TAG_ACC) ak_panic("ui.model: a prop's accessor, not %p", inner);
	model *m = ak_opaque_new(AK_TAG_OPAQUE, sizeof(model), model_drop);
	m->kind = in->kind;
	m->inner = ak_retain(in);
	m->bound = bound;
	if (!bound) {
		ak_closure *read = ak_closure_new(model_read, NULL, 1, model_read_caps);
		read->caps[0].p = ak_retain(in);
		m->own = ak_vm_follow_new(read, (ak_kind)m->kind, NULL, false).p;
		ak_release(read);
	}
	return (ak_val){.p = acc_new(ACC_MODEL, (ak_kind)m->kind, (ak_val){.p = m})};
}

static model *model_of(void *p, const char *what) {
	acc *a = p;
	if (!a || ((ak_obj *)a)->tag != AK_TAG_ACC || a->mode != ACC_MODEL) ak_panic("%s: a model, not %p", what, p);
	return a->v.p;
}

bool ak_vm_model_bound(void *p) { return model_of(p, "ui.model_bound")->bound; }

void ak_vm_model_set(void *p, ak_val v) {
	model *m = model_of(p, "ui.model_set");
	if (m->bound) ak_panic("ui.model_set: a bound prop's value is its caller's");
	ak_retain_val(v, (ak_kind)m->kind);
	ak_vm_cell_set(m->own, v);
}

// ---- Accessors of paths (rfcs/0013) ----

static acc *acc_of(void *p) {
	ak_obj *o = p;
	return o && o->tag == AK_TAG_ACC ? (acc *)p : NULL;
}

static rowinfo *row_info(void *row) { return &((apath *)((acc *)row)->v.p)->ri; }

void ak_vm_acc_row_set(void *row, ak_val item, ak_kind ik) {
	rowinfo *r = row_info(row);
	ak_retain_val(item, ik);
	ak_release_val(r->last, (ak_kind)r->ik);
	r->last = item;
	r->ik = (uint8_t)ik;
}

void ak_vm_acc_row_gone(void *row) { row_info(row)->eff = NULL; }  // what reads it after reads its last item

// row_stale reports whether a row's place may not be its item's now: its
// for (or one it's in) has yet to run since its items changed, or the
// row is gone.
static bool row_stale(void *row) {
	for (; row; row = row_info(row)->parent) {
		ak_effect *e = row_info(row)->eff;
		if (!e || ak_effect_stale(e)) return true;
	}
	return false;
}

// step_arg is the kind of the argument path step s takes, or AK_K_VOID
// for none; *w is the bytes it spans.
static ak_kind step_arg(const uint8_t *p, uint32_t n, uint32_t j, uint32_t *w) {
	switch (p[j]) {
	case AK_PS_FIELD:
	case AK_PS_OFIELD:
		*w = 3;
		return AK_K_VOID;
	case AK_PS_INDEX:
	case AK_PS_GET:
	case AK_PS_EACH:
		*w = 1;
		return AK_K_INT;
	case AK_PS_KEY:
	case AK_PS_VALUE:
	case AK_PS_HAS:
	case AK_PS_MEMBER:
		*w = 2;
		return j + 1 < n ? (ak_kind)p[j + 1] : AK_K_INT;
	}
	*w = 1;
	return AK_K_VOID;
}

// path_kinds fills kinds (when not NULL) with the kinds of the
// arguments p's steps take, and counts them.
static uint32_t path_kinds(const uint8_t *p, uint32_t n, uint8_t *kinds) {
	uint32_t k = 0, w;
	for (uint32_t j = 0; j < n; j += w) {
		ak_kind a = step_arg(p, n, j, &w);
		if (a != AK_K_VOID) {
			if (kinds) kinds[k] = (uint8_t)a;
			k++;
		}
	}
	return k;
}

// acc_path_new makes an accessor (of kind k) of the path p (args its
// arguments, index a cell of an int as its last argument when not NULL)
// below base: a cell, or an accessor, whose path it continues when it's
// a path's.
static acc *acc_path_new(void *base, const uint8_t *p, uint32_t n, const ak_val *args, void *index, ak_kind k) {
	acc *b = acc_of(base);
	const apath *bp = b && b->mode == ACC_PATH ? b->v.p : NULL;
	uint32_t bn = bp ? bp->n : 0, ba = bp ? bp->nargs : 0;
	uint32_t na = path_kinds(p, n, NULL);
	uint32_t tn = bn + n, ta = ba + na;
	// One allocation: the accessor, its path, its arguments, their
	// kinds, its steps.
	size_t at = (sizeof(acc) + 7) & ~(size_t)7;
	acc *a = ak_opaque_new(AK_TAG_ACC, at + sizeof(apath) + sizeof(ak_val) * ta + ta + tn, acc_drop);
	a->mode = ACC_PATH;
	a->kind = (uint8_t)k;
	apath *ap = (apath *)((char *)a + at);
	a->v.p = ap;
	if (bp) ap->root = ak_retain(bp->root);
	else if (b && b->mode == ACC_CELL) ap->root = ak_retain(b->v.p);
	else ap->root = ak_retain(base);
	if (bp && bp->row) {
		ap->row = ak_retain(bp->row);  // (never this one: it's being made)
		ap->row_n = bp->row_n;
		ap->row_a = bp->row_a;
	}
	ap->n = tn;
	ap->nargs = ta;
	ap->args = (ak_val *)(ap + 1);
	ap->kinds = (uint8_t *)(ap->args + ta);
	ap->steps = ap->kinds + ta;
	if (bn) memcpy(ap->steps, bp->steps, bn);
	if (n) memcpy(ap->steps + bn, p, n);
	for (uint32_t i = 0; i < ba; i++) {
		ap->args[i] = bp->args[i];
		ap->kinds[i] = bp->kinds[i];
		if (ap->kinds[i] == AK_K_VOID) ak_retain(ap->args[i].p);
		else ak_retain_val(ap->args[i], (ak_kind)ap->kinds[i]);
	}
	path_kinds(p, n, ap->kinds + ba);
	for (uint32_t i = 0; i < na; i++) {
		if (index && i == na - 1) {
			ap->args[ba + i].p = ak_retain(index);
			ap->kinds[ba + i] = AK_K_VOID;
			continue;
		}
		ap->args[ba + i] = args[i];
		ak_retain_val(args[i], (ak_kind)ap->kinds[ba + i]);
	}
	return a;
}

ak_val ak_vm_acc_path(void *base, const uint8_t *p, uint32_t n, const ak_val *args, ak_kind k) {
	return (ak_val){.p = acc_path_new(base, p, n, args, NULL, k)};
}

ak_val ak_vm_acc_row(void *base, void *index, ak_kind k, ak_effect *eff) {
	static const uint8_t step[1] = {AK_PS_INDEX};
	acc *a = acc_path_new(base, step, 1, NULL, index, k);
	apath *ap = a->v.p;
	// Its row's: the row it's in is the base's.
	void *parent = ap->row;
	ap->row = a;
	ap->own_row = true;
	ap->row_n = ap->n;
	ap->row_a = ap->nargs;
	ap->ri.eff = eff;
	ap->ri.parent = parent;  // the reference the base's row took
	return (ak_val){.p = a};
}

// read_below reads the path p below what a (not a path's) reads: it,
// whole, then the path in its value, untracked.
static ak_val read_below(acc *a, const uint8_t *p, uint32_t n, const ak_val *args) {
	ak_val v = ak_vm_acc_get(a);
	if (ak_error_pending()) return (ak_val){0};
	ak_val out = {0};
	ak_path_read(NULL, NULL, v, (ak_kind)a->kind, p, n, args, false, false, &out);
	ak_release_val(v, (ak_kind)a->kind);
	return out;
}

enum { PATH_STEPS = 256, PATH_ARGS = 32 };

// row_read reads ap's path past its row's item, then p, in the item as
// its for last placed it: an untracked read while the row's place may
// not be its item's (row_stale).
static ak_val row_read(apath *ap, const uint8_t *p, uint32_t n, const ak_val *args) {
	uint8_t sbuf[PATH_STEPS];
	ak_val abuf[PATH_ARGS];
	uint32_t na = p ? path_kinds(p, n, NULL) : 0;
	uint32_t sn = ap->n - ap->row_n, sa = ap->nargs - ap->row_a;
	uint32_t tn = sn + n, ta = sa + na;
	uint8_t *steps = tn <= PATH_STEPS ? sbuf : ak_alloc(tn ? tn : 1);
	ak_val *all = ta <= PATH_ARGS ? abuf : ak_alloc(sizeof(ak_val) * (ta ? ta : 1));
	if (sn) memcpy(steps, ap->steps + ap->row_n, sn);
	if (n) memcpy(steps + sn, p, n);
	for (uint32_t i = 0; i < sa; i++) {
		uint32_t j = ap->row_a + i;
		all[i] = ap->kinds[j] == AK_K_VOID ? ak_vm_cell_get(ap->args[j].p, false) : ap->args[j];
	}
	for (uint32_t i = 0; i < na; i++) all[sa + i] = args[i];
	ak_val out = {0};
	rowinfo *ri = row_info(ap->row);
	ak_path_read(NULL, NULL, ri->last, (ak_kind)ri->ik, steps, tn, all, false, false, &out);
	if (steps != sbuf) ak_free(steps);
	if (all != abuf) ak_free(all);
	return out;
}

// apath_read reads ap's path, then p (args its arguments), below its
// root: its cells' arguments read, tracked.
static ak_val apath_read(apath *ap, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args) {
	AK_UNUSED(k);
	if (ap->row && !ak_tracking() && row_stale(ap->row)) return row_read(ap, p, n, args);
	uint8_t sbuf[PATH_STEPS];
	ak_val abuf[PATH_ARGS];
	uint32_t na = p ? path_kinds(p, n, NULL) : 0;
	uint32_t tn = ap->n + n, ta = ap->nargs + na;
	uint8_t *steps = tn <= PATH_STEPS ? sbuf : ak_alloc(tn);
	ak_val *all = ta <= PATH_ARGS ? abuf : ak_alloc(sizeof(ak_val) * ta);
	memcpy(steps, ap->steps, ap->n);
	if (n) memcpy(steps + ap->n, p, n);
	for (uint32_t i = 0; i < ap->nargs; i++) {
		if (ap->kinds[i] == AK_K_VOID) all[i] = ak_vm_cell_get(ap->args[i].p, true);  // an int: nothing to release
		else all[i] = ap->args[i];
	}
	for (uint32_t i = 0; i < na; i++) all[ap->nargs + i] = args[i];
	ak_val out;
	acc *r = acc_of(ap->root);
	if (r) out = read_below(r, steps, tn, all);
	else out = ak_vm_cell_path(ap->root, steps, tn, all, false);
	if (steps != sbuf) ak_free(steps);
	if (all != abuf) ak_free(all);
	return out;
}

ak_val ak_vm_acc_getp(void *p, const uint8_t *path, uint32_t n, const ak_val *args) {
	acc *a = p;
	switch (a->mode) {
	case ACC_CELL:
		return ak_vm_cell_path(a->v.p, path, n, args, false);
	case ACC_PATH:
		return apath_read(a->v.p, (ak_kind)a->kind, path, n, args);
	case ACC_MODEL: {
		model *m = a->v.p;
		if (m->bound) return ak_vm_acc_getp(m->inner, path, n, args);
		return ak_vm_cell_path(m->own, path, n, args, false);
	}
	}
	return read_below(a, path, n, args);
}

// ak_vm_acc_keys follows, for a for keyed by a field path of its items
// (fields, nf of them), the keys of the array acc reads: one signal for
// them all when acc's is a path below a cell; else acc, whole.
void ak_vm_acc_keys(void *p, const uint16_t *fields, uint16_t nf) {
	acc *a = p;
	switch (a->mode) {
	case ACC_CELL:
		ak_vm_cell_keys(a->v.p, NULL, 0, NULL, fields, nf);
		return;
	case ACC_MODEL: {
		model *m = a->v.p;
		if (m->bound) ak_vm_acc_keys(m->inner, fields, nf);
		else ak_vm_cell_keys(m->own, NULL, 0, NULL, fields, nf);
		return;
	}
	case ACC_PATH: {
		apath *ap = a->v.p;
		if (!acc_of(ap->root)) {
			ak_val abuf[PATH_ARGS];
			// None when the path takes none (GCC would see abuf read unset).
			ak_val *all = !ap->nargs ? NULL : ap->nargs <= PATH_ARGS ? abuf : ak_alloc(sizeof(ak_val) * ap->nargs);
			for (uint32_t i = 0; i < ap->nargs; i++) all[i] = ap->kinds[i] == AK_K_VOID ? ak_vm_cell_get(ap->args[i].p, true) : ap->args[i];
			ak_vm_cell_keys(ap->root, ap->steps, ap->n, all, fields, nf);
			if (all != abuf) ak_free(all);
			return;
		}
		break;
	}
	}
	ak_val v = ak_vm_acc_get(a);  // not a path's: what it reads, whole
	if (!ak_error_pending()) ak_release_val(v, (ak_kind)a->kind);
}

// ak_vm_acc_seek sets the index an accessor of an array's element reads
// (its last argument, an int): a for's key's, made once and moved over
// the items.
void ak_vm_acc_seek(void *p, int64_t i) {
	acc *a = p;
	apath *ap = a->v.p;
	ap->args[ap->nargs - 1].i = i;
}

// ---- Untracked calls, effects, cleanups, batches ----

void ak_vm_untrack_begin(ak_co *co) {
	if (co->nuntracks == co->ucap) {
		co->ucap = co->ucap ? co->ucap * 2 : 8;
		co->untracks = ak_realloc(co->untracks, sizeof(void *) * co->ucap);
	}
	co->untracks[co->nuntracks++] = ak_untrack_begin();
}

ak_val ak_vm_call_untracked(ak_closure *c, ak_val *args, uint32_t nargs) {
	void *tok = ak_untrack_begin();
	ak_val v = ak_call(c, args, nargs);
	ak_untrack_end(tok);
	return v;
}

typedef struct {
	ak_closure *c;
	ak_comp_info info;
} effect_ctx;

static void effect_run(void *ctx) {
	effect_ctx *e = ctx;
	ak_val v = ak_call(e->c, NULL, 0);
	AK_UNUSED(v);  // effects return nothing
	raise_pending();
}

static void effect_drop(void *ctx) {
	effect_ctx *e = ctx;
	ak_release(e->c);
	ak_free(e);
}

// effect_describe names an effect for a panic as its source says it:
// "Picker's effect at picker.asuka:12" (its closure, Picker/effect, made
// where its declaration is).
static void effect_describe(void *ctx, char *buf, size_t n) {
	effect_ctx *e = ctx;
	ak_vm_where(e->c, e->c->invoke == ak_vm_closure_invoke ? NULL : "an effect of the runtime's", buf, n);
}

static const void *effect_site(void *ctx) { return ak_vm_where_site(((effect_ctx *)ctx)->c); }

// declared_effect reports whether c is an effect the source declares,
// effect { … } (its function Picker/effect), not a task's that restarts
// it (Picker/task deps): its writes are what effect-in-effect-pass names.
static bool declared_effect(const ak_closure *c) {
	if (c->invoke != ak_vm_closure_invoke) return false;
	const char *name = ak_func_name(c->fn), *last = strrchr(name, '/');
	return strcmp(last ? last + 1 : name, "effect") == 0;
}

ak_closure *ak_vm_effect_closure(ak_effect *e) {
	effect_ctx *x = ak_effect_ctx(e, effect_run);
	return x ? x->c : NULL;
}

void ak_vm_effect(ak_closure *c, bool render) {
	effect_ctx *e = ak_alloc(sizeof *e);
	e->c = ak_retain(c);
	e->info = (ak_comp_info){.drop = effect_drop, .label = label_of(c), .describe = render ? NULL : effect_describe, .site = effect_site, .app = !render && ak_diag_enabled && declared_effect(c)};
	ak_effect_new(effect_run, e, render ? AK_EFFECT_RENDER : AK_EFFECT_USER, &e->info);
}

static void cleanup_run(void *ctx) {
	ak_closure *c = ctx;
	ak_val v = ak_call(c, NULL, 0);
	AK_UNUSED(v);
	raise_pending();
	ak_release(c);
}

void ak_vm_cleanup(ak_closure *c) {
	if (!ak_current_owner()) return;
	ak_on_cleanup(cleanup_run, ak_retain(c));
}

void ak_vm_batch(bool begin) {
	if (begin) ak_batch_begin();
	else ak_batch_end();
}

// ---- Tasks and owners ----
//
// Each task has an owner of its own under the owner that started it:
// disposing that one cancels the task, and the task disposes its owner
// when it finishes, so a finished task holds nothing. The owner's
// cleanup refers to the task, not its future: the task is there until
// it disposes the owner, and the future's references are those who can
// await it (an error none of them can see goes to the owner's catcher).

static void task_owner_cleanup(void *ctx) {
	ak_task *t = ctx;
	if (t->fut) ak_future_cancel(t->fut);
}

static void register_cancel(void *ctx) { ak_on_cleanup(task_owner_cleanup, ctx); }

void ak_vm_task_own(ak_task *t, ak_owner *holder) {
	if (!holder && !ak_current_owner()) return;
	t->owner = holder ? ak_owner_new_in(holder) : ak_owner_new();
	ak_owner_retain(t->owner);
	ak_with_owner(t->owner, register_cancel, t);
}

typedef struct {
	ak_task *t;
	ak_val out;
	bool suspended;
} step_ctx;

static void step_in_owner(void *ctx) {
	step_ctx *s = ctx;
	s->suspended = ak_vm_run(&s->t->co, &s->out);
}

bool ak_vm_task_run(ak_task *t, ak_val *out) {
	if (!t->owner) return ak_vm_run(&t->co, out);
	step_ctx s = {t, {0}, false};
	ak_with_owner(t->owner, step_in_owner, &s);
	*out = s.out;
	return s.suspended;
}

static void raise_ctx(void *err) { ak_raise(err); }

// The owner, kept past its disposal for its catcher (its parents are
// held as long as it is), reached when the future is dropped unseen.
static void raise_unseen(void *ctx, void *err) {
	ak_owner *o = ctx;
	if (err) {
		if (o) ak_with_owner(o, raise_ctx, err);
		else ak_raise(err);
	}
	if (o) ak_owner_release(o);
}

void ak_vm_task_report_unseen(ak_task *t, ak_future *f) {
	if (t->owner) ak_owner_retain(t->owner);
	f->unseen = raise_unseen;
	f->unseen_ctx = t->owner;
}

// ak_vm_task_current is the future of the task running now and its
// owner (*owner; NULL when it has none), or NULL on the main coroutine:
// what an optimistic change (RFC 0017) belongs to.
ak_future *ak_vm_task_current(ak_owner **owner) {
	ak_co *co = ak_vm_cur;
	if (!co || !co->task || !co->task->fut) return NULL;
	*owner = co->task->owner;
	return co->task->fut;
}

void ak_vm_task_done(ak_task *t, void *err) {
	ak_owner *o = t->owner;
	t->owner = NULL;
	if (err) {
		if (o) ak_with_owner(o, raise_ctx, err);
		else ak_raise(err);
	}
	if (o) {
		ak_owner_dispose(o);
		ak_owner_release(o);
	}
}

// ---- Components' event handlers ----
//
// An async handler of a component's event (lang/LOWERING.md's
// "Components") is a task of the component that declares it, the
// receiver: whoever emits the event (a sync handler, a fn running on a
// task, an async function that waits for it), the handler runs until it
// finishes or the receiver goes. ui.owner, at the receiver's start, is
// the owner it was made under, which goes when it goes; ui.handler wraps
// the handler as it's passed: calling it starts the task held by that
// owner, its contexts and catcher those of where the handler is declared
// (the owner current then: a sheet's, a stack's screen's).

typedef struct {
	ak_opaque h;
	ak_owner *o;
} owner_ref;

static void owner_ref_drop(ak_opaque *p) { ak_owner_release(((owner_ref *)p)->o); }

static ak_val b_owner(ak_val *a, uint32_t n) {
	AK_UNUSED(a);
	AK_UNUSED(n);
	ak_owner *o = ak_current_owner();
	if (!o) return (ak_val){0};
	owner_ref *r = ak_opaque_new(AK_TAG_OPAQUE, sizeof *r, owner_ref_drop);
	ak_owner_retain(o);
	r->o = o;
	return (ak_val){.p = r};
}

// A handler's captures: the async closure, where it's declared and what
// holds its tasks (owners, retained: released by handler_drop).
static const uint8_t handler_caps[3] = {AK_K_REF, AK_K_INT, AK_K_INT};

static void handler_drop(ak_closure *c) {
	if (c->caps[1].p) ak_owner_release(c->caps[1].p);
	if (c->caps[2].p) ak_owner_release(c->caps[2].p);
}

typedef struct {
	ak_closure *c;
	ak_val *args;
	uint32_t nargs;
	ak_owner *holder;
	ak_val out;
} handler_call;

static void handler_start(void *ctx) {
	handler_call *h = ctx;
	h->out = (ak_val){.p = ak_vm_task_start_in(h->c->fn, h->c, h->args, h->nargs, h->holder)};
}

static ak_val handler_invoke(ak_closure *c, ak_val *args, uint32_t nargs) {
	ak_owner *where = c->caps[1].p, *holder = c->caps[2].p;
	// Its component is gone, and the handler with it.
	if (holder && ak_owner_disposed(holder)) return (ak_val){0};
	handler_call h = {c->caps[0].p, args, nargs, holder, {0}};
	if (where) ak_with_owner(where, handler_start, &h);
	else handler_start(&h);
	return h.out;
}

static ak_val b_handler(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_closure *f = a[0].p;
	owner_ref *r = a[1].p;
	if (!f || !ak_closure_async(f)) return (ak_val){.p = ak_retain(f)};
	ak_owner *where = ak_current_owner(), *holder = r ? r->o : where;
	ak_closure *c = ak_closure_new(handler_invoke, NULL, 3, handler_caps);
	c->caps[0].p = ak_retain(f);
	if (where) ak_owner_retain(where);
	c->caps[1].p = where;
	if (holder) ak_owner_retain(holder);
	c->caps[2].p = holder;
	c->drop = handler_drop;
	return (ak_val){.p = c};
}

// ---- Setup ----

static void err_retain(void *e) { ak_retain(e); }
static void err_release(void *e) { ak_release(e); }

void ak_vm_react_init(void) {
	ak_signal_set_error_refs(err_retain, err_release);
	ak_builtin_register("ui.owner", b_owner);
	ak_builtin_register("ui.handler", b_handler);
}
