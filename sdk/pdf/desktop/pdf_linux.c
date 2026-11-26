// pdf.View on Linux: the pages Poppler renders (libpoppler-glib, what
// GNOME's Papers and Evince show PDFs with), one under the other in a
// GtkScrolledWindow, each fitted to the view's width at its own aspect
// ratio, white on the window's background with a gap between them. A
// pinch (GtkGestureZoom) or Ctrl and the wheel zooms from 1x to 4x about
// the point under the fingers or the pointer, and the pages are rendered
// again, sharp, once the zoom settles. Its events: loaded once the
// document is open, pageChange as the user scrolls to another page (the
// one filling most of the view), error (no Poppler, which the view then
// says instead of the pages; a file that isn't there, isn't a PDF or is
// encrypted; a download that failed). It has no size of its own: its
// style gives it one.
//
// The pages are one widget of the document's size (AsukaPdfPages, a
// GtkWidget subclass) that draws only the pages in view, as textures. A
// thread of the view's own opens the document (downloading it first when
// it's a URL) and renders the pages, so a big one never holds up the main
// thread; Poppler's document is only ever touched there. A page's whole
// texture is at most BASE_PX pixels; zoomed in further, the part of it in
// view is rendered again at the zoom's scale over it. The whole pages'
// textures kept are at most CACHE_BYTES, the ones out of view longest
// going first.
//
// Poppler's and cairo's functions are asuka_linux.def's, as GTK's are.
#include "asuka_linux.h"
#include "pdf.asuka.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
	GAP = 12,               // px around the pages and between them
	BASE_PX = 4 << 20,      // a whole page's texture: at most 4 Mpx (16 MB)
	SETTLE_MS = 160,        // how long Ctrl and the wheel rest before the pages are rendered again
};
#define CACHE_BYTES ((size_t)96 << 20)
#define ZOOM_MAX 4.0

// GTK 4's class layout (ABI-stable for the 4.x series), as the host's
// AsukaView sets it (platform/linux/src/view.c).
#if UINTPTR_MAX == 0xffffffffffffffffu
enum { OFF_SIZE_ALLOCATE = 200, OFF_REQUEST_MODE = 224, OFF_MEASURE = 232, OFF_SNAPSHOT = 320 };
#else
enum { OFF_SIZE_ALLOCATE = 100, OFF_REQUEST_MODE = 112, OFF_MEASURE = 116, OFF_SNAPSHOT = 160 };
#endif
#define SET_VFUNC(class, off, fn) (*(void **)((char *)(class) + (off)) = (void *)(fn))

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CAIRO_MEMORY GDK_MEMORY_B8G8R8A8_PREMULTIPLIED  // cairo's ARGB32, in memory
#else
#define CAIRO_MEMORY GDK_MEMORY_A8R8G8B8_PREMULTIPLIED
#endif

static struct {
	bool tried;
	char *err;  // why there are no pages
	GType type;  // AsukaPdfPages
	atomic_int threads;  // the workers still running (a view gone, one ends once it's done what it was doing)
} S;

// poppler loads Poppler's GLib API (asuka_linux.def's POPPLER functions),
// once: false (S.err why) when it isn't there.
static bool poppler(void) {
	if (S.tried) return !S.err;
	S.tried = true;
	if (!asuka_linux() || !G(gtk_widget_add_css_class)) {
		S.err = asuka_strdup("platform/pdf: PDFs on Linux need GTK 4");
		return false;
	}
	if (!asuka_linux_load(ASUKA_LIB_POPPLER)) {
		S.err = asuka_sprintf("platform/pdf: PDFs on Linux need Poppler (libpoppler-glib8): %s isn't installed", asuka_linux_soname(ASUKA_LIB_POPPLER));
		return false;
	}
	if (!G(poppler_document_new_from_file) || !G(poppler_document_get_n_pages) || !G(poppler_document_get_page) || !G(poppler_page_get_size) ||
	    !G(poppler_page_render) || !G(cairo_image_surface_create_for_data) || !G(cairo_format_stride_for_width) || !G(cairo_create) ||
	    !G(cairo_destroy) || !G(cairo_surface_destroy) || !G(cairo_surface_flush) || !G(cairo_scale) || !G(cairo_translate) ||
	    !G(cairo_set_source_rgb) || !G(cairo_paint) || !G(gtk_snapshot_append_scaled_texture) || !G(gdk_memory_texture_new) ||
	    !G(gtk_gesture_zoom_new) || !G(gtk_event_controller_scroll_new) || !G(gtk_scrolled_window_get_vadjustment) ||
	    !G(gtk_widget_get_scale_factor) || !G(g_type_register_static_simple) || !G(g_filename_to_uri)) {
		S.err = asuka_strdup("platform/pdf: this Poppler (or GTK) is too old");
		return false;
	}
	return true;
}

// ---- The worker: the document, opened and rendered off the main thread ----

typedef enum { BASE, DETAIL } kind;

// A render: page's pixels at tw x th for the whole page, of which the
// rectangle at x, y, w x h (the whole page for a BASE).
typedef struct {
	kind kind;
	int page, tw, th, x, y, w, h;
} req;

typedef struct view view;

typedef struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int refs;  // under mu: the view's, the thread's and each result's
	bool quit;
	char *open;     // a source to open, the thread takes it
	uint64_t gen;   // the source's
	req *q;         // the renders wanted, in order
	size_t nq, capq;
	bool busy;
	req cur;  // the one rendering
	view *v;  // the main thread's only: NULL once the view's gone
} worker;

typedef enum { OPENED, FAILED, RENDERED } outcome;

// A result, which the worker hands to the main thread.
typedef struct {
	worker *wk;
	outcome what;
	uint64_t gen;
	int n;           // OPENED: the pages
	double *sizes;   // and their sizes in points, w and h each
	char *message;   // FAILED
	req r;           // RENDERED
	uint8_t *pixels; // cairo's ARGB32, stride bytes a row
	int stride;
} result;

static void worker_unref(worker *wk) {
	pthread_mutex_lock(&wk->mu);
	bool last = --wk->refs == 0;
	pthread_mutex_unlock(&wk->mu);
	if (!last) return;
	pthread_mutex_destroy(&wk->mu);
	pthread_cond_destroy(&wk->cv);
	free(wk->open);
	free(wk->q);
	free(wk);
}

static void deliver(void *p);

// post hands a result to the main thread.
static void post(worker *wk, result *r) {
	pthread_mutex_lock(&wk->mu);
	wk->refs++;
	pthread_mutex_unlock(&wk->mu);
	r->wk = wk;
	asuka_linux_main(deliver, r);
}

static void fail(worker *wk, uint64_t gen, char *message) {
	result *r = calloc(1, sizeof *r);
	r->what = FAILED;
	r->gen = gen;
	r->message = message;
	post(wk, r);
}

// open_doc opens source (a download first when it's a URL): NULL when it
// can't, which it says.
static PopplerDocument *open_doc(worker *wk, const char *source, uint64_t gen) {
	asuka_error err = {0};
	char *path = asuka_desktop_local_file(source, "application/pdf", &err);
	if (!path) {
		fail(wk, gen, asuka_strdup(err.message ? err.message : "the PDF couldn't be read"));
		asuka_error_free(&err);
		return NULL;
	}
	GError *e = NULL;
	char *uri = G(g_filename_to_uri)(path, NULL, &e);
	PopplerDocument *doc = uri ? G(poppler_document_new_from_file)(uri, NULL, &e) : NULL;
	G(g_free)(uri);
	free(path);
	if (!doc) {
		fail(wk, gen, asuka_strdup(e && e->message ? e->message : "it isn't a PDF"));
		if (e) G(g_error_free)(e);
		return NULL;
	}
	int n = G(poppler_document_get_n_pages)(doc);
	if (n <= 0) {
		G(g_object_unref)(doc);
		fail(wk, gen, asuka_strdup("the PDF has no pages"));
		return NULL;
	}
	result *r = calloc(1, sizeof *r);
	r->what = OPENED;
	r->gen = gen;
	r->n = n;
	r->sizes = malloc((size_t)n * 2 * sizeof *r->sizes);
	for (int i = 0; i < n; i++) {
		double w = 0, h = 0;
		PopplerPage *pg = G(poppler_document_get_page)(doc, i);
		if (pg) {
			G(poppler_page_get_size)(pg, &w, &h);
			G(g_object_unref)(pg);
		}
		r->sizes[2 * i] = w > 0 ? w : 612;  // a page Poppler can't size: a Letter page's
		r->sizes[2 * i + 1] = h > 0 ? h : 792;
	}
	post(wk, r);
	return doc;
}

// render renders r of doc: its pixels, white under the page's ink.
static void render(worker *wk, PopplerDocument *doc, req r, uint64_t gen) {
	PopplerPage *pg = G(poppler_document_get_page)(doc, r.page);
	if (!pg) return;
	double pw = 1, ph = 1;
	G(poppler_page_get_size)(pg, &pw, &ph);
	int stride = G(cairo_format_stride_for_width)(CAIRO_FORMAT_ARGB32, r.w);
	uint8_t *px = stride > 0 ? malloc((size_t)stride * (size_t)r.h) : NULL;
	if (!px) {
		G(g_object_unref)(pg);
		return;
	}
	cairo_surface_t *s = G(cairo_image_surface_create_for_data)(px, CAIRO_FORMAT_ARGB32, r.w, r.h, stride);
	cairo_t *cr = G(cairo_create)(s);
	G(cairo_set_source_rgb)(cr, 1, 1, 1);
	G(cairo_paint)(cr);
	G(cairo_translate)(cr, -r.x, -r.y);
	G(cairo_scale)(cr, r.tw / (pw > 0 ? pw : 1), r.th / (ph > 0 ? ph : 1));
	G(poppler_page_render)(pg, cr);
	G(cairo_destroy)(cr);
	G(cairo_surface_flush)(s);
	G(cairo_surface_destroy)(s);
	G(g_object_unref)(pg);
	result *res = calloc(1, sizeof *res);
	res->what = RENDERED;
	res->gen = gen;
	res->r = r;
	res->pixels = px;
	res->stride = stride;
	post(wk, res);
}

static void *work(void *p) {
	worker *wk = p;
	PopplerDocument *doc = NULL;
	uint64_t gen = 0;
	pthread_mutex_lock(&wk->mu);
	for (;;) {
		while (!wk->quit && !wk->open && (wk->nq == 0 || !doc)) pthread_cond_wait(&wk->cv, &wk->mu);
		if (wk->quit) break;
		if (wk->open) {
			char *source = wk->open;
			wk->open = NULL;
			gen = wk->gen;
			pthread_mutex_unlock(&wk->mu);
			if (doc) G(g_object_unref)(doc);
			doc = open_doc(wk, source, gen);
			free(source);
			pthread_mutex_lock(&wk->mu);
			continue;
		}
		req r = wk->q[0];
		memmove(wk->q, wk->q + 1, --wk->nq * sizeof *wk->q);
		wk->busy = true;
		wk->cur = r;
		pthread_mutex_unlock(&wk->mu);
		render(wk, doc, r, gen);
		pthread_mutex_lock(&wk->mu);
		wk->busy = false;
	}
	pthread_mutex_unlock(&wk->mu);
	if (doc) G(g_object_unref)(doc);
	worker_unref(wk);
	atomic_fetch_sub(&S.threads, 1);
	return NULL;
}

static worker *worker_new(void) {
	worker *wk = calloc(1, sizeof *wk);
	pthread_mutex_init(&wk->mu, NULL);
	pthread_cond_init(&wk->cv, NULL);
	wk->refs = 2;  // the view's and the thread's
	pthread_t t;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	atomic_fetch_add(&S.threads, 1);
	bool ok = pthread_create(&t, &a, work, wk) == 0;
	pthread_attr_destroy(&a);
	if (!ok) {
		atomic_fetch_sub(&S.threads, 1);
		wk->refs = 1;
		worker_unref(wk);
		return NULL;
	}
	return wk;
}

static bool same_req(const req *a, const req *b) {
	return a->kind == b->kind && a->page == b->page && a->tw == b->tw && a->th == b->th && a->x == b->x && a->y == b->y && a->w == b->w &&
	       a->h == b->h;
}

// want replaces the renders the worker has to do with rs (less the one
// it's doing, which comes anyway).
static void want(worker *wk, const req *rs, size_t n) {
	pthread_mutex_lock(&wk->mu);
	wk->nq = 0;
	for (size_t i = 0; i < n; i++) {
		if (wk->busy && same_req(&wk->cur, &rs[i])) continue;
		if (wk->nq == wk->capq) {
			wk->capq = wk->capq ? wk->capq * 2 : 8;
			wk->q = realloc(wk->q, wk->capq * sizeof *wk->q);
		}
		wk->q[wk->nq++] = rs[i];
	}
	if (wk->nq) pthread_cond_signal(&wk->cv);
	pthread_mutex_unlock(&wk->mu);
}

// ---- The view ----

typedef struct {
	double w, h;           // in points
	GdkTexture *tex;       // the whole page, tex_w px wide (NULL: none yet)
	int tex_w;
	uint64_t used;         // the draw it was last in, for eviction
	GdkTexture *detail;    // the part in view, sharper, at detail_w px wide for the whole page
	int detail_w;
	double dx, dy, dw, dh; // its rectangle in the page, as fractions of it
} page;

struct view {
	pdf_View base;
	pdf_View_events events;
	int refs;  // the view's, and each handler's, timer's and post's
	GtkWidget *sw, *pages;
	GtkAdjustment *hadj, *vadj;
	worker *wk;
	char *source;
	uint64_t gen;  // the source's
	int n;         // its pages, once open (0: none)
	page *pg;
	double *tops;  // each page's top in the content, then its height (n + 1)
	double laid;   // the content width tops is for
	double zoom;   // 1 to ZOOM_MAX
	double zoom0;  // at the pinch's start
	double px, py; // the pointer, in sw
	guint settle;  // the timer that renders once Ctrl and the wheel rest
	bool zooming;  // a pinch is going on: the pages are rendered once it ends
	int64_t shown;   // the page the app set or the user was told of last
	int64_t wanted;  // the page set before the pages were laid out
	bool has_wanted, setting, reporting, has_scroll, destroyed;
	bool failed;  // the source couldn't be shown: setting it again tries again
	double scroll_x, scroll_y;  // where to scroll once the pages are laid out again
	size_t bytes;   // the whole pages' textures'
	uint64_t clock; // counts draws
};

static view *ref(view *v) {
	v->refs++;
	return v;
}

static void clear_pages(view *v) {
	for (int i = 0; i < v->n; i++) {
		if (v->pg[i].tex) G(g_object_unref)(v->pg[i].tex);
		if (v->pg[i].detail) G(g_object_unref)(v->pg[i].detail);
	}
	free(v->pg);
	free(v->tops);
	v->pg = NULL;
	v->tops = NULL;
	v->n = 0;
	v->laid = 0;
	v->bytes = 0;
}

static void unref(view *v) {
	if (--v->refs) return;
	clear_pages(v);
	free(v->source);
	free(v);
}

static void unref_closure(gpointer data, gpointer closure) { unref(data); }
static void unref_notify(gpointer data) { unref(data); }

// hook connects a handler that holds the view until it's let go.
static void hook(gpointer obj, const char *signal, GCallback cb, view *v) {
	G(g_signal_connect_data)(obj, signal, cb, ref(v), (GClosureNotify)unref_closure, 0);
}

// fit is the pages' width at zoom 1: the viewport's, less the gaps.
static double fit(view *v) {
	GtkWidget *vp = G(gtk_widget_get_parent)(v->pages);
	int w = vp ? G(gtk_widget_get_width)(vp) : 0;
	return fmax(w - 2 * GAP, 1);
}

// content is the content's width at the zoom: the viewport's at 1.
static double content(view *v) { return v->zoom > 1 ? ceil(fit(v) * v->zoom + 2 * GAP) : fit(v) + 2 * GAP; }

// The pages' width in content c wide.
static double page_width(double c) { return fmax(c - 2 * GAP, 1); }

// layout works out the pages' tops for content c wide.
static void layout(view *v, double c) {
	if (!v->n || v->laid == c) return;
	double pw = page_width(c), y = GAP;
	for (int i = 0; i < v->n; i++) {
		v->tops[i] = y;
		y += pw * v->pg[i].h / v->pg[i].w + GAP;
	}
	v->tops[v->n] = y;
	v->laid = c;
}

static double page_height(view *v, int i) { return page_width(v->laid) * v->pg[i].h / v->pg[i].w; }
static double page_left(view *v) { return (G(gtk_widget_get_width)(v->pages) - page_width(v->laid)) / 2; }

// first_below is the first page whose bottom is below y.
static int first_below(view *v, double y) {
	int lo = 0, hi = v->n - 1;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (v->tops[mid] + page_height(v, mid) <= y) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

// in_view is the pages in view: first to last (false: none).
static bool in_view(view *v, int *first, int *last) {
	if (!v->n || v->laid <= 0) return false;
	double top = G(gtk_adjustment_get_value)(v->vadj), h = G(gtk_adjustment_get_page_size)(v->vadj);
	if (h <= 0) return false;
	*first = first_below(v, top);
	*last = *first;
	while (*last + 1 < v->n && v->tops[*last + 1] < top + h) (*last)++;
	return true;
}

// most_shown is the page filling most of the view.
static int most_shown(view *v) {
	int first, last;
	if (!in_view(v, &first, &last)) return 0;
	double top = G(gtk_adjustment_get_value)(v->vadj), bottom = top + G(gtk_adjustment_get_page_size)(v->vadj);
	int best = first;
	double most = -1;
	for (int i = first; i <= last; i++) {
		double seen = fmin(bottom, v->tops[i] + page_height(v, i)) - fmax(top, v->tops[i]);
		if (seen > most) most = seen, best = i;
	}
	return best;
}

// whole works out a page's whole texture's size for its size on the
// screen (tw x th px): that, or less to keep within BASE_PX.
static void whole(int tw, int th, int *bw, int *bh) {
	double area = (double)tw * th;
	if (area <= BASE_PX) {
		*bw = tw, *bh = th;
		return;
	}
	double k = sqrt(BASE_PX / area);
	*bw = (int)fmax(floor(tw * k), 1);
	*bh = (int)fmax(floor(th * k), 1);
}

// schedule asks the worker for what the pages in view lack: their whole
// textures at the scale they're shown at, the part in view sharper where
// that's more than a whole texture can hold, then the pages either side.
static void schedule(view *v) {
	int first, last;
	if (v->destroyed || !v->wk || v->zooming || !in_view(v, &first, &last)) return;
	int sf = G(gtk_widget_get_scale_factor)(v->pages);
	if (sf < 1) sf = 1;
	double pw = page_width(v->laid), left = page_left(v);
	double vx = G(gtk_adjustment_get_value)(v->hadj), vw = G(gtk_adjustment_get_page_size)(v->hadj);
	double vy = G(gtk_adjustment_get_value)(v->vadj), vh = G(gtk_adjustment_get_page_size)(v->vadj);
	req rs[64];
	size_t n = 0;
	int lo = first > 0 ? first - 1 : 0, hi = last + 1 < v->n ? last + 1 : last;
	for (int pass = 0; pass < 3; pass++)
		for (int i = pass == 2 ? lo : first; i <= (pass == 2 ? hi : last) && n < 64; i++) {
			page *p = &v->pg[i];
			double ph = page_height(v, i);
			int tw = (int)ceil(pw * sf), th = (int)ceil(ph * sf), bw, bh;
			whole(tw, th, &bw, &bh);
			bool shown = i >= first && i <= last;
			if (pass != 1) {
				// The whole page: the ones in view first, then those either side.
				if ((pass == 0) != shown || (p->tex && p->tex_w == bw)) continue;
				rs[n++] = (req){BASE, i, bw, bh, 0, 0, bw, bh};
				continue;
			}
			if (bw >= tw) {
				// Sharp whole: no detail.
				if (p->detail) {
					G(g_object_unref)(p->detail);
					p->detail = NULL;
				}
				continue;
			}
			// The part in view (in the page, px at tw), and the part to render:
			// a quarter of the view more each way.
			double x0 = fmax(vx - left, 0), x1 = fmin(vx + vw - left, pw), y0 = fmax(vy - v->tops[i], 0), y1 = fmin(vy + vh - v->tops[i], ph);
			if (x1 <= x0 || y1 <= y0) continue;
			if (p->detail && p->detail_w == tw && p->dx * pw <= x0 + 0.5 && (p->dx + p->dw) * pw >= x1 - 0.5 && p->dy * ph <= y0 + 0.5 &&
			    (p->dy + p->dh) * ph >= y1 - 0.5)
				continue;
			double mx = vw / 4, my = vh / 4;
			int X = (int)floor(fmax(x0 - mx, 0) * sf), Y = (int)floor(fmax(y0 - my, 0) * sf);
			int X1 = (int)fmin(ceil(fmin(x1 + mx, pw) * sf), tw), Y1 = (int)fmin(ceil(fmin(y1 + my, ph) * sf), th);
			if (X1 > X && Y1 > Y) rs[n++] = (req){DETAIL, i, tw, th, X, Y, X1 - X, Y1 - Y};
		}
	// The details of pages gone from view go.
	for (int i = 0; i < v->n; i++)
		if ((i < first || i > last) && v->pg[i].detail) {
			G(g_object_unref)(v->pg[i].detail);
			v->pg[i].detail = NULL;
		}
	want(v->wk, rs, n);
}

// evict lets the whole textures of pages out of view go, those drawn
// longest ago first, until they're within CACHE_BYTES.
static void evict(view *v) {
	int first = 0, last = -1;
	in_view(v, &first, &last);
	while (v->bytes > CACHE_BYTES) {
		int oldest = -1;
		for (int i = 0; i < v->n; i++)
			if (v->pg[i].tex && (i < first || i > last) && (oldest < 0 || v->pg[i].used < v->pg[oldest].used)) oldest = i;
		if (oldest < 0) return;
		page *p = &v->pg[oldest];
		v->bytes -= (size_t)G(gdk_texture_get_width)(p->tex) * (size_t)G(gdk_texture_get_height)(p->tex) * 4;
		G(g_object_unref)(p->tex);
		p->tex = NULL;
	}
}

// report tells the app the page the user scrolled to, once the loop's
// idle.
static void report(void *p) {
	view *v = p;
	v->reporting = false;
	if (!v->destroyed && v->n && !v->has_wanted && v->laid > 0) {
		int64_t now = most_shown(v);
		if (now != v->shown) {
			v->shown = now;
			pdf_View_pageChange(v->events, now);
		}
	}
	unref(v);
}

static void scrolled(GtkAdjustment *a, gpointer data) {
	view *v = data;
	if (v->destroyed || !v->n) return;
	schedule(v);
	// Not while the app's page waits for the pages' layout.
	if (v->setting || v->reporting || v->has_wanted || v->laid <= 0 || most_shown(v) == v->shown) return;
	v->reporting = true;
	asuka_desktop_post(report, ref(v));
}

static void panned(GtkAdjustment *a, gpointer data) { schedule(data); }

// go scrolls to page i's top, as the app asked: nothing's reported.
static void go(view *v, int64_t i) {
	if (i < 0) i = 0;
	if (i >= v->n) i = v->n - 1;
	v->setting = true;
	G(gtk_adjustment_set_value)(v->vadj, v->tops[i] - GAP);
	v->setting = false;
	schedule(v);
}

// zoom_to zooms to z about (ax, ay) in the view: the point of the page
// there stays there.
static void zoom_to(view *v, double z, double ax, double ay) {
	z = fmin(fmax(z, 1), ZOOM_MAX);
	if (!v->n || v->laid <= 0 || fabs(z - v->zoom) < 1e-4) return;
	double vx = G(gtk_adjustment_get_value)(v->hadj), vy = G(gtk_adjustment_get_value)(v->vadj);
	double cx = vx + ax, cy = vy + ay, left = page_left(v), pw = page_width(v->laid);
	int i = first_below(v, cy - GAP);
	double fx = (cx - left) / pw, fy = (cy - v->tops[i]) / page_height(v, i);
	v->zoom = z;
	double c = content(v);
	layout(v, c);
	pw = page_width(c);
	// The content is c wide once it's zoomed in (the viewport's width at 1).
	v->scroll_x = (c - pw) / 2 + fx * pw - ax;
	v->scroll_y = v->tops[i] + fy * page_height(v, i) - ay;
	v->has_scroll = true;
	G(gtk_widget_queue_resize)(v->pages);
}

static gboolean settled(gpointer data) {
	view *v = data;
	v->settle = 0;
	schedule(v);
	return 0;
}

static void zoom_begin(GtkGesture *g, gpointer seq, gpointer data) {
	view *v = data;
	v->zoom0 = v->zoom;
	v->zooming = true;
}

static void zoom_changed(GtkGesture *g, double scale, gpointer data) {
	view *v = data;
	double x = 0, y = 0;
	if (!G(gtk_gesture_get_bounding_box_center)(g, &x, &y)) {
		x = G(gtk_widget_get_width)(v->sw) / 2.0;
		y = G(gtk_widget_get_height)(v->sw) / 2.0;
	}
	zoom_to(v, v->zoom0 * scale, x, y);
}

static void zoom_end(GtkGesture *g, gpointer seq, gpointer data) {
	view *v = data;
	v->zooming = false;
	schedule(v);
}

static void pointer_moved(GtkEventController *c, double x, double y, gpointer data) {
	view *v = data;
	v->px = x;
	v->py = y;
}

// wheel zooms with Ctrl held, about the pointer; without, the scrolled
// window scrolls.
static gboolean wheel(GtkEventController *c, double dx, double dy, gpointer data) {
	view *v = data;
	if (!(G(gtk_event_controller_get_current_event_state)(c) & GDK_CONTROL_MASK) || !v->n) return 0;
	bool smooth = G(gtk_event_controller_scroll_get_unit) && G(gtk_event_controller_scroll_get_unit)((GtkEventControllerScroll *)c) == GDK_SCROLL_UNIT_SURFACE;
	zoom_to(v, v->zoom * (smooth ? exp(-dy / 200) : pow(1.2, -dy)), v->px, v->py);
	if (v->settle) G(g_source_remove)(v->settle);  // its notify lets its reference go
	v->settle = G(g_timeout_add_full)(G_PRIORITY_DEFAULT, SETTLE_MS, settled, ref(v), unref_notify);
	return 1;
}

// ---- AsukaPdfPages: the document's pages, drawn as they come in view ----

static view *pages_view(GtkWidget *w) {
	view *v = G(g_object_get_data)((GObject *)w, "asuka-pdf");
	return v && !v->destroyed ? v : NULL;
}

static GtkSizeRequestMode pages_request_mode(GtkWidget *w) { return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH; }

// measure: as wide as the viewport (nothing) at zoom 1, the zoom's times
// its pages' width zoomed in; as tall as the pages at a width.
static void pages_measure(GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat, int *min_b, int *nat_b) {
	view *v = pages_view(w);
	int s = 0;
	if (v && v->n) {
		if (o == GTK_ORIENTATION_HORIZONTAL) s = v->zoom > 1 ? (int)content(v) : 0;
		else {
			double c = for_size >= 0 ? for_size : v->laid > 0 ? v->laid : fit(v) + 2 * GAP, pw = page_width(c), y = GAP;
			for (int i = 0; i < v->n; i++) y += pw * v->pg[i].h / v->pg[i].w + GAP;
			s = (int)ceil(y);
		}
	}
	*min = *nat = s;
	if (min_b) *min_b = -1;
	if (nat_b) *nat_b = -1;
}

static gboolean relaid(gpointer data) {
	view *v = data;
	if (!v->destroyed) G(gtk_widget_queue_resize)(v->pages);
	return 0;
}

static void pages_allocate(GtkWidget *w, int width, int height, int baseline) {
	view *v = pages_view(w);
	if (!v || !v->n) return;
	// Zoomed in, the content's as wide as the viewport is times the zoom:
	// measured again once the viewport's another width.
	if (v->zoom > 1 && fabs(content(v) - width) >= 1) G(g_idle_add_full)(G_PRIORITY_HIGH_IDLE, relaid, ref(v), unref_notify);
	double before = v->laid;
	int at = before > 0 ? most_shown(v) : 0;
	double frac = before > 0 ? (G(gtk_adjustment_get_value)(v->vadj) - v->tops[at]) / page_height(v, at) : 0;
	layout(v, width);
	if (v->has_scroll) {
		v->has_scroll = false;
		G(gtk_adjustment_set_value)(v->hadj, v->scroll_x);
		G(gtk_adjustment_set_value)(v->vadj, v->scroll_y);
	} else if (v->has_wanted) {
		v->has_wanted = false;
		go(v, v->wanted);
	} else if (before > 0 && before != v->laid) {
		// Another width: the page at the top stays there.
		v->setting = true;
		G(gtk_adjustment_set_value)(v->vadj, v->tops[at] + frac * page_height(v, at));
		v->setting = false;
	}
	schedule(v);
}

static void pages_snapshot(GtkWidget *w, GtkSnapshot *snap) {
	view *v = pages_view(w);
	int first, last;
	if (!v || !in_view(v, &first, &last)) return;
	v->clock++;
	double pw = page_width(v->laid), left = page_left(v);
	GdkRGBA white = {1, 1, 1, 1}, shadow = {0, 0, 0, 0.22f};
	for (int i = first; i <= last; i++) {
		page *p = &v->pg[i];
		double ph = page_height(v, i);
		graphene_rect_t r = {{(float)left, (float)v->tops[i]}, {(float)pw, (float)ph}};
		GskRoundedRect rr;
		G(gsk_rounded_rect_init_from_rect)(&rr, &r, 0);
		G(gtk_snapshot_append_outset_shadow)(snap, &rr, &shadow, 0, 1, 0, 3);
		G(gtk_snapshot_append_color)(snap, &white, &r);
		if (p->tex) {
			p->used = v->clock;
			G(gtk_snapshot_append_scaled_texture)(snap, p->tex, GSK_SCALING_FILTER_LINEAR, &r);
		}
		if (p->detail) {
			graphene_rect_t d = {{(float)(left + p->dx * pw), (float)(v->tops[i] + p->dy * ph)}, {(float)(p->dw * pw), (float)(p->dh * ph)}};
			G(gtk_snapshot_append_scaled_texture)(snap, p->detail, GSK_SCALING_FILTER_LINEAR, &d);
		}
	}
}

static void pages_class_init(gpointer class, gpointer data) {
	SET_VFUNC(class, OFF_REQUEST_MODE, pages_request_mode);
	SET_VFUNC(class, OFF_MEASURE, pages_measure);
	SET_VFUNC(class, OFF_SIZE_ALLOCATE, pages_allocate);
	SET_VFUNC(class, OFF_SNAPSHOT, pages_snapshot);
	G(gtk_widget_class_set_css_name)(class, "asuka-pdf-pages");
}

static GtkWidget *pages_new(view *v) {
	if (!S.type) {
		GType parent = G(gtk_widget_get_type)();
		GTypeQuery q;
		G(g_type_query)(parent, &q);
		S.type = G(g_type_register_static_simple)(parent, "AsukaPdfPages", q.class_size, pages_class_init, q.instance_size, NULL, (GTypeFlags)0);
	}
	GtkWidget *w = G(g_object_new)(S.type, NULL);
	G(g_object_set_data_full)((GObject *)w, "asuka-pdf", ref(v), unref_notify);
	return w;
}

// ---- The worker's results, on the main thread ----

static void opened(view *v, result *r) {
	clear_pages(v);
	v->n = r->n;
	v->pg = calloc((size_t)r->n, sizeof *v->pg);
	v->tops = calloc((size_t)r->n + 1, sizeof *v->tops);
	for (int i = 0; i < r->n; i++) v->pg[i].w = r->sizes[2 * i], v->pg[i].h = r->sizes[2 * i + 1];
	v->zoom = 1;
	// The page the app set, or the first; laid out, it's scrolled to.
	int64_t at = v->has_wanted ? v->wanted : 0;
	if (at >= v->n) at = v->n - 1;
	if (at < 0) at = 0;
	v->wanted = at;
	v->has_wanted = true;
	bool moved = at != v->shown;
	v->shown = at;
	G(gtk_adjustment_set_value)(v->hadj, 0);
	G(gtk_widget_queue_resize)(v->pages);
	pdf_View_loaded(v->events, r->n);
	if (moved && !v->destroyed) pdf_View_pageChange(v->events, at);
}

static void rendered(view *v, result *r) {
	if (r->r.page >= v->n) return;
	page *p = &v->pg[r->r.page];
	GBytes *b = G(g_bytes_new_take)(r->pixels, (size_t)r->stride * (size_t)r->r.h);
	r->pixels = NULL;
	GdkTexture *t = G(gdk_memory_texture_new)(r->r.w, r->r.h, CAIRO_MEMORY, b, (gsize)r->stride);
	G(g_bytes_unref)(b);
	if (r->r.kind == BASE) {
		if (p->tex) {
			v->bytes -= (size_t)G(gdk_texture_get_width)(p->tex) * (size_t)G(gdk_texture_get_height)(p->tex) * 4;
			G(g_object_unref)(p->tex);
		}
		p->tex = t;
		p->tex_w = r->r.tw;
		p->used = v->clock;
		v->bytes += (size_t)r->r.w * (size_t)r->r.h * 4;
		evict(v);
	} else {
		if (p->detail) G(g_object_unref)(p->detail);
		p->detail = t;
		p->detail_w = r->r.tw;
		p->dx = (double)r->r.x / r->r.tw, p->dy = (double)r->r.y / r->r.th;
		p->dw = (double)r->r.w / r->r.tw, p->dh = (double)r->r.h / r->r.th;
	}
	G(gtk_widget_queue_draw)(v->pages);
}

static void deliver(void *p) {
	result *r = p;
	view *v = r->wk->v;
	if (v && !v->destroyed && r->gen == v->gen) {
		switch (r->what) {
		case OPENED: opened(v, r); break;
		case FAILED:
			v->failed = true;
			pdf_View_error(v->events, r->message);
			break;
		case RENDERED: rendered(v, r); break;
		}
	}
	worker_unref(r->wk);
	free(r->sizes);
	free(r->message);
	free(r->pixels);
	free(r);
}

// ---- pdf.View ----

static void emit_error(void *p) {
	view *v = p;
	if (!v->destroyed) pdf_View_error(v->events, S.err);
	unref(v);
}

static void set_source(pdf_View *self, const char *value) {
	view *v = (view *)self;
	if (!v->sw || (!v->failed && !strcmp(v->source ? v->source : "", value))) return;
	v->failed = false;
	free(v->source);
	v->source = asuka_strdup(value);
	v->gen++;
	clear_pages(v);
	G(gtk_widget_queue_resize)(v->pages);
	if (!*value) {
		if (v->wk) want(v->wk, NULL, 0);
		return;
	}
	if (!v->wk) {
		v->wk = worker_new();
		if (!v->wk) {
			pdf_View_error(v->events, "platform/pdf: no thread to open the PDF with");
			return;
		}
		v->wk->v = v;
	}
	pthread_mutex_lock(&v->wk->mu);
	free(v->wk->open);
	v->wk->open = asuka_strdup(value);
	v->wk->gen = v->gen;
	v->wk->nq = 0;
	pthread_cond_signal(&v->wk->cv);
	pthread_mutex_unlock(&v->wk->mu);
}

static void set_page(pdf_View *self, int64_t value) {
	view *v = (view *)self;
	// The page the view shows already (one it reported, coming back
	// bound) moves nothing.
	if (value == v->shown && !v->has_wanted) return;
	v->shown = value;
	if (!v->n || v->laid <= 0) {
		v->wanted = value;
		v->has_wanted = true;
		return;
	}
	v->has_wanted = false;
	go(v, value);
}

// measure: none of its own; its style gives it a size.
static bool measure(pdf_View *self, float w, float h, float *ow, float *oh) {
	*ow = isnan(w) ? 0 : w;
	*oh = isnan(h) ? 0 : h;
	return true;
}

static void destroy(pdf_View *self) {
	view *v = (view *)self;
	v->destroyed = true;
	if (v->settle) G(g_source_remove)(v->settle);
	v->settle = 0;
	if (v->wk) {
		pthread_mutex_lock(&v->wk->mu);
		v->wk->quit = true;
		pthread_cond_signal(&v->wk->cv);
		pthread_mutex_unlock(&v->wk->mu);
		v->wk->v = NULL;
		worker_unref(v->wk);
		v->wk = NULL;
	}
	unref(v);
}

static const pdf_View_vtable vt = {
	.destroy = destroy,
	.set_source = set_source,
	.set_page = set_page,
	.measure = measure,
};

pdf_View *pdf_make_View(pdf_View_events events) {
	view *v = calloc(1, sizeof *v);
	v->base.vt = &vt;
	v->events = events;
	v->refs = 1;
	v->zoom = 1;
	if (!poppler()) {
		if (G(gtk_label_new)) {
			GtkWidget *l = G(gtk_label_new)("PDFs need Poppler (libpoppler-glib8)");
			G(gtk_widget_add_css_class)(l, "dim-label");
			if (G(gtk_label_set_wrap)) G(gtk_label_set_wrap)((GtkLabel *)l, true);
			v->base.widget = l;
		}
		asuka_desktop_post(emit_error, ref(v));
		return &v->base;
	}
	v->sw = G(gtk_scrolled_window_new)();
	v->base.widget = v->sw;
	G(gtk_scrolled_window_set_policy)((GtkScrolledWindow *)v->sw, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	v->pages = pages_new(v);
	G(gtk_scrolled_window_set_child)((GtkScrolledWindow *)v->sw, v->pages);
	v->hadj = G(gtk_scrolled_window_get_hadjustment)((GtkScrolledWindow *)v->sw);
	v->vadj = G(gtk_scrolled_window_get_vadjustment)((GtkScrolledWindow *)v->sw);
	hook(v->vadj, "value-changed", (GCallback)scrolled, v);
	hook(v->hadj, "value-changed", (GCallback)panned, v);
	gpointer zoom = G(gtk_gesture_zoom_new)();
	G(gtk_event_controller_set_propagation_phase)(zoom, GTK_PHASE_CAPTURE);
	hook(zoom, "begin", (GCallback)zoom_begin, v);
	hook(zoom, "scale-changed", (GCallback)zoom_changed, v);
	hook(zoom, "end", (GCallback)zoom_end, v);
	hook(zoom, "cancel", (GCallback)zoom_end, v);
	G(gtk_widget_add_controller)(v->sw, zoom);
	gpointer motion = G(gtk_event_controller_motion_new)();
	hook(motion, "motion", (GCallback)pointer_moved, v);
	G(gtk_widget_add_controller)(v->sw, motion);
	gpointer scroll = G(gtk_event_controller_scroll_new)(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	G(gtk_event_controller_set_propagation_phase)(scroll, GTK_PHASE_CAPTURE);
	hook(scroll, "scroll", (GCallback)wheel, v);
	G(gtk_widget_add_controller)(v->sw, scroll);
	return &v->base;
}
