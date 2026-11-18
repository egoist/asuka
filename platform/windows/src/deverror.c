// deverror.c: asuka dev's error overlay (the DevError command, to the
// root; DevErrorClear removes it): an error the app didn't catch, a module
// that didn't load, a build that failed. It's a window of the host's own,
// not the app's views (the app's tree may be what's broken): a popup the
// main window owns, over its client area (below a hidden title bar's
// caption strip), following it as it moves, resizes, minimizes, changes
// DPI; an owned window, it stays over the main window, and it's raised
// over the app's sheets, popovers and dialogs (owned windows too) each
// time it's shown. It's drawn as Windows 11's: the content on the layer's
// color, scrolling, the title small, semibold and red (orange: a build
// that failed), the message larger and semibold, the lines of the source
// where it went wrong in a monospaced rounded box, the stack monospaced in
// the secondary color, each a read-only EDIT (selectable, copied with
// Ctrl+C); under them a band of buttons on the window's color, as a
// ContentDialog's: Reload in the accent color when the core offers it,
// Dismiss. They send DevErrorAction to the root; the core answers with
// DevErrorClear (or another DevError). Escape is Dismiss, Enter Reload
// (or the focused button), in the overlay or in the window it covers.
#include "win.h"

#define DE_CLASS L"AsukaDevError"
#define DE_BODY_CLASS L"AsukaDevErrorBody"

// Its measures, in points.
#define DE_PAD_X 24       // the content's sides
#define DE_PAD_Y 24       // above and below the content
#define DE_GAP_TITLE 8    // under the title
#define DE_GAP 16         // between the message, the source, the details
#define DE_BOX_PAD_X 16   // in the source's box
#define DE_BOX_PAD_Y 12
#define DE_BOX_RADIUS 8
#define DE_BAND_PAD_X 24  // the buttons' band
#define DE_BAND_PAD_Y 16
#define DE_BUTTON_H 32
#define DE_BUTTON_MIN_W 120
#define DE_BUTTON_GAP 8
#define DE_WHEEL_LINE 20  // a wheel's line

enum { ID_RELOAD = 1, ID_DISMISS = 2 };

static struct {
	HWND win;   // the popup (NULL until the first DevError)
	HWND body;  // the scrolling content, in it
	HWND title, message, source, details;  // read-only EDITs in the body
	HWND reload, dismiss;                  // BUTTONs in the popup
	HWND owner;  // the main window
	HWND prev;   // the window active before it took the activation
	node bn;     // what the buttons are drawn for (KIND_DEV_ERROR)
	bool on;     // DevError shown, DevErrorClear not since
	bool done;   // a button pressed: the core answers
	bool laying;
	bool source_wide;  // the source's lines are wider than its box: it scrolls sideways
	uint8_t flags;
	uint32_t root;  // the root's ID: where the actions go
	char *text8[4];  // title, message, source, details as they came (UTF-8)
	float k;         // pixels per point: the popup's DPI
	int scroll, content_h, view_h;
	float wheel;  // a wheel's turn not scrolled yet (pixels)
	RECT box;     // the source's box, in the content (pixels, unscrolled)
	int band;     // the buttons' band's top, in the popup
} E;

static int px(float v) { return (int)lroundf(v * E.k); }

static LRESULT CALLBACK de_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK body_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp);
static void layout(void);
static void layout_body(void);

static void register_classes(void) {
	static bool done;
	if (done) return;
	done = true;
	WNDCLASSEXW c = {sizeof c};
	c.hInstance = GetModuleHandleW(NULL);
	c.hCursor = LoadCursorW(NULL, IDC_ARROW);
	c.lpfnWndProc = de_proc;
	c.lpszClassName = DE_CLASS;
	RegisterClassExW(&c);
	c.lpfnWndProc = body_proc;
	c.lpszClassName = DE_BODY_CLASS;
	RegisterClassExW(&c);
}

// wide is utf8 as a new UTF-16 string, its line ends CRLF (an EDIT's).
static wchar_t *wide(ak_span s) {
	wchar_t *w = wh_wspan(s);
	size_t n = 0, lf = 0;
	for (; w[n]; n++)
		if (w[n] == '\n' && (n == 0 || w[n - 1] != '\r')) lf++;
	if (!lf) return w;
	wchar_t *out = wh_alloc((n + lf + 1) * sizeof *out);
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		if (w[i] == '\n' && (i == 0 || w[i - 1] != '\r')) out[k++] = '\r';
		out[k++] = w[i];
	}
	out[k] = 0;
	free(w);
	return out;
}

// ---- Its parts ----

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);

static HWND make_edit(int id, bool wide_lines, bool hscroll) {
	DWORD style = WS_CHILD | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL;
	if (wide_lines) style |= ES_AUTOHSCROLL;  // a code frame's lines aren't wrapped
	if (hscroll) style |= WS_HSCROLL;
	HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 0, 0, E.body, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
	SendMessageW(e, EM_SETLIMITTEXT, 0, 0);
	SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
	SetWindowSubclass(e, edit_proc, 1, 0);
	return e;
}

static HWND make_button(int id, const wchar_t *title) {
	HWND b = CreateWindowExW(0, L"BUTTON", title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, E.win, (HMENU)(INT_PTR)id,
		GetModuleHandleW(NULL), NULL);
	SendMessageW(b, WM_UPDATEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
	return b;
}

static void make(void) {
	register_classes();
	E.owner = H.window;
	E.win = CreateWindowExW(WS_EX_CONTROLPARENT, DE_CLASS, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0, E.owner, NULL, GetModuleHandleW(NULL), NULL);
	E.k = (float)wh_dpi(E.win) / 96;
	E.body = CreateWindowExW(WS_EX_CONTROLPARENT, DE_BODY_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL, 0, 0, 0, 0, E.win, NULL,
		GetModuleHandleW(NULL), NULL);
	ShowScrollBar(E.body, SB_VERT, FALSE);
	E.title = make_edit(1, false, false);
	E.message = make_edit(2, false, false);
	E.source = make_edit(3, true, false);
	E.details = make_edit(4, false, false);
	E.reload = make_button(ID_RELOAD, L"Reload");
	E.dismiss = make_button(ID_DISMISS, L"Dismiss");
	E.bn = (node){.kind = KIND_DEV_ERROR, .hwnd = E.win, .opacity = 1, .k = E.k, .xf = {.sx = 1, .sy = 1}};
}

// restyle gives the parts their fonts and the theme's colors: shown, the
// theme, the text size or the DPI changed.
static void restyle(void) {
	if (!E.win) return;
	E.bn.k = E.k;
	font_spec title = wh_spec(0, AK_FONT_WEIGHT_SEMIBOLD, 12, NULL);
	font_spec message = wh_spec(AK_TEXT_STYLE_TITLE2, 0, 0, NULL);
	font_spec mono = wh_spec(0, 0, 13, NULL);
	static int cascadia = -1;
	if (cascadia < 0) cascadia = wh_font_installed(L"Cascadia Mono");
	if (cascadia) mono.mono = true;
	else mono.family = "Consolas";
	title.k = message.k = mono.k = E.k;
	SendMessageW(E.title, WM_SETFONT, (WPARAM)wh_font(title), FALSE);
	SendMessageW(E.message, WM_SETFONT, (WPARAM)wh_font(message), FALSE);
	SendMessageW(E.source, WM_SETFONT, (WPARAM)wh_font(mono), FALSE);
	SendMessageW(E.details, WM_SETFONT, (WPARAM)wh_font(mono), FALSE);
	HWND themed[] = {E.body, E.source, E.title, E.message, E.details};
	for (size_t i = 0; i < WH_LEN(themed); i++) {
		wh_allow_dark(themed[i], H.dark);
		SetWindowTheme(themed[i], H.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
	}
	SendMessageW(E.reload, WM_SETFONT, (WPARAM)wh_body_font(&E.bn), FALSE);
	SendMessageW(E.dismiss, WM_SETFONT, (WPARAM)wh_body_font(&E.bn), FALSE);
}

// set_text gives an EDIT its text, shown when there's some.
static void set_text(HWND e, ak_span s) {
	wchar_t *w = wide(s);
	SetWindowTextW(e, w);
	free(w);
	ShowWindow(e, s.len ? SW_SHOWNA : SW_HIDE);
}

// ---- Laying it out ----

static int line_height(HWND e) {
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(e, WM_GETFONT, 0, 0));
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	return tm.tmHeight;
}

// widest is the width of an EDIT's longest line, in its font.
static int widest(HWND e) {
	int n = GetWindowTextLengthW(e);
	if (n <= 0) return 0;
	wchar_t *s = wh_alloc(((size_t)n + 1) * sizeof *s);
	n = GetWindowTextW(e, s, n + 1);
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(e, WM_GETFONT, 0, 0));
	int w = 0;
	for (int i = 0, start = 0; i <= n; i++) {
		if (i < n && s[i] != '\r' && s[i] != '\n') continue;
		if (i > start) {
			RECT r = {0};
			DrawTextW(dc, s + start, i - start, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | DT_EXPANDTABS);
			w = wh_maxi(w, r.right);
		}
		start = i + 1;
	}
	SelectObject(dc, old);
	free(s);
	return w;
}

// place puts an EDIT at x, y of the content, w wide, as tall as its lines
// (plus extra), and returns its height.
static int place(HWND e, int x, int y, int w, int extra) {
	SetWindowPos(e, NULL, x, y - E.scroll, w, 1000, SWP_NOZORDER | SWP_NOACTIVATE);
	int lines = (int)SendMessageW(e, EM_GETLINECOUNT, 0, 0);
	int h = wh_maxi(lines, 1) * line_height(e) + extra;
	SetWindowPos(e, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
	SendMessageW(e, EM_LINESCROLL, 0, -lines);  // its first line at its top
	return h;
}

// place_content lays the content out cw wide and returns its height.
static int place_content(int cw) {
	int x = px(DE_PAD_X), w = wh_maxi(cw - 2 * px(DE_PAD_X), px(40));
	int y = px(DE_PAD_Y);
	y += place(E.title, x, y, w, 0) + px(DE_GAP_TITLE);
	y += place(E.message, x, y, w, 0);
	E.box = (RECT){0};
	if (IsWindowVisible(E.source)) {
		y += px(DE_GAP);
		int bx = px(DE_BOX_PAD_X), by = px(DE_BOX_PAD_Y);
		int iw = wh_maxi(w - 2 * bx, px(20));
		bool wide_lines = widest(E.source) > iw;
		if (wide_lines != E.source_wide) {
			// Its sideways scroll bar comes or goes: the EDIT again, with or without.
			int len = GetWindowTextLengthW(E.source);
			wchar_t *s = wh_alloc(((size_t)len + 1) * sizeof *s);
			GetWindowTextW(E.source, s, len + 1);
			HFONT f = (HFONT)SendMessageW(E.source, WM_GETFONT, 0, 0);
			DestroyWindow(E.source);
			E.source = make_edit(3, true, wide_lines);
			SendMessageW(E.source, WM_SETFONT, (WPARAM)f, FALSE);
			wh_allow_dark(E.source, H.dark);
			SetWindowTheme(E.source, H.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
			SetWindowTextW(E.source, s);
			free(s);
			ShowWindow(E.source, SW_SHOWNA);
			E.source_wide = wide_lines;
		}
		int sb = wide_lines ? wh_system_metric(SM_CYHSCROLL, (int)lroundf(E.k * 96)) : 0;
		int h = place(E.source, x + bx, y + by, iw, sb);
		E.box = (RECT){x, y, x + w, y + by + h + by};
		y = E.box.bottom;
	}
	if (IsWindowVisible(E.details)) {
		y += px(DE_GAP);
		y += place(E.details, x, y, w, 0);
	}
	return y + px(DE_PAD_Y);
}

// layout_body lays the content out in the body, with a scroll bar when
// it's taller than the body.
static void layout_body(void) {
	if (!E.body || E.laying) return;
	E.laying = true;
	for (int pass = 0; pass < 3; pass++) {
		RECT c;
		GetClientRect(E.body, &c);
		E.content_h = place_content(c.right);
		E.view_h = c.bottom;
		int scroll = wh_maxi(wh_mini(E.scroll, E.content_h - E.view_h), 0);
		bool moved = scroll != E.scroll;
		E.scroll = scroll;
		SCROLLINFO si = {sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, E.content_h - 1, (UINT)wh_maxi(E.view_h, 0), E.scroll, 0};
		SetScrollInfo(E.body, SB_VERT, &si, TRUE);
		RECT c2;
		GetClientRect(E.body, &c2);
		if (c2.right == c.right && !moved) break;  // the scroll bar came or went: again, at the new width
	}
	E.laying = false;
	InvalidateRect(E.body, NULL, TRUE);
}

// layout places the body over the buttons' band, the buttons in it (the
// primary first, at the end; mirrored right to left).
static void layout(void) {
	if (!E.win) return;
	RECT r;
	GetClientRect(E.win, &r);
	int bh = px(DE_BUTTON_H * wh_font_scale());
	int band = bh + 2 * px(DE_BAND_PAD_Y);
	E.band = wh_maxi(r.bottom - band, 0);
	SetWindowPos(E.body, NULL, 0, 0, r.right, E.band, SWP_NOZORDER | SWP_NOACTIVATE);
	// The buttons as wide as each other: the widest title's, at least
	// DE_BUTTON_MIN_W.
	bool reload = (E.flags & AK_DEV_ERROR_RELOAD) != 0;
	HWND bs[2] = {reload ? E.reload : E.dismiss, reload ? E.dismiss : NULL};
	int nb = reload ? 2 : 1;
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_body_font(&E.bn));
	int bw = px(DE_BUTTON_MIN_W);
	for (int i = 0; i < nb; i++) {
		wchar_t t[64];
		int n = GetWindowTextW(bs[i], t, (int)WH_LEN(t));
		bw = wh_maxi(bw, wh_text_extent(dc, t, n).cx + px(24));
	}
	SelectObject(dc, old);
	int gap = px(DE_BUTTON_GAP);
	int x = r.right - px(DE_BAND_PAD_X) - (nb * bw + (nb - 1) * gap);
	for (int i = 0; i < nb; i++) {
		int at = x + i * (bw + gap);
		if (H.rtl) at = r.right - at - bw;
		SetWindowPos(bs[i], NULL, at, E.band + px(DE_BAND_PAD_Y), bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	ShowWindow(E.reload, reload ? SW_SHOWNA : SW_HIDE);
	layout_body();
	InvalidateRect(E.win, NULL, FALSE);
}

// ---- Over the main window ----

// over is where it goes: the main window's client area, on the screen,
// below a hidden title bar's caption strip.
static RECT over(void) {
	RECT r;
	GetClientRect(E.owner, &r);
	MapWindowPoints(E.owner, NULL, (POINT *)&r, 2);
	if (r.left > r.right) {
		LONG t = r.left;  // a mirrored window's
		r.left = r.right, r.right = t;
	}
	r.top += wh_px(NULL, wh_safe_top(H.main_ctl));
	if (r.bottom < r.top) r.bottom = r.top;
	return r;
}

// round_corners rounds its bottom corners as Windows 11 rounds the main
// window's (not maximized, not full screen).
static void round_corners(int w, int h) {
	bool round = wh_windows11() && !IsZoomed(E.owner) && !(H.main_ctl && H.main_ctl->full);
	if (!round || w <= 0 || h <= 0) {
		SetWindowRgn(E.win, NULL, TRUE);
		return;
	}
	int r = px(8);
	HRGN rgn = CreateRectRgn(0, 0, w, wh_maxi(h - r, 0));
	HRGN rr = CreateRoundRectRgn(0, 0, w + 1, h + 1, 2 * r, 2 * r);
	CombineRgn(rgn, rgn, rr, RGN_OR);
	DeleteObject(rr);
	SetWindowRgn(E.win, rgn, TRUE);  // the system's now
}

// wh_dev_error_follow keeps the overlay over the main window: moved,
// resized, minimized, hidden, shown.
void wh_dev_error_follow(void) {
	if (!E.win || !E.on) return;
	if (!IsWindowVisible(E.owner) || IsIconic(E.owner)) {
		if (IsWindowVisible(E.win)) ShowWindow(E.win, SW_HIDE);
		return;
	}
	RECT r = over(), now;
	GetWindowRect(E.win, &now);
	bool shown = IsWindowVisible(E.win);
	if (!EqualRect(&r, &now) || !shown) {
		SetWindowPos(E.win, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
			SWP_NOZORDER | SWP_NOACTIVATE | (shown ? 0 : SWP_SHOWWINDOW));
		round_corners(r.right - r.left, r.bottom - r.top);
	}
}

// ---- The actions ----

static void act(int action) {
	if (!E.on || E.done) return;
	if (action == AK_DEV_ERROR_ACTION_RELOAD && !(E.flags & AK_DEV_ERROR_RELOAD)) return;
	E.done = true;
	ak_event e = {.id = E.root, .kind = AK_EVENT_DEV_ERROR_ACTION, .i = action};
	wh_dispatch(&e);
}

bool wh_dev_error_primary(HWND ctl) { return ctl && ctl == E.reload; }

static void scroll_to(int y) {
	y = wh_maxi(wh_mini(y, E.content_h - E.view_h), 0);
	if (y == E.scroll) return;
	int dy = E.scroll - y;
	E.scroll = y;
	ScrollWindowEx(E.body, 0, dy, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
	SetScrollPos(E.body, SB_VERT, E.scroll, TRUE);
	UpdateWindow(E.body);
}

static void wheel(int delta) {
	UINT lines = 3;
	SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
	if (lines == WHEEL_PAGESCROLL) E.wheel -= (float)delta / WHEEL_DELTA * (float)E.view_h;
	else E.wheel -= (float)delta / WHEEL_DELTA * (float)lines * (float)px(DE_WHEEL_LINE);
	int d = (int)E.wheel;
	E.wheel -= (float)d;
	scroll_to(E.scroll + d);
}

static bool in_edit(HWND f) { return f && (f == E.title || f == E.message || f == E.source || f == E.details); }

// wh_dev_error_key takes a key for the overlay, or for the window it
// covers while it shows: Escape is Dismiss, Enter the focused button or
// Reload, Tab goes from button to button, the arrows and Page Up and
// Down scroll.
bool wh_dev_error_key(MSG *m) {
	if (!E.win || !E.on || !IsWindowVisible(E.win) || m->message != WM_KEYDOWN) return false;
	HWND top = GetAncestor(m->hwnd, GA_ROOT);
	bool in = top == E.win;
	if (!in && top != E.owner) return false;
	HWND f = GetFocus();
	bool button = in && (f == E.reload || f == E.dismiss);
	switch (m->wParam) {
	case VK_ESCAPE:
		act(AK_DEV_ERROR_ACTION_DISMISS);
		return true;
	case VK_RETURN:
		if (button) act(f == E.reload ? AK_DEV_ERROR_ACTION_RELOAD : AK_DEV_ERROR_ACTION_DISMISS);
		else if (E.flags & AK_DEV_ERROR_RELOAD) act(AK_DEV_ERROR_ACTION_RELOAD);
		else return false;
		return true;
	case VK_TAB: {
		if (!in) return false;
		HWND order[2] = {E.reload, E.dismiss};
		bool reload = (E.flags & AK_DEV_ERROR_RELOAD) != 0;
		HWND next = !reload ? E.dismiss : (f == E.reload) ? order[1] : order[0];
		SetFocus(next);
		SendMessageW(E.win, WM_CHANGEUISTATE, MAKELONG(UIS_CLEAR, UISF_HIDEFOCUS), 0);
		return true;
	}
	case VK_PRIOR:
	case VK_NEXT:
	case VK_UP:
	case VK_DOWN:
	case VK_HOME:
	case VK_END: {
		if (!in || in_edit(f)) return false;
		int line = px(DE_WHEEL_LINE), page = wh_maxi(E.view_h - line, line);
		int y = E.scroll;
		switch (m->wParam) {
		case VK_PRIOR: y -= page; break;
		case VK_NEXT: y += page; break;
		case VK_UP: y -= line; break;
		case VK_DOWN: y += line; break;
		case VK_HOME: y = 0; break;
		default: y = E.content_h; break;
		}
		scroll_to(y);
		return true;
	}
	}
	return false;
}

// ---- Its windows' messages ----

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
	(void)data;
	switch (m) {
	case WM_MOUSEWHEEL:
		if (hwnd == E.source && E.source_wide && (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT)) {
			SendMessageW(hwnd, EM_LINESCROLL, (WPARAM)(-GET_WHEEL_DELTA_WPARAM(wp) / 20), 0);
			return 0;
		}
		wheel(GET_WHEEL_DELTA_WPARAM(wp));  // the content scrolls, not the EDIT
		return 0;
	case WM_MOUSEHWHEEL:
		if (hwnd == E.source && E.source_wide) SendMessageW(hwnd, EM_LINESCROLL, (WPARAM)(GET_WHEEL_DELTA_WPARAM(wp) / 20), 0);
		return 0;
	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, edit_proc, id);
		break;
	}
	return DefSubclassProc(hwnd, m, wp, lp);
}

// edit_colors answers an EDIT's WM_CTLCOLORSTATIC: its text's color on
// the content's (the source's box's, the source).
static HBRUSH edit_colors(HWND e, HDC dc) {
	const palette *p = &H.theme;
	rgba bg = e == E.source ? p->bg : p->layer;
	rgba fg = p->label;
	if (e == E.title) fg = (E.flags & AK_DEV_ERROR_BUILD) ? p->orange : p->red;
	else if (e == E.details) fg = p->secondary;
	SetTextColor(dc, rgba_ref(rgba_over(fg, bg)));
	SetBkColor(dc, rgba_ref(bg));
	return wh_brush(bg);
}

static LRESULT CALLBACK body_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	switch (m) {
	case WM_SIZE:
		layout_body();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT c;
		GetClientRect(hwnd, &c);
		const palette *p = &H.theme;
		gfx g = g_new(dc);
		g_fill_rect(g, rgba_argb(p->layer), 0, 0, (float)c.right, (float)c.bottom);
		if (E.box.bottom > E.box.top) {
			float x = (float)E.box.left, y = (float)(E.box.top - E.scroll);
			float w = (float)(E.box.right - E.box.left), h = (float)(E.box.bottom - E.box.top), r = (float)px(DE_BOX_RADIUS);
			g_fill_round_rect(g, rgba_argb(p->bg), x, y, w, h, r);
			g_stroke_round_rect(g, rgba_argb(p->card_stroke), wh_maxf(E.k, 1), x + 0.5f, y + 0.5f, w - 1, h - 1, r);
		}
		g_close(g);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLOREDIT:
		return (LRESULT)edit_colors((HWND)lp, (HDC)wp);
	case WM_VSCROLL: {
		SCROLLINFO si = {sizeof si, SIF_ALL};
		GetScrollInfo(hwnd, SB_VERT, &si);
		int y = E.scroll, line = px(DE_WHEEL_LINE);
		switch (LOWORD(wp)) {
		case SB_LINEUP: y -= line; break;
		case SB_LINEDOWN: y += line; break;
		case SB_PAGEUP: y -= (int)si.nPage; break;
		case SB_PAGEDOWN: y += (int)si.nPage; break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: y = si.nTrackPos; break;
		case SB_TOP: y = 0; break;
		case SB_BOTTOM: y = E.content_h; break;
		}
		scroll_to(y);
		return 0;
	}
	case WM_MOUSEWHEEL:
		wheel(GET_WHEEL_DELTA_WPARAM(wp));
		return 0;
	}
	return DefWindowProcW(hwnd, m, wp, lp);
}

static LRESULT CALLBACK de_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	switch (m) {
	case WM_SIZE:
		if (E.body) layout();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		// The buttons' band: the window's color, a stroke above it.
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT c;
		GetClientRect(hwnd, &c);
		const palette *p = &H.theme;
		gfx g = g_new(dc);
		g_fill_rect(g, rgba_argb(p->bg), 0, (float)E.band, (float)c.right, (float)(c.bottom - E.band));
		g_fill_rect(g, rgba_argb(p->card_stroke), 0, (float)E.band, (float)c.right, wh_maxf((float)px(1), 1));
		g_close(g);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_NOTIFY: {
		NMHDR *hdr = (NMHDR *)lp;
		if (hdr->code == NM_CUSTOMDRAW && (hdr->hwndFrom == E.reload || hdr->hwndFrom == E.dismiss))
			return wh_draw_button(&E.bn, (NMCUSTOMDRAW *)lp);
		break;
	}
	case WM_CTLCOLORBTN:
		return (LRESULT)wh_brush(H.theme.bg);
	case WM_COMMAND:
		if (HIWORD(wp) == BN_CLICKED) {
			if (LOWORD(wp) == ID_RELOAD) act(AK_DEV_ERROR_ACTION_RELOAD);
			else if (LOWORD(wp) == ID_DISMISS) act(AK_DEV_ERROR_ACTION_DISMISS);
			return 0;
		}
		break;
	case WM_MOUSEWHEEL:
		wheel(GET_WHEEL_DELTA_WPARAM(wp));
		return 0;
	case WM_ACTIVATE:
		if (LOWORD(wp) != WA_INACTIVE) {
			H.in_use = E.owner;
			HWND f = GetFocus();
			if (!f || !IsChild(hwnd, f)) SetFocus((E.flags & AK_DEV_ERROR_RELOAD) ? E.reload : E.dismiss);
			return 0;
		}
		break;
	case WM_MOUSEACTIVATE:
		return MA_ACTIVATE;
	case WM_DPICHANGED:
		// Over the main window wherever Windows would put it: the size it
		// suggests isn't taken, the parts are laid out at the new scale.
		E.k = (float)LOWORD(wp) / 96;
		restyle();
		layout();
		wh_dev_error_follow();
		return 0;
	case WM_CLOSE:
		PostMessageW(E.owner, WM_CLOSE, 0, 0);  // Alt+F4 in it: the app's window's
		return 0;
	case WM_DESTROY:
		// Its owner went (the app quits).
		E.win = E.body = E.title = E.message = E.source = E.details = E.reload = E.dismiss = NULL;
		E.on = false;
		break;
	}
	return DefWindowProcW(hwnd, m, wp, lp);
}

// ---- The commands ----

// wh_dev_error shows DevError's error over the main window, or what it
// shows replaced, raised over the app's own windows again.
void wh_dev_error(node *root, const uint8_t *args, size_t len) {
	ak_dev_error e;
	if (!ak_decode_dev_error(args, len, &e) || !H.window) return;
	if (!E.win) make();
	if (!E.win) return;
	// Said again (its source came): screen readers aren't told again.
	bool again = E.on && E.text8[0] && E.text8[1] && strlen(E.text8[0]) == e.title.len && !memcmp(E.text8[0], e.title.p, e.title.len) &&
		strlen(E.text8[1]) == e.message.len && !memcmp(E.text8[1], e.message.p, e.message.len);
	E.on = true;
	E.done = false;
	E.root = root->id;
	E.flags = e.flags;
	ak_span parts[4] = {e.title, e.message, e.source, e.details};
	HWND edits[4] = {E.title, E.message, E.source, E.details};
	for (int i = 0; i < 4; i++) {
		free(E.text8[i]);
		E.text8[i] = wh_strndup(parts[i].p, parts[i].len);
		set_text(edits[i], parts[i]);
	}
	ShowWindow(E.title, SW_SHOWNA);
	wchar_t *t = wh_wspan(e.title);
	SetWindowTextW(E.win, t);  // its name, for screen readers
	free(t);
	E.k = (float)wh_dpi(E.win) / 96;
	restyle();
	E.scroll = 0;
	E.wheel = 0;
	SetScrollPos(E.body, SB_VERT, 0, FALSE);
	// Over the main window, over the app's own windows; activated when the
	// app is the one in use (not taken from another app, nor from a
	// snapshot's run).
	if (IsWindowVisible(E.owner) && !IsIconic(E.owner)) {
		RECT r = over();
		HWND fg = GetForegroundWindow();
		bool ours = fg && GetWindowThreadProcessId(fg, NULL) == GetCurrentThreadId();
		bool activate = ours && !wh_snapshotting() && GetActiveWindow() != E.win;
		if (activate) E.prev = GetActiveWindow();
		SetWindowPos(E.win, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE));
		round_corners(r.right - r.left, r.bottom - r.top);
		if (activate) SetFocus((E.flags & AK_DEV_ERROR_RELOAD) ? E.reload : E.dismiss);
	}
	layout();
	RedrawWindow(E.win, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
	if (!again) {
		// Screen readers hear it: the title and the message.
		size_t n = e.title.len + 2 + e.message.len;
		char *s = wh_alloc(n + 1);
		memcpy(s, e.title.p, e.title.len);
		memcpy(s + e.title.len, ": ", 2);
		memcpy(s + e.title.len + 2, e.message.p, e.message.len);
		wh_announce(s, n);
		free(s);
	}
}

// wh_dev_error_clear removes the overlay (hidden, kept for the next): the
// window active before it is again.
void wh_dev_error_clear(void) {
	if (!E.win || !E.on) return;
	E.on = false;
	E.done = false;
	bool active = GetActiveWindow() == E.win;
	SetWindowPos(E.win, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	if (active) {
		HWND back = E.prev && IsWindow(E.prev) && IsWindowVisible(E.prev) && IsWindowEnabled(E.prev) ? E.prev : E.owner;
		SetActiveWindow(back);
	}
	E.prev = NULL;
}

// wh_dev_error_restyle follows the theme, the accent and the text size.
void wh_dev_error_restyle(void) {
	if (!E.win) return;
	restyle();
	layout();
	RedrawWindow(E.win, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

// ---- Automation ----

bool wh_dev_error_showing(void) { return E.win && E.on && IsWindowVisible(E.win); }

// wh_dev_error_tap is automation's tap on the overlay's buttons: their
// click's BN_CLICKED (BM_CLICK, in a window that isn't active, may not
// click: the button's focus activates it, and a snapshot's run doesn't).
bool wh_dev_error_tap(const wchar_t *t) {
	if (!wh_dev_error_showing()) return false;
	HWND bs[2] = {E.reload, E.dismiss};
	for (int i = 0; i < 2; i++) {
		if (!IsWindowVisible(bs[i])) continue;
		wchar_t cur[64];
		GetWindowTextW(bs[i], cur, (int)WH_LEN(cur));
		if (!wcscmp(cur, t)) {
			SendMessageW(E.win, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(bs[i]), BN_CLICKED), (LPARAM)bs[i]);
			return true;
		}
	}
	return false;
}

// wh_dev_error_dump describes the overlay showing: its frame in the main
// window's client area (points), its texts, a line each (the source's and
// the details' each of theirs), and its buttons.
void wh_dev_error_dump(ak_buf *b) {
	if (!wh_dev_error_showing()) return;
	RECT r;
	GetWindowRect(E.win, &r);
	POINT at = {0, 0};
	ClientToScreen(E.owner, &at);
	ak_buf_printf(b, "dev error [%.0f,%.0f %.0fx%.0f]:\n", wh_pt(NULL, r.left - at.x), wh_pt(NULL, r.top - at.y), wh_pt(NULL, r.right - r.left),
		wh_pt(NULL, r.bottom - r.top));
	for (int i = 0; i < 4; i++) {
		const char *s = E.text8[i] ? E.text8[i] : "";
		while (*s) {
			const char *end = strchr(s, '\n');
			size_t n = end ? (size_t)(end - s) : strlen(s);
			ak_buf_put(b, "  ", 2);
			ak_fmt_quote(b, ak_strn(s, n));
			ak_buf_u8(b, '\n');
			s += n + (end ? 1 : 0);
		}
	}
	if (E.flags & AK_DEV_ERROR_RELOAD) ak_buf_put(b, "  button \"Reload\"\n", 18);
	ak_buf_put(b, "  button \"Dismiss\"\n", 19);
}

// wh_dev_error_print draws the overlay into dc, a picture of the main
// window (at its window rectangle win): a snapshot shows it as the screen
// does.
void wh_dev_error_print(HDC dc, RECT win) {
	if (!wh_dev_error_showing()) return;
	RECT r;
	GetWindowRect(E.win, &r);
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
	HGDIOBJ old = SelectObject(mem, bmp);
	PrintWindow(E.win, mem, PW_RENDERFULLCONTENT);
	int x = r.left - win.left, y = r.top - win.top;
	HRGN rgn = CreateRectRgn(0, 0, 0, 0);
	int saved = SaveDC(dc);
	if (GetWindowRgn(E.win, rgn) != ERROR) {
		OffsetRgn(rgn, x, y);
		SelectClipRgn(dc, rgn);
	}
	BitBlt(dc, x, y, w, h, mem, 0, 0, SRCCOPY);
	RestoreDC(dc, saved);
	DeleteObject(rgn);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
}
