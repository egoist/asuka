// The reactive graph (core/src/signal), ported from signal/*_test.go.
// The graph holds no values, so the tests keep theirs in small typed
// helpers: isig (an int signal) and imemo (an int memo).
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "t.h"
#include "signal/signal.h"

#if !defined(_WIN32)
#include <pthread.h>
#endif

typedef struct {
	ak_node n;
	int v;
} isig;

static int get(isig *s) {
	ak_signal_read(&s->n);
	return s->v;
}

static void set(isig *s, int v) {
	if (s->v == v) return; // equal: skipped, as signal.New's
	s->v = v;
	ak_signal_write(&s->n);
}

typedef struct {
	ak_memo *m;
	int v, runs;
	bool ready;
	int (*fn)(void *ctx);
	void *ctx;
} imemo;

static bool imemo_update(void *p) {
	imemo *m = p;
	m->runs++;
	int v = m->fn(m->ctx);
	bool changed = !m->ready || v != m->v;
	m->v = v;
	m->ready = true;
	return changed;
}

static void imemo_init(imemo *m, int (*fn)(void *ctx), void *ctx) {
	*m = (imemo){.fn = fn, .ctx = ctx};
	m->m = ak_memo_new(imemo_update, m, NULL);
}

static int mget(imemo *m) {
	ak_memo_read(m->m);
	return m->v;
}

static void mfree(imemo *m) { ak_owner_dispose(ak_memo_owner(m->m)); }

#define EFFECT(fn, ctx) ak_effect_new(fn, ctx, AK_EFFECT_USER, NULL)
#define RENDER(fn, ctx) ak_effect_new(fn, ctx, AK_EFFECT_RENDER, NULL)

// A log of strings, for orders and contexts.
typedef struct {
	char s[512];
} tlog;

static void logf_(tlog *l, const char *w) {
	if (l->s[0]) strcat(l->s, ",");
	strcat(l->s, w);
}

// Panics and uncaught errors, recorded instead of aborting.
static struct {
	int n;
	char msg[512];
} panics;

static void on_panic(void *ctx, const char *msg) {
	panics.n++;
	snprintf(panics.msg, sizeof panics.msg, "%s", msg);
}

static void catch_panics(void) {
	memset(&panics, 0, sizeof panics);
	ak_signal_set_panic_hook(on_panic, NULL);
}

static void stop_catching_panics(void) { ak_signal_set_panic_hook(NULL, NULL); }

// ---- TestEffectRunsOnChange

static struct {
	isig s;
	int got[8], n;
} erc;

static void erc_effect(void *ctx) { erc.got[erc.n++] = get(&erc.s); }
static void erc_build(void *ctx, ak_owner *root) { EFFECT(erc_effect, NULL); }

TEST(effect_runs_on_change) {
	erc.s.v = 1;
	ak_owner *r = ak_root(erc_build, NULL);
	set(&erc.s, 2);
	set(&erc.s, 2); // equal: skipped
	set(&erc.s, 3);
	CHECK_EQ_INT(erc.n, 3);
	CHECK_EQ_INT(erc.got[0], 1);
	CHECK_EQ_INT(erc.got[1], 2);
	CHECK_EQ_INT(erc.got[2], 3);
	ak_owner_dispose(r);
	CHECK(!erc.s.n.subs);
}

// ---- TestDiamondIsGlitchFree: a -> b, a -> c, (b, c) -> d: d sees
// consistent b and c, once.

static struct {
	isig a;
	imemo b, c;
	int druns, glitches;
} dia;

static int dia_b(void *ctx) { return get(&dia.a) * 2; }
static int dia_c(void *ctx) { return get(&dia.a) * 3; }
static void dia_d(void *ctx) {
	dia.druns++;
	int x = mget(&dia.b), y = mget(&dia.c);
	if (y * 2 != x * 3) dia.glitches++;
}
static void dia_build(void *ctx, ak_owner *root) {
	imemo_init(&dia.b, dia_b, NULL);
	imemo_init(&dia.c, dia_c, NULL);
	EFFECT(dia_d, NULL);
}

TEST(diamond_is_glitch_free) {
	dia.a.v = 1;
	ak_owner *r = ak_root(dia_build, NULL);
	set(&dia.a, 2);
	set(&dia.a, 5);
	CHECK_EQ_INT(dia.glitches, 0);
	CHECK_EQ_INT(dia.b.runs, 3);
	CHECK_EQ_INT(dia.c.runs, 3);
	CHECK_EQ_INT(dia.druns, 3);
	ak_owner_dispose(r);
	CHECK(!dia.a.n.subs);
}

// ---- TestMemoIsLazyAndCached

static isig lazy_a;
static int lazy_fn(void *ctx) { return get(&lazy_a) + 1; }

TEST(memo_is_lazy_and_cached) {
	lazy_a.v = 1;
	imemo m;
	imemo_init(&m, lazy_fn, NULL);
	CHECK_EQ_INT(m.runs, 0);
	CHECK_EQ_INT(mget(&m), 2);
	CHECK_EQ_INT(mget(&m), 2);
	CHECK_EQ_INT(m.runs, 1);
	set(&lazy_a, 5);
	CHECK_EQ_INT(m.runs, 1); // not eagerly
	CHECK_EQ_INT(mget(&m), 6);
	CHECK_EQ_INT(m.runs, 2);
	mfree(&m);
	CHECK(!lazy_a.n.subs);
}

// ---- TestMemoEqualityCutsOff

static struct {
	isig a;
	imemo parity;
	int runs;
} eq;

static int eq_parity(void *ctx) { return get(&eq.a) % 2 == 0; }
static void eq_effect(void *ctx) {
	mget(&eq.parity);
	eq.runs++;
}
static void eq_build(void *ctx, ak_owner *root) {
	imemo_init(&eq.parity, eq_parity, NULL);
	EFFECT(eq_effect, NULL);
}

TEST(memo_equality_cuts_off) {
	eq.a.v = 1;
	ak_owner *r = ak_root(eq_build, NULL);
	set(&eq.a, 3); // parity unchanged
	set(&eq.a, 5);
	CHECK_EQ_INT(eq.runs, 1);
	set(&eq.a, 6);
	CHECK_EQ_INT(eq.runs, 2);
	ak_owner_dispose(r);
}

// ---- TestDeepChainPropagates

static struct {
	isig a;
	imemo chain[50];
	int got;
} deep;

static int deep_first(void *ctx) { return get(&deep.a) + 1; }
static int deep_next(void *ctx) { return mget(ctx) + 1; }
static void deep_effect(void *ctx) { deep.got = mget(&deep.chain[49]); }
static void deep_build(void *ctx, ak_owner *root) { EFFECT(deep_effect, NULL); }

TEST(deep_chain_propagates) {
	imemo_init(&deep.chain[0], deep_first, NULL);
	for (int i = 1; i < 50; i++) imemo_init(&deep.chain[i], deep_next, &deep.chain[i - 1]);
	ak_owner *r = ak_root(deep_build, NULL);
	set(&deep.a, 10);
	CHECK_EQ_INT(deep.got, 60);
	ak_owner_dispose(r);
	for (int i = 49; i >= 0; i--) mfree(&deep.chain[i]);
	CHECK(!deep.a.n.subs);
}

// ---- TestDynamicDependencies

static struct {
	isig cond, x, y;
	int runs, got;
} dyn;

static void dyn_effect(void *ctx) {
	dyn.runs++;
	dyn.got = get(&dyn.cond) ? get(&dyn.x) : get(&dyn.y);
}
static void dyn_build(void *ctx, ak_owner *root) { EFFECT(dyn_effect, NULL); }

TEST(dynamic_dependencies) {
	dyn.cond.v = 1;
	dyn.x.v = 10;
	dyn.y.v = 20;
	ak_owner *r = ak_root(dyn_build, NULL);
	set(&dyn.y, 21); // not a dep yet
	CHECK_EQ_INT(dyn.runs, 1);
	set(&dyn.cond, 0);
	set(&dyn.x, 11); // no longer a dep
	CHECK_EQ_INT(dyn.runs, 2);
	CHECK_EQ_INT(dyn.got, 21);
	CHECK(!dyn.x.n.subs); // x still has a subscriber
	ak_owner_dispose(r);
}

// ---- TestBatch

static struct {
	isig a, b;
	int runs;
	bool flushing;
} bat;

static void bat_effect(void *ctx) {
	get(&bat.a);
	get(&bat.b);
	bat.runs++;
	bat.flushing = ak_signal_flushing();
}
static void bat_build(void *ctx, ak_owner *root) { EFFECT(bat_effect, NULL); }
static void bat_writes(void *ctx) {
	set(&bat.a, 10);
	set(&bat.b, 20);
	CHECK(ak_signal_batching());
	CHECK_EQ_INT(bat.runs, 1); // not inside the batch
}

TEST(batch) {
	bat.a.v = 1;
	bat.b.v = 2;
	ak_owner *r = ak_root(bat_build, NULL);
	ak_signal_batch(bat_writes, NULL);
	CHECK_EQ_INT(bat.runs, 2);
	CHECK(bat.flushing);
	ak_batch_begin();
	ak_batch_begin();
	set(&bat.a, 11);
	ak_batch_end();
	CHECK_EQ_INT(bat.runs, 2); // the outer batch is still open
	set(&bat.b, 21);
	ak_batch_end();
	CHECK_EQ_INT(bat.runs, 3);
	CHECK(!ak_signal_batching() && !ak_signal_flushing());
	ak_owner_dispose(r);
}

// ---- TestDisposeStopsEffects

static struct {
	isig a;
	imemo m;
	int runs, cleanups;
} dse;

static void dse_cleanup(void *ctx) { dse.cleanups++; }
static void dse_effect(void *ctx) {
	get(&dse.a);
	dse.runs++;
	ak_on_cleanup(dse_cleanup, NULL);
}
static int dse_memo(void *ctx) { return get(&dse.a); }
static void dse_reader(void *ctx) { mget(&dse.m); }
static void dse_build(void *ctx, ak_owner *root) {
	EFFECT(dse_effect, NULL);
	imemo_init(&dse.m, dse_memo, NULL);
	EFFECT(dse_reader, NULL);
}

TEST(dispose_stops_effects) {
	ak_owner *r = ak_root(dse_build, NULL);
	set(&dse.a, 1);
	CHECK_EQ_INT(dse.runs, 2);
	CHECK_EQ_INT(dse.cleanups, 1);
	ak_owner_dispose(r);
	CHECK_EQ_INT(dse.cleanups, 2);
	set(&dse.a, 2);
	CHECK_EQ_INT(dse.runs, 2); // not after dispose
	CHECK(!dse.a.n.subs);      // no subscribers after dispose
}

// ---- A cleanup that writes what its effect follows, as the effect goes,
// doesn't run it again: disposing stops it following first (rfcs/0016).

static struct {
	isig a;
	int runs, cleanups;
} cwf;

static void cwf_cleanup(void *ctx) {
	cwf.cleanups++;
	set(&cwf.a, cwf.a.v + 1);
}
static void cwf_effect(void *ctx) {
	get(&cwf.a);
	cwf.runs++;
	ak_on_cleanup(cwf_cleanup, NULL);
}
static void cwf_build(void *ctx, ak_owner *root) { EFFECT(cwf_effect, NULL); }

TEST(dispose_cleanup_writing_what_its_effect_follows_runs_it_no_more) {
	ak_owner *r = ak_root(cwf_build, NULL);
	CHECK_EQ_INT(cwf.runs, 1);
	ak_owner_dispose(r);
	CHECK_EQ_INT(cwf.cleanups, 1);
	CHECK_EQ_INT(cwf.runs, 1);
	CHECK(!cwf.a.n.subs);
}

// ---- TestNestedEffectsAreDisposedOnRerun

static struct {
	isig outer, inner;
	int inner_runs;
} nest;

static void nest_inner(void *ctx) {
	get(&nest.inner);
	nest.inner_runs++;
}
static void nest_outer(void *ctx) {
	get(&nest.outer);
	EFFECT(nest_inner, NULL);
}
static void nest_build(void *ctx, ak_owner *root) { EFFECT(nest_outer, NULL); }

TEST(nested_effects_are_disposed_on_rerun) {
	ak_owner *r = ak_root(nest_build, NULL);
	set(&nest.outer, 1); // the old inner effect disposed, a new one made
	nest.inner_runs = 0;
	set(&nest.inner, 1);
	CHECK_EQ_INT(nest.inner_runs, 1); // else the old inner leaked
	ak_owner_dispose(r);
	CHECK(!nest.inner.n.subs && !nest.outer.n.subs);
}

// ---- TestRootIsNotDisposedWithParent

static struct {
	isig a;
	int runs;
	ak_owner *inner;
} rnd;

static void rnd_effect(void *ctx) {
	get(&rnd.a);
	rnd.runs++;
}
static void rnd_inner(void *ctx, ak_owner *root) { EFFECT(rnd_effect, NULL); }
static void rnd_outer(void *ctx, ak_owner *root) { rnd.inner = ak_root(rnd_inner, NULL); }

TEST(root_is_not_disposed_with_parent) {
	ak_owner *outer = ak_root(rnd_outer, NULL);
	ak_owner_dispose(outer);
	set(&rnd.a, 1);
	CHECK_EQ_INT(rnd.runs, 2); // else a detached root was disposed with its parent
	ak_owner_dispose(rnd.inner);
	CHECK(!rnd.a.n.subs);
}

// ---- TestRenderEffectsRunBeforeUserEffects

static struct {
	isig a;
	tlog order;
} reo;

static void reo_user1(void *ctx) {
	get(&reo.a);
	logf_(&reo.order, "user1");
}
static void reo_render(void *ctx) {
	get(&reo.a);
	logf_(&reo.order, "render");
}
static void reo_user2(void *ctx) {
	get(&reo.a);
	logf_(&reo.order, "user2");
}
static void reo_build(void *ctx, ak_owner *root) {
	EFFECT(reo_user1, NULL);
	RENDER(reo_render, NULL);
	EFFECT(reo_user2, NULL);
}

TEST(render_effects_run_before_user_effects) {
	ak_owner *r = ak_root(reo_build, NULL);
	CHECK_EQ_STR(reo.order.s, "render,user1,user2"); // a render effect runs at once
	reo.order.s[0] = 0;
	set(&reo.a, 1);
	CHECK_EQ_STR(reo.order.s, "render,user1,user2");
	ak_owner_dispose(r);
}

// Many effects, sorted by the heapsort: render effects first, then
// creation order.

static struct {
	isig a;
	int order[64], n;
} many;

static void many_effect(void *ctx) {
	get(&many.a);
	many.order[many.n++] = (int)(intptr_t)ctx;
}
static void many_build(void *ctx, ak_owner *root) {
	for (int i = 0; i < 48; i++) ak_effect_new(many_effect, (void *)(intptr_t)i, i % 6 == 5 ? AK_EFFECT_RENDER : AK_EFFECT_USER, NULL);
}

TEST(many_effects_run_in_order) {
	ak_owner *r = ak_root(many_build, NULL);
	many.n = 0;
	set(&many.a, 1);
	REQUIRE(many.n == 48);
	int k = 0;
	for (int i = 5; i < 48; i += 6) CHECK_EQ_INT(many.order[k++], i);
	for (int i = 0; i < 48; i++) {
		if (i % 6 != 5) CHECK_EQ_INT(many.order[k++], i);
	}
	ak_owner_dispose(r);
}

// ---- TestEffectWriteSchedulesAnotherPass

static struct {
	isig a, b;
	int got;
} ews;

static void ews_double(void *ctx) { set(&ews.b, get(&ews.a) * 2); }
static void ews_read(void *ctx) { ews.got = get(&ews.b); }
static void ews_build(void *ctx, ak_owner *root) {
	EFFECT(ews_double, NULL);
	EFFECT(ews_read, NULL);
}

TEST(effect_write_schedules_another_pass) {
	ak_owner *r = ak_root(ews_build, NULL);
	set(&ews.a, 4);
	CHECK_EQ_INT(ews.got, 8);
	ak_owner_dispose(r);
}

// ---- TestInfiniteLoopPanics

static isig loop_a;
static const ak_comp_info loop_info = {.label = "loop (signal_test.c)"};
static void loop_effect(void *ctx) {
	ak_signal_note_write("loop_a", 6);
	set(&loop_a, get(&loop_a) + 1);
}
static void loop_build(void *ctx, ak_owner *root) { ak_effect_new(loop_effect, NULL, AK_EFFECT_USER, &loop_info); }

TEST(infinite_loop_panics) {
	catch_panics();
	ak_owner *r = ak_root(loop_build, NULL);
	CHECK_EQ_INT(panics.n, 1);
	CHECK(strstr(panics.msg, "100 passes") != NULL);
	CHECK(strstr(panics.msg, "effect loop (signal_test.c) is still writing loop_a after 100 passes") != NULL);
	CHECK(!ak_signal_flushing());
	stop_catching_panics();
	ak_owner_dispose(r);
	CHECK(!loop_a.n.subs);
}

// ---- TestSetInsideMemoPanics

static isig wim_a;
static int wim_fn(void *ctx) {
	set(&wim_a, 1);
	return 0;
}

TEST(write_inside_memo_panics) {
	catch_panics();
	imemo m;
	imemo_init(&m, wim_fn, NULL);
	mget(&m);
	CHECK_EQ_INT(panics.n, 1);
	CHECK(strstr(panics.msg, "inside a memo") != NULL);
	stop_catching_panics();
	mfree(&m);
}

// ---- A memo's own signal: made in its run, read and written there

static isig fresh_old;
static isig fresh_made[4];  // each run's own, kept until the memo goes
static int fresh_runs;
static int fresh_fn(void *ctx) {
	AK_UNUSED(ctx);
	isig *s = &fresh_made[fresh_runs++];
	ak_signal_init(&s->n);
	ak_signal_born(&s->n);
	set(s, 1);           // nothing reads it
	set(s, get(s) + 1);  // the memo reads it, then writes it: its own
	return get(s) + get(&fresh_old);
}

TEST(memo_writes_its_own_signal) {
	catch_panics();
	imemo m;
	imemo_init(&m, fresh_fn, NULL);
	CHECK_EQ_INT(mget(&m), 2);
	CHECK_EQ_INT(panics.n, 0);
	// What it read of others still makes it run again.
	set(&fresh_old, 10);
	CHECK_EQ_INT(mget(&m), 12);
	CHECK_EQ_INT(panics.n, 0);
	CHECK_EQ_INT(fresh_runs, 2);
	stop_catching_panics();
	mfree(&m);
	for (int i = 0; i < fresh_runs; i++) ak_signal_drop(&fresh_made[i].n);
}

// A signal made before the memo, which it reads, is still not its to write.
static isig older;
static int older_fn(void *ctx) {
	AK_UNUSED(ctx);
	set(&older, get(&older) + 1);
	return 0;
}

TEST(memo_writes_an_older_signal_panics) {
	catch_panics();
	imemo m;
	imemo_init(&m, older_fn, NULL);
	mget(&m);
	CHECK_EQ_INT(panics.n, 1);
	CHECK(strstr(panics.msg, "inside a memo") != NULL);
	stop_catching_panics();
	mfree(&m);
}

// ---- TestUntrack

static struct {
	isig a, b;
	int runs;
} unt;

static void unt_read_b(void *ctx) { get(&unt.b); }
static void unt_effect(void *ctx) {
	get(&unt.a);
	ak_untrack(unt_read_b, NULL);
	void *t = ak_untrack_begin();
	CHECK(!ak_tracking());
	get(&unt.b);
	ak_untrack_end(t);
	CHECK(ak_tracking());
	unt.runs++;
}
static void unt_build(void *ctx, ak_owner *root) { EFFECT(unt_effect, NULL); }

TEST(untrack) {
	ak_owner *r = ak_root(unt_build, NULL);
	set(&unt.b, 1);
	CHECK_EQ_INT(unt.runs, 1); // an untracked read subscribed
	set(&unt.a, 1);
	CHECK_EQ_INT(unt.runs, 2);
	ak_owner_dispose(r);
}

// ---- TestContext

static struct {
	char key; // its address is the key
	tlog got;
	ak_owner *inner_root;
} ctxt;

static void ctx_use(void) { logf_(&ctxt.got, ak_use_context(&ctxt.key, "default")); }
static void ctx_effect(void *ctx) { ctx_use(); }
static void ctx_use_cb(void *ctx) { ctx_use(); }
static void ctx_inner(void *ctx) { EFFECT(ctx_effect, NULL); }
static void ctx_root(void *ctx, ak_owner *root) { ctx_use(); }
static void ctx_outer(void *ctx) {
	ctx_use();
	ak_provide(&ctxt.key, "inner", ctx_inner, NULL);
	ctxt.inner_root = ak_root(ctx_root, NULL);
}
static void ctx_build(void *ctx, ak_owner *root) {
	ctx_use();
	ak_provide(&ctxt.key, "outer", ctx_outer, NULL);
}

TEST(context) {
	ak_owner *r = ak_root(ctx_build, NULL);
	CHECK_EQ_STR(ctxt.got.s, "default,outer,outer,inner");
	void *v;
	CHECK(!ak_context_lookup(&ctxt.key, &v));
	ak_owner_set_context(r, &ctxt.key, "set");
	ak_owner_set_context(r, &ctxt.key, "reset");
	ctxt.got.s[0] = 0;
	ak_with_owner(r, ctx_use_cb, NULL);
	CHECK_EQ_STR(ctxt.got.s, "reset");
	ak_owner_dispose(r);
	ak_owner_dispose(ctxt.inner_root); // it outlived its parent, which it keeps for its contexts
}

// ---- TestSteadyStateAllocatesNothing

static struct {
	isig a;
	imemo m;
	int sum;
} ssa;

static int ssa_memo(void *ctx) { return get(&ssa.a) * 2; }
static void ssa_render(void *ctx) { ssa.sum += mget(&ssa.m) + get(&ssa.a); }
static void ssa_cleanup(void *ctx) { ssa.sum++; }
static void ssa_user(void *ctx) {
	ssa.sum += get(&ssa.a);
	ak_on_cleanup(ssa_cleanup, NULL);
}
static void ssa_build(void *ctx, ak_owner *root) {
	imemo_init(&ssa.m, ssa_memo, NULL);
	RENDER(ssa_render, NULL);
	EFFECT(ssa_user, NULL);
}

TEST(steady_state_allocates_nothing) {
	ak_owner *r = ak_root(ssa_build, NULL);
	set(&ssa.a, 1); // warm up
	uint64_t before = ak_alloc_count;
	for (int i = 2; i < 1002; i++) set(&ssa.a, i);
	CHECK_EQ_INT(ak_alloc_count - before, 0);
	ak_owner_dispose(r);
}

// ---- TestCatch: Catch takes fn's error and its effects' errors, then
// and later; the effect keeps running when what it read changes. A
// handler that raises hands it to the catcher around it. Without a
// catcher, an error goes to the default handler.

static const char *nums[] = {"0", "1", "2", "3"};

static struct {
	isig n;
	tlog errs;
	int uncaught;
	void *uncaught_err;
} cat;

static void cat_record(void *ctx, void *err) { logf_(&cat.errs, err); }
static void cat_effect(void *ctx) {
	int v = get(&cat.n);
	if (v % 2 == 1) {
		ak_raise((void *)nums[v]);
		CHECK(ak_failed());
		return;
	}
	CHECK(!ak_failed());
}
static void cat_f(void *ctx) { EFFECT(cat_effect, NULL); }
static void cat_build(void *ctx, ak_owner *root) { ak_catch(cat_f, NULL, cat_record, NULL); }

static void cat_raise(void *ctx) { ak_raise(ctx); }
static void cat_rethrow(void *ctx, void *err) {
	CHECK_EQ_STR(err, "inner");
	ak_raise("handler: inner");
}
static void cat_nested(void *ctx) { ak_catch(cat_raise, "inner", cat_rethrow, NULL); }
static void cat_build2(void *ctx, ak_owner *root) {
	ak_catch(cat_raise, "build", cat_record, NULL);
	ak_catch(cat_nested, NULL, cat_record, NULL);
}

static void cat_uncaught(void *ctx, void *err) {
	cat.uncaught++;
	cat.uncaught_err = err;
}
static void cat_up(void *ctx) { ak_raise("up"); }
static void cat_build3(void *ctx, ak_owner *root) { EFFECT(cat_up, NULL); }

TEST(catch) {
	ak_owner *stop = ak_root(cat_build, NULL);
	set(&cat.n, 1);
	set(&cat.n, 2);
	set(&cat.n, 3);
	ak_owner_dispose(ak_root(cat_build2, NULL));
	ak_owner_dispose(stop);
	CHECK_EQ_STR(cat.errs.s, "1,3,build,handler: inner");
	ak_signal_set_error_handler(cat_uncaught, NULL);
	ak_owner_dispose(ak_root(cat_build3, NULL));
	ak_raise("outside"); // outside every run and owner: at once
	ak_signal_set_error_handler(NULL, NULL);
	CHECK_EQ_INT(cat.uncaught, 2);
	CHECK_EQ_STR(cat.uncaught_err, "outside");
}

// An uncaught error with no handler set panics.
TEST(uncaught_error_panics) {
	catch_panics();
	ak_raise("boom");
	CHECK_EQ_INT(panics.n, 1);
	CHECK(strstr(panics.msg, "no catcher") != NULL);
	stop_catching_panics();
}

// ---- TestCatchCostsNothingOutside

static isig ccn_n;
static void ccn_effect(void *ctx) { get(&ccn_n); }
static void ccn_build(void *ctx, ak_owner *root) { EFFECT(ccn_effect, NULL); }

TEST(catch_costs_nothing_outside) {
	ak_owner *r = ak_root(ccn_build, NULL);
	set(&ccn_n, 1);
	uint64_t before = ak_alloc_count;
	for (int i = 0; i < 100; i++) set(&ccn_n, ccn_n.v + 1);
	CHECK_EQ_INT(ak_alloc_count - before, 0);
	ak_owner_dispose(r);
}

// A memo that raises counts as changed and raises again in each reader,
// which its catcher gets; it recovers when its sources change.

static struct {
	isig a;
	imemo m;
	int seen, failed_reads;
	tlog errs;
} merr;

static int merr_memo(void *ctx) {
	int v = get(&merr.a);
	if (v % 2) {
		ak_raise("odd");
		return -1;
	}
	return v * 10;
}
static void merr_effect(void *ctx) {
	if (!ak_memo_read(merr.m.m)) {
		merr.failed_reads++;
		return;
	}
	merr.seen = merr.m.v;
}
static void merr_record(void *ctx, void *err) { logf_(&merr.errs, err); }
static void merr_f(void *ctx) {
	imemo_init(&merr.m, merr_memo, NULL);
	EFFECT(merr_effect, NULL);
}
static void merr_build(void *ctx, ak_owner *root) { ak_catch(merr_f, NULL, merr_record, NULL); }

TEST(memo_error_reaches_readers) {
	ak_owner *r = ak_root(merr_build, NULL);
	CHECK_EQ_INT(merr.seen, 0);
	set(&merr.a, 1);
	CHECK_EQ_INT(merr.failed_reads, 1);
	CHECK_EQ_STR(merr.errs.s, "odd");
	CHECK_EQ_STR(ak_memo_error(merr.m.m), "odd");
	set(&merr.a, 2);
	CHECK_EQ_INT(merr.seen, 20);
	CHECK(ak_memo_error(merr.m.m) == NULL);
	set(&merr.a, 3);
	CHECK_EQ_STR(merr.errs.s, "odd,odd");
	ak_owner_dispose(r);
	CHECK(!merr.a.n.subs);
}

// ---- Owners

static struct {
	tlog order;
	int drops;
} own;

static void own_cleanup(void *ctx) { logf_(&own.order, ctx); }
static void own_drop(void *ctx) { own.drops++; }
static const ak_comp_info own_info = {.drop = own_drop};
static void reg_child(void *ctx) { ak_on_cleanup(own_cleanup, "child"); }
static void reg_parent(void *ctx) {
	ak_on_cleanup(own_cleanup, "a");
	ak_on_cleanup(own_cleanup, "b");
	ak_with_owner(ak_owner_new(), reg_child, NULL);
	ak_on_cleanup(own_cleanup, "c");
}

TEST(dispose_order) {
	own.order.s[0] = 0;
	ak_owner *o = ak_root_new();
	ak_with_owner(o, reg_parent, NULL);
	ak_owner_dispose(o);
	// Children first, then cleanups, last registered first.
	CHECK_EQ_STR(own.order.s, "child,c,b,a");
}

static void keep_effect(void *ctx) {}
static void keep_build(void *ctx, ak_owner *root) {
	ak_owner **out = ctx;
	*out = ak_effect_owner(ak_effect_new(keep_effect, NULL, AK_EFFECT_RENDER, &own_info));
}

TEST(retained_owner_outlives_disposal) {
	own.drops = 0;
	ak_owner *e = NULL;
	ak_owner *r = ak_root(keep_build, &e);
	REQUIRE(e != NULL);
	ak_owner_retain(e);
	ak_owner_dispose(r);
	CHECK(ak_owner_disposed(e));
	CHECK_EQ_INT(own.drops, 0); // still retained
	ak_owner_dispose(e);        // twice: a no-op
	ak_owner_release(e);
	CHECK_EQ_INT(own.drops, 1);
}

// An effect that disposes the root it belongs to, mid-run, then reads:
// the read tracks nothing and nothing is used after it's freed.

static struct {
	isig a, b;
	ak_owner *root;
	int runs;
} selfd;

static void selfd_effect(void *ctx) {
	selfd.runs++;
	if (get(&selfd.a) == 1) {
		ak_owner_dispose(selfd.root);
		get(&selfd.b);
	}
}
static void selfd_build(void *ctx, ak_owner *root) {
	selfd.root = root;
	ak_effect_new(selfd_effect, NULL, AK_EFFECT_USER, &own_info);
}

TEST(effect_disposes_its_own_root) {
	own.drops = 0;
	ak_root(selfd_build, NULL);
	set(&selfd.a, 1);
	CHECK_EQ_INT(selfd.runs, 2);
	CHECK_EQ_INT(own.drops, 1);
	CHECK(!selfd.a.n.subs && !selfd.b.n.subs);
	set(&selfd.a, 2);
	CHECK_EQ_INT(selfd.runs, 2);
}

// ak_with_owner: what fn makes belongs to the owner, which disposes it.

static struct {
	isig a;
	int runs;
} wo;

static void wo_effect(void *ctx) {
	get(&wo.a);
	wo.runs++;
}
static void wo_make(void *ctx) {
	CHECK(!ak_tracking());
	EFFECT(wo_effect, NULL);
}

TEST(with_owner) {
	ak_owner *o = ak_owner_new(); // no current owner: the caller holds it
	ak_with_owner(o, wo_make, NULL);
	set(&wo.a, 1);
	CHECK_EQ_INT(wo.runs, 2);
	ak_owner_dispose(o);
	set(&wo.a, 2);
	CHECK_EQ_INT(wo.runs, 2);
	CHECK(!wo.a.n.subs);
}

// ak_owner_new_in: an owner held by one owner, its contexts and catcher
// another's (an event's handler: the receiver's, where it's declared).

static struct {
	char key;
	ak_owner *holder, *where, *made;
	tlog got;
	int cleanups;
} oni;

static void oni_cleanup(void *ctx) { oni.cleanups++; }
static void oni_use(void *ctx) {
	logf_(&oni.got, ak_use_context(&oni.key, "default"));
	ak_on_cleanup(oni_cleanup, NULL);
}
static void oni_make(void *ctx) {
	oni.made = ak_owner_new_in(oni.holder);
	ak_with_owner(oni.made, oni_use, NULL);
}
static void oni_where(void *ctx) {
	oni.where = ak_owner_new();
	ak_owner_set_context(oni.where, &oni.key, "where");
	ak_with_owner(oni.where, oni_make, NULL);
}
static void oni_build(void *ctx, ak_owner *root) {
	oni.holder = ak_owner_new();
	ak_with_owner(root, oni_where, NULL);
}

TEST(owner_new_in) {
	memset(&oni, 0, sizeof oni);
	ak_owner *r = ak_root(oni_build, NULL);
	CHECK_EQ_STR(oni.got.s, "where");  // the current owner's contexts
	ak_owner_retain(oni.made);
	ak_owner_dispose(oni.where);       // not disposed with where it was made
	CHECK(!ak_owner_disposed(oni.made));
	CHECK_EQ_INT(oni.cleanups, 0);
	ak_owner_dispose(oni.holder);      // but with its holder
	CHECK(ak_owner_disposed(oni.made));
	CHECK_EQ_INT(oni.cleanups, 1);
	ak_owner_release(oni.made);
	ak_owner_dispose(r);
	// Made with the current owner as the holder: an ordinary child.
	oni.got.s[0] = 0;
	r = ak_root(oni_build, NULL);
	oni.holder = oni.where;
	ak_with_owner(oni.where, oni_make, NULL);
	ak_owner_retain(oni.made);
	ak_owner_dispose(oni.where);
	CHECK(ak_owner_disposed(oni.made));
	ak_owner_release(oni.made);
	ak_owner_dispose(r);
}

// ---- Memos let go of their sources when unwatched.

static struct {
	isig a;
	imemo m;
	int unwatched;
	ak_owner *readers;
} unw;

static void unw_unwatched(void *ctx) { unw.unwatched++; }
static const ak_comp_info unw_info = {.unwatched = unw_unwatched};
static int unw_memo(void *ctx) { return get(&unw.a) + 1; }
static void unw_effect(void *ctx) { mget(&unw.m); }
static void unw_readers(void *ctx) { EFFECT(unw_effect, NULL); }
static void unw_build(void *ctx, ak_owner *root) {
	unw.m = (imemo){.fn = unw_memo};
	unw.m.m = ak_memo_new(imemo_update, &unw.m, &unw_info);
	unw.readers = ak_owner_new();
	ak_with_owner(unw.readers, unw_readers, NULL);
}

TEST(memo_unwatched) {
	ak_owner *r = ak_root(unw_build, NULL);
	CHECK_EQ_INT(unw.m.runs, 1);
	CHECK(unw.a.n.subs != NULL);
	ak_owner_dispose(unw.readers);
	CHECK_EQ_INT(unw.unwatched, 1);
	CHECK(!unw.a.n.subs); // the memo let go of a
	set(&unw.a, 5);
	CHECK_EQ_INT(unw.m.runs, 1);
	CHECK_EQ_INT(mget(&unw.m), 6); // recomputes on its next read
	CHECK_EQ_INT(unw.m.runs, 2);
	ak_owner_dispose(r);
}

// A disposed memo keeps its last value and tracks nothing.

static struct {
	isig a, b;
	imemo m;
	int runs, seen;
} dmemo;

static int dmemo_fn(void *ctx) { return get(&dmemo.a) * 2; }
static void dmemo_effect(void *ctx) {
	get(&dmemo.b);
	dmemo.runs++;
	dmemo.seen = mget(&dmemo.m);
}
static void dmemo_build(void *ctx, ak_owner *root) { EFFECT(dmemo_effect, NULL); }

TEST(disposed_memo) {
	dmemo.a.v = 1;
	imemo_init(&dmemo.m, dmemo_fn, NULL);
	ak_owner_retain(ak_memo_owner(dmemo.m.m));
	ak_owner *r = ak_root(dmemo_build, NULL);
	CHECK_EQ_INT(dmemo.seen, 2);
	ak_owner_dispose(ak_memo_owner(dmemo.m.m));
	CHECK(!dmemo.a.n.subs);
	set(&dmemo.b, 1);
	CHECK_EQ_INT(dmemo.runs, 2);
	CHECK_EQ_INT(dmemo.seen, 2);
	set(&dmemo.a, 7);
	CHECK_EQ_INT(dmemo.runs, 2);
	ak_owner_dispose(r);
	ak_owner_release(ak_memo_owner(dmemo.m.m));
}

// A dropped signal unsubscribes its readers.

static struct {
	isig s, t;
	int runs;
} drp;

static void drp_effect(void *ctx) {
	get(&drp.s);
	get(&drp.t);
	drp.runs++;
}
static void drp_build(void *ctx, ak_owner *root) { EFFECT(drp_effect, NULL); }

TEST(signal_drop) {
	ak_owner *r = ak_root(drp_build, NULL);
	ak_signal_drop(&drp.s.n);
	CHECK(!drp.s.n.subs);
	set(&drp.t, 1);
	CHECK_EQ_INT(drp.runs, 2);
	CHECK(!drp.s.n.subs); // the re-run's read tracked nothing
	ak_owner_dispose(r);
}

// The after-flush hook runs once effects settle; its writes run their
// effects, then the hook again, in the same flush.

static struct {
	isig a, b;
	int hooks, runs;
} afl;

static void afl_hook(void *ctx) {
	afl.hooks++;
	CHECK(ak_signal_flushing());
	if (afl.b.v == 0) set(&afl.b, 1);
}
static void afl_effect(void *ctx) {
	get(&afl.a);
	get(&afl.b);
	afl.runs++;
}
static void afl_build(void *ctx, ak_owner *root) { EFFECT(afl_effect, NULL); }

TEST(after_flush) {
	ak_signal_set_after_flush(afl_hook, NULL);
	ak_owner *r = ak_root(afl_build, NULL);
	// The first run, then b's write from the hook, then the hook again.
	CHECK_EQ_INT(afl.runs, 2);
	CHECK_EQ_INT(afl.hooks, 2);
	set(&afl.a, 1);
	CHECK_EQ_INT(afl.runs, 3);
	CHECK_EQ_INT(afl.hooks, 3);
	ak_signal_set_after_flush(NULL, NULL);
	ak_owner_dispose(r);
}

// ---- Ambient state. The C core needs no
// ambient mechanism: process-wide state the main thread's effects follow
// (the app's language) is a plain signal, and a worker thread that reads
// it doesn't track (ak_signal_on_main_thread). These are the same
// semantics, kept.

static isig ambient;
static void ambient_read(void) {
	if (ak_signal_on_main_thread()) ak_signal_read(&ambient.n);
}
static void ambient_changed(void) { set(&ambient, ambient.v + 1); }

static struct {
	int runs;
} amb1;

static void amb1_effect(void *ctx) {
	amb1.runs++;
	ambient_read();
}
static void amb1_build(void *ctx, ak_owner *root) { RENDER(amb1_effect, NULL); }

TEST(ambient_effect) {
	ak_owner *r = ak_root(amb1_build, NULL);
	ambient_changed();
	CHECK_EQ_INT(amb1.runs, 2);
	ambient_changed();
	CHECK_EQ_INT(amb1.runs, 3);
	ak_owner_dispose(r);
	ambient_changed();
	CHECK_EQ_INT(amb1.runs, 3);
}

// An effect follows what it reads alone: an effect it makes, or what it
// runs untracked, reads for itself.

static struct {
	int outer, inner, untracked, memo_reader, plain;
	int lang;
	imemo m;
} amb2;

static void amb2_read(void *ctx) { ambient_read(); }
static void amb2_inner(void *ctx) {
	amb2.inner++;
	ambient_read();
}
static void amb2_outer(void *ctx) {
	amb2.outer++;
	RENDER(amb2_inner, NULL);
}
static void amb2_untracked(void *ctx) {
	amb2.untracked++;
	ak_untrack(amb2_read, NULL);
}
static void amb2_plain(void *ctx) {
	amb2.plain++;
	ak_owner *o = ak_owner_new();
	ak_with_owner(o, amb2_read, NULL);
	ak_owner_dispose(o);
}
static int amb2_memo(void *ctx) {
	ambient_read();
	return amb2.lang;
}
static void amb2_memo_reader(void *ctx) {
	amb2.memo_reader++;
	mget(&amb2.m);
}
static void amb2_build(void *ctx, ak_owner *root) {
	RENDER(amb2_outer, NULL);
	RENDER(amb2_untracked, NULL);
	RENDER(amb2_plain, NULL);
	imemo_init(&amb2.m, amb2_memo, NULL);
	RENDER(amb2_memo_reader, NULL);
}

TEST(ambient_is_the_readers) {
	ak_owner *r = ak_root(amb2_build, NULL);
	ambient_changed();
	CHECK_EQ_INT(amb2.inner, 2);
	CHECK_EQ_INT(amb2.outer, 1);
	CHECK_EQ_INT(amb2.untracked, 1);
	CHECK_EQ_INT(amb2.plain, 1);
	CHECK_EQ_INT(amb2.memo_reader, 1); // the memo's value didn't change
	amb2.lang = 1;
	ambient_changed();
	CHECK_EQ_INT(amb2.memo_reader, 2);
	ak_owner_dispose(r);
}

static struct {
	isig s;
	int runs;
} amb3;

static void amb3_effect(void *ctx) {
	amb3.runs++;
	get(&amb3.s);
	ambient_read();
}
static void amb3_build(void *ctx, ak_owner *root) { RENDER(amb3_effect, NULL); }

TEST(ambient_with_signals) {
	amb3.s.v = 1;
	ak_owner *r = ak_root(amb3_build, NULL);
	set(&amb3.s, 2);
	ambient_changed();
	set(&amb3.s, 3);
	ambient_changed();
	CHECK_EQ_INT(amb3.runs, 5);
	uint64_t before = ak_alloc_count;
	for (int i = 0; i < 100; i++) ambient_changed();
	CHECK_EQ_INT(ak_alloc_count - before, 0);
	ak_owner_dispose(r);
}

#if !defined(_WIN32)
// A write off the main thread panics (debug builds and tests); a read
// there knows not to track.

static struct {
	isig s;
	bool on_main;
} thr;

static void *thr_worker(void *arg) {
	thr.on_main = ak_signal_on_main_thread();
	set(&thr.s, 1);
	return NULL;
}
static void thr_effect(void *ctx) { get(&thr.s); }
static void thr_build(void *ctx, ak_owner *root) { EFFECT(thr_effect, NULL); }

TEST(write_off_main_thread_panics) {
	ak_signal_bind_thread();
	CHECK(ak_signal_on_main_thread());
	ak_owner *r = ak_root(thr_build, NULL);
	catch_panics();
	pthread_t t;
	REQUIRE(pthread_create(&t, NULL, thr_worker, NULL) == 0);
	pthread_join(t, NULL);
	stop_catching_panics();
	CHECK(!thr.on_main);
	CHECK_EQ_INT(panics.n, 1);
	CHECK(strstr(panics.msg, "off the main thread") != NULL);
	ak_owner_dispose(r);
}
#endif

// ---- The benchmarks' shapes (bench_test.go, after js-reactivity-
// benchmark's kairo suite) allocate nothing once warm.

static isig bench_head;
static int bench_sum;

static void bench_writes(const char *name) {
	set(&bench_head, bench_head.v + 1);
	set(&bench_head, bench_head.v + 1);
	uint64_t before = ak_alloc_count;
	for (int i = 0; i < 200; i++) set(&bench_head, bench_head.v + 1);
	if (ak_alloc_count != before) T_FAIL("%s: %llu allocations in 200 writes", name, (unsigned long long)(ak_alloc_count - before));
}

static imemo bdeep[50];
static int bdeep_first(void *ctx) { return get(&bench_head) + 1; }
static void bdeep_effect(void *ctx) { bench_sum += mget(&bdeep[49]); }
static void bdeep_build(void *ctx, ak_owner *root) {
	imemo_init(&bdeep[0], bdeep_first, NULL);
	for (int i = 1; i < 50; i++) imemo_init(&bdeep[i], deep_next, &bdeep[i - 1]);
	EFFECT(bdeep_effect, NULL);
}

TEST(bench_deep_propagation) {
	ak_owner *r = ak_root(bdeep_build, NULL);
	bench_writes("deep");
	ak_owner_dispose(r);
}

static imemo bbroad1[50], bbroad2[50];
static int bbroad_m(void *ctx) { return get(&bench_head) + (int)(intptr_t)ctx; }
static void bbroad_effect(void *ctx) { bench_sum += mget(ctx); }
static void bbroad_build(void *ctx, ak_owner *root) {
	for (int i = 0; i < 50; i++) {
		imemo_init(&bbroad1[i], bbroad_m, (void *)(intptr_t)i);
		imemo_init(&bbroad2[i], deep_next, &bbroad1[i]);
		EFFECT(bbroad_effect, &bbroad2[i]);
	}
}

TEST(bench_broad_propagation) {
	ak_owner *r = ak_root(bbroad_build, NULL);
	bench_writes("broad");
	ak_owner_dispose(r);
}

static imemo bdia[5], bdia_total;
static int bdia_branch(void *ctx) { return get(&bench_head) + 1; }
static int bdia_sum(void *ctx) {
	int t = 0;
	for (int i = 0; i < 5; i++) t += mget(&bdia[i]);
	return t;
}
static void bdia_effect(void *ctx) { bench_sum += mget(&bdia_total); }
static void bdia_build(void *ctx, ak_owner *root) {
	for (int i = 0; i < 5; i++) imemo_init(&bdia[i], bdia_branch, NULL);
	imemo_init(&bdia_total, bdia_sum, NULL);
	EFFECT(bdia_effect, NULL);
}

TEST(bench_diamond) {
	ak_owner *r = ak_root(bdia_build, NULL);
	bench_writes("diamond");
	ak_owner_dispose(r);
}

static imemo bun_double, bun_inverse, bun_cur;
static int bun_double_fn(void *ctx) { return get(&bench_head) * 2; }
static int bun_inverse_fn(void *ctx) { return -get(&bench_head); }
static int bun_cur_fn(void *ctx) {
	int r = 0;
	for (int i = 0; i < 20; i++) r += get(&bench_head) % 2 ? mget(&bun_double) : mget(&bun_inverse);
	return r;
}
static void bun_effect(void *ctx) { bench_sum += mget(&bun_cur); }
static void bun_build(void *ctx, ak_owner *root) {
	imemo_init(&bun_double, bun_double_fn, NULL);
	imemo_init(&bun_inverse, bun_inverse_fn, NULL);
	imemo_init(&bun_cur, bun_cur_fn, NULL);
	EFFECT(bun_effect, NULL);
}

TEST(bench_unstable) {
	ak_owner *r = ak_root(bun_build, NULL);
	bench_writes("unstable");
	ak_owner_dispose(r);
	CHECK(!bench_head.n.subs);
}

// ---- A memo disposed while it updates: by its own update (it disposes
// its owner), or its update disposes the effect that reads it. The graph
// keeps what it walks alive and doesn't follow links the disposal freed.
// The test holds the memo, as the VM does what it still reads.

static struct {
	isig s1, s2;
	imemo m;
	ak_owner *reader;
	int dispose;  // 1: the memo itself, 2: its reader
	int runs;
} mdo;

static int mdo_memo(void *ctx) {
	int v = get(&mdo.s1);
	if (v == 1 && mdo.dispose == 1) ak_owner_dispose(ak_memo_owner(mdo.m.m));
	if (v == 1 && mdo.dispose == 2) ak_owner_dispose(mdo.reader);
	return v;
}
static void mdo_effect(void *ctx) {
	mget(&mdo.m);
	get(&mdo.s2);
	mdo.runs++;
}
static void mdo_build(void *ctx, ak_owner *root) {
	imemo_init(&mdo.m, mdo_memo, NULL);
	ak_owner_retain(ak_memo_owner(mdo.m.m));
	mdo.reader = ak_effect_owner(EFFECT(mdo_effect, NULL));
}

TEST(memo_disposes_itself_while_checked) {
	memset(&mdo, 0, sizeof mdo);
	mdo.dispose = 1;
	ak_owner *r = ak_root(mdo_build, NULL);
	CHECK_EQ_INT(mdo.runs, 1);
	set(&mdo.s1, 1);  // the effect checks the memo, which disposes itself
	CHECK_EQ_INT(mdo.m.v, 1);
	CHECK(!mdo.s1.n.subs);
	set(&mdo.s2, 1);  // the effect still runs on what it reads
	CHECK(mdo.runs >= 2);
	int runs = mdo.runs;
	ak_owner_dispose(r);
	set(&mdo.s2, 2);
	CHECK_EQ_INT(mdo.runs, runs);
	CHECK(!mdo.s1.n.subs && !mdo.s2.n.subs);
	ak_owner_release(ak_memo_owner(mdo.m.m));
}

TEST(memo_disposes_its_reader_while_checked) {
	memset(&mdo, 0, sizeof mdo);
	mdo.dispose = 2;
	ak_owner *r = ak_root(mdo_build, NULL);
	CHECK_EQ_INT(mdo.runs, 1);
	set(&mdo.s1, 1);  // the effect checks the memo, which disposes the effect
	CHECK_EQ_INT(mdo.runs, 1);
	set(&mdo.s2, 1);
	CHECK_EQ_INT(mdo.runs, 1);
	CHECK(!mdo.s2.n.subs);
	ak_owner_dispose(r);
	CHECK(!mdo.s1.n.subs);
	ak_owner_release(ak_memo_owner(mdo.m.m));
}

// Read untracked, the memo that disposes itself as it updates: nothing
// else holds it, so the read does until it's done.
static void mdo_build_alone(void *ctx, ak_owner *root) { imemo_init(&mdo.m, mdo_memo, NULL); }

TEST(memo_disposes_itself_while_read) {
	memset(&mdo, 0, sizeof mdo);
	mdo.dispose = 1;
	mdo.s1.v = 1;
	ak_owner *r = ak_root(mdo_build_alone, NULL);
	CHECK_EQ_INT(mget(&mdo.m), 1);
	CHECK_EQ_INT(mdo.m.runs, 1);
	CHECK(!mdo.s1.n.subs);
	ak_owner_dispose(r);
}

// ---- Made under a disposed owner: an effect that disposes its own root
// (and so itself) and goes on to register a cleanup and make effects, a
// memo and an owner. The cleanup runs at once; what it made never runs
// and is let go of as the effect returns, the effect then freed.

static struct {
	isig a, b;
	ak_owner *root;
	imemo m;
	int cleanups, child_runs, dropped, made;
} mud;

static void mud_drop(void *ctx) { mud.dropped++; }
static const ak_comp_info mud_info = {.drop = mud_drop};
static void mud_cleanup(void *ctx) { mud.cleanups++; }
static void mud_child(void *ctx) {
	get(&mud.b);
	mud.child_runs++;
}
static int mud_memo(void *ctx) { return get(&mud.b); }
static void mud_under(void *ctx) { ak_effect_new(mud_child, NULL, AK_EFFECT_USER, &mud_info); }
static void mud_effect(void *ctx) {
	if (get(&mud.a) != 1) return;
	ak_owner_dispose(mud.root);
	ak_on_cleanup(mud_cleanup, NULL);
	CHECK_EQ_INT(mud.cleanups, 1);
	ak_effect_new(mud_child, NULL, AK_EFFECT_USER, &mud_info);
	ak_effect_new(mud_child, NULL, AK_EFFECT_RENDER, &mud_info);
	imemo_init(&mud.m, mud_memo, NULL);
	mget(&mud.m);  // disposed as it's made: never computes
	ak_with_owner(ak_owner_new(), mud_under, NULL);
	mud.made++;
}
static void mud_build(void *ctx, ak_owner *root) {
	mud.root = root;
	ak_effect_new(mud_effect, NULL, AK_EFFECT_USER, &mud_info);
}

TEST(made_under_a_disposed_owner) {
	memset(&mud, 0, sizeof mud);
	ak_root(mud_build, NULL);
	set(&mud.a, 1);
	CHECK_EQ_INT(mud.made, 1);
	CHECK_EQ_INT(mud.cleanups, 1);
	CHECK_EQ_INT(mud.m.runs, 0);
	CHECK_EQ_INT(mud.child_runs, 0);
	set(&mud.b, 1);
	set(&mud.b, 2);
	CHECK_EQ_INT(mud.child_runs, 0);
	CHECK(!mud.a.n.subs && !mud.b.n.subs);
	CHECK_EQ_INT(mud.dropped, 4);  // the effect and the three it made: none kept
}

// ---- Follows (rfcs/0012): a state that follows what its update reads,
// and that may also be set. ifollow keeps an int, as imemo does.

typedef struct {
	ak_follow *f;
	int v, runs;
	bool ready;
	int (*fn)(void *ctx);
	void *ctx;
} ifollow;

static bool ifollow_update(void *p) {
	ifollow *f = p;
	f->runs++;
	int v = f->fn(f->ctx);
	bool changed = !f->ready || v != f->v;
	f->v = v;
	f->ready = true;
	return changed;
}

static void ifollow_init(ifollow *f, int (*fn)(void *ctx), void *ctx) {
	*f = (ifollow){.fn = fn, .ctx = ctx};
	f->f = ak_follow_new(ifollow_update, f, NULL);
}

static int fget(ifollow *f) {
	ak_follow_read(f->f);
	return f->v;
}

static bool *fset_written;

// fset is a write: the follow brought up to date first, then replaced.
static void fset(ifollow *f, int v) {
	ak_follow_refresh(f->f);
	if (fset_written) *fset_written = true;
	if (f->ready && f->v == v) return;  // equal: skipped
	f->v = v;
	f->ready = true;
	ak_follow_set(f->f);
}

static struct {
	isig s;
	ifollow f;
	int runs, seen, glitches;
	bool written;  // set since s last changed
	ak_owner *readers;
} fol;

static int fol_fn(void *ctx) {
	fset_written = &fol.written;
	fol.written = false;
	return get(&fol.s) * 10;
}
static void fol_effect(void *ctx) {
	fol.runs++;
	int s = get(&fol.s);
	fol.seen = fget(&fol.f);
	// Not set since s changed, it's s's: it's never behind what it reads.
	if (!fol.written && fol.seen != s * 10) fol.glitches++;
}
static void fol_readers(void *ctx) { EFFECT(fol_effect, NULL); }
static void fol_build(void *ctx, ak_owner *root) {
	ifollow_init(&fol.f, fol_fn, NULL);
	fol.readers = ak_owner_new();
	ak_with_owner(fol.readers, fol_readers, NULL);
}

static void fol_set_then_input(void *ctx) {
	fset(&fol.f, 99);
	set(&fol.s, 6);
}
static void fol_input_then_set(void *ctx) {
	set(&fol.s, 5);
	fset(&fol.f, 99);
}
static void fol_input_then_read(void *ctx) {
	set(&fol.s, 8);
	*(int *)ctx = fget(&fol.f);  // the update runs now, in the batch
}

TEST(follow_follows_until_set) {
	memset(&fol, 0, sizeof fol);
	fol.s.v = 1;
	ak_owner *r = ak_root(fol_build, NULL);
	CHECK_EQ_INT(fol.seen, 10);
	set(&fol.s, 2);
	CHECK_EQ_INT(fol.seen, 20);
	fset(&fol.f, 7);
	CHECK_EQ_INT(fol.seen, 7);
	CHECK_EQ_INT(fol.f.runs, 2);  // the set computed nothing
	fset(&fol.f, 7);  // equal: nothing runs
	CHECK_EQ_INT(fol.runs, 3);
	set(&fol.s, 3);
	CHECK_EQ_INT(fol.seen, 30);  // s again
	CHECK_EQ_INT(fol.runs, 4);
	CHECK_EQ_INT(fol.glitches, 0);
	ak_owner_dispose(r);
	CHECK(!fol.s.n.subs);
}

TEST(follow_write_and_input_in_one_batch) {
	memset(&fol, 0, sizeof fol);
	fol.s.v = 1;
	ak_owner *r = ak_root(fol_build, NULL);
	// A write after the input changed stands.
	ak_signal_batch(fol_input_then_set, NULL);
	CHECK_EQ_INT(fol.seen, 99);
	CHECK_EQ_INT(fol.runs, 2);
	// A write before it is replaced, when it changes.
	ak_signal_batch(fol_set_then_input, NULL);
	CHECK_EQ_INT(fol.seen, 60);
	CHECK_EQ_INT(fol.runs, 3);
	// Read right after the input changed, it's the input's.
	int got = 0;
	ak_signal_batch(fol_input_then_read, &got);
	CHECK_EQ_INT(got, 80);
	CHECK_EQ_INT(fol.glitches, 0);
	ak_owner_dispose(r);
}

TEST(follow_pulls_without_a_glitch) {
	memset(&fol, 0, sizeof fol);
	fol.s.v = 1;
	ak_owner *r = ak_root(fol_build, NULL);
	for (int i = 2; i < 20; i++) set(&fol.s, i);
	CHECK_EQ_INT(fol.glitches, 0);
	CHECK_EQ_INT(fol.runs, 19);  // once per write: no pass to catch up
	CHECK_EQ_INT(fol.f.runs, 19);
	ak_owner_dispose(r);
}

TEST(follow_keeps_its_sources_unwatched) {
	memset(&fol, 0, sizeof fol);
	fol.s.v = 1;
	ak_owner *r = ak_root(fol_build, NULL);
	fset(&fol.f, 5);
	ak_owner_dispose(fol.readers);  // its last reader goes
	CHECK(fol.s.n.subs != NULL);    // it still follows s
	CHECK_EQ_INT(fget(&fol.f), 5);  // the set value stands
	CHECK_EQ_INT(fol.f.runs, 1);
	set(&fol.s, 4);
	CHECK_EQ_INT(fol.f.runs, 1);     // lazy
	CHECK_EQ_INT(fget(&fol.f), 40);  // s's, once it changed
	// Held past its owner's disposal (as the VM holds it), it follows
	// nothing, and a set notifies nothing.
	ak_owner_retain(ak_follow_owner(fol.f.f));
	ak_owner_dispose(r);
	CHECK(!fol.s.n.subs);
	fset(&fol.f, 3);
	CHECK_EQ_INT(fget(&fol.f), 3);
	CHECK_EQ_INT(fol.f.runs, 2);
	ak_owner_release(ak_follow_owner(fol.f.f));
}

static int fixed_fn(void *ctx) { return 42; }

TEST(follow_reading_nothing_is_settled) {
	memset(&fol, 0, sizeof fol);
	fol.s.v = 1;
	ak_owner *r = ak_root(fol_build, NULL);
	ifollow k;
	ifollow_init(&k, fixed_fn, NULL);
	CHECK(!ak_follow_settled(k.f));  // not run yet
	CHECK_EQ_INT(fget(&k), 42);
	CHECK(ak_follow_settled(k.f));
	CHECK(!ak_follow_settled(fol.f.f));
	ak_owner_dispose(ak_follow_owner(k.f));
	ak_owner_dispose(r);
}

TEST(follow_steady_state_allocates_nothing) {
	memset(&fol, 0, sizeof fol);
	fol.s.v = 1;
	ak_owner *r = ak_root(fol_build, NULL);
	set(&fol.s, 2);  // warm up
	fset(&fol.f, 1);
	uint64_t before = ak_alloc_count;
	for (int i = 3; i < 1003; i++) {
		set(&fol.s, i);
		fset(&fol.f, -i);
	}
	CHECK_EQ_INT(ak_alloc_count - before, 0);
	CHECK_EQ_INT(fol.glitches, 0);
	ak_owner_dispose(r);
}

// ---- A signal told when its last reader goes (ak_wsignal) ----

static struct {
	ak_wsignal s;
	int unwatched, runs;
	bool read;
} ws;

static void ws_gone(ak_wsignal *s) {
	CHECK(s == &ws.s);
	ws.unwatched++;
}
static void ws_effect(void *ctx) {
	ws.runs++;
	if (ws.read) ak_signal_read(&ws.s.node);
}
static void ws_build(void *ctx, ak_owner *root) {
	RENDER(ws_effect, NULL);
	RENDER(ws_effect, NULL);
}

TEST(wsignal_unwatched) {
	ak_wsignal_init(&ws.s, ws_gone);
	ws.read = true;
	ak_owner *r = ak_root(ws_build, NULL);
	CHECK_EQ_INT(ws.unwatched, 0);
	ak_signal_write(&ws.s.node);
	CHECK_EQ_INT(ws.runs, 4);
	ws.read = false;  // they stop reading it: the last to go tells
	ak_signal_write(&ws.s.node);
	CHECK_EQ_INT(ws.unwatched, 1);
	ws.read = true;
	ak_owner_dispose(r);
	CHECK_EQ_INT(ws.unwatched, 1);
	r = ak_root(ws_build, NULL);
	ak_owner_dispose(r);  // disposing its readers tells too
	CHECK_EQ_INT(ws.unwatched, 2);
	r = ak_root(ws_build, NULL);
	ak_signal_drop(&ws.s.node);  // a drop doesn't
	CHECK_EQ_INT(ws.unwatched, 2);
	ak_owner_dispose(r);
}

T_MAIN()
