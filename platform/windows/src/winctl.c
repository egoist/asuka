// winctl.c: the app's top-level windows, the main one and each Window's,
// as the app controls them: the options they open with (WindowOptions'
// for the main window, a Window's Config prop), the WindowAction
// commands, their state reported as it changes (WindowState), the user's
// closing asked about first (CloseRequest), and where they were.
//
//   - Abilities are window styles: no sizing frame or maximize button for
//     a window that can't be resized, none of the minimize or maximize
//     buttons taken away; one that can't be closed has its close button
//     and its system menu's Close grayed, and the user's closing it does
//     nothing.
//   - Its most size is WM_GETMINMAXINFO's, as its least is.
//   - Always on top is the topmost band (HWND_TOPMOST).
//   - A window kept off the taskbar is owned by the host's hidden window:
//     Windows gives owned windows no taskbar button.
//   - A child Window is owned by its parent's window; a modal one
//     disables it while it's open.
//   - A background color fills the window behind its content. A
//     transparent window isn't: every view is a child window, composed
//     over the top-level window's opaque surface.
//   - Full screen is the window without its frame, covering its monitor,
//     its placement kept to go back to.
#include "win.h"

wh_wctl *wh_wctl_new(HWND hwnd, bool main) {
	wh_wctl *c = wh_alloc(sizeof *c);
	c->hwnd = hwnd;
	c->main = main;
	if (main) c->key = wh_strdup("main");
	wh_map_put(&H.ctls, (uint64_t)(uintptr_t)hwnd, c);
	return c;
}

void wh_wctl_free(wh_wctl *c) {
	if (!c) return;
	wh_map_del(&H.ctls, (uint64_t)(uintptr_t)c->hwnd);
	wh_forget_chrome(c);
	free(c->key);
	free(c->cfg_bytes);
	free(c->full);
	free(c->caption);
	free(c);
}

wh_wctl *wh_ctl_of(HWND hwnd) { return hwnd ? wh_map_get(&H.ctls, (uint64_t)(uintptr_t)hwnd) : NULL; }

// wh_ctl_set_config keeps a window's options: WindowOptions' args, a
// Window's Config.
void wh_ctl_set_config(wh_wctl *c, const uint8_t *args, size_t len) {
	free(c->cfg_bytes);
	c->cfg_bytes = wh_alloc(len ? len : 1);
	if (len) memcpy(c->cfg_bytes, args, len);
	c->cfg = ak_decode_window_config(c->cfg_bytes, len);
	if (c->main) {
		// The main window's state key: the app's, or "main".
		free(c->key);
		c->key = c->cfg.state_key.len ? wh_strndup(c->cfg.state_key.p, c->cfg.state_key.len) : wh_strdup("main");
	}
}

bool wh_ctl_has(const wh_wctl *c, uint32_t flag) { return (c->cfg.flags & flag) != 0; }

// wh_ctl_k is the window's pixels per point.
float wh_ctl_k(const wh_wctl *c) { return c->frame && c->frame->k > 0 ? c->frame->k : H.scale; }

// wh_ctl_node is what the window reports to: its Window node, or the root.
node *wh_ctl_node(wh_wctl *c) { return c->main && H.root ? H.root : c->n; }

DWORD wh_ctl_style(const wh_wctl *c) {
	if (wh_frameless(c)) return WS_POPUP | WS_CLIPCHILDREN;  // a panel, a flyout: no frame, no caption
	DWORD s = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
	if (wh_ctl_has(c, AK_WINDOW_FIXED_SIZE)) s &= ~(DWORD)(WS_THICKFRAME | WS_MAXIMIZEBOX);
	if (wh_ctl_has(c, AK_WINDOW_NO_MINIMIZE)) s &= ~(DWORD)WS_MINIMIZEBOX;
	if (wh_ctl_has(c, AK_WINDOW_NO_MAXIMIZE)) s &= ~(DWORD)WS_MAXIMIZEBOX;
	return s;
}

// wh_ctl_for is the window n controls: a Window node's, or the main
// window's for the root (which may not be the root yet: it's set last in
// the batch that mounts it).
wh_wctl *wh_ctl_for(node *n) {
	if (!n) return NULL;
	if (n->kind == AK_KIND_WINDOW) return wh_nav_ctl(n);
	if (n->kind == AK_KIND_VIEW && (!H.root || H.root == n) && H.main_ctl) {
		H.main_ctl->n = n;
		return H.main_ctl;
	}
	return NULL;
}

// wh_configure gives a window its options' styles, before it shows: its
// abilities, its title bar, the topmost band, its taskbar button.
void wh_configure(wh_wctl *c) {
	LONG_PTR cur = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
	SetWindowLongPtrW(c->hwnd, GWL_STYLE, (LONG_PTR)wh_ctl_style(c) | (cur & (WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE)));
	if (c->cfg.title_bar == AK_TITLE_BAR_HIDDEN && !c->hidden && !wh_frameless(c)) wh_hide_title_bar(c);
	wh_configure_frameless(c);
	SetWindowPos(c->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	HMENU sys = GetSystemMenu(c->hwnd, FALSE);
	if (sys) EnableMenuItem(sys, SC_CLOSE, MF_BYCOMMAND | (wh_ctl_has(c, AK_WINDOW_NO_CLOSE) ? MF_GRAYED : MF_ENABLED));
	if (wh_ctl_has(c, AK_WINDOW_ALWAYS_ON_TOP)) SetWindowPos(c->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (wh_ctl_has(c, AK_WINDOW_SKIP_TASKBAR) && !GetWindow(c->hwnd, GW_OWNER)) SetWindowLongPtrW(c->hwnd, GWLP_HWNDPARENT, (LONG_PTR)H.park);
	wh_layout_chrome(c);
}

// wh_least is the window's least content size, in points (0: none).
void wh_least(wh_wctl *c, float out[2]) {
	out[0] = out[1] = 0;
	if (c->main && (wh_frameless(c) || wh_ctl_has(c, AK_WINDOW_FIT_CONTENT))) {
		out[0] = H.min_size[0], out[1] = H.min_size[1];  // its options' alone
		return;
	}
	if (c->main) {
		wh_least_size(out);
		return;
	}
	if (c->n) wh_nav_min_size(c->n, out);
}

// wh_outer_size is the size of the window around a content of w by h
// points, at its DPI: its frame (none at the top under a hidden title bar)
// and menu bar.
void wh_outer_size(wh_wctl *c, float w, float h, int *ow, int *oh) {
	int dpi = wh_dpi(c->hwnd);
	float k = (float)dpi / 96;
	RECT r = {0, 0, (LONG)lroundf(w * k), (LONG)lroundf(h * k)};
	if (!c->full) {
		wh_adjust_window_rect(&r, (DWORD)GetWindowLongPtrW(c->hwnd, GWL_STYLE), 0, dpi);
		if (c->hidden) r.top = -wh_hidden_top();
		else if (GetMenu(c->hwnd)) r.bottom += wh_system_metric(SM_CYMENU, dpi);
	}
	*ow = r.right - r.left, *oh = r.bottom - r.top;
}

void wh_min_max(wh_wctl *c, MINMAXINFO *mi) {
	float least[2];
	wh_least(c, least);
	float most[2] = {c->cfg.max_width, c->cfg.max_height};
	if (least[0] > 0 || least[1] > 0) {
		int w, h;
		wh_outer_size(c, least[0], least[1], &w, &h);
		if (least[0] > 0) mi->ptMinTrackSize.x = wh_maxi(mi->ptMinTrackSize.x, w);
		if (least[1] > 0) mi->ptMinTrackSize.y = wh_maxi(mi->ptMinTrackSize.y, h);
	}
	if (most[0] > 0 || most[1] > 0) {
		int w, h;
		wh_outer_size(c, wh_maxf(most[0], 1), wh_maxf(most[1], 1), &w, &h);
		if (most[0] > 0) mi->ptMaxTrackSize.x = wh_maxi(w, mi->ptMinTrackSize.x);
		if (most[1] > 0) mi->ptMaxTrackSize.y = wh_maxi(h, mi->ptMinTrackSize.y);
	}
}

// wh_clamp_size keeps a content size within the window's least and most.
void wh_clamp_size(wh_wctl *c, float *w, float *h) {
	float least[2];
	wh_least(c, least);
	*w = wh_maxf(*w, least[0]), *h = wh_maxf(*h, least[1]);
	if (c->cfg.max_width > 0) *w = wh_minf(*w, wh_maxf(c->cfg.max_width, least[0]));
	if (c->cfg.max_height > 0) *h = wh_minf(*h, wh_maxf(c->cfg.max_height, least[1]));
}

// wh_size_content sizes the window so that its content is w by h points,
// keeping its top left.
void wh_size_content(wh_wctl *c, float w, float h) {
	wh_clamp_size(c, &w, &h);
	RECT cr, wr;
	GetClientRect(c->hwnd, &cr);
	GetWindowRect(c->hwnd, &wr);
	float k = wh_ctl_k(c);
	int dw = (int)lroundf(w * k) - cr.right, dh = (int)lroundf(h * k) - cr.bottom;
	if (!dw && !dh) return;
	SetWindowPos(c->hwnd, NULL, 0, 0, wr.right - wr.left + dw, wr.bottom - wr.top + dh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

RECT wh_frame_bounds(HWND hwnd) {
	RECT r;
	if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r) != S_OK) GetWindowRect(hwnd, &r);
	return r;
}

// wh_move_to puts the window's visible frame's top left at x, y points
// from the main display's top left (its invisible resize borders outside
// it).
void wh_move_to(wh_wctl *c, float x, float y) {
	float k = wh_ctl_k(c);
	RECT wr, fb = wh_frame_bounds(c->hwnd);
	GetWindowRect(c->hwnd, &wr);
	int px = (int)lroundf(x * k) - (fb.left - wr.left), py = (int)lroundf(y * k) - (fb.top - wr.top);
	SetWindowPos(c->hwnd, NULL, px, py, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT wh_work_area(HWND hwnd) {
	MONITORINFO mi = {sizeof mi};
	GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
	return mi.rcWork;
}

RECT wh_monitor_work_area(RECT r) {
	MONITORINFO mi = {sizeof mi};
	GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
	return mi.rcWork;
}

// wh_center centers the window on the work area of the monitor of from.
void wh_center(wh_wctl *c, HWND from) {
	RECT wr, work = wh_work_area(from);
	GetWindowRect(c->hwnd, &wr);
	int w = wr.right - wr.left, h = wr.bottom - wr.top;
	int x = work.left + ((work.right - work.left) - w) / 2;
	int y = work.top + wh_maxi(((work.bottom - work.top) - h) / 2, 0);
	SetWindowPos(c->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// wh_show_ctl shows a window (activate: and gives it the keyboard): the
// first time maximized or in full screen if it opens so or was left
// maximized.
void wh_show_ctl(wh_wctl *c, bool activate) {
	if (c->wait_fit) {
		// Sized to its content, which isn't laid out yet: it shows at that
		// size.
		c->show_pending = true;
		c->show_activate = c->show_activate || activate;
		return;
	}
	wh_before_show(c);  // a panel centered, a flyout placed
	bool first = !c->placed;
	// The show the app was started with (a shortcut's Run: Maximized or
	// Minimized), for the main window as it first shows; the system's own
	// first-show override is used up as the host starts (main.c).
	STARTUPINFOW si = {sizeof si};
	GetStartupInfoW(&si);
	WORD launch = c->main && first && (si.dwFlags & STARTF_USESHOWWINDOW) && !wh_snapshotting() ? si.wShowWindow : SW_SHOWNORMAL;
	if (launch == SW_SHOWMINIMIZED || launch == SW_SHOWMINNOACTIVE || launch == SW_MINIMIZE) ShowWindow(c->hwnd, SW_SHOWMINNOACTIVE);
	else if (first && (c->restore_max || wh_ctl_has(c, AK_WINDOW_MAXIMIZED) || launch == SW_SHOWMAXIMIZED) && !c->full) ShowWindow(c->hwnd, SW_SHOWMAXIMIZED);
	else if (wh_snapshotting() || !activate) ShowWindow(c->hwnd, SW_SHOWNOACTIVATE);
	else ShowWindow(c->hwnd, SW_SHOW);
	if (first) {
		if (!IsZoomed(c->hwnd)) GetWindowRect(c->hwnd, &c->normal);
		c->placed = true;
		if (wh_ctl_has(c, AK_WINDOW_FULLSCREEN)) wh_enter_full_screen(c);
	}
	if (activate && !wh_snapshotting()) SetForegroundWindow(c->hwnd);
	wh_layout_chrome(c);
	wh_report_state(c);
}

// wh_focus_ctl shows the window if it's hidden or minimized, and brings
// it to the front with the keyboard, as Windows' foreground lock allows
// after the user's input to the app.
void wh_focus_ctl(wh_wctl *c) {
	if (IsIconic(c->hwnd)) ShowWindow(c->hwnd, SW_RESTORE);
	if (!IsWindowVisible(c->hwnd)) {
		wh_show_ctl(c, true);
		return;
	}
	if (wh_snapshotting()) return;
	BringWindowToTop(c->hwnd);
	SetForegroundWindow(c->hwnd);
}

// chrome_changed tells the core of the room a window's content has under
// its caption buttons, which full screen takes away.
static void chrome_changed(wh_wctl *c) {
	if (c->main) {
		wh_environment_changed();
		return;
	}
	if (c->n && c->n->hwnd) {
		RECT r;
		GetClientRect(c->hwnd, &r);
		wh_report_screen(c->n, wh_pt(c->n, r.right), wh_pt(c->n, r.bottom));
	}
}

void wh_enter_full_screen(wh_wctl *c) {
	if (c->full) return;
	wh_full *f = wh_alloc(sizeof *f);
	f->style = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
	f->ex = GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE);
	f->place.length = sizeof f->place;
	GetWindowPlacement(c->hwnd, &f->place);
	f->maximized = IsZoomed(c->hwnd);
	c->full = f;
	MONITORINFO mi = {sizeof mi};
	GetMonitorInfoW(MonitorFromWindow(c->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
	RECT a = mi.rcMonitor;
	SetWindowLongPtrW(c->hwnd, GWL_STYLE, (f->style & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZE | WS_MINIMIZE)) | WS_POPUP);
	SetWindowPos(c->hwnd, NULL, a.left, a.top, a.right - a.left, a.bottom - a.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	wh_layout_chrome(c);
	chrome_changed(c);
}

void wh_exit_full_screen(wh_wctl *c) {
	wh_full *f = c->full;
	if (!f) return;
	c->full = NULL;
	SetWindowLongPtrW(c->hwnd, GWL_STYLE, (f->style & ~(LONG_PTR)(WS_MAXIMIZE | WS_MINIMIZE)) | (GetWindowLongPtrW(c->hwnd, GWL_STYLE) & WS_VISIBLE));
	SetWindowPlacement(c->hwnd, &f->place);
	SetWindowPos(c->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	free(f);
	wh_layout_chrome(c);
	chrome_changed(c);
}

// wh_safe_top is the room the window's caption buttons take at the top
// of its content, in points.
float wh_safe_top(const wh_wctl *c) { return c && c->hidden && !c->full ? CAPTION_HEIGHT : 0; }

// wh_window_action runs a WindowAction command on the window n controls.
void wh_window_action(node *n, const uint8_t *args, size_t len) {
	uint8_t a;
	float x = 0, y = 0;
	bool ok = ak_decode_window_action(args, len, &a, &x, &y);
	if (n->kind == AK_KIND_SHEET) {
		// A sheet's content measured (a fit detent): Fit alone.
		if (ok && a == AK_WINDOW_ACTION_FIT) wh_sheet_fit(n, x, y);
		return;
	}
	wh_wctl *c = wh_ctl_for(n);
	if (ok && !c && a == AK_WINDOW_ACTION_FIT && n->kind == AK_KIND_WINDOW && n->nav) {
		// Laid out before its window opens: it opens at that size.
		wh_nav_fitted(n, x, y);
		return;
	}
	if (!ok || !c || !c->hwnd || !IsWindow(c->hwnd)) return;
	bool restored = c->full == NULL;
	switch (a) {
	case AK_WINDOW_ACTION_SHOW:
		if (!IsWindowVisible(c->hwnd) || c->wait_fit) wh_show_ctl(c, true);
		break;
	case AK_WINDOW_ACTION_HIDE:
		c->show_pending = false;  // not shown as its size comes either
		ShowWindow(c->hwnd, SW_HIDE);
		break;
	case AK_WINDOW_ACTION_FOCUS:
		wh_focus_ctl(c);
		break;
	case AK_WINDOW_ACTION_MINIMIZE:
		ShowWindow(c->hwnd, SW_MINIMIZE);
		break;
	case AK_WINDOW_ACTION_MAXIMIZE:
		if (restored) ShowWindow(c->hwnd, SW_MAXIMIZE);
		break;
	case AK_WINDOW_ACTION_RESTORE:
		if (IsIconic(c->hwnd) || IsZoomed(c->hwnd)) ShowWindow(c->hwnd, SW_RESTORE);
		break;
	case AK_WINDOW_ACTION_FULLSCREEN:
		wh_enter_full_screen(c);
		break;
	case AK_WINDOW_ACTION_EXIT_FULLSCREEN:
		wh_exit_full_screen(c);
		break;
	case AK_WINDOW_ACTION_CENTER:
		if (restored && !IsZoomed(c->hwnd) && !IsIconic(c->hwnd)) {
			wh_center(c, c->hwnd);
			c->app_placed = true;
		}
		break;
	case AK_WINDOW_ACTION_SIZE:
		if (restored) {
			if (IsZoomed(c->hwnd) || IsIconic(c->hwnd)) ShowWindow(c->hwnd, SW_RESTORE);
			wh_size_content(c, x, y);
		}
		break;
	case AK_WINDOW_ACTION_POSITION:
		if (restored) {
			if (IsZoomed(c->hwnd) || IsIconic(c->hwnd)) ShowWindow(c->hwnd, SW_RESTORE);
			wh_move_to(c, x, y);
			c->app_placed = true;
		}
		break;
	case AK_WINDOW_ACTION_FIT:
		wh_fit_window(c, x, y);
		break;
	case AK_WINDOW_ACTION_CLICK_THROUGH:
		wh_set_click_through(c, true);
		break;
	case AK_WINDOW_ACTION_NOT_CLICK_THROUGH:
		wh_set_click_through(c, false);
		break;
	case AK_WINDOW_ACTION_ALWAYS_ON_TOP:
		SetWindowPos(c->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		break;
	case AK_WINDOW_ACTION_NOT_ALWAYS_ON_TOP:
		SetWindowPos(c->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		break;
	case AK_WINDOW_ACTION_CLOSE:
		// The core closes a Window itself (Destroy); this is the main
		// window, closing for good: the app quits.
		if (c->main) {
			wh_close_main();
			return;
		}
		break;
	}
	wh_report_state(c);
}

// window_state is what the window's WindowState reports: its visible
// frame's top left and its content's size, in points (a minimized
// window's as it was), and its state.
static ak_window_state window_state(wh_wctl *c) {
	ak_window_state s = c->sent;
	uint8_t f = 0;
	if (IsWindowVisible(c->hwnd)) f |= AK_WINDOW_STATE_VISIBLE;
	bool iconic = IsIconic(c->hwnd);
	if (c->active && !iconic) f |= AK_WINDOW_STATE_FOCUSED;
	if (iconic) {
		f |= AK_WINDOW_STATE_MINIMIZED;
	} else {
		float k = wh_ctl_k(c);
		RECT fb = wh_frame_bounds(c->hwnd), cr;
		GetClientRect(c->hwnd, &cr);
		s.x = (float)fb.left / k, s.y = (float)fb.top / k;
		s.width = (float)cr.right / k, s.height = (float)cr.bottom / k;
	}
	if (IsZoomed(c->hwnd)) f |= AK_WINDOW_STATE_MAXIMIZED;
	if (c->full) f |= AK_WINDOW_STATE_FULLSCREEN;
	if (GetWindowLongPtrW(c->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) f |= AK_WINDOW_STATE_ON_TOP;
	if (wh_click_through(c->hwnd)) f |= AK_WINDOW_STATE_CLICK_THROUGH;
	s.flags = f;
	return s;
}

// wh_report_state sends the window's state if the core listens for it and
// it changed.
void wh_report_state(wh_wctl *c) {
	node *n = wh_ctl_node(c);
	if (!n || n->dying || !node_listening(n, AK_EVENT_WINDOW_STATE) || !c->hwnd || !IsWindow(c->hwnd)) return;
	ak_window_state s = window_state(c);
	if (c->reported && memcmp(&s, &c->sent, sizeof s) == 0) return;
	c->sent = s, c->reported = true;
	ak_buf b = {0};
	ak_append_window_state(&b, &s);
	wh_emit_bytes(n, AK_EVENT_WINDOW_STATE, b.data, b.len);
	ak_buf_free(&b);
}

static void report_later(void *ctx) {
	wh_wctl *c = wh_ctl_of(ctx);
	if (c) wh_report_state(c);
}

// wh_listen_window follows the core starting to listen for a window's
// state: it's reported at once, once the batch is applied.
void wh_listen_window(node *n) {
	wh_wctl *c = wh_ctl_for(n);
	if (!c) return;  // a Window not opened yet reports as it opens
	c->reported = false;
	wh_after_core(report_later, c->hwnd);
}

// wh_ctl_message handles what every controlled window shares, before its
// own window procedure's: its most size, its hidden title bar, the user's
// closing, and its state as it changes.
LRESULT wh_ctl_message(wh_wctl *c, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = true;
	bool h;
	LRESULT r;
	if (c->key) wh_placement_message(c, m, wp);
	r = wh_title_bar_message(c, m, wp, lp, &h);
	if (h) return r;
	r = wh_light_dismiss_message(c, m, wp, lp, &h);
	if (h) return r;
	switch (m) {
	case WM_GETMINMAXINFO:
		wh_min_max(c, (MINMAXINFO *)lp);
		return 0;
	case WM_SYSCOMMAND:
		if ((wp & 0xFFF0) == SC_CLOSE && wh_ctl_has(c, AK_WINDOW_NO_CLOSE)) return 0;
		break;
	case WM_CLOSE: {
		// The user's: Alt+F4, the close button, the taskbar's menu.
		if (wh_ctl_has(c, AK_WINDOW_NO_CLOSE)) return 0;
		node *n = wh_ctl_node(c);
		if (n && !n->dying && node_listening(n, AK_EVENT_CLOSE_REQUEST)) {
			wh_emit_kind(n, AK_EVENT_CLOSE_REQUEST);
			return 0;
		}
		break;
	}
	case WM_ACTIVATE:
		c->active = LOWORD(wp) != WA_INACTIVE;
		if (c->caption) wh_caption_activate(c, c->active);
		wh_report_state(c);
		break;
	case WM_SIZE:
		wh_layout_chrome(c);
		wh_report_state(c);
		wh_fit_covers(c->hwnd);  // its full-screen sheets
		wh_round_covers(c->hwnd);
		wh_fit_sheets(c->hwnd);  // its sheets, to their detents
		break;
	case WM_MOVE:
	case WM_WINDOWPOSCHANGED:
		wh_report_state(c);
		wh_fit_covers(c->hwnd);
		break;
	}
	*handled = false;
	return 0;
}

// wh_close_main closes the main window, as its close button does when
// nothing asks first: for an app that keeps running with no window it
// hides, its root mounted; else the app quits, unless its on quit
// handlers keep it open.
void wh_close_main(void) {
	wh_save_placement(H.main_ctl);
	if ((H.config.keep_running || H.config.background || H.keep_running) && !H.quitting) {
		ShowWindow(H.window, SW_HIDE);
		wh_lifecycle(AK_LIFECYCLE_BACKGROUND);
		wh_report_state(H.main_ctl);
		return;
	}
	if (!wh_ask_quit(false)) return;
	H.quitting = true;
	DestroyWindow(H.window);
}

// wh_bg_of is the background of the top-level window hwnd: its options'
// color over the theme's, or the theme's.
rgba wh_bg_of(HWND hwnd) {
	wh_wctl *c = wh_ctl_of(hwnd);
	rgba bg;
	if (c && c->cfg.background && wh_resolve(c->cfg.background, &bg)) return rgba_over(bg, H.theme.bg);
	return H.theme.bg;
}

// wh_bg_in is the window background behind a view: its top-level
// window's.
rgba wh_bg_in(HWND hwnd) {
	if (!hwnd || !H.ctls.len) return H.theme.bg;
	return wh_bg_of(GetAncestor(hwnd, GA_ROOT));
}

// wh_under_caption reports whether a view is in a window whose title bar
// is hidden (its top strip the caption).
bool wh_under_caption(HWND hwnd) {
	wh_wctl *c = wh_ctl_of(GetAncestor(hwnd, GA_ROOT));
	return c && c->hidden && !c->full;
}

// wh_show_main shows the app's window, where the user left it (unless it
// opens hidden: WindowAction Show shows it), and its Windows.
void wh_show_main(void) {
	wh_wctl *c = H.main_ctl;
	wh_restore_placement(c);
	H.started = true;
	if (!wh_ctl_has(c, AK_WINDOW_HIDDEN)) {
		wh_show_ctl(c, true);
		UpdateWindow(H.window);
	}
	for (uint32_t i = 0; i < H.nwins; i++) {
		wh_wctl *w = wh_nav_ctl(H.wins[i]);
		if (w && !wh_ctl_has(w, AK_WINDOW_HIDDEN) && !IsWindowVisible(w->hwnd)) wh_show_ctl(w, true);
	}
}

// ---- Placement: each window comes back where the user left it, as
// macOS's frame autosave has it, by its key, in asuka-window.json in the
// app's data directory. The host keeps the normal rectangle itself, in
// screen coordinates (GetWindowPlacement's are workspace coordinates). ----

static dsk_winstate *store(void) { return H.states; }

void wh_save_placement(wh_wctl *c) {
	if (!c || !c->key || !store() || !c->placed || c->full || !IsWindow(c->hwnd) || IsIconic(c->hwnd)) return;
	bool zoomed = IsZoomed(c->hwnd);
	if (!zoomed) GetWindowRect(c->hwnd, &c->normal);
	RECT r = c->normal;
	if (r.right <= r.left || r.bottom <= r.top) return;
	char json[160];
	snprintf(json, sizeof json, "{\"X\":%ld,\"Y\":%ld,\"Width\":%ld,\"Height\":%ld,\"DPI\":%d,\"Maximized\":%s}", (long)r.left, (long)r.top,
		(long)(r.right - r.left), (long)(r.bottom - r.top), wh_dpi(c->hwnd), zoomed ? "true" : "false");
	dsk_winstate_save(store(), c->key, json);
}

static LONGLONG area(RECT r) { return (LONGLONG)wh_maxi(r.right - r.left, 0) * wh_maxi(r.bottom - r.top, 0); }

// on_screen moves a window's rectangle onto the work area of the monitor
// it's most on (or the nearest), unless it's on it already: its top in
// it, and at least half of it.
static RECT on_screen(RECT r) {
	RECT work = wh_monitor_work_area(r);
	RECT in = {wh_maxi(r.left, work.left), wh_maxi(r.top, work.top), wh_mini(r.right, work.right), wh_mini(r.bottom, work.bottom)};
	if (r.top >= work.top && r.top < work.bottom && 2 * area(in) >= area(r)) return r;
	int w = wh_mini(r.right - r.left, work.right - work.left), h = wh_mini(r.bottom - r.top, work.bottom - work.top);
	int x = wh_mini(wh_maxi(r.left, work.left), work.right - w), y = wh_mini(wh_maxi(r.top, work.top), work.bottom - h);
	return (RECT){x, y, x + w, y + h};
}

bool wh_restore_placement(wh_wctl *c) {
	if (!c || !c->key || !store()) return false;
	const dsk_json *p = dsk_winstate_load(store(), c->key);
	if (!p) return false;
	int x = (int)dsk_json_num(p, "X", 0), y = (int)dsk_json_num(p, "Y", 0);
	int w = (int)dsk_json_num(p, "Width", 0), h = (int)dsk_json_num(p, "Height", 0), pdpi = (int)dsk_json_num(p, "DPI", 0);
	bool maximized = dsk_json_bool(p, "Maximized", false);
	if (w <= 0 || h <= 0) return false;
	RECT r = on_screen((RECT){x, y, x + w, y + h});
	// To its monitor first, so the window takes its DPI, then its size
	// there: in pixels at the DPI it was saved at, scaled if the monitor's
	// is another.
	SetWindowPos(c->hwnd, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	int dpi = wh_dpi(c->hwnd);
	RECT wr;
	GetWindowRect(c->hwnd, &wr);
	if (c->main && dpi != H.dpi) wh_dpi_changed(dpi, &wr);
	else if (c->frame && (float)dpi / 96 != c->frame->k) wh_window_dpi_changed(c->frame, dpi, &wr);
	if (pdpi > 0 && pdpi != dpi) {
		r.right = r.left + (LONG)((LONGLONG)(r.right - r.left) * dpi / pdpi);
		r.bottom = r.top + (LONG)((LONGLONG)(r.bottom - r.top) * dpi / pdpi);
	}
	MINMAXINFO mi = {0};
	wh_min_max(c, &mi);
	r.right = r.left + wh_maxi(r.right - r.left, mi.ptMinTrackSize.x);
	r.bottom = r.top + wh_maxi(r.bottom - r.top, mi.ptMinTrackSize.y);
	if (mi.ptMaxTrackSize.x > 0) r.right = r.left + wh_mini(r.right - r.left, mi.ptMaxTrackSize.x);
	if (mi.ptMaxTrackSize.y > 0) r.bottom = r.top + wh_mini(r.bottom - r.top, mi.ptMaxTrackSize.y);
	r = on_screen(r);
	SetWindowPos(c->hwnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	c->normal = r;
	c->restore_max = maximized;
	c->app_placed = true;  // a panel shows there, not where the cursor is
	return true;
}

// wh_placement_message follows the user moving, sizing, maximizing and
// restoring a window, to save its placement when it settles.
void wh_placement_message(wh_wctl *c, UINT m, WPARAM wp) {
	switch (m) {
	case WM_ENTERSIZEMOVE:
		c->sizing = true;
		break;
	case WM_EXITSIZEMOVE:
		c->sizing = false;
		wh_save_placement(c);
		break;
	case WM_SIZE:
		// Maximized, restored, snapped by the keyboard: not while a drag
		// goes on.
		if (!c->sizing && (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED)) wh_save_placement(c);
		break;
	case WM_MOVE:
		if (!c->sizing && !IsZoomed(c->hwnd)) wh_save_placement(c);
		break;
	}
}
