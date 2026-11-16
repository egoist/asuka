// ops.h: the instructions whose bodies are more than an expression, as
// inline functions: the interpreter (interp.c) runs them, and the C that
// lang/cgen compiles a module to calls the same ones, so both backends
// mean the same thing by each instruction. R is a frame's registers;
// kinds its registers' kinds. A function that returns bool returns false
// when it threw (ak_throw: the caller unwinds).
#ifndef AK_VM_OPS_H
#define AK_VM_OPS_H

#include "vm/vm_int.h"

static inline void ak_op_setref(ak_val *R, uint16_t a, void *p) {
	void *old = R[a].p;
	R[a].p = p;
	ak_release(old);
}

// ak_op_store puts a result in register a (releasing what it held).
static inline void ak_op_store(const uint8_t *kinds, ak_val *R, uint16_t a, ak_val v) {
	if (a == 0xFFFF) return;
	if (kinds[a] == AK_K_REF) ak_op_setref(R, a, v.p);
	else R[a] = v;
}

static inline bool ak_op_divi(ak_val *R, uint16_t a, uint16_t b, uint16_t c, bool mod) {
	int64_t x = R[b].i, y = R[c].i;
	if (y == 0) {
		ak_throw_msg("division by zero");
		return false;
	}
	if (y == -1) R[a].i = mod ? 0 : (int64_t)(0 - (uint64_t)x);
	else R[a].i = mod ? x % y : x / y;
	return true;
}

// ak_op_shift is b << c, or b >> c (arithmetic) when right: a count of
// 64 or more shifts every bit out (0, or -1 for >> of a negative), as
// Go's do; a negative count throws.
static inline bool ak_op_shift(ak_val *R, uint16_t a, uint16_t b, uint16_t c, bool right) {
	int64_t x = R[b].i, n = R[c].i;
	if (n < 0) {
		ak_throw_msg("negative shift amount");
		return false;
	}
	if (right) R[a].i = n >= 64 ? (x < 0 ? -1 : 0) : x >> n;
	else R[a].i = n >= 64 ? 0 : (int64_t)((uint64_t)x << n);
	return true;
}

static inline int64_t ak_op_f2i(double d) {
	return d != d ? 0 : d >= 9.2233720368547758e18 ? INT64_MAX : d <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)d;
}

static inline ak_str *ak_op_tostr(ak_val v, uint16_t k) {
	switch (k) {
	case AK_K_INT: return ak_str_from_int(v.i);
	case AK_K_FLOAT: return ak_str_from_float(v.f);
	case AK_K_BOOL: return ak_str_from_bool(v.i != 0);
	default: return ak_retain(v.p ? v.p : ak_str_empty());
	}
}

static inline ak_str *ak_op_interp(const ak_val *R, uint16_t b, uint16_t n, const ak_str *kinds) {
	ak_strb sb = {0};
	for (uint16_t i = 0; i < n; i++) {
		ak_val v = R[b + i];
		switch (i < kinds->len ? kinds->data[i] : AK_K_REF) {
		case AK_K_INT: ak_strb_int(&sb, v.i); break;
		case AK_K_FLOAT: ak_strb_float(&sb, v.f); break;
		case AK_K_BOOL: ak_strb_bool(&sb, v.i != 0); break;
		default:
			if (v.p) ak_strb_str(&sb, v.p);
			break;
		}
	}
	return ak_strb_finish(&sb);
}

void ak_vm_closure_drop(ak_closure *c);

static inline ak_closure *ak_op_clos(const ak_func *g, const ak_val *caps) {
	ak_closure *c = ak_closure_new(ak_vm_closure_invoke, g, g->ncap, g->capkinds);
	for (uint16_t i = 0; i < g->ncap; i++) {
		c->caps[i] = caps[i];
		ak_retain_val(c->caps[i], (ak_kind)g->capkinds[i]);
	}
	ak_module_retain(g->mod);
	c->drop = ak_vm_closure_drop;
	return c;
}

static inline void ak_op_cap(ak_val *R, uint16_t a, const ak_closure *c, uint16_t i) {
	ak_val v = c->caps[i];
	if (c->capkinds[i] == AK_K_REF) ak_op_setref(R, a, ak_retain(v.p));
	else R[a] = v;
}

static inline void ak_op_vbox(ak_val *R, uint16_t a, ak_val v, uint16_t k) {
	ak_box *b = ak_box_new((ak_kind)k, v);
	ak_retain_val(b->v, (ak_kind)k);
	b->h.flags = 1;
	ak_op_setref(R, a, b);
}

static inline void ak_op_vget(ak_val *R, uint16_t a, const ak_box *b) {
	if (b->h.aux == AK_K_REF) ak_op_setref(R, a, ak_retain(b->v.p));
	else R[a] = b->v;
}

static inline void ak_op_vset(ak_box *b, ak_val v) {
	if (b->h.aux == AK_K_REF) {
		void *old = b->v.p;
		b->v.p = ak_retain(v.p);
		ak_release(old);
	} else {
		b->v = v;
	}
}

static inline ak_struct *ak_op_news(const ak_shape *s, const ak_val *f) {
	ak_struct *o = ak_struct_new(s);
	for (uint16_t i = 0; i < s->nfields; i++) {
		o->f[i] = f[i];
		ak_retain_val(o->f[i], (ak_kind)s->kinds[i]);
	}
	return o;
}

static inline void ak_op_getf(ak_val *R, uint16_t a, const ak_struct *o, uint16_t i) {
	ak_val v = o->f[i];
	if (o->shape->kinds[i] == AK_K_REF) ak_op_setref(R, a, ak_retain(v.p));
	else R[a] = v;
}

static inline void ak_op_setf(ak_val *R, uint16_t a, uint16_t i, ak_val v) {
	ak_struct *o = ak_struct_mut((ak_struct **)&R[a].p);
	if (o->shape->kinds[i] == AK_K_REF) {
		void *old = o->f[i].p;
		o->f[i].p = ak_retain(v.p);
		ak_release(old);
	} else {
		o->f[i] = v;
	}
}

static inline ak_enum *ak_op_newe(const ak_shape *s, uint16_t ci, const ak_val *f) {
	ak_enum *e = ak_enum_new(s, ci);
	const ak_case *c = &s->cases[ci];
	for (uint16_t i = 0; i < c->nfields; i++) {
		e->f[i] = f[i];
		ak_retain_val(e->f[i], (ak_kind)c->kinds[i]);
	}
	return e;
}

static inline void ak_op_getp(ak_val *R, uint16_t a, const ak_enum *e, uint16_t i) {
	const ak_case *c = &e->shape->cases[ak_enum_case(e)];
	ak_val v = e->f[i];
	if (c->kinds[i] == AK_K_REF) ak_op_setref(R, a, ak_retain(v.p));
	else R[a] = v;
}

static inline ak_arr *ak_op_newa(ak_kind k, const ak_shape *s, const ak_val *items, uint16_t n) {
	ak_arr *arr = ak_arr_new(k, s, n);
	for (uint16_t i = 0; i < n; i++) {
		arr->data[i] = items[i];
		ak_retain_val(arr->data[i], k);
	}
	arr->len = n;
	return arr;
}

static inline bool ak_op_idx(ak_val *R, uint16_t a, const ak_arr *arr, int64_t i) {
	if (!ak_arr_check(arr, i)) {
		ak_throw_msg("index %lld out of range (length %u)", (long long)i, arr->len);
		return false;
	}
	ak_val v = arr->data[i];
	if (ak_arr_kind(arr) == AK_K_REF) ak_op_setref(R, a, ak_retain(v.p));
	else R[a] = v;
	return true;
}

static inline bool ak_op_setidx(ak_val *R, uint16_t a, int64_t i, ak_val v) {
	ak_arr **ap = (ak_arr **)&R[a].p;
	if (!ak_arr_check(*ap, i)) {
		ak_throw_msg("index %lld out of range (length %u)", (long long)i, (*ap)->len);
		return false;
	}
	ak_retain_val(v, ak_arr_kind(*ap));
	ak_arr_set(ap, i, v);
	return true;
}

static inline int64_t ak_op_len(const ak_obj *o) {
	if (!o) return 0;
	switch (o->tag) {
	case AK_TAG_ARR: return ((const ak_arr *)o)->len;
	case AK_TAG_MAP: return ((const ak_map *)o)->live;
	case AK_TAG_STR: return ((const ak_str *)o)->len;
	case AK_TAG_BYTES: return ((const ak_bytes *)o)->len;
	}
	return 0;
}

static inline ak_map *ak_op_newm(ak_kind kk, ak_kind vk, const ak_val *kv, uint16_t n) {
	ak_map *m = ak_map_new(kk, kk == AK_K_REF, vk, NULL);
	for (uint16_t i = 0; i < n; i++) {
		ak_val k = kv[2 * i], v = kv[2 * i + 1];
		ak_retain_val(k, kk);
		ak_retain_val(v, vk);
		ak_map_set(&m, k, v);
	}
	return m;
}

static inline void *ak_op_mget(const ak_map *m, ak_val key) {
	ak_val v;
	if (!ak_map_get(m, key, &v)) return NULL;
	ak_kind vk = (ak_kind)(m->h.aux >> 4);
	return vk == AK_K_REF ? ak_retain(v.p) : (void *)ak_box_new(vk, v);
}

static inline void ak_op_mset(ak_val *R, uint16_t a, ak_val k, ak_val v) {
	ak_map **mp = (ak_map **)&R[a].p;
	ak_kind kk = (ak_kind)((*mp)->h.aux & 0xf), vk = (ak_kind)((*mp)->h.aux >> 4);
	ak_retain_val(k, kk);
	ak_retain_val(v, vk);
	ak_map_set(mp, k, v);
}

// ak_op_task starts the closure as a task (or, a C closure, takes the
// future it returns).
static inline ak_val ak_op_task(ak_closure *c) {
	if (c->invoke == ak_vm_closure_invoke) return (ak_val){.p = ak_vm_task_start(c->fn, c, NULL, 0)};
	return c->invoke(c, NULL, 0);
}

// ak_op_exit releases a frame's reference registers but keep (-1: none).
static inline void ak_op_exit(ak_val *R, const uint8_t *kinds, uint16_t n, int keep) {
	for (uint16_t i = 0; i < n; i++)
		if (kinds[i] == AK_K_REF && (int)i != keep) ak_release(R[i].p);
}

// ---- Functions that may suspend, compiled to C (ak_async_body) ----
//
// Such a function's frame is on the coroutine, as the interpreter's is,
// and its pc means what the interpreter's does: where it resumes. While
// its body runs, its registers are the body's C locals; when it leaves
// the frame waiting (it suspends, or a call it made goes on in the run
// loop), they go back into the frame.

// ak_op_enter moves frame fi's registers into R, the body's, leaving the
// frame's empty: nothing else releases them while the body runs.
static inline void ak_op_enter(ak_co *co, uint32_t fi, ak_val *R, uint16_t n) {
	ak_val *S = co->regs + co->frames[fi].base;
	memcpy(R, S, sizeof(ak_val) * n);
	memset(S, 0, sizeof(ak_val) * n);
}

// ak_op_leave moves R back into frame fi, which resumes at pc.
static inline void ak_op_leave(ak_co *co, uint32_t fi, const ak_val *R, uint16_t n, uint32_t pc) {
	memcpy(co->regs + co->frames[fi].base, R, sizeof(ak_val) * n);
	co->frames[fi].pc = pc;
}

// ak_op_pushes reports whether calling closure c from a compiled async
// function pushes a frame on its coroutine (as the interpreter's CALLC
// does) rather than calling it from C.
static inline bool ak_op_pushes(const ak_co *co, const ak_closure *c) {
	if (c->invoke != ak_vm_closure_invoke) return false;
	const ak_func *g = c->fn;
	return !g->native && !((g->flags & AK_F_ASYNC) && co->task == NULL);
}

// ak_op_acall runs the frame a compiled async function just pushed: its
// body, directly, when it's compiled too and the direct calls aren't
// deep (popping its frame when it returned or threw); else it's left to
// the run loop (AK_RUN_PUSHED), as is the rest of a call that suspended.
static inline int ak_op_acall(ak_co *co, ak_val *v) {
	const ak_func *g = co->frames[co->nframes - 1].fn;
	if (!g->resume || ak_vm_adepth >= AK_VM_ADEPTH_MAX) return AK_RUN_PUSHED;
	ak_vm_adepth++;
	int s = g->resume(co, v);
	ak_vm_adepth--;
	if (s == AK_RUN_RET || s == AK_RUN_THROW) ak_vm_pop_frame(co);
	return s;
}

#endif
