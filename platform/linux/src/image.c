// image.c: images are fetched and decoded off the main thread (GDK's
// loaders: PNG, JPEG, GIF, WebP, AVIF, HEIF, SVG… where they're
// installed; http and https by libsoup), kept in memory by source as GDK
// textures, and drawn by the node's view at its mode, clipped to its
// corners; Color tints them (a mask). Animated GIFs, APNGs and WebPs play
// (platform/desktop's dsk_anim: their frames' pictures by GDK's loaders).
//
// An Image's placeholder (the core's decoded hash, else the Placeholder
// image) is drawn under the image in its own mode until the image
// arrives, which fades in over it on the widget's frame clock
// (Transition; 0.25s over a placeholder). An animation plays frame after
// frame on GLib timeouts while it isn't Paused.
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
};

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
};

static GHashTable *cache;  // source → decoded

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

// read_source reads an image's bytes: an http(s) URL with libsoup (from
// the disk's cache when it's there, kept there when it may be), a
// file:// URL or a path, an asset's (relative to the assets). Any thread.
static GBytes *read_source(const char *src, char **err) {
	if (!strncmp(src, "http://", 7) || !strncmp(src, "https://", 8)) {
		size_t kept = 0;
		uint8_t *cached = dsk_image_cache_read(images_dir(), src, &kept);
		if (cached) return g_bytes_new_take(cached, kept);
		if (!soup_session_new) {
			*err = g_strdup_printf("%s: libsoup isn't installed to fetch it", src);
			return NULL;
		}
		SoupSession *s = soup_session_new();
		SoupMessage *m = soup_message_new("GET", src);
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
			if (dsk_image_cache_storable(src, cc)) dsk_image_cache_write(images_dir(), src, data, n);
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
	GBytes *b = read_source(d->src, &err);
	if (!b) {
		d->err = err ? err : g_strdup_printf("%s", "an image that can't be read");
		g_idle_add_full(0, decoded_main, d, NULL);
		return NULL;
	}
	gsize n;
	const uint8_t *p = g_bytes_get_data(b, &n);
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
	return 0;
}

// fetch decodes src (once: then from the cache) for n, or for cb, on a
// thread.
static void fetch_for(node *n, const char *src, bool placeholder, lx_texture_fn cb, void *ctx) {
	if (!cache) cache = g_hash_table_new(g_str_hash, g_str_equal);
	decoded *d = g_hash_table_lookup(cache, src);
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
		g_hash_table_insert(cache, d->src, d);
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

static void fetch(node *n, const char *src, bool placeholder) { fetch_for(n, src, placeholder, NULL, NULL); }

// lx_load_texture decodes an image (a URL, a file, an asset) as Images
// do, from the same cache: cb gets its texture (its first frame, if
// animated) or the error, on the main thread.
void lx_load_texture(const char *src, lx_texture_fn cb, void *ctx) { fetch_for(NULL, src, false, cb, ctx); }

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
		lx_queue_draw(n);
		return;
	}
	set_texture(n, d->texture);
	x->loaded = true;
	arrive(n);
	lx_emit(n, (ak_event){.kind = AK_EVENT_LOADED, .x = d->w, .y = d->h});
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
		set_texture(n, NULL);
		show_placeholder(n);
		if (!str_empty(n->source)) fetch(n, n->source, false);
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

// lx_draw_image draws a node's placeholder, then its texture over it
// (fading in as it arrives), each at its mode, clipped to the view's
// corners.
void lx_draw_image(node *n, GtkSnapshot *snap, const graphene_rect_t *b) {
	if (b->size.width == 0 || b->size.height == 0) return;
	image_extra *x = n->imgx;
	GskRoundedRect rr = lx_node_rounded(n, *b);
	gtk_snapshot_push_rounded_clip(snap, &rr);
	bool loaded = x && x->loaded;
	if (x && x->ph && (!loaded || x->fade < 1)) {
		int32_t mode = x->ph_mode >= 0 ? x->ph_mode : n->image_mode;
		draw_texture(n, snap, b, x->ph, mode, loaded ? 1 - x->fade : 1, x->ph_asset);
	}
	if (n->texture && loaded) draw_texture(n, snap, b, n->texture, n->image_mode, x ? x->fade : 1, true);
	gtk_snapshot_pop(snap);
}
