// pdf.View on Windows: the pages Windows' own PDF renderer draws
// (Windows.Data.Pdf, WinRT's, in every Windows 10 and later), one under
// the other in a window of the package's, fitted to its width on the
// window's background, each white with a thin border and a soft shadow
// below it, as the PDF viewers of Windows show them. The user scrolls
// with the wheel, the keys, a touchscreen's finger and the thumb at the
// edge (the host's: thin, wider under the mouse, shown while scrolling),
// and zooms from 1× to 4× with Ctrl and the wheel (a precision
// touchpad's pinch comes as that), a touchscreen's pinch, and Ctrl with
// + and - (0: back to the width).
//
// A worker thread of the view's opens the document and renders its
// pages, as WinRT's calls block it; the main thread asks it for the
// pages it shows (the ones next to them after), at the size they're
// drawn, and takes what it renders as messages to the view's window. A
// page bigger than 4 million pixels (zoomed in) is rendered whole at
// that size, which is stretched, and the part of it on screen at its own
// size over it. While a zoom or a resize goes on, what's rendered is
// stretched; once it stops, the pages are rendered again, sharp. What's
// rendered is kept up to 192 MB, the pages drawn least recently going
// first.
//
// An http(s) source is downloaded into the app's caches first, and the
// copy removed as the document goes.
#include "asuka_windows.h"
#include "pdf.asuka.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

enum {
	MSG_DONE = WM_APP + 0x51,  // the worker has results
	TIMER_BARS = 1,            // the scroll thumbs hide
	TIMER_SETTLE = 2,          // a zoom or a resize has stopped
	MARGIN = 12,               // around the pages, in points
	GAP = 10,                  // between them
};
#define BASE_PIXELS 4000000.0           // a page rendered whole has at most
#define CACHE_BYTES ((size_t)192 << 20)  // what's rendered, kept
#define MAX_ZOOM 4.0
#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

// ---- WinRT's PDF renderer, by its interfaces' slots ----

static const GUID iid_document_statics = {0x433A0B5F, 0xC007, 0x4788, {0x90, 0xF2, 0x08, 0x14, 0x3D, 0x92, 0x25, 0x99}};
static const GUID iid_render_options = {0x3C98056F, 0xB7CF, 0x4C29, {0x9A, 0x04, 0x52, 0xD9, 0x02, 0x67, 0xF4, 0x25}};
static const GUID iid_random_access_stream = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};
static const GUID iid_closable = {0x30D5A829, 0x7FA4, 0x4026, {0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E}};

#define DOCUMENT "rc(Windows.Data.Pdf.PdfDocument;{ac7ebedd-80fa-4089-846e-81b77ff5a86c})"

typedef struct {
	float width, height;
} wsize;
typedef struct {
	float x, y, width, height;
} wrect;

typedef HRESULT(STDMETHODCALLTYPE *get_ptr)(void *, void **);
typedef HRESULT(STDMETHODCALLTYPE *get_u32)(void *, UINT32 *);
typedef HRESULT(STDMETHODCALLTYPE *put_u32)(void *, UINT32);

// IPdfDocumentStatics
#define LOAD_FROM_STREAM 8
// IPdfDocument
#define GET_PAGE 6
#define PAGE_COUNT 7
// IPdfPage
#define RENDER_WITH_OPTIONS 7
#define PAGE_SIZE 10
// IPdfPageRenderOptions
#define PUT_SOURCE_RECT 7
#define PUT_DESTINATION_WIDTH 9
#define PUT_DESTINATION_HEIGHT 11
// IAsyncOperation<T>
#define GET_RESULTS 8

// shcore's streams over files and WinRT's streams (Windows 8 and later).
static struct {
	bool tried;
	HRESULT(WINAPI *on_file)(PCWSTR, DWORD, REFIID, void **);
	HRESULT(WINAPI *over)(IUnknown *, REFIID, void **);
} shcore;

static bool streams(void) {
	if (!shcore.tried) {
		shcore.tried = true;
		HMODULE m = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (m) {
			*(FARPROC *)&shcore.on_file = GetProcAddress(m, "CreateRandomAccessStreamOnFile");
			*(FARPROC *)&shcore.over = GetProcAddress(m, "CreateStreamOverRandomAccessStream");
		}
	}
	return shcore.on_file && shcore.over;
}

static void close_winrt(void *obj) {
	void *c = asuka_com_query(obj, &iid_closable);
	if (c) {
		ASUKA_SLOT(c, 6, HRESULT(STDMETHODCALLTYPE *)(void *))(c);  // Close
		asuka_com_release(c);
	}
}

static HBITMAP dib(int w, int h, void **bits) {
	BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0}};
	return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

// failed is a message of what failed and the system's word for hr, of
// malloc's.
static char *failed(HRESULT hr, const char *what) {
	asuka_error e = {0};
	asuka_win_fail(&e, hr, what);
	char *m = asuka_strdup(e.message ? e.message : what);
	asuka_error_free(&e);
	return m;
}

// ---- The worker and what it shares with the view ----

// A render: of a page at a size (pixels), the part of it rendered (all
// of it for a page's whole).
typedef struct {
	int page;
	int w, h;
	RECT part;
} key;

static bool same_key(const key *a, const key *b) { return a->page == b->page && a->w == b->w && a->h == b->h && EqualRect(&a->part, &b->part); }
static bool whole(const key *k) { return k->part.left == 0 && k->part.top == 0 && k->part.right == k->w && k->part.bottom == k->h; }

enum { LOADED, FAILED, RENDERED };

typedef struct result {
	struct result *next;
	int kind;
	uint32_t gen;  // the document's it is
	int n;         // LOADED: its pages, their sizes (DIPs: width, height)
	float *sizes;
	char *message;  // FAILED, and a RENDERED that failed
	key k;          // RENDERED
	HBITMAP bmp;
} result;

typedef struct {
	volatile LONG refs;  // the view's and its worker's
	CRITICAL_SECTION mu;
	CONDITION_VARIABLE cv;
	HWND hwnd;  // the view's window, told of results; NULL once it's gone
	bool quit;
	char *load;  // a source to open ("": none), the document's gen
	uint32_t load_gen;
	uint32_t gen;  // the document the wants are of
	key *want;     // the renders the view wants, the most wanted first
	size_t nwant, capwant;
	bool busy;  // rendering doing
	key doing;
	result *done, **tail;
} shared;

static void free_result(result *r) {
	if (r->bmp) DeleteObject(r->bmp);
	free(r->sizes);
	free(r->message);
	free(r);
}

static void unshare(shared *s) {
	if (InterlockedDecrement(&s->refs) > 0) return;
	for (result *r = s->done, *next; r; r = next) {
		next = r->next;
		free_result(r);
	}
	free(s->load);
	free(s->want);
	DeleteCriticalSection(&s->mu);
	free(s);
}

static void deliver(shared *s, result *r) {
	EnterCriticalSection(&s->mu);
	*s->tail = r;
	s->tail = &r->next;
	HWND hwnd = s->hwnd;
	LeaveCriticalSection(&s->mu);
	if (hwnd) PostMessageW(hwnd, MSG_DONE, 0, 0);
}

// The worker's document.
typedef struct {
	void *doc;     // IPdfDocument
	void *stream;  // the file's IRandomAccessStream
	char *copy;    // a download of its, removed with it
	uint32_t gen;
} document;

static void close_document(document *d) {
	if (d->doc) close_winrt(d->doc);
	asuka_com_release(d->doc);
	if (d->stream) close_winrt(d->stream);
	asuka_com_release(d->stream);
	if (d->copy) {
		wchar_t *w = asuka_wide(d->copy);
		DeleteFileW(w);
		wchar_t *slash = wcsrchr(w, L'\\');
		if (slash) {
			*slash = 0;
			RemoveDirectoryW(w);  // its own folder of the caches
		}
		free(w);
		free(d->copy);
	}
	*d = (document){0};
}

// open opens source as the worker's document: LOADED with its pages'
// sizes, or FAILED with why.
static result *open_document(document *d, const char *source, uint32_t gen) {
	result *r = calloc(1, sizeof *r);
	r->kind = FAILED;
	r->gen = gen;
	asuka_error e = {0};
	char *path = asuka_desktop_local_file(source, "application/pdf", &e);
	if (!path) {
		r->message = asuka_sprintf("platform/pdf: %s", e.message ? e.message : "no such file");
		asuka_error_free(&e);
		return r;
	}
	d->gen = gen;
	if (!strncmp(source, "http://", 7) || !strncmp(source, "https://", 8)) d->copy = asuka_strdup(path);
	void *statics = NULL, *op = NULL;
	HRESULT hr = E_NOTIMPL;
	if (!asuka_winrt() || !streams() || FAILED(hr = asuka_winrt_factory("Windows.Data.Pdf.PdfDocument", &iid_document_statics, &statics))) {
		r->message = failed(hr, "platform/pdf: this Windows has no PDF renderer (Windows.Data.Pdf)");
		goto out;
	}
	wchar_t *wpath = asuka_wide(path);
	hr = shcore.on_file(wpath, 0, &iid_random_access_stream, &d->stream);  // FileAccessMode.Read
	free(wpath);
	if (FAILED(hr)) {
		char *what = asuka_sprintf("platform/pdf: can't read %s", source);
		r->message = failed(hr, what);
		free(what);
		goto out;
	}
	hr = ASUKA_SLOT(statics, LOAD_FROM_STREAM, HRESULT(STDMETHODCALLTYPE *)(void *, void *, void **))(statics, d->stream, &op);
	if (SUCCEEDED(hr)) {
		GUID h = asuka_winrt_iid("pinterface(" ASUKA_WINRT_ASYNC_HANDLER ";" DOCUMENT ")");
		hr = asuka_winrt_wait(op, &h, NULL);
	}
	if (SUCCEEDED(hr)) hr = ASUKA_SLOT(op, GET_RESULTS, get_ptr)(op, &d->doc);
	if (FAILED(hr) || !d->doc) {
		// A password the document needs, which the view has none of.
		bool locked = hr == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) || hr == HRESULT_FROM_WIN32(ERROR_INVALID_PASSWORD);
		char *what = asuka_sprintf(locked ? "platform/pdf: %s is encrypted" : "platform/pdf: %s isn't a PDF Windows can read", source);
		r->message = failed(FAILED(hr) ? hr : E_FAIL, what);
		free(what);
		goto out;
	}
	UINT32 n = 0;
	ASUKA_SLOT(d->doc, PAGE_COUNT, get_u32)(d->doc, &n);
	r->sizes = calloc((size_t)n * 2 + 2, sizeof *r->sizes);
	for (UINT32 i = 0; i < n; i++) {
		void *page = NULL;
		wsize sz = {612, 792};  // a Letter page's, should one not say
		if (SUCCEEDED(ASUKA_SLOT(d->doc, GET_PAGE, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, void **))(d->doc, i, &page)) && page) {
			ASUKA_SLOT(page, PAGE_SIZE, HRESULT(STDMETHODCALLTYPE *)(void *, wsize *))(page, &sz);
			close_winrt(page);
			asuka_com_release(page);
		}
		if (!(sz.width > 0) || !(sz.height > 0)) sz = (wsize){612, 792};
		r->sizes[2 * i] = sz.width;
		r->sizes[2 * i + 1] = sz.height;
	}
	r->kind = LOADED;
	r->n = (int)n;
out:
	asuka_com_release(op);
	asuka_com_release(statics);
	free(path);
	if (r->kind != LOADED) close_document(d);
	return r;
}

// render renders k of the document's page, its pixels decoded with WIC
// into a bitmap: NULL with *hr why it couldn't.
static HBITMAP render(document *d, IWICImagingFactory *wic, const key *k, HRESULT *why) {
	void *page = NULL, *insp = NULL, *opts = NULL, *mem = NULL, *ras = NULL, *action = NULL;
	IStream *st = NULL;
	IWICBitmapDecoder *dec = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *conv = NULL;
	HBITMAP bmp = NULL;
	UINT32 pw = (UINT32)(k->part.right - k->part.left), ph = (UINT32)(k->part.bottom - k->part.top);
	HRESULT hr = wic ? S_OK : E_NOINTERFACE;
	if (SUCCEEDED(hr)) hr = ASUKA_SLOT(d->doc, GET_PAGE, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, void **))(d->doc, (UINT32)k->page, &page);
	wsize sz = {0};
	if (SUCCEEDED(hr)) hr = ASUKA_SLOT(page, PAGE_SIZE, HRESULT(STDMETHODCALLTYPE *)(void *, wsize *))(page, &sz);
	if (SUCCEEDED(hr)) hr = asuka_winrt_activate("Windows.Data.Pdf.PdfPageRenderOptions", &insp);
	if (SUCCEEDED(hr) && !(opts = asuka_com_query(insp, &iid_render_options))) hr = E_NOINTERFACE;
	if (SUCCEEDED(hr)) hr = ASUKA_SLOT(opts, PUT_DESTINATION_WIDTH, put_u32)(opts, pw);
	if (SUCCEEDED(hr)) hr = ASUKA_SLOT(opts, PUT_DESTINATION_HEIGHT, put_u32)(opts, ph);
	if (SUCCEEDED(hr) && !whole(k)) {
		// The part, in the page's own units (its Size's).
		double fx = sz.width / k->w, fy = sz.height / k->h;
		wrect src = {(float)(k->part.left * fx), (float)(k->part.top * fy), (float)(pw * fx), (float)(ph * fy)};
		hr = ASUKA_SLOT(opts, PUT_SOURCE_RECT, HRESULT(STDMETHODCALLTYPE *)(void *, wrect))(opts, src);
	}
	// A PNG, its default (it fails to write a BMP), which WIC decodes.
	if (SUCCEEDED(hr)) hr = asuka_winrt_activate("Windows.Storage.Streams.InMemoryRandomAccessStream", &mem);
	if (SUCCEEDED(hr) && !(ras = asuka_com_query(mem, &iid_random_access_stream))) hr = E_NOINTERFACE;
	if (SUCCEEDED(hr)) hr = ASUKA_SLOT(page, RENDER_WITH_OPTIONS, HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, void **))(page, ras, opts, &action);
	if (SUCCEEDED(hr)) hr = asuka_winrt_wait(action, &asuka_IID_AsyncActionCompletedHandler, NULL);
	if (SUCCEEDED(hr)) hr = shcore.over((IUnknown *)ras, &IID_IStream, (void **)&st);
	if (SUCCEEDED(hr)) {
		LARGE_INTEGER zero = {0};
		hr = IStream_Seek(st, zero, STREAM_SEEK_SET, NULL);
	}
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromStream(wic, st, NULL, WICDecodeMetadataCacheOnDemand, &dec);
	if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(wic, &conv);
	if (SUCCEEDED(hr))
		hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0,
		                                    WICBitmapPaletteTypeCustom);
	UINT fw = 0, fh = 0;
	if (SUCCEEDED(hr)) hr = IWICFormatConverter_GetSize(conv, &fw, &fh);
	if (SUCCEEDED(hr) && (fw != pw || fh != ph)) hr = E_UNEXPECTED;
	void *bits = NULL;
	if (SUCCEEDED(hr) && !(bmp = dib((int)fw, (int)fh, &bits))) hr = E_OUTOFMEMORY;
	if (SUCCEEDED(hr)) hr = IWICFormatConverter_CopyPixels(conv, NULL, fw * 4, fw * fh * 4, bits);
	if (FAILED(hr) && bmp) {
		DeleteObject(bmp);
		bmp = NULL;
	}
	if (conv) IWICFormatConverter_Release(conv);
	if (frame) IWICBitmapFrameDecode_Release(frame);
	if (dec) IWICBitmapDecoder_Release(dec);
	if (st) IStream_Release(st);
	asuka_com_release(action);
	asuka_com_release(ras);
	asuka_com_release(mem);
	asuka_com_release(opts);
	asuka_com_release(insp);
	if (page) close_winrt(page);
	asuka_com_release(page);
	*why = hr;
	return bmp;
}

// work is the view's worker: it opens what the view shows, and renders
// what it wants, the most wanted first, until the view goes.
static void work(void *p) {
	shared *s = p;
	HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);  // the thread's apartment
	IWICImagingFactory *wic = NULL;
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
	document d = {0};
	for (;;) {
		EnterCriticalSection(&s->mu);
		while (!s->quit && !s->load && s->nwant == 0) SleepConditionVariableCS(&s->cv, &s->mu, INFINITE);
		if (s->quit) {
			LeaveCriticalSection(&s->mu);
			break;
		}
		if (s->load) {
			char *source = s->load;
			uint32_t gen = s->load_gen;
			s->load = NULL;
			LeaveCriticalSection(&s->mu);
			close_document(&d);
			if (*source) deliver(s, open_document(&d, source, gen));
			free(source);
			continue;
		}
		key k = s->want[0];
		uint32_t gen = s->gen;
		memmove(s->want, s->want + 1, --s->nwant * sizeof *s->want);
		s->busy = true;
		s->doing = k;
		LeaveCriticalSection(&s->mu);
		if (d.doc && d.gen == gen) {
			result *r = calloc(1, sizeof *r);
			r->kind = RENDERED;
			r->gen = gen;
			r->k = k;
			HRESULT hr = S_OK;
			r->bmp = render(&d, wic, &k, &hr);
			if (!r->bmp) {
				char *what = asuka_sprintf("platform/pdf: page %d can't be rendered", k.page + 1);
				r->message = failed(hr, what);
				free(what);
			}
			deliver(s, r);
		}
		EnterCriticalSection(&s->mu);
		s->busy = false;
		LeaveCriticalSection(&s->mu);
	}
	close_document(&d);
	if (wic) IWICImagingFactory_Release(wic);
	if (SUCCEEDED(com)) CoUninitialize();
	unshare(s);
}

// ---- The view ----

typedef struct {
	key k;
	HBITMAP bmp;  // NULL: it couldn't be rendered
	size_t bytes;
	uint64_t used;
} cached;

typedef struct view {
	pdf_View base;
	pdf_View_events events;
	int refs;  // the view's and its window's
	HWND hwnd;
	shared *s;
	char *source;
	uint32_t gen;  // the document's: a new source, a new one
	// The document: its pages' sizes (DIPs: width, height).
	int n;
	float *sizes;
	bool render_failed;  // said once
	// Where the pages are, in pixels: each one's top and height, a page's
	// width, the content's size, where it's scrolled to.
	double k;
	int cw, ch;
	double pw, *tops, *heights, content_w, content_h;
	double sx, sy, zoom;
	int page;           // the app's last, or the last the user scrolled to
	bool to_page;       // page is to be scrolled to once the pages are laid out
	bool settling;      // a zoom or a resize going on
	cached *cache;      // what's rendered
	size_t ncache, bytes;
	uint64_t use, painted;  // the paint's use, its first
	// The scroll thumbs: shown, under the mouse, dragged (1 the vertical
	// one, 2 the horizontal).
	bool bars, tracking;
	int hot, drag, drag_at;
	double drag_from;
	ULONGLONG gesture_arg;
	POINT gesture;
	HBITMAP back;  // the back buffer
	SIZE back_size;
	bool alone;  // no worker could start
	bool destroyed;
} view;

static void clear_cache(view *v) {
	for (size_t i = 0; i < v->ncache; i++)
		if (v->cache[i].bmp) DeleteObject(v->cache[i].bmp);
	v->ncache = v->bytes = 0;
}

static void unref(view *v) {
	if (--v->refs > 0) return;
	clear_cache(v);
	free(v->cache);
	free(v->sizes);
	free(v->tops);
	free(v->heights);
	free(v->source);
	if (v->back) DeleteObject(v->back);
	unshare(v->s);
	free(v);
}

static double scale(view *v) {
	static UINT(WINAPI * dpi_for)(HWND);
	static bool looked;
	if (!looked) {
		looked = true;
		*(FARPROC *)&dpi_for = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
	}
	UINT dpi = dpi_for && v->hwnd ? dpi_for(v->hwnd) : 0;
	if (!dpi) {
		HDC dc = GetDC(NULL);
		dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
		ReleaseDC(NULL, dc);
	}
	return dpi ? dpi / 96.0 : 1;
}

static int px(view *v, double pt) { return (int)lround(pt * v->k); }

static void invalidate(view *v) {
	if (v->hwnd) InvalidateRect(v->hwnd, NULL, FALSE);
}

static double max_sx(view *v) { return fmax(v->content_w - v->cw, 0); }
static double max_sy(view *v) { return fmax(v->content_h - v->ch, 0); }

static void clamp(view *v) {
	v->sx = fmax(fmin(v->sx, max_sx(v)), 0);
	v->sy = fmax(fmin(v->sy, max_sy(v)), 0);
}

// layout places the pages at the view's size and zoom: each its width
// (the view's but its margins, times the zoom), at its aspect ratio.
static void layout(view *v) {
	RECT r = {0};
	if (v->hwnd) GetClientRect(v->hwnd, &r);
	v->k = scale(v);
	v->cw = r.right;
	v->ch = r.bottom;
	double m = MARGIN * v->k, g = GAP * v->k;
	v->pw = fmax(v->cw - 2 * m, 1) * v->zoom;
	double y = m;
	for (int i = 0; i < v->n; i++) {
		v->tops[i] = y;
		v->heights[i] = v->pw * v->sizes[2 * i + 1] / v->sizes[2 * i];
		y += v->heights[i] + g;
	}
	v->content_h = v->n ? y - g + m : 0;
	v->content_w = v->n ? v->pw + 2 * m : 0;
	clamp(v);
}

static bool laid_out(view *v) { return v->n > 0 && v->cw > 0 && v->ch > 0; }

// page_at is the page at content's y (or the one above the gap it's in).
static int page_at(view *v, double y) {
	int lo = 0, hi = v->n - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (v->tops[mid] <= y) lo = mid;
		else hi = mid - 1;
	}
	return lo;
}

// most is the page covering most of the view (-1: none).
static int most(view *v) {
	if (!laid_out(v)) return -1;
	int best = -1;
	double area = 0;
	for (int i = page_at(v, v->sy); i < v->n && v->tops[i] < v->sy + v->ch; i++) {
		double a = fmin(v->tops[i] + v->heights[i], v->sy + v->ch) - fmax(v->tops[i], v->sy);
		if (a > area) area = a, best = i;
	}
	return best;
}

// A point of a page, and where it is in the view: what stays put as the
// pages are laid out again (a zoom's point, a resize's top).
typedef struct {
	int page;
	double fx, fy, x, y;
} anchor;

static anchor anchor_at(view *v, double x, double y) {
	int i = page_at(v, v->sy + y);
	double m = MARGIN * v->k;
	return (anchor){i, (v->sx + x - m) / v->pw, (v->sy + y - v->tops[i]) / v->heights[i], x, y};
}

static void restore(view *v, anchor a) {
	double m = MARGIN * v->k;
	v->sx = m + a.fx * v->pw - a.x;
	v->sy = v->tops[a.page] + a.fy * v->heights[a.page] - a.y;
	clamp(v);
}

// scroll_to_page shows the page at the top, once the pages are laid out.
static void scroll_to_page(view *v) {
	if (!laid_out(v)) {
		v->to_page = true;
		return;
	}
	v->to_page = false;
	int p = v->page < 0 ? 0 : v->page >= v->n ? v->n - 1 : v->page;
	v->sy = v->tops[p] - MARGIN * v->k;
	clamp(v);
	invalidate(v);
}

static void show_bars(view *v) {
	if (!v->hwnd) return;
	SetTimer(v->hwnd, TIMER_BARS, 1200, NULL);
	if (!v->bars) {
		v->bars = true;
		invalidate(v);
	}
}

// moved follows the user's scrolling or zooming: the thumbs shown, and the
// page covering most of the view, when it's another, told.
static void moved(view *v) {
	show_bars(v);
	invalidate(v);
	int p = most(v);
	if (p < 0 || p == v->page || v->destroyed) return;
	v->page = p;  // first: the app's binding sets it back, at once
	pdf_View_pageChange(v->events, p);
}

static bool scroll_by(view *v, double dx, double dy) {
	if (!laid_out(v)) return false;
	double x = v->sx, y = v->sy;
	v->sx += dx;
	v->sy += dy;
	clamp(v);
	if (x == v->sx && y == v->sy) return false;
	moved(v);
	return true;
}

static void settle(view *v) {
	v->settling = true;
	if (v->hwnd) SetTimer(v->hwnd, TIMER_SETTLE, 150, NULL);
}

// zoom_to zooms to z (1× to 4×) around a point of the view, which stays
// where it is.
static void zoom_to(view *v, double z, POINT p) {
	if (!laid_out(v)) return;
	z = fmax(fmin(z, MAX_ZOOM), 1);
	if (fabs(z - v->zoom) < 1e-9) return;
	anchor a = anchor_at(v, p.x, p.y);
	v->zoom = z;
	layout(v);
	restore(v, a);
	settle(v);
	moved(v);
}

// ---- What's rendered ----

static cached *find(view *v, const key *k) {
	for (size_t i = 0; i < v->ncache; i++)
		if (same_key(&v->cache[i].k, k)) return &v->cache[i];
	return NULL;
}

static void drop(view *v, size_t i) {
	if (v->cache[i].bmp) DeleteObject(v->cache[i].bmp);
	v->bytes -= v->cache[i].bytes;
	v->cache[i] = v->cache[--v->ncache];
}

// keep keeps a render: a page's whole replaces its others (and the parts
// rendered at another size), a part its others; then the least recently
// drawn go, down to the budget, but those the last paint drew.
static void keep(view *v, key k, HBITMAP bmp) {
	for (size_t i = v->ncache; i-- > 0;) {
		const key *o = &v->cache[i].k;
		if (o->page != k.page) continue;
		if (whole(&k) ? whole(o) || o->w != k.w : !whole(o)) drop(v, i);
	}
	v->cache = realloc(v->cache, (v->ncache + 1) * sizeof *v->cache);
	size_t bytes = bmp ? (size_t)(k.part.right - k.part.left) * (size_t)(k.part.bottom - k.part.top) * 4 : 0;
	v->cache[v->ncache++] = (cached){k, bmp, bytes, ++v->use};
	v->bytes += bytes;
	while (v->bytes > CACHE_BYTES) {
		size_t oldest = SIZE_MAX;
		for (size_t i = 0; i < v->ncache; i++)
			if (v->cache[i].bytes && v->cache[i].used < v->painted && (oldest == SIZE_MAX || v->cache[i].used < v->cache[oldest].used)) oldest = i;
		if (oldest == SIZE_MAX) break;
		drop(v, oldest);
	}
}

// page_size is a page's size in pixels, as it's drawn now.
static void page_size(view *v, int i, int *w, int *h) {
	*w = (int)fmax(lround(v->pw), 1);
	*h = (int)fmax(lround(v->heights[i]), 1);
}

// base_key is a page's whole render: at its size, or smaller, at most
// BASE_PIXELS.
static key base_key(view *v, int i) {
	int w, h;
	page_size(v, i, &w, &h);
	double area = (double)w * h;
	if (area > BASE_PIXELS) {
		double f = sqrt(BASE_PIXELS / area);
		w = (int)fmax(floor(w * f), 1);
		h = (int)fmax(floor(h * f), 1);
	}
	return (key){i, w, h, {0, 0, w, h}};
}

static bool big(view *v, int i) {
	int w, h;
	page_size(v, i, &w, &h);
	return (double)w * h > BASE_PIXELS;
}

// page_rect is where a page is in the view (pixels).
static RECT page_rect(view *v, int i) {
	int w, h;
	page_size(v, i, &w, &h);
	int x = (int)lround(MARGIN * v->k - v->sx), y = (int)lround(v->tops[i] - v->sy);
	return (RECT){x, y, x + w, y + h};
}

// seen is what of a page shows in the view, in its own pixels.
static bool seen(view *v, RECT pr, RECT *out) {
	RECT c = {0, 0, v->cw, v->ch}, r;
	if (!IntersectRect(&r, &pr, &c)) return false;
	OffsetRect(&r, -pr.left, -pr.top);
	*out = r;
	return true;
}

static void add_want(key **ws, size_t *n, size_t *cap, key k) {
	for (size_t i = 0; i < *n; i++)
		if (same_key(&(*ws)[i], &k)) return;
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 8;
		*ws = realloc(*ws, *cap * sizeof **ws);
	}
	(*ws)[(*n)++] = k;
}

// want tells the worker what the view wants rendered: the whole of each
// page it shows, at its size (but while a zoom or resize goes on, when
// one's there to stretch), the part shown of one too big for that, and
// then the pages next to those shown.
static void want(view *v) {
	key *ws = NULL;
	size_t n = 0, cap = 0;
	if (laid_out(v)) {
		int first = page_at(v, v->sy), last = first;
		for (int i = first; i < v->n && v->tops[i] < v->sy + v->ch; i++) {
			last = i;
			key b = base_key(v, i);
			bool any = false;
			for (size_t j = 0; j < v->ncache && !any; j++) any = v->cache[j].k.page == i && v->cache[j].bmp && whole(&v->cache[j].k);
			if (!find(v, &b) && (!v->settling || !any)) add_want(&ws, &n, &cap, b);
		}
		for (int i = first; i <= last && !v->settling; i++) {
			RECT shown;
			if (!big(v, i) || !seen(v, page_rect(v, i), &shown)) continue;
			int w, h;
			page_size(v, i, &w, &h);
			bool have = false;
			for (size_t j = 0; j < v->ncache && !have; j++) {
				const key *o = &v->cache[j].k;
				RECT both;
				have = o->page == i && o->w == w && o->h == h && !whole(o) && IntersectRect(&both, &o->part, &shown) && EqualRect(&both, &shown);
			}
			if (have) continue;
			// What shows and a quarter of the view around it, so that a
			// little scrolling needs nothing new.
			RECT part = {shown.left - v->cw / 4, shown.top - v->ch / 4, shown.right + v->cw / 4, shown.bottom + v->ch / 4};
			RECT all = {0, 0, w, h};
			IntersectRect(&part, &part, &all);
			add_want(&ws, &n, &cap, (key){i, w, h, part});
		}
		if (!v->settling) {
			int next[2] = {first - 1, last + 1};
			for (int j = 0; j < 2; j++) {
				if (next[j] < 0 || next[j] >= v->n) continue;
				key b = base_key(v, next[j]);
				if (!find(v, &b)) add_want(&ws, &n, &cap, b);
			}
		}
	}
	shared *s = v->s;
	EnterCriticalSection(&s->mu);
	s->nwant = 0;
	s->gen = v->gen;
	for (size_t i = 0; i < n; i++) {
		if (s->busy && same_key(&s->doing, &ws[i])) continue;  // on its way
		if (s->nwant == s->capwant) {
			s->capwant = s->capwant ? s->capwant * 2 : 8;
			s->want = realloc(s->want, s->capwant * sizeof *s->want);
		}
		s->want[s->nwant++] = ws[i];
	}
	if (s->nwant) WakeConditionVariable(&s->cv);
	LeaveCriticalSection(&s->mu);
	free(ws);
}

// ---- Painting ----

static uint32_t mix(uint32_t rgba, double a) {
	int r = rgba >> 24, g = (rgba >> 16) & 255, b = (rgba >> 8) & 255;
	return (uint32_t)lround(r * (1 - a)) << 24 | (uint32_t)lround(g * (1 - a)) << 16 | (uint32_t)lround(b * (1 - a)) << 8 | 255;
}

static void fill(HDC dc, RECT r, uint32_t rgba) {
	HBRUSH b = CreateSolidBrush(RGB(rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255));
	FillRect(dc, &r, b);
	DeleteObject(b);
}

// blit draws bmp (bw×bh) to dest, stretched to it if it's another size,
// only as far as it's in clip.
static void blit(HDC dc, HDC src, HBITMAP bmp, int bw, int bh, RECT dest, RECT clip) {
	RECT d;
	if (!IntersectRect(&d, &dest, &clip)) return;
	HGDIOBJ old = SelectObject(src, bmp);
	int dw = dest.right - dest.left, dh = dest.bottom - dest.top;
	if (dw == bw && dh == bh) {
		BitBlt(dc, d.left, d.top, d.right - d.left, d.bottom - d.top, src, d.left - dest.left, d.top - dest.top, SRCCOPY);
	} else {
		double fx = (double)bw / dw, fy = (double)bh / dh;
		int x0 = (int)floor((d.left - dest.left) * fx), y0 = (int)floor((d.top - dest.top) * fy);
		int x1 = (int)ceil((d.right - dest.left) * fx), y1 = (int)ceil((d.bottom - dest.top) * fy);
		x1 = x1 > bw ? bw : x1;
		y1 = y1 > bh ? bh : y1;
		// The destination of the source's whole pixels, so that the
		// picture doesn't shift.
		int l = dest.left + (int)floor(x0 / fx), t = dest.top + (int)floor(y0 / fy);
		int r = dest.left + (int)ceil(x1 / fx), b = dest.top + (int)ceil(y1 / fy);
		SetStretchBltMode(dc, HALFTONE);
		SetBrushOrgEx(dc, 0, 0, NULL);
		StretchBlt(dc, l, t, r - l, b - t, src, x0, y0, x1 - x0, y1 - y0, SRCCOPY);
	}
	SelectObject(src, old);
}

// paint_page draws a page: its border and shadow, white, then the best
// of it rendered (stretched while what's at its size comes), and the
// part of it rendered at its size over that.
static void paint_page(view *v, HDC dc, HDC src, int i, uint32_t bg) {
	RECT pr = page_rect(v, i), c = {0, 0, v->cw, v->ch};
	int s = px(v, 1) > 0 ? px(v, 1) : 1;
	fill(dc, (RECT){pr.left - s, pr.top - s, pr.right + s, pr.bottom + s}, mix(bg, 0.13));  // the border
	fill(dc, (RECT){pr.left, pr.bottom + s, pr.right + s, pr.bottom + 2 * s}, mix(bg, 0.08));
	fill(dc, (RECT){pr.left + s, pr.bottom + 2 * s, pr.right + s, pr.bottom + 3 * s}, mix(bg, 0.04));  // the shadow, below
	fill(dc, pr, 0xFFFFFFFF);
	int w = pr.right - pr.left;
	cached *best = NULL;
	for (size_t j = 0; j < v->ncache; j++) {
		cached *e = &v->cache[j];
		if (e->k.page != i || !e->bmp || !whole(&e->k)) continue;
		if (!best || abs(e->k.w - w) < abs(best->k.w - w)) best = e;
	}
	if (best) {
		blit(dc, src, best->bmp, best->k.w, best->k.h, pr, c);
		best->used = ++v->use;
	}
	int h = pr.bottom - pr.top;
	for (size_t j = 0; j < v->ncache; j++) {
		cached *e = &v->cache[j];
		if (e->k.page != i || !e->bmp || whole(&e->k) || e->k.w != w || e->k.h != h) continue;
		RECT at = e->k.part;
		OffsetRect(&at, pr.left, pr.top);
		blit(dc, src, e->bmp, at.right - at.left, at.bottom - at.top, at, c);
		e->used = ++v->use;
	}
}

// thumb is a scroll thumb's box: the host's, 4 points thick (8 under the
// mouse), 2 in from the edge, at least 24 long. false: nothing to scroll.
static bool thumb(view *v, bool horizontal, bool wide, RECT *out) {
	double content = horizontal ? v->content_w : v->content_h, along = horizontal ? v->cw : v->ch, pos = horizontal ? v->sx : v->sy;
	if (content <= along + 0.5 || along <= 0) return false;
	int thick = px(v, wide ? 8 : 4), inset = px(v, 2);
	int length = (int)fmax(along * along / content, px(v, 24));
	int at = (int)((along - length) * pos / (content - along));
	if (horizontal) *out = (RECT){at, v->ch - thick - inset, at + length, v->ch - inset};
	else *out = (RECT){v->cw - thick - inset, at, v->cw - inset, at + length};
	return true;
}

static void paint_bars(view *v, HDC dc) {
	if (!v->bars && !v->drag) return;
	bool dark = asuka_desktop_dark();
	for (int which = 1; which <= 2; which++) {
		bool on = v->hot == which || v->drag == which;
		RECT t;
		if (!thumb(v, which == 2, on, &t)) continue;
		COLORREF c = on ? (dark ? RGB(0xCE, 0xCE, 0xCE) : RGB(0x6E, 0x6E, 0x6E)) : (dark ? RGB(0x9A, 0x9A, 0x9A) : RGB(0x8A, 0x8A, 0x8A));
		HBRUSH b = CreateSolidBrush(c);
		HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
		int r = which == 2 ? t.bottom - t.top : t.right - t.left;
		RoundRect(dc, t.left, t.top, t.right + 1, t.bottom + 1, r, r);
		SelectObject(dc, ob);
		SelectObject(dc, op);
		DeleteObject(b);
	}
}

// paint draws the view into dc, through a back buffer: the window's
// background, the pages it shows, the thumbs; then asks for what's
// missing.
static void paint(view *v, HDC dc) {
	int w = v->cw, h = v->ch;
	if (w <= 0 || h <= 0) return;
	if (!v->back || v->back_size.cx != w || v->back_size.cy != h) {
		if (v->back) DeleteObject(v->back);
		void *bits;
		v->back = dib(w, h, &bits);
		v->back_size = (SIZE){w, h};
		if (!v->back) return;
	}
	HDC mem = CreateCompatibleDC(dc), src = CreateCompatibleDC(dc);
	HGDIOBJ old = SelectObject(mem, v->back);
	uint32_t bg = asuka_desktop_color((uint64_t)1 << 32 | 4) | 255;  // the semantic Background: the window's
	fill(mem, (RECT){0, 0, w, h}, bg);
	v->painted = v->use + 1;
	if (laid_out(v)) {
		double s = 3 * v->k;  // the border and the shadow below a page
		for (int i = page_at(v, v->sy - s); i < v->n && v->tops[i] < v->sy + h + s; i++) paint_page(v, mem, src, i, bg);
	}
	paint_bars(v, mem);
	BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
	SelectObject(mem, old);
	DeleteDC(src);
	DeleteDC(mem);
	want(v);
}

// ---- What the worker sends ----

static void took(view *v, result *r) {
	if (r->gen != v->gen || v->destroyed) return;
	switch (r->kind) {
	case LOADED:
		free(v->sizes);
		free(v->tops);
		free(v->heights);
		v->n = r->n;
		v->sizes = r->sizes;
		r->sizes = NULL;
		v->tops = calloc((size_t)v->n + 1, sizeof *v->tops);
		v->heights = calloc((size_t)v->n + 1, sizeof *v->heights);
		layout(v);
		scroll_to_page(v);
		invalidate(v);
		pdf_View_loaded(v->events, v->n);
		return;
	case FAILED: pdf_View_error(v->events, r->message); return;
	case RENDERED:
		keep(v, r->k, r->bmp);
		r->bmp = NULL;
		invalidate(v);
		if (r->message && !v->render_failed) {
			v->render_failed = true;
			pdf_View_error(v->events, r->message);
		}
		return;
	}
}

static void take(view *v) {
	shared *s = v->s;
	EnterCriticalSection(&s->mu);
	result *r = s->done;
	s->done = NULL;
	s->tail = &s->done;
	LeaveCriticalSection(&s->mu);
	v->refs++;  // an event's handler may let the view go
	for (result *next; r; r = next) {
		next = r->next;
		took(v, r);
		free_result(r);
	}
	unref(v);
}

// ---- Its window's messages ----

static POINT point_of(LPARAM lp) { return (POINT){(short)LOWORD(lp), (short)HIWORD(lp)}; }

static int hot_at(view *v, POINT p) {
	RECT t;
	int reach = px(v, 12);  // the wide thumb and a little more
	if (thumb(v, false, true, &t) && p.x >= v->cw - reach && p.y >= t.top && p.y < t.bottom) return 1;
	if (thumb(v, true, true, &t) && p.y >= v->ch - reach && p.x >= t.left && p.x < t.right) return 2;
	return 0;
}

static UINT wheel_lines(void) {
	UINT lines = 3;
	SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
	return lines == 0 || lines > 100 ? 3 : lines;
}

static bool message(void *ctx, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, LRESULT *result) {
	view *v = ctx;
	*result = 0;
	switch (m) {
	case WM_NCDESTROY:
		EnterCriticalSection(&v->s->mu);
		v->s->hwnd = NULL;
		LeaveCriticalSection(&v->s->mu);
		v->hwnd = NULL;
		unref(v);
		return false;
	case MSG_DONE: take(v); return true;
	case WM_ERASEBKGND: *result = 1; return true;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		paint(v, dc);
		EndPaint(hwnd, &ps);
		return true;
	}
	case WM_SIZE:
	case WM_DPICHANGED_AFTERPARENT: {
		// The page at the top stays there.
		bool was = laid_out(v);
		anchor a = was ? anchor_at(v, 0, 0) : (anchor){0};
		layout(v);
		if (was && laid_out(v)) {
			restore(v, a);
			settle(v);
		}
		if (v->to_page) scroll_to_page(v);
		invalidate(v);
		return m == WM_DPICHANGED_AFTERPARENT;
	}
	case WM_THEMECHANGED:
	case WM_SYSCOLORCHANGE:
	case WM_SETTINGCHANGE: invalidate(v); return false;
	case WM_TIMER:
		if (wp == TIMER_SETTLE) {
			KillTimer(hwnd, TIMER_SETTLE);
			v->settling = false;
			invalidate(v);
		} else if (wp == TIMER_BARS) {
			if (v->hot || v->drag) return true;  // under the mouse: it stays
			KillTimer(hwnd, TIMER_BARS);
			v->bars = false;
			invalidate(v);
		}
		return true;
	case WM_GETDLGCODE: *result = DLGC_WANTARROWS | DLGC_WANTCHARS; return true;
	case WM_MOUSEMOVE: {
		POINT p = point_of(lp);
		if (!v->tracking) {
			TRACKMOUSEEVENT tm = {sizeof tm, TME_LEAVE, hwnd, 0};
			v->tracking = TrackMouseEvent(&tm);
		}
		if (v->drag) {
			bool horizontal = v->drag == 2;
			double content = horizontal ? v->content_w : v->content_h, along = horizontal ? v->cw : v->ch;
			double pos = v->drag_from + ((horizontal ? p.x : p.y) - v->drag_at) * content / fmax(along, 1);
			if (horizontal) scroll_by(v, pos - v->sx, 0);
			else scroll_by(v, 0, pos - v->sy);
			return true;
		}
		int hot = hot_at(v, p);
		if (hot != v->hot) {
			v->hot = hot;
			invalidate(v);
		}
		if (laid_out(v) && (v->content_h > v->ch || v->content_w > v->cw)) show_bars(v);
		return true;
	}
	case WM_MOUSELEAVE:
		v->tracking = false;
		if (v->hot) {
			v->hot = 0;
			invalidate(v);
		}
		return true;
	case WM_LBUTTONDOWN: {
		POINT p = point_of(lp);
		SetFocus(hwnd);
		int hot = hot_at(v, p);
		if (hot) {
			SetCapture(hwnd);
			v->drag = hot;
			v->drag_at = hot == 2 ? p.x : p.y;
			v->drag_from = hot == 2 ? v->sx : v->sy;
			invalidate(v);
		}
		return true;
	}
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		if (v->drag) {
			v->drag = 0;
			if (GetCapture() == hwnd) ReleaseCapture();
			show_bars(v);
			invalidate(v);
		}
		return m == WM_LBUTTONUP;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL: {
		if (!laid_out(v)) return false;
		int delta = GET_WHEEL_DELTA_WPARAM(wp);
		WORD keys = GET_KEYSTATE_WPARAM(wp);
		if (m == WM_MOUSEWHEEL && keys & MK_CONTROL) {
			// A notch zooms by a quarter; a touchpad's pinch comes in
			// small steps, as fine as it is.
			POINT p = point_of(lp);
			ScreenToClient(hwnd, &p);
			zoom_to(v, v->zoom * pow(1.25, delta / (double)WHEEL_DELTA), p);
			return true;
		}
		double d = (double)delta / WHEEL_DELTA * wheel_lines() * 16 * v->k;
		// At an end, the view it's in scrolls (DefWindowProc hands it on).
		if (m == WM_MOUSEHWHEEL) return scroll_by(v, d, 0);
		if (keys & MK_SHIFT) return scroll_by(v, -d, 0);
		return scroll_by(v, 0, -d);
	}
	case WM_KEYDOWN: {
		if (!laid_out(v)) return false;
		bool ctrl = GetKeyState(VK_CONTROL) < 0;
		POINT mid = {v->cw / 2, v->ch / 2};
		double line = 40 * v->k, page = fmax(v->ch - line, line);
		switch (wp) {
		case VK_OEM_PLUS:
		case VK_ADD:
			if (!ctrl) return false;
			zoom_to(v, v->zoom * 1.25, mid);
			return true;
		case VK_OEM_MINUS:
		case VK_SUBTRACT:
			if (!ctrl) return false;
			zoom_to(v, v->zoom / 1.25, mid);
			return true;
		case '0':
		case VK_NUMPAD0:
			if (!ctrl) return false;
			zoom_to(v, 1, mid);
			return true;
		case VK_UP: scroll_by(v, 0, -line); return true;
		case VK_DOWN: scroll_by(v, 0, line); return true;
		case VK_LEFT: scroll_by(v, -line, 0); return true;
		case VK_RIGHT: scroll_by(v, line, 0); return true;
		case VK_PRIOR: scroll_by(v, 0, -page); return true;
		case VK_NEXT: scroll_by(v, 0, page); return true;
		case VK_SPACE: scroll_by(v, 0, GetKeyState(VK_SHIFT) < 0 ? -page : page); return true;
		case VK_HOME: scroll_by(v, 0, -v->sy); return true;
		case VK_END: scroll_by(v, 0, v->content_h); return true;
		}
		return false;
	}
	case WM_GESTURE: {
		GESTUREINFO gi = {sizeof gi};
		if (!GetGestureInfo((HGESTUREINFO)lp, &gi) || !laid_out(v)) return false;
		POINT p = {gi.ptsLocation.x, gi.ptsLocation.y};
		ScreenToClient(hwnd, &p);
		switch (gi.dwID) {
		case GID_ZOOM:
			if (!(gi.dwFlags & GF_BEGIN) && v->gesture_arg > 0 && gi.ullArguments > 0) zoom_to(v, v->zoom * (double)gi.ullArguments / v->gesture_arg, p);
			v->gesture_arg = gi.ullArguments;
			break;
		case GID_PAN:
			if (!(gi.dwFlags & GF_BEGIN)) scroll_by(v, v->gesture.x - p.x, v->gesture.y - p.y);
			v->gesture = p;
			break;
		case GID_END: v->gesture_arg = 0; break;
		default: return false;
		}
		CloseGestureInfoHandle((HGESTUREINFO)lp);
		return true;
	}
	}
	return false;
}

// ---- The package's View: its props ----

static void set_source(pdf_View *self, const char *value) {
	view *v = (view *)self;
	if (v->source && !strcmp(v->source, value)) return;
	free(v->source);
	v->source = asuka_strdup(value);
	v->gen++;
	// The document that was goes; the page the app has is the new one's.
	clear_cache(v);
	free(v->sizes);
	v->sizes = NULL;
	v->n = 0;
	v->zoom = 1;
	v->sx = v->sy = 0;
	v->render_failed = false;
	v->to_page = true;
	layout(v);
	if (v->alone && *value) {
		pdf_View_error(v->events, "platform/pdf: no thread could start to open it");
		return;
	}
	shared *s = v->s;
	EnterCriticalSection(&s->mu);
	free(s->load);
	s->load = asuka_strdup(value);
	s->load_gen = v->gen;
	s->gen = v->gen;
	s->nwant = 0;
	WakeConditionVariable(&s->cv);
	LeaveCriticalSection(&s->mu);
	invalidate(v);
}

static void set_page(pdf_View *self, int64_t value) {
	view *v = (view *)self;
	int p = value < 0 ? 0 : value > INT32_MAX ? INT32_MAX : (int)value;
	// What the user scrolled to, set back by the app's binding: it's there.
	if (p == v->page) return;
	v->page = p;
	scroll_to_page(v);
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
	shared *s = v->s;
	EnterCriticalSection(&s->mu);
	s->quit = true;
	s->hwnd = NULL;
	WakeConditionVariable(&s->cv);
	LeaveCriticalSection(&s->mu);
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
	v->k = 1;
	shared *s = calloc(1, sizeof *s);
	s->refs = 1;
	InitializeCriticalSection(&s->mu);
	InitializeConditionVariable(&s->cv);
	s->tail = &s->done;
	v->s = s;
	v->hwnd = asuka_win_window(0, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, (HWND)events.parent, message, v);
	if (v->hwnd) {
		v->refs++;  // the window's, until WM_NCDESTROY
		s->hwnd = v->hwnd;
		// Pinches and one finger's pans, with inertia; no rotation.
		GESTURECONFIG c[3] = {{GID_ZOOM, GC_ZOOM, 0}, {GID_ROTATE, 0, GC_ROTATE},
		                      {GID_PAN, GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_INERTIA,
		                       GC_PAN_WITH_GUTTER}};
		SetGestureConfig(v->hwnd, 0, 3, c, sizeof *c);
	}
	s->refs++;  // the worker's
	if (!asuka_desktop_spawn(work, s)) {
		s->refs--;
		v->alone = true;
	}
	v->base.widget = v->hwnd;
	return &v->base;
}
