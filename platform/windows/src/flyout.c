// flyout.c: panels, flyouts, windows sized to their content and
// click-through windows (WindowConfig's Panel, Flyout, FitContent,
// ClickThrough).
//
//   - A panel and a flyout are frameless popups and tool windows, of a
//     class with the drop shadow of menus, with Windows 11's rounded
//     corners, and no taskbar button.
//   - Either takes the keyboard as it shows and closes as the user closes
//     it (WM_CLOSE's path) on an Escape its content leaves and as it
//     loses the keyboard to a window that isn't its own (the windows it
//     owns, through others in turn); it closes once the activation has
//     settled (a posted message).
//   - A press on the tray icon a flyout hangs from leaves it open, and the
//     click is the app's to toggle it; a click on the window a flyout
//     hangs from only closes the flyout (MA_ACTIVATEANDEAT).
//   - A panel shows centered across the work area of the monitor with the
//     cursor, a third of the way down, unless the app placed it.
//   - A flyout is placed against its anchor (dsk_flyout_place) as it
//     shows, as its anchor changes and as its size does, in the work area
//     of the anchor's monitor, at that monitor's DPI.
//   - A window sized to its content takes the size WindowAction Fit
//     gives; one that opens shown waits for its first Fit.
//   - A click-through window is layered and transparent to the mouse.
#include "win.h"

enum {
	WM_LIGHT_DISMISS = WM_APP + 0x60,   // a panel or flyout lost the keyboard: it closes if it's still away
	WM_FLYOUT_REFOCUS = WM_APP + 0x61,  // its tray icon was clicked: it gets the keyboard back if it stays
};

bool wh_frameless(const wh_wctl *c) { return wh_ctl_has(c, AK_WINDOW_PANEL | AK_WINDOW_FLYOUT); }

static bool anchor(wh_wctl *c, ak_flyout *f) {
	if (!wh_ctl_has(c, AK_WINDOW_FLYOUT) || !c->n) return false;
	return wh_nav_flyout(c->n, f);
}

// wh_ex_style is the extended style a Window's window is made with: a
// tool window, topmost for a tray icon's flyout and a flyout of a topmost
// window.
DWORD wh_ex_style(wh_wctl *c) {
	DWORD ex = 0;
	ak_flyout f;
	if (wh_frameless(c)) ex |= WS_EX_TOOLWINDOW;
	if (anchor(c, &f) && f.tray) ex |= WS_EX_TOPMOST;
	if (c->owner && (GetWindowLongPtrW(c->owner, GWL_EXSTYLE) & WS_EX_TOPMOST)) ex |= WS_EX_TOPMOST;
	return ex;
}

void wh_configure_frameless(wh_wctl *c) {
	if (wh_frameless(c)) {
		SetWindowLongPtrW(c->hwnd, GWL_EXSTYLE, GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
		DWORD pref = 2;  // DWMWCP_ROUND
		DwmSetWindowAttribute(c->hwnd, 33, &pref, sizeof pref);  // DWMWA_WINDOW_CORNER_PREFERENCE
		if (c->main && GetMenu(c->hwnd)) SetMenu(c->hwnd, NULL);
		ak_flyout f;
		if (anchor(c, &f) && f.tray) SetWindowPos(c->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (wh_ctl_has(c, AK_WINDOW_CLICK_THROUGH)) wh_set_click_through(c, true);
}

#define THROUGH (WS_EX_LAYERED | WS_EX_TRANSPARENT)

// wh_set_click_through lets the mouse through the window, or not; where
// it has views that take clicks, it takes the mouse over them
// (clickregion.c).
void wh_set_click_through(wh_wctl *c, bool on) {
	c->through = on;
	wh_apply_click_through(c, on);
	wh_click_region_update();
}

// wh_apply_click_through makes the window let the mouse through, or not,
// as it is now.
void wh_apply_click_through(wh_wctl *c, bool on) {
	LONG_PTR ex = GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE);
	if (on == ((ex & THROUGH) == THROUGH)) return;
	if (on) {
		SetWindowLongPtrW(c->hwnd, GWL_EXSTYLE, ex | THROUGH);
		SetLayeredWindowAttributes(c->hwnd, 0, 255, LWA_ALPHA);
		return;
	}
	SetWindowLongPtrW(c->hwnd, GWL_EXSTYLE, ex & ~(LONG_PTR)THROUGH);
	RedrawWindow(c->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

bool wh_click_through(HWND hwnd) {
	wh_wctl *c = wh_ctl_of(hwnd);
	return c ? c->through : (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & THROUGH) == THROUGH;
}

static void content_size(wh_wctl *c, float *w, float *h) {
	if (c->fitted) {
		*w = c->fit[0], *h = c->fit[1];
		return;
	}
	float k = wh_ctl_k(c);
	RECT cr;
	GetClientRect(c->hwnd, &cr);
	*w = (float)cr.right / k, *h = (float)cr.bottom / k;
}

// take_dpi moves the window onto the monitor whose work area is work, at
// dpi, before it's placed there: its views at that scale.
static void take_dpi(wh_wctl *c, RECT work, int dpi) {
	if ((float)dpi / 96 == wh_ctl_k(c)) return;
	SetWindowPos(c->hwnd, NULL, work.left, work.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	int now = wh_dpi(c->hwnd);
	if ((float)now / 96 == wh_ctl_k(c)) return;  // WM_DPICHANGED took it there
	RECT wr;
	GetWindowRect(c->hwnd, &wr);
	if (c->main) wh_dpi_changed(now, &wr);
	else if (c->frame) wh_window_dpi_changed(c->frame, now, &wr);
}

static dsk_rect rectf(RECT r) { return (dsk_rect){r.left, r.top, r.right - r.left, r.bottom - r.top}; }

static void set_rect(HWND hwnd, dsk_rect r) {
	SetWindowPos(hwnd, NULL, (int)lround(r.x), (int)lround(r.y), (int)lround(r.w), (int)lround(r.h), SWP_NOZORDER | SWP_NOACTIVATE);
}

static void monitor_at(POINT p, RECT *work, int *dpi) {
	HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = {sizeof mi};
	GetMonitorInfoW(m, &mi);
	*work = mi.rcWork;
	UINT x = 96, y = 96;
	typedef HRESULT(WINAPI * get_dpi)(HMONITOR, int, UINT *, UINT *);
	static get_dpi f;
	static bool tried;
	if (!tried) {
		tried = true;
		HMODULE sh = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (sh) f = (get_dpi)(void *)GetProcAddress(sh, "GetDpiForMonitor");
	}
	if (f) f(m, 0, &x, &y);  // MDT_EFFECTIVE_DPI
	*dpi = (int)x;
}

static void center_panel(wh_wctl *c) {
	if (c->full || IsZoomed(c->hwnd) || IsIconic(c->hwnd)) return;
	POINT p;
	GetCursorPos(&p);
	RECT work;
	int dpi;
	monitor_at(p, &work, &dpi);
	take_dpi(c, work, dpi);
	float k = wh_ctl_k(c), w, h;
	content_size(c, &w, &h);
	RECT wr, cr;
	GetWindowRect(c->hwnd, &wr);
	GetClientRect(c->hwnd, &cr);
	int fw = (wr.right - wr.left) - cr.right, fh = (wr.bottom - wr.top) - cr.bottom;  // none, for a frameless panel
	set_rect(c->hwnd, dsk_flyout_center(lroundf(w * k) + fw, lroundf(h * k) + fh, rectf(work)));
}

// anchor_rect is a flyout's anchor on the screen, in pixels: its tray
// icon's rectangle (the cursor where Windows doesn't say), or its
// rectangle of its parent's content.
static RECT anchor_rect(wh_wctl *c, const ak_flyout *f) {
	POINT p;
	GetCursorPos(&p);
	RECT cursor = {p.x, p.y, p.x, p.y};
	if (f->tray) {
		RECT r;
		return wh_tray_icon_rect(f->tray, &r) ? r : cursor;
	}
	node *pn = wh_node(c->cfg.parent);
	HWND content = NULL;
	float k = H.scale;
	if (pn && pn->kind == AK_KIND_WINDOW && wh_nav_ctl(pn) && pn->hwnd) content = pn->hwnd, k = wh_ctl_k(wh_nav_ctl(pn));
	else if (H.root && H.root->hwnd) content = H.root->hwnd;
	if (!content) return cursor;
	RECT o;
	GetWindowRect(content, &o);  // screen pixels, also in a mirrored window
	int x = o.left + (int)lroundf(f->x * k), y = o.top + (int)lroundf(f->y * k);
	return (RECT){x, y, x + (int)lroundf(f->width * k), y + (int)lroundf(f->height * k)};
}

void wh_place_flyout(wh_wctl *c) {
	ak_flyout f;
	if (!anchor(c, &f) || !c->hwnd || !IsWindow(c->hwnd)) return;
	RECT a = anchor_rect(c, &f);
	MONITORINFO mi = {sizeof mi};
	HMONITOR m = MonitorFromRect(&a, MONITOR_DEFAULTTONEAREST);
	GetMonitorInfoW(m, &mi);
	POINT mid = {(a.left + a.right) / 2, (a.top + a.bottom) / 2};
	RECT work;
	int dpi;
	monitor_at(mid, &work, &dpi);
	take_dpi(c, mi.rcWork, dpi);
	float k = wh_ctl_k(c), w, h;
	content_size(c, &w, &h);
	set_rect(c->hwnd, dsk_flyout_place(rectf(a), lroundf(w * k), lroundf(h * k), f.placement, f.gap * k, rectf(mi.rcWork)));
}

void wh_before_show(wh_wctl *c) {
	if (wh_ctl_has(c, AK_WINDOW_FLYOUT)) wh_place_flyout(c);
	else if (wh_ctl_has(c, AK_WINDOW_PANEL) && !c->app_placed && !wh_ctl_has(c, AK_WINDOW_POSITIONED)) center_panel(c);
}

static void fit_arrived(wh_wctl *c) {
	if (!c->wait_fit) return;
	c->wait_fit = false;
	if (c->show_pending) {
		c->show_pending = false;
		wh_show_ctl(c, c->show_activate);
	}
}

void wh_fit_window(wh_wctl *c, float w, float h) {
	c->fit[0] = w, c->fit[1] = h, c->fitted = true;
	if (!c->full && !IsZoomed(c->hwnd) && !IsIconic(c->hwnd)) {
		if (wh_ctl_has(c, AK_WINDOW_FLYOUT)) wh_place_flyout(c);
		else wh_size_content(c, w, h);
	}
	fit_arrived(c);
}

static bool light_dismisses(wh_wctl *c) { return wh_frameless(c) && c->hwnd && IsWindowVisible(c->hwnd); }

// owned_by reports whether w is top or owned by it, through others.
static bool owned_by(HWND w, HWND top) {
	for (int i = 0; w && i < 64; i++, w = GetWindow(w, GW_OWNER))
		if (w == top) return true;
	return false;
}

static bool owns_active(wh_wctl *c) {
	HWND fg = GetForegroundWindow();
	return fg && owned_by(fg, c->hwnd);
}

static bool pressed_on_anchor(wh_wctl *c) {
	ak_flyout f;
	if (!anchor(c, &f) || !f.tray || !(GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0)) return false;
	RECT r;
	POINT p;
	GetCursorPos(&p);
	return wh_tray_icon_rect(f.tray, &r) && PtInRect(&r, p);
}

// may_dismiss closes a panel or a flyout that lost the keyboard to next,
// once the activation has settled.
static void may_dismiss(wh_wctl *c, HWND next) {
	if (!light_dismisses(c) || c->dismiss_pending) return;
	if ((next && owned_by(next, c->hwnd)) || !IsWindowEnabled(c->hwnd) || pressed_on_anchor(c)) return;
	c->dismiss_pending = true;
	PostMessageW(c->hwnd, WM_LIGHT_DISMISS, 0, 0);
}

static bool has_active_flyout(HWND hwnd) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.ctls, &i, NULL, &v)) {
		wh_wctl *c = v;
		if (wh_ctl_has(c, AK_WINDOW_FLYOUT) && c->owner == hwnd && light_dismisses(c) && IsWindowEnabled(c->hwnd) && owns_active(c)) return true;
	}
	return false;
}

static void close_as_user(wh_wctl *c) {
	if (c->hwnd && IsWindow(c->hwnd)) SendMessageW(c->hwnd, WM_CLOSE, 0, 0);
}

LRESULT wh_light_dismiss_message(wh_wctl *c, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = false;
	switch (m) {
	case WM_ACTIVATE:
		if (LOWORD(wp) == WA_INACTIVE) may_dismiss(c, (HWND)lp);
		break;
	case WM_ACTIVATEAPP:
		if (!wp) may_dismiss(c, NULL);  // another app's window is activated
		break;
	case WM_MOUSEACTIVATE:
		if (LOWORD(lp) == HTCLIENT && has_active_flyout(c->hwnd)) {
			*handled = true;
			return MA_ACTIVATEANDEAT;
		}
		break;
	case WM_LIGHT_DISMISS:
		c->dismiss_pending = false;
		if (light_dismisses(c) && IsWindowEnabled(c->hwnd) && !owns_active(c)) close_as_user(c);
		*handled = true;
		return 0;
	case WM_FLYOUT_REFOCUS:
		if (light_dismisses(c) && IsWindowEnabled(c->hwnd) && !owns_active(c) && !wh_snapshotting()) SetForegroundWindow(c->hwnd);
		*handled = true;
		return 0;
	}
	return 0;
}

// wh_window_activated follows one of the app's top-level windows being
// activated: the panels and flyouts it isn't an own window of have lost
// the keyboard.
void wh_window_activated(HWND hwnd) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.ctls, &i, NULL, &v)) {
		wh_wctl *c = v;
		if (c->hwnd != hwnd && light_dismisses(c) && !owned_by(hwnd, c->hwnd)) may_dismiss(c, hwnd);
	}
}

// wh_escape_dismisses closes the panel or the flyout an Escape is for,
// unless its content takes it (an open combo box's list closes).
bool wh_escape_dismisses(MSG *m) {
	wh_wctl *c = wh_ctl_of(GetAncestor(m->hwnd, GA_ROOT));
	if (!c || !light_dismisses(c) || m->message != WM_KEYDOWN) return false;
	if (SendMessageW(m->hwnd, CB_GETDROPPEDSTATE, 0, 0)) return false;
	close_as_user(c);
	return true;
}

// wh_tray_clicked follows a click on tray icon id: a flyout hanging from
// it that the click leaves open gets the keyboard back.
void wh_tray_clicked(uint32_t id) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.ctls, &i, NULL, &v)) {
		wh_wctl *c = v;
		ak_flyout f;
		if (anchor(c, &f) && f.tray == id && light_dismisses(c)) PostMessageW(c->hwnd, WM_FLYOUT_REFOCUS, 0, 0);
	}
}

// wh_center_over centers a window over another (a dialog over a panel or
// a flyout), within that one's monitor's work area.
void wh_center_over(wh_wctl *c, HWND over) {
	RECT o, wr;
	GetWindowRect(over, &o);
	GetWindowRect(c->hwnd, &wr);
	RECT work = wh_monitor_work_area(o);
	int w = wr.right - wr.left, h = wr.bottom - wr.top;
	int x = o.left + ((o.right - o.left) - w) / 2, y = o.top + ((o.bottom - o.top) - h) / 2;
	x = wh_mini(wh_maxi(x, work.left), wh_maxi(work.right - w, work.left));
	y = wh_mini(wh_maxi(y, work.top), wh_maxi(work.bottom - h, work.top));
	SetWindowPos(c->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool wh_is_frameless(HWND hwnd) {
	wh_wctl *c = wh_ctl_of(hwnd);
	return c && wh_frameless(c);
}
