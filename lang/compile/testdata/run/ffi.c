// The C ffi.asuka calls.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asuka_ffi.h"

int64_t ffi_add(int64_t a, int64_t b) { return a + b; }
int8_t ffi_wrap8(int8_t x) { return x; }
uint8_t ffi_wrapu8(uint8_t x) { return x; }
uint64_t ffi_big(void) { return UINT64_MAX; }
int16_t ffi_neg16(int16_t x) { return (int16_t)-x; }
uint32_t ffi_u32(uint32_t x) { return x + 1; }
float ffi_half(float x) { return x / 2; }
double ffi_scale(double x, double by) { return x * by; }
bool ffi_not(bool b) { return !b; }

uint64_t ffi_len(const char *s) { return strlen(s); }
const char *ffi_greeting(void) { return "héllo from C"; }
const char *ffi_maybe(bool some) { return some ? "some" : NULL; }
bool ffi_is_null(const char *s) { return s == NULL; }

int64_t ffi_sum(const uint8_t *p, size_t n) {
	int64_t s = 0;
	for (size_t i = 0; i < n; i++) s += p[i];
	return s;
}
int64_t ffi_bytes_len(const uint8_t *p, size_t n) { return p ? (int64_t)n : -1; }

static int cell = 42;
void *ffi_ptr(void) { return &cell; }
void *ffi_null(void) { return NULL; }
int ffi_deref(void *p) { return p ? *(int *)p : -1; }

// Counters an extern type's release moves.
typedef struct counter { int64_t value; } counter;
static int64_t opened, closed;
counter *counter_new(int64_t start) {
	if (start < 0) return NULL;
	counter *c = malloc(sizeof *c);
	c->value = start;
	opened++;
	return c;
}
int64_t counter_bump(counter *c) { return ++c->value; }
void counter_free(counter *c) {
	closed++;
	free(c);
}
int64_t counters_open(void) { return opened - closed; }
int64_t counters_closed(void) { return closed; }

typedef struct { int32_t x; float y; } point;
typedef struct { point a, b; void *tag; bool closed; uint8_t width; } line;
point point_scale(point p, int32_t by) { return (point){p.x * by, p.y * (float)by}; }
double line_length2(line l) {
	double dx = l.b.x - l.a.x, dy = l.b.y - l.a.y;
	return dx * dx + dy * dy + (l.closed ? 1000 : 0) + l.width;
}
line line_flip(line l) { return (line){l.b, l.a, l.tag, !l.closed, (uint8_t)(l.width + 1)}; }

// Callbacks.
int64_t each_number(int64_t n, void (*f)(void *, int64_t), void *ctx) {
	for (int64_t i = 0; i < n; i++) f(ctx, i);
	return n;
}
int32_t fold(int32_t n, int32_t (*f)(void *, int32_t, int32_t), void *ctx) {
	int32_t acc = 0;
	for (int32_t i = 1; i <= n; i++) acc = f(ctx, acc, i);
	return acc;
}
void each_word(const char *s, void (*f)(void *, const char *, bool), void *ctx) {
	char buf[64];
	while (*s) {
		size_t n = strcspn(s, " ");
		memcpy(buf, s, n);
		buf[n] = 0;
		f(ctx, buf, s[n] == 0);
		s += n;
		while (*s == ' ') s++;
	}
	f(ctx, NULL, true);
}
bool maybe_call(void (*f)(void *), void *ctx) {
	if (!f) return false;
	f(ctx);
	return true;
}

// A callback kept, then called by a later extern call, and released.
static void (*kept)(void *, int64_t);
static void *kept_ctx;
void keep(void (*f)(void *, int64_t), void *ctx) {
	if (kept_ctx) asuka_callback_release(kept_ctx);
	kept = f;
	kept_ctx = ctx;
	asuka_callback_retain(ctx);
}
void call_kept(int64_t v) {
	if (kept) kept(kept_ctx, v);
}
void drop_kept(void) {
	if (kept_ctx) asuka_callback_release(kept_ctx);
	kept = NULL;
	kept_ctx = NULL;
}

// A callback called from another thread: posted to the main thread.
#if defined(_WIN32)
#include <windows.h>
static HANDLE worker;
static DWORD WINAPI work(LPVOID arg) {
	(void)arg;
	kept(kept_ctx, 7);
	return 0;
}
void call_kept_on_thread(void) { worker = CreateThread(NULL, 0, work, NULL, 0, NULL); }
void join_thread(void) {
	WaitForSingleObject(worker, INFINITE);
	CloseHandle(worker);
}
#else
#include <pthread.h>
static pthread_t worker;
static void *work(void *arg) {
	(void)arg;
	kept(kept_ctx, 7);
	return NULL;
}
void call_kept_on_thread(void) { pthread_create(&worker, NULL, work, NULL); }
void join_thread(void) { pthread_join(worker, NULL); }
#endif
