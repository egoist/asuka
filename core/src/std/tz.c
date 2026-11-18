// tz.c: named time zones (tz.h): a name's zone, a zone's offset at an
// instant (its transitions, then its POSIX TZ string, as RFC 8536 reads
// a TZif file), a wall time's instant, and the device's zone's name.
#define _POSIX_C_SOURCE 200809L  // readlink
#include "std/tz.h"
#include "base/thread.h"
#include "rt/sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__wasm__)
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#endif

// ---- Names ----

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// cmp_fold orders s[0:n] against name as the table is sorted, by ASCII
// lowercase (no two names are the same in it).
static int cmp_fold(const char *s, size_t n, const char *name) {
	size_t i = 0;
	for (; i < n && name[i]; i++) {
		int a = lower((unsigned char)s[i]), b = lower((unsigned char)name[i]);
		if (a != b) return a - b;
	}
	if (i < n) return 1;
	return name[i] ? -1 : 0;
}

const ak_tz_name *ak_tz_find(const char *s, size_t n) {
	size_t lo = 0, hi = ak_tz_nnames;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = cmp_fold(s, n, ak_tz_strs + ak_tz_names[mid].name);
		if (c == 0) return &ak_tz_names[mid];
		if (c < 0) hi = mid;
		else lo = mid + 1;
	}
	return NULL;
}

// ---- The calendar ----

static int64_t floor_div(int64_t a, int64_t b) {
	int64_t q = a / b;
	return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// days_from_civil is the days since 1970-01-01 of y-m-d (Howard
// Hinnant's algorithm), year_of the year a day since then is in.
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
	y -= m <= 2;
	int64_t era = floor_div(y, 400);
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static int64_t year_of(int64_t days) {
	int64_t z = days + 719468;
	int64_t era = floor_div(z, 146097);
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	return yoe + era * 400 + (mp >= 10);
}

// ---- POSIX TZ strings (a TZif file's footer) ----
//
// std offset [dst [offset] [,start[/time],end[/time]]], with RFC 8536's
// extensions (hours to 167, negative rule times): a port of Go's tzset
// (time/zoneinfo.go).

typedef struct {
	const char *p, *end;
} cur;

static bool tz_num(cur *c, int lo, int hi, int *out) {
	int n = 0, k = 0;
	while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
		n = n * 10 + (*c->p++ - '0');
		k++;
		if (n > hi) return false;
	}
	if (k == 0 || n < lo) return false;
	*out = n;
	return true;
}

static bool tz_peek(const cur *c, char ch) { return c->p < c->end && *c->p == ch; }

static bool tz_name(cur *c, const char **name, uint32_t *len) {
	if (tz_peek(c, '<')) {
		const char *q = memchr(c->p + 1, '>', (size_t)(c->end - c->p - 1));
		if (!q) return false;
		*name = c->p + 1, *len = (uint32_t)(q - c->p - 1);
		c->p = q + 1;
		return true;
	}
	const char *q = c->p;
	while (q < c->end && !((*q >= '0' && *q <= '9') || *q == ',' || *q == '-' || *q == '+')) q++;
	if (q - c->p < 3) return false;
	*name = c->p, *len = (uint32_t)(q - c->p);
	c->p = q;
	return true;
}

// tz_offset reads [+-]hh[:mm[:ss]] as seconds.
static bool tz_offset(cur *c, int *out) {
	bool neg = false;
	if (tz_peek(c, '+')) c->p++;
	else if (tz_peek(c, '-')) c->p++, neg = true;
	int h, m = 0, s = 0;
	if (!tz_num(c, 0, 24 * 7, &h)) return false;
	if (tz_peek(c, ':')) {
		c->p++;
		if (!tz_num(c, 0, 59, &m)) return false;
		if (tz_peek(c, ':')) {
			c->p++;
			if (!tz_num(c, 0, 59, &s)) return false;
		}
	}
	int off = h * 3600 + m * 60 + s;
	*out = neg ? -off : off;
	return true;
}

typedef struct {
	char kind;  // 'J' (1-365, no Feb 29), 'D' (0-365), 'M' (month.week.day)
	int day, week, mon, time;
} tz_rule;

static bool tz_rule_read(cur *c, tz_rule *r) {
	*r = (tz_rule){0};
	if (tz_peek(c, 'J')) {
		c->p++;
		r->kind = 'J';
		if (!tz_num(c, 1, 365, &r->day)) return false;
	} else if (tz_peek(c, 'M')) {
		c->p++;
		r->kind = 'M';
		if (!tz_num(c, 1, 12, &r->mon) || !tz_peek(c, '.')) return false;
		c->p++;
		if (!tz_num(c, 1, 5, &r->week) || !tz_peek(c, '.')) return false;
		c->p++;
		if (!tz_num(c, 0, 6, &r->day)) return false;
	} else {
		r->kind = 'D';
		if (!tz_num(c, 0, 365, &r->day)) return false;
	}
	r->time = 2 * 3600;
	if (tz_peek(c, '/')) {
		c->p++;
		return tz_offset(c, &r->time);
	}
	return true;
}

// tz_rule_time is when r happens in year, in seconds since the year's
// start in UTC, for a zone off seconds east before it.
static int64_t tz_rule_time(int64_t year, const tz_rule *r, int off) {
	static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int64_t d;
	switch (r->kind) {
	case 'J':
		d = r->day - 1 + (is_leap(year) && r->day >= 60);
		break;
	case 'D':
		d = r->day;
		break;
	default: {
		// The weekday of the month's first day, then the week's day.
		int64_t first = days_from_civil(year, r->mon, 1);
		int64_t dow = ((first + 4) % 7 + 7) % 7;  // 1970-01-01 was a Thursday
		int64_t md = r->day - dow;
		if (md < 0) md += 7;
		int64_t len = mdays[r->mon - 1] + (r->mon == 2 && is_leap(year));
		for (int i = 1; i < r->week; i++) {
			if (md + 7 >= len) break;
			md += 7;
		}
		d = before[r->mon - 1] + (r->mon > 2 && is_leap(year)) + md;
	}
	}
	return d * 86400 + r->time - off;
}

// tz_footer is the TZ string s's offset at Unix second t.
static bool tz_footer(const char *s, int64_t t, ak_tz_at *out) {
	cur c = {s, s + strlen(s)};
	const char *sn, *dn;
	uint32_t sl, dl;
	int soff, doff;
	if (!tz_name(&c, &sn, &sl) || !tz_offset(&c, &soff)) return false;
	soff = -soff;  // a TZ string's offsets are west of UTC
	if (c.p == c.end || *c.p == ',') {
		*out = (ak_tz_at){soff, sn, sl};
		return true;
	}
	if (!tz_name(&c, &dn, &dl)) return false;
	if (c.p == c.end || *c.p == ',') {
		doff = soff + 3600;
	} else {
		if (!tz_offset(&c, &doff)) return false;
		doff = -doff;
	}
	tz_rule start, end;
	if (c.p == c.end) {
		// tzcode's default rules: the United States'.
		start = (tz_rule){'M', 0, 2, 3, 7200};
		end = (tz_rule){'M', 0, 1, 11, 7200};
	} else {
		if (*c.p != ',' && *c.p != ';') return false;
		c.p++;
		if (!tz_rule_read(&c, &start) || !tz_peek(&c, ',')) return false;
		c.p++;
		if (!tz_rule_read(&c, &end) || c.p != c.end) return false;
	}
	int64_t days = floor_div(t, 86400);
	int64_t year = year_of(days);
	int64_t ystart = days_from_civil(year, 1, 1) * 86400;
	int64_t ysec = t - ystart;
	int64_t a = tz_rule_time(year, &start, soff), b = tz_rule_time(year, &end, doff);
	ak_tz_at std = {soff, sn, sl}, dst = {doff, dn, dl};
	if (b < a) {
		// The southern hemisphere's: daylight time across the new year.
		int64_t x = a;
		a = b, b = x;
		ak_tz_at y = std;
		std = dst, dst = y;
	}
	*out = ysec < a || ysec >= b ? std : dst;
	return true;
}

// ---- Offsets ----

static ak_tz_at type_at(const ak_tz_zone *z, unsigned i) {
	const ak_tz_type *ty = &ak_tz_types[ak_tz_ztypes[z->types + i]];
	const char *a = ak_tz_strs + ty->abbr;
	return (ak_tz_at){ty->off, a, (uint32_t)strlen(a)};
}

// trans_secs is transition i's seconds past its minute (most have none).
static int64_t trans_secs(unsigned i) {
	size_t lo = 0, hi = ak_tz_nsecs;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (ak_tz_sec_idx[mid] == i) return ak_tz_secs[mid];
		if (ak_tz_sec_idx[mid] < i) lo = mid + 1;
		else hi = mid;
	}
	return 0;
}

// at_or_before says transition i is at or before Unix second t.
static bool at_or_before(unsigned i, int64_t tm, int64_t ts) {
	int64_t m = ak_tz_mins[i];
	return m < tm || (m == tm && trans_secs(i) <= ts);
}

ak_tz_at ak_tz_offset(const ak_tz_zone *z, int64_t t) {
	int64_t tm = floor_div(t, 60), ts = t - tm * 60;
	unsigned lo = 0, hi = z->ntrans;
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;
		if (at_or_before(z->trans + mid, tm, ts)) lo = mid + 1;
		else hi = mid;
	}
	// After the last transition (or with none), the TZ string says.
	const char *footer = ak_tz_strs + z->footer;
	ak_tz_at r;
	if (lo == z->ntrans && *footer && tz_footer(footer, t, &r)) return r;
	return type_at(z, lo == 0 ? 0 : ak_tz_tidx[z->trans + lo - 1]);
}

int32_t ak_tz_wall_offset(int64_t w, int32_t (*offset_at)(const void *ctx, int64_t t), const void *ctx) {
	// A zone's offset changes at most once in two days: the offsets a day
	// either side are the ones before and after any change near w.
	int32_t before = offset_at(ctx, w - 86400), after = offset_at(ctx, w + 86400);
	if (before == after) return before;
	bool b = offset_at(ctx, w - before) == before, a = offset_at(ctx, w - after) == after;
	if (b && a) return before > after ? before : after;  // repeated: the earlier instant
	if (a && !b) return after;
	return before;  // skipped, or only before's: the offset before the change
}

// ---- The device's zone's name ----

size_t (*ak_tz_local_name_hook)(char *out, size_t cap);

// put writes s[0:n] into out, in the database's spelling when it knows
// the name.
static size_t put(char *out, size_t cap, const char *s, size_t n) {
	const ak_tz_name *nm = ak_tz_find(s, n);
	if (nm) s = ak_tz_strs + nm->name, n = strlen(s);
	if (n == 0 || n >= cap) return 0;
	memcpy(out, s, n);
	out[n] = 0;
	return n;
}

#if !defined(_WIN32) && !defined(__wasm__)
// in_zoneinfo puts a path's name under a zoneinfo directory (posix/ and
// right/ are the same zones, with and without leap seconds).
static size_t in_zoneinfo(const char *path, char *out, size_t cap) {
	const char *z = strstr(path, "zoneinfo/");
	if (!z) return 0;
	z += 9;
	if (strncmp(z, "posix/", 6) == 0) z += 6;
	else if (strncmp(z, "right/", 6) == 0) z += 6;
	return put(out, cap, z, strlen(z));
}

#if !defined(__ANDROID__)
// from_link follows path's symbolic links to a zoneinfo file's.
static size_t from_link(const char *path, char *out, size_t cap) {
	char at[1024], to[1024];
	snprintf(at, sizeof at, "%s", path);
	for (int depth = 0; depth < 8; depth++) {
		ssize_t n = readlink(at, to, sizeof to - 1);
		if (n <= 0) return 0;
		to[n] = 0;
		size_t got = in_zoneinfo(to, out, cap);
		if (got) return got;
		if (to[0] == '/') {
			memcpy(at, to, (size_t)n + 1);
		} else {
			char *slash = strrchr(at, '/');
			size_t dir = slash ? (size_t)(slash - at) + 1 : 0;
			if (dir + (size_t)n + 1 > sizeof at) return 0;
			memcpy(at + dir, to, (size_t)n + 1);
		}
	}
	return 0;
}
#endif
#endif

size_t ak_tz_local_name(char *out, size_t cap) {
	if (cap) out[0] = 0;
	bool utc = false;
	if (ak_sched_manual()) return put(out, cap, "UTC", 3);
	ak_zone_offset(0, &utc);
	if (utc) return put(out, cap, "UTC", 3);
	if (ak_tz_local_name_hook) {
		char buf[128];
		size_t n = ak_tz_local_name_hook(buf, sizeof buf);
		return n && n < sizeof buf ? put(out, cap, buf, n) : 0;
	}
#if defined(_WIN32)
	// Windows names its zones its own way (Pacific Standard Time); its
	// ICU (Windows 10 1903 on) maps them to IANA's.
	DYNAMIC_TIME_ZONE_INFORMATION tz;
	if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID) return 0;
	HMODULE icu = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!icu) return 0;
	typedef int32_t(__cdecl * conv)(const uint16_t *, int32_t, const char *, uint16_t *, int32_t, int *);
	conv f = (conv)(void *)GetProcAddress(icu, "ucal_getTimeZoneIDForWindowsID");
	if (!f) return 0;
	uint16_t id[64];
	int status = 0;
	int32_t n = f((const uint16_t *)tz.TimeZoneKeyName, (int32_t)wcslen(tz.TimeZoneKeyName), NULL, id, 64, &status);
	if (status > 0 || n <= 0 || n > 64) return 0;
	char buf[128];
	int len = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)id, n, buf, (int)sizeof buf - 1, NULL, NULL);
	return len > 0 ? put(out, cap, buf, (size_t)len) : 0;
#elif defined(__wasm__)
	return 0;
#else
	// TZ names the zone (":Europe/Paris", a zoneinfo file's path) or is
	// a rule of its own ("EST5EDT,M3.2.0,M11.1.0": no name unless the
	// database has it); empty, it's UTC.
	const char *tz = getenv("TZ");
	if (tz) {
		if (*tz == ':') tz++;
		if (!*tz) return put(out, cap, "UTC", 3);
		size_t got = in_zoneinfo(tz, out, cap);
		if (got || *tz == '/') return got;
		return ak_tz_find(tz, strlen(tz)) ? put(out, cap, tz, strlen(tz)) : 0;
	}
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX];
	int n = __system_property_get("persist.sys.timezone", prop);
	return n > 0 ? put(out, cap, prop, (size_t)n) : 0;
#else
	// /etc/localtime links to the zone's file; iOS keeps its link in
	// /var/db/timezone.
	// /var/db/timezone. Older Debians say it in /etc/timezone too.
	size_t got = from_link("/etc/localtime", out, cap);
	if (!got) got = from_link("/var/db/timezone/localtime", out, cap);
	FILE *f = got ? NULL : fopen("/etc/timezone", "r");
	if (f) {
		char buf[128];
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
		got = put(out, cap, buf, n);
	}
	return got;
#endif
#endif
}
