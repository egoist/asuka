// The CPU profiler (core/src/vm/prof.c): the sampler thread asks, the
// main thread's interpreter takes the samples at its switch points, and
// what it took says the functions it was in. Its program is
// testdata/modules/prof.asuka, compiled as asuka dev compiles.
#include "t.h"
#include "vm/vm_int.h"
#include "vm/prof.h"
#include "vm/debug.h"
#include "signal/signal.h"

#include <stdio.h>
#include <stdlib.h>

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

static ak_module *load(void) {
	ak_signal_bind_thread();
	ak_vm_init();
	size_t n;
	uint8_t *p = read_module("prof", &n);
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

// The samples it took while the main thread ran spin are in fib, called
// from spin; once it's stopped, none are asked for, and the interpreter
// is back in its own loop.
TEST(samples_the_main_thread) {
	ak_module *m = load();
	REQUIRE(m);
	const ak_func *spin = ak_module_find(m, "spin");
	REQUIRE(spin);
	REQUIRE(ak_prof_available());
	REQUIRE(ak_prof_start(100000));  // 100 µs
	// Until it has taken some: the sampler's thread runs when it runs.
	for (int i = 0; i < 20000 && ak_prof_samples() < 20; i++) {
		ak_val v = ak_vm_call(spin, NULL, 0);
		CHECK_EQ_INT(v.i, 2584);
	}
	ak_prof_section(AK_PROF_IDLE);
	ak_prof_stop();
	CHECK(ak_prof_samples() >= 20);
	CHECK_EQ_INT(atomic_load(&ak_dbg_on) & AK_DBG_ON_PROF, 0);
	ak_buf b = {0};
	ak_prof_write(&b, 0);
	ak_buf_u8(&b, 0);
	const char *j = (const char *)b.data;
	CHECK(strstr(j, "\"interval\":100000") != NULL);
	CHECK(strstr(j, "[\"fib\",\"prof.asuka\",4]") != NULL);
	CHECK(strstr(j, "[\"spin\",\"prof.asuka\",11]") != NULL);
	CHECK(strstr(j, "\"nodes\":[[-1,-1],") != NULL);
	ak_buf_free(&b);
	// Stopped: nothing more is sampled.
	uint32_t n = ak_prof_samples();
	for (int i = 0; i < 20; i++) ak_vm_call(spin, NULL, 0);
	CHECK_EQ_INT(ak_prof_samples(), n);
	ak_prof_reset();
	CHECK_EQ_INT(ak_prof_samples(), 0);
	ak_module_release(m);
}

// The time due as the main thread changes what it does is that section's.
TEST(sections_take_their_time) {
	REQUIRE(ak_prof_start(100000));
	ak_prof_section(AK_PROF_LAYOUT);
	// A tick before the switch was the app's (start's section): its time
	// went to it as the section changed, and its flag mustn't end the
	// wait. What's due from here on is the layout's.
	atomic_fetch_and(&ak_dbg_on, ~AK_DBG_ON_PROF);
	// Busy outside the app's code until the sampler said some was due.
	for (int i = 0; i < 2000000 && !(atomic_load(&ak_dbg_on) & AK_DBG_ON_PROF); i++) {
		volatile int x = 0;
		for (int k = 0; k < 1000; k++) x += k;
	}
	ak_prof_section(AK_PROF_IDLE);
	ak_prof_stop();
	CHECK(ak_prof_samples() >= 1);
	ak_buf b = {0};
	ak_prof_write(&b, 0);
	ak_buf_u8(&b, 0);
	CHECK(strstr((const char *)b.data, "[\"(layout)\",\"\",0]") != NULL);
	ak_buf_free(&b);
	ak_prof_reset();
}

T_MAIN()
