// prof.c: the CPU profiler (prof.h).
#include "vm/prof.h"
#include "vm/vm_int.h"
#include "vm/debug.h"
#include "base/thread.h"

#include <stdio.h>
#include <string.h>

int64_t (*ak_prof_clock)(void) = ak_now_ns;

#if AK_VM_DEBUGGER

// The most samples a recording keeps: two minutes at 1 ms.
#define MAX_SAMPLES 120000
#define DEFAULT_INTERVAL 1000000  // 1 ms

// ---- What the sampler thread and the main thread share ----

static _Atomic int section;            // what the main thread does (AK_PROF_*)
static _Atomic int64_t due;            // ns the samples not taken yet stand for
static _Atomic uint32_t gen;           // the sampler running's; another: it ends
static ak_mutex smu = AK_MUTEX_INIT;   // alive, and the sampler's sleep
static ak_cond scv;
static bool scv_ready, alive;
static uint32_t interval;

// ---- The main thread's ----

static bool running;

typedef struct {
	char *name, *file;
	uint32_t nlen, flen, line;
	uint64_t hash;
} pfunc;
typedef struct {
	int32_t parent, func;
} pnode;
typedef struct {
	int64_t t;
	uint32_t node, w;
} psample;

static AK_VEC(pfunc) funcs;
static AK_VEC(pnode) nodes;
static AK_VEC(psample) samples;
static bool truncated;
// The functions by their name and file (their ids + 1; 0: empty): by
// what they say, not their address, which a reload's module may reuse.
static uint32_t *fslots;
static uint32_t fcap;
// The call tree's children by parent and function (node + 1; 0: empty).
static uint64_t *ckeys;
static uint32_t *cvals;
static uint32_t ccap, cused;

static uint64_t mix(uint64_t x) {
	x ^= x >> 33;
	x *= UINT64_C(0xff51afd7ed558ccd);
	x ^= x >> 33;
	x *= UINT64_C(0xc4ceb9fe1a85ec53);
	return x ^ (x >> 33);
}

static uint32_t func_id(const char *name, uint32_t nlen, const char *file, uint32_t flen, uint32_t line) {
	uint64_t h = mix(ak_hash_bytes(name, nlen) ^ (ak_hash_bytes(file, flen) * 31));
	if ((uint64_t)funcs.len * 2 >= fcap) {
		uint32_t nc = fcap ? fcap * 2 : 256;
		uint32_t *ns = ak_alloc(sizeof(uint32_t) * nc);
		for (uint32_t i = 0; i < funcs.len; i++) {
			uint32_t j = (uint32_t)funcs.data[i].hash & (nc - 1);
			while (ns[j]) j = (j + 1) & (nc - 1);
			ns[j] = i + 1;
		}
		ak_free(fslots);
		fslots = ns, fcap = nc;
	}
	uint32_t j = (uint32_t)h & (fcap - 1);
	for (; fslots[j]; j = (j + 1) & (fcap - 1)) {
		const pfunc *f = &funcs.data[fslots[j] - 1];
		if (f->hash == h && f->nlen == nlen && f->flen == flen && memcmp(f->name, name, nlen) == 0 && memcmp(f->file, file, flen) == 0)
			return fslots[j] - 1;
	}
	pfunc f = {ak_alloc(nlen + 1), ak_alloc(flen + 1), nlen, flen, line, h};
	memcpy(f.name, name, nlen);
	memcpy(f.file, file, flen);
	ak_vec_push(&funcs, f);
	fslots[j] = funcs.len;
	return funcs.len - 1;
}

static uint32_t vm_func(const ak_func *f) {
	return func_id(f->name->data, f->name->len, f->file->data, f->file->len, f->nlines ? f->lines[0].line : 0);
}

static const char *const section_names[AK_PROF_NSECTIONS] = {
	"(idle)", "(runtime)", "(props batch)", "(layout)", "(frames batch)", "(layout handlers)",
};

static uint32_t section_func(int s) {
	const char *n = section_names[s];
	return func_id(n, (uint32_t)strlen(n), "", 0, 0);
}

// child is the node of func called from parent.
static uint32_t child(uint32_t parent, uint32_t func) {
	if ((uint64_t)cused * 2 >= ccap) {
		uint32_t nc = ccap ? ccap * 2 : 1024;
		uint64_t *nk = ak_alloc(sizeof(uint64_t) * nc);
		uint32_t *nv = ak_alloc(sizeof(uint32_t) * nc);
		for (uint32_t i = 0; i < ccap; i++) {
			if (!cvals[i]) continue;
			uint32_t j = (uint32_t)mix(ckeys[i]) & (nc - 1);
			while (nv[j]) j = (j + 1) & (nc - 1);
			nk[j] = ckeys[i], nv[j] = cvals[i];
		}
		ak_free(ckeys);
		ak_free(cvals);
		ckeys = nk, cvals = nv, ccap = nc;
	}
	uint64_t key = (uint64_t)parent << 32 | func;
	uint32_t j = (uint32_t)mix(key) & (ccap - 1);
	for (; cvals[j]; j = (j + 1) & (ccap - 1))
		if (ckeys[j] == key) return cvals[j] - 1;
	ak_vec_push(&nodes, ((pnode){(int32_t)parent, (int32_t)func}));
	ckeys[j] = key, cvals[j] = nodes.len;
	cused++;
	return nodes.len - 1;
}

static void add(uint32_t node, int64_t w) {
	if (samples.len >= MAX_SAMPLES) {
		truncated = true;
		return;
	}
	ak_vec_push(&samples, ((psample){ak_prof_clock(), node, w > UINT32_MAX ? UINT32_MAX : (uint32_t)w}));
}

void ak_prof_take(ak_co *co, uint32_t pc) {
	atomic_fetch_and(&ak_dbg_on, ~AK_DBG_ON_PROF);
	int64_t w = atomic_exchange(&due, 0);
	if (!running || w <= 0) return;
	uint32_t node = 0;
	for (uint32_t i = 0; i < co->nframes; i++) {
		const ak_frame *fr = &co->frames[i];
		const ak_func *f = fr->fn;
		if (f->resume) continue;  // compiled to C: no pc of its own
		uint32_t fpc = i == co->nframes - 1 ? pc : (fr->pc ? fr->pc - 1 : 0);
		node = child(node, vm_func(f));
		// The bodies inlined where it is, outermost first (they're listed
		// innermost first).
		const ak_inline *in[32];
		uint32_t n = 0;
		for (uint32_t k = 0; k < f->ninlines && n < AK_LEN(in); k++)
			if (fpc >= f->inlines[k].start && fpc < f->inlines[k].end) in[n++] = &f->inlines[k];
		while (n) node = child(node, vm_func(&f->mod->funcs[in[--n]->func]));
	}
	add(node, w);
}

void ak_prof_section(int s) {
	if (!running) return;
	int old = atomic_exchange_explicit(&section, s, memory_order_relaxed);
	if (old == s) return;
	int64_t w = atomic_exchange(&due, 0);
	if (w > 0 && old != AK_PROF_IDLE) add(child(0, section_func(old)), w);
}

// sampler is the sampler thread's: it ends when gen changes.
static void sampler(void *arg) {
	uint32_t my = (uint32_t)(uintptr_t)arg;
	ak_mutex_lock(&smu);
	int64_t last = ak_now_ns();
	bool was_idle = true;
	while (atomic_load(&gen) == my) {
		ak_cond_wait_ns(&scv, &smu, interval);
		if (atomic_load(&gen) != my) break;
		int64_t now = ak_now_ns(), dt = now - last;
		last = now;
		bool idle = atomic_load_explicit(&section, memory_order_relaxed) == AK_PROF_IDLE;
		// Busy since it last looked, idle then: the time it was idle isn't due.
		if (was_idle && dt > (int64_t)interval) dt = interval;
		was_idle = idle;
		if (idle || dt <= 0) continue;
		atomic_fetch_add(&due, dt);
		atomic_fetch_or(&ak_dbg_on, AK_DBG_ON_PROF);
	}
	alive = false;
	ak_cond_broadcast(&scv);
	ak_mutex_unlock(&smu);
}

static void free_all(void) {
	for (uint32_t i = 0; i < funcs.len; i++) {
		ak_free(funcs.data[i].name);
		ak_free(funcs.data[i].file);
	}
	ak_vec_free(&funcs);
	ak_vec_free(&nodes);
	ak_vec_free(&samples);
	ak_free(fslots);
	ak_free(ckeys);
	ak_free(cvals);
	fslots = NULL, ckeys = NULL, cvals = NULL;
	fcap = ccap = cused = 0;
	truncated = false;
}

bool ak_prof_available(void) { return true; }

void ak_prof_stop(void) {
	if (!running) return;
	running = false;
	ak_mutex_lock(&smu);
	atomic_fetch_add(&gen, 1);
	ak_cond_broadcast(&scv);
	while (alive) ak_cond_wait(&scv, &smu);
	ak_mutex_unlock(&smu);
	atomic_fetch_and(&ak_dbg_on, ~AK_DBG_ON_PROF);
	atomic_store(&due, 0);
}

bool ak_prof_start(uint32_t interval_ns) {
	ak_prof_reset();
	if (!scv_ready) {
		ak_cond_init(&scv);
		scv_ready = true;
	}
	interval = interval_ns ? interval_ns : DEFAULT_INTERVAL;
	ak_vec_push(&nodes, ((pnode){-1, -1}));  // node 0, the root
	atomic_store(&section, AK_PROF_APP);  // started by a frame of asuka dev's, in the app's turn
	ak_mutex_lock(&smu);
	alive = true;
	uint32_t my = atomic_fetch_add(&gen, 1) + 1;
	ak_mutex_unlock(&smu);
	running = true;
	if (!ak_thread_start(sampler, (void *)(uintptr_t)my)) {
		running = false;
		alive = false;
		return false;
	}
	return true;
}

uint32_t ak_prof_samples(void) { return samples.len; }

static void json_str(ak_buf *b, const char *s, size_t n) {
	ak_buf_u8(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') {
			ak_buf_u8(b, '\\');
			ak_buf_u8(b, c);
		} else if (c < 0x20) {
			ak_buf_printf(b, "\\u%04x", c);
		} else {
			ak_buf_u8(b, c);
		}
	}
	ak_buf_u8(b, '"');
}

void ak_prof_write(ak_buf *b, int64_t t0) {
	ak_buf_printf(b, "\"interval\":%u,\"truncated\":%s,\"funcs\":[", interval, truncated ? "true" : "false");
	for (uint32_t i = 0; i < funcs.len; i++) {
		const pfunc *f = &funcs.data[i];
		ak_buf_put(b, i ? ",[" : "[", i ? 2 : 1);
		json_str(b, f->name, f->nlen);
		ak_buf_u8(b, ',');
		json_str(b, f->file, f->flen);
		ak_buf_printf(b, ",%u]", f->line);
	}
	ak_buf_put(b, "],\"nodes\":[", 11);
	for (uint32_t i = 0; i < nodes.len; i++) ak_buf_printf(b, "%s[%d,%d]", i ? "," : "", nodes.data[i].parent, nodes.data[i].func);
	ak_buf_put(b, "],\"samples\":[", 13);
	for (uint32_t i = 0; i < samples.len; i++) {
		const psample *s = &samples.data[i];
		ak_buf_printf(b, "%s[%lld,%u,%u]", i ? "," : "", (long long)(s->t - t0), s->node, s->w);
	}
	ak_buf_u8(b, ']');
}

void ak_prof_reset(void) {
	ak_prof_stop();
	free_all();
}

#else

bool ak_prof_available(void) { return false; }
bool ak_prof_start(uint32_t interval_ns) {
	AK_UNUSED(interval_ns);
	return false;
}
void ak_prof_stop(void) {}
void ak_prof_section(int s) { AK_UNUSED(s); }
uint32_t ak_prof_samples(void) { return 0; }
void ak_prof_write(ak_buf *b, int64_t t0) {
	AK_UNUSED(t0);
	ak_buf_printf(b, "\"interval\":0,\"truncated\":false,\"funcs\":[],\"nodes\":[],\"samples\":[]");
}
void ak_prof_reset(void) {}
void ak_prof_take(struct ak_co *co, uint32_t pc) {
	AK_UNUSED(co);
	AK_UNUSED(pc);
}

#endif
