// checksum.c: CRC-32 with the system's zlib (or the app's own, where the
// platform has none), and callbacks into Asuka.
#include "checksum.h"

#include <stdlib.h>
#include <string.h>

#include "asuka_ffi.h"

#if defined(CHECKSUM_OWN_CRC)
static uint32_t crc_of(uint32_t crc, const uint8_t *p, size_t n) {
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

// The zlib this app calls directly, where there's none.
const char *zlibVersion(void);
const char *zlibVersion(void) { return "built in"; }
#else
#include <zlib.h>

// crc32 takes a uInt's length (crc32_z, a size_t's, is Android 9's and
// later): a gigabyte at a time.
static uint32_t crc_of(uint32_t crc, const uint8_t *p, size_t n) {
	uLong c = crc;
	while (n > 0) {
		uInt k = n > (1u << 30) ? (1u << 30) : (uInt)n;
		c = crc32(c, p, k);
		p += k;
		n -= k;
	}
	return (uint32_t)c;
}
#endif

uint32_t checksum_crc32(const uint8_t *data, size_t len) { return crc_of(0, data, len); }

int64_t checksum_seed(void) { return CHECKSUM_SEED; }

int32_t checksum_lines(const char *text, void (*each)(void *, int64_t, uint32_t), void *ctx) {
	int32_t n = 0;
	for (const char *s = text; *s;) {
		size_t len = strcspn(s, "\n");
		each(ctx, n++, crc_of(0, (const uint8_t *)s, len));
		s += len;
		if (*s == '\n') s++;
	}
	return n;
}

// A checksum on another thread: its callback is posted to the main
// thread, and released once called.
typedef struct {
	char *text;
	void (*done)(void *, uint32_t);
	void *ctx;
} job;

static void work(job *j) {
	j->done(j->ctx, crc_of(0, (const uint8_t *)j->text, strlen(j->text)));
	asuka_callback_release(j->ctx);
	free(j->text);
	free(j);
}

#if defined(__wasm__)
// No threads: at once.
void checksum_later(const char *text, void (*done)(void *, uint32_t), void *ctx) {
	job j = {(char *)text, done, ctx};
	asuka_callback_retain(ctx);
	j.done(j.ctx, crc_of(0, (const uint8_t *)text, strlen(text)));
	asuka_callback_release(ctx);
}
void checksum_wait(void) {}
#elif defined(_WIN32)
#include <windows.h>
static HANDLE worker;
static DWORD WINAPI run(LPVOID arg) {
	work(arg);
	return 0;
}
void checksum_later(const char *text, void (*done)(void *, uint32_t), void *ctx) {
	job *j = malloc(sizeof *j);
	j->text = strdup(text);
	j->done = done;
	j->ctx = ctx;
	asuka_callback_retain(ctx);
	worker = CreateThread(NULL, 0, run, j, 0, NULL);
}
void checksum_wait(void) {
	if (!worker) return;
	WaitForSingleObject(worker, INFINITE);
	CloseHandle(worker);
	worker = NULL;
}
#else
#include <pthread.h>
static pthread_t worker;
static int started;
static void *run(void *arg) {
	work(arg);
	return NULL;
}
void checksum_later(const char *text, void (*done)(void *, uint32_t), void *ctx) {
	job *j = malloc(sizeof *j);
	j->text = strdup(text);
	j->done = done;
	j->ctx = ctx;
	asuka_callback_retain(ctx);
	started = pthread_create(&worker, NULL, run, j) == 0;
}
void checksum_wait(void) {
	if (started) pthread_join(worker, NULL);
	started = 0;
}
#endif
