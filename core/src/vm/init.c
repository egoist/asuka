#include "vm/vm_int.h"
#include "rt/lib.h"
#include "std/std.h"
#include "vm/ffi.h"
#include "base/thread.h"


extern const ak_lib_entry ak_lib_spawn;
extern const ak_lib_entry ak_worker_lib[];
extern const ak_lib_entry ak_live_lib;

void ak_vm_builtins_init(void) {
	for (const ak_lib_entry *e = ak_lib; e->name; e++) ak_builtin_register(e->name, e->fn);
	for (const ak_lib_entry *e = ak_stream_lib; e->name; e++) ak_builtin_register(e->name, e->fn);
	for (const ak_lib_entry *e = ak_int_lib; e->name; e++) ak_builtin_register(e->name, e->fn);
	for (const ak_lib_entry *const *t = ak_std_libs; *t; t++)
		for (const ak_lib_entry *e = *t; e->name; e++) ak_builtin_register(e->name, e->fn);
	ak_builtin_register(ak_lib_spawn.name, ak_lib_spawn.fn);
	for (const ak_lib_entry *e = ak_worker_lib; e->name; e++) ak_builtin_register(e->name, e->fn);
	ak_builtin_register(ak_live_lib.name, ak_live_lib.fn);
	extern void ak_vm_freeze_init(void);
	ak_vm_freeze_init();
	// The glue lang/cffi generates for the app's extern declarations (RFC
	// 0008), when the build has one (core/link: ak_link_ffi).
	if (ak_link_ffi) ak_link_ffi();
}

// A closure spawn copied for a worker holds the module too.
void ak_closure_copied(ak_closure *from, ak_closure *to) {
	AK_UNUSED(to);
	if (from->invoke == ak_vm_closure_invoke) ak_module_retain(((const ak_func *)from->fn)->mod);
}

void ak_vm_init(void) {
	static bool done;
	if (done) return;
	done = true;
	ak_ffi_init();
	ak_vm_builtins_init();
	ak_vm_json_init();
	extern void ak_vm_literal_init(void);
	ak_vm_literal_init();
	extern void ak_vm_bench_init(void);
	ak_vm_bench_init();
	extern void ak_vm_each_init(void);
	ak_vm_each_init();
	extern void ak_native_vm_init(void);
	ak_native_vm_init();
	extern void ak_sqlite_init(void);
	ak_sqlite_init();
	ak_vm_react_init();
	extern void ak_vm_thread_end(void);
	ak_thread_end = ak_vm_thread_end;
}
