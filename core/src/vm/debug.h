// debug.h: the VM's debugger, asuka dev's: breakpoints by file and line,
// stepping by line and frame (over, in, out), pausing, and on each stop
// the stack (each frame's function, file and line, innermost first) with
// each frame's locals (bytecode.Local: the names lang/ir records in code
// it doesn't optimize), written as JSON for the CLI (abi/dev.h's debug
// frames carry it).
//
// The app's code runs on the main thread, so a stop pauses the main
// thread, as a native debugger's breakpoint does: the UI doesn't draw
// until it goes on. While it's paused, the dev connection's reader thread
// hands it the CLI's commands directly (ak_dbg_command,
// ak_dbg_variables: a condition variable the paused thread waits on),
// not through a post, which the paused thread wouldn't run.
//
// It's compiled into development builds only (AK_DEBUG: a player's core;
// and the tests'), and not for the web, whose page can't wait on the
// main thread for a command. A release build's interpreter has no hook.
// In a development build, the interpreter runs a copy of its loop that
// calls the hook before each instruction while ak_dbg_on is set
// (breakpoints, a step, a pause), and its own otherwise, which asks the
// flag only as a frame is entered or left and a jump is taken
// (interp.c): nothing per instruction.
//
// Breakpoints are kept by file and line and resolved against each
// function's line table as it first runs with them (the lines of the
// bodies inlined in it are their own files'), so they hold across
// modules: a reload's functions resolve them again as they run. One may
// have a condition, a log message (a logpoint, which logs and goes on)
// and a hit condition (it stops at its nth hit, ...): the CLI compiles
// the condition and the message for each function the line is in, as
// functions of the module's extension (vm_int.h's ak_module_extend: what
// the debugger runs of the CLI's against the module), which the engine
// calls with the frame's locals as the line's reached, without
// stopping, none of the app's breakpoints or errors stopping it
// meanwhile.
//
// While paused, the CLI may evaluate an expression in a frame of the
// stop's (an editor's watch, hover, console), compiled likewise: it runs
// on the paused main thread, its locals the frame's (what a state holds
// now, read without following it), its side effects the app's; its
// value is answered as a local's is, its children asked for as theirs.
//
// Errors: with the filters set (ak_dbg_exceptions), it stops where an
// error is thrown, before it unwinds the stack: every one (all), or one
// that nothing the app has catches (uncaught: no catch around it in the
// frames it would unwind, through the calls of builtins it passes, nor a
// Boundary around the effect or the handler it ends; a task's, nor
// someone awaiting it; a defer's handler, which throws it on, doesn't
// catch it), once an error.
//
// What it shows of a value: what it holds now, read without running any
// of the app's code (a type's own description isn't called; a state's
// cell, a let's memo and a parameter's accessor show the value they hold,
// as last computed), as the language writes it ("Todo(title: \"Milk\",
// done: false)", [1, 2], .circle(radius: 2), none), cut short past a
// length and a depth; a struct's, an enum's, an array's, a map's and a
// set's parts are its children, which ak_dbg_variables lists.
#ifndef AK_VM_DEBUG_H
#define AK_VM_DEBUG_H

#include "vm/vm.h"

#if (defined(AK_DEBUG) || defined(AK_TEST)) && !defined(__wasm__)
#define AK_VM_DEBUGGER 1
#else
#define AK_VM_DEBUGGER 0
#endif

// What a frame's index means in ak_dbg_variables when this bit's set:
// the value of an evaluation of the stop's (ak_dbg_evaluate's answer's
// "ref"), its path's first index 0.
#define AK_DBG_RESULT_FRAME 0x80000000u
// No function (ak_dbg_code's cond and log).
#define AK_DBG_NONE 0xFFFFFFFFu

// Commands (dev.h's debug-command frame's byte).
enum {
	AK_DBG_CONTINUE = 1,
	AK_DBG_NEXT = 2,      // step over: the next line of this frame, or its caller's
	AK_DBG_STEP_IN = 3,   // the next line run, in whichever function
	AK_DBG_STEP_OUT = 4,  // the caller's next instruction, once this frame returned
	AK_DBG_PAUSE = 5,     // stop at the next instruction the app runs
};

// What the debugger tells the CLI (its sink's event).
enum {
	// Stopped: JSON {"reason": "breakpoint" | "step" | "pause",
	// "frames": [frame...], "totalFrames": n}; a frame is {"name", "file",
	// "line", "locals": [variable...]}; a variable is {"name", "type",
	// "value", "children": n} ("children" left out when it has none).
	AK_DBG_STOPPED = 1,
	// Continued: the app runs again (no JSON).
	AK_DBG_CONTINUED = 2,
	// Variables: a request's answer, under its number: JSON
	// {"variables": [variable...], "total": n}, or {"error": "..."}.
	AK_DBG_VARIABLES = 3,
	// Result: an evaluation's answer, under its number: JSON {"result":
	// variable, "ref": n} (ref: its children's frame, with
	// AK_DBG_RESULT_FRAME, while the stop lasts), or {"error": "..."}.
	AK_DBG_RESULT = 4,
	// Log: a logpoint's message, or why a breakpoint's code failed (it
	// stops then): JSON {"file", "line", "text"} or {"file", "line",
	// "error"}.
	AK_DBG_LOG = 5,
};

// Error filters (ak_dbg_exceptions; dev.h's debug-exceptions byte).
enum {
	AK_DBG_EXC_UNCAUGHT = 1,  // an error nothing in the app catches
	AK_DBG_EXC_ALL = 2,       // every error thrown
};

// Hit conditions: a breakpoint (whose condition, if any, holds) stops
// at the hits whose count (from 1) is ==, >=, >, <=, < hit_n, or a
// multiple of it.
enum { AK_DBG_HIT_ANY, AK_DBG_HIT_EQ, AK_DBG_HIT_GE, AK_DBG_HIT_GT, AK_DBG_HIT_LE, AK_DBG_HIT_LT, AK_DBG_HIT_MOD };

// A breakpoint's code for one function its line is in: its condition
// and its log message, compiled by the CLI as functions of the module's
// extension (ak_dbg_extend), taking the frame's locals params (indexes
// in the function's locals).
typedef struct {
	uint64_t base;       // the module's hash (lang/bytecode's Hash)
	uint32_t func;       // the module's function
	uint32_t cond, log;  // the extension's functions: a bool, a string; AK_DBG_NONE: none
	uint32_t nparams;
	const uint32_t *params;
	const char *error;   // why it didn't compile (NULL: it did); it stops, logging why
	size_t elen;
} ak_dbg_code;

// Breakpoint flags.
enum { AK_DBG_BP_COND = 1, AK_DBG_BP_LOG = 2 };

// A breakpoint: its line, with a condition, a log message (its code,
// for each function the line's in: those of another module than the
// one running are passed over, the line not stopping), and a hit
// condition. key is what its hit count goes with: set again, a line
// with the same key keeps its count.
typedef struct {
	uint32_t line, key;
	uint8_t flags, hit_op;
	uint32_t hit_n;
	uint32_t ncode;
	const ak_dbg_code *code;
} ak_dbg_bp;

// ak_dbg_sink is where the debugger's events go (dev.c's: frames to the
// CLI). STOPPED and CONTINUED are sent on the main thread; VARIABLES on
// the main thread while it's paused, else on the requester's.
typedef void (*ak_dbg_sink)(int event, uint32_t seq, const char *json, size_t len);

// ak_dbg_available reports whether this build has the debugger.
bool ak_dbg_available(void);
// ak_dbg_set_sink sets the sink; NULL: none, and the app never stops
// (there's no one to tell). Any thread.
void ak_dbg_set_sink(ak_dbg_sink sink);
// ak_dbg_set_breakpoints replaces file's breakpoints (file as the
// module's functions name it: bytecode.FuncDef's File) with bps (n:
// none), which it copies. Any thread.
void ak_dbg_set_breakpoints(const char *file, size_t flen, const ak_dbg_bp *bps, uint32_t n);
// ak_dbg_extend keeps an extension of a module (lang/bytecode's
// Extension, which names the module by its hash): the functions of the
// CLI's that breakpoints and evaluations name. One that has more of the
// same module's replaces the one kept. Any thread.
void ak_dbg_extend(const uint8_t *ext, size_t n);
// ak_dbg_exceptions sets which errors stop the app (AK_DBG_EXC_*; 0:
// none). Any thread.
void ak_dbg_exceptions(int filters);
// ak_dbg_evaluate evaluates function func of the extension of module
// base in a frame of the stop's (an index in its frames), its arguments
// the frame's locals params: answered through the sink (AK_DBG_RESULT,
// seq) by the paused main thread, its value written by its descriptor
// desc, or at once with an error when it isn't paused. Any thread.
void ak_dbg_evaluate(uint32_t seq, uint32_t frame, uint64_t base, uint32_t func, const uint32_t *params, uint32_t np, const char *desc, size_t dlen);
// ak_dbg_forget_extensions lets go of the extensions built (the
// module they extend is going: a reload). Main thread.
void ak_dbg_forget_extensions(void);
// ak_dbg_command runs a command: continue or a step goes to the paused
// main thread, and is dropped when it isn't paused; pause stops the app
// at its next instruction (on the main thread). Any thread.
void ak_dbg_command(int cmd);
// ak_dbg_variables asks for the children of a value of the stop's
// (frame: an index in the stop's frames; path: the index of a local in
// the frame's locals, then of a child at each level, npath >= 1), from
// start, count of them (0: up to 1000): answered through the sink
// (AK_DBG_VARIABLES, seq) by the paused main thread, or at once with an
// error when it isn't paused. Any thread.
void ak_dbg_variables(uint32_t seq, uint32_t frame, const uint32_t *path, uint32_t npath, uint32_t start, uint32_t count);
// ak_dbg_hold(true) keeps the app from stopping, and lets it go on if
// it's paused, until ak_dbg_hold(false): a reload on its way to the main
// thread (dev.c), which runs once the paused code returned. Any thread.
void ak_dbg_hold(bool hold);
// ak_dbg_paused reports whether the main thread is paused at a stop.
bool ak_dbg_paused(void);
// ak_dbg_reset drops every breakpoint, the step and the pause, and lets
// a paused app go on: the CLI went. Any thread.
void ak_dbg_reset(void);

#if AK_VM_DEBUGGER
#include <stdatomic.h>
// ak_dbg_on is nonzero while the interpreter calls ak_dbg_hook before
// each instruction: its bits say who wants it. AK_DBG_ON_DEBUG while
// breakpoints are set, a step runs or a pause is asked; AK_DBG_ON_PROF
// while the profiler's sampler wants a sample of the main thread's stack
// (vm/prof.h), which the hook takes and clears.
extern _Atomic int ak_dbg_on;
enum { AK_DBG_ON_DEBUG = 1, AK_DBG_ON_PROF = 2 };
struct ak_co;
// ak_dbg_hook is called before the instruction at pc of co's top frame
// runs (an interpreted function's), when ak_dbg_on: it may stop there,
// and returns once the app goes on. It runs none of the app's code.
void ak_dbg_hook(struct ak_co *co, uint32_t pc);
// ak_dbg_exc is nonzero while errors stop the app (the filters set):
// the interpreter calls ak_dbg_thrown as an error is about to unwind a
// frame.
extern _Atomic int ak_dbg_exc;
// ak_dbg_thrown is called as the error pending is about to unwind co's
// top frame at pc (thrown there, or by what it called; skip_top: that
// frame's handlers don't take it), when ak_dbg_exc: it may stop there.
// It runs none of the app's code but an error's message.
void ak_dbg_thrown(struct ak_co *co, uint32_t pc, bool skip_top);
// ak_vm_dbg_forget lets go of f's breakpoints (its module is freed).
void ak_vm_dbg_forget(ak_func *f);
#else
static inline void ak_vm_dbg_forget(ak_func *f) { (void)f; }
#endif

#endif
