// image.c: Image views. Images are fetched and decoded off the main thread
// by Windows's own codecs (WIC: PNG, JPEG, GIF, BMP, TIFF, ICO, and HEIF,
// WebP, AVIF and RAW where their extensions are installed), turned upright
// as their EXIF orientation says, kept in memory by source, and drawn with
// GDI+ at the view's mode, clipped to its corners. An animated GIF, APNG or
// WebP (its frames' pictures by WIC's codecs) plays here, frame after frame
// on a timer (platform/desktop's dsk_anim), while it isn't Paused. The
// placeholder (an asset, or the core's decoded hash, in its own mode) shows
// until the image arrives, which fades in over it (Transition; 0.25s over a
// placeholder).
//
// A source is fetched once at a time: the views waiting for it are kept
// by source and called when it's decoded. The cache, the waiting views
// and the decoded images are the main thread's; a fetch's thread only
// posts its result.
#include "host.h"
#include "parts.h"
#include "draw.h"

#include <stdarg.h>
#include <wincodec.h>

// A decoded image: its premultiplied BGRA pixels (an animation's first
// frame), its bitmap, and its animation; or why it isn't one.
typedef struct decoded {
	char *src;
	int w, h;
	uint8_t *pixels;
	GpBitmap *bmp;  // made on the main thread, the first time it's shown
	dsk_anim *anim;
	char *err;
	struct decoded *next;  // the cache's chain
} decoded;

struct wh_image {
	char *source, *placeholder;
	int32_t mode;
	int32_t ph_mode;  // PlaceholderMode; -1: Mode
	decoded *dec;     // the image, once it loads
	bool loaded;
	GpBitmap *bmp;  // what shows: the image, or its animation's frame
	int bw, bh;
	GpBitmap *tinted;
	ak_color tint;
	// The placeholder until the image arrives: an asset's (tinted as the
	// image is) or the hash's (closed with the view).
	GpBitmap *ph;
	int pw, phh;
	GpBitmap *hash;
	int hw, hh;
	double trans;
	float fade;  // the image's opacity as it arrives
	double fade_start, fade_for;
	UINT_PTR fade_timer;
	bool paused;
	bool playing;  // player is ready
	dsk_anim_player player;
	GpBitmap *frame;  // the animation's frame showing
	uint8_t *frame_px;
	UINT_PTR frame_timer;
	int wait;  // the frame's time left, when paused (ms)
};

// wh_premultiply_rgba turns RGBA, not premultiplied, into GDI+'s PARGB
// (premultiplied BGRA), in place.
void wh_premultiply_rgba(uint8_t *px, size_t n) {
	for (size_t i = 0; i + 3 < n; i += 4) {
		uint32_t a = px[i + 3], r = px[i], g = px[i + 1], b = px[i + 2];
		px[i] = (uint8_t)(b * a / 255);
		px[i + 1] = (uint8_t)(g * a / 255);
		px[i + 2] = (uint8_t)(r * a / 255);
	}
}

static double now_seconds(void) { return (double)wh_now_ns() / 1e9; }

static void dispose(GpBitmap **b) {
	if (*b) GdipDisposeImage((GpImage *)*b);
	*b = NULL;
}

// ---- Decoding (any thread, in COM) ----

static bool wic_rgba(IWICImagingFactory *f, const uint8_t *data, size_t len, uint8_t **out, int *ow, int *oh);

// still decodes a frame's picture for dsk_anim (RGBA, not premultiplied).
static bool still(void *ctx, const uint8_t *data, size_t len, uint8_t **rgba, int *w, int *h) {
	return wic_rgba(ctx, data, len, rgba, w, h);
}

// orientation is a frame's EXIF orientation (System.Photo.Orientation:
// JPEG's, TIFF's, HEIF's), 1 if it has none.
static int orientation(IWICBitmapFrameDecode *frame) {
	IWICMetadataQueryReader *qr = NULL;
	if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &qr)) || !qr) return 1;
	static const wchar_t *names[] = {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
	int o = 1;
	for (size_t i = 0; i < WH_LEN(names) && o == 1; i++) {
		PROPVARIANT pv;
		PropVariantInit(&pv);
		if (FAILED(IWICMetadataQueryReader_GetMetadataByName(qr, names[i], &pv))) continue;
		int v = 0;
		switch (pv.vt) {
		case VT_UI2: v = pv.uiVal; break;
		case VT_UI4: v = (int)pv.ulVal; break;
		case VT_I4: v = (int)pv.lVal; break;
		case VT_UI1: v = pv.bVal; break;
		}
		PropVariantClear(&pv);
		if (v >= 1 && v <= 8) o = v;
	}
	IWICMetadataQueryReader_Release(qr);
	return o;
}

// orient turns RGBA pixels as their EXIF orientation (1 to 8) says, so
// they're upright.
static uint8_t *orient(uint8_t *px, int *pw, int *ph, int o) {
	if (o <= 1 || o > 8) return px;
	int w = *pw, h = *ph, dw = w, dh = h;
	if (o >= 5) dw = h, dh = w;
	uint8_t *out = malloc((size_t)dw * (size_t)dh * 4);
	if (!out) return px;
	for (int y = 0; y < dh; y++)
		for (int x = 0; x < dw; x++) {
			int sx = 0, sy = 0;
			switch (o) {
			case 2: sx = w - 1 - x, sy = y; break;
			case 3: sx = w - 1 - x, sy = h - 1 - y; break;
			case 4: sx = x, sy = h - 1 - y; break;
			case 5: sx = y, sy = x; break;
			case 6: sx = y, sy = h - 1 - x; break;
			case 7: sx = w - 1 - y, sy = h - 1 - x; break;
			case 8: sx = w - 1 - y, sy = x; break;
			}
			memcpy(out + ((size_t)y * (size_t)dw + (size_t)x) * 4, px + ((size_t)sy * (size_t)w + (size_t)sx) * 4, 4);
		}
	free(px);
	*pw = dw, *ph = dh;
	return out;
}

// wic_rgba decodes an image file's bytes, upright, as RGBA not
// premultiplied (malloc'd).
static bool wic_rgba(IWICImagingFactory *f, const uint8_t *data, size_t len, uint8_t **out, int *ow, int *oh) {
	if (!f || !len || len > UINT32_MAX) return false;
	IWICStream *s = NULL;
	IWICBitmapDecoder *dec = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *conv = NULL;
	bool ok = false;
	UINT w = 0, h = 0;
	if (FAILED(IWICImagingFactory_CreateStream(f, &s)) || FAILED(IWICStream_InitializeFromMemory(s, (BYTE *)data, (DWORD)len))) goto done;
	if (FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)s, NULL, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
	if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))) goto done;
	IWICBitmapFrameDecode_GetSize(frame, &w, &h);
	if (!w || !h) goto done;
	if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &conv))) goto done;
	if (FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0,
			WICBitmapPaletteTypeCustom)))
		goto done;
	size_t stride = (size_t)w * 4;
	if (stride * h > UINT32_MAX) goto done;
	uint8_t *px = malloc(stride * h);
	if (!px) goto done;
	if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, (UINT)stride, (UINT)(stride * h), px))) {
		free(px);
		goto done;
	}
	for (size_t i = 0; i < stride * h; i += 4) {
		uint8_t t = px[i];
		px[i] = px[i + 2], px[i + 2] = t;  // BGRA to RGBA
	}
	int iw = (int)w, ih = (int)h;
	px = orient(px, &iw, &ih, orientation(frame));
	*out = px, *ow = iw, *oh = ih;
	ok = true;
done:
	if (conv) IWICFormatConverter_Release(conv);
	if (frame) IWICBitmapFrameDecode_Release(frame);
	if (dec) IWICBitmapDecoder_Release(dec);
	if (s) IWICStream_Release(s);
	return ok;
}

static char *errorf(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return wh_strdup(buf);
}

// images_dir is where images fetched over http(s) are kept across
// launches: the app's caches' asuka-images (dsk_image_cache_*).
static const char *images_dir(void) {
	static char *dir;
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	BOOL pending;
	if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
		dir = dsk_join(asuka_desktop_cache_dir(), "asuka-images");
		InitOnceComplete(&once, 0, NULL);
	}
	return dir;
}

// http_get fetches a URL (redirects followed) into *out, from the disk's
// cache if it's there, keeping it there if it may: NULL, or why not.
static char *http_get(const char *url, uint8_t **out, size_t *len) {
	if ((*out = dsk_image_cache_read(images_dir(), url, len))) return NULL;
	asuka_response r;
	asuka_error e = {0};
	if (!asuka_fetch("GET", url, NULL, 0, NULL, 0, 0, NULL, &r, &e)) {
		char *err = errorf("%s", e.message ? e.message : url);
		asuka_error_free(&e);
		return err;
	}
	char *err = NULL;
	if (r.status != 200) {
		err = errorf("%s: %d", url, r.status);
		asuka_response_free(&r);
		return err;
	}
	const char *cc = NULL;
	for (size_t i = 0; i < r.nheaders; i++)
		if (!strcmp(r.names[i], "cache-control")) cc = r.values[i];
	if (dsk_image_cache_storable(url, cc)) dsk_image_cache_write(images_dir(), url, r.body.data, r.body.len);
	*out = r.body.data, *len = r.body.len;
	r.body.data = NULL;
	asuka_response_free(&r);
	return NULL;
}

static bool absolute(const char *p) {
	return (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) || (p[0] == '\\' && p[1] == '\\') || (p[0] == '/' && p[1] == '/');
}

// decode reads and decodes src: an animation (GIF, APNG, WebP), else a
// still by WIC.
static decoded *decode(const char *src, const char *assets) {
	decoded *d = wh_alloc(sizeof *d);
	d->src = wh_strdup(src);
	uint8_t *data = NULL;
	size_t len = 0;
	if (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8)) {
		d->err = http_get(src, &data, &len);
	} else {
		char *path;
		if (!strncmp(src, "file://", 7)) path = wh_strdup(src + 7);
		else if (absolute(src)) path = wh_strdup(src);
		else path = dsk_join(assets, src);
		for (char *c = path; *c; c++)
			if (*c == '/') *c = '\\';
		data = (uint8_t *)dsk_read_file(path, &len);
		if (!data) d->err = errorf("open %s: the system cannot find the file specified", path);
		free(path);
	}
	if (d->err) return d;
	IWICImagingFactory *f = NULL;
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
	if (dsk_anim_is_animated(data, len)) {
		dsk_anim *a = dsk_anim_decode(data, len, f ? still : NULL, f);
		dsk_anim_player p;
		if (a && dsk_anim_player_init(&p, a)) {
			dsk_anim_next(&p, NULL);
			size_t n = (size_t)a->w * (size_t)a->h * 4;
			d->pixels = wh_alloc(n);
			memcpy(d->pixels, p.canvas, n);
			wh_premultiply_rgba(d->pixels, n);
			d->w = a->w, d->h = a->h, d->anim = a;
			dsk_anim_player_free(&p);
		} else {
			dsk_anim_free(a);
		}
	}
	if (!d->anim) {
		uint8_t *px = NULL;
		int w = 0, h = 0;
		if (!wic_rgba(f, data, len, &px, &w, &h)) {
			d->err = errorf("%s: a format Windows has no codec for", src);
		} else if (!w || !h) {
			free(px);
			d->err = errorf("%s: an empty image", src);
		} else {
			wh_premultiply_rgba(px, (size_t)w * (size_t)h * 4);
			d->pixels = px, d->w = w, d->h = h;
		}
	}
	if (f) IWICImagingFactory_Release(f);
	free(data);
	return d;
}

// ---- The cache and fetching (main thread) ----

static wh_map cache;  // by the source's hash: a chain of decoded

typedef struct {
	char *src;
	uint32_t id;
	bool placeholder;
} waiter;

static waiter *waiters;
static uint32_t nwaiters, capwaiters;

static uint64_t hash(const char *s) {
	uint64_t h = 1469598103934665603ull;
	for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
	return h ? h : 1;
}

static decoded *cached(const char *src) {
	for (decoded *d = wh_map_get(&cache, hash(src)); d; d = d->next)
		if (!strcmp(d->src, src)) return d;
	return NULL;
}

typedef struct {
	char *src, *assets;
	decoded *d;
} fetch_job;

static void arrived(node *n, decoded *d, const char *src, bool placeholder);

static void fetched(void *ctx) {
	fetch_job *j = ctx;
	decoded *d = j->d;
	uint64_t k = hash(d->src);
	d->next = wh_map_get(&cache, k);
	wh_map_put(&cache, k, d);
	// The views waiting for it, taken first: what they do may wait anew.
	waiter *ready = NULL;
	uint32_t nready = 0;
	for (uint32_t i = 0; i < nwaiters;) {
		if (!strcmp(waiters[i].src, d->src)) {
			ready = wh_realloc(ready, (nready + 1) * sizeof *ready);
			ready[nready++] = waiters[i];
			waiters[i] = waiters[--nwaiters];
		} else {
			i++;
		}
	}
	for (uint32_t i = 0; i < nready; i++) {
		node *n = wh_node(ready[i].id);
		if (n && n->image) arrived(n, d, d->src, ready[i].placeholder);
		free(ready[i].src);
	}
	free(ready);
	free(j->src);
	free(j->assets);
	free(j);
}

static DWORD WINAPI fetch_thread(void *ctx) {
	fetch_job *j = ctx;
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	j->d = decode(j->src, j->assets);
	if (SUCCEEDED(hr)) CoUninitialize();
	wh_post(fetched, j);
	return 0;
}

// fetch calls arrived for n on the main thread with src decoded, from the
// cache or a thread of its own.
static void fetch(node *n, const char *src, bool placeholder) {
	decoded *d = cached(src);
	if (d) {
		arrived(n, d, src, placeholder);
		return;
	}
	bool fetching = false;
	for (uint32_t i = 0; i < nwaiters && !fetching; i++) fetching = !strcmp(waiters[i].src, src);
	if (nwaiters == capwaiters) {
		capwaiters = capwaiters ? capwaiters * 2 : 16;
		waiters = wh_realloc(waiters, capwaiters * sizeof *waiters);
	}
	waiters[nwaiters++] = (waiter){wh_strdup(src), n->id, placeholder};
	if (fetching) return;
	fetch_job *j = wh_alloc(sizeof *j);
	j->src = wh_strdup(src);
	j->assets = wh_strdup(H.assets ? H.assets : "assets");
	HANDLE t = CreateThread(NULL, 0, fetch_thread, j, 0, NULL);
	if (t) {
		CloseHandle(t);
	} else {
		j->d = wh_alloc(sizeof *j->d);
		j->d->src = wh_strdup(src);
		j->d->err = errorf("%s: no thread to fetch it", src);
		wh_post(fetched, j);
	}
}

// ---- Timers: the fade and the animation's frames ----

enum { FADE, FRAME };

typedef struct {
	UINT_PTR id;
	uint32_t node;
	int what;
} timer;

static timer timers[256];
static uint32_t ntimers;

static void CALLBACK on_timer(HWND hwnd, UINT m, UINT_PTR id, DWORD time);

static UINT_PTR start_timer(node *n, int what, UINT ms) {
	UINT_PTR id = SetTimer(NULL, 0, ms ? ms : 1, on_timer);
	if (!id) return 0;
	if (ntimers < WH_LEN(timers)) timers[ntimers++] = (timer){id, n->id, what};
	return id;
}

static void stop_timer(UINT_PTR *id) {
	if (!*id) return;
	KillTimer(NULL, *id);
	for (uint32_t i = 0; i < ntimers; i++)
		if (timers[i].id == *id) {
			timers[i] = timers[--ntimers];
			break;
		}
	*id = 0;
}

static void next_frame(node *n, int wait);

static void fade_step(node *n) {
	wh_image *im = n->image;
	double t = (now_seconds() - im->fade_start) / im->fade_for;
	if (t >= 1) {
		im->fade = 1;
		im->ph = NULL, im->pw = im->phh = 0;
		stop_timer(&im->fade_timer);
	} else {
		im->fade = (float)t;
	}
	wh_redraw(n);
}

static void frame_step(node *n) {
	wh_image *im = n->image;
	stop_timer(&im->frame_timer);
	if (!im->playing || im->paused || !im->loaded) return;
	int d = 0;
	if (!dsk_anim_next(&im->player, &d)) return;  // played its loops: the last frame stays
	const dsk_anim *a = im->player.a;
	size_t size = (size_t)a->w * (size_t)a->h * 4;
	if (!im->frame_px) im->frame_px = wh_alloc(size);
	memcpy(im->frame_px, im->player.canvas, size);
	wh_premultiply_rgba(im->frame_px, size);
	dispose(&im->frame);
	im->frame = g_bitmap(a->w, a->h, im->frame_px);
	im->bmp = im->frame, im->bw = a->w, im->bh = a->h;
	dispose(&im->tinted);
	wh_redraw(n);
	im->wait = d;
	next_frame(n, d);
}

static void CALLBACK on_timer(HWND hwnd, UINT m, UINT_PTR id, DWORD time) {
	(void)hwnd, (void)m, (void)time;
	for (uint32_t i = 0; i < ntimers; i++) {
		if (timers[i].id != id) continue;
		timer t = timers[i];
		node *n = wh_node(t.node);
		if (!n || !n->image) {
			UINT_PTR x = id;
			stop_timer(&x);
			return;
		}
		if (t.what == FADE) fade_step(n);
		else frame_step(n);
		return;
	}
	KillTimer(NULL, id);  // its view is gone
}

// next_frame shows the animation's next frame after wait, and the one
// after it after its time, while it plays.
static void next_frame(node *n, int wait) {
	wh_image *im = n->image;
	stop_timer(&im->frame_timer);
	if (!im->playing || im->paused || !im->loaded) return;
	im->frame_timer = start_timer(n, FRAME, (UINT)wait);
}

// ---- The view ----

void wh_image_new(node *n) {
	n->image = wh_alloc(sizeof *n->image);
	n->image->ph_mode = -1;
	n->image->fade = 1;
}

// release lets go of the tinted image.
static void release(wh_image *im) { dispose(&im->tinted); }

// stop_playing lets go of the animation's frame and player.
static void stop_playing(wh_image *im) {
	stop_timer(&im->frame_timer);
	if (im->playing) dsk_anim_player_free(&im->player);
	im->playing = false;
	if (im->bmp == im->frame) im->bmp = NULL;
	dispose(&im->frame);
	free(im->frame_px);
	im->frame_px = NULL;
}

void wh_image_close(node *n) {
	wh_image *im = n->image;
	if (!im) return;
	release(im);
	stop_playing(im);
	stop_timer(&im->fade_timer);
	if (im->ph == im->hash) im->ph = NULL;
	dispose(&im->hash);
	for (uint32_t i = 0; i < nwaiters;) {
		if (waiters[i].id == n->id) {
			free(waiters[i].src);
			waiters[i] = waiters[--nwaiters];
		} else {
			i++;
		}
	}
	free(im->source);
	free(im->placeholder);
	free(im);
	n->image = NULL;
}

// show_placeholder shows the image's placeholder while it hasn't loaded:
// the hash's, else the Placeholder image's.
static void show_placeholder(node *n) {
	wh_image *im = n->image;
	if (im->loaded) return;
	if (im->hash) {
		im->ph = im->hash, im->pw = im->hw, im->phh = im->hh;
		wh_redraw(n);
		return;
	}
	im->ph = NULL;
	wh_redraw(n);
	if (im->placeholder && *im->placeholder) fetch(n, im->placeholder, true);
}

// arrive fades the image in over its placeholder, or from nothing.
static void arrive(node *n) {
	wh_image *im = n->image;
	double d = im->trans;
	if (d < 0) d = 0;
	else if (d == 0 && im->ph) d = 0.25;
	stop_timer(&im->fade_timer);
	if (d <= 0 || !n->hwnd || !IsWindowVisible(n->hwnd)) {
		im->fade = 1, im->ph = NULL;
		return;
	}
	im->fade = 0;
	im->fade_start = now_seconds(), im->fade_for = d;
	im->fade_timer = start_timer(n, FADE, 16);
}

static void arrived(node *n, decoded *d, const char *src, bool placeholder) {
	wh_image *im = n->image;
	if (placeholder) {
		if (im->loaded || !im->placeholder || strcmp(im->placeholder, src) != 0 || im->hash) return;
	} else if (!im->source || strcmp(im->source, src) != 0) {
		return;
	}
	if (d->err) {
		if (!placeholder) wh_emit_str(n, AK_EVENT_FAILED, d->err, strlen(d->err));
		return;
	}
	if (!d->bmp) d->bmp = g_bitmap(d->w, d->h, d->pixels);
	if (placeholder) {
		im->ph = d->bmp, im->pw = d->w, im->phh = d->h;
		wh_redraw(n);
		return;
	}
	im->dec = d;
	im->bmp = d->bmp, im->bw = d->w, im->bh = d->h;
	release(im);
	im->loaded = true;
	arrive(n);
	ak_event e = {.kind = AK_EVENT_LOADED, .x = d->w, .y = d->h};
	wh_emit(n, &e);
	if (d->anim && dsk_anim_player_init(&im->player, d->anim)) {
		im->playing = true;
		dsk_anim_next(&im->player, &im->wait);  // the first frame, as the bitmap shows it
		wh_emit_kind(n, AK_EVENT_ANIMATED);
		if (!im->paused) next_frame(n, im->wait);
	}
	wh_redraw(n);
}

// placeholder_pixels reads PlaceholderImage's bytes (u32 width, u32
// height, RGBA) as a bitmap.
static GpBitmap *placeholder_bitmap(const uint8_t *b, size_t n, int *w, int *h) {
	if (n < 8) return NULL;
	uint32_t pw = wh_u32(b), ph = wh_u32(b + 4);
	if (!pw || !ph || pw > 1 << 14 || ph > 1 << 14 || n < 8 + (size_t)pw * ph * 4) return NULL;
	size_t size = (size_t)pw * ph * 4;
	uint8_t *px = wh_alloc(size);
	memcpy(px, b + 8, size);
	wh_premultiply_rgba(px, size);
	GpBitmap *bmp = g_bitmap((int)pw, (int)ph, px);
	free(px);
	*w = (int)pw, *h = (int)ph;
	return bmp;
}

void wh_set_image_prop(node *n, uint16_t p, const ak_value *v) {
	wh_image *im = n->image;
	if (!im) return;
	switch (p) {
	case AK_PROP_IMAGE_SOURCE:
		free(im->source);
		im->source = wh_strndup(v->s.p, v->s.len);
		im->loaded = false;
		im->dec = NULL;
		stop_playing(im);
		im->bmp = NULL, im->bw = im->bh = 0;
		stop_timer(&im->fade_timer);
		im->fade = 1;
		release(im);
		show_placeholder(n);
		if (*im->source) fetch(n, im->source, false);
		break;
	case AK_PROP_IMAGE_PLACEHOLDER:
		free(im->placeholder);
		im->placeholder = wh_strndup(v->s.p, v->s.len);
		show_placeholder(n);
		break;
	case AK_PROP_IMAGE_PLACEHOLDER_IMAGE:
		if (im->ph == im->hash) im->ph = NULL;
		dispose(&im->hash);
		im->hash = placeholder_bitmap((const uint8_t *)v->s.p, v->s.len, &im->hw, &im->hh);
		show_placeholder(n);
		break;
	case AK_PROP_IMAGE_PLACEHOLDER_MODE:
		im->ph_mode = (int32_t)v->i;
		wh_redraw(n);
		break;
	case AK_PROP_IMAGE_TRANSITION:
		im->trans = v->f;
		break;
	case AK_PROP_IMAGE_PAUSED:
		im->paused = v->i != 0;
		if (im->paused) stop_timer(&im->frame_timer);
		else next_frame(n, im->wait);
		break;
	case AK_PROP_IMAGE_MODE:
		im->mode = (int32_t)v->i;
		wh_redraw(n);
		break;
	}
}

void wh_image_size(node *n, float *w, float *h) {
	wh_image *im = n->image;
	if (im && im->bmp) *w = (float)im->bw, *h = (float)im->bh;
}

void wh_image_tint_changed(node *n) {
	if (n->image) release(n->image);
}

// place is where a bitmap of iw×ih goes in a box of w×h at a mode: the
// part of it drawn (s) and where (d).
static void place(node *n, int32_t mode, float iw, float ih, float w, float h, float d[4], float s[4]) {
	s[0] = s[1] = 0, s[2] = iw, s[3] = ih;
	d[0] = d[1] = 0, d[2] = w, d[3] = h;
	switch (mode) {
	case AK_IMAGE_MODE_FIT: {
		float k = wh_minf(w / iw, h / ih);
		d[2] = iw * k, d[3] = ih * k;
		d[0] = (w - d[2]) / 2, d[1] = (h - d[3]) / 2;
		break;
	}
	case AK_IMAGE_MODE_FILL: {
		float k = wh_maxf(w / iw, h / ih);
		s[2] = w / k, s[3] = h / k;
		s[0] = (iw - s[2]) / 2, s[1] = (ih - s[3]) / 2;
		break;
	}
	case AK_IMAGE_MODE_CENTER: {
		float k = wh_scale_of(n);
		d[2] = iw * k, d[3] = ih * k;
		d[0] = (w - d[2]) / 2, d[1] = (h - d[3]) / 2;
		break;
	}
	}
}

// tinted is the image in n's color, its alpha kept (a template image).
static GpBitmap *tinted(node *n) {
	wh_image *im = n->image;
	if (im->tinted && im->tint == n->color) return im->tinted;
	release(im);
	rgba c;
	if (!wh_resolve(n->color, &c)) return im->bmp;
	const uint8_t *px = im->bmp == im->frame && im->frame ? im->frame_px : im->dec ? im->dec->pixels : NULL;
	if (!px) return im->bmp;
	size_t size = (size_t)im->bw * (size_t)im->bh * 4;
	uint8_t *out = wh_alloc(size);
	for (size_t i = 0; i < size; i += 4) {
		uint32_t a = (uint32_t)px[i + 3] * c.a / 255;
		out[i] = (uint8_t)(c.b * a / 255);
		out[i + 1] = (uint8_t)(c.g * a / 255);
		out[i + 2] = (uint8_t)(c.r * a / 255);
		out[i + 3] = (uint8_t)a;
	}
	im->tinted = g_bitmap(im->bw, im->bh, out);
	im->tint = n->color;
	free(out);
	return im->tinted ? im->tinted : im->bmp;
}

void wh_paint_image(node *n, gfx g, float w, float h, float radius) {
	wh_image *im = n->image;
	if (!im) return;
	GpPath *clip = NULL;
	if (radius > 0) {
		clip = g_path();
		float rr[4];
		wh_radii(n, w, h, rr);  // its corners, each its own
		g_path_corners(clip, 0, 0, w, h, rr);
		g_clip_path(g, clip);
	}
	float d[4], s[4];
	if (im->ph && im->pw > 0 && im->phh > 0 && (!im->loaded || im->fade < 1)) {
		int32_t mode = im->ph_mode >= 0 ? im->ph_mode : im->mode;
		place(n, mode, (float)im->pw, (float)im->phh, w, h, d, s);
		g_draw_image(g, im->ph, im->loaded ? 1 - im->fade : 1, d[0], d[1], d[2], d[3], s[0], s[1], s[2], s[3]);
	}
	if (im->loaded && im->bmp && im->bw > 0 && im->bh > 0) {
		GpBitmap *b = n->color ? tinted(n) : im->bmp;
		place(n, im->mode, (float)im->bw, (float)im->bh, w, h, d, s);
		g_draw_image(g, b, im->fade, d[0], d[1], d[2], d[3], s[0], s[1], s[2], s[3]);
	}
	if (clip) {
		g_reset_clip(g);
		GdipDeletePath(clip);
	}
}
