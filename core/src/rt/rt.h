// rt.h: the values Asuka programs compute with, shared by the bytecode
// VM (core/src/vm) and the C that release builds compile to (lang/cgen).
//
// A value is 8 bytes (ak_val). Its type is known statically, so a value
// carries no tag: the compiler knows whether a slot holds an int, a
// float, a bool or a reference to a heap object (ak_obj), and emits the
// matching operations. Heap objects are reference counted; values with
// value semantics (strings, bytes, arrays, maps, structs, tuples, enums
// with payloads, boxed optionals) are copied on write: a mutation of an
// object someone else also holds copies it first, so nobody else sees it.
//
// Everything here runs on the main thread, except the functions that say
// otherwise (ak_deep_copy, used to cross into spawn's worker threads).
#ifndef AK_RT_H
#define AK_RT_H

#include "base/base.h"

typedef union ak_val {
	int64_t i;  // int, bool (0/1), Duration (ns), Color, payloadless enums
	double f;   // float
	void *p;    // a heap object, NULL for none
} ak_val;

// How a slot is stored: what the runtime needs to release, copy, compare
// and hash a value without knowing its full type.
typedef enum ak_kind {
	AK_K_VOID = 0,
	AK_K_INT,
	AK_K_FLOAT,
	AK_K_BOOL,
	AK_K_REF,  // a heap object or NULL
} ak_kind;

// AK_K_U64TEXT is never a slot's kind (a u64 is an int's 64 bits): what
// TOSTR is told of a u64, which it writes unsigned.
#define AK_K_U64TEXT 5

// ---- Integers ----
//
// The integer types, by their code (lang/check's IntCode): log2 of their
// bytes, 4 more when they're unsigned. A value of one is held as an int,
// sign- or zero-extended from its width (canonical: a u8 is 0 to 255), a
// u64 as its 64 bits (past int's largest, a negative int64).
typedef enum ak_int_type {
	AK_I8,
	AK_I16,
	AK_I32,
	AK_I64,  // int
	AK_U8,
	AK_U16,
	AK_U32,
	AK_U64,
} ak_int_type;

extern const char *const ak_int_names[8];  // "i8" … "u64", "int" for AK_I64

static inline bool ak_int_signed(int t) { return t < AK_U8; }

// ak_int_wrap is v's low bits at type t, sign- or zero-extended.
static inline int64_t ak_int_wrap(int64_t v, int t) {
	switch (t) {
	case AK_I8: return (int8_t)v;
	case AK_I16: return (int16_t)v;
	case AK_I32: return (int32_t)v;
	case AK_U8: return (uint8_t)v;
	case AK_U16: return (uint16_t)v;
	case AK_U32: return (uint32_t)v;
	default: return v;
	}
}

// ak_int_min and ak_int_max are type t's range (a u64's largest as its
// bits: -1).
static inline int64_t ak_int_min(int t) { return ak_int_signed(t) ? (t == AK_I64 ? INT64_MIN : -((int64_t)1 << ((8 << t) - 1))) : 0; }
static inline int64_t ak_int_max(int t) {
	if (t == AK_U64 || t == AK_I64) return t == AK_U64 ? -1 : INT64_MAX;
	return ak_int_signed(t) ? ((int64_t)1 << ((8 << t) - 1)) - 1 : ((int64_t)1 << (8 << (t & 3))) - 1;
}

// ak_int_fits reports whether v, a value of type from, is one of type to.
static inline bool ak_int_fits(int64_t v, int from, int to) {
	if (v < 0 && (from == AK_U64 || to == AK_U64)) return from == to;  // a u64 past int's largest, or a negative
	return ak_int_wrap(v, to) == v;
}

// ak_int_clamp is v, of type from, as the nearest value of type to.
static inline int64_t ak_int_clamp(int64_t v, int from, int to) {
	if (ak_int_fits(v, from, to)) return v;
	return v < 0 && from != AK_U64 ? ak_int_min(to) : ak_int_max(to);
}

// ak_int_lt orders two values of type t.
static inline bool ak_int_lt(int64_t a, int64_t b, int t) { return t == AK_U64 ? (uint64_t)a < (uint64_t)b : a < b; }

// ak_int_mul is a * b at type t, false when it overflows.
static inline bool ak_int_mul(int64_t a, int64_t b, int t, int64_t *out) {
	if (t == AK_U64) {
		uint64_t u;
		if (__builtin_mul_overflow((uint64_t)a, (uint64_t)b, &u)) return false;
		*out = (int64_t)u;
		return true;
	}
	return !__builtin_mul_overflow(a, b, out) && ak_int_wrap(*out, t) == *out;
}

// ak_int_pow is b ** e at type t (e as unsigned: a count), false when it
// overflows. By squaring: one multiplication per bit of e, the last
// square not taken (it would overflow where the result doesn't).
static inline bool ak_int_pow(int64_t b, uint64_t e, int t, int64_t *out) {
	int64_t r = 1;
	if (b == 0 || b == 1) {
		*out = e == 0 ? 1 : b;
		return true;
	}
	if (b == -1 && t != AK_U64) {
		*out = e & 1 ? -1 : 1;
		return true;
	}
	for (;;) {
		if ((e & 1) && !ak_int_mul(r, b, t, &r)) return false;
		e >>= 1;
		if (!e) break;
		if (!ak_int_mul(b, b, t, &b)) return false;
	}
	*out = r;
	return true;
}

// ak_int_parse reads text (n bytes) whole as an integer of type t in
// radix (2 to 36, letters in either case), an optional sign first: false
// when it isn't one, or doesn't fit.
bool ak_int_parse(const char *text, size_t n, int radix, int t, int64_t *out);
// ak_int_text writes v, of type t, in radix (2 to 36, lowercase) into
// buf (66 bytes hold any): its length.
int ak_int_text(char *buf, int64_t v, int t, int radix);
// ak_int_throw_fit throws that v, of type from, doesn't fit type to.
void ak_int_throw_fit(int64_t v, int from, int to);

// A heap object's tag.
typedef enum ak_tag {
	AK_TAG_STR = 1,
	AK_TAG_BYTES,
	AK_TAG_ARR,
	AK_TAG_MAP,
	AK_TAG_STRUCT,   // structs, tuples, errors
	AK_TAG_ENUM,     // a case of an enum with payloads
	AK_TAG_BOX,      // an optional scalar that isn't none
	AK_TAG_CLOSURE,
	AK_TAG_CELL,     // a state cell (vm, ui)
	AK_TAG_MEMO,
	AK_TAG_ACC,      // an accessor: what a component's parameter reads
	AK_TAG_STORE,
	AK_TAG_TASK,
	AK_TAG_VIEW,
	AK_TAG_NATIVE,   // native/native.h's objects
	AK_TAG_OPAQUE,   // runtime objects with their own destructor
	AK_TAG_TREE,     // a B-tree: std/collections' SortedMap's and SortedSet's (tree.c)
	AK_TAG_TNODE,    // one of its nodes
} ak_tag;

typedef struct ak_obj {
	uint32_t rc;
	uint8_t tag;
	uint8_t flags;
	uint16_t aux;  // per tag: an array's element kind, a box's kind
} ak_obj;

#define AK_RC_IMMORTAL 0xffffffffu

// A string compiled code builds in its own C frame (lang/cgen's stack
// strings: ak_strbuf) is immortal too, so that a release of it does
// nothing, and flagged AK_STR_STACK (its h.flags): it never outlives the
// function that made it, which the compiler proved keeps it nowhere. A
// retain is a keep, so the tests' builds (AK_TEST: ASan's) panic on one
// (ak_retain_local is the frame's own copy of it); not every build with
// assertions, as the check in each inlined retain changes how the
// interpreter's loop compiles. A copy for another thread copies it.
#define AK_STR_STACK 8
static inline bool ak_obj_on_stack(const ak_obj *o) { return o->tag == AK_TAG_STR && (o->flags & AK_STR_STACK); }

void ak_destroy(ak_obj *o);

// ak_obj_live counts the objects made and not yet destroyed (immortal
// ones aside: constants, never destroyed), so tests can tell that what code made is freed: a
// runner's -leaks. Atomic: workers (spawn) make objects too. Counted in a
// build with assertions (NDEBUG unset), as ak_alloc_count is.
extern int64_t ak_obj_live;
#ifndef NDEBUG
#define ak_obj_count(d) __atomic_add_fetch(&ak_obj_live, (d), __ATOMIC_RELAXED)
#else
#define ak_obj_count(d) ((void)0)
#endif

static inline void *ak_retain(void *p) {
	if (p) {
		ak_obj *o = (ak_obj *)p;
		if (o->rc != AK_RC_IMMORTAL) o->rc++;
#ifdef AK_TEST
		else if (AK_UNLIKELY(ak_obj_on_stack(o))) ak_panic("a string on a function's stack retained: it would outlive its frame");
#endif
	}
	return p;
}

// ak_retain_local is ak_retain of what may be a stack string
// (AK_STR_STACK), by compiled code copying it into another of the
// frame's registers, which the compiler proved no longer-lived than it.
static inline void *ak_retain_local(void *p) {
	if (p) {
		ak_obj *o = (ak_obj *)p;
		if (o->rc != AK_RC_IMMORTAL) o->rc++;
	}
	return p;
}

static inline void ak_release(void *p) {
	if (p) {
		ak_obj *o = (ak_obj *)p;
		if (o->rc != AK_RC_IMMORTAL && --o->rc == 0) ak_destroy(o);
	}
}

// AK_HIDE_PTR(p) hides what the pointer lvalue p holds from the C
// compiler's analysis, at no cost: a value whose kind is known only at
// run time may be an int the compiler sees as a constant (0, 1, ...),
// and GCC then reads ak_retain's ->rc on that path as a read at that
// address (-Warray-bounds, an error at -O2), though it never runs.
#if defined(__GNUC__) || defined(__clang__)
#define AK_HIDE_PTR(p) __asm__("" : "+r"(p))
#else
#define AK_HIDE_PTR(p) ((void)0)
#endif

static inline void ak_release_val(ak_val v, ak_kind k) {
	if (k == AK_K_REF) {
		void *p = v.p;
		AK_HIDE_PTR(p);
		ak_release(p);
	}
}

static inline void ak_retain_val(ak_val v, ak_kind k) {
	if (k == AK_K_REF) {
		void *p = v.p;
		AK_HIDE_PTR(p);
		ak_retain(p);
	}
}

// ak_unique reports whether the caller holds the only reference, so it
// may change the object in place.
static inline bool ak_unique(const void *p) { return ((const ak_obj *)p)->rc == 1; }

void *ak_obj_new(ak_tag tag, size_t size);      // rc 1, zeroed past the header
void *ak_obj_new_raw(ak_tag tag, size_t size);  // rc 1, the rest the caller's to write
void ak_make_immortal(void *p);

// ---- Shapes: the layout of a struct, a tuple or an enum's cases ----

typedef struct ak_shape ak_shape;

typedef struct ak_case {
	const char *name;
	uint16_t nfields;
	const uint8_t *kinds;          // ak_kind per payload field
	const ak_shape *const *shapes; // per field, the shape of a struct/enum field, or NULL
	const char *const *names;      // field labels, or NULL
} ak_case;

struct ak_shape {
	const char *name;
	uint16_t nfields;  // struct: its fields; enum: 0
	const uint8_t *kinds;
	const ak_shape *const *shapes;
	const char *const *names;
	uint16_t ncases;   // enums with payloads
	const ak_case *cases;
	bool is_error;     // declared `: Error`; field 0 is message when it has one
	// lits: its fields' names carry, after their JSON descriptors, how the
	// language writes them (a module's: vm/load.c's rname, vm/literal.c).
	bool lits;
	void *singletons;  // runtime: payloadless cases of a payload enum (ak_enum **)
	// What the type says itself (lang/check's Own): 1 << AK_HOOK_* for
	// each of its own ==, hash, text and JSON, which ak_shape_call calls;
	// jdesc is the descriptor of what its toJSON gives (fromJSON takes).
	uint8_t hooks;
	const char *jdesc;
};

// A type's own methods the runtime calls (a shape's hooks): equals(a, b)
// -> bool, hash(a) -> int, description(a) -> string, toJSON(a) -> J,
// fromJSON(j) -> T (it may throw), compare(a, b) -> int (what orders a
// type that says its own <: a B-tree's keys, tree.c).
enum { AK_HOOK_EQ, AK_HOOK_HASH, AK_HOOK_SHOW, AK_HOOK_TOJSON, AK_HOOK_FROMJSON, AK_HOOK_CMP, AK_NHOOKS };
// ak_shape_call calls the shape's hook with args (borrowed), returning
// its result (owned); an error it raises is pending. The VM's, set as it
// loads a module.
extern ak_val (*ak_shape_call)(const ak_shape *s, int hook, ak_val *args, uint32_t n);
static inline bool ak_shape_has(const ak_shape *s, int hook) { return s->hooks & (1u << hook); }

// ---- Strings ----

typedef struct ak_str {
	ak_obj h;       // h.flags: AK_STR_* (what's known of its bytes); h.aux: its room (ak_str_room)
	uint32_t len;
	uint32_t hash;  // 0: not computed yet
	char data[];    // len bytes, then a NUL
} ak_str;

// What a string's h.flags cache of its bytes, as its hash is cached:
// worked out when first asked for (ak_str_ascii), and before it becomes
// immortal (ak_str_hash), so worker threads only read them. Code that
// changes a string's bytes after it's made (it's the caller's, fresh
// from ak_str_new(NULL, n)) does so before anything reads them.
#define AK_STR_KNOWN 1  // AK_STR_ASCII says whether it's ASCII
#define AK_STR_ASCII 2  // every byte below 0x80: a character a byte
#define AK_STR_SLAB 4   // a piece of a slab (ak_pieces): h.aux is where in it, in 8 bytes
// AK_STR_STACK (8, above ak_retain): in a function's C frame (ak_strbuf)

// ak_str_room is how many bytes a string has room for before its NUL:
// its length, as it's made; once it's grown in place (ak_str_append),
// 2^h.aux - 1, its buffer a power of two that doubles as it fills.
static inline size_t ak_str_room(const ak_str *s) {
	return !s->h.aux || (s->h.flags & AK_STR_SLAB) ? s->len : ((size_t)1 << s->h.aux) - 1;
}

ak_str *ak_str_new(const char *s, size_t n);  // s NULL: n bytes the caller fills

// Pieces: the many small strings one operation cuts a string into
// (split, lines, fields, chars) are made one after another in slabs of
// 16 KB, each piece a string of its own (counted, released and freed one
// by one as any), the slab freed with its last piece: one allocation
// for hundreds of pieces, where each would take malloc's and free's
// tens of nanoseconds, and the pieces side by side in memory as the
// array that holds them walks them. A piece kept alone keeps its slab:
// 16 KB at most, not the string it was cut from (Go's substrings keep
// all of it); a slab is no bigger than what the pieces still to come
// can need (left, a bound: what cutting "1.2.3" takes, not 16 KB). A
// piece over AK_PIECE_MAX is a string of its own.
//
// ak_pieces_start begins cutting from (the pieces' bytes are its), left
// a bound of what they'll need (AK_PIECES_LEFT of n pieces of bytes
// bytes in all; 0: unknown, slabs of 16 KB); ak_piece makes the next
// piece, of n bytes from s; ak_pieces_end ends it, before any piece is
// let go of. A piece of 16 bytes or fewer is a header and one 16-byte
// copy, inline (the bytes past it that it copies are written over or
// past its end): a split's are words. A slab's count of its pieces is
// written as it's left, not one by one (each a store a load waits for).
typedef struct ak_pieces {
	char *slab, *at, *end;  // the slab being filled: at its next piece's place, end less 16 bytes of room
	const char *from_end;   // past the string cut's NUL: what a copy may read up to
	uint32_t count;         // the pieces made in the slab
	uint8_t flags;          // what each piece's h.flags are
	size_t left;            // what the pieces to come need at most (0: unknown)
} ak_pieces;
#define AK_PIECES_LEFT(n, bytes) ((size_t)(n) * (sizeof(ak_str) + 8) + (size_t)(bytes))
#define AK_PIECE_MAX 240
#if defined(__SANITIZE_ADDRESS__)
#define AK_NO_SLABS 1  // each piece a string of its own, so a use after its free is caught
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#define AK_NO_SLABS 1
#endif
#endif
void ak_pieces_start(ak_pieces *p, const ak_str *from, size_t left);
ak_str *ak_piece_slow(ak_pieces *p, const char *s, size_t n);
static AK_ALWAYS_INLINE ak_str *ak_piece(ak_pieces *p, const char *s, size_t n) {
#if !defined(AK_NO_SLABS)
	size_t need = (sizeof(ak_str) + n + 1 + 7) & ~(size_t)7;
	if (AK_LIKELY(n - 1 < 16 && need <= (size_t)(p->end - p->at) && s + 16 <= p->from_end)) {
		ak_str *o = (ak_str *)p->at;
		o->h = (ak_obj){.rc = 1, .tag = AK_TAG_STR, .flags = p->flags, .aux = (uint16_t)((size_t)(p->at - p->slab) >> 3)};
		o->len = (uint32_t)n;
		o->hash = 0;
		memcpy(o->data, s, 16);
		o->data[n] = 0;
		p->at += need;
		p->count++;
		ak_obj_count(1);
		return o;
	}
#endif
	return ak_piece_slow(p, s, n);
}
void ak_pieces_end(ak_pieces *p);
// ak_find_byte is where c first is in [p, end), or NULL: a word at a
// time, inline, which for the short runs between separators is what a
// call to memchr costs several times over.
static inline const char *ak_find_byte(const char *p, const char *end, char c) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	const uint64_t ones = 0x0101010101010101ull, pat = ones * (uint8_t)c;
	while (end - p >= 8) {
		uint64_t w;
		memcpy(&w, p, 8);
		w ^= pat;
		uint64_t z = (w - ones) & ~w & (ones << 7);  // its lowest set: the first byte that's c
		if (z) return p + (__builtin_ctzll(z) >> 3);
		p += 8;
	}
#endif
	for (; p < end; p++)
		if (*p == c) return p;
	return NULL;
}
// ak_space is the white space fields and trim cut at: ASCII's.
extern const bool ak_space_table[256];
static inline bool ak_space(char c) { return ak_space_table[(uint8_t)c]; }
// ak_piece_slab is the slab a piece is in: its count of live pieces
// first.
static inline uint32_t *ak_piece_slab(const ak_str *s) { return (uint32_t *)((char *)s - ((size_t)s->h.aux << 3)); }
// ak_piece_free frees a piece (its count released): its slab, when it
// was the last.
static inline void ak_piece_free(ak_str *s) {
	uint32_t *slab = ak_piece_slab(s);
	if (--*slab == 0) ak_free(slab);
}
static inline ak_str *ak_str_c(const char *s) { return ak_str_new(s, strlen(s)); }
ak_str *ak_str_empty(void);  // immortal
ak_str *ak_str_concat(ak_str *a, ak_str *b);
// ak_str_append makes *s *s + t: in place when *s is its holder's alone
// and has room, else in a buffer of twice the room (grown in place when
// *s is unique), so a string built by appending in a loop (s += x) is
// copied O(log n) times, as Swift's unique-reference append and Go's
// append grow theirs. Takes *s's reference; t borrowed (*s itself too).
void ak_str_append(ak_str **s, const ak_str *t);
ak_str *ak_str_from_int(int64_t v);
ak_str *ak_str_from_uint(uint64_t v);
ak_str *ak_str_from_float(double v);  // shortest round-trip form
// ak_float_text writes v as ak_str_from_float does into buf (32 bytes
// room); its length. No allocation.
int ak_float_text(char *buf, double v);
// ak_float_fixed and ak_float_exp write v as printf's "%.*f" and "%.*e"
// do, digit for digit (rounded half to even, as printf rounds the exact
// binary value), into buf (AK_FLOAT_FMT_MAX bytes room): its length.
// Without snprintf for what's printed (a precision to 19, digits that
// fit 64 bits): exact integer arithmetic.
#define AK_FLOAT_FMT_MAX 400
int ak_float_fixed(char *buf, double v, int prec);
int ak_float_exp(char *buf, double v, int prec);
ak_str *ak_str_from_bool(bool v);
bool ak_str_eq(const ak_str *a, const ak_str *b);
int ak_str_cmp(const ak_str *a, const ak_str *b);
uint32_t ak_str_hash(ak_str *s);
// ak_hash_bytes: n bytes' hash, wyhash's (final version 4): eight bytes
// a multiply, well mixed in every bit. Pure: any thread.
uint64_t ak_hash_bytes(const void *p, size_t n);
ak_str *ak_str_slice(ak_str *s, int64_t from, int64_t to);  // byte offsets, clamped
int64_t ak_str_count(const ak_str *s);   // characters (code points)
bool ak_str_ascii(const ak_str *s);      // whether every byte is ASCII (cached)
// ak_str_char_offset: the byte offset of character c (code point), 0 for
// c <= 0, len past the last.
int64_t ak_str_char_offset(const ak_str *s, int64_t c);
int64_t ak_str_find(const ak_str *s, const ak_str *sub);  // byte offset or -1
int64_t ak_str_find_last(const ak_str *s, const ak_str *sub);  // byte offset or -1
// ak_mem_find: where needle (nn bytes) first is in h (hn bytes), or -1:
// in time linear in hn (a rare byte's memchr, the two-way algorithm
// when that finds too many false starts).
int64_t ak_mem_find(const void *h, size_t hn, const void *needle, size_t nn);
int64_t ak_mem_find_last(const void *h, size_t hn, const void *needle, size_t nn);
bool ak_str_has_prefix(const ak_str *s, const ak_str *p);
bool ak_str_has_suffix(const ak_str *s, const ak_str *p);
bool ak_str_contains(const ak_str *s, const ak_str *sub);
ak_str *ak_str_lower(ak_str *s);
ak_str *ak_str_upper(ak_str *s);
ak_str *ak_str_fold(ak_str *s);  // Unicode's full case folding
// Without case (full case folding, rt/unicode.h; no normalization):
int ak_str_cmp_fold(const ak_str *a, const ak_str *b);
bool ak_str_eq_fold(const ak_str *a, const ak_str *b);
bool ak_str_has_prefix_fold(const ak_str *s, const ak_str *p);
bool ak_str_has_suffix_fold(const ak_str *s, const ak_str *p);
bool ak_str_contains_fold(const ak_str *s, const ak_str *sub);
ak_str *ak_str_trim(ak_str *s);
ak_str *ak_str_replace(ak_str *s, const ak_str *old, const ak_str *new_);
ak_str *ak_str_replace_n(ak_str *s, const ak_str *old, const ak_str *new_, int64_t count);  // the first count (all: < 0)
struct ak_arr *ak_str_split(ak_str *s, const ak_str *sep);
// ak_str_split_n: at most count parts (all: < 0), the last the rest; an
// empty sep, the characters (count aside: lib.c's split cuts them).
struct ak_arr *ak_str_split_n(ak_str *s, const ak_str *sep, int64_t count);
bool ak_str_to_int(const ak_str *s, int64_t *out);
bool ak_str_to_float(const ak_str *s, double *out);

// A builder for interpolated strings: no intermediate strings. Zero is
// an empty one; what it holds is in small until it outgrows it, then in
// b, so a short string costs one allocation, the result's.
typedef struct ak_strb {
	ak_buf b;
	uint32_t n;      // bytes in small, while b.data is NULL
	char small[116];
} ak_strb;
void ak_strb_str(ak_strb *b, const ak_str *s);
void ak_strb_cstr(ak_strb *b, const char *s, size_t n);
void ak_strb_int(ak_strb *b, int64_t v);
void ak_strb_uint(ak_strb *b, uint64_t v);
void ak_strb_float(ak_strb *b, double v);
void ak_strb_bool(ak_strb *b, bool v);
void ak_strb_char(ak_strb *b, char c);
// ak_strb_room: room for n more bytes at the returned pointer, which
// ak_strb_wrote(b, k) then says k of were written.
char *ak_strb_room(ak_strb *b, size_t n);
static inline void ak_strb_wrote(ak_strb *b, size_t k) {
	if (b->b.data) b->b.len += k;
	else b->n += (uint32_t)k;
}
static inline size_t ak_strb_len(const ak_strb *b) { return b->b.data ? b->b.len : b->n; }
// ak_strb_buf: what b holds, all of it in b->b (moved there from small),
// for code that reads or takes the buffer itself; b goes on taking more.
ak_buf *ak_strb_buf(ak_strb *b);
// ak_str_of_buf is b's bytes as a string, b taken (left empty): a big
// buffer becomes the string in place, not copied.
ak_str *ak_str_of_buf(ak_buf *b);
// ak_strs_join is the n strings at v (none: empty) one after another:
// their lengths summed first, the result made at its size, each copied
// into it once.
ak_str *ak_strs_join(const ak_val *v, uint32_t n);

// Stack strings. A string compiled code makes (a + b + c, an
// interpolation) whose value the compiler proved goes nowhere but into
// what reads it in place (its length, a comparison, a search, another
// string, a print: lang/cgen's stack strings) is built in a buffer in
// the function's C frame when it fits, as Go's concatstrings writes a
// result that doesn't escape into the caller's 32-byte tmpBuf: no
// allocation, nothing freed. It's immortal (a release does nothing) and
// flagged AK_STR_STACK; one that doesn't fit is made on the heap, as
// any.
#define AK_STRBUF_ROOM 63
typedef union ak_strbuf {
	ak_str s;
	char b[sizeof(ak_str) + AK_STRBUF_ROOM + 1];
} ak_strbuf;
// ak_strbuf_str is buf as a string of n bytes (n <= AK_STRBUF_ROOM,
// its NUL written), the caller's to fill.
static inline ak_str *ak_strbuf_str(ak_strbuf *buf, size_t n) {
	ak_str *s = &buf->s;
	s->h = (ak_obj){.rc = AK_RC_IMMORTAL, .tag = AK_TAG_STR, .flags = AK_STR_STACK};
	s->len = (uint32_t)n;
	s->hash = 0;
	s->data[n] = 0;
	return s;
}
// Buffered output: std/os's Writer (os.writer(.stdout)). Each stream,
// stdout (1) and stderr (2), has one buffer, the main thread's, which
// every writer of it shares: put appends to it, and it's written out
// when it fills, on flush, before anything else is written to its
// stream (print, os.writeTo, a log line), when the main thread's loop
// goes idle, before work is handed to another thread or a command
// starts, and as the program ends (rt/lib.c). On another thread a put
// writes through, at once. ak_stdio_put's fast path, a copy, is inline
// (compiled code calls it directly: lang/cgen's intrinsics).
typedef struct ak_outbuf {
	char *data;
	size_t len, cap;
} ak_outbuf;
extern ak_outbuf ak_outbufs[3];  // [1] stdout's, [2] stderr's
// ak_on_main reports whether this is the main thread (the one the VM was
// last set up on: ak_mark_main_thread).
extern uint32_t ak_main_epoch;
extern AK_TLS uint32_t ak_thread_epoch;
static inline bool ak_on_main(void) { return ak_thread_epoch == ak_main_epoch; }
void ak_mark_main_thread(void);
void ak_stdio_put_slow(int fd, const void *p, size_t n);
static inline void ak_stdio_put(int64_t fd, const void *p, size_t n) {
	int f = fd == 2 ? 2 : 1;
	if (AK_LIKELY(ak_on_main())) {
		ak_outbuf *b = &ak_outbufs[f];
		if (AK_LIKELY(n <= b->cap - b->len)) {
			memcpy(b->data + b->len, p, n);
			b->len += n;
			return;
		}
	}
	ak_stdio_put_slow(f, p, n);
}
// ak_stdio_buffer makes stream fd's buffer hold at least size bytes
// (the largest any writer asked for); ak_stdio_flush writes out what it
// holds, ak_stdio_flush_all both streams'. The main thread's: elsewhere
// they do nothing.
void ak_stdio_buffer(int fd, int64_t size);
void ak_stdio_flush(int fd);
void ak_stdio_flush_all(void);

// ak_strs_join_in is ak_strs_join in buf when it fits there; ak_str_concat_in
// ak_str_concat's; ak_strb_finish_in ak_strb_finish's.
ak_str *ak_strs_join_in(ak_strbuf *buf, const ak_val *v, uint32_t n);
ak_str *ak_str_concat_in(ak_strbuf *buf, ak_str *a, ak_str *b);
// ak_strb_fmt writes v, of kind k (int, float; a string: text), by an
// interpolation's format ("{x:>8.2f}"'s spec, n bytes): as str.fmt_int,
// str.fmt_float and str.fmt_text do (rt/lib.c).
void ak_strb_fmt(ak_strb *b, ak_val v, ak_kind k, const char *spec, size_t n);
ak_str *ak_strb_finish(ak_strb *b);  // frees the builder's buffer
ak_str *ak_strb_finish_in(ak_strb *b, ak_strbuf *buf);
void ak_strb_free(ak_strb *b);       // drops what it holds

// ---- Bytes ----

typedef struct ak_bytes {
	ak_obj h;
	uint32_t len;
	uint8_t data[];
} ak_bytes;

ak_bytes *ak_bytes_new(const void *p, size_t n);  // p NULL: n zero bytes
ak_bytes *ak_bytes_raw(size_t n);                 // n bytes the caller writes whole

// ---- Arrays ----

// How an array stores its elements. An int, a float or a reference is
// a value of 8 bytes (ak_val, untagged: its type is the array's); a
// bool is a byte, and a sized integer (i8 to u32) its own width,
// packed as Go's []bool and []uint8 and Swift's [Bool] and [UInt8] are:
// a [bool] of 50 million takes 50 MB, not 400. Packed elements are
// scalars: a packed array holds no references. Code that reads or
// writes elements of any array goes through ak_arr_get and ak_arr_put;
// code that knows its array holds values takes them as ak_arr_vals.
typedef enum ak_elem {
	AK_E_VAL = 0,  // ak_val
	AK_E_BOOL,     // bool, a byte
	AK_E_I8,
	AK_E_I16,
	AK_E_I32,
	AK_E_U8,
	AK_E_U16,
	AK_E_U32,
} ak_elem;

// ak_int_elem is how an array stores a sized integer of type t (its code,
// ak_int_type): packed, but int's and u64's 64 bits, values.
static inline ak_elem ak_int_elem(int t) {
	static const uint8_t e[8] = {AK_E_I8, AK_E_I16, AK_E_I32, AK_E_VAL, AK_E_U8, AK_E_U16, AK_E_U32, AK_E_VAL};
	return (ak_elem)e[t & 7];
}
// AK_ARR_KIND is an array's elements' kind with how they're stored, as
// ak_arr_new takes it (NEWA's kind operand, the element kind map takes):
// the kind in the low 4 bits, an ak_elem above (0: the kind's own
// storage, a value's or a bool's byte).
#define AK_ARR_KIND(k, e) ((ak_kind)((unsigned)(k) | (unsigned)(e) << 4))

// ak_desc_arr_kind is k, an array's elements' kind read from their type
// descriptor d (json.h's), with how they're stored: "n" and a sized
// integer type's code, packed at its width.
static inline ak_kind ak_desc_arr_kind(const char *d, ak_kind k) {
	return d[0] == 'n' && d[1] >= '0' && d[1] <= '7' ? AK_ARR_KIND(k, ak_int_elem(d[1] - '0')) : k;
}

// ak_elem_shift is log2 of an element's bytes.
static inline uint8_t ak_elem_shift(ak_elem e) {
	static const uint8_t s[] = {3, 0, 0, 1, 2, 0, 1, 2};
	return s[e];
}

typedef struct ak_arr {
	ak_obj h;  // h.aux: the element kind; h.flags: 1 when elements are strings (for equality)
	// Its buffer: front free slots, then cap slots from items (its len
	// elements, then free ones). Changes at the start move items within
	// it, so an array is a double-ended queue: pushing and popping at
	// either end is O(1), amortized (arr.c).
	uint32_t len, cap, front;
	uint8_t elem;   // ak_elem: how its elements are stored
	uint8_t shift;  // log2 of an element's bytes (ak_elem_shift)
	const ak_shape *shape;  // the elements' shape when they're structs or enums
	void *items;
} ak_arr;

// ak_arr_new makes an empty array of elements of kind elem: bools
// packed, a byte each; others values, unless elem says how to store
// them (AK_ARR_KIND: a sized integer's width; ak_arr_new_packed).
ak_arr *ak_arr_new(ak_kind elem, const ak_shape *shape, uint32_t cap);
ak_arr *ak_arr_new_packed(ak_kind kind, ak_elem elem, uint32_t cap);
static inline ak_kind ak_arr_kind(const ak_arr *a) { return (ak_kind)a->h.aux; }
static inline bool ak_arr_packed(const ak_arr *a) { return a->elem != AK_E_VAL; }
// ak_arr_vals is the elements of an array of values (not packed).
static inline ak_val *ak_arr_vals(const ak_arr *a) {
#ifndef NDEBUG
	if (AK_UNLIKELY(a->elem != AK_E_VAL)) ak_panic("ak_arr_vals of a packed array (elements of type %d)", a->elem);
#endif
	return (ak_val *)a->items;
}
// ak_arr_bools is the elements of a [bool], a byte each: C's bool, not
// a character type, so that a write of one isn't taken to change
// anything else (C's aliasing: a char may be any object's byte), and
// compiled code keeps the array's length and buffer in registers
// across a loop of writes.
static inline bool *ak_arr_bools(const ak_arr *a) {
#ifndef NDEBUG
	if (AK_UNLIKELY(a->elem != AK_E_BOOL)) ak_panic("ak_arr_bools of an array of elements of type %d", a->elem);
#endif
	return (bool *)a->items;
}
// ak_elem_get reads an element of type e at p, as a value (a sized
// integer sign- or zero-extended); ak_elem_put writes v there,
// narrowed. Neither counts references.
static inline ak_val ak_elem_get(const void *p, ak_elem e) {
	switch (e) {
	case AK_E_VAL: return *(const ak_val *)p;
	case AK_E_BOOL: return (ak_val){.i = *(const bool *)p};
	case AK_E_U8: return (ak_val){.i = *(const uint8_t *)p};
	case AK_E_I8: return (ak_val){.i = *(const int8_t *)p};
	case AK_E_I16: return (ak_val){.i = *(const int16_t *)p};
	case AK_E_U16: return (ak_val){.i = *(const uint16_t *)p};
	case AK_E_I32: return (ak_val){.i = *(const int32_t *)p};
	case AK_E_U32: return (ak_val){.i = *(const uint32_t *)p};
	}
	return (ak_val){0};
}
static inline void ak_elem_put(void *p, ak_elem e, ak_val v) {
	switch (e) {
	case AK_E_VAL: *(ak_val *)p = v; break;
	case AK_E_BOOL: *(bool *)p = v.i != 0; break;
	case AK_E_I8:
	case AK_E_U8: *(uint8_t *)p = (uint8_t)v.i; break;
	case AK_E_I16:
	case AK_E_U16: *(uint16_t *)p = (uint16_t)v.i; break;
	case AK_E_I32:
	case AK_E_U32: *(uint32_t *)p = (uint32_t)v.i; break;
	}
}
// ak_arr_at is where element i is.
static inline void *ak_arr_at(const ak_arr *a, uint32_t i) { return (char *)a->items + ((size_t)i << a->shift); }
// ak_arr_get is element i (in range) as a value, borrowed; ak_arr_put
// stores v as element i, neither releasing what was there nor
// retaining v (ak_arr_set does both). A value's inline, a packed one's
// a call (ak_arr_get_packed, ak_arr_put_packed).
ak_val ak_arr_get_packed(const ak_arr *a, uint32_t i);
void ak_arr_put_packed(ak_arr *a, uint32_t i, ak_val v);
static inline ak_val ak_arr_get(const ak_arr *a, uint32_t i) {
	if (AK_LIKELY(a->elem == AK_E_VAL)) return ((const ak_val *)a->items)[i];
	return ak_arr_get_packed(a, i);
}
static inline void ak_arr_put(ak_arr *a, uint32_t i, ak_val v) {
	if (AK_LIKELY(a->elem == AK_E_VAL)) ((ak_val *)a->items)[i] = v;
	else ak_arr_put_packed(a, i, v);
}
// ak_arr_mut makes *a unique (copying it if it's shared) and returns it.
ak_arr *ak_arr_mut(ak_arr **a);
void ak_arr_push_slow(ak_arr **a, ak_val v);
// ak_arr_room reports whether a has room for one more at its end. Its
// cap is read on its own (a relaxed atomic load is a plain one), so
// that the C compiler doesn't read len and cap as one 8-byte word: in a
// loop of pushes that read would follow the store of len's 4 bytes, a
// store it can't take its bytes from (a load forwarded from a store
// must lie within it), and each push would wait for the one before it
// to reach the cache (a fused map's loop: 3× the cycles).
static inline bool ak_arr_room(const ak_arr *a) {
	return a->len < __atomic_load_n(&a->cap, __ATOMIC_RELAXED);
}
// ak_arr_push appends v, taking its reference: inline when the array is
// its holder's alone, has room, and holds values.
static inline void ak_arr_push(ak_arr **ap, ak_val v) {
	ak_arr *a = *ap;
	if (AK_LIKELY(ak_unique(a) && ak_arr_room(a) && a->elem == AK_E_VAL)) {
		((ak_val *)a->items)[a->len++] = v;
		return;
	}
	ak_arr_push_slow(ap, v);
}
// ak_arr_push_bool is ak_arr_push of a bool to a [bool] (packed: its
// byte), inline likewise.
static inline void ak_arr_push_bool(ak_arr **ap, bool v) {
	ak_arr *a = *ap;
	if (AK_LIKELY(ak_unique(a) && ak_arr_room(a) && a->elem == AK_E_BOOL)) {
		((bool *)a->items)[a->len++] = v;
		return;
	}
	ak_arr_push_slow(ap, (ak_val){.i = v});
}
void ak_arr_reserve(ak_arr **a, uint32_t n);         // room for n more, made unique
// ak_arr_reserve_big is ak_arr_reserve when the room is a big block
// (AK_ARR_BIG bytes or more: what of it is never written isn't
// resident), else nothing: a filter's room, the most it can keep, taken
// once rather than grown, copied, as it fills.
#define AK_ARR_BIG ((size_t)1 << 20)
void ak_arr_reserve_big(ak_arr **a, uint32_t n);
void ak_arr_insert(ak_arr **a, int64_t i, ak_val v); // takes v's reference
ak_val ak_arr_remove_at(ak_arr **a, int64_t i);      // returns the reference
void ak_arr_set(ak_arr **a, int64_t i, ak_val v);    // takes v's reference
// ak_arr_base is what's allocated: the front free slots' start.
static inline void *ak_arr_base(const ak_arr *a) { return a->items ? (char *)a->items - ((size_t)a->front << a->shift) : NULL; }
bool ak_arr_check(const ak_arr *a, int64_t i);       // in range
ak_arr *ak_arr_slice(ak_arr *a, int64_t from, int64_t to);
ak_arr *ak_arr_concat(ak_arr *a, ak_arr *b);
ak_arr *ak_arr_copy(const ak_arr *a);
// ak_arr_like is an empty array of a's elements (their kind, storage
// and shape) with room for cap.
ak_arr *ak_arr_like(const ak_arr *a, uint32_t cap);

// ---- Maps: insertion ordered, keys int or string ----

#define AK_MAP_STRKEYS 1  // h.flags: the keys are strings
#define AK_MAP_SET 2      // h.flags: a set, its values void (ak_set_*)

typedef struct ak_map {
	ak_obj h;  // h.aux: key kind | value kind << 4; h.flags: AK_MAP_STRKEYS, AK_MAP_SET
	uint32_t len, cap;   // entries used, entries allocated (includes deleted)
	uint32_t live;       // entries not deleted
	uint32_t nslots;     // index slots (a power of 2)
	ak_val *keys, *vals;
	uint8_t *dead;
	uint64_t *slots;     // map.c's index: the key's hash << 32 | entry + 1 (0 empty)
	const ak_shape *vshape;
} ak_map;

ak_map *ak_map_new(ak_kind key, bool string_keys, ak_kind val, const ak_shape *vshape);
ak_map *ak_map_mut(ak_map **m);
bool ak_map_get(const ak_map *m, ak_val key, ak_val *out);  // borrowed
void ak_map_set(ak_map **m, ak_val key, ak_val v);          // takes both references
// ak_map_put_new sets key's value unless it has one (then releasing key
// and v, the map as it was): whether it was new. One probe.
bool ak_map_put_new(ak_map **m, ak_val key, ak_val v);
bool ak_map_delete(ak_map **m, ak_val key);
ak_arr *ak_map_keys(const ak_map *m);
ak_arr *ak_map_values(const ak_map *m);

// ---- Sets: a map whose values are void, insertion ordered ----
//
// A Set<T> is an ak_map of T's keys (ints, or strings) to nothing,
// flagged AK_MAP_SET: it's copied on write, compared, hashed, frozen and
// copied across threads as a map is, and iterates in insertion order
// (an element removed and inserted again goes last).

ak_map *ak_set_new(ak_kind elem, bool string_elems);
static inline bool ak_map_is_set(const ak_map *m) { return (m->h.flags & AK_MAP_SET) != 0; }
bool ak_set_has(const ak_map *s, ak_val x);      // x borrowed
bool ak_set_insert(ak_map **s, ak_val x);        // takes x's reference; whether x was new
bool ak_set_remove(ak_map **s, ak_val x);        // x borrowed; whether it was there
ak_map *ak_set_union(const ak_map *a, const ak_map *b);         // a's elements, then b's new ones
ak_map *ak_set_intersection(const ak_map *a, const ak_map *b);  // a's that b has, in a's order
ak_map *ak_set_subtracting(const ak_map *a, const ak_map *b);   // a's that b hasn't
bool ak_set_is_subset(const ak_map *a, const ak_map *b);        // every element of a is b's
ak_map *ak_set_from_arr(const ak_arr *a);  // its distinct elements, first occurrence first

// ---- B-trees: std/collections' SortedMap and SortedSet (tree.c) ----
//
// A tree (ak_tree, AK_TAG_TREE) holds its entries' count, how its keys
// are ordered, and its root, a node (ak_tnode, AK_TAG_TNODE): a value
// whose keys, values (none in a set's) and children are in it, one
// allocation, as Rust's BTreeMap's are (Go's google/btree's node is two
// slices more). Nodes are counted, and shared by the trees copied from
// one another until one changes: a change copies the nodes on its path
// from the root that are shared (path copying, as Swift's collections
// copy on write), so a copy of a tree is O(1) and a change O(log n).
//
// Keys are ordered by the tree's mode: an int's, a u64's, a float's or a
// string's own < in C, or a type's own compare (its shape's
// AK_HOOK_CMP); a SortedMap's by, a closure, is passed to each call that
// orders (none: the mode's), as the tree doesn't keep it.
//
// A node's slots are its keys, its values and its children, each group
// AK_TREE_MAX long (children one more), scalars' groups first: what
// holds references is one run of slots, s[from, to), whose unused slots
// are NULL, so a walk (obj.c's destroying, freezing, copying) goes
// through a node as through an array of references.
#define AK_TREE_MAX 63  // a node's keys at most
#define AK_TREE_MIN 31  // and at least, but the root's
#define AK_TREE_DEPTH 24  // levels at most (31^12 entries is past any memory)

typedef enum ak_tree_mode {
	AK_TM_INT,    // int64's < (int, Duration, char, i8 … u32, a payloadless enum's case)
	AK_TM_U64,    // uint64's
	AK_TM_FLOAT,  // double's
	AK_TM_STR,    // strings', bytewise (ak_str_cmp)
	AK_TM_CMP,    // the keys' type's own compare (their shape's AK_HOOK_CMP; an enum's without one, its cases' order)
} ak_tree_mode;

#define AK_TNODE_LEAF 1  // h.flags: it has no children

typedef struct ak_tnode {
	ak_obj h;                 // h.aux: its keys' count; h.flags: AK_TNODE_LEAF
	uint16_t ko, vo, co;      // where its keys, values and children start in s
	uint16_t from, to;        // its references: s[from, to)
	ak_val s[];
} ak_tnode;

typedef struct ak_tree {
	ak_obj h;
	uint8_t mode;     // ak_tree_mode
	uint8_t kk, vk;   // its keys' and values' kinds; vk AK_K_VOID: a set's
	uint8_t height;   // the levels below its root
	int64_t count;
	ak_val root;      // an ak_tnode, NULL when it's empty
	// kshape: AK_TM_CMP's keys' shape when they're ints (an enum without
	// payloads that has a compare); a struct's or an enum's is its own.
	const ak_shape *kshape;
} ak_tree;

static inline uint32_t ak_tnode_len(const ak_tnode *n) { return n->h.aux; }
static inline bool ak_tnode_leaf(const ak_tnode *n) { return n->h.flags & AK_TNODE_LEAF; }
static inline ak_val *ak_tnode_keys(const ak_tnode *n) { return (ak_val *)n->s + n->ko; }
static inline ak_val *ak_tnode_vals(const ak_tnode *n) { return (ak_val *)n->s + n->vo; }
static inline ak_tnode *ak_tnode_kid(const ak_tnode *n, uint32_t i) { return n->s[n->co + i].p; }
static inline bool ak_tree_is_set(const ak_tree *t) { return t->vk == AK_K_VOID; }
// ak_tree_desc_mode is how keys of the type of JSON's descriptor d
// (json.h's) are ordered; ak_tree_desc_kind, a value's kind by it.
ak_tree_mode ak_tree_desc_mode(const char *d);
ak_kind ak_tree_desc_kind(const char *d);
ak_tree *ak_tree_new(ak_tree_mode mode, ak_kind kk, ak_kind vk);  // empty
// ak_tnode_shell is a copy of n but for what it references (NULL, which
// obj.c's walk fills in): for another thread (ak_deep_copy).
ak_tnode *ak_tnode_shell(const ak_tnode *n);

// What orders a tree's keys in a call: by, a less-than closure, when
// it's given, else the tree's mode.
typedef struct ak_tree_order {
	uint8_t mode;
	struct ak_closure *by;
	const ak_shape *kshape;  // the tree's
} ak_tree_order;

// ak_tree_mut makes *t unique (a copy sharing its nodes, when it's
// shared) and returns it.
ak_tree *ak_tree_mut(ak_tree **t);

// Finding and changing (keys and values borrowed; what's stored is
// retained). A by that raises an error leaves the tree whole (as it was,
// or with nodes merged or split on the way), the error pending. What
// changes a tree takes a unique one (ak_tree_mut's), and copies the
// nodes it changes that are shared.
// ak_tree_find is where key is: its node and index (false: it isn't).
bool ak_tree_find(const ak_tree *t, ak_tree_order o, ak_val key, const ak_tnode **at, uint32_t *i);
// ak_tree_put gives key value v (a set's: none): whether it's new.
bool ak_tree_put(ak_tree *t, ak_tree_order o, ak_val key, ak_val v);
// ak_tree_delete removes key: whether it was there; its value (its
// reference) in *out, unless out is NULL.
bool ak_tree_delete(ak_tree *t, ak_tree_order o, ak_val key, ak_val *out);
// ak_tree_near is the entry nearest key: below, the greatest less than
// key (or equal, with equal); else the least greater (or equal). false:
// none.
bool ak_tree_near(const ak_tree *t, ak_tree_order o, ak_val key, bool below, bool equal, const ak_tnode **at, uint32_t *i);
// ak_tree_end is the first entry (last: the last).
bool ak_tree_end(const ak_tree *t, bool last, const ak_tnode **at, uint32_t *i);
// ak_tree_append puts key and v (references taken) after every entry:
// a tree built in its order (a kept value read back), no comparison.
void ak_tree_append(ak_tree *t, ak_val key, ak_val v);

// An in-order walk: from the first entry, or from the first at or after
// a key (after: past it), its entries one by one (borrowed). A tree a
// walk is over doesn't change while it's walked.
typedef struct ak_titer {
	const ak_tnode *n[AK_TREE_DEPTH];
	uint32_t i[AK_TREE_DEPTH];
	int top;
} ak_titer;
void ak_titer_start(ak_titer *it, const ak_tree *t);
void ak_titer_seek(ak_titer *it, const ak_tree *t, ak_tree_order o, ak_val from, bool after);
bool ak_titer_next(ak_titer *it, const ak_tnode **at, uint32_t *i);

// ak_tree_eq compares two trees' entries in order (same count, keys and
// values ak_val_eq); ak_tree_hash hashes them as it compares.
bool ak_tree_eq(const ak_tree *a, const ak_tree *b);
uint64_t ak_tree_hash(const ak_tree *t);

// ---- Structs, tuples and enums ----

typedef struct ak_struct {
	ak_obj h;
	const ak_shape *shape;
	ak_val f[];
} ak_struct;

ak_struct *ak_struct_new(const ak_shape *s);  // fields zero; the caller fills them
ak_struct *ak_struct_mut(ak_struct **s);

typedef struct ak_enum {
	ak_obj h;  // h.aux: the case
	const ak_shape *shape;
	ak_val f[];
} ak_enum;

ak_enum *ak_enum_new(const ak_shape *s, uint16_t c);  // a payloadless case: a shared immortal
static inline uint16_t ak_enum_case(const ak_enum *e) { return e->h.aux; }

typedef struct ak_box {
	ak_obj h;  // h.aux: the kind
	ak_val v;
} ak_box;

ak_box *ak_box_new(ak_kind k, ak_val v);

// ---- Closures ----

typedef struct ak_closure ak_closure;

// A closure's code: the VM's functions and compiled C functions alike.
// It is called with its arguments (borrowed) and returns its result
// (owned). An error is raised with ak_throw; the caller checks
// ak_error_pending().
typedef ak_val (*ak_invoke_fn)(ak_closure *c, ak_val *args, uint32_t nargs);

struct ak_closure {
	ak_obj h;
	ak_invoke_fn invoke;
	void (*drop)(ak_closure *c);  // after its captures are released; may be NULL
	const void *fn;               // the VM's function, or C code's data
	const uint8_t *capkinds;
	uint16_t ncap;
	// sig: the VM's function's ak_func.sig when it's compiled to C, its
	// direct body called with its arguments in registers (lang/cgen's
	// closure calls test it, and take the body from fn); 0 otherwise.
	// Copied as the closure is made (ak_vm_closure_sig), so a call tests
	// one word of the closure's, not its invoke and then its function's.
	uint32_t sig;
	ak_val caps[];
};

ak_closure *ak_closure_new(ak_invoke_fn invoke, const void *fn, uint16_t ncap, const uint8_t *capkinds);
static inline ak_val ak_call(ak_closure *c, ak_val *args, uint32_t nargs) { return c->invoke(c, args, nargs); }

// Objects with their own destructor (runtime objects: cells, tasks,
// views).
typedef struct ak_opaque {
	ak_obj h;
	void (*drop)(struct ak_opaque *o);
} ak_opaque;

void *ak_opaque_new(ak_tag tag, size_t size, void (*drop)(ak_opaque *));

// ---- Generic operations, by kind ----
//
// Each walks a value however deep it nests (a list as an enum, a million
// links long) in bounded C stack: a spawn's thread has 512 KB. One
// nested less deep than a walk's frames on the C stack takes no
// allocation (obj.c); releasing never allocates.

bool ak_val_eq(ak_val a, ak_val b, ak_kind k);
// ak_val_hash hashes as ak_val_eq compares: equal values hash the same.
uint64_t ak_val_hash(ak_val v, ak_kind k);
bool ak_obj_eq(const ak_obj *a, const ak_obj *b);
// ak_deep_copy copies a value for another thread: every object it holds
// is new, with no references shared. Closures, cells and other runtime
// objects can't be copied (the checker keeps them out of spawn).
ak_val ak_deep_copy(ak_val v, ak_kind k);
// ak_freeze makes v and everything it holds immortal: a constant, shared
// by every thread without reference counting. Each object it makes
// immortal goes into made (when not NULL), whose owner (the module whose
// init froze it) frees them with ak_frozen_free once nothing can reach
// them; without one, they're never freed.
typedef AK_VEC(ak_obj *) ak_frozen;
void ak_freeze(ak_val v, ak_kind k, ak_frozen *made);
void ak_frozen_free(ak_frozen *made);
// A constant made on first use, which threads (spawn's workers) may ask
// for at once: ak_constant_get is what *slot holds (NULL: not made yet);
// ak_constant_put freezes v, just made, and puts it in *slot unless
// another thread put one there first (v is then freed), and returns the
// one *slot holds, whole on every thread.
static inline void *ak_constant_get(void *const *slot) { return __atomic_load_n(slot, __ATOMIC_ACQUIRE); }
void *ak_constant_put(void **slot, void *v);

// ---- What a builtin changed in place ----
//
// The builtins that change an array, a map or a set in place (push,
// insert, removeAt, removeWhere, pop, clear, sort, a map's set and
// remove, a set's insert and remove) record what they changed, for the
// write that puts it back in its state (vm/path.h's ak_path_put): it
// notifies the readers of what changed, not of the rest. A key is
// recorded as its hash (ak_val_hash): a reader of another key with the
// same hash runs too, which is harmless. Each thread has its own; the
// put that reads it clears it.

enum {
	AK_CHANGE_UNKNOWN = 0,  // nothing recorded: anything may have changed
	AK_CHANGE_NONE,         // nothing changed
	AK_CHANGE_RANGE,        // an array's elements from lo to hi (hi -1: to the end)
	AK_CHANGE_KEY,          // a map's value at a key, or a set's member
};

typedef struct ak_change {
	uint8_t what;
	bool len;  // the length changed
	int64_t lo, hi;
	uint64_t hash;  // the key's
} ak_change;

extern AK_TLS ak_change ak_last_change;

static inline void ak_change_none(void) { ak_last_change = (ak_change){.what = AK_CHANGE_NONE}; }
static inline void ak_change_range(int64_t lo, int64_t hi, bool len) {
	ak_last_change = (ak_change){.what = AK_CHANGE_RANGE, .len = len, .lo = lo, .hi = hi};
}
void ak_change_key(ak_val key, ak_kind kk, bool len);

// ---- Errors ----
//
// Throwing sets the pending error and returns normally; every caller of
// something that throws checks ak_error_pending() and returns at once,
// so the error unwinds to a catch. Cancellation is an error too, one no
// boundary reports.

void ak_throw(void *err);  // takes the reference
void ak_throw_msg(const char *fmt, ...) AK_PRINTF(1, 2);
void ak_throw_cancel(void);
// The thread's pending error (NULL: none): read inline, as every call
// that may throw is followed by the test.
extern AK_TLS void *ak_pending_error;
static inline bool ak_error_pending(void) { return ak_pending_error != NULL; }
// ak_error_slot is where this thread's pending error is: *slot is
// ak_error_pending's, read without a call or a TLS lookup (the VM's
// dispatch loop takes it once). It's valid as long as the thread is.
void *const *ak_error_slot(void);
void *ak_error_take(void);  // the pending error, cleared; the caller owns it
bool ak_is_cancel(const void *err);
// ak_throw_hook, when set, sees each error as it's thrown (a program
// notes where: abi/program.c). Any thread.
extern void (*ak_throw_hook)(void *err);
// ak_frame_hook, when set, hears a function compiled to C (lang/cgen)
// left by an error, from line: the frames a program's trace has that the
// VM's stack doesn't. The error's path only.
extern void (*ak_frame_hook)(const char *fn, const char *file, uint32_t line);
// ak_retrace_hook, when set, hears that the error pending was thrown by
// the instruction the VM's innermost frame now says (its pc saved after
// the throw: an overflow's, an index's): a trace taken as it was thrown
// is taken again.
extern void (*ak_retrace_hook)(void);
ak_str *ak_error_message(const void *err);  // borrowed

extern const ak_shape ak_shape_error;  // the built-in Error(message: string)

#endif
