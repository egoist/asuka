// maps.View on Windows: a map drawn here, of OpenStreetMap's tiles
// (Windows has no map control for desktop apps: its MapControl is UWP's,
// and needs a Bing Maps key), as GNOME Maps' libshumate does on Linux.
// The tiles come from tile.openstreetmap.org as its usage policy asks: a
// User-Agent naming the app, two downloads at a time, the tiles kept on
// disk (the app's caches, asuka-map-tiles) and used for a week before
// they're asked for again, the attribution "© OpenStreetMap
// contributors" shown on the map, linking to its copyright page. While a
// tile comes, the one above it that's there is drawn bigger.
//
// On the tiles, with GDI+: the polygons (their holes cut out), circles
// and lines (dashed, along great circles), then the markers (pins,
// images of the app's, clusters: maps_geo.c's, the web's), the selected
// one's callout, the attribution. A heading turns it all (the tiles drawn
// unturned into a bigger buffer, then turned onto the window); the map
// stays flat (no pitch).
//
// Satellite and Hybrid have no free imagery source: they show the
// streets, as Muted does; showsUser has no location layer (nothing
// shown, as on Linux). The user moves it with a drag (a finger's pan),
// zooms with the wheel (a touchpad's pinch comes as Ctrl and the wheel),
// a finger's pinch, a double click (Shift: out), and + and -; the arrow
// keys move it. A click on a marker selects it and shows its title and
// subtitle in a callout above it; a draggable one moves with a drag; a
// right click is a long press. It has no size of its own: its style
// gives it one.
#include "asuka_windows.h"
#include "maps.asuka.h"
#include "maps_geo.h"

#include <math.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	tile_size = 256,
	max_zoom = 19,
	downloads = 2,    // at a time, as the policy allows
	mem_tiles = 384,  // decoded tiles kept
	timer_wheel = 1,  // the wheel's region report, once it stops
	timer_anim = 2,   // a move animated
	timer_idle = 3,   // a move the app made at once: settled
};
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TILE_FRESH_MS (7ULL * 24 * 3600 * 1000)  // the tile policy's least
#define ATTRIBUTION L"© OpenStreetMap contributors"

// ---- GDI+, its flat API ----

typedef float REAL;
typedef struct {
	REAL X, Y;
} PointF;
static struct {
	bool tried, ok;
	int(WINAPI *CreateFromHDC)(HDC, void **);
	int(WINAPI *DeleteGraphics)(void *);
	int(WINAPI *SetSmoothingMode)(void *, int);
	int(WINAPI *SetPixelOffsetMode)(void *, int);
	int(WINAPI *SetCompositingQuality)(void *, int);
	int(WINAPI *SetInterpolationMode)(void *, int);
	int(WINAPI *TranslateWorldTransform)(void *, REAL, REAL, int);
	int(WINAPI *RotateWorldTransform)(void *, REAL, int);
	int(WINAPI *ResetWorldTransform)(void *);
	int(WINAPI *CreatePath)(int, void **);
	int(WINAPI *DeletePath)(void *);
	int(WINAPI *AddPathArc)(void *, REAL, REAL, REAL, REAL, REAL, REAL);
	int(WINAPI *AddPathLine)(void *, REAL, REAL, REAL, REAL);
	int(WINAPI *AddPathLine2)(void *, const PointF *, INT);
	int(WINAPI *StartPathFigure)(void *);
	int(WINAPI *ClosePathFigure)(void *);
	int(WINAPI *CreateSolidFill)(UINT32, void **);
	int(WINAPI *DeleteBrush)(void *);
	int(WINAPI *CreatePen1)(UINT32, REAL, int, void **);
	int(WINAPI *DeletePen)(void *);
	int(WINAPI *SetPenLineCap197819)(void *, int, int, int);
	int(WINAPI *SetPenLineJoin)(void *, int);
	int(WINAPI *SetPenDashArray)(void *, const REAL *, INT);
	int(WINAPI *FillPath)(void *, void *, void *);
	int(WINAPI *DrawPath)(void *, void *, void *);
	int(WINAPI *FillEllipse)(void *, void *, REAL, REAL, REAL, REAL);
	int(WINAPI *DrawEllipse)(void *, void *, REAL, REAL, REAL, REAL);
	int(WINAPI *FillRectangle)(void *, void *, REAL, REAL, REAL, REAL);
	int(WINAPI *CreateBitmapFromScan0)(INT, INT, INT, INT, BYTE *, void **);
	int(WINAPI *DisposeImage)(void *);
	int(WINAPI *DrawImageRect)(void *, void *, REAL, REAL, REAL, REAL);
} gp;

static bool gdiplus(void) {
	if (gp.tried) return gp.ok;
	gp.tried = true;
	HMODULE m = LoadLibraryExW(L"gdiplus.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m) return false;
	struct {
		UINT32 version;
		void *debug;
		BOOL no_thread, no_codecs;
	} in = {1, NULL, FALSE, FALSE};
	int(WINAPI * startup)(ULONG_PTR *, const void *, void *) = NULL;
	*(FARPROC *)&startup = GetProcAddress(m, "GdiplusStartup");
	ULONG_PTR token;
	if (!startup || startup(&token, &in, NULL) != 0) return false;
#define F(name) *(FARPROC *)&gp.name = GetProcAddress(m, "Gdip" #name)
	F(CreateFromHDC);
	F(DeleteGraphics);
	F(SetSmoothingMode);
	F(SetPixelOffsetMode);
	F(SetCompositingQuality);
	F(SetInterpolationMode);
	F(TranslateWorldTransform);
	F(RotateWorldTransform);
	F(ResetWorldTransform);
	F(CreatePath);
	F(DeletePath);
	F(AddPathArc);
	F(AddPathLine);
	F(AddPathLine2);
	F(StartPathFigure);
	F(ClosePathFigure);
	F(CreateSolidFill);
	F(DeleteBrush);
	F(CreatePen1);
	F(DeletePen);
	F(SetPenLineCap197819);
	F(SetPenLineJoin);
	F(SetPenDashArray);
	F(FillPath);
	F(DrawPath);
	F(FillEllipse);
	F(DrawEllipse);
	F(FillRectangle);
	F(CreateBitmapFromScan0);
	F(DisposeImage);
	F(DrawImageRect);
#undef F
	gp.ok = gp.CreateFromHDC && gp.DeleteGraphics && gp.SetSmoothingMode && gp.SetPixelOffsetMode && gp.SetCompositingQuality &&
	        gp.SetInterpolationMode && gp.TranslateWorldTransform && gp.RotateWorldTransform && gp.ResetWorldTransform && gp.CreatePath &&
	        gp.DeletePath && gp.AddPathArc && gp.AddPathLine && gp.AddPathLine2 && gp.StartPathFigure && gp.ClosePathFigure &&
	        gp.CreateSolidFill && gp.DeleteBrush && gp.CreatePen1 && gp.DeletePen && gp.SetPenLineCap197819 && gp.SetPenLineJoin &&
	        gp.SetPenDashArray && gp.FillPath && gp.DrawPath && gp.FillEllipse && gp.DrawEllipse && gp.FillRectangle &&
	        gp.CreateBitmapFromScan0 && gp.DisposeImage && gp.DrawImageRect;
	return gp.ok;
}

static UINT32 argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) { return (UINT32)a << 24 | (UINT32)r << 16 | (UINT32)g << 8 | b; }

// argb_of is a color as asuka_desktop_color has it (0xRRGGBBAA), as GDI+'s.
static UINT32 argb_of(uint32_t rgba) { return argb((uint8_t)rgba, (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8)); }

// graphics draws antialiased into dc (NULL: no GDI+).
static void *graphics(HDC dc) {
	void *g = NULL;
	if (!gdiplus() || gp.CreateFromHDC(dc, &g) != 0) return NULL;
	gp.SetSmoothingMode(g, 4);       // AntiAlias
	gp.SetPixelOffsetMode(g, 4);     // Half: pixel centers at .5
	gp.SetCompositingQuality(g, 4);  // AssumeLinear
	return g;
}

static void fill_path(void *g, UINT32 color, void *path) {
	void *b = NULL;
	gp.CreateSolidFill(color, &b);
	gp.FillPath(g, b, path);
	gp.DeleteBrush(b);
}

static void fill_round_rect(void *g, UINT32 color, REAL x, REAL y, REAL w, REAL h, REAL r) {
	void *p = NULL;
	gp.CreatePath(0, &p);
	r = fminf(r, fminf(w / 2, h / 2));
	REAL d = 2 * r;
	gp.StartPathFigure(p);
	gp.AddPathArc(p, x, y, d, d, 180, 90);
	gp.AddPathArc(p, x + w - d, y, d, d, 270, 90);
	gp.AddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
	gp.AddPathArc(p, x, y + h - d, d, d, 90, 90);
	gp.ClosePathFigure(p);
	fill_path(g, color, p);
	gp.DeletePath(p);
}

// ---- The tiles, shared by the maps ----

typedef struct {
	int32_t z, x, y;
} tkey;

static bool same(tkey a, tkey b) { return a.z == b.z && a.x == b.x && a.y == b.y; }

typedef struct view view;

static struct {
	bool made;
	struct {
		tkey k;
		HBITMAP bmp;
		uint64_t used;
	} *bmps;
	size_t nbmps;
	tkey *want;  // asked for, not here yet
	size_t nwant;
	struct {
		tkey k;
		ULONGLONG until;  // couldn't be had: not asked again until then
	} *failed;
	size_t nfailed;
	uint64_t use;
	view **views;
	size_t nviews;
	HDC dc;  // selects a tile to draw it
	int workers;
	char *dir, *agent;
	// The downloads' queue, newest last: what's on screen now first.
	CRITICAL_SECTION mu;
	CONDITION_VARIABLE cv;
	struct {
		tkey k;
		LONG64 frame;
	} *jobs;
	size_t njobs, capjobs;
	volatile LONG64 frame;
} T;

static void tiles(void) {
	if (T.made) return;
	T.made = true;
	InitializeCriticalSection(&T.mu);
	InitializeConditionVariable(&T.cv);
	T.dir = asuka_sprintf("%s\\asuka-map-tiles", asuka_desktop_cache_dir());
	// Asuka and the app (not the user), to OpenStreetMap's servers, as
	// their policies ask.
	const char *app = asuka_desktop_app_id();
	T.agent = asuka_sprintf("Asuka/1 (%s; Windows; +https://github.com/egoist/asuka)", app && *app ? app : asuka_desktop_app_name());
}

// tile_url is where a tile comes from: ASUKA_MAP_TILES ({z}, {x}, {y}),
// else OpenStreetMap's.
static char *tile_url(tkey k) {
	const char *t = getenv("ASUKA_MAP_TILES");
	if (!t || !*t) t = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
	asuka_writer w = {0};
	for (const char *p = t; *p;) {
		int v = -1;
		if (!strncmp(p, "{z}", 3)) v = k.z;
		else if (!strncmp(p, "{x}", 3)) v = k.x;
		else if (!strncmp(p, "{y}", 3)) v = k.y;
		if (v >= 0) {
			char n[16];
			int len = snprintf(n, sizeof n, "%d", v);
			asuka_writer_put(&w, n, (size_t)len);
			p += 3;
		} else {
			asuka_writer_put(&w, p++, 1);
		}
	}
	asuka_writer_u8(&w, 0);
	char *s = asuka_strdup((char *)w.data);
	asuka_writer_free(&w);
	return s;
}

// decode is an image file's pixels as a 256×256 tile's BGRA (of
// malloc's), or NULL.
static uint8_t *decode(const char *path) {
	UINT w, h;
	uint8_t *px = NULL;
	if (!asuka_win_image_load(path, &w, &h, &px)) return NULL;
	if (w == tile_size && h == tile_size) return px;
	uint8_t *out = malloc(tile_size * tile_size * 4);  // another size: its nearest pixels
	for (int y = 0; y < tile_size; y++)
		for (int x = 0; x < tile_size; x++)
			memcpy(out + (y * tile_size + x) * 4, px + ((size_t)(y * h / tile_size) * w + x * w / tile_size) * 4, 4);
	free(px);
	return out;
}

static ULONGLONG now_ms(void) { return GetTickCount64(); }

// age is how long ago a file was written (ms), or false if it isn't there.
static bool age(const char *path, ULONGLONG *ms) {
	wchar_t *w = asuka_wide(path);
	WIN32_FILE_ATTRIBUTE_DATA a;
	bool ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
	free(w);
	if (!ok) return false;
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULONGLONG now = (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
	ULONGLONG at = (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
	*ms = now > at ? (now - at) / 10000 : 0;
	return true;
}

// load is a tile's pixels: the disk's, if fresh; else the server's, kept
// on disk; else the disk's, however old. Off the main thread.
static uint8_t *load(tkey k, char **err) {
	char *path = asuka_sprintf("%s\\%d\\%d\\%d.png", T.dir, k.z, k.x, k.y);
	ULONGLONG ms;
	bool stale = false;
	if (age(path, &ms)) {
		if (ms < TILE_FRESH_MS) {
			uint8_t *px = decode(path);
			if (px) {
				free(path);
				return px;
			}
		} else {
			stale = true;
		}
	}
	char *url = tile_url(k);
	asuka_header ua = {"User-Agent", T.agent};
	asuka_response r = {0};
	asuka_error e = {0};
	uint8_t *px = NULL;
	if (!asuka_fetch("GET", url, &ua, 1, NULL, 0, 30000, NULL, &r, &e)) {
		*err = asuka_strdup(e.message ? e.message : "no answer");
	} else if (r.status != 200) {
		*err = asuka_sprintf("HTTP %d", r.status);
	} else {
		char *dir = asuka_sprintf("%s\\%d\\%d", T.dir, k.z, k.x);
		wchar_t *wd = asuka_wide(dir);
		SHCreateDirectoryExW(NULL, wd, NULL);
		free(wd);
		free(dir);
		char *tmp = asuka_sprintf("%s.%lu.tmp", path, GetCurrentThreadId());
		wchar_t *wt = asuka_wide(tmp), *wp = asuka_wide(path);
		HANDLE f = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD n = 0;
		bool written = f != INVALID_HANDLE_VALUE && WriteFile(f, r.body.data, (DWORD)r.body.len, &n, NULL) && n == r.body.len;
		if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
		px = written ? decode(tmp) : NULL;
		if (px) MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING);
		else DeleteFileW(wt);
		if (!px) *err = asuka_strdup(written ? "not an image" : "can't keep it");
		free(wt);
		free(wp);
		free(tmp);
	}
	asuka_error_free(&e);
	asuka_response_free(&r);
	free(url);
	if (!px && stale) {
		px = decode(path);
		if (px) {
			free(*err);
			*err = NULL;
		}
	}
	free(path);
	return px;
}

static void unwant(tkey k) {
	for (size_t i = 0; i < T.nwant; i++)
		if (same(T.want[i], k)) {
			T.want[i] = T.want[--T.nwant];
			return;
		}
}

typedef struct {
	tkey k;
	uint8_t *px;
	char *err;
} came;

static void tile_came(view *v);
static void tile_failed(view *v, const char *err);

static void trim(void) {
	// The least recently drawn go, down to three quarters.
	while (T.nbmps > mem_tiles * 3 / 4) {
		size_t oldest = 0;
		for (size_t i = 1; i < T.nbmps; i++)
			if (T.bmps[i].used < T.bmps[oldest].used) oldest = i;
		DeleteObject(T.bmps[oldest].bmp);
		T.bmps[oldest] = T.bmps[--T.nbmps];
	}
}

static HBITMAP dib(int w, int h, void **bits) {
	BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0}};
	return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

// loaded keeps a tile that came, and redraws the maps; one that couldn't
// be had is asked again in half a minute, and the maps say so once.
static void loaded(void *p) {
	came *c = p;
	unwant(c->k);
	if (!c->px) {
		T.failed = realloc(T.failed, (T.nfailed + 1) * sizeof *T.failed);
		T.failed[T.nfailed].k = c->k;
		T.failed[T.nfailed++].until = now_ms() + 30000;
		for (size_t i = 0; i < T.nviews; i++) tile_failed(T.views[i], c->err ? c->err : "unknown");
	} else {
		void *bits = NULL;
		HBITMAP bmp = dib(tile_size, tile_size, &bits);
		if (bmp) {
			memcpy(bits, c->px, tile_size * tile_size * 4);
			T.bmps = realloc(T.bmps, (T.nbmps + 1) * sizeof *T.bmps);
			T.bmps[T.nbmps].k = c->k;
			T.bmps[T.nbmps].bmp = bmp;
			T.bmps[T.nbmps++].used = ++T.use;
			if (T.nbmps > mem_tiles) trim();
			for (size_t i = 0; i < T.nviews; i++) tile_came(T.views[i]);
		}
	}
	free(c->px);
	free(c->err);
	free(c);
}

static void forget_want(void *p) {
	tkey *k = p;
	unwant(*k);
	free(k);
}

// work loads tiles, the newest asked first; those asked frames ago (off
// screen since) are let go.
static void work(void *unused) {
	for (;;) {
		EnterCriticalSection(&T.mu);
		while (T.njobs == 0) SleepConditionVariableCS(&T.cv, &T.mu, INFINITE);
		tkey k = T.jobs[T.njobs - 1].k;
		LONG64 frame = T.jobs[--T.njobs].frame;
		LeaveCriticalSection(&T.mu);
		if (InterlockedCompareExchange64(&T.frame, 0, 0) > frame + 2) {
			tkey *kk = malloc(sizeof *kk);
			*kk = k;
			asuka_desktop_post(forget_want, kk);
			continue;
		}
		came *c = calloc(1, sizeof *c);
		c->k = k;
		c->px = load(k, &c->err);
		asuka_desktop_post(loaded, c);
	}
}

// get is a tile's bitmap, if it's here; else it's asked for (once).
static HBITMAP get(tkey k, bool ask) {
	for (size_t i = 0; i < T.nbmps; i++)
		if (same(T.bmps[i].k, k)) {
			T.bmps[i].used = ++T.use;
			return T.bmps[i].bmp;
		}
	if (!ask) return NULL;
	for (size_t i = 0; i < T.nwant; i++)
		if (same(T.want[i], k)) return NULL;
	for (size_t i = 0; i < T.nfailed; i++)
		if (same(T.failed[i].k, k)) {
			if (now_ms() < T.failed[i].until) return NULL;
			T.failed[i] = T.failed[--T.nfailed];
			break;
		}
	T.want = realloc(T.want, (T.nwant + 1) * sizeof *T.want);
	T.want[T.nwant++] = k;
	EnterCriticalSection(&T.mu);
	if (T.njobs == T.capjobs) {
		T.capjobs = T.capjobs ? T.capjobs * 2 : 64;
		T.jobs = realloc(T.jobs, T.capjobs * sizeof *T.jobs);
	}
	T.jobs[T.njobs].k = k;
	T.jobs[T.njobs++].frame = InterlockedCompareExchange64(&T.frame, 0, 0);
	while (T.workers < downloads && asuka_desktop_spawn(work, NULL)) T.workers++;
	WakeConditionVariable(&T.cv);
	LeaveCriticalSection(&T.mu);
	return NULL;
}


// ---- The markers' images, shared by the maps ----

typedef struct {
	char *src;
	int state;  // 0 coming, 1 here, 2 can't be had (the pin shows)
	UINT w, h;
	uint8_t *px;  // premultiplied BGRA
	void *bmp;    // GDI+'s, of px
} image;

static struct {
	image **v;
	size_t n;
} I;

typedef struct {
	image *im;
	char *src;
	uint8_t *px;
	UINT w, h;
} image_job;

static void invalidate(view *v);

static void image_came(void *p) {
	image_job *j = p;
	image *im = j->im;
	im->state = 2;
	if (j->px) {
		im->px = j->px;
		im->w = j->w;
		im->h = j->h;
		if (gdiplus() && gp.CreateBitmapFromScan0((INT)im->w, (INT)im->h, (INT)im->w * 4, 0xE200B, im->px, &im->bmp) == 0)  // 32bppPARGB
			im->state = 1;
	}
	free(j->src);
	free(j);
	for (size_t i = 0; i < T.nviews; i++) invalidate(T.views[i]);
}

// image_load reads an image off the main thread: an asset, a file, a URL
// (downloaded into the app's caches).
static void image_load(void *p) {
	image_job *j = p;
	asuka_error e = {0};
	char *path = asuka_desktop_local_file(j->src, "", &e);
	if (path && !asuka_win_image_load(path, &j->w, &j->h, &j->px)) j->px = NULL;
	free(path);
	asuka_error_free(&e);
	asuka_desktop_post(image_came, j);
}

// image_of is a source's image: here, or coming.
static image *image_of(const char *src) {
	for (size_t i = 0; i < I.n; i++)
		if (!strcmp(I.v[i]->src, src)) return I.v[i];
	image *im = calloc(1, sizeof *im);
	im->src = asuka_strdup(src);
	I.v = realloc(I.v, (I.n + 1) * sizeof *I.v);
	I.v[I.n++] = im;
	image_job *j = calloc(1, sizeof *j);
	j->im = im;
	j->src = asuka_strdup(src);
	if (!asuka_desktop_spawn(image_load, j)) {
		im->state = 2;
		free(j->src);
		free(j);
	}
	return im;
}

// ---- The view ----

typedef struct {
	char *id, *title, *subtitle;
	double lat, lng;  // where it is (where it was dragged to)
	double z, w, h, ax, ay;
	bool colored, draggable, clusters;
	uint32_t rgba;
	image *im;  // NULL: the pin
} marker;

// A polygon (its rings: the outline, then the holes), a circle (its
// outline) or a line, as fractions of the world, unwrapped.
typedef struct {
	int kind;  // 0 a polygon, 1 a circle, 2 a line: drawn in that order when their zIndex is the same
	char *id;
	double z;
	size_t order;
	mg_path *rings;
	size_t nrings;
	UINT32 fill, stroke;
	double width;
	bool dashed;
} shape;

// What's drawn of the markers: one alone, or a cluster.
typedef struct {
	size_t i;  // the marker's index, or the cluster's
	bool cluster;
	double z;
	size_t order;
} item;

struct view {
	maps_View base;
	maps_View_events events;
	int refs;  // the view's, its window's and its posts'
	HWND hwnd;
	// Where it shows: the center, as fractions of the world (Web
	// Mercator, x east from 180°W, y south from 85°N), the zoom (the
	// world is 256·2^zoom points wide) and the heading (degrees clockwise
	// from north that the top faces).
	double cx, cy, zoom, heading;
	int pending;  // 1 a region, 2 a camera: waits for the view's size
	maps_Region pending_region;
	maps_Camera pending_camera;
	bool shown;  // it was placed: the next move animates
	marker *markers;
	size_t nmarkers;
	shape *shapes;
	size_t nshapes;
	bool clustering, cluster_colored;
	uint32_t cluster_rgba;
	mg_group *groups;  // of the markers that cluster, their members the markers' indices
	size_t ngroups;
	int group_level;
	bool grouped;
	char *selected;  // the marker whose callout shows
	RECT callout;
	bool has_callout;
	bool interactive;
	// The mouse.
	bool down, dragging, doubled;
	POINT down_at, last_up_at, gesture;
	double down_cx, down_cy;
	ULONGLONG last_up;
	ULONGLONG gesture_arg;
	long mdrag;  // the marker pressed to drag (-1: none)
	bool mdragging;
	double grab_x, grab_y;  // from the pointer to the marker's point
	// Its moves: the user's since it settled, any since then, a move
	// event posted.
	bool user_moved, unsettled, move_posted;
	struct {
		bool on, around;  // around: a zoom about a point (at), which stays
		double cx, cy, zoom, heading, tcx, tcy, tzoom, theading, ms;
		POINT at;
		ULONGLONG start;
	} anim;
	HBITMAP back, frame;  // the back buffer; the map unturned, for a heading
	SIZE back_size;
	int frame_size;
	void *frame_bits;
	double attr_k;  // the attribution's width, measured at this scale
	int attr_w;
	bool failed;  // tiles couldn't be had: said once
	bool destroyed;
};

static void free_markers(view *v) {
	for (size_t i = 0; i < v->nmarkers; i++) {
		free(v->markers[i].id);
		free(v->markers[i].title);
		free(v->markers[i].subtitle);
	}
	free(v->markers);
	v->markers = NULL;
	v->nmarkers = 0;
}

static void free_shape(shape *s) {
	for (size_t r = 0; r < s->nrings; r++) mg_path_free(&s->rings[r]);
	free(s->rings);
	free(s->id);
}

static void unref(view *v) {
	if (--v->refs > 0) return;
	free_markers(v);
	for (size_t i = 0; i < v->nshapes; i++) free_shape(&v->shapes[i]);
	free(v->shapes);
	mg_groups_free(v->groups, v->ngroups);
	free(v->selected);
	if (v->back) DeleteObject(v->back);
	if (v->frame) DeleteObject(v->frame);
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

static RECT client(view *v) {
	RECT r = {0};
	if (v->hwnd) GetClientRect(v->hwnd, &r);
	return r;
}

// size is the view's size in points.
static void size(view *v, double *w, double *h) {
	RECT r = client(v);
	double k = scale(v);
	*w = (r.right - r.left) / k;
	*h = (r.bottom - r.top) / k;
}

// world is the world's width in points at the zoom.
static double world(view *v) { return tile_size * exp2(v->zoom); }

static void invalidate(view *v) {
	if (v->hwnd) InvalidateRect(v->hwnd, NULL, FALSE);
}

// near is x moved by whole worlds to the copy nearest to.
static double near_x(double x, double to) { return x + mg_shift(x, to); }

// rot turns (x, y) by deg degrees clockwise (the screen's y is down).
static void rot(double deg, double x, double y, double *ox, double *oy) {
	double t = deg * M_PI / 180, c = cos(t), s = sin(t);
	*ox = c * x - s * y;
	*oy = s * x + c * y;
}

// to_screen is where a place (fractions of the world, x near the
// center's) is in the window (pixels): the map unturned about its center,
// turned by the heading.
static void to_screen(view *v, double x, double y, double *sx, double *sy) {
	RECT r = client(v);
	double W = world(v) * scale(v), ox, oy;
	rot(-v->heading, (x - v->cx) * W, (y - v->cy) * W, &ox, &oy);
	*sx = (r.right - r.left) / 2.0 + ox;
	*sy = (r.bottom - r.top) / 2.0 + oy;
}

// point is where a place is in the window (pixels): the copy of the
// world nearest the center.
static void point(view *v, double lat, double lng, double *x, double *y) { to_screen(v, near_x(mg_x(lng), v->cx), mg_merc_y(lat), x, y); }

// unturned is a point of the window as the map's unturned: pixels from
// the center.
static void unturned(view *v, double sx, double sy, double *fx, double *fy) {
	RECT r = client(v);
	rot(v->heading, sx - (r.right - r.left) / 2.0, sy - (r.bottom - r.top) / 2.0, fx, fy);
}

// at is the place at a point of the view (pixels).
static maps_Coordinate at_xy(view *v, double sx, double sy) {
	double fx, fy, W = world(v) * scale(v);
	unturned(v, sx, sy, &fx, &fy);
	double x = v->cx + fx / W, y = v->cy + fy / W;
	x -= floor(x);
	return (maps_Coordinate){mg_merc_lat(y), mg_norm_lng(x * 360 - 180)};
}

static maps_Coordinate at(view *v, POINT p) { return at_xy(v, p.x, p.y); }

// clamp keeps the zoom in range and the map's poles in view; the world
// wraps east and west.
static void clamp(view *v) {
	double w, h;
	size(v, &w, &h);
	double least = h > 0 ? log2(fmax(h, 1) / tile_size) : 0;
	v->zoom = fmax(fmin(v->zoom, max_zoom), fmax(least, 0));
	v->cx -= floor(v->cx);
	double half = h / 2 / world(v);
	v->cy = fmax(fmin(v->cy, 1 - half), half);
	v->heading -= 360 * floor(v->heading / 360);
	if (v->heading > 360 - 1e-9) v->heading = 0;
}

// now is where the map looks from, and the region it shows (north up).
static void now(view *v, maps_Camera *c, maps_Region *r) {
	double w, h;
	size(v, &w, &h);
	maps_Coordinate center = {mg_merc_lat(v->cy), mg_norm_lng(v->cx * 360 - 180)};
	*c = (maps_Camera){center, v->zoom, v->heading, 0};
	*r = (maps_Region){center, 0, 0};
	mg_span(v->cx, v->cy, v->zoom, w, h, &r->latDelta, &r->lngDelta);
}

static void emit_move(void *p) {
	view *v = p;
	v->move_posted = false;
	if (!v->destroyed && v->hwnd) {
		maps_Camera c;
		maps_Region r;
		now(v, &c, &r);
		maps_View_cameraMove(v->events, &c, &r);
	}
	unref(v);
}

// changed: the map moved (user: the user moved it). cameraMove goes once
// the loop's idle, at most once a frame.
static void changed(view *v, bool user) {
	v->unsettled = true;
	if (user) v->user_moved = true;
	if (!v->move_posted && !v->destroyed) {
		v->move_posted = true;
		v->refs++;
		asuka_desktop_post(emit_move, v);
	}
	invalidate(v);
}

// settle tells the app where the map came to rest: cameraIdle, and the
// region and camera the user moved it to.
static void settle(view *v) {
	if (v->hwnd) KillTimer(v->hwnd, timer_idle);
	if (!v->unsettled || v->destroyed) return;
	v->unsettled = false;
	maps_Camera c;
	maps_Region r;
	now(v, &c, &r);
	if (v->user_moved) {
		v->user_moved = false;
		maps_View_regionChange(v->events, &r);
		maps_View_cameraChange(v->events, &c);
	}
	maps_View_cameraIdle(v->events, &c, &r);
}


static void stop_anim(view *v) {
	if (!v->anim.on) return;
	v->anim.on = false;
	if (v->hwnd) KillTimer(v->hwnd, timer_anim);
}

// go moves the map where the app says: animated once it's shown.
static void go(view *v, double cx, double cy, double zoom, double heading, bool animated) {
	stop_anim(v);
	zoom = fmax(fmin(zoom, max_zoom), 0);
	if (!animated || !v->shown || !v->hwnd) {
		v->shown = true;
		v->cx = cx;
		v->cy = cy;
		v->zoom = zoom;
		v->heading = heading;
		clamp(v);
		changed(v, false);
		if (v->hwnd) SetTimer(v->hwnd, timer_idle, 30, NULL);
		else settle(v);
		return;
	}
	double dh = heading - v->heading;
	dh -= 360 * floor(dh / 360 + 0.5);
	v->anim.on = true;
	v->anim.around = false;
	v->anim.cx = v->cx;
	v->anim.cy = v->cy;
	v->anim.zoom = v->zoom;
	v->anim.heading = v->heading;
	v->anim.tcx = near_x(cx, v->cx);
	v->anim.tcy = cy;
	v->anim.tzoom = zoom;
	v->anim.theading = v->heading + dh;
	v->anim.ms = 300;
	v->anim.start = now_ms();
	SetTimer(v->hwnd, timer_anim, 15, NULL);
}

// there reports whether the map shows that already (the move the user
// made, given back).
static bool there(view *v, double cx, double cy, double zoom, double heading) {
	double dh = heading - v->heading;
	dh -= 360 * floor(dh / 360 + 0.5);
	return fabs(near_x(cx, v->cx) - v->cx) < 1e-9 && fabs(cy - v->cy) < 1e-9 && fabs(zoom - v->zoom) < 1e-6 && fabs(dh) < 1e-6;
}

// apply shows the pending region or camera, once the view has a size.
static void apply(view *v) {
	double w, h;
	size(v, &w, &h);
	if (!v->pending || w <= 0 || h <= 0) return;
	double cx, cy, zoom, heading = v->heading;
	if (v->pending == 1) {
		maps_Region r = v->pending_region;
		cx = mg_x(r.center.lng);
		cy = mg_merc_y(r.center.lat);
		zoom = mg_zoom_for(r.center.lat, r.latDelta, r.lngDelta, w, h, max_zoom);
	} else {
		maps_Camera c = v->pending_camera;
		cx = mg_x(c.center.lng);
		cy = mg_merc_y(c.center.lat);
		zoom = c.zoom;
		heading = c.heading;
	}
	v->pending = 0;
	if (v->shown && there(v, cx, cy, fmax(fmin(zoom, max_zoom), 0), heading)) return;
	go(v, cx, cy, zoom, heading, true);
}

// zoom_by zooms by dz around a point (pixels), which stays where it is.
static void zoom_by(view *v, double dz, POINT p, bool user) {
	double k = scale(v), ox, oy;
	unturned(v, p.x, p.y, &ox, &oy);
	ox /= k;
	oy /= k;
	double W0 = world(v);
	double px = v->cx + ox / W0, py = v->cy + oy / W0;
	v->zoom += dz;
	clamp(v);
	double W = world(v);
	v->cx = px - ox / W;
	v->cy = py - oy / W;
	clamp(v);
	changed(v, user);
}

// pan_by moves the map by dx, dy pixels.
static void pan_by(view *v, double dx, double dy) {
	double k = scale(v), W = world(v), fx, fy;
	rot(v->heading, dx, dy, &fx, &fy);
	v->cx -= fx / k / W;
	v->cy -= fy / k / W;
	clamp(v);
	changed(v, true);
}

static void step_anim(view *v) {
	double f = fmin((now_ms() - v->anim.start) / v->anim.ms, 1);
	f = 1 - (1 - f) * (1 - f);  // easing out
	if (v->anim.around) {
		zoom_by(v, v->anim.zoom + (v->anim.tzoom - v->anim.zoom) * f - v->zoom, v->anim.at, true);
	} else {
		v->cx = v->anim.cx + (v->anim.tcx - v->anim.cx) * f;
		v->cy = v->anim.cy + (v->anim.tcy - v->anim.cy) * f;
		v->zoom = v->anim.zoom + (v->anim.tzoom - v->anim.zoom) * f;
		v->heading = v->anim.heading + (v->anim.theading - v->anim.heading) * f;
		clamp(v);
		changed(v, false);
	}
	if (f >= 1) {
		stop_anim(v);
		settle(v);
	}
}

// animate_zoom zooms by dz around a point, over a fifth of a second.
static void animate_zoom(view *v, double dz, POINT p) {
	stop_anim(v);
	v->anim.on = true;
	v->anim.around = true;
	v->anim.zoom = v->zoom;
	v->anim.tzoom = fmax(fmin(v->zoom + dz, max_zoom), 0);
	v->anim.at = p;
	v->anim.ms = 200;
	v->anim.start = now_ms();
	SetTimer(v->hwnd, timer_anim, 15, NULL);
}

static void tile_came(view *v) { invalidate(v); }

static void tile_failed(view *v, const char *err) {
	if (v->failed || v->destroyed) return;
	v->failed = true;
	char *m = asuka_sprintf("maps: the map's tiles can't be loaded: %s", err);
	maps_View_error(v->events, m);
	free(m);
}

// ---- The markers: alone and clustered ----

// regroup works the clusters out again when the markers or the zoom
// level changed.
static void regroup(view *v) {
	int level = mg_level(v->zoom);
	if (v->grouped && (!v->clustering || level == v->group_level)) return;
	mg_groups_free(v->groups, v->ngroups);
	v->groups = NULL;
	v->ngroups = 0;
	v->grouped = true;
	v->group_level = level;
	if (!v->clustering) return;
	mg_ll *pts = malloc((v->nmarkers ? v->nmarkers : 1) * sizeof *pts);
	size_t *map = malloc((v->nmarkers ? v->nmarkers : 1) * sizeof *map), n = 0;
	for (size_t i = 0; i < v->nmarkers; i++)
		if (v->markers[i].clusters) {
			pts[n] = (mg_ll){v->markers[i].lat, v->markers[i].lng};
			map[n++] = i;
		}
	v->ngroups = mg_cluster(pts, n, v->zoom, &v->groups);
	for (size_t g = 0; g < v->ngroups; g++)
		for (size_t m = 0; m < v->groups[g].n; m++) v->groups[g].members[m] = map[v->groups[g].members[m]];
	free(pts);
	free(map);
}

static int cmp_item(const void *a, const void *b) {
	const item *x = a, *y = b;
	if (x->z != y->z) return x->z < y->z ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
}

// items is what's drawn of the markers, bottom first.
static size_t items(view *v, item **out) {
	regroup(v);
	item *it = malloc((v->nmarkers + 1) * sizeof *it);
	size_t n = 0;
	for (size_t i = 0; i < v->nmarkers; i++)
		if (!v->clustering || !v->markers[i].clusters) it[n++] = (item){i, false, v->markers[i].z, i};
	for (size_t g = 0; g < v->ngroups; g++) {
		const mg_group *gr = &v->groups[g];
		if (gr->n == 1) {
			it[n++] = (item){gr->members[0], false, v->markers[gr->members[0]].z, gr->members[0]};
			continue;
		}
		double z = -INFINITY;
		for (size_t m = 0; m < gr->n; m++) z = fmax(z, v->markers[gr->members[m]].z);
		it[n++] = (item){g, true, z, gr->members[0]};
	}
	if (n > 1) qsort(it, n, sizeof *it, cmp_item);
	*out = it;
	return n;
}

// box is what an item covers in the window (pixels), and where its
// place is; image: the marker shows its image.
static void box(view *v, const item *it, double *x0, double *y0, double *x1, double *y1, bool *image) {
	double k = scale(v), x, y;
	*image = false;
	if (it->cluster) {
		const mg_group *g = &v->groups[it->i];
		point(v, g->at.lat, g->at.lng, &x, &y);
		int digits = g->n >= 1000 ? 4 : g->n >= 100 ? 3 : g->n >= 10 ? 2 : 1;
		double r = (28 + 7 * (digits - 1)) * k / 2;
		*x0 = x - r, *y0 = y - r, *x1 = x + r, *y1 = y + r;
		return;
	}
	const marker *m = &v->markers[it->i];
	point(v, m->lat, m->lng, &x, &y);
	if (m->im && m->im->state == 1) {
		double iw = m->im->w / k, ih = m->im->h / k, w = m->w, h = m->h;
		if (w <= 0 && h <= 0) w = iw, h = ih;
		else if (w <= 0) w = ih > 0 ? h * iw / ih : h;
		else if (h <= 0) h = iw > 0 ? w * ih / iw : w;
		w *= k;
		h *= k;
		*x0 = x - m->ax * w, *y0 = y - m->ay * h, *x1 = *x0 + w, *y1 = *y0 + h;
		*image = true;
		return;
	}
	double w = 26 * k, h = 36 * k;  // the pin: its tip at the place
	*x0 = x - w / 2, *y0 = y - h, *x1 = x + w / 2, *y1 = y;
}

// item_at is what's drawn of the markers at p, the topmost.
static bool item_at(view *v, POINT p, item *out) {
	item *it;
	size_t n = items(v, &it);
	bool found = false;
	for (size_t i = n; i-- > 0 && !found;) {
		double x0, y0, x1, y1;
		bool image;
		box(v, &it[i], &x0, &y0, &x1, &y1, &image);
		if (!it[i].cluster && v->markers[it[i].i].im && v->markers[it[i].i].im->state == 0) continue;  // not drawn yet
		if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1) {
			*out = it[i];
			found = true;
		}
	}
	free(it);
	return found;
}

// ---- The shapes ----

static int cmp_shape(const void *a, const void *b) {
	const shape *x = a, *y = b;
	if (x->z != y->z) return x->z < y->z ? -1 : 1;
	if (x->kind != y->kind) return x->kind - y->kind;
	return x->order < y->order ? -1 : x->order > y->order;
}

static void clear_kind(view *v, int kind) {
	size_t n = 0;
	for (size_t i = 0; i < v->nshapes; i++) {
		if (v->shapes[i].kind == kind) free_shape(&v->shapes[i]);
		else v->shapes[n++] = v->shapes[i];
	}
	v->nshapes = n;
}

static shape *add_shape(view *v, int kind, const char *id, double z, size_t order, size_t nrings) {
	v->shapes = realloc(v->shapes, (v->nshapes + 1) * sizeof *v->shapes);
	shape *s = &v->shapes[v->nshapes++];
	*s = (shape){kind, asuka_strdup(id ? id : ""), z, order, calloc(nrings, sizeof(mg_path)), nrings, 0, 0, 0, false};
	return s;
}

static mg_ll *lls(maps_list_Coordinate c) {
	mg_ll *p = malloc((c.len ? c.len : 1) * sizeof *p);
	for (size_t i = 0; i < c.len; i++) p[i] = (mg_ll){c.data[i].lat, c.data[i].lng};
	return p;
}

// project puts a shape's ring in a frame (pixels, its center at fw/2,
// fh/2), moved by shift worlds.
static void project(view *v, const mg_path *ring, double shift, double fw, double fh, mg_path *out) {
	double W = world(v) * scale(v);
	out->n = 0;
	for (size_t i = 0; i < ring->n; i++) mg_path_push(out, (mg_xy){fw / 2 + (ring->p[i].x + shift - v->cx) * W, fh / 2 + (ring->p[i].y - v->cy) * W});
}

static void add_figure(void *path, const mg_path *pts, bool close) {
	if (pts->n < 2) return;
	PointF *f = malloc(pts->n * sizeof *f);
	for (size_t i = 0; i < pts->n; i++) f[i] = (PointF){(REAL)pts->p[i].x, (REAL)pts->p[i].y};
	gp.StartPathFigure(path);
	gp.AddPathLine2(path, f, (INT)pts->n);
	if (close) gp.ClosePathFigure(path);
	free(f);
}

// paint_shapes draws the polygons, circles and lines into a frame fw×fh
// pixels, unturned, what's far off it cut away.
static void paint_shapes(view *v, HDC dc, double fw, double fh) {
	if (!v->nshapes) return;
	void *g = graphics(dc);
	if (!g) return;
	double k = scale(v);
	double bx0 = -fw, by0 = -fh, bx1 = 2 * fw, by1 = 2 * fh;
	mg_path pts = {0}, cut = {0};
	for (size_t i = 0; i < v->nshapes; i++) {
		const shape *s = &v->shapes[i];
		if (!s->nrings || !s->rings[0].n) continue;
		double shift = mg_shift(s->rings[0].p[0].x, v->cx);
		void *path = NULL;
		gp.CreatePath(0, &path);  // FillModeAlternate: the holes cut out
		for (size_t r = 0; r < s->nrings; r++) {
			project(v, &s->rings[r], shift, fw, fh, &pts);
			if (s->kind < 2) {
				cut.n = 0;
				mg_clip_ring(pts.p, pts.n, bx0, by0, bx1, by1, &cut);
				if (cut.n >= 3) add_figure(path, &cut, true);
				continue;
			}
			// A line: its parts in the box, each a figure.
			cut.n = 0;
			for (size_t j = 1; j < pts.n; j++) {
				mg_xy a = pts.p[j - 1], b = pts.p[j];
				if (!mg_clip_segment(&a, &b, bx0, by0, bx1, by1)) continue;
				if (cut.n && (cut.p[cut.n - 1].x != a.x || cut.p[cut.n - 1].y != a.y)) {
					add_figure(path, &cut, false);
					cut.n = 0;
				}
				if (!cut.n) mg_path_push(&cut, a);
				mg_path_push(&cut, b);
			}
			add_figure(path, &cut, false);
		}
		if (s->kind < 2 && s->fill >> 24) fill_path(g, s->fill, path);
		if (s->width > 0 && s->stroke >> 24) {
			void *pen = NULL;
			gp.CreatePen1(s->stroke, (REAL)(s->width * k), 2, &pen);  // UnitPixel
			gp.SetPenLineJoin(pen, 2);                                 // round
			if (s->dashed) {
				static const REAL dash[] = {4, 2};  // in widths
				gp.SetPenLineCap197819(pen, 0, 0, 0);
				gp.SetPenDashArray(pen, dash, 2);
			} else {
				gp.SetPenLineCap197819(pen, 2, 2, 0);
			}
			gp.DrawPath(g, pen, path);
			gp.DeletePen(pen);
		}
		gp.DeletePath(path);
	}
	mg_path_free(&pts);
	mg_path_free(&cut);
	gp.DeleteGraphics(g);
}

// shape_at is the topmost shape with an id at p: in a polygon or circle
// (not in its holes), or within half a line's width and 6 points of it.
static const shape *shape_at(view *v, POINT p) {
	double fx, fy, k = scale(v);
	unturned(v, p.x, p.y, &fx, &fy);
	mg_path pts = {0};
	const shape *hit = NULL;
	for (size_t i = v->nshapes; i-- > 0 && !hit;) {
		const shape *s = &v->shapes[i];
		if (!*s->id || !s->nrings || !s->rings[0].n) continue;
		double shift = mg_shift(s->rings[0].p[0].x, v->cx);
		bool in = false;
		for (size_t r = 0; r < s->nrings; r++) {
			project(v, &s->rings[r], shift, 0, 0, &pts);
			if (s->kind == 2) in = in || mg_line_distance(pts.p, pts.n, fx, fy) <= (s->width / 2 + 6) * k;
			else if (pts.n >= 3) in ^= mg_inside(pts.p, pts.n, fx, fy);
		}
		if (in) hit = s;
	}
	mg_path_free(&pts);
	return hit;
}

// ---- Painting ----

// font is Segoe UI at pt points.
static HFONT font(double k, double pt, int weight) {
	LOGFONTW lf = {0};
	lf.lfHeight = -(LONG)lround(pt * k);
	lf.lfWeight = weight;
	lf.lfQuality = CLEARTYPE_QUALITY;
	wcscpy(lf.lfFaceName, L"Segoe UI");
	return CreateFontIndirectW(&lf);
}

// attribution is the attribution's box (pixels), at the bottom right.
static RECT attribution(view *v) {
	RECT r = client(v);
	double k = scale(v);
	if (v->attr_k != k) {
		HFONT f = font(k, 11, 400);
		HDC dc = CreateCompatibleDC(NULL);
		HGDIOBJ old = SelectObject(dc, f);
		RECT m = {0, 0, 4000, 4000};
		DrawTextW(dc, ATTRIBUTION, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, old);
		DeleteDC(dc);
		DeleteObject(f);
		v->attr_k = k;
		v->attr_w = m.right - m.left;
	}
	LONG w = v->attr_w + (LONG)lround(10 * k), h = (LONG)lround(16 * k);
	return (RECT){r.right - w, r.bottom - h, r.right, r.bottom};
}

static bool in_rect(RECT r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

static bool attribution_hit(view *v, POINT p) { return in_rect(attribution(v), p); }

static void blit(HDC dc, HBITMAP bmp, int x, int y, int w, int h, int sx, int sy, int ss) {
	HGDIOBJ old = SelectObject(T.dc, bmp);
	if (w == ss && h == ss) BitBlt(dc, x, y, w, h, T.dc, sx, sy, SRCCOPY);
	else StretchBlt(dc, x, y, w, h, T.dc, sx, sy, ss, ss, SRCCOPY);
	SelectObject(T.dc, old);
}

// paint_tiles draws the tiles of the zoom level nearest the view's at its
// pixels (a 2× display: the next level's), each scaled to fit, into a
// frame W×H pixels about the map's center; one that isn't here yet is
// drawn from one above it that is.
static void paint_tiles(view *v, HDC dc, int W, int H) {
	InterlockedIncrement64(&T.frame);
	if (!T.dc) T.dc = CreateCompatibleDC(NULL);
	double k = scale(v);
	double zf = v->zoom + log2(k);
	int z = (int)fmax(0, fmin(round(zf), max_zoom));
	int n = 1 << z;
	double world_px = tile_size * exp2(zf), tile_px = world_px / n;
	double ox = v->cx * world_px - W / 2.0, oy = v->cy * world_px - H / 2.0;
	int tx0 = (int)floor(ox / tile_px), tx1 = (int)floor((ox + W - 1) / tile_px);
	int ty0 = (int)fmax(floor(oy / tile_px), 0), ty1 = (int)fmin(floor((oy + H - 1) / tile_px), n - 1);
	for (int ty = ty0; ty <= ty1; ty++)
		for (int tx = tx0; tx <= tx1; tx++) {
			int x0 = (int)floor(tx * tile_px - ox), x1 = (int)floor((tx + 1) * tile_px - ox);
			int y0 = (int)floor(ty * tile_px - oy), y1 = (int)floor((ty + 1) * tile_px - oy);
			int wx = ((tx % n) + n) % n;
			HBITMAP bmp = get((tkey){z, wx, ty}, true);
			if (bmp) {
				blit(dc, bmp, x0, y0, x1 - x0, y1 - y0, 0, 0, tile_size);
				continue;
			}
			for (int dz = 1; dz <= z && dz <= 6; dz++) {
				bmp = get((tkey){z - dz, wx >> dz, ty >> dz}, false);
				if (bmp) {
					int part = tile_size >> dz;
					blit(dc, bmp, x0, y0, x1 - x0, y1 - y0, (wx & ((1 << dz) - 1)) * part, (ty & ((1 << dz) - 1)) * part, part);
					break;
				}
			}
		}
}

// paint_callout draws a marker's title and subtitle in a bubble above
// what shows it (top: its top, cx its middle).
static void paint_callout(view *v, HDC dc, void *g, const marker *m, double cxp, double top) {
	double k = scale(v);
	HFONT tf = font(k, 14, 600), sf = font(k, 12, 400);
	wchar_t *title = asuka_wide(m->title), *sub = asuka_wide(m->subtitle);
	RECT tr = {0}, sr = {0};
	struct {
		HFONT f;
		wchar_t *s;
		RECT *r;
		COLORREF color;
	} lines[2] = {{tf, title, &tr, RGB(0x1A, 0x1A, 0x1A)}, {sf, sub, &sr, RGB(0x5F, 0x5F, 0x5F)}};
	for (int i = 0; i < 2; i++)
		if (*lines[i].s) {
			*lines[i].r = (RECT){0, 0, 4000, 4000};
			HGDIOBJ old = SelectObject(dc, lines[i].f);
			DrawTextW(dc, lines[i].s, -1, lines[i].r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
			SelectObject(dc, old);
		}
	LONG pad = (LONG)lround(10 * k);
	LONG tw = tr.right - tr.left, sw = sr.right - sr.left;
	LONG w = (tw > sw ? tw : sw) + 2 * pad;
	LONG h = (tr.bottom - tr.top) + (sr.bottom - sr.top) + 2 * (LONG)lround(7 * k);
	LONG cx = (LONG)cxp, bottom = (LONG)top - (LONG)lround(6 * k);
	RECT box = {cx - w / 2, bottom - h, cx + w / 2, bottom};
	fill_round_rect(g, argb(40, 0, 0, 0), (REAL)box.left, (REAL)box.top + (REAL)k, (REAL)(box.right - box.left), (REAL)(box.bottom - box.top),
	                (REAL)(8 * k));
	fill_round_rect(g, argb(255, 255, 255, 255), (REAL)box.left, (REAL)box.top, (REAL)(box.right - box.left), (REAL)(box.bottom - box.top),
	                (REAL)(8 * k));
	SetBkMode(dc, TRANSPARENT);
	LONG y = box.top + (LONG)lround(7 * k);
	for (int i = 0; i < 2; i++) {
		if (!*lines[i].s) continue;
		HGDIOBJ old = SelectObject(dc, lines[i].f);
		SetTextColor(dc, lines[i].color);
		LONG lh = lines[i].r->bottom - lines[i].r->top;
		RECT rr = {box.left + pad, y, box.right - pad, y + lh};
		DrawTextW(dc, lines[i].s, -1, &rr, DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, old);
		y += lh;
	}
	v->callout = box;
	v->has_callout = true;
	free(title);
	free(sub);
	DeleteObject(tf);
	DeleteObject(sf);
}

// paint_pin draws the pin, a teardrop, its tip at the box's bottom.
static void paint_pin(view *v, void *g, const marker *m, double x0, double y0, double x1, double y1) {
	REAL k = (REAL)scale(v);
	REAL cx = (REAL)(x0 + x1) / 2, w = (REAL)(x1 - x0), tip = (REAL)y1, top = (REAL)y0, rad = w / 2;
	void *p = NULL;
	gp.CreatePath(0, &p);
	gp.AddPathArc(p, cx - rad, top, w, w, 120, 300);
	gp.AddPathLine(p, cx + rad * 0.5f, top + rad + rad * (REAL)(sqrt(3) / 2), cx, tip);
	gp.ClosePathFigure(p);
	gp.TranslateWorldTransform(g, 0, k, 0);
	fill_path(g, argb(60, 0, 0, 0), p);
	gp.TranslateWorldTransform(g, 0, -k, 0);
	UINT32 color = m->colored ? argb_of(m->rgba) : argb(255, 0xE8, 0x11, 0x23);  // Windows's red
	fill_path(g, color, p);
	void *pen = NULL;
	gp.CreatePen1(argb(255, 255, 255, 255), 1.5f * k, 2, &pen);  // UnitPixel
	gp.DrawPath(g, pen, p);
	gp.DeletePen(pen);
	gp.DeletePath(p);
	void *dot = NULL;
	gp.CreateSolidFill(argb(255, 255, 255, 255), &dot);
	gp.FillEllipse(g, dot, cx - rad * 0.38f, top + rad - rad * 0.38f, rad * 0.76f, rad * 0.76f);
	gp.DeleteBrush(dot);
}

// paint_cluster draws a cluster: a circle saying how many.
static void paint_cluster(view *v, HDC dc, void *g, const mg_group *gr, double x0, double y0, double x1, double y1) {
	REAL k = (REAL)scale(v), d = (REAL)(x1 - x0);
	uint32_t c = v->cluster_colored ? v->cluster_rgba : asuka_desktop_color((uint64_t)1 << 32 | 8);  // the accent
	void *b = NULL;
	gp.CreateSolidFill(argb(60, 0, 0, 0), &b);
	gp.FillEllipse(g, b, (REAL)x0, (REAL)y0 + k, d, d);
	gp.DeleteBrush(b);
	gp.CreateSolidFill(argb_of(c), &b);
	gp.FillEllipse(g, b, (REAL)x0, (REAL)y0, d, d);
	gp.DeleteBrush(b);
	void *pen = NULL;
	gp.CreatePen1(argb(255, 255, 255, 255), 2 * k, 2, &pen);
	gp.DrawEllipse(g, pen, (REAL)x0 + k, (REAL)y0 + k, d - 2 * k, d - 2 * k);
	gp.DeletePen(pen);
	wchar_t n[16];
	swprintf(n, 16, L"%u", (unsigned)gr->n);
	HFONT f = font(k, 12, 700);
	HGDIOBJ old = SelectObject(dc, f);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	RECT r = {(LONG)x0, (LONG)y0, (LONG)x1, (LONG)y1};
	DrawTextW(dc, n, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_CENTER | DT_VCENTER);
	SelectObject(dc, old);
	DeleteObject(f);
}

// paint_markers draws the markers (pins, images, clusters) in their
// order, and the selected one's callout.
static void paint_markers(view *v, HDC dc) {
	v->has_callout = false;
	if (!v->nmarkers) return;
	void *g = graphics(dc);
	if (!g) return;
	gp.SetInterpolationMode(g, 7);  // HighQualityBicubic
	item *it;
	size_t n = items(v, &it);
	const marker *sel = NULL;
	double sel_x = 0, sel_top = 0;
	for (size_t i = 0; i < n; i++) {
		double x0, y0, x1, y1;
		bool image;
		box(v, &it[i], &x0, &y0, &x1, &y1, &image);
		if (it[i].cluster) {
			paint_cluster(v, dc, g, &v->groups[it[i].i], x0, y0, x1, y1);
			continue;
		}
		const marker *m = &v->markers[it[i].i];
		if (image) gp.DrawImageRect(g, m->im->bmp, (REAL)x0, (REAL)y0, (REAL)(x1 - x0), (REAL)(y1 - y0));
		else if (!m->im || m->im->state == 2) paint_pin(v, g, m, x0, y0, x1, y1);
		else continue;  // its image is coming
		if (v->selected && !strcmp(m->id, v->selected) && (*m->title || *m->subtitle)) {
			sel = m;
			sel_x = (x0 + x1) / 2;
			sel_top = y0;
		}
	}
	free(it);
	if (sel) paint_callout(v, dc, g, sel, sel_x, sel_top);
	gp.DeleteGraphics(g);
}

// paint_attribution draws "© OpenStreetMap contributors" at the bottom
// right, as OpenStreetMap asks of maps of its data.
static void paint_attribution(view *v, HDC dc) {
	RECT a = attribution(v);
	double k = scale(v);
	void *g = graphics(dc);
	if (g) {
		void *b = NULL;
		gp.CreateSolidFill(argb(190, 255, 255, 255), &b);
		gp.FillRectangle(g, b, (REAL)a.left, (REAL)a.top, (REAL)(a.right - a.left), (REAL)(a.bottom - a.top));
		gp.DeleteBrush(b);
		gp.DeleteGraphics(g);
	}
	HFONT f = font(k, 11, 400);
	HGDIOBJ old = SelectObject(dc, f);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(0x33, 0x33, 0x33));
	RECT r = a;
	r.right -= (LONG)lround(5 * k);
	DrawTextW(dc, ATTRIBUTION, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_RIGHT | DT_VCENTER);
	SelectObject(dc, old);
	DeleteObject(f);
}

// paint draws the map into dc: the tiles and shapes (turned, with a
// heading), the markers, a callout, the attribution; through a back
// buffer.
static void paint(view *v, HDC dc) {
	RECT r = client(v);
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	if (!v->back || v->back_size.cx != w || v->back_size.cy != h) {
		if (v->back) DeleteObject(v->back);
		void *bits;
		v->back = dib(w, h, &bits);
		v->back_size = (SIZE){w, h};
		if (!v->back) return;
	}
	HDC mem = CreateCompatibleDC(dc);
	HGDIOBJ old = SelectObject(mem, v->back);
	HBRUSH land = CreateSolidBrush(RGB(0xE8, 0xE4, 0xDE));  // before its tiles come
	FillRect(mem, &r, land);
	void *g = NULL;
	if (v->heading == 0 || !(g = graphics(mem))) {
		paint_tiles(v, mem, w, h);
		paint_shapes(v, mem, w, h);
	} else {
		// Unturned into a square that holds the window turned any way,
		// then turned onto it.
		int D = (int)ceil(hypot(w, h)) + 2;
		if (!v->frame || v->frame_size != D) {
			if (v->frame) DeleteObject(v->frame);
			v->frame = dib(D, D, &v->frame_bits);
			v->frame_size = D;
		}
		if (v->frame) {
			HDC fm = CreateCompatibleDC(dc);
			HGDIOBJ fold = SelectObject(fm, v->frame);
			RECT fr = {0, 0, D, D};
			FillRect(fm, &fr, land);
			paint_tiles(v, fm, D, D);
			paint_shapes(v, fm, D, D);
			GdiFlush();
			void *bmp = NULL;
			if (gp.CreateBitmapFromScan0(D, D, D * 4, 0x22009, v->frame_bits, &bmp) == 0) {  // 32bppRGB
				gp.SetInterpolationMode(g, 3);                                                // Bilinear
				gp.TranslateWorldTransform(g, (REAL)(w / 2.0), (REAL)(h / 2.0), 0);
				gp.RotateWorldTransform(g, (REAL)-v->heading, 0);
				gp.TranslateWorldTransform(g, (REAL)(-D / 2.0), (REAL)(-D / 2.0), 0);
				gp.DrawImageRect(g, bmp, 0, 0, (REAL)D, (REAL)D);
				gp.DisposeImage(bmp);
			}
			SelectObject(fm, fold);
			DeleteDC(fm);
		}
		gp.DeleteGraphics(g);
	}
	DeleteObject(land);
	paint_markers(v, mem);
	paint_attribution(v, mem);
	BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
	SelectObject(mem, old);
	DeleteDC(mem);
}

// ---- What the user does ----

static void select_marker(view *v, const char *id) {
	bool same = (!id && !v->selected) || (id && v->selected && !strcmp(id, v->selected));
	free(v->selected);
	v->selected = id ? asuka_strdup(id) : NULL;
	invalidate(v);
	if (!same) maps_View_selectedChange(v->events, id ? id : "");
}

// fit_members zooms in to a cluster's markers.
static void fit_members(view *v, const mg_group *gr) {
	mg_ll *pts = malloc(gr->n * sizeof *pts);
	for (size_t i = 0; i < gr->n; i++) pts[i] = (mg_ll){v->markers[gr->members[i]].lat, v->markers[gr->members[i]].lng};
	double x0, x1, y0, y1, w, h;
	mg_ll c;
	double z;
	size(v, &w, &h);
	if (mg_bounds(pts, gr->n, &x0, &x1, &y0, &y1) && (x1 - x0 > 1e-12 || y1 - y0 > 1e-12) &&
	    mg_fit(pts, gr->n, w, h, v->heading, 48, 48, 48, 48, max_zoom, &c, &z))
		go(v, mg_x(c.lng), mg_merc_y(c.lat), fmax(z, v->zoom), v->heading, true);
	free(pts);
}

// click is a click that didn't move the map: on the attribution, a
// callout, a marker or a cluster, a shape, or the map.
static void click(view *v, POINT p) {
	if (attribution_hit(v, p)) {
		ShellExecuteW(NULL, L"open", L"https://www.openstreetmap.org/copyright", NULL, NULL, SW_SHOWNORMAL);
		return;
	}
	if (v->has_callout && v->selected && in_rect(v->callout, p)) {
		char *id = asuka_strdup(v->selected);
		maps_View_calloutPress(v->events, id);
		free(id);
		return;
	}
	item it;
	if (item_at(v, p, &it)) {
		if (it.cluster) {
			const mg_group *gr = &v->groups[it.i];
			maps_list_string ids = {calloc(gr->n, sizeof(char *)), gr->n};
			for (size_t i = 0; i < gr->n; i++) ids.data[i] = asuka_strdup(v->markers[gr->members[i]].id);
			mg_group copy = {malloc(gr->n * sizeof(size_t)), gr->n, gr->at};
			memcpy(copy.members, gr->members, gr->n * sizeof(size_t));
			maps_View_clusterPress(v->events, ids);
			maps_list_string_free(&ids);
			if (!v->destroyed) fit_members(v, &copy);
			free(copy.members);
			return;
		}
		char *id = asuka_strdup(v->markers[it.i].id);
		maps_View_markerPress(v->events, id);
		if (!v->destroyed) select_marker(v, id);
		free(id);
		return;
	}
	if (v->selected) select_marker(v, NULL);
	const shape *s = shape_at(v, p);
	if (s) {
		char *id = asuka_strdup(s->id);
		maps_View_overlayPress(v->events, id);
		free(id);
		return;
	}
	maps_Coordinate c = at(v, p);
	maps_View_press(v->events, &c);
}

static POINT point_of(LPARAM lp) { return (POINT){(short)LOWORD(lp), (short)HIWORD(lp)}; }

// drag_to moves the marker being dragged to under the pointer.
static maps_Coordinate drag_to(view *v, POINT p) {
	maps_Coordinate c = at_xy(v, p.x + v->grab_x, p.y + v->grab_y);
	marker *m = &v->markers[v->mdrag];
	m->lat = c.lat;
	m->lng = c.lng;
	v->grouped = false;
	invalidate(v);
	return c;
}

static void end_drag(view *v, POINT p) {
	if (v->mdrag >= 0 && v->mdragging) {
		maps_Coordinate c = drag_to(v, p);
		char *id = asuka_strdup(v->markers[v->mdrag].id);
		v->mdrag = -1;
		v->mdragging = false;
		maps_View_markerDragEnd(v->events, id, &c);
		free(id);
	}
	v->mdrag = -1;
	v->mdragging = false;
}

static bool message(void *ctx, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, LRESULT *result) {
	view *v = ctx;
	*result = 0;
	switch (m) {
	case WM_NCDESTROY:
		v->hwnd = NULL;
		unref(v);
		return false;
	case WM_ERASEBKGND: *result = 1; return true;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		paint(v, dc);
		EndPaint(hwnd, &ps);
		return true;
	}
	case WM_SIZE:
		if (v->pending) apply(v);
		else clamp(v);
		invalidate(v);
		return false;
	case WM_GETDLGCODE: *result = DLGC_WANTARROWS | DLGC_WANTCHARS; return true;
	case WM_SETCURSOR:
		if (v->interactive && LOWORD(lp) == HTCLIENT) {
			POINT p;
			GetCursorPos(&p);
			ScreenToClient(hwnd, &p);
			item it;
			bool hand = attribution_hit(v, p) || (v->has_callout && in_rect(v->callout, p)) || item_at(v, p, &it);
			SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(hand ? 32649 : 32512)));  // IDC_HAND, IDC_ARROW
			*result = 1;
			return true;
		}
		return false;
	case WM_LBUTTONDOWN: {
		POINT p = point_of(lp);
		SetFocus(hwnd);
		if (!v->interactive) return true;
		SetCapture(hwnd);
		stop_anim(v);
		v->down = true;
		v->dragging = false;
		v->down_at = p;
		v->down_cx = v->cx;
		v->down_cy = v->cy;
		v->mdrag = -1;
		item it;
		bool on = item_at(v, p, &it);
		if (on && !it.cluster && v->markers[it.i].draggable) {
			double x, y;
			point(v, v->markers[it.i].lat, v->markers[it.i].lng, &x, &y);
			v->mdrag = (long)it.i;
			v->grab_x = x - p.x;
			v->grab_y = y - p.y;
		}
		v->doubled = v->last_up && now_ms() - v->last_up < GetDoubleClickTime() && labs(p.x - v->last_up_at.x) <= 4 && labs(p.y - v->last_up_at.y) <= 4;
		if (v->doubled && !on && !attribution_hit(v, p)) animate_zoom(v, wp & MK_SHIFT ? -1 : 1, p);
		return true;
	}
	case WM_MOUSEMOVE: {
		if (!v->down) return false;
		POINT p = point_of(lp);
		bool away = labs(p.x - v->down_at.x) > 3 || labs(p.y - v->down_at.y) > 3;
		if (v->mdrag >= 0) {
			if (!v->mdragging && away) {
				v->mdragging = true;
				double x, y;
				point(v, v->markers[v->mdrag].lat, v->markers[v->mdrag].lng, &x, &y);
				maps_Coordinate c = at_xy(v, x, y);
				char *id = asuka_strdup(v->markers[v->mdrag].id);
				maps_View_markerDragStart(v->events, id, &c);
				free(id);
			}
			if (v->mdragging && v->mdrag >= 0) {
				maps_Coordinate c = drag_to(v, p);
				char *id = asuka_strdup(v->markers[v->mdrag].id);
				maps_View_markerDrag(v->events, id, &c);
				free(id);
			}
			return true;
		}
		if (!v->dragging && away) v->dragging = true;
		if (v->dragging) {
			v->cx = v->down_cx;
			v->cy = v->down_cy;
			pan_by(v, p.x - v->down_at.x, p.y - v->down_at.y);
		}
		return true;
	}
	case WM_LBUTTONUP: {
		if (!v->down) return false;
		POINT p = point_of(lp);
		v->down = false;
		ReleaseCapture();
		if (v->mdragging) {
			end_drag(v, p);
			return true;
		}
		v->mdrag = -1;
		if (v->dragging) {
			v->dragging = false;
			settle(v);
			return true;
		}
		if (v->doubled) {
			v->doubled = false;
			v->last_up = 0;
			return true;
		}
		v->last_up = now_ms();
		v->last_up_at = p;
		click(v, p);
		return true;
	}
	case WM_RBUTTONUP: {
		POINT p = point_of(lp);
		item it;
		if (!v->interactive || item_at(v, p, &it)) return true;
		maps_Coordinate c = at(v, p);
		maps_View_longPress(v->events, &c);
		return true;
	}
	case WM_CAPTURECHANGED:
		if (v->down && (HWND)lp != hwnd) {
			POINT p;
			GetCursorPos(&p);
			ScreenToClient(hwnd, &p);
			end_drag(v, p);
			v->down = v->dragging = false;
			settle(v);
		}
		return false;
	case WM_MOUSEWHEEL: {
		if (!v->interactive) return false;
		stop_anim(v);
		double delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.0;
		POINT p = point_of(lp);
		ScreenToClient(hwnd, &p);
		// A notch is half a level; a touchpad's pinch (Ctrl) comes in
		// small steps, as fine as it is.
		zoom_by(v, delta / 2, p, true);
		SetTimer(hwnd, timer_wheel, 300, NULL);
		return true;
	}
	case WM_TIMER:
		if (wp == timer_wheel) {
			KillTimer(hwnd, timer_wheel);
			settle(v);
		} else if (wp == timer_anim) {
			if (v->anim.on) step_anim(v);
			else KillTimer(hwnd, timer_anim);
		} else if (wp == timer_idle) {
			settle(v);
		}
		return true;
	case WM_KEYDOWN: {
		if (!v->interactive) return false;
		RECT r = client(v);
		LONG w = r.right - r.left, h = r.bottom - r.top;
		POINT mid = {w / 2, h / 2};
		double step = (w < h ? w : h) / 8.0;
		switch (wp) {
		case VK_OEM_PLUS:
		case VK_ADD: animate_zoom(v, 1, mid); return true;
		case VK_OEM_MINUS:
		case VK_SUBTRACT: animate_zoom(v, -1, mid); return true;
		case VK_LEFT: pan_by(v, step, 0); break;
		case VK_RIGHT: pan_by(v, -step, 0); break;
		case VK_UP: pan_by(v, 0, step); break;
		case VK_DOWN: pan_by(v, 0, -step); break;
		default: return false;
		}
		settle(v);
		return true;
	}
	case WM_GESTURE: {
		GESTUREINFO gi = {sizeof gi};
		if (!GetGestureInfo((HGESTUREINFO)lp, &gi) || !v->interactive) return false;
		POINT p = {gi.ptsLocation.x, gi.ptsLocation.y};
		ScreenToClient(hwnd, &p);
		switch (gi.dwID) {
		case GID_ZOOM:
			if (!(gi.dwFlags & GF_BEGIN) && v->gesture_arg > 0 && gi.ullArguments > 0)
				zoom_by(v, log2((double)gi.ullArguments / v->gesture_arg), p, true);
			v->gesture_arg = gi.ullArguments;
			break;
		case GID_PAN:
			if (v->mdrag >= 0) break;  // a marker's drag, not the map's
			if (!(gi.dwFlags & GF_BEGIN)) pan_by(v, p.x - v->gesture.x, p.y - v->gesture.y);
			v->gesture = p;
			break;
		case GID_END:
			v->gesture_arg = 0;
			settle(v);
			break;
		default: return false;
		}
		CloseGestureInfoHandle((HGESTUREINFO)lp);
		return true;
	}
	}
	return false;
}

// ---- The package's View: its props ----

static void set_region(maps_View *self, const maps_Region *r) {
	view *v = (view *)self;
	if (r->latDelta <= 0 && r->lngDelta <= 0 && r->center.lat == 0 && r->center.lng == 0) return;
	v->pending = 1;
	v->pending_region = *r;
	apply(v);
}

static void set_camera(maps_View *self, const maps_Camera *c) {
	view *v = (view *)self;
	if (c->zoom <= 0) return;
	v->pending = 2;
	v->pending_camera = *c;
	apply(v);
}

static void set_markers(maps_View *self, maps_list_Marker ms) {
	view *v = (view *)self;
	if (v->mdrag >= 0) {
		v->mdrag = -1;
		v->mdragging = false;
	}
	free_markers(v);
	v->markers = calloc(ms.len ? ms.len : 1, sizeof *v->markers);
	v->nmarkers = ms.len;
	for (size_t i = 0; i < ms.len; i++) {
		const maps_Marker *m = &ms.data[i];
		marker *mk = &v->markers[i];
		mk->id = asuka_strdup(m->id);
		mk->title = asuka_strdup(m->title);
		mk->subtitle = asuka_strdup(m->subtitle);
		mk->lat = m->at.lat;
		mk->lng = m->at.lng;
		mk->z = m->zIndex;
		mk->w = m->width;
		mk->h = m->height;
		mk->ax = m->anchor.x;
		mk->ay = m->anchor.y;
		mk->draggable = m->draggable;
		mk->clusters = m->clusters;
		if ((mk->colored = m->color != NULL)) mk->rgba = asuka_desktop_color((uint64_t)*m->color);
		mk->im = m->image && *m->image ? image_of(m->image) : NULL;
	}
	v->grouped = false;
	invalidate(v);
}

static void set_polygons(maps_View *self, maps_list_Polygon ps) {
	view *v = (view *)self;
	clear_kind(v, 0);
	for (size_t i = 0; i < ps.len; i++) {
		const maps_Polygon *p = &ps.data[i];
		shape *s = add_shape(v, 0, p->id, p->zIndex, i, 1 + p->holes.len);
		mg_ll *pts = lls(p->points);
		mg_ring(&s->rings[0], pts, p->points.len);
		free(pts);
		for (size_t h = 0; h < p->holes.len; h++) {
			pts = lls(p->holes.data[h]);
			mg_path *ring = &s->rings[1 + h];
			mg_ring(ring, pts, p->holes.data[h].len);
			free(pts);
			// The same world as the outline.
			if (ring->n && s->rings[0].n) {
				double d = mg_shift(ring->p[0].x, s->rings[0].p[0].x);
				for (size_t j = 0; j < ring->n; j++) ring->p[j].x += d;
			}
		}
		s->fill = argb_of(asuka_desktop_color((uint64_t)p->fill));
		s->stroke = argb_of(asuka_desktop_color((uint64_t)p->stroke));
		s->width = p->strokeWidth;
	}
	if (v->nshapes > 1) qsort(v->shapes, v->nshapes, sizeof *v->shapes, cmp_shape);
	invalidate(v);
}

static void set_circles(maps_View *self, maps_list_Circle cs) {
	view *v = (view *)self;
	clear_kind(v, 1);
	for (size_t i = 0; i < cs.len; i++) {
		const maps_Circle *c = &cs.data[i];
		shape *s = add_shape(v, 1, c->id, c->zIndex, i, 1);
		if (c->radius > 0) mg_circle(&s->rings[0], (mg_ll){c->center.lat, c->center.lng}, c->radius);
		s->fill = argb_of(asuka_desktop_color((uint64_t)c->fill));
		s->stroke = argb_of(asuka_desktop_color((uint64_t)c->stroke));
		s->width = c->strokeWidth;
	}
	if (v->nshapes > 1) qsort(v->shapes, v->nshapes, sizeof *v->shapes, cmp_shape);
	invalidate(v);
}

static void set_polylines(maps_View *self, maps_list_Polyline ls) {
	view *v = (view *)self;
	clear_kind(v, 2);
	for (size_t i = 0; i < ls.len; i++) {
		const maps_Polyline *l = &ls.data[i];
		shape *s = add_shape(v, 2, l->id, l->zIndex, i, 1);
		mg_ll *pts = lls(l->points);
		mg_line(&s->rings[0], pts, l->points.len, l->geodesic);
		free(pts);
		s->stroke = argb_of(asuka_desktop_color((uint64_t)l->color));
		s->width = l->width;
		s->dashed = l->dashed;
	}
	if (v->nshapes > 1) qsort(v->shapes, v->nshapes, sizeof *v->shapes, cmp_shape);
	invalidate(v);
}

static void set_selected(maps_View *self, const char *id) {
	view *v = (view *)self;
	free(v->selected);
	v->selected = id && *id ? asuka_strdup(id) : NULL;
	invalidate(v);
}

static void set_clustering(maps_View *self, bool on) {
	view *v = (view *)self;
	v->clustering = on;
	v->grouped = false;
	invalidate(v);
}

static void set_cluster_color(maps_View *self, const int64_t *c) {
	view *v = (view *)self;
	if ((v->cluster_colored = c != NULL)) v->cluster_rgba = asuka_desktop_color((uint64_t)*c);
	invalidate(v);
}

static void set_map_type(maps_View *self, maps_MapType t) {}  // OpenStreetMap's streets only
static void set_shows_user(maps_View *self, bool on) {}
static void set_interactive(maps_View *self, bool on) { ((view *)self)->interactive = on; }

// measure: none of its own; its style gives it a size.
static bool measure(maps_View *self, float w, float h, float *ow, float *oh) {
	*ow = isnan(w) ? 0 : w;
	*oh = isnan(h) ? 0 : h;
	return true;
}

static void destroy(maps_View *self) {
	view *v = (view *)self;
	v->destroyed = true;
	stop_anim(v);
	for (size_t i = 0; i < T.nviews; i++)
		if (T.views[i] == v) {
			T.views[i] = T.views[--T.nviews];
			break;
		}
	unref(v);
}

static const maps_View_vtable vt = {
	.destroy = destroy,
	.set_region = set_region,
	.set_camera = set_camera,
	.set_markers = set_markers,
	.set_polylines = set_polylines,
	.set_polygons = set_polygons,
	.set_circles = set_circles,
	.set_selected = set_selected,
	.set_clustering = set_clustering,
	.set_clusterColor = set_cluster_color,
	.set_mapType = set_map_type,
	.set_showsUser = set_shows_user,
	.set_interactive = set_interactive,
	.measure = measure,
};

maps_View *maps_make_View(maps_View_events events) {
	tiles();
	view *v = calloc(1, sizeof *v);
	v->base.vt = &vt;
	v->events = events;
	v->refs = 1;
	v->interactive = true;
	v->cx = v->cy = 0.5;
	v->zoom = 1;
	v->mdrag = -1;
	T.views = realloc(T.views, (T.nviews + 1) * sizeof *T.views);
	T.views[T.nviews++] = v;
	v->hwnd = asuka_win_window(0, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, (HWND)events.parent, message, v);
	if (v->hwnd) {
		v->refs++;  // the window's, until WM_NCDESTROY
		// Pinches and one finger's pans, with inertia; no rotation.
		GESTURECONFIG c[3] = {{GID_ZOOM, GC_ZOOM, 0}, {GID_ROTATE, 0, GC_ROTATE},
		                      {GID_PAN, GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_INERTIA,
		                       GC_PAN_WITH_GUTTER}};
		SetGestureConfig(v->hwnd, 0, 3, c, sizeof *c);
	}
	v->base.widget = v->hwnd;
	return &v->base;
}

// ---- Maps: the functions that move a map, by the Ref it's in ----

// find is the map a Ref holds: the view made for that node.
static view *find(uint32_t id) {
	for (size_t i = 0; id && i < T.nviews; i++)
		if (T.views[i]->events.view == id && !T.views[i]->destroyed) return T.views[i];
	return NULL;
}

static void m_move_to(maps_Maps *self, uint32_t id, const maps_Region *r, bool animated, asuka_error *err) {
	view *v = find(id);
	if (!v) return;
	double w, h;
	size(v, &w, &h);
	if (w <= 0 || h <= 0) {
		v->pending = 1;
		v->pending_region = *r;
		return;
	}
	go(v, mg_x(r->center.lng), mg_merc_y(r->center.lat), mg_zoom_for(r->center.lat, r->latDelta, r->lngDelta, w, h, max_zoom), v->heading, animated);
}

static void m_move_camera(maps_Maps *self, uint32_t id, const maps_Camera *c, bool animated, asuka_error *err) {
	view *v = find(id);
	if (!v) return;
	go(v, mg_x(c->center.lng), mg_merc_y(c->center.lat), c->zoom > 0 ? c->zoom : v->zoom, c->heading, animated);
}

static void m_fit(maps_Maps *self, uint32_t id, maps_list_Coordinate cs, const maps_Insets *p, bool animated, asuka_error *err) {
	view *v = find(id);
	if (!v || !cs.len) return;
	double w, h, z;
	size(v, &w, &h);
	mg_ll *pts = lls(cs), c;
	if (w > 0 && h > 0 && mg_fit(pts, cs.len, w, h, v->heading, p->top, p->left, p->bottom, p->right, max_zoom, &c, &z))
		go(v, mg_x(c.lng), mg_merc_y(c.lat), z, v->heading, animated);
	free(pts);
}

static maps_Camera m_camera(maps_Maps *self, uint32_t id, asuka_error *err) {
	view *v = find(id);
	maps_Camera c = {0};
	maps_Region r;
	if (v) now(v, &c, &r);
	return c;
}

static void m_release(maps_Maps *self) {}

static const maps_Maps_vtable maps_vt = {
	.release = m_release,
	.moveTo = m_move_to,
	.moveCamera = m_move_camera,
	.fit = m_fit,
	.camera = m_camera,
};

maps_Maps *maps_make_Maps(asuka_error *err) {
	static maps_Maps m = {&maps_vt};
	return &m;
}
