#include "rt/sched.h"
#include "base/thread.h"

// ---- The ready queue: a ring of callbacks ----

typedef struct { ak_cb cb; void *ctx; } item;

static struct {
	item *items;
	uint32_t head, len, cap;
} ready;

void ak_ready(ak_cb cb, void *ctx) {
	if (ready.len == ready.cap) {
		uint32_t c = ready.cap ? ready.cap * 2 : 64;
		item *n = ak_alloc(sizeof(item) * c);
		for (uint32_t i = 0; i < ready.len; i++) n[i] = ready.items[(ready.head + i) % ready.cap];
		ak_free(ready.items);
		ready.items = n;
		ready.head = 0;
		ready.cap = c;
	}
	ready.items[(ready.head + ready.len) % ready.cap] = (item){cb, ctx};
	ready.len++;
}

bool ak_sched_has_ready(void) { return ready.len > 0; }

static void arm_trim(void);

bool ak_sched_run(void) {
	bool ran = false;
	while (ready.len) {
		item it = ready.items[ready.head];
		ready.head = (ready.head + 1) % ready.cap;
		ready.len--;
		it.cb(it.ctx);
		ran = true;
	}
	if (ran) arm_trim();
	return ran;
}

// ---- Posted callbacks, from any thread ----

static ak_mutex post_mu = AK_MUTEX_INIT;
static AK_VEC(item) posted, running;
static void (*wake_host)(void);
static ak_cond post_cond;
static bool post_cond_ready;
static int holds;  // under post_mu

void ak_sched_hold(void) {
	ak_mutex_lock(&post_mu);
	holds++;
	ak_mutex_unlock(&post_mu);
}

void ak_sched_unhold(void) {
	ak_mutex_lock(&post_mu);
	holds--;
	if (post_cond_ready) ak_cond_broadcast(&post_cond);
	ak_mutex_unlock(&post_mu);
}

bool ak_sched_held(void) {
	ak_mutex_lock(&post_mu);
	bool h = holds > 0;
	ak_mutex_unlock(&post_mu);
	return h;
}

void ak_sched_wait_posted(void) {
	ak_mutex_lock(&post_mu);
	if (!post_cond_ready) {
		ak_cond_init(&post_cond);
		post_cond_ready = true;
	}
	while (posted.len == 0 && holds > 0) ak_cond_wait(&post_cond, &post_mu);
	ak_mutex_unlock(&post_mu);
}

void ak_sched_set_wake(void (*wake)(void)) { wake_host = wake; }

void ak_sched_wake(void) {
	ak_mutex_lock(&post_mu);
	void (*w)(void) = wake_host;
	ak_mutex_unlock(&post_mu);
	if (w) w();
}

void ak_post(ak_cb cb, void *ctx) {
	ak_mutex_lock(&post_mu);
	bool first = posted.len == 0;
	ak_vec_push(&posted, ((item){cb, ctx}));
	if (post_cond_ready) ak_cond_broadcast(&post_cond);
	void (*w)(void) = wake_host;
	ak_mutex_unlock(&post_mu);
	if (first && w) w();
}

// run_nested runs what's posted when a posted callback had the host run
// them again (a modal dialog's loop, which handles the host's wake): the
// outer run is going through running, so these are taken whole into a
// vector of their own.
static void run_nested(void) {
	for (;;) {
		ak_mutex_lock(&post_mu);
		AK_VEC(item) t = {posted.data, posted.len, posted.cap};
		posted.data = NULL;
		posted.len = posted.cap = 0;
		ak_mutex_unlock(&post_mu);
		for (uint32_t i = 0; i < t.len; i++) t.data[i].cb(t.data[i].ctx);
		bool done = t.len == 0;
		ak_mutex_lock(&post_mu);
		if (!posted.data) {  // its buffer back, for the next posts
			posted.data = t.data;
			posted.cap = t.cap;
			t.data = NULL;
		}
		ak_mutex_unlock(&post_mu);
		ak_vec_free(&t);
		if (done) return;
	}
}

static int running_posted;  // main thread

void ak_sched_run_posted(void) {
	if (running_posted) {
		run_nested();
		ak_sched_run();
		return;
	}
	running_posted++;
	for (;;) {
		ak_mutex_lock(&post_mu);
		// Swap the queues so callbacks can post again without deadlock.
		AK_VEC(item) t = {posted.data, posted.len, posted.cap};
		posted.data = running.data;
		posted.len = 0;
		posted.cap = running.cap;
		running.data = t.data;
		running.len = t.len;
		running.cap = t.cap;
		ak_mutex_unlock(&post_mu);
		if (running.len == 0) break;
		for (uint32_t i = 0; i < running.len; i++) running.data[i].cb(running.data[i].ctx);
		running.len = 0;
	}
	running_posted--;
	ak_sched_run();
}

// ---- Timers ----

typedef struct timer {
	uint64_t id;
	int64_t at;
	ak_cb cb;
	void *ctx;
} timer;

static ak_mutex timer_mu = AK_MUTEX_INIT;
static ak_cond timer_cond;
static AK_VEC(timer) timers;  // a min-heap on at (then id)
static uint64_t next_timer_id;
static bool timer_thread_started;
static void (*host_schedule)(uint64_t, int64_t);
static void (*host_cancel)(uint64_t);
static bool manual;
static int64_t manual_now;

int64_t ak_sched_now(void) { return manual ? manual_now : ak_now_ns(); }
bool ak_sched_manual(void) { return manual; }
int64_t ak_sched_wall(void) { return manual ? AK_TEST_EPOCH + manual_now : ak_wall_ns(); }

static bool less(const timer *a, const timer *b) { return a->at < b->at || (a->at == b->at && a->id < b->id); }

static void heap_push(timer t) {
	ak_vec_push(&timers, t);
	uint32_t i = timers.len - 1;
	while (i > 0) {
		uint32_t p = (i - 1) / 2;
		if (!less(&timers.data[i], &timers.data[p])) break;
		timer x = timers.data[i];
		timers.data[i] = timers.data[p];
		timers.data[p] = x;
		i = p;
	}
}

static void heap_fix(uint32_t i) {
	for (;;) {
		uint32_t l = 2 * i + 1, r = l + 1, m = i;
		if (l < timers.len && less(&timers.data[l], &timers.data[m])) m = l;
		if (r < timers.len && less(&timers.data[r], &timers.data[m])) m = r;
		if (m == i) break;
		timer x = timers.data[i];
		timers.data[i] = timers.data[m];
		timers.data[m] = x;
		i = m;
	}
}

static timer heap_pop(void) {
	timer t = timers.data[0];
	timers.data[0] = timers.data[--timers.len];
	if (timers.len) heap_fix(0);
	return t;
}

static bool heap_remove(uint64_t id, timer *out) {
	for (uint32_t i = 0; i < timers.len; i++) {
		if (timers.data[i].id != id) continue;
		*out = timers.data[i];
		timers.data[i] = timers.data[--timers.len];
		if (i < timers.len) {
			heap_fix(i);
			// It may also need to move up.
			while (i > 0) {
				uint32_t p = (i - 1) / 2;
				if (!less(&timers.data[i], &timers.data[p])) break;
				timer x = timers.data[i];
				timers.data[i] = timers.data[p];
				timers.data[p] = x;
				i = p;
			}
		}
		return true;
	}
	return false;
}

static bool expired_posted;

// run_expired runs, on the main thread, every timer that's due. The
// timer thread only wakes the main thread; it never runs or removes a
// timer, so a cancel on the main thread is final.
static void run_expired(void *arg) {
	AK_UNUSED(arg);
	for (;;) {
		ak_mutex_lock(&timer_mu);
		expired_posted = false;
		if (timers.len == 0 || timers.data[0].at > ak_sched_now()) {
			if (timer_thread_started) ak_cond_signal(&timer_cond);
			ak_mutex_unlock(&timer_mu);
			return;
		}
		timer t = heap_pop();
		ak_mutex_unlock(&timer_mu);
		t.cb(t.ctx);
	}
}

static void timer_thread(void *arg) {
	AK_UNUSED(arg);
	ak_mutex_lock(&timer_mu);
	for (;;) {
		if (timers.len == 0 || expired_posted) {
			ak_cond_wait(&timer_cond, &timer_mu);
			continue;
		}
		int64_t now = ak_now_ns();
		if (timers.data[0].at > now) {
			ak_cond_wait_ns(&timer_cond, &timer_mu, timers.data[0].at - now);
			continue;
		}
		expired_posted = true;
		ak_mutex_unlock(&timer_mu);
		ak_post(run_expired, NULL);
		ak_mutex_lock(&timer_mu);
	}
}

void ak_sched_set_timer(void (*schedule)(uint64_t, int64_t), void (*cancel)(uint64_t)) {
	host_schedule = schedule;
	host_cancel = cancel;
}

void ak_sched_manual_clock(bool on) {
	manual = on;
	manual_now = 0;
}

uint64_t ak_after(int64_t ns, ak_cb cb, void *ctx) {
	if (ns < 0) ns = 0;
	ak_mutex_lock(&timer_mu);
	uint64_t id = ++next_timer_id;
	// Due at now + ns, or (a sleep past int's range) never.
	int64_t now = ak_sched_now();
	timer t = {id, ns > INT64_MAX - now ? INT64_MAX : now + ns, cb, ctx};
	heap_push(t);
	bool thread = !manual && !host_schedule;
	if (thread && !timer_thread_started) {
		ak_cond_init(&timer_cond);
		timer_thread_started = ak_thread_start(timer_thread, NULL);
		if (!timer_thread_started) ak_panic("ak_after: no timer thread and no host timers");
	}
	if (thread) ak_cond_signal(&timer_cond);
	ak_mutex_unlock(&timer_mu);
	if (!manual && host_schedule) host_schedule(id, ns);
	return id;
}

void ak_timer_cancel(uint64_t id) {
	timer t;
	ak_mutex_lock(&timer_mu);
	bool found = heap_remove(id, &t);
	ak_mutex_unlock(&timer_mu);
	if (found && host_cancel && !manual) host_cancel(id);
}

void ak_timer_fire(uint64_t id) {
	timer t;
	ak_mutex_lock(&timer_mu);
	bool found = heap_remove(id, &t);
	ak_mutex_unlock(&timer_mu);
	if (found) t.cb(t.ctx);
}

// ---- Giving memory back ----
//
// A turn of the main thread's work that grew its allocator's heap (a
// spike: a big file parsed, a long list built) has what it freed given
// back to the system a while later (ak_alloc_trim), as the app goes quiet
// or, if it's still busy, then: an app that stops allocating doesn't keep
// the spike's memory. One timer at a time, armed only when the heap grew
// again, so an idle app isn't woken; none under a test's clock.
#define TRIM_AFTER ((int64_t)2000000000)
static bool trim_armed;

static void trim(void *ctx) {
	AK_UNUSED(ctx);
	trim_armed = false;
	ak_alloc_trim();
}

static void arm_trim(void) {
	if (trim_armed || manual || !ak_alloc_grown()) return;
	trim_armed = true;
	// The scheduler's own allocation (a timer), not what a test counts.
	uint64_t count = ak_alloc_count, bytes = ak_alloc_bytes;
	ak_after(TRIM_AFTER, trim, NULL);
	ak_alloc_count = count, ak_alloc_bytes = bytes;
}

bool ak_sched_next_timer(int64_t *at) {
	ak_mutex_lock(&timer_mu);
	bool any = timers.len > 0;
	if (any) *at = timers.data[0].at;
	ak_mutex_unlock(&timer_mu);
	return any;
}

void ak_sched_advance(int64_t ns) {
	AK_ASSERT(manual);
	int64_t until = manual_now + ns;
	for (;;) {
		ak_mutex_lock(&timer_mu);
		if (timers.len == 0 || timers.data[0].at > until) {
			ak_mutex_unlock(&timer_mu);
			break;
		}
		timer t = heap_pop();
		manual_now = t.at;
		ak_mutex_unlock(&timer_mu);
		t.cb(t.ctx);
		ak_sched_run();
	}
	manual_now = until;
}

// ---- Futures ----

struct ak_waiter {
	ak_cb cb;
	void *ctx;
	ak_waiter *next;
};

static void future_drop(ak_opaque *o) {
	ak_future *f = (ak_future *)o;
	if (f->unseen) f->unseen(f->unseen_ctx, f->state == AK_FUT_FAILED && !f->seen ? ak_retain(f->error) : NULL);
	ak_release_val(f->result, (ak_kind)f->kind);
	ak_release(f->error);
	for (ak_waiter *w = f->waiters; w;) {
		ak_waiter *n = w->next;
		ak_free(w);
		w = n;
	}
	if (f->drop_ctx) f->drop_ctx(f->ctx);
}

ak_future *ak_future_new(ak_kind k) {
	ak_future *f = ak_opaque_new(AK_TAG_TASK, sizeof(ak_future), future_drop);
	f->kind = (uint8_t)k;
	return f;
}

static void settle(ak_future *f) {
	// Waiters run from the ready queue, in the order they waited.
	ak_waiter *rev = NULL;
	for (ak_waiter *w = f->waiters; w;) {
		ak_waiter *n = w->next;
		w->next = rev;
		rev = w;
		w = n;
	}
	f->waiters = NULL;
	for (ak_waiter *w = rev; w;) {
		ak_waiter *n = w->next;
		ak_ready(w->cb, w->ctx);
		ak_free(w);
		w = n;
	}
}

void ak_future_resolve(ak_future *f, ak_val v) {
	if (f->state != AK_FUT_PENDING) {
		ak_release_val(v, (ak_kind)f->kind);
		return;
	}
	f->state = AK_FUT_DONE;
	f->result = v;
	settle(f);
}

void ak_future_reject(ak_future *f, void *err) {
	if (f->state != AK_FUT_PENDING) {
		ak_release(err);
		return;
	}
	f->state = AK_FUT_FAILED;
	f->error = err;
	settle(f);
}

void ak_future_wait(ak_future *f, ak_cb cb, void *ctx) {
	f->seen = true;
	if (f->state != AK_FUT_PENDING) {
		ak_ready(cb, ctx);
		return;
	}
	ak_waiter *w = ak_alloc(sizeof *w);
	w->cb = cb;
	w->ctx = ctx;
	w->next = f->waiters;
	f->waiters = w;
}

bool ak_future_unwait(ak_future *f, ak_cb cb, void *ctx) {
	for (ak_waiter **pp = &f->waiters; *pp; pp = &(*pp)->next) {
		ak_waiter *w = *pp;
		if (w->cb == cb && w->ctx == ctx) {
			*pp = w->next;
			ak_free(w);
			return true;
		}
	}
	return false;
}

void *ak_future_waiter(const ak_future *f, ak_cb cb) {
	for (const ak_waiter *w = f->waiters; w; w = w->next)
		if (w->cb == cb) return w->ctx;
	return NULL;
}

void ak_future_cancel(ak_future *f) {
	if (f->state != AK_FUT_PENDING) return;
	if (f->cancel) {
		// The work's to stop, and it settles f once it has: a task once
		// it's unwound through its defers, a native call once the
		// platform answers. So whoever waits for f waits for that.
		void (*c)(ak_future *) = f->cancel;
		f->cancel = NULL;
		c(f);
		return;
	}
	// The cancellation is made through the pending error's slot: one
	// already there (an error unwinding, whose frames' values are let go,
	// a stream's waiting next among them) stays.
	void *pending = ak_error_pending() ? ak_error_take() : NULL;
	ak_throw_cancel();
	void *err = ak_error_take();
	if (pending) ak_throw(pending);
	ak_future_reject(f, err);
}

ak_val ak_future_take(ak_future *f) {
	f->seen = true;
	if (f->state == AK_FUT_FAILED) {
		ak_throw(ak_retain(f->error));
		return (ak_val){0};
	}
	AK_ASSERT(f->state == AK_FUT_DONE);
	ak_retain_val(f->result, (ak_kind)f->kind);
	return f->result;
}

// ---- Sleep ----
//
// The timer holds no reference: dropping the future cancels its timer,
// so a sleep nobody awaits any more costs nothing.

static void sleep_fire(void *p) {
	ak_future *f = p;
	f->ctx = NULL;
	f->cancel = NULL;
	ak_future_resolve(f, (ak_val){0});
}

static void sleep_stop(void *ctx) {
	uint64_t id = (uint64_t)(uintptr_t)ctx;
	if (id) ak_timer_cancel(id);
}

static void sleep_cancel(ak_future *f) {
	sleep_stop(f->ctx);
	f->ctx = NULL;
	ak_throw_cancel();
	ak_future_reject(f, ak_error_take());
}

ak_future *ak_sleep(int64_t ns) {
	ak_future *f = ak_future_new(AK_K_VOID);
	f->cancel = sleep_cancel;
	f->drop_ctx = sleep_stop;
	f->ctx = (void *)(uintptr_t)ak_after(ns, sleep_fire, f);
	return f;
}
