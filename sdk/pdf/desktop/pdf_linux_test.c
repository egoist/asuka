// pdf_linux_test.c: pdf.View on Linux (platform/desktop's TestStdUnits,
// with GTK under Xvfb): the view's C, included, made and driven in a GTK
// window as the host would, its events caught here instead of sent to a
// core. tests/three.pdf has three pages of 300 x 400 points. Without
// Poppler, the view says so: that's what's checked then.
#define pdf_View_loaded test_loaded
#define pdf_View_pageChange test_page_change
#define pdf_View_error test_error
#include "pdf_linux.c"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

// What each view (by its node) raised.
static struct {
	int64_t loaded;  // its pages, -1 before
	int changes;     // pageChange's
	int64_t page;    // the last one's
	char *error;
} got[8];

void test_loaded(pdf_View_events e, int64_t pages) { got[e.view].loaded = pages; }

void test_page_change(pdf_View_events e, int64_t page) {
	got[e.view].changes++;
	got[e.view].page = page;
}

void test_error(pdf_View_events e, const char *message) {
	free(got[e.view].error);
	got[e.view].error = asuka_strdup(message);
}

static bool timed_out;

static gboolean time_out(gpointer p) {
	timed_out = true;
	return 0;
}

// WAIT runs GTK's loop until cond holds (failing after 30 s of nothing:
// a guard, not what's tested).
#define WAIT(cond)                                                                       \
	do {                                                                                 \
		timed_out = false;                                                               \
		guint t_ = G(g_timeout_add_full)(G_PRIORITY_DEFAULT, 30000, time_out, NULL, NULL); \
		while (!(cond) && !timed_out) G(g_main_context_iteration)(NULL, 1);              \
		if (!timed_out) G(g_source_remove)(t_);                                          \
		if (!(cond)) {                                                                   \
			printf("%s:%d: timed out waiting for %s\n", __FILE__, __LINE__, #cond);      \
			failed = 1;                                                                  \
		}                                                                                \
	} while (0)

// settle runs the loop until it has nothing to do.
static void settle(void) {
	for (int i = 0; i < 1000 && G(g_main_context_iteration)(NULL, 0); i++) {
	}
}

static view *make(uint32_t node, GtkWidget **win, int w, int h) {
	got[node].loaded = -1;
	view *v = (view *)pdf_make_View((pdf_View_events){node, NULL});
	*win = G(gtk_window_new)();
	G(gtk_window_set_default_size)((GtkWindow *)*win, w, h);
	G(gtk_window_set_child)((GtkWindow *)*win, v->base.widget);
	G(gtk_window_present)((GtkWindow *)*win);
	return v;
}

static void unmake(view *v, GtkWidget *win) {
	v->base.vt->destroy(&v->base);
	G(gtk_window_destroy)((GtkWindow *)win);
	settle();
}

static double value(view *v) { return G(gtk_adjustment_get_value)(v->vadj); }
static double bottom(view *v) { return G(gtk_adjustment_get_upper)(v->vadj) - G(gtk_adjustment_get_page_size)(v->vadj); }
static bool laid_out(view *v) { return v->n && v->laid > 0 && !v->has_wanted && G(gtk_adjustment_get_upper)(v->vadj) >= v->tops[v->n] - 1; }

// An HTTP server that answers one request with the file at path.
typedef struct {
	int fd;
	const char *path;
} server;

static void *serve(void *p) {
	server *s = p;
	int c = accept(s->fd, NULL, NULL);
	if (c < 0) return NULL;
	char buf[4096];
	size_t n = 0;
	while (n < sizeof buf - 1) {
		ssize_t r = read(c, buf + n, sizeof buf - 1 - n);
		if (r <= 0) break;
		n += (size_t)r;
		buf[n] = 0;
		if (strstr(buf, "\r\n\r\n")) break;
	}
	FILE *f = fopen(s->path, "rb");
	char body[1 << 16];
	size_t len = f ? fread(body, 1, sizeof body, f) : 0;
	if (f) fclose(f);
	char head[256];
	int hn = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/pdf\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", len);
	if (write(c, head, (size_t)hn) < 0 || write(c, body, len) < 0) printf("serve: write failed\n");
	close(c);
	return NULL;
}

static int listen_on(int *port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	socklen_t al = sizeof a;
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 1) < 0 || getsockname(fd, (struct sockaddr *)&a, &al) < 0) return -1;
	*port = ntohs(a.sin_port);
	return fd;
}

// The document: loaded, laid out to the view's width, rendered; the page
// the app sets scrolled to and never echoed; the user's scrolling told.
static void document(const char *pdf) {
	GtkWidget *win;
	view *v = make(1, &win, 500, 700);
	v->base.vt->set_page(&v->base, 0);
	v->base.vt->set_source(&v->base, pdf);
	WAIT(got[1].loaded >= 0 || got[1].error);
	CHECK(got[1].loaded == 3);
	CHECK(!got[1].error);
	WAIT(laid_out(v));
	CHECK(v->n == 3);
	CHECK(got[1].changes == 0);
	// Fitted to the width, at 3:4, a gap around each.
	int W = G(gtk_widget_get_width)(G(gtk_widget_get_parent)(v->pages));
	double pw = page_width(v->laid);
	CHECK(W > 100);
	CHECK(fabs(v->laid - W) < 1);
	CHECK(fabs(pw - (W - 2 * GAP)) < 1);
	CHECK(fabs(page_height(v, 1) - pw * 4 / 3) < 0.01);
	CHECK(fabs(v->tops[1] - (GAP + pw * 4 / 3 + GAP)) < 0.01);
	CHECK(fabs(G(gtk_adjustment_get_upper)(v->vadj) - ceil(v->tops[3])) < 1);
	CHECK(G(gtk_adjustment_get_upper)(v->hadj) <= W + 0.5);
	// The page in view rendered at its size on the screen.
	WAIT(v->pg[0].tex != NULL);
	int sf = G(gtk_widget_get_scale_factor)(v->pages);
	CHECK(v->pg[0].tex_w == (int)ceil(pw * sf));
	CHECK(G(gdk_texture_get_height)(v->pg[0].tex) == (int)ceil(page_height(v, 0) * sf));

	// setPage(2): page 3 at the top (or as far as it scrolls), nothing told.
	v->base.vt->set_page(&v->base, 2);
	settle();
	CHECK(fabs(value(v) - fmin(v->tops[2] - GAP, bottom(v))) < 1);
	CHECK(most_shown(v) == 2);
	CHECK(got[1].changes == 0);
	WAIT(v->pg[2].tex != NULL);
	// setPage(1): page 2 at the top.
	v->base.vt->set_page(&v->base, 1);
	settle();
	CHECK(fabs(value(v) - fmin(v->tops[1] - GAP, bottom(v))) < 1);
	CHECK(got[1].changes == 0);

	// The user scrolls to the top: pageChange(0), once.
	G(gtk_adjustment_set_value)(v->vadj, 0);
	settle();
	CHECK(got[1].changes == 1 && got[1].page == 0);
	// It comes back bound: nothing moves, nothing's told.
	v->base.vt->set_page(&v->base, 0);
	settle();
	CHECK(value(v) == 0);
	CHECK(got[1].changes == 1);
	// Scrolling within the page tells nothing more.
	G(gtk_adjustment_set_value)(v->vadj, 30);
	settle();
	CHECK(got[1].changes == 1);

	// Zoomed 2x about the view's middle: the point of the page there stays.
	double cw = G(gtk_adjustment_get_page_size)(v->hadj) / 2, ch = G(gtk_adjustment_get_page_size)(v->vadj) / 2;
	G(gtk_adjustment_set_value)(v->vadj, 200);
	settle();
	int i = first_below(v, value(v) + ch - GAP);
	double fx = (G(gtk_adjustment_get_value)(v->hadj) + cw - page_left(v)) / pw, fy = (value(v) + ch - v->tops[i]) / page_height(v, i);
	zoom_to(v, 2, cw, ch);
	WAIT(G(gtk_widget_get_width)(v->pages) == (int)content(v));
	settle();
	double pw2 = page_width(v->laid);
	CHECK(fabs(pw2 - (W - 2 * GAP) * 2) < 1.5);
	CHECK(fabs(G(gtk_adjustment_get_upper)(v->hadj) - content(v)) < 1);
	CHECK(fabs((G(gtk_adjustment_get_value)(v->hadj) + cw - page_left(v)) - fx * pw2) < 1.5);
	CHECK(fabs((value(v) + ch - v->tops[i]) - fy * page_height(v, i)) < 1.5);
	// Rendered again, sharp at 2x.
	int shown = most_shown(v);
	WAIT(v->pg[shown].tex && v->pg[shown].tex_w == (int)ceil(pw2 * sf));

	// 4x: more than a whole page's texture holds, the part in view sharp.
	zoom_to(v, ZOOM_MAX, 0, 0);
	WAIT(G(gtk_widget_get_width)(v->pages) == (int)content(v));
	settle();
	double pw4 = page_width(v->laid);
	int tw = (int)ceil(pw4 * sf), th = (int)ceil(page_height(v, 0) * sf), bw, bh;
	whole(tw, th, &bw, &bh);
	shown = most_shown(v);
	if (bw < tw) {
		WAIT(v->pg[shown].detail && v->pg[shown].detail_w == tw);
		CHECK(v->pg[shown].tex && v->pg[shown].tex_w == bw);
		CHECK((size_t)bw * (size_t)bh <= BASE_PX);
		printf("4x: pages %d px wide, their whole textures %d, the part in view's %dx%d\n", tw, bw, G(gdk_texture_get_width)(v->pg[shown].detail),
		       G(gdk_texture_get_height)(v->pg[shown].detail));
	} else {
		printf("the view is too small for a detail at 4x (%d px): not checked\n", tw);
	}
	// Not past 4x; back to 1x, as wide as the view again.
	zoom_to(v, 9, 0, 0);
	CHECK(v->zoom == ZOOM_MAX);
	zoom_to(v, 1, 0, 0);
	WAIT(G(gtk_widget_get_width)(v->pages) == W);
	settle();
	CHECK(fabs(G(gtk_adjustment_get_upper)(v->hadj) - W) < 1);
	CHECK(v->bytes <= CACHE_BYTES);
	unmake(v, win);
}

// What can't be shown: an error, and no pages.
static void bad(uint32_t node, const char *source, const char *want) {
	GtkWidget *win;
	view *v = make(node, &win, 300, 400);
	v->base.vt->set_source(&v->base, source);
	WAIT(got[node].error || got[node].loaded >= 0);
	CHECK(got[node].loaded == -1);
	CHECK(got[node].error && *got[node].error);
	if (want && got[node].error && !strstr(got[node].error, want)) printf("%s: got %s, want %s in it\n", source, got[node].error, want), failed = 1;
	CHECK(v->n == 0);
	// Set again, it's tried again.
	free(got[node].error);
	got[node].error = NULL;
	v->base.vt->set_source(&v->base, source);
	WAIT(got[node].error != NULL);
	unmake(v, win);
}

// A URL: downloaded, then shown.
static void url(const char *pdf) {
	int port;
	int fd = listen_on(&port);
	CHECK(fd >= 0);
	if (fd < 0) return;
	server s = {fd, pdf};
	pthread_t t;
	pthread_create(&t, NULL, serve, &s);
	char u[64];
	snprintf(u, sizeof u, "http://127.0.0.1:%d/three.pdf", port);
	GtkWidget *win;
	view *v = make(5, &win, 300, 400);
	v->base.vt->set_source(&v->base, u);
	WAIT(got[5].loaded >= 0 || got[5].error);
	CHECK(got[5].loaded == 3);
	if (got[5].error) printf("%s: %s\n", u, got[5].error);
	pthread_join(t, NULL);
	close(fd);
	unmake(v, win);
}

int main(void) {
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("no display: skipped (run under xvfb-run)\n");
		return 0;
	}
	if (!asuka_linux() || !G(gtk_init)) {
		printf("no GTK: skipped\n");
		return 0;
	}
	G(gtk_init)();
	char pdf[PATH_MAX];
	if (!realpath("../tests/three.pdf", pdf)) {
		printf("no tests/three.pdf\n");
		return 1;
	}
	if (!poppler()) {
		// No Poppler: the view says so, and so does its error.
		GtkWidget *win;
		view *v = make(1, &win, 300, 400);
		CHECK(got[1].error && strstr(got[1].error, "need Poppler (libpoppler-glib8)"));
		CHECK(v->base.widget != NULL);
		v->base.vt->set_source(&v->base, pdf);
		unmake(v, win);
		printf("%s: only the view without it checked\n", S.err);
		return failed;
	}
	document(pdf);
	bad(2, "/nonexistent/none.pdf", "no file at");
	// Not a PDF: Poppler's message.
	char text[PATH_MAX];
	snprintf(text, sizeof text, "%s/not.pdf", getenv("ASUKA_DATA") ? getenv("ASUKA_DATA") : ".");
	FILE *f = fopen(text, "w");
	if (f) {
		fputs("This is plain text, not a PDF.\n", f);
		fclose(f);
	}
	bad(3, text, NULL);
	printf("not a PDF: %s\n", got[3].error ? got[3].error : "(none)");
	url(pdf);
	// The workers end before the libraries they call are unloaded.
	WAIT(atomic_load(&S.threads) == 0);
	for (size_t i = 0; i < sizeof got / sizeof *got; i++) free(got[i].error);
	return failed;
}
