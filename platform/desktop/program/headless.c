// headless.c: the host a program runs on (docs/programs.md) on Windows and
// Linux when its packages have platform code (std/http's WinHTTP and
// libsoup): no window, the main thread's loop, and asuka_desktop.h's host
// half for the packages' C, which the native registry (src/native.c)
// serves. Its main replaces the program's own (lang/cgen's, weak).
//
// The loop is the core's own (asuka_program_main: what's posted runs, it
// waits for more), or on Linux, when GLib is there, GLib's main loop, so
// that what packages run through GLib's main context (asuka_linux_main, a
// bus's signals) runs too: the core's posts go to it as idle callbacks.
//
// The program's name and version are compiled in (ASUKA_PROGRAM_NAME,
// ASUKA_PROGRAM_VERSION); its directories follow internal/appdir's rules,
// as an app's do.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "asuka.h"
#include "asuka_desktop.h"
#include "rt/sched.h"
#include "base/thread.h"
#include "std/std.h"
#if defined(__linux__)
#include "asuka_linux.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define SEP "/"
#endif

#ifndef ASUKA_PROGRAM_NAME
#define ASUKA_PROGRAM_NAME "program"
#endif
#ifndef ASUKA_PROGRAM_VERSION
#define ASUKA_PROGRAM_VERSION ""
#endif

static uint64_t main_thread;

void asuka_desktop_post(void (*fn)(void *ctx), void *ctx) { ak_post(fn, ctx); }
bool asuka_desktop_on_main(void) { return ak_thread_id() == main_thread; }

// No windows, no views.
void *asuka_desktop_window(uint32_t view) {
	(void)view;
	return NULL;
}
void *asuka_desktop_view(uint32_t view) {
	(void)view;
	return NULL;
}
void *asuka_desktop_webview(uint32_t view) {
	(void)view;
	return NULL;
}
uint32_t asuka_desktop_color(uint64_t color) { return color > 0xffffffffull ? 0x000000ff : (uint32_t)color; }
bool asuka_desktop_dark(void) { return false; }

const char *asuka_desktop_app_name(void) { return ASUKA_PROGRAM_NAME; }
const char *asuka_desktop_app_id(void) { return ASUKA_PROGRAM_NAME; }
const char *asuka_desktop_app_version(void) { return ASUKA_PROGRAM_VERSION; }
const char *asuka_desktop_app_build(void) { return ""; }
const char *asuka_desktop_app_setting(const char *key) {
	(void)key;
	return "";
}

// ---- Directories ----

static char *join(const char *a, const char *b) { return asuka_sprintf("%s" SEP "%s", a, b); }

static void make_dirs(const char *p) {
	char *s = asuka_strdup(p);
	for (char *c = s + 1; *c; c++) {
		if (*c != '/' && *c != '\\') continue;
		char k = *c;
		*c = 0;
#if defined(_WIN32)
		_mkdir(s);
#else
		mkdir(s, 0755);
#endif
		*c = k;
	}
#if defined(_WIN32)
	_mkdir(s);
#else
	mkdir(s, 0755);
#endif
	free(s);
}

static const char *env_or(const char *name) {
	const char *v = getenv(name);
	return v && *v ? v : NULL;
}

// base is an XDG base directory (its variable, when absolute, else HOME's
// fallback), or on Windows a known folder's variable.
static char *base(const char *xdg, const char *fallback, const char *win) {
#if defined(_WIN32)
	(void)xdg, (void)fallback;
	const char *v = env_or(win);
	return v ? asuka_strdup(v) : NULL;
#else
	(void)win;
	const char *v = env_or(xdg);
	if (v && v[0] == '/') return asuka_strdup(v);
	const char *home = env_or("HOME");
	return home ? join(home, fallback) : NULL;
#endif
}

static char *dirs[5];

static const char *dir(int k, const char *env, const char *xdg, const char *fallback, const char *win, const char *sub) {
	if (!dirs[k]) {
		const char *e = env_or(env);
		if (e) {
			dirs[k] = asuka_strdup(e);
		} else {
			char *b = base(xdg, fallback, win);
			char *d = b ? join(b, ASUKA_PROGRAM_NAME) : asuka_strdup(".");
			free(b);
			dirs[k] = sub ? join(d, sub) : d;
			if (sub) free(d);
		}
		make_dirs(dirs[k]);
	}
	return dirs[k];
}

const char *asuka_desktop_data_dir(void) { return dir(0, "ASUKA_DATA", "XDG_DATA_HOME", ".local/share", "LOCALAPPDATA", NULL); }
const char *asuka_desktop_config_dir(void) { return dir(1, "ASUKA_CONFIG", "XDG_CONFIG_HOME", ".config", "APPDATA", NULL); }
#if defined(_WIN32)
const char *asuka_desktop_cache_dir(void) { return dir(2, "ASUKA_CACHE", "", "", "LOCALAPPDATA", ASUKA_PROGRAM_NAME SEP "Cache"); }
const char *asuka_desktop_logs_dir(void) { return dir(3, "ASUKA_LOGS", "", "", "LOCALAPPDATA", ASUKA_PROGRAM_NAME SEP "Logs"); }
#else
const char *asuka_desktop_cache_dir(void) { return dir(2, "ASUKA_CACHE", "XDG_CACHE_HOME", ".cache", "", NULL); }
const char *asuka_desktop_logs_dir(void) { return dir(3, "ASUKA_LOGS", "XDG_STATE_HOME", ".local/state", "", NULL); }
#endif

// A program's assets are compiled in (std/assets): the directory it's
// in, for a package that names a file beside it.
const char *asuka_desktop_assets_dir(void) {
	if (!dirs[4]) {
#if defined(_WIN32)
		wchar_t w[MAX_PATH];
		DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH);
		int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL);
		char *p = malloc((size_t)k + 1);
		WideCharToMultiByte(CP_UTF8, 0, w, (int)n, p, k, NULL, NULL);
		p[k] = 0;
#else
		char *p = calloc(4096, 1);
		if (readlink("/proc/self/exe", p, 4095) <= 0) strcpy(p, "./x");
#endif
		char *slash = strrchr(p, SEP[0]);
		if (slash) *slash = 0;
		dirs[4] = p;
	}
	return dirs[4];
}

// ---- The loop ----

#if defined(__linux__)
static gboolean run_posted(gpointer p) {
	(void)p;
	asuka_run_posted();
	return 0;
}

// glib_post asks GLib's main loop to run what the core posted. Any thread.
static void glib_post(void) { G(g_idle_add_full)(G_PRIORITY_DEFAULT, run_posted, NULL, NULL); }
#endif

int main(int argc, char **argv) {
	main_thread = ak_thread_id();
	asuka_host_callbacks cb;
	memset(&cb, 0, sizeof cb);
	asuka_native_callbacks(&cb);
#if defined(__linux__)
	if (asuka_linux() && G(g_main_loop_new) && G(g_main_loop_run) && G(g_idle_add_full)) {
		cb.post = glib_post;
		int code = asuka_program_start(argc, argv, &cb);
		if (code >= 0) return code;
		GMainLoop *loop = G(g_main_loop_new)(NULL, 0);
		G(g_main_loop_run)(loop);  // the program exits as main ends
		return 0;
	}
#endif
#if defined(_WIN32)
	(void)argc, (void)argv;
	return asuka_program_main(0, NULL, &cb);  // the arguments in UTF-16
#else
	return asuka_program_main(argc, argv, &cb);
#endif
}
