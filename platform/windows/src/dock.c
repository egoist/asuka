// dock.c: platform/dock on Windows, the taskbar button of the app's main
// window, and the app's badge (platform/notify's setBadge).
//
// "progress" is ITaskbarList3's progress (normal, indeterminate, paused in
// yellow, error in red), set again when Explorer makes the button again
// (TaskbarButtonCreated: the window showing, Explorer restarting).
// "attention" flashes the button of the window in use (FlashWindowEx):
// once for an informational request, which leaves the button lit until
// the app's used, and with its title bar until the window comes to the
// foreground for a critical one; nothing while the app is in the
// foreground.
//
// The badge: for an app with a package identity, its tile's and taskbar
// button's (BadgeUpdateManager); for one without, which has no badge of
// the system's, an overlay icon on its taskbar button with the count
// drawn on it, as Teams and Outlook do.
#include "modules.h"

static struct {
	bool shown;  // a progress set
	TBPFLAG state;
	double value;
	UINT button_msg;  // TaskbarButtonCreated
	uint32_t flash_id;  // the last attention request
	HWND flash_win;
	int badge;
} dock;

static UINT button_created(void) {
	if (!dock.button_msg) dock.button_msg = RegisterWindowMessageW(L"TaskbarButtonCreated");
	return dock.button_msg;
}

void *wh_taskbar(void) {
	static ITaskbarList3 *t;
	static bool tried;
	if (!tried) {
		tried = true;
		if (SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&t))) ITaskbarList3_HrInit(t);
		else t = NULL;
	}
	return t;
}

static void set_progress(void) {
	ITaskbarList3 *t = wh_taskbar();
	if (!t || !H.window) return;
	ITaskbarList3_SetProgressState(t, H.window, dock.state);
	if (dock.state != TBPF_NOPROGRESS && dock.state != TBPF_INDETERMINATE) {
		double v = dock.value < 0 ? 0 : dock.value > 1 ? 1 : dock.value;
		ITaskbarList3_SetProgressValue(t, H.window, (ULONGLONG)llround(v * 10000), 10000);
	}
}

// progress: u8 state (none, normal, indeterminate, paused, error), f64
// value.
static void m_progress(wh_reply *r, const uint8_t *a, size_t n) {
	static const TBPFLAG states[] = {TBPF_NOPROGRESS, TBPF_NORMAL, TBPF_INDETERMINATE, TBPF_PAUSED, TBPF_ERROR};
	button_created();
	TBPFLAG state = TBPF_NOPROGRESS;
	double value = 0;
	if (n >= 9) {
		if (a[0] < WH_LEN(states)) state = states[a[0]];
		memcpy(&value, a + 1, 8);
	}
	dock.state = state, dock.value = value, dock.shown = state != TBPF_NOPROGRESS;
	// The button may not be there yet (the window not shown): set again
	// as it's made.
	set_progress();
	wh_reply_nil(r);
}

static bool foreground_is_ours(void) {
	HWND fg = GetForegroundWindow();
	if (!fg) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

// attention: u8 critical; answers u32 the request's ID, 0 if none was
// made.
static void m_attention(wh_reply *r, const uint8_t *a, size_t n) {
	HWND w = wh_window_in_use();
	if (!w || foreground_is_ours()) {
		wh_reply_u32(r, 0);
		return;
	}
	FLASHWINFO fi = {sizeof fi};
	fi.hwnd = w;
	if (n > 0 && a[0] == 1) fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
	else fi.dwFlags = FLASHW_TRAY, fi.uCount = 1;
	FlashWindowEx(&fi);
	dock.flash_id++;
	dock.flash_win = w;
	wh_reply_u32(r, dock.flash_id);
}

// cancelAttention: u32 the request's ID.
static void m_cancel_attention(wh_reply *r, const uint8_t *a, size_t n) {
	if (n >= 4 && wh_u32(a) == dock.flash_id && dock.flash_win) {
		FLASHWINFO fi = {sizeof fi};
		fi.hwnd = dock.flash_win, fi.dwFlags = FLASHW_STOP;
		FlashWindowEx(&fi);
		dock.flash_win = NULL;
	}
	wh_reply_nil(r);
}

static const wh_method_def dock_methods[] = {
	{"progress", m_progress}, {"attention", m_attention}, {"cancelAttention", m_cancel_attention}, {NULL, NULL},
};

const wh_module_def wh_dock_module = {"dock", dock_methods};

// ---- The badge ----

// wh_packaged reports whether the app runs from a package (an MSIX: the
// Store's, App Installer's, Add-AppxPackage's): it has a package identity
// (GetCurrentPackageFullName, Windows 8's, looked up).
bool wh_packaged(void) {
	UINT32 n = 0;
	typedef LONG(WINAPI * name_fn)(UINT32 *, PWSTR);
	static name_fn f;
	static bool looked;
	if (!looked) {
		looked = true;
		f = (name_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFullName");
	}
	return f && f(&n, NULL) != APPMODEL_ERROR_NO_PACKAGE;
}

static const GUID IID_BadgeManagerStatics = {0x33400FAA, 0x6DD5, 0x4105, {0xAE, 0xBC, 0x9B, 0x50, 0xFC, 0xA4, 0x92, 0xDA}};
static const GUID IID_BadgeFactory = {0xEDF255CE, 0x0618, 0x4D59, {0x94, 0x8A, 0x5A, 0x61, 0x04, 0x0C, 0x52, 0xF9}};

typedef HRESULT(STDMETHODCALLTYPE *com_get_fn)(void *, void **);
typedef HRESULT(STDMETHODCALLTYPE *com_put_fn)(void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *com_void_fn)(void *);
typedef HRESULT(STDMETHODCALLTYPE *com_make_fn)(void *, void *, void **);

// package_badge sets a packaged app's badge to n (0: none).
static bool package_badge(int n) {
	void *st;
	asuka_winrt_factory("Windows.UI.Notifications.BadgeUpdateManager", &IID_BadgeManagerStatics, &st);
	if (!st) return false;
	void *up = NULL;
	ASUKA_SLOT(st, 6, com_get_fn)(st, &up);  // CreateBadgeUpdaterForApplication
	asuka_com_release(st);
	if (!up) return false;
	bool ok = false;
	if (n <= 0) {
		ok = SUCCEEDED(ASUKA_SLOT(up, 7, com_void_fn)(up));  // Clear
	} else {
		char xml[64];
		snprintf(xml, sizeof xml, "<badge value=\"%d\"/>", n);
		void *doc = wh_xml_document(xml);
		void *f = NULL;
		if (doc) asuka_winrt_factory("Windows.UI.Notifications.BadgeNotification", &IID_BadgeFactory, &f);
		void *b = NULL;
		if (f) ASUKA_SLOT(f, 6, com_make_fn)(f, doc, &b);  // CreateBadgeNotification
		if (b) ok = SUCCEEDED(ASUKA_SLOT(up, 6, com_put_fn)(up, b));  // Update
		asuka_com_release(b);
		asuka_com_release(f);
		asuka_com_release(doc);
	}
	asuka_com_release(up);
	return ok;
}

// badge_pixels draws n, white on Windows's red, in a circle size pixels
// across (a rounded bar for more digits): BGRA, straight alpha.
static uint8_t *badge_pixels(int n, int size) {
	wchar_t text[8];
	if (n > 99) wcscpy(text, L"99+");
	else swprintf(text, WH_LEN(text), L"%d", n);
	int len = (int)wcslen(text);
	uint8_t *out = wh_alloc((size_t)size * size * 4);
	HDC dc = CreateCompatibleDC(NULL);
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = size, bi.bmiHeader.biHeight = -size, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32;
	void *bits = NULL;
	HBITMAP tb = dc ? CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
	if (!tb) {
		if (dc) DeleteDC(dc);
		return out;
	}
	HGDIOBJ old = SelectObject(dc, tb);
	int h = (int)lround(size * (len > 2 ? 0.52 : len > 1 ? 0.66 : 0.78));
	LOGFONTW lf = {0};
	lf.lfHeight = -h, lf.lfWeight = FW_BOLD, lf.lfQuality = ANTIALIASED_QUALITY;
	wcscpy(lf.lfFaceName, L"Segoe UI");
	HFONT font = CreateFontIndirectW(&lf);
	HGDIOBJ of = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	RECT rc = {0, 0, size, size};
	DrawTextW(dc, text, len, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	GdiFlush();
	SelectObject(dc, of);
	DeleteObject(font);
	const uint8_t *tpx = bits;
	// The shape: a circle, or a pill as wide as the image for more digits.
	double cx = size / 2.0, cy = size / 2.0, rad = size / 2.0, half_w = 0;
	if (len > 1) rad = size * 0.36, half_w = size / 2.0 - rad;
	const double red = 0xC4, green = 0x2B, blue = 0x1C;  // Windows 11's critical red
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) {
			double px = x + 0.5, py = y + 0.5;
			double dx = fmax(fabs(px - cx) - half_w, 0);
			double d = hypot(dx, py - cy);
			double a = fmin(fmax(rad - d + 0.5, 0), 1);
			if (a == 0) continue;
			int i = (y * size + x) * 4;
			double t = tpx[i + 1] / 255.0;  // the text's coverage
			out[i] = (uint8_t)lround(blue + (255 - blue) * t);
			out[i + 1] = (uint8_t)lround(green + (255 - green) * t);
			out[i + 2] = (uint8_t)lround(red + (255 - red) * t);
			out[i + 3] = (uint8_t)lround(a * 255);
		}
	SelectObject(dc, old);
	DeleteObject(tb);
	DeleteDC(dc);
	return out;
}

static bool taskbar_badge(int n) {
	ITaskbarList3 *t = wh_taskbar();
	if (!t || !H.window) return false;
	HICON icon = NULL;
	wchar_t label[16] = L"";
	if (n > 0) {
		int size = 16 * (H.dpi > 96 ? H.dpi : 96) / 96;
		uint8_t *px = badge_pixels(n, size);
		icon = wh_icon_from_pixels(px, size, size);
		free(px);
		swprintf(label, WH_LEN(label), L"%d", n);
	}
	HRESULT hr = ITaskbarList3_SetOverlayIcon(t, H.window, icon, label);
	if (icon) DestroyIcon(icon);
	return SUCCEEDED(hr);
}

bool wh_set_badge_count(int n) {
	dock.badge = n;
	if (wh_packaged()) return package_badge(n);
	button_created();
	return taskbar_badge(n);
}

// wh_badge_message sets the overlay again when the taskbar button's made
// again (the window shown, Explorer restarted).
void wh_badge_message(UINT m) {
	if (dock.button_msg && m == dock.button_msg && dock.badge > 0 && !wh_packaged()) taskbar_badge(dock.badge);
}

// wh_dock_message sets the progress again on a new taskbar button.
void wh_dock_message(UINT m) {
	if (dock.shown && dock.button_msg && m == dock.button_msg) set_progress();
}
