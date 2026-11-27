// crash.c: platform/crash's record of the app's runs and its reports, in the
// core, for every host: what only a host sees (an uncaught exception, a
// fatal signal in native code, MetricKit's diagnostics, how Android's runs
// ended) it writes to the record's inbox, which the core reads into
// reports.
//
// A host begins the run's record as the app starts (asuka_crash_start,
// before asuka_start), with JSON of the device and the app; it's kept
// only for an app that reads its reports (one that imports platform/crash:
// lang/cgen says so; asuka dev's player always). The directory,
// crashes in the app's directory, has:
//
//	runs/<run>.crash    the core's own fatal error (ak_panic), with the
//	                    VM's stack: empty unless the run crashed
//	runs/<run>.ctx      the run's context, a JSON object a line: the run,
//	                    the device, breadcrumbs, values, foreground, and
//	                    which reports were made of it
//	inbox/<name>.json   what a host wrote of a crash it saw (an uncaught
//	                    exception; iOS's MetricKit diagnostics; how
//	                    Android's last runs ended, with a tombstone or an
//	                    ANR's traces in a .trace beside it)
//	reports/<id>.json   the reports, kept until the app deletes them
//
// The formats are the ones older versions of Asuka's apps wrote too, so a
// record an older version of an app left reads the same. A run holds a
// lock on its .crash file while it lives (flock), so a reader tells a run
// that ended from one still going (the app and its share extension).
//
// Builtins (sdk/crash's native fns):
//
//	crash.coreStarted() -> bool        the record is kept
//	crash.coreBreadcrumb(message)      written at once
//	crash.coreSet(key, value)          "" removes it
//	crash.coreRecord(message)          a nonfatal report, with the VM's stack
//	crash.coreReports() -> string      the reports, collected, as JSON of
//	                                   sdk/crash's [Report] (blocks: spawn)
//	crash.coreDelete(id)
//	crash.coreVersion() -> int         changes as reports may have come
//	crash.coreChanged(version) async   once it's no longer version
//
// The record's calls are safe on any thread.
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE  // flock, getprogname
#elif !defined(_WIN32)
#define _GNU_SOURCE  // flock
#endif
#include "std/std.h"
#include "rt/sched.h"
#include "base/thread.h"
#include "vm/vm.h"
#include "asuka.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(__wasm__)
#include <sys/file.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#endif

// ak_app_crash: the app compiled into the build reads its crash reports
// (lang/cgen sets it).
bool ak_app_crash;

#if defined(_WIN32)
// Windows: the POSIX calls the record makes, on Win32's wide calls (paths
// are UTF-8), and flock on LockFileEx. A lock there is mandatory: a live
// run's .crash file can't be read, which nothing does but to try its
// lock.
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4

typedef long long w_ssize;
#define ssize_t w_ssize

static wchar_t *wpath(const char *p) {
	int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
	if (n <= 0) n = 1;
	wchar_t *w = ak_alloc((size_t)n * sizeof *w);
	w[0] = 0;
	MultiByteToWideChar(CP_UTF8, 0, p, -1, w, n);
	return w;
}

static int w_open(const char *path, int flags, int mode) {
	(void)mode;
	wchar_t *w = wpath(path);
	int fd = _wopen(w, flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
	ak_free(w);
	return fd;
}

static int w_unlink(const char *path) {
	wchar_t *w = wpath(path);
	BOOL ok = DeleteFileW(w);
	ak_free(w);
	return ok ? 0 : -1;
}

static int w_rename(const char *from, const char *to) {
	wchar_t *a = wpath(from), *b = wpath(to);
	BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
	ak_free(a);
	ak_free(b);
	return ok ? 0 : -1;
}

static int w_mkdir(const char *path, int mode) {
	(void)mode;
	size_t n = strlen(path);
	if (n == 2 && path[1] == ':') {  // a drive
		errno = EEXIST;
		return -1;
	}
	wchar_t *w = wpath(path);
	BOOL ok = CreateDirectoryW(w, NULL);
	DWORD e = ok ? 0 : GetLastError();
	// One there already: some shares say so as access denied.
	DWORD a = ok ? 0 : GetFileAttributesW(w);
	ak_free(w);
	if (ok) return 0;
	errno = e == ERROR_ALREADY_EXISTS || (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? EEXIST : ENOENT;
	return -1;
}

static int flock(int fd, int op) {
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) return -1;
	OVERLAPPED o = {0};
	DWORD f = (op & LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (op & LOCK_NB ? LOCKFILE_FAIL_IMMEDIATELY : 0);
	if (LockFileEx(h, f, 0, MAXDWORD, MAXDWORD, &o)) return 0;
	errno = EWOULDBLOCK;
	return -1;
}

#define open(p, f, ...) w_open(p, f, 0)
#define read(fd, b, n) _read(fd, b, (unsigned)(n))
#define write(fd, b, n) _write(fd, b, (unsigned)(n))
#define close _close
#define fsync _commit
#define getpid _getpid
#define unlink w_unlink
#define rename w_rename
#define mkdir w_mkdir
#endif


#define MAX_BREADCRUMBS 100
#define MAX_REPORTS 50
#define KEEP_RUNS_MS (7LL * 24 * 3600 * 1000)

// ---- JSON values ----
//
// A small tree, for what hosts write (MetricKit's payloads, exceptions,
// exit records), the context's lines and the reports.

enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ };

typedef struct jv {
	uint8_t t;
	bool b;
	bool integral, neg;  // a number: an integer (its magnitude u), its sign
	uint64_t u;
	double n;
	char *s;  // a string, NUL-terminated (it may hold NULs: slen)
	size_t slen;
	struct jv **v;  // an array's items, an object's values
	char **k;       // an object's keys
	uint32_t len, cap;
} jv;

static jv *jv_new(uint8_t t) {
	jv *j = ak_alloc(sizeof *j);
	memset(j, 0, sizeof *j);
	j->t = t;
	return j;
}

static void jv_free(jv *j) {
	if (!j) return;
	ak_free(j->s);
	for (uint32_t i = 0; i < j->len; i++) {
		jv_free(j->v[i]);
		if (j->k) ak_free(j->k[i]);
	}
	ak_free(j->v);
	ak_free(j->k);
	ak_free(j);
}

static char *dupn(const char *s, size_t n) {
	char *p = ak_alloc(n + 1);
	if (n) memcpy(p, s, n);
	p[n] = 0;
	return p;
}

static char *sdup(const char *s) { return dupn(s ? s : "", s ? strlen(s) : 0); }

// dup_c is s for a host to free (asuka_crash_start's answer).
static char *dup_c(const char *s) {
	size_t n = strlen(s);
	char *p = malloc(n + 1);
	if (p) memcpy(p, s, n + 1);
	return p;
}

static jv *jv_string(const char *s) {
	jv *j = jv_new(JV_STR);
	j->slen = s ? strlen(s) : 0;
	j->s = dupn(s ? s : "", j->slen);
	return j;
}

static jv *jv_stringn(const uint8_t *p, size_t n) {
	jv *j = jv_new(JV_STR);
	j->slen = n;
	j->s = dupn((const char *)p, n);
	return j;
}

static jv *jv_int(int64_t i) {
	jv *j = jv_new(JV_NUM);
	j->integral = true;
	j->neg = i < 0;
	j->u = i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i;
	j->n = (double)i;
	return j;
}

static jv *jv_uint(uint64_t u) {
	jv *j = jv_new(JV_NUM);
	j->integral = true;
	j->u = u;
	j->n = (double)u;
	return j;
}

static jv *jv_bool(bool b) {
	jv *j = jv_new(JV_BOOL);
	j->b = b;
	return j;
}

static void jv_grow(jv *j) {
	if (j->len < j->cap) return;
	j->cap = j->cap ? j->cap * 2 : 4;
	j->v = ak_realloc(j->v, j->cap * sizeof *j->v);
	if (j->t == JV_OBJ) j->k = ak_realloc(j->k, j->cap * sizeof *j->k);
}

static void jv_push(jv *a, jv *x) {
	jv_grow(a);
	a->v[a->len++] = x;
}

static jv *jv_get(const jv *o, const char *key) {
	if (!o || o->t != JV_OBJ) return NULL;
	for (uint32_t i = 0; i < o->len; i++)
		if (strcmp(o->k[i], key) == 0) return o->v[i];
	return NULL;
}

// jv_set sets key (replacing what was there); x NULL removes it.
static void jv_set(jv *o, const char *key, jv *x) {
	for (uint32_t i = 0; i < o->len; i++) {
		if (strcmp(o->k[i], key) != 0) continue;
		jv_free(o->v[i]);
		if (x) {
			o->v[i] = x;
			return;
		}
		ak_free(o->k[i]);
		memmove(&o->v[i], &o->v[i + 1], (o->len - i - 1) * sizeof *o->v);
		memmove(&o->k[i], &o->k[i + 1], (o->len - i - 1) * sizeof *o->k);
		o->len--;
		return;
	}
	if (!x) return;
	jv_grow(o);
	o->k[o->len] = sdup(key);
	o->v[o->len++] = x;
}

static const char *jv_s(const jv *j) { return j && j->t == JV_STR ? j->s : ""; }
static const char *jv_sget(const jv *o, const char *key) { return jv_s(jv_get(o, key)); }

// jv_i and jv_u read a number as an integer, saturating: what hosts
// write may be beyond the type's range.
static int64_t jv_i(const jv *j) {
	if (!j || j->t != JV_NUM) return 0;
	if (!j->integral) {
		if (j->n != j->n) return 0;
		if (j->n >= 0x1p63) return INT64_MAX;
		if (j->n < -0x1p63) return INT64_MIN;
		return (int64_t)j->n;
	}
	if (!j->neg) return j->u > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)j->u;
	if (j->u > (uint64_t)INT64_MAX) return INT64_MIN;  // -2^63 itself, or below it
	return -(int64_t)j->u;
}

static uint64_t jv_u(const jv *j) {
	if (!j || j->t != JV_NUM || j->neg) return 0;
	if (j->integral) return j->u;
	if (!(j->n > 0)) return 0;  // NaN too
	return j->n >= 0x1p64 ? UINT64_MAX : (uint64_t)j->n;
}

static bool jv_b(const jv *j) { return j && j->t == JV_BOOL && j->b; }

typedef struct {
	const char *p, *end;
	int depth;
	bool err;
} jp;

static void ws(jp *r) {
	while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) r->p++;
}

static unsigned hex4(jp *r) {
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		if (r->p >= r->end) {
			r->err = true;
			return 0;
		}
		char c = *r->p++;
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else r->err = true;
	}
	return v;
}

static char *pstring(jp *r, size_t *len) {
	ak_buf b = {0};
	r->p++;  // "
	while (r->p < r->end && *r->p != '"') {
		char c = *r->p++;
		if (c != '\\') {
			ak_buf_u8(&b, (uint8_t)c);
			continue;
		}
		if (r->p >= r->end) break;
		c = *r->p++;
		switch (c) {
		case 'n': ak_buf_u8(&b, '\n'); break;
		case 't': ak_buf_u8(&b, '\t'); break;
		case 'r': ak_buf_u8(&b, '\r'); break;
		case 'b': ak_buf_u8(&b, '\b'); break;
		case 'f': ak_buf_u8(&b, '\f'); break;
		case 'u': {
			unsigned cp = hex4(r);
			if (cp >= 0xd800 && cp < 0xdc00 && r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
				r->p += 2;
				unsigned lo = hex4(r);
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
			}
			char out[4];
			ak_buf_put(&b, out, ak_utf8_encode(cp, out));
			break;
		}
		default: ak_buf_u8(&b, (uint8_t)c); break;
		}
	}
	if (r->p >= r->end) r->err = true;
	else r->p++;
	*len = b.len;
	char *s = dupn(b.data ? (const char *)b.data : "", b.len);
	ak_buf_free(&b);
	return s;
}

static jv *pvalue(jp *r) {
	ws(r);
	if (r->p >= r->end || ++r->depth > 200) {
		r->err = true;
		return NULL;
	}
	jv *j = NULL;
	char c = *r->p;
	if (c == '{' || c == '[') {
		bool obj = c == '{';
		j = jv_new(obj ? JV_OBJ : JV_ARR);
		r->p++;
		ws(r);
		if (r->p < r->end && *r->p == (obj ? '}' : ']')) r->p++;
		else
			for (;;) {
				char *key = NULL;
				if (obj) {
					ws(r);
					size_t kl;
					if (r->p >= r->end || *r->p != '"') {
						r->err = true;
						break;
					}
					key = pstring(r, &kl);
					ws(r);
					if (r->p >= r->end || *r->p != ':') {
						ak_free(key);
						r->err = true;
						break;
					}
					r->p++;
				}
				jv *x = pvalue(r);
				if (r->err) {
					ak_free(key);
					jv_free(x);
					break;
				}
				jv_grow(j);
				if (obj) j->k[j->len] = key;
				j->v[j->len++] = x;
				ws(r);
				if (r->p < r->end && *r->p == ',') {
					r->p++;
					continue;
				}
				if (r->p < r->end && *r->p == (obj ? '}' : ']')) {
					r->p++;
					break;
				}
				r->err = true;
				break;
			}
	} else if (c == '"') {
		j = jv_new(JV_STR);
		j->s = pstring(r, &j->slen);
	} else if (r->end - r->p >= 4 && memcmp(r->p, "true", 4) == 0) {
		j = jv_bool(true);
		r->p += 4;
	} else if (r->end - r->p >= 5 && memcmp(r->p, "false", 5) == 0) {
		j = jv_bool(false);
		r->p += 5;
	} else if (r->end - r->p >= 4 && memcmp(r->p, "null", 4) == 0) {
		j = jv_new(JV_NULL);
		r->p += 4;
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		const char *s = r->p;
		while (r->p < r->end && strchr("+-0123456789.eE", *r->p)) r->p++;
		char num[64];
		size_t n = (size_t)(r->p - s) < sizeof num - 1 ? (size_t)(r->p - s) : sizeof num - 1;
		memcpy(num, s, n);
		num[n] = 0;
		j = jv_new(JV_NUM);
		j->n = strtod(num, NULL);
		j->neg = num[0] == '-';
		if (!strpbrk(num, ".eE")) {
			j->integral = true;
			j->u = strtoull(num + j->neg, NULL, 10);
		}
	} else {
		r->err = true;
	}
	r->depth--;
	return j;
}

// jv_parse parses text; NULL when it isn't JSON.
static jv *jv_parse(const char *p, size_t n) {
	jp r = {p, p + n, 0, false};
	jv *j = pvalue(&r);
	ws(&r);
	if (r.err || r.p != r.end) {
		jv_free(j);
		return NULL;
	}
	return j;
}

static void w_str(ak_buf *b, const char *s, size_t n) {
	ak_buf_u8(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') {
			ak_buf_u8(b, '\\');
			ak_buf_u8(b, c);
		} else if (c == '\n') {
			ak_buf_put(b, "\\n", 2);
		} else if (c == '\t') {
			ak_buf_put(b, "\\t", 2);
		} else if (c == '\r') {
			ak_buf_put(b, "\\r", 2);
		} else if (c < 0x20) {
			ak_buf_printf(b, "\\u%04x", c);
		} else {
			ak_buf_u8(b, c);
		}
	}
	ak_buf_u8(b, '"');
}

static void w_cstr(ak_buf *b, const char *s) { w_str(b, s ? s : "", s ? strlen(s) : 0); }

static void jv_write(ak_buf *b, const jv *j) {
	if (!j) {
		ak_buf_put(b, "null", 4);
		return;
	}
	switch (j->t) {
	case JV_NULL: ak_buf_put(b, "null", 4); break;
	case JV_BOOL: ak_buf_printf(b, "%s", j->b ? "true" : "false"); break;
	case JV_NUM:
		if (j->integral) ak_buf_printf(b, "%s%llu", j->neg ? "-" : "", (unsigned long long)j->u);
		else ak_buf_printf(b, "%.17g", j->n);
		break;
	case JV_STR: w_str(b, j->s, j->slen); break;
	case JV_ARR:
	case JV_OBJ:
		ak_buf_u8(b, j->t == JV_ARR ? '[' : '{');
		for (uint32_t i = 0; i < j->len; i++) {
			if (i) ak_buf_u8(b, ',');
			if (j->t == JV_OBJ) {
				w_cstr(b, j->k[i]);
				ak_buf_u8(b, ':');
			}
			jv_write(b, j->v[i]);
		}
		ak_buf_u8(b, j->t == JV_ARR ? ']' : '}');
		break;
	}
}

// ---- Times ----

static int64_t days_from_civil(int64_t y, int m, int d) {
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = yoe + era * 400 + (*m <= 2);
}

// rfc3339 writes ms (since 1970) as RFC 3339, in UTC: what Go's
// time.Time reads.
static void rfc3339(int64_t ms, char out[40]) {
	// Days and the ms into the day, floored without multiplying back: ms
	// is whatever a host wrote, int64's ends too.
	int64_t days = ms / 86400000, rem = ms % 86400000;
	if (rem < 0) rem += 86400000, days--;
	int64_t y;
	int m, d;
	civil_from_days(days, &y, &m, &d);
	snprintf(out, 40, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", (long long)y, m, d, (int)(rem / 3600000), (int)(rem / 60000 % 60), (int)(rem / 1000 % 60), (int)(rem % 1000));
}

static int digits(const char **p, int n) {
	int v = 0;
	for (int i = 0; i < n; i++) {
		if (**p < '0' || **p > '9') return -1;
		v = v * 10 + (*(*p)++ - '0');
	}
	return v;
}

// parse_time reads "2006-01-02T15:04:05.999999999Z07:00" (or with a space
// for the T, MetricKit's): ms since 1970, the zone as it says; without a
// zone (local), *local is set and the time is as if UTC. 0 when it can't.
static int64_t parse_time(const char *s, bool *local) {
	const char *p = s;
	int y = digits(&p, 4);
	if (y < 0 || *p++ != '-') return 0;
	int mo = digits(&p, 2);
	if (mo < 0 || *p++ != '-') return 0;
	int d = digits(&p, 2);
	if (d < 0 || (*p != 'T' && *p != ' ')) return 0;
	p++;
	int h = digits(&p, 2);
	if (h < 0 || *p++ != ':') return 0;
	int mi = digits(&p, 2);
	if (mi < 0 || *p++ != ':') return 0;
	int se = digits(&p, 2);
	if (se < 0) return 0;
	int64_t ms = (days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se) * 1000;
	if (*p == '.') {
		p++;
		int64_t scale = 100;
		while (*p >= '0' && *p <= '9') {
			ms += (*p++ - '0') * scale;
			scale /= 10;
		}
	}
	while (*p == ' ') p++;
	if (local) *local = false;
	if (*p == 'Z') return ms;
	if (*p == '+' || *p == '-') {
		int sign = *p++ == '-' ? -1 : 1;
		int oh = digits(&p, 2);
		if (*p == ':') p++;
		int om = digits(&p, 2);
		if (oh < 0 || om < 0) return 0;
		return ms - sign * (int64_t)(oh * 3600 + om * 60) * 1000;
	}
	if (local) *local = true;
	return ms;
}

// local_time reads a time without a zone as the device's.
static int64_t local_time(const char *s) {
	bool local = false;
	int64_t ms = parse_time(s, &local);
	if (!ms || !local) return ms;
	int64_t t = ms / 1000;
	int32_t off = ak_zone_offset(t - ak_zone_offset(t, NULL), NULL);
	return ms - (int64_t)off * 1000;
}

static int64_t now_ms(void) { return ak_wall_ns() / 1000000; }

// ---- Files ----

static char *path_join(const char *dir, const char *name) {
	size_t n = strlen(dir) + strlen(name) + 2;
	char *p = ak_alloc(n);
	snprintf(p, n, "%s/%s", dir, name);
	return p;
}

static bool mkdirs(const char *path) {
	char buf[2048];
	size_t n = strlen(path);
	if (n >= sizeof buf) return false;
	memcpy(buf, path, n + 1);
	for (size_t i = 1; i <= n; i++) {
		if (buf[i] != '/' && buf[i] != '\\' && buf[i] != 0) continue;
		char c = buf[i];
		buf[i] = 0;
		if (mkdir(buf, 0700) != 0 && errno != EEXIST) return false;
		buf[i] = c;
	}
	return true;
}

// read_all reads a file whole (ak_free it); NULL when it can't.
static char *read_all(const char *path, size_t *len) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) return NULL;
	ak_buf b = {0};
	for (;;) {
		ak_buf_reserve(&b, 65536);
		ssize_t got = read(fd, b.data + b.len, 65536);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) break;
		b.len += (size_t)got;
	}
	close(fd);
	ak_buf_u8(&b, 0);
	*len = b.len - 1;
	return (char *)b.data;
}

static bool write_all(int fd, const void *p, size_t n) {
	const char *c = p;
	while (n) {
		ssize_t w = write(fd, c, n);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return false;
		c += w;
		n -= (size_t)w;
	}
	return true;
}

// write_file writes a file whole or not at all (a temporary, renamed).
static bool write_file(const char *path, const void *p, size_t n) {
	size_t pl = strlen(path) + 5;
	char *tmp = ak_alloc(pl);
	snprintf(tmp, pl, "%s.tmp", path);
	int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	bool ok = fd >= 0 && write_all(fd, p, n);
	if (fd >= 0) close(fd);
	ok = ok && rename(tmp, path) == 0;
	if (!ok) unlink(tmp);
	ak_free(tmp);
	return ok;
}

#if defined(_WIN32)
static bool exists(const char *path) {
	wchar_t *w = wpath(path);
	bool ok = GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
	ak_free(w);
	return ok;
}

static int64_t mtime_ms(const char *path) {
	WIN32_FILE_ATTRIBUTE_DATA a;
	wchar_t *w = wpath(path);
	BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
	ak_free(w);
	if (!ok) return 0;
	// 100 ns since 1601 to milliseconds since 1970.
	uint64_t t = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
	return (int64_t)(t / 10000) - 11644473600000LL;
}
#else
static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static int64_t mtime_ms(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return 0;
#if defined(__APPLE__)
	return (int64_t)st.st_mtimespec.tv_sec * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
	return (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
}
#endif

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// list_dir is a directory's names, sorted (free with free_names).
static char **list_dir(const char *path, uint32_t *count) {
	*count = 0;
	AK_VEC(char *) v = {0};
#if defined(_WIN32)
	char *pat = path_join(path, "*");
	wchar_t *w = wpath(pat);
	ak_free(pat);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(w, &fd);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) return NULL;
	do {
		if (fd.cFileName[0] == '.') continue;
		int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, NULL, 0, NULL, NULL);
		if (n <= 0) continue;
		char *name = ak_alloc((size_t)n);
		WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, n, NULL, NULL);
		ak_vec_push(&v, name);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(path);
	if (!d) return NULL;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		ak_vec_push(&v, sdup(e->d_name));
	}
	closedir(d);
#endif
	if (v.len > 1) qsort(v.data, v.len, sizeof *v.data, cmp_names);
	*count = v.len;
	return v.data;
}

static void free_names(char **names, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) ak_free(names[i]);
	ak_free(names);
}

static bool has_suffix(const char *s, const char *suf) {
	size_t n = strlen(s), k = strlen(suf);
	return n >= k && strcmp(s + n - k, suf) == 0;
}

static bool has_prefix(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }

// ---- The run's record ----

typedef struct {
	char *key, *value;
} kv;

typedef struct {
	int64_t time;
	char *message;
} crumb;

static ak_mutex mu = AK_MUTEX_INIT;
static bool on;          // asuka_crash_start ran
static char *dir;        // crashes, in the app's directory
static char run_id[64];  // this run's: its start in ms, its pid
static int crash_fd = -1, ctx_fd = -1;
static char *ctx_path;
static int appended;
static int64_t started;
static int pid;
static char process[64] = "app";
static jv *info;  // the device and the app (an object)
static AK_VEC(crumb) crumbs;
static AK_VEC(kv) values;
static bool fg;
static _Atomic int64_t version;

#if (defined(__aarch64__) || defined(__arm64__)) && defined(__ILP32__)
#define ARCH "arm64_32" // an Apple Watch's: its own slice of the executable
#elif defined(__aarch64__) || defined(__arm64__)
#define ARCH "arm64"
#elif defined(__x86_64__)
#define ARCH "amd64"
#elif defined(__arm__)
#define ARCH "arm"
#elif defined(__i386__)
#define ARCH "386"
#elif defined(__wasm__)
#define ARCH "wasm"
#else
#define ARCH ""
#endif

// The process: "share" in the app's share sheet (an iOS share extension,
// Android's :share process), "app" otherwise.
static void find_process(void) {
	const char *share = getenv("ASUKA_SHARE_SHEET");
	if (share && strcmp(share, "1") == 0) {
		snprintf(process, sizeof process, "share");
		return;
	}
#if defined(__APPLE__)
	char exe[4096];
	uint32_t size = sizeof exe;
	if (_NSGetExecutablePath(exe, &size) == 0 && strstr(exe, ".appex/")) snprintf(process, sizeof process, "share");
#elif defined(__ANDROID__) || defined(__linux__)
	size_t n;
	char *cmd = read_all("/proc/self/cmdline", &n);
	if (cmd) {
		char *colon = strchr(cmd, ':');
		if (colon && colon[1]) snprintf(process, sizeof process, "%s", colon + 1);
		ak_free(cmd);
	}
#endif
}

static void line(ak_buf *b, const char *key, jv *value) {
	b->len = 0;
	ak_buf_u8(b, '{');
	w_cstr(b, key);
	ak_buf_u8(b, ':');
	jv_write(b, value);
	ak_buf_put(b, "}\n", 2);
}

// The context's lines (crashrun's Line), written as they change.
static jv *run_line(void) {
	jv *r = jv_new(JV_OBJ);
	char t[40];
	rfc3339(started, t);
	jv_set(r, "pid", jv_int(pid));
	jv_set(r, "start", jv_string(t));
	jv_set(r, "process", jv_string(process));
	jv_set(r, "arch", jv_string(ARCH));
	return r;
}

static void append_line(const char *key, jv *value) {
	ak_buf b = {0};
	line(&b, key, value);
	jv_free(value);
	write_all(ctx_fd, b.data, b.len);
	ak_buf_free(&b);
	appended++;
}

static void append_raw(ak_buf *b) {
	write_all(ctx_fd, b->data, b->len);
	appended++;
}

// compact writes the context whole, with only what it holds now, once it
// has twice the lines it needs, so it stays small however many
// breadcrumbs the run adds. mu is held.
static void compact(void) {
	if (appended <= 2 * (MAX_BREADCRUMBS + (int)values.len) + 8) return;
	ak_buf all = {0}, b = {0};
	line(&b, "run", run_line());
	ak_buf_put(&all, b.data, b.len);
	if (info && info->len) {
		b.len = 0;
		ak_buf_put(&b, "{\"info\":", 8);
		jv_write(&b, info);
		ak_buf_put(&b, "}\n", 2);
		ak_buf_put(&all, b.data, b.len);
	}
	if (fg) ak_buf_put(&all, "{\"fg\":true}\n", 12);
	for (uint32_t i = 0; i < values.len; i++) {
		b.len = 0;
		ak_buf_put(&b, "{\"k\":", 5);
		w_cstr(&b, values.data[i].key);
		ak_buf_put(&b, ",\"v\":", 5);
		w_cstr(&b, values.data[i].value);
		ak_buf_put(&b, "}\n", 2);
		ak_buf_put(&all, b.data, b.len);
	}
	for (uint32_t i = 0; i < crumbs.len; i++) {
		b.len = 0;
		ak_buf_put(&b, "{\"b\":", 5);
		w_cstr(&b, crumbs.data[i].message);
		ak_buf_printf(&b, ",\"t\":%lld}\n", (long long)crumbs.data[i].time);
		ak_buf_put(&all, b.data, b.len);
	}
	ak_buf_free(&b);
	if (write_file(ctx_path, all.data, all.len)) {
		int fd = open(ctx_path, O_WRONLY | O_APPEND);
		if (fd >= 0) {
			close(ctx_fd);
			ctx_fd = fd;
			appended = (int)(crumbs.len + values.len) + 3;
		}
	}
	ak_buf_free(&all);
}

static void info_line(void) {
	ak_buf b = {0};
	ak_buf_put(&b, "{\"info\":", 8);
	jv_write(&b, info);
	ak_buf_put(&b, "}\n", 2);
	append_raw(&b);
	ak_buf_free(&b);
}

// The core's own fatal error (ak_panic), into the run's .crash file: its
// message and the VM's stack, as it ends. No allocation (the heap may be
// what failed). It runs on the thread that panicked, one panic at a time
// (ak_panic sees to it), and its stack is that thread's own (the VM's
// coroutines are per thread: a spawn's worker's is the spawned code's).
static void on_panic(const char *msg) {
	int fd = crash_fd;
	if (fd < 0) return;
	static char stack[8192];
	write_all(fd, "panic: ", 7);
	write_all(fd, msg, strlen(msg));
	write_all(fd, "\n\n", 2);
	ak_vm_traceback(stack, sizeof stack);
	write_all(fd, stack, strlen(stack));
	fsync(fd);
}

// enabled: the app reads its crash reports (lang/cgen's flag), or no app
// is compiled in (asuka dev's player, which runs any).
static bool enabled(void) { return ak_app_loader == NULL || ak_app_crash; }

static void merge_info(const jv *in) {
	if (!info) info = jv_new(JV_OBJ);
	static const char *const keys[] = {"model", "system", "systemVersion", "appVersion", "build"};
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		const char *v = jv_sget(in, keys[i]);
		if (*v) jv_set(info, keys[i], jv_string(v));
	}
}

// begin opens the run's files and points the core's panics at them. mu
// is held.
static void begin(void) {
	char *app = ak_fs_app_dir();
	dir = path_join(app, "crashes");
	ak_free(app);
	started = now_ms();
#if defined(__wasm__)
	// A page has no process ID: one of its own names its run.
	uint32_t r = 0;
	ak_os_random(&r, sizeof r);
	pid = (int)(1 + r % 0x7ffffffe);
#else
	pid = (int)getpid();
#endif
	find_process();
	char *runs = path_join(dir, "runs"), *inbox = path_join(dir, "inbox");
	bool ok = mkdirs(runs) && mkdirs(inbox);
	ak_free(inbox);
	if (!ok) {
		ak_free(runs);
		return;
	}
	snprintf(run_id, sizeof run_id, "%lld-%d", (long long)started, pid);
	char name[96];
	snprintf(name, sizeof name, "%s.crash", run_id);
	char *crash = path_join(runs, name);
	crash_fd = open(crash, O_CREAT | O_WRONLY | O_APPEND, 0600);
	ak_free(crash);
	if (crash_fd < 0) {
		ak_free(runs);
		return;
	}
#if !defined(__wasm__)
	flock(crash_fd, LOCK_EX | LOCK_NB);
#endif
	ak_panic_hook = on_panic;
	snprintf(name, sizeof name, "%s.ctx", run_id);
	ctx_path = path_join(runs, name);
	ak_free(runs);
	ctx_fd = open(ctx_path, O_CREAT | O_WRONLY | O_APPEND, 0600);
	if (ctx_fd < 0) return;
	append_line("run", run_line());
}

static bool recording(void) { return on && ctx_fd >= 0; }

// reading: the record's reports can be read here (this process keeps it,
// or, on the web, a spawn worker was told where the page's is).
static bool reading(void) {
	ak_mutex_lock(&mu);
	bool r = recording() || (!on && dir != NULL);
	ak_mutex_unlock(&mu);
	return r;
}

#if defined(__wasm__)
#define EXPORT(name) __attribute__((export_name(name), visibility("default")))

// asuka_crash_where is the record's directory and this run, a line each
// ("" when it isn't kept): for spawn's workers, which read its reports.
EXPORT("asuka_crash_where") const char *asuka_crash_where(void) {
	static char where[1200];
	ak_mutex_lock(&mu);
	snprintf(where, sizeof where, "%s\n%s", recording() ? dir : "", recording() ? run_id : "");
	ak_mutex_unlock(&mu);
	return where;
}

// asuka_crash_read makes a spawn worker's core read the page's record
// (asuka_crash_where's), keeping none of its own.
EXPORT("asuka_crash_read") void asuka_crash_read(const char *where) {
	const char *nl = strchr(where, '\n');
	if (!nl || nl == where) return;
	ak_mutex_lock(&mu);
	if (!on) {
		ak_free(dir);
		dir = dupn(where, (size_t)(nl - where));
		snprintf(run_id, sizeof run_id, "%s", nl + 1);
	}
	ak_mutex_unlock(&mu);
}
#endif

char *asuka_crash_start(const char *info_json) {
	if (!enabled()) return dup_c("");
	ak_mutex_lock(&mu);
	if (!on) {
		on = true;
		begin();
	}
	jv *in = info_json ? jv_parse(info_json, strlen(info_json)) : NULL;
	if (in && in->t == JV_OBJ) {
		merge_info(in);
		if (ctx_fd >= 0) info_line();
		jv *f = jv_get(in, "foreground");
		if (f && f->t == JV_BOOL && f->b != fg && ctx_fd >= 0) {
			fg = f->b;
			append_line("fg", jv_bool(fg));
		}
	}
	jv_free(in);
	char *out = dup_c(ctx_fd >= 0 ? dir : "");
	ak_mutex_unlock(&mu);
	return out;
}

static void breadcrumb(const char *msg) {
	ak_mutex_lock(&mu);
	if (recording()) {
		crumb c = {now_ms(), sdup(msg)};
		ak_vec_push(&crumbs, c);
		if (crumbs.len > MAX_BREADCRUMBS) {
			ak_free(crumbs.data[0].message);
			memmove(crumbs.data, crumbs.data + 1, (crumbs.len - 1) * sizeof *crumbs.data);
			crumbs.len--;
		}
		ak_buf b = {0};
		ak_buf_put(&b, "{\"b\":", 5);
		w_cstr(&b, msg);
		ak_buf_printf(&b, ",\"t\":%lld}\n", (long long)c.time);
		append_raw(&b);
		ak_buf_free(&b);
		compact();
	}
	ak_mutex_unlock(&mu);
}

static void set_value(const char *key, const char *value) {
	ak_mutex_lock(&mu);
	if (recording()) {
		int at = -1;
		for (uint32_t i = 0; i < values.len; i++)
			if (strcmp(values.data[i].key, key) == 0) at = (int)i;
		bool same = at >= 0 ? strcmp(values.data[at].value, value) == 0 : !*value;
		if (!same) {
			ak_buf b = {0};
			ak_buf_put(&b, "{\"k\":", 5);
			w_cstr(&b, key);
			if (*value) {
				ak_buf_put(&b, ",\"v\":", 5);
				w_cstr(&b, value);
				if (at >= 0) {
					ak_free(values.data[at].value);
					values.data[at].value = sdup(value);
				} else {
					kv x = {sdup(key), sdup(value)};
					ak_vec_push(&values, x);
				}
			} else {
				ak_free(values.data[at].key);
				ak_free(values.data[at].value);
				values.data[at] = values.data[--values.len];
			}
			ak_buf_put(&b, "}\n", 2);
			append_raw(&b);
			ak_buf_free(&b);
			compact();
		}
	}
	ak_mutex_unlock(&mu);
}

void ak_crash_lifecycle(int state) {
	if (!on) return;
	const char *msg = NULL;
	ak_mutex_lock(&mu);
	if (recording() && state <= 1 && fg != (state == 0)) {
		fg = state == 0;
		append_line("fg", jv_bool(fg));
	}
	ak_mutex_unlock(&mu);
	switch (state) {
	case 0: msg = "app: foreground"; break;
	case 1: msg = "app: background"; break;
	case 2: msg = "app: memory warning"; break;
	}
	if (msg) breadcrumb(msg);
}

// ---- Waiting for reports ----

typedef struct waiter {
	ak_future *fut;
	int64_t seen;
	struct waiter *next;
} waiter;

static waiter *waiters;  // main thread only

static void wake_waiters(void *ctx) {
	AK_UNUSED(ctx);
	int64_t v = version;
	for (waiter **p = &waiters; *p;) {
		waiter *w = *p;
		if (w->seen == v && w->fut->state == AK_FUT_PENDING) {
			p = &w->next;
			continue;
		}
		*p = w->next;
		ak_future_resolve(w->fut, (ak_val){.i = v});
		ak_release(w->fut);
		ak_free(w);
	}
}

// changed says reports may have come (a host wrote one, the app recorded
// one): what waits reads them again. Any thread.
static void changed(void) {
	version++;
	ak_post(wake_waiters, NULL);
}

void asuka_crash_notify(void) { changed(); }

// ---- Reports ----
//
// A report is a JSON object in the kept reports' format (times RFC 3339,
// an uptime in nanoseconds): what's kept in reports/. Made of a frame's
// and a thread's objects too.

static jv *frame_obj(const char *function, const char *file, int64_t line_no, const char *binary, const char *binary_id, uint64_t address, uint64_t offset) {
	jv *f = jv_new(JV_OBJ);
	if (function && *function) jv_set(f, "function", jv_string(function));
	if (file && *file) jv_set(f, "file", jv_string(file));
	if (line_no) jv_set(f, "line", jv_int(line_no));
	if (binary && *binary) jv_set(f, "binary", jv_string(binary));
	if (binary_id && *binary_id) jv_set(f, "binaryId", jv_string(binary_id));
	if (address) jv_set(f, "address", jv_uint(address));
	if (offset) jv_set(f, "offset", jv_uint(offset));
	return f;
}

static jv *thread_obj(const char *name, bool crashed) {
	jv *t = jv_new(JV_OBJ);
	jv_set(t, "name", jv_string(name));
	if (crashed) jv_set(t, "crashed", jv_bool(true));
	jv_set(t, "frames", jv_new(JV_ARR));
	return t;
}

static void add_frame(jv *thread, jv *frame) { jv_push(jv_get(thread, "frames"), frame); }

static jv *report_obj(const char *id, const char *kind, bool fatal, int64_t time, const char *message, const char *text) {
	jv *r = jv_new(JV_OBJ);
	char t[40];
	rfc3339(time, t);
	jv_set(r, "id", jv_string(id));
	jv_set(r, "kind", jv_string(kind));
	jv_set(r, "fatal", jv_bool(fatal));
	jv_set(r, "time", jv_string(t));
	jv_set(r, "message", jv_string(message));
	jv_set(r, "text", jv_string(text));
	jv_set(r, "threads", jv_new(JV_ARR));
	return r;
}

static int64_t report_time(const jv *r) { return parse_time(jv_sget(r, "time"), NULL); }

// vm_thread reads ak_vm_traceback's lines ("name (file:line)") into a thread.
static jv *vm_thread(const char *stack) {
	jv *t = thread_obj("main", true);
	for (const char *p = stack; *p;) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		char *l = dupn(p, n);
		char *open = strrchr(l, '(');
		char *colon = open ? strrchr(open, ':') : NULL;
		if (open && colon && open > l) {
			open[-1] = 0;
			*colon = 0;
			add_frame(t, frame_obj(l, open + 1, atoll(colon + 1), NULL, NULL, 0, 0));
		} else if (n) {
			add_frame(t, frame_obj(l, NULL, 0, NULL, NULL, 0, 0));
		}
		ak_free(l);
		p = nl ? nl + 1 : p + n;
	}
	return t;
}

// The run's context, now: for a report of this run (record).
static jv *context_now(int64_t at) {
	jv *c = jv_new(JV_OBJ);
	ak_mutex_lock(&mu);
	static const char *const keys[] = {"appVersion", "build", "model", "system", "systemVersion"};
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		const char *v = jv_sget(info, keys[i]);
		if (*v) jv_set(c, keys[i], jv_string(v));
	}
	char t[40];
	rfc3339(started, t);
	jv_set(c, "arch", jv_string(ARCH));
	jv_set(c, "process", jv_string(process));
	jv_set(c, "pid", jv_int(pid));
	jv_set(c, "started", jv_string(t));
	if (at > started) jv_set(c, "uptime", jv_int((at - started) * 1000000));
	jv_set(c, "foreground", jv_bool(fg));
	jv *bs = jv_new(JV_ARR);
	for (uint32_t i = 0; i < crumbs.len; i++) {
		jv *b = jv_new(JV_OBJ);
		rfc3339(crumbs.data[i].time, t);
		jv_set(b, "time", jv_string(t));
		jv_set(b, "message", jv_string(crumbs.data[i].message));
		jv_push(bs, b);
	}
	jv_set(c, "breadcrumbs", bs);
	if (values.len) {
		jv *vs = jv_new(JV_OBJ);
		for (uint32_t i = 0; i < values.len; i++) jv_set(vs, values.data[i].key, jv_string(values.data[i].value));
		jv_set(c, "values", vs);
	}
	ak_mutex_unlock(&mu);
	return c;
}

static bool save_report(const char *base, const jv *r) {
	char *reports = path_join(base, "reports");
	mkdirs(reports);
	char name[300];
	snprintf(name, sizeof name, "%s.json", jv_sget(r, "id"));
	char *path = path_join(reports, name);
	ak_buf b = {0};
	jv_write(&b, r);
	bool ok = write_file(path, b.data, b.len);
	ak_buf_free(&b);
	ak_free(path);
	ak_free(reports);
	return ok;
}

// ---- Collecting ----

typedef struct {
	jv *run;     // its first line's object
	jv *info;    // the device and the app
	bool fg;
	jv *values;  // an object
	jv *crumbs;  // an array of lines ({b, t})
	AK_VEC(char *) reported;
} run_ctx;

typedef struct {
	char id[64];
	int64_t start;
	int pid;
	bool read;
	run_ctx ctx;
} run_file;

typedef struct {
	char *dir;
	const char *current;
	AK_VEC(run_file) runs;
	AK_VEC(jv *) kept;
} collector;

// A report made of what a host wrote, before its context.
typedef struct record {
	jv *rep;
	int pid;
	bool exact;               // its time is when it happened (not a period's end)
	const char *unless[3];    // left out if the run has a report of one of these kinds
	int fg;                   // -1: not said
	jv *info;                 // the device and the app, as the record says (MetricKit's)
	char run[64];             // the run it was in, if the host said
} record;

typedef AK_VEC(record) records;

static char *run_path(collector *c, const run_file *r, const char *ext) {
	char name[96];
	snprintf(name, sizeof name, "runs/%s%s", r->id, ext);
	return path_join(c->dir, name);
}

static void read_ctx(const char *path, run_ctx *rc) {
	memset(rc, 0, sizeof *rc);
	rc->values = jv_new(JV_OBJ);
	rc->crumbs = jv_new(JV_ARR);
	size_t n;
	char *text = read_all(path, &n);
	if (!text) return;
	for (char *p = text; *p;) {
		char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		jv *l = jv_parse(p, len);  // a line cut short as the process ended: none
		p = nl ? nl + 1 : p + len;
		if (!l || l->t != JV_OBJ) {
			jv_free(l);
			continue;
		}
		jv *x;
		if ((x = jv_get(l, "run")) && x->t == JV_OBJ) {
			// Taken from the line, which keeps a null in its place.
			jv_free(rc->run);
			rc->run = x;
			for (uint32_t i = 0; i < l->len; i++)
				if (l->v[i] == x) l->v[i] = jv_new(JV_NULL);
		} else if ((x = jv_get(l, "info")) && x->t == JV_OBJ) {
			if (!rc->info) rc->info = jv_new(JV_OBJ);
			for (uint32_t i = 0; i < x->len; i++)
				if (x->v[i]->t == JV_STR && *x->v[i]->s) jv_set(rc->info, x->k[i], jv_string(x->v[i]->s));
		} else if ((x = jv_get(l, "fg")) && x->t == JV_BOOL) {
			rc->fg = x->b;
		} else if ((x = jv_get(l, "k")) && x->t == JV_STR && *x->s) {
			jv *v = jv_get(l, "v");
			jv_set(rc->values, x->s, v && v->t == JV_STR ? jv_string(v->s) : NULL);
		} else if ((x = jv_get(l, "reported")) && x->t == JV_STR && *x->s) {
			ak_vec_push(&rc->reported, sdup(x->s));
		} else if (jv_i(jv_get(l, "t"))) {
			jv *b = jv_new(JV_OBJ);
			jv_set(b, "b", jv_string(jv_sget(l, "b")));
			jv_set(b, "t", jv_int(jv_i(jv_get(l, "t"))));
			jv_push(rc->crumbs, b);
		}
		jv_free(l);
	}
	ak_free(text);
}

static void free_ctx(run_ctx *rc) {
	jv_free(rc->run);
	jv_free(rc->info);
	jv_free(rc->values);
	jv_free(rc->crumbs);
	for (uint32_t i = 0; i < rc->reported.len; i++) ak_free(rc->reported.data[i]);
	ak_free(rc->reported.data);
}

static run_ctx *context_of(collector *c, run_file *r) {
	if (!r->read) {
		char *p = run_path(c, r, ".ctx");
		read_ctx(p, &r->ctx);
		ak_free(p);
		r->read = true;
	}
	return &r->ctx;
}

static bool was_reported(run_ctx *rc, const char *kind) {
	for (uint32_t i = 0; i < rc->reported.len; i++)
		if (strcmp(rc->reported.data[i], kind) == 0) return true;
	return false;
}

static int cmp_runs(const void *a, const void *b) {
	const run_file *x = a, *y = b;
	return x->start < y->start ? -1 : x->start > y->start;
}

static void list_runs(collector *c) {
	char *runs = path_join(c->dir, "runs");
	uint32_t n;
	char **names = list_dir(runs, &n);
	ak_free(runs);
	for (uint32_t i = 0; i < n; i++) {
		const char *name = names[i];
		if (has_suffix(name, ".tmp")) continue;
		const char *dot = strrchr(name, '.');
		size_t idlen = dot ? (size_t)(dot - name) : strlen(name);
		if (idlen >= sizeof c->runs.data->id) continue;
		char id[64];
		memcpy(id, name, idlen);
		id[idlen] = 0;
		bool seen = false;
		for (uint32_t j = 0; j < c->runs.len && !seen; j++) seen = strcmp(c->runs.data[j].id, id) == 0;
		char *dash = strchr(id, '-');
		if (seen || !dash) continue;
		char *e1, *e2;
		long long ms = strtoll(id, &e1, 10);
		long p = strtol(dash + 1, &e2, 10);
		if (e1 != dash || *e2) continue;
		run_file r;
		memset(&r, 0, sizeof r);
		snprintf(r.id, sizeof r.id, "%s", id);
		r.start = ms;
		r.pid = (int)p;
		ak_vec_push(&c->runs, r);
	}
	free_names(names, n);
	if (c->runs.len > 1) qsort(c->runs.data, c->runs.len, sizeof *c->runs.data, cmp_runs);
}

#if defined(__wasm__)
// The web: a page marks its run alive (crash.js), which the page says.
__attribute__((import_module("asuka"), import_name("crash_alive"))) int js_crash_alive(const char *path, uint32_t len);
#endif

// alive reports whether a run's .crash file is locked: its process is
// still going (on the web, its page).
static bool alive(const char *path) {
#if defined(__wasm__)
	return js_crash_alive(path, (uint32_t)strlen(path)) != 0;
#else
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	bool locked = flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
	close(fd);
	return locked;
#endif
}

static void info_into(jv *ctx, const jv *i, bool over) {
	static const char *const keys[] = {"model", "system", "systemVersion", "appVersion", "build"};
	for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
		const char *v = jv_sget(i, keys[k]);
		if (*v && (over || !*jv_sget(ctx, keys[k]))) jv_set(ctx, keys[k], jv_string(v));
	}
}

// attach gives rep run r's context (NULL: none known).
static void attach(collector *c, jv *rep, run_file *r) {
	if (!r) return;
	run_ctx *rc = context_of(c, r);
	jv *ctx = jv_new(JV_OBJ);
	int64_t start = r->start;
	if (rc->run) {
		int64_t t = parse_time(jv_sget(rc->run, "start"), NULL);
		if (t) start = t;
		if (*jv_sget(rc->run, "process")) jv_set(ctx, "process", jv_string(jv_sget(rc->run, "process")));
		if (*jv_sget(rc->run, "arch")) jv_set(ctx, "arch", jv_string(jv_sget(rc->run, "arch")));
	}
	jv_set(ctx, "pid", jv_int(r->pid));
	char t[40];
	rfc3339(start, t);
	jv_set(ctx, "started", jv_string(t));
	jv_set(ctx, "foreground", jv_bool(rc->fg));
	if (rc->info) info_into(ctx, rc->info, true);
	jv *bs = jv_new(JV_ARR);
	uint32_t from = rc->crumbs->len > MAX_BREADCRUMBS ? rc->crumbs->len - MAX_BREADCRUMBS : 0;
	for (uint32_t i = from; i < rc->crumbs->len; i++) {
		jv *b = jv_new(JV_OBJ);
		rfc3339(jv_i(jv_get(rc->crumbs->v[i], "t")), t);
		jv_set(b, "time", jv_string(t));
		jv_set(b, "message", jv_string(jv_sget(rc->crumbs->v[i], "b")));
		jv_push(bs, b);
	}
	jv_set(ctx, "breadcrumbs", bs);
	if (rc->values->len) {
		jv *vs = jv_new(JV_OBJ);
		for (uint32_t i = 0; i < rc->values->len; i++) jv_set(vs, rc->values->k[i], jv_string(jv_s(rc->values->v[i])));
		jv_set(ctx, "values", vs);
	}
	int64_t when = report_time(rep);
	if (when > start) jv_set(ctx, "uptime", jv_int((when - start) * 1000000));
	jv_set(rep, "context", ctx);
}

// mark notes in r's context that a report of kind was made of it.
static void mark(collector *c, run_file *r, const char *kind) {
	run_ctx *rc = context_of(c, r);
	if (was_reported(rc, kind)) return;
	ak_vec_push(&rc->reported, sdup(kind));
	char *p = run_path(c, r, ".ctx");
	int fd = open(p, O_WRONLY | O_APPEND);
	ak_free(p);
	if (fd < 0) return;
	ak_buf b = {0};
	ak_buf_put(&b, "{\"reported\":", 12);
	w_cstr(&b, kind);
	ak_buf_put(&b, "}\n", 2);
	write_all(fd, b.data, b.len);
	ak_buf_free(&b);
	close(fd);
}

// keep saves rep (taken) among the reports; false when it couldn't be.
static bool keep(collector *c, jv *rep) {
	if (!save_report(c->dir, rep)) {
		jv_free(rep);
		return false;
	}
	const char *id = jv_sget(rep, "id");
	for (uint32_t i = 0; i < c->kept.len; i++) {
		if (strcmp(jv_sget(c->kept.data[i], "id"), id) == 0) {
			jv_free(c->kept.data[i]);
			c->kept.data[i] = rep;
			return true;
		}
	}
	ak_vec_push(&c->kept, rep);
	return true;
}

// pending reports whether a report of kind from run r is kept.
static bool pending(collector *c, run_file *r, const char *kind) {
	for (uint32_t i = 0; i < c->kept.len; i++) {
		jv *k = c->kept.data[i], *ctx = jv_get(k, "context");
		if (strcmp(jv_sget(k, "kind"), kind) == 0 && jv_i(jv_get(ctx, "pid")) == r->pid && parse_time(jv_sget(ctx, "started"), NULL) == r->start) return true;
	}
	return false;
}

// match is the run of process pid (0: not known) that was running at t:
// the last to start before it. Not this one: a host reports what ended
// before.
static run_file *match(collector *c, int pid_, int64_t t) {
	run_file *best = NULL;
	for (uint32_t i = 0; i < c->runs.len; i++) {
		run_file *r = &c->runs.data[i];
		if (strcmp(r->id, c->current) == 0 || (pid_ != 0 && r->pid != pid_)) continue;
		if (t && r->start > t + 1000) continue;
		if (!best || r->start > best->start) best = r;
	}
	return best;
}

static run_file *by_id(collector *c, const char *id) {
	for (uint32_t i = 0; i < c->runs.len; i++)
		if (strcmp(c->runs.data[i].id, id) == 0) return &c->runs.data[i];
	return NULL;
}

// add saves rec's report (taking it), with the context of the run it was
// in; false when it couldn't be saved.
static bool add(collector *c, record *rec) {
	run_file *r = rec->run[0] ? by_id(c, rec->run) : match(c, rec->pid, report_time(rec->rep));
	if (r) {
		for (int i = 0; i < 3 && rec->unless[i]; i++) {
			if (was_reported(context_of(c, r), rec->unless[i]) || pending(c, r, rec->unless[i])) {
				jv_free(rec->rep);
				jv_free(rec->info);
				return true;
			}
		}
	}
	jv *rep = rec->rep;
	attach(c, rep, r);
	jv *ctx = jv_get(rep, "context");
	if (!ctx) {
		ctx = jv_new(JV_OBJ);
		jv_set(rep, "context", ctx);
	}
	if (!rec->exact) jv_set(ctx, "uptime", NULL);
	if (rec->fg >= 0) jv_set(ctx, "foreground", jv_bool(rec->fg));
	if (rec->info) info_into(ctx, rec->info, r == NULL);
	jv_free(rec->info);
	if (!jv_i(jv_get(ctx, "pid")) && rec->pid) jv_set(ctx, "pid", jv_int(rec->pid));
	char kind[32];
	snprintf(kind, sizeof kind, "%s", jv_sget(rep, "kind"));
	if (!keep(c, rep)) return false;
	if (r && strcmp(r->id, c->current) != 0) mark(c, r, kind);
	return true;
}

// core_crashes makes a report of each run that ended with the core's
// fatal error in its .crash file, and drops the .crash of each that
// ended.
static void core_crashes(collector *c) {
	for (uint32_t i = 0; i < c->runs.len; i++) {
		run_file *r = &c->runs.data[i];
		if (strcmp(r->id, c->current) == 0) continue;
		char *path = run_path(c, r, ".crash");
		if (!exists(path) || alive(path)) {
			ak_free(path);
			continue;
		}
		size_t n;
		char *out = read_all(path, &n);
		const char *s = out ? out : "";
		while (*s == ' ' || *s == '\n' || *s == '\t') s++;
		bool kept = true;
		if (*s) {
			const char *nl = strchr(s, '\n');
			char *msg = dupn(s, nl ? (size_t)(nl - s) : strlen(s));
			char id[96];
			snprintf(id, sizeof id, "%s-%s", has_prefix(s, "panic: ") ? "panic" : "go", r->id);
			// Go's crash output (an earlier version's record) or the core's.
			const char *kind = has_prefix(s, "panic: ") && !strstr(s, "goroutine ") ? "panic" : "go";
			jv *rep = report_obj(id, kind, true, mtime_ms(path), msg, s);
			ak_free(msg);
			if (strcmp(kind, "panic") == 0) {
				const char *stack = strstr(s, "\n\n");
				if (stack) jv_push(jv_get(rep, "threads"), vm_thread(stack + 2));
			}
			attach(c, rep, r);
			kept = keep(c, rep);
			if (kept) mark(c, r, kind);
		}
		ak_free(out);
		if (kept) {
			unlink(path);
			char *stderr_path = run_path(c, r, ".stderr");
			unlink(stderr_path);
			ak_free(stderr_path);
		}
		ak_free(path);
	}
}

// ---- What hosts write ----

static const char *signal_name(int64_t s) {
	static const char *const names[] = {NULL, "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGEMT", "SIGFPE", "SIGKILL", "SIGBUS", "SIGSEGV", "SIGSYS", "SIGPIPE", "SIGALRM", "SIGTERM"};
	return s > 0 && s < (int64_t)(sizeof names / sizeof *names) ? names[s] : NULL;
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r') s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
	return s;
}

// native_frame reads an Android native frame: "#00 pc 000000000004a1c0
// /data/app/.../libasuka.so (f+12) (BuildId: 1a2b)".
static jv *native_frame(const char *l) {
	char pcs[32], bin[1024];
	int used = 0;
	if (l[0] != '#' || sscanf(l, "%*s pc %31s %1023s%n", pcs, bin, &used) < 2) return NULL;
	char *e;
	uint64_t pc = strtoull(pcs, &e, 16);
	if (*e) return NULL;
	char *rest = sdup(l + used), *fn = NULL, *build = NULL;
	char *b = strstr(rest, "(BuildId: ");
	if (b) {
		build = trim(b + 10);
		size_t n = strlen(build);
		if (n && build[n - 1] == ')') build[n - 1] = 0;
		*b = 0;
	}
	char *r = trim(rest);
	if (*r == '(') {
		fn = r + 1;
		size_t n = strlen(fn);
		if (n && fn[n - 1] == ')') fn[--n] = 0;
		char *plus = strrchr(fn, '+');
		if (plus && plus > fn) *plus = 0;
	}
	jv *f = frame_obj(fn, NULL, 0, bin, build, 0, pc);
	ak_free(rest);
	return f;
}

// jvm_frame reads "at a.B.c(B.kt:12)", "at a.B.c(Native Method)", and an
// ANR trace's "native: #00 pc ...".
static jv *jvm_frame(const char *l) {
	if (has_prefix(l, "at ")) {
		char *s = sdup(l + 3);
		char *open = strchr(s, '('), *file = NULL;
		int64_t line_no = 0;
		if (open) {
			*open = 0;
			char *loc = open + 1;
			size_t n = strlen(loc);
			if (n && loc[n - 1] == ')') loc[n - 1] = 0;
			char *colon = strchr(loc, ':');
			if (colon) {
				*colon = 0;
				file = loc;
				line_no = atoll(colon + 1);
			} else if (strcmp(loc, "Native Method") != 0 && strcmp(loc, "Unknown Source") != 0) {
				file = loc;
			}
		}
		jv *f = frame_obj(s, file, line_no, NULL, NULL, 0, 0);
		ak_free(s);
		return f;
	}
	if (has_prefix(l, "native: ")) return native_frame(l + 8);
	return NULL;
}

// jvm_frames adds a stack trace's frames, the exception's own (before any
// "Caused by:"), to thread t.
static void jvm_frames(jv *t, const char *stack) {
	char *s = sdup(stack);
	for (char *p = s, *next; p; p = next) {
		next = strchr(p, '\n');
		if (next) *next++ = 0;
		char *l = trim(p);
		if (has_prefix(l, "Caused by:")) break;
		jv *f = jvm_frame(l);
		if (f) add_frame(t, f);
	}
	ak_free(s);
}

static void exception_records(const char *id, const char *text, size_t n, records *out) {
	jv *e = jv_parse(text, n);
	if (!e || e->t != JV_OBJ) {
		jv_free(e);
		return;
	}
	ak_buf msg = {0}, body = {0};
	ak_buf_printf(&msg, "%s", jv_sget(e, "name"));
	if (*jv_sget(e, "reason")) ak_buf_printf(&msg, ": %s", jv_sget(e, "reason"));
	ak_buf_u8(&msg, 0);
	ak_buf_printf(&body, "%s\n\n%s", (char *)msg.data, jv_sget(e, "stack"));
	jv *crashed = thread_obj(jv_sget(e, "thread"), true);
	jv *frames = jv_get(e, "frames");
	if (frames && frames->t == JV_ARR && frames->len) {
		for (uint32_t i = 0; i < frames->len; i++) {
			jv *f = frames->v[i];
			add_frame(crashed, frame_obj(jv_sget(f, "function"), jv_sget(f, "file"), jv_i(jv_get(f, "line")), jv_sget(f, "binary"), jv_sget(f, "binaryId"), jv_u(jv_get(f, "address")), jv_u(jv_get(f, "offset"))));
		}
	} else {
		jvm_frames(crashed, jv_sget(e, "stack"));
	}
	jv *threads = jv_new(JV_ARR);
	jv_push(threads, crashed);
	jv *others = jv_get(e, "threads");
	for (uint32_t i = 0; others && others->t == JV_ARR && i < others->len; i++) {
		jv *th = others->v[i];
		ak_buf_printf(&body, "\n\n\"%s\"\n%s", jv_sget(th, "name"), jv_sget(th, "stack"));
		jv *t = thread_obj(jv_sget(th, "name"), false);
		jvm_frames(t, jv_sget(th, "stack"));
		jv_push(threads, t);
	}
	ak_buf_u8(&body, 0);
	jv *fatal = jv_get(e, "fatal");
	record rec = {0};
	rec.rep = report_obj(id, "exception", !fatal || fatal->t != JV_BOOL || fatal->b, jv_i(jv_get(e, "time")), (char *)msg.data, (char *)body.data);
	jv_set(rec.rep, "threads", threads);
	rec.pid = (int)jv_i(jv_get(e, "pid"));
	rec.exact = true;
	rec.fg = -1;
	snprintf(rec.run, sizeof rec.run, "%s", jv_sget(e, "run"));
	ak_vec_push(out, rec);
	ak_buf_free(&msg);
	ak_buf_free(&body);
	jv_free(e);
}

// MetricKit's diagnostic payloads (MXDiagnosticPayload's
// jsonRepresentation), as the iOS host writes each it gets.

static const char *mach_exception(int64_t t) {
	static const char *const names[] = {NULL, "EXC_BAD_ACCESS", "EXC_BAD_INSTRUCTION", "EXC_ARITHMETIC", "EXC_EMULATION", "EXC_SOFTWARE", "EXC_BREAKPOINT", "EXC_SYSCALL", "EXC_MACH_SYSCALL", "EXC_RPC_ALERT", "EXC_CRASH", "EXC_RESOURCE", "EXC_GUARD", "EXC_CORPSE_NOTIFY"};
	return t > 0 && t < (int64_t)(sizeof names / sizeof *names) ? names[t] : NULL;
}

// mx_threads reads a call stack tree: a stack each thread (a crash's), or
// samples merged into a tree (a hang's, a CPU or disk exception's), whose
// heaviest path is a stack. The root frame is the innermost; the crashed
// thread comes first.
static jv *mx_threads(const jv *tree) {
	jv *out = jv_new(JV_ARR);
	bool per_thread = jv_b(jv_get(tree, "callStackPerThread"));
	const jv *stacks = jv_get(tree, "callStacks");
	for (uint32_t i = 0; stacks && stacks->t == JV_ARR && i < stacks->len; i++) {
		const jv *s = stacks->v[i];
		char name[32];
		snprintf(name, sizeof name, "%s %u", per_thread ? "Thread" : "Samples", i);
		bool crashed = jv_b(jv_get(s, "threadAttributed"));
		jv *t = thread_obj(name, crashed);
		const jv *at = jv_get(s, "callStackRootFrames");
		while (at && at->t == JV_ARR && at->len) {
			const jv *f = at->v[0];
			for (uint32_t k = 1; k < at->len; k++)
				if (jv_i(jv_get(at->v[k], "sampleCount")) > jv_i(jv_get(f, "sampleCount"))) f = at->v[k];
			add_frame(t, frame_obj(NULL, NULL, 0, jv_sget(f, "binaryName"), jv_sget(f, "binaryUUID"), jv_u(jv_get(f, "address")), jv_u(jv_get(f, "offsetIntoBinaryTextSegment"))));
			at = jv_get(f, "subFrames");
		}
		if (crashed && out->len) {
			jv_push(out, NULL);
			memmove(out->v + 1, out->v, (out->len - 1) * sizeof *out->v);
			out->v[0] = t;
		} else {
			jv_push(out, t);
		}
	}
	return out;
}

static void mx_text(ak_buf *b, const char *msg, const jv *meta, const jv *threads, const jv *payload) {
	ak_buf_printf(b, "%s\n", msg);
	char app[256];
	snprintf(app, sizeof app, "%s (%s)", jv_sget(meta, "appVersion"), jv_sget(meta, "appBuildVersion"));
	struct { const char *k, *v; } lines[] = {
		{"App", app}, {"OS", jv_sget(meta, "osVersion")}, {"Device", jv_sget(meta, "deviceType")},
	};
	for (size_t i = 0; i < sizeof lines / sizeof *lines; i++)
		if (*lines[i].v) ak_buf_printf(b, "%s: %s\n", lines[i].k, lines[i].v);
	const jv *code = jv_get(meta, "exceptionCode");
	if (code && code->t == JV_NUM) ak_buf_printf(b, "Exception code: %#llx\n", (unsigned long long)jv_i(code));
	if (*jv_sget(meta, "terminationReason")) ak_buf_printf(b, "Termination reason: %s\n", jv_sget(meta, "terminationReason"));
	if (*jv_sget(meta, "virtualMemoryRegionInfo")) ak_buf_printf(b, "Memory region: %s\n", jv_sget(meta, "virtualMemoryRegionInfo"));
	const jv *objc = jv_get(meta, "objectiveCexceptionReason");
	if (objc && *jv_sget(objc, "className")) ak_buf_printf(b, "Exception class: %s\n", jv_sget(objc, "className"));
	ak_buf_printf(b, "Period: %s – %s\n", jv_sget(payload, "timeStampBegin"), jv_sget(payload, "timeStampEnd"));
	jv *images = jv_new(JV_OBJ);
	for (uint32_t i = 0; i < threads->len; i++) {
		const jv *t = threads->v[i], *fs = jv_get(t, "frames");
		ak_buf_printf(b, "\n%s%s:\n", jv_sget(t, "name"), jv_b(jv_get(t, "crashed")) ? " Crashed" : "");
		for (uint32_t k = 0; k < fs->len; k++) {
			const jv *f = fs->v[k];
			ak_buf_printf(b, "%-3u %-30s 0x%016llx %s + %llu\n", k, jv_sget(f, "binary"), (unsigned long long)jv_u(jv_get(f, "address")), jv_sget(f, "binary"), (unsigned long long)jv_u(jv_get(f, "offset")));
			const char *uuid = jv_sget(f, "binaryId");
			if (*uuid && !jv_get(images, uuid)) jv_set(images, uuid, jv_string(jv_sget(f, "binary")));
		}
	}
	if (images->len) {
		ak_buf_printf(b, "\nBinary Images:\n");
		for (uint32_t i = 0; i < images->len; i++) ak_buf_printf(b, "%s %s\n", images->k[i], jv_s(images->v[i]));
	}
	jv_free(images);
}

// mx_os splits "iPhone OS 26.0 (23A341)" or "macOS 15.1 (24B83)" into the
// system and its version (an info object's).
static void mx_os(jv *info_, const char *os) {
	char s[256];
	snprintf(s, sizeof s, "%s", os);
	char *paren = strstr(s, " (");
	if (paren) *paren = 0;
	char *sp = strrchr(s, ' ');
	if (!sp) {
		jv_set(info_, "systemVersion", jv_string(s));
		return;
	}
	*sp = 0;
	const char *sys = s;
	if (strcmp(sys, "iPhone OS") == 0) sys = "iOS";
	else if (strcmp(sys, "Mac OS X") == 0 || strcmp(sys, "Mac OS") == 0) sys = "macOS";
	jv_set(info_, "system", jv_string(sys));
	jv_set(info_, "systemVersion", jv_string(sp + 1));
}

static void lower(char *s) {
	for (; *s; s++)
		if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static void mx_records(const char *id, const char *text, size_t n, records *out) {
	jv *p = jv_parse(text, n);
	if (!p || p->t != JV_OBJ) {
		jv_free(p);
		return;
	}
	int64_t end = local_time(jv_sget(p, "timeStampEnd"));
	if (!end) end = local_time(jv_sget(p, "timeStampBegin"));
	static const struct {
		const char *key;
	} groups[] = {{"crashDiagnostics"}, {"hangDiagnostics"}, {"cpuExceptionDiagnostics"}, {"diskWriteExceptionDiagnostics"}, {"appLaunchDiagnostics"}};
	uint32_t made = 0;
	for (size_t g = 0; g < sizeof groups / sizeof *groups; g++) {
		const jv *ds = jv_get(p, groups[g].key);
		for (uint32_t i = 0; ds && ds->t == JV_ARR && i < ds->len; i++) {
			const jv *d = ds->v[i], *m = jv_get(d, "diagnosticMetaData");
			char msg[600];
			const char *kind = "native";
			bool fatal = g == 0, abort_ = false;
			if (g == 0) {
				const jv *objc = jv_get(m, "objectiveCexceptionReason");
				const jv *et = jv_get(m, "exceptionType"), *sig = jv_get(m, "signal");
				if (objc && (*jv_sget(objc, "exceptionName") || *jv_sget(objc, "composedMessage"))) {
					kind = "exception";
					snprintf(msg, sizeof msg, "%s%s%s", jv_sget(objc, "exceptionName"), *jv_sget(objc, "composedMessage") ? ": " : "", jv_sget(objc, "composedMessage"));
				} else {
					char what[200] = "";
					if (et && et->t == JV_NUM) {
						const char *name = mach_exception(jv_i(et));
						if (name) snprintf(what, sizeof what, "%s", name);
						else snprintf(what, sizeof what, "exception %lld", (long long)jv_i(et));
					}
					if (sig && sig->t == JV_NUM) {
						const char *name = signal_name(jv_i(sig));
						char s[48];
						if (name) snprintf(s, sizeof s, "%s", name);
						else snprintf(s, sizeof s, "signal %lld", (long long)jv_i(sig));
						size_t k = strlen(what);
						snprintf(what + k, sizeof what - k, k ? " (%s)" : "%s", s);
						abort_ = jv_i(sig) == 6;
					}
					snprintf(msg, sizeof msg, "%s", *what ? what : "Crash");
					char reason[400];
					snprintf(reason, sizeof reason, "%s", jv_sget(m, "terminationReason"));
					lower(reason);
					if (strstr(reason, "8badf00d")) {
						kind = "hang";
						snprintf(msg, sizeof msg, "Watchdog: the app didn't answer in time (%s)", jv_sget(m, "terminationReason"));
					} else if (strstr(reason, "jetsam") || strstr(reason, "per-process-limit") || strstr(reason, "memory limit")) {
						kind = "oom";
						snprintf(msg, sizeof msg, "Memory limit: %s", jv_sget(m, "terminationReason"));
					}
				}
			} else if (g == 1) {
				kind = "hang";
				snprintf(msg, sizeof msg, "Hang: the main thread was busy for %s", jv_sget(m, "hangDuration"));
			} else if (g == 2) {
				kind = "resource";
				snprintf(msg, sizeof msg, "CPU: %s in %s", jv_sget(m, "totalCPUTime"), jv_sget(m, "totalSampledTime"));
			} else if (g == 3) {
				kind = "resource";
				snprintf(msg, sizeof msg, "Disk writes: %s", jv_sget(m, "writesCaused"));
			} else {
				kind = "hang";
				snprintf(msg, sizeof msg, "Slow launch: %s", jv_sget(m, "launchDuration"));
			}
			char rid[300];
			snprintf(rid, sizeof rid, "%s-%u", id, made++);
			jv *threads = mx_threads(jv_get(d, "callStackTree"));
			ak_buf body = {0};
			mx_text(&body, msg, m, threads, p);
			ak_buf_u8(&body, 0);
			record rec = {0};
			rec.rep = report_obj(rid, kind, fatal, end, msg, (char *)body.data);
			ak_buf_free(&body);
			jv_set(rec.rep, "threads", threads);
			rec.pid = (int)jv_i(jv_get(m, "pid"));
			rec.fg = -1;
			rec.info = jv_new(JV_OBJ);
			if (*jv_sget(m, "deviceType")) jv_set(rec.info, "model", jv_string(jv_sget(m, "deviceType")));
			if (*jv_sget(m, "appVersion")) jv_set(rec.info, "appVersion", jv_string(jv_sget(m, "appVersion")));
			if (*jv_sget(m, "appBuildVersion")) jv_set(rec.info, "build", jv_string(jv_sget(m, "appBuildVersion")));
			if (*jv_sget(m, "osVersion")) mx_os(rec.info, jv_sget(m, "osVersion"));
			if (strcmp(kind, "exception") == 0 || abort_) {
				// The exception the app's handler reported, or the core's
				// panic that aborted: the same crash.
				rec.unless[0] = "exception", rec.unless[1] = "panic", rec.unless[2] = "native";
			}
			ak_vec_push(out, rec);
		}
	}
	jv_free(p);
}

// ---- Android's tombstones (AOSP's tombstone.proto, what a report needs) ----

typedef struct {
	const uint8_t *p, *end;
	bool err;
} pb;

static uint64_t pb_varint(pb *r) {
	uint64_t v = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		if (r->p >= r->end) {
			r->err = true;
			return 0;
		}
		uint8_t b = *r->p++;
		v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80)) return v;
	}
	r->err = true;
	return 0;
}

// pb_field reads the next field: its number and its value (a varint or a
// fixed one) or its bytes (length-delimited).
static bool pb_field(pb *r, int *num, uint64_t *v, const uint8_t **b, size_t *n) {
	if (r->p >= r->end || r->err) return false;
	uint64_t key = pb_varint(r);
	*num = (int)(key >> 3);
	*v = 0, *b = NULL, *n = 0;
	switch (key & 7) {
	case 0: *v = pb_varint(r); break;
	case 1:
		if (r->end - r->p < 8) return r->err = true, false;
		for (int i = 0; i < 8; i++) *v |= (uint64_t)r->p[i] << (8 * i);
		r->p += 8;
		break;
	case 2: {
		uint64_t len = pb_varint(r);
		if (r->err || len > (uint64_t)(r->end - r->p)) return r->err = true, false;
		*b = r->p, *n = (size_t)len;
		r->p += len;
		break;
	}
	case 5:
		if (r->end - r->p < 4) return r->err = true, false;
		for (int i = 0; i < 4; i++) *v |= (uint64_t)r->p[i] << (8 * i);
		r->p += 4;
		break;
	default: return r->err = true, false;
	}
	return !r->err;
}

typedef struct {
	uint64_t id;
	char *name;
	jv *frames;  // frame objects ({function, file, functionOffset, buildId, pc, relPc} as tombstone fields)
	AK_VEC(char *) notes;
} ts_thread;

typedef struct {
	char *fingerprint, *abort, *signame, *code;
	uint64_t pid, tid, uptime, fault;
	int64_t signo, codeno, sender;
	bool has_signal, has_fault, has_sender;
	AK_VEC(char *) command;
	AK_VEC(char *) causes;
	AK_VEC(ts_thread) threads;
} tombstone;

static jv *ts_frame(const uint8_t *p, size_t n) {
	pb r = {p, p + n, false};
	int num;
	uint64_t v;
	const uint8_t *b;
	size_t k;
	jv *f = jv_new(JV_OBJ);
	while (pb_field(&r, &num, &v, &b, &k)) {
		switch (num) {
		case 1: jv_set(f, "rel", jv_uint(v)); break;
		case 2: jv_set(f, "pc", jv_uint(v)); break;
		case 4: jv_set(f, "function", jv_stringn(b, k)); break;
		case 5: jv_set(f, "off", jv_uint(v)); break;
		case 6: jv_set(f, "file", jv_stringn(b, k)); break;
		case 8: jv_set(f, "build", jv_stringn(b, k)); break;
		}
	}
	return f;
}

static void ts_free(tombstone *t) {
	ak_free(t->fingerprint), ak_free(t->abort), ak_free(t->signame), ak_free(t->code);
	for (uint32_t i = 0; i < t->command.len; i++) ak_free(t->command.data[i]);
	for (uint32_t i = 0; i < t->causes.len; i++) ak_free(t->causes.data[i]);
	for (uint32_t i = 0; i < t->threads.len; i++) {
		ak_free(t->threads.data[i].name);
		jv_free(t->threads.data[i].frames);
		for (uint32_t k = 0; k < t->threads.data[i].notes.len; k++) ak_free(t->threads.data[i].notes.data[k]);
		ak_free(t->threads.data[i].notes.data);
	}
	ak_free(t->command.data), ak_free(t->causes.data), ak_free(t->threads.data);
}

static bool ts_decode(const uint8_t *p, size_t n, tombstone *t) {
	memset(t, 0, sizeof *t);
	if (!n) return false;
	pb r = {p, p + n, false};
	int num;
	uint64_t v;
	const uint8_t *b;
	size_t k;
	while (pb_field(&r, &num, &v, &b, &k)) {
		switch (num) {
		case 2: ak_free(t->fingerprint), t->fingerprint = dupn((const char *)b, k); break;
		case 5: t->pid = v; break;
		case 6: t->tid = v; break;
		case 9: ak_vec_push(&t->command, dupn((const char *)b, k)); break;
		case 10: {
			t->has_signal = true;
			pb s = {b, b + k, false};
			int sn;
			uint64_t sv;
			const uint8_t *sb;
			size_t sk;
			while (pb_field(&s, &sn, &sv, &sb, &sk)) {
				switch (sn) {
				case 1: t->signo = (int32_t)sv; break;
				case 2: ak_free(t->signame), t->signame = dupn((const char *)sb, sk); break;
				case 3: t->codeno = (int32_t)sv; break;
				case 4: ak_free(t->code), t->code = dupn((const char *)sb, sk); break;
				case 5: t->has_sender = sv != 0; break;
				case 7: t->sender = (int32_t)sv; break;
				case 8: t->has_fault = sv != 0; break;
				case 9: t->fault = sv; break;
				}
			}
			break;
		}
		case 14: ak_free(t->abort), t->abort = dupn((const char *)b, k); break;
		case 15: {
			pb c = {b, b + k, false};
			int cn;
			uint64_t cv;
			const uint8_t *cb;
			size_t ck;
			while (pb_field(&c, &cn, &cv, &cb, &ck)) {
				if (cn == 1) {
					ak_vec_push(&t->causes, dupn((const char *)cb, ck));
					break;
				}
			}
			break;
		}
		case 16: {
			// map<uint32, Thread>: an entry each, key 1 and value 2.
			pb e = {b, b + k, false};
			int en;
			uint64_t ev;
			const uint8_t *eb;
			size_t ek;
			ts_thread th = {0};
			th.frames = jv_new(JV_ARR);
			uint64_t key = 0;
			while (pb_field(&e, &en, &ev, &eb, &ek)) {
				if (en == 1) key = ev;
				if (en != 2) continue;
				pb tr = {eb, eb + ek, false};
				int tn;
				uint64_t tv;
				const uint8_t *tb;
				size_t tk;
				while (pb_field(&tr, &tn, &tv, &tb, &tk)) {
					switch (tn) {
					case 1: th.id = (uint64_t)(int32_t)tv; break;
					case 2: ak_free(th.name), th.name = dupn((const char *)tb, tk); break;
					case 4: jv_push(th.frames, ts_frame(tb, tk)); break;
					case 7: ak_vec_push(&th.notes, dupn((const char *)tb, tk)); break;
					}
				}
			}
			if (!th.id) th.id = key;
			if (!th.name) th.name = sdup("");
			ak_vec_push(&t->threads, th);
			break;
		}
		case 20: t->uptime = v; break;
		}
	}
	if (r.err || (t->pid == 0 && t->threads.len == 0)) {
		ts_free(t);
		return false;
	}
	return true;
}

static int cmp_ts_threads(const void *a, const void *b) {
	const ts_thread *x = a, *y = b;
	return x->id < y->id ? -1 : x->id > y->id;
}

// ts_order puts the crashed thread first, the rest by ID.
static void ts_order(tombstone *t) {
	if (t->threads.len > 1) qsort(t->threads.data, t->threads.len, sizeof *t->threads.data, cmp_ts_threads);
	for (uint32_t i = 0; i < t->threads.len; i++) {
		if (t->threads.data[i].id != t->tid) continue;
		ts_thread th = t->threads.data[i];
		memmove(t->threads.data + 1, t->threads.data, i * sizeof *t->threads.data);
		t->threads.data[0] = th;
		break;
	}
}

static void ts_message(const tombstone *t, char *out, size_t n) {
	if (!t->has_signal) {
		snprintf(out, n, "Native crash");
		return;
	}
	const char *name = t->signame && *t->signame ? t->signame : signal_name(t->signo);
	size_t k = name ? (size_t)snprintf(out, n, "%s", name) : (size_t)snprintf(out, n, "signal %lld", (long long)t->signo);
	if (t->code && *t->code && k < n) k += (size_t)snprintf(out + k, n - k, " (%s)", t->code);
	if (k < n) {
		if (t->abort && *t->abort) snprintf(out + k, n - k, ": %s", t->abort);
		else if (t->causes.len && *t->causes.data[0]) snprintf(out + k, n - k, ": %s", t->causes.data[0]);
	}
}

static void ts_text(const tombstone *t, ak_buf *b) {
	if (t->fingerprint && *t->fingerprint) ak_buf_printf(b, "Build fingerprint: '%s'\n", t->fingerprint);
	if (t->uptime) ak_buf_printf(b, "Process uptime: %llus\n", (unsigned long long)t->uptime);
	const char *name = t->threads.len && t->threads.data[0].id == t->tid ? t->threads.data[0].name : "";
	ak_buf_printf(b, "pid: %llu, tid: %llu, name: %s  >>> ", (unsigned long long)t->pid, (unsigned long long)t->tid, name);
	for (uint32_t i = 0; i < t->command.len; i++) ak_buf_printf(b, "%s%s", i ? " " : "", t->command.data[i]);
	ak_buf_printf(b, " <<<\n");
	if (t->has_signal) {
		ak_buf_printf(b, "signal %lld (%s), code %lld (%s)", (long long)t->signo, t->signame ? t->signame : "", (long long)t->codeno, t->code ? t->code : "");
		if (t->has_sender) ak_buf_printf(b, " from pid %lld", (long long)t->sender);
		if (t->has_fault) ak_buf_printf(b, ", fault addr 0x%llx", (unsigned long long)t->fault);
		ak_buf_printf(b, "\n");
	}
	for (uint32_t i = 0; i < t->causes.len; i++) ak_buf_printf(b, "Cause: %s\n", t->causes.data[i]);
	if (t->abort && *t->abort) ak_buf_printf(b, "Abort message: '%s'\n", t->abort);
	for (uint32_t i = 0; i < t->threads.len; i++) {
		const ts_thread *th = &t->threads.data[i];
		if (i == 0) ak_buf_printf(b, "\nbacktrace:\n");
		else ak_buf_printf(b, "\n\"%s\" tid=%llu\n", th->name, (unsigned long long)th->id);
		for (uint32_t k = 0; k < th->notes.len; k++) ak_buf_printf(b, "    NOTE: %s\n", th->notes.data[k]);
		for (uint32_t k = 0; k < th->frames->len; k++) {
			const jv *f = th->frames->v[k];
			ak_buf_printf(b, "      #%02u pc %016llx  %s", k, (unsigned long long)jv_u(jv_get(f, "rel")), jv_sget(f, "file"));
			if (*jv_sget(f, "function")) ak_buf_printf(b, " (%s+%llu)", jv_sget(f, "function"), (unsigned long long)jv_u(jv_get(f, "off")));
			if (*jv_sget(f, "build")) ak_buf_printf(b, " (BuildId: %s)", jv_sget(f, "build"));
			ak_buf_printf(b, "\n");
		}
	}
}

static jv *ts_stacks(const tombstone *t) {
	jv *out = jv_new(JV_ARR);
	for (uint32_t i = 0; i < t->threads.len; i++) {
		const ts_thread *th = &t->threads.data[i];
		jv *x = thread_obj(th->name, th->id == t->tid);
		for (uint32_t k = 0; k < th->frames->len; k++) {
			const jv *f = th->frames->v[k];
			add_frame(x, frame_obj(jv_sget(f, "function"), NULL, 0, jv_sget(f, "file"), jv_sget(f, "build"), jv_u(jv_get(f, "pc")), jv_u(jv_get(f, "rel"))));
		}
		jv_push(out, x);
	}
	return out;
}

// anr_threads reads an ANR's traces (the Java threads' dump): each thread
// a header ("main" prio=5 tid=1 Native), its frames; main first.
static jv *anr_threads(const char *trace) {
	jv *out = jv_new(JV_ARR);
	jv *cur = NULL;
	char *s = sdup(trace);
	for (char *p = s, *next; p; p = next) {
		next = strchr(p, '\n');
		if (next) *next++ = 0;
		char *l = trim(p);
		if (has_prefix(l, "----- end")) break;
		if (*l == '"') {
			char *q = strchr(l + 1, '"');
			if (q) *q = 0;
			cur = thread_obj(l + 1, false);
			jv_push(out, cur);
			continue;
		}
		if (!cur) continue;
		if (!*l) {
			cur = NULL;
			continue;
		}
		jv *f = jvm_frame(l);
		if (f) add_frame(cur, f);
	}
	ak_free(s);
	for (uint32_t i = 0; i < out->len; i++) {
		if (strcmp(jv_sget(out->v[i], "name"), "main") != 0) continue;
		jv *m = out->v[i];
		jv_set(m, "crashed", jv_bool(true));
		memmove(out->v + 1, out->v, i * sizeof *out->v);
		out->v[0] = m;
		break;
	}
	return out;
}

// How an Android process ended (ApplicationExitInfo), as the host writes
// each on the next launch, its trace beside it.
enum { EXIT_LOW_MEMORY = 3, EXIT_CRASH = 4, EXIT_CRASH_NATIVE = 5, EXIT_ANR = 6, EXIT_INIT_FAILURE = 7, EXIT_EXCESSIVE = 9 };

static void exit_records(const char *id, const char *text, size_t n, const char *trace, size_t tn, records *out) {
	jv *e = jv_parse(text, n);
	if (!e || e->t != JV_OBJ) {
		jv_free(e);
		return;
	}
	const char *desc = jv_sget(e, "description");
	int64_t reason = jv_i(jv_get(e, "reason")), status = jv_i(jv_get(e, "status")), imp = jv_i(jv_get(e, "importance"));
	char msg[700];
	const char *kind;
	ak_buf body = {0};
	jv *threads = NULL;
	record rec = {0};
	switch (reason) {
	case EXIT_CRASH_NATIVE: {
		kind = "native";
		tombstone t;
		int64_t sig = status;
		if (ts_decode((const uint8_t *)trace, tn, &t)) {
			ts_order(&t);
			ts_message(&t, msg, sizeof msg);
			ts_text(&t, &body);
			threads = ts_stacks(&t);
			sig = t.signo;
			ts_free(&t);
		} else {
			snprintf(msg, sizeof msg, "%s", status > 0 && signal_name(status) ? signal_name(status) : "Native crash");
			ak_buf_printf(&body, "%s\n%s\n%.*s", msg, desc, (int)tn, trace ? trace : "");
		}
		// The core's panic ends with abort: the same crash.
		if (sig == 6) rec.unless[0] = "panic", rec.unless[1] = "go";
		break;
	}
	case EXIT_CRASH:
		// The app's own handler reports the exception, with its stack:
		// this is for one it didn't see.
		kind = "exception";
		snprintf(msg, sizeof msg, "%s", *desc ? desc : "Uncaught exception");
		ak_buf_printf(&body, "%s", msg);
		rec.unless[0] = "exception";
		break;
	case EXIT_ANR:
		kind = "hang";
		snprintf(msg, sizeof msg, "ANR: %s", *desc ? desc : "the app didn't answer");
		ak_buf_printf(&body, "%s\n\n%.*s", msg, (int)tn, trace ? trace : "");
		{
			char *t = dupn(trace ? trace : "", tn);
			threads = anr_threads(t);
			ak_free(t);
		}
		break;
	case EXIT_LOW_MEMORY:
		kind = "oom";
		snprintf(msg, sizeof msg, "Low memory: the system ended the app (%lld MB in use)", (long long)(jv_i(jv_get(e, "pss")) / 1024));
		ak_buf_printf(&body, "%s\n%s", msg, desc);
		break;
	case EXIT_EXCESSIVE:
		kind = "resource";
		snprintf(msg, sizeof msg, "Excessive resource use: %s", *desc ? desc : "the system ended the app");
		ak_buf_printf(&body, "%s", msg);
		break;
	case EXIT_INIT_FAILURE:
		kind = "native";
		snprintf(msg, sizeof msg, "The app failed to start: %s", *desc ? desc : "initialization failure");
		ak_buf_printf(&body, "%s", msg);
		break;
	default:
		ak_buf_free(&body);
		jv_free(e);
		return;
	}
	ak_buf_u8(&body, 0);
	rec.rep = report_obj(id, kind, true, jv_i(jv_get(e, "time")), msg, (char *)body.data);
	if (threads) jv_set(rec.rep, "threads", threads);
	ak_buf_free(&body);
	rec.pid = (int)jv_i(jv_get(e, "pid"));
	rec.exact = true;
	rec.fg = imp > 0 && imp <= 100;  // IMPORTANCE_FOREGROUND
	ak_vec_push(out, rec);
	jv_free(e);
}

// inbox makes reports of what the hosts wrote, and removes each once its
// reports are kept.
static void inbox(collector *c) {
	char *in = path_join(c->dir, "inbox");
	uint32_t n;
	char **names = list_dir(in, &n);
	for (uint32_t i = 0; i < n; i++) {
		const char *name = names[i];
		if (!has_suffix(name, ".json")) continue;
		char *base = dupn(name, strlen(name) - 5);
		char *path = path_join(in, name);
		size_t len;
		char *text = read_all(path, &len);
		records recs = {0};
		char trace_name[600];
		snprintf(trace_name, sizeof trace_name, "%s.trace", base);
		char *trace_path = path_join(in, trace_name);
		if (text) {
			if (has_prefix(base, "exception-")) exception_records(base, text, len, &recs);
			else if (has_prefix(base, "metrickit-")) mx_records(base, text, len, &recs);
			else if (has_prefix(base, "exit-")) {
				size_t tl = 0;
				char *trace = read_all(trace_path, &tl);
				exit_records(base, text, len, trace, tl, &recs);
				ak_free(trace);
			}
		}
		bool ok = text != NULL;
		for (uint32_t k = 0; k < recs.len; k++)
			if (!add(c, &recs.data[k])) ok = false;
		if (ok) {
			unlink(path);
			unlink(trace_path);
		}
		ak_free(recs.data);
		ak_free(text);
		ak_free(trace_path);
		ak_free(path);
		ak_free(base);
	}
	free_names(names, n);
	ak_free(in);
}

static void read_reports(collector *c) {
	char *dirp = path_join(c->dir, "reports");
	mkdirs(dirp);
	uint32_t n;
	char **names = list_dir(dirp, &n);
	for (uint32_t i = 0; i < n; i++) {
		if (!has_suffix(names[i], ".json")) continue;
		char *p = path_join(dirp, names[i]);
		size_t len;
		char *text = read_all(p, &len);
		jv *r = text ? jv_parse(text, len) : NULL;
		ak_free(text);
		if (!r || r->t != JV_OBJ || !*jv_sget(r, "id")) {
			jv_free(r);
			if (text) unlink(p);  // not a report: never one
		} else {
			ak_vec_push(&c->kept, r);
		}
		ak_free(p);
	}
	free_names(names, n);
	ak_free(dirp);
}

// prune_runs drops the contexts of runs that ended a while ago (what a
// host reports of them so late comes without one), and what the inbox
// kept that no reader took.
static void prune_runs(collector *c) {
	int64_t now = now_ms();
	for (uint32_t i = 0; i < c->runs.len; i++) {
		run_file *r = &c->runs.data[i];
		char *crash = run_path(c, r, ".crash"), *ctx = run_path(c, r, ".ctx");
		if (strcmp(r->id, c->current) != 0 && !exists(crash)) {
			int64_t t = mtime_ms(ctx);
			if (!t || now - t > KEEP_RUNS_MS) {
				unlink(ctx);
				char *tmp = run_path(c, r, ".ctx.tmp");
				unlink(tmp);
				ak_free(tmp);
			}
		}
		ak_free(crash);
		ak_free(ctx);
	}
	char *in = path_join(c->dir, "inbox");
	uint32_t n;
	char **names = list_dir(in, &n);
	for (uint32_t i = 0; i < n; i++) {
		char *p = path_join(in, names[i]);
		int64_t t = mtime_ms(p);
		if (t && now - t > KEEP_RUNS_MS) unlink(p);
		ak_free(p);
	}
	free_names(names, n);
	ak_free(in);
}

static int cmp_reports(const void *a, const void *b) {
	int64_t x = report_time(*(jv *const *)a), y = report_time(*(jv *const *)b);
	return x < y ? -1 : x > y;
}

// ---- sdk/crash's Report ----

static void out_str(ak_buf *b, const char *key, const char *v, bool comma) {
	if (comma) ak_buf_u8(b, ',');
	w_cstr(b, key);
	ak_buf_u8(b, ':');
	w_cstr(b, v);
}

static void out_int(ak_buf *b, const char *key, long long v) {
	ak_buf_u8(b, ',');
	w_cstr(b, key);
	ak_buf_printf(b, ":%lld", v);
}

static void out_bool(ak_buf *b, const char *key, bool v) {
	ak_buf_u8(b, ',');
	w_cstr(b, key);
	ak_buf_printf(b, ":%s", v ? "true" : "false");
}

// out_report writes a report (as kept) as sdk/crash's Report:
// times in ms, every field there.
static void out_report(ak_buf *b, const jv *r) {
	ak_buf_u8(b, '{');
	out_str(b, "id", jv_sget(r, "id"), false);
	out_str(b, "kind", jv_sget(r, "kind"), true);
	out_bool(b, "fatal", jv_b(jv_get(r, "fatal")));
	out_int(b, "time", report_time(r));
	out_str(b, "message", jv_sget(r, "message"), true);
	out_str(b, "text", jv_sget(r, "text"), true);
	ak_buf_put(b, ",\"threads\":[", 12);
	const jv *ts = jv_get(r, "threads");
	for (uint32_t i = 0; ts && ts->t == JV_ARR && i < ts->len; i++) {
		const jv *t = ts->v[i], *fs = jv_get(t, "frames");
		if (i) ak_buf_u8(b, ',');
		ak_buf_u8(b, '{');
		out_str(b, "name", jv_sget(t, "name"), false);
		out_bool(b, "crashed", jv_b(jv_get(t, "crashed")));
		ak_buf_put(b, ",\"frames\":[", 11);
		for (uint32_t k = 0; fs && fs->t == JV_ARR && k < fs->len; k++) {
			const jv *f = fs->v[k];
			if (k) ak_buf_u8(b, ',');
			ak_buf_u8(b, '{');
			out_str(b, "function", jv_sget(f, "function"), false);
			out_str(b, "file", jv_sget(f, "file"), true);
			out_int(b, "line", jv_i(jv_get(f, "line")));
			out_str(b, "binary", jv_sget(f, "binary"), true);
			out_str(b, "binaryId", jv_sget(f, "binaryId"), true);
			out_int(b, "address", (long long)jv_u(jv_get(f, "address")));
			out_int(b, "offset", (long long)jv_u(jv_get(f, "offset")));
			ak_buf_u8(b, '}');
		}
		ak_buf_put(b, "]}", 2);
	}
	ak_buf_put(b, "],\"context\":{", 13);
	const jv *c = jv_get(r, "context");
	static const char *const keys[] = {"appVersion", "build", "model", "system", "systemVersion", "arch", "process"};
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) out_str(b, keys[i], jv_sget(c, keys[i]), i > 0);
	out_int(b, "pid", jv_i(jv_get(c, "pid")));
	const char *st = jv_sget(c, "started");
	out_int(b, "started", *st ? parse_time(st, NULL) : 0);
	out_int(b, "uptime", jv_i(jv_get(c, "uptime")));
	out_bool(b, "foreground", jv_b(jv_get(c, "foreground")));
	ak_buf_put(b, ",\"breadcrumbs\":[", 16);
	const jv *bs = jv_get(c, "breadcrumbs");
	for (uint32_t i = 0; bs && bs->t == JV_ARR && i < bs->len; i++) {
		if (i) ak_buf_u8(b, ',');
		ak_buf_u8(b, '{');
		w_cstr(b, "time");
		ak_buf_printf(b, ":%lld", (long long)parse_time(jv_sget(bs->v[i], "time"), NULL));
		out_str(b, "message", jv_sget(bs->v[i], "message"), true);
		ak_buf_u8(b, '}');
	}
	ak_buf_put(b, "],\"values\":{", 12);
	const jv *vs = jv_get(c, "values");
	for (uint32_t i = 0; vs && vs->t == JV_OBJ && i < vs->len; i++) out_str(b, vs->k[i], jv_s(vs->v[i]), i > 0);
	ak_buf_put(b, "}}}", 3);
}

static ak_mutex collect_mu = AK_MUTEX_INIT;

// collect turns what the runs that ended and the hosts left into
// reports, and writes the reports kept (the oldest first, past
// MAX_REPORTS the oldest deleted) as sdk/crash's [Report] into out. One
// collection at a time in this process; between processes, under the
// directory's collect.lock.
static void collect(ak_buf *out) {
	ak_mutex_lock(&collect_mu);
	collector c = {0};
	c.dir = sdup(dir);
	c.current = run_id;
	char *lock_path = path_join(c.dir, "collect.lock");
	int lock = open(lock_path, O_CREAT | O_RDWR, 0600);
	ak_free(lock_path);
#if !defined(__wasm__)
	if (lock >= 0) flock(lock, LOCK_EX);
#endif
	list_runs(&c);
	read_reports(&c);
	core_crashes(&c);
	inbox(&c);
	prune_runs(&c);
	if (c.kept.len > 1) qsort(c.kept.data, c.kept.len, sizeof *c.kept.data, cmp_reports);
	uint32_t from = c.kept.len > MAX_REPORTS ? c.kept.len - MAX_REPORTS : 0;
	for (uint32_t i = 0; i < from; i++) {
		char name[300];
		snprintf(name, sizeof name, "reports/%s.json", jv_sget(c.kept.data[i], "id"));
		char *p = path_join(c.dir, name);
		unlink(p);
		ak_free(p);
	}
	ak_buf_u8(out, '[');
	for (uint32_t i = from; i < c.kept.len; i++) {
		if (i > from) ak_buf_u8(out, ',');
		out_report(out, c.kept.data[i]);
	}
	ak_buf_u8(out, ']');
	if (lock >= 0) close(lock);
	for (uint32_t i = 0; i < c.kept.len; i++) jv_free(c.kept.data[i]);
	ak_free(c.kept.data);
	for (uint32_t i = 0; i < c.runs.len; i++)
		if (c.runs.data[i].read) free_ctx(&c.runs.data[i].ctx);
	ak_free(c.runs.data);
	ak_free(c.dir);
	ak_mutex_unlock(&collect_mu);
}

// record_error makes a nonfatal report of an error the app handled, with
// the VM's stack: it's read with the crashes, at once.
static void record_error(const char *message) {
	int64_t now = now_ms();
	char id[96];
	static _Atomic uint32_t seq;
	snprintf(id, sizeof id, "nonfatal-%lld-%d-%u", (long long)now, pid, ++seq);
	char stack[8192];
	ak_vm_traceback(stack, sizeof stack);
	ak_buf text = {0};
	ak_buf_printf(&text, "%s\n\n%s", message, stack);
	ak_buf_u8(&text, 0);
	jv *r = report_obj(id, "nonfatal", false, now, message, (char *)text.data);
	ak_buf_free(&text);
	jv_push(jv_get(r, "threads"), vm_thread(stack));
	jv_set(r, "context", context_now(now));
	bool ok = save_report(dir, r);
	jv_free(r);
	if (ok) changed();
}

// ---- Builtins ----

#define STR(i) ((ak_str *)args[i].p)

static ak_val b_started(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	ak_mutex_lock(&mu);
	bool r = recording();
	ak_mutex_unlock(&mu);
	return (ak_val){.i = r};
}

static ak_val b_breadcrumb(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	breadcrumb(STR(0)->data);
	return (ak_val){0};
}

static ak_val b_set(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	set_value(STR(0)->data, STR(1)->data);
	return (ak_val){0};
}

static ak_val b_record(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	if (recording()) record_error(STR(0)->data);
	return (ak_val){0};
}

static ak_val b_reports(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	if (!reading()) {
		ak_throw_msg("crash: the app's crash reports aren't kept (no host started them, or its data directory can't be written)");
		return (ak_val){0};
	}
	ak_buf b = {0};
	collect(&b);
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = s};
}

static ak_val b_delete(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const char *id = STR(0)->data;
	if (!reading() || !*id || strchr(id, '/') || strcmp(id, ".") == 0 || strcmp(id, "..") == 0) return (ak_val){0};
	char name[600];
	snprintf(name, sizeof name, "reports/%s.json", id);
	char *p = path_join(dir, name);
	unlink(p);
	ak_free(p);
	return (ak_val){0};
}

static ak_val b_version(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.i = version};
}

static void cancel_wait(ak_future *f) {
	for (waiter **p = &waiters; *p; p = &(*p)->next) {
		if ((*p)->fut != f) continue;
		waiter *w = *p;
		*p = w->next;
		ak_release(w->fut);
		ak_free(w);
		break;
	}
	ak_throw_cancel();
	ak_future_reject(f, ak_error_take());
}

// crash.coreChanged(v): a future of the version once it isn't v.
static ak_val b_changed(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_future *f = ak_future_new(AK_K_INT);
	if (version != args[0].i) {
		ak_future_resolve(f, (ak_val){.i = version});
		return (ak_val){.p = f};
	}
	waiter *w = ak_alloc(sizeof *w);
	w->fut = ak_retain(f);
	w->seen = args[0].i;
	w->next = waiters;
	waiters = w;
	f->cancel = cancel_wait;
	return (ak_val){.p = f};
}


const ak_lib_entry ak_std_crash[] = {
	{"crash.coreStarted", b_started},
	{"crash.coreBreadcrumb", b_breadcrumb},
	{"crash.coreSet", b_set},
	{"crash.coreRecord", b_record},
	{"crash.coreReports", b_reports},
	{"crash.coreDelete", b_delete},
	{"crash.coreVersion", b_version},
	{"crash.coreChanged", b_changed},
	{NULL, NULL},
};
