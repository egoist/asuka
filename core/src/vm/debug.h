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
// modules: a reload's functions resolve them again as they run.
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
};

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
// module's functions name it: bytecode.FuncDef's File) with lines (n:
// none). Any thread.
void ak_dbg_set_breakpoints(const char *file, size_t flen, const uint32_t *lines, uint32_t n);
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
// each instruction: breakpoints are set, a step runs, a pause is asked.
extern _Atomic int ak_dbg_on;
struct ak_co;
// ak_dbg_hook is called before the instruction at pc of co's top frame
// runs (an interpreted function's), when ak_dbg_on: it may stop there,
// and returns once the app goes on. It runs none of the app's code.
void ak_dbg_hook(struct ak_co *co, uint32_t pc);
// ak_vm_dbg_forget lets go of f's breakpoints (its module is freed).
void ak_vm_dbg_forget(ak_func *f);
#else
static inline void ak_vm_dbg_forget(ak_func *f) { (void)f; }
#endif

#endif
