// The shared desktop C's tests (platform/desktop/desktop_test.go builds
// and runs them on this computer).
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  // strdup, readlink under -std=c17
#endif
#include "desktop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

static void test_json(void) {
	const char *t = "{\"a\": \"x\\u00e9\\n\", \"b\": [1, 2.5, true, null], \"c\": {\"d\": false}, \"a\": \"last\"}";
	dsk_json *j = dsk_json_parse(t, strlen(t));
	CHECK(j && j->type == DSK_JOBJ);
	CHECK(strcmp(dsk_json_str(j, "a", ""), "last") == 0);
	const dsk_json *b = dsk_json_get(j, "b");
	CHECK(b && b->type == DSK_JARR && b->n == 4 && b->kids[1].num == 2.5 && b->kids[2].type == DSK_JTRUE);
	CHECK(dsk_json_bool(dsk_json_get(j, "c"), "d", true) == false);
	dsk_jw w = {0};
	dsk_jw_value(&w, j);
	char *out = dsk_jw_take(&w, NULL);
	CHECK(out && strcmp(out, "{\"a\":\"x\xc3\xa9\\n\",\"b\":[1,2.5,true,null],\"c\":{\"d\":false},\"a\":\"last\"}") == 0);
	free(out);
	dsk_json_free(j);
	CHECK(dsk_json_parse("{\"a\":}", 6) == NULL);
	CHECK(dsk_json_parse("[1,2] x", 7) == NULL);
	const char *s = "\"\\ud83d\\ude00<\"";
	j = dsk_json_parse(s, strlen(s));
	CHECK(j && strcmp(j->str, "\xf0\x9f\x98\x80<") == 0);
	dsk_jw_value(&w, j);
	out = dsk_jw_take(&w, NULL);
	CHECK(out && strcmp(out, "\"\xf0\x9f\x98\x80\\u003c\"") == 0);
	free(out);
	dsk_json_free(j);
}

static void test_exists(const char *dir) {
	CHECK(dsk_exists(dir));
	char *p = dsk_join(dir, "nothing-here");
	CHECK(!dsk_exists(p));
	dsk_write_file(p, "x", 1);
	CHECK(dsk_exists(p));
	free(p);
	CHECK(!dsk_exists(NULL) && !dsk_exists(""));
}

static void test_state(const char *dir) {
	char *path = dsk_join(dir, "asuka-window.json");
	dsk_write_file(path, "{\"x\":10,\"y\":20}", 15);  // an older file: the main window's
	dsk_winstate *s = dsk_winstate_open(path);
	const dsk_json *m = dsk_winstate_load(s, "main");
	CHECK(m && dsk_json_num(m, "x", 0) == 10);
	dsk_winstate_save(s, "prefs", "{\"w\":3}");
	dsk_winstate_close(s);
	s = dsk_winstate_open(path);
	CHECK(dsk_json_num(dsk_winstate_load(s, "prefs"), "w", 0) == 3);
	CHECK(dsk_json_num(dsk_winstate_load(s, "main"), "y", 0) == 20);
	dsk_winstate_forget(s, "prefs");
	CHECK(dsk_winstate_load(s, "prefs") == NULL);
	dsk_winstate_close(s);
	free(path);

	path = dsk_join(dir, "asuka-routes.json");
	dsk_routes *r = dsk_routes_open(path);
	CHECK(dsk_routes_set(r, "main", "/inbox/3", 8, "{}", 2));
	CHECK(!dsk_routes_set(r, "main", "/inbox/3", 8, "{}", 2));
	dsk_routes_flush(r);
	dsk_routes_close(r);
	r = dsk_routes_open(path);
	const char *href, *state;
	size_t sl;
	CHECK(dsk_routes_location(r, "main", &href, &state, &sl) && strcmp(href, "/inbox/3") == 0 && sl == 2);
	dsk_routes_close(r);
	free(path);

	// Where the file is: none under automation unless ASUKA_ROUTES=1,
	// none under asuka dev.
	setenv("ASUKA_DATA", dir, 1);
	unsetenv("ASUKA_SCRIPT"), unsetenv("ASUKA_ROUTES"), unsetenv("ASUKA_DEV");
	unsetenv("ASUKA_SNAPSHOT"), unsetenv("ASUKA_TEST"), unsetenv("ASUKA_STATE");
	path = dsk_routes_path();
	char *want = dsk_join(dir, "asuka-routes.json");
	CHECK(path && strcmp(path, want) == 0);
	free(path);
	setenv("ASUKA_SCRIPT", "tap:x", 1);
	path = dsk_routes_path();
	CHECK(path && !*path);
	free(path);
	setenv("ASUKA_ROUTES", "1", 1);
	path = dsk_routes_path();
	CHECK(path && strcmp(path, want) == 0);
	free(path);
	unsetenv("ASUKA_SCRIPT"), unsetenv("ASUKA_ROUTES");
	setenv("ASUKA_DEV", "127.0.0.1:1", 1);
	path = dsk_routes_path();
	CHECK(path && !*path);
	free(path);
	unsetenv("ASUKA_DEV");
	free(want);
}

static bool same(dsk_rect a, dsk_rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

// the desktop hosts' cases (the web's window.test.js has them too).
static void test_flyout(void) {
	dsk_rect work = {0, 25, 1440, 875}, icon = {1200, 0, 24, 25};
	struct {
		dsk_rect anchor;
		double w, h;
		int p;
		double gap;
		dsk_rect want;
	} cs[] = {
		{icon, 280, 300, 0, 0, {1072, 25, 280, 300}},
		{{1420, 0, 20, 25}, 280, 300, 0, 0, {1160, 25, 280, 300}},
		{{1300, 100, 100, 20}, 200, 100, 1, 4, {1200, 124, 200, 100}},
		{{100, 100, 100, 20}, 60, 50, 2, 0, {140, 120, 60, 50}},
		{{1300, 870, 24, 30}, 280, 300, 0, 8, {1160, 562, 280, 300}},
		{{600, 500, 40, 20}, 100, 100, 3, 4, {570, 396, 100, 100}},
		{{100, 300, 50, 20}, 100, 60, 7, 0, {150, 300, 100, 60}},
		{{1400, 300, 20, 20}, 100, 60, 6, 0, {1300, 280, 100, 60}},
		{icon, 200, 2000, 0, 0, {1112, 25, 200, 875}},
	};
	for (size_t i = 0; i < sizeof cs / sizeof *cs; i++) CHECK(same(dsk_flyout_place(cs[i].anchor, cs[i].w, cs[i].h, cs[i].p, cs[i].gap, work), cs[i].want));
	CHECK(same(dsk_flyout_center(600, 60, work), (dsk_rect){420, 25 + 815.0 / 3, 600, 60}));
}

static void test_words(void) {
	CHECK(strcmp(dsk_word("ja_JP", "back"), "\xe6\x88\xbb\xe3\x82\x8b") == 0);  // 戻る
	CHECK(strcmp(dsk_word("en-US", "ok"), "OK") == 0);
	CHECK(strcmp(dsk_word("xx", "search"), "Search") == 0);
	CHECK(strcmp(dsk_word("zh-TW", "back"), dsk_word("zh-Hant", "back")) == 0);
	char out[256];
	const char *vars[] = {"app", "Notes"};
	dsk_word_format(out, sizeof out, "en", "quit", vars, 2);
	CHECK(strcmp(out, "Quit Notes") == 0);
	const char *count[] = {"count", "3"};
	dsk_word_format(out, sizeof out, "en", "badgeCount", count, 2);
	CHECK(strcmp(out, "3 items") == 0);
	const char *one[] = {"count", "1"};
	dsk_word_format(out, sizeof out, "en", "badgeCount", one, 2);
	CHECK(strcmp(out, "1 item") == 0);
}

// ---- Animated images ----

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

// A still decoder that reads no pixels: a PNG's IHDR size, filled with
// gray 200 at alpha 200; a WebP's fake VP8L byte, its gray, at the size
// the test says.
typedef struct {
	int calls, w[2], h[2];
} fake_still;

static bool still(void *ctx, const uint8_t *d, size_t n, uint8_t **rgba, int *w, int *h) {
	fake_still *f = ctx;
	int i = f->calls++;
	uint8_t c = 200, a = 200;
	if (n > 24 && !memcmp(d + 1, "PNG", 3)) {
		*w = (int)be32(d + 16), *h = (int)be32(d + 20);
		CHECK(!memcmp(d + n - 8, "IEND", 4));
	} else {
		CHECK(n >= 21 && !memcmp(d, "RIFF", 4) && !memcmp(d + 12, "VP8L", 4));
		c = d[20], a = 255;
		*w = f->w[i & 1], *h = f->h[i & 1];
	}
	size_t px = (size_t)*w * (size_t)*h;
	*rgba = malloc(px * 4);
	for (size_t k = 0; k < px; k++) (*rgba)[k * 4] = (*rgba)[k * 4 + 1] = (*rgba)[k * 4 + 2] = c, (*rgba)[k * 4 + 3] = a;
	return true;
}

static uint8_t *at(dsk_anim_player *p, int x, int y) { return p->canvas + ((size_t)y * (size_t)p->a->w + (size_t)x) * 4; }

static uint8_t *read_test_file(const char *dir, const char *name, size_t *n) {
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	return (uint8_t *)dsk_read_file(path, n);
}

static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24), b[1] = (uint8_t)(v >> 16), b[2] = (uint8_t)(v >> 8), b[3] = (uint8_t)v; }

// chunk appends a PNG chunk (its CRC unchecked here) at b + *n.
static void chunk(uint8_t *b, size_t *n, const char *type, const uint8_t *data, uint32_t len) {
	put32(b + *n, len);
	memcpy(b + *n + 4, type, 4);
	if (len) memcpy(b + *n + 8, data, len);
	memset(b + *n + 8 + len, 0, 4);
	*n += 12 + len;
}

// An APNG's frame offsets are 32 bits unsigned: one past what an int holds,
// or at its edge, is off the canvas, never an int overflow placing it.
static void test_apng_offsets(void) {
	static const uint32_t xs[] = {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u};
	for (size_t i = 0; i < sizeof xs / sizeof *xs; i++) {
		uint8_t b[512];
		size_t n = 8;
		memcpy(b, "\x89PNG\r\n\x1a\n", 8);
		uint8_t ihdr[13] = {0, 0, 0, 4, 0, 0, 0, 4, 8, 6, 0, 0, 0};
		chunk(b, &n, "IHDR", ihdr, 13);
		uint8_t actl[8] = {0, 0, 0, 2, 0, 0, 0, 0};
		chunk(b, &n, "acTL", actl, 8);
		uint8_t fc[26] = {0};
		put32(fc + 4, 4), put32(fc + 8, 4);
		chunk(b, &n, "fcTL", fc, 26);
		chunk(b, &n, "IDAT", (const uint8_t *)"x", 1);
		put32(fc, 1), put32(fc + 4, 2), put32(fc + 8, 2);
		put32(fc + 12, xs[i]), put32(fc + 16, xs[i]);
		chunk(b, &n, "fcTL", fc, 26);
		uint8_t fd[5] = {0, 0, 0, 2, 'y'};
		chunk(b, &n, "fdAT", fd, 5);
		chunk(b, &n, "IEND", NULL, 0);
		fake_still f = {0};
		dsk_anim *a = dsk_anim_decode(b, n, still, &f);
		CHECK(a && a->n == 2);
		if (!a) continue;
		dsk_anim_player p;
		CHECK(dsk_anim_player_init(&p, a));
		dsk_anim_next(&p, NULL);
		dsk_anim_next(&p, NULL);  // off the canvas: the first frame stays
		CHECK(at(&p, 0, 0)[3] == 200 && at(&p, 3, 3)[3] == 200);
		dsk_anim_next(&p, NULL);
		dsk_anim_player_free(&p);
		dsk_anim_free(a);
	}
}

static void test_anim(const char *dir) {
	size_t n;
	uint8_t *d = read_test_file(dir, "anim.gif", &n);
	CHECK(d && dsk_anim_is_animated(d, n));
	dsk_anim *a = d ? dsk_anim_decode(d, n, NULL, NULL) : NULL;
	free(d);
	CHECK(a && a->w == 4 && a->h == 4 && a->n == 2 && a->loops == 3);
	if (a) {
		CHECK(a->frames[0].delay_ms == 50 && a->frames[1].delay_ms == 100 && a->frames[1].disposal == DSK_DISPOSE_BACKGROUND);
		dsk_anim_player p;
		CHECK(dsk_anim_player_init(&p, a));
		int ms = 0;
		CHECK(dsk_anim_next(&p, &ms) && ms == 50);
		CHECK(at(&p, 3, 3)[0] == 255 && at(&p, 3, 3)[3] == 255);
		dsk_anim_next(&p, &ms);
		CHECK(at(&p, 3, 3)[2] == 255 && at(&p, 0, 0)[0] == 255);
		CHECK(at(&p, 2, 2)[0] == 255 && at(&p, 2, 2)[2] == 0);  // transparent: frame 0 shows
		dsk_anim_next(&p, &ms);  // frame 1's rectangle cleared, the second loop
		CHECK(p.last == 0 && at(&p, 3, 3)[0] == 255);
		dsk_anim_next(&p, &ms);
		dsk_anim_next(&p, &ms);
		dsk_anim_next(&p, &ms);
		CHECK(!dsk_anim_next(&p, &ms));  // three plays
		CHECK(at(&p, 3, 3)[2] == 255);  // the last frame stays
		dsk_anim_player_free(&p);
		dsk_anim_free(a);
	}

	d = read_test_file(dir, "anim.png", &n);
	CHECK(d && dsk_anim_is_animated(d, n));
	fake_still f = {0};
	a = d ? dsk_anim_decode(d, n, still, &f) : NULL;
	free(d);
	CHECK(a && a->n == 2 && a->loops == 0 && f.calls == 2);
	if (a) {
		dsk_anim_frame *f1 = &a->frames[1];
		CHECK(f1->x == 1 && f1->y == 1 && f1->w == 2 && f1->h == 2 && f1->delay_ms == 30 && f1->blend && f1->disposal == DSK_DISPOSE_PREVIOUS);
		dsk_anim_player p;
		dsk_anim_player_init(&p, a);
		dsk_anim_next(&p, NULL);
		CHECK(at(&p, 1, 1)[3] == 200);
		dsk_anim_next(&p, NULL);
		CHECK(at(&p, 1, 1)[3] > 200 && at(&p, 0, 0)[3] == 200);  // over itself: more opaque
		dsk_anim_next(&p, NULL);  // disposed to the previous, then the loop starts again
		CHECK(p.last == 0 && at(&p, 1, 1)[3] == 200);
		dsk_anim_player_free(&p);
		dsk_anim_free(a);
	}

	d = read_test_file(dir, "anim.webp", &n);
	CHECK(d && dsk_anim_is_animated(d, n));
	fake_still g = {0, {4, 2}, {4, 2}};
	a = d ? dsk_anim_decode(d, n, still, &g) : NULL;
	free(d);
	CHECK(a && a->w == 4 && a->h == 4 && a->n == 2 && a->loops == 2);
	if (a) {
		CHECK(a->frames[1].x == 2 && !a->frames[1].blend && a->frames[1].disposal == DSK_DISPOSE_BACKGROUND && a->frames[1].delay_ms == 40);
		dsk_anim_player p;
		dsk_anim_player_init(&p, a);
		dsk_anim_next(&p, NULL);
		dsk_anim_next(&p, NULL);
		CHECK(at(&p, 3, 3)[0] == 20 && at(&p, 0, 0)[0] == 10);
		dsk_anim_player_free(&p);
		dsk_anim_free(a);
	}
	CHECK(!dsk_anim_is_animated((const uint8_t *)"GIF89a", 6));
	test_apng_offsets();
}

// Names Windows reads as written, and those it reads as something else:
// a parent (".. "), a drive or a stream (a colon), a device, a name it
// trims.
static void test_windows_names(void) {
	static const char *const ok[] = {"a", "a.txt", ".asuka-files", "...x", "x..y", "CONSOLE", "con2", "COM10", "LPT", "nul_", "a b", " lead", "\xc3\xa9t\xc3\xa9", "COM\xc2\xb4"};
	static const char *const bad[] = {"", ".", "..", ".. ", "...", "x.", "x ", "x. ", "C:", "C:x", "a:stream", "a::$DATA", "a\\b", "a/b", "a<b",
	                                  "a>b", "a\"b", "a|b", "a?b", "a*b", "a\tb", "CON", "con", "Nul.txt", "aux.tar.gz", "PRN ", "COM1", "lpt9.log",
	                                  "COM0", "COM1 .txt", "CONIN$", "conout$.x", "COM\xc2\xb9", "LPT\xc2\xb3.txt"};
	for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
		if (!dsk_windows_name(ok[i], strlen(ok[i]))) fprintf(stderr, "dsk_windows_name(\"%s\") false\n", ok[i]), failed++;
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
		if (dsk_windows_name(bad[i], strlen(bad[i]))) fprintf(stderr, "dsk_windows_name(\"%s\") true\n", bad[i]), failed++;
	CHECK(!dsk_windows_name("a\0b", 3));
}

// The app's settings (asuka-app.json): its own, and its packages'
// (settings: asuka_desktop_app_setting's), strings only.
static void test_config(const char *dir) {
	char *p = dsk_join(dir, "asuka-app.json");
	const char *j = "{\"name\":\"Notes\",\"settings\":{\"analytics.measurementId\":\"G-ABC\",\"analytics.debug\":\"true\",\"n\":3}}";
	dsk_write_file(p, j, strlen(j));
	setenv("ASUKA_APP_CONFIG", p, 1);
	dsk_config c;
	dsk_config_load(&c);
	CHECK(c.name && !strcmp(c.name, "Notes"));
	CHECK(!strcmp(dsk_config_setting(&c, "analytics.measurementId"), "G-ABC"));
	CHECK(!strcmp(dsk_config_setting(&c, "analytics.debug"), "true"));
	CHECK(!strcmp(dsk_config_setting(&c, "n"), "") && !strcmp(dsk_config_setting(&c, "none"), ""));
	dsk_config_free(&c);
	CHECK(c.nsettings == 0 && !strcmp(dsk_config_setting(&c, "analytics.debug"), ""));
	unsetenv("ASUKA_APP_CONFIG");
	free(p);
}

int main(int argc, char **argv) {
	test_windows_names();
	test_words();
	test_flyout();
	test_json();
	if (argc > 1) {
		test_state(argv[1]);
		test_exists(argv[1]);
		test_anim(argv[1]);
		test_config(argv[1]);
	}
	fprintf(stderr, "%d failures\n", failed);
	return failed ? 1 : 0;
}
