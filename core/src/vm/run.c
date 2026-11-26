#include "vm/vm_int.h"
#include "vm/ops.h"

// The run loop: a coroutine's frames run until a boundary frame returns
// or the coroutine suspends. A frame of a function compiled to C
// (ak_async_body) runs its body, which leaves the frame returned, threw,
// waiting on a frame it pushed, or suspended; an interpreted frame runs
// on the interpreter (interp.c), which comes back here when the top
// frame is a compiled function's. A release build compiles every
// function, so it runs here and in the bodies only.

static inline ak_co *cur_co(void) {
	if (!ak_vm_cur) ak_vm_cur = &ak_vm_main;
	return ak_vm_cur;
}

// returned hands the result v of the frame just popped (its caller's
// register dst, of kind k) to the caller, the top frame.
static void returned(ak_co *co, ak_val v, uint16_t dst, ak_kind k) {
	if (dst == 0xFFFF) {
		ak_release_val(v, k);
		return;
	}
	ak_frame *fr = &co->frames[co->nframes - 1];
	ak_op_store(fr->fn->kinds, co->regs + fr->base, dst, v);
}

// threw unwinds an error from a compiled function's frame, popped: to the
// first caller that takes it (an interpreted one's handler covering the
// call, or a compiled one, which resumes with the error pending and finds
// its handler). False when the error leaves the run (a boundary frame,
// or the coroutine's first, threw).
static bool threw(ak_co *co, bool boundary, uint32_t callpc) {
	for (;;) {
		if (boundary || co->nframes == 0) return false;
		ak_frame *fr = &co->frames[co->nframes - 1];
		if (fr->fn->resume) return true;
		int64_t h = ak_vm_find_handler(fr->fn, callpc);
		if (h >= 0) {
			fr->pc = (uint32_t)h;
			return true;
		}
		boundary = fr->boundary;
		callpc = fr->callpc;
		ak_vm_pop(co, -1);
	}
}

bool ak_vm_run(ak_co *co, ak_val *out) {
	ak_co *prev = cur_co();
	ak_vm_cur = co;
	// The C stack's limit its compiled functions read is this run's
	// thread's: a task may resume on another thread than the last run's
	// (a program's main queue, drained by libdispatch's threads), whose
	// stack is elsewhere.
	co->lim = 0;
	bool suspended = false;
	for (;;) {
		ak_frame *fr = &co->frames[co->nframes - 1];
		const ak_func *F = fr->fn;
		if (!F->resume) {
			int s = ak_vm_interp(co, out);
			if (s == AK_RUN_PUSHED) continue;
			suspended = s == AK_RUN_SUSPEND;
			break;
		}
		ak_val v = {0};
		int s = F->resume(co, &v);
		if (s == AK_RUN_SUSPEND) {
			suspended = true;
			break;
		}
		if (s == AK_RUN_PUSHED) continue;
		fr = &co->frames[co->nframes - 1];
		bool boundary = fr->boundary;
		uint16_t dst = fr->dst;
		uint32_t callpc = fr->callpc;
		ak_vm_pop_frame(co);
		if (s == AK_RUN_RET) {
			if (boundary || co->nframes == 0) {
				*out = v;
				break;
			}
			returned(co, v, dst, (ak_kind)F->ret);
			continue;
		}
		if (!threw(co, boundary, callpc)) {
			*out = (ak_val){0};
			break;
		}
	}
	ak_vm_cur = prev;
	return suspended;
}
