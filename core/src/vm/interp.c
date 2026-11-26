#include "vm/vm_int.h"
#include "vm/ops.h"
#include "vm/debug.h"

#include <stdio.h>

// Each thread runs the VM on its own coroutines: the main thread, and
// spawn's workers.
AK_TLS ak_co ak_vm_main;
AK_TLS ak_co *ak_vm_cur;

static inline ak_co *cur_co(void) {
	if (!ak_vm_cur) ak_vm_cur = &ak_vm_main;
	return ak_vm_cur;
}

// ak_vm_thread_end lets go of this thread's coroutine for calls from C
// (ak_thread_end's, on a thread that ends: a worker's): the main thread's
// lives as long as the process.
void ak_vm_thread_end(void) {
	ak_co *co = &ak_vm_main;
	AK_ASSERT(co->nframes == 0);
	ak_free(co->regs);
	ak_free(co->frames);
	ak_free(co->untracks);
	*co = (ak_co){0};
	ak_vm_cur = NULL;
}

#define MAX_FRAMES AK_VM_MAX_FRAMES

// ---- Frames ----

bool ak_vm_push(ak_co *co, const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs, uint16_t dst, bool boundary, uint32_t callpc) {
	if (co->nframes >= MAX_FRAMES) {
		ak_throw_msg("stack overflow in %s", f->name->data);
		return false;
	}
	if (nargs != f->nparams) ak_panic("vm: %s takes %u arguments, called with %u", f->name->data, f->nparams, nargs);
	uint32_t base = co->top;
	if (!co->regs || base + f->nregs > co->cap) {  // registers from its first call, even one with none
		// A task's coroutine starts small (most run a call or two deep
		// and end); a thread's main one, which every call from C runs
		// on, at its size for good.
		uint32_t c = co->cap ? co->cap * 2 : co->task ? 64 : 1024;
		while (c < base + f->nregs) c *= 2;
		// args may point into the registers being moved.
		ptrdiff_t off = (args && co->regs && args >= co->regs && args < co->regs + co->cap) ? args - co->regs : -1;
		co->regs = ak_realloc(co->regs, sizeof(ak_val) * c);
		co->cap = c;
		if (off >= 0) args = co->regs + off;
	}
	ak_val *R = co->regs + base;
	// Its reference registers start as none (push_fast says why only
	// those).
	for (uint32_t j = 0; j < f->nrefs; j++) R[f->refs[j]].p = NULL;
	uint8_t inout = 0;
	uint32_t at = 0;
	ak_val *argp = NULL;
	uint32_t i = 0;
	if ((f->flags & AK_F_MUT) && nargs) {
		// A mut fn's receiver moves in, and back where it came from as
		// the frame ends: a register of the coroutine's (by its index,
		// which stays as the registers grow), or C's.
		R[0] = args[0];
		if (f->kinds[0] == AK_K_REF) args[0].p = NULL;
		if (co->regs && args >= co->regs && args < co->regs + co->cap) {
			inout = AK_INOUT_REG;
			at = (uint32_t)(args - co->regs);
		} else {
			inout = AK_INOUT_PTR;
			argp = args;
		}
		i = 1;
	}
	for (; i < nargs; i++) {
		R[i] = args[i];
		ak_retain_val(R[i], (ak_kind)f->kinds[i]);
	}
	co->top = base + f->nregs;
	if (co->nframes == co->fcap) {
		co->fcap = co->fcap ? co->fcap * 2 : co->task ? 8 : 64;
		co->frames = ak_realloc(co->frames, sizeof(ak_frame) * co->fcap);
	}
	if (clos) ak_retain(clos);
	co->frames[co->nframes++] = (ak_frame){.fn = f, .clos = clos, .pc = 0, .callpc = callpc, .base = base, .udepth = co->nuntracks, .dst = dst, .boundary = boundary, .inout = inout, .at = at, .argp = argp};
	return true;
}

// call_native_mut calls a compiled mut fn (AK_F_MUT) with the argument
// block at the coroutine's register at: it's given a copy of the block,
// so that the receiver it puts back as it ends lands in that register
// wherever the call moved the registers to.
static ak_val call_native_mut(ak_co *co, const ak_func *g, uint32_t at, uint32_t n) {
	ak_val buf[16];
	ak_val *A = n <= 16 ? buf : ak_alloc(sizeof(ak_val) * n);
	memcpy(A, co->regs + at, sizeof(ak_val) * n);
	if (g->kinds[0] == AK_K_REF) co->regs[at].p = NULL;
	ak_val v = g->native(A, NULL);
	co->regs[at] = A[0];
	if (A != buf) ak_free(A);
	return v;
}

void ak_vm_pop(ak_co *co, int keep) {
	ak_frame *fr = &co->frames[co->nframes - 1];
	ak_val *R = co->regs + fr->base;
	const ak_func *f = fr->fn;
	uint32_t i = 0;
	if (fr->inout) {
		// A mut fn's receiver goes back to its caller's register (and
		// stays the result's too when it's returned).
		ak_val *to = fr->inout == AK_INOUT_REG ? co->regs + fr->at : fr->argp;
		*to = R[0];
		if (keep == 0) ak_retain_val(R[0], (ak_kind)f->kinds[0]);
		i = 1;
	}
	for (uint32_t j = 0; j < f->nrefs; j++) {
		uint32_t r = f->refs[j];
		if (r >= i && (int)r != keep) ak_release(R[r].p);
	}
	co->top = fr->base;
	ak_closure *c = fr->clos;
	co->nframes--;
	if (c) ak_release(c);
}

int64_t ak_vm_find_handler(const ak_func *f, uint32_t pc) {
	// The innermost handler is the one listed first (the compiler emits
	// inner ranges before outer ones).
	for (uint32_t i = 0; i < f->nhandlers; i++)
		if (pc >= f->handlers[i].start && pc < f->handlers[i].end) return f->handlers[i].target;
	return -1;
}

static uint32_t line_of(const ak_func *f, uint32_t pc) {
	uint32_t line = 0;
	for (uint32_t i = 0; i < f->nlines && f->lines[i].pc <= pc; i++) line = f->lines[i].line;
	return line;
}

// inline_at is the innermost function body inlined at pc in f (the
// optimizer's: they're listed innermost first), or NULL.
static const ak_inline *inline_at(const ak_func *f, uint32_t pc) {
	for (uint32_t i = 0; i < f->ninlines; i++)
		if (pc >= f->inlines[i].start && pc < f->inlines[i].end) return &f->inlines[i];
	return NULL;
}

void ak_vm_traceback(char *buf, size_t cap) {
	size_t n = 0;
	buf[0] = 0;
	ak_co *co = cur_co();
	for (uint32_t i = co->nframes; i-- > 0 && n < cap;) {
		ak_frame *fr = &co->frames[i];
		// A function compiled to C tells its line as an error leaves it
		// (ak_frame_hook): its frame's pc is where it last left.
		if (fr->fn->resume) continue;
		uint32_t pc = fr->pc ? fr->pc - 1 : 0, line = line_of(fr->fn, pc);
		// The functions inlined where it is: their frames, innermost
		// first, as if they were called.
		bool full = false;
		for (uint32_t k = 0; k < fr->fn->ninlines && !full; k++) {
			const ak_inline *in = &fr->fn->inlines[k];
			if (pc < in->start || pc >= in->end) continue;
			const ak_func *g = &fr->fn->mod->funcs[in->func];
			int w = snprintf(buf + n, cap - n, "%s (%s:%u)\n", g->name->data, g->file->data, line);
			if (w < 0 || (size_t)w >= cap - n) full = true;
			else n += (size_t)w;
			line = in->line;
		}
		if (full) break;
		int w = snprintf(buf + n, cap - n, "%s (%s:%u)\n", fr->fn->name->data, fr->fn->file->data, line);
		if (w < 0 || (size_t)w >= cap - n) break;
		n += (size_t)w;
	}
}

// ak_vm_here names where the VM is, as its source says it: the
// innermost frame of the app's that runs as bytecode, "Grid's on press
// at app.asuka:9" (ak_vm_where's form, at the frame's line).
void ak_vm_here(char *buf, size_t cap) {
	snprintf(buf, cap, "the app");
	ak_co *co = cur_co();
	for (uint32_t i = co->nframes; i-- > 0;) {
		ak_frame *fr = &co->frames[i];
		if (fr->fn->resume) continue;
		uint32_t pc = fr->pc ? fr->pc - 1 : 0;
		const ak_inline *in = inline_at(fr->fn, pc);
		const ak_func *f = in ? &fr->fn->mod->funcs[in->func] : fr->fn;
		const char *name = f->name->data;
		size_t owner = strcspn(name, "/");
		const char *last = strrchr(name, '/');
		const char *what = last ? last + 1 : name;
		uint32_t line = line_of(fr->fn, pc);
		if (name[owner]) snprintf(buf, cap, "%.*s's %s at %s:%u", (int)owner, name, what, f->file->data, line);
		else snprintf(buf, cap, "%s at %s:%u", what, f->file->data, line);
		return;
	}
}

// ---- Interface calls ----

const ak_func *ak_vm_method(void *obj, ak_str *name) {
	const ak_obj *o = obj;
	const ak_shape *s = NULL;
	if (o && o->tag == AK_TAG_STRUCT) s = ((const ak_struct *)o)->shape;
	else if (o && o->tag == AK_TAG_ENUM) s = ((const ak_enum *)o)->shape;
	if (!s || s == &ak_shape_error) return NULL;
	// Shapes the VM loaded carry their methods and module. A method's
	// name is its module's constant (the loader interns it): the same
	// pointer as the call's, unless they're two modules' (a reload's).
	const ak_vm_shape *vs = (const ak_vm_shape *)s;
	for (uint32_t i = 0; i < vs->nmethods; i++)
		if (vs->methods[i].name == name) return &vs->mod->funcs[vs->methods[i].func];
	for (uint32_t i = 0; i < vs->nmethods; i++)
		if (ak_str_eq(vs->methods[i].name, name)) return &vs->mod->funcs[vs->methods[i].func];
	return NULL;
}

// ---- The interpreter ----
//
// Dispatch is threaded where the compiler has computed gotos (GCC,
// Clang: every target's): each instruction's code ends by jumping to the
// next one's, through a table, so each has a branch of its own for the
// CPU to predict. Elsewhere (or with -DAK_VM_SWITCH) it's a switch in a
// loop. Either way an instruction's code knows its width: ip moves past
// it as it starts, and a points at its operands.

#if !defined(AK_VM_SWITCH) && (defined(__GNUC__) || defined(__clang__))
#define AK_VM_THREADED 1
#endif

#define setref ak_op_setref

static inline uint32_t off32(const uint16_t *c) { return (uint32_t)c[0] | (uint32_t)c[1] << 16; }
static inline int32_t joff(const uint16_t *c) { return (int32_t)off32(c); }

#define store(f, R, a, v) ak_op_store((f)->kinds, R, a, v)

// push_fast pushes the frame of a call to a bytecode function (not
// compiled to C), f called with nargs arguments from args, its result to
// go in dst, in the common case: room for the frame and its registers
// (more than they need: a coroutine without any has none), the arguments
// f takes, not a mut fn. Otherwise it does nothing and returns NULL
// (ak_vm_push's general path is the caller's); else the frame. Inlined
// in each call: a call that returns at once costs this, its entry and
// RET's. A boundary frame is a call from C's (call_sync), whose return
// returns to C.
//
// Only the frame's reference registers start as none: the others are
// written before they're read (each holds one kind, and its first use
// is a write), and what's left in them from an earlier frame is never
// released or followed.
static AK_ALWAYS_INLINE ak_frame *push_fast(ak_co *co, const ak_func *f, ak_closure *clos, const ak_val *args, uint32_t nargs, uint16_t dst, bool boundary, uint32_t callpc) {
	uint32_t base = co->top, nf = co->nframes;
	if (AK_UNLIKELY(nf >= MAX_FRAMES || nf == co->fcap || nargs != f->nparams || (f->flags & AK_F_MUT) || base + f->nregs >= co->cap)) return NULL;
	ak_val *R = co->regs + base;
	const uint16_t *refs = f->refs;
	for (uint32_t j = 0, n = f->nrefs; j < n; j++) R[refs[j]].p = NULL;
	const uint8_t *k = f->kinds;
	for (uint32_t i = 0; i < nargs; i++) {
		R[i] = args[i];
		ak_retain_val(R[i], (ak_kind)k[i]);
	}
	co->top = base + f->nregs;
	if (clos) ak_retain(clos);
	ak_frame *fr = &co->frames[nf];
	co->nframes = nf + 1;
	*fr = (ak_frame){.fn = f, .clos = clos, .callpc = callpc, .base = base, .udepth = co->nuntracks, .dst = dst, .boundary = boundary};
	return fr;
}

// The loop (interp_loop.h): interp_run, and in a build with the
// debugger interp_hooked, which calls the hook before each instruction,
// run while the debugger wants to see each one (ak_dbg_on: breakpoints,
// a step, a pause). Each leaves for the other as the flag changes: its
// frame resumes where it left (AK_RUN_PUSHED: the run loop comes back
// here).
#define INTERP_NAME interp_run
#define INTERP_HOOKED 0
#include "vm/interp_loop.h"
#if AK_VM_DEBUGGER
#define INTERP_NAME interp_hooked
#define INTERP_HOOKED 1
#include "vm/interp_loop.h"
#endif

int ak_vm_interp(ak_co *co, ak_val *out) {
#if AK_VM_DEBUGGER
	if (AK_UNLIKELY(atomic_load_explicit(&ak_dbg_on, memory_order_relaxed))) return interp_hooked(co, out);
#endif
	return interp_run(co, out);
}

int ak_vm_await(ak_co *co, ak_future *f) {
	ak_task *t = co->task;
	if (t && t->cancelled) {
		ak_throw_cancel();
		return -1;
	}
	if (f->state != AK_FUT_PENDING) return 0;
	const ak_func *F = co->frames[co->nframes - 1].fn;
	if (!t) ak_panic("vm: await outside an async context, in %s", F->name->data);
	for (uint32_t i = co->nframes; i-- > 1;)
		if (co->frames[i].boundary) ak_panic("vm: await in a synchronous call from C, in %s", F->name->data);
	t->awaiting = ak_retain(f);
	ak_future_wait(f, ak_vm_task_resume, t);
	return 1;
}

void ak_vm_yield(ak_co *co) {
	ak_task *t = co->task;
	if (!t) ak_panic("vm: yield outside a task");
	ak_ready(ak_vm_task_resume, t);
}

// ---- Calls from C ----

ak_ret ak_cg_overflow(const char *fn) {
	ak_throw_msg("stack overflow in %s", fn);
	return (ak_ret){.e = 1};
}

void ak_cg_unwind(const char *fn, const char *file, uint32_t line) {
	if (ak_frame_hook) ak_frame_hook(fn, file, line);
}

void ak_vm_closure_drop(ak_closure *c) { ak_module_release(((const ak_func *)c->fn)->mod); }

static ak_val call_sync(const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs) {
	ak_co *co = cur_co();
	// Each call from C nests the run loop on the C stack. co runs on
	// this thread now (this call is in a run of it, or it's the thread's
	// main one), so its limit, as its error's slot, is this thread's.
	if (AK_UNLIKELY(ak_cg_stack_low(ak_op_lim(co)))) {
		ak_throw_msg("stack overflow in %s", f->name->data);
		return (ak_val){0};
	}
	if (AK_UNLIKELY(!push_fast(co, f, clos, args, nargs, 0xFFFF, true, 0)) && !ak_vm_push(co, f, clos, args, nargs, 0xFFFF, true, 0)) return (ak_val){0};
	if (AK_UNLIKELY(!co->err)) co->err = ak_error_slot();
	ak_val out;
	bool suspended = ak_vm_run_here(co, &out);
	if (suspended) ak_panic("vm: %s suspended in a synchronous call", f->name->data);
	return out;
}

ak_val ak_vm_closure_invoke(ak_closure *c, ak_val *args, uint32_t nargs) {
	const ak_func *f = c->fn;
	if (f->native) return f->native(args, c);
	if (f->flags & AK_F_ASYNC) return (ak_val){.p = ak_vm_task_start(f, c, args, nargs)};
	return call_sync(f, c, args, nargs);
}

ak_str *ak_vm_error_text(void *err) {
	ak_str *name = ak_str_c("message");
	const ak_func *m = ak_vm_method(err, name);
	ak_release(name);
	if (m) {
		ak_val a = {.p = err};
		ak_val v = ak_vm_call(m, &a, 1);
		if (!ak_error_pending() && v.p) return v.p;
		if (ak_error_pending()) ak_release(ak_error_take());
	}
	return ak_retain(ak_error_message(err));
}

bool ak_closure_async(const ak_closure *c) {
	if (c->invoke != ak_vm_closure_invoke) return false;
	const ak_func *f = c->fn;
	return !f->native && (f->flags & AK_F_ASYNC);
}

ak_val ak_vm_call(const ak_func *f, ak_val *args, uint32_t nargs) {
	if (f->flags & AK_F_ASYNC) {
		// Its result, if it finished; its error, if it failed; else none
		// while it runs on as a task.
		ak_future *fut = ak_vm_task_start(f, NULL, args, nargs);
		ak_val v = {0};
		if (fut->state != AK_FUT_PENDING) v = ak_future_take(fut);
		ak_release(fut);
		return v;
	}
	if (f->native) return f->native(args, NULL);
	return call_sync(f, NULL, args, nargs);
}

ak_closure *ak_vm_closure(const ak_func *f) {
	ak_closure *c = ak_closure_new(ak_vm_closure_invoke, f, 0, NULL);
	c->sig = f->sig;
	ak_module_retain(f->mod);
	c->drop = ak_vm_closure_drop;
	return c;
}

void ak_vm_run_ready(void) { ak_sched_run(); }
bool ak_vm_has_ready(void) { return ak_sched_has_ready(); }
