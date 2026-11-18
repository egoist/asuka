// trace.c: where an error was thrown (trace.h). Tracing costs a walk of
// the VM's stack per error thrown, so it's on only where a report reads
// it: a program, asuka dev's player.
#include "abi/trace.h"
#include "base/thread.h"
#include "rt/rt.h"
#include "vm/vm.h"

#include <stdio.h>
#include <string.h>

static struct {
	void *traced;  // the last error thrown (retained)
	char vm[4096];
	char c[4096];  // compiled functions' frames, innermost first
	uint64_t thread;
} T;

static void on_throw(void *err) {
	// Thrown again (a rethrow, a frame it unwinds through): it keeps
	// where it was first thrown.
	if (err == T.traced || ak_thread_id() != T.thread || ak_is_cancel(err)) return;
	if (T.traced) ak_release(T.traced);
	T.traced = ak_retain(err);
	ak_vm_traceback(T.vm, sizeof T.vm);
	T.c[0] = 0;
}

static void on_frame(const char *fn, const char *file, uint32_t line) {
	if (ak_thread_id() != T.thread || !T.traced || !ak_error_pending()) return;
	size_t n = strlen(T.c);
	if (n + 2 < sizeof T.c) snprintf(T.c + n, sizeof T.c - n, "%s (%s:%u)\n", fn, file, line);
}

void ak_trace_start(void) {
	T.thread = ak_thread_id();
	ak_throw_hook = on_throw;
	ak_frame_hook = on_frame;
}

void ak_trace_stop(void) {
	ak_throw_hook = NULL;
	ak_frame_hook = NULL;
	if (T.traced) ak_release(T.traced);
	T.traced = NULL;
}

void ak_trace_of(const void *err, const char **c, const char **vm) {
	bool mine = err && err == T.traced;
	*c = mine ? T.c : "";
	*vm = mine ? T.vm : "";
}
