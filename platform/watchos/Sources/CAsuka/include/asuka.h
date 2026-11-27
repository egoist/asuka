// The C ABI of Asuka's core: what a host (Swift, Kotlin, JavaScript
// through WebAssembly, the desktop hosts in Go through purego) calls, and
// the callbacks it gives the core. Implemented by core/src/abi/abi.c. Every call comes from
// the main thread, unless it says otherwise.
//
// Starting: asuka_start(callbacks), with the host's environment reported
// (asuka_event, an Environment event for root 0) before or after it; then
// the app: asuka_load(module) for bytecode (the dev player, code push), or
// asuka_run(app_main) for an app compiled to C (lang/cgen's release
// builds, which generate app_main).
#ifndef ASUKA_H
#define ASUKA_H

#include <stdint.h>
#include <stddef.h>

// ASUKA_API marks what the core's library exports (its other symbols are
// hidden).
#if defined(_WIN32) && defined(AK_EXPORTS)
#define ASUKA_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define ASUKA_API __attribute__((visibility("default")))
#else
#define ASUKA_API
#endif

typedef struct {
	// Applies a batch of encoded ops (ops/ops.go). The buffer is valid
	// during the call only.
	void (*apply)(const uint8_t *buf, size_t len);
	// Measures view id: width/height are known sizes or NaN; avail* are
	// the space available: a definite size, +inf (max-content) or -inf
	// (min-content).
	void (*measure)(uint32_t id, float width, float height, float availWidth, float availHeight, float *outWidth, float *outHeight);
	// Asks for asuka_run_posted to be called soon on the main thread. Any
	// thread.
	void (*post)(void);
	// Typed native modules (package native's Backend), from any thread:
	// make an object, call its method (answers into out, cap bytes, else
	// into *big, which native_free frees; the answer's length returned),
	// start a method that takes a context (asuka_native_done answers it),
	// cancel it, let an object go.
	size_t (*native_new)(const uint8_t *cls, size_t clsLen, uint32_t iface, uint8_t *out, size_t cap, uint8_t **big);
	size_t (*native_call)(uint64_t handle, uint32_t method, const uint8_t *in, size_t len, uint8_t *out, size_t cap, uint8_t **big);
	void (*native_start)(uint64_t handle, uint32_t method, const uint8_t *in, size_t len, uint64_t call);
	void (*native_cancel)(uint64_t call);
	void (*native_release)(uint64_t handle);
	void (*native_free)(void *p);
	// measure_v, when set, is used instead of measure: the four inputs
	// (width, height, availWidth, availHeight) and the two outputs by
	// pointer, for hosts whose FFI can't pass floats (Go's purego on
	// Windows).
	void (*measure_v)(uint32_t id, const float *in, float *out);
} asuka_host_callbacks;

ASUKA_API void asuka_start(asuka_host_callbacks *callbacks);
// asuka_unload lets the running app go: its views (the host gets their
// Destroy ops now), its state and its module. A host that goes, or a
// test about to start the core for another host, calls it. Main thread.
ASUKA_API void asuka_unload(void);
// Loads a compiled module (lang/bytecode), runs its init and mounts its
// app as the root. A module loaded while one runs replaces it (the old
// app is unmounted). It returns 0, or -1 with a message the host can
// show: *err (asuka_free_string frees it).
ASUKA_API int asuka_load(const uint8_t *module, size_t len, char **err);
// Runs the app compiled to C into this build (lang/cgen): as asuka_load,
// with the app's own module. 0, or -1 with a message in *err.
ASUKA_API int asuka_run(char **err);
// Whether an app is compiled into this build: 0 in asuka dev's players.
ASUKA_API int asuka_has_app(void);

// A program (docs/programs.md): a module whose fn main is its entry,
// compiled into this build (lang/cgen), with no UI. asuka_program_main
// runs it on the core's own loop to its end and returns its exit code:
// argv its arguments (argv[0] the program), callbacks a headless host's
// (the native_* of packages' platform code) or NULL. asuka_program_start
// is for a host with a run loop of its own (callbacks->post asks it to
// call asuka_run_posted): it starts the program and returns -1, or the
// exit code when it has ended already; as main ends, the process exits.
ASUKA_API int asuka_program_main(int argc, char **argv, asuka_host_callbacks *callbacks);
ASUKA_API int asuka_program_start(int argc, char **argv, asuka_host_callbacks *callbacks);
// Connects to asuka dev at host:port (a player: the simulator's, an
// emulator's, a phone's, a desktop's): the CLI sends the app's bytecode
// and loads it again on every save, keeping its state and views; the
// player's logs go to the CLI. host may name several, comma-separated,
// tried in turn (a Mac's addresses on the network, its Bonjour name);
// the player connects again whenever the connection ends. hello is the
// player's JSON (platform, device). 0, or -1 with a message in *err.
ASUKA_API int asuka_dev_connect(const char *host, int port, const char *hello, char **err);
// Takes asuka dev's connections on the loopback's port as well, for an
// iPhone the Mac reaches through the cable (usbmuxd); a connection taken
// there replaces the player's own. 0, or -1 with a message in *err.
ASUKA_API int asuka_dev_listen(int port, const char *hello, char **err);
// Loads a module as asuka dev's reloads do, keeping the state cells and
// views of the app it replaces: for a player that gets the modules
// itself (the web's page, which has no socket to connect with). The state
// cells kept (0 for the first), or -1 with a message in *err. *micros, if
// not NULL, is how long the load took.
ASUKA_API int asuka_dev_reload(const uint8_t *module, size_t len, uint32_t *micros, char **err);
// Loads a module as asuka dev's restarts do: like asuka_dev_reload, but
// keeping none of the app's state, which all starts again from its
// initial values. 0, or -1 with a message in *err.
ASUKA_API int asuka_dev_restart(const uint8_t *module, size_t len, uint32_t *micros, char **err);
// Gives the core a frame of asuka dev's (core/src/abi/dev.h: a build
// that failed, one that's fixed, an uncaught error's source), for a
// player that gets them itself (the web's page): the error overlay shows
// what they say. kind is the frame's, p its payload.
ASUKA_API void asuka_dev_frame(int kind, const uint8_t *p, size_t len);
// Memory for bytes a host passes in (the web's JavaScript), and for what
// the core hands out (asuka_native_invoke's answer): asuka_free frees it,
// never the C library's free (it's the core's allocator's).
ASUKA_API void *asuka_alloc(size_t n);
ASUKA_API void asuka_free(void *p);
// Frees a string the core returned.
ASUKA_API void asuka_free_string(char *s);
ASUKA_API void asuka_attach(void);
ASUKA_API void asuka_event(const uint8_t *buf, size_t len);
ASUKA_API int64_t asuka_list_count(uint32_t list);
ASUKA_API void asuka_list_bind(uint32_t list, int64_t index, uint32_t cell);
ASUKA_API void asuka_list_unbind(uint32_t list, uint32_t cell);
ASUKA_API void asuka_run_posted(void);

// asuka_quit_request asks the app whether it may quit (its on quit
// handlers), on the desktop, as it's about to however the quit started
// (the user's Quit, the Quit command, closing the main window where that
// quits, a signal); ending: the session is ending, when it can't be
// refused. It returns 1 if the app canceled it (never while ending).
// Main thread.
ASUKA_API int asuka_quit_request(int ending);
// Sets an environment variable for the Go code (os.Setenv), which copied
// the process's as it loaded.
ASUKA_API void asuka_setenv(const char *key, const char *value);
// Sets the app's own directory, where it keeps its files (std/fs's
// appDir: Application Support on Apple platforms, the app's files
// directory on Android, its data directory on desktops). Before
// asuka_start; without it the core reads ASUKA_DATA, else guesses.
ASUKA_API void asuka_set_app_dir(const char *path);
// Sets one of the app's directories, ASUKA_DIR_*: where it keeps its
// settings (std/fs's configDir), what it can make again (cacheDir), its
// logs (logDir); ASUKA_DIR_APP is asuka_set_app_dir's. Before asuka_start;
// without it the core reads ASUKA_CONFIG, ASUKA_CACHE or ASUKA_LOGS, else
// puts it in the platform's place for it.
enum { ASUKA_DIR_APP = 0, ASUKA_DIR_CONFIG = 1, ASUKA_DIR_CACHE = 2, ASUKA_DIR_LOGS = 3 };
ASUKA_API void asuka_set_dir(int kind, const char *path);
// The address the main window's first Router starts at, the next time
// it's built ("" or NULL: its initial, or the link the app is opened
// with): where a browser opened the page, where a desktop window was left.
// Main thread, before asuka_load or asuka_run.
ASUKA_API void asuka_set_start(const char *href);
// Begins the run's crash record (platform/crash: core/src/std/crash.c), with
// info, JSON of the device and the app ("model", "system",
// "systemVersion", "appVersion", "build"; "foreground" if the app starts
// in front), after asuka_set_app_dir: the directory the host writes its
// native reports in (its inbox), "" if the app doesn't read its reports.
// Any thread, before asuka_start; free the result.
ASUKA_API char *asuka_crash_start(const char *info);
// Says the host wrote a report in the directory's inbox. Any thread.
ASUKA_API void asuka_crash_notify(void);
// Typed native modules' way back to the core: a started call's answer
// (any thread); a callback's call (an Asuka function the module was
// given), on the main thread, its answer (none: 0) into *out, which the
// caller frees (asuka_free).
ASUKA_API void asuka_native_done(uint64_t call, const uint8_t *p, size_t len);
ASUKA_API size_t asuka_native_invoke(uint64_t id, const uint8_t *p, size_t len, uint8_t **out);

#endif
