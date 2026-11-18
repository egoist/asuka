// web.c: the core as WebAssembly, for the web host (platform/web). The
// JavaScript host is reached through imports (module "asuka"); it calls
// in through the C ABI (asuka.h), exported. Batches and measurements go
// through a small shared buffer (ak_web_io) the host reads in place, as
// the Go web host did: one crossing per batch, nothing allocated per
// measure.
//
// spawn runs on the page's Web Workers (platform/web/src/spawn.js), each
// an instance of this same module with the same app: the page's instance
// sends a job (the closure's function, by its index in the module, and
// its captures, which are values: vm/keep.c's encoding, which names its
// types), a worker's instance runs it (asuka_worker_start ran the
// module's init without a UI first) and answers its result or error in
// the same encoding, and the page settles the spawn's future.
//
// A worker (rt/worker.c) is a Web Worker of its own (platform/web/src/
// workers.js), the same core with the same app: its init's closure
// (worker_start) makes its state there, kept for the calls that follow
// (worker_call: a job as spawn's, whose function takes the state), each
// answered as spawn's are (asuka_spawn_done), a call that's cancelled
// before it ran with ANSWER_CANCELLED.
//
// Only compiled into wasm builds.
#ifdef __wasm__

#include "asuka.h"
#include "rt/sched.h"
#include "rt/lib.h"
#include "rt/worker.h"
#include "base/thread.h"
#include "std/tz.h"
#include "vm/vm_int.h"
#include "vm/keep.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMPORT(name) __attribute__((import_module("asuka"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name), visibility("default")))

IMPORT("apply") void js_apply(void);
IMPORT("measure") void js_measure(void);
IMPORT("post") void js_post(void);
IMPORT("timer_set") void js_timer_set(double id, double ms);
IMPORT("timer_clear") void js_timer_clear(double id);
IMPORT("zone_offset") double js_zone_offset(double unix_seconds);
IMPORT("zone_name") uint32_t js_zone_name(char *out, uint32_t cap);
IMPORT("spawn") void js_spawn(double id, const uint8_t *job, uint32_t len);
IMPORT("spawn_cancel") void js_spawn_cancel(double id);
IMPORT("worker_start") void js_worker_start(double id, const uint8_t *job, uint32_t len);
IMPORT("worker_call") void js_worker_call(double id, double job, const uint8_t *bytes, uint32_t len);
IMPORT("worker_stop") void js_worker_stop(double id, int cancel);
// Typed native modules (native packages' JavaScript, platform/web's
// native.js): a call's answer is its length, then native_answer copies it
// to where the core puts it.
IMPORT("native_new") uint32_t js_native_new(const uint8_t *cls, uint32_t len, uint32_t iface);
IMPORT("native_call") uint32_t js_native_call(uint64_t handle, uint32_t method, const uint8_t *in, uint32_t len);
IMPORT("native_answer") void js_native_answer(uint8_t *dst);
IMPORT("native_start") void js_native_start(uint64_t handle, uint32_t method, const uint8_t *in, uint32_t len, uint64_t call);
IMPORT("native_cancel") void js_native_cancel(uint64_t call);
IMPORT("native_release") void js_native_release(uint64_t handle);

// io: Apply's batch address and length (u32 0, 1); Measure's node (u32
// 2) and its constraints, then its answer (f32 3..6).
static union {
	uint32_t u32[8];
	float f32[8];
} io;

static void apply(const uint8_t *buf, size_t len) {
	io.u32[0] = (uint32_t)(uintptr_t)buf;
	io.u32[1] = (uint32_t)len;
	js_apply();
}

static void measure(uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	io.u32[2] = id;
	io.f32[3] = w;
	io.f32[4] = h;
	io.f32[5] = aw;
	io.f32[6] = ah;
	js_measure();
	*ow = io.f32[3];
	*oh = io.f32[4];
}

static void post(void) { js_post(); }

static void timer_schedule(uint64_t id, int64_t ns) { js_timer_set((double)id, (double)ns / 1e6); }
static void timer_cancel(uint64_t id) { js_timer_clear((double)id); }

// The browser's zone, for dates (ak_zone_offset), and its name
// (std/time's, ak_tz_local_name).
static int32_t zone_offset(int64_t t) { return (int32_t)js_zone_offset((double)t); }
static size_t zone_name(char *out, size_t cap) { return js_zone_name(out, (uint32_t)cap); }

// A native answer of n bytes: into out when it fits, else into *big
// (native_free's).
static size_t native_answer(uint32_t n, uint8_t *out, size_t cap, uint8_t **big) {
	uint8_t *dst = out;
	if (n > cap) dst = *big = ak_alloc(n);
	js_native_answer(dst);
	return n;
}

static size_t native_new(const uint8_t *cls, size_t len, uint32_t iface, uint8_t *out, size_t cap, uint8_t **big) {
	return native_answer(js_native_new(cls, (uint32_t)len, iface), out, cap, big);
}

static size_t native_call(uint64_t h, uint32_t method, const uint8_t *in, size_t len, uint8_t *out, size_t cap, uint8_t **big) {
	return native_answer(js_native_call(h, method, in, (uint32_t)len), out, cap, big);
}

static void native_start(uint64_t h, uint32_t method, const uint8_t *in, size_t len, uint64_t call) {
	js_native_start(h, method, in, (uint32_t)len, call);
}

static void native_cancel(uint64_t call) { js_native_cancel(call); }
static void native_release(uint64_t h) { js_native_release(h); }
static void native_free(void *p) { ak_free(p); }

static asuka_host_callbacks callbacks = {
	.apply = apply,
	.measure = measure,
	.post = post,
	.native_new = native_new,
	.native_call = native_call,
	.native_start = native_start,
	.native_cancel = native_cancel,
	.native_release = native_release,
	.native_free = native_free,
};

EXPORT("asuka_web_start") void asuka_web_start(void) {
	ak_sched_set_timer(timer_schedule, timer_cancel);
	ak_zone_offset_hook = zone_offset;
	ak_tz_local_name_hook = zone_name;
	asuka_start(&callbacks);
}

EXPORT("asuka_io") uint32_t asuka_io(void) { return (uint32_t)(uintptr_t)&io; }
EXPORT("asuka_timer_fire") void asuka_timer_fire(double id) { ak_timer_fire((uint64_t)id); }

// ---- spawn, on the page's workers ----
//
// A job: u32 the function's index, u8 the result's kind, u32 the
// captures' count, then each capture (ak_keep_encode, by the function's
// capture kinds). An answer: the result (of the job's kind; nothing for
// none), the error (a reference), or, when the worker couldn't run it,
// a message.

enum { ANSWER_OK, ANSWER_THREW, ANSWER_FAILED, ANSWER_CANCELLED };

int ak_abi_init_only(ak_module *m, char **err);
ak_module *ak_abi_module(void);
void ak_vm_closure_drop(ak_closure *c);

// A shape by its name: the module's, or the runtime's own Error.
static const ak_shape *find_shape(void *ctx, const char *name) {
	const ak_shape *s = ctx ? ak_module_shape_named(ctx, name) : NULL;
	if (!s && strcmp(name, ak_shape_error.name) == 0) s = &ak_shape_error;
	return s;
}

// The page's side: the jobs out, by ID.
typedef struct remote {
	uint64_t id;
	ak_future *fut;  // retained
	ak_module *mod;  // retained: the shapes the answer is read with
	struct remote *next;
} remote;

static remote *remotes;
static uint64_t last_job;
static bool workers;  // the page runs spawn on workers (asuka_web_workers)

static remote *take_remote(uint64_t id) {
	for (remote **p = &remotes; *p; p = &(*p)->next) {
		if ((*p)->id == id) {
			remote *j = *p;
			*p = j->next;
			return j;
		}
	}
	return NULL;
}

static void drop_remote(remote *j) {
	ak_future *f = j->fut;
	f->cancel = NULL;
	f->ctx = NULL;
	ak_release(f);
	ak_module_release(j->mod);
	ak_free(j);
	ak_sched_unhold();
}

// A cancelled spawn: one not started yet never starts, one running runs
// to its end and its answer is dropped; the future fails now.
static void cancel_remote(ak_future *f) {
	remote *j = f->ctx ? take_remote(((remote *)f->ctx)->id) : NULL;
	if (!j) return;
	js_spawn_cancel((double)j->id);
	ak_throw_cancel();
	ak_future_reject(f, ak_error_take());
	drop_remote(j);
}

// encode_job is c as a job of kind k into b: false when it can't go (not
// the VM's, or what it captures isn't a value, which the checker keeps
// out).
static bool encode_job(ak_buf *b, ak_closure *c, ak_kind k) {
	if (c->invoke != ak_vm_closure_invoke) return false;
	const ak_func *fn = c->fn;
	ak_buf_u32(b, fn->index);
	ak_buf_u8(b, (uint8_t)k);
	ak_buf_u32(b, c->ncap);
	for (uint16_t i = 0; i < c->ncap; i++) {
		if (!ak_keep_encode(b, c->caps[i], c->capkinds ? (ak_kind)c->capkinds[i] : AK_K_INT)) {
			ak_buf_free(b);
			return false;
		}
	}
	return true;
}

// remote_new is a job out, settling f with its answer.
static remote *remote_new(ak_future *f, ak_module *mod) {
	remote *j = ak_alloc(sizeof *j);
	j->id = ++last_job;
	j->fut = ak_retain(f);
	j->mod = mod;
	ak_module_retain(j->mod);
	j->next = remotes;
	remotes = j;
	ak_sched_hold();  // until it's answered or cancelled
	return j;
}

static bool spawn_on_worker(ak_closure *c, ak_kind k, ak_future *f) {
	ak_buf b = {0};
	if (!workers || !encode_job(&b, c, k)) return false;  // run here
	remote *j = remote_new(f, ((const ak_func *)c->fn)->mod);
	f->ctx = j;
	f->cancel = cancel_remote;
	js_spawn((double)j->id, b.data, (uint32_t)b.len);
	ak_buf_free(&b);
	return true;
}

// ---- workers, each on a Web Worker of its own ----

static uint64_t last_worker;

static bool worker_start(ak_closure *init, uint64_t *id) {
	ak_buf b = {0};
	if (!encode_job(&b, init, AK_K_REF)) return false;  // in place
	*id = ++last_worker;
	js_worker_start((double)*id, b.data, (uint32_t)b.len);
	ak_buf_free(&b);
	return true;
}

static void worker_call(uint64_t id, ak_closure *c, ak_kind k, ak_future *f) {
	ak_buf b = {0};
	if (!encode_job(&b, c, k)) {
		ak_throw_msg("worker: a call that can't cross to its Web Worker");
		ak_future_reject(f, ak_error_take());
		return;
	}
	remote *j = remote_new(f, ((const ak_func *)c->fn)->mod);
	js_worker_call((double)id, (double)j->id, b.data, (uint32_t)b.len);
	ak_buf_free(&b);
}

static void worker_stop(uint64_t id, bool cancel) { js_worker_stop((double)id, cancel); }

static const ak_worker_host web_workers = {worker_start, worker_call, worker_stop};

// asuka_web_workers says the page has workers for spawn and workers (0:
// spawn runs in place, on the page's thread, and so do workers).
EXPORT("asuka_web_workers") void asuka_web_workers(int on) {
	workers = on != 0;
	ak_spawn_elsewhere = workers ? spawn_on_worker : NULL;
	ak_worker_elsewhere = workers ? &web_workers : NULL;
}

// asuka_spawn_done settles job id with a worker's answer (status: an
// ANSWER_*), then runs what waited for it. A cancelled job's is dropped.
EXPORT("asuka_spawn_done") void asuka_spawn_done(double id, int status, const uint8_t *p, size_t n) {
	remote *j = take_remote((uint64_t)id);
	if (!j) return;
	ak_future *f = j->fut;
	ak_kind k = (ak_kind)f->kind;
	ak_reader r = {p, n, 0, false};
	ak_val v = {0};
	f->cancel = NULL;
	if (status == ANSWER_OK && (k == AK_K_VOID || ak_keep_decode(&r, k, find_shape, j->mod, &v))) {
		ak_future_resolve(f, k == AK_K_VOID ? (ak_val){0} : v);
	} else if (status == ANSWER_THREW && ak_keep_decode(&r, AK_K_REF, find_shape, j->mod, &v) && v.p) {
		ak_future_reject(f, v.p);
	} else if (status == ANSWER_CANCELLED) {
		// A worker's call, queued as its owner went.
		ak_throw_cancel();
		ak_future_reject(f, ak_error_take());
	} else {
		if (status == ANSWER_FAILED) ak_throw_msg("spawn: %.*s", (int)(n < 400 ? n : 400), (const char *)p);
		else ak_throw_msg("spawn: the worker's answer doesn't fit the app");
		ak_future_reject(f, ak_error_take());
	}
	drop_remote(j);
	ak_ui_run_posted();
}

// The worker's side: a core without a UI. What the module's init does
// for one (an accent, a language) goes nowhere.
static void worker_apply(const uint8_t *buf, size_t len) { AK_UNUSED(buf), AK_UNUSED(len); }

static void worker_measure(uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(id), AK_UNUSED(w), AK_UNUSED(h), AK_UNUSED(aw), AK_UNUSED(ah);
	*ow = 0;
	*oh = 0;
}

static asuka_host_callbacks worker_callbacks = {
	.apply = worker_apply,
	.measure = worker_measure,
	.post = post,
};

static ak_buf answer;

// answered points io at the answer (u32 0, 1) and returns status.
static int answered(int status) {
	io.u32[0] = (uint32_t)(uintptr_t)answer.data;
	io.u32[1] = (uint32_t)answer.len;
	return status;
}

static int worker_failed(const char *msg) {
	answer.len = 0;
	ak_buf_put(&answer, msg, strlen(msg));
	return answered(ANSWER_FAILED);
}

// asuka_worker_start makes this instance a worker's: the core without a
// UI, and the app's module (bytecode, or with none the app compiled into
// the build) with its init run. 0, or -1 with a message in io.
extern bool ak_in_spawn_worker;

EXPORT("asuka_worker_start") int asuka_worker_start(const uint8_t *module, size_t n) {
	ak_in_spawn_worker = true;
	ak_sched_set_timer(timer_schedule, timer_cancel);
	ak_zone_offset_hook = zone_offset;
	ak_tz_local_name_hook = zone_name;
	asuka_start(&worker_callbacks);
	ak_module *m = NULL;
	char msg[512];
	if (n) {
		m = ak_module_load(module, n, msg, sizeof msg);
		if (!m) {
			worker_failed(msg);
			return -1;
		}
	}
	char *err = NULL;
	if (ak_abi_init_only(m, &err) != 0) {
		snprintf(msg, sizeof msg, "%s", err ? err : "asuka: the module's init failed");
		free(err);
		worker_failed(msg);
		return -1;
	}
	ak_ui_run_posted();
	return 0;
}

// asuka_worker_call runs a job: its answer is in io (address, length)
// until the next call; the result an ANSWER_*.
// decode_job is the closure a job runs (and its result's kind), or NULL
// with why in *why.
static ak_closure *decode_job(const uint8_t *p, size_t n, ak_kind *k, const char **why) {
	ak_module *m = ak_abi_module();
	ak_reader r = {p, n, 0, false};
	uint32_t fi = ak_read_u32(&r);
	*k = (ak_kind)ak_read_u8(&r);
	uint32_t ncap = ak_read_u32(&r);
	if (r.err || !m || fi >= m->nfuncs || m->funcs[fi].ncap != ncap) {
		*why = "the job isn't one of this app's (a module of another version?)";
		return NULL;
	}
	const ak_func *fn = &m->funcs[fi];
	ak_closure *c = ak_closure_new(ak_vm_closure_invoke, fn, fn->ncap, fn->capkinds);
	ak_module_retain(fn->mod);
	c->drop = ak_vm_closure_drop;
	for (uint32_t i = 0; i < ncap; i++) {
		if (!ak_keep_decode(&r, (ak_kind)fn->capkinds[i], find_shape, m, &c->caps[i])) {
			ak_release(c);
			*why = "its captures don't fit the app (a module of another version?)";
			return NULL;
		}
	}
	return c;
}

// threw is the answer of a job that threw e (taken).
static int threw(void *e) {
	answer.len = 0;
	if (!ak_keep_encode(&answer, (ak_val){.p = e}, AK_K_REF)) {
		// An error that isn't a value: its message, as an Error.
		answer.len = 0;
		ak_throw_msg("%s", ak_error_message(e)->data);
		void *plain = ak_error_take();
		ak_keep_encode(&answer, (ak_val){.p = plain}, AK_K_REF);
		ak_release(plain);
	}
	ak_release(e);
	return ANSWER_THREW;
}

// A worker's instance (asuka_worker_new): its state, which each job
// takes, or the error its init threw, which each job answers.
static bool is_worker;
static ak_val worker_env;
static void *worker_err;

// asuka_worker_new makes this instance a worker's: its init's job, run
// now, makes the state the jobs from now on take. ANSWER_OK, or what
// asuka_worker_call would answer for its error.
EXPORT("asuka_worker_new") int asuka_worker_new(const uint8_t *p, size_t n) {
	answer.len = 0;
	is_worker = true;
	ak_kind k;
	const char *why = NULL;
	ak_closure *c = decode_job(p, n, &k, &why);
	if (!c) {
		ak_throw_msg("worker: %s", why);
		worker_err = ak_error_take();
		return ANSWER_FAILED;
	}
	worker_env = ak_call(c, NULL, 0);
	if (ak_error_pending()) worker_err = ak_error_take();
	ak_release(c);
	ak_ui_run_posted();
	return worker_err ? ANSWER_THREW : ANSWER_OK;
}

EXPORT("asuka_worker_call") int asuka_worker_call(const uint8_t *p, size_t n) {
	answer.len = 0;
	ak_kind k;
	const char *why = NULL;
	ak_closure *c = decode_job(p, n, &k, &why);
	if (!c) return worker_failed(why);
	if (is_worker && worker_err) {
		// Its init threw: each call fails with that.
		ak_release(c);
		return answered(threw(ak_retain(worker_err)));
	}
	ak_val v = is_worker ? ak_call(c, &worker_env, 1) : ak_call(c, NULL, 0);
	int status = ANSWER_OK;
	if (ak_error_pending()) {
		status = threw(ak_error_take());
	} else {
		if (k != AK_K_VOID && !ak_keep_encode(&answer, v, k)) {
			ak_release_val(v, k);
			ak_release(c);
			return worker_failed("its result isn't a value");
		}
		ak_release_val(v, k);
	}
	ak_release(c);
	ak_ui_run_posted();
	return answered(status);
}

// asuka_worker_answered lets a large answer's memory go once the page
// has it.
EXPORT("asuka_worker_answered") void asuka_worker_answered(void) {
	if (answer.cap > (1u << 22)) ak_buf_free(&answer);
}

#endif
