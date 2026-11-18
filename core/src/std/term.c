// term.c: std/term's builtins: the terminal a program runs in (docs/
// programs.md): whether styled text shows on a stream, the terminal's
// size, and stdin's echo for a secret. termios and ioctl on macOS and
// Linux, the console's modes on Windows (virtual terminal processing
// turned on once, so the codes show). Phones and the web have no
// terminal; nor has a test's process (the runner's tests, os.fake),
// whose streams aren't terminals and whose environment is os.fake's.
//
//	term.colorsAt(fd) -> bool       styled text shows on fd (1 stdout, 2 stderr)
//	term.setChoice(fd, choice)      0 auto, 1 always, 2 never; fd -1: both streams
//	term.dimension(which) -> int    0 columns, 1 rows; 0 when unknown
//	term.setEcho(on) -> bool        stdin's echo off, or back; whether it changed
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif
#include "std/std.h"
#include "base/thread.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || defined(__wasm__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define AK_TERM_NONE 1  // a phone's app or the web: no terminal
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#elif !defined(AK_TERM_NONE)
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

enum { AUTO, ALWAYS, NEVER };

static ak_mutex mu = AK_MUTEX_INIT;
static int choice[3];  // per fd: setColors's
#if defined(_WIN32)
static bool vt_tried[3], vt_ok[3];
#endif

void ak_term_reset(void) {
	ak_mutex_lock(&mu);
	memset(choice, 0, sizeof choice);
	ak_mutex_unlock(&mu);
}

// env_set: the variable is set and not empty; its value in *out when out
// isn't NULL (ak_free it).
static bool env_set(const char *name, char **out) {
	char *v = ak_os_getenv(name, false);
	bool set = v && *v;
	if (out && set) *out = v;
	else ak_free(v);
	return set;
}

// terminal reports whether fd is a terminal, not a file or a pipe.
static bool terminal(int fd) {
	if (fd < 0 || fd > 2 || ak_os_test_process()) return false;
#if defined(AK_TERM_NONE)
	return false;
#elif defined(_WIN32)
	return _isatty(fd) != 0;
#else
	return isatty(fd) != 0;
#endif
}

#if defined(_WIN32)
static HANDLE std_handle(int fd) {
	return GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}
#endif

// vt makes fd show escape codes: on Windows, the console's virtual
// terminal processing turned on (once); false when the console can't
// (before Windows 10). A stream that isn't a console passes them on.
static bool vt(int fd) {
#if defined(_WIN32)
	if (fd < 1 || fd > 2 || ak_os_test_process()) return true;
	ak_mutex_lock(&mu);
	if (!vt_tried[fd]) {
		vt_tried[fd] = true;
		vt_ok[fd] = true;
		HANDLE h = std_handle(fd);
		DWORD mode;
		if (h != INVALID_HANDLE_VALUE && h && GetConsoleMode(h, &mode)) {
			vt_ok[fd] = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) || SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
		}
	}
	bool ok = vt_ok[fd];
	ak_mutex_unlock(&mu);
	return ok;
#else
	AK_UNUSED(fd);
	return true;
#endif
}

// colors decides whether styled text shows on fd: setColors's choice;
// else FORCE_COLOR (set, and not "0" or "false": on; those: off),
// NO_COLOR (set: off), TERM=dumb (off); else whether fd is a terminal.
static bool colors(int fd) {
	if (fd < 0 || fd > 2) return false;
	ak_mutex_lock(&mu);
	int c = choice[fd];
	ak_mutex_unlock(&mu);
	if (c == NEVER) return false;
	if (c == ALWAYS) {
		vt(fd);
		return true;
	}
	char *force = NULL;
	if (env_set("FORCE_COLOR", &force)) {
		bool on = strcmp(force, "0") != 0 && strcmp(force, "false") != 0;
		ak_free(force);
		if (on) vt(fd);
		return on;
	}
	if (env_set("NO_COLOR", NULL)) return false;
	char *t = NULL;
	if (env_set("TERM", &t)) {
		bool dumb = strcmp(t, "dumb") == 0;
		ak_free(t);
		if (dumb) return false;
	}
	return terminal(fd) && vt(fd);
}

static ak_val colors_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = colors((int)args[0].i)};
}

static ak_val set_choice_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t fd = args[0].i, c = args[1].i;
	if (c < AUTO || c > NEVER) c = AUTO;
	ak_mutex_lock(&mu);
	if (fd < 0) choice[1] = choice[2] = (int)c;
	else if (fd <= 2) choice[fd] = (int)c;
	ak_mutex_unlock(&mu);
	return (ak_val){0};
}

// ---- The terminal's size ----

// env_int is a variable's value as a positive number, 0 when it isn't one.
static int64_t env_int(const char *name) {
	char *v = NULL;
	if (!env_set(name, &v)) return 0;
	char *end;
	long long k = strtoll(v, &end, 10);
	bool ok = *end == 0 && k > 0 && k < 1000000;
	ak_free(v);
	return ok ? k : 0;
}

// window is the terminal's size behind stdout, stderr or stdin (the
// first that's one): false when none is.
static bool window(int64_t *cols, int64_t *rows) {
	if (ak_os_test_process()) return false;
#if defined(AK_TERM_NONE)
	AK_UNUSED(cols), AK_UNUSED(rows);
	return false;
#elif defined(_WIN32)
	for (int fd = 1; fd <= 2; fd++) {
		CONSOLE_SCREEN_BUFFER_INFO info;
		HANDLE h = std_handle(fd);
		if (h && h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
			*cols = info.srWindow.Right - info.srWindow.Left + 1;
			*rows = info.srWindow.Bottom - info.srWindow.Top + 1;
			return *cols > 0 && *rows > 0;
		}
	}
	return false;
#else
	const int fds[] = {1, 2, 0};
	for (size_t i = 0; i < 3; i++) {
		struct winsize ws;
		if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
			*cols = ws.ws_col;
			*rows = ws.ws_row;
			return true;
		}
	}
	return false;
#endif
}

static ak_val dimension_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	bool rows = args[0].i == 1;
	int64_t v = env_int(rows ? "LINES" : "COLUMNS");
	if (v > 0) return (ak_val){.i = v};
	int64_t c = 0, r = 0;
	if (!window(&c, &r)) return (ak_val){.i = 0};
	return (ak_val){.i = rows ? r : c};
}

// ---- Echo ----

// While a secret is read stdin's echo is off: the mode it had is kept
// to put back, as the read ends or the process does (Ctrl-C, a signal
// that ends it).
static bool hidden;
#if defined(_WIN32)
static DWORD saved_mode;

static BOOL WINAPI on_ctrl(DWORD event) {
	AK_UNUSED(event);
	SetConsoleMode(std_handle(0), saved_mode);
	return FALSE;  // the next handler's, and the default's: the process ends
}
#elif !defined(AK_TERM_NONE)
static struct termios saved_tio;
static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
static struct sigaction old_actions[4];

static void on_signal(int sig) {
	tcsetattr(0, TCSANOW, &saved_tio);
	for (size_t i = 0; i < 4; i++)
		if (signals[i] == sig) sigaction(sig, &old_actions[i], NULL);
	raise(sig);
}
#endif

static bool echo_off(void) {
	if (hidden || !terminal(0)) return false;
#if defined(AK_TERM_NONE)
	return false;
#elif defined(_WIN32)
	HANDLE h = std_handle(0);
	DWORD mode;
	if (!h || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
	saved_mode = mode;
	SetConsoleCtrlHandler(on_ctrl, TRUE);
	if (!SetConsoleMode(h, (mode & ~(DWORD)ENABLE_ECHO_INPUT) | ENABLE_LINE_INPUT)) {
		SetConsoleCtrlHandler(on_ctrl, FALSE);
		return false;
	}
	hidden = true;
	return true;
#else
	struct termios t;
	if (tcgetattr(0, &t) != 0) return false;
	saved_tio = t;
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	for (size_t i = 0; i < 4; i++) sigaction(signals[i], &sa, &old_actions[i]);
	t.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK);
	t.c_lflag |= ICANON;
	// What was typed before the question isn't the secret.
	if (tcsetattr(0, TCSAFLUSH, &t) != 0) {
		for (size_t i = 0; i < 4; i++) sigaction(signals[i], &old_actions[i], NULL);
		return false;
	}
	hidden = true;
	return true;
#endif
}

static bool echo_on(void) {
	if (!hidden) return false;
	hidden = false;
#if defined(_WIN32)
	SetConsoleMode(std_handle(0), saved_mode);
	SetConsoleCtrlHandler(on_ctrl, FALSE);
#elif !defined(AK_TERM_NONE)
	tcsetattr(0, TCSANOW, &saved_tio);
	for (size_t i = 0; i < 4; i++) sigaction(signals[i], &old_actions[i], NULL);
#endif
	return true;
}

static ak_val set_echo_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_mutex_lock(&mu);
	bool changed = args[0].i ? echo_on() : echo_off();
	ak_mutex_unlock(&mu);
	return (ak_val){.i = changed};
}

const ak_lib_entry ak_std_term[] = {
	{"term.colorsAt", colors_fn},
	{"term.setChoice", set_choice_fn},
	{"term.dimension", dimension_fn},
	{"term.setEcho", set_echo_fn},
	{NULL, NULL},
};
