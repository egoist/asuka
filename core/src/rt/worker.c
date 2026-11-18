// worker.c: a worker (RFC 0011; lang/LOWERING.md's "Workers"), a value
// on a thread of its own. Each has its thread and a queue of calls: what
// spawn's pool does for one closure (spawn.c), kept alive.
//
// worker.new(init) starts it: init, a closure whose captures are the
// worker's arguments (deep-copied over, as spawn's are), runs first on
// the thread and makes the worker's state (its env: its vars' boxes, its
// lets). worker.call(w, c, kind) queues a call: c, a closure whose
// captures are the call's arguments (deep-copied over) and whose one
// parameter is the state, runs on the thread in the order calls were
// made; its result (or error) is deep-copied back, and the main thread
// settles the call's future (ak_post). The state is made, used and freed
// on the thread only, so its reference counts stay plain ones.
//
// Stopping. A worker belongs to the owner it was made under (a
// component, a store's maker): disposing that stops it at once: the call
// running finishes, those queued are cancelled (their futures fail with
// the cancellation, which callers see as their task's, not an error). A
// worker whose last reference goes stops once its queued calls ran (a
// worker made where no owner is, in a test or a program). Either way the
// thread lets go of its state and ends; the scheduler is held until it
// has (ak_sched_hold), so a test's loop sees everything freed.
//
// What's whose. A closure's copy, its captures deep-copied, is the
// thread's: what it runs stores them in the state (an argument kept in a
// var), so it's let go of on the thread, as the state is, and the
// reference counts the two share are only ever the thread's. The closure
// it was copied from is the main thread's, and holds the module (the
// VM's code; its copy does too: ak_closure_copied): the worker keeps it
// until the work is done, the init's until the thread has ended and each
// call's until its result is delivered (the result may hold the module's
// constants), and lets it go on the main thread, so a dev reload never
// frees a module a worker still runs, and frees it where the views and
// state that read its constants live.
//
// Where the core has no threads (the web), a host may run workers
// elsewhere (ak_worker_elsewhere: a Web Worker each, abi/web.c); without
// one, a worker runs in place: its state on the main thread, each call
// run as it's made.
#include "rt/sched.h"
#include "rt/lib.h"
#include "rt/worker.h"
#include "base/thread.h"
#include "signal/signal.h"

#include <stdatomic.h>

typedef struct wcall {
	ak_closure *c;     // the copy the thread runs, and lets go of
	ak_closure *orig;  // what it was copied from: the main thread's, retained
	ak_future *fut;    // retained; touched on the main thread only
	ak_kind kind;
	ak_val result;
	void *err;
	struct wcall *next;
} wcall;

typedef struct wstate {
	// Its holders: the handle, the thread (until it ends) and the owner's
	// cleanup (until it runs). Any of them may let go last.
	atomic_int rc;
	ak_mutex mu;
	ak_cond cond;
	wcall *head, *tail;  // under mu
	bool stop;           // under mu: no more calls; the thread ends once head is
	bool stopping;       // main thread: a stop was asked for (and the scheduler held)
	ak_closure *init;    // the copy the thread makes the state with (and lets go of)
	ak_closure *orig;    // what it was copied from: the main thread's, retained
	// In place: its state, on the main thread, or the error its init
	// threw.
	bool inplace;
	ak_val env;
	void *err;
	// Elsewhere (the web): the host's id for it.
	bool remote;
	uint64_t id;
} wstate;

typedef struct ak_worker {
	ak_opaque h;
	wstate *s;
} ak_worker;

const ak_worker_host *ak_worker_elsewhere;

static void state_release(wstate *s) {
	if (atomic_fetch_sub(&s->rc, 1) != 1) return;
	// In place, its state is the main thread's (where the last holder
	// lets go: the handle or the cleanup).
	if (s->inplace) {
		ak_release(s->env.p);
		ak_release(s->err);
	}
	if (!s->remote) {
		ak_mutex_destroy(&s->mu);
		ak_cond_destroy(&s->cond);
	}
	ak_free(s);
}

// ---- The thread ----

static void deliver(void *p) {
	wcall *c = p;
	if (c->err) ak_future_reject(c->fut, c->err);
	else ak_future_resolve(c->fut, c->result);
	ak_release(c->fut);
	ak_release(c->orig);
	ak_free(c);
	ak_sched_unhold();
}

// stopped: the thread has let go of the state (main thread). The init
// goes here, last: it holds the module the state was made with.
static void stopped(void *p) {
	wstate *s = p;
	ak_release(s->orig);
	s->orig = NULL;
	state_release(s);
	ak_sched_unhold();
}

// copy_error is err deep-copied for the main thread (a reference).
static void *copy_error(void *err) { return ak_deep_copy((ak_val){.p = err}, AK_K_REF).p; }

static void run(void *p) {
	wstate *s = p;
	ak_val env = ak_call(s->init, NULL, 0);
	void *err = ak_error_pending() ? ak_error_take() : NULL;
	for (;;) {
		ak_mutex_lock(&s->mu);
		while (!s->head && !s->stop) ak_cond_wait(&s->cond, &s->mu);
		wcall *c = s->head;
		if (c) {
			s->head = c->next;
			if (!s->head) s->tail = NULL;
		}
		ak_mutex_unlock(&s->mu);
		if (!c) break;  // stopped, and nothing queued
		if (err) {
			// Its init threw: each call fails with that.
			c->err = copy_error(err);
		} else {
			ak_val v = ak_call(c->c, &env, 1);
			if (ak_error_pending()) {
				void *e = ak_error_take();
				c->err = copy_error(e);
				ak_release(e);
			} else {
				c->result = ak_deep_copy(v, c->kind);
				ak_release_val(v, c->kind);
			}
		}
		ak_release(c->c);
		c->c = NULL;
		ak_post(deliver, c);
	}
	ak_release(env.p);
	ak_release(err);
	ak_release(s->init);
	s->init = NULL;
	extern void ak_vm_thread_end(void);
	ak_vm_thread_end();  // the VM's registers and frames on this thread
	ak_post(stopped, s);
}

// ---- The main thread's side ----

// stop asks the worker to stop; cancel cancels the calls queued.
static void stop(wstate *s, bool cancel) {
	if (s->inplace) {
		s->stopping = true;
		return;
	}
	if (s->remote) {
		if (!s->stopping) {
			s->stopping = true;
			ak_worker_elsewhere->stop(s->id, cancel);
		} else if (cancel) {
			ak_worker_elsewhere->stop(s->id, true);
		}
		return;
	}
	wcall *queued = NULL;
	ak_mutex_lock(&s->mu);
	if (cancel) {
		queued = s->head;
		s->head = s->tail = NULL;
	}
	s->stop = true;
	ak_cond_signal(&s->cond);
	ak_mutex_unlock(&s->mu);
	if (!s->stopping) {
		s->stopping = true;
		ak_sched_hold();  // until the thread has let go of the state (stopped)
	}
	while (queued) {
		wcall *c = queued;
		queued = c->next;
		ak_throw_cancel();
		ak_future_reject(c->fut, ak_error_take());
		ak_release(c->fut);
		ak_release(c->c);  // never the thread's: it's the main thread's to let go of
		ak_release(c->orig);
		ak_free(c);
		ak_sched_unhold();
	}
}

// The owner that made it was disposed: it stops at once.
static void disposed(void *p) {
	wstate *s = p;
	stop(s, true);
	state_release(s);
}

static void handle_drop(ak_opaque *o) {
	ak_worker *w = (ak_worker *)o;
	stop(w->s, false);
	state_release(w->s);
}

static wstate *state_of(ak_val v) {
	ak_worker *w = v.p;
	if (!w || w->h.h.tag != AK_TAG_OPAQUE || w->h.drop != handle_drop) ak_panic("worker: not a worker");
	return w->s;
}

// worker.new(init): a worker, its thread started with init, which makes
// its state.
static ak_val new_fn(ak_val *args, uint32_t nargs) {
	AK_UNUSED(nargs);
	ak_closure *init = args[0].p;
	wstate *s = ak_alloc(sizeof *s);
	memset(s, 0, sizeof *s);
	atomic_init(&s->rc, 1);
	ak_worker *w = ak_opaque_new(AK_TAG_OPAQUE, sizeof *w, handle_drop);
	w->s = s;
	if (ak_worker_elsewhere && ak_worker_elsewhere->start(init, &s->id)) {
		s->remote = true;
	} else {
		ak_mutex_init(&s->mu);
		ak_cond_init(&s->cond);
		s->init = ak_closure_deep_copy(init);
		s->orig = ak_retain(init);
		atomic_fetch_add(&s->rc, 1);  // the thread's
		if (!ak_thread_start(run, s)) {
			// No threads: in place, on the main thread.
			atomic_fetch_sub(&s->rc, 1);
			s->inplace = true;
			ak_release(s->init);
			ak_release(s->orig);
			s->init = s->orig = NULL;
			s->env = ak_call(init, NULL, 0);
			if (ak_error_pending()) s->err = ak_error_take();
		}
	}
	if (ak_current_owner()) {
		atomic_fetch_add(&s->rc, 1);  // the cleanup's
		ak_on_cleanup(disposed, s);
	}
	return (ak_val){.p = w};
}

// worker.call(w, c, kind): a future of c's result (of kind), c run with
// the worker's state on its thread, after the calls made before it.
static ak_val call_fn(ak_val *args, uint32_t nargs) {
	wstate *s = state_of(args[0]);
	ak_closure *c = args[1].p;
	ak_kind k = nargs > 2 ? (ak_kind)args[2].i : AK_K_VOID;
	ak_future *f = ak_future_new(k);
	if (s->stopping) {
		// Its owner went: nothing more runs.
		ak_throw_cancel();
		ak_future_reject(f, ak_error_take());
		return (ak_val){.p = f};
	}
	if (s->remote) {
		ak_worker_elsewhere->call(s->id, c, k, f);
		return (ak_val){.p = f};
	}
	if (s->inplace) {
		if (s->err) {
			ak_future_reject(f, ak_retain(s->err));
			return (ak_val){.p = f};
		}
		ak_val v = ak_call(c, &s->env, 1);
		if (ak_error_pending()) ak_future_reject(f, ak_error_take());
		else ak_future_resolve(f, v);
		return (ak_val){.p = f};
	}
	wcall *wc = ak_alloc(sizeof *wc);
	memset(wc, 0, sizeof *wc);
	wc->c = ak_closure_deep_copy(c);
	wc->orig = ak_retain(c);
	wc->fut = ak_retain(f);
	f->held = 1;  // the call's reference, until it's delivered
	wc->kind = k;
	ak_sched_hold();  // until deliver runs, or the call is cancelled
	ak_mutex_lock(&s->mu);
	if (s->tail) s->tail->next = wc;
	else s->head = wc;
	s->tail = wc;
	ak_cond_signal(&s->cond);
	ak_mutex_unlock(&s->mu);
	return (ak_val){.p = f};
}

const ak_lib_entry ak_worker_lib[] = {
	{"worker.new", new_fn},
	{"worker.call", call_fn},
	{NULL, NULL},
};
