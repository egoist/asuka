// signal.c: signals, memos, effects, batching and the flush.
#include "signal/signal_impl.h"

#include <stdarg.h>
#include <stdio.h>

#define g ak_sig

void ak_sig_panic(const char *code, const char *fmt, ...) {
	char buf[512];
	int at = code ? snprintf(buf, sizeof buf, "error[%s]: ", code) : 0;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + at, sizeof buf - (size_t)at, fmt, ap);
	va_end(ap);
	if (g->panic_hook) {
		g->panic_hook(g->panic_ctx, buf);
		return;
	}
	ak_panic("%s", buf);
}

void ak_signal_set_panic_hook(void (*fn)(void *ctx, const char *msg), void *ctx) {
	g->panic_hook = fn;
	g->panic_ctx = ctx;
}

const char *ak_sig_describe(const ak_comp *c, char *buf, size_t n) {
	const char *kind = (c->node.flags & AK_F_FOLLOW) ? "state" : (c->node.flags & AK_F_MEMO) ? "memo" : "effect";
	if (c->info && c->info->describe) c->info->describe(c->ctx, buf, n);
	else if (c->info && c->info->label) snprintf(buf, n, "%s %s", kind, c->info->label);
	else snprintf(buf, n, "an unnamed %s", kind);
	return buf;
}

void ak_signal_note_write(const char *what, size_t len) {
	if (g->effect) {
		g->effect->wrote = what;
		g->effect->wrote_len = len;
	}
}

// Signals.

void ak_signal_read(ak_node *n) {
	if (g->active) ak_sig_track(n, g->active);
}

void ak_signal_born(ak_node *n) {
	if (g->in_memo > 0) n->flags |= AK_F_FRESH;
}

// own_scratch reports whether n is the running computation's own: read,
// if at all, only by it (a store it made reading its own state as it's
// set up: sqlite's migrate counting the tables it wrote).
static bool own_scratch(const ak_node *n) {
	return !n->subs || (n->subs == n->subs_tail && n->subs->sub == g->active);
}

// diag_wrote notes that e, an effect the app declares, wrote n, among
// the pass's last writes (held while noted). diag_take moves them out,
// for the pass that runs what they dirtied; diag_release lets go of them.
#define AK_DIAG_WRITES 8
typedef struct ak_diag_write diag_write;

static void diag_wrote(ak_comp *e, const ak_node *n) {
	diag_write *slot = &g->diag_writes[g->ndiag_writes++ % AK_DIAG_WRITES];
	if (slot->w) ak_owner_release(&slot->w->owner);
	ak_owner_retain(&e->owner);
	*slot = (diag_write){e, n};
}

static uint32_t diag_take(diag_write *out) {
	uint32_t n = g->ndiag_writes < AK_DIAG_WRITES ? g->ndiag_writes : AK_DIAG_WRITES;
	for (uint32_t i = 0; i < AK_DIAG_WRITES; i++) {
		out[i] = g->diag_writes[i];
		g->diag_writes[i] = (diag_write){0};
	}
	g->ndiag_writes = 0;
	return n;
}

static void diag_release(diag_write *ws) {
	for (uint32_t i = 0; i < AK_DIAG_WRITES; i++)
		if (ws[i].w) ak_owner_release(&ws[i].w->owner);
}

// diag_clear lets go of the writes noted: a flush starts, or it ended.
static void diag_clear(void) {
	if (!g->ndiag_writes) return;
	diag_write ws[AK_DIAG_WRITES];
	diag_take(ws);
	diag_release(ws);
}

void ak_signal_write(ak_node *n) {
	if (!ak_sig_check_thread()) return;
	if (AK_UNLIKELY(g->in_memo > 0)) {
		// A memo may write only a signal it made in this run that nothing
		// else reads: that can't make anything stale (it isn't done yet).
		if (!((n->flags & AK_F_FRESH) && own_scratch(n))) {
			char d[256];
			ak_sig_panic("write-in-memo", "signal: write inside a memo (%s)", g->active ? ak_sig_describe(g->active, d, sizeof d) : "?");
			return;
		}
		return;
	}
	if (n->subs) {
		// An app's effect's write, which may run a binding again in the
		// flush it ran in (effect-in-effect-pass).
		if (AK_UNLIKELY(ak_diag_enabled) && g->effect && g->effect->info && g->effect->info->app) diag_wrote(g->effect, n);
		ak_sig_propagate(n->subs, AK_F_DIRTY);
		if (g->batch_depth == 0 && !g->flushing) ak_sig_flush();
	}
}

void ak_wsignal_init(ak_wsignal *s, void (*unwatched)(ak_wsignal *s)) {
	ak_signal_init(&s->node);
	s->node.flags = AK_F_UNWATCH;
	s->unwatched = unwatched;
}

void ak_signal_drop(ak_node *n) {
	n->flags = (n->flags | AK_F_DISPOSED) & ~(uint32_t)AK_F_UNWATCH;
	ak_sig_unlink_all_subs(n);
}

// Memos.

static ak_comp *comp_new(uint32_t flags, void *ctx, const ak_comp_info *info) {
	ak_comp *c = ak_alloc(sizeof *c);
	c->node.flags = flags;
	c->ctx = ctx;
	c->info = info;
	c->owner.comp = c;
	c->owner.refs = 1;
	ak_sig_owner_attach(&c->owner, g->owner);
	if (c->owner.doomed) c->node.flags |= AK_F_DISPOSED;  // never runs
	return c;
}

ak_memo *ak_memo_new(bool (*update)(void *ctx), void *ctx, const ak_comp_info *info) {
	ak_comp *m = comp_new(AK_F_MEMO | AK_F_DIRTY, ctx, info);
	m->fn.update = update;
	return m;
}

ak_owner *ak_memo_owner(ak_memo *m) { return &m->owner; }
void *ak_memo_error(const ak_memo *m) { return m->err; }

bool ak_memo_read(ak_memo *m) {
	ak_node *n = &m->node;
	if (n->flags & AK_F_DISPOSED) return true;
	if (n->flags & (AK_F_DIRTY | AK_F_PENDING)) {
		// Held: updating it (or a memo it reads) may dispose it.
		ak_owner_retain(&m->owner);
		if ((n->flags & AK_F_DIRTY) || ak_sig_check_dirty(m)) {
			if (ak_sig_memo_update(m) && n->subs) ak_sig_shallow_propagate(n->subs);
		} else {
			n->flags &= ~(uint32_t)AK_F_PENDING;
		}
		if (n->flags & AK_F_DISPOSED) {
			ak_owner_release(&m->owner);
			return true;
		}
		ak_owner_release(&m->owner);  // not the last hold: its owner's
	}
	if (g->active) ak_sig_track(n, g->active);
	if (m->err) {
		ak_raise(ak_sig_err_retain(m->err));
		return false;
	}
	return true;
}

// ak_sig_memo_update recomputes m and reports whether its value changed.
// An error raised in it is m's until its next update.
bool ak_sig_memo_update(ak_comp *m) {
	ak_owner_retain(&m->owner);
	ak_frame f = {.prev = g->frame};
	g->frame = &f;
	ak_sig_owner_clean(&m->owner);
	ak_comp *prev_sub = g->active;
	ak_owner *prev_owner = g->owner;
	g->active = m;
	g->owner = &m->owner;
	g->in_memo++;
	ak_sig_err_release(m->err);
	m->err = NULL;
	ak_sig_start_tracking(m);
	bool changed = m->fn.update(m->ctx);
	g->in_memo--;
	g->active = prev_sub;
	g->owner = prev_owner;
	if (m->node.flags & AK_F_DISPOSED) ak_sig_unlink_all_deps(m);
	else ak_sig_end_tracking(m);
	ak_sig_owner_settle(&m->owner);
	g->frame = f.prev;
	if (f.failed) {
		m->err = f.err;
		changed = true; // its readers must see the error
	}
	ak_owner_release(&m->owner);
	return changed;
}

// ak_sig_memo_unwatched: with no subscribers left, a memo lets go of its
// sources so it stops being notified, and recomputes on its next read.
void ak_sig_memo_unwatched(ak_comp *m) {
	if (m->node.flags & AK_F_DISPOSED) return;
	ak_sig_unlink_all_deps(m);
	m->node.flags |= AK_F_DIRTY;
	if (m->info && m->info->unwatched) m->info->unwatched(m->ctx);
}

// Follows.

ak_follow *ak_follow_new(bool (*update)(void *ctx), void *ctx, const ak_comp_info *info) {
	ak_comp *f = comp_new(AK_F_MEMO | AK_F_FOLLOW | AK_F_DIRTY, ctx, info);
	f->fn.update = update;
	return f;
}

void ak_follow_refresh(ak_follow *f) {
	ak_node *n = &f->node;
	if ((n->flags & AK_F_DISPOSED) || !(n->flags & (AK_F_DIRTY | AK_F_PENDING))) return;
	ak_owner_retain(&f->owner);
	ak_comp *prev = g->active;
	g->active = NULL;
	if ((n->flags & AK_F_DIRTY) || ak_sig_check_dirty(f)) {
		if (ak_sig_memo_update(f) && n->subs) ak_sig_shallow_propagate(n->subs);
	} else {
		n->flags &= ~(uint32_t)AK_F_PENDING;
	}
	g->active = prev;
	ak_owner_release(&f->owner);
}

void ak_follow_set(ak_follow *f) {
	ak_node *n = &f->node;
	if (n->flags & AK_F_DISPOSED) return;
	n->flags &= ~(uint32_t)(AK_F_DIRTY | AK_F_PENDING);
	ak_sig_err_release(f->err);
	f->err = NULL;
	ak_signal_write(n);
}

bool ak_follow_settled(const ak_follow *f) {
	return !f->deps && !(f->node.flags & (AK_F_DIRTY | AK_F_PENDING)) && f->run > 0;
}

// Effects.

static void effect_update(ak_comp *e) {
	ak_owner_retain(&e->owner);
	ak_frame f = {.prev = g->frame};
	g->frame = &f;
	ak_sig_owner_clean(&e->owner);
	ak_comp *prev_sub = g->active, *prev_effect = g->effect;
	ak_owner *prev_owner = g->owner;
	g->active = e;
	g->effect = e;
	g->owner = &e->owner;
	e->wrote = NULL;
	ak_sig_start_tracking(e);
	e->fn.run(e->ctx);
	g->active = prev_sub;
	g->effect = prev_effect;
	g->owner = prev_owner;
	// After an error too: it re-runs when what it read so far changes.
	if (e->node.flags & AK_F_DISPOSED) ak_sig_unlink_all_deps(e);
	else ak_sig_end_tracking(e);
	ak_sig_owner_settle(&e->owner);
	g->frame = f.prev;
	if (f.failed) ak_sig_handle_error(&e->owner, f.err);
	ak_owner_release(&e->owner);
}

ak_effect *ak_effect_new(void (*run)(void *ctx), void *ctx, uint32_t flags, const ak_comp_info *info) {
	bool render = (flags & AK_EFFECT_RENDER) != 0;
	ak_comp *e = comp_new(AK_F_EFFECT | (render ? AK_F_RENDER : AK_F_DIRTY), ctx, info);
	e->fn.run = run;
	e->seq = ++g->seq;
	if (e->node.flags & AK_F_DISPOSED) {
		// Made under a disposed owner: never runs.
	} else if (render) {
		// Its first run, now: one a user effect's write makes again
		// before the flush it's in ends is a pass late.
		if (AK_UNLIKELY(ak_diag_enabled)) e->diag_flush = g->flush_id + 1;
		effect_update(e);
	} else {
		ak_sig_enqueue(e);
		if (g->batch_depth == 0 && !g->flushing) ak_sig_flush();
	}
	return e;
}

ak_owner *ak_effect_owner(ak_effect *e) { return &e->owner; }

bool ak_effect_stale(const ak_effect *e) { return (e->node.flags & (AK_F_DIRTY | AK_F_PENDING | AK_F_DISPOSED)) != 0; }

ak_effect *ak_current_effect(void) { return g->effect; }

void *ak_effect_ctx(const ak_effect *e, void (*run)(void *ctx)) { return e && e->fn.run == run ? e->ctx : NULL; }

// Tracking and batching.

void *ak_untrack_begin(void) {
	ak_comp *prev = g->active;
	g->active = NULL;
	return prev;
}

void ak_untrack_end(void *token) { g->active = token; }

void ak_untrack(void (*fn)(void *ctx), void *ctx) {
	void *t = ak_untrack_begin();
	fn(ctx);
	ak_untrack_end(t);
}

bool ak_tracking(void) { return g->active != NULL; }

bool ak_signal_in_memo(void) { return g->in_memo > 0; }

void ak_batch_begin(void) { g->batch_depth++; }

void ak_batch_end(void) {
	AK_ASSERT(g->batch_depth > 0);
	if (--g->batch_depth == 0 && !g->flushing && g->queue.len) ak_sig_flush();
}

void ak_signal_batch(void (*fn)(void *ctx), void *ctx) {
	ak_batch_begin();
	fn(ctx);
	ak_batch_end();
}

bool ak_signal_batching(void) { return g->batch_depth > 0; }

int ak_batch_suspend(void) {
	AK_ASSERT(!g->flushing);
	int d = g->batch_depth;
	g->batch_depth = 0;
	if (d && g->queue.len) ak_sig_flush();
	return d;
}

void ak_batch_resume(int depth) {
	AK_ASSERT(g->batch_depth == 0);
	g->batch_depth = depth;
}
bool ak_signal_flushing(void) { return g->flushing; }

void ak_signal_set_after_flush(void (*fn)(void *ctx), void *ctx) {
	g->after_flush = fn;
	g->after_flush_ctx = ctx;
}

// The flush.

void ak_sig_enqueue(ak_comp *e) {
	if (e->node.flags & AK_F_QUEUED) return;
	e->node.flags |= AK_F_QUEUED;
	g->enqueued++;  // a count, not a branch: what a write runs (wide-fanout)
	ak_owner_retain(&e->owner);
	ak_vec_push(&g->queue, e);
}

// Render effects first, then creation order.
static inline uint64_t order(const ak_comp *c) {
	return (c->node.flags & AK_F_RENDER ? 0 : 1ull << 63) | c->seq;
}

static void sift(ak_comp **a, size_t i, size_t n) {
	for (;;) {
		size_t c = 2 * i + 1;
		if (c >= n) return;
		if (c + 1 < n && order(a[c + 1]) > order(a[c])) c++;
		if (order(a[i]) >= order(a[c])) return;
		ak_comp *t = a[i];
		a[i] = a[c];
		a[c] = t;
		i = c;
	}
}

// sort_effects sorts in place, allocating nothing: insertion sort for the
// usual short, nearly sorted queue, heapsort beyond.
static void sort_effects(ak_comp **a, size_t n) {
	if (n <= 16) {
		for (size_t i = 1; i < n; i++) {
			ak_comp *x = a[i];
			uint64_t k = order(x);
			size_t j = i;
			for (; j > 0 && order(a[j - 1]) > k; j--) a[j] = a[j - 1];
			a[j] = x;
		}
		return;
	}
	for (size_t i = n / 2; i-- > 0;) sift(a, i, n);
	for (size_t end = n - 1; end > 0; end--) {
		ak_comp *t = a[0];
		a[0] = a[end];
		a[end] = t;
		sift(a, 0, end);
	}
}

static void drop_queue(void) {
	for (uint32_t i = 0; i < g->queue.len; i++) {
		ak_comp *e = g->queue.data[i];
		e->node.flags &= ~(uint32_t)AK_F_QUEUED;
		ak_owner_release(&e->owner);
	}
	g->queue.len = 0;
}

// diag_render_run checks a render effect about to run: one that ran in
// this flush already (or was made in it) runs again because an effect of
// the app's wrote what it reads in the pass before (ws, n of them): a
// pass late (effect-in-effect-pass). The writer is the one whose node
// it read.
static void diag_render_run(ak_comp *e, const diag_write *ws, uint32_t nw) {
	if (e->diag_flush == g->flush_id + 1) {
		for (uint32_t i = 0; i < AK_DIAG_WRITES && nw; i++) {
			const diag_write *d = &ws[i];
			if (!d->w || !ak_comp_reads(e, d->n)) continue;
			ak_comp *w = d->w;
			// Once per binding's site.
			if (ak_diag_first(ak_comp_site(e), "effect-in-effect-pass")) {
				char wn[512], en[512], msg[1536];
				ak_comp_name(w, wn, sizeof wn);
				ak_comp_name(e, en, sizeof en);
				if (w->wrote) snprintf(msg, sizeof msg, "%s wrote %.*s, which %s reads: the binding ran twice in one flush, a pass late; a let or a following state settles in one pass", wn, (int)w->wrote_len, w->wrote, en);
				else snprintf(msg, sizeof msg, "%s wrote what %s reads: the binding ran twice in one flush, a pass late; a let or a following state settles in one pass", wn, en);
				const char *where[2] = {wn, en};
				ak_diag("effect-in-effect-pass", msg, where, 2);
			}
			break;
		}
	}
	e->diag_flush = g->flush_id + 1;
}

// run_pass runs the effects of a pass, in g->running's order; with diag,
// it checks each binding as it runs (inlined twice: the check costs the
// pass without it nothing).
static AK_ALWAYS_INLINE void run_pass(bool diag, const diag_write *ws, uint32_t nw) {
	for (uint32_t i = 0; i < g->running.len; i++) {
		ak_comp *e = g->running.data[i];
		g->running.data[i] = NULL;
		uint32_t f = e->node.flags &= ~(uint32_t)AK_F_QUEUED;
		if (!(f & AK_F_DISPOSED)) {
			if ((f & AK_F_DIRTY) || ((f & AK_F_PENDING) && ak_sig_check_dirty(e))) {
				if (diag && (f & AK_F_RENDER)) diag_render_run(e, ws, nw);
				effect_update(e);
			} else {
				e->node.flags &= ~(uint32_t)AK_F_PENDING;
			}
		}
		ak_owner_release(&e->owner);
	}
}

void ak_sig_flush(void) {
	g->flushing = true;
	diag_clear();
	for (int round = 0;; round++) {
		for (int pass = 0; g->queue.len; pass++) {
			if (pass == AK_MAX_PASSES) {
				// The effect to name: one that wrote a state it named, the
				// loop's writer, else the first.
				ak_comp *e = g->queue.data[0];
				for (uint32_t i = 0; i < g->queue.len; i++)
					if (g->queue.data[i]->wrote) {
						e = g->queue.data[i];
						break;
					}
				char d[256];
				ak_sig_describe(e, d, sizeof d);
				const char *wrote = e->wrote;
				int len = (int)e->wrote_len;
				drop_queue();
				g->flushing = false;
				if (wrote) ak_sig_panic("effect-loop", "signal: %s is still writing %.*s after %d passes (an infinite loop?)", d, len, wrote, AK_MAX_PASSES);
				else ak_sig_panic("effect-loop", "signal: effects still dirty after %d passes (an infinite loop?), e.g. %s", AK_MAX_PASSES, d);
				return;
			}
			ak_comp_vec t = g->running;
			g->running = g->queue;
			g->queue = t;
			g->queue.len = 0;
			sort_effects(g->running.data, g->running.len);
			// The pass, with the diagnostics' check or without: one branch
			// here, none per effect.
			if (AK_UNLIKELY(ak_diag_enabled)) {
				// The bindings this pass runs again were dirtied by the
				// last pass's writes: the user effect that made them.
				diag_write ws[AK_DIAG_WRITES];
				uint32_t nw = diag_take(ws);
				run_pass(true, ws, nw);
				diag_release(ws);
			} else {
				run_pass(false, NULL, 0);
			}
			g->running.len = 0;
		}
		if (!g->after_flush) break;
		if (round == AK_MAX_PASSES) {
			drop_queue();
			g->flushing = false;
			ak_sig_panic("effect-loop", "signal: the after-flush hook dirtied effects %d times (an infinite loop?)", AK_MAX_PASSES);
			return;
		}
		g->after_flush(g->after_flush_ctx);
		if (!g->queue.len) break;
	}
	diag_clear();
	// What runs from now on (a batch's new bindings, at once) is the next
	// flush's: diag_flush is flush_id + 1 for this one's.
	g->flush_id++;
	g->flushing = false;
}

// ---- Diagnostics ----

const char *ak_comp_name(const ak_comp *c, char *buf, size_t n) { return ak_sig_describe(c, buf, n); }

const void *ak_comp_site(const ak_comp *c) {
	if (c->info && c->info->site) return c->info->site(c->ctx);
	return c;
}

uint64_t ak_signal_enqueued(void) { return g->enqueued; }

void ak_signal_each_reader(const ak_node *n, void (*fn)(void *ctx, const ak_comp *c), void *ctx) {
	for (const ak_link *l = n->subs; l; l = l->next_sub) fn(ctx, l->sub);
}

// reads_node walks c's deps, and the memos' among them, for n; depth
// bounds a walk the graph's acyclic shape already ends.
static bool reads_node(const ak_comp *c, const ak_node *n, int depth) {
	for (const ak_link *l = c->deps; l; l = l->next_dep) {
		if (l->dep == n) return true;
		if (depth > 0 && (l->dep->flags & AK_F_MEMO) && reads_node((const ak_comp *)l->dep, n, depth - 1)) return true;
	}
	return false;
}

bool ak_comp_reads(const ak_comp *c, const ak_node *n) { return reads_node(c, n, 64); }
