// imagecache_test.c: the images' cache (src/imagecache.c): what's written
// reads back, a file another URL's name shares reads as a miss, the least
// recently used go past the limit, and what may be kept.
// platform/desktop's TestImageCache runs it with a directory of its own.
#include "desktop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utime.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

// The cache's names, as imagecache.c makes them.
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

static bool there(const char *dir, const char *url) {
	size_t len = 0;
	uint8_t *b = dsk_image_cache_read(dir, url, &len);
	free(b);
	return b != NULL;
}

static void age(const char *dir, const char *url, long seconds) {
	char *p = file_of(dir, url);
	struct utimbuf t = {time(NULL) - seconds, time(NULL) - seconds};
	CHECK(utime(p, &t) == 0);
	free(p);
}

int main(int argc, char **argv) {
	if (argc < 2) return 2;
	const char *dir = argv[1];
	dsk_mkdirs(dir);

	// Written, read back; another URL isn't there; an empty body is kept.
	const char *a = "https://example.com/a.png";
	dsk_image_cache_write(dir, a, (const uint8_t *)"PNGDATA", 7);
	size_t len = 0;
	uint8_t *b = dsk_image_cache_read(dir, a, &len);
	CHECK(b && len == 7 && !memcmp(b, "PNGDATA", 7));
	free(b);
	CHECK(!there(dir, "https://example.com/b.png"));
	dsk_image_cache_write(dir, "https://example.com/empty", (const uint8_t *)"", 0);
	b = dsk_image_cache_read(dir, "https://example.com/empty", &len);
	CHECK(b && len == 0);
	free(b);

	// A file under a's name that says another URL (two URLs' names the
	// same) is a miss, not that URL's image.
	{
		const char *other = "https://example.com/other.png";
		uint8_t f[64];
		size_t ul = strlen(other);
		memcpy(f, "AKI1", 4);
		f[4] = (uint8_t)ul, f[5] = f[6] = f[7] = 0;
		memcpy(f + 8, other, ul);
		memcpy(f + 8 + ul, "XY", 2);
		char *p = file_of(dir, a);
		CHECK(dsk_write_file(p, f, 8 + ul + 2));
		free(p);
		CHECK(!there(dir, a));
		// Nor is a file of something else.
		p = file_of(dir, a);
		CHECK(dsk_write_file(p, "garbage", 7));
		free(p);
		CHECK(!there(dir, a));
	}

	// Trimming: the least recently used go first. Three files of 4 KB, 1
	// the oldest and 3 the newest, then 1 read (used now: 2 is the
	// oldest); past 9 KB, 2 goes and 1 and 3 stay.
	dsk_image_cache_trim(dir, 0);  // from nothing
	char big[4096];
	memset(big, 'x', sizeof big);
	const char *urls[] = {"https://t/1", "https://t/2", "https://t/3"};
	for (int i = 0; i < 3; i++) dsk_image_cache_write(dir, urls[i], (const uint8_t *)big, sizeof big);
	age(dir, urls[0], 300);
	age(dir, urls[1], 200);
	age(dir, urls[2], 100);
	CHECK(there(dir, urls[0]));  // used now
	dsk_image_cache_trim(dir, 9 << 10);
	CHECK(there(dir, urls[0]));
	CHECK(!there(dir, urls[1]));
	CHECK(there(dir, urls[2]));
	// Under a limit of 0, all go.
	dsk_image_cache_trim(dir, 0);
	CHECK(!there(dir, urls[0]) && !there(dir, urls[2]));

	// What may be kept.
	CHECK(dsk_image_cache_storable("https://example.com/x.png", NULL));
	CHECK(dsk_image_cache_storable("https://example.com/x.png", "public, max-age=60"));
	CHECK(!dsk_image_cache_storable("https://example.com/x.png", "No-Store"));
	CHECK(!dsk_image_cache_storable("http://localhost:8790/a.png", NULL));
	CHECK(!dsk_image_cache_storable("http://127.0.0.1/a.png", NULL));
	CHECK(dsk_image_cache_storable("https://localhost.example.com/a.png", NULL));

	printf(failed ? "FAIL\n" : "ok\n");
	return failed;
}
