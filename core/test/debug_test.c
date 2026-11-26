#define _POSIX_C_SOURCE 200809L  // nanosleep under -std=c17
// The VM's debugger (core/src/vm/debug.c): breakpoints, steps, pauses,
// what a stop says of the stack and its locals, and the paused main
// thread taking commands from another thread. Its program is
// testdata/modules/debug.asuka, compiled as asuka dev compiles (TestC
// compiles it: AK_TEST_MODULES).
#include "t.h"
#include "vm/vm_int.h"
#include "vm/debug.h"
#include "base/thread.h"
#include "signal/signal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define FILE_NAME "debug.asuka"
// debug.asuka's lines.
#define L_SUM 19     // let sum = a + b
#define L_RETSUM 20  // return sum
#define L_TOTAL 24   // var total = 0
#define L_P 25       // let p = Point(x: 1, y: 2)
#define L_FOR 30     // for i in 0..<3 {
#define L_BODY 31    //   total = add(total, i)
#define L_LABEL 33   // let label = "done"
#define L_VALUES 55  // return bump() + ages.count()

static uint8_t *read_module(const char *name, size_t *n) {
	const char *dir = getenv("AK_TEST_MODULES");
	if (!dir) return NULL;
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.akb", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = malloc((size_t)len);
	*n = fread(p, 1, (size_t)len, f);
	fclose(f);
	return p;
}

static void run_init(void *ctx) { ak_vm_call(ctx, NULL, 0); }

// load loads debug.asuka's module and runs its init.
static ak_module *load(void) {
	static bool inited;
	if (!inited) {
		inited = true;
		ak_signal_bind_thread();
		ak_vm_init();
	}
	size_t n;
	uint8_t *p = read_module("debug", &n);
	if (!p) return NULL;
	char err[256];
	ak_module *m = ak_module_load(p, n, err, sizeof err);
	free(p);
	if (!m) {
		fprintf(stderr, "%s\n", err);
		return NULL;
	}
	ak_owner *root = ak_root_new();
	ak_with_owner(root, run_init, (void *)ak_module_func(m, ak_module_init(m)));
	ak_owner_dispose(root);
	return m;
}

static int64_t call(ak_module *m, const char *fn) {
	ak_val v = ak_vm_call(ak_module_find(m, fn), NULL, 0);
	CHECK(!ak_error_pending());
	return v.i;
}

// ---- What the debugger told ----

#define MAX_STOPS 16
static char *stops[MAX_STOPS];
static int nstops, ncontinued;
static char *vars;  // the last variables answer
static uint32_t vars_seq;
// What the sink does at each stop: the commands it gives (0: none, an
// other thread gives it), and a variables request first.
static int script[MAX_STOPS];
static int nscript;
static int ask_frame = -1;
static uint32_t ask_path[4], ask_npath;

static char *dup_json(const char *json, size_t len) {
	char *s = malloc(len + 1);
	memcpy(s, json, len);
	s[len] = 0;
	return s;
}

// What evaluations answered (AK_DBG_RESULT) and what logpoints and
// failed conditions said (AK_DBG_LOG), in order; on_stop, when set, asks
// more at each stop, before its command.
#define MAX_ANSWERS 16
static char *answers[MAX_ANSWERS], *logs[MAX_ANSWERS];
static uint32_t answer_seqs[MAX_ANSWERS];
static int nanswers, nlogs;
static void (*on_stop)(int stop);

static void sink(int event, uint32_t seq, const char *json, size_t len) {
	switch (event) {
	case AK_DBG_STOPPED: {
		int i = nstops;
		if (nstops < MAX_STOPS) stops[nstops++] = dup_json(json, len);
		if (ask_frame >= 0) ak_dbg_variables(7, (uint32_t)ask_frame, ask_path, ask_npath, 0, 0);
		if (on_stop) on_stop(i);
		int cmd = i < nscript ? script[i] : AK_DBG_CONTINUE;
		if (cmd) ak_dbg_command(cmd);
		break;
	}
	case AK_DBG_CONTINUED:
		ncontinued++;
		break;
	case AK_DBG_VARIABLES:
		free(vars);
		vars = dup_json(json, len);
		vars_seq = seq;
		break;
	case AK_DBG_RESULT:
		if (nanswers < MAX_ANSWERS) answer_seqs[nanswers] = seq, answers[nanswers++] = dup_json(json, len);
		break;
	case AK_DBG_LOG:
		if (nlogs < MAX_ANSWERS) logs[nlogs++] = dup_json(json, len);
		break;
	}
}

static void reset(void) {
	for (int i = 0; i < nstops; i++) free(stops[i]);
	for (int i = 0; i < nanswers; i++) free(answers[i]);
	for (int i = 0; i < nlogs; i++) free(logs[i]);
	nstops = ncontinued = nscript = nanswers = nlogs = 0;
	ask_frame = -1;
	on_stop = NULL;
	free(vars);
	vars = NULL;
	ak_dbg_reset();
	ak_dbg_exceptions(0);
	ak_dbg_set_sink(sink);
}

// ---- What the debugger's CLI compiled: debug.evals ----

// An expression of debug.evals (core_test.go's compileEvals): compiled
// for a frame of function fn at pc, it's the extension's function func,
// taking the locals params, its value of descriptor desc.
typedef struct {
	char name[32], desc[32];
	uint32_t fn, pc, func, np, params[16];
} eval_t;
static eval_t evals[32];
static int nevals;
static uint8_t *ext;
static size_t next;

static uint8_t *read_file(const char *name, size_t *n) {
	const char *dir = getenv("AK_TEST_MODULES");
	if (!dir) return NULL;
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = malloc((size_t)len + 1);
	*n = fread(p, 1, (size_t)len, f);
	p[*n] = 0;
	fclose(f);
	return p;
}

// load_evals reads debug.evals and debug.akx, once, and hands the
// debugger the extension.
static bool load_evals(void) {
	if (!ext) {
		size_t n;
		char *t = (char *)read_file("debug.evals", &n);
		if (!t) return false;
		ext = read_file("debug.akx", &next);
		for (char *line = strtok(t, "\n"); line && nevals < 32; line = strtok(NULL, "\n")) {
			eval_t *e = &evals[nevals];
			int off = 0;
			if (sscanf(line, "%31s %u %u %u %31s %u%n", e->name, &e->fn, &e->pc, &e->func, e->desc, &e->np, &off) < 6) continue;
			for (uint32_t i = 0; i < e->np && i < 16; i++) {
				int k = 0;
				sscanf(line + off, " %u%n", &e->params[i], &k);
				off += k;
			}
			nevals++;
		}
		free(t);
	}
	if (!ext) return false;
	ak_dbg_extend(ext, next);
	return true;
}

static const eval_t *eval_named(const char *name) {
	for (int i = 0; i < nevals; i++)
		if (strcmp(evals[i].name, name) == 0) return &evals[i];
	T_FAIL("no eval %s", name);
	return &evals[0];
}

static uint64_t hash_of(ak_module *m) { return m->hash; }
static ak_module *eval_module;

// evaluate asks the paused main thread to evaluate name in a frame of
// the stop's.
static void evaluate(uint32_t seq, uint32_t frame, const char *name) {
	const eval_t *e = eval_named(name);
	ak_dbg_evaluate(seq, frame, hash_of(eval_module), e->func, e->params, e->np, e->desc, strlen(e->desc));
}

// a breakpoint at line of fn's (by name) whose condition (cond) or log
// message (log) is debug.evals' expression of that name.
static ak_dbg_code code_for(ak_module *m, const char *cond, const char *log) {
	const eval_t *e = eval_named(cond ? cond : log);
	return (ak_dbg_code){.base = hash_of(m), .func = e->fn, .cond = cond ? e->func : AK_DBG_NONE, .log = log ? e->func : AK_DBG_NONE, .nparams = e->np, .params = e->params};
}

static void breakpoints(const uint32_t *lines, uint32_t n) {
	ak_dbg_bp bps[16];
	for (uint32_t i = 0; i < n && i < 16; i++) bps[i] = (ak_dbg_bp){.line = lines[i]};
	ak_dbg_set_breakpoints(FILE_NAME, strlen(FILE_NAME), bps, n);
}

// top_line is the line of a stop's innermost frame.
static int top_line(const char *stop) {
	const char *l = strstr(stop, "\"line\":");
	return l ? atoi(l + 7) : -1;
}

static bool top_is(const char *stop, const char *fn) {
	char want[64];
	snprintf(want, sizeof want, "\"frames\":[{\"name\":\"%s\"", fn);
	return strstr(stop, want) != NULL;
}

#define CHECK_HAS(s, sub) do { const char *s_ = (s); if (!s_ || !strstr(s_, sub)) T_FAIL("%s has no %s:\n%s", #s, sub, s_ ? s_ : "(null)"); } while (0)

// ---- Tests ----

TEST(debug_off) {
	ak_module *m = load();
	if (!m) {
		fprintf(stderr, "debug: no AK_TEST_MODULES (go test ./core builds them)\n");
		return;
	}
	reset();
	// Nothing set: the interpreter's flag is off, and run runs through.
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 0);
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(nstops, 0);
	ak_module_release(m);
}

// A breakpoint in a function a loop calls stops each time, with the
// stack (innermost first) and each frame's locals as they are then.
TEST(debug_breakpoint) {
	ak_module *m = load();
	if (!m) return;
	reset();
	uint32_t lines[] = {L_RETSUM};
	breakpoints(lines, 1);
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 1);
	CHECK_EQ_INT(call(m, "run"), 4);
	REQUIRE(nstops == 3);
	CHECK_EQ_INT(ncontinued, 3);
	for (int i = 0; i < 3; i++) {
		CHECK(top_is(stops[i], "add"));
		CHECK_EQ_INT(top_line(stops[i]), L_RETSUM);
		CHECK_HAS(stops[i], "{\"reason\":\"breakpoint\",\"totalFrames\":2,");
		CHECK_HAS(stops[i], "\"file\":\"debug.asuka\"");
	}
	const char *s = stops[0];
	if (getenv("T_VERBOSE")) fprintf(stderr, "%s\n", s);
	CHECK_HAS(s, "\"locals\":[{\"name\":\"a\",\"type\":\"int\",\"value\":\"0\"},{\"name\":\"b\",\"type\":\"int\",\"value\":\"0\"},{\"name\":\"sum\",\"type\":\"int\",\"value\":\"0\"}]");
	CHECK_HAS(stops[1], "{\"name\":\"a\",\"type\":\"int\",\"value\":\"0\"},{\"name\":\"b\",\"type\":\"int\",\"value\":\"1\"},{\"name\":\"sum\",\"type\":\"int\",\"value\":\"1\"}");
	CHECK_HAS(stops[2], "{\"name\":\"a\",\"type\":\"int\",\"value\":\"1\"},{\"name\":\"b\",\"type\":\"int\",\"value\":\"2\"},{\"name\":\"sum\",\"type\":\"int\",\"value\":\"3\"}");
	// The caller: run, at the call's line, its locals in scope there (not
	// label, declared after the loop), as the language writes them.
	CHECK_HAS(s, "{\"name\":\"run\",\"file\":\"debug.asuka\",\"line\":31,\"locals\":[");
	CHECK_HAS(s, "{\"name\":\"total\",\"type\":\"int\",\"value\":\"0\"}");
	CHECK_HAS(s, "{\"name\":\"p\",\"type\":\"Point\",\"value\":\"Point(x: 1, y: 2)\",\"children\":2}");
	CHECK_HAS(s, "{\"name\":\"names\",\"type\":\"[string]\",\"value\":\"[\\\"a\\\", \\\"b\\\"]\",\"children\":2}");
	CHECK_HAS(s, "{\"name\":\"shape\",\"type\":\"Shape\",\"value\":\".circle(radius: 1.5)\",\"children\":1}");
	CHECK_HAS(s, "{\"name\":\"light\",\"type\":\"Light\",\"value\":\".green\"}");
	CHECK_HAS(s, "{\"name\":\"wait\",\"type\":\"duration\",\"value\":\"300ms\"}");
	CHECK_HAS(s, "{\"name\":\"i\",\"type\":\"int\",\"value\":\"0\"}");
	CHECK(!strstr(s, "\"label\""));
	CHECK_HAS(stops[2], "{\"name\":\"total\",\"type\":\"int\",\"value\":\"1\"}");
	CHECK_HAS(stops[2], "{\"name\":\"i\",\"type\":\"int\",\"value\":\"2\"}");

	// Removed: it runs through.
	breakpoints(NULL, 0);
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 0);
	reset();
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(nstops, 0);
	ak_module_release(m);
}

// A breakpoint in a loop's body stops each time round.
TEST(debug_loop) {
	ak_module *m = load();
	if (!m) return;
	reset();
	uint32_t lines[] = {L_BODY};
	breakpoints(lines, 1);
	CHECK_EQ_INT(call(m, "run"), 4);
	REQUIRE(nstops == 3);
	for (int i = 0; i < 3; i++) {
		CHECK(top_is(stops[i], "run"));
		CHECK_EQ_INT(top_line(stops[i]), L_BODY);
		char want[64];
		snprintf(want, sizeof want, "{\"name\":\"i\",\"type\":\"int\",\"value\":\"%d\"}", i);
		CHECK_HAS(stops[i], want);
	}
	ak_module_release(m);
}

// Over, in and out, by line and frame.
TEST(debug_step) {
	ak_module *m = load();
	if (!m) return;
	reset();
	uint32_t lines[] = {L_FOR};
	breakpoints(lines, 1);
	int cmds[] = {AK_DBG_NEXT, AK_DBG_STEP_IN, AK_DBG_NEXT, AK_DBG_STEP_OUT, AK_DBG_NEXT, AK_DBG_NEXT, AK_DBG_CONTINUE};
	memcpy(script, cmds, sizeof cmds);
	nscript = (int)(sizeof cmds / sizeof *cmds);
	CHECK_EQ_INT(call(m, "run"), 4);
	struct {
		const char *fn;
		int line;
		const char *reason;
	} want[] = {
	    {"run", L_FOR, "breakpoint"},   // stopped at the for
	    {"run", L_BODY, "step"},        // over: its body's line
	    {"add", L_SUM, "step"},         // in: add's first line
	    {"add", L_RETSUM, "step"},      // over
	    {"run", L_BODY, "step"},        // out: back in run, after the call
	    {"run", L_FOR, "step"},         // over: the loop goes round
	    {"run", L_BODY, "step"},        // over: the body, i 1
	};
	REQUIRE(nstops == (int)(sizeof want / sizeof *want));
	for (int i = 0; i < nstops; i++) {
		if (!top_is(stops[i], want[i].fn) || top_line(stops[i]) != want[i].line) T_FAIL("stop %d: want %s:%d, got %s", i, want[i].fn, want[i].line, stops[i]);
		char r[64];
		snprintf(r, sizeof r, "{\"reason\":\"%s\"", want[i].reason);
		CHECK_HAS(stops[i], r);
	}
	CHECK_HAS(stops[2], "\"totalFrames\":2");
	CHECK_HAS(stops[6], "{\"name\":\"i\",\"type\":\"int\",\"value\":\"1\"}");
	// Over steps over a call, and the line after the loop comes after it.
	reset();
	uint32_t body[] = {L_BODY};
	breakpoints(body, 1);
	int over[] = {AK_DBG_NEXT, AK_DBG_NEXT, AK_DBG_STEP_OUT};
	memcpy(script, over, sizeof over);
	nscript = 3;
	CHECK_EQ_INT(call(m, "run"), 4);
	// body (i 0), over the call to the for, over to the body (i 1: the
	// breakpoint, which stops first), out: the breakpoint again (i 2),
	// before run returns.
	REQUIRE(nstops == 4);
	CHECK_EQ_INT(top_line(stops[0]), L_BODY);
	CHECK_EQ_INT(top_line(stops[1]), L_FOR);
	CHECK_HAS(stops[1], "{\"reason\":\"step\"");
	for (int i = 2; i < 4; i++) {
		CHECK_EQ_INT(top_line(stops[i]), L_BODY);
		CHECK_HAS(stops[i], "{\"reason\":\"breakpoint\"");
	}
	CHECK_HAS(stops[3], "{\"name\":\"i\",\"type\":\"int\",\"value\":\"2\"}");
	ak_module_release(m);
}

// Pause stops at the next instruction the app runs.
TEST(debug_pause) {
	ak_module *m = load();
	if (!m) return;
	reset();
	ak_dbg_command(AK_DBG_PAUSE);
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 1);
	int cmds[] = {AK_DBG_NEXT};
	memcpy(script, cmds, sizeof cmds);
	nscript = 1;
	CHECK_EQ_INT(call(m, "run"), 4);
	REQUIRE(nstops == 2);
	CHECK(top_is(stops[0], "run"));
	CHECK_HAS(stops[0], "{\"reason\":\"pause\"");
	CHECK_EQ_INT(top_line(stops[0]), L_TOTAL);
	CHECK_EQ_INT(top_line(stops[1]), L_P);
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 0);
	ak_module_release(m);
}

// A value's children, by its path from a local.
TEST(debug_variables) {
	ak_module *m = load();
	if (!m) return;
	reset();
	uint32_t lines[] = {L_RETSUM};
	breakpoints(lines, 1);
	// Frame 1 is run's; its locals: total, p, names, shape, ...
	ask_frame = 1;
	ask_path[0] = 1;  // p
	ask_npath = 1;
	nscript = 1;
	script[0] = AK_DBG_CONTINUE;
	call(m, "run");
	CHECK_EQ_INT(vars_seq, 7);
	CHECK_EQ_STR(vars, "{\"total\":2,\"variables\":[{\"name\":\"x\",\"type\":\"int\",\"value\":\"1\"},{\"name\":\"y\",\"type\":\"int\",\"value\":\"2\"}]}");
	reset();
	breakpoints(lines, 1);
	ask_frame = 1;
	ask_path[0] = 2;  // names
	ask_npath = 1;
	call(m, "run");
	CHECK_EQ_STR(vars, "{\"total\":2,\"variables\":[{\"name\":\"[0]\",\"type\":\"string\",\"value\":\"\\\"a\\\"\"},{\"name\":\"[1]\",\"type\":\"string\",\"value\":\"\\\"b\\\"\"}]}");
	reset();
	breakpoints(lines, 1);
	ask_frame = 1;
	ask_path[0] = 3;  // shape
	ask_npath = 1;
	call(m, "run");
	CHECK_EQ_STR(vars, "{\"total\":1,\"variables\":[{\"name\":\"radius\",\"type\":\"float\",\"value\":\"1.5\"}]}");
	reset();
	breakpoints(lines, 1);
	ask_frame = 1;
	ask_path[0] = 1;
	ask_path[1] = 5;  // p has 2
	ask_npath = 2;
	call(m, "run");
	CHECK_EQ_STR(vars, "{\"error\":\"no such variable\"}");
	// Not paused: an error at once.
	reset();
	ak_dbg_variables(9, 0, ask_path, 1, 0, 0);
	CHECK_EQ_INT(vars_seq, 9);
	CHECK_EQ_STR(vars, "{\"error\":\"not paused\"}");
	ak_module_release(m);
}

// The paused main thread takes its commands from another thread: the
// connection's reader, which hands them over directly.
static ak_mutex hmu = AK_MUTEX_INIT;
static ak_cond hcv;
static int hstops;
static char *hvars;

static void handoff_sink(int event, uint32_t seq, const char *json, size_t len) {
	AK_UNUSED(seq);
	ak_mutex_lock(&hmu);
	if (event == AK_DBG_STOPPED) hstops++;
	if (event == AK_DBG_VARIABLES) {
		free(hvars);
		hvars = dup_json(json, len);
	}
	ak_cond_broadcast(&hcv);
	ak_mutex_unlock(&hmu);
}

static void helper(void *arg) {
	AK_UNUSED(arg);
	// Each stop: ask for run's p, wait for the answer, then go on.
	for (int i = 1; i <= 3; i++) {
		ak_mutex_lock(&hmu);
		while (hstops < i) ak_cond_wait(&hcv, &hmu);
		free(hvars);
		hvars = NULL;
		ak_mutex_unlock(&hmu);
		CHECK(ak_dbg_paused());
		uint32_t path[] = {1};
		ak_dbg_variables(1, 1, path, 1, 0, 0);
		ak_mutex_lock(&hmu);
		while (!hvars) ak_cond_wait(&hcv, &hmu);
		ak_mutex_unlock(&hmu);
		CHECK(strstr(hvars, "{\"name\":\"x\",\"type\":\"int\",\"value\":\"1\"}") != NULL);
		ak_dbg_command(i < 3 ? AK_DBG_CONTINUE : AK_DBG_PAUSE);  // a pause while paused does nothing
		if (i == 3) {
			// The CLI went: what's paused goes on.
			ak_dbg_reset();
		}
	}
}

TEST(debug_handoff) {
	ak_module *m = load();
	if (!m) return;
	reset();
	ak_cond_init(&hcv);
	hstops = 0;
	ak_dbg_set_sink(handoff_sink);
	uint32_t lines[] = {L_RETSUM};
	breakpoints(lines, 1);
	REQUIRE(ak_thread_start(helper, NULL));
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(hstops, 3);
	CHECK(!ak_dbg_paused());
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 0);  // reset: no breakpoints left
	free(hvars);
	hvars = NULL;
	ak_module_release(m);
}

// Breakpoints are by file and line: a module loaded after they were set
// (a reload's) stops at them too; a hold lets the app run past them.
TEST(debug_reload) {
	ak_module *m = load();
	if (!m) return;
	reset();
	uint32_t lines[] = {L_LABEL};
	breakpoints(lines, 1);
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(nstops, 1);
	ak_module *m2 = load();
	REQUIRE(m2);
	CHECK_EQ_INT(call(m2, "run"), 4);
	CHECK_EQ_INT(nstops, 2);
	CHECK_EQ_INT(top_line(stops[1]), L_LABEL);
	ak_dbg_hold(true);
	CHECK_EQ_INT(call(m2, "run"), 4);
	CHECK_EQ_INT(nstops, 2);
	ak_dbg_hold(false);
	CHECK_EQ_INT(call(m2, "run"), 4);
	CHECK_EQ_INT(nstops, 3);
	// No sink: nothing stops (no one would hear).
	ak_dbg_set_sink(NULL);
	CHECK_EQ_INT(call(m2, "run"), 4);
	CHECK_EQ_INT(nstops, 3);
	reset();
	ak_dbg_set_sink(NULL);
	ak_module_release(m2);
	ak_module_release(m);
}

// How a stop shows values of each kind: through a var's box, optionals,
// maps, sets, a recursive struct cut short past three levels, a long
// string cut short, a char, a tuple, a sized integer, a closure.
TEST(debug_values) {
	ak_module *m = load();
	if (!m) return;
	reset();
	uint32_t lines[] = {L_VALUES};
	breakpoints(lines, 1);
	ask_frame = 0;
	ask_path[0] = 6;  // deep
	ask_path[1] = 1;  // its next
	ask_npath = 2;
	call(m, "values");
	REQUIRE(nstops == 1);
	const char *s = stops[0];
	if (getenv("T_VERBOSE")) fprintf(stderr, "%s\n", s);
	CHECK_HAS(s, "{\"name\":\"count\",\"type\":\"int\",\"value\":\"2\"}");
	CHECK_HAS(s, "{\"name\":\"bump\",\"type\":\"fn\",\"value\":\"fn values/");
	CHECK_HAS(s, "{\"name\":\"some\",\"type\":\"int?\",\"value\":\"5\"}");
	CHECK_HAS(s, "{\"name\":\"nothing\",\"type\":\"int?\",\"value\":\"none\"}");
	CHECK_HAS(s, "{\"name\":\"ages\",\"type\":\"[string: int]\",\"value\":\"[\\\"ann\\\": 30, \\\"bo\\\": 4]\",\"children\":2}");
	CHECK_HAS(s, "{\"name\":\"ids\",\"type\":\"Set<int>\",\"value\":\"Set([3, 1])\",\"children\":2}");
	CHECK_HAS(s, "{\"name\":\"deep\",\"type\":\"Node\",\"value\":\"Node(name: \\\"a\\\", next: Node(name: \\\"b\\\", next: Node(name: \\\"c\\\", next: Node(name: \\\"d\\\", next: …))))\",\"children\":2}");
	char want[512] = "{\"name\":\"long\",\"type\":\"string\",\"value\":\"\\\"";
	for (int i = 0; i < 120; i++) strcat(want, "x");
	strcat(want, "…\\\"\"}");
	CHECK_HAS(s, want);
	CHECK_HAS(s, "{\"name\":\"letter\",\"type\":\"char\",\"value\":\"'q'\"}");
	CHECK_HAS(s, "{\"name\":\"pair\",\"type\":\"(int,string)\",\"value\":\"(1, \\\"one\\\")\",\"children\":2}");
	CHECK_HAS(s, "{\"name\":\"small\",\"type\":\"u8\",\"value\":\"200\"}");
	// deep.next's children: its name, and its own next (an optional).
	CHECK_EQ_STR(vars, "{\"total\":2,\"variables\":[{\"name\":\"name\",\"type\":\"string\",\"value\":\"\\\"b\\\"\"},{\"name\":\"next\",\"type\":\"Node?\",\"value\":\"Node(name: \\\"c\\\", next: Node(name: \\\"d\\\", next: Node(name: \\\"e\\\", next: none)))\",\"children\":2}]}");
	ak_module_release(m);
}

// ---- Evaluating, conditions, logpoints ----

static void evaluate_at_add(int stop) {
	if (stop != 0) return;
	evaluate(1, 0, "sum");      // a * 10 + b, in add's frame
	evaluate(2, 1, "swapped");  // a struct, in run's: its children next
	uint32_t root[] = {0};
	ak_dbg_variables(9, AK_DBG_RESULT_FRAME | 1, root, 1, 0, 0);
	evaluate(3, 1, "names");    // a closure of its own
	evaluate(4, 0, "zero");     // it throws
	evaluate(5, 5, "sum");      // no such frame
}

// While paused, an expression evaluates in a frame of the stop's with
// its locals: its value as a local's, its children asked for by the
// result's frame; what it throws, its message. The app goes on as it
// was.
TEST(debug_evaluate) {
	ak_module *m = load();
	if (!m) return;
	reset();
	if (!load_evals()) {
		fprintf(stderr, "debug: no debug.evals (go test ./core compiles it)\n");
		ak_module_release(m);
		return;
	}
	eval_module = m;
	uint32_t lines[] = {L_RETSUM};
	breakpoints(lines, 1);
	on_stop = evaluate_at_add;
	CHECK_EQ_INT(call(m, "run"), 4);
	REQUIRE(nstops == 3);
	REQUIRE(nanswers == 5);
	CHECK_EQ_INT(answer_seqs[0], 1);
	// add(0, 0): a * 10 + b.
	CHECK_EQ_STR(answers[0], "{\"result\":{\"name\":\"\",\"type\":\"int\",\"value\":\"0\"},\"ref\":0}");
	CHECK_HAS(answers[1], "\"value\":\"Point(x: 2, y: 1)\",\"children\":2},\"ref\":1}");
	CHECK_EQ_STR(vars, "{\"total\":2,\"variables\":[{\"name\":\"x\",\"type\":\"int\",\"value\":\"2\"},{\"name\":\"y\",\"type\":\"int\",\"value\":\"1\"}]}");
	CHECK_HAS(answers[2], "\"type\":\"[string]\",\"value\":\"[\\\"a!\\\", \\\"b!\\\"]\"");
	CHECK_EQ_STR(answers[3], "{\"error\":\"division by zero\"}");
	CHECK_EQ_STR(answers[4], "{\"error\":\"no such frame\"}");
	// Not paused: an error at once.
	evaluate(6, 0, "sum");
	CHECK_EQ_STR(answers[5], "{\"error\":\"not paused\"}");
	// What a module the extension isn't of evaluates: nothing.
	reset();
	breakpoints(lines, 1);
	on_stop = evaluate_at_add;  // no extension now: the CLI's code changed
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_HAS(answers[0], "{\"error\":\"the app's code changed since: evaluate it again\"}");
	ak_dbg_forget_extensions();
	ak_module_release(m);
}

// set_code sets a breakpoint at L_BODY whose code is debug.evals' cond
// or log, with a hit condition.
static void set_code(ak_module *m, const char *cond, const char *log, uint8_t hit_op, uint32_t hit_n, const char *error) {
	ak_dbg_code c = {0};
	if (cond || log) c = code_for(m, cond, log);
	if (error) c.error = error, c.elen = strlen(error);
	ak_dbg_bp bp = {.line = L_BODY, .flags = (uint8_t)((cond ? AK_DBG_BP_COND : 0) | (log ? AK_DBG_BP_LOG : 0)), .hit_op = hit_op, .hit_n = hit_n, .ncode = 1, .code = &c};
	if (!cond && !log) bp.ncode = 0;
	ak_dbg_set_breakpoints(FILE_NAME, strlen(FILE_NAME), &bp, 1);
}

// A breakpoint's condition, its hit count, a logpoint: evaluated as the
// line's reached, the app stopping only where they say.
TEST(debug_conditions) {
	ak_module *m = load();
	if (!m) return;
	reset();
	if (!load_evals()) {
		ak_module_release(m);
		return;
	}
	// i == 2: the third time round.
	set_code(m, "iseq", NULL, 0, 0, NULL);
	CHECK_EQ_INT(call(m, "run"), 4);
	REQUIRE(nstops == 1);
	CHECK_HAS(stops[0], "{\"name\":\"i\",\"type\":\"int\",\"value\":\"2\"}");
	// The second hit; every second; from the second on.
	struct {
		uint8_t op;
		uint32_t n;
		int stops;
		const char *first;
	} hits[] = {
	    {AK_DBG_HIT_EQ, 2, 1, "1"},
	    {AK_DBG_HIT_MOD, 2, 1, "1"},
	    {AK_DBG_HIT_GE, 2, 2, "1"},
	    {AK_DBG_HIT_LT, 2, 1, "0"},
	};
	for (size_t h = 0; h < sizeof hits / sizeof *hits; h++) {
		reset();
		REQUIRE(load_evals());
		set_code(m, NULL, NULL, hits[h].op, hits[h].n, NULL);
		CHECK_EQ_INT(call(m, "run"), 4);
		if (nstops != hits[h].stops) T_FAIL("hit op %d %u: %d stops", hits[h].op, hits[h].n, nstops);
		char want[64];
		snprintf(want, sizeof want, "{\"name\":\"i\",\"type\":\"int\",\"value\":\"%s\"}", hits[h].first);
		if (nstops) CHECK_HAS(stops[0], want);
	}
	// A logpoint: what it says each time, and no stop.
	reset();
	REQUIRE(load_evals());
	set_code(m, NULL, "logi", 0, 0, NULL);
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(nstops, 0);
	REQUIRE(nlogs == 3);
	CHECK_EQ_STR(logs[0], "{\"file\":\"debug.asuka\",\"line\":31,\"text\":\"i is 0, total 0\"}");
	CHECK_EQ_STR(logs[2], "{\"file\":\"debug.asuka\",\"line\":31,\"text\":\"i is 2, total 1\"}");
	// A condition that throws stops, saying why; one that didn't compile
	// too.
	reset();
	REQUIRE(load_evals());
	set_code(m, "isbad", NULL, 0, 0, NULL);
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(nstops, 3);
	REQUIRE(nlogs == 3);
	CHECK_EQ_STR(logs[0], "{\"file\":\"debug.asuka\",\"line\":31,\"error\":\"the breakpoint's condition: division by zero\"}");
	reset();
	REQUIRE(load_evals());
	set_code(m, "iseq", NULL, 0, 0, "unknown name nope");
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(nstops, 3);
	REQUIRE(nlogs >= 1);
	CHECK_HAS(logs[0], "\"error\":\"unknown name nope\"");
	// Code compiled for another module (the one coming): not this one's
	// line, which goes on.
	reset();
	REQUIRE(load_evals());
	ak_dbg_code c = code_for(m, "iseq", NULL);
	c.base ^= 1;
	ak_dbg_bp bp = {.line = L_BODY, .flags = AK_DBG_BP_COND, .ncode = 1, .code = &c};
	ak_dbg_set_breakpoints(FILE_NAME, strlen(FILE_NAME), &bp, 1);
	CHECK_EQ_INT(call(m, "run"), 4);
	CHECK_EQ_INT(nstops, 0);
	ak_dbg_forget_extensions();
	ak_module_release(m);
}

static void call_uncaught(void *ctx) {
	ak_vm_call(ak_module_find(ctx, "uncaughtOne"), NULL, 0);
	if (ak_error_pending()) ak_release(ak_error_take());
}

static void ignore_error(void *ctx, void *err) {
	AK_UNUSED(ctx);
	AK_UNUSED(err);
}

// Errors: with uncaught set, the app stops where an error nothing
// catches is thrown (a defer, which throws it on, doesn't), once; with
// all, at every one. The stop has the error, and the frames as they
// were.
TEST(debug_exceptions) {
	ak_module *m = load();
	if (!m) return;
	reset();
	ak_dbg_exceptions(AK_DBG_EXC_UNCAUGHT);
	CHECK_EQ_INT(atomic_load(&ak_dbg_exc), AK_DBG_EXC_UNCAUGHT);
	CHECK_EQ_INT(call(m, "caughtOne"), -1);
	CHECK_EQ_INT(nstops, 0);
	ak_vm_call(ak_module_find(m, "uncaughtOne"), NULL, 0);
	REQUIRE(ak_error_pending());
	ak_release(ak_error_take());
	REQUIRE(nstops == 1);
	const char *s = stops[0];
	if (getenv("T_VERBOSE")) fprintf(stderr, "%s\n", s);
	CHECK_HAS(s, "{\"reason\":\"exception\",\"exception\":{\"type\":\"Oops\",");
	CHECK_HAS(s, "\"uncaught\":true}");
	CHECK(top_is(s, "fail"));
	CHECK_EQ_INT(top_line(s), 74);
	CHECK_HAS(s, "{\"name\":\"doubled\",\"type\":\"int\",\"value\":\"4\"}");
	CHECK_HAS(s, "{\"name\":\"uncaughtOne\"");
	// An instruction's: divide by zero, in divide called by divideBy.
	reset();
	ak_dbg_exceptions(AK_DBG_EXC_UNCAUGHT);
	ak_val zero = {.i = 0};
	ak_vm_call(ak_module_find(m, "divideBy"), &zero, 1);
	REQUIRE(ak_error_pending());
	ak_release(ak_error_take());
	REQUIRE(nstops == 1);
	CHECK_HAS(stops[0], "\"exception\":{\"type\":\"Error\",\"message\":\"division by zero\",\"uncaught\":true}");
	CHECK(top_is(stops[0], "divide"));
	CHECK_EQ_INT(top_line(stops[0]), 89);
	CHECK_HAS(stops[0], "{\"name\":\"b\",\"type\":\"int\",\"value\":\"0\"}");
	// A Boundary's (a catcher's) around where it ends: caught.
	reset();
	ak_dbg_exceptions(AK_DBG_EXC_UNCAUGHT);
	ak_owner *c = ak_catch(call_uncaught, m, ignore_error, NULL);
	ak_owner_dispose(c);
	CHECK_EQ_INT(nstops, 0);
	// All: the one caught stops too, saying it's caught.
	reset();
	ak_dbg_exceptions(AK_DBG_EXC_ALL);
	CHECK_EQ_INT(call(m, "caughtOne"), -1);
	REQUIRE(nstops == 1);
	CHECK_HAS(stops[0], "\"uncaught\":false}");
	CHECK_EQ_INT(top_line(stops[0]), 74);
	// None: none.
	reset();
	CHECK_EQ_INT(call(m, "caughtOne"), -1);
	ak_vm_call(ak_module_find(m, "uncaughtOne"), NULL, 0);
	ak_release(ak_error_take());
	CHECK_EQ_INT(nstops, 0);
	ak_module_release(m);
}

#if !defined(_WIN32)
// Through asuka dev's connection (abi/dev.c): the CLI's frames reach the
// debugger on the connection's own thread, which answers pings while the
// app is paused; a module that arrives lets a paused app go on, stopping
// nowhere until the main thread loaded it; the CLI going drops what it
// set.
#include "abi/dev.h"
#include "rt/sched.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int cli = -1;

static bool read_frame(uint8_t *kind, uint8_t *buf, size_t cap, size_t *n) {
	uint8_t h[5];
	for (size_t off = 0; off < 5;) {
		ssize_t r = recv(cli, h + off, 5 - off, 0);
		if (r <= 0) return false;
		off += (size_t)r;
	}
	uint32_t len = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
	*kind = h[4];
	*n = len - 1;
	for (size_t off = 0; off < *n;) {
		ssize_t r = recv(cli, off < cap ? buf + off : buf, off < cap ? *n - off : 1, 0);
		if (r <= 0) return false;
		off += (size_t)r;
	}
	if (*n < cap) buf[*n] = 0;
	return true;
}

static void send_raw(uint8_t kind, const void *p, size_t n) {
	uint8_t h[5] = {(uint8_t)(n + 1), (uint8_t)((n + 1) >> 8), (uint8_t)((n + 1) >> 16), (uint8_t)((n + 1) >> 24), kind};
	send(cli, h, 5, 0);
	if (n) send(cli, p, n, 0);
}

static void send_u32s(uint8_t kind, const uint32_t *v, size_t n) { send_raw(kind, v, n * 4); }  // little-endian hosts

// expect reads frames until one of kind (pings, logs between).
static bool expect(uint8_t kind, uint8_t *buf, size_t cap) {
	uint8_t k;
	size_t n;
	while (read_frame(&k, buf, cap, &n))
		if (k == kind) return true;
	return false;
}

static atomic_int cli_done;

// cli_side plays asuka dev's part while the main thread runs run().
static void cli_side(void *arg) {
	AK_UNUSED(arg);
	static uint8_t buf[1 << 16];
	// Stop 1, the breakpoint: a ping, a variables request, a step over.
	REQUIRE(expect(AK_DEV_DEBUG_STOPPED, buf, sizeof buf));
	CHECK(strstr((char *)buf, "{\"reason\":\"breakpoint\"") != NULL);
	send_raw(AK_DEV_PING, NULL, 0);
	REQUIRE(expect(AK_DEV_PING, buf, sizeof buf));
	uint32_t req[] = {42, 1, 0, 0, 1, 1};  // seq, frame 1 (run), start, count, n, path: p
	send_u32s(AK_DEV_DEBUG_VARIABLES, req, 6);
	REQUIRE(expect(AK_DEV_DEBUG_VALUES, buf, sizeof buf));
	CHECK(buf[0] == 42 && buf[1] == 0);
	CHECK(strstr((char *)buf + 4, "{\"name\":\"x\",\"type\":\"int\",\"value\":\"1\"}") != NULL);
	uint8_t over = AK_DBG_NEXT;
	send_raw(AK_DEV_DEBUG_COMMAND, &over, 1);
	REQUIRE(expect(AK_DEV_DEBUG_CONTINUED, buf, sizeof buf));
	// Stop 2, the step: back in run. A module arrives: it goes on, and
	// stops nowhere (not at add's breakpoint) until the module's loaded.
	REQUIRE(expect(AK_DEV_DEBUG_STOPPED, buf, sizeof buf));
	CHECK(strstr((char *)buf, "{\"reason\":\"step\",\"totalFrames\":1,\"frames\":[{\"name\":\"run\"") != NULL);
	send_raw(AK_DEV_MODULE, "AKBC!", 5);
	REQUIRE(expect(AK_DEV_DEBUG_CONTINUED, buf, sizeof buf));
	atomic_store(&cli_done, 1);
}

static void count_reload(const uint8_t *p, size_t n, bool fresh) {
	AK_UNUSED(p);
	AK_UNUSED(n);
	AK_UNUSED(fresh);
	CHECK(!ak_dbg_paused());
}

TEST(debug_dev_connection) {
	ak_module *m = load();
	if (!m) return;
	reset();
	int ln = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001)};
	REQUIRE(bind(ln, (struct sockaddr *)&a, sizeof a) == 0);
	socklen_t al = sizeof a;
	getsockname(ln, (struct sockaddr *)&a, &al);
	listen(ln, 4);
	ak_dev_set_reload(count_reload);
	char err[128];
	REQUIRE(ak_dev_connect("127.0.0.1", ntohs(a.sin_port), "{\"platform\":\"test\"}", false, err, sizeof err));
	struct pollfd pf = {.fd = ln, .events = POLLIN};
	REQUIRE(poll(&pf, 1, 10000) == 1);
	cli = accept(ln, NULL, NULL);
	REQUIRE(cli >= 0);
	uint8_t buf[256];
	REQUIRE(expect(AK_DEV_HELLO, buf, sizeof buf));
	// The breakpoint, then a ping: answered once the frame before it was
	// read and handed over, on the same thread.
	// file, n, then line, key, flags, hit op, hit n, codes (none).
	uint8_t bp[4 + sizeof FILE_NAME - 1 + 4 + 18];
	uint32_t flen = sizeof FILE_NAME - 1, one = 1, line = L_RETSUM, zero = 0;
	uint8_t *w = bp;
	memcpy(w, &flen, 4), w += 4;
	memcpy(w, FILE_NAME, flen), w += flen;
	memcpy(w, &one, 4), w += 4;
	memcpy(w, &line, 4), w += 4;
	memcpy(w, &zero, 4), w += 4;
	*w++ = 0, *w++ = 0;
	memcpy(w, &zero, 4), w += 4;
	memcpy(w, &zero, 4), w += 4;
	send_raw(AK_DEV_DEBUG_BREAKPOINTS, bp, (size_t)(w - bp));
	send_raw(AK_DEV_PING, NULL, 0);
	REQUIRE(expect(AK_DEV_PING, buf, sizeof buf));
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 1);

	REQUIRE(ak_thread_start(cli_side, NULL));
	CHECK_EQ_INT(call(m, "run"), 4);
	// What the CLI read last (continued) was sent as run went on: wait
	// for its side to finish.
	for (int i = 0; i < 100000 && !atomic_load(&cli_done); i++) {
		struct timespec ts = {0, 100000};
		nanosleep(&ts, NULL);
	}
	CHECK_EQ_INT(atomic_load(&cli_done), 1);
	// The module, on the main thread: its hold goes.
	ak_sched_run_posted();
	CHECK(!ak_dbg_paused());

	// The CLI goes: its breakpoints go (the connection's thread resets
	// the debugger as it ends: wait for it).
	close(cli);
	for (int i = 0; i < 100000 && atomic_load(&ak_dbg_on); i++) {
		struct timespec ts = {0, 100000};
		nanosleep(&ts, NULL);
	}
	CHECK_EQ_INT(atomic_load(&ak_dbg_on), 0);
	close(ln);
	ak_module_release(m);
}
#endif

T_MAIN()
