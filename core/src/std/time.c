// time.c: std/time's builtins. A Time (std/time/time.asuka) is an
// instant (nanoseconds since the Unix epoch), its zone, the zone's
// offset at that instant, and the wall clock's fields, which the Asuka
// side computes from the first three. What it needs from C:
//
//	time.wallNanos() -> int            now; with the manual clock (tests),
//	                                   2026-01-01T00:00:00Z plus its time
//	time.monotonic() -> Duration       (rt/lib.c's)
//	time.localOffset(unix) -> int      the local zone's offset from UTC at
//	                                   that second, in seconds east (UTC in
//	                                   tests, so they see the same dates)
//	time.localWallOffset(wall) -> int  the local zone's offset that makes a
//	                                   wall-clock second an instant (tz.h's
//	                                   rule for skipped and repeated ones)
//	time.zoneID(name) -> string        a named zone's name as the database
//	                                   spells it; throws for one it lacks
//	time.zoneOffset(name, unix) -> int, time.zoneWallOffset(name, wall)
//	                                   likewise for a named zone (tz.c's;
//	                                   UTC's for a name it doesn't know)
//	time.localZoneName() -> string     the device's zone's name, or ""
//	time.zoneNames() -> [string]       the places' zones, and UTC
//	time.tzVersion() -> string         the database's release: 2026f
//	time.formatFields(pattern, …, zone, unix) -> string
//	time.parseRFC3339(s) -> [instant, offset, zone start, zone end]   throws
//	time.parseDurationNanos(s) -> int  a Go duration's nanoseconds; throws
#define _POSIX_C_SOURCE 200809L  // localtime_r under -std=c17
#include "std/std.h"
#include "std/tz.h"
#include "rt/sched.h"
#include "base/thread.h"

#include <stdio.h>
#include <time.h>

#define STR(i) ((ak_str *)args[i].p)

// days_from_civil is the number of days from 1970-01-01 to y-m-d in the
// proleptic Gregorian calendar (Howard Hinnant's algorithm).
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

// ---- The clock and the zone ----

static ak_val wall_nanos(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.i = ak_sched_wall()};
}

// local_offset is the device's zone's offset at t, in seconds east (the
// base's: the platform's own, the browser's through its hook, UTC when
// asuka dev runs the app in UTC).
static int64_t local_offset(int64_t t) {
	if (ak_sched_manual()) return 0;
	return ak_zone_offset(t, NULL);
}

static ak_val local_offset_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = local_offset(args[0].i)};
}

static int32_t local_at(const void *ctx, int64_t t) {
	AK_UNUSED(ctx);
	return (int32_t)local_offset(t);
}

static ak_val local_wall_offset_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = ak_tz_wall_offset(args[0].i, local_at, NULL)};
}

// ---- Named zones ----

static const ak_tz_zone *named(const ak_str *s) {
	const ak_tz_name *nm = ak_tz_find(s->data, s->len);
	return nm ? &ak_tz_zones[nm->zone] : NULL;
}

static int32_t named_at(const void *ctx, int64_t t) { return ak_tz_offset(ctx, t).off; }

static ak_val zone_id_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_tz_name *nm = ak_tz_find(STR(0)->data, STR(0)->len);
	if (!nm) {
		ak_throw_msg("time: no time zone is named %s in the tz database (%s): an IANA name, like America/New_York", STR(0)->data, ak_tz_version);
		return (ak_val){0};
	}
	return (ak_val){.p = ak_str_c(ak_tz_strs + nm->name)};
}

static ak_val zone_offset_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_tz_zone *z = named(STR(0));
	return (ak_val){.i = z ? ak_tz_offset(z, args[1].i).off : 0};
}

static ak_val zone_wall_offset_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_tz_zone *z = named(STR(0));
	return (ak_val){.i = z ? ak_tz_wall_offset(args[1].i, named_at, z) : 0};
}

static ak_val local_zone_name_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	char buf[128];
	size_t len = ak_tz_local_name(buf, sizeof buf);
	return (ak_val){.p = ak_str_new(buf, len)};
}

static ak_val zone_names_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	// The table is in lowercase's order; the list is in the names'.
	uint32_t count = 0;
	for (uint16_t i = 0; i < ak_tz_nnames; i++) count += ak_tz_names[i].listed;
	const char **names = ak_alloc(count * sizeof *names);
	uint32_t k = 0;
	for (uint16_t i = 0; i < ak_tz_nnames; i++)
		if (ak_tz_names[i].listed) names[k++] = ak_tz_strs + ak_tz_names[i].name;
	// An insertion sort: the order is nearly the names' already.
	for (uint32_t i = 1; i < count; i++) {
		const char *x = names[i];
		uint32_t j = i;
		for (; j > 0 && strcmp(names[j - 1], x) > 0; j--) names[j] = names[j - 1];
		names[j] = x;
	}
	ak_arr *a = ak_str_arr(count);
	for (uint32_t i = 0; i < count; i++) ak_str_arr_push(&a, names[i], strlen(names[i]));
	ak_free(names);
	return (ak_val){.p = a};
}

static ak_val tzdata_version_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(ak_tz_version)};
}

// ---- Formatting ----
//
// A subset of Unicode's date patterns (UTS #35), in English: letters are
// fields, repeated for width or form; text in '…' is literal, '' a quote;
// anything else is itself.

static const char *const month_names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static const char *const day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// A Time made by hand has whatever fields it was given: they format
// without overflowing, months and weekdays around the year and the week.

// magnitude is |v|, INT64_MIN's too.
static uint64_t magnitude(int64_t v) { return v < 0 ? 0 - (uint64_t)v : (uint64_t)v; }

// wrap is v mod n, from 0 to n-1.
static int64_t wrap(int64_t v, int64_t n) {
	int64_t r = v % n;
	return r < 0 ? r + n : r;
}

static void num(ak_buf *b, int64_t v, int width) {
	if (v < 0) ak_buf_u8(b, '-');
	ak_buf_printf(b, "%0*llu", width, (unsigned long long)magnitude(v));
}

// day_of_year is y-m-d's day in its year (month 1 to 12).
static int64_t day_of_year(int64_t y, int64_t m, int64_t d) {
	static const int16_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
	uint64_t days = (uint64_t)(before[m - 1] + (m > 2 && leap));
	return (int64_t)(days + (uint64_t)d);  // a day past the year's wraps, never overflows
}

static void name(ak_buf *b, const char *s, int count) {
	size_t len = strlen(s);
	if (count == 3) len = 3;          // Jan, Mon
	else if (count == 5) len = 1;     // J, M
	else if (count == 6) len = 2;     // Mo (days)
	ak_buf_put(b, s, len);
}

static void zone(ak_buf *b, int64_t off, bool colon, bool minutes, bool z) {
	if (off == 0 && z) {
		ak_buf_u8(b, 'Z');
		return;
	}
	ak_buf_u8(b, off < 0 ? '-' : '+');
	uint64_t mag = magnitude(off);
	unsigned long long h = mag / 3600, m = mag / 60 % 60;
	ak_buf_printf(b, "%02llu", h);
	if (minutes || m) ak_buf_printf(b, colon ? ":%02llu" : "%02llu", m);
}

typedef struct {
	int64_t year, month, day, hour, minute, second, nano, weekday, offset;
	const ak_str *zone;  // its IANA name, or ""
	int64_t unix;        // the instant, for its zone's abbreviation then
} fields;

// gmt is the localized GMT format: GMT, GMT+1, GMT+5:30 (short) or
// GMT+01:00 (long).
static void gmt(ak_buf *b, int64_t off, bool long_form) {
	ak_buf_put(b, "GMT", 3);
	if (off == 0) return;
	ak_buf_u8(b, off < 0 ? '-' : '+');
	uint64_t mag = magnitude(off);
	unsigned long long h = mag / 3600, m = mag / 60 % 60;
	if (long_form) ak_buf_printf(b, "%02llu:%02llu", h, m);
	else if (m) ak_buf_printf(b, "%llu:%02llu", h, m);
	else ak_buf_printf(b, "%llu", h);
}

// abbreviation is the time's zone's abbreviation then (EST, CET), when
// the database has one that's a name (not +03) at this offset.
static bool abbreviation(ak_buf *b, const fields *f) {
	if (!f->zone || !f->zone->len) return false;
	const ak_tz_name *nm = ak_tz_find(f->zone->data, f->zone->len);
	if (!nm) return false;
	ak_tz_at at = ak_tz_offset(&ak_tz_zones[nm->zone], f->unix);
	if (at.off != f->offset || !at.abbr_len || at.abbr[0] == '+' || at.abbr[0] == '-') return false;
	ak_buf_put(b, at.abbr, at.abbr_len);
	return true;
}

static void field(ak_buf *b, char c, int count, const fields *f) {
	switch (c) {
	case 'G':
		ak_buf_put(b, f->year > 0 ? "AD" : "BC", 2);
		break;
	case 'y':
	case 'u':
		if (count == 2) num(b, (f->year % 100 + 100) % 100, 2);
		else num(b, f->year, count);
		break;
	case 'M':
	case 'L':
		if (count <= 2) num(b, f->month, count);
		else name(b, month_names[wrap(wrap(f->month, 12) + 11, 12)], count);
		break;
	case 'd':
		num(b, f->day, count);
		break;
	case 'D':
		num(b, day_of_year(f->year, wrap(wrap(f->month, 12) + 11, 12) + 1, f->day), count);
		break;
	case 'E':
	case 'c':
	case 'e':
		if ((c == 'e' || c == 'c') && count <= 2) num(b, wrap(f->weekday, 7) + 1, count);
		else name(b, day_names[wrap(f->weekday, 7)], count < 3 ? 3 : count);
		break;
	case 'a':
		ak_buf_put(b, f->hour < 12 ? "AM" : "PM", 2);
		break;
	case 'h':
		num(b, f->hour % 12 == 0 ? 12 : f->hour % 12, count);
		break;
	case 'H':
		num(b, f->hour, count);
		break;
	case 'K':
		num(b, f->hour % 12, count);
		break;
	case 'k':
		num(b, f->hour == 0 ? 24 : f->hour, count);
		break;
	case 'm':
		num(b, f->minute, count);
		break;
	case 's':
		num(b, f->second, count);
		break;
	case 'S': {
		// Fractions of a second, truncated to count digits.
		char d[10];
		snprintf(d, sizeof d, "%09lld", (long long)f->nano);
		for (int i = 0; i < count; i++) ak_buf_u8(b, i < 9 ? (uint8_t)d[i] : '0');
		break;
	}
	case 'Z':
		if (count == 4) {
			ak_buf_put(b, "GMT", 3);
			if (f->offset) zone(b, f->offset, true, true, false);
		} else {
			zone(b, f->offset, count == 5, true, count == 5);
		}
		break;
	case 'X':
	case 'x':
		zone(b, f->offset, count >= 3, count >= 2, c == 'X');
		break;
	case 'z':
		if (count >= 4 || !abbreviation(b, f)) gmt(b, f->offset, count >= 4);
		break;
	case 'O':
		gmt(b, f->offset, count >= 4);
		break;
	case 'V':
		if (!f->zone || !f->zone->len) {
			zone(b, f->offset, true, true, true);
		} else if (count == 3) {
			// The exemplar city: the name's last part, its _ spaces.
			const char *city = strrchr(f->zone->data, '/');
			city = city ? city + 1 : f->zone->data;
			for (; *city; city++) ak_buf_u8(b, *city == '_' ? ' ' : (uint8_t)*city);
		} else {
			ak_buf_put(b, f->zone->data, f->zone->len);
		}
		break;
	default:
		for (int i = 0; i < count; i++) ak_buf_u8(b, (uint8_t)c);
	}
}

static ak_str *format(const ak_str *p, const fields *f) {
	ak_buf b = {0};
	for (uint32_t i = 0; i < p->len;) {
		char c = p->data[i];
		if (c == '\'') {
			if (i + 1 < p->len && p->data[i + 1] == '\'') {
				ak_buf_u8(&b, '\'');
				i += 2;
				continue;
			}
			// Quoted text, '' inside it a quote.
			i++;
			while (i < p->len) {
				if (p->data[i] == '\'') {
					if (i + 1 < p->len && p->data[i + 1] == '\'') {
						ak_buf_u8(&b, '\'');
						i += 2;
						continue;
					}
					i++;
					break;
				}
				ak_buf_u8(&b, (uint8_t)p->data[i++]);
			}
			continue;
		}
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
			int count = 0;
			while (i < p->len && p->data[i] == c) i++, count++;
			field(&b, c, count, f);
			continue;
		}
		ak_buf_u8(&b, (uint8_t)c);
		i++;
	}
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	return s;
}

static ak_val format_fields(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	fields f = {args[1].i, args[2].i, args[3].i, args[4].i, args[5].i, args[6].i, args[7].i, args[8].i, args[9].i, n > 10 ? STR(10) : NULL, n > 11 ? args[11].i : 0};
	return (ak_val){.p = format(STR(0), &f)};
}

// ---- RFC 3339 ----

static bool digits(const char *s, size_t len, size_t *i, int n, int64_t *out) {
	int64_t v = 0;
	for (int k = 0; k < n; k++) {
		if (*i >= len || s[*i] < '0' || s[*i] > '9') return false;
		v = v * 10 + (s[(*i)++] - '0');
	}
	*out = v;
	return true;
}

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int64_t month_days(int64_t y, int64_t m) {
	static const int8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return m == 2 && leap(y) ? 29 : d[m - 1];
}

// parse_rfc3339 reads 2006-01-02T15:04:05.999999999Z07:00 (a t or a space
// for the T, z for Z, the fraction optional), from 1678 to 2261.
static bool parse_rfc3339(const char *s, size_t len, int64_t *instant, int64_t *offset) {
	size_t i = 0;
	int64_t y, mo, d, h, mi, sec, nano = 0, off = 0;
	if (!digits(s, len, &i, 4, &y) || i >= len || s[i++] != '-') return false;
	if (!digits(s, len, &i, 2, &mo) || i >= len || s[i++] != '-') return false;
	if (!digits(s, len, &i, 2, &d) || i >= len) return false;
	char t = s[i++];
	if (t != 'T' && t != 't' && t != ' ') return false;
	if (!digits(s, len, &i, 2, &h) || i >= len || s[i++] != ':') return false;
	if (!digits(s, len, &i, 2, &mi) || i >= len || s[i++] != ':') return false;
	if (!digits(s, len, &i, 2, &sec)) return false;
	if (i < len && s[i] == '.') {
		i++;
		int k = 0;
		int64_t scale = 100000000;
		while (i < len && s[i] >= '0' && s[i] <= '9') {
			nano += (s[i++] - '0') * scale;
			scale /= 10;
			k++;
		}
		if (k == 0) return false;
	}
	if (i >= len) return false;
	if (s[i] == 'Z' || s[i] == 'z') {
		i++;
	} else if (s[i] == '+' || s[i] == '-') {
		int sign = s[i++] == '-' ? -1 : 1;
		int64_t oh, om;
		if (!digits(s, len, &i, 2, &oh) || i >= len || s[i++] != ':' || !digits(s, len, &i, 2, &om)) return false;
		if (oh > 23 || om > 59) return false;
		off = sign * (oh * 3600 + om * 60);
	} else {
		return false;
	}
	if (i != len) return false;
	if (mo < 1 || mo > 12 || d < 1 || d > month_days(y, mo) || h > 23 || mi > 59 || sec > 59) return false;
	int64_t secs = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - off;
	// An instant is an int of nanoseconds: 1677 to 2262.
	if (__builtin_mul_overflow(secs, 1000000000, instant) || __builtin_add_overflow(*instant, nano, instant)) return false;
	*offset = off;
	return true;
}

// parse_offset reads an annotation's offset, +01:00, as seconds east.
static bool parse_offset(const char *s, size_t len, int64_t *off) {
	size_t i = 1;
	int64_t h, m;
	if (len != 6 || (s[0] != '+' && s[0] != '-') || !digits(s, len, &i, 2, &h) || s[i++] != ':' || !digits(s, len, &i, 2, &m)) return false;
	if (h > 23 || m > 59) return false;
	*off = (s[0] == '-' ? -1 : 1) * (h * 3600 + m * 60);
	return true;
}

// parse_fn reads RFC 3339's time and RFC 9557's annotations after it:
// a time zone first ([Europe/Paris], [!Europe/Paris], [+01:00]), which
// must agree with the offset, then [key=value]s, ignored unless critical
// ([!u-ca=…]). Its answer: the instant, the offset, and where the zone's
// name is in s (start and end bytes; equal without one).
static ak_val parse_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *str = STR(0);
	const char *s = str->data;
	size_t len = str->len;
	const char *br = memchr(s, '[', len);
	size_t base = br ? (size_t)(br - s) : len;
	int64_t instant, offset, zs = 0, ze = 0;
	if (!parse_rfc3339(s, base, &instant, &offset)) {
		ak_throw_msg("time: %s isn't an RFC 3339 time from 1678 to 2261 (2006-01-02T15:04:05Z)", s);
		return (ak_val){0};
	}
	for (size_t i = base, k = 0; i < len; k++) {
		const char *close = s[i] == '[' ? memchr(s + i, ']', len - i) : NULL;
		if (!close) {
			ak_throw_msg("time: %s has text after its time that isn't an annotation ([Europe/Paris])", s);
			return (ak_val){0};
		}
		size_t a = i + 1, e = (size_t)(close - s);
		bool critical = a < e && s[a] == '!';
		a += critical;
		if (memchr(s + a, '=', e - a)) {
			if (critical) {
				ak_throw_msg("time: %s has a critical annotation it doesn't know ([!%.*s])", s, (int)(e - a), s + a);
				return (ak_val){0};
			}
		} else if (k > 0 || a == e) {
			ak_throw_msg("time: %s has a time zone annotation that isn't its first", s);
			return (ak_val){0};
		} else if (s[a] == '+' || s[a] == '-') {
			int64_t off;
			if (!parse_offset(s + a, e - a, &off) || off != offset) {
				ak_throw_msg("time: %s's annotation %.*s isn't its offset", s, (int)(e - a), s + a);
				return (ak_val){0};
			}
		} else {
			const ak_tz_name *nm = ak_tz_find(s + a, e - a);
			if (!nm) {
				ak_throw_msg("time: %s names a time zone the tz database (%s) doesn't have: %.*s", s, ak_tz_version, (int)(e - a), s + a);
				return (ak_val){0};
			}
			int64_t secs = instant / 1000000000 - (instant % 1000000000 < 0);
			int32_t want = ak_tz_offset(&ak_tz_zones[nm->zone], secs).off;
			if (want != offset) {
				ak_buf b = {0};
				zone(&b, want, true, true, false);
				ak_buf_u8(&b, 0);
				ak_throw_msg("time: %s's offset isn't %.*s's then (%s)", s, (int)(e - a), s + a, (const char *)b.data);
				ak_buf_free(&b);
				return (ak_val){0};
			}
			zs = (int64_t)a, ze = (int64_t)e;
		}
		i = e + 1;
	}
	ak_arr *r = ak_arr_new(AK_K_INT, NULL, 4);
	r->data[0].i = instant;
	r->data[1].i = offset;
	r->data[2].i = zs;
	r->data[3].i = ze;
	r->len = 4;
	return (ak_val){.p = r};
}

// ---- Durations ----

// leading_fraction reads a fraction's digits: as many as an int holds,
// and its scale (10 to their count); those past them are only skipped.
static uint64_t leading_fraction(const char *s, size_t len, size_t *i, double *scale) {
	uint64_t x = 0;
	bool overflow = false;
	*scale = 1;
	for (; *i < len && s[*i] >= '0' && s[*i] <= '9'; (*i)++) {
		if (overflow) continue;
		if (x > (UINT64_C(1) << 63) / 10) {
			overflow = true;
			continue;
		}
		uint64_t y = x * 10 + (uint64_t)(s[*i] - '0');
		if (y > UINT64_C(1) << 63) {
			overflow = true;
			continue;
		}
		x = y;
		*scale *= 10;
	}
	return x;
}

// unit_ns is a duration's unit's nanoseconds: Go's, and min (Asuka's
// literals': 2min); 0 for none.
static uint64_t unit_ns(const char *u, size_t n) {
	static const struct {
		const char *name;
		uint64_t ns;
	} units[] = {
		{"ns", 1}, {"us", 1000}, {"\xc2\xb5s", 1000}, {"\xce\xbcs", 1000}, {"ms", 1000000},
		{"s", 1000000000}, {"m", 60000000000}, {"min", 60000000000}, {"h", 3600000000000},
	};
	for (size_t k = 0; k < sizeof units / sizeof units[0]; k++)
		if (strlen(units[k].name) == n && memcmp(units[k].name, u, n) == 0) return units[k].ns;
	return 0;
}

// parse_duration reads Go's durations (time.ParseDuration): a sign, then
// numbers with units: 1h30m, 2.5s, -1m30s, 250ms; 0 alone needs none.
// why says what's wrong when it can't.
static bool parse_duration(const char *s, size_t len, int64_t *out, const char **why, const char **unit, size_t *unit_len) {
	size_t i = 0;
	bool neg = false;
	*why = NULL;
	if (i < len && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
	if (len - i == 1 && s[i] == '0') {
		*out = 0;
		return true;
	}
	if (i == len) return false;
	const uint64_t limit = UINT64_C(1) << 63;
	uint64_t d = 0;
	while (i < len) {
		if (!(s[i] == '.' || (s[i] >= '0' && s[i] <= '9'))) return false;
		size_t start = i;
		uint64_t v = 0;
		for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
			if (v > limit / 10) goto overflow;
			v = v * 10 + (uint64_t)(s[i] - '0');
			if (v > limit) goto overflow;
		}
		bool pre = i != start, post = false;
		uint64_t f = 0;
		double scale = 1;
		if (i < len && s[i] == '.') {
			i++;
			size_t fs = i;
			f = leading_fraction(s, len, &i, &scale);
			post = i != fs;
		}
		if (!pre && !post) return false;
		size_t u = i;
		while (i < len && s[i] != '.' && !(s[i] >= '0' && s[i] <= '9')) i++;
		if (u == i) {
			*why = "it has no unit (1h, 30m, 2.5s, 250ms)";
			return false;
		}
		uint64_t ns = unit_ns(s + u, i - u);
		if (!ns) {
			*why = "unit";
			*unit = s + u, *unit_len = i - u;
			return false;
		}
		if (v > limit / ns) goto overflow;
		v *= ns;
		if (f > 0) {
			v += (uint64_t)((double)f * ((double)ns / scale));
			if (v > limit) goto overflow;
		}
		d += v;
		if (d > limit) goto overflow;
	}
	if (!neg && d > limit - 1) goto overflow;
	*out = neg ? (int64_t)(0 - d) : (int64_t)d;
	return true;
overflow:
	*why = "it's longer than a Duration holds (about 292 years)";
	return false;
}

static ak_val parse_duration_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const char *why, *unit = NULL;
	size_t unit_len = 0;
	int64_t d;
	if (!parse_duration(STR(0)->data, STR(0)->len, &d, &why, &unit, &unit_len)) {
		if (why && unit) ak_throw_msg("time: %s isn't a duration: its unit %.*s isn't one of h, m (or min), s, ms, us (or \xc2\xb5s) and ns", STR(0)->data, (int)unit_len, unit);
		else if (why) ak_throw_msg("time: %s isn't a duration: %s", STR(0)->data, why);
		else ak_throw_msg("time: %s isn't a duration (1h30m, 250ms, 2.5s, -1m)", STR(0)->data);
		return (ak_val){0};
	}
	return (ak_val){.i = d};
}

const ak_lib_entry ak_std_time[] = {
	{"time.wallNanos", wall_nanos},
	{"time.localOffset", local_offset_fn},
	{"time.formatFields", format_fields},
	{"time.parseRFC3339", parse_fn},
	{"time.localWallOffset", local_wall_offset_fn},
	{"time.zoneID", zone_id_fn},
	{"time.zoneOffset", zone_offset_fn},
	{"time.zoneWallOffset", zone_wall_offset_fn},
	{"time.localZoneName", local_zone_name_fn},
	{"time.zoneNames", zone_names_fn},
	{"time.tzVersion", tzdata_version_fn},
	{"time.parseDurationNanos", parse_duration_fn},
	{NULL, NULL},
};
