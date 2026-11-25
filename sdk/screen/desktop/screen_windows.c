// platform/screen on Windows: the monitors attached to the desktop
// (EnumDisplayMonitors, GetMonitorInfo), each at its effective DPI, named
// by its EDID (QueryDisplayConfig) and at its current mode's refresh
// rate. A change is WM_DISPLAYCHANGE, WM_SETTINGCHANGE (the work area) or
// WM_DPICHANGED, which the system window hears; the displays are read
// again then, and again a moment later, as some changes settle after the
// message. Capture is kept out with WDA_EXCLUDEFROMCAPTURE on the app's
// windows, those that show later too. Main thread.
#include "asuka_windows.h"
#include "screen.asuka.h"

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

static const screen_Screens_vtable vt = {.displays = s_displays, .watchDisplays = s_watch, .lock = s_lock, .setBars = s_bars, .preventCapture = s_capture,
	.setBrightness = s_set_brightness, .brightness = s_brightness, .watchScreenshots = s_shots};

screen_Screens *screen_make_Screens(asuka_error *err) {
	screen_Screens *s = malloc(sizeof *s);
	s->vt = &vt;
	return s;
}
