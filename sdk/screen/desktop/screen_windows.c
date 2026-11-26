// platform/screen on Windows: the monitors attached to the desktop
// (EnumDisplayMonitors, GetMonitorInfo), each at its effective DPI, named
// by its EDID (QueryDisplayConfig) and at its current mode's refresh
// rate. A change is WM_DISPLAYCHANGE, WM_SETTINGCHANGE (the work area) or
// WM_DPICHANGED, which the system window hears; the displays are read
// again then, and again a moment later, as some changes settle after the
// message. Capture is kept out with WDA_EXCLUDEFROMCAPTURE on the app's
// windows, those that show later too. A capture copies the monitor's
// pixels from the screen (BitBlt with CAPTUREBLT, so layered windows are
// in it), in physical pixels, and writes them as a PNG (WIC) on a thread
// of its own. Main thread, but the capture's work.
#include "asuka_windows.h"
#include "screen.asuka.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

typedef struct {
	wchar_t device[32];
	RECT bounds, work;
	int dpi;
	bool primary;
	char *name;
	double hz;
} monitor;

typedef struct {
	monitor *m;
	size_t n, cap;
} monitors;

static int monitor_dpi(HMONITOR h) {
	static HRESULT(WINAPI * get)(HMONITOR, int, UINT *, UINT *);
	static bool loaded;
	if (!loaded) {
		loaded = true;
		HMODULE shcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (shcore) *(FARPROC *)&get = GetProcAddress(shcore, "GetDpiForMonitor");
	}
	UINT x = 0, y = 0;
	if (get && get(h, 0, &x, &y) == S_OK && x) return (int)x;  // MDT_EFFECTIVE_DPI
	HDC dc = GetDC(NULL);
	int d = GetDeviceCaps(dc, LOGPIXELSX);
	ReleaseDC(NULL, dc);
	return d ? d : 96;
}

static BOOL CALLBACK each(HMONITOR h, HDC dc, LPRECT r, LPARAM p) {
	monitors *ms = (monitors *)p;
	MONITORINFOEXW mi;
	memset(&mi, 0, sizeof mi);
	mi.cbSize = sizeof mi;
	if (!GetMonitorInfoW(h, (MONITORINFO *)&mi)) return TRUE;
	if (ms->n == ms->cap) {
		ms->cap = ms->cap ? ms->cap * 2 : 4;
		ms->m = realloc(ms->m, ms->cap * sizeof *ms->m);
	}
	monitor *m = &ms->m[ms->n++];
	memset(m, 0, sizeof *m);
	wcsncpy(m->device, mi.szDevice, 31);
	m->bounds = mi.rcMonitor;
	m->work = mi.rcWork;
	m->dpi = monitor_dpi(h);
	m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	return TRUE;
}

// names fills the monitors' names and refresh rates from the active
// display paths (QueryDisplayConfig), by their sources' GDI names.
static void names(monitors *ms) {
	UINT32 np = 0, nm = 0;
	DISPLAYCONFIG_PATH_INFO *paths = NULL;
	DISPLAYCONFIG_MODE_INFO *modes = NULL;
	LONG r = ERROR_INSUFFICIENT_BUFFER;
	for (int i = 0; i < 3 && r == ERROR_INSUFFICIENT_BUFFER; i++) {  // the configuration can change between the calls
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) return;
		free(paths);
		free(modes);
		paths = calloc(np, sizeof *paths);
		modes = calloc(nm ? nm : 1, sizeof *modes);
		r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL);
	}
	for (UINT32 i = 0; r == ERROR_SUCCESS && i < np; i++) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
		memset(&src, 0, sizeof src);
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof src;
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
		DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
		memset(&tgt, 0, sizeof tgt);
		tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tgt.header.size = sizeof tgt;
		tgt.header.adapterId = paths[i].targetInfo.adapterId;
		tgt.header.id = paths[i].targetInfo.id;
		bool named = DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS && tgt.monitorFriendlyDeviceName[0];
		bool internal = paths[i].targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
		DISPLAYCONFIG_RATIONAL rate = paths[i].targetInfo.refreshRate;
		for (size_t k = 0; k < ms->n; k++) {
			monitor *m = &ms->m[k];
			if (m->name || wcscmp(m->device, src.viewGdiDeviceName) != 0) continue;  // a clone's second target: the first's
			if (named) m->name = asuka_narrow(tgt.monitorFriendlyDeviceName, -1);
			else if (internal) m->name = asuka_strdup("Built-in Display");
			if (rate.Denominator) m->hz = (int)((double)rate.Numerator / rate.Denominator * 100 + 0.5) / 100.0;
		}
	}
	free(paths);
	free(modes);
}

static int cmp(const void *a, const void *b) {
	const monitor *x = a, *y = b;
	if (x->primary != y->primary) return x->primary ? -1 : 1;
	return wcscmp(x->device, y->device);
}

// read_displays places the monitors in points: each one's origin is its
// top left corner in pixels over the smallest scale of the monitors, and
// it goes on in its own scale (its size, its work area's place in it), so
// monitors never overlap.
static screen_list_Display read_displays(void) {
	monitors ms = {0};
	EnumDisplayMonitors(NULL, NULL, each, (LPARAM)&ms);
	names(&ms);
	qsort(ms.m, ms.n, sizeof *ms.m, cmp);
	screen_list_Display out = {calloc(ms.n ? ms.n : 1, sizeof *out.data), ms.n};
	double least = 0;
	for (size_t i = 0; i < ms.n; i++) {
		double s = ms.m[i].dpi / 96.0;
		if (least == 0 || s < least) least = s;
	}
	for (size_t i = 0; i < ms.n; i++) {
		monitor *m = &ms.m[i];
		if (!m->name) {
			DISPLAY_DEVICEW dd;
			memset(&dd, 0, sizeof dd);
			dd.cb = sizeof dd;
			m->name = EnumDisplayDevicesW(m->device, 0, &dd, 0) ? asuka_narrow(dd.DeviceString, -1) : asuka_strdup("");
		}
		if (m->hz == 0) {
			DEVMODEW dm;
			memset(&dm, 0, sizeof dm);
			dm.dmSize = sizeof dm;
			if (EnumDisplaySettingsW(m->device, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) m->hz = dm.dmDisplayFrequency;
		}
		double k = m->dpi / 96.0;
		double x = m->bounds.left / least, y = m->bounds.top / least;
		screen_Display *d = &out.data[i];
		d->id = asuka_narrow(m->device, -1);
		d->name = m->name;
		d->bounds = (screen_Rect){x, y, (m->bounds.right - m->bounds.left) / k, (m->bounds.bottom - m->bounds.top) / k};
		d->workArea = (screen_Rect){x + (m->work.left - m->bounds.left) / k, y + (m->work.top - m->bounds.top) / k, (m->work.right - m->work.left) / k,
		                            (m->work.bottom - m->work.top) / k};
		d->scale = k;
		d->refreshRate = m->hz;
		d->primary = m->primary;
	}
	free(ms.m);
	return out;
}

static screen_list_Display s_displays(screen_Screens *self, asuka_error *err) { return read_displays(); }

typedef struct {
	screen_fn_list_Display changed;
	asuka_call *call;
	int id;
	UINT_PTR timer;
	screen_list_Display last;
	bool first;
} watcher;

static bool same(const screen_list_Display *a, const screen_list_Display *b) {
	if (a->len != b->len) return false;
	for (size_t i = 0; i < a->len; i++) {
		const screen_Display *x = &a->data[i], *y = &b->data[i];
		if (strcmp(x->id, y->id) || strcmp(x->name, y->name) || memcmp(&x->bounds, &y->bounds, sizeof x->bounds) ||
		    memcmp(&x->workArea, &y->workArea, sizeof x->workArea) || x->scale != y->scale || x->refreshRate != y->refreshRate || x->primary != y->primary)
			return false;
	}
	return true;
}

static void report(watcher *w) {
	screen_list_Display ds = read_displays();
	if (!w->first && same(&ds, &w->last)) {
		screen_list_Display_free(&ds);
		return;
	}
	w->first = false;
	screen_list_Display_free(&w->last);
	w->last = ds;
	screen_fn_list_Display_call(w->changed, ds);
}

static watcher *settling;  // the watcher the timer reads again for (one timer per change, the last)

static void CALLBACK again(HWND hwnd, UINT m, UINT_PTR id, DWORD t) {
	KillTimer(NULL, id);
	if (settling && settling->timer == id) {
		settling->timer = 0;
		report(settling);
	}
}

static void on_message(void *ctx, UINT m, WPARAM wp, LPARAM lp) {
	watcher *w = ctx;
	if (m != WM_DISPLAYCHANGE && m != WM_SETTINGCHANGE && m != WM_DPICHANGED) return;
	report(w);
	if (w->timer) KillTimer(NULL, w->timer);
	w->timer = SetTimer(NULL, 0, 250, again);
	settling = w;
}

static void stop(void *p) {
	watcher *w = p;
	asuka_win_system_message_stop(w->id);
	if (w->timer) KillTimer(NULL, w->timer);
	if (settling == w) settling = NULL;
	screen_list_Display_free(&w->last);
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(w->call, &e);
	free(w);
}

static void s_watch(screen_Screens *self, screen_fn_list_Display f, asuka_call *call) {
	watcher *w = calloc(1, sizeof *w);
	w->changed = f;
	w->call = call;
	w->first = true;
	w->id = asuka_win_system_message(on_message, w);
	report(w);
	asuka_call_on_cancel(call, stop, w);
}

static void s_lock(screen_Screens *self, screen_Orientation o, asuka_error *err) {}
static void s_bars(screen_Screens *self, screen_BarStyle s, bool a, bool b, asuka_error *err) {}

static struct {
	bool on;
	HWINEVENTHOOK hook;
} capture;

static void exclude(HWND hwnd, bool on) {
	static BOOL(WINAPI * set)(HWND, DWORD);
	static bool loaded;
	if (!loaded) {
		loaded = true;
		*(FARPROC *)&set = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
	}
	if (!set) return;
	DWORD a = 0;  // WDA_NONE
	if (on) {
		OSVERSIONINFOW v = {sizeof v};
		RTL_OSVERSIONINFOW rv = {sizeof rv};
		LONG(WINAPI * rtl)(RTL_OSVERSIONINFOW *) = NULL;
		*(FARPROC *)&rtl = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
		DWORD build = rtl && rtl(&rv) == 0 ? rv.dwBuildNumber : 0;
		(void)v;
		a = build >= 19041 ? 0x11 : 0x01;  // WDA_EXCLUDEFROMCAPTURE, else WDA_MONITOR
	}
	set(hwnd, a);
}

static BOOL CALLBACK each_window(HWND hwnd, LPARAM on) {
	if (IsWindowVisible(hwnd)) exclude(hwnd, on != 0);
	return TRUE;
}

// shown: the windows that show later too (a sheet, a Window).
static void CALLBACK shown(HWINEVENTHOOK h, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD thread, DWORD t) {
	if (obj != OBJID_WINDOW || child != CHILDID_SELF) return;
	if (capture.on && GetAncestor(hwnd, GA_ROOT) == hwnd) exclude(hwnd, true);
}

static void s_capture(screen_Screens *self, bool on, asuka_error *err) {
	capture.on = on;
	EnumThreadWindows(GetCurrentThreadId(), each_window, on);
	if (on && !capture.hook) {
		capture.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, shown, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
	} else if (!on && capture.hook) {
		UnhookWinEvent(capture.hook);
		capture.hook = NULL;
	}
}

static void stop_shots(void *call) {
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(call, &e);
}

// A desktop doesn't tell an app of its screenshots: it waits for the end.
static void s_shots(screen_Screens *self, screen_fn_int taken, asuka_call *call) { asuka_call_on_cancel(call, stop_shots, call); }

// The display's brightness is the user's to set on a desktop: the app
// neither sets nor reads it.
static void s_set_brightness(screen_Screens *self, double level, asuka_error *err) { (void)self, (void)level, (void)err; }
static double s_brightness(screen_Screens *self, asuka_error *err) { (void)self, (void)err; return -1; }

// ---- Capture ----

// physical has the calling thread see the monitors in physical pixels
// (per-monitor aware, as the host makes the process), returning what it
// saw them in before, for unphysical: NULL where Windows can't (before
// 10's 1607, where a process that's aware is so on every thread).
typedef DPI_AWARENESS_CONTEXT(WINAPI *set_thread_dpi)(DPI_AWARENESS_CONTEXT);
static set_thread_dpi thread_dpi(void) {
	static set_thread_dpi set;
	static atomic_bool loaded;
	if (!atomic_load(&loaded)) {
		*(FARPROC *)&set = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
		atomic_store(&loaded, true);
	}
	return set;
}
static DPI_AWARENESS_CONTEXT physical(void) {
	set_thread_dpi set = thread_dpi();
	return set ? set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;
}
static void unphysical(DPI_AWARENESS_CONTEXT was) {
	set_thread_dpi set = thread_dpi();
	if (set && was) set(was);
}

typedef struct {
	const wchar_t *device;  // NULL: the primary
	RECT r;
	bool found;
} finding;

static BOOL CALLBACK find_each(HMONITOR h, HDC dc, LPRECT r, LPARAM p) {
	finding *f = (finding *)p;
	MONITORINFOEXW mi;
	memset(&mi, 0, sizeof mi);
	mi.cbSize = sizeof mi;
	if (!GetMonitorInfoW(h, (MONITORINFO *)&mi)) return TRUE;
	if (f->device ? wcscmp(mi.szDevice, f->device) != 0 : !(mi.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
	f->r = mi.rcMonitor;
	f->found = true;
	return FALSE;
}

// find_monitor is the rectangle of the monitor whose device name is
// display ("": the primary), in physical pixels, in the virtual screen's
// coordinates: false if none is.
static bool find_monitor(const char *display, RECT *r) {
	finding f = {0};
	wchar_t *w = display && *display ? asuka_wide(display) : NULL;
	f.device = w;
	DPI_AWARENESS_CONTEXT was = physical();
	EnumDisplayMonitors(NULL, NULL, find_each, (LPARAM)&f);
	unphysical(was);
	free(w);
	*r = f.r;
	return f.found && f.r.right > f.r.left && f.r.bottom > f.r.top;
}

// shot_path is a new file's name in the app's caches (dir), made unique
// by the time, the process and a count.
static char *shot_path(const char *dir) {
	static atomic_uint count;
	char *sub = asuka_sprintf("%s\\asuka-screen", dir);
	wchar_t *w = asuka_wide(sub);
	CreateDirectoryW(w, NULL);  // there already, or made
	free(w);
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	char *path = asuka_sprintf("%s\\capture-%llx-%lx-%x.png", sub, t, (unsigned long)GetCurrentProcessId(), atomic_fetch_add(&count, 1) + 1);
	free(sub);
	return path;
}

// shoot captures the display ("": the primary) as a PNG in dir: its path
// (malloc's), or NULL with err set (CaptureError.notAvailable when
// there's no such display). Any thread; *width and *height its pixels.
static char *shoot(const char *display, const char *dir, int *width, int *height, asuka_error *err) {
	RECT r;
	if (!find_monitor(display, &r)) {
		asuka_error_declared(err, screen_CaptureError_notAvailable);
		return NULL;
	}
	int w = r.right - r.left, h = r.bottom - r.top;
	DPI_AWARENESS_CONTEXT was = physical();  // the screen's DC in physical pixels too
	HDC screen = GetDC(NULL);
	HDC mem = screen ? CreateCompatibleDC(screen) : NULL;
	BITMAPINFO bi;
	memset(&bi, 0, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;  // top down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP bmp = mem ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
	bool copied = false;
	if (bmp && bits) {
		HGDIOBJ old = SelectObject(mem, bmp);
		copied = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != 0;
		GdiFlush();
		SelectObject(mem, old);
	}
	DWORD why = copied ? 0 : GetLastError();
	if (mem) DeleteDC(mem);
	if (screen) ReleaseDC(NULL, screen);
	unphysical(was);
	if (!copied) {
		if (bmp) DeleteObject(bmp);
		asuka_win_fail(err, HRESULT_FROM_WIN32(why ? why : ERROR_ACCESS_DENIED), "screen: can't copy the screen");
		return NULL;
	}
	uint8_t *px = bits;
	for (size_t i = 0, n = (size_t)w * h * 4; i < n; i += 4) px[i + 3] = 255;  // GDI leaves alpha 0: opaque
	asuka_bytes png = {0};
	bool encoded = asuka_win_png((UINT)w, (UINT)h, px, &png);
	DeleteObject(bmp);
	if (!encoded) {
		asuka_error_failed(err, "screen", 0, "screen: can't encode the capture as a PNG");
		return NULL;
	}
	char *path = shot_path(dir);
	wchar_t *wp = asuka_wide(path);
	FILE *f = _wfopen(wp, L"wb");
	free(wp);
	bool ok = f && fwrite(png.data, 1, png.len, f) == png.len;
	if (f && fclose(f) != 0) ok = false;
	free(png.data);
	if (!ok) {
		asuka_error_failed(err, "screen", 0, "screen: can't write %s", path);
		free(path);
		return NULL;
	}
	*width = w;
	*height = h;
	return path;
}

typedef struct {
	asuka_call *call;
	char *display, *dir;
} shot;

static void shoot_off_main(void *p) {
	shot *s = p;
	asuka_error err = {0};
	int w, h;
	char *path = shoot(s->display, s->dir, &w, &h, &err);
	if (path) screen_Screens_capture_done(s->call, path);  // takes path
	else asuka_call_fail(s->call, &err);
	free(s->display);
	free(s->dir);
	free(s);
}

// A display that isn't there fails at once; the copy and its PNG are a
// worker's (a 4K screen's PNG takes a while).
static void s_shoot(screen_Screens *self, const char *display, asuka_call *call) {
	RECT r;
	if (!find_monitor(display, &r)) {
		asuka_error err = {0};
		asuka_error_declared(&err, screen_CaptureError_notAvailable);
		asuka_call_fail(call, &err);
		return;
	}
	shot *s = calloc(1, sizeof *s);
	s->call = call;
	s->display = asuka_strdup(display ? display : "");
	s->dir = asuka_strdup(asuka_desktop_cache_dir());
	if (!asuka_desktop_spawn(shoot_off_main, s)) shoot_off_main(s);
}

static const screen_Screens_vtable vt = {.displays = s_displays, .watchDisplays = s_watch, .lock = s_lock, .setBars = s_bars, .preventCapture = s_capture,
	.setBrightness = s_set_brightness, .brightness = s_brightness, .watchScreenshots = s_shots, .capture = s_shoot};

screen_Screens *screen_make_Screens(asuka_error *err) {
	screen_Screens *s = malloc(sizeof *s);
	s->vt = &vt;
	return s;
}
