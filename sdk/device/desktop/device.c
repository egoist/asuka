// platform/device on Windows and Linux (macOS runs the package's Swift): the
// system's open dialog for the pickers (their files copied to the app's
// caches, as a phone's pickers give them), the system's browser, the
// clipboard. What both share: copying what's picked, and reading a
// photo's orientation and an MPEG-4 video's size and length from their
// files.
#ifdef _WIN32
#include "asuka_windows.h"
#define SEP "\\"
#else
#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L  // fseeko, ftello
#endif
#include <errno.h>
#include <sys/stat.h>
#define SEP "/"
#endif
#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool device_web_url(const char *u) { return strncmp(u, "https://", 8) == 0 || strncmp(u, "http://", 7) == 0; }

const char *device_mime(const char *name) {
	static const char *table[][2] = {
	    {".png", "image/png"},   {".jpg", "image/jpeg"},  {".jpeg", "image/jpeg"}, {".gif", "image/gif"},       {".webp", "image/webp"},
	    {".heic", "image/heic"}, {".heif", "image/heif"}, {".bmp", "image/bmp"},   {".tif", "image/tiff"},      {".tiff", "image/tiff"},
	    {".svg", "image/svg+xml"}, {".ico", "image/vnd.microsoft.icon"}, {".pdf", "application/pdf"}, {".txt", "text/plain; charset=utf-8"},
	    {".csv", "text/csv; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"}, {".html", "text/html; charset=utf-8"},
	    {".htm", "text/html; charset=utf-8"}, {".json", "application/json"}, {".xml", "text/xml; charset=utf-8"}, {".zip", "application/zip"},
	    {".mp3", "audio/mpeg"},  {".m4a", "audio/mp4"},   {".aac", "audio/aac"},   {".wav", "audio/wav"},       {".flac", "audio/flac"},
	    {".ogg", "audio/ogg"},   {".opus", "audio/ogg"},  {".mp4", "video/mp4"},   {".m4v", "video/mp4"},       {".mov", "video/quicktime"},
	    {".webm", "video/webm"}, {".avi", "video/x-msvideo"}, {".mkv", "video/x-matroska"},
	};
	const char *dot = strrchr(name, '.');
	if (!dot) return "";
	for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
		const char *a = dot, *b = table[i][0];
		while (*a && *b && (*a | 32) == (*b | 32)) a++, b++;
		if (!*a && !*b) return table[i][1];
	}
	return "";
}

static void random_name(char out[25]) {
	static unsigned seq;
	unsigned long long r = 0;
	for (int i = 0; i < 4; i++) r = r << 16 ^ (unsigned long long)rand();
	snprintf(out, 25, "%016llx%08x", r, ++seq);
}

static bool make_dir(const char *p) {
#ifdef _WIN32
	wchar_t *w = asuka_wide(p);
	BOOL ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
	free(w);
	return ok;
#else
	return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}

static FILE *open_file(const char *p, const char *mode) {
#ifdef _WIN32
	wchar_t *w = asuka_wide(p), *m = asuka_wide(mode);
	FILE *f = _wfopen(w, m);
	free(w);
	free(m);
	return f;
#else
	return fopen(p, mode);
#endif
}

static const char *base_name(const char *p) {
	const char *b = p;
	for (const char *q = p; *q; q++)
		if (*q == '/' || *q == '\\') b = q + 1;
	return b;
}

bool device_copy_picked(char **paths, size_t n, size_t limit, device_list_File *out, asuka_error *err) {
	if (limit > 0 && n > limit) n = limit;
	*out = (device_list_File){calloc(n ? n : 1, sizeof *out->data), 0};
	char *root = asuka_sprintf("%s" SEP "asuka-files", asuka_desktop_cache_dir());
	make_dir(root);
	for (size_t i = 0; i < n; i++) {
		char name[25];
		random_name(name);
		char *dir = asuka_sprintf("%s" SEP "%s", root, name);
		const char *base = base_name(paths[i]);
		char *to = asuka_sprintf("%s" SEP "%s", dir, base);
		bool ok = make_dir(dir);
		FILE *in = ok ? open_file(paths[i], "rb") : NULL;
		FILE *o = in ? open_file(to, "wb") : NULL;
		int64_t size = 0;
		ok = in && o;
		char buf[65536];
		size_t got;
		while (ok && (got = fread(buf, 1, sizeof buf, in)) > 0) {
			ok = fwrite(buf, 1, got, o) == got;
			size += (int64_t)got;
		}
		if (in) fclose(in);
		if (o && fclose(o) != 0) ok = false;
		free(dir);
		if (!ok) {
			asuka_error_failed(err, "device", 0, "couldn't copy %s", paths[i]);
			free(to);
			free(root);
			device_list_File_free(out);
			return false;
		}
		out->data[out->len++] = (device_File){to, asuka_strdup(base), asuka_strdup(device_mime(base)), size};
	}
	free(root);
	return true;
}

char *device_write_cache(const char *dir, const char *ext, const void *data, size_t len, asuka_error *err) {
	char *d = asuka_sprintf("%s" SEP "%s", asuka_desktop_cache_dir(), dir);
	make_dir(d);
	char name[25];
	random_name(name);
	char *p = asuka_sprintf("%s" SEP "%s.%s", d, name, ext);
	free(d);
	FILE *f = open_file(p, "wb");
	bool ok = f && fwrite(data, 1, len, f) == len;
	if (f && fclose(f) != 0) ok = false;
	if (!ok) {
		asuka_error_failed(err, "device", 0, "couldn't write %s", p);
		free(p);
		return NULL;
	}
	return p;
}

// copy_file copies from to `to`: its bytes, or -1 if it can't.
static int64_t copy_file(const char *from, const char *to) {
	FILE *in = open_file(from, "rb");
	FILE *o = in ? open_file(to, "wb") : NULL;
	bool ok = in && o;
	int64_t size = 0;
	char buf[65536];
	size_t got;
	while (ok && (got = fread(buf, 1, sizeof buf, in)) > 0) {
		ok = fwrite(buf, 1, got, o) == got;
		size += (int64_t)got;
	}
	if (in && ferror(in)) ok = false;
	if (in) fclose(in);
	if (o && fclose(o) != 0) ok = false;
	return ok ? size : -1;
}

static void remove_file(const char *p) {
#ifdef _WIN32
	wchar_t *w = asuka_wide(p);
	DeleteFileW(w);
	free(w);
#else
	remove(p);
#endif
}

static bool seek_to(FILE *f, int64_t at) {
#ifdef _WIN32
	return _fseeki64(f, at, SEEK_SET) == 0;
#else
	return fseeko(f, (off_t)at, SEEK_SET) == 0;
#endif
}

static int64_t file_size(FILE *f) {
#ifdef _WIN32
	if (_fseeki64(f, 0, SEEK_END)) return -1;
	return _ftelli64(f);
#else
	if (fseeko(f, 0, SEEK_END)) return -1;
	return (int64_t)ftello(f);
#endif
}

static int64_t size_of(const char *path) {
	FILE *f = open_file(path, "rb");
	if (!f) return 0;
	int64_t n = file_size(f);
	fclose(f);
	return n > 0 ? n : 0;
}

void device_fit(int64_t w, int64_t h, int64_t max, int64_t *ow, int64_t *oh) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (max > 0 && (w > max || h > max)) {
		// The long side max; the short one rounded, never 0.
		if (w >= h) h = (h * max + w / 2) / w, w = max;
		else w = (w * max + h / 2) / h, h = max;
	}
	*ow = w > 0 ? w : 1;
	*oh = h > 0 ? h : 1;
}

// ---- EXIF's orientation ----

static uint32_t rd16(const uint8_t *p, bool le) { return le ? (uint32_t)(p[0] | p[1] << 8) : (uint32_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p, bool le) {
	return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
	          : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

// tiff_orientation is IFD0's Orientation (0x0112) in a TIFF block.
static int tiff_orientation(const uint8_t *t, size_t n) {
	if (n < 8) return 1;
	bool le = t[0] == 'I' && t[1] == 'I';
	if (!le && !(t[0] == 'M' && t[1] == 'M')) return 1;
	if (rd16(t + 2, le) != 42) return 1;
	uint32_t ifd = rd32(t + 4, le);
	if (ifd > n || n - ifd < 2) return 1;
	uint32_t count = rd16(t + ifd, le);
	for (uint32_t i = 0; i < count; i++) {
		size_t e = (size_t)ifd + 2 + (size_t)i * 12;
		if (e + 12 > n) break;
		if (rd16(t + e, le) != 0x0112) continue;
		uint32_t v = rd16(t + e + 8, le);  // a SHORT, first in its value
		return v >= 1 && v <= 8 ? (int)v : 1;
	}
	return 1;
}

int device_jpeg_orientation(const uint8_t *d, size_t n) {
	if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
	size_t i = 2;
	while (i + 4 <= n) {
		if (d[i] != 0xFF) return 1;
		uint8_t m = d[i + 1];
		if (m == 0xFF) {  // fill
			i++;
			continue;
		}
		if (m == 0xDA || m == 0xD9) return 1;  // the image's data: no EXIF before it
		size_t len = (size_t)d[i + 2] << 8 | d[i + 3];
		if (len < 2) return 1;
		if (m == 0xE1 && len >= 8 && i + 10 <= n && !memcmp(d + i + 4, "Exif\0\0", 6)) {
			size_t start = i + 10, end = i + 2 + len;
			if (end > n) end = n;
			return start < end ? tiff_orientation(d + start, end - start) : 1;
		}
		i += 2 + len;
	}
	return 1;
}

int device_file_orientation(const char *path) {
	FILE *f = open_file(path, "rb");
	if (!f) return 1;
	size_t cap = 256 * 1024;
	uint8_t *b = malloc(cap);
	size_t n = fread(b, 1, cap, f);
	fclose(f);
	int o = device_jpeg_orientation(b, n);
	free(b);
	return o;
}

// ---- MPEG-4's (ISO BMFF's) boxes ----

typedef struct {
	FILE *f;
	char type[5];
	int64_t body, end;  // where its content starts, and where it ends
} box;

// box_at reads the header of the box at `at` (within `end`).
static bool box_at(FILE *f, int64_t at, int64_t end, box *b) {
	uint8_t h[16];
	if (at + 8 > end || !seek_to(f, at) || fread(h, 1, 8, f) != 8) return false;
	uint64_t size = rd32(h, false);
	int64_t hdr = 8;
	if (size == 1) {
		if (fread(h + 8, 1, 8, f) != 8) return false;
		size = (uint64_t)rd32(h + 8, false) << 32 | rd32(h + 12, false);
		hdr = 16;
	} else if (size == 0) {
		size = (uint64_t)(end - at);  // to the end
	}
	if (size < (uint64_t)hdr || size > (uint64_t)(end - at)) return false;
	b->f = f;
	memcpy(b->type, h + 4, 4);
	b->type[4] = 0;
	b->body = at + hdr;
	b->end = at + (int64_t)size;
	return true;
}

// box_find is parent's first child of a type.
static bool box_find(const box *parent, const char *type, box *out) {
	for (int64_t at = parent->body; box_at(parent->f, at, parent->end, out); at = out->end)
		if (!memcmp(out->type, type, 4)) return true;
	return false;
}

// box_read reads up to n bytes of a box's content: how many.
static size_t box_read(const box *b, uint8_t *buf, size_t n) {
	int64_t have = b->end - b->body;
	if ((int64_t)n > have) n = (size_t)(have > 0 ? have : 0);
	if (!n || !seek_to(b->f, b->body)) return 0;
	return fread(buf, 1, n, b->f);
}

static int32_t s32(const uint8_t *p) { return (int32_t)rd32(p, false); }

// track_video reads a trak's video size, as its matrix turns it: false if
// it isn't video.
static bool track_video(const box *trak, int64_t *w, int64_t *h) {
	box mdia, hdlr, tkhd;
	uint8_t b[96] = {0};
	if (!box_find(trak, "mdia", &mdia) || !box_find(&mdia, "hdlr", &hdlr)) return false;
	if (box_read(&hdlr, b, 12) < 12 || memcmp(b + 8, "vide", 4)) return false;
	int64_t tw = 0, th = 0;
	bool turned = false;
	if (box_find(trak, "tkhd", &tkhd)) {
		size_t n = box_read(&tkhd, b, sizeof b);
		size_t m = b[0] == 1 ? 4 + 32 + 16 : 4 + 20 + 16;  // where its matrix is
		if (n >= m + 44) {
			int32_t a = s32(b + m), bb = s32(b + m + 4), c = s32(b + m + 12), d = s32(b + m + 16);
			turned = a == 0 && d == 0 && bb != 0 && c != 0;  // 90° or 270°
			tw = (int64_t)(rd32(b + m + 36, false) >> 16);
			th = (int64_t)(rd32(b + m + 40, false) >> 16);
		}
	}
	if (tw <= 0 || th <= 0) {  // the sample description's
		box minf, stbl, stsd;
		if (box_find(&mdia, "minf", &minf) && box_find(&minf, "stbl", &stbl) && box_find(&stbl, "stsd", &stsd) && box_read(&stsd, b, 44) >= 44) {
			tw = rd16(b + 8 + 32, false);
			th = rd16(b + 8 + 34, false);
		}
	}
	*w = turned ? th : tw;
	*h = turned ? tw : th;
	return true;
}

bool device_mp4_info(const char *path, int64_t *w, int64_t *h, int64_t *ms) {
	*w = *h = *ms = 0;
	FILE *f = open_file(path, "rb");
	if (!f) return false;
	box file = {f, "", 0, file_size(f)}, moov, mvhd, trak;
	bool video = false;
	if (file.end > 8 && box_find(&file, "moov", &moov)) {
		uint8_t b[32] = {0};
		if (box_find(&moov, "mvhd", &mvhd)) {
			size_t n = box_read(&mvhd, b, sizeof b);
			uint64_t scale = 0, dur = 0;
			if (b[0] == 1 && n >= 32) scale = rd32(b + 20, false), dur = (uint64_t)rd32(b + 24, false) << 32 | rd32(b + 28, false);
			else if (b[0] == 0 && n >= 20) scale = rd32(b + 12, false), dur = rd32(b + 16, false);
			if (scale && dur != UINT64_MAX && dur != UINT32_MAX) *ms = (int64_t)(dur / scale * 1000 + dur % scale * 1000 / scale);
		}
		for (int64_t at = moov.body; !video && box_at(f, at, moov.end, &trak); at = trak.end)
			if (!memcmp(trak.type, "trak", 4)) video = track_video(&trak, w, h);
	}
	fclose(f);
	if (!video) *ms = 0;
	return video;
}

// ---- Picked media ----

bool device_copy_media(char **paths, size_t n, size_t limit, int64_t max_size, double quality, const device_media_ops *ops, device_list_NativeMedia *out,
                       asuka_error *err) {
	if (limit > 0 && n > limit) n = limit;
	*out = (device_list_NativeMedia){calloc(n ? n : 1, sizeof *out->data), 0};
	char *root = asuka_sprintf("%s" SEP "asuka-photos", asuka_desktop_cache_dir());
	make_dir(root);
	bool convert = max_size > 0 || quality > 0;
	double q = quality > 0 && quality <= 1 ? quality : 0.9;
	for (size_t i = 0; i < n; i++) {
		char name[25];
		random_name(name);
		char *dir = asuka_sprintf("%s" SEP "%s", root, name);
		const char *base = base_name(paths[i]);
		const char *mime = device_mime(base);
		device_NativeMedia m = {0};
		m.video = strncmp(mime, "video/", 6) == 0;
		bool ok = make_dir(dir);
		if (ok && !m.video && convert && ops && ops->jpeg) {
			const char *dot = strrchr(base, '.');
			int stem = (int)(dot && dot != base ? (size_t)(dot - base) : strlen(base));
			char *to = asuka_sprintf("%s" SEP "%.*s.jpg", dir, stem, base);
			if (ops->jpeg(paths[i], to, max_size, q, &m.width, &m.height)) {
				m.path = to;
				m.type = asuka_strdup("image/jpeg");
				m.size = size_of(to);
			} else {  // not an image it reads: copied as it is
				remove_file(to);
				free(to);
				m.width = m.height = 0;
			}
		}
		if (ok && !m.path) {
			char *to = asuka_sprintf("%s" SEP "%s", dir, base);
			m.size = copy_file(paths[i], to);
			if (m.size < 0) {
				remove_file(to);
				free(to);
				ok = false;
			} else {
				m.path = to;
				m.type = asuka_strdup(mime);
				if (m.video) {
					if (!device_mp4_info(to, &m.width, &m.height, &m.durationMs) && ops && ops->video) ops->video(to, &m.width, &m.height, &m.durationMs);
				} else if (ops && ops->image_size && !ops->image_size(to, &m.width, &m.height)) {
					m.width = m.height = 0;
				}
			}
		}
		free(dir);
		if (!ok) {
			asuka_error_failed(err, "device", 0, "couldn't copy %s", paths[i]);
			free(root);
			device_list_NativeMedia_free(out);
			return false;
		}
		m.name = asuka_strdup(base);
		out->data[out->len++] = m;
	}
	free(root);
	return true;
}
