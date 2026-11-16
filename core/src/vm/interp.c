#include "vm/vm_int.h"
#include "vm/ops.h"

#include <stdio.h>

// Each thread runs the VM on its own coroutines: the main thread, and
// spawn's workers.
AK_TLS ak_co ak_vm_main;
AK_TLS ak_co *ak_vm_cur;
AK_TLS uint32_t ak_vm_adepth;

static inline ak_co *cur_co(void) {
	if (!ak_vm_cur) ak_vm_cur = &ak_vm_main;
	return ak_vm_cur;
}

#define MAX_FRAMES 10000

// ---- Frames ----

bool ak_vm_push(ak_co *co, const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs, uint16_t dst, bool boundary, uint32_t callpc) {
	if (co->nframes >= MAX_FRAMES) {
		ak_throw_msg("stack overflow in %s", f->name->data);
		return false;
	}
	if (nargs != f->nparams) ak_panic("vm: %s takes %u arguments, called with %u", f->name->data, f->nparams, nargs);
	uint32_t base = co->top;
	if (base + f->nregs > co->cap) {
		uint32_t c = co->cap ? co->cap * 2 : 1024;
		while (c < base + f->nregs) c *= 2;
		// args may point into the registers being moved.
		ptrdiff_t off = (args && co->regs && args >= co->regs && args < co->regs + co->cap) ? args - co->regs : -1;
		co->regs = ak_realloc(co->regs, sizeof(ak_val) * c);
		co->cap = c;
		if (off >= 0) args = co->regs + off;
	}
	ak_val *R = co->regs + base;
	if (f->nregs) memset(R, 0, sizeof(ak_val) * f->nregs);  // a coroutine's first call may have no registers yet
	for (uint32_t i = 0; i < nargs; i++) {
		R[i] = args[i];
		ak_retain_val(R[i], (ak_kind)f->kinds[i]);
	}
	co->top = base + f->nregs;
	if (co->nframes == co->fcap) {
		co->fcap = co->fcap ? co->fcap * 2 : 64;
		co->frames = ak_realloc(co->frames, sizeof(ak_frame) * co->fcap);
	}
	if (clos) ak_retain(clos);
	co->frames[co->nframes++] = (ak_frame){.fn = f, .clos = clos, .pc = 0, .callpc = callpc, .base = base, .dst = dst, .boundary = boundary};
	return true;
}

void ak_vm_pop(ak_co *co, int keep) {
	ak_frame *fr = &co->frames[co->nframes - 1];
	ak_val *R = co->regs + fr->base;
	const ak_func *f = fr->fn;
	for (uint32_t i = 0; i < f->nregs; i++)
		if (f->kinds[i] == AK_K_REF && (int)i != keep) ak_release(R[i].p);
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

void ak_vm_traceback(char *buf, size_t cap) {
	size_t n = 0;
	buf[0] = 0;
	ak_co *co = cur_co();
	for (uint32_t i = co->nframes; i-- > 0 && n < cap;) {
		ak_frame *fr = &co->frames[i];
		// A function compiled to C tells its line as an error leaves it
		// (ak_frame_hook): its frame's pc is where it last left.
		if (fr->fn->resume) continue;
		int w = snprintf(buf + n, cap - n, "%s (%s:%u)\n", fr->fn->name->data, fr->fn->file->data, line_of(fr->fn, fr->pc ? fr->pc - 1 : 0));
		if (w < 0) break;
		n += (size_t)w;
	}
}

// ---- Interface calls ----

const ak_func *ak_vm_method(void *obj, ak_str *name) {
	const ak_obj *o = obj;
	const ak_shape *s = NULL;
	if (o && o->tag == AK_TAG_STRUCT) s = ((const ak_struct *)o)->shape;
	else if (o && o->tag == AK_TAG_ENUM) s = ((const ak_enum *)o)->shape;
	if (!s || s == &ak_shape_error) return NULL;
	// Shapes the VM loaded carry their methods and module.
	const ak_vm_shape *vs = (const ak_vm_shape *)s;
	for (uint32_t i = 0; i < vs->nmethods; i++)
		if (vs->methods[i].name == name || ak_str_eq(vs->methods[i].name, name)) return &vs->mod->funcs[vs->methods[i].func];
	return NULL;
}

// ---- The interpreter ----

#define setref ak_op_setref

static inline uint32_t off32(const uint16_t *c) { return (uint32_t)c[0] | (uint32_t)c[1] << 16; }

#define store(f, R, a, v) ak_op_store((f)->kinds, R, a, v)

int ak_vm_interp(ak_co *co, ak_val *out) {
	ak_frame *fr;
	const ak_func *F;
	const uint16_t *code;
	ak_val *R;
	ak_module *M;
	uint32_t pc, ipc = 0;

#define LOAD() do { \
		fr = &co->frames[co->nframes - 1]; \
		F = fr->fn; \
		code = F->code; \
		R = co->regs + fr->base; \
		M = F->mod; \
		pc = fr->pc; \
	} while (0)
	// After anything that may run more VM code (and grow the stacks).
#define RELOAD() do { \
		fr = &co->frames[co->nframes - 1]; \
		R = co->regs + fr->base; \
	} while (0)
#define CHECK_ERR() do { if (AK_UNLIKELY(ak_error_pending())) goto unwind; } while (0)

	LOAD();
	for (;;) {
		ipc = pc;
		uint16_t op = code[pc];
		const uint16_t *a = code + pc + 1;
		pc += ak_op_width[op];
		switch (op) {
		case AK_OP_NOP:
			break;
		case AK_OP_MOV:
			R[a[0]] = R[a[1]];
			break;
		case AK_OP_MOVR:
			if (a[0] != a[1]) setref(R, a[0], ak_retain(R[a[1]].p));
			break;
		case AK_OP_CLR:
			setref(R, a[0], NULL);
			break;
		case AK_OP_KINT:
			R[a[0]].i = M->ints[a[1]];
			break;
		case AK_OP_KSMALL:
			R[a[0]].i = (int16_t)a[1];
			break;
		case AK_OP_KFLOAT:
			R[a[0]].f = M->floats[a[1]];
			break;
		case AK_OP_KSTR:
			setref(R, a[0], ak_retain(M->strings[a[1]]));
			break;
		case AK_OP_KNONE:
			setref(R, a[0], NULL);
			break;
		case AK_OP_GGET:
			if (a[2] == AK_K_REF) setref(R, a[0], ak_retain(M->globals[a[1]].p));
			else R[a[0]] = M->globals[a[1]];
			break;
		case AK_OP_GSET:
			if (a[2] == AK_K_REF) {
				void *old = M->globals[a[0]].p;
				M->globals[a[0]].p = ak_retain(R[a[1]].p);
				ak_release(old);
			} else {
				M->globals[a[0]] = R[a[1]];
			}
			break;

		case AK_OP_ADDI: R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i + (uint64_t)R[a[2]].i); break;
		case AK_OP_SUBI: R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i - (uint64_t)R[a[2]].i); break;
		case AK_OP_MULI: R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i * (uint64_t)R[a[2]].i); break;
		case AK_OP_DIVI:
		case AK_OP_MODI:
			if (!ak_op_divi(R, a[0], a[1], a[2], op == AK_OP_MODI)) goto unwind;
			break;
		case AK_OP_NEGI: R[a[0]].i = (int64_t)(0 - (uint64_t)R[a[1]].i); break;
		case AK_OP_ADDIK: R[a[0]].i = (int64_t)((uint64_t)R[a[1]].i + (uint64_t)(int64_t)(int16_t)a[2]); break;
		case AK_OP_EQI: R[a[0]].i = R[a[1]].i == R[a[2]].i; break;
		case AK_OP_NEI: R[a[0]].i = R[a[1]].i != R[a[2]].i; break;
		case AK_OP_LTI: R[a[0]].i = R[a[1]].i < R[a[2]].i; break;
		case AK_OP_LEI: R[a[0]].i = R[a[1]].i <= R[a[2]].i; break;
		case AK_OP_BAND: R[a[0]].i = R[a[1]].i & R[a[2]].i; break;
		case AK_OP_BOR: R[a[0]].i = R[a[1]].i | R[a[2]].i; break;
		case AK_OP_BXOR: R[a[0]].i = R[a[1]].i ^ R[a[2]].i; break;
		case AK_OP_BNOT: R[a[0]].i = ~R[a[1]].i; break;
		case AK_OP_SHL:
		case AK_OP_SHR:
			if (!ak_op_shift(R, a[0], a[1], a[2], op == AK_OP_SHR)) goto unwind;
			break;

		case AK_OP_ADDF: R[a[0]].f = R[a[1]].f + R[a[2]].f; break;
		case AK_OP_SUBF: R[a[0]].f = R[a[1]].f - R[a[2]].f; break;
		case AK_OP_MULF: R[a[0]].f = R[a[1]].f * R[a[2]].f; break;
		case AK_OP_DIVF: R[a[0]].f = R[a[1]].f / R[a[2]].f; break;
		case AK_OP_MODF: R[a[0]].f = fmod(R[a[1]].f, R[a[2]].f); break;
		case AK_OP_NEGF: R[a[0]].f = -R[a[1]].f; break;
		case AK_OP_EQF: R[a[0]].i = R[a[1]].f == R[a[2]].f; break;
		case AK_OP_NEF: R[a[0]].i = R[a[1]].f != R[a[2]].f; break;
		case AK_OP_LTF: R[a[0]].i = R[a[1]].f < R[a[2]].f; break;
		case AK_OP_LEF: R[a[0]].i = R[a[1]].f <= R[a[2]].f; break;
		case AK_OP_I2F: R[a[0]].f = (double)R[a[1]].i; break;
		case AK_OP_F2I: R[a[0]].i = ak_op_f2i(R[a[1]].f); break;
		case AK_OP_NOT: R[a[0]].i = !R[a[1]].i; break;

		case AK_OP_EQ: R[a[0]].i = ak_val_eq(R[a[1]], R[a[2]], (ak_kind)a[3]); break;
		case AK_OP_NE: R[a[0]].i = !ak_val_eq(R[a[1]], R[a[2]], (ak_kind)a[3]); break;
		case AK_OP_ISNONE: R[a[0]].i = R[a[1]].p == NULL; break;

		case AK_OP_CONCAT: setref(R, a[0], ak_str_concat(R[a[1]].p, R[a[2]].p)); break;
		case AK_OP_TOSTR: setref(R, a[0], ak_op_tostr(R[a[1]], a[2])); break;
		case AK_OP_INTERP: setref(R, a[0], ak_op_interp(R, a[1], a[2], M->strings[a[3]])); break;
		case AK_OP_LTS: R[a[0]].i = ak_str_cmp(R[a[1]].p, R[a[2]].p) < 0; break;

		case AK_OP_JMP: pc += off32(a); break;
		case AK_OP_JT: if (R[a[0]].i) pc += off32(a + 1); break;
		case AK_OP_JF: if (!R[a[0]].i) pc += off32(a + 1); break;
		case AK_OP_JNONE: if (!R[a[0]].p) pc += off32(a + 1); break;
		case AK_OP_JSOME: if (R[a[0]].p) pc += off32(a + 1); break;

		case AK_OP_CALL: {
			const ak_func *g = &M->funcs[a[1]];
			fr->pc = pc;
			if (g->native) {
				ak_val v = g->native(R + a[2], NULL);
				RELOAD();
				CHECK_ERR();
				if (a[0] == 0xFFFF) ak_release_val(v, (ak_kind)g->ret);
				else store(F, R, a[0], v);
				break;
			}
			if (!ak_vm_push(co, g, NULL, R + a[2], a[3], a[0], false, ipc)) goto unwind;
			LOAD();
			if (F->resume) return AK_RUN_PUSHED;
			break;
		}
		case AK_OP_CALLC: {
			ak_closure *c = R[a[1]].p;
			if (!c) ak_panic("vm: calling none");
			fr->pc = pc;
			if (c->invoke == ak_vm_closure_invoke && !((const ak_func *)c->fn)->native && !(((const ak_func *)c->fn)->flags & AK_F_ASYNC && co->task == NULL)) {
				// A VM closure: call it in this loop.
				if (!ak_vm_push(co, c->fn, c, R + a[2], a[3], a[0], false, ipc)) goto unwind;
				LOAD();
				if (F->resume) return AK_RUN_PUSHED;
			} else {
				ak_val v = c->invoke(c, R + a[2], a[3]);
				RELOAD();
				CHECK_ERR();
				store(F, R, a[0], v);
			}
			break;
		}
		case AK_OP_CALLB: {
			fr->pc = pc;
			ak_val v = M->builtins[a[1]](R + a[2], a[3]);
			RELOAD();
			CHECK_ERR();
			store(F, R, a[0], v);
			break;
		}
		case AK_OP_CALLI: {
			ak_str *name = M->strings[a[1]];
			void *obj = R[a[2]].p;
			const ak_func *g = ak_vm_method(obj, name);
			if (!g) ak_panic("vm: no method %s", name->data);
			fr->pc = pc;
			if (g->native) {
				ak_val v = g->native(R + a[2], NULL);
				RELOAD();
				CHECK_ERR();
				if (a[0] == 0xFFFF) ak_release_val(v, (ak_kind)g->ret);
				else store(F, R, a[0], v);
				break;
			}
			if (!ak_vm_push(co, g, NULL, R + a[2], a[3], a[0], false, ipc)) goto unwind;
			LOAD();
			if (F->resume) return AK_RUN_PUSHED;
			break;
		}
		case AK_OP_RET:
		case AK_OP_RET0: {
			ak_val v = {0};
			int keep = -1;
			if (op == AK_OP_RET) {
				v = R[a[0]];
				if (F->kinds[a[0]] == AK_K_REF) keep = a[0];
			}
			ak_kind k = op == AK_OP_RET ? (ak_kind)F->kinds[a[0]] : AK_K_VOID;
			bool boundary = fr->boundary;
			uint16_t dst = fr->dst;
			ak_vm_pop(co, keep);
			if (boundary || co->nframes == 0) {
				*out = v;
				return AK_RUN_RET;
			}
			LOAD();
			if (dst == 0xFFFF) ak_release_val(v, k);
			else store(F, R, dst, v);
			if (F->resume) return AK_RUN_PUSHED;
			break;
		}
		case AK_OP_THROW: {
			void *e = R[a[0]].p;
			R[a[0]].p = NULL;
			ak_throw(e);
			goto unwind;
		}
		case AK_OP_CATCH:
			setref(R, a[0], ak_error_take());
			break;
		case AK_OP_RETHROW: {
			void *e = R[a[0]].p;
			R[a[0]].p = NULL;
			ak_throw(e);
			// Handlers of the same function don't catch it again: look
			// from the caller on.
			goto unwind_frame;
		}

		case AK_OP_CLOS: setref(R, a[0], ak_op_clos(&M->funcs[a[1]], R + a[2])); break;
		case AK_OP_CAP: ak_op_cap(R, a[0], fr->clos, a[1]); break;
		case AK_OP_VBOX: ak_op_vbox(R, a[0], R[a[1]], a[2]); break;
		case AK_OP_VGET: ak_op_vget(R, a[0], R[a[1]].p); break;
		case AK_OP_VSET: ak_op_vset(R[a[0]].p, R[a[1]]); break;
		case AK_OP_NEWS: setref(R, a[0], ak_op_news(&M->shapes[a[1]].s, R + a[2])); break;
		case AK_OP_GETF: ak_op_getf(R, a[0], R[a[1]].p, a[2]); break;
		case AK_OP_SETF: ak_op_setf(R, a[0], a[1], R[a[2]]); break;
		case AK_OP_NEWE: setref(R, a[0], ak_op_newe(&M->shapes[a[1]].s, a[2], R + a[3])); break;
		case AK_OP_ECASE: R[a[0]].i = ak_enum_case(R[a[1]].p); break;
		case AK_OP_GETP: ak_op_getp(R, a[0], R[a[1]].p, a[2]); break;
		case AK_OP_BOX: setref(R, a[0], ak_box_new((ak_kind)a[2], R[a[1]])); break;
		case AK_OP_UNBOX: R[a[0]] = ((ak_box *)R[a[1]].p)->v; break;
		case AK_OP_NEWA: setref(R, a[0], ak_op_newa((ak_kind)a[1], a[2] == 0xFFFF ? NULL : &M->shapes[a[2]].s, R + a[3], a[4])); break;
		case AK_OP_IDX:
			if (!ak_op_idx(R, a[0], R[a[1]].p, R[a[2]].i)) goto unwind;
			break;
		case AK_OP_SETIDX:
			if (!ak_op_setidx(R, a[0], R[a[1]].i, R[a[2]])) goto unwind;
			break;
		case AK_OP_LEN: R[a[0]].i = ak_op_len(R[a[1]].p); break;
		case AK_OP_NEWM: setref(R, a[0], ak_op_newm((ak_kind)a[1], (ak_kind)a[2], R + a[3], a[4])); break;
		case AK_OP_MGET: setref(R, a[0], ak_op_mget(R[a[1]].p, R[a[2]])); break;
		case AK_OP_MSET: ak_op_mset(R, a[0], R[a[1]], R[a[2]]); break;

		case AK_OP_CELL:
		case AK_OP_CELLK: {
			ak_val v = R[a[1]];
			ak_retain_val(v, (ak_kind)a[2]);
			fr->pc = pc;
			ak_val c = ak_vm_cell_new(v, (ak_kind)a[2], op == AK_OP_CELLK ? M->strings[a[3]] : NULL);
			RELOAD();
			setref(R, a[0], c.p);
			break;
		}
		case AK_OP_CGET:
		case AK_OP_CPEEK: {
			fr->pc = pc;
			ak_val v = ak_vm_cell_get(R[a[1]].p, op == AK_OP_CGET);
			RELOAD();
			store(F, R, a[0], v);
			break;
		}
		case AK_OP_CSET: {
			ak_val v = R[a[1]];
			ak_retain_val(v, (ak_kind)F->kinds[a[1]]);
			fr->pc = pc;
			ak_vm_cell_set(R[a[0]].p, v);
			RELOAD();
			CHECK_ERR();
			break;
		}
		case AK_OP_CTAKE:
			store(F, R, a[0], ak_vm_cell_take(R[a[1]].p));
			break;
		case AK_OP_CPUT: {
			ak_val v = R[a[1]];
			if (F->kinds[a[1]] == AK_K_REF) R[a[1]].p = NULL;  // moved
			fr->pc = pc;
			ak_vm_cell_put(R[a[0]].p, v);
			RELOAD();
			CHECK_ERR();
			break;
		}
		case AK_OP_MEMO: {
			fr->pc = pc;
			ak_val m = ak_vm_memo_new(R[a[1]].p, (ak_kind)a[2]);
			RELOAD();
			setref(R, a[0], m.p);
			break;
		}
		case AK_OP_MREAD: {
			fr->pc = pc;
			ak_val v = ak_vm_memo_get(R[a[1]].p);
			RELOAD();
			CHECK_ERR();
			store(F, R, a[0], v);
			break;
		}
		case AK_OP_ACCV: {
			ak_val v = R[a[1]];
			ak_retain_val(v, (ak_kind)a[2]);
			setref(R, a[0], ak_vm_acc_value(v, (ak_kind)a[2]).p);
			break;
		}
		case AK_OP_ACCF:
			setref(R, a[0], ak_vm_acc_func(R[a[1]].p, (ak_kind)a[2]).p);
			break;
		case AK_OP_ACCC:
			setref(R, a[0], ak_vm_acc_cell(R[a[1]].p).p);
			break;
		case AK_OP_AGET: {
			fr->pc = pc;
			ak_val v = ak_vm_acc_get(R[a[1]].p);
			RELOAD();
			CHECK_ERR();
			store(F, R, a[0], v);
			break;
		}
		case AK_OP_UNTRACK: {
			fr->pc = pc;
			ak_val v = ak_vm_untrack(R[a[1]].p, R + a[2], a[3]);
			RELOAD();
			CHECK_ERR();
			store(F, R, a[0], v);
			break;
		}
		case AK_OP_EFFECT:
		case AK_OP_RENDER:
			fr->pc = pc;
			ak_vm_effect(R[a[0]].p, op == AK_OP_RENDER);
			RELOAD();
			CHECK_ERR();
			break;
		case AK_OP_CLEANUP:
			ak_vm_cleanup(R[a[0]].p);
			break;
		case AK_OP_BATCH:
			fr->pc = pc;
			ak_vm_batch(a[0] != 0);
			RELOAD();
			CHECK_ERR();
			break;

		case AK_OP_TASK: {
			ak_closure *c = R[a[1]].p;
			fr->pc = pc;
			ak_val f = ak_op_task(c);
			RELOAD();
			CHECK_ERR();
			setref(R, a[0], f.p);
			break;
		}
		case AK_OP_AWAIT: {
			ak_future *f = R[a[1]].p;
			int w = ak_vm_await(co, f);
			if (w < 0) goto unwind;
			if (w > 0) {
				// Suspend; AWAIT runs again when resumed, with f done.
				fr->pc = ipc;
				return AK_RUN_SUSPEND;
			}
			ak_val v = ak_future_take(f);
			CHECK_ERR();
			store(F, R, a[0], v);
			break;
		}
		case AK_OP_YIELD:
			ak_vm_yield(co);
			fr->pc = pc;
			return AK_RUN_SUSPEND;
		default:
			ak_panic("vm: bad opcode %u in %s at %u", op, F->name->data, ipc);
		}
		continue;

	unwind:
		// An error is pending: find a handler, in this frame, then its
		// callers'.
		{
			int64_t h = ak_vm_find_handler(F, ipc);
			if (h >= 0) {
				pc = (uint32_t)h;
				continue;
			}
		}
	unwind_frame:
		for (;;) {
			bool boundary = fr->boundary;
			uint32_t callpc = fr->callpc;
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
			ipc = callpc;
			int64_t h = ak_vm_find_handler(F, ipc);
			if (h >= 0) {
				pc = (uint32_t)h;
				break;
			}
		}
	}
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

void ak_vm_closure_drop(ak_closure *c) { ak_module_release(((const ak_func *)c->fn)->mod); }

static ak_val call_sync(const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs) {
	ak_co *co = cur_co();
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
	ak_module_retain(f->mod);
	c->drop = ak_vm_closure_drop;
	return c;
}

void ak_vm_run_ready(void) { ak_sched_run(); }
bool ak_vm_has_ready(void) { return ak_sched_has_ready(); }
