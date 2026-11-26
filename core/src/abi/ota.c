// ota.c: an over-the-air update run in place of the build's code (RFC
// 0006). platform/update puts one in the app's directory, in Updates:
//
//   Updates/current     three lines: its version, the fingerprint of the
//                       build it was made against, its module's file name
//   Updates/<file>      its module (bytecode, as asuka dev sends one)
//   Updates/launching   its version, while a launch of it is in its first
//                       seconds, and how many launches before this one
//                       ended in theirs
//
// At launch (asuka_run) the core runs the update when its fingerprint is
// this build's (link.c's ak_link_fingerprint: what the build's native
// side gives code); otherwise the build's own code. A launch that finds
// launching left behind follows one that ended early (a crash, a hang the
// system killed, or the user closing the app at once): after two in a
// row, as a crash at launch repeats and a user's closing doesn't, the
// update is set aside (its files renamed .bad, never taken again) and the
// build's code runs. So is one whose module doesn't load. The mark goes
// once the app has run for a while after it mounted.
#include "abi/ota.h"
#include "rt/sched.h"
#include "std/std.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// How long a launch of an update runs before it counts as good.
#define GOOD_AFTER_NS (10 * (int64_t)1000000000)

static char *dup(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	memcpy(p, s, n);
	return p;
}

static char *join(const char *dir, const char *name) {
	size_t a = strlen(dir), b = strlen(name);
	char *p = malloc(a + b + 2);
	memcpy(p, dir, a);
	p[a] = '/';
	memcpy(p + a + 1, name, b + 1);
	return p;
}

// updates is the app's Updates directory (malloc'd), or NULL.
static char *updates(void) {
	char *app = ak_fs_app_dir();  // the core's allocator's: ak_free
	if (!app || !*app) {
		ak_free(app);
		return NULL;
	}
	char *d = join(app, "Updates");
	ak_free(app);
	return d;
}

static char *read_all(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap + 1);
	for (;;) {
		if (n == cap) {
			cap *= 2;
			buf = realloc(buf, cap + 1);
		}
		size_t got = fread(buf + n, 1, cap - n, f);
		if (!got) break;
		n += got;
	}
	fclose(f);
	buf[n] = 0;
	if (len) *len = n;
	return buf;
}

static bool write_text(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	bool ok = fputs(text, f) >= 0;
	return fclose(f) == 0 && ok;
}

// line takes the next line of *s (its newline cut), or NULL.
static char *line(char **s) {
	if (!*s || !**s) return NULL;
	char *start = *s, *nl = strchr(start, '\n');
	if (nl) {
		*nl = 0;
		*s = nl + 1;
	} else {
		*s = start + strlen(start);
	}
	size_t n = strlen(start);
	if (n && start[n - 1] == '\r') start[n - 1] = 0;
	return start;
}

// set_aside renames the update's module and record .bad: it's never taken
// again, and platform/update sees it was (a version it won't download
// again).
static void set_aside(const char *dir, const char *file) {
	char *cur = join(dir, "current"), *mod = join(dir, file);
	char *bad_cur = malloc(strlen(mod) + 16), *bad_mod = malloc(strlen(mod) + 8);
	snprintf(bad_cur, strlen(mod) + 16, "%s.current.bad", mod);
	snprintf(bad_mod, strlen(mod) + 8, "%s.bad", mod);
	rename(cur, bad_cur);
	rename(mod, bad_mod);
	char *launching = join(dir, "launching");
	remove(launching);
	free(launching);
	free(cur), free(mod), free(bad_cur), free(bad_mod);
}

static char *launching_path;  // set while a launch of an update is new
static uint64_t good_timer;
static char *taken_dir, *taken_file;  // the update taken, until it's mounted
static char running[128];             // its version

static void good(void *ctx) {
	(void)ctx;
	good_timer = 0;
	if (launching_path) {
		remove(launching_path);
		free(launching_path);
		launching_path = NULL;
	}
}

ak_module *ak_ota_take(void) { return ak_ota_take_for(ak_link_fingerprint); }

ak_module *ak_ota_take_for(const char *fp) {
	if (!fp || !*fp) return NULL;  // a build without updates
	char *dir = updates();
	if (!dir) return NULL;
	char *cur_path = join(dir, "current");
	char *rec = read_all(cur_path, NULL);
	free(cur_path);
	ak_module *m = NULL;
	if (rec) {
		char *s = rec;
		char *version = line(&s), *fingerprint = line(&s), *file = line(&s);
		if (version && fingerprint && file && *file && !strchr(file, '/') && strcmp(fingerprint, fp) == 0) {
			char *lpath = join(dir, "launching");
			char *last = read_all(lpath, NULL);
			// The launches of this version before that ended early.
			int early = 0;
			if (last) {
				char *ls = last, *lv = line(&ls), *ln = line(&ls);
				if (lv && strcmp(lv, version) == 0) early = 1 + (ln ? atoi(ln) : 0);
			}
			free(last);
			if (early >= 2) {
				fprintf(stderr, "asuka: update %s ended its last launch early: the build's code runs\n", version);
				set_aside(dir, file);
			} else {
				char *mpath = join(dir, file);
				size_t n = 0;
				char *bytes = read_all(mpath, &n);
				free(mpath);
				char msg[256];
				m = bytes ? ak_module_load((const uint8_t *)bytes, n, msg, sizeof msg) : NULL;
				free(bytes);
				if (!m) {
					fprintf(stderr, "asuka: update %s doesn't load: the build's code runs\n", version);
					set_aside(dir, file);
				} else {
					taken_dir = dup(dir), taken_file = dup(file);
					snprintf(running, sizeof running, "%s", version);
					char mark[160];
					snprintf(mark, sizeof mark, "%s\n%d\n", version, early);
					if (write_text(lpath, mark)) {
						launching_path = lpath;
						lpath = NULL;
					}
				}
			}
			free(lpath);
		}
		free(rec);
	}
	free(dir);
	return m;
}

static void forget_taken(void) {
	free(taken_dir), free(taken_file);
	taken_dir = taken_file = NULL;
}

void ak_ota_mounted(void) {
	forget_taken();
	if (launching_path && !good_timer) good_timer = ak_after(GOOD_AFTER_NS, good, NULL);
}

void ak_ota_failed(void) {
	if (taken_dir && taken_file) set_aside(taken_dir, taken_file);
	forget_taken();
	free(launching_path);
	launching_path = NULL;
	running[0] = 0;
}

const char *ak_ota_running(void) { return running; }

static void relaunch(void *ctx) {
	(void)ctx;
	running[0] = 0;
	ak_module *u = ak_ota_take();
	if (u) {
		if (ak_abi_relaunch(u) == 0) {
			ak_ota_mounted();
			return;
		}
		ak_ota_failed();
	}
	ak_abi_relaunch(NULL);
}

void ak_ota_relaunch(void) { ak_after(0, relaunch, NULL); }
