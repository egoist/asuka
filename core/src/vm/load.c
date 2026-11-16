#include "vm/vm_int.h"

#include <stdatomic.h>

#include <stdio.h>

// ---- Builtins ----

typedef struct { const char *name; ak_builtin_fn fn; } builtin_entry;
static AK_VEC(builtin_entry) builtins;

void ak_builtin_register(const char *name, ak_builtin_fn fn) {
	for (uint32_t i = 0; i < builtins.len; i++) {
		if (strcmp(builtins.data[i].name, name) == 0) {
			builtins.data[i].fn = fn;
			return;
		}
	}
	ak_vec_push(&builtins, ((builtin_entry){name, fn}));
}

ak_builtin_fn ak_builtin_find(const char *name) {
	for (uint32_t i = 0; i < builtins.len; i++)
		if (strcmp(builtins.data[i].name, name) == 0) return builtins.data[i].fn;
	return NULL;
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

static rfield *rfields(loader *l, uint32_t *n) {
	*n = count(l, 9);
	rfield *f = ak_alloc(sizeof(rfield) * (*n ? *n : 1));
	for (uint32_t i = 0; i < *n; i++) {
		f[i].name = rstr(l);
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
		f->ret = ak_read_u8(&l->r);
		f->flags = ak_read_u8(&l->r);
		if (f->nparams > f->nregs) fail(l, "more parameters than registers");
		f->ncode = count(l, 2);
		f->code = ak_alloc(sizeof(uint16_t) * (f->ncode ? f->ncode : 1));
		for (uint32_t c = 0; c < f->ncode; c++) f->code[c] = ak_read_u16(&l->r);
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
	if (l->failed) {
		ak_module_release(m);
		return NULL;
	}
	return m;
}

void ak_module_retain(ak_module *m) { atomic_fetch_add(&m->rc, 1); }

void ak_module_release(ak_module *m) {
	if (atomic_fetch_sub(&m->rc, 1) != 1) return;
	for (uint32_t i = 0; i < m->nglobals; i++) ak_release_val(m->globals[i], (ak_kind)m->gkinds[i]);
	ak_free(m->globals);
	ak_free(m->gkinds);
	for (uint32_t i = 0; i < m->nfuncs; i++) {
		ak_func *f = &m->funcs[i];
		ak_release(f->name);
		ak_release(f->file);
		ak_free(f->kinds);
		ak_free(f->capkinds);
		ak_free(f->code);
		ak_free(f->handlers);
		ak_free(f->lines);
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
	for (uint32_t i = 0; i < m->ntests; i++) ak_release(m->tests[i].name);
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

void ak_module_attach(ak_module *m, ak_val (*const *bodies)(ak_val *args, ak_closure *clos), int (*const *resumes)(struct ak_co *co, ak_val *out), uint32_t n) {
	for (uint32_t i = 0; i < n && i < m->nfuncs; i++) {
		m->funcs[i].native = bodies[i];
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
