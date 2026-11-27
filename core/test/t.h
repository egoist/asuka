// t.h: the core's test harness. A test file defines tests with TEST and
// ends with T_MAIN(); core_test.go builds each core/test/*_test.c with
// the whole core and runs it. Run one test: ./x_test name.
#ifndef AK_T_H
#define AK_T_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef void (*t_fn)(void);
typedef struct { const char *name; t_fn fn; } t_case;

extern t_case t_cases[];
extern int t_ncases;
extern int t_failed;
extern const char *t_current;
// AK_TEST_DIAG=1: the run-time diagnostics on (base.h), as in a
// development build; core_test.go runs every test both ways.
extern bool ak_diag_enabled;

#define TEST(name) \
	static void test_##name(void); \
	__attribute__((constructor)) static void reg_##name(void) { t_cases[t_ncases++] = (t_case){#name, test_##name}; } \
	static void test_##name(void)

#define T_FAIL(...) do { fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, t_current); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); t_failed++; } while (0)
#define CHECK(c) do { if (!(c)) T_FAIL("check failed: %s", #c); } while (0)
#define REQUIRE(c) do { if (!(c)) { T_FAIL("required: %s", #c); return; } } while (0)
#define CHECK_EQ_INT(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) T_FAIL("%s == %s: %lld != %lld", #a, #b, a_, b_); } while (0)
#define CHECK_EQ_F(a, b) do { double a_ = (a), b_ = (b); if (!(fabs(a_ - b_) < 1e-3 || (a_ != a_ && b_ != b_))) T_FAIL("%s == %s: %g != %g", #a, #b, a_, b_); } while (0)
#define CHECK_EQ_STR(a, b) do { const char *a_ = (a), *b_ = (b); if (strcmp(a_ ? a_ : "(null)", b_ ? b_ : "(null)") != 0) T_FAIL("%s == %s:\n got: %s\nwant: %s", #a, #b, a_, b_); } while (0)

// AK_TEST_DIR: the directory a test runs in, for a runner that can't
// set it (a watch simulator's spawn: core/watchos_test.go).
#if !defined(_WIN32)
int chdir(const char *path); // unistd.h's: not included, as tests name their own read and write
#define T_CHDIR() do { const char *d_ = getenv("AK_TEST_DIR"); if (d_ && chdir(d_) != 0) perror(d_); } while (0)
#else
#define T_CHDIR() ((void)0)
#endif

#define T_MAIN() \
	t_case t_cases[4096]; int t_ncases; int t_failed; const char *t_current; \
	int main(int argc, char **argv) { \
		int ran = 0; \
		if (getenv("AK_TEST_DIAG")) ak_diag_enabled = true; \
		T_CHDIR(); \
		for (int i = 0; i < t_ncases; i++) { \
			if (argc > 1 && strstr(t_cases[i].name, argv[1]) == NULL) continue; \
			t_current = t_cases[i].name; \
			int before = t_failed; \
			t_cases[i].fn(); \
			ran++; \
			if (getenv("T_VERBOSE")) fprintf(stderr, "%s %s\n", t_failed == before ? "ok  " : "FAIL", t_current); \
		} \
		fprintf(stderr, "%d tests, %d failures\n", ran, t_failed); \
		return t_failed ? 1 : 0; \
	}

#endif
