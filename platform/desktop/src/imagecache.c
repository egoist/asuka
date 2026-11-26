// imagecache.c: images fetched over http(s), kept across launches in the
// app's caches, as iOS's and Android's hosts keep them (AsukaDiskCache):
// a file a URL, the least recently used going first past the limit. A
// file is named by two hashes of its URL and holds the URL before the
// image's bytes, so a name two URLs share is read as a miss.
#include "desktop.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <sys/utime.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#endif

#define MAGIC "AKI1"

// fnv is FNV-1a over s, from seed: two seeds make a 128-bit name.
static uint64_t fnv(const char *s, uint64_t seed) {
	uint64_t h = seed;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static char *file_of(const char *dir, const char *url) {
	char name[40];
	snprintf(name, sizeof name, "%016llx%016llx", (unsigned long long)fnv(url, 0xcbf29ce484222325ULL),
	         (unsigned long long)fnv(url, 0x84222325cbf29ce4ULL));
	return dsk_join(dir, name);
}

#ifdef _WIN32
static wchar_t *wide(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
	if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}
#endif

// touch marks path used now (its modification time), for the trim.
static void touch(const char *path) {
#ifdef _WIN32
	wchar_t *w = wide(path);
	if (w) _wutime(w, NULL);
	free(w);
#else
	utime(path, NULL);
#endif
}

uint8_t *dsk_image_cache_read(const char *dir, const char *url, size_t *len) {
	*len = 0;
	if (!dir || !url) return NULL;
	char *path = file_of(dir, url);
	if (!path) return NULL;
	size_t n = 0;
	char *b = dsk_read_file(path, &n);
	size_t ul = strlen(url);
	if (!b || n < 8 + ul || memcmp(b, MAGIC, 4) != 0) {
		free(b);
		free(path);
		return NULL;
	}
	uint32_t got = (uint32_t)(unsigned char)b[4] | (uint32_t)(unsigned char)b[5] << 8 | (uint32_t)(unsigned char)b[6] << 16 |
	               (uint32_t)(unsigned char)b[7] << 24;
	if (got != ul || memcmp(b + 8, url, ul) != 0) {  // another URL's, of the same name
		free(b);
		free(path);
		return NULL;
	}
	touch(path);
	free(path);
	size_t off = 8 + ul;
	*len = n - off;
	memmove(b, b + off, *len);
	return (uint8_t *)b;
}

// ---- Trimming ----

typedef struct {
	char *path;
	long long size, mtime;
} entry;

static int older(const void *a, const void *b) {
	long long x = ((const entry *)a)->mtime, y = ((const entry *)b)->mtime;
	return x < y ? -1 : x > y;
}

// list is dir's files (not its directories): their paths, sizes and
// modification times.
static entry *list(const char *dir, size_t *count) {
	entry *es = NULL;
	size_t n = 0, cap = 0;
#ifdef _WIN32
	char *pattern = dsk_join(dir, "*");
	wchar_t *wp = pattern ? wide(pattern) : NULL;
	free(pattern);
	WIN32_FIND_DATAW fd;
	HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
	free(wp);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			char name[MAX_PATH * 3];
			if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, (int)sizeof name, NULL, NULL)) continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				entry *m = realloc(es, cap * sizeof *es);
				if (!m) break;
				es = m;
			}
			es[n].path = dsk_join(dir, name);
			es[n].size = (long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
			es[n].mtime = (long long)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
			if (es[n].path) n++;
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
#else
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char *p = dsk_join(dir, e->d_name);
			struct stat st;
			if (!p || stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
				free(p);
				continue;
			}
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				entry *m = realloc(es, cap * sizeof *es);
				if (!m) {
					free(p);
					break;
				}
				es = m;
			}
			es[n++] = (entry){p, (long long)st.st_size, (long long)st.st_mtime};
		}
		closedir(d);
	}
#endif
	*count = n;
	return es;
}

static void remove_file(const char *path) {
#ifdef _WIN32
	wchar_t *w = wide(path);
	if (w) DeleteFileW(w);
	free(w);
#else
	remove(path);
#endif
}

void dsk_image_cache_trim(const char *dir, long long limit) {
	size_t n = 0;
	entry *es = list(dir, &n);
	long long total = 0;
	for (size_t i = 0; i < n; i++) total += es[i].size;
	if (total > limit) {
		qsort(es, n, sizeof *es, older);
		for (size_t i = 0; i < n && total > limit; i++) {
			remove_file(es[i].path);
			total -= es[i].size;
		}
	}
	for (size_t i = 0; i < n; i++) free(es[i].path);
	free(es);
}

// ---- Writing ----

static atomic_bool trimmed;

void dsk_image_cache_write(const char *dir, const char *url, const uint8_t *data, size_t len) {
	if (!dir || !url || !data) return;
	size_t ul = strlen(url);
	if (ul > UINT32_MAX || len > SIZE_MAX - 8 - ul) return;
	uint8_t *b = malloc(8 + ul + len);
	if (!b) return;
	memcpy(b, MAGIC, 4);
	b[4] = (uint8_t)ul, b[5] = (uint8_t)(ul >> 8), b[6] = (uint8_t)(ul >> 16), b[7] = (uint8_t)(ul >> 24);
	memcpy(b + 8, url, ul);
	memcpy(b + 8 + ul, data, len);
	char *path = file_of(dir, url);
	if (path) dsk_write_file(path, b, 8 + ul + len);
	free(path);
	free(b);
	// Once a run, after its first write (on the loader's thread, not the
	// main one): what's past the limit goes, the least recently used first.
	if (!atomic_exchange(&trimmed, true)) dsk_image_cache_trim(dir, DSK_IMAGE_CACHE_LIMIT);
}

bool dsk_image_cache_storable(const char *url, const char *cache_control) {
	if (cache_control) {
		char lower[256];
		size_t i = 0;
		for (; cache_control[i] && i + 1 < sizeof lower; i++) {
			char c = cache_control[i];
			lower[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
		}
		lower[i] = 0;
		if (strstr(lower, "no-store")) return false;
	}
	// Not this machine's (asuka dev's assets, which change).
	const char *h = strstr(url, "://");
	h = h ? h + 3 : url;
	static const char *const local[] = {"localhost", "127.0.0.1", "[::1]"};
	for (size_t i = 0; i < sizeof local / sizeof *local; i++) {
		size_t n = strlen(local[i]);
		if (!strncmp(h, local[i], n) && (h[n] == 0 || h[n] == ':' || h[n] == '/')) return false;
	}
	return true;
}
