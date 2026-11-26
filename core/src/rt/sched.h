// sched.h: futures, the main thread's queues and timers: what async code
// (the VM's tasks, compiled C, native calls) waits on and resumes from.
//
// Three queues run on the main thread:
//   - ready: callbacks to run soon, in order (a future's waiters, tasks
//     to resume). ak_sched_run drains it; the UI runtime calls it after
//     each event and posted callback, before it flushes.
//   - posted: callbacks other threads ask for (ak_post, thread-safe).
//     The host is asked to call ak_sched_run_posted soon (ak_sched_set_wake).
//   - timers: callbacks after a delay (ak_after), from a timer thread
//     that posts them, or a host's own timers (ak_sched_set_timer).
#ifndef AK_SCHED_H
#define AK_SCHED_H

#include "rt/rt.h"

typedef void (*ak_cb)(void *ctx);

// ak_ready queues cb to run on the main thread at the next ak_sched_run.
void ak_ready(ak_cb cb, void *ctx);
// ak_sched_run runs ready callbacks until none is left; false when there
// was nothing to run.
bool ak_sched_run(void);
bool ak_sched_has_ready(void);

// ak_post queues cb to run on the main thread. Any thread.
void ak_post(ak_cb cb, void *ctx);
void ak_sched_run_posted(void);
// Work on other threads that will post back (spawn's jobs, native calls)
// holds the scheduler; a test's loop waits for it (ak_sched_wait_posted)
// rather than deciding that nothing is left to run.
void ak_sched_hold(void);
void ak_sched_unhold(void);
bool ak_sched_held(void);
// ak_sched_wait_posted blocks until something is posted or nothing holds
// the scheduler. Tests only: the main thread never blocks otherwise.
void ak_sched_wait_posted(void);
// ak_sched_set_wake sets what asks the host to call ak_sched_run_posted
// (the host's post callback). Any thread may call wake.
void ak_sched_set_wake(void (*wake)(void));
// ak_sched_wake asks the host to call ak_sched_run_posted soon: work made
// ready after the last ak_sched_run of an event (a task a flush disposed,
// resumed to unwind) runs then, not at the next event.
void ak_sched_wake(void);

// ak_after runs cb on the main thread after ns nanoseconds; the result
// cancels it with ak_timer_cancel (a no-op once it ran).
uint64_t ak_after(int64_t ns, ak_cb cb, void *ctx);
void ak_timer_cancel(uint64_t id);
// ak_sched_set_timer replaces the timer thread with the host's timers
// (the web's setTimeout): schedule(id, ns) must call ak_timer_fire(id)
// on the main thread after ns.
void ak_sched_set_timer(void (*schedule)(uint64_t id, int64_t ns), void (*cancel)(uint64_t id));
void ak_timer_fire(uint64_t id);

// Tests drive time: with a manual clock, ak_after's timers fire only when
// ak_sched_advance moves the clock past them.
void ak_sched_manual_clock(bool on);
void ak_sched_advance(int64_t ns);
// ak_sched_next_timer gives when the next timer is due (false: none).
bool ak_sched_next_timer(int64_t *at);
int64_t ak_sched_now(void);
// ak_sched_manual reports the manual clock (a test's run). Any thread.
bool ak_sched_manual(void);
// ak_sched_wall is the time since the Unix epoch in nanoseconds: the
// system's, or with the manual clock AK_TEST_EPOCH plus the manual
// clock's time, so tests see the same dates on every run. Any thread.
int64_t ak_sched_wall(void);
#define AK_TEST_EPOCH INT64_C(1767225600000000000)  // 2026-01-01T00:00:00Z

// ---- Futures ----

typedef enum { AK_FUT_PENDING, AK_FUT_DONE, AK_FUT_FAILED } ak_fut_state;

typedef struct ak_waiter ak_waiter;

typedef struct ak_future {
	ak_opaque h;  // tag AK_TAG_TASK
	uint8_t state;
	uint8_t kind;  // the result's kind
	ak_val result;
	void *error;
	ak_waiter *waiters;
	// cancel is called once when the future is cancelled while pending
	// (its owner went away, a race was won): it stops the work (a timer, a
	// native call, a task), and the future is settled once the work has
	// stopped (a task, once it's unwound through its defers). Without
	// one, cancelling rejects the future at once.
	void (*cancel)(struct ak_future *f);
	void *ctx;
	void (*drop_ctx)(void *ctx);
	// held: references the work itself holds until it settles (a native
	// call's table), not a waiter's: a task cancelled while it waits
	// cancels the future when nobody else holds it.
	uint8_t held;
	// seen: whether anything has looked for its outcome (waited for it,
	// or taken it). A failed future dropped unseen gives its error to
	// unseen(unseen_ctx, err), as JavaScript reports a rejection nobody
	// handled; dropped otherwise, unseen(unseen_ctx, NULL) lets ctx go.
	bool seen;
	void (*unseen)(void *ctx, void *err);
	void *unseen_ctx;
} ak_future;

ak_future *ak_future_new(ak_kind k);
void ak_future_resolve(ak_future *f, ak_val v);  // takes v; main thread
void ak_future_reject(ak_future *f, void *err);   // takes err
// ak_future_wait calls cb(ctx) from the ready queue once f is no longer
// pending (at once, queued, if it isn't now).
void ak_future_wait(ak_future *f, ak_cb cb, void *ctx);
// ak_future_unwait removes a waiter ak_future_wait added (a no-op once
// it was queued).
bool ak_future_unwait(ak_future *f, ak_cb cb, void *ctx);  // whether it was waiting
// ak_future_waiter is the ctx of f's first waiter that waits with cb
// (NULL: none): the debugger's, finding the task awaiting a task.
void *ak_future_waiter(const ak_future *f, ak_cb cb);
void ak_future_cancel(ak_future *f);
// ak_future_take gives the result (owned, retained) or throws the error.
ak_val ak_future_take(ak_future *f);

// ak_sleep is a future resolved after ns.
ak_future *ak_sleep(int64_t ns);

#endif
