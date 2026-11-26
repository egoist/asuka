// capture.c: a view drawn into an image file (imageedit.capture): the
// "view" module's "capture". GTK renders the view's window by its
// renderer at the scale asked, the view's part of it (what shows there), and
// writes it as a PNG (GDK) or a JPEG (GdkPixbuf, on white) into the app's
// caches. args: u32 the native view, f32 x, y, width, height of what's
// drawn in it (points; a width below 0: all of it), u8 format (0 JPEG,
// 1 PNG), f32 quality, f32 scale (pixels per point; 0: the window's);
// answers the file's path.
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A capture waiting for its window to have a size (one just shown isn't
// laid out yet).
typedef struct {
	reply *r;
	ak_id id;
	float rect[4], quality, scale;
	bool png;
	int tries;
	float last[6];  // the window's size and the view's bounds, at the last try
} capture_t;

static gboolean attempt(gpointer data);

static void capture(ak_span args, reply *r) {
	ak_reader rd = {(const uint8_t *)args.p, args.len, 0, false};
	capture_t *c = calloc(1, sizeof *c);
	c->r = r;
	c->id = ak_read_u32(&rd);
	for (int i = 0; i < 4; i++) c->rect[i] = ak_read_f32(&rd);
	c->png = ak_read_u8(&rd) == 1;
	c->quality = ak_read_f32(&rd);
	c->scale = ak_read_f32(&rd);
	if (rd.err) {
		reply_err(r, "bad arguments");
		free(c);
		return;
	}
	attempt(c);
}

// draw draws the capture once its window has a size: false to try again.
static bool draw(capture_t *c) {
	reply *r = c->r;
	node *n = node_get(c->id);
	float rect[4] = {c->rect[0], c->rect[1], c->rect[2], c->rect[3]}, quality = c->quality, scale = c->scale;
	bool png = c->png;
	GtkWidget *w = n ? n->widget : NULL;
	GtkNative *native = w ? gtk_widget_get_native(w) : NULL;
	if (!w || !native || !gtk_widget_get_mapped(w)) {
		reply_err(r, "capture: the view isn't showing");
		return true;
	}
	float k = scale > 0 ? scale : (float)gtk_widget_get_scale_factor(w);
	// The window as it shows (its paintable: what a widget's alone draws
	// nothing for), its part where the view is.
	GtkWidget *win = (GtkWidget *)native;
	graphene_rect_t at;
	if (!gtk_widget_compute_bounds(w, win, &at)) {
		reply_err(r, "capture: the view isn't showing");
		return true;
	}
	float ww = (float)gtk_widget_get_width(win), wh = (float)gtk_widget_get_height(win);
	// A window just shown is laid out over its first frames: drawn once
	// its size and the view's place hold from one try to the next.
	float now[6] = {ww, wh, at.origin.x, at.origin.y, at.size.width, at.size.height};
	bool steady = ww > 0 && memcmp(now, c->last, sizeof now) == 0;
	memcpy(c->last, now, sizeof now);
	if (!steady && c->tries++ < 60) return false;
	bool whole = rect[2] < 0 || rect[3] < 0;
	if (whole) rect[0] = rect[1] = 0, rect[2] = at.size.width, rect[3] = at.size.height;  // as GTK placed it
	if (rect[2] < 1 || rect[3] < 1) {
		reply_err(r, "capture: the view has no size");
		return true;
	}
	GdkPaintable *paint = gtk_widget_paintable_new(win);
	// At its own size: a window's paintable is its widget's bounds, and
	// what it draws outside them (its frame's shadow) is outside them too,
	// not scaled into them.
	float pw = (float)gdk_paintable_get_intrinsic_width(paint), ph = (float)gdk_paintable_get_intrinsic_height(paint);
	if (pw <= 0 || ph <= 0) pw = ww, ph = wh;
	GtkSnapshot *s = gtk_snapshot_new();
	gtk_snapshot_scale(s, k, k);
	gdk_paintable_snapshot(paint, (GdkSnapshot *)s, pw, ph);
	GskRenderNode *nd = gtk_snapshot_free_to_node(s);
	g_object_unref(paint);
	GskRenderer *rr = gtk_native_get_renderer(native);
	// The widget's origin in the paintable: inside its frame's margin.
	float mx = (pw - ww) / 2, my = (ph - wh) / 2;
	graphene_rect_t vp = {{(mx + at.origin.x + rect[0]) * k, (my + at.origin.y + rect[1]) * k}, {rect[2] * k, rect[3] * k}};
	GdkTexture *t = nd && rr ? gsk_renderer_render_texture(rr, nd, &vp) : NULL;
	if (nd) gsk_render_node_unref(nd);
	if (!t) {
		reply_err(r, "capture: the view couldn't be drawn");
		return true;
	}
	char dir[4096], path[4200];
	snprintf(dir, sizeof dir, "%s/asuka-capture", lx_cache_dir());
	dsk_mkdirs(dir);
	snprintf(path, sizeof path, "%s/%08x%08x.%s", dir, (unsigned)(lx_now() * 1000), (unsigned)rand(), png ? "png" : "jpg");
	bool ok;
	if (png) {
		ok = gdk_texture_save_to_png(t, path);
	} else {
		// GDK's pixels: BGRA, premultiplied; JPEG's: RGB, on white.
		int tw = gdk_texture_get_width(t), th = gdk_texture_get_height(t);
		guchar *px = malloc((size_t)tw * (size_t)th * 4), *rgb = malloc((size_t)tw * (size_t)th * 3);
		gdk_texture_download(t, px, (gsize)tw * 4);
		for (size_t i = 0; i < (size_t)tw * (size_t)th; i++) {
			unsigned a = px[i * 4 + 3];
			for (int c = 0; c < 3; c++) rgb[i * 3 + c] = (guchar)(px[i * 4 + 2 - c] + (255 - a));
		}
		free(px);
		GdkPixbuf *pb = gdk_pixbuf_new_from_data(rgb, GDK_COLORSPACE_RGB, false, 8, tw, th, tw * 3, NULL, NULL);
		char q[8];
		snprintf(q, sizeof q, "%d", (int)(quality * 100 + 0.5f));
		GError *err = NULL;
		ok = pb && gdk_pixbuf_save(pb, path, "jpeg", &err, "quality", q, NULL);
		if (err) g_error_free(err);
		if (pb) g_object_unref(pb);
		free(rgb);
	}
	g_object_unref(t);
	if (ok) reply_str(r, path);
	else reply_err(r, "capture: the image couldn't be written");
	return true;
}

static gboolean attempt(gpointer data) {
	capture_t *c = data;
	if (!draw(c)) {
		g_timeout_add(50, attempt, c);
		return 0; // G_SOURCE_REMOVE
	}
	free(c);
	return 0; // G_SOURCE_REMOVE
}

static const method_t capture_methods[] = {{"view", "capture", capture}};

static void capture_modules(void) { lx_register_methods(capture_methods, AK_LEN(capture_methods)); }

const area lx_area_capture = {
	.name = "capture",
	.modules = capture_modules,
};
