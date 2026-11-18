// owner.c: owners, cleanups, contexts and error catchers.
#include "signal/signal_impl.h"

#define g ak_sig

void ak_owner_retain(ak_owner *o) { o->refs++; }

static void owner_free(ak_owner *o) {
	ak_owner *parent = o->parent, *holder = o->holder;
	for (ak_ctx_entry *e = o->ctx; e;) {
		ak_ctx_entry *next = e->next;
		ak_free(e);
		e = next;
	}
	ak_vec_free(&o->cleanups);
	if (o->comp) {
		ak_comp *c = o->comp;
		ak_sig_err_release(c->err);
		if (c->info && c->info->drop) c->info->drop(c->ctx);
		ak_free(c);
	} else {
		ak_free(o);
	}
	if (parent) ak_owner_release(parent);
	if (holder) ak_owner_release(holder);
}

void ak_owner_release(ak_owner *o) {
	AK_ASSERT(o->refs > 0);
	if (--o->refs == 0) owner_free(o);
}

// hold puts o in holder's children, which disposes it with itself.
static void hold(ak_owner *o, ak_owner *holder) {
	o->doomed = holder->disposed || holder->doomed;
	o->attached = true;
	o->prev = holder->last;
	if (holder->last) holder->last->next = o;
	else holder->first = o;
	holder->last = o;
}

// ak_sig_owner_attach puts o under parent, which then holds it.
void ak_sig_owner_attach(ak_owner *o, ak_owner *parent) {
	o->parent = parent;
	if (!parent) return;
	ak_owner_retain(parent);
	o->catcher = parent->catcher;
	hold(o, parent);
}

static void detach(ak_owner *o) {
	if (!o->attached) return;
	ak_owner *p = o->holder ? o->holder : o->parent;
	if (o->prev) o->prev->next = o->next;
	else p->first = o->next;
	if (o->next) o->next->prev = o->prev;
	else p->last = o->prev;
	o->prev = o->next = NULL;
	o->attached = false;
}

static ak_owner *owner_alloc(void) {
	ak_owner *o = ak_alloc(sizeof *o);
	o->refs = 1;
	return o;
}

static void owner_dispose(ak_owner *o);

// ak_sig_owner_clean disposes o's children (last made first) and runs its
// cleanups (last registered first), leaving o usable: a computation
// cleans its owner before each re-run.
void ak_sig_owner_clean(ak_owner *o) {
	if (!o->first && !o->cleanups.len) return;
	ak_comp *prev_sub = g->active;
	g->active = NULL;
	for (ak_owner *c; (c = o->last);) owner_dispose(c); // detaches c
	while (o->cleanups.len) {
		ak_cleanup cl = ak_vec_pop(&o->cleanups);
		cl.fn(cl.ctx);
	}
	g->active = prev_sub;
}

// ak_sig_owner_settle: code that ran under o returned. If o was disposed
// meanwhile, what the code made under it after that (doomed) is disposed
// now, and o lets go of it.
void ak_sig_owner_settle(ak_owner *o) {
	if (o->disposed && o->first) ak_sig_owner_clean(o);
}

static void owner_dispose(ak_owner *o) {
	if (o->disposed) return;
	o->disposed = true;
	ak_owner_retain(o);
	detach(o);
	// A computation stops following first: what its cleanups write (one
	// that undoes what an effect's run set up) mustn't run it again as
	// it goes.
	if (o->comp) {
		o->comp->node.flags |= AK_F_DISPOSED;
		ak_sig_unlink_all_deps(o->comp);
	}
	ak_sig_owner_clean(o);
	if (o->comp) ak_sig_unlink_all_subs(&o->comp->node);
	ak_owner_release(o); // the parent's or the creator's hold
	ak_owner_release(o);
}

void ak_owner_dispose(ak_owner *o) { owner_dispose(o); }
bool ak_owner_disposed(const ak_owner *o) { return o->disposed; }

ak_owner *ak_owner_new(void) {
	ak_owner *o = owner_alloc();
	ak_sig_owner_attach(o, g->owner);
	return o;
}

ak_owner *ak_root_new(void) {
	ak_owner *o = owner_alloc();
	ak_owner *p = g->owner;
	o->parent = p;
	if (p) {
		ak_owner_retain(p);
		o->catcher = p->catcher;
	}
	return o;
}

ak_owner *ak_owner_new_in(ak_owner *holder) {
	if (holder == g->owner) return ak_owner_new();
	ak_owner *o = ak_root_new();
	if (holder) {
		ak_owner_retain(holder);
		o->holder = holder;
		hold(o, holder);
	}
	return o;
}

ak_owner *ak_root(void (*fn)(void *ctx, ak_owner *root), void *ctx) {
	ak_owner *o = ak_root_new();
	ak_owner *prev_owner = g->owner;
	ak_comp *prev_sub = g->active;
	g->owner = o;
	g->active = NULL;
	ak_owner_retain(o);  // fn may dispose it
	ak_batch_begin();
	fn(ctx, o);
	ak_batch_end();
	g->owner = prev_owner;
	g->active = prev_sub;
	ak_sig_owner_settle(o);
	ak_owner_release(o);
	return o;
}

ak_owner *ak_current_owner(void) { return g->owner; }

void ak_with_owner(ak_owner *o, void (*fn)(void *ctx), void *ctx) {
	ak_owner *prev_owner = g->owner;
	ak_comp *prev_sub = g->active;
	if (o) ak_owner_retain(o);  // fn may dispose it
	g->owner = o;
	g->active = NULL;
	fn(ctx);
	g->owner = prev_owner;
	g->active = prev_sub;
	if (o) {
		ak_sig_owner_settle(o);
		ak_owner_release(o);
	}
}

void ak_with_owner_tracked(ak_owner *o, void (*fn)(void *ctx), void *ctx) {
	ak_owner *prev_owner = g->owner;
	if (o) ak_owner_retain(o);
	g->owner = o;
	fn(ctx);
	g->owner = prev_owner;
	if (o) {
		ak_sig_owner_settle(o);
		ak_owner_release(o);
	}
}

ak_owner *ak_owner_enter(void) {
	ak_owner *prev = g->owner;
	g->owner = ak_owner_new();
	return prev;
}

void ak_owner_leave(ak_owner *prev) { g->owner = prev; }

void ak_on_cleanup(void (*fn)(void *ctx), void *ctx) {
	ak_owner *o = g->owner;
	if (!o) return;
	if (o->disposed || o->doomed) {
		// Disposed already: it runs now, as it would have then.
		ak_comp *prev_sub = g->active;
		g->active = NULL;
		fn(ctx);
		g->active = prev_sub;
		return;
	}
	ak_vec_push(&o->cleanups, ((ak_cleanup){fn, ctx}));
}

// Contexts.

void ak_owner_set_context(ak_owner *o, const void *key, void *value) {
	for (ak_ctx_entry *e = o->ctx; e; e = e->next) {
		if (e->key == key) {
			e->val = value;
			return;
		}
	}
	ak_ctx_entry *e = ak_alloc(sizeof *e);
	e->key = key;
	e->val = value;
	e->next = o->ctx;
	o->ctx = e;
}

ak_owner *ak_provide(const void *key, void *value, void (*fn)(void *ctx), void *ctx) {
	ak_owner *o = ak_owner_new();
	ak_owner_set_context(o, key, value);
	ak_owner *prev_owner = g->owner;
	ak_owner_retain(o);  // fn may dispose it
	g->owner = o;
	fn(ctx);
	g->owner = prev_owner;
	ak_sig_owner_settle(o);
	ak_owner_release(o);
	return o;
}

bool ak_context_lookup(const void *key, void **value) {
	for (ak_owner *o = g->owner; o; o = o->parent) {
		for (ak_ctx_entry *e = o->ctx; e; e = e->next) {
			if (e->key == key) {
				*value = e->val;
				return true;
			}
		}
	}
	return false;
}

void *ak_use_context(const void *key, void *def) {
	void *v;
	return ak_context_lookup(key, &v) ? v : def;
}

// Errors.

ak_owner *ak_catcher_new(ak_error_fn on_error, void *ctx) {
	ak_owner *o = ak_owner_new();
	o->catcher = o;
	o->on_error = on_error;
	o->error_ctx = ctx;
	return o;
}

ak_owner *ak_catch(void (*fn)(void *ctx), void *ctx, ak_error_fn on_error, void *ectx) {
	ak_owner *o = ak_catcher_new(on_error, ectx);
	ak_owner_retain(o);
	ak_owner *prev_owner = g->owner;
	ak_comp *prev_sub = g->active;
	ak_frame f = {.prev = g->frame};
	g->frame = &f;
	g->owner = o;
	fn(ctx);
	g->owner = prev_owner;
	g->active = prev_sub;
	g->frame = f.prev;
	if (f.failed) ak_sig_handle_error(o, f.err);
	ak_sig_owner_settle(o);
	ak_owner_release(o);
	return o;
}

void ak_raise(void *err) {
	ak_frame *f = g->frame;
	if (f) {
		if (!f->failed) {
			f->failed = true;
			f->err = err;
		} else {
			ak_sig_err_release(err);  // only the first error of a run is kept
		}
		return;
	}
	ak_sig_handle_error(g->owner, err);
}

bool ak_failed(void) { return g->frame && g->frame->failed; }

void ak_signal_set_error_refs(void (*retain)(void *err), void (*release)(void *err)) {
	g->err_retain = retain;
	g->err_release = release;
}

void ak_signal_set_error_handler(ak_error_fn fn, void *ctx) {
	g->on_error = fn;
	g->on_error_ctx = ctx;
}

// ak_sig_handle_error gives err to the catcher of o, or to the default
// handler when there is none. The handler runs in a frame of its own: what
// it raises goes to the catcher around its catcher.
void ak_sig_handle_error(ak_owner *o, void *err) {
	ak_owner *c = o ? o->catcher : NULL;
	if (!c) {
		if (g->on_error) g->on_error(g->on_error_ctx, err);
		else ak_sig_panic(NULL, "signal: an error no catcher handled (%p)", err);
		ak_sig_err_release(err);
		return;
	}
	ak_owner *up = c->parent && c->parent->catcher ? c->parent : NULL;
	ak_owner_retain(c);
	ak_frame f = {.prev = g->frame};
	g->frame = &f;
	c->on_error(c->error_ctx, err);
	ak_sig_err_release(err);
	g->frame = f.prev;
	if (f.failed) ak_sig_handle_error(up, f.err);
	ak_owner_release(c);
}
