#include "vm/vm_int.h"

// Tasks: an async function on a coroutine of its own. A task runs at
// once, until it finishes or awaits something not done yet; a future's
// completion resumes it from the ready queue. Its future holds its
// result. Cancelling the future (its owner went away) resumes it with a
// cancellation, which unwinds it through its handlers and defers.

static void task_cancel(ak_future *f);

static void finish(ak_task *t, ak_val out, bool failed) {
	ak_future *f = t->fut;
	t->fut = NULL;
	f->cancel = NULL;
	f->ctx = NULL;
	void *err = failed ? ak_error_take() : NULL;
	// An error nobody has looked for goes to the catcher of the task's
	// owner, unless the task was cancelled: its owner went, and what it
	// threw unwinding (a defer's) is no one's to report. With no one else
	// holding the future, now; else once it's dropped, if no one looked
	// for it meanwhile (a handler that threw before its first await: who
	// started it may await it, or let it go).
	bool report = err && !ak_is_cancel(err) && !t->cancelled && !f->seen;
	void *unseen = report && f->h.h.rc == 1 ? ak_retain(err) : NULL;
	if (report && !unseen) ak_vm_task_report_unseen(t, f);
	AK_ASSERT(t->co.nframes == 0);
	// Settle the future before the owner goes: disposing it cancels what
	// is still pending.
	if (failed) ak_future_reject(f, err);
	else ak_future_resolve(f, out);
	ak_vm_task_done(t, unseen);
	ak_module_release(t->mod);
	ak_free(t->co.regs);
	ak_free(t->co.frames);
	ak_free(t->co.untracks);
	ak_free(t);
	ak_release(f);  // the task's reference
}

static void step(ak_task *t) {
	ak_val out;
	t->running = true;
	bool suspended = ak_vm_task_run(t, &out);
	t->running = false;
	if (!suspended) finish(t, out, ak_error_pending());
}

ak_future *ak_vm_task_start(const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs) {
	return ak_vm_task_start_in(f, clos, args, nargs, NULL);
}

ak_future *ak_vm_task_start_in(const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs, ak_owner *holder) {
	ak_future *fut = ak_future_new((ak_kind)f->ret);
	ak_task *t = ak_alloc(sizeof *t);
	t->fut = ak_retain(fut);
	t->co.task = t;
	t->mod = f->mod;
	ak_module_retain(f->mod);  // its code runs on until it finishes
	fut->cancel = task_cancel;
	fut->ctx = t;
	ak_vm_task_own(t, holder);
	if (!ak_vm_push(&t->co, f, clos, args, nargs, 0xFFFF, true, 0)) finish(t, (ak_val){0}, true);
	else step(t);
	return fut;
}

void ak_vm_task_resume(void *p) {
	ak_task *t = p;
	ak_future *f = t->awaiting;
	t->awaiting = NULL;
	ak_release(f);
	step(t);
}

static void task_cancel(ak_future *f) {
	ak_task *t = f->ctx;
	if (!t || t->cancelled) return;
	t->cancelled = true;
	if (t->running || !t->awaiting) return;  // its next await throws
	// Stop waiting: resume it now, and its AWAIT throws the cancellation.
	ak_future *w = t->awaiting;
	// Settled already: its resume is on the ready queue, and the task's
	// next await throws.
	if (!ak_future_unwait(w, ak_vm_task_resume, t)) return;
	// Nobody else can see what it waited for (the task's register and
	// t->awaiting hold the only references): stop that work too.
	if (w->h.h.rc <= 2u + w->held) ak_future_cancel(w);
	// It unwinds through its defers here, not later from the ready queue:
	// its owner going cancels it, and what the owner's cleanups let go
	// after (the component's memos, which a defer may read) is still
	// there. An error already unwinding (the canceller's) waits for it.
	void *pending = ak_error_pending() ? ak_error_take() : NULL;
	ak_vm_task_resume(t);
	if (pending) ak_throw(pending);
}

// ---- all(items, f, limit:) ----
//
// async.all_each(items, f, limit, kind) starts f with each item as a task,
// limit of them at a time (0: all at once), and returns a future of their
// results in items' order, an array of kind. The first that fails cancels
// the others, and none starts after it; the future fails with its error
// once every task started has stopped, as all(a, b) does. Cancelling the
// future cancels the tasks. The tasks started after the first ones are
// started under the owner that was current as all began.

typedef struct {
	ak_future *out;
	ak_arr *items;
	ak_closure *f;
	ak_owner *owner;  // the caller's (NULL: none)
	ak_future **futs; // each item's task, once started
	uint32_t next;    // the next item to start
	uint32_t running; // started and not settled
	uint32_t limit;
	ak_kind kind;
	int32_t failed;   // the first item that failed, -1
	bool stopping;    // failed or cancelled: start no more
} each_ctx;

typedef struct {
	each_ctx *c;
	uint32_t i;
} each_item;

static void each_done(void *p);

// cancel_error makes a cancellation, as ak_future_cancel does: through
// the pending error's slot, keeping an error that's there.
static void *cancel_error(void) {
	void *pending = ak_error_pending() ? ak_error_take() : NULL;
	ak_throw_cancel();
	void *err = ak_error_take();
	if (pending) ak_throw(pending);
	return err;
}

// each_call calls f with item i: its future, or a resolved one when f
// doesn't wait (a function that isn't async).
static void each_call(void *p) {
	each_item *it = p;
	each_ctx *c = it->c;
	ak_val arg = c->items->data[it->i];
	ak_closure *f = c->f;
	ak_future *fut;
	bool async = f->invoke != ak_vm_closure_invoke || ak_closure_async(f);
	ak_val v = ak_call(f, &arg, 1);
	if (async && !ak_error_pending()) {
		fut = v.p;
	} else {
		fut = ak_future_new(c->kind);
		if (ak_error_pending()) ak_future_reject(fut, ak_error_take());
		else ak_future_resolve(fut, v);
	}
	c->futs[it->i] = fut;
}

static void each_stop(each_ctx *c);

static void each_start(each_ctx *c) {
	while (!c->stopping && c->next < c->items->len && (c->limit == 0 || c->running < c->limit)) {
		if (c->owner && ak_owner_disposed(c->owner)) {
			// Its owner went: what it started stops, and nothing more starts.
			each_stop(c);
			return;
		}
		each_item *it = ak_alloc(sizeof *it);
		it->c = c;
		it->i = c->next++;
		c->running++;
		if (c->owner) ak_with_owner(c->owner, each_call, it);
		else each_call(it);
		ak_retain(c->out);
		ak_future_wait(c->futs[it->i], each_done, it);
	}
}

static void each_settle(each_ctx *c) {
	ak_future *out = c->out;
	if (c->failed >= 0) {
		ak_future_reject(out, ak_retain(c->futs[c->failed]->error));
		return;
	}
	ak_kind k = c->kind == AK_K_VOID ? AK_K_INT : c->kind;
	ak_arr *res = ak_arr_new(k, NULL, c->items->len);
	for (uint32_t i = 0; i < c->items->len; i++) {
		ak_val v = {0};
		if (c->kind != AK_K_VOID) {
			v = c->futs[i]->result;
			ak_retain_val(v, k);
			if (k == AK_K_REF && v.p && ((ak_obj *)v.p)->tag == AK_TAG_STR) res->h.flags = 1;
		}
		res->data[res->len++] = v;
	}
	ak_future_resolve(out, (ak_val){.p = res});
}

static void each_stop(each_ctx *c) {
	c->stopping = true;
	for (uint32_t j = 0; j < c->next; j++)
		if (c->futs[j]->state == AK_FUT_PENDING) ak_future_cancel(c->futs[j]);
}

static void each_done(void *p) {
	each_item *it = p;
	each_ctx *c = it->c;
	ak_future *out = c->out;
	ak_future *f = c->futs[it->i];
	uint32_t i = it->i;
	ak_free(it);
	c->running--;
	if (f->state == AK_FUT_FAILED && !c->stopping) {
		// The first failure: the others are cancelled, none starts.
		c->failed = (int32_t)i;
		each_stop(c);
	}
	if (out->state == AK_FUT_PENDING) {
		each_start(c);
		if (c->running == 0) {
			if (c->failed >= 0 || (c->next == c->items->len && !c->stopping)) each_settle(c);
			else if (c->stopping) ak_future_reject(out, cancel_error());
		}
	}
	ak_release(out);
}

static void each_cancel(ak_future *f) {
	each_ctx *c = f->ctx;
	if (c->running == 0) {
		ak_future_reject(f, cancel_error());
		return;
	}
	each_stop(c);
}

static void each_drop(void *p) {
	each_ctx *c = p;
	for (uint32_t i = 0; i < c->next; i++) ak_release(c->futs[i]);
	ak_free(c->futs);
	ak_release(c->items);
	ak_release(c->f);
	if (c->owner) ak_owner_release(c->owner);
	ak_free(c);
}

static ak_val all_each(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *items = args[0].p;
	int64_t limit = args[2].i;
	if (limit < 0) {
		ak_throw_msg("all's limit is %lld: it must be 0 (no limit) or more", (long long)limit);
		return (ak_val){0};
	}
	ak_kind k = (ak_kind)args[3].i;
	ak_future *out = ak_future_new(k == AK_K_VOID ? AK_K_VOID : AK_K_REF);
	each_ctx *c = ak_alloc(sizeof *c);
	*c = (each_ctx){.out = out, .items = ak_retain(items), .f = ak_retain(args[1].p), .limit = (uint32_t)(limit > UINT32_MAX ? UINT32_MAX : limit), .kind = k, .failed = -1};
	c->futs = ak_alloc(sizeof(ak_future *) * (items->len ? items->len : 1));
	c->owner = ak_current_owner();
	if (c->owner) ak_owner_retain(c->owner);
	out->ctx = c;
	out->drop_ctx = each_drop;
	out->cancel = each_cancel;
	if (items->len == 0) {
		each_settle(c);
		return (ak_val){.p = out};
	}
	each_start(c);
	return (ak_val){.p = out};
}

void ak_vm_each_init(void) { ak_builtin_register("async.all_each", all_each); }
