// The standard library's builtins (core/src/std), called as the VM
// calls them: what the Asuka programs of lang/compile/testdata/run reach
// less easily (the local zone, hashes' block boundaries, programs a
// Regex was made with by hand, paths).
#define _POSIX_C_SOURCE 200809L  // setenv, tzset
#include "t.h"
#include "std/std.h"
#include "rt/sched.h"

#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

static ak_lib_fn fn(const char *name) {
	for (const ak_lib_entry *const *t = ak_std_libs; *t; t++)
		for (const ak_lib_entry *e = *t; e->name; e++)
			if (strcmp(e->name, name) == 0) return e->fn;
	for (const ak_lib_entry *e = ak_lib; e->name; e++)
		if (strcmp(e->name, name) == 0) return e->fn;
	fprintf(stderr, "no builtin %s\n", name);
	abort();
}

static ak_val S(const char *s) { return (ak_val){.p = ak_str_c(s)}; }
static ak_val I(int64_t i) { return (ak_val){.i = i}; }

// call calls a builtin with args, releasing the references among them
// (refs: a bit per argument).
static ak_val call(const char *name, ak_val *args, uint32_t n, uint32_t refs) {
	ak_val r = fn(name)(args, n);
	for (uint32_t i = 0; i < n; i++)
		if (refs >> i & 1) ak_release(args[i].p);
	return r;
}

static void check_str(ak_val v, const char *want, int line) {
	ak_str *s = v.p;
	if (!s || strcmp(s->data, want) != 0) {
		fprintf(stderr, "std_test.c:%d: %s: got %s, want %s\n", line, t_current, s ? s->data : "(none)", want);
		t_failed++;
	}
	ak_release(s);
}
#define CHECK_STR(v, want) check_str(v, want, __LINE__)

static int64_t offset_at(int64_t unix) { return call("time.localOffset", (ak_val[]){I(unix)}, 1, 0).i; }

TEST(local_offsets) {
#if !defined(_WIN32)
	const char *old = getenv("TZ");
	char *saved = old ? strcpy(malloc(strlen(old) + 1), old) : NULL;
	setenv("TZ", "America/New_York", 1);
	tzset();
	CHECK_EQ_INT(offset_at(1767225600), -5 * 3600);  // 2026-01-01: EST
	CHECK_EQ_INT(offset_at(1782864000), -4 * 3600);  // 2026-07-01: EDT
	setenv("TZ", "Asia/Kolkata", 1);
	tzset();
	CHECK_EQ_INT(offset_at(1767225600), 5 * 3600 + 1800);
	CHECK_EQ_INT(offset_at(-86400 * 365), 5 * 3600 + 1800);
	setenv("TZ", "UTC", 1);
	tzset();
	CHECK_EQ_INT(offset_at(1767225600), 0);
	// The manual clock (tests): UTC whatever TZ says, at the test epoch.
	setenv("TZ", "America/New_York", 1);
	tzset();
	ak_sched_manual_clock(true);
	CHECK_EQ_INT(offset_at(1782864000), 0);
	CHECK_EQ_INT(call("time.wallNanos", NULL, 0, 0).i, AK_TEST_EPOCH);
	ak_sched_advance(1500000000);
	CHECK_EQ_INT(call("time.wallNanos", NULL, 0, 0).i, AK_TEST_EPOCH + 1500000000);
	ak_sched_manual_clock(false);
	if (saved) setenv("TZ", saved, 1);
	else unsetenv("TZ");
	tzset();
	free(saved);
#endif
}

static ak_val fmt(const char *pattern, int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s, int64_t ns, int64_t wd, int64_t off) {
	return call("time.formatFields", (ak_val[]){S(pattern), I(y), I(mo), I(d), I(h), I(mi), I(s), I(ns), I(wd), I(off)}, 10, 1);
}

TEST(time_format) {
	CHECK_STR(fmt("yyyy-MM-dd'T'HH:mm:ssXXX", 2026, 3, 7, 15, 4, 5, 0, 6, -9000), "2026-03-07T15:04:05-02:30");
	CHECK_STR(fmt("y yy yyyyy", 7, 1, 1, 0, 0, 0, 0, 0, 0), "7 07 00007");
	CHECK_STR(fmt("h a K k", 2026, 1, 1, 0, 0, 0, 0, 4, 0), "12 AM 0 24");
	CHECK_STR(fmt("h a", 2026, 1, 1, 12, 0, 0, 0, 4, 0), "12 PM");
	CHECK_STR(fmt("D", 2024, 12, 31, 0, 0, 0, 0, 2, 0), "366");
	CHECK_STR(fmt("x xx xxx X", 2026, 1, 1, 0, 0, 0, 0, 4, 0), "+00 +0000 +00:00 Z");
	CHECK_STR(fmt("X", 2026, 1, 1, 0, 0, 0, 0, 4, 19800), "+0530");
	CHECK_STR(fmt("SS", 2026, 1, 1, 0, 0, 0, 999999999, 4, 0), "99");
	CHECK_STR(fmt("'unterminated", 2026, 1, 1, 0, 0, 0, 0, 4, 0), "unterminated");
	CHECK_STR(fmt("", 2026, 1, 1, 0, 0, 0, 0, 4, 0), "");
	CHECK_STR(fmt("é-ç", 2026, 1, 1, 0, 0, 0, 0, 4, 0), "é-ç");
}

TEST(time_format_any_fields) {
	// A Time made by hand has whatever fields it was given: they format
	// (months and weekdays around the year and the week), never read
	// outside the names or overflow.
	CHECK_STR(fmt("MMMM MMM", 2026, 0, 1, 0, 0, 0, 0, 4, 0), "December Dec");
	CHECK_STR(fmt("MMMM", 2026, -40, 1, 0, 0, 0, 0, 4, 0), "August");
	CHECK_STR(fmt("MMMM", 2026, 13, 1, 0, 0, 0, 0, 4, 0), "January");
	CHECK_STR(fmt("EEEE e", 2026, 1, 1, 0, 0, 0, 0, -1, 0), "Saturday 7");
	CHECK_STR(fmt("EEE e", 2026, 1, 1, 0, 0, 0, 0, INT64_MAX, 0), "Sun 1");
	CHECK_STR(fmt("y", INT64_MIN, 1, 1, 0, 0, 0, 0, 4, 0), "-9223372036854775808");
	CHECK_STR(fmt("xxx", 2026, 1, 1, 0, 0, 0, 0, 4, INT64_MIN), "-2562047788015215:30");
	CHECK_STR(fmt("D", INT64_MAX, 3, 1, 0, 0, 0, 0, 4, 0), "60");
	CHECK_STR(fmt("D", INT64_MIN, 12, 31, 0, 0, 0, 0, 4, 0), "366");
	CHECK_STR(fmt("D", 2026, 1, INT64_MAX, 0, 0, 0, 0, 4, 0), "9223372036854775807");
}

TEST(time_parse) {
	const char *good[] = {"2024-02-29T00:00:00Z", "1678-01-01T00:00:00Z", "2262-04-11T23:47:16.854775807Z"};
	for (size_t i = 0; i < AK_LEN(good); i++) {
		ak_val r = call("time.parseRFC3339", (ak_val[]){S(good[i])}, 1, 1);
		CHECK(!ak_error_pending() && r.p);
		ak_release(r.p);
	}
	ak_val r = call("time.parseRFC3339", (ak_val[]){S("1970-01-01T01:00:00.5+01:00")}, 1, 1);
	ak_arr *a = r.p;
	REQUIRE(a && a->len == 4);  // and where its zone annotation is: none
	CHECK_EQ_INT(a->data[0].i, 500000000);
	CHECK_EQ_INT(a->data[1].i, 3600);
	ak_release(a);
	const char *bad[] = {"2023-02-29T00:00:00Z", "2026-01-01T24:00:00Z", "2026-01-01T00:00:60Z", "2026-01-01T00:00:00.Z", "2026-01-01T00:00:00", "2026-01-01T00:00:00+0100", "2026-01-01T00:00:00Zjunk", "", "0001-01-01T00:00:00Z", "2262-04-11T23:47:16.854775808Z"};
	for (size_t i = 0; i < AK_LEN(bad); i++) {
		call("time.parseRFC3339", (ak_val[]){S(bad[i])}, 1, 1);
		if (!ak_error_pending()) T_FAIL("%s parsed", bad[i]);
		ak_release(ak_error_take());
	}
}

static void hex(const uint8_t *p, size_t n, char *out) {
	for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
}

TEST(sha) {
	// Lengths around the padding's boundaries (55, 56, 64 bytes), and a
	// million: FIPS 180's vectors and sha256sum's.
	struct { size_t n; const char *sha256; } cases[] = {
		{55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
		{56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
		{64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
		{1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		uint8_t *p = malloc(cases[i].n), out[32];
		memset(p, 'a', cases[i].n);
		ak_sha256(p, cases[i].n, out);
		char h[65];
		hex(out, 32, h);
		CHECK_EQ_STR(h, cases[i].sha256);
		free(p);
	}
	uint8_t out[20];
	char h[41];
	ak_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
	hex(out, 20, h);
	CHECK_EQ_STR(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(base64_padding) {
	const char *ok[][2] = {{"Zg", "f"}, {"Zg==", "f"}, {"Zm8", "fo"}, {"Zm8=", "fo"}, {"", ""}, {"Z m 9 v", "foo"}};
	for (size_t i = 0; i < AK_LEN(ok); i++) {
		ak_val r = call("encoding.base64Decode", (ak_val[]){S(ok[i][0])}, 1, 1);
		ak_bytes *b = r.p;
		REQUIRE(!ak_error_pending() && b);
		if (b->len != strlen(ok[i][1]) || memcmp(b->data, ok[i][1], b->len) != 0) T_FAIL("%s", ok[i][0]);
		ak_release(b);
	}
	const char *bad[] = {"Z", "Zg=", "Zg===", "Zm8==", "=Zg"};
	for (size_t i = 0; i < AK_LEN(bad); i++) {
		call("encoding.base64Decode", (ak_val[]){S(bad[i])}, 1, 1);
		if (!ak_error_pending()) T_FAIL("%s decoded", bad[i]);
		ak_release(ak_error_take());
	}
}

TEST(regex_programs) {
	ak_val prog = call("regex.program", (ak_val[]){S("(a+)b")}, 1, 1);
	REQUIRE(prog.p);
	ak_val r = call("regex.test", (ak_val[]){prog, S("xaab")}, 2, 2);
	CHECK(r.i);
	// A program changed by hand: a jump out of range, a bad magic, a
	// truncation. Each throws instead of running.
	ak_bytes *b = prog.p;
	ak_bytes *bad[3];
	for (int k = 0; k < 3; k++) bad[k] = ak_bytes_new(b->data, b->len);
	for (uint32_t i = 20; i + 12 <= b->len; i += 12)
		if (bad[0]->data[i] == 5 /* I_SPLIT */) bad[0]->data[i + 4] = 0xff, bad[0]->data[i + 5] = 0xff;
	bad[1]->data[0] = 'X';
	bad[2]->len -= 3;
	for (int k = 0; k < 3; k++) {
		call("regex.test", (ak_val[]){{.p = bad[k]}, S("aab")}, 2, 3);
		if (!ak_error_pending()) T_FAIL("program %d ran", k);
		ak_release(ak_error_take());
	}
	call("regex.test", (ak_val[]){{.p = NULL}, S("aab")}, 2, 2);
	CHECK(ak_error_pending());
	ak_release(ak_error_take());
	ak_release(prog.p);
	// Too large: repeats of repeats in groups.
	call("regex.program", (ak_val[]){S("((a{1000}){1000}){1000}")}, 1, 1);
	CHECK(ak_error_pending());
	ak_release(ak_error_take());
	// Nested too deep.
	char deep[2 * 300 + 2];
	for (int i = 0; i < 300; i++) deep[i] = '(', deep[300 + 1 + i] = ')';
	deep[300] = 'a';
	deep[601] = 0;
	call("regex.program", (ak_val[]){S(deep)}, 1, 1);
	CHECK(ak_error_pending());
	ak_release(ak_error_take());
}

// handmade makes a program by hand: its instructions ({op, x, y}) and
// its capture count, no ranges or names.
static ak_bytes *handmade(const uint32_t (*in)[3], uint32_t ninst, uint32_t ncap) {
	ak_bytes *b = ak_bytes_new(NULL, 20 + 12 * ninst);
	uint32_t head[4] = {ninst, 0, ncap, 0};
	memcpy(b->data, "AKRE", 4);
	for (int k = 0; k < 4; k++)
		for (int j = 0; j < 4; j++) b->data[4 + 4 * k + j] = (uint8_t)(head[k] >> 8 * j);
	for (uint32_t k = 0; k < ninst; k++)
		for (int f = 0; f < 3; f++)
			for (int j = 0; j < 4; j++) b->data[20 + 12 * k + 4 * f + j] = f == 0 ? (j == 0 ? (uint8_t)in[k][0] : 0) : (uint8_t)(in[k][f] >> 8 * j);
	return b;
}

TEST(regex_handmade_captures) {
	// Programs whose SAVEs put a match's captures where no pattern would:
	// a match with no start, a group with a start and no end, a match that
	// ends before it starts. Each throws instead of reading outside the
	// text or matching forever.
	enum { CHAR = 1, SAVE = 7, MATCH = 9 };
	const uint32_t no_start[][3] = {{SAVE, 1, 0}, {MATCH, 0, 0}};
	const uint32_t no_end[][3] = {{SAVE, 0, 0}, {SAVE, 2, 0}, {SAVE, 1, 0}, {MATCH, 0, 0}};
	const uint32_t backwards[][3] = {{SAVE, 1, 0}, {CHAR, 'a', 0}, {SAVE, 0, 0}, {MATCH, 0, 0}};
	struct {
		const uint32_t (*in)[3];
		uint32_t n, ncap;
	} progs[] = {{no_start, 2, 1}, {no_end, 4, 2}, {backwards, 4, 1}};
	for (size_t k = 0; k < AK_LEN(progs); k++) {
		ak_bytes *b = handmade(progs[k].in, progs[k].n, progs[k].ncap);
		ak_val r = call("regex.substitute", (ak_val[]){{.p = b}, S("abc"), S("[$1]"), I(-1)}, 4, 0x6);
		if (!ak_error_pending()) T_FAIL("program %zu substituted", k);
		ak_release(ak_error_take());
		ak_release(r.p);
		r = call("regex.search", (ak_val[]){{.p = b}, S("abc"), I(-1)}, 3, 0x2);
		if (!ak_error_pending()) T_FAIL("program %zu searched", k);
		ak_release(ak_error_take());
		ak_release(r.p);
		ak_release(b);
	}
}

TEST(regex_empty_repeats) {
	// A repeat of nothing compiles to nothing, however many times: not a
	// thousand to the fourth compiles of it.
	ak_val prog = call("regex.program", (ak_val[]){S("(?:(?:(?:(?:){1000}){1000}){1000}){1000}x")}, 1, 1);
	REQUIRE(prog.p);
	ak_val r = call("regex.test", (ak_val[]){prog, S("ax")}, 2, 3);
	CHECK(r.i);
	// A group in it still counts: a thousand saves of it, a thousand times
	// over, is too large.
	call("regex.program", (ak_val[]){S("(?:(?:()){1000}){1000}")}, 1, 1);
	CHECK(ak_error_pending());
	ak_release(ak_error_take());
}

// xorshift64: the randomized tests' numbers, the same every run.
static uint64_t rng = 0x9e3779b97f4a7c15;
static uint64_t next_rand(void) {
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

TEST(regex_random) {
	// Patterns made of the syntax's pieces compile or throw; compiled
	// programs with bytes changed load or throw; whatever loads searches
	// and substitutes within the text, through every template form.
	static const char *const pieces[] = {"a", "b", ".", "\\w", "\\d", "\\b", "^", "$", "(", ")", "(?:", "(?P<n>", "|", "*", "+", "?", "*?", "{2}", "{0,3}", "{1,}", "[a-c]", "[^b]", "(?i)", "(?m)", "(?s)", "\\", "é", "\xff", "", "{1000}"};
	static const char *const texts[] = {"", "a", "abcab", "aaaa bbbb", "x\nay\n", ("é\xff" "a1_b2")};
	static const char *const tmpls[] = {"", "$0", "[$1]", "${n}", "$$", "${", "$x$1y", "${0}${1}${2}"};
	for (int it = 0; it < 4000; it++) {
		char pat[96];
		size_t pl = 0;
		int np = (int)(next_rand() % 10);
		for (int k = 0; k < np; k++) {
			const char *q = pieces[next_rand() % AK_LEN(pieces)];
			size_t ql = strlen(q);
			if (pl + ql >= sizeof pat) break;
			memcpy(pat + pl, q, ql);
			pl += ql;
		}
		pat[pl] = 0;
		ak_val prog = call("regex.program", (ak_val[]){S(pat)}, 1, 1);
		if (!prog.p) {
			REQUIRE(ak_error_pending());
			ak_release(ak_error_take());
			continue;
		}
		ak_bytes *b = prog.p;
		// Half the time, a few of its bytes changed.
		if (next_rand() & 1) {
			int flips = 1 + (int)(next_rand() % 4);
			for (int k = 0; k < flips; k++) b->data[next_rand() % b->len] ^= (uint8_t)(1u << (next_rand() % 8));
		}
		const char *text = texts[next_rand() % AK_LEN(texts)];
		ak_val r = call("regex.test", (ak_val[]){prog, S(text)}, 2, 2);
		if (ak_error_pending()) {
			ak_release(ak_error_take());
			ak_release(b);
			continue;
		}
		r = call("regex.search", (ak_val[]){prog, S(text), I((int64_t)(next_rand() % 4) - 1)}, 3, 2);
		if (ak_error_pending()) ak_release(ak_error_take());
		ak_arr *ms = r.p;
		for (uint32_t m = 0; ms && m < ms->len; m++) {
			ak_arr *caps = ms->data[m].p;
			for (uint32_t c = 0; c + 1 < caps->len; c += 2) {
				int64_t lo = caps->data[c].i, hi = caps->data[c + 1].i;
				if (lo == -1 && hi == -1) continue;
				if (!(lo >= 0 && lo <= hi && (size_t)hi <= strlen(text))) T_FAIL("%s on %s: a capture at %lld..%lld", pat, text, (long long)lo, (long long)hi);
			}
		}
		ak_release(ms);
		r = call("regex.substitute", (ak_val[]){prog, S(text), S(tmpls[next_rand() % AK_LEN(tmpls)]), I(-1)}, 4, 6);
		if (ak_error_pending()) ak_release(ak_error_take());
		ak_release(r.p);
		r = call("regex.names", (ak_val[]){prog}, 1, 0);
		ak_release(r.p);
		ak_release(b);
	}
}

static ak_val search(const char *pattern, const char *text) {
	ak_val prog = call("regex.program", (ak_val[]){S(pattern)}, 1, 1);
	if (!prog.p) return prog;
	return call("regex.search", (ak_val[]){prog, S(text), I(-1)}, 3, 3);
}

TEST(regex_offsets) {
	// Invalid UTF-8 reads as U+FFFD, a byte at a time; \b at the text's
	// ends; anchors in the middle of a search.
	ak_val r = search(".", "a\xff" "b");
	ak_arr *a = r.p;
	REQUIRE(a);
	CHECK_EQ_INT(a->len, 3);
	ak_release(a);
	r = search("\\bx", "x x");
	a = r.p;
	REQUIRE(a && a->len == 2);
	CHECK_EQ_INT(((ak_arr *)a->data[1].p)->data[0].i, 2);
	ak_release(a);
	r = search("^a", "aaa");
	a = r.p;
	CHECK_EQ_INT(a->len, 1);
	ak_release(a);
	r = search("(?m)^a", "a\na\nb");
	a = r.p;
	CHECK_EQ_INT(a->len, 2);
	ak_release(a);
	// (?i) on classes and on Greek.
	r = search("(?i)[α-γ]+", "ΑΒΓδ");
	a = r.p;
	REQUIRE(a && a->len == 1);
	CHECK_EQ_INT(((ak_arr *)a->data[0].p)->data[1].i, 6);
	ak_release(a);
}

TEST(regex_alloc_bounded) {
	// Matching allocates per call, not per character: a longer text costs
	// no more allocations.
	ak_val prog = call("regex.program", (ak_val[]){S("(\\w+)@(\\w+)\\.com")}, 1, 1);
	char text[20001];
	memset(text, 'x', 20000);
	text[20000] = 0;
	ak_val t = S(text);
	uint64_t before = ak_alloc_count;
	call("regex.test", (ak_val[]){prog, t}, 2, 0);
	uint64_t one = ak_alloc_count - before;
	CHECK(one <= 4);
	ak_release(t.p);
	ak_release(prog.p);
}

TEST(paths) {
#if !defined(_WIN32)
	const char *clean[][2] = {
		{"", "."}, {".", "."}, {"/", "/"}, {"//a//b//", "/a/b"}, {"a/../..", ".."}, {"a/../../b", "../b"},
		{"/a/../../b", "/b"}, {"./a/./b/.", "a/b"}, {"../a/..", ".."}, {"a/b/../c/./d", "a/c/d"},
	};
	for (size_t i = 0; i < AK_LEN(clean); i++) CHECK_STR(call("fs.clean", (ak_val[]){S(clean[i][0])}, 1, 1), clean[i][1]);
	const char *dir[][2] = {{"/a/b", "/a"}, {"a", "."}, {"/", "/"}, {"a/b/", "a/b"}, {"", "."}};
	for (size_t i = 0; i < AK_LEN(dir); i++) CHECK_STR(call("fs.dir", (ak_val[]){S(dir[i][0])}, 1, 1), dir[i][1]);
#endif
}

TEST(app_dir) {
	ak_fs_set_app_dir("/x/y");
	CHECK_STR(call("fs.appDir", NULL, 0, 0), "/x/y");
	ak_fs_set_app_dir(NULL);
	char *d = ak_fs_temp_dir();
	REQUIRE(d);
	ak_fs_set_app_dir(d);
	CHECK_STR(call("fs.appDir", NULL, 0, 0), d);
	CHECK(call("fs.exists", (ak_val[]){S(d)}, 1, 1).i);
	CHECK(ak_fs_remove_tree(d));
	CHECK(!call("fs.exists", (ak_val[]){S(d)}, 1, 1).i);
	ak_fs_set_app_dir(NULL);
	ak_free(d);
}

TEST(url_parts) {
	ak_val r = call("url.parts", (ak_val[]){S("http://u@[::1]:/p")}, 1, 1);
	ak_arr *a = r.p;
	REQUIRE(a && a->len == 7);
	CHECK_EQ_STR(((ak_str *)a->data[2].p)->data, "::1");
	CHECK_EQ_STR(((ak_str *)a->data[3].p)->data, "");
	ak_release(a);
	r = call("url.parts", (ak_val[]){S("http://a@b@c:1/")}, 1, 1);
	a = r.p;
	REQUIRE(a && a->len == 7);
	CHECK_EQ_STR(((ak_str *)a->data[1].p)->data, "a@b");
	CHECK_EQ_STR(((ak_str *)a->data[2].p)->data, "c");
	ak_release(a);
	ak_str *nul = ak_str_new("a\0b", 3);
	r = call("url.escape", (ak_val[]){{.p = nul}}, 1, 1);
	CHECK_STR(r, "a%00b");
}

TEST(random_seeded) {
	call("random.seed", (ak_val[]){I(7)}, 1, 0);
	int64_t a = call("random.int", (ak_val[]){I(INT64_MIN), I(INT64_MAX)}, 2, 0).i;
	call("random.seed", (ak_val[]){I(7)}, 1, 0);
	int64_t b = call("random.int", (ak_val[]){I(INT64_MIN), I(INT64_MAX)}, 2, 0).i;
	CHECK_EQ_INT(a, b);
}

// std/markdown: the paragraphs std/markdown's view draws, as JSON.
TEST(markdown_read) {
	CHECK_STR(call("markdown.read", (ak_val[]){S("")}, 1, 1), "[]");
	CHECK_STR(call("markdown.read",
	               (ak_val[]){S("# Plan\n\nThree **steps**, *in* `order`:\n\n1. Read [docs](https://example.com) and [no](javascript:x)\n2. Write\n"
	                            "   - [x] nested\n\n> quoted\n\n---\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n```go\nfmt.Println(\"1\")\n```\n\n"
	                            "A&amp;B &#x263A; www.asuka.dev ~~gone~~<br>raw\n")},
	               1, 1),
	          "[{\"runs\":[{\"text\":\"Plan\"}],\"heading\":1},"
	          "{\"runs\":[{\"text\":\"Three \"},{\"text\":\"steps\",\"bold\":true},{\"text\":\", \"},{\"text\":\"in\",\"italic\":true},"
	          "{\"text\":\" \"},{\"text\":\"order\",\"code\":true},{\"text\":\":\"}]},"
	          "{\"runs\":[{\"text\":\"Read \"},{\"text\":\"docs\",\"link\":\"https://example.com\"},{\"text\":\" and \"},"
	          "{\"text\":\"no\",\"link\":\"javascript:x\"}],\"indent\":1,\"marker\":\"1.\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"Write\"}],\"indent\":1,\"marker\":\"2.\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"nested\"}],\"indent\":2,\"marker\":\"• ☑\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"quoted\",\"quoted\":true}],\"indent\":1,\"quote\":1},"
	          "{\"runs\":[],\"rule\":true},"
	          "{\"runs\":[{\"text\":\"a\",\"bold\":true},{\"text\":\"\\t\"},{\"text\":\"b\",\"bold\":true}],\"row\":true,\"header\":true,"
	          "\"aligns\":\"--\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"1\"},{\"text\":\"\\t\"},{\"text\":\"2\"}],\"row\":true,\"aligns\":\"--\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"fmt.Println(\\\"1\\\")\",\"code\":true}],\"code\":true,\"language\":\"go\"},"
	          "{\"runs\":[{\"text\":\"A&B ☺ \"},{\"text\":\"www.asuka.dev\",\"link\":\"http://www.asuka.dev\"},{\"text\":\" \"},"
	          "{\"text\":\"gone\",\"struck\":true},{\"text\":\"<br>raw\"}]}]");
	// A quote's list, a loose list, a hard break, a heading's emphasis.
	CHECK_STR(call("markdown.read", (ak_val[]){S("> - a\n>\n> - b\n\n## *It*  \nx\\\ny")}, 1, 1),
	          "[{\"runs\":[{\"text\":\"a\",\"quoted\":true}],\"indent\":2,\"quote\":1,\"marker\":\"•\"},"
	          "{\"runs\":[{\"text\":\"b\",\"quoted\":true}],\"indent\":2,\"quote\":1,\"marker\":\"•\"},"
	          "{\"runs\":[{\"text\":\"It\",\"italic\":true}],\"heading\":2},"
	          "{\"runs\":[{\"text\":\"x\\ny\"}]}]");
	// An image (its text its description), one in a link; a table's
	// alignments; a quote in a quote; a code block's language, the first
	// word of its info string.
	CHECK_STR(call("markdown.read",
	               (ak_val[]){S("![a cat](cat.png) and [![b](b.png)](https://b.dev)\n\n| l | c | r |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n\n"
	                            "> > deep\n\n```swift title=x\nlet a = 1\n```")},
	               1, 1),
	          "[{\"runs\":[{\"text\":\"a cat\",\"image\":\"cat.png\"},{\"text\":\" and \"},"
	          "{\"text\":\"b\",\"link\":\"https://b.dev\",\"image\":\"b.png\"}]},"
	          "{\"runs\":[{\"text\":\"l\",\"bold\":true},{\"text\":\"\\t\"},{\"text\":\"c\",\"bold\":true},{\"text\":\"\\t\"},"
	          "{\"text\":\"r\",\"bold\":true}],\"row\":true,\"header\":true,\"aligns\":\"lcr\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"1\"},{\"text\":\"\\t\"},{\"text\":\"2\"},{\"text\":\"\\t\"},{\"text\":\"3\"}],\"row\":true,"
	          "\"aligns\":\"lcr\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"deep\",\"quoted\":true}],\"indent\":2,\"quote\":2},"
	          "{\"runs\":[{\"text\":\"let a = 1\",\"code\":true}],\"code\":true,\"language\":\"swift\"}]");
	// An ordered list's start; a task not done.
	CHECK_STR(call("markdown.read", (ak_val[]){S("7. a\n8. [ ] b")}, 1, 1),
	          "[{\"runs\":[{\"text\":\"a\"}],\"indent\":1,\"marker\":\"7.\",\"tight\":true},"
	          "{\"runs\":[{\"text\":\"b\"}],\"indent\":1,\"marker\":\"8. ☐\",\"tight\":true}]");
}

// A link keeps its destination as written, whatever it is: what opens is
// the view's to decide (std/markdown's opens web, mail and phone links
// unless the app handles them).
TEST(markdown_links) {
	static const char *const cases[][2] = {
	    {"[a](https://x.dev/p?q=1)", "https://x.dev/p?q=1"},
	    {"[a](HTTP://X.DEV)", "HTTP://X.DEV"},
	    {"[a](mailto:me@x.dev)", "mailto:me@x.dev"},
	    {"<https://x.dev>", "https://x.dev"},
	    {"[a](javascript:alert(1))", "javascript:alert(1)"},
	    {"[a](/relative)", "/relative"},
	    {"[a](notes/2.md#top)", "notes/2.md#top"},
	    {"[a](<with space>)", "with space"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		ak_val r = call("markdown.read", (ak_val[]){S(cases[i][0])}, 1, 1);
		const char *got = strstr(((ak_str *)r.p)->data, "\"link\":\"");
		char want[128];
		snprintf(want, sizeof want, "\"link\":\"%s\"", cases[i][1]);
		if (!got || strncmp(got, want, strlen(want)) != 0) {
			fprintf(stderr, "std_test.c:%d: %s: %s read as %s\n", __LINE__, t_current, cases[i][0], ((ak_str *)r.p)->data);
			t_failed++;
		}
		ak_release(r.p);
	}
}

TEST(markdown_heal) {
	static const char *const cases[][2] = {
	    {"", ""},
	    {"Some **bold", "Some **bold**"},
	    {"Some *it and **both", "Some *it and **both***"},
	    {"a `code", "a `code`"},
	    {"```go\nfmt.", "```go\nfmt.\n```"},
	    {"```go\nfmt.\n", "```go\nfmt.\n```"},
	    {"~~~\nx\n~~~\n\n```", "~~~\nx\n~~~\n\n```\n```"},
	    {"see [docs](https://ex", "see [docs](https://ex)"},
	    {"* item and snake_case", "* item and snake_case"},
	    {"~~gone", "~~gone~~"},
	    {"done **here**\n\nnext", "done **here**\n\nnext"},
	    {"**a\n\nb _c", "**a\n\nb _c_"},
	    {"\\*not", "\\*not"},
	    {"é_x_é", "é_x_é"},
	    {"`a*` *b", "`a*` *b*"},
	    {"__a ~~b", "__a ~~b~~__"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) CHECK_STR(call("markdown.heal", (ak_val[]){S(cases[i][0])}, 1, 1), cases[i][1]);
}

// The language's own (rt/lib.c), at the ends of their ranges.

// float.fixed(digits) writes every digit, however many the number has.
TEST(fixed_large) {
	ak_val r = call("str.fixed", (ak_val[]){{.f = 1e300}, I(2)}, 2, 0);
	ak_str *s = r.p;
	CHECK_EQ_INT(s->len, 304);
	CHECK(s->data[0] == '1' && strcmp(s->data + 301, ".00") == 0);
	ak_release(s);
	r = call("str.fixed", (ak_val[]){{.f = -1.5}, I(20)}, 2, 0);
	CHECK_STR(r, "-1.50000000000000000000");
}

// A range longer than an array holds is an error, not one cut to its
// low 32 bits; the ends of int's range don't overflow.
TEST(ranges) {
	ak_val r = call("arr.range", (ak_val[]){I(0), I((INT64_C(1) << 32) + 1)}, 2, 0);
	void *e = ak_error_take();
	CHECK(e != NULL && r.p == NULL);
	ak_release(e);
	r = call("arr.range", (ak_val[]){I(-2), I(INT64_MAX)}, 2, 0);
	e = ak_error_take();
	CHECK(e != NULL && r.p == NULL);
	ak_release(e);
	r = call("arr.range", (ak_val[]){I(INT64_MAX - 2), I(INT64_MAX)}, 2, 0);
	REQUIRE(!ak_error_pending());
	ak_arr *a = r.p;
	REQUIRE(a->len == 2);
	CHECK(a->data[1].i == INT64_MAX - 1);
	ak_release(a);
	r = call("arr.range", (ak_val[]){I(3), I(-3)}, 2, 0);
	CHECK_EQ_INT(((ak_arr *)r.p)->len, 0);
	ak_release(r.p);
	// a..b takes b, the largest int too; b < a is none; a span past an
	// array's by one is an error.
	r = call("arr.range", (ak_val[]){I(INT64_MAX - 1), I(INT64_MAX), I(1)}, 3, 0);
	REQUIRE(!ak_error_pending());
	a = r.p;
	REQUIRE(a->len == 2);
	CHECK(a->data[1].i == INT64_MAX);
	ak_release(a);
	r = call("arr.range", (ak_val[]){I(3), I(2), I(1)}, 3, 0);
	CHECK_EQ_INT(((ak_arr *)r.p)->len, 0);
	ak_release(r.p);
	r = call("arr.range", (ak_val[]){I(0), I(UINT32_MAX), I(1)}, 3, 0);
	e = ak_error_take();
	CHECK(e != NULL && r.p == NULL);
	ak_release(e);
	r = call("arr.range", (ak_val[]){I(INT64_MIN), I(INT64_MAX), I(1)}, 3, 0);
	e = ak_error_take();
	CHECK(e != NULL && r.p == NULL);
	ak_release(e);
}

// overflowed is whether the call before threw "integer overflow".
static bool overflowed(void) {
	void *e = ak_error_take();
	bool ok = e != NULL && strcmp(ak_error_message(e)->data, "integer overflow") == 0;
	ak_release(e);
	return ok;
}

// The checked int's value, or INT64_MIN for none.
static int64_t checked(const char *name, int64_t a, int64_t b) {
	ak_box *r = call(name, (ak_val[]){I(a), I(b)}, 2, 0).p;
	if (!r) return INT64_MIN;
	int64_t v = r->v.i;
	ak_release(r);
	return v;
}

// Integers past their range throw, as the language's arithmetic does:
// abs(Int.min), a sum past Int.max; the checked methods give none.
// random_int's span is the whole range's.
TEST(int_extremes) {
	call("math.absi", (ak_val[]){I(INT64_MIN)}, 1, 0);
	CHECK(overflowed());
	CHECK_EQ_INT(call("math.absi", (ak_val[]){I(-5)}, 1, 0).i, 5);
	CHECK(call("math.absi", (ak_val[]){I(-INT64_MAX)}, 1, 0).i == INT64_MAX);
	ak_arr *xs = ak_arr_new(AK_K_INT, NULL, 3);
	xs->data[0].i = INT64_MAX - 1, xs->data[1].i = -5, xs->data[2].i = 6;
	xs->len = 3;
	CHECK(call("arr.sum", (ak_val[]){{.p = xs}}, 1, 0).i == INT64_MAX);
	xs->data[1].i = 5;
	call("arr.sum", (ak_val[]){{.p = xs}}, 1, 0);
	CHECK(overflowed());
	ak_release(xs);
	CHECK(checked("int.add_checked", INT64_MAX, 1) == INT64_MIN);
	CHECK(checked("int.add_checked", INT64_MAX, -1) == INT64_MAX - 1);
	CHECK(checked("int.sub_checked", -2, INT64_MAX) == INT64_MIN);
	CHECK(checked("int.sub_checked", 3, 5) == -2);
	CHECK(checked("int.mul_checked", INT64_MIN, -1) == INT64_MIN);
	CHECK(checked("int.mul_checked", -4, 5) == -20);
	REQUIRE(!ak_error_pending());
	for (int i = 0; i < 100; i++) {
		int64_t v = call("math.random_int", (ak_val[]){I(INT64_MIN), I(INT64_MAX)}, 2, 0).i;
		CHECK(v < INT64_MAX);
		v = call("math.random_int", (ak_val[]){I(-1), I(INT64_MAX)}, 2, 0).i;
		CHECK(v >= -1 && v < INT64_MAX);
	}
}

// race of no tasks has none to finish first: it waits, as never()
// does, until it's cancelled; nothing is left behind.
// Closures for the collections' builtins: a string's first letter as
// a key; an int's half when it's even, none when it's odd (a boxed
// optional); "x" throws.
static ak_val first_letter(ak_closure *c, ak_val *args, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	ak_str *s = args[0].p;
	if (s->len == 1 && s->data[0] == 'x') {
		ak_throw_msg("no x");
		return (ak_val){0};
	}
	return (ak_val){.p = ak_str_slice(s, 0, 1)};
}

static ak_val half_of_even(ak_closure *c, ak_val *args, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	if (args[0].i % 2) return (ak_val){.p = NULL};
	return (ak_val){.p = ak_box_new(AK_K_INT, (ak_val){.i = args[0].i / 2})};
}

static ak_arr *strs(const char *const *xs, uint32_t n) {
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, n);
	a->h.flags = 1;
	for (uint32_t i = 0; i < n; i++) a->data[a->len++].p = ak_str_c(xs[i]);
	return a;
}

// grouped, unique and compactMap hold what they keep and let go of the
// rest, an error included: no object outlives them.
TEST(collections_refs) {
	int64_t live = ak_obj_live;
	ak_closure *key = ak_closure_new(first_letter, NULL, 0, NULL);
	ak_arr *words = strs((const char *[]){"apple", "bob", "avocado", "bob"}, 4);
	ak_map *g = call("arr.grouped", (ak_val[]){{.p = words}, {.p = key}, I(AK_K_REF)}, 3, 0).p;
	REQUIRE(g && g->live == 2);
	ak_arr *keys = ak_map_keys(g);
	CHECK_STR((ak_val){.p = ak_retain(keys->data[0].p)}, "a");
	CHECK_STR((ak_val){.p = ak_retain(keys->data[1].p)}, "b");
	ak_val as;
	ak_str *a = ak_str_c("a");
	REQUIRE(ak_map_get(g, (ak_val){.p = a}, &as));
	CHECK_EQ_INT(((ak_arr *)as.p)->len, 2);
	ak_release(a);
	ak_release(keys);
	ak_release(g);
	ak_arr *u = call("arr.unique", (ak_val[]){{.p = words}}, 1, 0).p;
	CHECK_EQ_INT(u->len, 3);
	ak_release(u);
	ak_arr *bad = strs((const char *[]){"a", "x", "b"}, 3);
	CHECK(call("arr.grouped", (ak_val[]){{.p = bad}, {.p = key}, I(AK_K_REF)}, 3, 0).p == NULL);
	void *e = ak_error_take();
	CHECK(e != NULL);
	ak_release(e);
	ak_release(bad);
	ak_release(words);
	ak_release(key);
	ak_closure *half = ak_closure_new(half_of_even, NULL, 0, NULL);
	ak_arr *ns = ak_arr_new(AK_K_INT, NULL, 5);
	for (int i = 0; i < 5; i++) ns->data[ns->len++].i = i;
	ak_arr *h = call("arr.compact_map", (ak_val[]){{.p = ns}, {.p = half}, I(AK_K_INT)}, 3, 0).p;
	REQUIRE(h && h->len == 3);
	CHECK(h->data[0].i == 0 && h->data[1].i == 1 && h->data[2].i == 2);
	ak_release(h);
	ak_arr *ch = call("arr.chunked", (ak_val[]){{.p = ns}, I(2)}, 2, 0).p;
	CHECK_EQ_INT(ch->len, 3);
	ak_release(ch);
	call("arr.chunked", (ak_val[]){{.p = ns}, I(0)}, 2, 0);
	e = ak_error_take();
	CHECK(e != NULL);
	ak_release(e);
	ak_release(ns);
	ak_release(half);
	CHECK_EQ_INT(ak_obj_live, live);
}

// Radixes: every int round-trips; past an int's range is none.
TEST(radixes) {
	ak_str *s = call("int.to_string", (ak_val[]){I(INT64_MIN), I(2)}, 2, 0).p;
	CHECK_EQ_INT(s->len, 65);
	ak_box *b = call("str.to_int_radix", (ak_val[]){{.p = s}, I(2)}, 2, 0).p;
	REQUIRE(b);
	CHECK(b->v.i == INT64_MIN);
	ak_release(b);
	ak_release(s);
	CHECK_STR(call("int.to_string", (ak_val[]){I(-35), I(36)}, 2, 0), "-z");
	ak_str *big = ak_str_c("9223372036854775808");
	CHECK(call("str.to_int_radix", (ak_val[]){{.p = big}, I(10)}, 2, 0).p == NULL);
	ak_release(big);
	ak_str *sign = ak_str_c("-");
	CHECK(call("str.to_int_radix", (ak_val[]){{.p = sign}, I(10)}, 2, 0).p == NULL);
	ak_release(sign);
	CHECK_STR(call("str.reversed", (ak_val[]){{.p = ak_str_c("añ😀b")}}, 1, 1), "b😀ña");
	REQUIRE(!ak_error_pending());
}

TEST(race_of_none) {
	int64_t live = ak_obj_live;
	ak_arr *none = ak_arr_new(AK_K_REF, NULL, 0);
	ak_future *f = call("async.race", (ak_val[]){{.p = none}}, 1, 1).p;
	CHECK(f->state == AK_FUT_PENDING);
	ak_future_cancel(f);
	CHECK(f->state == AK_FUT_FAILED);
	ak_release(f);
	CHECK_EQ_INT(ak_obj_live, live);
}

// A timer far in the future (a sleep of years) is due then, not at once
// from a wrapped sum.
static int fired;
static void fire(void *ctx) {
	AK_UNUSED(ctx);
	fired++;
}

TEST(far_timers) {
	ak_sched_manual_clock(true);
	ak_sched_advance(1000000000);
	uint64_t id = ak_after(INT64_MAX, fire, NULL);
	ak_sched_advance(1000000000);
	ak_sched_run_posted();
	CHECK_EQ_INT(fired, 0);
	ak_timer_cancel(id);
	ak_sched_manual_clock(false);
}

#if !defined(_WIN32)
#include <sys/wait.h>

// A child's exit is reaped on the main thread, as its wait's answer is
// delivered: until then its pid stays its own (a zombie), so a kill the
// app sends meanwhile can't reach another process that reused it.
TEST(command_wait_reaps_on_the_main_thread) {
	ak_arr *argv = ak_str_arr(2);
	ak_str_arr_push(&argv, "-c", 2);
	ak_str_arr_push(&argv, "exit 3", 6);
	ak_arr *env = ak_str_arr(0);
	ak_val h = call("command.procSpawn", (ak_val[]){S("sh"), {.p = argv}, S(""), {.p = env}, I(0), I(2), I(2), I(2)}, 8, 0xf);
	REQUIRE(h.i > 0);
	pid_t pid = (pid_t)call("command.procPid", (ak_val[]){h}, 1, 0).i;
	ak_future *f = call("command.procWait", (ak_val[]){h}, 1, 0).p;
	ak_sched_wait_posted();  // it exited: its answer is posted, not delivered
	siginfo_t si;
	memset(&si, 0, sizeof si);
	CHECK(waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT | WNOHANG) == 0 && si.si_pid == pid);
	call("command.procKill", (ak_val[]){h, I(9)}, 2, 0);
	ak_sched_run_posted();
	REQUIRE(f->state == AK_FUT_DONE);
	CHECK_EQ_INT(f->result.i, 3);
	CHECK(waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT | WNOHANG) == -1);  // reaped
	ak_release(f);
	call("command.procRelease", (ak_val[]){h}, 1, 0);
}

// A child ended by a signal: 128 + it.
TEST(command_wait_signalled) {
	ak_arr *argv = ak_str_arr(2);
	ak_str_arr_push(&argv, "-c", 2);
	ak_str_arr_push(&argv, "kill -9 $$", 10);
	ak_arr *env = ak_str_arr(0);
	ak_val h = call("command.procSpawn", (ak_val[]){S("sh"), {.p = argv}, S(""), {.p = env}, I(0), I(2), I(2), I(2)}, 8, 0xf);
	REQUIRE(h.i > 0);
	ak_future *f = call("command.procWait", (ak_val[]){h}, 1, 0).p;
	while (f->state == AK_FUT_PENDING) {
		ak_sched_wait_posted();
		ak_sched_run_posted();
	}
	CHECK_EQ_INT(f->result.i, 128 + 9);
	ak_release(f);
	call("command.procRelease", (ak_val[]){h}, 1, 0);
}
#endif

#if !defined(_WIN32)
// fs.home and tempDir from the environment; abs against the working
// directory.
TEST(user_dirs) {
	char *home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	char *tmp = getenv("TMPDIR") ? strdup(getenv("TMPDIR")) : NULL;
	setenv("HOME", "/home/ann/", 1);
	CHECK_STR(call("fs.home", NULL, 0, 0), "/home/ann");
	unsetenv("HOME");
	ak_str *h = call("fs.home", NULL, 0, 0).p;  // the user's records
	CHECK(h && h->len > 0 && h->data[0] == '/');
	ak_release(h);
	setenv("TMPDIR", "/var/t//", 1);
	CHECK_STR(call("fs.tempDir", NULL, 0, 0), "/var/t");
	unsetenv("TMPDIR");
	CHECK_STR(call("fs.tempDir", NULL, 0, 0), "/tmp");
	if (home) setenv("HOME", home, 1);
	if (tmp) setenv("TMPDIR", tmp, 1);
	free(home);
	free(tmp);

	CHECK_STR(call("fs.absolute", (ak_val[]){S("/a/./b/../c")}, 1, 1), "/a/c");
	char cwd[4096];
	REQUIRE(getcwd(cwd, sizeof cwd));
	CHECK_STR(call("fs.absolute", (ak_val[]){S("")}, 1, 1), cwd);
	char want[4200];
	snprintf(want, sizeof want, "%s/x", strcmp(cwd, "/") == 0 ? "" : cwd);
	CHECK_STR(call("fs.absolute", (ak_val[]){S("a/../x/")}, 1, 1), want);
}

// fs.walkInfo: a link is what it links to, marked as a link (walk
// doesn't go into one); a broken one is the link itself.
TEST(walk_links) {
	char *d = ak_fs_temp_dir();
	REQUIRE(d);
	char sub[4200], link[4200], broken[4200];
	snprintf(sub, sizeof sub, "%s/sub", d);
	snprintf(link, sizeof link, "%s/link", d);
	snprintf(broken, sizeof broken, "%s/broken", d);
	REQUIRE(ak_fs_make_dirs(sub));
	REQUIRE(symlink(sub, link) == 0);
	REQUIRE(symlink("nowhere", broken) == 0);
	ak_arr *a = call("fs.walkInfo", (ak_val[]){S(sub)}, 1, 1).p;
	REQUIRE(a);
	CHECK(a->data[0].i == 1 && a->data[3].i == 0);
	ak_release(a);
	a = call("fs.walkInfo", (ak_val[]){S(link)}, 1, 1).p;
	REQUIRE(a);
	CHECK(a->data[0].i == 1 && a->data[3].i == 1);
	ak_release(a);
	a = call("fs.walkInfo", (ak_val[]){S(broken)}, 1, 1).p;
	REQUIRE(a);
	CHECK(a->data[0].i == 0 && a->data[3].i == 1);
	ak_release(a);
	CHECK(ak_fs_remove_tree(d));
	ak_free(d);
}
#endif

// term's choice of colors: setColors's, then FORCE_COLOR, NO_COLOR and
// TERM=dumb; a pipe or a file shows none.
#if !defined(_WIN32)
TEST(term_colors) {
	const char *names[] = {"FORCE_COLOR", "NO_COLOR", "TERM", "COLUMNS", "LINES"};
	char *saved[5];
	for (int i = 0; i < 5; i++) {
		const char *v = getenv(names[i]);
		saved[i] = v ? strdup(v) : NULL;
		unsetenv(names[i]);
	}
	int fd = 9;  // not a terminal (the tests' streams may be)
	CHECK(!call("term.colorsAt", (ak_val[]){I(fd)}, 1, 0).i);
	setenv("FORCE_COLOR", "1", 1);
	CHECK(call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);
	setenv("NO_COLOR", "1", 1);
	CHECK(call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);  // FORCE_COLOR wins
	setenv("FORCE_COLOR", "0", 1);
	CHECK(!call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);
	unsetenv("FORCE_COLOR");
	CHECK(!call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);
	setenv("NO_COLOR", "", 1);  // empty: as if it weren't set
	setenv("TERM", "dumb", 1);
	CHECK(!call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);
	call("term.setChoice", (ak_val[]){I(-1), I(1)}, 2, 0);  // always
	CHECK(call("term.colorsAt", (ak_val[]){I(1)}, 1, 0).i);
	CHECK(call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);
	setenv("FORCE_COLOR", "1", 1);
	call("term.setChoice", (ak_val[]){I(2), I(2)}, 2, 0);  // never, stderr only
	CHECK(call("term.colorsAt", (ak_val[]){I(1)}, 1, 0).i);
	CHECK(!call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);
	ak_term_reset();
	CHECK(call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);

	setenv("COLUMNS", "132", 1);
	setenv("LINES", "x", 1);  // not a number: the terminal's, if any
	CHECK_EQ_INT(call("term.dimension", (ak_val[]){I(0)}, 1, 0).i, 132);
	CHECK(call("term.dimension", (ak_val[]){I(1)}, 1, 0).i >= 0);
	// A test's process: no terminal, and only os.fake's environment.
	ak_os_testing(true);
	CHECK(!call("term.colorsAt", (ak_val[]){I(2)}, 1, 0).i);
	CHECK_EQ_INT(call("term.dimension", (ak_val[]){I(0)}, 1, 0).i, 0);
	CHECK(!call("term.setEcho", (ak_val[]){I(0)}, 1, 0).i);
	ak_os_testing(false);
	ak_os_reset();

	for (int i = 0; i < 5; i++) {
		if (saved[i]) setenv(names[i], saved[i], 1);
		else unsetenv(names[i]);
		free(saved[i]);
	}
}
#endif

T_MAIN()
