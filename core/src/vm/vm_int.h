// vm_int.h: the VM's insides, shared by its files.
#ifndef AK_VM_INT_H
#define AK_VM_INT_H

#include "vm/vm.h"
#include <stdatomic.h>
#include "rt/sched.h"
#include "rt/json.h"
#include "vm/opcodes_gen.h"
#include "signal/signal.h"

typedef struct { uint32_t start, end, target; } ak_handler;
typedef struct { uint32_t pc, line; } ak_line;
// A function's body inlined in another's: its code [start, end), in
// func, called at line (bytecode.Inline).
typedef struct { uint32_t start, end, func, line; } ak_inline;
// A name of the source's a function holds in register reg over its pcs
// [from, to) (bytecode.Local), for the debugger (debug.c): via says how
// the register holds the value (AK_LOCAL_*), desc is its type's
// descriptor (rt/json.h's, "d" a duration, "c" a color; "-" none).
typedef struct {
	ak_str *name, *desc;
	uint32_t from, to;
	uint16_t reg;
	uint8_t via;
} ak_local;
enum { AK_LOCAL_VALUE, AK_LOCAL_BOX, AK_LOCAL_SIGNAL };

// A function's body compiled to C (lang/cgen): it takes its arguments
// (borrowed; it retains what it keeps) and the running closure, and
// returns its result (owned), throwing as builtins do.
typedef ak_val (*ak_native_body)(ak_val *args, ak_closure *clos);
// The same body as compiled code calls a closure of it (lang/cgen's
// fN_c): the closure, the thread's stack limit, the arguments (a mut
// fn's receiver moved in and back through args[0]); its result with
// whether it threw, no thread-local read on the way.
typedef ak_ret (*ak_direct_body)(ak_closure *clos, uintptr_t lim, ak_val *args);

struct ak_co;
// A function that may suspend (async, or one that awaits), compiled to C
// (lang/cgen): it runs the coroutine's top frame, its own, from the
// frame's pc (0, or where it left), on registers it moves out of the
// frame as it starts and back in when it leaves the frame waiting. It
// returns how it left (AK_RUN_*): returned (*out its result, owned),
// threw (the error pending; no handler of its took it), called a
// function the run loop runs (its frame pushed above), or suspended. A
// frame it returned or threw from is left for the caller to pop, its
// registers released.
typedef int (*ak_async_body)(struct ak_co *co, ak_val *out);
enum { AK_RUN_RET, AK_RUN_THROW, AK_RUN_PUSHED, AK_RUN_SUSPEND };

struct ak_func {
	ak_module *mod;
	ak_native_body native;  // set when the function was compiled to C
	ak_direct_body direct;  // likewise, as a compiled closure call calls it
	// body is its direct body itself (lang/cgen's fN_: its arguments as C
	// values, the closure, the stack limit but on arm64: ops.h's
	// AK_CG_LIM_P), which a compiled call
	// whose arguments' kinds make sig (ak_cg_sig) calls; sig is 0 when
	// it can't be called so (a mut fn, one that may suspend, one of more
	// than AK_CG_SIG_MAX parameters).
	void (*body)(void);
	uint32_t sig;
	ak_async_body resume;   // likewise, for one that may suspend
	ak_str *name;
	ak_str *file;
	uint16_t nparams;
	uint16_t nregs;
	uint8_t *kinds;     // per register
	uint16_t *refs;     // the reference registers, ascending: what leaving releases
	uint16_t nrefs;
	uint8_t *capkinds;  // per capture
	uint16_t ncap;
	uint8_t ret;        // the result's kind
	uint8_t flags;      // bytecode.FAsync, FThrows
	uint16_t *code;
	uint32_t ncode;
	ak_handler *handlers;
	uint32_t nhandlers;
	ak_line *lines;
	uint32_t nlines;
	ak_inline *inlines;
	uint32_t ninlines;
	ak_local *locals;
	uint32_t nlocals;
	uint32_t index;
	// The debugger's: where its breakpoints are (debug.c), made as it's
	// first run with some set.
	struct ak_dbg_fn *dbg;
};

#define AK_F_ASYNC 1
#define AK_F_THROWS 2
// AK_F_MUT: a mut fn, whose first parameter is inout (bytecode.FMut): a
// call moves it in from the caller's argument register, not retained,
// and the frame moves it back there as it ends, returned or thrown.
#define AK_F_MUT 4
// (bytecode.FSized, 8, is lang/cgen's: the VM doesn't read it.)

typedef struct {
	ak_str *name;
	uint32_t func;
} ak_method;

typedef struct {
	ak_shape s;
	struct ak_module *mod;
	ak_method *methods;
	uint32_t nmethods;
	// hook: the functions of the type's own ==, hash, text, JSON and
	// order (rt.h's AK_HOOK_*, s.hooks saying which), from its methods
	// named #eq, #hash, #show, #json, #fromjson (those two followed by a
	// NUL and the descriptor s.jdesc) and #cmp (its compare).
	uint32_t hook[AK_NHOOKS];
	// Storage the shape's arrays point into.
	uint8_t *kinds;
	const ak_shape **shapes;
	const char **names;
	ak_case *cases;
	const char **case_names;  // made on first use (json_lib.c)
} ak_vm_shape;

struct ak_module {
	_Atomic uint32_t rc;  // the loader's, one per live closure of its code and per task; any thread
	int64_t *ints;
	uint32_t nints;
	double *floats;
	uint32_t nfloats;
	ak_str **strings;
	uint32_t nstrings;
	ak_vm_shape *shapes;
	uint32_t nshapes;
	ak_func *funcs;
	uint32_t nfuncs;
	ak_builtin_fn *builtins;
	uint32_t nbuiltins;
	ak_val *globals;
	uint8_t *gkinds;
	uint32_t nglobals;
	uint32_t init;
	int32_t app;
	int32_t main;  // a program's fn main, -1: none
	struct {
		ak_str *name;
		uint32_t func;
		uint8_t flags;  // 1: a bench
		uint32_t nparams;
		ak_str **params;  // a property test's: name, descriptor, name, ...
	} *tests;
	uint32_t ntests;
	struct { ak_str **data; uint32_t len, cap; } names;  // what shapes' names point into
	// How native calls find the module's types (native_lib.c): kept as
	// long as the module, for callbacks that outlive their call.
	ak_json_types native_types;
	ak_frozen frozen;  // the constants its init froze (rt.freeze), freed with it
};

void ak_module_retain(ak_module *m);
// ak_vm_running_module is the module whose code is running: the VM's
// frame's, else the one compiled into the build (what builtins that
// look up the module's types, JSON's and native calls', ask).
ak_module *ak_vm_running_module(void);
// What decoding by descriptor needs (rt/json.h): the running module's
// types, or m's.
void ak_vm_json_types(ak_json_types *out);
void ak_vm_module_json_types(ak_module *m, ak_json_types *out);

// How deep a coroutine's frames go: past it, a call is a stack overflow.
#define AK_VM_MAX_FRAMES 10000

// A call frame on a coroutine.
typedef struct {
	const ak_func *fn;
	ak_closure *clos;  // retained for the call, or NULL
	uint32_t pc;       // where to resume (in words)
	uint32_t callpc;   // the caller's call instruction, for its handlers
	uint32_t base;     // r0's index in the coroutine's registers
	uint32_t udepth;   // the coroutine's untracks when it was pushed
	uint16_t dst;      // the caller's register for the result
	uint8_t boundary;  // returning from it returns to C
	// A mut fn's (AK_F_MUT): where its first argument goes back as it
	// ends: the coroutine's register at, or (from C) *argp.
	uint8_t inout;     // 0: none, AK_INOUT_REG or AK_INOUT_PTR
	uint32_t at;
	ak_val *argp;
} ak_frame;

#define AK_INOUT_REG 1
#define AK_INOUT_PTR 2

typedef struct ak_co {
	ak_val *regs;
	uint32_t top, cap;
	ak_frame *frames;
	uint32_t nframes, fcap;
	// What UNTRACK saved (ak_untrack_begin's tokens), for RETRACK, and for
	// an error unwinding a frame inside a peek: it ends what the frame
	// began (ak_vm_retrack).
	void **untracks;
	uint32_t nuntracks, ucap;
	struct ak_task *task;  // NULL for the main coroutine
	// What compiled async functions' calls read (ops.h): the C stack's
	// limit on the thread it runs on (ak_stack_limit, kept for a run, as
	// ak_vm_run starts it again: a thread-local's read is a call on
	// Apple's systems, and a task's runs may be on different threads),
	// and how deep they call each other directly in C (ak_op_acall).
	uintptr_t lim;
	uint32_t adepth;
	// The thread's pending error's slot (ak_error_slot), which the
	// interpreter reads after each call: set as lim is, for a run, and
	// once for a thread's main coroutine (call_sync).
	void *const *err;
} ak_co;

// ak_vm_untrack_begin stops tracking reads (UNTRACK), saving the running
// computation on co's stack; ak_vm_retrack tracks again what the stack
// held at depth (RETRACK; a frame's udepth as an error unwinds it).
void ak_vm_untrack_begin(ak_co *co);
static inline void ak_vm_retrack(ak_co *co, uint32_t depth) {
	while (co->nuntracks > depth) ak_untrack_end(co->untracks[--co->nuntracks]);
}

// A task: an async function running on its own coroutine, and the
// future of its result.
struct ak_task {
	ak_future *fut;     // retained
	ak_co co;
	ak_future *awaiting;  // retained while suspended
	ak_owner *owner;      // its own owner, under the one that started it
	ak_module *mod;       // retained while it runs
	bool running, cancelled;
};

extern AK_TLS ak_co *ak_vm_cur;  // the coroutine running now (NULL: the main one)
extern AK_TLS ak_co ak_vm_main;

// ak_vm_run runs co from its top frame until a boundary frame returns
// (its result in *out; an error pending if it threw) or the coroutine
// suspends (true): the run loop (run.c). Frames of functions compiled to
// C run their bodies; interpreted frames run on the interpreter.
bool ak_vm_run(ak_co *co, ak_val *out);
// ak_vm_run_here is ak_vm_run of the coroutine running now on this
// thread (ak_vm_cur's: a call from C nested in a run of it, or the
// thread's main one), whose lim and err are this thread's already.
bool ak_vm_run_here(ak_co *co, ak_val *out);
// ak_vm_interp interprets co from its top frame, an interpreted
// function's, until a boundary frame returns (AK_RUN_RET, as
// ak_vm_run's), the coroutine suspends (AK_RUN_SUSPEND), or the top frame
// is a compiled function's (AK_RUN_PUSHED: called, returned to with its
// result stored, or unwound to with the error pending), which ak_vm_run
// runs.
int ak_vm_interp(ak_co *co, ak_val *out);
// ak_vm_pop pops the top frame, releasing its registers (but the one at
// keep, moved out: -1 for none) and its closure.
void ak_vm_pop(ak_co *co, int keep);
// ak_vm_find_handler is the target of f's innermost handler covering
// pc, or -1.
int64_t ak_vm_find_handler(const ak_func *f, uint32_t pc);
// ak_vm_push pushes a frame calling f with args; false (an error thrown)
// on stack overflow.
bool ak_vm_push(ak_co *co, const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs, uint16_t dst, bool boundary, uint32_t callpc);
ak_val ak_vm_closure_invoke(ak_closure *c, ak_val *args, uint32_t nargs);

// ak_vm_pop_frame pops the top frame of a function compiled to C, whose
// registers its body released or moved out: it releases its closure.
static inline void ak_vm_pop_frame(ak_co *co) {
	ak_frame *fr = &co->frames[co->nframes - 1];
	ak_closure *c = fr->clos;
	co->top = fr->base;
	co->nframes--;
	if (c) ak_release(c);
}
// ak_vm_await is what AWAIT does before it takes f's result, for the
// coroutine's top frame: -1 when it threw (the task was cancelled), 1
// when f is pending and the coroutine suspends (the task resumes once f
// settles: AWAIT runs again), 0 when f is settled.
int ak_vm_await(ak_co *co, ak_future *f);
// ak_vm_yield queues the coroutine's task to resume (YIELD), as it
// suspends.
void ak_vm_yield(ak_co *co);
// How deep compiled async functions call each other directly, in C, on
// a coroutine (ak_co.adepth): past a bound, or with the C stack low, a
// call goes through the run loop instead, so that async recursion takes
// no C stack.
#define AK_VM_ADEPTH_MAX 64
// ak_vm_task_start starts f as a task and returns its future (owned).
ak_future *ak_vm_task_start(const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs);
// ak_vm_task_start_in is ak_vm_task_start with the task held by holder
// (disposing it cancels the task), its contexts and catcher the current
// owner's (ak_owner_new_in).
ak_future *ak_vm_task_start_in(const ak_func *f, ak_closure *clos, ak_val *args, uint32_t nargs, ak_owner *holder);
void ak_vm_task_resume(void *task);

// The reactive glue (react.c): cells, memos, accessors, effects, owners.
ak_val ak_vm_cell_new(ak_val v, ak_kind k, ak_str *keep_key);
// ak_vm_follow_new makes a follow (rfcs/0012): a cell whose value the
// closure update computes, following what it reads (FOLLOWK); self: it
// takes the state's current value, an optional. Read and written as a
// cell is.
ak_val ak_vm_follow_new(ak_closure *update, ak_kind k, ak_str *keep_key, bool self);
ak_val ak_vm_cell_get(void *cell, bool track);
void ak_vm_cell_set(void *cell, ak_val v);         // takes v
ak_val ak_vm_cell_take(void *cell);
void ak_vm_cell_put(void *cell, ak_val v);         // takes v
// ak_vm_cell_path reads the value at path p (n bytes, args its
// arguments: vm/path.h) below the cell's (CPATH), or with len its length
// (CLEN): tracked while a computation is, following the path. It throws
// as the read would (an index out of range).
ak_val ak_vm_cell_path(void *cell, const uint8_t *p, uint32_t n, const ak_val *args, bool len);
// ak_vm_cell_putp puts v (taken) back in the cell after a change in
// place at the place path p says, and tells the readers of what changed
// (CPUTP; vm/path.h's ak_path_put).
void ak_vm_cell_putp(void *cell, ak_val v, const uint8_t *p, uint32_t n, const ak_val *args);
// ak_vm_cell_keys subscribes the running computation to the keys at the
// field path fields (nf of them) of the items of the array at path p
// below the cell: a for's (vm/path.h's ak_path_keys).
void ak_vm_cell_keys(void *cell, const uint8_t *p, uint32_t n, const ak_val *args, const uint16_t *fields, uint16_t nf);
// ak_vm_cell_nodes counts the path nodes below a cell (tests).
uint32_t ak_vm_cell_nodes(void *cell);
ak_val ak_vm_memo_new(ak_closure *c, ak_kind k);
ak_val ak_vm_memo_get(void *memo);
ak_val ak_vm_acc_value(ak_val v, ak_kind k);
ak_val ak_vm_acc_func(ak_closure *c, ak_kind k);
ak_val ak_vm_acc_cell(void *cell_or_memo);
ak_val ak_vm_model_new(void *inner, bool bound);
bool ak_vm_model_bound(void *model);
void ak_vm_model_set(void *model, ak_val v);
ak_val ak_vm_acc_get(void *acc);
// ak_vm_acc_path makes an accessor of the path p (args its arguments)
// below base: a cell, or an accessor, whose path it continues when it's
// one's (ACCP). k is its value's kind.
ak_val ak_vm_acc_path(void *base, const uint8_t *p, uint32_t n, const ak_val *args, ak_kind k);
// ak_vm_acc_row is the accessor of a for's row's item: base's element
// at the index the cell index holds (a row's, which changes as it moves),
// made by eff (its for's effect). ak_vm_acc_row_set gives it its element
// as the for places it: what an untracked read reads while its place may
// not be its item's (its for, or one it's in, has yet to run since their
// items changed), and once ak_vm_acc_row_gone says the row went.
ak_val ak_vm_acc_row(void *base, void *index, ak_kind k, ak_effect *eff);
void ak_vm_acc_row_set(void *row, ak_val item, ak_kind ik);
void ak_vm_acc_row_gone(void *row);
// ak_vm_acc_getp reads the path p below what accessor acc reads
// (AGETP): following that path when acc's is one (a cell's, a path's),
// else acc's value whole and the path in it.
ak_val ak_vm_acc_getp(void *acc, const uint8_t *p, uint32_t n, const ak_val *args);
// ak_vm_acc_keys follows the keys of the array acc reads, at the field
// path fields of its items (a for's key t.id): one signal for them all.
void ak_vm_acc_keys(void *acc, const uint16_t *fields, uint16_t nf);
// ak_vm_acc_seek moves an accessor of an array's element (made by
// ak_vm_acc_path with one index step last) to the element at i.
void ak_vm_acc_seek(void *acc, int64_t i);
ak_val ak_vm_call_untracked(ak_closure *c, ak_val *args, uint32_t nargs);
// ak_vm_dbg_peek reads what a state's cell, a let's memo or an accessor
// holds now (borrowed), running nothing and following nothing: false
// when it can't say without running code (a memo not computed yet, an
// accessor that calls a function or reads a path). The debugger's.
bool ak_vm_dbg_peek(void *p, ak_val *v, ak_kind *k);
void ak_vm_effect(ak_closure *c, bool render);
void ak_vm_cleanup(ak_closure *c);
void ak_vm_batch(bool begin);
// ak_vm_method finds an interface method of a struct's or an enum's
// shape (NULL: none).
const ak_func *ak_vm_method(void *obj, ak_str *name);
void ak_vm_react_init(void);
void ak_vm_json_init(void);
void ak_vm_task_own(ak_task *t, ak_owner *holder);  // ties a task to holder (NULL: the current owner)
bool ak_vm_task_run(ak_task *t, ak_val *out);  // ak_vm_run, under the task's owner
// ak_vm_task_done lets the task's owner go; err (owned, or NULL) is an
// error nobody can see through the task's future, raised to the owner's
// catcher.
void ak_vm_task_done(ak_task *t, void *err);
// ak_vm_task_report_unseen: the task failed, and f, its future, is held
// by someone who hasn't looked for its outcome: if they drop it without
// looking, its error goes to the catcher of the task's owner then.
void ak_vm_task_report_unseen(ak_task *t, ak_future *f);

#endif
