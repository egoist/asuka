// signal.h: Asuka's fine-grained reactivity: signals, memos and effects
// (alien-signals' algorithm), owned as in Solid.
//
// Writes push dirty flags through the graph; reads pull values. A memo
// recomputes only when one of its sources really changed, and an effect
// runs at most once per flush, so updates are glitch-free (a diamond sees
// one consistent state) and do no unnecessary work. Links between nodes
// are reused across runs and kept on a free list: a computation that
// re-runs reading the same sources allocates nothing.
//
// The graph holds no values. The caller (the VM, the UI runtime) owns
// each value and tells the graph when it reads one (ak_signal_read,
// ak_memo_read) and when it changed one (ak_signal_write). A memo's value
// is recomputed by the caller's update callback, under tracking.
//
// Main thread only, except ak_signal_on_main_thread.
#ifndef AK_SIGNAL_H
#define AK_SIGNAL_H

#include "base/base.h"

typedef struct ak_link ak_link;
typedef struct ak_owner ak_owner;
typedef struct ak_comp ak_comp; // a memo or an effect
typedef ak_comp ak_memo;
typedef ak_comp ak_effect;

// ak_node is a signal's place in the graph, embedded in (or allocated
// next to) the caller's value. Zero is a valid, unread signal.
typedef struct ak_node {
	ak_link *subs, *subs_tail; // computations that read it
	uint32_t flags;
} ak_node;

// How many times effects may dirty each other within one flush before it
// panics as an infinite loop.
#define AK_MAX_PASSES 100

// Signals.

static inline void ak_signal_init(ak_node *n) { memset(n, 0, sizeof *n); }
// ak_signal_born marks a signal made now: one made inside a memo may be
// written there while nothing but that memo reads it (the memo makes a
// store and sets it up, reading what it set), which isn't a side effect
// anything else sees.
void ak_signal_born(ak_node *n);
// ak_signal_read subscribes the running computation, if any, to n.
void ak_signal_read(ak_node *n);
// ak_signal_write tells the graph the caller changed n's value (the
// caller skips writes it considers equal). Outside a batch and a flush,
// the effects it dirtied run before it returns. A write inside a memo
// panics, but to a signal the memo made in this run (ak_signal_born) that
// nothing but the memo reads.
void ak_signal_write(ak_node *n);
// ak_signal_drop unsubscribes everything that read n, before the caller
// frees it. Reads of a dropped signal track nothing.
void ak_signal_drop(ak_node *n);
static inline bool ak_signal_watched(const ak_node *n) { return n->subs != NULL; }

// Memos and effects.

// ak_comp_info describes a kind of memo or effect; it must outlive them
// (a static const per kind). Every field is optional.
typedef struct ak_comp_info {
	// drop runs once the computation is disposed and nothing retains it
	// (ak_owner_retain): the caller frees ctx and the value there.
	void (*drop)(void *ctx);
	// unwatched runs when a memo's last subscriber goes. The graph has
	// already let go of the memo's sources; it recomputes on its next
	// read.
	void (*unwatched)(void *ctx);
	const char *label; // names it in panics, e.g. "Counter.total (app.ak:12)"
} ak_comp_info;

// ak_memo_new makes a memo owned by the current owner (by the caller
// when there is none: ak_owner_dispose(ak_memo_owner(m)) disposes it).
// It is lazy: update first runs on the first read. update recomputes the
// caller's value, reading its sources tracked, and reports whether the
// value changed.
ak_memo *ak_memo_new(bool (*update)(void *ctx), void *ctx, const ak_comp_info *info);
// ak_memo_read brings m up to date and subscribes the running
// computation. It returns false when m's last update raised an error:
// it raised it again (ak_raise) in the reader, which must return.
// Reading a disposed memo does neither: the caller's value is the last
// one computed.
bool ak_memo_read(ak_memo *m);
ak_owner *ak_memo_owner(ak_memo *m);
// ak_memo_error is the error m's last update raised, or NULL. It is the
// caller's pointer, held until m's next update or its drop.
void *ak_memo_error(const ak_memo *m);

enum {
	AK_EFFECT_USER = 0,
	// A render effect (the UI runtime's, for props) runs at once, and
	// within a flush before every user effect.
	AK_EFFECT_RENDER = 1 << 0,
};

// ak_effect_new makes an effect owned by the current owner (by the
// caller when there is none). A user effect runs now if no batch or flush
// is in progress, else at the end of it; then again whenever what run
// read changes. Each run first cleans the effect's own owner: what the
// last run made is disposed and its cleanups run. The pointer returned is
// borrowed from the owner.
ak_effect *ak_effect_new(void (*run)(void *ctx), void *ctx, uint32_t flags, const ak_comp_info *info);
ak_owner *ak_effect_owner(ak_effect *e);

// Tracking and batching.

// ak_untrack runs fn without subscribing the running computation to what
// it reads; ak_untrack_begin and ak_untrack_end bracket the same.
void ak_untrack(void (*fn)(void *ctx), void *ctx);
void *ak_untrack_begin(void);
void ak_untrack_end(void *token);
// ak_tracking reports whether a computation is tracking reads.
bool ak_tracking(void);

// ak_signal_batch runs fn and the effects its writes dirtied once, after it.
void ak_signal_batch(void (*fn)(void *ctx), void *ctx);
void ak_batch_begin(void);
void ak_batch_end(void);
bool ak_signal_batching(void);
bool ak_signal_flushing(void);
// ak_batch_suspend leaves the batches open now, running the effects their
// writes dirtied, and returns their depth for ak_batch_resume, which opens
// them again: for a driver that must see each write's effects at once
// (a UI test, resumed inside a host's callback). Not while flushing.
int ak_batch_suspend(void);
void ak_batch_resume(int depth);
// ak_signal_set_after_flush sets fn to run at the end of each flush, once
// effects settled (the UI runtime's layout and frames pass). Writes in fn
// run their effects, then fn again, in the same flush.
void ak_signal_set_after_flush(void (*fn)(void *ctx), void *ctx);

// Owners.
//
// An owner owns memos, effects, child owners and cleanups. Disposing it
// disposes what it owns (last made first), runs its cleanups (last
// registered first) and unsubscribes its computations. Disposal is
// explicit: nothing is ever disposed because nothing points to it.
//
// Memory: an owner attached to a parent is held by the parent until
// disposed; one with no parent (a root, or made with no current owner) by
// its creator, until ak_owner_dispose. Whoever else keeps a pointer past
// that retains it. An owner retains its parent, so contexts and catchers
// stay reachable from it.

// ak_owner_new makes an owner under the current one, disposed with it.
ak_owner *ak_owner_new(void);
// ak_root_new makes an owner not disposed with the current one, though
// it sees its contexts and its catcher.
ak_owner *ak_root_new(void);
// ak_root runs fn with a new root as the current owner, untracked and in
// a batch: effects fn makes run once it returns. It returns the root,
// unless fn disposed it.
ak_owner *ak_root(void (*fn)(void *ctx, ak_owner *root), void *ctx);
// ak_owner_dispose disposes o and everything it owns, and drops the hold
// of its parent or creator. Disposing twice is a no-op.
void ak_owner_dispose(ak_owner *o);
bool ak_owner_disposed(const ak_owner *o);
void ak_owner_retain(ak_owner *o);
void ak_owner_release(ak_owner *o);
// ak_current_owner is the owner new computations would belong to.
ak_owner *ak_current_owner(void);
// ak_with_owner runs fn with o as the current owner, untracked.
void ak_with_owner(ak_owner *o, void (*fn)(void *ctx), void *ctx);
// ak_with_owner_tracked runs fn with o as the current owner, the running
// computation still tracking what fn reads: what an effect builds under
// an owner of its own, that it disposes itself (the UI's Dynamic).
void ak_with_owner_tracked(ak_owner *o, void (*fn)(void *ctx), void *ctx);
// ak_owner_enter makes an owner under the current one (disposed with it)
// the current owner, the running computation still tracking, and returns
// the one it replaced, for ak_owner_leave: what a component that
// provides builds under, so its contexts are its own views', not its
// siblings'. A caller that unwinds past the leave (an error) is inside an
// ak_with_owner, which restores the owner as it returns.
ak_owner *ak_owner_enter(void);
void ak_owner_leave(ak_owner *prev);
// ak_on_cleanup registers fn to run when the current owner is disposed,
// or before the current computation re-runs. With no owner it does
// nothing.
void ak_on_cleanup(void (*fn)(void *ctx), void *ctx);

// Contexts pass values down the owner tree, keyed by an address.

// ak_provide runs fn under a new owner where key is value, and returns
// it: held by the current owner (by the caller when there is none).
ak_owner *ak_provide(const void *key, void *value, void (*fn)(void *ctx), void *ctx);
void ak_owner_set_context(ak_owner *o, const void *key, void *value);
// ak_context_lookup finds the nearest value for key from the current
// owner up.
bool ak_context_lookup(const void *key, void **value);
void *ak_use_context(const void *key, void *def);

// Errors.
//
// C can't unwind, so an error doesn't travel up the stack: a computation
// that fails calls ak_raise and returns. Within an effect's run, a memo's
// update or ak_catch's fn, the error is held and delivered when that run
// returns, with the graph's state restored; the rest of the run should
// not go on (ak_failed tells code deeper down). An effect that raised
// keeps what it read so far and re-runs when that changes. A memo that
// raised counts as changed and raises the error again in every reader.
// Outside every run, ak_raise delivers at once.
//
// Delivery goes to the nearest catcher of the effect's owner (of the
// current owner, outside a run). An error raised inside a catcher's
// handler goes to the catcher around it. With no catcher, to the default
// handler (ak_signal_set_error_handler; panics when unset).

typedef void (*ak_error_fn)(void *ctx, void *err);

// ak_catcher_new makes an owner under the current one that catches what
// the computations it owns raise.
ak_owner *ak_catcher_new(ak_error_fn on_error, void *ctx);
// ak_catch runs fn under a new catcher, which catches fn's own error and
// those of the effects it owns, then and later. It returns the catcher,
// held by the current owner (by the caller when there is none).
ak_owner *ak_catch(void (*fn)(void *ctx), void *ctx, ak_error_fn on_error, void *ectx);
// Errors are the caller's objects; the graph holds them through the
// caller's references (ak_signal_set_error_refs; none: plain pointers).
// ak_raise takes a reference. Delivery lends the error to the handler
// and releases it after; a second error raised in the same run is
// released at once; a memo holds its error until its next update or its
// drop, and raises a new reference of it in each reader.
void ak_raise(void *err);
void ak_signal_set_error_refs(void (*retain)(void *err), void (*release)(void *err));
// ak_failed reports whether the running computation raised an error.
bool ak_failed(void);
void ak_signal_set_error_handler(ak_error_fn fn, void *ctx);

// Debug.

// ak_signal_bind_thread makes the calling thread the main thread. Builds
// with AK_DEBUG (and tests) then panic on a write from another one.
void ak_signal_bind_thread(void);
// ak_signal_on_main_thread reports whether the caller runs on the bound
// main thread (true before any is bound). Any thread. Code that a worker
// thread may also run (the app's language, read by formatting) reads
// such state from a plain signal and tracks it only here.
bool ak_signal_on_main_thread(void);
// ak_signal_set_panic_hook makes the graph's panics (an infinite loop of
// effects, a write inside a memo or off the main thread, an uncaught
// error) call fn instead of aborting. When fn returns, the graph gives
// up the operation: the flush stops, the write is not propagated.
void ak_signal_set_panic_hook(void (*fn)(void *ctx, const char *msg), void *ctx);

#endif
