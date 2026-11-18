// std/toml's parser (core/src/std/toml.c) against the TOML test suite's
// TOML 1.0 cases (github.com/toml-lang/toml-test, vendored in
// testdata/toml/cases.txt by its gen.go): each valid document reads as
// the suite's JSON says, each invalid one is an error.
#include "t.h"
#include "std/std.h"

static char *read_file(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *p = malloc((size_t)len + 1);
	*n = fread(p, 1, (size_t)len, f);
	p[*n] = 0;
	fclose(f);
	return p;
}

TEST(toml_test_suite) {
	size_t n;
	char *all = read_file("testdata/toml/cases.txt", &n);
	REQUIRE(all);
	int valid = 0, invalid = 0;
	for (size_t i = 0; i < n;) {
		char kind[16], name[256];
		size_t tn = 0, jn = 0;
		const char *nl = memchr(all + i, '\n', n - i);
		REQUIRE(nl);
		char line[512];
		size_t ll = (size_t)(nl - (all + i));
		REQUIRE(ll < sizeof line);
		memcpy(line, all + i, ll);
		line[ll] = 0;
		int got = sscanf(line, "%15s %255s %zu %zu", kind, name, &tn, &jn);
		REQUIRE(got >= 3);
		const char *toml = nl + 1;
		const char *json = toml + tn;
		i = (size_t)(json - all) + (got == 4 ? jn : 0) + 1;
		ak_buf out = {0};
		char err[300] = {0};
		bool ok = ak_toml_tagged(toml, tn, &out, err, sizeof err);
		if (strcmp(kind, "valid") == 0) {
			valid++;
			if (!ok) T_FAIL("%s: %s", name, err);
			else if (out.len != jn || memcmp(out.data, json, jn) != 0) T_FAIL("%s:\n got: %.*s\nwant: %.*s", name, (int)out.len, (const char *)out.data, (int)jn, json);
		} else {
			invalid++;
			if (ok) T_FAIL("%s: no error; read as %.*s", name, (int)out.len, (const char *)out.data);
		}
		ak_buf_free(&out);
	}
	CHECK(valid > 200);
	CHECK(invalid > 400);
	free(all);
}

// Errors say where: line:column (in characters).
TEST(toml_errors) {
	struct {
		const char *src, *want;
	} cases[] = {
		{"a = 1\nb = \n", "2:5: expected a value"},
		{"a = 1\na = 2\n", "2:1: a key defined twice: 'a'"},
		{"[t]\n[t]\n", "2:1: a table defined twice: [t]"},
		{"s = \"é\\q\"", "1:7: a bad escape: \\q"},
		{"n = 01", "1:5: a number with a leading zero"},
		{"d = 2026-02-30", "1:5: a date out of range: 2026-02-30"},
		{"x = 1 y = 2", "1:7: expected the end of the line after a value"},
		{"t = {a = 1,\n b = 2}", "1:12: an inline table must be on one line"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		ak_buf out = {0};
		char err[300] = {0};
		CHECK(!ak_toml_tagged(cases[i].src, strlen(cases[i].src), &out, err, sizeof err));
		CHECK_EQ_STR(err, cases[i].want);
		ak_buf_free(&out);
	}
}

// Deep nesting is an error, not a deep recursion.
TEST(toml_depth) {
	size_t k = 100000;
	char *s = malloc(k * 2 + 8);
	memcpy(s, "a = ", 4);
	for (size_t i = 0; i < k; i++) s[4 + i] = '[';
	s[4 + k] = 0;
	ak_buf out = {0};
	char err[300] = {0};
	CHECK(!ak_toml_tagged(s, strlen(s), &out, err, sizeof err));
	CHECK(strstr(err, "nested too deeply") != NULL);
	// Dotted keys and headers too.
	size_t m = 0;
	for (size_t i = 0; i < 2000; i++) s[m++] = 'a', s[m++] = '.';
	memcpy(s + m, "b = 1", 6);
	CHECK(!ak_toml_tagged(s, strlen(s), &out, err, sizeof err));
	CHECK(strstr(err, "nested too deeply") != NULL);
	ak_buf_free(&out);
	free(s);
}

// Many keys in a table are found by its index.
TEST(toml_many_keys) {
	ak_buf src = {0};
	for (int i = 0; i < 5000; i++) ak_buf_printf(&src, "k%d = %d\n", i, i);
	ak_buf_printf(&src, "k17 = 1\n");
	ak_buf out = {0};
	char err[300] = {0};
	CHECK(!ak_toml_tagged((const char *)src.data, src.len, &out, err, sizeof err));
	CHECK_EQ_STR(err, "5001:1: a key defined twice: 'k17'");
	ak_buf_free(&src);
	ak_buf_free(&out);
}

T_MAIN()
