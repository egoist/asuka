// react.c: the VM's state, on the signal graph (core/src/signal): cells,
// memos, accessors, effects, cleanups, and tasks tied to owners.
#include "vm/vm_int.h"
#include "signal/signal.h"
#include "vm/keep.h"

// ---- Cells ----

typedef struct {
	ak_opaque h;
	ak_node node;
	ak_val v;
	uint8_t kind;
} cell;

static void cell_drop(ak_opaque *o) {
	cell *c = (cell *)o;
	if (o->h.flags & 1) ak_keep_forget(c);  // a keyed cell (keep.c)
	ak_signal_drop(&c->node);
	ak_release_val(c->v, (ak_kind)c->kind);
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

ak_val ak_vm_cell_get(void *p, bool track) {
	cell *c = p;
	if (track) ak_signal_read(&c->node);
	ak_retain_val(c->v, (ak_kind)c->kind);
	return c->v;
}

void ak_vm_cell_set(void *p, ak_val v) {
	cell *c = p;
	if (ak_val_eq(c->v, v, (ak_kind)c->kind)) {
		ak_release_val(v, (ak_kind)c->kind);
		return;
	}
	ak_val old = c->v;
	c->v = v;
	ak_release_val(old, (ak_kind)c->kind);
	ak_signal_write(&c->node);
}

ak_val ak_vm_cell_take(void *p) {
	cell *c = p;
	ak_val v = c->v;
	if (c->kind == AK_K_REF) c->v.p = NULL;
	return v;
}

void ak_vm_cell_put(void *p, ak_val v) {
	cell *c = p;
	ak_release_val(c->v, (ak_kind)c->kind);
	c->v = v;
	ak_signal_write(&c->node);
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

static void memo_drop(ak_opaque *o) {
	memo *m = (memo *)o;
	ak_release_val(m->v, (ak_kind)m->kind);
	ak_release(m->c);
}

ak_val ak_vm_memo_new(ak_closure *c, ak_kind k) {
	memo *m = ak_opaque_new(AK_TAG_MEMO, sizeof(memo), memo_drop);
	m->kind = (uint8_t)k;
	m->c = ak_retain(c);
	m->info = (ak_comp_info){.drop = memo_comp_drop, .label = label_of(c)};
	ak_retain(m);  // the graph's, until the computation is dropped
	m->m = ak_memo_new(memo_update, m, &m->info);
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

enum { ACC_VALUE, ACC_FUNC, ACC_CELL, ACC_MEMO, ACC_MODEL };

typedef struct {
	ak_opaque h;
	uint8_t mode, kind;
	ak_val v;  // the value, or the closure, cell or memo
} acc;

static void acc_drop(ak_opaque *o) {
	acc *a = (acc *)o;
	if (a->mode == ACC_VALUE) ak_release_val(a->v, (ak_kind)a->kind);
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

static ak_val model_get(void *m);

ak_val ak_vm_acc_get(void *p) {
	acc *a = p;
	switch (a->mode) {
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
// it reads a value of its own, which an assignment sets and which takes
// the caller's value each time that changes (a render effect, when the
// caller's isn't a constant).

typedef struct {
	ak_opaque h;
	ak_node node;  // the own value's readers
	ak_val v;      // the own value (unbound)
	acc *inner;    // the caller's accessor
	uint8_t kind;
	bool bound;
} model;

static void model_drop(ak_opaque *o) {
	model *m = (model *)o;
	ak_signal_drop(&m->node);
	ak_release_val(m->v, (ak_kind)m->kind);
	ak_release(m->inner);
}

static ak_val model_get(void *p) {
	model *m = p;
	if (m->bound) return ak_vm_acc_get(m->inner);
	ak_signal_read(&m->node);
	ak_retain_val(m->v, (ak_kind)m->kind);
	return m->v;
}

// model_store takes v as the own value.
static void model_store(model *m, ak_val v) {
	if (ak_val_eq(m->v, v, (ak_kind)m->kind)) {
		ak_release_val(v, (ak_kind)m->kind);
		return;
	}
	ak_val old = m->v;
	m->v = v;
	ak_release_val(old, (ak_kind)m->kind);
	ak_signal_write(&m->node);
}

// The caller's value, each time it changes, replaces the own.
static void model_follow(void *ctx) {
	model *m = ctx;
	ak_val v = ak_vm_acc_get(m->inner);
	if (ak_error_pending()) {
		raise_pending();
		return;
	}
	model_store(m, v);
}

static void model_follow_drop(void *ctx) { ak_release(ctx); }

static const ak_comp_info model_follow_info = {.drop = model_follow_drop, .label = "a prop's caller's value"};

ak_val ak_vm_model_new(void *inner, bool bound) {
	acc *in = inner;
	if (!in || ((ak_obj *)in)->tag != AK_TAG_ACC) ak_panic("ui.model: a prop's accessor, not %p", inner);
	model *m = ak_opaque_new(AK_TAG_OPAQUE, sizeof(model), model_drop);
	ak_signal_born(&m->node);
	m->kind = in->kind;
	m->inner = ak_retain(in);
	m->bound = bound;
	if (!bound) {
		// The caller's value now, untracked: what builds the component
		// doesn't follow it.
		void *tok = ak_untrack_begin();
		m->v = ak_vm_acc_get(in);
		ak_untrack_end(tok);
		if (ak_error_pending()) {
			ak_release(m);
			return (ak_val){0};
		}
		if (in->mode != ACC_VALUE) ak_effect_new(model_follow, ak_retain(m), AK_EFFECT_RENDER, &model_follow_info);
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
	model_store(m, v);
}

// ---- Untracked calls, effects, cleanups, batches ----

ak_val ak_vm_untrack(ak_closure *c, ak_val *args, uint32_t nargs) {
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

void ak_vm_effect(ak_closure *c, bool render) {
	effect_ctx *e = ak_alloc(sizeof *e);
	e->c = ak_retain(c);
	e->info = (ak_comp_info){.drop = effect_drop, .label = label_of(c)};
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
// when it finishes, so a finished task holds nothing.

static void task_owner_cleanup(void *ctx) {
	ak_future *f = ctx;
	ak_future_cancel(f);
	ak_release(f);
}

static void register_cancel(void *ctx) { ak_on_cleanup(task_owner_cleanup, ak_retain(ctx)); }

void ak_vm_task_own(ak_task *t) {
	if (!ak_current_owner()) return;
	t->owner = ak_owner_new();
	ak_owner_retain(t->owner);
	ak_with_owner(t->owner, register_cancel, t->fut);
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

// ---- Setup ----

static void err_retain(void *e) { ak_retain(e); }
static void err_release(void *e) { ak_release(e); }

void ak_vm_react_init(void) { ak_signal_set_error_refs(err_retain, err_release); }
