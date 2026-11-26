// devperf.c: asuka dev's performance overlay (the DevPerf command, to the
// root): the core's numbers (frames a second, flushes, the last flush's
// phases, memory) in a small box over the top trailing corner of the main
// window's client area. A layered popup the main window owns, as the
// inspector's highlight is: over the window and, raised as it's drawn,
// over its sheets and popovers; transparent to the mouse, never
// activated, and not in the accessibility tree (it's no control). Its
// pixels are drawn here: the text, monospaced and light, on a dark
// translucent rounded box. The core sends its text twice a second while
// it's on, and none to remove it.
#include "win.h"

#define DP_CLASS L"AsukaDevPerf"
#define DP_PAD_X 6
#define DP_PAD_Y 4
#define DP_MARGIN 8  // from the trailing edge
#define DP_TOP 6     // from the top, below a hidden title bar's buttons
#define DP_RADIUS 6
#define DP_ALPHA 184  // the box's

static HWND dp_win;
static wchar_t *dp_text;

static LRESULT CALLBACK dp_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	if (m == WM_NCHITTEST) return HTTRANSPARENT;
	if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
	if (m == WM_GETOBJECT) return 0;  // nothing for a screen reader
	return DefWindowProcW(hwnd, m, wp, lp);
}

static void dp_clear(void) {
	if (dp_win) DestroyWindow(dp_win);
	dp_win = NULL;
	free(dp_text);
	dp_text = NULL;
}

// inside reports whether (x, y) is in a w by h box with corners of radius r.
static bool inside(int x, int y, int w, int h, int r) {
	int cx = x < r ? r : x >= w - r ? w - r - 1 : x;
	int cy = y < r ? r : y >= h - r ? h - r - 1 : y;
	int dx = x - cx, dy = y - cy;
	return dx * dx + dy * dy <= r * r;
}

// draw draws the box for the text at the main window's top trailing
// corner, and shows it.
static void draw(void) {
	HWND owner = H.window;
	if (!dp_text || !owner || !IsWindowVisible(owner) || IsIconic(owner)) {
		if (dp_win) ShowWindow(dp_win, SW_HIDE);
		return;
	}
	HINSTANCE inst = GetModuleHandleW(NULL);
	if (!dp_win) {
		static bool registered;
		if (!registered) {
			WNDCLASSEXW wc = {sizeof wc};
			wc.lpfnWndProc = dp_proc;
			wc.hInstance = inst;
			wc.lpszClassName = DP_CLASS;
			RegisterClassExW(&wc);
			registered = true;
		}
		dp_win = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, DP_CLASS, L"", WS_POPUP, 0, 0, 1, 1, owner, NULL, inst, NULL);
		if (!dp_win) return;
	}
	float k = (float)wh_dpi(owner) / 96;
	int pad_x = (int)lroundf(DP_PAD_X * k), pad_y = (int)lroundf(DP_PAD_Y * k), radius = (int)lroundf(DP_RADIUS * k);
	HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
	HFONT font = CreateFontW(-(int)lroundf(10 * k), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                         ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	HGDIOBJ old_font = SelectObject(mdc, font);
	RECT tr = {0, 0, 0, 0};
	DrawTextW(mdc, dp_text, -1, &tr, DT_CALCRECT | DT_NOPREFIX | DT_LEFT);
	int w = tr.right + 2 * pad_x, h = tr.bottom + 2 * pad_y;
	// The corner: the client area's top trailing one, below a hidden title
	// bar's caption buttons, in screen pixels (a mirrored window's client
	// origin is its top right).
	RECT cr;
	GetClientRect(owner, &cr);
	POINT o = {0, 0};
	ClientToScreen(owner, &o);
	if (GetWindowLongPtrW(owner, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) o.x -= cr.right;
	int margin = (int)lroundf(DP_MARGIN * k);
	POINT at = {o.x + (H.rtl ? margin : cr.right - w - margin), o.y + (int)lroundf((wh_safe_top(H.main_ctl) + DP_TOP) * k)};
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP bm = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (bm && bits) {
		HGDIOBJ old = SelectObject(mdc, bm);
		// White text on black, grayscale: each pixel's gray is the text's
		// coverage, which the box's alpha is made from.
		memset(bits, 0, (size_t)w * (size_t)h * 4);
		SetBkMode(mdc, TRANSPARENT);
		SetTextColor(mdc, RGB(255, 255, 255));
		RECT text = {pad_x, pad_y, w - pad_x, h - pad_y};
		DrawTextW(mdc, dp_text, -1, &text, DT_NOPREFIX | DT_LEFT);
		GdiFlush();
		uint32_t *px = bits;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint32_t *p = &px[(size_t)y * (size_t)w + (size_t)x];
				if (!inside(x, y, w, h, radius)) {
					*p = 0;
					continue;
				}
				uint32_t c = *p & 0xFF;  // the text's coverage
				uint32_t a = DP_ALPHA + (255 - DP_ALPHA) * c / 255;
				*p = a << 24 | c << 16 | c << 8 | c;  // premultiplied: white over black
			}
		}
		POINT src = {0, 0};
		SIZE size = {w, h};
		BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
		UpdateLayeredWindow(dp_win, sdc, &at, &size, mdc, &src, 0, &bf, ULW_ALPHA);
		SelectObject(mdc, old);
		DeleteObject(bm);
	}
	SelectObject(mdc, old_font);
	DeleteObject(font);
	DeleteDC(mdc);
	ReleaseDC(NULL, sdc);
	// Over the app's sheets and popovers, which the main window owns too,
	// while the app is the one in use; else where it is: raised, it would
	// come over the windows of the app in use.
	HWND fg = GetForegroundWindow();
	bool ours = fg && GetWindowThreadProcessId(fg, NULL) == GetCurrentThreadId();
	SetWindowPos(dp_win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | (ours ? 0 : SWP_NOZORDER));
}

void wh_dev_perf(const uint8_t *args, size_t len) {
	if (!len) {
		dp_clear();
		return;
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)args, (int)len, NULL, 0);
	wchar_t *t = malloc(sizeof(wchar_t) * (size_t)(n + 1));
	if (!t) return;
	MultiByteToWideChar(CP_UTF8, 0, (const char *)args, (int)len, t, n);
	t[n] = 0;
	free(dp_text);
	dp_text = t;
	draw();
}

// wh_dev_perf_follow keeps the box in the main window's corner as it
// moves, resizes, minimizes or comes back.
void wh_dev_perf_follow(void) {
	if (dp_text) draw();
}

void wh_dev_perf_dump(ak_buf *b) {
	if (!dp_text || !dp_win || !IsWindowVisible(dp_win)) return;
	int n = WideCharToMultiByte(CP_UTF8, 0, dp_text, -1, NULL, 0, NULL, NULL);
	char *s = malloc((size_t)n);
	if (!s) return;
	WideCharToMultiByte(CP_UTF8, 0, dp_text, -1, s, n, NULL, NULL);
	ak_buf_printf(b, "DevPerf %s\n", s);
	free(s);
}
