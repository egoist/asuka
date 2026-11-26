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
		uint32_t c = co->cap ? co->cap * 2 : 1024;
		while (c < base + f->nregs) c *= 2;
		// args may point into the registers being moved.
		ptrdiff_t off = (args && co->regs && args >= co->regs && args < co->regs + co->cap) ? args - co->regs : -1;
		co->regs = ak_realloc(co->regs, sizeof(ak_val) * c);
		co->cap = c;
		if (off >= 0) args = co->regs + off;
	}
	ak_val *R = co->regs + base;
	if (f->nregs) memset(R, 0, sizeof(ak_val) * f->nregs);
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
		co->fcap = co->fcap ? co->fcap * 2 : 64;
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

// push_frame is ak_vm_push for a call from the interpreter: a bytecode
// function (not compiled to C) called with nargs arguments from the
// caller's registers args, its result to go in dst.
static inline bool push_frame(ak_co *co, const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs, uint16_t dst, uint32_t callpc) {
	if (AK_UNLIKELY(co->nframes >= MAX_FRAMES || nargs != f->nparams || co->nframes == co->fcap || (f->flags & AK_F_MUT) || co->top + f->nregs > co->cap))
		return ak_vm_push(co, f, clos, args, nargs, dst, false, callpc);
	uint32_t base = co->top;
	ak_val *R = co->regs + base;
	const uint8_t *k = f->kinds;
	uint32_t i = 0;
	for (; i < nargs; i++) {
		R[i] = args[i];
		ak_retain_val(R[i], (ak_kind)k[i]);
	}
	if (f->nregs > nargs) memset(R + nargs, 0, sizeof(ak_val) * (f->nregs - nargs));
	co->top = base + f->nregs;
	if (clos) ak_retain(clos);
	co->frames[co->nframes++] = (ak_frame){.fn = f, .clos = clos, .callpc = callpc, .base = base, .udepth = co->nuntracks, .dst = dst};
	return true;
}

int ak_vm_interp(ak_co *co, ak_val *out) {
	ak_frame *fr;
	const ak_func *F;
	const uint16_t *code, *ip, *a;
	ak_val *R;
	ak_module *M;
	// The pending error, read in place: what CHECK_ERR asks after a call.
	void *const *err = ak_error_slot();

#define LOAD() do { \
		fr = &co->frames[co->nframes - 1]; \
		F = fr->fn; \
		code = F->code; \
		R = co->regs + fr->base; \
		M = F->mod; \
		ip = code + fr->pc; \
		DEBUG_CHECK(); \
	} while (0)
	// After anything that may run more VM code (and grow the stacks).
#define RELOAD() do { \
		fr = &co->frames[co->nframes - 1]; \
		R = co->regs + fr->base; \
	} while (0)
	// Where the frame resumes, saved before a call (the pc after it).
#define SAVE() (fr->pc = (uint32_t)(ip - code))
	// A jump taken (a loop's way round): the debugger may want to see it.
#define JUMP(off) do { ip += (off); DEBUG_CHECK(); } while (0)
	// The instruction running's pc, for its handlers and its line.
#define IPC() ((uint32_t)(a - 1 - code))
#define CHECK_ERR() do { if (AK_UNLIKELY(*err != NULL)) goto unwind; } while (0)

	// The debugger's hook (debug.h), in a development build. Threaded,
	// dispatch goes through one of two tables: the instructions' own, or,
	// while the debugger wants to see each instruction (ak_dbg_on:
	// breakpoints, a step, a pause), one whose every entry is the hook,
	// which then runs the instruction. Which one is decided again as a
	// frame is entered or left, a jump is taken (a loop goes round) and
	// the hook returns: nothing is asked per instruction. With a switch,
	// each instruction asks the flag.
#if AK_VM_DEBUGGER
#define DEBUG_ON() atomic_load_explicit(&ak_dbg_on, memory_order_relaxed)
#define DEBUG_HOOK() if (AK_UNLIKELY(DEBUG_ON())) goto debug_hook
#else
#define DEBUG_HOOK() (void)0
#endif

#if AK_VM_THREADED
#define LABEL(name) &&L_##name,
	static const void *const dispatch[AK_OP__COUNT] = {AK_OP_LIST(LABEL)};
#undef LABEL
#if AK_VM_DEBUGGER
#define LABEL(name) &&debug_hook,
	static const void *const debug_dispatch[AK_OP__COUNT] = {AK_OP_LIST(LABEL)};
#undef LABEL
	const void *const *disp = dispatch;
#define DEBUG_CHECK() (disp = AK_UNLIKELY(DEBUG_ON()) ? debug_dispatch : dispatch)
#define DISPATCH disp
#else
#define DEBUG_CHECK() (void)0
#define DISPATCH dispatch
#endif
#define OP(name) L_##name: ip += AK_OPW_##name;
#define NEXT() do { a = ip + 1; goto *DISPATCH[*ip]; } while (0)
#else
#define DEBUG_CHECK() (void)0
#define OP(name) case AK_OP_##name: ip += AK_OPW_##name;
#define NEXT() goto next
#endif

	LOAD();
#if AK_VM_THREADED
	NEXT();
#else
next:
	a = ip + 1;
	DEBUG_HOOK();
#if AK_VM_DEBUGGER
run:
#endif
	switch (*ip) {
#endif
	OP(NOP)
		NEXT();
	OP(MOV)
		R[a[0]] = R[a[1]];
		NEXT();
	OP(MOVR)
		if (a[0] != a[1]) setref(R, a[0], ak_retain(R[a[1]].p));
		NEXT();
	OP(CLR)
		setref(R, a[0], NULL);
		NEXT();
	OP(KINT)
		R[a[0]].i = M->ints[off32(a + 1)];
		NEXT();
	OP(KSMALL)
		R[a[0]].i = (int16_t)a[1];
		NEXT();
	OP(KFLOAT)
		R[a[0]].f = M->floats[off32(a + 1)];
		NEXT();
	OP(KSTR)
		setref(R, a[0], ak_retain(M->strings[off32(a + 1)]));
		NEXT();
	OP(KNONE)
		setref(R, a[0], NULL);
		NEXT();
	OP(GGET) {
		ak_val *g = &M->globals[off32(a + 1)];
		if (a[3] == AK_K_REF) setref(R, a[0], ak_retain(g->p));
		else R[a[0]] = *g;
		NEXT();
	}
	OP(GSET) {
		ak_val *g = &M->globals[off32(a)];
		if (a[3] == AK_K_REF) {
			void *old = g->p;
			g->p = ak_retain(R[a[2]].p);
			ak_release(old);
		} else {
			*g = R[a[2]];
		}
		NEXT();
	}
	OP(GTAKE) {
		ak_val *g = &M->globals[off32(a + 1)];
		setref(R, a[0], g->p);
		g->p = NULL;
		NEXT();
	}

	OP(ADDI)
		if (!ak_op_addi(R, a[0], a[1], a[2])) goto thrown;
		NEXT();
	OP(SUBI)
		if (!ak_op_subi(R, a[0], a[1], a[2])) goto thrown;
		NEXT();
	OP(MULI)
		if (!ak_op_muli(R, a[0], a[1], a[2])) goto thrown;
		NEXT();
	OP(DIVI)
		if (!ak_op_divi(R, a[0], a[1], a[2], false)) goto thrown;
		NEXT();
	OP(MODI)
		if (!ak_op_divi(R, a[0], a[1], a[2], true)) goto thrown;
		NEXT();
	OP(NEGI)
		if (!ak_op_negi(R, a[0], a[1])) goto thrown;
		NEXT();
	OP(WADDI) R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i + (uint64_t)R[a[2]].i); NEXT();
	OP(WSUBI) R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i - (uint64_t)R[a[2]].i); NEXT();
	OP(WMULI) R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i * (uint64_t)R[a[2]].i); NEXT();
	OP(ADDIK) R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i + (uint64_t)(int64_t)(int16_t)a[2]); NEXT();
	OP(EQI) R[a[0]].i = R[a[1]].i == R[a[2]].i; NEXT();
	OP(NEI) R[a[0]].i = R[a[1]].i != R[a[2]].i; NEXT();
	OP(LTI) R[a[0]].i = R[a[1]].i < R[a[2]].i; NEXT();
	OP(LEI) R[a[0]].i = R[a[1]].i <= R[a[2]].i; NEXT();
	OP(BAND) R[a[0]].i = R[a[1]].i & R[a[2]].i; NEXT();
	OP(BOR) R[a[0]].i = R[a[1]].i | R[a[2]].i; NEXT();
	OP(BXOR) R[a[0]].i = R[a[1]].i ^ R[a[2]].i; NEXT();
	OP(BNOT) R[a[0]].i = ~R[a[1]].i; NEXT();
	OP(SHL)
		if (!ak_op_shift(R, a[0], a[1], a[2], false)) goto thrown;
		NEXT();
	OP(SHR)
		if (!ak_op_shift(R, a[0], a[1], a[2], true)) goto thrown;
		NEXT();
	OP(POWI)
		if (!ak_op_pown(R, a[0], a[1], a[2], AK_I64 | AK_I64 << 4)) goto thrown;
		NEXT();

	OP(ADDN)
		if (!ak_op_addn(R, a[0], a[1], a[2], a[3])) goto thrown;
		NEXT();
	OP(SUBN)
		if (!ak_op_subn(R, a[0], a[1], a[2], a[3])) goto thrown;
		NEXT();
	OP(MULN)
		if (!ak_op_muln(R, a[0], a[1], a[2], a[3])) goto thrown;
		NEXT();
	OP(DIVN)
		if (!ak_op_divn(R, a[0], a[1], a[2], a[3], false)) goto thrown;
		NEXT();
	OP(MODN)
		if (!ak_op_divn(R, a[0], a[1], a[2], a[3], true)) goto thrown;
		NEXT();
	OP(NEGN)
		if (!ak_op_negn(R, a[0], a[1], a[2])) goto thrown;
		NEXT();
	OP(POWN)
		if (!ak_op_pown(R, a[0], a[1], a[2], a[3])) goto thrown;
		NEXT();
	OP(SHLN)
		if (!ak_op_shiftn(R, a[0], a[1], a[2], a[3], false)) goto thrown;
		NEXT();
	OP(SHRN)
		if (!ak_op_shiftn(R, a[0], a[1], a[2], a[3], true)) goto thrown;
		NEXT();
	OP(LTU) R[a[0]].i = (uint64_t)R[a[1]].i < (uint64_t)R[a[2]].i; NEXT();
	OP(LEU) R[a[0]].i = (uint64_t)R[a[1]].i <= (uint64_t)R[a[2]].i; NEXT();
	OP(WRAP) R[a[0]].i = ak_int_wrap(R[a[1]].i, a[2]); NEXT();
	OP(CONVN)
		if (!ak_op_convn(R, a[0], a[1], a[2])) goto thrown;
		NEXT();
	OP(CLAMPN) R[a[0]].i = ak_int_clamp(R[a[1]].i, a[2] >> 4, a[2] & 15); NEXT();
	OP(F2N) R[a[0]].i = ak_op_f2n(R[a[1]].f, a[2]); NEXT();
	OP(U2F) R[a[0]].f = (double)(uint64_t)R[a[1]].i; NEXT();

	OP(ADDF) R[a[0]].f = R[a[1]].f + R[a[2]].f; NEXT();
	OP(SUBF) R[a[0]].f = R[a[1]].f - R[a[2]].f; NEXT();
	OP(MULF) R[a[0]].f = R[a[1]].f * R[a[2]].f; NEXT();
	OP(DIVF) R[a[0]].f = R[a[1]].f / R[a[2]].f; NEXT();
	OP(MODF) R[a[0]].f = fmod(R[a[1]].f, R[a[2]].f); NEXT();
	OP(POWF) R[a[0]].f = pow(R[a[1]].f, R[a[2]].f); NEXT();
	OP(NEGF) R[a[0]].f = -R[a[1]].f; NEXT();
	OP(EQF) R[a[0]].i = R[a[1]].f == R[a[2]].f; NEXT();
	OP(NEF) R[a[0]].i = R[a[1]].f != R[a[2]].f; NEXT();
	OP(LTF) R[a[0]].i = R[a[1]].f < R[a[2]].f; NEXT();
	OP(LEF) R[a[0]].i = R[a[1]].f <= R[a[2]].f; NEXT();
	OP(I2F) R[a[0]].f = (double)R[a[1]].i; NEXT();
	OP(F2I) R[a[0]].i = ak_op_f2i(R[a[1]].f); NEXT();
	OP(NOT) R[a[0]].i = !R[a[1]].i; NEXT();

	OP(EQ) R[a[0]].i = ak_val_eq(R[a[1]], R[a[2]], (ak_kind)a[3]); NEXT();
	OP(NE) R[a[0]].i = !ak_val_eq(R[a[1]], R[a[2]], (ak_kind)a[3]); NEXT();
	OP(ISNONE) R[a[0]].i = R[a[1]].p == NULL; NEXT();

	OP(CONCAT) ak_op_concat(R, a[0], a[1], a[2]); NEXT();
	OP(TOSTR) setref(R, a[0], ak_op_tostr(R[a[1]], a[2])); NEXT();
	OP(INTERP) setref(R, a[0], ak_op_interp(R, a[1], a[2], M->strings[off32(a + 3)])); NEXT();
	OP(LTS) R[a[0]].i = ak_str_cmp(R[a[1]].p, R[a[2]].p) < 0; NEXT();

	OP(JMP) JUMP(joff(a)); NEXT();
	OP(JT) if (R[a[0]].i) JUMP(joff(a + 1)); NEXT();
	OP(JF) if (!R[a[0]].i) JUMP(joff(a + 1)); NEXT();
	OP(JNONE) if (!R[a[0]].p) JUMP(joff(a + 1)); NEXT();
	OP(JSOME) if (R[a[0]].p) JUMP(joff(a + 1)); NEXT();
	OP(ISSHAPE) R[a[0]].i = ak_op_isshape(R[a[1]].p, &M->shapes[off32(a + 2)].s); NEXT();

	OP(CALL) {
		const ak_func *g = &M->funcs[off32(a + 1)];
		SAVE();
		if (g->native) {
			ak_val v = g->flags & AK_F_MUT ? call_native_mut(co, g, (uint32_t)(R + a[3] - co->regs), a[4]) : g->native(R + a[3], NULL);
			RELOAD();
			CHECK_ERR();
			if (a[0] == 0xFFFF) ak_release_val(v, (ak_kind)g->ret);
			else store(F, R, a[0], v);
			NEXT();
		}
		if (!push_frame(co, g, NULL, R + a[3], a[4], a[0], IPC())) goto unwind;
		LOAD();
		if (F->resume) return AK_RUN_PUSHED;
		NEXT();
	}
	OP(CALLC) {
		ak_closure *c = R[a[1]].p;
		if (!c) ak_panic("vm: calling none");
		SAVE();
		if (ak_op_pushes(co, c) && (F->flags & AK_F_ASYNC || !(((const ak_func *)c->fn)->flags & AK_F_ASYNC))) {
			// A VM closure: call it in this loop. An async one runs on
			// this task only when the caller may suspend too: a
			// function that can't (compiled to C, a plain C function)
			// starts it as a task of its own.
			if (!push_frame(co, c->fn, c, R + a[2], a[3], a[0], IPC())) goto unwind;
			LOAD();
			if (F->resume) return AK_RUN_PUSHED;
		} else {
			ak_val v = c->invoke(c, R + a[2], a[3]);
			RELOAD();
			CHECK_ERR();
			store(F, R, a[0], v);
		}
		NEXT();
	}
	OP(CALLB) {
		SAVE();
		ak_val v = M->builtins[a[1]](R + a[2], a[3]);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(CALLI) {
		ak_str *name = M->strings[off32(a + 1)];
		void *obj = R[a[3]].p;
		const ak_func *g = ak_vm_method(obj, name);
		if (!g) ak_panic("vm: no method %s", name->data);
		SAVE();
		if (g->native) {
			ak_val v = g->flags & AK_F_MUT ? call_native_mut(co, g, (uint32_t)(R + a[3] - co->regs), a[4]) : g->native(R + a[3], NULL);
			RELOAD();
			CHECK_ERR();
			if (a[0] == 0xFFFF) ak_release_val(v, (ak_kind)g->ret);
			else store(F, R, a[0], v);
			NEXT();
		}
		if (!push_frame(co, g, NULL, R + a[3], a[4], a[0], IPC())) goto unwind;
		LOAD();
		if (F->resume) return AK_RUN_PUSHED;
		NEXT();
	}
	OP(RET) {
		ak_val v = R[a[0]];
		bool ref = F->kinds[a[0]] == AK_K_REF;
		bool boundary = fr->boundary;
		uint16_t dst = fr->dst;
		if (AK_LIKELY(!fr->inout)) {
			// ak_vm_pop, inline: the frame's references but the result.
			for (uint32_t j = 0, n = F->nrefs; j < n; j++) {
				uint32_t r = F->refs[j];
				if (r != a[0] || !ref) ak_release(R[r].p);
			}
			ak_closure *c = fr->clos;
			co->top = fr->base;
			co->nframes--;
			if (c) ak_release(c);
		} else {
			ak_vm_pop(co, ref ? a[0] : -1);
		}
		if (boundary || co->nframes == 0) {
			*out = v;
			return AK_RUN_RET;
		}
		LOAD();
		if (dst == 0xFFFF) {
			if (ref) ak_release(v.p);
		} else {
			store(F, R, dst, v);
		}
		if (F->resume) return AK_RUN_PUSHED;
		NEXT();
	}
	OP(RET0) {
		bool boundary = fr->boundary;
		uint16_t dst = fr->dst;
		ak_vm_pop(co, -1);
		if (boundary || co->nframes == 0) {
			*out = (ak_val){0};
			return AK_RUN_RET;
		}
		LOAD();
		if (dst != 0xFFFF) store(F, R, dst, (ak_val){0});
		if (F->resume) return AK_RUN_PUSHED;
		NEXT();
	}
	OP(THROW) {
		void *e = R[a[0]].p;
		R[a[0]].p = NULL;
		// Where it's thrown, for its trace: this instruction.
		fr->pc = IPC() + 1;
		ak_throw(e);
		goto unwind;
	}
	OP(CATCH)
		setref(R, a[0], ak_error_take());
		NEXT();
	OP(RETHROW) {
		void *e = R[a[0]].p;
		R[a[0]].p = NULL;
		ak_throw(e);
		// Handlers of the same function don't catch it again: look
		// from the caller on.
		goto unwind_frame;
	}

	OP(CLOS) setref(R, a[0], ak_op_clos(&M->funcs[off32(a + 1)], R + a[3])); NEXT();
	OP(CAP) ak_op_cap(R, a[0], fr->clos, a[1]); NEXT();
	OP(VBOX) ak_op_vbox(R, a[0], R[a[1]], a[2]); NEXT();
	OP(VGET) ak_op_vget(R, a[0], R[a[1]].p); NEXT();
	OP(VSET) ak_op_vset(R[a[0]].p, R[a[1]]); NEXT();
	OP(VTAKE) ak_op_vtake(R, a[0], R[a[1]].p); NEXT();
	OP(NEWS) setref(R, a[0], ak_op_news(&M->shapes[off32(a + 1)].s, R + a[3])); NEXT();
	OP(GETF) ak_op_getf(R, a[0], R[a[1]].p, a[2]); NEXT();
	OP(SETF) ak_op_setf(R, a[0], a[1], R[a[2]]); NEXT();
	OP(NEWE) setref(R, a[0], ak_op_newe(&M->shapes[off32(a + 1)].s, a[3], R + a[4])); NEXT();
	OP(ECASE) R[a[0]].i = ak_enum_case(R[a[1]].p); NEXT();
	OP(GETP) ak_op_getp(R, a[0], R[a[1]].p, a[2]); NEXT();
	OP(BOX) setref(R, a[0], ak_box_new((ak_kind)a[2], R[a[1]])); NEXT();
	OP(UNBOX) R[a[0]] = ((ak_box *)R[a[1]].p)->v; NEXT();
	OP(NEWA) {
		uint32_t s = off32(a + 2);
		setref(R, a[0], ak_op_newa((ak_kind)a[1], s == 0xFFFFFFFF ? NULL : &M->shapes[s].s, R + a[4], a[5]));
		NEXT();
	}
	OP(IDX)
		if (!ak_op_idx(R, a[0], R[a[1]].p, R[a[2]].i)) goto thrown;
		NEXT();
	OP(SETIDX)
		if (!ak_op_setidx(R, a[0], R[a[1]].i, R[a[2]])) goto thrown;
		NEXT();
	OP(LEN) R[a[0]].i = ak_op_len(R[a[1]].p); NEXT();
	OP(NEWM) setref(R, a[0], ak_op_newm((ak_kind)a[1], (ak_kind)a[2], R + a[3], a[4])); NEXT();
	OP(MGET) setref(R, a[0], ak_op_mget(R[a[1]].p, R[a[2]])); NEXT();
	OP(MSET) ak_op_mset(R, a[0], R[a[1]], R[a[2]]); NEXT();
	OP(MGETJ)
		if (!ak_op_mgetj(R, a[0], R[a[1]].p, R[a[2]])) JUMP(joff(a + 3));
		NEXT();

	OP(CELL) {
		ak_val v = R[a[1]];
		ak_retain_val(v, (ak_kind)a[2]);
		SAVE();
		ak_val c = ak_vm_cell_new(v, (ak_kind)a[2], NULL);
		RELOAD();
		setref(R, a[0], c.p);
		NEXT();
	}
	OP(CELLK) {
		ak_val v = R[a[1]];
		ak_retain_val(v, (ak_kind)a[2]);
		SAVE();
		ak_val c = ak_vm_cell_new(v, (ak_kind)a[2], M->strings[off32(a + 3)]);
		RELOAD();
		setref(R, a[0], c.p);
		NEXT();
	}
	OP(FOLLOWK) {
		SAVE();
		ak_val c = ak_vm_follow_new(R[a[1]].p, (ak_kind)a[2], M->strings[off32(a + 3)], a[5] != 0);
		RELOAD();
		CHECK_ERR();
		setref(R, a[0], c.p);
		NEXT();
	}
	OP(CGET) {
		SAVE();
		ak_val v = ak_vm_cell_get(R[a[1]].p, true);
		RELOAD();
		CHECK_ERR();  // a follow's initializer's error
		store(F, R, a[0], v);
		NEXT();
	}
	OP(CPEEK) {
		SAVE();
		ak_val v = ak_vm_cell_get(R[a[1]].p, false);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(CSET) {
		ak_val v = R[a[1]];
		ak_retain_val(v, (ak_kind)F->kinds[a[1]]);
		SAVE();
		ak_vm_cell_set(R[a[0]].p, v);
		RELOAD();
		CHECK_ERR();
		NEXT();
	}
	OP(CTAKE)
		store(F, R, a[0], ak_vm_cell_take(R[a[1]].p));
		NEXT();
	OP(CPUT) {
		ak_val v = R[a[1]];
		if (F->kinds[a[1]] == AK_K_REF) R[a[1]].p = NULL;  // moved
		SAVE();
		ak_vm_cell_put(R[a[0]].p, v);
		RELOAD();
		CHECK_ERR();
		NEXT();
	}
	OP(CPATH) {
		const ak_str *path = M->strings[off32(a + 2)];
		SAVE();
		ak_val v = ak_vm_cell_path(R[a[1]].p, (const uint8_t *)path->data, path->len, R + a[4], false);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(CLEN) {
		const ak_str *path = M->strings[off32(a + 2)];
		SAVE();
		ak_val v = ak_vm_cell_path(R[a[1]].p, (const uint8_t *)path->data, path->len, R + a[4], true);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(CPUTP) {
		ak_val v = R[a[1]];
		if (F->kinds[a[1]] == AK_K_REF) R[a[1]].p = NULL;  // moved
		const ak_str *path = M->strings[off32(a + 2)];
		SAVE();
		ak_vm_cell_putp(R[a[0]].p, v, (const uint8_t *)path->data, path->len, R + a[4]);
		RELOAD();
		CHECK_ERR();
		NEXT();
	}
	OP(MEMO) {
		SAVE();
		ak_val m = ak_vm_memo_new(R[a[1]].p, (ak_kind)a[2]);
		RELOAD();
		setref(R, a[0], m.p);
		NEXT();
	}
	OP(MREAD) {
		SAVE();
		ak_val v = ak_vm_memo_get(R[a[1]].p);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(ACCV) {
		ak_val v = R[a[1]];
		ak_retain_val(v, (ak_kind)a[2]);
		setref(R, a[0], ak_vm_acc_value(v, (ak_kind)a[2]).p);
		NEXT();
	}
	OP(ACCF)
		setref(R, a[0], ak_vm_acc_func(R[a[1]].p, (ak_kind)a[2]).p);
		NEXT();
	OP(ACCC)
		setref(R, a[0], ak_vm_acc_cell(R[a[1]].p).p);
		NEXT();
	OP(AGET) {
		SAVE();
		ak_val v = ak_vm_acc_get(R[a[1]].p);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(ACCP) {
		const ak_str *path = M->strings[off32(a + 2)];
		setref(R, a[0], ak_vm_acc_path(R[a[1]].p, (const uint8_t *)path->data, path->len, R + a[4], (ak_kind)a[5]).p);
		NEXT();
	}
	OP(AGETP) {
		const ak_str *path = M->strings[off32(a + 2)];
		SAVE();
		ak_val v = ak_vm_acc_getp(R[a[1]].p, (const uint8_t *)path->data, path->len, R + a[4]);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(CALLU) {
		SAVE();
		ak_val v = ak_vm_call_untracked(R[a[1]].p, R + a[2], a[3]);
		RELOAD();
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(UNTRACK) ak_vm_untrack_begin(co); NEXT();
	OP(RETRACK) ak_vm_retrack(co, fr->udepth + a[0]); NEXT();
	OP(EFFECT)
		SAVE();
		ak_vm_effect(R[a[0]].p, false);
		RELOAD();
		CHECK_ERR();
		NEXT();
	OP(RENDER)
		SAVE();
		ak_vm_effect(R[a[0]].p, true);
		RELOAD();
		CHECK_ERR();
		NEXT();
	OP(CLEANUP)
		ak_vm_cleanup(R[a[0]].p);
		NEXT();
	OP(BATCH)
		SAVE();
		ak_vm_batch(a[0] != 0);
		RELOAD();
		CHECK_ERR();
		NEXT();

	OP(TASK) {
		ak_closure *c = R[a[1]].p;
		SAVE();
		ak_val f = ak_op_task(c);
		RELOAD();
		CHECK_ERR();
		setref(R, a[0], f.p);
		NEXT();
	}
	OP(AWAIT) {
		ak_future *f = R[a[1]].p;
		int w = ak_vm_await(co, f);
		if (w < 0) goto unwind;
		if (w > 0) {
			// Suspend; AWAIT runs again when resumed, with f done.
			fr->pc = IPC();
			return AK_RUN_SUSPEND;
		}
		ak_val v = ak_future_take(f);
		CHECK_ERR();
		store(F, R, a[0], v);
		NEXT();
	}
	OP(YIELD)
		ak_vm_yield(co);
		SAVE();
		return AK_RUN_SUSPEND;

	// The optimizer's (ops.h says what each replaces).
	OP(ADDIS)
		if (!ak_op_addk(R, a[0], R[a[1]].i, (int16_t)a[2])) goto thrown;
		NEXT();
	OP(SUBIS)
		if (!ak_op_subk(R, a[0], R[a[1]].i, (int16_t)a[2])) goto thrown;
		NEXT();
	OP(MULIS)
		if (!ak_op_mulk(R, a[0], R[a[1]].i, (int16_t)a[2])) goto thrown;
		NEXT();
	OP(DIVIS) R[a[0]].i = R[a[1]].i / (int16_t)a[2]; NEXT();
	OP(MODIS) R[a[0]].i = R[a[1]].i % (int16_t)a[2]; NEXT();
	OP(DIVIK) R[a[0]].i = R[a[1]].i / M->ints[off32(a + 2)]; NEXT();
	OP(MODIK) R[a[0]].i = R[a[1]].i % M->ints[off32(a + 2)]; NEXT();
	OP(JLTI) if (R[a[0]].i < R[a[1]].i) JUMP(joff(a + 2)); NEXT();
	OP(JLEI) if (R[a[0]].i <= R[a[1]].i) JUMP(joff(a + 2)); NEXT();
	OP(JEQI) if (R[a[0]].i == R[a[1]].i) JUMP(joff(a + 2)); NEXT();
	OP(JNEI) if (R[a[0]].i != R[a[1]].i) JUMP(joff(a + 2)); NEXT();
	OP(JLTIS) if (R[a[0]].i < (int16_t)a[1]) JUMP(joff(a + 2)); NEXT();
	OP(JLEIS) if (R[a[0]].i <= (int16_t)a[1]) JUMP(joff(a + 2)); NEXT();
	OP(JGTIS) if (R[a[0]].i > (int16_t)a[1]) JUMP(joff(a + 2)); NEXT();
	OP(JGEIS) if (R[a[0]].i >= (int16_t)a[1]) JUMP(joff(a + 2)); NEXT();
	OP(JEQIS) if (R[a[0]].i == (int16_t)a[1]) JUMP(joff(a + 2)); NEXT();
	OP(JNEIS) if (R[a[0]].i != (int16_t)a[1]) JUMP(joff(a + 2)); NEXT();
	OP(IDXF)
		if (!ak_op_idxf(R, a[0], R[a[1]].p, R[a[2]].i, a[3])) goto thrown;
		NEXT();
	OP(SETIF)
		if (!ak_op_setif(R, a[0], R[a[1]].i, a[2], R[a[3]])) goto thrown;
		NEXT();

#if !AK_VM_THREADED
	default:
		ak_panic("vm: bad opcode %u in %s at %u", *ip, F->name->data, IPC());
	}
#endif

#if AK_VM_DEBUGGER
debug_hook:
	// It may stop here, and wait; it runs none of the app's code. Then
	// the instruction runs.
	ak_dbg_hook(co, (uint32_t)(ip - code));
	RELOAD();
#if AK_VM_THREADED
	DEBUG_CHECK();
	goto *dispatch[*ip];
#else
	goto run;
#endif
#endif

thrown:
	// The running instruction threw (not a call it made): its frame's pc
	// is where, for the error's trace, taken again from here (the frame
	// saves its pc only as it calls).
	fr->pc = IPC() + 1;
	if (ak_retrace_hook) ak_retrace_hook();
unwind:
	// An error is pending: find a handler, in this frame, then its
	// callers'.
	{
		int64_t h = ak_vm_find_handler(F, IPC());
		if (h >= 0) {
			ip = code + h;
			NEXT();
		}
	}
unwind_frame:
	for (;;) {
		bool boundary = fr->boundary;
		uint32_t callpc = fr->callpc;
		// Out of a peek the error left: what it untracked tracks again.
		ak_vm_retrack(co, fr->udepth);
		ak_vm_pop(co, -1);
		if (boundary || co->nframes == 0) {
			*out = (ak_val){0};
			return AK_RUN_RET;
		}
		LOAD();
		// A function compiled to C finds its handler as it resumes,
		// the error pending: the run loop resumes it.
		if (F->resume) return AK_RUN_PUSHED;
		// The call didn't return: its destination keeps what it had.
		int64_t h = ak_vm_find_handler(F, callpc);
		if (h >= 0) {
			ip = code + h;
			NEXT();
		}
	}
#undef OP
#undef NEXT
#undef DEBUG_HOOK
#undef DEBUG_CHECK
#undef DEBUG_ON
#undef DISPATCH
#undef JUMP
#undef LOAD
#undef RELOAD
#undef SAVE
#undef IPC
#undef CHECK_ERR
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
	// Each call from C nests the run loop on the C stack.
	if (AK_UNLIKELY(ak_stack_low())) {
		ak_throw_msg("stack overflow in %s", f->name->data);
		return (ak_val){0};
	}
	if (!ak_vm_push(co, f, clos, args, nargs, 0xFFFF, true, 0)) return (ak_val){0};
	ak_val out;
	bool suspended = ak_vm_run(co, &out);
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
