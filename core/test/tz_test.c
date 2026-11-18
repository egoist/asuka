// Named time zones (std/tz.c, tzdata_gen.c): every case of
// core/test/testdata/tz_cases.txt (each zone as Go's time package reads
// the same release: transitions, its TZ string's years, 1800 and 2200),
// wall times a zone skips and repeats, names, the device's zone's name,
// and std/time's builtins over them (formats, RFC 9557, durations).
#define _POSIX_C_SOURCE 200809L  // setenv
#include "t.h"
#include "std/std.h"
#include "std/tz.h"
#include "rt/sched.h"

#include <time.h>

// read_file reads a file of the repository (ASUKA_ROOT, as core_test.go
// runs the tests; else from core/test).
static char *read_file(const char *rel) {
	const char *root = getenv("ASUKA_ROOT");
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", root ? root : "../..", rel);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)n + 1);
	size_t got = fread(b, 1, (size_t)n, f);
	fclose(f);
	b[got] = 0;
	return b;
}

static const ak_tz_zone *zone(const char *name) {
	const ak_tz_name *n = ak_tz_find(name, strlen(name));
	if (!n) {
		T_FAIL("no zone %s", name);
		return NULL;
	}
	return &ak_tz_zones[n->zone];
}

TEST(tz_cases) {
	char *text = read_file("core/test/testdata/tz_cases.txt");
	REQUIRE(text);
	int cases = 0, zones = 0;
	char last[128] = "";
	const ak_tz_zone *z = NULL;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		if (line[0] == '#') continue;
		char name[128], abbr[32];
		long long t, off;
		if (sscanf(line, "%127s %lld %lld %31s", name, &t, &off, abbr) != 4) {
			T_FAIL("bad case %s", line);
			continue;
		}
		if (strcmp(name, last) != 0) {
			snprintf(last, sizeof last, "%s", name);
			z = zone(name);
			zones++;
		}
		if (!z) continue;
		ak_tz_at at = ak_tz_offset(z, t);
		if (at.off != off || at.abbr_len != strlen(abbr) || memcmp(at.abbr, abbr, at.abbr_len) != 0)
			T_FAIL("%s at %lld: %d %.*s, want %lld %s", name, t, at.off, (int)at.abbr_len, at.abbr, off, abbr);
		cases++;
	}
	free(text);
	CHECK(zones > 300);
	CHECK(cases > 3000);
}

// Every transition reads back as itself: one second before it is the
// type before, it the type it names.
TEST(tz_transitions) {
	for (size_t i = 0; i < ak_tz_nnames; i++) {
		const ak_tz_zone *z = &ak_tz_zones[ak_tz_names[i].zone];
		for (unsigned k = 0; k < z->ntrans; k++) {
			unsigned g = z->trans + k;
			int64_t t = (int64_t)ak_tz_mins[g] * 60;
			for (size_t s = 0; s < ak_tz_nsecs; s++)
				if (ak_tz_sec_idx[s] == g) t += ak_tz_secs[s];
			const ak_tz_type *ty = &ak_tz_types[ak_tz_ztypes[z->types + ak_tz_tidx[g]]];
			if (k + 1 < z->ntrans || !ak_tz_strs[z->footer]) CHECK_EQ_INT(ak_tz_offset(z, t).off, ty->off);
			if (k > 0) {
				const ak_tz_type *prev = &ak_tz_types[ak_tz_ztypes[z->types + ak_tz_tidx[g - 1]]];
				CHECK_EQ_INT(ak_tz_offset(z, t - 1).off, prev->off);
			}
		}
	}
}

static int32_t named_at(const void *z, int64_t t) { return ak_tz_offset(z, t).off; }

// wall is the instant (Unix seconds) of a wall time in a zone.
static int64_t wall(const char *name, int y, int mo, int d, int h, int mi) {
	// Days from the civil date, Howard Hinnant's way.
	int64_t yy = y - (mo <= 2);
	int64_t era = (yy >= 0 ? yy : yy - 399) / 400, yoe = yy - era * 400;
	int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
	int64_t w = days * 86400 + h * 3600 + mi * 60;
	const ak_tz_zone *z = zone(name);
	return z ? w - ak_tz_wall_offset(w, named_at, z) : 0;
}

TEST(tz_wall_times) {
	// New York, 2026: 2:00 becomes 3:00 on March 8 (07:00Z), 2:00 1:00 on
	// November 1 (06:00Z).
	CHECK_EQ_INT(wall("America/New_York", 2026, 3, 8, 1, 59), 1772953140);  // 06:59Z
	CHECK_EQ_INT(wall("America/New_York", 2026, 3, 8, 2, 30), 1772955000);  // skipped: 07:30Z, 3:30 EDT
	CHECK_EQ_INT(wall("America/New_York", 2026, 3, 8, 3, 0), 1772953200);   // 07:00Z
	CHECK_EQ_INT(wall("America/New_York", 2026, 11, 1, 1, 30), 1793511000); // repeated: the first, 05:30Z
	CHECK_EQ_INT(wall("America/New_York", 2026, 11, 1, 2, 0), 1793516400);  // 07:00Z
	CHECK_EQ_INT(wall("America/New_York", 2026, 11, 1, 0, 59), 1793509140);
	// Lord Howe's half hour: 2:00 becomes 2:30 on October 4, 2026; 2:00
	// 1:30 on April 5.
	CHECK_EQ_INT(wall("Australia/Lord_Howe", 2026, 10, 4, 2, 15), 1791042300);  // skipped: 2:45, 15:45Z
	CHECK_EQ_INT(wall("Australia/Lord_Howe", 2026, 10, 4, 2, 45), 1791042300);
	CHECK_EQ_INT(wall("Australia/Lord_Howe", 2026, 4, 5, 1, 45), 1775313900);  // repeated: the first, 14:45Z
	CHECK_EQ_INT(wall("Australia/Lord_Howe", 2026, 4, 5, 2, 15), 1775317500);
	// A wall time with no change near it is the zone's.
	CHECK_EQ_INT(wall("Asia/Tokyo", 2026, 6, 1, 9, 0), 1780272000);
	// UTC, and Etc/GMT+5 (POSIX's sign: five hours behind).
	CHECK_EQ_INT(wall("UTC", 1970, 1, 1, 0, 0), 0);
	CHECK_EQ_INT(wall("Etc/GMT+5", 1970, 1, 1, 0, 0), 5 * 3600);
}

TEST(tz_names) {
	CHECK(ak_tz_find("America/New_York", 16) != NULL);
	CHECK(ak_tz_find("AMERICA/NEW_YORK", 16) == ak_tz_find("america/new_york", 16));
	CHECK(ak_tz_find("America/New_Yor", 15) == NULL);
	CHECK(ak_tz_find("America/New_York\0", 17) == NULL);
	CHECK(ak_tz_find("", 0) == NULL);
	CHECK(ak_tz_find("Asia/Calcutta", 13)->zone == ak_tz_find("Asia/Kolkata", 12)->zone);
	CHECK_EQ_STR(ak_tz_strs + ak_tz_find("us/eastern", 10)->name, "US/Eastern");
	CHECK(ak_tz_find("Europe/Amsterdam", 16)->listed);
	CHECK(!ak_tz_find("Asia/Calcutta", 13)->listed);
	CHECK(ak_tz_find("UTC", 3)->listed);
	// Sorted by lowercase, as the lookup searches.
	for (size_t i = 1; i < ak_tz_nnames; i++) {
		const char *a = ak_tz_strs + ak_tz_names[i - 1].name, *b = ak_tz_strs + ak_tz_names[i].name;
		size_t k = 0;
		while (a[k] && b[k] && (a[k] | 32 * (a[k] >= 'A' && a[k] <= 'Z')) == (b[k] | 32 * (b[k] >= 'A' && b[k] <= 'Z'))) k++;
		int ca = a[k] >= 'A' && a[k] <= 'Z' ? a[k] + 32 : a[k], cb = b[k] >= 'A' && b[k] <= 'Z' ? b[k] + 32 : b[k];
		if (ca >= cb) T_FAIL("%s before %s", a, b);
	}
}

static ak_lib_fn fn(const char *name) {
	for (const ak_lib_entry *const *t = ak_std_libs; *t; t++)
		for (const ak_lib_entry *e = *t; e->name; e++)
			if (strcmp(e->name, name) == 0) return e->fn;
	fprintf(stderr, "no builtin %s\n", name);
	abort();
}

static ak_val S(const char *s) { return (ak_val){.p = ak_str_c(s)}; }
static ak_val I(int64_t i) { return (ak_val){.i = i}; }

static void check_str(ak_val v, const char *want, int line) {
	ak_str *s = v.p;
	if (!s || strcmp(s->data, want) != 0) {
		fprintf(stderr, "tz_test.c:%d: %s: got %s, want %s\n", line, t_current, s ? s->data : "(none)", want);
		t_failed++;
	}
	ak_release(s);
}

static void local_name(const char *want, int line) {
	check_str(fn("time.localZoneName")(NULL, 0), want, line);
}

TEST(tz_local_name) {
#if !defined(_WIN32)
	const char *old = getenv("TZ");
	char *saved = old ? strcpy(malloc(strlen(old) + 1), old) : NULL;
	setenv("TZ", "Europe/Paris", 1);
	tzset();
	local_name("Europe/Paris", __LINE__);
	setenv("TZ", ":asia/calcutta", 1);
	tzset();
	local_name("Asia/Calcutta", __LINE__);
	setenv("TZ", "/usr/share/zoneinfo/posix/America/Sao_Paulo", 1);
	tzset();
	local_name("America/Sao_Paulo", __LINE__);
	setenv("TZ", "<+03>-3", 1);  // a rule, not a name
	tzset();
	local_name("", __LINE__);
	setenv("TZ", "UTC", 1);
	tzset();
	local_name("UTC", __LINE__);
	// The manual clock (tests): UTC whatever TZ says.
	setenv("TZ", "Asia/Tokyo", 1);
	tzset();
	ak_sched_manual_clock(true);
	local_name("UTC", __LINE__);
	ak_sched_manual_clock(false);
	if (saved) setenv("TZ", saved, 1);
	else unsetenv("TZ");
	tzset();
	free(saved);
#endif
}

TEST(tz_formats) {
	int64_t july = 1783170300;  // 2026-07-04T13:05:00Z
	// 2026-07-04 09:05, a Saturday: only the zone's fields matter here.
#define FMT(pattern, off, zone, unix, want)                                                              \
	do {                                                                                              \
		ak_val a_[12] = {S(pattern), I(2026), I(7), I(4), I(9), I(5), I(0), I(0), I(6), I(off), S(zone), I(unix)}; \
		check_str(fn("time.formatFields")(a_, 12), want, __LINE__);                                    \
		ak_release(a_[0].p), ak_release(a_[10].p);                                                     \
	} while (0)
	FMT("z zzzz O OOOO VV VVV", -4 * 3600, "America/New_York", july, "EDT GMT-04:00 GMT-4 GMT-04:00 America/New_York New York");
	// An offset the zone doesn't have then: no abbreviation.
	FMT("z", -5 * 3600, "America/New_York", july, "GMT-5");
	FMT("z VV VVV", 0, "UTC", july, "UTC UTC UTC");
	FMT("z VV", 19800, "", july, "GMT+5:30 +05:30");
	FMT("z VV", 0, "", july, "GMT Z");
	FMT("z", 4 * 3600, "Asia/Dubai", july, "GMT+4");
	FMT("VVV", 0, "America/Argentina/Buenos_Aires", july, "Buenos Aires");
	FMT("z", 0, "Nowhere/Else", july, "GMT");
#undef FMT
}

static int64_t duration(const char *s, bool *ok) {
	ak_val a = S(s);
	ak_val r = fn("time.parseDurationNanos")(&a, 1);
	ak_release(a.p);
	*ok = !ak_error_pending();
	if (!*ok) ak_release(ak_error_take());
	return r.i;
}

TEST(tz_durations) {
	static const struct {
		const char *s;
		int64_t ns;
	} good[] = {
		{"0", 0}, {"5s", 5000000000}, {"30s", 30000000000}, {"1478s", 1478000000000}, {"-5s", -5000000000}, {"+5s", 5000000000},
		{"-0", 0}, {"+0", 0}, {"5.0s", 5000000000}, {"5.6s", 5600000000}, {"5.s", 5000000000}, {".5s", 500000000},
		{"1.0s", 1000000000}, {"1.00s", 1000000000}, {"1.004s", 1004000000}, {"1.0040s", 1004000000}, {"100.00100s", 100001000000},
		{"10ns", 10}, {"11us", 11000}, {"12\xc2\xb5s", 12000}, {"12\xce\xbcs", 12000}, {"13ms", 13000000}, {"14s", 14000000000},
		{"15m", 900000000000}, {"15min", 900000000000}, {"16h", 57600000000000}, {"3h30m", 12600000000000},
		{"10.5s4m", 250500000000}, {"-2m3.4s", -123400000000}, {"1h2m3s4ms5us6ns", 3723004005006},
		{"39h9m14.425s", 140954425000000}, {"52763797000ns", 52763797000}, {"0.3333333333333333333h", 1200000000000},
		{"9007199254740993ns", 9007199254740993}, {"9223372036854775807ns", INT64_MAX}, {"9223372036854775.807us", INT64_MAX},
		{"9223372036s854ms775us807ns", INT64_MAX}, {"-9223372036854775808ns", INT64_MIN}, {"-9223372036854775.808us", INT64_MIN},
		{"-9223372036s854ms775us808ns", INT64_MIN}, {"-2562047h47m16.854775808s", INT64_MIN},
		{"0.100000000000000000000h", 360000000000}, {"0.830103483285477580700h", 2988372539827},
	};
	for (size_t i = 0; i < AK_LEN(good); i++) {
		bool ok;
		int64_t got = duration(good[i].s, &ok);
		if (!ok || got != good[i].ns) T_FAIL("%s: %lld (%s), want %lld", good[i].s, (long long)got, ok ? "ok" : "error", (long long)good[i].ns);
	}
	const char *bad[] = {"", "3", "-", "s", ".", "-.", ".s", "+.s", "1d", "\x85\x85", "\xff" "ff", "hello \xff" "ff world",
	                     "9223372036854775808ns", "9223372036854775.808us", "-9223372036854775809ns", "2562048h", "3000000h", "1 h"};
	for (size_t i = 0; i < AK_LEN(bad); i++) {
		bool ok;
		duration(bad[i], &ok);
		if (ok) T_FAIL("%s parsed", bad[i]);
	}
}

TEST(tz_rfc9557) {
	const char *good[] = {"2026-03-07T15:04:05+01:00[Europe/Paris]", "2026-03-07T15:04:05+01:00[!Europe/Paris][u-ca=iso8601]", "2026-03-07T15:04:05Z[UTC]",
	                      "2026-03-07T15:04:05Z[Etc/UTC]", "2026-03-07T15:04:05-05:00[-05:00]", "2026-03-07T15:04:05Z[a=b][c=d]"};
	for (size_t i = 0; i < AK_LEN(good); i++) {
		ak_val a = S(good[i]);
		ak_val r = fn("time.parseRFC3339")(&a, 1);
		ak_release(a.p);
		if (ak_error_pending()) {
			void *e = ak_error_take();
			ak_str *m = ak_error_message(e);
			T_FAIL("%s: %s", good[i], m ? m->data : "?");
			ak_release(e);
			continue;
		}
		ak_release(r.p);
	}
	ak_val a = S("2026-03-07T15:04:05+01:00[Europe/Paris]");
	ak_arr *r = fn("time.parseRFC3339")(&a, 1).p;
	REQUIRE(r && r->len == 4);
	CHECK_EQ_INT(r->data[1].i, 3600);
	CHECK_EQ_INT(r->data[2].i, 26);
	CHECK_EQ_INT(r->data[3].i, 38);
	ak_release(r);
	ak_release(a.p);
	const char *bad[] = {"2026-03-07T15:04:05Z[Europe/Paris]", "2026-03-07T15:04:05Z[]", "2026-03-07T15:04:05Z[!]", "2026-03-07T15:04:05Z[UTC][UTC]",
	                     "2026-03-07T15:04:05Z[UTC]x", "2026-03-07T15:04:05Z[UTC", "2026-03-07T15:04:05Z]", "2026-03-07T15:04:05Z[!x=y]", "2026-03-07T15:04:05Z[+0000]"};
	for (size_t i = 0; i < AK_LEN(bad); i++) {
		ak_val b = S(bad[i]);
		fn("time.parseRFC3339")(&b, 1);
		ak_release(b.p);
		if (!ak_error_pending()) T_FAIL("%s parsed", bad[i]);
		else ak_release(ak_error_take());
	}
}

T_MAIN()
