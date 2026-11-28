// camera_test.c: what camera's Windows and Linux C share (camera.c), on
// every system (platform/desktop's TestStdUnits): a digital zoom's crop,
// a photo's fit, frames resampled (cropped, mirrored, averaged), an
// optical zoom's values, frameRate's clock and its ring of files, a
// recording's byte limit, and videoQuality's sizes nearest first.
#include "camera.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

static void zoom_crop(void) {
	camera_rect r = camera_zoom_crop(640, 480, 1);
	CHECK(r.x == 0 && r.y == 0 && r.w == 640 && r.h == 480);
	r = camera_zoom_crop(640, 480, 0.5);  // never wider than the camera
	CHECK(r.w == 640 && r.h == 480);
	r = camera_zoom_crop(640, 480, 2);
	CHECK(r.x == 160 && r.y == 120 && r.w == 320 && r.h == 240);
	r = camera_zoom_crop(1920, 1080, 4);
	CHECK(r.x == 720 && r.y == 405 && r.w == 480 && r.h == 270);
	r = camera_zoom_crop(3, 3, 100);
	CHECK(r.w == 1 && r.h == 1 && r.x == 1 && r.y == 1);
	CHECK(camera_clamp(NAN, 1, 4) == 1);
	CHECK(camera_clamp(9, 1, 4) == 4);
	CHECK(camera_clamp(2.5, 1, 4) == 2.5);
}

static void fit(void) {
	int w, h;
	camera_fit(4032, 3024, 0, &w, &h);
	CHECK(w == 4032 && h == 3024);
	camera_fit(4032, 3024, 2048, &w, &h);
	CHECK(w == 2048 && h == 1536);
	camera_fit(1080, 1920, 480, &w, &h);
	CHECK(w == 270 && h == 480);
	camera_fit(100, 50, 480, &w, &h);  // never up
	CHECK(w == 100 && h == 50);
	camera_fit(10000, 1, 100, &w, &h);
	CHECK(w == 100 && h == 1);
}

static void resample(void) {
	// 4×2 RGBA: each pixel's red its x, green its y, blue 7.
	uint8_t src[4 * 2 * 4];
	for (int y = 0; y < 2; y++)
		for (int x = 0; x < 4; x++) {
			uint8_t *p = src + (y * 4 + x) * 4;
			p[0] = (uint8_t)(x * 10), p[1] = (uint8_t)(y * 100), p[2] = 7, p[3] = 255;
		}
	uint8_t out[4 * 2 * 3];
	camera_resample(src, 16, (camera_rect){0, 0, 4, 2}, false, out, 4, 2);
	CHECK(out[0] == 0 && out[3] == 10 && out[9] == 30 && out[12 + 1] == 100 && out[2] == 7);
	camera_resample(src, 16, (camera_rect){0, 0, 4, 2}, true, out, 4, 2);  // mirrored
	CHECK(out[0] == 30 && out[9] == 0);
	camera_resample(src, 16, (camera_rect){0, 0, 4, 2}, false, out, 2, 1);  // halved: averaged
	CHECK(out[0] == 5 && out[1] == 50 && out[3] == 25 && out[5] == 7);
	camera_resample(src, 16, (camera_rect){1, 1, 2, 1}, false, out, 2, 1);  // a crop
	CHECK(out[0] == 10 && out[3] == 20 && out[1] == 100);
}

static void optical(void) {
	double max = 0;
	CHECK(camera_zoom_optical((camera_zoom_range){100, 500}, &max) && max == 5);
	CHECK(!camera_zoom_optical((camera_zoom_range){0, 10}, &max));
	CHECK(!camera_zoom_optical((camera_zoom_range){100, 100}, &max));
	camera_zoom_range r = {100, 500};
	CHECK(camera_zoom_value(r, 1) == 100);
	CHECK(camera_zoom_value(r, 2.5) == 250);
	CHECK(camera_zoom_value(r, 9) == 500);
	CHECK(camera_zoom_value(r, 0.5) == 100);
}

static bool exists(const char *p) {
	struct stat st;
	return stat(p, &st) == 0;
}

static void frames_clock(void) {
	camera_frames f = {.view = 7, .rate = 4, .size = 480, .started = 1000};
	char *path = NULL;
	int64_t ms = 0;
	CHECK(camera_frames_due(&f, 1000, &path, &ms) && ms == 0 && strstr(path, "frame-7-0.jpg"));
	free(path);
	CHECK(!camera_frames_due(&f, 1100, &path, &ms));  // one's being written
	f.writing = false;
	CHECK(!camera_frames_due(&f, 1100, &path, &ms));  // a quarter second apart
	CHECK(camera_frames_due(&f, 1260, &path, &ms) && ms == 260 && strstr(path, "frame-7-1.jpg"));
	free(path);
	f.writing = false;
	CHECK(camera_frames_due(&f, 1500, &path, &ms));  // on its beat (1250 + 250)
	free(path);
	f.writing = false;
	CHECK(camera_frames_due(&f, 1750, &path, &ms));
	CHECK(strstr(path, "frame-7-3.jpg") != NULL);
	free(path);
	f.writing = false;
	CHECK(camera_frames_due(&f, 9000, &path, &ms) && strstr(path, "frame-7-0.jpg"));  // the ring's first again
	FILE *o = fopen(path, "wb");
	CHECK(o != NULL);
	if (o) fclose(o);
	CHECK(exists(path));
	camera_frames_clean(7);
	CHECK(!exists(path));
	free(path);
	f.writing = false;
	f.rate = 100;  // at most 15 a second
	f.last = 0;
	CHECK(camera_frames_due(&f, 20000, &path, &ms));
	free(path);
	f.writing = false;
	CHECK(!camera_frames_due(&f, 20060, &path, &ms));
	CHECK(camera_frames_due(&f, 20067, &path, &ms));
	free(path);
	f = (camera_frames){.rate = 0};
	CHECK(!camera_frames_due(&f, 5, &path, &ms));
}

static void bytes(void) {
	CHECK(!camera_record_full(1000, 0, 8e6, 250));  // no limit
	// 8 Mb/s, read every 250 ms: 1 MB a read; two ahead, and the index.
	CHECK(!camera_record_full(5000000, 10000000, 8e6, 250));
	CHECK(!camera_record_full(9400000, 10000000, 8e6, 250));
	CHECK(camera_record_full(9500000, 10000000, 8e6, 250));
	CHECK(camera_record_full(12000000, 10000000, 8e6, 250));
}

static void qualities(void) {
	camera_size s[4];
	camera_quality_sizes(camera_VideoQuality_fullHD, s);
	CHECK(s[0].w == 1920 && s[1].w == 1280 && s[2].w == 3840 && s[3].w == 640);
	camera_quality_sizes(camera_VideoQuality_sd, s);
	CHECK(s[0].w == 640 && s[1].w == 1280 && s[2].w == 1920 && s[3].w == 3840);
	camera_quality_sizes(camera_VideoQuality_uhd, s);
	CHECK(s[0].w == 3840 && s[1].w == 1920 && s[3].w == 640);
	camera_size hd = {1280, 720};
	CHECK(camera_size_distance(hd, 1280, 720) == 0);
	CHECK(camera_size_distance(hd, 1920, 1080) < camera_size_distance(hd, 3840, 2160));
	CHECK(camera_size_distance(hd, 960, 720) > 0);
	CHECK(camera_size_distance(hd, 1280, 720) < camera_size_distance(hd, 1280, 960));
	CHECK(camera_size_distance(hd, 0, 0) > 1000);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	zoom_crop();
	fit();
	resample();
	optical();
	frames_clock();
	bytes();
	qualities();
	printf(failed ? "FAIL\n" : "ok\n");
	return failed;
}
