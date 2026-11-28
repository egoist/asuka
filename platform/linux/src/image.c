// image.c: images are fetched and decoded off the main thread (GDK's
// loaders: PNG, JPEG, GIF, WebP, AVIF, HEIF… where they're installed;
// http and https by libsoup), kept in memory by source as GDK textures,
// and drawn by the node's view at its mode, clipped to its corners; Color
// tints them (a mask). SVG is read by the core, as on every platform, and
// drawn as vectors (its display list, GSK's paths) at the view's size. Animated GIFs, APNGs and WebPs play
// (platform/desktop's dsk_anim: their frames' pictures by GDK's loaders).
//
// An Image's placeholder (the core's decoded hash, else the Placeholder
// image) is drawn under the image in its own mode until the image
// arrives, which fades in over it on the widget's frame clock
// (Transition; 0.25s over a placeholder). An animation plays frame after
// frame on GLib timeouts while it isn't Paused.
//
// Headers go with an http(s) source's request; Cache says where it's
// kept: Default in memory (the decoded cache, by source) and, fetched over
// http(s), on disk unless its response says no-store; Memory never reads
// or writes the disk; None keeps it nowhere (each Image of it loads it
// again, its own). A decoded image is counted: the cache holds it, and so
// does each Image showing it (its SVG, its animation), so clearing the
// cache (the "image" module's clearCache) lets the images showing keep
// what they show. The module's prefetch loads into the caches as an
// Image would, joining loads in flight.
#include "host.h"

#include <stdatomic.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct decoded decoded;
struct decoded {
	char *src;
	int w, h;
	GdkTexture *texture;
	dsk_anim *anim;
	// SVG: the core's document (core/src/ui/svg.c, read the same on every
	// platform) and its size in points, drawn as vectors at the view's
	// size; its texture, GDK's, only for what takes a texture.
	uint8_t *svg;
	size_t svg_len;
	float sw, sh;
	char *err;
	bool done;
	// Waiting for it: the nodes (by ID) and whether as a placeholder.
	AK_VEC(struct {
		ak_id id;
		bool placeholder;
		lx_texture_fn cb;  // or a callback's
		void *ctx;
	}) waiting;
	// The worker's: the file's bytes and the first frame's pixels.
	GBytes *bytes;
	uint8_t *first;
	// How it's loaded: the request's headers (Headers' encoding), and
	// whether the disk's cache is read and written (Cache Default).
	ak_buf headers;
	bool disk;
	// Who holds it: the cache (in it by its source), the Images showing it.
	bool cached;
	int refs;
};

typedef struct {
	lx_display *d;
	float w, h;
	int32_t mode;
	bool set;
} svg_drawn;

struct image_extra {
	GdkTexture *hash;  // PlaceholderImage's texture
	GdkTexture *ph;    // the placeholder showing
	bool ph_asset;
	int32_t ph_mode;  // PlaceholderMode; -1: the image's
	double trans;
	float fade;  // the image's opacity as it arrives
	int fade_gen;
	bool paused;
	dsk_anim_player player;
	bool playing;
	int frame_gen;
	int wait;  // ms
	char *placeholder;  // the Placeholder image's source
	bool loaded;
	double fade_start, fade_dur;
	guint fade_tick;
	// The image's and the placeholder's SVG (the cache's), and their
	// display lists as last drawn: for a box, a mode and a tint.
	const decoded *svg, *ph_svg;
	svg_drawn drawn, ph_drawn;
	// What it holds: the image and the placeholder it shows.
	decoded *held, *ph_held;
	ak_buf headers;  // Headers, for the loads of the sources that follow
	int32_t cache;   // ImageCache
};

static GHashTable *cache;  // source → decoded

static void decoded_free(decoded *d) {
	free(d->src);
	if (d->texture) g_object_unref(d->texture);
	if (d->anim) dsk_anim_free(d->anim);
	asuka_free(d->svg);
	g_free(d->err);
	ak_vec_free(&d->waiting);
	ak_buf_free(&d->headers);
	free(d);
}

// release_decoded drops a hold on d: gone when nothing holds it.
static void release_decoded(decoded *d) {
	if (!d) return;
	if (d->refs > 0) d->refs--;
	if (!d->refs && !d->cached && d->done) decoded_free(d);
}

static void hold(decoded **slot, decoded *d) {
	if (*slot == d) return;
	if (d) d->refs++;
	decoded *old = *slot;
	*slot = d;
	release_decoded(old);
}

static image_extra *imx(node *n) {
	if (!n->imgx) {
		n->imgx = calloc(1, sizeof *n->imgx);
		if (!n->imgx) abort();
		n->imgx->ph_mode = -1;
		n->imgx->fade = 1;
	}
	return n->imgx;
}

static void set_placeholder(image_extra *x, GdkTexture *t, bool asset) {
	x->ph_asset = asset;
	x->ph_svg = NULL;
	if (x->ph == t) return;
	if (x->ph) g_object_unref(x->ph);
	x->ph = t;
	if (t) g_object_ref(t);
}

void lx_release_image(node *n) {
	image_extra *x = n->imgx;
	if (!x) return;
	x->fade_gen++;
	x->frame_gen++;
	if (x->fade_tick && n->widget) gtk_widget_remove_tick_callback(n->widget, x->fade_tick);
	set_placeholder(x, NULL, false);
	if (x->hash) g_object_unref(x->hash);
	if (x->playing) dsk_anim_player_free(&x->player);
	free(x->placeholder);
	lx_display_free(x->drawn.d);
	lx_display_free(x->ph_drawn.d);
	hold(&x->held, NULL);
	hold(&x->ph_held, NULL);
	ak_buf_free(&x->headers);
	free(x);
	n->imgx = NULL;
}

static void set_texture(node *n, GdkTexture *t) {
	if (n->texture == t) return;
	if (n->texture) g_object_unref(n->texture);
	n->texture = t;
	if (t) g_object_ref(t);
	lx_queue_draw(n);
}

// rgba_texture is a texture (a new reference) of RGBA pixels, not
// premultiplied.
static GdkTexture *rgba_texture(int w, int h, const uint8_t *px) {
	if (w <= 0 || h <= 0 || !px) return NULL;
	GBytes *b = g_bytes_new(px, (gsize)w * (gsize)h * 4);
	GdkTexture *t = gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8, b, (gsize)w * 4);
	g_bytes_unref(b);
	return t;
}

// ---- Decoding, off the main thread ----

// still decodes a frame's still file (a PNG, a WebP) with GDK's loaders:
// RGBA, not premultiplied. Any thread.
static bool still(void *ctx, const uint8_t *data, size_t n, uint8_t **rgba, int *w, int *h) {
	(void)ctx;
	GBytes *b = g_bytes_new(data, n);
	GError *err = NULL;
	GdkTexture *t = gdk_texture_new_from_bytes(b, &err);
	g_bytes_unref(b);
	if (err) g_error_free(err);
	if (!t) return false;
	*w = gdk_texture_get_width(t);
	*h = gdk_texture_get_height(t);
	size_t stride = (size_t)*w * 4;
	uint8_t *px = malloc(stride * (size_t)(*h ? *h : 1));
	if (!px) {  // a frame too big to hold: the animation isn't one
		g_object_unref(t);
		return false;
	}
	gdk_texture_download(t, px, stride);  // B, G, R, A, premultiplied
	g_object_unref(t);
	for (size_t i = 0; i < stride * (size_t)*h; i += 4) {
		uint8_t bl = px[i], g = px[i + 1], r = px[i + 2], a = px[i + 3];
		if (a && a != 255) {
			r = (uint8_t)(r * 255u / a);
			g = (uint8_t)(g * 255u / a);
			bl = (uint8_t)(bl * 255u / a);
		}
		px[i] = r;
		px[i + 1] = g;
		px[i + 2] = bl;
		px[i + 3] = a;
	}
	*rgba = px;
	return true;
}

// images_dir is where images fetched over http(s) are kept across
// launches: the app's caches' asuka-images (dsk_image_cache_*).
static const char *images_dir(void) {
	static _Atomic(char *) dir;
	char *d = atomic_load(&dir);
	if (d) return d;
	char *made = dsk_join(lx_cache_dir(), "asuka-images"), *none = NULL;
	if (!atomic_compare_exchange_strong(&dir, &none, made)) free(made);  // another thread's first
	return atomic_load(&dir);
}

// add_headers puts Headers' (u32 count, then name and value in turn,
// u32-length strings) on a request.
static void add_headers(SoupMessage *m, const ak_buf *h) {
	if (!m || !h->len || !soup_message_get_request_headers || !soup_message_headers_append) return;
	SoupMessageHeaders *hd = soup_message_get_request_headers(m);
	ak_reader r = {h->data, h->len, 0, false};
	uint32_t count = ak_read_u32(&r);
	for (uint32_t i = 0; i + 1 < count; i += 2) {
		uint32_t nl, vl;
		const uint8_t *name = ak_read_bytes(&r, &nl);
		const uint8_t *value = ak_read_bytes(&r, &vl);
		if (r.err) break;
		char *ns = strndup((const char *)name, nl), *vs = strndup((const char *)value, vl);
		if (ns && vs && *ns) soup_message_headers_append(hd, ns, vs);
		free(ns);
		free(vs);
	}
}

// read_source reads an image's bytes: an http(s) URL with libsoup, its
// request with headers (from the disk's cache when it's there and disk
// says so, kept there when it may be), a file:// URL or a path, an
// asset's (relative to the assets). Any thread.
static GBytes *read_source(const char *src, const ak_buf *headers, bool disk, char **err) {
	if (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8)) {
		size_t kept = 0;
		uint8_t *cached = disk ? dsk_image_cache_read(images_dir(), src, &kept) : NULL;
		if (cached) return g_bytes_new_take(cached, kept);
		if (!soup_session_new) {
			*err = g_strdup_printf("%s: libsoup isn't installed to fetch it", src);
			return NULL;
		}
		SoupSession *s = soup_session_new();
		// A User-Agent, as std/http's (desktop fetch.c): some servers
		// (Wikimedia's) refuse a request with none.
		if (soup_session_set_user_agent) soup_session_set_user_agent(s, "Asuka");
		SoupMessage *m = soup_message_new("GET", src);
		add_headers(m, headers);
		GError *e = NULL;
		GBytes *b = m ? soup_session_send_and_read(s, m, NULL, &e) : NULL;
		guint status = m ? soup_message_get_status(m) : 0;
		if (e) {
			*err = g_strdup_printf("%s: %s", src, e->message);
			g_error_free(e);
		} else if (status != 200) {
			*err = g_strdup_printf("%s: %u %s", src, status, m ? S(soup_message_get_reason_phrase(m)) : "");
			if (b) g_bytes_unref(b);
			b = NULL;
		} else if (b) {
			SoupMessageHeaders *hdrs = soup_message_get_response_headers(m);
			const char *cc = hdrs ? soup_message_headers_get_one(hdrs, "Cache-Control") : NULL;
			gsize n = 0;
			const uint8_t *data = g_bytes_get_data(b, &n);
			if (disk && dsk_image_cache_storable(src, cc)) dsk_image_cache_write(images_dir(), src, data, n);
		}
		if (m) g_object_unref(m);
		g_object_unref(s);
		return b;
	}
	char *path;
	if (!strncmp(src, "file://", 7))
		path = strdup(src + 7);
	else if (src[0] == '/')
		path = strdup(src);
	else
		path = dsk_join(S(H.assets), src);
	size_t n;
	char *data = dsk_read_file(path, &n);
	if (!data) {
		*err = g_strdup_printf("open %s: no such file", path);
		free(path);
		return NULL;
	}
	free(path);
	return g_bytes_new_take(data, n);
}

static gboolean decoded_main(gpointer data);

static gpointer decode_thread(gpointer data) {
	decoded *d = data;
	char *err = NULL;
	GBytes *b = read_source(d->src, &d->headers, d->disk, &err);
	if (!b) {
		d->err = err ? err : g_strdup_printf("%s", "an image that can't be read");
		g_idle_add_full(0, decoded_main, d, NULL);
		return NULL;
	}
	gsize n;
	const uint8_t *p = g_bytes_get_data(b, &n);
	// SVG: the core reads it, as every platform does; GDK's texture of it
	// (librsvg's, where it's installed) only for what wants a texture.
	uint8_t *doc = NULL;
	size_t dn = asuka_svg_parse(p, n, &doc);
	if (dn) {
		asuka_svg_size(doc, dn, &d->sw, &d->sh);
		d->svg = doc, d->svg_len = dn;
		d->w = (int)ceilf(d->sw), d->h = (int)ceilf(d->sh);
		d->texture = gdk_texture_new_from_bytes(b, NULL);
		g_bytes_unref(b);
		g_idle_add_full(0, decoded_main, d, NULL);
		return NULL;
	}
	if (dsk_anim_is_animated(p, n)) {
		d->anim = dsk_anim_decode(p, n, still, NULL);
		dsk_anim_player pl;
		// The canvas is the file's size, which may be more than there's
		// memory for: then it's a still's, GDK's to decode (or refuse).
		if (d->anim && !dsk_anim_player_init(&pl, d->anim)) {
			dsk_anim_free(d->anim);
			d->anim = NULL;
		}
		if (d->anim) {
			dsk_anim_next(&pl, NULL);
			d->first = pl.canvas;
			pl.canvas = NULL;
			dsk_anim_player_free(&pl);
			d->w = d->anim->w;
			d->h = d->anim->h;
			g_bytes_unref(b);
			g_idle_add_full(0, decoded_main, d, NULL);
			return NULL;
		}
	}
	GError *e = NULL;
	d->texture = gdk_texture_new_from_bytes(b, &e);  // GDK's loaders, any thread
	g_bytes_unref(b);
	if (!d->texture) {
		d->err = g_strdup_printf("%s: an image in a format there's no decoder for%s%s", d->src, e ? ": " : "", e ? e->message : "");
	} else {
		d->w = gdk_texture_get_width(d->texture);
		d->h = gdk_texture_get_height(d->texture);
	}
	if (e) g_error_free(e);
	g_idle_add_full(0, decoded_main, d, NULL);
	return NULL;
}

static void loaded(node *n, decoded *d, bool placeholder);

static gboolean decoded_main(gpointer data) {
	decoded *d = data;
	if (d->first) {
		d->texture = rgba_texture(d->w, d->h, d->first);
		free(d->first);
		d->first = NULL;
	}
	d->done = true;
	lx_enter_begin();
	for (uint32_t i = 0; i < d->waiting.len; i++) {
		if (d->waiting.data[i].cb) {
			d->waiting.data[i].cb(d->waiting.data[i].ctx, d->texture, d->err);
			continue;
		}
		node *n = node_get(d->waiting.data[i].id);
		if (n) loaded(n, d, d->waiting.data[i].placeholder);
	}
	d->waiting.len = 0;
	lx_enter_end();
	if (!d->cached && !d->refs) decoded_free(d);  // kept nowhere, shown by none
	return 0;
}

// fetch_for decodes src (once: then from the cache) for n, or for cb, on
// a thread, its request with headers (Headers' encoding), kept as mode
// (ImageCache) says.
static void fetch_for(node *n, const char *src, bool placeholder, lx_texture_fn cb, void *ctx, ak_span headers, int32_t mode) {
	if (!cache) cache = g_hash_table_new(g_str_hash, g_str_equal);
	decoded *d = mode == AK_IMAGE_CACHE_NONE ? NULL : g_hash_table_lookup(cache, src);
	if (d && d->done) {
		if (cb)
			cb(ctx, d->texture, d->err);
		else
			loaded(n, d, placeholder);
		return;
	}
	bool start = !d;
	if (!d) {
		d = calloc(1, sizeof *d);
		if (!d) abort();
		d->src = strdup(src);
		ak_buf_put(&d->headers, headers.p, headers.len);
		d->disk = mode == AK_IMAGE_CACHE_DEFAULT;
		if (mode != AK_IMAGE_CACHE_NONE) {
			d->cached = true;
			g_hash_table_insert(cache, d->src, d);
		}
	}
	ak_vec_reserve(&d->waiting, 1);
	d->waiting.data[d->waiting.len].id = n ? n->id : 0;
	d->waiting.data[d->waiting.len].placeholder = placeholder;
	d->waiting.data[d->waiting.len].cb = cb;
	d->waiting.data[d->waiting.len].ctx = ctx;
	d->waiting.len++;
	if (start) {
		if (!strncmp(src, "http", 4)) lx_lib_load(ASUKA_LIB_SOUP);  // on the main thread: dlopen once
		g_thread_unref(g_thread_new("asuka-image", decode_thread, d));
	}
}

// fetch loads an Image's source (with its Headers, kept as its Cache
// says), or its placeholder (as any image).
static void fetch(node *n, const char *src, bool placeholder) {
	image_extra *x = n->imgx;
	if (placeholder || !x)
		fetch_for(n, src, placeholder, NULL, NULL, (ak_span){0}, AK_IMAGE_CACHE_DEFAULT);
	else
		fetch_for(n, src, false, NULL, NULL, ak_strn((const char *)x->headers.data, x->headers.len), x->cache);
}

// lx_load_texture decodes an image (a URL, a file, an asset) as Images
// do, from the same cache: cb gets its texture (its first frame, if
// animated) or the error, on the main thread.
void lx_load_texture(const char *src, lx_texture_fn cb, void *ctx) { fetch_for(NULL, src, false, cb, ctx, (ak_span){0}, AK_IMAGE_CACHE_DEFAULT); }

// ---- Showing ----

static void next_frame(node *n, int wait);

static gboolean fade_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
	(void)w;
	(void)clock;
	node *n = data;
	image_extra *x = n->imgx;
	if (!x) return 0;
	float t = (float)((lx_now() - x->fade_start) / x->fade_dur);
	if (t >= 1) {
		x->fade = 1;
		set_placeholder(x, NULL, false);
		x->fade_tick = 0;
	} else {
		x->fade = t;
	}
	lx_queue_draw(n);
	return t < 1;
}

// arrive fades the image in over its placeholder, or from nothing.
static void arrive(node *n) {
	image_extra *x = imx(n);
	double d = x->trans;
	if (d < 0) d = 0;
	if (d == 0 && x->ph) d = 0.25;
	x->fade_gen++;
	if (x->fade_tick && n->widget) gtk_widget_remove_tick_callback(n->widget, x->fade_tick);
	x->fade_tick = 0;
	if (d <= 0 || !n->widget || !gtk_widget_get_mapped(n->widget)) {
		x->fade = 1;
		set_placeholder(x, NULL, false);
		return;
	}
	x->fade = 0;
	x->fade_start = lx_now();
	x->fade_dur = d;
	x->fade_tick = gtk_widget_add_tick_callback(n->widget, fade_tick, n, NULL);
}

static void loaded(node *n, decoded *d, bool placeholder) {
	image_extra *x = imx(n);
	if (placeholder) {
		if (x->loaded || !str_eq(x->placeholder, d->src) || x->hash) return;
	} else if (!str_eq(n->source, d->src)) {
		return;
	}
	if (d->err) {
		if (!placeholder) lx_emit(n, (ak_event){.kind = AK_EVENT_FAILED, .s = ak_cstr(d->err)});
		return;
	}
	if (placeholder) {
		set_placeholder(x, d->texture, true);
		hold(&x->ph_held, d);
		x->ph_svg = d->svg ? d : NULL;
		x->ph_drawn.set = false;
		lx_queue_draw(n);
		return;
	}
	hold(&x->held, d);
	x->svg = d->svg ? d : NULL;
	x->drawn.set = false;
	set_texture(n, d->svg ? NULL : d->texture);
	x->loaded = true;
	arrive(n);
	lx_queue_draw(n);
	lx_emit(n, (ak_event){.kind = AK_EVENT_LOADED, .x = d->svg ? d->sw : d->w, .y = d->svg ? d->sh : d->h});
	if (d->anim) {
		lx_emit(n, (ak_event){.kind = AK_EVENT_ANIMATED});
		if (x->playing) dsk_anim_player_free(&x->player);
		// No memory for another canvas: its first frame stays, still.
		x->playing = dsk_anim_player_init(&x->player, d->anim);
		x->wait = 0;
		if (x->playing && !dsk_anim_next(&x->player, &x->wait)) x->wait = -1;
		if (x->playing && !x->paused) next_frame(n, x->wait);
	}
}

typedef struct {
	ak_id id;
	int gen;
} frame_ref;

static gboolean frame_due(gpointer data) {
	frame_ref *r = data;
	node *n = node_get(r->id);
	image_extra *x = n ? n->imgx : NULL;
	if (x && x->frame_gen == r->gen && x->playing) {
		int d = 0;
		if (dsk_anim_next(&x->player, &d)) {
			GdkTexture *t = rgba_texture(x->player.a->w, x->player.a->h, x->player.canvas);
			set_texture(n, t);
			if (t) g_object_unref(t);  // set_texture holds it
			x->wait = d;
			next_frame(n, d);
		}
	}
	free(r);
	return 0;
}

// next_frame shows the animation's next frame after wait, and the one
// after it after its time, while it plays.
static void next_frame(node *n, int wait) {
	image_extra *x = imx(n);
	x->frame_gen++;
	if (!x->playing || x->paused || !x->loaded || wait < 0) return;  // < 0: played its loops
	frame_ref *r = malloc(sizeof *r);
	if (!r) abort();
	r->id = n->id;
	r->gen = x->frame_gen;
	g_timeout_add((guint)(wait > 1 ? wait : 1), frame_due, r);
}

// show_placeholder shows the image's placeholder while it hasn't loaded:
// the hash's, else the Placeholder image's.
static void show_placeholder(node *n) {
	image_extra *x = imx(n);
	if (x->loaded) return;
	if (x->hash) {
		set_placeholder(x, x->hash, false);
		lx_queue_draw(n);
		return;
	}
	set_placeholder(x, NULL, false);
	lx_queue_draw(n);
	if (!str_empty(x->placeholder)) fetch(n, x->placeholder, true);
}

// hash_texture is PlaceholderImage's texture (NULL: none): u32 width,
// height, then RGBA.
static GdkTexture *hash_texture(ak_span b) {
	if (b.len < 8) return NULL;
	const uint8_t *p = (const uint8_t *)b.p;
	uint32_t w = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	uint32_t h = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
	if (!w || !h || b.len < 8 + (size_t)w * h * 4) return NULL;
	return rgba_texture((int)w, (int)h, p + 8);
}

void lx_set_image_prop(node *n, ak_prop p, const ak_value *v) {
	image_extra *x = imx(n);
	switch (p) {
	case AK_PROP_IMAGE_SOURCE:
		str_set(&n->source, v->s);
		x->loaded = false;
		if (x->playing) {
			dsk_anim_player_free(&x->player);
			x->playing = false;
		}
		x->frame_gen++;
		x->fade_gen++;
		x->fade = 1;
		x->svg = NULL;
		hold(&x->held, NULL);
		set_texture(n, NULL);
		show_placeholder(n);
		if (!str_empty(n->source)) fetch(n, n->source, false);
		break;
	case AK_PROP_IMAGE_HEADERS:
		ak_buf_reset(&x->headers);
		ak_buf_put(&x->headers, v->s.p, v->s.len);
		break;
	case AK_PROP_IMAGE_CACHE:
		x->cache = (int32_t)v->i;
		break;
	case AK_PROP_IMAGE_PLACEHOLDER:
		str_set(&x->placeholder, v->s);
		show_placeholder(n);
		break;
	case AK_PROP_IMAGE_MODE:
		n->image_mode = (int32_t)v->i;
		lx_queue_draw(n);
		break;
	case AK_PROP_IMAGE_PLACEHOLDER_IMAGE:
		if (x->hash) g_object_unref(x->hash);
		x->hash = hash_texture(v->s);
		show_placeholder(n);
		break;
	case AK_PROP_IMAGE_PLACEHOLDER_MODE:
		x->ph_mode = (int32_t)v->i;
		lx_queue_draw(n);
		break;
	case AK_PROP_IMAGE_TRANSITION:
		x->trans = v->f;
		break;
	case AK_PROP_IMAGE_PAUSED:
		x->paused = v->i != 0;
		if (x->paused)
			x->frame_gen++;
		else
			next_frame(n, x->wait);
		break;
	}
}

// draw_texture draws t at mode in bounds, at an opacity, tinted by the
// node's Color if tint.
static void draw_texture(node *n, GtkSnapshot *snap, const graphene_rect_t *b, GdkTexture *t, int32_t mode, float alpha, bool tint) {
	float iw = (float)gdk_texture_get_width(t), ih = (float)gdk_texture_get_height(t);
	if (iw == 0 || ih == 0 || alpha <= 0) return;
	graphene_rect_t r = *b;
	float s;
	switch (mode) {
	case AK_IMAGE_MODE_FIT:
		s = fminf(b->size.width / iw, b->size.height / ih);
		r.size.width = iw * s;
		r.size.height = ih * s;
		break;
	case AK_IMAGE_MODE_FILL:
		s = fmaxf(b->size.width / iw, b->size.height / ih);
		r.size.width = iw * s;
		r.size.height = ih * s;
		break;
	case AK_IMAGE_MODE_CENTER: {
		float k = H.env.scale > 0 ? H.env.scale : 1;
		r.size.width = iw / k;
		r.size.height = ih / k;
		break;
	}
	}
	r.origin.x = (b->size.width - r.size.width) / 2;
	r.origin.y = (b->size.height - r.size.height) / 2;
	if (alpha < 1) gtk_snapshot_push_opacity(snap, alpha);
	GdkRGBA c;
	if (tint && n->color && lx_resolve(n->color, &c)) {
		gtk_snapshot_push_mask(snap, GSK_MASK_MODE_ALPHA);
		gtk_snapshot_append_scaled_texture(snap, t, GSK_SCALING_FILTER_TRILINEAR, &r);
		gtk_snapshot_pop(snap);
		gtk_snapshot_append_color(snap, &c, &r);
		gtk_snapshot_pop(snap);
	} else {
		gtk_snapshot_append_scaled_texture(snap, t, GSK_SCALING_FILTER_TRILINEAR, &r);
	}
	if (alpha < 1) gtk_snapshot_pop(snap);
}

bool lx_image_svg_size(node *n, float *w, float *h) {
	image_extra *x = n->imgx;
	if (!x || !x->loaded || !x->svg) return false;
	*w = x->svg->sw, *h = x->svg->sh;
	return true;
}

// draw_svg draws an SVG in the box at a mode and an opacity: the core's
// commands for it (fitted), read into GSK objects again only when the box
// or the mode change; a tint (not 0) colors it as a template, the color
// masked by all its shape, as a texture's.
static void draw_svg(node *n, GtkSnapshot *snap, const graphene_rect_t *b, const decoded *d, svg_drawn *dr, int32_t mode, float alpha, ak_color tint) {
	if (alpha <= 0) return;
	if (!dr->set || dr->w != b->size.width || dr->h != b->size.height || dr->mode != mode) {
		uint8_t *out = NULL;
		size_t m = asuka_svg_draw(d->svg, d->svg_len, b->size.width, b->size.height, mode, 0, 0, &out);
		if (!dr->d) dr->d = lx_display_new();
		lx_display_set(dr->d, n, out, m);
		asuka_free(out);
		*dr = (svg_drawn){dr->d, b->size.width, b->size.height, mode, true};
	}
	GdkRGBA c;
	bool tinted = tint && lx_resolve(tint, &c);
	if (alpha < 1) gtk_snapshot_push_opacity(snap, alpha);
	if (tinted) gtk_snapshot_push_mask(snap, GSK_MASK_MODE_ALPHA);
	gtk_snapshot_save(snap);
	gtk_snapshot_translate(snap, &b->origin);
	lx_display_draw(dr->d, snap);
	gtk_snapshot_restore(snap);
	if (tinted) {
		gtk_snapshot_pop(snap);
		gtk_snapshot_append_color(snap, &c, b);
		gtk_snapshot_pop(snap);
	}
	if (alpha < 1) gtk_snapshot_pop(snap);
}

// lx_draw_image draws a node's placeholder, then its texture over it
// (fading in as it arrives), each at its mode, clipped to the view's
// corners.
void lx_draw_image(node *n, GtkSnapshot *snap, const graphene_rect_t *b) {
	if (b->size.width == 0 || b->size.height == 0) return;
	image_extra *x = n->imgx;
	GskRoundedRect rr = lx_node_rounded(n, *b);
	gtk_snapshot_push_rounded_clip(snap, &rr);
	bool loaded = x && x->loaded;
	if (x && x->ph_svg && (!loaded || x->fade < 1)) {
		int32_t mode = x->ph_mode >= 0 ? x->ph_mode : n->image_mode;
		draw_svg(n, snap, b, x->ph_svg, &x->ph_drawn, mode, loaded ? 1 - x->fade : 1, x->ph_asset ? n->color : 0);
	} else if (x && x->ph && (!loaded || x->fade < 1)) {
		int32_t mode = x->ph_mode >= 0 ? x->ph_mode : n->image_mode;
		draw_texture(n, snap, b, x->ph, mode, loaded ? 1 - x->fade : 1, x->ph_asset);
	}
	if (x && x->svg && loaded) draw_svg(n, snap, b, x->svg, &x->drawn, n->image_mode, x->fade, n->color);
	else if (n->texture && loaded) draw_texture(n, snap, b, n->texture, n->image_mode, x ? x->fade : 1, true);
	gtk_snapshot_pop(snap);
}

// ---- The "image" module: prefetch, clearCache ----

static void prefetched(void *ctx, GdkTexture *t, const char *err) {
	(void)ctx;
	(void)t;
	(void)err;  // an Image of it says why
}

// prefetch: u32 n, then n u32-length URLs, then the headers (Headers'
// encoding): each loaded into the caches as an Image of it would be
// (joining a load in flight), off the main thread.
static void image_prefetch(ak_span args, reply *r) {
	ak_reader rd = {(const uint8_t *)args.p, args.len, 0, false};
	uint32_t n = ak_read_u32(&rd);
	size_t urls = rd.off;
	for (uint32_t i = 0; i < n && !rd.err; i++) {
		uint32_t l;
		ak_read_bytes(&rd, &l);
	}
	if (rd.err) {
		reply_err(r, "image.prefetch: arguments too short");
		return;
	}
	ak_span headers = ak_strn((const char *)rd.p + rd.off, rd.len - rd.off);
	rd.off = urls;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t l;
		const uint8_t *u = ak_read_bytes(&rd, &l);
		char *src = strndup((const char *)u, l);
		if (src && *src) fetch_for(NULL, src, false, prefetched, NULL, headers, AK_IMAGE_CACHE_DEFAULT);
		free(src);
	}
	reply_ok(r, NULL, 0);
}

static gpointer clear_disk(gpointer data) {
	(void)data;
	dsk_image_cache_trim(images_dir(), 0);  // every file goes
	return NULL;
}

typedef AK_VEC(decoded *) decoded_vec;

// forget takes a decoded image out of the cache, unless it's in flight
// (it lands in the cache), into gone (freed after: its key is its src).
static gboolean forget(gpointer k, gpointer v, gpointer data) {
	(void)k;
	decoded *d = v;
	if (!d->done) return 0;
	d->cached = false;
	ak_vec_push((decoded_vec *)data, d);
	return 1;
}

// clearCache: u8 flags, 1 the memory's (the decoded images no Image is
// loading: those showing keep theirs), 2 the disk's (on a thread).
static void image_clear(ak_span args, reply *r) {
	uint8_t flags = args.len ? (uint8_t)args.p[0] : 3;
	if ((flags & 1) && cache) {
		decoded_vec gone = {0};
		g_hash_table_foreach_remove(cache, forget, &gone);
		for (uint32_t i = 0; i < gone.len; i++)
			if (!gone.data[i]->refs) decoded_free(gone.data[i]);
		ak_vec_free(&gone);
	}
	if (flags & 2) g_thread_unref(g_thread_new("asuka-image-clear", clear_disk, NULL));
	reply_ok(r, NULL, 0);
}

static const method_t image_methods[] = {
	{"image", "prefetch", image_prefetch},
	{"image", "clearCache", image_clear},
};

static void image_modules(void) { lx_register_methods(image_methods, AK_LEN(image_methods)); }

const area lx_area_image = {
	.name = "image",
	.modules = image_modules,
};
