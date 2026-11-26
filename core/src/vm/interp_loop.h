// interp_loop.h: the interpreter's loop (interp.c, which says how it
// dispatches), as a function named INTERP_NAME: interp.c makes it once,
// and in a build with the debugger again with INTERP_HOOKED set, the
// copy that calls the debugger's hook before each instruction.

static int INTERP_NAME(ak_co *co, ak_val *out) {
	ak_frame *fr;
	const ak_func *F;
	const uint16_t *code, *ip, *a;
	ak_val *R;
	ak_module *M;
	// The pending error, read in place: what CHECK_ERR asks after a call.
	void *const *err = co->err;

#define LOAD() do { \
		fr = &co->frames[co->nframes - 1]; \
		F = fr->fn; \
		code = F->code; \
		R = co->regs + fr->base; \
		M = F->mod; \
		ip = code + fr->pc; \
	} while (0)
	// Into the frame push_fast just pushed, of g: what LOAD would read.
#define ENTER(nf, g) do { \
		fr = (nf); \
		F = (g); \
		code = F->code; \
		R = co->regs + fr->base; \
		M = F->mod; \
		ip = code; \
	} while (0)
	// A call to the bytecode function g (closure clos) with n arguments
	// from args, its result to go in the instruction's a[0]: its frame
	// pushed and entered, or the error unwound.
#define PUSH(g, clos, args, n) do { \
		const ak_func *g_ = (g); \
		ak_frame *nf_ = push_fast(co, g_, (clos), (args), (n), a[0], false, IPC()); \
		if (AK_LIKELY(nf_)) { \
			ENTER(nf_, g_); \
		} else { \
			if (!ak_vm_push(co, g_, (clos), (args), (n), a[0], false, IPC())) goto unwind; \
			LOAD(); \
		} \
	} while (0)
	// After anything that may run more VM code (and grow the stacks).
#define RELOAD() do { \
		fr = &co->frames[co->nframes - 1]; \
		R = co->regs + fr->base; \
	} while (0)
	// Where the frame resumes, saved before a call (the pc after it).
#define SAVE() (fr->pc = (uint32_t)(ip - code))
	// A jump taken (a loop's way round): the debugger may want to see it.
#define JUMP(off) do { ip += (off); SWITCHPOINT(); } while (0)
	// The instruction running's pc, for its handlers and its line.
#define IPC() ((uint32_t)(a - 1 - code))
#define CHECK_ERR() do { if (AK_UNLIKELY(*err != NULL)) goto unwind; } while (0)

	// The debugger's hook (debug.h), in a development build, where this
	// function is made twice (interp.c). The fast one (INTERP_HOOKED 0)
	// asks ak_dbg_on only as a frame is entered or left and as a jump is
	// taken (a loop goes round): set, it leaves, its frame to resume
	// where it is (AK_RUN_PUSHED: the run loop calls ak_vm_interp again,
	// which runs the hooked one). Nothing is asked per instruction, and
	// its dispatch is a release build's. The hooked one calls the hook
	// before each instruction, and leaves the same way once the flag is
	// clear.
#if AK_VM_DEBUGGER
#define DEBUG_ON() atomic_load_explicit(&ak_dbg_on, memory_order_relaxed)
#endif
#if AK_VM_DEBUGGER && !INTERP_HOOKED
#define SWITCHPOINT() do { if (AK_UNLIKELY(DEBUG_ON())) { SAVE(); return AK_RUN_PUSHED; } } while (0)
#else
#define SWITCHPOINT() (void)0
#endif

#if AK_VM_THREADED
#define LABEL(name) &&L_##name,
	static const void *const dispatch[AK_OP__COUNT] = {AK_OP_LIST(LABEL)};
#undef LABEL
#define OP(name) L_##name: ip += AK_OPW_##name;
#if INTERP_HOOKED
#define NEXT() goto next
#else
#define NEXT() do { a = ip + 1; goto *dispatch[*ip]; } while (0)
#endif
#else
#define OP(name) case AK_OP_##name: ip += AK_OPW_##name;
#define NEXT() goto next
#endif

	LOAD();
#if AK_VM_THREADED && !INTERP_HOOKED
	NEXT();
#else
next:
	a = ip + 1;
#if INTERP_HOOKED
	if (AK_UNLIKELY(!DEBUG_ON())) {
		SAVE();
		return AK_RUN_PUSHED;
	}
	// It may stop here, and wait; it runs none of the app's code. Then
	// the instruction runs.
	ak_dbg_hook(co, (uint32_t)(ip - code));
	RELOAD();
#endif
#if AK_VM_THREADED
	goto *dispatch[*ip];
#else
	switch (*ip) {
#endif
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
		PUSH(g, NULL, R + a[3], a[4]);
		if (F->resume) return AK_RUN_PUSHED;
		SWITCHPOINT();
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
			PUSH((const ak_func *)c->fn, c, R + a[2], a[3]);
			if (F->resume) return AK_RUN_PUSHED;
			SWITCHPOINT();
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
		PUSH(g, NULL, R + a[3], a[4]);
		if (F->resume) return AK_RUN_PUSHED;
		SWITCHPOINT();
		NEXT();
	}
	OP(RET) {
		ak_val v = R[a[0]];
		bool ref = F->kinds[a[0]] == AK_K_REF;
		bool boundary = fr->boundary;
		uint16_t dst = fr->dst;
		if (AK_LIKELY(!fr->inout)) {
			// ak_vm_pop, inline: the frame's references but the result
			// (a reference register, if it's one of them).
			const uint16_t *refs = F->refs;
			uint32_t keep = a[0];
			for (uint32_t j = 0, n = F->nrefs; j < n; j++) {
				uint32_t r = refs[j];
				if (r != keep) ak_release(R[r].p);
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
		SWITCHPOINT();
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
		SWITCHPOINT();
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
			SWITCHPOINT();
			NEXT();
		}
	}
#undef OP
#undef NEXT
#undef SWITCHPOINT
#undef DEBUG_ON
#undef JUMP
#undef LOAD
#undef ENTER
#undef PUSH
#undef RELOAD
#undef SAVE
#undef IPC
#undef CHECK_ERR
}

#undef INTERP_NAME
#undef INTERP_HOOKED
