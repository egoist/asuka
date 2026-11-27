#include "t.h"
#include "vm/keep.h"
#include "vm/vm_int.h"

// Old and new versions of a Todo: the new one gained a field (an int,
// which starts at 0) and lost one, and moved its fields around.
static const uint8_t old_kinds[] = {AK_K_REF, AK_K_BOOL, AK_K_INT};
static const char *const old_names[] = {"title", "done", "legacy"};
static const ak_shape old_todo = {.name = "Todo", .nfields = 3, .kinds = old_kinds, .names = old_names};

static const uint8_t new_kinds[] = {AK_K_BOOL, AK_K_INT, AK_K_REF};
static const char *const new_names[] = {"done", "priority", "title"};
static const ak_shape new_todo = {.name = "Todo", .nfields = 3, .kinds = new_kinds, .names = new_names};

// A new field of a reference type has no zero: conversion fails.
static const uint8_t bad_kinds[] = {AK_K_REF, AK_K_REF};
static const char *const bad_names[] = {"title", "note"};
static const ak_shape bad_todo = {.name = "Todo", .nfields = 2, .kinds = bad_kinds, .names = bad_names};

static const uint8_t ready_kinds[] = {AK_K_REF};
static const ak_case cases[] = {{.name = "pending"}, {.name = "ready", .nfields = 1, .kinds = ready_kinds}};
static ak_shape old_load = {.name = "Load", .ncases = 2, .cases = cases};
static const ak_case new_cases[] = {{.name = "ready", .nfields = 1, .kinds = ready_kinds}, {.name = "failed", .nfields = 1, .kinds = ready_kinds}, {.name = "pending"}};
static ak_shape new_load = {.name = "Load", .ncases = 3, .cases = new_cases};

static const ak_shape *which;
static const ak_shape *find(void *ctx, const char *name) {
	AK_UNUSED(ctx);
	if (strcmp(name, "Todo") == 0) return which;
	if (strcmp(name, "Load") == 0) return &new_load;
	return NULL;
}

static ak_val todo(const char *title, bool done) {
	ak_struct *t = ak_struct_new(&old_todo);
	t->f[0].p = ak_str_c(title);
	t->f[1].i = done;
	t->f[2].i = 99;
	return (ak_val){.p = t};
}

TEST(struct_by_field_names) {
	ak_arr *a = ak_arr_new(AK_K_REF, &old_todo, 0);
	ak_arr_push(&a, todo("milk", true));
	ak_arr_push(&a, todo("eggs", false));
	ak_buf b = {0};
	REQUIRE(ak_keep_encode(&b, (ak_val){.p = a}, AK_K_REF));
	which = &new_todo;
	ak_reader r = {b.data, b.len, 0, false};
	ak_val out;
	REQUIRE(ak_keep_decode(&r, AK_K_REF, find, NULL, &out));
	ak_arr *n = out.p;
	REQUIRE(n->len == 2);
	ak_struct *t0 = ak_arr_vals(n)[0].p;
	CHECK(t0->shape == &new_todo);
	CHECK_EQ_INT(t0->f[0].i, 1);
	CHECK_EQ_INT(t0->f[1].i, 0);
	CHECK_EQ_STR(((ak_str *)t0->f[2].p)->data, "milk");
	CHECK(n->shape == &new_todo);
	ak_release(n);

	which = &bad_todo;
	r = (ak_reader){b.data, b.len, 0, false};
	CHECK(!ak_keep_decode(&r, AK_K_REF, find, NULL, &out));
	ak_release(a);
	ak_buf_free(&b);
}

TEST(enums_by_case_names) {
	ak_enum *e = ak_enum_new(&old_load, 1);
	e->f[0].p = ak_str_c("ok");
	ak_buf b = {0};
	REQUIRE(ak_keep_encode(&b, (ak_val){.p = e}, AK_K_REF));
	ak_reader r = {b.data, b.len, 0, false};
	ak_val out;
	REQUIRE(ak_keep_decode(&r, AK_K_REF, find, NULL, &out));
	ak_enum *n = out.p;
	CHECK_EQ_INT(ak_enum_case(n), 0);  // "ready" moved to 0
	CHECK_EQ_STR(((ak_str *)n->f[0].p)->data, "ok");
	ak_release(n);
	ak_release(e);
	ak_buf_free(&b);
	ak_free(old_load.singletons);
	ak_free(new_load.singletons);
}

TEST(kinds_must_match) {
	ak_buf b = {0};
	ak_keep_encode(&b, (ak_val){.i = 5}, AK_K_INT);
	ak_reader r = {b.data, b.len, 0, false};
	ak_val out;
	CHECK(!ak_keep_decode(&r, AK_K_FLOAT, find, NULL, &out));
	r = (ak_reader){b.data, b.len, 0, false};
	CHECK(ak_keep_decode(&r, AK_K_INT, find, NULL, &out));
	CHECK_EQ_INT(out.i, 5);
	// A string where an int is wanted now: skipped, not kept.
	ak_buf_reset(&b);
	ak_str *s = ak_str_c("x");
	ak_keep_encode(&b, (ak_val){.p = s}, AK_K_REF);
	r = (ak_reader){b.data, b.len, 0, false};
	CHECK(!ak_keep_decode(&r, AK_K_INT, find, NULL, &out));
	ak_release(s);
	ak_buf_free(&b);
}

TEST(maps_and_boxes) {
	ak_map *m = ak_map_new(AK_K_REF, true, AK_K_REF, NULL);
	ak_map_set(&m, (ak_val){.p = ak_str_c("a")}, (ak_val){.p = ak_box_new(AK_K_INT, (ak_val){.i = 1})});
	ak_buf b = {0};
	REQUIRE(ak_keep_encode(&b, (ak_val){.p = m}, AK_K_REF));
	ak_reader r = {b.data, b.len, 0, false};
	ak_val out;
	REQUIRE(ak_keep_decode(&r, AK_K_REF, find, NULL, &out));
	CHECK(ak_obj_eq(out.p, &m->h));
	ak_release(out.p);
	ak_release(m);
	ak_buf_free(&b);
}

// A cell made with a key: kept by a reload (save), started again by a
// restart (discard).
static int64_t cell_int(ak_val c) { return ak_vm_cell_get(c.p, false).i; }

TEST(reload_keeps_restart_discards) {
	ak_keep_start();
	ak_keep_use(find, NULL);
	ak_str *key = ak_str_c("Counter.count");
	ak_val c = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, key);
	ak_vm_cell_set(c.p, (ak_val){.i = 5});

	// A reload: the new cell starts from the old one's value.
	ak_keep_save();
	CHECK_EQ_INT(ak_keep_saved(), 1);
	ak_release(c.p);
	ak_keep_use(find, NULL);
	c = ak_vm_cell_new((ak_val){.i = 10}, AK_K_INT, key);
	CHECK_EQ_INT(cell_int(c), 5);
	CHECK_EQ_INT(ak_keep_restored(), 1);

	// A restart: it starts from its initial value.
	ak_keep_discard();
	CHECK_EQ_INT(ak_keep_saved(), 0);
	ak_release(c.p);
	ak_keep_use(find, NULL);
	c = ak_vm_cell_new((ak_val){.i = 10}, AK_K_INT, key);
	CHECK_EQ_INT(cell_int(c), 10);
	CHECK_EQ_INT(ak_keep_restored(), 0);

	ak_release(c.p);
	ak_release(key);
	ak_keep_stop();
}

// A value no cell took at a reload (its state's initializer was edited:
// another key) isn't there for the next: an initializer edited, then
// edited back, starts from it.
TEST(reload_drops_what_no_cell_took) {
	ak_keep_start();
	ak_keep_use(find, NULL);
	ak_str *k0 = ak_str_c("Counter.count#0"), *k10 = ak_str_c("Counter.count#10");
	ak_val c = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, k0);
	ak_vm_cell_set(c.p, (ak_val){.i = 3});

	// = 10: a new key, the new initial value.
	ak_keep_save();
	ak_release(c.p);
	ak_keep_use(find, NULL);
	c = ak_vm_cell_new((ak_val){.i = 10}, AK_K_INT, k10);
	CHECK_EQ_INT(cell_int(c), 10);

	// Back to = 0: from 0, not the 3 saved two reloads ago.
	ak_keep_save();
	ak_release(c.p);
	ak_keep_use(find, NULL);
	c = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, k0);
	CHECK_EQ_INT(cell_int(c), 0);
	CHECK_EQ_INT(ak_keep_restored(), 0);

	ak_release(c.p);
	ak_release(k0);
	ak_release(k10);
	ak_keep_stop();
}

// While asuka dev shows a preview (ak_keep_hold), the app's values wait
// for it, through the preview's reloads: the app comes back as it was.
TEST(reload_holds_the_app_while_a_preview_shows) {
	ak_keep_start();
	ak_keep_use(find, NULL);
	ak_str *app = ak_str_c("Counter.count#0"), *pv = ak_str_c("preview \"Zero\".n#0");
	ak_val c = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, app);
	ak_vm_cell_set(c.p, (ak_val){.i = 3});

	// The preview, then a reload of it: the app's 3 isn't taken, and held.
	ak_keep_hold(false);
	ak_keep_save();
	ak_release(c.p);
	for (int i = 0; i < 2; i++) {
		ak_keep_use(find, NULL);
		c = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, pv);
		ak_keep_hold(true);
		ak_keep_save();
		ak_release(c.p);
	}
	// The app again.
	ak_keep_use(find, NULL);
	c = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, app);
	CHECK_EQ_INT(cell_int(c), 3);
	ak_keep_hold(false);

	ak_release(c.p);
	ak_release(app);
	ak_release(pv);
	ak_keep_stop();
}

// A follow (rfcs/0012) made with a key: a reload keeps its value as a
// write, which stands until what it follows changes.
static struct {
	ak_node n;
	int64_t v;
	ak_str *key;
	ak_val f;
} kf;

static ak_val kf_update(ak_closure *c, ak_val *args, uint32_t nargs) {
	AK_UNUSED(c);
	AK_UNUSED(args);
	AK_UNUSED(nargs);
	ak_signal_read(&kf.n);
	return (ak_val){.i = kf.v * 10};
}

static void kf_make(void *ctx, ak_owner *root) {
	AK_UNUSED(ctx);
	AK_UNUSED(root);
	ak_closure *u = ak_closure_new(kf_update, NULL, 0, NULL);
	kf.f = ak_vm_follow_new(u, AK_K_INT, kf.key, false);
	ak_release(u);
}

TEST(reload_keeps_a_follow_as_a_write) {
	memset(&kf, 0, sizeof kf);
	kf.v = 1;
	kf.key = ak_str_c("Edit.draft");
	ak_keep_start();
	ak_keep_use(find, NULL);
	ak_owner *r = ak_root(kf_make, NULL);
	CHECK_EQ_INT(cell_int(kf.f), 10);
	ak_vm_cell_set(kf.f.p, (ak_val){.i = 5});
	CHECK_EQ_INT(cell_int(kf.f), 5);

	// A reload: the new follow starts from what the old one held.
	ak_keep_save();
	CHECK_EQ_INT(ak_keep_saved(), 1);
	ak_owner_dispose(r);
	ak_release(kf.f.p);
	ak_keep_use(find, NULL);
	r = ak_root(kf_make, NULL);
	CHECK_EQ_INT(cell_int(kf.f), 5);
	CHECK_EQ_INT(ak_keep_restored(), 1);
	// What it follows changes: the initializer's again.
	kf.v = 2;
	ak_signal_write(&kf.n);
	CHECK_EQ_INT(cell_int(kf.f), 20);

	ak_owner_dispose(r);
	ak_release(kf.f.p);
	ak_keep_discard();
	ak_release(kf.key);
	ak_keep_stop();
}

// A follow's initializer reads its component's env, which the component's
// function puts in the box its members' thunks capture only once it has
// made every member (lang/LOWERING.md): a value kept across a reload
// doesn't run it as the follow is made, but at its first read, after.
// (A store's `state intro = readIntro(shelf)` read the box empty there:
// "vm: calling none".)
static struct {
	int64_t *env;  // the box: NULL until the maker has made every member
	int early;     // runs that found it empty
} ke;

static ak_val ke_update(ak_closure *c, ak_val *args, uint32_t nargs) {
	AK_UNUSED(c);
	AK_UNUSED(args);
	AK_UNUSED(nargs);
	ak_signal_read(&kf.n);
	if (!ke.env) {
		ke.early++;
		return (ak_val){.i = -1};
	}
	return (ak_val){.i = *ke.env * 10};
}

static void ke_make(void *ctx, ak_owner *root) {
	AK_UNUSED(root);
	ke.env = NULL;
	ak_closure *u = ak_closure_new(ke_update, NULL, 0, NULL);
	kf.f = ak_vm_follow_new(u, AK_K_INT, kf.key, false);
	ak_release(u);
	ke.env = ctx;  // every member made: the box filled
}

TEST(reload_runs_a_kept_follow_after_its_maker) {
	memset(&kf, 0, sizeof kf);
	memset(&ke, 0, sizeof ke);
	int64_t env = 1;
	kf.key = ak_str_c("Vista.intro");
	ak_keep_start();
	ak_keep_use(find, NULL);
	ak_owner *r = ak_root(ke_make, &env);
	CHECK_EQ_INT(cell_int(kf.f), 10);
	ak_vm_cell_set(kf.f.p, (ak_val){.i = 5});

	ak_keep_save();
	CHECK_EQ_INT(ak_keep_saved(), 1);
	ak_owner_dispose(r);
	ak_release(kf.f.p);
	ak_keep_use(find, NULL);
	r = ak_root(ke_make, &env);
	CHECK_EQ_INT(ke.early, 0);
	CHECK_EQ_INT(cell_int(kf.f), 5);
	CHECK_EQ_INT(ak_keep_restored(), 1);
	CHECK_EQ_INT(ke.early, 0);
	// What it follows changes: the initializer's again.
	env = 2;
	ak_signal_write(&kf.n);
	CHECK_EQ_INT(cell_int(kf.f), 20);

	// Kept again, and written before its first read: the write stands.
	ak_keep_save();
	ak_owner_dispose(r);
	ak_release(kf.f.p);
	ak_keep_use(find, NULL);
	r = ak_root(ke_make, &env);
	ak_vm_cell_set(kf.f.p, (ak_val){.i = 7});
	CHECK_EQ_INT(cell_int(kf.f), 7);
	CHECK_EQ_INT(ke.early, 0);

	ak_owner_dispose(r);
	ak_release(kf.f.p);
	ak_keep_discard();
	ak_release(kf.key);
	ak_keep_stop();
}

// A follow's reads, its initializer's runs (given its current value, a
// scalar boxed as an optional) and its writes allocate nothing once
// it's made.
static ak_val kf_self_update(ak_closure *c, ak_val *args, uint32_t nargs) {
	AK_UNUSED(c);
	AK_UNUSED(nargs);
	ak_signal_read(&kf.n);
	ak_box *prev = args[0].p;
	return (ak_val){.i = (prev ? prev->v.i : 0) + kf.v};
}

static void kf_make_self(void *ctx, ak_owner *root) {
	AK_UNUSED(ctx);
	AK_UNUSED(root);
	ak_closure *u = ak_closure_new(kf_self_update, NULL, 0, NULL);
	kf.f = ak_vm_follow_new(u, AK_K_INT, NULL, true);
	ak_release(u);
}

TEST(follow_allocates_nothing_in_the_steady_state) {
	memset(&kf, 0, sizeof kf);
	kf.v = 1;
	ak_owner *r = ak_root(kf_make_self, NULL);
	CHECK_EQ_INT(cell_int(kf.f), 1);  // none the first time
	kf.v = 2;
	ak_signal_write(&kf.n);
	CHECK_EQ_INT(cell_int(kf.f), 3);  // its value, then 2 more
	uint64_t before = ak_alloc_count;
	for (int i = 0; i < 1000; i++) {
		ak_signal_write(&kf.n);
		(void)cell_int(kf.f);
		ak_vm_cell_set(kf.f.p, (ak_val){.i = i});
	}
	CHECK_EQ_INT(ak_alloc_count - before, 0);
	ak_owner_dispose(r);
	ak_release(kf.f.p);
}

T_MAIN()
