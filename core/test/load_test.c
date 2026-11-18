// Loaders (control.c's): let x = load f(…). A load is pending only
// before its first answer; a run after that keeps its case until it
// answers (RFC 0014), busy says one is in flight, and an answer equal to
// the last changes nothing. The tests count what each signal notifies:
// a binding that reads only the state, one that reads only loading.
#include "uih.h"

#include "rt/sched.h"

static void *id;            // the cell the thunk reads
static ak_val loader;       // the app's loader (the test's reference)
static ak_future *runs[16]; // each run's future (the test's reference)
static int nruns;
static int state_reads, busy_reads, value_reads, builds;

// The thunk: a run is a future the test settles.
static ak_val thunk(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	(void)ak_vm_cell_get(id, true);
	ak_future *f = ak_future_new(AK_K_INT);
	runs[nruns++] = ak_retain(f);
	return (ak_val){.p = f};
}

static ak_val state_of(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	state_reads++;
	return (ak_val){.i = ak_ui_load_state(loader)};
}

static ak_val busy_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	busy_reads++;
	return (ak_val){.p = ak_str_c(ak_ui_load_loading(loader) ? "busy" : "idle")};
}

static ak_val value_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	value_reads++;
	ak_val v = ak_ui_load_value(loader);
	char s[32];
	snprintf(s, sizeof s, "value %lld", v.p ? (long long)((ak_box *)v.p)->v.i : -1LL);
	if (v.p) ak_release(v.p);
	return (ak_val){.p = ak_str_c(s)};
}

// A case's content, built once per case: builds counts them, so a
// reload that kept the content builds nothing.
static ak_val case_view(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	builds++;
	static const char *const names[] = {"pending", "ready", "failed"};
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	set_str(t, AK_KEY_TEXT_TEXT, names[a[0].i]);
	return (ak_val){.p = t};
}

static ak_val load_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_closure *t = fn(thunk, NULL);
	loader = ak_ui_load(t);
	ak_release(t);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_closure *sel = fn(state_of, NULL), *build = fn(case_view, NULL);
	add(col, ak_ui_switch(sel, build));
	ak_release(sel), ak_release(build);
	ak_view *busy = ak_ui_new(AK_EL_TEXT);
	bind(busy, AK_KEY_TEXT_TEXT, busy_text, NULL);
	add(col, busy);
	ak_view *val = ak_ui_new(AK_EL_TEXT);
	bind(val, AK_KEY_TEXT_TEXT, value_text, NULL);
	add(col, val);
	return (ak_val){.p = col};
}

static ak_uitest *start(void) {
	nruns = state_reads = busy_reads = value_reads = builds = 0;
	id = cell_int(1);
	ak_uitest *a = mount(load_app, NULL);
	ak_uitest_settle(a);
	return a;
}

static void settle(ak_uitest *a) {
	ak_sched_run();
	ak_uitest_settle(a);
}

static void answer(ak_uitest *a, int i, int64_t v) {
	ak_future_resolve(runs[i], (ak_val){.i = v});
	settle(a);
}

static void *error_of(const char *msg) {
	ak_struct *e = ak_struct_new(&ak_shape_error);
	e->f[0].p = ak_str_c(msg);
	return e;
}

static void fail(ak_uitest *a, int i, const char *msg) {
	ak_future_reject(runs[i], error_of(msg));
	settle(a);
}

static void finish(ak_uitest *a) {
	done(a);
	ak_release(loader.p);
	for (int i = 0; i < nruns; i++) ak_release(runs[i]);
	ak_release(id);
}

TEST(pending_only_before_the_first_answer) {
	ak_uitest *a = start();
	REQUIRE(nruns == 1);
	check_text(root(a), "pending|busy|value -1");
	answer(a, 0, 10);
	check_text(root(a), "ready|idle|value 10");
	CHECK_EQ_INT(builds, 2);
	finish(a);
}

// A run after the first keeps the content (built once) and writes busy
// alone; its answer writes the value, not the state: it's still ready.
TEST(a_run_keeps_its_case) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	int states = state_reads, busies = busy_reads;
	reset(a);
	write_i(id, 2);
	settle(a);
	REQUIRE(nruns == 2);
	check_text(root(a), "ready|busy|value 10");
	CHECK_EQ_INT(state_reads, states);
	CHECK_EQ_INT(busy_reads, busies + 1);
	CHECK_EQ_INT(builds, 2);
	CHECK_EQ_INT(ak_fake_count(ak_uitest_host(a), AK_OP_DESTROY), 0);
	int values = value_reads;
	answer(a, 1, 20);
	check_text(root(a), "ready|idle|value 20");
	CHECK_EQ_INT(state_reads, states);
	CHECK_EQ_INT(value_reads, values + 1);
	CHECK_EQ_INT(busy_reads, busies + 2);
	CHECK_EQ_INT(builds, 2);
	CHECK_EQ_INT(ak_fake_count(ak_uitest_host(a), AK_OP_DESTROY), 0);
	finish(a);
}

// An answer equal to the last notifies nothing but busy.
TEST(an_equal_answer_notifies_nothing) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	int states = state_reads, busies = busy_reads;
	ak_ui_load_reload(loader);
	settle(a);
	REQUIRE(nruns == 2);
	check_text(root(a), "ready|busy|value 10");
	answer(a, 1, 10);
	check_text(root(a), "ready|idle|value 10");
	CHECK_EQ_INT(state_reads, states);
	CHECK_EQ_INT(busy_reads, busies + 2);
	finish(a);
}

// A run a newer one replaced changes nothing when it answers, and the
// newer one's answer is the one shown; cancelling it changes nothing.
TEST(a_replaced_run_changes_nothing) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	write_i(id, 2);
	settle(a);
	write_i(id, 3);
	settle(a);
	REQUIRE(nruns == 3);
	int states = state_reads, busies = busy_reads;
	answer(a, 1, 20);
	ak_future_cancel(runs[1]);
	settle(a);
	check_text(root(a), "ready|busy|value 10");
	CHECK_EQ_INT(state_reads, states);
	CHECK_EQ_INT(busy_reads, busies);
	answer(a, 2, 30);
	check_text(root(a), "ready|idle|value 30");
	CHECK_EQ_INT(builds, 2);
	finish(a);
}

// A run that fails after a value is failed, the value kept; failing the
// same way again notifies nothing; a later answer is ready again.
TEST(a_failure_keeps_the_value) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	write_i(id, 2);
	settle(a);
	fail(a, 1, "offline");
	check_text(root(a), "failed|idle|value 10");
	CHECK_EQ_INT(builds, 3);
	ak_val e = ak_ui_load_error(loader);
	REQUIRE(e.p);
	CHECK_EQ_STR(ak_error_message(e.p)->data, "offline");
	ak_release(e.p);
	int states = state_reads;
	ak_ui_load_reload(loader);
	settle(a);
	check_text(root(a), "failed|busy|value 10");
	fail(a, 2, "offline");
	check_text(root(a), "failed|idle|value 10");
	CHECK_EQ_INT(state_reads, states);
	ak_ui_load_reload(loader);
	settle(a);
	fail(a, 3, "timed out");
	CHECK_EQ_INT(state_reads, states + 1);
	ak_ui_load_reload(loader);
	settle(a);
	answer(a, 4, 10);
	check_text(root(a), "ready|idle|value 10");
	CHECK_EQ_INT(state_reads, states + 2);
	e = ak_ui_load_error(loader);
	CHECK(e.p == NULL);
	finish(a);
}

// Disposing the component while a run is in flight: no run is in flight
// any more, its answer changes nothing, and nothing outlives the app.
TEST(disposal_ends_the_run) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	write_i(id, 2);
	settle(a);
	CHECK(ak_ui_load_loading(loader));
	ak_buf err = {0};
	if (!ak_uitest_unmount(a, &err)) T_FAIL("%s", cstr(&err));
	ak_buf_free(&err);
	ak_release(a);
	CHECK(!ak_ui_load_loading(loader));
	ak_future_resolve(runs[1], (ak_val){.i = 20});
	ak_sched_run();
	CHECK_EQ_INT(ak_ui_load_state(loader), 1);
	ak_val v = ak_ui_load_value(loader);
	CHECK_EQ_INT(((ak_box *)v.p)->v.i, 10);
	ak_release(v.p);
	write_i(id, 3);
	ak_ui_flush();
	CHECK_EQ_INT(nruns, 2);  // it never runs again
	ak_release(loader.p);
	for (int i = 0; i < nruns; i++) ak_release(runs[i]);
	ak_release(id);
	ak_ui_stats s;
	ak_ui_get_stats(&s);
	CHECK_EQ_INT(s.views, 0);
}

// A run that keeps its case allocates no more than its ticket.
TEST(a_run_allocates_its_ticket) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	ak_ui_load_reload(loader);
	settle(a);
	answer(a, 1, 10);
	uint64_t before = ak_alloc_count;
	ak_ui_load_reload(loader);
	settle(a);
	answer(a, 2, 10);
	// The thunk's future, its reference in runs, and the run's ticket;
	// the waiter the future keeps for it.
	uint64_t per_run = ak_alloc_count - before;
	ak_ui_load_reload(loader);
	settle(a);
	answer(a, 3, 10);
	CHECK_EQ_INT(ak_alloc_count - before, 2 * per_run);
	finish(a);
}

// ---- Optimistic changes (RFC 0017) ----

static int times_runs;

static ak_val plus1(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	return (ak_val){.i = a[0].i + 1};
}

static ak_val times10(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	times_runs++;
	return (ak_val){.i = a[0].i * 10};
}

static ak_val throws(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_throw_msg("no");
	return (ak_val){0};
}

// A task: its future and its owner, ended as the VM's tasks end (the
// future settled, then the owner disposed).
typedef struct {
	ak_future *f;
	ak_owner *o;
} task;

static task task_new(void) { return (task){ak_future_new(AK_K_VOID), ak_root_new()}; }

static void task_end(task *t, const char *fail) {
	if (fail) ak_future_reject(t->f, error_of(fail));
	else ak_future_resolve(t->f, (ak_val){0});
	ak_owner_dispose(t->o);
	ak_release(t->f);
	ak_sched_run();
}

static void change(ak_uitest *a, ak_invoke_fn f, task *t) {
	ak_closure *c = fn(f, NULL);
	ak_ui_load_optimistic_in(loader, c, t ? t->f : NULL, t ? t->o : NULL);
	ak_release(c);
	ak_uitest_settle(a);
}

static struct {
	int n;
	char code[64];
	char msg[1024];
} said;

static void record(void *ctx, const char *code, const char *msg, const char *const *where, int nwhere) {
	AK_UNUSED(ctx), AK_UNUSED(where), AK_UNUSED(nwhere);
	said.n++;
	snprintf(said.code, sizeof said.code, "%s", code);
	snprintf(said.msg, sizeof said.msg, "%s", msg);
}

static bool diag_was;

static void listen(void) {
	diag_was = ak_diag_enabled;
	ak_diag_enabled = true;
	ak_diag_reset();
	memset(&said, 0, sizeof said);
	ak_diag_set_handler(record, NULL);
}

static void unlisten(void) {
	ak_diag_set_handler(NULL, NULL);
	ak_diag_enabled = diag_was;
}

// A change shows at once, in one write; the answer it waited for replaces
// it in the flush that shows it: equal to it, nothing is written (no
// flash of the old value); unequal, one write, the answer's.
TEST(a_layer_goes_with_the_answer) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	int values = value_reads, states = state_reads;
	task t = task_new();
	change(a, plus1, &t);
	check_text(root(a), "ready|idle|value 11");
	CHECK_EQ_INT(value_reads, values + 1);
	CHECK_EQ_INT(state_reads, states);
	CHECK_EQ_INT(ak_ui_load_layers(loader), 1);
	ak_ui_load_reload(loader);
	settle(a);
	check_text(root(a), "ready|busy|value 11");
	answer(a, 1, 11);
	check_text(root(a), "ready|idle|value 11");
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	CHECK_EQ_INT(value_reads, values + 1);
	CHECK_EQ_INT(state_reads, states);
	task_end(&t, NULL);
	CHECK_EQ_INT(value_reads, values + 1);
	// Again, and the answer differs: one write, of the answer.
	t = task_new();
	change(a, plus1, &t);
	check_text(root(a), "ready|idle|value 12");
	ak_ui_load_reload(loader);
	settle(a);
	answer(a, 2, 20);
	check_text(root(a), "ready|idle|value 20");
	CHECK_EQ_INT(value_reads, values + 3);
	task_end(&t, NULL);
	CHECK_EQ_INT(value_reads, values + 3);
	finish(a);
}

// A run in flight when the change is made confirms it: its answer takes
// it out, though it was asked before.
TEST(a_run_in_flight_confirms_a_layer) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	ak_ui_load_reload(loader);
	settle(a);
	task t = task_new();
	change(a, plus1, &t);
	check_text(root(a), "ready|busy|value 11");
	task_end(&t, NULL);  // a run is in flight: it stays
	CHECK_EQ_INT(ak_ui_load_layers(loader), 1);
	answer(a, 1, 30);
	check_text(root(a), "ready|idle|value 30");
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	finish(a);
}

// A throw out of the task that made it undoes it.
TEST(a_throw_undoes_a_layer) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	int values = value_reads;
	task t = task_new();
	change(a, plus1, &t);
	ak_ui_load_reload(loader);
	settle(a);
	check_text(root(a), "ready|busy|value 11");
	task_end(&t, "offline");
	check_text(root(a), "ready|busy|value 10");
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	CHECK_EQ_INT(value_reads, values + 2);
	answer(a, 1, 10);
	check_text(root(a), "ready|idle|value 10");
	CHECK_EQ_INT(value_reads, values + 2);
	finish(a);
}

// Two layers: the first one's failure takes it out from under the
// second, which is applied again to what's below (the answer).
TEST(layers_stack_and_go_on_their_own) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	times_runs = 0;
	task t1 = task_new(), t2 = task_new();
	change(a, plus1, &t1);
	change(a, times10, &t2);
	check_text(root(a), "ready|idle|value 110");
	CHECK_EQ_INT(times_runs, 1);
	ak_ui_load_reload(loader);
	settle(a);
	task_end(&t1, "offline");
	check_text(root(a), "ready|busy|value 100");
	CHECK_EQ_INT(times_runs, 2);
	CHECK_EQ_INT(ak_ui_load_layers(loader), 1);
	task_end(&t2, NULL);  // the run in flight confirms it
	CHECK_EQ_INT(ak_ui_load_layers(loader), 1);
	answer(a, 1, 100);
	check_text(root(a), "ready|idle|value 100");
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	CHECK_EQ_INT(times_runs, 2);
	finish(a);
}

// An answer that lands while a later layer waits re-applies it to the
// answer: f runs again when what's under it changes.
TEST(an_answer_under_a_layer_runs_it_again) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	ak_ui_load_reload(loader);
	settle(a);
	task t1 = task_new();
	change(a, plus1, &t1);  // confirmed by run 1, in flight
	answer(a, 1, 10);
	task t2 = task_new();
	times_runs = 0;
	change(a, times10, &t2);  // waits for the next run
	check_text(root(a), "ready|idle|value 100");
	ak_ui_load_reload(loader);
	settle(a);
	task_end(&t1, NULL);
	task_end(&t2, NULL);
	write_i(id, 2);  // a newer run replaces the one in flight
	settle(a);
	answer(a, 3, 7);
	check_text(root(a), "ready|idle|value 7");
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	CHECK_EQ_INT(times_runs, 1);
	finish(a);
}

// A task that ends with no run to confirm its change takes it back, and
// says so once per site (optimistic-unconfirmed); one that asked for a
// run before it ended keeps it until the run answers.
TEST(a_layer_without_a_run_goes_with_its_task) {
	listen();
	ak_uitest *a = start();
	answer(a, 0, 10);
	ak_closure *c = fn(plus1, NULL);
	for (int i = 0; i < 2; i++) {
		task t = task_new();
		ak_ui_load_optimistic_in(loader, c, t.f, t.o);
		settle(a);
		check_text(root(a), "ready|idle|value 11");
		task_end(&t, NULL);
		ak_uitest_settle(a);
		check_text(root(a), "ready|idle|value 10");
		CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	}
	CHECK_EQ_INT(said.n, 1);
	CHECK_EQ_STR(said.code, "optimistic-unconfirmed");
	task t = task_new();
	ak_ui_load_optimistic_in(loader, c, t.f, t.o);
	ak_ui_load_reload(loader);
	task_end(&t, NULL);
	settle(a);
	check_text(root(a), "ready|busy|value 11");
	answer(a, 1, 11);
	check_text(root(a), "ready|idle|value 11");
	CHECK_EQ_INT(said.n, 1);
	ak_release(c);
	finish(a);
	unlisten();
}

// Synchronous code (no task): its end is the ready queue's next run,
// before the flush that would show it: with no run asked for, it never
// shows; with one, it shows until the run answers.
TEST(a_layer_of_synchronous_code) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	int values = value_reads;
	change(a, plus1, NULL);
	check_text(root(a), "ready|idle|value 10");
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	ak_closure *c = fn(plus1, NULL);
	ak_ui_load_optimistic_in(loader, c, NULL, NULL);
	ak_release(c);
	ak_ui_load_reload(loader);
	settle(a);
	check_text(root(a), "ready|busy|value 11");
	answer(a, 1, 11);
	check_text(root(a), "ready|idle|value 11");
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	// The first change and its undoing ran the binding (their ops one
	// flush's: nothing showed), the second once, its answer not.
	CHECK_EQ_INT(value_reads, values + 3);
	finish(a);
}

// On a load that isn't ready, nothing (optimistic-not-ready); f's throw
// is the caller's, and makes no layer.
TEST(a_change_needs_a_ready_load) {
	listen();
	ak_uitest *a = start();
	task t = task_new();
	change(a, plus1, &t);
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	CHECK_EQ_INT(said.n, 1);
	CHECK_EQ_STR(said.code, "optimistic-not-ready");
	answer(a, 0, 10);
	ak_closure *c = fn(throws, NULL);
	ak_ui_load_optimistic_in(loader, c, t.f, t.o);
	ak_release(c);
	REQUIRE(ak_error_pending());
	ak_release(ak_error_take());
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	check_text(root(a), "ready|idle|value 10");
	task_end(&t, NULL);
	finish(a);
	unlisten();
}

// The load going takes its layers with it; their tasks ending after
// changes nothing.
TEST(layers_go_with_their_load) {
	ak_uitest *a = start();
	answer(a, 0, 10);
	task t = task_new();
	change(a, plus1, &t);
	ak_buf err = {0};
	if (!ak_uitest_unmount(a, &err)) T_FAIL("%s", cstr(&err));
	ak_buf_free(&err);
	ak_release(a);
	CHECK_EQ_INT(ak_ui_load_layers(loader), 0);
	task_end(&t, "offline");
	ak_release(loader.p);
	for (int i = 0; i < nruns; i++) ak_release(runs[i]);
	ak_release(id);
}

// With no layers, a run that changes the value allocates what it did
// before RFC 0017 (the run's future and ticket, the binding's text): the
// stack is one test, the shown cell's write none.
TEST(an_answer_without_layers_allocates_nothing_more) {
	ak_uitest *a = start();
	int64_t v = 10;
	answer(a, 0, v++);
	for (int i = 1; i <= 3; i++) {  // warm: the vectors the runs use grow
		ak_ui_load_reload(loader);
		settle(a);
		answer(a, i, v++);
	}
	uint64_t before = ak_alloc_count;
	ak_ui_load_reload(loader);
	settle(a);
	answer(a, 4, v++);
	uint64_t per_run = ak_alloc_count - before;
	for (int i = 5; i <= 6; i++) {
		ak_ui_load_reload(loader);
		settle(a);
		answer(a, i, v++);
	}
	CHECK_EQ_INT(ak_alloc_count - before, 3 * per_run);
	finish(a);
}

// ---- reload f(…) (RFC 0017) ----

// A keyed load: its thunk says its key and its argument (id's value), as
// a load of a named function's call does (load.key).
typedef struct {
	int64_t key;
	void *id;              // an int cell
	ak_val loader;
	ak_future *runs[8];
	int nruns;
} keyed;

static ak_val keyed_thunk(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	keyed *k = (keyed *)c->fn;
	ak_val arg = ak_vm_cell_get(k->id, true);
	ak_str *kinds = ak_str_c("1");
	ak_ui_load_key(k->key, kinds, &arg, 1);
	ak_release(kinds);
	ak_future *f = ak_future_new(AK_K_INT);
	k->runs[k->nruns++] = ak_retain(f);
	return (ak_val){.p = f};
}

static void keyed_make(void *ctx, ak_owner *root) {
	AK_UNUSED(root);
	keyed **ks = ctx;
	for (keyed **k = ks; *k; k++) {
		ak_closure *t = fn(keyed_thunk, *k);
		(*k)->loader = ak_ui_load(t);
		ak_release(t);
	}
}

static void keyed_init(keyed *k, int64_t key, int64_t id) {
	memset(k, 0, sizeof *k);
	k->key = key;
	k->id = cell_int(id);
}

static void keyed_free(keyed *k) {
	ak_release(k->loader.p);
	for (int i = 0; i < k->nruns; i++) ak_release(k->runs[i]);
	ak_release(k->id);
}

static void run_all(void) {
	ak_ui_flush();
	ak_sched_run();
	ak_ui_flush();
}

static ak_val reload_of(int64_t key, const int64_t *arg) {
	ak_str *kinds = ak_str_c(arg ? "1" : "");
	ak_val a = {.i = arg ? *arg : 0};
	ak_val f = ak_ui_load_reload_all(key, arg != NULL, kinds, &a, arg ? 1 : 0);
	ak_release(kinds);
	return f;
}

static int fut_state(ak_val f) { return ((ak_future *)f.p)->state; }

// reload reaches the loads of its key in every root, those with equal
// arguments when it gives them; its future answers once each has (a
// load that goes meanwhile counts), and a reload that reaches none
// answers at once.
TEST(reload_reaches_every_root) {
	setup();
	keyed a1, a2, b1, b2;
	keyed_init(&a1, 7, 1), keyed_init(&a2, 7, 2), keyed_init(&b1, 7, 1), keyed_init(&b2, 8, 1);
	keyed *in_a[] = {&a1, &a2, NULL}, *in_b[] = {&b1, &b2, NULL};
	uint32_t before = ak_ui_load_registered();
	ak_owner *ra = ak_root(keyed_make, in_a), *rb = ak_root(keyed_make, in_b);
	run_all();
	REQUIRE(a1.nruns == 1 && a2.nruns == 1 && b1.nruns == 1 && b2.nruns == 1);
	CHECK_EQ_INT(ak_ui_load_registered(), before + 4);
	for (keyed **k = (keyed *[]){&a1, &a2, &b1, &b2, NULL}; *k; k++) ak_future_resolve((*k)->runs[0], (ak_val){.i = 1});
	run_all();

	// Equal arguments: a1 and b1, not a2 (2), not b2 (another key).
	int64_t one = 1;
	ak_val f = reload_of(7, &one);
	run_all();
	CHECK_EQ_INT(a1.nruns, 2);
	CHECK_EQ_INT(b1.nruns, 2);
	CHECK_EQ_INT(a2.nruns, 1);
	CHECK_EQ_INT(b2.nruns, 1);
	CHECK(ak_ui_load_loading(a1.loader) && ak_ui_load_loading(b1.loader));
	CHECK_EQ_INT(fut_state(f), AK_FUT_PENDING);
	ak_future_resolve(a1.runs[1], (ak_val){.i = 2});
	run_all();
	CHECK_EQ_INT(fut_state(f), AK_FUT_PENDING);
	// b1's root goes: what waited for it counts it answered.
	ak_owner_dispose(rb);
	run_all();
	CHECK_EQ_INT(fut_state(f), AK_FUT_DONE);
	CHECK_EQ_INT(ak_ui_load_registered(), before + 2);
	ak_release(f.p);

	// No arguments: every load of the key, whatever its arguments.
	f = reload_of(7, NULL);
	run_all();
	CHECK_EQ_INT(a1.nruns, 3);
	CHECK_EQ_INT(a2.nruns, 2);
	ak_future_resolve(a2.runs[1], (ak_val){.i = 3});
	ak_future_reject(a1.runs[2], error_of("offline"));  // a failure is an answer
	run_all();
	CHECK_EQ_INT(fut_state(f), AK_FUT_DONE);
	CHECK_EQ_INT(ak_ui_load_state(a1.loader), 2);
	ak_release(f.p);

	// An argument that changed is what's compared: a2 is 5 now.
	write_i(a2.id, 5);
	run_all();
	ak_future_resolve(a2.runs[2], (ak_val){.i = 5});
	int64_t two = 2, five = 5;
	f = reload_of(7, &two);
	CHECK_EQ_INT(fut_state(f), AK_FUT_DONE);  // none has 2
	ak_release(f.p);
	f = reload_of(7, &five);
	run_all();
	CHECK_EQ_INT(a2.nruns, 4);
	// A later run, started before it answers, answers it too.
	write_i(a2.id, 6);
	run_all();
	CHECK_EQ_INT(a2.nruns, 5);
	ak_future_resolve(a2.runs[4], (ak_val){.i = 6});
	run_all();
	CHECK_EQ_INT(fut_state(f), AK_FUT_DONE);
	ak_release(f.p);

	// A key no load has answers at once.
	f = reload_of(9, NULL);
	CHECK_EQ_INT(fut_state(f), AK_FUT_DONE);
	ak_release(f.p);

	ak_owner_dispose(ra);
	CHECK_EQ_INT(ak_ui_load_registered(), before);
	ak_sched_run();
	for (keyed **k = (keyed *[]){&a1, &a2, &b1, &b2, NULL}; *k; k++) keyed_free(*k);
}

// A registered load's runs allocate no more than an unregistered one's:
// its entry and its arguments' array are made with its first run.
TEST(a_registered_load_allocates_its_entry_once) {
	setup();
	keyed k;
	keyed_init(&k, 11, 1);
	keyed *ks[] = {&k, NULL};
	ak_owner *r = ak_root(keyed_make, ks);
	run_all();
	int64_t v = 1;
	ak_future_resolve(k.runs[0], (ak_val){.i = v++});
	for (int i = 0; i < 2; i++) {  // warm
		ak_ui_load_reload(k.loader);
		run_all();
		ak_future_resolve(k.runs[k.nruns - 1], (ak_val){.i = v++});
		run_all();
	}
	uint64_t before = ak_alloc_count;
	ak_ui_load_reload(k.loader);
	run_all();
	ak_future_resolve(k.runs[k.nruns - 1], (ak_val){.i = v++});
	run_all();
	uint64_t per_run = ak_alloc_count - before;
	for (int i = 0; i < 2; i++) {
		ak_ui_load_reload(k.loader);
		run_all();
		ak_future_resolve(k.runs[k.nruns - 1], (ak_val){.i = v++});
		run_all();
	}
	CHECK_EQ_INT(ak_alloc_count - before, 3 * per_run);
	// The future, the test's reference, the ticket, the future's waiter.
	CHECK(per_run <= 4);
	ak_owner_dispose(r);
	ak_sched_run();
	keyed_free(&k);
}

// ---- Grouped loads (RFC 0017) ----

// A group of two calls: each its own cell (what its arguments read), its
// own runs; combine makes their answers one value, a * 100 + b.
static void *gid[2];
static ak_future *gruns[2][16];
static int ngruns[2];
static int combines;

static ak_val group_thunk(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	int i = (int)(intptr_t)c->fn;
	ak_val arg = ak_vm_cell_get(gid[i], true);
	ak_str *kinds = ak_str_c("1");
	ak_ui_load_key(20 + i, kinds, &arg, 1);  // keys 20 and 21: two functions
	ak_release(kinds);
	ak_future *f = ak_future_new(AK_K_INT);
	gruns[i][ngruns[i]++] = ak_retain(f);
	return (ak_val){.p = f};
}

static ak_val combine2(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	combines++;
	return (ak_val){.i = a[0].i * 100 + a[1].i};
}

static bool group_busy = true;  // a binding reads .loading

static ak_val group_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_closure *t[2] = {fn(group_thunk, (void *)(intptr_t)0), fn(group_thunk, (void *)(intptr_t)1)};
	ak_closure *comb = fn(combine2, NULL);
	loader = ak_ui_load_group(comb, AK_K_INT, t, 2);
	ak_release(t[0]), ak_release(t[1]), ak_release(comb);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_closure *sel = fn(state_of, NULL), *build = fn(case_view, NULL);
	add(col, ak_ui_switch(sel, build));
	ak_release(sel), ak_release(build);
	if (group_busy) {
		ak_view *busy = ak_ui_new(AK_EL_TEXT);
		bind(busy, AK_KEY_TEXT_TEXT, busy_text, NULL);
		add(col, busy);
	}
	ak_view *val = ak_ui_new(AK_EL_TEXT);
	bind(val, AK_KEY_TEXT_TEXT, value_text, NULL);
	add(col, val);
	return (ak_val){.p = col};
}

static ak_uitest *group_start(void) {
	state_reads = busy_reads = value_reads = builds = combines = 0;
	ngruns[0] = ngruns[1] = 0;
	gid[0] = cell_int(1);
	gid[1] = cell_int(2);
	ak_uitest *a = mount(group_app, NULL);
	ak_uitest_settle(a);
	return a;
}

static void ganswer(ak_uitest *a, int slot, int64_t v) {
	ak_future_resolve(gruns[slot][ngruns[slot] - 1], (ak_val){.i = v});
	settle(a);
}

static void group_finish(ak_uitest *a) {
	done(a);
	ak_release(loader.p);
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < ngruns[i]; j++) ak_release(gruns[i][j]);
		ak_release(gid[i]);
	}
}

// Pending until both answered, then both shown in one write; a slow call
// holds the other's answer, which is shown with its own.
TEST(a_group_lands_together) {
	ak_uitest *a = group_start();
	REQUIRE(ngruns[0] == 1 && ngruns[1] == 1);
	check_text(root(a), "pending|busy|value -1");
	ganswer(a, 0, 1);
	check_text(root(a), "pending|busy|value -1");
	int values = value_reads, states = state_reads;
	ganswer(a, 1, 2);
	check_text(root(a), "ready|idle|value 102");
	CHECK_EQ_INT(value_reads, values + 1);
	CHECK_EQ_INT(state_reads, states + 1);
	// Both again: the first answer is held for the second.
	ak_ui_load_reload(loader);
	settle(a);
	REQUIRE(ngruns[0] == 2 && ngruns[1] == 2);
	check_text(root(a), "ready|busy|value 102");
	values = value_reads;
	ganswer(a, 0, 3);
	check_text(root(a), "ready|busy|value 102");
	CHECK_EQ_INT(value_reads, values);
	ganswer(a, 1, 4);
	check_text(root(a), "ready|idle|value 304");
	CHECK_EQ_INT(value_reads, values + 1);
	CHECK_EQ_INT(builds, 2);
	group_finish(a);
}

// A change of what one call reads runs that call alone: the other's
// answer stays, and what the group shows changes in one write.
TEST(a_group_runs_in_part) {
	ak_uitest *a = group_start();
	ganswer(a, 0, 1);
	ganswer(a, 1, 2);
	int values = value_reads, c0 = combines;
	write_i(gid[1], 5);
	settle(a);
	CHECK_EQ_INT(ngruns[0], 1);
	CHECK_EQ_INT(ngruns[1], 2);
	check_text(root(a), "ready|busy|value 102");
	ganswer(a, 1, 5);
	check_text(root(a), "ready|idle|value 105");
	CHECK_EQ_INT(value_reads, values + 1);
	CHECK_EQ_INT(combines, c0 + 1);
	// An answer equal to the last makes nothing: no tuple, no write.
	write_i(gid[1], 6);
	settle(a);
	ganswer(a, 1, 5);
	CHECK_EQ_INT(combines, c0 + 1);
	CHECK_EQ_INT(value_reads, values + 1);
	group_finish(a);
}

// One call failing fails the group, the first in source order, its
// value kept; once it answers, it's ready again.
TEST(a_group_fails_with_its_first_error) {
	ak_uitest *a = group_start();
	ganswer(a, 0, 1);
	ganswer(a, 1, 2);
	ak_ui_load_reload(loader);
	settle(a);
	ak_future_reject(gruns[1][1], error_of("posts"));
	settle(a);
	ak_future_reject(gruns[0][1], error_of("user"));
	settle(a);
	check_text(root(a), "failed|idle|value 102");
	ak_val e = ak_ui_load_error(loader);
	CHECK_EQ_STR(ak_error_message(e.p)->data, "user");
	ak_release(e.p);
	// Only the first call again: the second's failure still stands.
	write_i(gid[0], 7);
	settle(a);
	ganswer(a, 0, 7);
	check_text(root(a), "failed|idle|value 702");
	e = ak_ui_load_error(loader);
	CHECK_EQ_STR(ak_error_message(e.p)->data, "posts");
	ak_release(e.p);
	write_i(gid[1], 8);
	settle(a);
	ganswer(a, 1, 8);
	check_text(root(a), "ready|idle|value 708");
	group_finish(a);
}

// A group that fails before every call answered once is failed, with no
// value; it's pending no more.
TEST(a_group_fails_before_its_first_answer) {
	ak_uitest *a = group_start();
	ganswer(a, 0, 1);
	ak_future_reject(gruns[1][0], error_of("posts"));
	settle(a);
	check_text(root(a), "failed|idle|value -1");
	ak_ui_load_reload(loader);
	settle(a);
	ganswer(a, 0, 1);
	ganswer(a, 1, 2);
	check_text(root(a), "ready|idle|value 102");
	group_finish(a);
}

// reload of one call's function runs that call only; its future answers
// as the group lands.
TEST(reload_reaches_a_group_by_one_call) {
	ak_uitest *a = group_start();
	ganswer(a, 0, 1);
	ganswer(a, 1, 2);
	ak_str *kinds = ak_str_c("1");
	ak_val arg = {.i = 2};
	ak_val f = ak_ui_load_reload_all(21, true, kinds, &arg, 1);
	settle(a);
	CHECK_EQ_INT(ngruns[0], 1);
	CHECK_EQ_INT(ngruns[1], 2);
	CHECK_EQ_INT(fut_state(f), AK_FUT_PENDING);
	ganswer(a, 1, 9);
	check_text(root(a), "ready|idle|value 109");
	CHECK_EQ_INT(fut_state(f), AK_FUT_DONE);
	ak_release(f.p);
	// Both of its calls' functions: one wait, both run.
	f = ak_ui_load_reload_all(20, false, kinds, NULL, 0);
	ak_val g = ak_ui_load_reload_all(21, false, kinds, NULL, 0);
	settle(a);
	CHECK_EQ_INT(ngruns[0], 2);
	CHECK_EQ_INT(ngruns[1], 3);
	ganswer(a, 0, 1);
	CHECK_EQ_INT(fut_state(f), AK_FUT_PENDING);
	ganswer(a, 1, 9);
	CHECK_EQ_INT(fut_state(f), AK_FUT_DONE);
	CHECK_EQ_INT(fut_state(g), AK_FUT_DONE);
	ak_release(f.p), ak_release(g.p);
	ak_release(kinds);
	group_finish(a);
}

// A call that answered and waits for a slow sibling for more than 2 s,
// with nothing reading .loading, is said (long-hold), naming the wait;
// read, it isn't.
TEST(a_group_holding_an_answer_says_so) {
	listen();
	ak_sched_manual_clock(true);
	for (int watched = 0; watched < 2; watched++) {
		ak_diag_reset();
		said.n = 0;
		group_busy = watched;
		ak_uitest *a = group_start();
		ganswer(a, 0, 1);
		ganswer(a, 1, 2);
		write_i(gid[0], 3);
		write_i(gid[1], 4);
		settle(a);
		ganswer(a, 0, 3);
		ak_sched_advance(1000000000LL);
		ak_sched_run();
		CHECK_EQ_INT(said.n, 0);
		ak_sched_advance(1500000000LL);
		ak_sched_run();
		CHECK_EQ_INT(said.n, watched ? 0 : 1);
		if (!watched) {
			CHECK_EQ_STR(said.code, "long-hold");
			CHECK(strstr(said.msg, "has held a call's answer for 2 s, waiting for a call's") != NULL);
		}
		ganswer(a, 1, 4);
		group_finish(a);
	}
	group_busy = true;
	ak_sched_manual_clock(false);
	unlisten();
}

// A group's runs, steady, allocate their tickets and futures, the
// combined value's only when an answer changed (an int here: none).
TEST(a_group_allocates_its_runs) {
	ak_uitest *a = group_start();
	ganswer(a, 0, 1);
	ganswer(a, 1, 2);
	int64_t v = 3;
	for (int i = 0; i < 6; i++) {  // warm: the vectors the runs use grow
		ak_ui_load_reload(loader);
		settle(a);
		ganswer(a, 0, v);
		ganswer(a, 1, v++);
	}
	uint64_t before = ak_alloc_count;
	ak_ui_load_reload(loader);
	settle(a);
	ganswer(a, 0, v);
	ganswer(a, 1, v++);
	uint64_t per = ak_alloc_count - before;
	for (int i = 0; i < 2; i++) {
		ak_ui_load_reload(loader);
		settle(a);
		ganswer(a, 0, v);
		ganswer(a, 1, v++);
	}
	CHECK_EQ_INT(ak_alloc_count - before, 3 * per);
	group_finish(a);
}

T_MAIN();
