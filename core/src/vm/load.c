#include "vm/vm_int.h"
#include "vm/debug.h"

#include <stdatomic.h>

#include <stdio.h>

// ---- Builtins ----

// The builtins by name: a hash table (open addressing, at most half
// full), so registering the core's hundreds as it starts and finding a
// module's as it loads cost a lookup each, not a search of them all.
typedef struct {
	const char *name;  // NULL: empty
	ak_builtin_fn fn;
	uint32_t hash;
} builtin_entry;
static builtin_entry *builtins;
static uint32_t builtins_cap, builtins_len;

static uint32_t name_hash(const char *s) {
	uint32_t h = 2166136261u;  // FNV-1a
	for (; *s; s++) {
		h ^= (uint8_t)*s;
		h *= 16777619u;
	}
	return h;
}

static builtin_entry *builtin_slot(const char *name, uint32_t h) {
	for (uint32_t i = h & (builtins_cap - 1);; i = (i + 1) & (builtins_cap - 1)) {
		builtin_entry *e = &builtins[i];
		if (!e->name || (e->hash == h && strcmp(e->name, name) == 0)) return e;
	}
}

void ak_builtin_register(const char *name, ak_builtin_fn fn) {
	if ((builtins_len + 1) * 2 > builtins_cap) {
		builtin_entry *old = builtins;
		uint32_t n = builtins_cap;
		builtins_cap = n ? n * 2 : 1024;
		builtins = ak_alloc(sizeof(builtin_entry) * builtins_cap);
		for (uint32_t i = 0; i < n; i++)
			if (old[i].name) *builtin_slot(old[i].name, old[i].hash) = old[i];
		ak_free(old);
	}
	uint32_t h = name_hash(name);
	builtin_entry *e = builtin_slot(name, h);
	if (!e->name) builtins_len++;
	*e = (builtin_entry){name, fn, h};  // a name registered again: the new fn
}

// rt.freeze(value, kind): the value, made a constant (rt.h's ak_freeze)
// that the running module owns: it's freed with the module. Only init
// freezes, on the thread that loaded the module.
static ak_val rt_freeze(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_module *m = ak_vm_running_module();
	ak_freeze(args[0], (ak_kind)args[1].i, m ? &m->frozen : NULL);
	return args[0];
}

void ak_vm_freeze_init(void) { ak_builtin_register("rt.freeze", rt_freeze); }

ak_builtin_fn ak_builtin_find(const char *name) {
	if (!builtins_cap) return NULL;
	builtin_entry *e = builtin_slot(name, name_hash(name));
	return e->name ? e->fn : NULL;
}

// ---- Loading ----

typedef struct {
	ak_reader r;
	char *err;
	size_t errcap;
	bool failed;
} loader;

static void fail(loader *l, const char *msg) {
	if (!l->failed) snprintf(l->err, l->errcap, "bytecode: %s", msg);
	l->failed = true;
}

static uint32_t count(loader *l, uint32_t elem) {
	uint32_t n = ak_read_u32(&l->r);
	// Each element takes at least elem bytes: a bound on what's left.
	if (l->r.err || (uint64_t)n * elem > l->r.len - l->r.off) {
		fail(l, "truncated module");
		return 0;
	}
	return n;
}

static ak_str *rstr(loader *l) {
	uint32_t n;
	const uint8_t *p = ak_read_bytes(&l->r, &n);
	if (l->r.err) {
		fail(l, "truncated module");
		return ak_str_empty();
	}
	return ak_str_new((const char *)p, n);
}

static uint8_t *rkinds(loader *l, uint32_t *n) {
	*n = count(l, 1);
	uint8_t *k = ak_alloc(*n ? *n : 1);
	for (uint32_t i = 0; i < *n; i++) {
		k[i] = ak_read_u8(&l->r);
		if (k[i] > AK_K_REF) fail(l, "bad kind");
	}
	return k;
}

typedef struct {
	ak_str *name;
	uint8_t kind;
	int32_t shape;
} rfield;

// rname reads a field's name, which may carry its JSON descriptor after a
// NUL ("title\0s": bytecode.Field): one that doesn't gets an empty one,
// so that what reads a field's descriptor past its name's NUL
// (ak_json_field_desc, the debugger's) reads within the string.
static ak_str *rname(loader *l) {
	ak_str *s = rstr(l);
	if (memchr(s->data, 0, s->len)) return s;
	ak_str *t = ak_str_new(s->data, s->len + 1);  // its NUL, then the string's own
	ak_release(s);
	return t;
}

static rfield *rfields(loader *l, uint32_t *n) {
	*n = count(l, 9);
	rfield *f = ak_alloc(sizeof(rfield) * (*n ? *n : 1));
	for (uint32_t i = 0; i < *n; i++) {
		f[i].name = rname(l);
		f[i].kind = ak_read_u8(&l->r);
		f[i].shape = (int32_t)ak_read_u32(&l->r);
	}
	return f;
}

static ak_str *intern(ak_module *m, ak_str *s) {
	for (uint32_t i = 0; i < m->nstrings; i++) {
		if (ak_str_eq(m->strings[i], s)) {
			ak_release(s);
			return m->strings[i];  // immortal, freed with the module
		}
	}
	return s;
}

// own_hook notes a method of a type's own ==, hash, text, JSON or order
// (its reserved name: lang/ir's own.go) as the shape's hook.
static void own_hook(ak_vm_shape *vs, const ak_method *mt) {
	static const char *const names[AK_NHOOKS] = {"#eq", "#hash", "#show", "#json", "#fromjson", "#cmp"};
	const ak_str *n = mt->name;
	if (n->len == 0 || n->data[0] != '#') return;
	for (int h = 0; h < AK_NHOOKS; h++) {
		size_t k = strlen(names[h]);
		if (n->len < k || strncmp(n->data, names[h], k) != 0 || n->data[k] != 0) continue;
		vs->s.hooks |= (uint8_t)(1u << h);
		vs->hook[h] = mt->func;
		if (k < n->len) vs->s.jdesc = n->data + k + 1;  // #json, a NUL, its descriptor
		return;
	}
}

// shape_call is rt's ak_shape_call: the hook's function of the module
// that loaded the shape.
static ak_val shape_call(const ak_shape *s, int hook, ak_val *args, uint32_t n) {
	const ak_vm_shape *vs = (const ak_vm_shape *)s;
	const ak_func *f = &vs->mod->funcs[vs->hook[hook]];
	return f->native ? f->native(args, NULL) : ak_vm_call(f, args, n);
}

static void fill_fields(ak_module *m, loader *l, rfield *rf, uint32_t n, uint8_t *kinds, const ak_shape **shapes, const char **names) {
	for (uint32_t i = 0; i < n; i++) {
		kinds[i] = rf[i].kind;
		if (rf[i].shape >= 0) {
			if ((uint32_t)rf[i].shape >= m->nshapes) fail(l, "bad shape index");
			else shapes[i] = &m->shapes[rf[i].shape].s;
		}
		names[i] = rf[i].name->data;  // kept alive by the module (fieldnames below)
	}
}

ak_module *ak_module_load(const uint8_t *p, size_t n, char *err, size_t errcap) {
	loader L = {{p, n, 0, false}, err, errcap, false}, *l = &L;
	if (n < 8 || memcmp(p, "AKBC", 4) != 0) {
		fail(l, "not a module");
		return NULL;
	}
	l->r.off = 4;
	uint32_t version = ak_read_u32(&l->r);
	if (version != AK_BC_VERSION) {
		snprintf(err, errcap, "bytecode: version %u, want %d", version, AK_BC_VERSION);
		return NULL;
	}
	ak_module *m = ak_alloc(sizeof *m);
	m->rc = 1;
	m->nints = count(l, 8);
	m->ints = ak_alloc(sizeof(int64_t) * (m->nints ? m->nints : 1));
	for (uint32_t i = 0; i < m->nints; i++) m->ints[i] = (int64_t)ak_read_u64(&l->r);
	m->nfloats = count(l, 8);
	m->floats = ak_alloc(sizeof(double) * (m->nfloats ? m->nfloats : 1));
	for (uint32_t i = 0; i < m->nfloats; i++) m->floats[i] = ak_read_f64(&l->r);
	m->nstrings = count(l, 4);
	m->strings = ak_alloc(sizeof(ak_str *) * (m->nstrings ? m->nstrings : 1));
	// Constants are immortal: reading them costs no reference counting,
	// and spawn's threads share them. A module is freed only once nothing
	// of what ran it is left (on a reload, after the old views went and
	// kept state was copied into the new module's values).
	for (uint32_t i = 0; i < m->nstrings; i++) {
		m->strings[i] = rstr(l);
		ak_make_immortal(m->strings[i]);
	}

	// Shapes refer to each other by index: allocate them all first.
	m->nshapes = count(l, 4);
	m->shapes = ak_alloc(sizeof(ak_vm_shape) * (m->nshapes ? m->nshapes : 1));
	__typeof__(m->names) *names = &m->names;
	ak_shape_call = shape_call;  // what rt calls a type's own ==, hash and JSON through
	for (uint32_t i = 0; i < m->nshapes && !l->failed; i++) {
		ak_vm_shape *vs = &m->shapes[i];
		vs->mod = m;
		ak_str *name = rstr(l);
		ak_vec_push(names, name);
		vs->s.name = name->data;
		uint32_t nf;
		rfield *rf = rfields(l, &nf);
		vs->s.nfields = (uint16_t)nf;
		vs->kinds = ak_alloc(nf ? nf : 1);
		vs->shapes = ak_alloc(sizeof(ak_shape *) * (nf ? nf : 1));
		vs->names = ak_alloc(sizeof(char *) * (nf ? nf : 1));
		fill_fields(m, l, rf, nf, vs->kinds, vs->shapes, vs->names);
		for (uint32_t j = 0; j < nf; j++) ak_vec_push(names, rf[j].name);
		ak_free(rf);
		vs->s.kinds = vs->kinds;
		vs->s.shapes = vs->shapes;
		vs->s.names = vs->names;
		uint32_t nc = count(l, 8);
		vs->s.ncases = (uint16_t)nc;
		vs->cases = ak_alloc(sizeof(ak_case) * (nc ? nc : 1));
		for (uint32_t c = 0; c < nc && !l->failed; c++) {
			ak_str *cn = rstr(l);
			ak_vec_push(names, cn);
			uint32_t cnf;
			rfield *crf = rfields(l, &cnf);
			uint8_t *ck = ak_alloc(cnf ? cnf : 1);
			const ak_shape **cs = ak_alloc(sizeof(ak_shape *) * (cnf ? cnf : 1));
			const char **cnames = ak_alloc(sizeof(char *) * (cnf ? cnf : 1));
			fill_fields(m, l, crf, cnf, ck, cs, cnames);
			for (uint32_t j = 0; j < cnf; j++) ak_vec_push(names, crf[j].name);
			ak_free(crf);
			vs->cases[c] = (ak_case){.name = cn->data, .nfields = (uint16_t)cnf, .kinds = ck, .shapes = cs, .names = cnames};
		}
		vs->s.cases = vs->cases;
		vs->s.is_error = ak_read_u8(&l->r) == 1;
		vs->nmethods = count(l, 8);
		vs->methods = ak_alloc(sizeof(ak_method) * (vs->nmethods ? vs->nmethods : 1));
		for (uint32_t k = 0; k < vs->nmethods; k++) {
			vs->methods[k].name = intern(m, rstr(l));
			vs->methods[k].func = ak_read_u32(&l->r);
			own_hook(vs, &vs->methods[k]);
		}
	}

	m->nfuncs = count(l, 8);
	m->funcs = ak_alloc(sizeof(ak_func) * (m->nfuncs ? m->nfuncs : 1));
	for (uint32_t i = 0; i < m->nfuncs && !l->failed; i++) {
		ak_func *f = &m->funcs[i];
		f->mod = m;
		f->index = i;
		f->name = rstr(l);
		f->file = rstr(l);
		f->nparams = ak_read_u16(&l->r);
		uint32_t nk, nc;
		f->kinds = rkinds(l, &nk);
		f->nregs = (uint16_t)nk;
		f->capkinds = rkinds(l, &nc);
		f->ncap = (uint16_t)nc;
		f->nrefs = 0;
		for (uint32_t r = 0; r < nk; r++) f->nrefs += f->kinds[r] == AK_K_REF;
		f->refs = ak_alloc(sizeof(uint16_t) * (f->nrefs ? f->nrefs : 1));
		for (uint32_t r = 0, j = 0; r < nk; r++)
			if (f->kinds[r] == AK_K_REF) f->refs[j++] = (uint16_t)r;
		f->ret = ak_read_u8(&l->r);
		f->flags = ak_read_u8(&l->r);
		if (f->nparams > f->nregs) fail(l, "more parameters than registers");
		f->ncode = count(l, 2);
		f->code = ak_alloc(sizeof(uint16_t) * (f->ncode ? f->ncode : 1));
		for (uint32_t c = 0; c < f->ncode; c++) f->code[c] = ak_read_u16(&l->r);
		// Every instruction is one the interpreter has, whole: its
		// dispatch indexes a table by the opcode, unchecked.
		for (uint32_t pc = 0; pc < f->ncode && !l->failed;) {
			uint16_t op = f->code[pc];
			if (op >= AK_OP__COUNT) fail(l, "bad opcode");
			else if (pc + ak_op_width[op] > f->ncode) fail(l, "truncated instruction");
			else pc += ak_op_width[op];
		}
		f->nhandlers = count(l, 12);
		f->handlers = ak_alloc(sizeof(ak_handler) * (f->nhandlers ? f->nhandlers : 1));
		for (uint32_t h = 0; h < f->nhandlers; h++) {
			f->handlers[h].start = ak_read_u32(&l->r);
			f->handlers[h].end = ak_read_u32(&l->r);
			f->handlers[h].target = ak_read_u32(&l->r);
		}
		f->nlines = count(l, 8);
		f->lines = ak_alloc(sizeof(ak_line) * (f->nlines ? f->nlines : 1));
		for (uint32_t h = 0; h < f->nlines; h++) {
			f->lines[h].pc = ak_read_u32(&l->r);
			f->lines[h].line = ak_read_u32(&l->r);
		}
		f->ninlines = count(l, 16);
		f->inlines = ak_alloc(sizeof(ak_inline) * (f->ninlines ? f->ninlines : 1));
		for (uint32_t h = 0; h < f->ninlines; h++) {
			f->inlines[h].start = ak_read_u32(&l->r);
			f->inlines[h].end = ak_read_u32(&l->r);
			f->inlines[h].func = ak_read_u32(&l->r);
			f->inlines[h].line = ak_read_u32(&l->r);
		}
		f->nlocals = count(l, 19);
		f->locals = ak_alloc(sizeof(ak_local) * (f->nlocals ? f->nlocals : 1));
		for (uint32_t h = 0; h < f->nlocals; h++) {
			ak_local *lc = &f->locals[h];
			lc->name = rstr(l);
			lc->reg = ak_read_u16(&l->r);
			lc->via = ak_read_u8(&l->r);
			lc->desc = rstr(l);
			lc->from = ak_read_u32(&l->r);
			lc->to = ak_read_u32(&l->r);
			if (lc->reg >= f->nregs || lc->via > AK_LOCAL_SIGNAL || lc->from > lc->to || lc->to > f->ncode) fail(l, "bad local");
		}
	}

	m->nbuiltins = count(l, 4);
	m->builtins = ak_alloc(sizeof(ak_builtin_fn) * (m->nbuiltins ? m->nbuiltins : 1));
	for (uint32_t i = 0; i < m->nbuiltins && !l->failed; i++) {
		ak_str *name = rstr(l);
		m->builtins[i] = ak_builtin_find(name->data);
		if (!m->builtins[i] && !l->failed) {
			snprintf(err, errcap, "bytecode: no builtin %s", name->data);
			l->failed = true;
		}
		ak_release(name);
	}
	uint32_t ng;
	m->gkinds = rkinds(l, &ng);
	m->nglobals = ng;
	m->globals = ak_alloc(sizeof(ak_val) * (ng ? ng : 1));
	m->init = ak_read_u32(&l->r);
	m->app = (int32_t)ak_read_u32(&l->r);
	m->main = (int32_t)ak_read_u32(&l->r);
	m->ntests = count(l, 8);
	m->tests = ak_alloc(sizeof(*m->tests) * (m->ntests ? m->ntests : 1));
	for (uint32_t i = 0; i < m->ntests; i++) {
		m->tests[i].name = rstr(l);
		m->tests[i].func = ak_read_u32(&l->r);
		m->tests[i].flags = ak_read_u8(&l->r);
		uint32_t np = count(l, 8);
		m->tests[i].nparams = np;
		m->tests[i].params = ak_alloc(sizeof(ak_str *) * 2 * (np ? np : 1));
		for (uint32_t k = 0; k < 2 * np; k++) m->tests[i].params[k] = rstr(l);
	}
	if (l->r.err) fail(l, "truncated module");
	if (!l->failed && m->nfuncs && m->init >= m->nfuncs) fail(l, "bad init function");
	if (!l->failed && m->app >= (int32_t)m->nfuncs) fail(l, "bad app function");
	if (!l->failed && m->main >= (int32_t)m->nfuncs) fail(l, "bad main function");
	for (uint32_t i = 0; i < m->ntests && !l->failed; i++)
		if (m->tests[i].func >= m->nfuncs) fail(l, "bad test function");
	for (uint32_t i = 0; i < m->nshapes && !l->failed; i++)
		for (uint32_t k = 0; k < m->shapes[i].nmethods; k++)
			if (m->shapes[i].methods[k].func >= m->nfuncs) fail(l, "bad method function");
	for (uint32_t i = 0; i < m->nfuncs && !l->failed; i++)
		for (uint32_t k = 0; k < m->funcs[i].ninlines; k++)
			if (m->funcs[i].inlines[k].func >= m->nfuncs) fail(l, "bad inlined function");
	if (l->failed) {
		ak_module_release(m);
		return NULL;
	}
	return m;
}

void ak_module_retain(ak_module *m) { atomic_fetch_add(&m->rc, 1); }

static void module_free(void *ctx);

// ak_module_release may run on any thread (a closure a spawn's worker
// made and let go); the module is freed on the main thread, where
// everything that reads its constants without holding it (views, state,
// delivered results) lives: a last reference let go elsewhere posts the
// free there.
void ak_module_release(ak_module *m) {
	if (atomic_fetch_sub(&m->rc, 1) != 1) return;
	if (!ak_signal_on_main_thread()) {
		ak_post(module_free, m);
		return;
	}
	module_free(m);
}

static void module_free(void *ctx) {
	ak_module *m = ctx;
	for (uint32_t i = 0; i < m->nglobals; i++) ak_release_val(m->globals[i], (ak_kind)m->gkinds[i]);
	ak_free(m->globals);
	ak_frozen_free(&m->frozen);
	ak_free(m->gkinds);
	for (uint32_t i = 0; i < m->nfuncs; i++) {
		ak_func *f = &m->funcs[i];
		ak_release(f->name);
		ak_release(f->file);
		ak_free(f->kinds);
		ak_free(f->refs);
		ak_free(f->capkinds);
		ak_free(f->code);
		ak_free(f->handlers);
		ak_free(f->lines);
		ak_free(f->inlines);
		for (uint32_t k = 0; k < f->nlocals; k++) {
			ak_release(f->locals[k].name);
			ak_release(f->locals[k].desc);
		}
		ak_free(f->locals);
		ak_vm_dbg_forget(f);
	}
	ak_free(m->funcs);
	for (uint32_t i = 0; i < m->nshapes; i++) {
		ak_vm_shape *vs = &m->shapes[i];
		ak_free(vs->kinds);
		ak_free((void *)vs->shapes);
		ak_free((void *)vs->names);
		for (uint32_t c = 0; c < vs->s.ncases; c++) {
			ak_free((void *)vs->cases[c].kinds);
			ak_free((void *)vs->cases[c].shapes);
			ak_free((void *)vs->cases[c].names);
		}
		ak_free(vs->cases);
		ak_free((void *)vs->case_names);
		for (uint32_t k = 0; k < vs->nmethods; k++) {
			ak_str *n = vs->methods[k].name;
			bool constant = false;
			for (uint32_t i = 0; i < m->nstrings && !constant; i++) constant = m->strings[i] == n;
			if (!constant) ak_release(n);
		}
		ak_free(vs->methods);
		// Payloadless singletons of payload enums.
		if (vs->s.singletons) {
			ak_enum **tab = vs->s.singletons;
			for (uint32_t c = 0; c < vs->s.ncases; c++) ak_free(tab[c]);
			ak_free(tab);
		}
	}
	ak_free(m->shapes);
	for (uint32_t i = 0; i < m->names.len; i++) ak_release(m->names.data[i]);
	ak_vec_free(&m->names);
	for (uint32_t i = 0; i < m->nstrings; i++) ak_free(m->strings[i]);
	ak_free(m->strings);
	ak_free(m->ints);
	ak_free(m->floats);
	ak_free(m->builtins);
	for (uint32_t i = 0; i < m->ntests; i++) {
		ak_release(m->tests[i].name);
		for (uint32_t k = 0; k < 2 * m->tests[i].nparams; k++) ak_release(m->tests[i].params[k]);
		ak_free(m->tests[i].params);
	}
	ak_free(m->tests);
	ak_free(m);
}

uint32_t ak_module_nfuncs(const ak_module *m) { return m->nfuncs; }
const ak_func *ak_module_func(const ak_module *m, uint32_t i) { return i < m->nfuncs ? &m->funcs[i] : NULL; }

const ak_func *ak_module_find(const ak_module *m, const char *name) {
	for (uint32_t i = 0; i < m->nfuncs; i++)
		if (strcmp(m->funcs[i].name->data, name) == 0) return &m->funcs[i];
	return NULL;
}

uint32_t ak_module_init(const ak_module *m) { return m->init; }
int32_t ak_module_app(const ak_module *m) { return m->app; }
int32_t ak_module_main(const ak_module *m) { return m->main; }
uint32_t ak_module_ntests(const ak_module *m) { return m->ntests; }

const char *ak_module_test(const ak_module *m, uint32_t i, uint32_t *func) {
	if (i >= m->ntests) return NULL;
	*func = m->tests[i].func;
	return m->tests[i].name->data;
}

bool ak_module_test_bench(const ak_module *m, uint32_t i) { return i < m->ntests && (m->tests[i].flags & 1); }

uint32_t ak_module_test_nparams(const ak_module *m, uint32_t i) { return i < m->ntests ? m->tests[i].nparams : 0; }

const char *ak_module_test_param(const ak_module *m, uint32_t i, uint32_t k, const char **name) {
	if (i >= m->ntests || k >= m->tests[i].nparams) return NULL;
	*name = m->tests[i].params[2 * k]->data;
	return m->tests[i].params[2 * k + 1]->data;
}

const ak_shape *ak_module_shape_named(const ak_module *m, const char *name) {
	for (uint32_t i = 0; i < m->nshapes; i++)
		if (strcmp(m->shapes[i].s.name, name) == 0) return &m->shapes[i].s;
	return NULL;
}

const ak_shape *ak_module_shape(const ak_module *m, uint32_t i) { return i < m->nshapes ? &m->shapes[i].s : NULL; }
const char *ak_func_name(const ak_func *f) { return f->name->data; }
bool ak_func_async(const ak_func *f) { return f->flags & AK_F_ASYNC; }

// The module compiled into this build, if any: compiled code runs with no
// VM frame to say which module it's in.
static ak_module *compiled;

void ak_module_attach(ak_module *m, ak_val (*const *bodies)(ak_val *args, ak_closure *clos), ak_ret (*const *directs)(ak_closure *clos, uintptr_t lim, ak_val *args), void (*const *direct_bodies)(void), int (*const *resumes)(struct ak_co *co, ak_val *out), uint32_t n) {
	for (uint32_t i = 0; i < n && i < m->nfuncs; i++) {
		ak_func *f = &m->funcs[i];
		f->native = bodies[i];
		f->direct = directs[i];
		f->body = direct_bodies[i];
		f->sig = 0;
		if (f->body && !(f->flags & (AK_F_MUT | AK_F_ASYNC)) && f->nparams <= AK_CG_SIG_MAX) {
			uint32_t sig = 0x80000000u | (uint32_t)f->nparams << 16;
			for (uint16_t p = 0; p < f->nparams; p++) sig |= ak_cg_sig_kind(f->kinds[p]) << (2 * p);
			f->sig = sig;
		}
		m->funcs[i].resume = resumes[i];
	}
	compiled = m;
}

ak_module *ak_vm_running_module(void) {
	ak_co *co = ak_vm_cur ? ak_vm_cur : &ak_vm_main;
	if (co->nframes) return co->frames[co->nframes - 1].fn->mod;
	return compiled;
}

ak_module *(*ak_app_loader)(void);
