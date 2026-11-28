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

// ak_op_overflow throws the error an int's +, -, * or - of the smallest
// int (or its / -1) gives when the result doesn't fit: as dividing by
// zero, a catch takes it. Out of line: the checks' paths stay short (not
// inline as well: GCC rejects noinline on an inline function; unused: a
// file may include this without an int operation).
__attribute__((cold, noinline, unused)) static bool ak_op_overflow(void) {
	ak_throw_msg("integer overflow");
	return false;
}

// ak_addi, ak_subi, ak_muli and ak_negi are ADDI, SUBI, MULI and NEGI
// on values: the operation, checked (the CPU's overflow flag, one
// predicted branch), its result in *r; false (*r as it was) when it
// overflowed and threw. ak_op_addi... are the same on a frame's
// registers, the interpreter's.
static inline bool ak_addi(int64_t *r, int64_t b, int64_t c) {
	int64_t v;
	if (__builtin_expect(__builtin_add_overflow(b, c, &v), 0)) return ak_op_overflow();
	*r = v;
	return true;
}

static inline bool ak_subi(int64_t *r, int64_t b, int64_t c) {
	int64_t v;
	if (__builtin_expect(__builtin_sub_overflow(b, c, &v), 0)) return ak_op_overflow();
	*r = v;
	return true;
}

static inline bool ak_muli(int64_t *r, int64_t b, int64_t c) {
	int64_t v;
	if (__builtin_expect(__builtin_mul_overflow(b, c, &v), 0)) return ak_op_overflow();
	*r = v;
	return true;
}

static inline bool ak_negi(int64_t *r, int64_t b) {
	int64_t v;
	if (__builtin_expect(__builtin_sub_overflow((int64_t)0, b, &v), 0)) return ak_op_overflow();
	*r = v;
	return true;
}

// ak_op_division throws division by zero (cold, as ak_op_overflow).
__attribute__((cold, noinline, unused)) static bool ak_op_division(void) {
	ak_throw_msg("division by zero");
	return false;
}

static inline bool ak_divi(int64_t *r, int64_t x, int64_t y, bool mod) {
	if (__builtin_expect(y == 0, 0)) return ak_op_division();
	if (y == -1) {
		// x % -1 is 0; x / -1 is -x, which the smallest int hasn't.
		if (mod) *r = 0;
		else if (x == INT64_MIN) return ak_op_overflow();
		else *r = -x;
	} else {
		*r = mod ? x % y : x / y;
	}
	return true;
}

// ak_shift is x << n, or x >> n (arithmetic) when right: a count of 64
// or more shifts every bit out (0, or -1 for >> of a negative), as Go's
// do; a negative count throws.
static inline bool ak_shift(int64_t *r, int64_t x, int64_t n, bool right) {
	if (__builtin_expect(n < 0, 0)) {
		ak_throw_msg("negative shift amount");
		return false;
	}
	if (right) *r = n >= 64 ? (x < 0 ? -1 : 0) : x >> n;
	else *r = n >= 64 ? 0 : (int64_t)((uint64_t)x << n);
	return true;
}

static inline bool ak_op_addi(ak_val *R, uint16_t a, uint16_t b, uint16_t c) { return ak_addi(&R[a].i, R[b].i, R[c].i); }
static inline bool ak_op_subi(ak_val *R, uint16_t a, uint16_t b, uint16_t c) { return ak_subi(&R[a].i, R[b].i, R[c].i); }
static inline bool ak_op_muli(ak_val *R, uint16_t a, uint16_t b, uint16_t c) { return ak_muli(&R[a].i, R[b].i, R[c].i); }
static inline bool ak_op_negi(ak_val *R, uint16_t a, uint16_t b) { return ak_negi(&R[a].i, R[b].i); }
static inline bool ak_op_divi(ak_val *R, uint16_t a, uint16_t b, uint16_t c, bool mod) { return ak_divi(&R[a].i, R[b].i, R[c].i, mod); }
static inline bool ak_op_shift(ak_val *R, uint16_t a, uint16_t b, uint16_t c, bool right) { return ak_shift(&R[a].i, R[b].i, R[c].i, right); }

// ---- The sized integers (ADDN … U2F) on values: t is the type's code
// (rt.h's ak_int_type; a u64's values its bits), *r written only when
// it fits; false when it threw. ak_op_addn... are the same on a frame's
// registers, the interpreter's. ----

static inline bool ak_addn(int64_t *r, int64_t b, int64_t c, int t) {
	int64_t v;
	if (t == AK_U64) {
		uint64_t u;
		if (__builtin_expect(__builtin_add_overflow((uint64_t)b, (uint64_t)c, &u), 0)) return ak_op_overflow();
		*r = (int64_t)u;
		return true;
	}
	if (__builtin_expect(__builtin_add_overflow(b, c, &v) || ak_int_wrap(v, t) != v, 0)) return ak_op_overflow();
	*r = v;
	return true;
}

static inline bool ak_subn(int64_t *r, int64_t b, int64_t c, int t) {
	int64_t v;
	if (t == AK_U64) {
		uint64_t u;
		if (__builtin_expect(__builtin_sub_overflow((uint64_t)b, (uint64_t)c, &u), 0)) return ak_op_overflow();
		*r = (int64_t)u;
		return true;
	}
	if (__builtin_expect(__builtin_sub_overflow(b, c, &v) || ak_int_wrap(v, t) != v, 0)) return ak_op_overflow();
	*r = v;
	return true;
}

static inline bool ak_muln(int64_t *r, int64_t b, int64_t c, int t) {
	int64_t v;
	if (__builtin_expect(!ak_int_mul(b, c, t, &v), 0)) return ak_op_overflow();
	*r = v;
	return true;
}

// ak_divn is DIVN, or MODN when mod: unsigned division for an unsigned
// type (a u64's past int's largest are negative int64s).
static inline bool ak_divn(int64_t *r, int64_t x, int64_t y, int t, bool mod) {
	if (__builtin_expect(y == 0, 0)) return ak_op_division();
	if (!ak_int_signed(t)) {
		uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
		*r = (int64_t)(mod ? ux % uy : ux / uy);
	} else if (y == -1) {
		if (mod) *r = 0;
		else if (x == ak_int_min(t)) return ak_op_overflow();
		else *r = -x;
	} else {
		*r = mod ? x % y : x / y;
	}
	return true;
}

static inline bool ak_negn(int64_t *r, int64_t b, int t) {
	if (__builtin_expect(b == ak_int_min(t), 0)) return ak_op_overflow();
	*r = -b;
	return true;
}

// ak_op_count reads a shift's count or a power's exponent, c of type ct:
// false (it threw) when it's negative; a u64's past int's largest is
// past every width.
static inline bool ak_op_count(int64_t c, int ct, uint64_t *n, const char *what) {
	if (__builtin_expect(c < 0 && ct != AK_U64, 0)) {
		ak_throw_msg("%s", what);
		return false;
	}
	*n = (uint64_t)c;
	return true;
}

// ak_pown is POWN, b of type t & 15, c of type t >> 4 (POWI's t is 0x33:
// int ** int).
static inline bool ak_pown(int64_t *r, int64_t b, int64_t c, int t) {
	uint64_t e;
	if (!ak_op_count(c, t >> 4, &e, "a negative exponent: an integer's negative power isn't an integer")) return false;
	int64_t v;
	if (__builtin_expect(!ak_int_pow(b, e, t & 15, &v), 0)) return ak_op_overflow();
	*r = v;
	return true;
}

// ak_shiftn is SHLN, or SHRN when right: a count of 64 or more shifts
// every bit out; << drops the bits past the type's width, >> is
// arithmetic for a signed type and logical for an unsigned one.
static inline bool ak_shiftn(int64_t *r, int64_t x, int64_t c, int t, bool right) {
	uint64_t n;
	if (!ak_op_count(c, t >> 4, &n, "negative shift amount")) return false;
	int vt = t & 15;
	if (!right) *r = n >= 64 ? 0 : ak_int_wrap((int64_t)((uint64_t)x << n), vt);
	else if (ak_int_signed(vt)) *r = n >= 64 ? (x < 0 ? -1 : 0) : x >> n;
	else *r = n >= 64 ? 0 : (int64_t)((uint64_t)x >> n);
	return true;
}

// ak_convn is CONVN: v, of type t >> 4, as type t & 15, which throws when
// it doesn't fit.
static inline bool ak_convn(int64_t *r, int64_t v, int t) {
	if (__builtin_expect(!ak_int_fits(v, t >> 4, t & 15), 0)) {
		ak_int_throw_fit(v, t >> 4, t & 15);
		return false;
	}
	*r = v;
	return true;
}

static inline bool ak_op_addn(ak_val *R, uint16_t a, uint16_t b, uint16_t c, int t) { return ak_addn(&R[a].i, R[b].i, R[c].i, t); }
static inline bool ak_op_subn(ak_val *R, uint16_t a, uint16_t b, uint16_t c, int t) { return ak_subn(&R[a].i, R[b].i, R[c].i, t); }
static inline bool ak_op_muln(ak_val *R, uint16_t a, uint16_t b, uint16_t c, int t) { return ak_muln(&R[a].i, R[b].i, R[c].i, t); }
static inline bool ak_op_divn(ak_val *R, uint16_t a, uint16_t b, uint16_t c, int t, bool mod) { return ak_divn(&R[a].i, R[b].i, R[c].i, t, mod); }
static inline bool ak_op_negn(ak_val *R, uint16_t a, uint16_t b, int t) { return ak_negn(&R[a].i, R[b].i, t); }
static inline bool ak_op_pown(ak_val *R, uint16_t a, uint16_t b, uint16_t c, int t) { return ak_pown(&R[a].i, R[b].i, R[c].i, t); }
static inline bool ak_op_shiftn(ak_val *R, uint16_t a, uint16_t b, uint16_t c, int t, bool right) { return ak_shiftn(&R[a].i, R[b].i, R[c].i, t, right); }
static inline bool ak_op_convn(ak_val *R, uint16_t a, uint16_t b, int t) { return ak_convn(&R[a].i, R[b].i, t); }

// ak_op_f2n is F2N: toward zero, the nearest value past type t's range,
// 0 for NaN.
static inline int64_t ak_op_f2n(double d, int t) {
	if (d != d) return 0;
	if (t == AK_U64) return d <= 0 ? 0 : d >= 18446744073709551616.0 ? -1 : (int64_t)(uint64_t)d;
	int64_t v = d >= 9.2233720368547758e18 ? INT64_MAX : d <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)d;
	return ak_int_clamp(v, AK_I64, t);
}


static inline int64_t ak_op_f2i(double d) {
	return d != d ? 0 : d >= 9.2233720368547758e18 ? INT64_MAX : d <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)d;
}

static inline ak_str *ak_op_tostr(ak_val v, uint16_t k) {
	switch (k) {
	case AK_K_INT: return ak_str_from_int(v.i);
	case AK_K_FLOAT: return ak_str_from_float(v.f);
	case AK_K_BOOL: return ak_str_from_bool(v.i != 0);
	case AK_K_U64TEXT: return ak_str_from_uint((uint64_t)v.i);
	default: return ak_retain(v.p ? v.p : ak_str_empty());
	}
}

// ak_op_interp is INTERP: n registers from b one after another, as
// kinds says: a byte each, the register's kind, or AK_INTERP_FMT | its
// kind, then its format's length and its format (a part written by
// "{x:>8.2f}"'s spec: ak_strb_fmt), so a formatted part takes no
// string of its own. Past kinds' end, parts are strings: no kinds at
// all, all strings (a + b + c), joined at their size (ak_strs_join).
// ak_op_interp_in is INTERP into buf (compiled code's stack string,
// rt.h's ak_strbuf) when the result fits there; buf NULL: on the heap.
#define AK_INTERP_FMT 0x80
static inline ak_str *ak_op_interp_in(ak_strbuf *buf, const ak_val *R, uint16_t b, uint16_t n, const ak_str *kinds) {
	if (kinds->len == 0) return buf ? ak_strs_join_in(buf, R + b, n) : ak_strs_join(R + b, n);
	ak_strb sb = {0};
	const uint8_t *k = (const uint8_t *)kinds->data, *kend = k + kinds->len;
	for (uint16_t i = 0; i < n; i++) {
		ak_val v = R[b + i];
		uint8_t kd = k < kend ? *k++ : AK_K_REF;
		if (kd & AK_INTERP_FMT) {
			uint8_t len = k < kend ? *k++ : 0;
			if (len > kend - k) len = (uint8_t)(kend - k);
			ak_strb_fmt(&sb, v, (ak_kind)(kd & ~AK_INTERP_FMT), (const char *)k, len);
			k += len;
			continue;
		}
		switch (kd) {
		case AK_K_INT: ak_strb_int(&sb, v.i); break;
		case AK_K_FLOAT: ak_strb_float(&sb, v.f); break;
		case AK_K_BOOL: ak_strb_bool(&sb, v.i != 0); break;
		default:
			if (v.p) ak_strb_str(&sb, v.p);
			break;
		}
	}
	return buf ? ak_strb_finish_in(&sb, buf) : ak_strb_finish(&sb);
}

static inline ak_str *ak_op_interp(const ak_val *R, uint16_t b, uint16_t n, const ak_str *kinds) { return ak_op_interp_in(NULL, R, b, n, kinds); }

void ak_vm_closure_drop(ak_closure *c);

static inline ak_closure *ak_op_clos(const ak_func *g, const ak_val *caps) {
	ak_closure *c = ak_closure_new(ak_vm_closure_invoke, g, g->ncap, g->capkinds);
	c->sig = g->sig;
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

// ak_vbox_new is VBOX's box: a var's, holding v (retained).
static inline ak_box *ak_vbox_new(ak_val v, ak_kind k) {
	ak_box *b = ak_box_new(k, v);
	ak_retain_val(b->v, k);
	b->h.flags = 1;
	return b;
}

static inline void ak_op_vbox(ak_val *R, uint16_t a, ak_val v, uint16_t k) { ak_op_setref(R, a, ak_vbox_new(v, (ak_kind)k)); }

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

// VTAKE: a box's reference moved out, none left in it, so a change of it
// in place copies nothing; VSET puts it back.
static inline void ak_op_vtake(ak_val *R, uint16_t a, ak_box *b) {
	ak_op_setref(R, a, b->v.p);
	b->v.p = NULL;
}

static inline ak_struct *ak_op_news(const ak_shape *s, const ak_val *f) {
	ak_struct *o = ak_struct_new(s);
	for (uint16_t i = 0; i < s->nfields; i++) {
		o->f[i] = f[i];
		ak_retain_val(o->f[i], (ak_kind)s->kinds[i]);
	}
	return o;
}

// ak_op_isshape is ISSHAPE: whether obj (an interface's value, or none)
// is a struct or an enum of shape s, its type: a type test.
static inline bool ak_op_isshape(const void *obj, const ak_shape *s) {
	const ak_obj *o = obj;
	if (!o) return false;
	if (o->tag == AK_TAG_STRUCT) return ((const ak_struct *)o)->shape == s;
	if (o->tag == AK_TAG_ENUM) return ((const ak_enum *)o)->shape == s;
	return false;
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
	ak_arr *arr = ak_arr_new(k, s, n);  // k: AK_ARR_KIND's
	for (uint16_t i = 0; i < n; i++) {
		ak_arr_put(arr, i, items[i]);
		ak_retain_val(items[i], (ak_kind)(k & 0xf));
	}
	arr->len = n;
	return arr;
}

// ak_op_concat is CONCAT: a = b + c; a = a + c (s += x) appends in
// place when a's string is its alone (out of the interpreter's loop).
__attribute__((noinline, unused)) static void ak_op_concat(ak_val *R, uint16_t a, uint16_t b, uint16_t c) {
	if (a == b) {
		ak_str_append((ak_str **)&R[a].p, R[c].p);
		return;
	}
	ak_op_setref(R, a, ak_str_concat(R[b].p, R[c].p));
}

// AK_VALS, AK_REFS and AK_BOOLS are an array's elements as compiled
// code reads and writes them in place (lang/cgen's elemSlot, elemRef):
// an array of values', those of references read and written as
// pointers (not through the ak_val union, which C takes to alias
// anything, so a store of a struct's field through one would have the
// element's pointer read again), a packed [bool]'s bools
// (ak_arr_bools). A packed array holds no references.
//
// A reference's slot is a value's (8 bytes) wherever a pointer is
// smaller (wasm32, the watch's arm64_32): its pointer then the rest of
// the value, so AK_REFS(a)[i].p is element i's pointer at a value's
// stride, as AK_VALS(a)[i].p is, and a loop's cursor of references
// (AK_CUR_OPEN) steps a value at a time.
typedef struct ak_ref_slot {
	void *p;
#if UINTPTR_MAX == UINT32_MAX
	uint32_t rest_;
#endif
} ak_ref_slot;
_Static_assert(sizeof(ak_ref_slot) == sizeof(ak_val), "a reference's slot is a value's");
_Static_assert(_Alignof(ak_ref_slot) <= _Alignof(ak_val), "a value's slot holds a reference's");
#define AK_VALS(a) ((ak_val *)(a)->items)
#define AK_REFS(a) ((ak_ref_slot *)(a)->items)
#define AK_BOOLS(a) ((bool *)(a)->items)

// ak_op_range throws an index out of arr's range (cold: the checks'
// paths stay short).
// The checks themselves are inline: one unsigned compare (a negative
// index is a huge one).
__attribute__((cold, noinline, unused)) static bool ak_op_range(const ak_arr *arr, int64_t i) {
	ak_throw_msg("index %lld out of range (length %u)", (long long)i, arr->len);
	return false;
}

// A packed array's element read and written out of the interpreter's
// loop (its handlers small: the registers it keeps are every
// instruction's).
__attribute__((noinline, unused)) static void ak_op_idx_packed(ak_val *R, uint16_t a, const ak_arr *arr, int64_t i) {
	R[a].i = arr->elem == AK_E_BOOL ? AK_BOOLS(arr)[i] : ak_arr_get_packed(arr, (uint32_t)i).i;
}

__attribute__((noinline, unused)) static void ak_op_setidx_packed(ak_arr *arr, int64_t i, ak_val v) {
	if (arr->elem == AK_E_BOOL) AK_BOOLS(arr)[i] = v.i != 0;
	else ak_arr_put_packed(arr, (uint32_t)i, v);
}

static inline bool ak_op_idx(ak_val *R, uint16_t a, const ak_arr *arr, int64_t i) {
	if (AK_UNLIKELY((uint64_t)i >= (uint64_t)arr->len)) return ak_op_range(arr, i);
	if (AK_UNLIKELY(arr->elem != AK_E_VAL)) {
		ak_op_idx_packed(R, a, arr, i);
		return true;
	}
	ak_val v = AK_VALS(arr)[i];
	if (ak_arr_kind(arr) == AK_K_REF) ak_op_setref(R, a, ak_retain(v.p));
	else R[a] = v;
	return true;
}

// SETIDX of an array this register holds alone changes it in place
// here (ak_arr_set's work for a unique array: the old element let go
// of); a shared one is copied first by ak_arr_set.
static inline bool ak_op_setidx(ak_val *R, uint16_t a, int64_t i, ak_val v) {
	ak_arr **ap = (ak_arr **)&R[a].p;
	ak_arr *arr = *ap;
	if (AK_UNLIKELY((uint64_t)i >= (uint64_t)arr->len)) return ak_op_range(arr, i);
	ak_kind k = ak_arr_kind(arr);
	ak_retain_val(v, k);
	if (AK_LIKELY(ak_unique(arr))) {
		if (AK_LIKELY(arr->elem == AK_E_VAL)) {
			if (k == AK_K_REF) ak_release(AK_VALS(arr)[i].p);
			AK_VALS(arr)[i] = v;
		} else {
			ak_op_setidx_packed(arr, i, v);
		}
	} else {
		ak_arr_set(ap, i, v);
	}
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

// ak_op_mgetj puts m[key] in register a, of the map's value kind (a
// scalar as it is: no box); false when m has no key.
static inline bool ak_op_mgetj(ak_val *R, uint16_t a, const ak_map *m, ak_val key) {
	ak_val v;
	if (!ak_map_get(m, key, &v)) return false;
	if ((ak_kind)(m->h.aux >> 4) == AK_K_REF) ak_op_setref(R, a, ak_retain(v.p));
	else R[a] = v;
	return true;
}

// ak_mset is MSET on the map *mp: k and v retained as its kinds say.
static inline void ak_mset(ak_map **mp, ak_val k, ak_val v) {
	ak_kind kk = (ak_kind)((*mp)->h.aux & 0xf), vk = (ak_kind)((*mp)->h.aux >> 4);
	ak_retain_val(k, kk);
	ak_retain_val(v, vk);
	ak_map_set(mp, k, v);
}

// ak_mset_k is ak_mset for compiled code, which knows whether k and v
// may be references (kref, vref: constants): a scalar's is never
// retained.
static inline void ak_mset_k(ak_map **mp, ak_val k, ak_val v, bool kref, bool vref) {
	ak_kind kk = (ak_kind)((*mp)->h.aux & 0xf), vk = (ak_kind)((*mp)->h.aux >> 4);
	if (kref) ak_retain_val(k, kk);
	if (vref) ak_retain_val(v, vk);
	ak_map_set(mp, k, v);
}

static inline void ak_op_mset(ak_val *R, uint16_t a, ak_val k, ak_val v) { ak_mset((ak_map **)&R[a].p, k, v); }

// ak_op_task starts the closure as a task (or, a C closure, takes the
// future it returns).
static inline ak_val ak_op_task(ak_closure *c) {
	if (c->invoke == ak_vm_closure_invoke) return (ak_val){.p = ak_vm_task_start(c->fn, c, NULL, 0)};
	return c->invoke(c, NULL, 0);
}

// ---- What the optimizer makes (lang/ir's passes) ----
//
// Each is what the instructions it replaces did: the same results, the
// same errors with the same messages.

// ADDIS, SUBIS and MULIS (an int and a constant) are ak_addi, ak_subi
// and ak_muli with the constant as c: checked as ADDI, SUBI and MULI
// are. ak_op_addk... are the interpreter's, on a frame's registers.
static inline bool ak_op_addk(ak_val *R, uint16_t a, int64_t x, int64_t k) { return ak_addi(&R[a].i, x, k); }
static inline bool ak_op_subk(ak_val *R, uint16_t a, int64_t x, int64_t k) { return ak_subi(&R[a].i, x, k); }
static inline bool ak_op_mulk(ak_val *R, uint16_t a, int64_t x, int64_t k) { return ak_muli(&R[a].i, x, k); }

// ak_op_idxf is IDXF: the field f of the struct at index i of arr (IDX,
// then GETF, with no reference to the element taken).
static inline bool ak_op_idxf(ak_val *R, uint16_t a, const ak_arr *arr, int64_t i, uint16_t f) {
	if (AK_UNLIKELY((uint64_t)i >= (uint64_t)arr->len)) return ak_op_range(arr, i);
	ak_op_getf(R, a, AK_VALS(arr)[i].p, f);
	return true;
}

// ak_setif is SETIF on the array *ap: the field f of the struct at index
// i set to v (retained as the field's kind says), the array and then the
// struct copied first if they're shared (what IDX, SETIDX of none, SETF
// and SETIDX back did, with no reference to the element taken).
static inline bool ak_setif(ak_arr **ap, int64_t i, uint16_t f, ak_val v) {
	if (AK_UNLIKELY((uint64_t)i >= (uint64_t)(*ap)->len)) return ak_op_range(*ap, i);
	ak_arr *arr = ak_arr_mut(ap);
	ak_struct *o = ak_struct_mut((ak_struct **)&AK_VALS(arr)[i].p);
	if (o->shape->kinds[f] == AK_K_REF) {
		void *old = o->f[f].p;
		o->f[f].p = ak_retain(v.p);
		ak_release(old);
	} else {
		o->f[f] = v;
	}
	return true;
}

static inline bool ak_op_setif(ak_val *R, uint16_t a, int64_t i, uint16_t f, ak_val v) { return ak_setif((ak_arr **)&R[a].p, i, f, v); }

// ak_op_exit releases a frame's reference registers but keep (-1: none).
static inline void ak_op_exit(ak_val *R, const uint8_t *kinds, uint16_t n, int keep) {
	for (uint16_t i = 0; i < n; i++)
		if (kinds[i] == AK_K_REF && (int)i != keep) ak_release(R[i].p);
}

// ---- Functions compiled to C (lang/cgen) ----
//
// A synchronous function's direct body takes its arguments as C values,
// then its closure (and, but on arm64, the thread's stack limit: below),
// and returns its result with whether it threw: ak_ret, two machine
// registers on the ABIs' calls (x0 and x1, rax and rdx), so a compiled
// caller tests a register for the error rather than the thread's
// pending one. Its check of the stack costs no thread-local read: one
// compare of the stack pointer, as Go's prologue is.
// ak_cg_sp is the C stack's pointer, read without making a frame (so
// that a body's check comes before its frame is set up, and a path that
// needs none, a recursion's base case, makes none).
static inline uintptr_t ak_cg_sp(void) {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
	// Read as the register it is, 64 bits (arm64_32's pointers, an
	// Apple Watch's, are 32: the stack's in its low 4 GB).
	uint64_t sp;
	__asm__("mov %0, sp" : "=r"(sp));
	return (uintptr_t)sp;
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
	uintptr_t sp;
	__asm__("mov %%rsp, %0" : "=r"(sp));
	return sp;
#else
	return (uintptr_t)__builtin_frame_address(0);
#endif
}

// ak_cg_stack_low is ak_stack_low with the thread's limit given: the C
// stack's pointer below it. On the web, the engine's stack is asked too
// (base.h).
static inline bool ak_cg_stack_low(uintptr_t lim) {
#if defined(__wasm__)
	AK_UNUSED(lim);
	return ak_stack_low();
#else
	return ak_cg_sp() < lim;
#endif
}

// The thread's stack limit in compiled code. On arm64 the app's C is
// compiled with x28 reserved (-ffixed-x28, Zig's -mcpu=...+reserve_x28,
// and -DAK_CG_X28: cbuild.AppFlags; unreserved, the asm below overwrites
// what the compiler keeps there), as Go keeps its g in a register of its
// own: every way into the app's C from the core (fN, fN_c, an async
// body's fN) sets x28 to the thread's limit and puts back the core's
// (x28 is callee-saved: the core keeps
// its own across its calls into the app, and the app's across the
// core's), so a check is one compare of the stack pointer with it (cmp
// sp, x28) and a direct body takes no limit: no register of a recursing
// function's holds it, and no call passes it on. Elsewhere (amd64, the
// web, a C compiler without the flag) the limit is a direct body's
// parameter, lim_: the C cgen writes is the same either way.
//
//   AK_CG_LIM_P   a direct body's parameter after its closure: ", uintptr_t lim_", or none
//   AK_CG_LIM_T   its type, in a pointer to a body's type: ", uintptr_t", or none
//   AK_CG_LIM_A   the argument a call passes it: ", lim_", or none
//   AK_CG_LIM     the limit's value (for an ak_direct_body, which takes it)
//   AK_CG_CHECK(l)   goes to label l when the stack is past the limit
//   AK_CG_ENTER(lim), AK_CG_LEAVE()   a way in from the core: the limit set, and the core's put back
#if defined(__aarch64__) && defined(AK_CG_X28) && (defined(__GNUC__) || defined(__clang__))
#define AK_CG_LIM_P
#define AK_CG_LIM_T
#define AK_CG_LIM_A
static inline uintptr_t ak_cg_x28(void) {
	uintptr_t v;
	__asm__("mov %0, x28" : "=r"(v));
	return v;
}
#define AK_CG_LIM ak_cg_x28()
// A compare and a branch (asm goto: a flag output would be set into a
// register, then tested).
#define AK_CG_CHECK(l) __asm__ goto("cmp sp, x28\n\tb.lo %l0" : : : "cc" : l)
#define AK_CG_ENTER(lim) \
	uintptr_t x28_; \
	__asm__ volatile("mov %0, x28\n\tmov x28, %1" : "=&r"(x28_) : "r"((uintptr_t)(lim)) : "memory")
#define AK_CG_LEAVE() __asm__ volatile("mov x28, %0" : : "r"(x28_) : "memory")
#else
#define AK_CG_LIM_P , uintptr_t lim_
#define AK_CG_LIM_T , uintptr_t
#define AK_CG_LIM_A , lim_
#define AK_CG_LIM lim_
#define AK_CG_CHECK(l) \
	do { \
		if (AK_UNLIKELY(ak_cg_stack_low(lim_))) goto l; \
	} while (0)
#define AK_CG_ENTER(lim) uintptr_t lim_ = (lim)
#define AK_CG_LEAVE() AK_UNUSED(lim_)
#endif

// ak_cg_pending is ak_error_pending() for compiled code: the thread's
// error slot found once in a function (*slot, NULL until then), then
// read as memory. A thread-local's read is a call on some systems
// (Apple's), and every call to a builtin is followed by this test.
static inline bool ak_cg_pending(void *const **slot) {
	if (AK_UNLIKELY(!*slot)) *slot = ak_error_slot();
	return **slot != NULL;
}

// AK_CG_OPAQUE(x) hides what the int lvalue x holds from the C
// compiler's analysis (it costs no instruction): an add lang/cgen knows
// can't overflow is written unchecked and then this, so that the C
// compiler sees no more of it than it did of the checked add's result.
#if defined(__GNUC__) || defined(__clang__)
#define AK_CG_OPAQUE(x) __asm__("" : "+r"(x))
#else
#define AK_CG_OPAQUE(x) ((void)0)
#endif

// ak_cg_overflow throws a stack overflow in fn and returns a body's
// result for it: in another file (interp.c), so that a body's check
// tail-calls it and the check's path needs no frame.
ak_ret ak_cg_overflow(const char *fn) __attribute__((cold));

// ak_cg_unwind tells ak_frame_hook (if set) of a compiled function's
// frame an error leaves, at line. It's cold and in another file, so the
// C compiler takes every way to it as unlikely and lays the error's
// paths out of the way: a call's test of the error it returns falls
// through to what follows when it didn't throw, and the function's
// returns aren't jumped to (the hook's own test would read as an even
// branch, and the error's block would sit in the way).
void ak_cg_unwind(const char *fn, const char *file, uint32_t line) __attribute__((cold));

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
	if (!n) return;  // R is NULL then
	ak_val *S = co->regs + co->frames[fi].base;
	memcpy(R, S, sizeof(ak_val) * n);
	memset(S, 0, sizeof(ak_val) * n);
}

// ak_op_leave moves R back into frame fi, which resumes at pc.
static inline void ak_op_leave(ak_co *co, uint32_t fi, const ak_val *R, uint16_t n, uint32_t pc) {
	if (n) memcpy(co->regs + co->frames[fi].base, R, sizeof(ak_val) * n);
	co->frames[fi].pc = pc;
}

// ak_op_pushes reports whether calling closure c from a function that
// may suspend pushes a frame on its coroutine (the interpreter's CALLC,
// a compiled async function's) rather than calling it from C.
static inline bool ak_op_pushes(const ak_co *co, const ak_closure *c) {
	if (c->invoke != ak_vm_closure_invoke) return false;
	const ak_func *g = c->fn;
	return !g->native && !((g->flags & AK_F_ASYNC) && co->task == NULL);
}

// ak_op_apush pushes the frame of a call a compiled async function makes
// (lang/cgen's acall): f with clos, nargs arguments from args, its
// result to go in dst. Inline, as the interpreter's push_frame: a call
// that doesn't wait costs this, the callee's body and a pop, so its
// common case (room for the frame and its registers, not a mut fn) is
// stores, not ak_vm_push's general path.
static inline bool ak_op_apush(ak_co *co, const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs, uint16_t dst, uint32_t callpc) {
	if (AK_UNLIKELY(co->nframes >= AK_VM_MAX_FRAMES || nargs != f->nparams || co->nframes == co->fcap || (f->flags & AK_F_MUT) || co->top + f->nregs > co->cap))
		return ak_vm_push(co, f, clos, args, nargs, dst, false, callpc);
	uint32_t base = co->top;
	ak_val *R = co->regs + base;
	const uint8_t *k = f->kinds;
	for (uint32_t i = 0; i < nargs; i++) {
		R[i] = args[i];
		ak_retain_val(R[i], (ak_kind)k[i]);  // its pointer hidden from GCC (rt.h)
	}
	for (uint32_t i = nargs; i < f->nregs; i++) R[i] = (ak_val){0};
	co->top = base + f->nregs;
	if (clos) ak_retain(clos);
	co->frames[co->nframes++] = (ak_frame){.fn = f, .clos = clos, .callpc = callpc, .base = base, .udepth = co->nuntracks, .dst = dst};
	return true;
}

// ak_op_append_* is a fused loop's push (lang/cgen's arr.append) of an
// int (_i), a float (_f) or a reference (_p): the array comes back,
// grown or copied, rather than through its address, so it stays in a
// register across the loop (the slow path's address is taken in a
// function of its own). The element is stored as an ak_val: stored as
// its C type, the compiler keeps the source's pointer in a register
// and the loop retires two instructions fewer, but takes half again as
// many cycles on Apple's M5 (bench/std's seq).
__attribute__((noinline, cold, unused)) static ak_arr *ak_op_append_slow(ak_arr *a, ak_val v) {
	ak_arr_push_slow(&a, v);
	return a;
}
#define AK_OP_APPEND(name, T, member) \
	static inline ak_arr *ak_op_append_##name(ak_arr *a, T v) { \
		if (AK_LIKELY(ak_arr_room(a) && a->elem == AK_E_VAL && ak_unique(a))) { \
			((ak_val *)a->items)[a->len++] = (ak_val){.member = v}; \
			return a; \
		} \
		return ak_op_append_slow(a, (ak_val){.member = v}); \
	} \
	/* arr.append_room's: the array is the loop's own (made there, so \
	   unique), holds values, and has the room (reserved for all it \
	   appends), so a store and a count; a build with assertions (tests', \
	   a player's) checks it. */ \
	static inline void ak_op_append_room_##name(ak_arr *a, T v) { \
		AK_OP_ROOM_CHECK(a); \
		((ak_val *)a->items)[a->len++] = (ak_val){.member = v}; \
	} \
	/* arr.append_own's: the array is the loop's own and holds values \
	   (a filter's), so only its room is checked. */ \
	static inline ak_arr *ak_op_append_own_##name(ak_arr *a, T v) { \
		if (AK_LIKELY(ak_arr_room(a))) { \
			AK_OP_OWN_CHECK(a); \
			((ak_val *)a->items)[a->len++] = (ak_val){.member = v}; \
			return a; \
		} \
		return ak_op_append_slow(a, (ak_val){.member = v}); \
	}
#ifndef NDEBUG
#define AK_OP_ROOM_CHECK(a) AK_ASSERT(ak_unique(a) && ak_arr_room(a) && (a)->elem == AK_E_VAL)
#define AK_OP_OWN_CHECK(a) AK_ASSERT(ak_unique(a) && (a)->elem == AK_E_VAL)
#else
#define AK_OP_ROOM_CHECK(a) ((void)(a))
#define AK_OP_OWN_CHECK(a) ((void)(a))
#endif
// A fused loop's write cursor (lang/cgen's cursor.go): where its next
// append's element goes (w) and where the array's room ends (e), in
// locals the loop keeps, its count written back (AK_CUR_CLOSE) on the
// ways out of the loop. The array is the loop's own, of values; a
// cursor points to int64_t, double or ak_ref_slot, each a value's size.
#define AK_CUR_OPEN(a, w, e) do { \
		ak_arr *c_ = (a); \
		AK_OP_OWN_CHECK(c_); \
		(w) = (void *)((ak_val *)c_->items + c_->len); \
		(e) = (void *)((ak_val *)c_->items + c_->cap); \
	} while (0)
#define AK_CUR_CLOSE(a, w) (((ak_arr *)(a))->len = (uint32_t)((ak_val *)(void *)(w) - (ak_val *)((ak_arr *)(a))->items))
#ifndef NDEBUG
#define AK_CUR_ROOM(w, e) AK_ASSERT((void *)(w) < (void *)(e))
#else
#define AK_CUR_ROOM(w, e) ((void)0)
#endif
AK_OP_APPEND(i, int64_t, i)
AK_OP_APPEND(f, double, f)
AK_OP_APPEND(p, void *, p)
#undef AK_OP_APPEND

// ak_op_lim is the C stack's limit for a compiled async function's body
// on co: the thread's, kept on the coroutine for its run (ak_vm_run
// clears it, since a task may resume on another thread), so a call reads
// no thread-local.
static inline uintptr_t ak_op_lim(ak_co *co) {
	if (AK_UNLIKELY(!co->lim)) co->lim = ak_stack_limit();
	return co->lim;
}

// ak_op_acall runs the frame a compiled async function just pushed: its
// body, directly, when it's compiled too, the direct calls aren't deep
// and the C stack isn't low (lim: the caller's ak_op_lim), popping its
// frame when it returned or threw; else it's left to the run loop
// (AK_RUN_PUSHED), as is the rest of a call that suspended.
static inline int ak_op_acall(ak_co *co, ak_val *v, uintptr_t lim) {
	const ak_func *g = co->frames[co->nframes - 1].fn;
	if (!g->resume || co->adepth >= AK_VM_ADEPTH_MAX || ak_cg_stack_low(lim)) return AK_RUN_PUSHED;
	co->adepth++;
	int s = g->resume(co, v);
	co->adepth--;
	if (s == AK_RUN_RET || s == AK_RUN_THROW) ak_vm_pop_frame(co);
	return s;
}

#endif
