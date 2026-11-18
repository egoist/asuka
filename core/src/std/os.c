// os.c: std/os's builtins: the process a program runs as (docs/
// programs.md): its arguments, its environment, its standard streams and
// its exit. libc's (POSIX), the CRT's and Win32's in UTF-16 on Windows, so
// the same on every desktop. Phones and the web have no arguments and no
// stdin, and can't exit.
//
// A test (the runner's -test, ak_os_testing) never ends the process: exit
// records its code and cancels the task that called it, args are empty and
// stdin is too, unless os.fake gave them; os.fake also captures what's
// printed. FakeOS.run runs a program's main as the process would
// (os.runMain): 0, 1 with "error: …" for an uncaught error, or exit's code.
//
//	os.args() -> [string]            the arguments, without the program
//	os.program() -> string           the program, as it was started
//	os.env(name) -> string?
//	os.environList() -> [string]     name, value, name, value…
//	os.cwd() -> string
//	os.exit(code)                    flushes stdout and stderr, exits
//	os.readLine() -> string?         a line of stdin, without its end
//	os.readLines() -> [string]?      the lines there are (blocking for one)
//	os.readAll() -> bytes            the rest of stdin
//	os.write(fd, text) / writeBytes(fd, data)   1 stdout, 2 stderr
//	os.isTerminal(fd) -> bool        0 stdin, 1 stdout, 2 stderr
//	os.setFake(args, env, hasEnv, stdin)   a test's process (os.fake)
//	os.fakeOutput(fd) -> string      what the test wrote to fd
//	os.fakeExit() -> int             the code exit was called with (-1: none)
//	os.runMain(main) -> Task<int>    main run as a program is
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif
#include "std/std.h"
#include "rt/sched.h"
#include "base/thread.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || defined(__wasm__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define AK_OS_NO_PROCESS 1  // a phone's app or the web: no arguments, stdin or exit
#endif

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <crt_externs.h>
#endif

#define STR(i) ((ak_str *)args[i].p)

// ---- The process's arguments ----

static ak_mutex mu = AK_MUTEX_INIT;  // all of the state below
static char **argv_;                 // the program's (ak_os_set_args), or the system's
static int argc_ = -1;               // -1: not known yet

void ak_os_set_args(int argc, char **argv) {
	ak_mutex_lock(&mu);
	for (int i = 0; i < argc_; i++) ak_free(argv_[i]);
	ak_free(argv_);
	argc_ = argc < 0 ? 0 : argc;
	argv_ = ak_alloc(sizeof(char *) * (size_t)(argc_ + 1));
	for (int i = 0; i < argc_; i++) {
		size_t n = strlen(argv[i]);
		argv_[i] = memcpy(ak_alloc(n + 1), argv[i], n + 1);
	}
	ak_mutex_unlock(&mu);
}

#if defined(_WIN32)
// utf8 is w in UTF-8 (ak_free it).
static char *utf8(const wchar_t *w, int len) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
	char *s = ak_alloc((size_t)n + 1);
	WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
	s[n] = 0;
	return s;
}

static wchar_t *wide(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = ak_alloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
	if (n <= 0) w[0] = 0;
	else MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}
#endif

#if !defined(AK_OS_NO_PROCESS)
// system_args reads the arguments the process started with, for a host
// that didn't give them (a desktop app's): under mu.
static void system_args(void) {
	argc_ = 0;
	argv_ = ak_alloc(sizeof(char *));
#if defined(_WIN32)
	int n = 0;
	wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
	if (!w) return;
	argv_ = ak_realloc(argv_, sizeof(char *) * (size_t)(n + 1));
	for (int i = 0; i < n; i++) argv_[i] = utf8(w[i], -1);
	argc_ = n;
	LocalFree(w);
#elif defined(__APPLE__) && !defined(AK_OS_NO_PROCESS)
	int n = *_NSGetArgc();
	char **v = *_NSGetArgv();
	argv_ = ak_realloc(argv_, sizeof(char *) * (size_t)(n + 1));
	for (int i = 0; i < n; i++) {
		size_t k = strlen(v[i]);
		argv_[i] = memcpy(ak_alloc(k + 1), v[i], k + 1);
	}
	argc_ = n;
#elif defined(__linux__) && !defined(AK_OS_NO_PROCESS)
	FILE *f = fopen("/proc/self/cmdline", "rb");
	if (!f) return;
	ak_buf b = {0};
	char tmp[4096];
	size_t k;
	while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) ak_buf_put(&b, tmp, k);
	fclose(f);
	for (size_t i = 0; i < b.len;) {
		size_t e = i;
		while (e < b.len && b.data[e]) e++;
		argv_ = ak_realloc(argv_, sizeof(char *) * (size_t)(argc_ + 2));
		char *s = ak_alloc(e - i + 1);
		memcpy(s, b.data + i, e - i);
		s[e - i] = 0;
		argv_[argc_++] = s;
		i = e + 1;
	}
	ak_buf_free(&b);
#endif
}
#endif

// ---- A test's process ----

static void stdin_reset(void);

static struct {
	bool testing;  // the runner's tests: the process never ends
	bool faked;    // os.fake: args, env and stdin are the fake's
	char **args;
	int nargs;
	char **env;  // name, value, …
	int nenv;
	bool has_env;
	ak_buf in;       // stdin
	size_t in_off;
	ak_buf out, err;  // what's written
	int64_t exit;     // -1: exit wasn't called
	int capturing;    // FakeOS.run's in flight: what's written is kept
	bool loose;       // exit was called outside FakeOS.run: it ended a test
} fake = {.exit = -1};

static void print_hook(const char *s, size_t n) {
	ak_mutex_lock(&mu);
	ak_buf_put(&fake.out, s, n);
	ak_buf_put(&fake.out, "\n", 1);
	ak_mutex_unlock(&mu);
}

static void free_list(char **l, int n) {
	for (int i = 0; i < n; i++) ak_free(l[i]);
	ak_free(l);
}

static void fake_clear(void) {
	free_list(fake.args, fake.nargs);
	free_list(fake.env, fake.nenv);
	ak_buf_free(&fake.in);
	ak_buf_free(&fake.out);
	ak_buf_free(&fake.err);
	bool testing = fake.testing;
	memset(&fake, 0, sizeof fake);
	fake.testing = testing;
	fake.exit = -1;
}

void ak_os_testing(bool on) {
	ak_mutex_lock(&mu);
	fake.testing = on;
	ak_mutex_unlock(&mu);
}

void ak_os_reset(void) {
	stdin_reset();
	ak_term_reset();
	ak_mutex_lock(&mu);
	if (ak_lib_print_hook == print_hook) ak_lib_print_hook = NULL;
	fake_clear();
	ak_mutex_unlock(&mu);
}

bool ak_os_test_exit(int64_t *code) {
	ak_mutex_lock(&mu);
	bool done = fake.loose;
	if (code) *code = fake.exit;
	ak_mutex_unlock(&mu);
	return done;
}

// taken_exit is exit's code, -1 when it wasn't called, now FakeOS.run's.
static int64_t taken_exit(void) {
	ak_mutex_lock(&mu);
	int64_t c = fake.exit;
	fake.loose = false;
	ak_mutex_unlock(&mu);
	return c;
}

static char **str_list(const ak_arr *a, int *n) {
	*n = a ? (int)a->len : 0;
	char **l = ak_alloc(sizeof(char *) * (size_t)(*n + 1));
	for (int i = 0; i < *n; i++) {
		const ak_str *s = a->data[i].p;
		l[i] = memcpy(ak_alloc(s->len + 1), s->data, s->len + 1);
	}
	return l;
}

// setFake(args, env, hasEnv, stdin)
static ak_val fake_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_mutex_lock(&mu);
	fake_clear();
	fake.faked = true;
	fake.args = str_list(args[0].p, &fake.nargs);
	fake.env = str_list(args[1].p, &fake.nenv);
	fake.has_env = args[2].i != 0;
	const ak_bytes *in = args[3].p;
	if (in && in->len) ak_buf_put(&fake.in, in->data, in->len);
	ak_mutex_unlock(&mu);
	stdin_reset();
	return (ak_val){0};
}

static ak_val fake_output_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_mutex_lock(&mu);
	ak_buf *b = args[0].i == 2 ? &fake.err : &fake.out;
	ak_str *s = ak_str_new((const char *)b->data, b->len);
	ak_mutex_unlock(&mu);
	return (ak_val){.p = s};
}

static ak_val fake_exit_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	ak_mutex_lock(&mu);
	int64_t c = fake.exit;
	ak_mutex_unlock(&mu);
	return (ak_val){.i = c};
}

// ---- Arguments, environment ----

static ak_val args_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	ak_arr *out = ak_str_arr(0);
	ak_mutex_lock(&mu);
	if (fake.faked || fake.testing) {
		for (int i = 0; i < fake.nargs; i++) ak_str_arr_push(&out, fake.args[i], strlen(fake.args[i]));
	} else {
#if !defined(AK_OS_NO_PROCESS)
		if (argc_ < 0) system_args();
		for (int i = 1; i < argc_; i++) ak_str_arr_push(&out, argv_[i], strlen(argv_[i]));
#endif
	}
	ak_mutex_unlock(&mu);
	return (ak_val){.p = out};
}

static ak_val program_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	ak_mutex_lock(&mu);
	const char *p = "";
	if (fake.faked || fake.testing) {
		p = "test";
	} else {
#if !defined(AK_OS_NO_PROCESS)
		if (argc_ < 0) system_args();
		if (argc_ > 0) p = argv_[0];
#endif
	}
	ak_str *s = ak_str_c(p);
	ak_mutex_unlock(&mu);
	return (ak_val){.p = s};
}

// env_get is name's value in the program's environment (a fake's under
// os.fake with env:), NULL when it isn't set (ak_free it). In a test
// without one, real says whether the real environment counts.
static char *env_get(const char *name, size_t len, bool real) {
	ak_mutex_lock(&mu);
	if (fake.faked && fake.has_env) {
		char *v = NULL;
		for (int i = 0; i + 1 < fake.nenv; i += 2)
			if (strcmp(fake.env[i], name) == 0) {
				ak_free(v);
				size_t k = strlen(fake.env[i + 1]);
				v = memcpy(ak_alloc(k + 1), fake.env[i + 1], k + 1);
			}
		ak_mutex_unlock(&mu);
		return v;
	}
	bool test = fake.faked || fake.testing;
	ak_mutex_unlock(&mu);
	if (test && !real) return NULL;
	if (!len || memchr(name, '=', len) || memchr(name, 0, len)) return NULL;
#if defined(_WIN32)
	wchar_t *w = wide(name);
	DWORD k = GetEnvironmentVariableW(w, NULL, 0);
	char *v = NULL;
	if (k > 0) {
		wchar_t *val = ak_alloc(sizeof(wchar_t) * k);
		DWORD got = GetEnvironmentVariableW(w, val, k);
		v = utf8(val, (int)got);
		ak_free(val);
	} else if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
		v = memcpy(ak_alloc(1), "", 1);
	}
	ak_free(w);
	return v;
#else
	const char *v = getenv(name);
	if (!v) return NULL;
	size_t k = strlen(v);
	return memcpy(ak_alloc(k + 1), v, k + 1);
#endif
}

char *ak_os_getenv(const char *name, bool real) { return env_get(name, strlen(name), real); }

bool ak_os_test_process(void) {
	ak_mutex_lock(&mu);
	bool test = fake.faked || fake.testing;
	ak_mutex_unlock(&mu);
	return test;
}

static ak_val env_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *name = STR(0);
	char *v = env_get(name->data, name->len, true);
	ak_str *s = v ? ak_str_c(v) : NULL;
	ak_free(v);
	return (ak_val){.p = s};
}

static ak_val environ_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	ak_arr *out = ak_str_arr(0);
	ak_mutex_lock(&mu);
	if (fake.faked && fake.has_env) {
		for (int i = 0; i < fake.nenv; i++) ak_str_arr_push(&out, fake.env[i], strlen(fake.env[i]));
		ak_mutex_unlock(&mu);
		return (ak_val){.p = out};
	}
	ak_mutex_unlock(&mu);
#if defined(_WIN32)
	wchar_t *block = GetEnvironmentStringsW();
	for (wchar_t *e = block; e && *e; e += wcslen(e) + 1) {
		if (*e == L'=') continue;  // a drive's directory: =C:=C:\…
		char *s = utf8(e, -1);
		char *eq = strchr(s, '=');
		if (eq) {
			ak_str_arr_push(&out, s, (size_t)(eq - s));
			ak_str_arr_push(&out, eq + 1, strlen(eq + 1));
		}
		ak_free(s);
	}
	if (block) FreeEnvironmentStringsW(block);
#elif !defined(__wasm__)
#if defined(__APPLE__)
	char **env = *_NSGetEnviron();
#else
	extern char **environ;
	char **env = environ;
#endif
	for (; env && *env; env++) {
		const char *eq = strchr(*env, '=');
		if (!eq) continue;
		ak_str_arr_push(&out, *env, (size_t)(eq - *env));
		ak_str_arr_push(&out, eq + 1, strlen(eq + 1));
	}
#endif
	return (ak_val){.p = out};
}

static ak_val cwd_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
#if defined(_WIN32)
	wchar_t buf[32768];
	DWORD k = GetCurrentDirectoryW(32768, buf);
	char *s = utf8(buf, (int)k);
	ak_str *out = ak_str_c(s);
	ak_free(s);
	return (ak_val){.p = out};
#elif defined(__wasm__)
	return (ak_val){.p = ak_str_c("/")};
#else
	char buf[4096];
	return (ak_val){.p = ak_str_c(getcwd(buf, sizeof buf) ? buf : ".")};
#endif
}

// ---- Exit ----

static ak_val exit_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t code = args[0].i;
	ak_mutex_lock(&mu);
	bool test = fake.testing || fake.faked;
	if (test) {
		fake.exit = code < 0 ? 255 : code & 255;
		fake.loose = true;
	}
	ak_mutex_unlock(&mu);
	if (test) {
		// The task that called it ends here, its defers run on the way.
		ak_throw_cancel();
		return (ak_val){0};
	}
#if defined(AK_OS_NO_PROCESS)
	ak_throw_msg("os.exit: an app here can't end its process (a program can, on a desktop)");
	return (ak_val){0};
#else
	fflush(stdout);
	fflush(stderr);
	exit((int)(code & 255));
#endif
}

// ---- Standard streams ----

#if !defined(AK_OS_NO_PROCESS)
static ak_mutex in_mu = AK_MUTEX_INIT;  // stdin's buffer, read by any thread
static ak_buf inbuf;
static size_t inoff;
static bool ineof;

// stdin_reset forgets what's been read of stdin: a test's new process.
static void stdin_reset(void) {
	ak_mutex_lock(&in_mu);
	ak_buf_free(&inbuf);
	inoff = 0;
	ineof = false;
	ak_mutex_unlock(&in_mu);
}

// fill reads more of stdin into inbuf, blocking: false at its end. Under
// in_mu.
static bool fill(void) {
	if (ineof) return false;
	if (inoff > 0 && inoff == inbuf.len) inoff = inbuf.len = 0;
	ak_mutex_lock(&mu);
	bool faked = fake.faked || fake.testing;
	if (faked) {
		size_t k = fake.in.len - fake.in_off;
		if (k) ak_buf_put(&inbuf, fake.in.data + fake.in_off, k);
		fake.in_off = fake.in.len;
		ak_mutex_unlock(&mu);
		if (!k) ineof = true;
		return k > 0;
	}
	ak_mutex_unlock(&mu);
	char tmp[65536];
#if defined(_WIN32)
	_setmode(0, _O_BINARY);
	int k = _read(0, tmp, sizeof tmp);
#else
	ssize_t k = read(0, tmp, sizeof tmp);
#endif
	if (k <= 0) {
		ineof = true;
		return false;
	}
	ak_buf_put(&inbuf, tmp, (size_t)k);
	return true;
}

// a line from inbuf[inoff:]: its end (the '\n', or inbuf.len), or -1.
static int64_t line_end(bool eof) {
	if (inbuf.len <= inoff) return -1;
	const uint8_t *p = memchr(inbuf.data + inoff, '\n', inbuf.len - inoff);
	if (p) return p - inbuf.data;
	return eof && inbuf.len > inoff ? (int64_t)inbuf.len : -1;
}

static ak_str *take_line(int64_t end) {
	size_t from = inoff, to = (size_t)end;
	inoff = to < inbuf.len ? to + 1 : to;
	if (to > from && inbuf.data[to - 1] == '\r') to--;
	return ak_str_new((const char *)inbuf.data + from, to - from);
}
#else
static void stdin_reset(void) {}
#endif

static ak_val read_line_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
#if defined(AK_OS_NO_PROCESS)
	return (ak_val){.p = NULL};
#else
	ak_mutex_lock(&in_mu);
	int64_t end;
	while ((end = line_end(ineof)) < 0 && fill()) {
	}
	ak_str *s = end < 0 ? NULL : take_line(end);
	ak_mutex_unlock(&in_mu);
	return (ak_val){.p = s};
#endif
}

static ak_val read_lines_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
#if defined(AK_OS_NO_PROCESS)
	return (ak_val){.p = NULL};
#else
	ak_mutex_lock(&in_mu);
	while (line_end(ineof) < 0 && fill()) {
	}
	ak_arr *out = NULL;
	int64_t end;
	while ((end = line_end(ineof)) >= 0) {
		if (!out) out = ak_str_arr(8);
		ak_str *s = take_line(end);
		ak_arr_push(&out, (ak_val){.p = s});
		if (out->len >= 4096) break;
	}
	ak_mutex_unlock(&in_mu);
	return (ak_val){.p = out};
#endif
}

static ak_val read_all_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
#if defined(AK_OS_NO_PROCESS)
	return (ak_val){.p = ak_bytes_new(NULL, 0)};
#else
	ak_mutex_lock(&in_mu);
	while (fill()) {
	}
	ak_bytes *b = inbuf.len > inoff ? ak_bytes_new(inbuf.data + inoff, inbuf.len - inoff) : ak_bytes_new(NULL, 0);
	inoff = inbuf.len = 0;
	ak_mutex_unlock(&in_mu);
	return (ak_val){.p = b};
#endif
}

static void write_fd(int64_t fd, const void *p, size_t len) {
	ak_mutex_lock(&mu);
	if (fake.capturing) {
		ak_buf_put(fd == 2 ? &fake.err : &fake.out, p, len);
		ak_mutex_unlock(&mu);
		return;
	}
	ak_mutex_unlock(&mu);
	FILE *f = fd == 2 ? stderr : stdout;
	fwrite(p, 1, len, f);
	fflush(f);
}

static ak_val write_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	write_fd(args[0].i, STR(1)->data, STR(1)->len);
	return (ak_val){0};
}

static ak_val write_bytes_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_bytes *b = args[1].p;
	write_fd(args[0].i, b->data, b->len);
	return (ak_val){0};
}

static ak_val is_terminal_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_mutex_lock(&mu);
	bool test = fake.faked || fake.testing;
	ak_mutex_unlock(&mu);
	if (test || args[0].i < 0 || args[0].i > 2) return (ak_val){.i = 0};
#if defined(AK_OS_NO_PROCESS)
	return (ak_val){.i = 0};
#elif defined(_WIN32)
	return (ak_val){.i = _isatty((int)args[0].i) != 0};
#else
	return (ak_val){.i = isatty((int)args[0].i) != 0};
#endif
}

// ---- Running main as a program would (tests) ----

typedef struct {
	ak_future *out, *main;
} run_t;

// run_end settles out as the program's end would: exit's code, 1 with
// the error (written to stderr) for one main threw, else 0.
// capture starts (on) or ends keeping what's written: print's, os's.
static void capture(bool on) {
	ak_mutex_lock(&mu);
	fake.capturing += on ? 1 : -1;
	ak_lib_print_hook = fake.capturing > 0 ? print_hook : NULL;
	ak_mutex_unlock(&mu);
}

static void run_end(ak_future *out, void *err) {
	int64_t code = taken_exit();
	if (code >= 0) {
		// exit was called: its cancellation is how main ended.
	} else if (err && ak_is_cancel(err)) {
		// The test went: so does what it ran.
		capture(false);
		ak_future_reject(out, err);
		return;
	} else if (err) {
		ak_str *m = ak_vm_error_text(err);
		write_fd(2, "error: ", 7);
		write_fd(2, m->data, m->len);
		write_fd(2, "\n", 1);
		ak_release(m);
		code = 1;
	} else {
		code = 0;
	}
	capture(false);
	if (err) ak_release(err);
	ak_future_resolve(out, (ak_val){.i = code});
}

static void run_done(void *ctx) {
	run_t *r = ctx;
	void *err = NULL;
	if (r->main->state == AK_FUT_FAILED) {
		err = r->main->error;
		ak_retain(err);
	}
	r->out->ctx = NULL;
	r->out->cancel = NULL;
	run_end(r->out, err);
	ak_release(r->main);
	ak_release(r->out);
	ak_free(r);
}

static void run_cancel(ak_future *f) {
	run_t *r = f->ctx;
	if (r) ak_future_cancel(r->main);
}

static ak_val run_main_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_closure *c = args[0].p;
	ak_future *out = ak_future_new(AK_K_INT);
	ak_mutex_lock(&mu);
	fake.exit = -1;
	fake.loose = false;
	ak_mutex_unlock(&mu);
	capture(true);
	bool async = ak_closure_async(c);
	ak_val v = ak_call(c, NULL, 0);
	if (ak_error_pending()) {
		run_end(out, ak_error_take());
		return (ak_val){.p = out};
	}
	if (!async) {
		run_end(out, NULL);
		return (ak_val){.p = out};
	}
	run_t *r = ak_alloc(sizeof *r);
	r->main = v.p;
	r->out = out;
	ak_retain(out);
	out->ctx = r;
	out->cancel = run_cancel;
	ak_future_wait(r->main, run_done, r);
	return (ak_val){.p = out};
}

const ak_lib_entry ak_std_os[] = {
	{"os.args", args_fn},
	{"os.program", program_fn},
	{"os.env", env_fn},
	{"os.environList", environ_fn},
	{"os.cwd", cwd_fn},
	{"os.exit", exit_fn},
	{"os.readLine", read_line_fn},
	{"os.readLines", read_lines_fn},
	{"os.readAll", read_all_fn},
	{"os.write", write_fn},
	{"os.writeBytes", write_bytes_fn},
	{"os.isTerminal", is_terminal_fn},
	{"os.setFake", fake_fn},
	{"os.fakeOutput", fake_output_fn},
	{"os.fakeExit", fake_exit_fn},
	{"os.runMain", run_main_fn},
	{NULL, NULL},
};

// ---- std/flag ----

// flag.spec<T>() -> string: what the compiler wrote of T (lang/ir/flag.go),
// passed as the call's last argument.
static ak_val flag_spec_fn(ak_val *args, uint32_t n) { return (ak_val){.p = ak_retain(args[n - 1].p)}; }

const ak_lib_entry ak_std_flag[] = {
	{"flag.spec", flag_spec_fn},
	{NULL, NULL},
};
