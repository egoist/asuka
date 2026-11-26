// prof.h: the CPU profiler of asuka dev's Performance recordings: which
// of the app's functions the main thread spends its time in, sampled.
//
// A sampler thread wakes every interval (1 ms by default) and, while the
// main thread is busy (ak_prof_section: not idle), adds the time since it
// last woke to what's due and sets ak_dbg_on's AK_DBG_ON_PROF bit. The
// interpreter asks ak_dbg_on as a frame is entered or left and a jump is
// taken (the debugger's switch points, vm/debug.h: nothing per
// instruction); set, it runs the hooked copy of its loop, whose hook
// takes the sample on the main thread itself (ak_prof_take): the stack of
// the coroutine running, its frames and the bodies inlined in them, with
// the time due as its weight. So no thread reads another's stack, and
// nothing in the VM changes for it: a sample costs a switch to the hooked
// loop and back. What the main thread does outside the app's code (the
// flush's crossings into the host, its layout) is said by its section,
// and the time due as it changes is the section's own (a frame named for
// it: "(layout)"), as is the core's own work between the app's functions
// ("(runtime)"). Samples land at the next switch point, so a builtin that
// runs long counts in the function that called it.
//
// Only in development builds with threads (the debugger's builds,
// AK_VM_DEBUGGER): not in a release, nor on the web.
#ifndef AK_VM_PROF_H
#define AK_VM_PROF_H

#include "base/base.h"

// What the main thread does (ak_prof_section).
enum {
	AK_PROF_IDLE,    // the host's: nothing of the core's runs
	AK_PROF_APP,     // the app's code: handlers, effects, tasks, the core's work for them
	AK_PROF_PROPS,   // the props batch's crossing: the host applies it
	AK_PROF_LAYOUT,  // layout, and the host's measures
	AK_PROF_FRAMES,  // the frames batch's crossing
	AK_PROF_AFTER,   // after the frames: layout handlers, and what they change
	AK_PROF_NSECTIONS,
};

// ak_prof_clock is the clock samples are timed by (ak_now_ns); tests set
// one of their own, as ui/perf.c's flushes are.
extern int64_t (*ak_prof_clock)(void);

// ak_prof_available reports whether this build samples.
bool ak_prof_available(void);
// ak_prof_start forgets what it sampled before and starts sampling every
// interval_ns (0: 1 ms): false when it can't (no threads, no debugger's
// loop). Main thread.
bool ak_prof_start(uint32_t interval_ns);
// ak_prof_stop stops sampling, keeping what it sampled for ak_prof_write.
// Main thread.
void ak_prof_stop(void);
// ak_prof_section says what the main thread does from now on (AK_PROF_*);
// the time due for what it did is the old section's. Main thread; cheap
// when not sampling.
void ak_prof_section(int section);
// ak_prof_samples is how many samples were taken.
uint32_t ak_prof_samples(void);
// ak_prof_write appends what was sampled as a JSON object's members,
// times from t0 (ak_prof_clock's): "interval" (ns), "funcs" ([name, file,
// line] each: a function, or a section's, whose file is ""), "nodes"
// ([parent, func] each: the call tree, node 0 its root, [-1, -1]),
// "samples" ([t, node, weight] each: when it was taken, the stack's
// node, and the time it stands for, ns), "truncated" (too many to keep).
void ak_prof_write(ak_buf *b, int64_t t0);
// ak_prof_reset stops and frees everything (tests; a recording that's done).
void ak_prof_reset(void);

struct ak_co;
// ak_prof_take takes the sample due (debug.c's hook, on the main thread,
// when ak_dbg_on has AK_DBG_ON_PROF): co's stack as the instruction at
// pc of its top frame is about to run.
void ak_prof_take(struct ak_co *co, uint32_t pc);

#endif
