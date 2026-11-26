// pdf_windows_test.c: the Windows view (pdf_windows.c) in a real window,
// given tests/three.pdf (three pages, 300×400 points, a big digit on
// each): it's loaded with its 3 pages, page 1 is rendered (dark ink on
// white), setting page 2 scrolls to the third page without telling the
// app it did, the user's scrolling tells it, Ctrl and the wheel zoom in
// and the pages are rendered again at their new size (a part of each,
// once they're too big to render whole), and a file that isn't there and
// one that isn't a PDF each fail. platform/windows' TestStdViews runs it
// on Windows.
//
// Its events come here, not to a core.
#define pdf_View_loaded test_loaded
#define pdf_View_pageChange test_page_change
#define pdf_View_error test_error
#include "pdf_windows.c"

static int failures;

#define CHECK(c, ...)                                     \
	do {                                                  \
		if (!(c)) {                                       \
			printf("%s:%d: %s: ", __FILE__, __LINE__, #c); \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
			failures++;                                   \
		}                                                 \
	} while (0)

// What the views raised, by their node.
static struct {
	int loaded, pages;
	int changes, last_page;
	int errors;
	char error[512];
} got[3];

void test_loaded(pdf_View_events e, int64_t pages) {
	got[e.view].loaded++;
	got[e.view].pages = (int)pages;
}

void test_page_change(pdf_View_events e, int64_t page) {
	got[e.view].changes++;
	got[e.view].last_page = (int)page;
}

void test_error(pdf_View_events e, const char *message) {
	got[e.view].errors++;
	snprintf(got[e.view].error, sizeof got[e.view].error, "%s", message);
}

// pump runs the window's messages until done says so, or a minute goes
// by (a failure).
static bool pump(bool (*done)(void *ctx), void *ctx) {
	ULONGLONG end = GetTickCount64() + 60000;
	while (!done(ctx)) {
		if (GetTickCount64() > end) return false;
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!done(ctx)) MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
	}
	return true;
}

static bool loaded_or_failed(void *ctx) {
	int i = (int)(intptr_t)ctx;
	return got[i].loaded || got[i].errors;
}

// rendered: the page has a render of its whole at its size now (and, for
// one too big for that, of the part shown too).
typedef struct {
	view *v;
	int page;
} which;

static cached *base_of(view *v, int page) {
	if (!laid_out(v)) return NULL;
	key b = base_key(v, page);
	cached *c = find(v, &b);
	return c && c->bmp ? c : NULL;
}

static cached *part_of(view *v, int page) {
	int w, h;
	page_size(v, page, &w, &h);
	for (size_t i = 0; i < v->ncache; i++) {
		cached *c = &v->cache[i];
		if (c->k.page == page && c->bmp && !whole(&c->k) && c->k.w == w && c->k.h == h) return c;
	}
	return NULL;
}

static bool rendered(void *ctx) {
	which *w = ctx;
	return base_of(w->v, w->page) && (!big(w->v, w->page) || part_of(w->v, w->page));
}

// ink is the share of a render's pixels that are dark, and of those
// that are white.
static void ink(cached *c, double *dark, double *white) {
	DIBSECTION ds;
	*dark = *white = 0;
	if (GetObjectW(c->bmp, sizeof ds, &ds) != sizeof ds || !ds.dsBm.bmBits) return;
	const uint8_t *p = ds.dsBm.bmBits;
	size_t n = (size_t)ds.dsBm.bmWidth * (size_t)ds.dsBm.bmHeight, nd = 0, nw = 0;
	for (size_t i = 0; i < n; i++, p += 4) {
		int l = (p[0] + p[1] + p[2]) / 3;
		nd += l < 96;
		nw += l > 240;
	}
	*dark = (double)nd / (double)n;
	*white = (double)nw / (double)n;
}

static pdf_View *make(HWND top, uint32_t node, const char *source) {
	pdf_View *p = pdf_make_View((pdf_View_events){.view = node, .parent = top});
	view *v = (view *)p;
	SetWindowPos(v->hwnd, NULL, 0, 0, 500, 700, SWP_NOZORDER);
	p->vt->set_source(p, source);
	return p;
}

static void wheel(view *v, int notches, WPARAM keys) {
	RECT r;
	GetWindowRect(v->hwnd, &r);
	SendMessageW(v->hwnd, WM_MOUSEWHEEL, MAKEWPARAM(keys, notches * WHEEL_DELTA), MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2));
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);  // all of it, should it crash
	HWND top = CreateWindowExW(0, L"STATIC", L"pdf", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 640, 820, NULL, NULL,
	                           GetModuleHandleW(NULL), NULL);
	char three[MAX_PATH], absent[MAX_PATH], text[MAX_PATH];
	GetFullPathNameA("..\\tests\\three.pdf", MAX_PATH, three, NULL);
	GetFullPathNameA("..\\tests\\absent.pdf", MAX_PATH, absent, NULL);
	GetFullPathNameA("not-a.pdf", MAX_PATH, text, NULL);
	FILE *f = fopen(text, "wb");
	CHECK(f != NULL, "can't write %s", text);
	if (f) {
		fputs("This is text, not a PDF.\n", f);
		fclose(f);
	}

	// Loaded, its pages fitted to the view's width at their aspect ratio.
	pdf_View *p = make(top, 0, three);
	view *v = (view *)p;
	CHECK(pump(loaded_or_failed, (void *)0), "neither loaded nor failed");
	CHECK(got[0].loaded == 1 && got[0].pages == 3 && got[0].errors == 0, "loaded %d times, %d pages; error: %s", got[0].loaded, got[0].pages,
	      got[0].error);
	CHECK(v->n == 3, "%d pages", v->n);
	if (v->n == 3) {
		double m = MARGIN * v->k;
		CHECK(fabs(v->pw - (v->cw - 2 * m)) < 1e-6, "a page is %g wide in a view %d wide", v->pw, v->cw);
		CHECK(fabs(v->heights[0] / v->pw - 400.0 / 300) < 0.01, "a page's aspect ratio: %g", v->heights[0] / v->pw);
		CHECK(v->sy == 0 && got[0].changes == 0, "at %g, %d page changes", v->sy, got[0].changes);
	}

	// The first page, rendered at its size: a dark digit on white.
	which w0 = {v, 0};
	CHECK(pump(rendered, &w0), "page 1 wasn't rendered: %s", got[0].error);
	cached *c = base_of(v, 0);
	if (c) {
		double dark, white;
		ink(c, &dark, &white);
		CHECK(dark > 0.01 && white > 0.5, "page 1: %.3f dark, %.3f white", dark, white);
		int w, h;
		page_size(v, 0, &w, &h);
		CHECK(c->k.w == w && c->k.h == h, "rendered %dx%d for %dx%d", c->k.w, c->k.h, w, h);
	}

	if (v->n != 3) {
		printf("no pages: the rest can't run\n");
		return 1;
	}
	// The app sets page 2: the third page shows, at the top or as far as
	// the pages go, and the app isn't told what it set.
	p->vt->set_page(p, 2);
	CHECK(most(v) == 2, "page %d covers the view most", most(v));
	CHECK(fabs(v->sy - fmin(v->tops[2] - MARGIN * v->k, max_sy(v))) < 1e-6, "scrolled to %g, page 3 at %g (the most %g)", v->sy, v->tops[2],
	      max_sy(v));
	CHECK(got[0].changes == 0, "%d page changes for the app's", got[0].changes);
	double at = v->sy;
	p->vt->set_page(p, 2);  // what its binding sets back
	CHECK(v->sy == at, "set again, it scrolled to %g", v->sy);
	which w2 = {v, 2};
	CHECK(pump(rendered, &w2), "page 3 wasn't rendered");

	// The user scrolls back to the top: told, once, of the first page.
	SendMessageW(v->hwnd, WM_KEYDOWN, VK_HOME, 0);
	CHECK(v->sy == 0, "home: at %g", v->sy);
	CHECK(got[0].changes == 1 && got[0].last_page == 0 && v->page == 0, "%d changes, the last to %d", got[0].changes, got[0].last_page);
	// And down with the wheel, to the second page.
	for (int i = 0; i < 100 && most(v) == 0 && v->sy < max_sy(v); i++) wheel(v, -1, 0);
	CHECK(got[0].changes == 2 && got[0].last_page == 1, "%d changes, the last to %d", got[0].changes, got[0].last_page);
	SendMessageW(v->hwnd, WM_KEYDOWN, VK_HOME, 0);

	// Ctrl and the wheel zoom in around the pointer, and the pages are
	// rendered again, sharp, once it stops.
	int w_before = (int)lround(v->pw);
	wheel(v, 4, MK_CONTROL);
	CHECK(fabs(v->zoom - pow(1.25, 4)) < 1e-6, "zoomed to %g", v->zoom);
	CHECK(v->content_w > v->cw && v->settling, "%g wide in %d, settling %d", v->content_w, v->cw, v->settling);
	CHECK(pump(rendered, &w0), "page 1 wasn't rendered again at %g×", v->zoom);
	c = base_of(v, 0);
	CHECK(c && c->k.w > w_before * 2, "rendered %d wide, at first %d", c ? c->k.w : 0, w_before);
	// All the way in: too big to render whole, the part shown is rendered
	// at its size.
	wheel(v, 8, MK_CONTROL);
	CHECK(v->zoom == MAX_ZOOM, "zoomed to %g", v->zoom);
	CHECK(big(v, 0), "page 1 at 4× is small enough to render whole");
	CHECK(pump(rendered, &w0), "page 1's part wasn't rendered at 4×");
	c = part_of(v, 0);
	if (c) {
		RECT shown;
		seen(v, page_rect(v, 0), &shown);
		RECT both;
		IntersectRect(&both, &c->k.part, &shown);
		CHECK(EqualRect(&both, &shown), "the part rendered doesn't cover what shows");
	}
	SendMessageW(v->hwnd, WM_KEYDOWN, '0', 0);  // without Ctrl: nothing
	CHECK(v->zoom == MAX_ZOOM, "zoomed to %g", v->zoom);
	printf("pdf (windows): %d pages, %zu renders kept (%zu KB)\n", v->n, v->ncache, v->bytes / 1024);

	// A file that isn't there, and one that isn't a PDF.
	pdf_View *q = make(top, 1, absent), *r = make(top, 2, text);
	CHECK(pump(loaded_or_failed, (void *)1), "the missing file neither loaded nor failed");
	CHECK(pump(loaded_or_failed, (void *)2), "the text neither loaded nor failed");
	CHECK(got[1].loaded == 0 && got[1].errors == 1 && !strncmp(got[1].error, "platform/pdf: ", 14), "missing: loaded %d, error %s", got[1].loaded,
	      got[1].error);
	CHECK(got[2].loaded == 0 && got[2].errors == 1 && !strncmp(got[2].error, "platform/pdf: ", 14), "text: loaded %d, error %s", got[2].loaded,
	      got[2].error);
	printf("missing: %s\ntext: %s\n", got[1].error, got[2].error);

	float mw, mh;
	p->vt->measure(p, 500, NAN, &mw, &mh);
	CHECK(mw == 500 && mh == 0, "measured %gx%g", mw, mh);

	// They go as the host lets them: destroy, then their windows.
	pdf_View *all[3] = {p, q, r};
	for (int i = 0; i < 3; i++) {
		HWND hwnd = ((view *)all[i])->hwnd;
		all[i]->vt->destroy(all[i]);
		DestroyWindow(hwnd);
	}
	DestroyWindow(top);
	DeleteFileA(text);
	return failures ? 1 : 0;
}
