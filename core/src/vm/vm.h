// vm.h: the bytecode VM. It loads a module the compiler wrote
// (lang/bytecode) and runs its functions on the main thread.
//
// Calls. ak_vm_call runs a function to completion and returns its result
// (owned); an error it throws is left pending (ak_error_pending). An
// async function called from C runs on a coroutine of its own: if it
// suspends (an await on something not done yet), ak_vm_call returns at
// once with a none result and the coroutine becomes a task that resumes
// when what it awaits completes.
//
// Closures the VM makes are ordinary rt closures: C code (the UI
// runtime) calls them with ak_call.
#ifndef AK_VM_H
#define AK_VM_H

#include "rt/rt.h"

typedef struct ak_module ak_module;
typedef struct ak_func ak_func;

// A builtin: a C function bytecode calls by name (CALLB). It borrows its
// arguments and returns an owned result; it throws with ak_throw.
typedef ak_val (*ak_builtin_fn)(ak_val *args, uint32_t nargs);

// ak_builtin_register makes fn callable by name. Builtins are registered
// once, before modules load (ak_vm_builtins_init registers the core's).
void ak_builtin_register(const char *name, ak_builtin_fn fn);
ak_builtin_fn ak_builtin_find(const char *name);
void ak_vm_builtins_init(void);
// ak_vm_init sets the VM up: its builtins, and the signal graph's hold
// on its errors. Once, on the main thread, before loading a module.
void ak_vm_init(void);

// ak_module_load reads a module. On failure it returns NULL with a
// message in err.
ak_module *ak_module_load(const uint8_t *p, size_t n, char *err, size_t errcap);
// ak_module_release drops the loader's reference; the module is freed
// when the last closure of its code goes too.
void ak_module_release(ak_module *m);
void ak_module_retain(ak_module *m);

uint32_t ak_module_nfuncs(const ak_module *m);
const ak_func *ak_module_func(const ak_module *m, uint32_t i);
const ak_func *ak_module_find(const ak_module *m, const char *name);
uint32_t ak_module_init(const ak_module *m);
int32_t ak_module_app(const ak_module *m);
// ak_module_main is a program's fn main (docs/programs.md), -1 for an
// app or a library.
int32_t ak_module_main(const ak_module *m);
uint32_t ak_module_ntests(const ak_module *m);
const char *ak_module_test(const ak_module *m, uint32_t i, uint32_t *func);
const ak_shape *ak_module_shape(const ak_module *m, uint32_t i);
const ak_shape *ak_module_shape_named(const ak_module *m, const char *name);
const char *ak_func_name(const ak_func *f);
bool ak_func_async(const ak_func *f);
// ak_closure_async reports whether calling c (ak_call) starts a task and
// returns its future, rather than running it to its end.
bool ak_closure_async(const ak_closure *c);
// ak_vm_error_text is an error's message as Asuka's e.message reads it:
// a user error's message method's (its field, its fn message(), its
// type's and case's name), a runtime error's message. Owned.
ak_str *ak_vm_error_text(void *err);

struct ak_co;
// ak_module_attach gives a module's functions their bodies compiled to
// C (lang/cgen): bodies[i] is function i's, a synchronous one's, and
// resumes[i] one's that may suspend (ak_async_body, vm_int.h), or both
// NULL to interpret it.
void ak_module_attach(ak_module *m, ak_val (*const *bodies)(ak_val *args, ak_closure *clos), int (*const *resumes)(struct ak_co *co, ak_val *out), uint32_t n);

// ak_app_loader loads the app compiled into this build (lang/cgen's C
// sets it as it starts); NULL when none is (a development build, which
// gets bytecode).
extern ak_module *(*ak_app_loader)(void);

// What the build links in beside the core (core/link/link.c, compiled for
// each link: the core itself is the same for every app, a prebuilt
// archive): the app compiled into it (lang/cgen's asuka_app_module; NULL
// for a player, which loads bytecode) and its externs' glue (lang/cffi's
// asuka_ffi_register; NULL without), and std/sqlite's SQLite when the
// app imports std/sqlite (its asq_call, std/sqlite/csrc/asq.h; NULL
// without). Named here, so that a static library's linker keeps their
// objects.
extern ak_module *(*const ak_link_app)(void);
extern void (*const ak_link_ffi)(void);
extern intptr_t (*const ak_link_sqlite)(uintptr_t op, uintptr_t conn, uintptr_t arg, const uint8_t *in, uintptr_t inLen, uint8_t *out, uintptr_t outCap);

// ak_vm_call calls f with its arguments (borrowed).
ak_val ak_vm_call(const ak_func *f, ak_val *args, uint32_t nargs);

// ak_vm_closure makes a closure of f with no captures.
ak_closure *ak_vm_closure(const ak_func *f);

// The tasks async code runs on.
typedef struct ak_task ak_task;

// ak_vm_run_ready resumes every task whose await completed, until none
// is left. The UI runtime calls it after each event and posted callback.
void ak_vm_run_ready(void);
bool ak_vm_has_ready(void);

// ak_vm_traceback writes the VM's call stack (innermost first, a line per
// frame: function, file:line) into buf. For error reports.
void ak_vm_traceback(char *buf, size_t cap);

#endif
