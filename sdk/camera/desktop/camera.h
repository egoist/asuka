// camera.h: what camera's Windows and Linux C share (camera.c): a
// started call waiting on the camera, the kinds of code a view reads and
// the codes it's seen, the files photos and videos go in, and the take
// dialog's words.
#ifndef CAMERA_H
#define CAMERA_H

#include "camera.asuka.h"

// A call waiting on the camera (take, capture, record): kept in one slot
// (a view's, a dialog's, a photo being saved), which it clears as it's
// answered. If its task goes first, it's answered as cancelled, the slot
// cleared, and cancel(ctx) run (a recording stopped, a dialog closed).
// Main thread.
typedef struct camera_pending camera_pending;

// camera_wait makes the call wait in *slot (where it's kept until
// answered).
void camera_wait(asuka_call *call, void (*done)(asuka_call *call, char *value), camera_pending **slot, void (*cancel)(void *ctx), void *ctx);
// camera_move keeps the waiting call of *from in *to instead.
void camera_move(camera_pending **from, camera_pending **to);
// camera_answer answers the call in *slot (if any) with value; camera_fail
// with the platform's error (a message), camera_fail_declared with one of
// the package's.
void camera_answer(camera_pending **slot, const char *value);
void camera_fail(camera_pending **slot, const char *message);
void camera_fail_declared(camera_pending **slot, uint32_t index);

// The kinds of code a view reads (none: every kind).
typedef struct {
	char **kinds;
	size_t n;
} camera_kinds;

void camera_kinds_set(camera_kinds *k, camera_list_string list);
bool camera_kinds_wants(const camera_kinds *k, const char *kind);
void camera_kinds_free(camera_kinds *k);

// The codes in view: one is new as it comes into view, and again once
// it's been out of it a second.
typedef struct {
	struct {
		char *key;
		int64_t at;
	} *seen;
	size_t n, cap;
} camera_seen;

bool camera_seen_new(camera_seen *s, const char *kind, const char *value, int64_t now_ms);
void camera_seen_free(camera_seen *s);

// camera_now_ms is a monotonic clock's milliseconds.
int64_t camera_now_ms(void);

// camera_file is a new file's path (malloc's) in the app's caches'
// asuka-camera, of extension ext (".jpg"), made if missing: NULL if it
// can't be.
char *camera_file(const char *ext);

// ---- Zoom, frames, photos: the math both platforms share ----

// The digital zoom's most (a webcam's, cropping the middle of what it
// sees), and the frames' most a second.
#define CAMERA_DIGITAL_ZOOM 4.0
#define CAMERA_MAX_FRAME_RATE 15.0

// camera_clamp is v within lo and hi (lo if v is NaN).
double camera_clamp(double v, double lo, double hi);

// A part of a frame, in its pixels.
typedef struct {
	int x, y, w, h;
} camera_rect;

// camera_zoom_crop is the middle of a w×h frame that a zoom of factor
// shows (all of it at 1 or less).
camera_rect camera_zoom_crop(int w, int h, double zoom);
// camera_fit is w×h scaled down so its long side is at most max (0: as it
// is; never up), at least 1×1.
void camera_fit(int w, int h, int max, int *ow, int *oh);

// camera_resample writes the part r of a frame of 4-byte pixels (rows
// of stride bytes; its channels' order rgb: 0, 1, 2 for RGBA, 2, 1, 0 for
// BGRA) as dw×dh pixels of 3 bytes in that order (out's rows dw*3), each
// the average of the pixels it covers, mirrored left to right if mirror.
void camera_resample(const uint8_t *src, size_t stride, camera_rect r, bool mirror, uint8_t *out, int dw, int dh);

// An optical zoom's range as a camera's control gives it (UVC's zoom,
// V4L2's zoom_absolute, IAMCameraControl's): a factor is a value over its
// least, when its least is more than 0 (100 to 500: 1× to 5×). false when
// its values aren't such.
typedef struct {
	long min, max;
} camera_zoom_range;

bool camera_zoom_optical(camera_zoom_range r, double *max_zoom);
long camera_zoom_value(camera_zoom_range r, double zoom);

// A view's frames: frameRate's clock, and the ring of files they're
// written to.
typedef struct {
	unsigned view;
	double rate;      // a second (0: none)
	int size;         // the long side's most
	int64_t started;  // the view's start (camera_now_ms)
	int64_t last;     // the last one sampled
	int next;         // the ring's next file
	bool writing;     // one's being written: the next waits for it
} camera_frames;

#define CAMERA_FRAME_RING 4

// camera_frames_due reports whether a frame should be sampled at now:
// frameRate's interval since the last, none being written. It takes it
// if so (*path: the ring's next file, malloc's; *ms: since the start).
bool camera_frames_due(camera_frames *f, int64_t now, char **path, int64_t *ms);
// camera_frames_clean removes a view's ring's files.
void camera_frames_clean(unsigned view);

// camera_record_full reports whether a recording of size bytes so far
// should stop to stay within max bytes, at bitrate bits a second, its
// size read every interval ms (what it writes by the next read, and its
// index's, kept below max).
bool camera_record_full(int64_t size, int64_t max, double bitrate, int interval);

// The nearest of videoQuality's sizes, in order of preference: sizes[0]
// is the one asked for.
typedef struct {
	int w, h;
} camera_size;

void camera_quality_sizes(camera_VideoQuality q, camera_size sizes[4]);
// camera_size_distance is how far a camera's size is from the one asked
// for: 0 the same, less nearer.
double camera_size_distance(camera_size want, int w, int h);

// The take dialog's words, in a language: the first that the user's
// languages (locales, most preferred first: "de-DE", "zh-Hant-TW", "pt_BR")
// match, else English.
typedef struct {
	const char *lang, *take, *cancel;
} camera_word;

extern const camera_word camera_words[];
const camera_word *camera_words_for(const char *const *locales, size_t n);

#endif
