// signals.c: std/os's signals: Ctrl-C, a terminate, a hangup, as the
// program sees them.
//
// A signal arrives on a thread of its own (the C handler writes a byte to
// a pipe a watcher thread reads; Windows calls its console handler on a
// new thread), which decides at once, then posts the rest to the main
// thread:
//
//   - When an os.signals stream reads that kind, the stream gets it.
//   - Else, in a program whose main is a task, the first one ends the
//     program as its main's cancellation: its defers run, and the exit
//     code is 128 and the signal's number (130 for Ctrl-C). A second, or
//     one in a program whose main isn't a task (synchronous code, which
//     nothing cancels), ends the process at once, as the system's
//     default would.
//
// The handlers are installed as a program starts (ak_os_signals_program),
// or, in a desktop app, as the first stream starts reading; a phone's app
// and the web have none, and their streams never give a signal.
//
//	os.watchSignals(mask) -> int        a stream's watcher
//	os.unwatchSignals(id)
//	os.nextSignal(id) -> future of int  its next signal's kind
//	os.fakeSignal(kind)                 a test's: as if it arrived
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L  // sigaction, SA_RESTART
#endif
#include "std/std.h"
#include "rt/sched.h"
#include "base/thread.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || defined(__wasm__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define AK_SIGNALS_NONE 1  // a phone's app or the web: no signals to hear
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(AK_SIGNALS_NONE)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

// The kinds, as std/os's Signal: interrupt, terminate, hangup.
enum { K_INTERRUPT, K_TERMINATE, K_HANGUP, NKINDS };

// Their numbers on POSIX, for the exit code (128 and it) on every system.
static const int signo[NKINDS] = {2, 15, 1};

static atomic_int readers[NKINDS];  // watchers reading each kind (main thread writes)
static atomic_bool graceful;        // a program whose main is a task: its end is a cancellation
static void (*on_default)(int code);  // the program's end for a signal (program.c)

// ---- Watchers (main thread) ----

#define PENDING 16

typedef struct watcher {
	int64_t id;
	int mask;
	ak_future *waiting;
	int pending[PENDING];  // kinds arrived while it wasn't waiting
	int npending;
	struct watcher *next;
} watcher;

static watcher *watchers;
static int64_t next_id = 1;

static void install(void);

static watcher *find(int64_t id) {
	for (watcher *w = watchers; w; w = w->next)
		if (w->id == id) return w;
	return NULL;
}

static ak_val watch_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	watcher *w = ak_alloc(sizeof *w);
	memset(w, 0, sizeof *w);
	w->id = next_id++;
	w->mask = (int)args[0].i & ((1 << NKINDS) - 1);
	w->next = watchers;
	watchers = w;
	for (int k = 0; k < NKINDS; k++)
		if (w->mask & 1 << k) atomic_fetch_add(&readers[k], 1);
	install();
	return (ak_val){.i = w->id};
}

static ak_val unwatch_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	for (watcher **p = &watchers; *p; p = &(*p)->next) {
		watcher *w = *p;
		if (w->id != args[0].i) continue;
		*p = w->next;
		for (int k = 0; k < NKINDS; k++)
			if (w->mask & 1 << k) atomic_fetch_sub(&readers[k], 1);
		if (w->waiting) {
			ak_future *f = w->waiting;
			w->waiting = NULL;
			f->ctx = NULL;
			f->cancel = NULL;
			ak_future_cancel(f);
			ak_release(f);
		}
		ak_free(w);
		break;
	}
	return (ak_val){0};
}

static void next_cancel(ak_future *f) {
	watcher *w = f->ctx;
	f->ctx = NULL;
	if (w && w->waiting == f) {
		w->waiting = NULL;
		ak_release(f);
	}
	ak_future_cancel(f);
}

static ak_val next_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_future *f = ak_future_new(AK_K_INT);
	watcher *w = find(args[0].i);
	if (!w) return (ak_val){.p = f};  // gone: it waits for nothing, until it's cancelled
	if (w->npending) {
		int k = w->pending[0];
		memmove(w->pending, w->pending + 1, sizeof(int) * (size_t)--w->npending);
		ak_future_resolve(f, (ak_val){.i = k});
		return (ak_val){.p = f};
	}
	w->waiting = ak_retain(f);
	f->ctx = w;
	f->cancel = next_cancel;
	return (ak_val){.p = f};
}

// deliver gives kind to the watchers that read it; false when none does.
static bool deliver(int kind) {
	bool any = false;
	for (watcher *w = watchers; w; w = w->next) {
		if (!(w->mask & 1 << kind)) continue;
		any = true;
		if (w->waiting) {
			ak_future *f = w->waiting;
			w->waiting = NULL;
			f->ctx = NULL;
			f->cancel = NULL;
			ak_future_resolve(f, (ak_val){.i = kind});
			ak_release(f);
		} else if (w->npending < PENDING) {
			w->pending[w->npending++] = kind;
		}
	}
	return any;
}

#if !defined(AK_SIGNALS_NONE)
// arrived_main is a signal on the main thread: a stream's, or the
// program's end.
static void arrived_main(void *ctx) {
	int kind = (int)(intptr_t)ctx;
	if (deliver(kind)) return;
	if (on_default) on_default(128 + signo[kind]);
}

// ---- Arriving (the watcher thread, Windows' handler thread) ----

static atomic_bool ending;  // a signal began the program's end: the next ends it at once

static void exit_now(int kind) {
	fflush(stdout);
	fflush(stderr);
	_Exit(128 + signo[kind]);
}

static void arrived(int kind) {
	if (atomic_load(&readers[kind]) > 0) {
		ak_post(arrived_main, (void *)(intptr_t)kind);
		return;
	}
	if (!atomic_load(&graceful) || atomic_exchange(&ending, true)) exit_now(kind);
	ak_post(arrived_main, (void *)(intptr_t)kind);
}
#endif

#if defined(_WIN32)

static HANDLE ended_event;  // set as the program ends: a close waits for it

static BOOL WINAPI on_ctrl(DWORD event) {
	switch (event) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		arrived(K_INTERRUPT);
		return TRUE;
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		// Windows ends the process as this returns (or 5 s on): it waits
		// for the program's end, its defers, a little less.
		arrived(K_TERMINATE);
		if (ended_event) WaitForSingleObject(ended_event, 4500);
		return TRUE;
	}
	return FALSE;
}

static void install(void) {
	static atomic_bool done;
	if (atomic_exchange(&done, true)) return;
	ended_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	SetConsoleCtrlHandler(on_ctrl, TRUE);
}

void ak_os_signals_ended(void) {
	if (ended_event) SetEvent(ended_event);
}

#elif !defined(AK_SIGNALS_NONE)

static int pipe_w = -1;

static void on_signal(int sig) {
	int saved = errno;
	unsigned char k = sig == SIGINT ? K_INTERRUPT : sig == SIGTERM ? K_TERMINATE : K_HANGUP;
	ssize_t r = write(pipe_w, &k, 1);
	(void)r;
	errno = saved;
}

static void watch_thread(void *arg) {
	int fd = (int)(intptr_t)arg;
	for (;;) {
		unsigned char k;
		ssize_t r = read(fd, &k, 1);
		if (r == 1 && k < NKINDS) arrived(k);
		else if (r < 0 && errno != EINTR) return;
	}
}

static void install(void) {
	static atomic_bool done;
	if (atomic_exchange(&done, true)) return;
	int p[2];
	if (pipe(p) != 0) return;
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFL, O_NONBLOCK);  // a burst the thread hasn't read drops, not blocks
	pipe_w = p[1];
	if (!ak_thread_start(watch_thread, (void *)(intptr_t)p[0])) return;
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
}

void ak_os_signals_ended(void) {}

#else

static void install(void) {}
void ak_os_signals_ended(void) {}

#endif

// ak_os_signals_program makes the process's signals a program's: end is
// how one no stream reads ends it, gracefully when its main is a task.
void ak_os_signals_program(void (*end)(int code), bool main_is_task) {
	on_default = end;
	atomic_store(&graceful, main_is_task);
	install();
}

// ---- Tests ----

// fakeSignal(kind): as if it arrived, in a test: a stream's, or the end
// of the main os.fake's run runs (FakeOS.signal).
static ak_val fake_signal_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int kind = (int)args[0].i;
	if (kind < 0 || kind >= NKINDS) return (ak_val){0};
	if (!deliver(kind)) ak_os_fake_end(128 + signo[kind]);
	return (ak_val){0};
}

void ak_os_signals_reset(void) {
	while (watchers) {
		watcher *w = watchers;
		ak_val id = {.i = w->id};
		unwatch_fn(&id, 1);
	}
}

const ak_lib_entry ak_std_signals[] = {
	{"os.watchSignals", watch_fn},
	{"os.unwatchSignals", unwatch_fn},
	{"os.nextSignal", next_fn},
	{"os.fakeSignal", fake_signal_fn},
	{NULL, NULL},
};
