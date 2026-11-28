// platform/camera on Windows (Media Foundation's capture engine,
// camera_windows.c) and Linux (GStreamer, camera_linux.c); macOS runs
// the package's Swift. What both share. Main thread, unless it says.
#ifdef _WIN32
#define _CRT_RAND_S
#endif
#include "camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include "asuka_windows.h"
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

struct camera_pending {
	asuka_call *call;
	void (*done)(asuka_call *call, char *value);
	camera_pending **slot;
	void (*cancel)(void *ctx);
	void *ctx;
};

static void cancelled(void *p) {
	camera_pending *w = p;
	if (w->slot) *w->slot = NULL;
	if (w->cancel) w->cancel(w->ctx);
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(w->call, &e);
	free(w);
}

void camera_wait(asuka_call *call, void (*done)(asuka_call *call, char *value), camera_pending **slot, void (*cancel)(void *ctx), void *ctx) {
	camera_pending *w = malloc(sizeof *w);
	*w = (camera_pending){call, done, slot, cancel, ctx};
	*slot = w;
	asuka_call_on_cancel(call, cancelled, w);
}

void camera_move(camera_pending **from, camera_pending **to) {
	camera_pending *w = *from;
	*from = NULL;
	*to = w;
	if (w) w->slot = to;
}

static camera_pending *take(camera_pending **slot) {
	camera_pending *w = *slot;
	*slot = NULL;
	return w;
}

void camera_answer(camera_pending **slot, const char *value) {
	camera_pending *w = take(slot);
	if (!w) return;
	w->done(w->call, asuka_strdup(value));
	free(w);
}

void camera_fail(camera_pending **slot, const char *message) {
	camera_pending *w = take(slot);
	if (!w) return;
	asuka_error e = {0};
	asuka_error_failed(&e, "camera", 0, "%s", message);
	asuka_call_fail(w->call, &e);
	free(w);
}

void camera_fail_declared(camera_pending **slot, uint32_t index) {
	camera_pending *w = take(slot);
	if (!w) return;
	asuka_error e = {0};
	asuka_error_declared(&e, index);
	asuka_call_fail(w->call, &e);
	free(w);
}

void camera_kinds_set(camera_kinds *k, camera_list_string list) {
	camera_kinds_free(k);
	k->kinds = calloc(list.len + 1, sizeof *k->kinds);
	for (size_t i = 0; i < list.len; i++)
		if (list.data[i] && *list.data[i]) k->kinds[k->n++] = asuka_strdup(list.data[i]);
}

bool camera_kinds_wants(const camera_kinds *k, const char *kind) {
	if (!k->n) return true;
	for (size_t i = 0; i < k->n; i++)
		if (!strcmp(k->kinds[i], kind)) return true;
	return false;
}

void camera_kinds_free(camera_kinds *k) {
	for (size_t i = 0; i < k->n; i++) free(k->kinds[i]);
	free(k->kinds);
	*k = (camera_kinds){0};
}

bool camera_seen_new(camera_seen *s, const char *kind, const char *value, int64_t now) {
	char *key = asuka_sprintf("%s%c%s", kind, 1, value);
	bool fresh = true;
	size_t i;
	for (i = 0; i < s->n; i++)
		if (!strcmp(s->seen[i].key, key)) break;
	if (i < s->n) {
		fresh = now - s->seen[i].at > 1000;
		s->seen[i].at = now;
		free(key);
	} else {
		if (s->n == s->cap) {
			s->cap = s->cap ? s->cap * 2 : 8;
			s->seen = realloc(s->seen, s->cap * sizeof *s->seen);
		}
		s->seen[s->n].key = key;
		s->seen[s->n++].at = now;
	}
	if (s->n > 32) {  // forget what's been out of view a while
		size_t j = 0;
		for (size_t k = 0; k < s->n; k++) {
			if (now - s->seen[k].at > 1000) free(s->seen[k].key);
			else s->seen[j++] = s->seen[k];
		}
		s->n = j;
	}
	return fresh;
}

void camera_seen_free(camera_seen *s) {
	for (size_t i = 0; i < s->n; i++) free(s->seen[i].key);
	free(s->seen);
	*s = (camera_seen){0};
}

int64_t camera_now_ms(void) {
#ifdef _WIN32
	return (int64_t)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// random_name is 24 hex digits, from the system's random source.
static void random_name(char out[25]) {
	unsigned char b[12] = {0};
#ifdef _WIN32
	for (int i = 0; i < 12; i += 4) {
		unsigned int v = 0;
		rand_s(&v);
		memcpy(b + i, &v, 4);
	}
#else
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		uint64_t v = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)getpid() << 40;
		memcpy(b, &v, 8);
	}
	if (fd >= 0) close(fd);
#endif
	for (int i = 0; i < 12; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

char *camera_file(const char *ext) {
	char *dir = asuka_sprintf("%s/asuka-camera", asuka_desktop_cache_dir());
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0755);
#endif
	char name[25];
	random_name(name);
	char *path = asuka_sprintf("%s/%s%s", dir, name, ext);
	free(dir);
	FILE *f = fopen(path, "wb");  // there, and writable
	if (!f) {
		free(path);
		return NULL;
	}
	fclose(f);
	return path;
}

// ---- Zoom, frames, photos ----

double camera_clamp(double v, double lo, double hi) {
	if (!(v >= lo)) return lo;  // NaN too
	return v > hi ? hi : v;
}

camera_rect camera_zoom_crop(int w, int h, double zoom) {
	camera_rect r = {0, 0, w, h};
	if (!(zoom > 1) || w <= 0 || h <= 0) return r;
	r.w = (int)(w / zoom + 0.5);
	r.h = (int)(h / zoom + 0.5);
	if (r.w < 1) r.w = 1;
	if (r.h < 1) r.h = 1;
	r.x = (w - r.w) / 2;
	r.y = (h - r.h) / 2;
	return r;
}

void camera_fit(int w, int h, int max, int *ow, int *oh) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (max > 0 && (w > max || h > max)) {
		if (w >= h) h = (int)(((int64_t)h * max + w / 2) / w), w = max;
		else w = (int)(((int64_t)w * max + h / 2) / h), h = max;
	}
	*ow = w > 0 ? w : 1;
	*oh = h > 0 ? h : 1;
}

void camera_resample(const uint8_t *src, size_t stride, camera_rect r, bool mirror, uint8_t *out, int dw, int dh) {
	if (r.w <= 0 || r.h <= 0 || dw <= 0 || dh <= 0) return;
	for (int y = 0; y < dh; y++) {
		// The source rows this one covers: [y0, y1).
		int y0 = (int)((int64_t)y * r.h / dh), y1 = (int)((int64_t)(y + 1) * r.h / dh);
		if (y1 <= y0) y1 = y0 + 1;
		uint8_t *o = out + (size_t)y * dw * 3;
		for (int x = 0; x < dw; x++) {
			int ox = mirror ? dw - 1 - x : x;
			int x0 = (int)((int64_t)ox * r.w / dw), x1 = (int)((int64_t)(ox + 1) * r.w / dw);
			if (x1 <= x0) x1 = x0 + 1;
			uint32_t s0 = 0, s1 = 0, s2 = 0, n = 0;
			for (int sy = y0; sy < y1; sy++) {
				const uint8_t *row = src + (size_t)(r.y + sy) * stride + (size_t)(r.x + x0) * 4;
				for (int sx = x0; sx < x1; sx++, row += 4) s0 += row[0], s1 += row[1], s2 += row[2], n++;
			}
			o[x * 3] = (uint8_t)((s0 + n / 2) / n);
			o[x * 3 + 1] = (uint8_t)((s1 + n / 2) / n);
			o[x * 3 + 2] = (uint8_t)((s2 + n / 2) / n);
		}
	}
}

bool camera_zoom_optical(camera_zoom_range r, double *max_zoom) {
	if (r.min <= 0 || r.max <= r.min) return false;
	*max_zoom = (double)r.max / (double)r.min;
	return true;
}

long camera_zoom_value(camera_zoom_range r, double zoom) {
	double v = camera_clamp(zoom, 1, (double)r.max / (double)(r.min > 0 ? r.min : 1)) * (double)r.min;
	long l = (long)(v + 0.5);
	return l < r.min ? r.min : l > r.max ? r.max : l;
}

static char *frame_path(unsigned view, int n) {
	char *dir = asuka_sprintf("%s/asuka-camera", asuka_desktop_cache_dir());
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0755);
#endif
	char *p = asuka_sprintf("%s/frame-%u-%d.jpg", dir, view, n);
	free(dir);
	return p;
}

bool camera_frames_due(camera_frames *f, int64_t now, char **path, int64_t *ms) {
	if (!(f->rate > 0) || f->writing) return false;
	double rate = f->rate > CAMERA_MAX_FRAME_RATE ? CAMERA_MAX_FRAME_RATE : f->rate;
	int64_t every = (int64_t)(1000 / rate + 0.5);
	if (f->last && now - f->last < every) return false;
	// On the interval's beat, unless it fell behind a whole one.
	f->last = f->last && now - f->last < 2 * every ? f->last + every : now;
	f->writing = true;
	*path = frame_path(f->view, f->next);
	f->next = (f->next + 1) % CAMERA_FRAME_RING;
	*ms = now - f->started;
	return true;
}

void camera_frames_clean(unsigned view) {
	for (int i = 0; i < CAMERA_FRAME_RING; i++) {
		char *p = frame_path(view, i);
#ifdef _WIN32
		wchar_t *w = asuka_wide(p);
		DeleteFileW(w);
		free(w);
#else
		unlink(p);
#endif
		free(p);
	}
}

bool camera_record_full(int64_t size, int64_t max, double bitrate, int interval) {
	if (max <= 0) return false;
	// Two reads' worth of what it writes, and its index (a second's
	// samples' sizes and times: a few KB), kept clear of max.
	double ahead = bitrate / 8 * (2.0 * interval / 1000) + 16384;
	return (double)size + ahead >= (double)max;
}

void camera_quality_sizes(camera_VideoQuality q, camera_size sizes[4]) {
	static const camera_size all[4] = {{640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160}};
	int at = q >= 0 && q <= 3 ? (int)q : 2;
	// Itself, then the rest nearest first, the smaller first of two as
	// near (a camera's 720p before its 4K for 1080p).
	int n = 0;
	sizes[n++] = all[at];
	for (int d = 1; d < 4; d++) {
		if (at - d >= 0) sizes[n++] = all[at - d];
		if (at + d < 4) sizes[n++] = all[at + d];
	}
}

double camera_size_distance(camera_size want, int w, int h) {
	// By the pixels' count, on a log scale (twice as many as far as half),
	// then a little for a shape that isn't the one asked for.
	double a = (double)want.w * want.h, b = (double)w * h;
	if (b <= 0) return 1e9;
	double r = a > b ? a / b : b / a;
	double sa = (double)want.w / want.h, sb = (double)w / h;
	double shape = sa > sb ? sa / sb : sb / sa;
	return (r - 1) + (shape - 1) * 0.5;
}

// lang_is compares a language tag with one of the table's, ignoring case
// and '_' for '-'.
static bool lang_is(const char *a, size_t n, const char *b) {
	size_t i = 0;
	for (; i < n && b[i]; i++) {
		char x = a[i] == '_' ? '-' : a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return false;
	}
	return i == n && !b[i];
}

static const camera_word *word(const char *lang, size_t n) {
	for (const camera_word *w = camera_words; w->lang; w++)
		if (lang_is(lang, n, w->lang)) return w;
	return NULL;
}

static bool has_part(const char *tag, size_t n, const char *part) {
	size_t k = strlen(part);
	for (size_t i = 0; i + k <= n; i++)
		if ((i == 0 || tag[i - 1] == '-' || tag[i - 1] == '_') && (i + k == n || tag[i + k] == '-' || tag[i + k] == '_' || tag[i + k] == '.' || tag[i + k] == '@') &&
		    lang_is(tag + i, k, part))
			return true;
	return false;
}

const camera_word *camera_words_for(const char *const *locales, size_t count) {
	for (size_t i = 0; i < count; i++) {
		const char *l = locales[i];
		if (!l || !*l) continue;
		size_t n = strcspn(l, ".@");  // "de_DE.UTF-8", "sr@latin"
		const camera_word *w = word(l, n);
		if (w) return w;
		size_t base = strcspn(l, "-_.@");
		if (base > n) base = n;
		if (lang_is(l, base, "zh")) {
			bool hant = has_part(l, n, "Hant") || has_part(l, n, "TW") || has_part(l, n, "HK") || has_part(l, n, "MO");
			return word(hant ? "zh-Hant" : "zh-Hans", hant ? 7 : 7);
		}
		if (lang_is(l, base, "no") || lang_is(l, base, "nn")) w = word("nb", 2);
		else if (lang_is(l, base, "tl")) w = word("fil", 3);
		else w = word(l, base);
		if (w) return w;
	}
	const camera_word *en = word("en", 2);
	static const camera_word fallback = {"en", "Take Photo", "Cancel"};
	return en ? en : &fallback;
}
