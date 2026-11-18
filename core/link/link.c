// link.c: what a build links in beside the core, compiled for each link
// with its defines. The core's own objects are the same for every app (a
// prebuilt archive, or one built once into the build cache); what differs
// from build to build is named here (vm.h's ak_link_app, ak_link_ffi):
//
//   AK_APP_COMPILED  an app is compiled in (lang/cgen's asuka_app_module);
//                    without, the build is a player, which loads bytecode
//   AK_FFI           the glue of the app's extern declarations (lang/cffi's
//                    asuka_ffi_register)
//   AK_STD_SQLITE    std/sqlite's C (its asuka.toml's [c]: SQLite and asq,
//                    asq_call), which core/src/sqlite calls: an app that
//                    imports std/sqlite links it, one that doesn't doesn't
//   AK_USES_<NAME>   the app calls builtins named <name>.… (fs.join: FS),
//                    so the std packages' table of them is registered;
//                    AK_USES_ALL, or no app (a player, which loads any
//                    bytecode): every table. A table no build names is no
//                    root of the link, and the linker leaves its C out
//                    (std/compress's DEFLATE, std/toml's parser, ...)
//
// Naming them keeps their objects in a static library's link.
#include <stddef.h>
#include <stdint.h>

#include "std/std.h"

typedef struct ak_module ak_module;

#if defined(AK_APP_COMPILED)
ak_module *asuka_app_module(void);
ak_module *(*const ak_link_app)(void) = asuka_app_module;
#else
ak_module *(*const ak_link_app)(void) = NULL;
#endif

#if defined(AK_FFI)
void asuka_ffi_register(void);
void (*const ak_link_ffi)(void) = asuka_ffi_register;
#else
void (*const ak_link_ffi)(void) = NULL;
#endif

#if defined(AK_STD_SQLITE)
intptr_t asq_call(uintptr_t op, uintptr_t conn, uintptr_t arg, const uint8_t *in, uintptr_t inLen, uint8_t *out, uintptr_t outCap);
intptr_t (*const ak_link_sqlite)(uintptr_t, uintptr_t, uintptr_t, const uint8_t *, uintptr_t, uint8_t *, uintptr_t) = asq_call;
#else
intptr_t (*const ak_link_sqlite)(uintptr_t, uintptr_t, uintptr_t, const uint8_t *, uintptr_t, uint8_t *, uintptr_t) = NULL;
#endif


// ---- The std packages' tables (std/std.h's ak_std_libs) ----

#if !defined(AK_APP_COMPILED) || defined(AK_USES_ALL)
#define AK_USES_EVERY 1
#endif

const ak_lib_entry *const ak_std_libs[] = {
#if defined(AK_USES_EVERY) || defined(AK_USES_TIME)
	ak_std_time,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_URL)
	ak_std_url,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_REGEX)
	ak_std_regex,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_ENCODING)
	ak_std_encoding,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_CRYPTO)
	ak_std_crypto,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_HTML)
	ak_std_html,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_RANDOM)
	ak_std_random,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_FS)
	ak_std_fs,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_CRASH)
	ak_std_crash,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_UPDATE)
	ak_std_update,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_MARKDOWN)
	ak_std_markdown,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_OS)
	ak_std_os,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_FLAG)
	ak_std_flag,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_COMMAND)
	ak_std_command,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_TERM)
	ak_std_term,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_OS)
	ak_std_signals,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_TOML)
	ak_std_toml,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_CSV)
	ak_std_csv,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_COMPRESS)
	ak_std_compress,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_ARCHIVE)
	ak_std_archive,
#endif
#if defined(AK_USES_EVERY) || defined(AK_USES_HTTP)
	ak_std_http,
#endif
	NULL,
};
