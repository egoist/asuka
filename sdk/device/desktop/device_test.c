// device_test.c: what device's Windows and Linux C share for pickMedia
// (device.c), on every system (platform/desktop's TestStdUnits): a JPEG's
// EXIF orientation, an MPEG-4's and a QuickTime movie's video size (turned
// as its matrix turns it) and length, the fit a photo's scaled to, and
// device_copy_media copying a picked video and photo (and writing a JPEG
// through the platform's op when asked), in the order picked, at most max.
#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

// The glue's (device.asuka.c, which a portable test isn't built with).
void device_NativeMedia_free(device_NativeMedia *v) {
	free(v->path);
	free(v->name);
	free(v->type);
}

void device_list_NativeMedia_free(device_list_NativeMedia *v) {
	for (size_t i = 0; i < v->len; i++) device_NativeMedia_free(&v->data[i]);
	free(v->data);
	v->data = NULL;
	v->len = 0;
}

void device_File_free(device_File *v) {
	free(v->path);
	free(v->name);
	free(v->type);
}

void device_list_File_free(device_list_File *v) {
	for (size_t i = 0; i < v->len; i++) device_File_free(&v->data[i]);
	free(v->data);
	v->data = NULL;
	v->len = 0;
}

// ---- Bytes, written big or little endian ----

typedef struct {
	uint8_t b[4096];
	size_t n;
} buf;

static void u8(buf *o, unsigned v) { o->b[o->n++] = (uint8_t)v; }
static void u16(buf *o, unsigned v, bool le) {
	if (le) u8(o, v & 255), u8(o, v >> 8);
	else u8(o, v >> 8), u8(o, v & 255);
}
static void u32(buf *o, uint32_t v, bool le) {
	if (le) u16(o, v & 0xFFFF, true), u16(o, v >> 16, true);
	else u16(o, v >> 16, false), u16(o, v & 0xFFFF, false);
}
static void put(buf *o, const void *p, size_t n) {
	memcpy(o->b + o->n, p, n);
	o->n += n;
}

// jpeg is a JPEG's first bytes with an APP0, then an APP1 of EXIF whose
// IFD0 holds orientation (0: no APP1).
static void jpeg(buf *o, int orientation, bool le) {
	o->n = 0;
	u8(o, 0xFF), u8(o, 0xD8);
	u8(o, 0xFF), u8(o, 0xE0), u16(o, 16, false), put(o, "JFIF\0\1\1\0\0\1\0\1\0\0", 14);
	if (orientation) {
		buf t = {{0}, 0};
		put(&t, le ? "II" : "MM", 2), u16(&t, 42, le), u32(&t, 8, le);
		u16(&t, 2, le);                                                  // two entries
		u16(&t, 0x010F, le), u16(&t, 2, le), u32(&t, 4, le), put(&t, "Cam\0", 4);  // Make
		u16(&t, 0x0112, le), u16(&t, 3, le), u32(&t, 1, le), u16(&t, (unsigned)orientation, le), u16(&t, 0, le);
		u32(&t, 0, le);
		u8(o, 0xFF), u8(o, 0xE1), u16(o, (unsigned)(2 + 6 + t.n), false), put(o, "Exif\0\0", 6), put(o, t.b, t.n);
	}
	u8(o, 0xFF), u8(o, 0xDA), u16(o, 2, false);
}

static void orientations(void) {
	buf b;
	for (int o = 1; o <= 8; o++) {
		jpeg(&b, o, o % 2 == 0);
		CHECK(device_jpeg_orientation(b.b, b.n) == o);
	}
	jpeg(&b, 0, false);
	CHECK(device_jpeg_orientation(b.b, b.n) == 1);
	jpeg(&b, 6, false);
	CHECK(device_jpeg_orientation(b.b, 20) == 1);  // cut short: none
	CHECK(device_jpeg_orientation((const uint8_t *)"\x89PNG\r\n\x1a\n", 8) == 1);
	CHECK(device_jpeg_orientation(NULL, 0) == 1);
}

static void fits(void) {
	int64_t w, h;
	device_fit(4032, 3024, 1024, &w, &h);
	CHECK(w == 1024 && h == 768);
	device_fit(3024, 4032, 1024, &w, &h);
	CHECK(w == 768 && h == 1024);
	device_fit(800, 600, 1024, &w, &h);
	CHECK(w == 800 && h == 600);
	device_fit(800, 600, 0, &w, &h);
	CHECK(w == 800 && h == 600);
}

// ---- A movie's boxes ----

static size_t box_open(buf *o, const char *type) {
	size_t at = o->n;
	u32(o, 0, false);
	put(o, type, 4);
	return at;
}

static void box_close(buf *o, size_t at) {
	uint32_t n = (uint32_t)(o->n - at);
	o->b[at] = (uint8_t)(n >> 24), o->b[at + 1] = (uint8_t)(n >> 16), o->b[at + 2] = (uint8_t)(n >> 8), o->b[at + 3] = (uint8_t)n;
}

// track writes a trak of handler (its tkhd of w×h and a turn of 0, 90,
// 180 or 270 degrees; version 1 if v1), with an stsd's size too.
static void track(buf *o, const char *handler, int w, int h, int turn, bool v1, bool sized) {
	size_t trak = box_open(o, "trak"), tkhd = box_open(o, "tkhd");
	u8(o, v1), u8(o, 0), u16(o, 3, false);
	if (v1) {
		for (int i = 0; i < 32; i++) u8(o, 0);
	} else {
		for (int i = 0; i < 20; i++) u8(o, 0);
	}
	for (int i = 0; i < 16; i++) u8(o, 0);
	int32_t one = 0x10000, a = one, b = 0, c = 0, d = one;
	if (turn == 90) a = 0, b = one, c = -one, d = 0;
	if (turn == 180) a = -one, d = -one;
	if (turn == 270) a = 0, b = -one, c = one, d = 0;
	int32_t m[9] = {a, b, 0, c, d, 0, 0, 0, 0x40000000};
	for (int i = 0; i < 9; i++) u32(o, (uint32_t)m[i], false);
	u32(o, sized ? (uint32_t)w << 16 : 0, false), u32(o, sized ? (uint32_t)h << 16 : 0, false);
	box_close(o, tkhd);
	size_t mdia = box_open(o, "mdia"), hdlr = box_open(o, "hdlr");
	u32(o, 0, false), u32(o, 0, false), put(o, handler, 4), u32(o, 0, false), u32(o, 0, false), u32(o, 0, false), u8(o, 0);
	box_close(o, hdlr);
	size_t minf = box_open(o, "minf"), stbl = box_open(o, "stbl"), stsd = box_open(o, "stsd");
	u32(o, 0, false), u32(o, 1, false);
	size_t entry = box_open(o, "avc1");
	for (int i = 0; i < 6; i++) u8(o, 0);
	u16(o, 1, false);
	for (int i = 0; i < 16; i++) u8(o, 0);
	u16(o, (unsigned)w, false), u16(o, (unsigned)h, false);
	for (int i = 0; i < 50; i++) u8(o, 0);
	box_close(o, entry);
	box_close(o, stsd), box_close(o, stbl), box_close(o, minf), box_close(o, mdia), box_close(o, trak);
}

// movie writes a file: ftyp, an mdat, then a moov (as a camera writes
// it: the index last) of a sound track and a video one.
static void movie(const char *path, int w, int h, int turn, bool v1, bool sized, uint32_t scale, uint32_t duration) {
	buf o = {{0}, 0};
	size_t ftyp = box_open(&o, "ftyp");
	put(&o, "isom\0\0\2\0isommp41", 16);
	box_close(&o, ftyp);
	size_t mdat = box_open(&o, "mdat");
	for (int i = 0; i < 300; i++) u8(&o, (unsigned)i);
	box_close(&o, mdat);
	size_t moov = box_open(&o, "moov"), mvhd = box_open(&o, "mvhd");
	if (v1) {
		u32(&o, 0x01000000, false);
		u32(&o, 0, false), u32(&o, 0, false), u32(&o, 0, false), u32(&o, 0, false);  // created, modified
		u32(&o, scale, false), u32(&o, 0, false), u32(&o, duration, false);
	} else {
		u32(&o, 0, false), u32(&o, 0, false), u32(&o, 0, false), u32(&o, scale, false), u32(&o, duration, false);
	}
	for (int i = 0; i < 80; i++) u8(&o, 0);
	box_close(&o, mvhd);
	track(&o, "soun", 0, 0, 0, v1, true);
	track(&o, "vide", w, h, turn, v1, sized);
	box_close(&o, moov);
	FILE *f = fopen(path, "wb");
	fwrite(o.b, 1, o.n, f);
	fclose(f);
}

static void movies(const char *dir) {
	char path[1024];
	snprintf(path, sizeof path, "%s/movie.mp4", dir);
	int64_t w, h, ms;
	movie(path, 1920, 1080, 0, false, true, 600, 3000);
	CHECK(device_mp4_info(path, &w, &h, &ms) && w == 1920 && h == 1080 && ms == 5000);
	movie(path, 1920, 1080, 90, false, true, 44100, 44100 * 2 + 4410);  // a phone held upright
	CHECK(device_mp4_info(path, &w, &h, &ms) && w == 1080 && h == 1920 && ms == 2100);
	movie(path, 1280, 720, 180, true, true, 1000, 1500);
	CHECK(device_mp4_info(path, &w, &h, &ms) && w == 1280 && h == 720 && ms == 1500);
	movie(path, 640, 480, 270, false, false, 30, 90);  // tkhd's size 0: the sample description's
	CHECK(device_mp4_info(path, &w, &h, &ms) && w == 480 && h == 640 && ms == 3000);
	FILE *f = fopen(path, "wb");
	fputs("not a movie at all, but text long enough to be read", f);
	fclose(f);
	CHECK(!device_mp4_info(path, &w, &h, &ms) && w == 0 && ms == 0);
	snprintf(path, sizeof path, "%s/none.mp4", dir);
	CHECK(!device_mp4_info(path, &w, &h, &ms));
}

// ---- Copying what's picked ----

static int jpegs, sizes, videos;

static bool fake_size(const char *path, int64_t *w, int64_t *h) {
	sizes++;
	*w = 30, *h = 20;
	return true;
}

// fake_jpeg "writes a JPEG": the file's text, and its size scaled as
// device_fit does a 3000×2000 photo's.
static bool fake_jpeg(const char *from, const char *to, int64_t max, double quality, int64_t *w, int64_t *h) {
	jpegs++;
	if (strstr(from, "bad")) return false;
	FILE *f = fopen(to, "wb");
	fprintf(f, "jpeg q=%.2f", quality);
	fclose(f);
	device_fit(3000, 2000, max, w, h);
	return true;
}

static void fake_video(const char *path, int64_t *w, int64_t *h, int64_t *ms) {
	videos++;
	*w = 1, *h = 2, *ms = 3;
}

static const device_media_ops ops = {fake_size, fake_jpeg, fake_video};

static char *write_text(const char *dir, const char *name, const char *text) {
	char *p = asuka_sprintf("%s/%s", dir, name);
	FILE *f = fopen(p, "wb");
	fputs(text, f);
	fclose(f);
	return p;
}

static void copying(const char *dir) {
	char mp4[1024];
	snprintf(mp4, sizeof mp4, "%s/Clip.MOV", dir);
	movie(mp4, 1920, 1080, 90, false, true, 600, 600);
	char *photo = write_text(dir, "IMG_1.HEIC", "a photo");
	char *webm = write_text(dir, "talk.webm", "a webm");
	char *bad = write_text(dir, "bad.png", "not an image");
	char *paths[] = {mp4, photo, webm, bad};
	device_list_NativeMedia got;
	asuka_error err = {0};

	// As they are: copied, read for their sizes.
	CHECK(device_copy_media(paths, 4, 3, 0, 0, &ops, &got, &err) && got.len == 3);
	if (got.len == 3) {
		device_NativeMedia *v = &got.data[0], *p = &got.data[1], *w = &got.data[2];
		CHECK(v->video && !strcmp(v->name, "Clip.MOV") && !strcmp(v->type, "video/quicktime") && v->width == 1080 && v->height == 1920 &&
		      v->durationMs == 1000 && v->size > 300 && strstr(v->path, "asuka-photos") && strstr(v->path, "Clip.MOV"));
		CHECK(!p->video && !strcmp(p->name, "IMG_1.HEIC") && !strcmp(p->type, "image/heic") && p->width == 30 && p->height == 20 && p->size == 7);
		CHECK(w->video && !strcmp(w->type, "video/webm") && w->width == 1 && w->height == 2 && w->durationMs == 3);
		CHECK(strcmp(v->path, mp4) != 0);
		FILE *f = fopen(p->path, "rb");
		char text[16] = {0};
		CHECK(f && fread(text, 1, sizeof text - 1, f) == 7 && !strcmp(text, "a photo"));
		if (f) fclose(f);
	}
	CHECK(jpegs == 0 && sizes == 1 && videos == 1);  // the movie read itself
	device_list_NativeMedia_free(&got);

	// Scaled: photos written as JPEGs, videos as they are; what isn't an
	// image the op reads, copied.
	CHECK(device_copy_media(paths, 4, 0, 1024, 0, &ops, &got, &err) && got.len == 4);
	if (got.len == 4) {
		device_NativeMedia *p = &got.data[1], *b = &got.data[3];
		CHECK(!strcmp(p->type, "image/jpeg") && !strcmp(p->name, "IMG_1.HEIC") && strstr(p->path, "IMG_1.jpg") && p->width == 1024 && p->height == 683 &&
		      p->size == (int64_t)strlen("jpeg q=0.90"));
		CHECK(!strcmp(b->type, "image/png") && strstr(b->path, "bad.png") && b->width == 30);
		CHECK(got.data[0].video && got.data[0].width == 1080);
	}
	CHECK(jpegs == 2);
	device_list_NativeMedia_free(&got);
	CHECK(device_copy_media(paths + 1, 1, 0, 0, 0.5, &ops, &got, &err) && got.len == 1 && got.data[0].size == (int64_t)strlen("jpeg q=0.50"));
	device_list_NativeMedia_free(&got);

	// None picked: none.
	CHECK(device_copy_media(NULL, 0, 0, 0, 0, &ops, &got, &err) && got.len == 0);
	device_list_NativeMedia_free(&got);

	// A file that's gone fails.
	char *gone = asuka_sprintf("%s/gone.jpg", dir);
	CHECK(!device_copy_media(&gone, 1, 0, 0, 0, &ops, &got, &err) && err.status);
	asuka_error_free(&err);
	free(gone);
	free(photo), free(webm), free(bad);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const char *dir = asuka_desktop_cache_dir();
	orientations();
	fits();
	movies(dir);
	copying(dir);
	printf(failed ? "FAIL\n" : "ok\n");
	return failed;
}
