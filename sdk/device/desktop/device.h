// device.h: what device's Windows and Linux C share (device.c).
#ifndef DEVICE_H
#define DEVICE_H

#include "device.asuka.h"

// device_copy_picked copies the files into the app's caches, each in a
// folder of its own (so its name stays its own), as a phone's pickers
// give them; false with err set.
bool device_copy_picked(char **paths, size_t n, size_t limit, device_list_File *out, asuka_error *err);
// device_write_cache writes data into a new file of the app's caches'
// folder dir, with extension ext: its path (NULL with err set).
char *device_write_cache(const char *dir, const char *ext, const void *data, size_t len, asuka_error *err);
// device_web_url reports whether u is an http(s) URL.
bool device_web_url(const char *u);
// device_mime is a file name's MIME type by its extension ("" unknown).
const char *device_mime(const char *name);

// What a platform's C does with a picked photo or video for
// device_copy_media, off the main thread. Any may be NULL.
typedef struct {
	// image_size is an image file's size, upright (its EXIF orientation
	// applied): false if it can't read it.
	bool (*image_size)(const char *path, int64_t *w, int64_t *h);
	// jpeg decodes an image file, turns it upright, scales it down so its
	// long side is at most max (0: as it is; never up) and writes it to
	// `to` as a JPEG of quality (0 to 1): its size; false if it can't.
	bool (*jpeg)(const char *from, const char *to, int64_t max, double quality, int64_t *w, int64_t *h);
	// video is a video's size (after its rotation) and length (ms) where
	// the MPEG-4 reader (device_mp4_info) found none: 0s where it can't
	// tell.
	void (*video)(const char *path, int64_t *w, int64_t *h, int64_t *ms);
} device_media_ops;

// device_copy_media copies the photos and videos picked (at most limit:
// 0 any) into the app's caches' asuka-photos, each in a folder of its
// own, and says what each is. With max_size or quality (0 to 1) not 0,
// an image is written as a JPEG instead, upright, its long side at most
// max_size, of quality (0: 0.9); videos are copied as they are. false
// with err set.
bool device_copy_media(char **paths, size_t n, size_t limit, int64_t max_size, double quality, const device_media_ops *ops, device_list_NativeMedia *out,
                       asuka_error *err);

// device_jpeg_orientation is a JPEG's EXIF orientation (1 to 8; 1 when it
// has none, or isn't a JPEG), from its first bytes.
int device_jpeg_orientation(const uint8_t *data, size_t len);
// device_file_orientation is an image file's EXIF orientation, likewise.
int device_file_orientation(const char *path);
// device_fit is w×h scaled down so its long side is at most max (0: as it
// is; never up), at least 1×1.
void device_fit(int64_t w, int64_t h, int64_t max, int64_t *ow, int64_t *oh);
// device_mp4_info reads an MPEG-4 or QuickTime file's (ISO BMFF: .mp4,
// .mov, .m4v, .3gp) video track's size, turned as its matrix turns it,
// and the movie's length: false if it isn't one, or has no video.
bool device_mp4_info(const char *path, int64_t *w, int64_t *h, int64_t *ms);

#endif
