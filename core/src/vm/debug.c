// debug.c: the VM's debugger (debug.h).
#include "vm/debug.h"
#include "vm/vm_int.h"
#include "base/thread.h"
#include "signal/signal.h"
#include "rt/unicode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if AK_VM_DEBUGGER

_Atomic int ak_dbg_on;

// ---- What other threads set ----
//
// Under mu: the breakpoints, the sink, whether the main thread is paused
// and the command it's handed, and the requests it answers while paused.
// The interpreter reads only atomics as it runs: ak_dbg_on, and in the
// hook bp_count, bp_gen, pause_req, step_on and held.

static ak_mutex mu = AK_MUTEX_INIT;
static ak_cond cv;  // the paused main thread waits on it
static bool cv_ready;
static ak_dbg_sink sink;

typedef struct {
	char *file;
	size_t flen;
	uint32_t *lines;
	uint32_t n;
} bpfile;
static bpfile *bps;
static uint32_t nbps;
static _Atomic uint32_t bp_count;  // the breakpoints set, every file's
static _Atomic uint32_t bp_gen = 1;  // changes with them: functions resolve them again
static _Atomic int pause_req, step_on, held;

static bool paused;
static int resume;  // the command the paused main thread goes on with (0: none yet)

typedef struct vreq {
	struct vreq *next;
	uint32_t seq, frame, start, count, npath;
	uint32_t path[];
} vreq;
static vreq *vq, *vq_tail;

static void lock(void) {
	ak_mutex_lock(&mu);
	if (!cv_ready) {
		ak_cond_init(&cv);
		cv_ready = true;
	}
}

static void unlock(void) { ak_mutex_unlock(&mu); }

// update_on sets ak_dbg_on from what wants the hook (mu held).
static void update_on(void) {
	int on = atomic_load(&bp_count) > 0 || atomic_load(&pause_req) || atomic_load(&step_on);
	atomic_store(&ak_dbg_on, on);
}

// ---- The main thread's ----

// The step running (AK_DBG_NEXT, STEP_IN, STEP_OUT; 0: none) and where
// it started: the coroutine, how deep its frames went, the frame's base
// and function, its line, and the last pc it ran in that frame.
static int step;
static ak_co *step_co;
static const ak_func *step_fn;
static uint32_t step_depth, step_base, step_line, step_pc;

// The last stop, which a breakpoint further along the same line of the
// same frame doesn't stop at again (a line's code in two runs of the
// line table: a call's arguments on the lines below it).
static struct {
	ak_co *co;
	const ak_func *fn;
	uint32_t depth, base, line, pc;
	bool valid;
} last;

// Where the paused main thread is: the coroutine, and the pc of its top
// frame's instruction about to run.
static ak_co *stop_co;
static uint32_t stop_pc;

struct ak_dbg_fn {
	uint32_t gen;  // bp_gen when resolved
	bool any;
	uint64_t bits[];  // per word of code: a breakpoint's pc
};

void ak_vm_dbg_forget(ak_func *f) {
	ak_free(f->dbg);
	f->dbg = NULL;
}

static uint32_t line_at(const ak_func *f, uint32_t pc) {
	uint32_t lo = 0, hi = f->nlines;  // the first entry past pc
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (f->lines[mid].pc <= pc) lo = mid + 1;
		else hi = mid;
	}
	return lo ? f->lines[lo - 1].line : 0;
}

static bool line_starts(const ak_func *f, uint32_t pc) {
	uint32_t lo = 0, hi = f->nlines;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (f->lines[mid].pc < pc) lo = mid + 1;
		else hi = mid;
	}
	return lo < f->nlines && f->lines[lo].pc == pc;
}

// code_file is the file of f's code at pc: the function's inlined there,
// or f's.
static const ak_str *code_file(const ak_func *f, uint32_t pc) {
	for (uint32_t i = 0; i < f->ninlines; i++)
		if (pc >= f->inlines[i].start && pc < f->inlines[i].end) return f->mod->funcs[f->inlines[i].func].file;
	return f->file;
}

// resolve marks the pcs of f's breakpoints: each line-table entry of a
// line that has one, in its code's file.
static struct ak_dbg_fn *resolve(ak_func *f) {
	size_t words = (f->ncode + 63) / 64;
	struct ak_dbg_fn *d = f->dbg;
	if (!d) d = f->dbg = ak_alloc(sizeof *d + sizeof(uint64_t) * (words ? words : 1));
	memset(d->bits, 0, sizeof(uint64_t) * (words ? words : 1));
	d->any = false;
	lock();
	d->gen = atomic_load(&bp_gen);
	for (uint32_t b = 0; b < nbps; b++) {
		const bpfile *bf = &bps[b];
		for (uint32_t e = 0; e < f->nlines; e++) {
			const ak_line *ln = &f->lines[e];
			const ak_str *file = code_file(f, ln->pc);
			if (file->len != bf->flen || memcmp(file->data, bf->file, bf->flen) != 0) continue;
			for (uint32_t k = 0; k < bf->n; k++) {
				if (bf->lines[k] != ln->line || ln->pc >= f->ncode) continue;
				d->bits[ln->pc >> 6] |= (uint64_t)1 << (ln->pc & 63);
				d->any = true;
			}
		}
	}
	unlock();
	return d;
}

static void stop(ak_co *co, uint32_t pc, const char *reason);

// again reports whether a breakpoint at pc is further along the line the
// frame last stopped at.
static bool again(ak_co *co, const ak_frame *fr, uint32_t pc) {
	return last.valid && last.co == co && last.depth == co->nframes && last.base == fr->base && last.fn == fr->fn && pc > last.pc &&
	       last.line == line_at(fr->fn, pc);
}

// step_stops reports whether the step running stops before pc.
static bool step_stops(ak_co *co, const ak_frame *fr, uint32_t pc) {
	const ak_func *f = fr->fn;
	uint32_t depth = co->nframes;
	bool same = co == step_co && depth == step_depth && fr->base == step_base && f == step_fn;
	if (same) {
		if (step == AK_DBG_STEP_OUT) return false;
		uint32_t line = line_at(f, pc);
		// Another line; or the same again, from its start (a loop's).
		bool stops = line && (line != step_line || (pc <= step_pc && line_starts(f, pc)));
		step_pc = pc;
		return stops;
	}
	if (!line_at(f, pc)) return false;
	if (step == AK_DBG_STEP_IN) return true;  // whatever runs next
	// Over and out: on the same coroutine, back in a caller, or past the
	// frame (it returned to C, which ran on and called other code at its
	// depth). Not in what it calls, nor in other coroutines' code (a
	// task's await lets others run).
	return co == step_co && depth <= step_depth;
}

void ak_dbg_hook(ak_co *co, uint32_t pc) {
	if (!ak_signal_on_main_thread() || atomic_load_explicit(&held, memory_order_relaxed)) return;
	ak_frame *fr = &co->frames[co->nframes - 1];
	if (atomic_load_explicit(&pause_req, memory_order_relaxed)) {
		stop(co, pc, "pause");
		return;
	}
	if (atomic_load_explicit(&bp_count, memory_order_relaxed)) {
		ak_func *f = (ak_func *)fr->fn;
		struct ak_dbg_fn *d = f->dbg;
		if (!d || d->gen != atomic_load_explicit(&bp_gen, memory_order_acquire)) d = resolve(f);
		if (d->any && (d->bits[pc >> 6] >> (pc & 63) & 1) && !again(co, fr, pc)) {
			stop(co, pc, "breakpoint");
			return;
		}
	}
	if (step && !atomic_load_explicit(&step_on, memory_order_relaxed)) step = 0;  // reset
	if (step && step_stops(co, fr, pc)) stop(co, pc, "step");
}

// ---- Values, as the language writes them ----

// The most a value's text is (bytes), and its parts' nesting: past them,
// "…".
#define SHOW_MAX 200
#define SHOW_DEPTH 3
#define SHOW_ITEMS 10   // an array's, a map's, a set's items shown
#define SHOW_STR 120    // a string's bytes shown
#define MAX_FRAMES 128  // a stop's frames
#define MAX_CHILDREN 1000

typedef struct {
	ak_buf *b;
	size_t limit;
	ak_module *mod;  // the frame's: its shapes' field descriptors, its enums' cases
} shower;

static void put(shower *s, const char *p) { ak_buf_put(s->b, p, strlen(p)); }
static bool full(const shower *s) { return s->b->len >= s->limit; }

static ak_kind desc_kind(const char *d) {
	switch (*d) {
	case 'i':
	case 'e':
	case 'd':
	case 'c':
	case 'r':
	case 'n':
		return AK_K_INT;
	case 'f':
		return AK_K_FLOAT;
	case 'b':
		return AK_K_BOOL;
	default:
		return AK_K_REF;
	}
}

// dend is past the descriptor at d.
static const char *dend(const char *d) {
	for (;;) {
		switch (*d) {
		case '?':
		case '[':
		case '{':
		case '<':
			d++;
			continue;
		case 'S':
		case 'E':
		case 'J':
		case 'e': {
			const char *q = strchr(d, ';');
			return q ? q + 1 : d + strlen(d);
		}
		case 'n':
			return d[1] ? d + 2 : d + 1;
		case 'm':
			return dend(dend(d + 1));
		case '\0':
			return d;
		default:
			return d + 1;
		}
	}
}

// type_name writes a type's name without its packages' paths
// ("./model.Todo" is Todo, "Box<std/time.Date>" Box<Date>), n bytes of it.
static void type_name(ak_buf *b, const char *name, size_t n) {
	const char *seg = name, *end = name + n;
	for (const char *p = name;; p++) {
		char c = p < end ? *p : 0;
		if (c == '<' || c == '>' || c == ',' || c == '(' || c == ')' || c == '?' || c == '[' || c == ']' || c == ' ' || c == ':' || c == 0) {
			const char *dot = NULL;
			for (const char *q = seg; q < p; q++)
				if (*q == '.') dot = q;
			const char *from = dot ? dot + 1 : seg;
			ak_buf_put(b, from, (size_t)(p - from));
			if (!c) return;
			ak_buf_put(b, p, 1);
			seg = p + 1;
		}
	}
}

// own_shape reports whether sh is one of the module's (whose fields'
// names carry their descriptors: load.c's rname).
static bool own_shape(const ak_module *m, const ak_shape *sh) {
	return m && m->nshapes && (const void *)sh >= (const void *)&m->shapes[0] && (const void *)sh < (const void *)&m->shapes[m->nshapes];
}

static const char *name_desc(const shower *s, const ak_shape *sh, const char *name) {
	if (!name || !own_shape(s->mod, sh)) return "-";
	const char *d = name + strlen(name) + 1;
	return *d ? d : "-";
}

static void peel(ak_val *v, ak_kind *k, const char **d);

// show_type writes the type the descriptor d says (true), or nothing
// (false: it says none, or a tuple, whose value says).
static bool show_type(ak_buf *b, const char *d) {
	static const char *const ints[8] = {"i8", "i16", "i32", "int", "u8", "u16", "u32", "u64"};
	const char *simple = NULL;
	switch (*d) {
	case 'i': simple = "int"; break;
	case 'f': simple = "float"; break;
	case 'b': simple = "bool"; break;
	case 's': simple = "string"; break;
	case 'y': simple = "bytes"; break;
	case 'r': simple = "char"; break;
	case 'd': simple = "duration"; break;
	case 'c': simple = "color"; break;
	case 'n': simple = d[1] >= '0' && d[1] <= '7' ? ints[d[1] - '0'] : "int"; break;
	case '?':
		if (!show_type(b, d + 1)) return false;
		ak_buf_u8(b, '?');
		return true;
	case '[':
		ak_buf_u8(b, '[');
		if (!show_type(b, d + 1)) ak_buf_put(b, "_", 1);
		ak_buf_u8(b, ']');
		return true;
	case '{':
		ak_buf_put(b, "[string: ", 9);
		if (!show_type(b, d + 1)) ak_buf_put(b, "_", 1);
		ak_buf_u8(b, ']');
		return true;
	case '<':
		ak_buf_put(b, "Set<", 4);
		if (!show_type(b, d + 1)) ak_buf_put(b, "_", 1);
		ak_buf_u8(b, '>');
		return true;
	case 'm':
		ak_buf_u8(b, '[');
		if (!show_type(b, d + 1)) ak_buf_put(b, "_", 1);
		ak_buf_put(b, ": ", 2);
		if (!show_type(b, dend(d + 1))) ak_buf_put(b, "_", 1);
		ak_buf_u8(b, ']');
		return true;
	case 'S':
	case 'E':
	case 'J':
	case 'e': {
		const char *q = strchr(d, ';');
		type_name(b, d + 1, q ? (size_t)(q - d - 1) : strlen(d + 1));
		return true;
	}
	default:
		return false;
	}
	ak_buf_put(b, simple, strlen(simple));
	return true;
}

// value_type writes v's type: its descriptor's, else what the value says.
static void value_type(ak_buf *b, ak_val v, ak_kind k, const char *d) {
	if (show_type(b, d)) return;
	peel(&v, &k, &d);
	switch (k) {
	case AK_K_INT: ak_buf_put(b, "int", 3); return;
	case AK_K_FLOAT: ak_buf_put(b, "float", 5); return;
	case AK_K_BOOL: ak_buf_put(b, "bool", 4); return;
	case AK_K_VOID: ak_buf_put(b, "void", 4); return;
	default: break;
	}
	const ak_obj *o = v.p;
	const char *t = "none";
	if (o) {
		switch (o->tag) {
		case AK_TAG_STR: t = "string"; break;
		case AK_TAG_BYTES: t = "bytes"; break;
		case AK_TAG_ARR: t = "array"; break;
		case AK_TAG_MAP: t = ak_map_is_set((const ak_map *)o) ? "Set" : "map"; break;
		case AK_TAG_STRUCT: {
			const char *n = ((const ak_struct *)o)->shape->name;
			type_name(b, n, strlen(n));
			return;
		}
		case AK_TAG_ENUM: {
			const char *n = ((const ak_enum *)o)->shape->name;
			type_name(b, n, strlen(n));
			return;
		}
		case AK_TAG_CLOSURE: t = "fn"; break;
		case AK_TAG_TASK: t = "task"; break;
		case AK_TAG_VIEW: t = "view"; break;
		case AK_TAG_TREE: t = ((const ak_tree *)o)->vk == AK_K_VOID ? "SortedSet" : "SortedMap"; break;
		case AK_TAG_STORE: t = "store"; break;
		case AK_TAG_CELL: t = "state"; break;
		case AK_TAG_MEMO: t = "let"; break;
		case AK_TAG_ACC: t = "accessor"; break;
		default: t = "object"; break;
		}
	}
	ak_buf_put(b, t, strlen(t));
}

// peel reads through what holds a value: a state's cell, a let's memo,
// an accessor (what they hold now, if they can say without running
// code), and an optional that isn't none (its value, its descriptor's).
static void peel(ak_val *v, ak_kind *k, const char **d) {
	for (int guard = 0; guard < 16; guard++) {
		if (**d == '?' && *k != AK_K_REF) {
			(*d)++;
			continue;
		}
		if (*k != AK_K_REF || !v->p) return;
		const ak_obj *o = v->p;
		if (o->tag == AK_TAG_CELL || o->tag == AK_TAG_MEMO || o->tag == AK_TAG_ACC) {
			ak_val pv;
			ak_kind pk;
			if (!ak_vm_dbg_peek(v->p, &pv, &pk)) return;
			*v = pv;
			*k = pk;
			continue;
		}
		if (**d == '?') {
			(*d)++;
			if (desc_kind(*d) != AK_K_REF && o->tag == AK_TAG_BOX) {
				*k = (ak_kind)o->aux;
				*v = ((const ak_box *)o)->v;
			}
			continue;
		}
		if (o->tag == AK_TAG_BOX && desc_kind(*d) == AK_K_REF && **d != 'J') {
			// An optional scalar whose type the descriptor doesn't say.
			*k = (ak_kind)o->aux;
			*v = ((const ak_box *)o)->v;
			continue;
		}
		return;
	}
}

static void show(shower *s, ak_val v, ak_kind k, const char *d, int depth);

static void show_float(shower *s, double f) {
	if (f != f) {
		put(s, "nan");
		return;
	}
	if (isinf(f)) {
		put(s, f > 0 ? "inf" : "-inf");
		return;
	}
	ak_strb t = {0};
	ak_strb_float(&t, f);
	ak_buf *tb = ak_strb_buf(&t);
	ak_buf_put(s->b, tb->data, tb->len);
	if (!memchr(tb->data, '.', tb->len) && !memchr(tb->data, 'e', tb->len)) put(s, ".0");
	ak_strb_free(&t);
}

static void show_duration(shower *s, int64_t ns) {
	static const struct {
		int64_t n;
		const char *unit;
	} units[] = {{3600000000000LL, "h"}, {60000000000LL, "min"}, {1000000000LL, "s"}, {1000000LL, "ms"}, {1000LL, "us"}};
	if (ns == 0) {
		put(s, "0s");
		return;
	}
	for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
		if (ns % units[i].n == 0) {
			ak_buf_printf(s->b, "%lld%s", (long long)(ns / units[i].n), units[i].unit);
			return;
		}
	}
	ak_buf_printf(s->b, "%lldns", (long long)ns);
}

static void show_int(shower *s, int64_t i, const char *d) {
	switch (*d) {
	case 'd':
		show_duration(s, i);
		return;
	case 'c': {
		ak_builtin_fn hex = ak_builtin_find("color.hex");
		if (hex) {
			ak_val arg = {.i = i}, r = hex(&arg, 1);
			if (ak_error_pending()) ak_release(ak_error_take());
			if (r.p) {
				ak_buf_put(s->b, ((ak_str *)r.p)->data, ((ak_str *)r.p)->len);
				ak_release(r.p);
				return;
			}
		}
		break;
	}
	case 'r': {
		char u[4];
		size_t n = i >= 0 && i <= 0x10FFFF ? ak_utf8_encode((uint32_t)i, u) : 0;
		ak_buf_u8(s->b, '\'');
		if (i == '\n') put(s, "\\n");
		else if (i == '\t') put(s, "\\t");
		else if (i == '\'' || i == '\\') ak_buf_printf(s->b, "\\%c", (char)i);
		else ak_buf_put(s->b, u, n);
		ak_buf_u8(s->b, '\'');
		return;
	}
	case 'n':
		if (d[1] == '7') {
			ak_buf_printf(s->b, "%llu", (unsigned long long)i);
			return;
		}
		break;
	case 'e': {
		const char *q = strchr(d, ';');
		ak_json_types t;
		const char *const *cases;
		uint32_t n;
		if (s->mod && q) {
			ak_vm_module_json_types(s->mod, &t);
			if (t.cases(t.ctx, d + 1, (size_t)(q - d - 1), &cases, &n) && i >= 0 && (uint64_t)i < n) {
				ak_buf_printf(s->b, ".%s", cases[i]);
				return;
			}
		}
		break;
	}
	}
	ak_buf_printf(s->b, "%lld", (long long)i);
}

static void show_str(shower *s, const char *p, size_t n) {
	ak_buf_u8(s->b, '"');
	size_t shown = n > SHOW_STR ? SHOW_STR : n;
	while (shown < n && shown > 0 && ((unsigned char)p[shown] & 0xC0) == 0x80) shown--;  // a whole character
	for (size_t i = 0; i < shown; i++) {
		unsigned char c = (unsigned char)p[i];
		switch (c) {
		case '"': put(s, "\\\""); break;
		case '\\': put(s, "\\\\"); break;
		case '\n': put(s, "\\n"); break;
		case '\t': put(s, "\\t"); break;
		case '\r': put(s, "\\r"); break;
		default:
			if (c < 0x20 || c == 0x7f) ak_buf_printf(s->b, "\\u{%x}", c);
			else ak_buf_u8(s->b, c);
		}
	}
	if (shown < n) put(s, "…");
	ak_buf_u8(s->b, '"');
}

// show_fields writes a struct's fields or an enum case's payload: name: v,
// or v alone where the name is a position ("0").
static void show_fields(shower *s, const ak_val *f, uint16_t n, const uint8_t *kinds, const char *const *names, const ak_shape *sh, int depth) {
	bool first = true;
	for (uint16_t i = 0; i < n; i++) {
		const char *name = names ? names[i] : NULL;
		if (name && name[0] == '$') continue;  // the lowering's own (a component's owner)
		if (!first) put(s, ", ");
		first = false;
		if (full(s)) {
			put(s, "…");
			return;
		}
		if (name && *name && !(name[0] >= '0' && name[0] <= '9')) ak_buf_printf(s->b, "%s: ", name);
		show(s, f[i], (ak_kind)kinds[i], name_desc(s, sh, name), depth + 1);
	}
}

static void map_descs(const char *d, const char **kd, const char **vd) {
	*kd = *vd = "-";
	if (*d == '{') *kd = "s", *vd = d + 1;
	else if (*d == 'm') *kd = d + 1, *vd = dend(d + 1);
	else if (*d == '<') *kd = d + 1;
}

static void show(shower *s, ak_val v, ak_kind k, const char *d, int depth) {
	peel(&v, &k, &d);
	switch (k) {
	case AK_K_INT: show_int(s, v.i, d); return;
	case AK_K_FLOAT: show_float(s, v.f); return;
	case AK_K_BOOL: put(s, v.i ? "true" : "false"); return;
	case AK_K_VOID: put(s, "()"); return;
	default: break;
	}
	const ak_obj *o = v.p;
	if (!o) {
		put(s, "none");
		return;
	}
	if (depth > SHOW_DEPTH && o->tag != AK_TAG_STR && o->tag != AK_TAG_BOX) {
		put(s, "…");
		return;
	}
	switch (o->tag) {
	case AK_TAG_STR: {
		const ak_str *str = v.p;
		show_str(s, str->data, str->len);
		return;
	}
	case AK_TAG_BYTES:
		ak_buf_printf(s->b, "<%u bytes>", ((const ak_bytes *)o)->len);
		return;
	case AK_TAG_ARR: {
		const ak_arr *a = v.p;
		const char *ed = *d == '[' ? d + 1 : "-";
		ak_kind ek = (ak_kind)(ak_arr_kind(a) & 0xf);
		put(s, "[");
		for (uint32_t i = 0; i < a->len; i++) {
			if (i) put(s, ", ");
			if (i >= SHOW_ITEMS || full(s)) {
				put(s, "…");
				break;
			}
			show(s, ak_arr_get(a, i), ek, ed, depth + 1);
		}
		put(s, "]");
		return;
	}
	case AK_TAG_MAP: {
		const ak_map *m = v.p;
		const char *kd, *vd;
		map_descs(d, &kd, &vd);
		ak_kind kk = (ak_kind)(m->h.aux & 0xf), vk = (ak_kind)(m->h.aux >> 4);
		bool set = ak_map_is_set(m);
		put(s, set ? "Set([" : "[");
		uint32_t shown = 0;
		for (uint32_t i = 0; i < m->len; i++) {
			if (m->dead[i]) continue;
			if (shown) put(s, ", ");
			if (shown >= SHOW_ITEMS || full(s)) {
				put(s, "…");
				break;
			}
			show(s, m->keys[i], kk, kd, depth + 1);
			if (!set) {
				put(s, ": ");
				show(s, m->vals[i], vk, vd, depth + 1);
			}
			shown++;
		}
		if (!set && !m->live) put(s, ":");
		put(s, set ? "])" : "]");
		return;
	}
	case AK_TAG_STRUCT: {
		const ak_struct *st = v.p;
		const ak_shape *sh = st->shape;
		bool tuple = sh->name[0] == '(';
		if (!tuple) type_name(s->b, sh->name, strlen(sh->name));
		put(s, "(");
		show_fields(s, st->f, sh->nfields, sh->kinds, sh->names, sh, depth);
		put(s, ")");
		return;
	}
	case AK_TAG_ENUM: {
		const ak_enum *e = v.p;
		const ak_shape *sh = e->shape;
		uint16_t c = ak_enum_case(e);
		if (c >= sh->ncases) {
			put(s, "<enum>");
			return;
		}
		const ak_case *cs = &sh->cases[c];
		ak_buf_printf(s->b, ".%s", cs->name);
		if (cs->nfields) {
			put(s, "(");
			show_fields(s, e->f, cs->nfields, cs->kinds, cs->names, sh, depth);
			put(s, ")");
		}
		return;
	}
	case AK_TAG_BOX:
		show(s, ((const ak_box *)o)->v, (ak_kind)o->aux, "-", depth);
		return;
	case AK_TAG_CLOSURE: {
		const ak_closure *c = v.p;
		if (c->invoke == ak_vm_closure_invoke) ak_buf_printf(s->b, "fn %s", ((const ak_func *)c->fn)->name->data);
		else put(s, "fn");
		return;
	}
	case AK_TAG_CELL:
	case AK_TAG_MEMO:
	case AK_TAG_ACC: put(s, "<not computed>"); return;
	case AK_TAG_TASK: put(s, "<task>"); return;
	case AK_TAG_VIEW: put(s, "<view>"); return;
	case AK_TAG_STORE: put(s, "<store>"); return;
	case AK_TAG_TREE: ak_buf_printf(s->b, "<%lld entries>", (long long)((const ak_tree *)o)->count); return;
	case AK_TAG_NATIVE: put(s, "<native object>"); return;
	default: put(s, "<object>"); return;
	}
}

// show_value writes v's text into b, cut short past SHOW_MAX bytes.
static void show_value(ak_buf *b, ak_module *mod, ak_val v, ak_kind k, const char *d) {
	shower s = {b, b->len + SHOW_MAX, mod};
	size_t start = b->len;
	show(&s, v, k, d, 0);
	if (b->len - start > SHOW_MAX) {
		size_t cut = start + SHOW_MAX;
		while (cut > start && (b->data[cut] & 0xC0) == 0x80) cut--;
		b->len = cut;
		ak_buf_put(b, "…", strlen("…"));
	}
}

// ---- Children ----

// A value's child: its name, value, kind and descriptor.
typedef struct {
	ak_buf name;
	ak_val v;
	ak_kind k;
	const char *d;
} child;

static uint32_t nchildren(ak_val v, ak_kind k, const char *d) {
	peel(&v, &k, &d);
	if (k != AK_K_REF || !v.p) return 0;
	const ak_obj *o = v.p;
	switch (o->tag) {
	case AK_TAG_STRUCT: {
		const ak_shape *sh = ((const ak_struct *)o)->shape;
		uint32_t n = 0;
		for (uint16_t i = 0; i < sh->nfields; i++) n += !(sh->names && sh->names[i] && sh->names[i][0] == '$');
		return n;
	}
	case AK_TAG_ENUM: {
		const ak_enum *e = v.p;
		return ak_enum_case(e) < e->shape->ncases ? e->shape->cases[ak_enum_case(e)].nfields : 0;
	}
	case AK_TAG_ARR: return ((const ak_arr *)o)->len;
	case AK_TAG_MAP: return ((const ak_map *)o)->live;
	default: return 0;
	}
}

// child_at finds v's child i (< nchildren), into c (its name appended to
// c->name).
static void child_at(ak_module *mod, ak_val v, ak_kind k, const char *d, uint32_t i, child *c) {
	peel(&v, &k, &d);
	const ak_obj *o = v.p;
	shower s = {&c->name, (size_t)-1, mod};
	c->v = (ak_val){0};
	c->k = AK_K_REF;
	c->d = "-";
	switch (o->tag) {
	case AK_TAG_STRUCT: {
		const ak_struct *st = v.p;
		const ak_shape *sh = st->shape;
		for (uint16_t f = 0; f < sh->nfields; f++) {
			const char *name = sh->names ? sh->names[f] : NULL;
			if (name && name[0] == '$') continue;
			if (i--) continue;
			if (name) put(&s, name);
			else ak_buf_printf(&c->name, "%u", f);
			c->v = st->f[f];
			c->k = (ak_kind)sh->kinds[f];
			c->d = name_desc(&s, sh, name);
			return;
		}
		return;
	}
	case AK_TAG_ENUM: {
		const ak_enum *e = v.p;
		const ak_shape *sh = e->shape;
		const ak_case *cs = &sh->cases[ak_enum_case(e)];
		const char *name = cs->names ? cs->names[i] : NULL;
		if (name && *name) put(&s, name);
		else ak_buf_printf(&c->name, "%u", i);
		c->v = e->f[i];
		c->k = (ak_kind)cs->kinds[i];
		c->d = name_desc(&s, sh, name);
		return;
	}
	case AK_TAG_ARR: {
		const ak_arr *a = v.p;
		ak_buf_printf(&c->name, "[%u]", i);
		c->v = ak_arr_get(a, i);
		c->k = (ak_kind)(ak_arr_kind(a) & 0xf);
		c->d = *d == '[' ? d + 1 : "-";
		return;
	}
	case AK_TAG_MAP: {
		const ak_map *m = v.p;
		const char *kd, *vd;
		map_descs(d, &kd, &vd);
		uint32_t at = 0;
		for (; at < m->len; at++)
			if (!m->dead[at] && !i--) break;
		if (at == m->len) return;
		ak_kind kk = (ak_kind)(m->h.aux & 0xf), vk = (ak_kind)(m->h.aux >> 4);
		if (ak_map_is_set(m)) {
			ak_buf_printf(&c->name, "[%u]", at);
			c->v = m->keys[at];
			c->k = kk;
			c->d = kd;
		} else {
			show_value(&c->name, mod, m->keys[at], kk, kd);
			c->v = m->vals[at];
			c->k = vk;
			c->d = vd;
		}
		return;
	}
	default: return;
	}
}

// ---- JSON ----

static void jstr(ak_buf *b, const char *p, size_t n) {
	ak_buf_u8(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];
		if (c == '"' || c == '\\') {
			ak_buf_u8(b, '\\');
			ak_buf_u8(b, c);
		} else if (c == '\n') {
			ak_buf_put(b, "\\n", 2);
		} else if (c == '\t') {
			ak_buf_put(b, "\\t", 2);
		} else if (c == '\r') {
			ak_buf_put(b, "\\r", 2);
		} else if (c < 0x20) {
			ak_buf_printf(b, "\\u%04x", c);
		} else {
			ak_buf_u8(b, c);
		}
	}
	ak_buf_u8(b, '"');
}

static void jcstr(ak_buf *b, const char *s) { jstr(b, s, strlen(s)); }

// var_json writes a variable: {"name", "type", "value", "children"}.
static void var_json(ak_buf *b, ak_module *mod, const char *name, size_t nlen, ak_val v, ak_kind k, const char *d) {
	ak_buf t = {0};
	ak_buf_put(b, "{\"name\":", 8);
	jstr(b, name, nlen);
	ak_buf_put(b, ",\"type\":", 8);
	value_type(&t, v, k, d);
	jstr(b, (const char *)t.data, t.len);
	t.len = 0;
	ak_buf_put(b, ",\"value\":", 9);
	show_value(&t, mod, v, k, d);
	jstr(b, (const char *)t.data, t.len);
	uint32_t n = nchildren(v, k, d);
	if (n) ak_buf_printf(b, ",\"children\":%u", n);
	ak_buf_u8(b, '}');
	ak_buf_free(&t);
}

// ---- The stop's frames ----

// A frame as the debugger shows it: an interpreted frame's, or a body
// inlined in one (fr NULL: its locals aren't known).
typedef struct {
	const ak_func *fn;     // the code's function
	const ak_func *shown;  // the function it shows: fn, or the one inlined
	const ak_frame *fr;
	uint32_t pc, line;
} vframe;

static uint32_t stop_frames(vframe *out, uint32_t cap, uint32_t *total) {
	ak_co *co = stop_co;
	uint32_t n = 0, t = 0;
	for (uint32_t i = co->nframes; i-- > 0;) {
		const ak_frame *fr = &co->frames[i];
		const ak_func *f = fr->fn;
		if (f->resume) continue;  // compiled to C: no pc of its own
		uint32_t pc = i == co->nframes - 1 ? stop_pc : (fr->pc ? fr->pc - 1 : 0);
		uint32_t line = line_at(f, pc);
		for (uint32_t k = 0; k < f->ninlines; k++) {
			const ak_inline *in = &f->inlines[k];
			if (pc < in->start || pc >= in->end) continue;
			if (n < cap) out[n++] = (vframe){f, &f->mod->funcs[in->func], NULL, pc, line};
			t++;
			line = in->line;
		}
		if (n < cap) out[n++] = (vframe){f, f, fr, pc, line};
		t++;
	}
	*total = t;
	return n;
}

// frame_locals lists the locals of vf in scope at its pc, into idx (an
// index in its function's locals each): one per name, the innermost.
static uint32_t frame_locals(const vframe *vf, uint32_t *idx, uint32_t cap) {
	if (!vf->fr) return 0;
	const ak_func *f = vf->fn;
	uint32_t n = 0;
	for (uint32_t i = 0; i < f->nlocals && n < cap; i++) {
		const ak_local *lc = &f->locals[i];
		if (vf->pc < lc->from || vf->pc >= lc->to) continue;
		bool shadowed = false;
		for (uint32_t j = i + 1; j < f->nlocals && !shadowed; j++) {
			const ak_local *o = &f->locals[j];
			shadowed = vf->pc >= o->from && vf->pc < o->to && ak_str_eq(o->name, lc->name);
		}
		if (!shadowed) idx[n++] = i;
	}
	return n;
}

static void local_value(const vframe *vf, const ak_local *lc, ak_val *v, ak_kind *k) {
	const ak_val *R = stop_co->regs + vf->fr->base;
	*v = R[lc->reg];
	*k = (ak_kind)vf->fn->kinds[lc->reg];
	if (lc->via == AK_LOCAL_BOX && *k == AK_K_REF && v->p && ((const ak_obj *)v->p)->tag == AK_TAG_BOX) {
		const ak_box *b = v->p;
		*k = (ak_kind)b->h.aux;
		*v = b->v;
	}
}

#define MAX_LOCALS 512

static void write_stopped(ak_buf *b, const char *reason) {
	vframe *fs = ak_alloc(sizeof(vframe) * MAX_FRAMES);
	uint32_t *idx = ak_alloc(sizeof(uint32_t) * MAX_LOCALS);
	uint32_t total, n = stop_frames(fs, MAX_FRAMES, &total);
	ak_buf_put(b, "{\"reason\":", 10);
	jcstr(b, reason);
	ak_buf_printf(b, ",\"totalFrames\":%u,\"frames\":[", total);
	for (uint32_t i = 0; i < n; i++) {
		const vframe *vf = &fs[i];
		if (i) ak_buf_u8(b, ',');
		ak_buf_put(b, "{\"name\":", 8);
		jstr(b, vf->shown->name->data, vf->shown->name->len);
		ak_buf_put(b, ",\"file\":", 8);
		jstr(b, vf->shown->file->data, vf->shown->file->len);
		ak_buf_printf(b, ",\"line\":%u,\"locals\":[", vf->line);
		uint32_t nl = frame_locals(vf, idx, MAX_LOCALS);
		for (uint32_t j = 0; j < nl; j++) {
			const ak_local *lc = &vf->fn->locals[idx[j]];
			ak_val v;
			ak_kind k;
			local_value(vf, lc, &v, &k);
			if (j) ak_buf_u8(b, ',');
			var_json(b, vf->fn->mod, lc->name->data, lc->name->len, v, k, lc->desc->data);
		}
		ak_buf_put(b, "]}", 2);
	}
	ak_buf_put(b, "]}", 2);
	ak_free(idx);
	ak_free(fs);
}

// answer writes a variables request's answer: the children of the value
// its path names.
static void answer(const vreq *r, ak_buf *b) {
	vframe *fs = ak_alloc(sizeof(vframe) * MAX_FRAMES);
	uint32_t *idx = ak_alloc(sizeof(uint32_t) * MAX_LOCALS);
	uint32_t total, n = stop_frames(fs, MAX_FRAMES, &total);
	const char *err = NULL;
	if (r->frame >= n || r->npath == 0) {
		err = "no such frame";
		goto done;
	}
	const vframe *vf = &fs[r->frame];
	uint32_t nl = frame_locals(vf, idx, MAX_LOCALS);
	if (r->path[0] >= nl) {
		err = "no such variable";
		goto done;
	}
	ak_module *mod = vf->fn->mod;
	const ak_local *lc = &vf->fn->locals[idx[r->path[0]]];
	ak_val v;
	ak_kind k;
	local_value(vf, lc, &v, &k);
	const char *d = lc->desc->data;
	for (uint32_t p = 1; p < r->npath; p++) {
		if (r->path[p] >= nchildren(v, k, d)) {
			err = "no such variable";
			goto done;
		}
		child c = {0};
		child_at(mod, v, k, d, r->path[p], &c);
		ak_buf_free(&c.name);
		v = c.v, k = c.k, d = c.d;
	}
	uint32_t nc = nchildren(v, k, d);
	uint32_t count = r->count && r->count < MAX_CHILDREN ? r->count : MAX_CHILDREN;
	ak_buf_printf(b, "{\"total\":%u,\"variables\":[", nc);
	for (uint32_t i = r->start; i < nc && i - r->start < count; i++) {
		child c = {0};
		child_at(mod, v, k, d, i, &c);
		if (i > r->start) ak_buf_u8(b, ',');
		var_json(b, mod, (const char *)c.name.data, c.name.len, c.v, c.k, c.d);
		ak_buf_free(&c.name);
	}
	ak_buf_put(b, "]}", 2);
done:
	if (err) {
		ak_buf_put(b, "{\"error\":", 9);
		jcstr(b, err);
		ak_buf_u8(b, '}');
	}
	ak_free(idx);
	ak_free(fs);
}

// ---- Stopping ----

static void stop(ak_co *co, uint32_t pc, const char *reason) {
	const ak_frame *fr = &co->frames[co->nframes - 1];
	lock();
	ak_dbg_sink s = sink;
	if (!s) {
		// No one to tell: on as if it hadn't stopped.
		atomic_store(&pause_req, 0);
		atomic_store(&step_on, 0);
		update_on();
		unlock();
		step = 0;
		return;
	}
	paused = true;
	resume = 0;
	atomic_store(&pause_req, 0);
	atomic_store(&step_on, 0);
	update_on();
	unlock();
	step = 0;
	stop_co = co;
	stop_pc = pc;
	last.valid = true;
	last.co = co;
	last.fn = fr->fn;
	last.depth = co->nframes;
	last.base = fr->base;
	last.line = line_at(fr->fn, pc);
	last.pc = pc;

	ak_buf j = {0};
	write_stopped(&j, reason);
	s(AK_DBG_STOPPED, 0, (const char *)j.data, j.len);
	ak_buf_free(&j);

	// Wait for the command to go on with, answering what's asked
	// meanwhile.
	int cmd;
	lock();
	for (;;) {
		if (atomic_load(&held) || !sink) {
			cmd = AK_DBG_CONTINUE;
			break;
		}
		if (vq) {
			vreq *r = vq;
			vq = r->next;
			if (!vq) vq_tail = NULL;
			ak_dbg_sink rs = sink;
			unlock();
			ak_buf a = {0};
			answer(r, &a);
			rs(AK_DBG_VARIABLES, r->seq, (const char *)a.data, a.len);
			ak_buf_free(&a);
			ak_free(r);
			lock();
			continue;
		}
		if (resume) {
			cmd = resume;
			break;
		}
		ak_cond_wait(&cv, &mu);
	}
	paused = false;
	resume = 0;
	bool stepping = cmd == AK_DBG_NEXT || cmd == AK_DBG_STEP_IN || cmd == AK_DBG_STEP_OUT;
	atomic_store(&step_on, stepping);
	update_on();
	s = sink;
	unlock();
	if (stepping) {
		step = cmd;
		step_co = co;
		step_fn = fr->fn;
		step_depth = co->nframes;
		step_base = fr->base;
		step_line = line_at(fr->fn, pc);
		step_pc = pc;
	}
	stop_co = NULL;
	if (s) s(AK_DBG_CONTINUED, 0, NULL, 0);
}

// ---- What other threads ask ----

bool ak_dbg_available(void) { return true; }

void ak_dbg_set_sink(ak_dbg_sink s) {
	lock();
	sink = s;
	ak_cond_broadcast(&cv);
	unlock();
}

static void free_bpfile(bpfile *b) {
	ak_free(b->file);
	ak_free(b->lines);
}

static void count_bps(void) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < nbps; i++) n += bps[i].n;
	atomic_store(&bp_count, n);
	atomic_fetch_add(&bp_gen, 1);
	update_on();
}

void ak_dbg_set_breakpoints(const char *file, size_t flen, const uint32_t *lines, uint32_t n) {
	lock();
	bpfile *b = NULL;
	for (uint32_t i = 0; i < nbps && !b; i++)
		if (bps[i].flen == flen && memcmp(bps[i].file, file, flen) == 0) b = &bps[i];
	if (!b && n) {
		bps = ak_realloc(bps, sizeof(bpfile) * (nbps + 1));
		b = &bps[nbps++];
		b->file = ak_alloc(flen + 1);
		memcpy(b->file, file, flen);
		b->file[flen] = 0;
		b->flen = flen;
		b->lines = NULL;
		b->n = 0;
	}
	if (b) {
		ak_free(b->lines);
		b->lines = NULL;
		b->n = n;
		if (n) {
			b->lines = ak_alloc(sizeof(uint32_t) * n);
			memcpy(b->lines, lines, sizeof(uint32_t) * n);
		} else {
			free_bpfile(b);
			*b = bps[--nbps];
		}
	}
	count_bps();
	unlock();
}

void ak_dbg_command(int cmd) {
	lock();
	if (cmd == AK_DBG_PAUSE) {
		if (!paused) {
			atomic_store(&pause_req, 1);
			update_on();
		}
	} else if (paused && cmd >= AK_DBG_CONTINUE && cmd <= AK_DBG_STEP_OUT) {
		resume = cmd;
		ak_cond_broadcast(&cv);
	}
	unlock();
}

void ak_dbg_variables(uint32_t seq, uint32_t frame, const uint32_t *path, uint32_t npath, uint32_t start, uint32_t count) {
	lock();
	if (!paused || !sink) {
		ak_dbg_sink s = sink;
		unlock();
		static const char err[] = "{\"error\":\"not paused\"}";
		if (s) s(AK_DBG_VARIABLES, seq, err, sizeof err - 1);
		return;
	}
	vreq *r = ak_alloc(sizeof *r + sizeof(uint32_t) * (npath ? npath : 1));
	*r = (vreq){.seq = seq, .frame = frame, .start = start, .count = count, .npath = npath};
	if (npath) memcpy(r->path, path, sizeof(uint32_t) * npath);
	if (vq_tail) vq_tail->next = r;
	else vq = r;
	vq_tail = r;
	ak_cond_broadcast(&cv);
	unlock();
}

void ak_dbg_hold(bool hold) {
	lock();
	atomic_store(&held, hold);
	ak_cond_broadcast(&cv);
	unlock();
}

bool ak_dbg_paused(void) {
	lock();
	bool p = paused;
	unlock();
	return p;
}

void ak_dbg_reset(void) {
	lock();
	for (uint32_t i = 0; i < nbps; i++) free_bpfile(&bps[i]);
	ak_free(bps);
	bps = NULL;
	nbps = 0;
	atomic_store(&pause_req, 0);
	atomic_store(&step_on, 0);
	count_bps();
	if (paused) {
		resume = AK_DBG_CONTINUE;
		ak_cond_broadcast(&cv);
	}
	for (vreq *r = vq, *next; r; r = next) {
		next = r->next;
		ak_free(r);
	}
	vq = vq_tail = NULL;
	unlock();
}

#else

bool ak_dbg_available(void) { return false; }
void ak_dbg_set_sink(ak_dbg_sink sink) { AK_UNUSED(sink); }
void ak_dbg_set_breakpoints(const char *file, size_t flen, const uint32_t *lines, uint32_t n) {
	AK_UNUSED(file);
	AK_UNUSED(flen);
	AK_UNUSED(lines);
	AK_UNUSED(n);
}
void ak_dbg_command(int cmd) { AK_UNUSED(cmd); }
void ak_dbg_variables(uint32_t seq, uint32_t frame, const uint32_t *path, uint32_t npath, uint32_t start, uint32_t count) {
	AK_UNUSED(seq);
	AK_UNUSED(frame);
	AK_UNUSED(path);
	AK_UNUSED(npath);
	AK_UNUSED(start);
	AK_UNUSED(count);
}
void ak_dbg_hold(bool hold) { AK_UNUSED(hold); }
bool ak_dbg_paused(void) { return false; }
void ak_dbg_reset(void) {}

#endif
