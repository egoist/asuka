// automation.c: the debug automation: ASUKA_SCRIPT's steps, then
// ASUKA_SNAPSHOT (the window as a PNG once it has shown, ASUKA_DUMP the
// view tree beside it) and quit. The steps run on the main thread, a timer
// apart: 1.5 s before the first (images and fonts load), 0.4 s between
// them. ASUKA_COMMANDS runs them from a file while the app runs.
#include "win.h"
#include "nav.h"

#include <wincodec.h>

bool wh_snapshotting(void) { return H.snapshotting; }

// ---- PNGs, by WIC's encoder ----

// wh_write_png writes w by h pixels (BGRA, top row first) as a PNG.
bool wh_write_png(const char *path, int w, int h, const uint8_t *bgra, int stride) {
	IWICImagingFactory *f = NULL;
	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) return false;
	bool ok = false;
	IWICStream *s = NULL;
	IWICBitmapEncoder *e = NULL;
	IWICBitmapFrameEncode *fr = NULL;
	wchar_t *wp = wh_wide(path, (int)strlen(path));
	if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &s)) && SUCCEEDED(IWICStream_InitializeFromFilename(s, wp, GENERIC_WRITE)) &&
		SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &e)) &&
		SUCCEEDED(IWICBitmapEncoder_Initialize(e, (IStream *)s, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(e, &fr, NULL)) && SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, NULL)) &&
		SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, (UINT)w, (UINT)h))) {
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
		if (SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt)) && IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA) &&
			SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, (UINT)h, (UINT)stride, (UINT)(stride * h), (BYTE *)bgra)) &&
			SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(e)))
			ok = true;
	}
	free(wp);
	if (fr) IWICBitmapFrameEncode_Release(fr);
	if (e) IWICBitmapEncoder_Release(e);
	if (s) IWICStream_Release(s);
	IWICImagingFactory_Release(f);
	return ok;
}

static HBITMAP dib(HDC dc, int w, int h, uint8_t **px) {
	BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
	return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)px, NULL, 0);
}

static void opaque(uint8_t *px, size_t n) {
	for (size_t i = 0; i < n; i++) px[i * 4 + 3] = 255;
}

// write_window_png writes what a window shows (PrintWindow, without
// bringing it to the front): only what shows, not its invisible resize
// borders.
static void write_window_png(HWND hwnd, const char *path) {
	RECT r, vis = wh_frame_bounds(hwnd);
	GetWindowRect(hwnd, &r);
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	HDC screen = GetDC(NULL);
	HDC dc = CreateCompatibleDC(screen);
	ReleaseDC(NULL, screen);
	uint8_t *px;
	HBITMAP bmp = dib(dc, w, h, &px);
	HGDIOBJ old = SelectObject(dc, bmp);
	PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);
	if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) {
		// A mirrored window comes out of PrintWindow mirrored again (its
		// caption buttons on the right, its text backwards): turned back,
		// as the screen shows it.
		GdiFlush();
		for (int y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)px + (size_t)y * (size_t)w;
			for (int a = 0, b = w - 1; a < b; a++, b--) {
				uint32_t t = row[a];
				row[a] = row[b], row[b] = t;
			}
		}
	}
	if (hwnd == H.window) {
		wh_dev_error_print(dc, r);  // asuka dev's error overlay, over it
		wh_dev_perf_print(dc, r);   // and its performance overlay, over all
	}
	GdiFlush();
	int x0 = vis.left - r.left, y0 = vis.top - r.top, vw = vis.right - vis.left, vh = vis.bottom - vis.top;
	if (x0 < 0 || y0 < 0 || x0 + vw > w || y0 + vh > h || vw <= 0 || vh <= 0) x0 = y0 = 0, vw = w, vh = h;
	opaque(px, (size_t)w * (size_t)h);
	wh_write_png(path, vw, vh, px + ((size_t)y0 * (size_t)w + (size_t)x0) * 4, w * 4);
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
}

// write_screen_png writes what the screen shows at r.
static void write_screen_png(RECT r, const char *path) {
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	HDC screen = GetDC(NULL);
	HDC dc = CreateCompatibleDC(screen);
	uint8_t *px;
	HBITMAP bmp = dib(dc, w, h, &px);
	HGDIOBJ old = SelectObject(dc, bmp);
	BitBlt(dc, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
	ReleaseDC(NULL, screen);
	GdiFlush();
	opaque(px, (size_t)w * (size_t)h);
	wh_write_png(path, w, h, px, w * 4);
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
}

static HWND open_menu_window(void) {
	GUITHREADINFO gi = {sizeof gi};
	if (!GetGUIThreadInfo(GetCurrentThreadId(), &gi) || !(gi.flags & GUI_INMENUMODE)) return NULL;
	return FindWindowW(L"#32768", NULL);
}

// wh_snapshot writes the window, its title bar too, as a PNG; with sheets
// open the top sheet's beside it (file.sheet.png), a menu open as the
// screen shows it (file.menu.png), each Window open (file.window.png,
// file.window2.png…).
void wh_snapshot(const char *path) {
	write_window_png(H.window, path);
	size_t n = strlen(path);
	char *base = wh_strndup(path, n >= 4 && _stricmp(path + n - 4, ".png") == 0 ? n - 4 : n);
	char name[MAX_PATH * 3];
	if (H.nsheets) {
		snprintf(name, sizeof name, "%s.sheet.png", base);
		write_window_png(wh_nav_window(H.sheets[H.nsheets - 1]), name);
	}
	HWND menu = open_menu_window();
	if (menu) {
		RECT r;
		GetWindowRect(menu, &r);
		snprintf(name, sizeof name, "%s.menu.png", base);
		write_screen_png(r, name);
	}
	HWND list = wh_suggest_popup();
	if (list) {
		// A field's suggestions, with the window they're over, as the
		// screen shows them.
		RECT r, w = wh_frame_bounds(H.window);
		GetWindowRect(list, &r);
		UnionRect(&r, &r, &w);
		snprintf(name, sizeof name, "%s.list.png", base);
		write_screen_png(r, name);
	}
	for (uint32_t i = 0; i < H.nwins; i++) {
		if (i == 0) snprintf(name, sizeof name, "%s.window.png", base);
		else snprintf(name, sizeof name, "%s.window%u.png", base, i + 1);
		write_window_png(wh_nav_window(H.wins[i]), name);
	}
	free(base);
}

// ---- The view tree, described ----

static const char *kind_name(uint16_t k, char *buf) {
	switch (k) {
	case KIND_ROW: return "Row";
	case KIND_CELL: return "Cell";
	case KIND_HEADER: return "Header";
	case KIND_TAB_BAR: return "TabBar";
	case KIND_COMMAND_BAR: return "CommandBar";
	case KIND_WINDOW_FRAME: return "WindowFrame";
	case KIND_TABLE_CELL: return "TableCell";
	}
	const char *s = k < AK_KIND_COUNT ? ak_kind_name((ak_view_kind)k) : NULL;
	if (s) return s;
	sprintf(buf, "Kind(%u)", k);
	return buf;
}

static void quote_w(ak_buf *b, const wstr *s) {
	int n;
	char *u = wh_utf8(s->p, s->n, &n);
	ak_fmt_quote(b, ak_strn(u, (size_t)n));
	free(u);
}

static void walk(ak_buf *b, node *n, int depth) {
	char kb[32];
	for (int i = 0; i < depth; i++) ak_buf_put(b, "  ", 2);
	ak_buf_printf(b, "%u %s", n->id & ~AK_HOST_ID_BIT, kind_name(n->kind, kb));
	if (n->hwnd) {
		HWND p = GetParent(n->hwnd);
		RECT r = wh_rect_in(n->hwnd, p);
		ak_buf_printf(b, " [%.0f,%.0f %.0fx%.0f]", wh_pt(n, r.left), wh_pt(n, r.top), wh_pt(n, r.right - r.left), wh_pt(n, r.bottom - r.top));
		if (!IsWindowVisible(n->hwnd)) ak_buf_put(b, " hidden", 7);
		if (p == H.park) ak_buf_put(b, " parked", 7);
		if (n == wh_focused_node()) ak_buf_put(b, " focused", 8);
	}
	if (n->text.n) {
		ak_buf_u8(b, ' ');
		quote_w(b, &n->text);
	}
	if (n->kind == AK_KIND_RICH_TEXT && n->ctl) {
		// Its text as the control has it, its paragraphs ended by \n.
		int len = GetWindowTextLengthW(n->ctl);
		wstr t = {0};
		if (len > 0) {
			wchar_t *s = wh_alloc(((size_t)len + 1) * sizeof *s);
			len = GetWindowTextW(n->ctl, s, len + 1);
			int k = 0;
			for (int i = 0; i < len; i++) {
				if (s[i] == '\r' && i + 1 < len && s[i + 1] == '\n') continue;
				s[k++] = s[i] == '\r' ? '\n' : s[i];
			}
			wstr_setw(&t, s, k);
			free(s);
			ak_buf_u8(b, ' ');
			quote_w(b, &t);
			wstr_free(&t);
		}
	}
	if (n->title.n) {
		ak_buf_put(b, " title=", 7);
		quote_w(b, &n->title);
	}
	if (n->kind == AK_KIND_CHIP || n->kind == AK_KIND_DISCLOSURE) wh_own_dump(n, b);
	if (n->kind == AK_KIND_DATE_RANGE_PICKER) wh_range_dump(n, b);
	if (n->suggest) wh_suggest_dump(n, b);
	if (wh_zoomable(n)) ak_buf_printf(b, " zoom=%.2f offset=%.0f,%.0f", (double)wh_scroll_zoom(n), (double)n->scroll_x, (double)n->scroll_y);
	ak_buf_u8(b, '\n');
	for (uint32_t i = 0; i < n->nkids; i++) walk(b, n->kids[i], depth + 1);
	if (n->list) wh_list_dump(n, b, depth, walk);
	if (n->table && n->kind != KIND_TABLE_CELL) wh_table_dump(n, b, depth, walk);
}

static void menu_items(ak_buf *b, HMENU m, int depth) {
	int count = GetMenuItemCount(m);
	for (int j = 0; j < count; j++) {
		wchar_t s[256] = {0};
		GetMenuStringW(m, (UINT)j, s, 256, MF_BYPOSITION);
		UINT state = GetMenuState(m, (UINT)j, MF_BYPOSITION);
		wstr w = {0};
		if (state & MF_SEPARATOR) wstr_setw(&w, L"---", 3);
		else if (state & MF_GRAYED) {
			wchar_t t[300];
			swprintf(t, 300, L"%ls (off)", s);
			wstr_setw(&w, t, -1);
		} else wstr_setw(&w, s, -1);
		for (int i = 0; i < depth; i++) ak_buf_put(b, "  ", 2);
		quote_w(b, &w);
		ak_buf_u8(b, '\n');
		wstr_free(&w);
		HMENU sub = GetSubMenu(m, j);
		if (sub) menu_items(b, sub, depth + 1);
	}
}

char *wh_dump(size_t *len) {
	ak_buf b = {0};
	if (H.root) walk(&b, H.root, 0);
	for (uint32_t i = 0; i < H.nsheets; i++) {
		// Its client area's size, in points, and whether its window is
		// kept from the user (modal) or usable under it (an undimmed
		// detent).
		node *s = H.sheets[i];
		char line[96];
		RECT cr = {0};
		if (s->nav->window) GetClientRect(s->nav->window, &cr);
		int k = snprintf(line, sizeof line, "sheet: %gx%g %s\n", (double)lroundf(wh_pt(s, cr.right)), (double)lroundf(wh_pt(s, cr.bottom)),
		                 s->nav->owner && IsWindowEnabled(s->nav->owner) ? "usable" : "modal");
		ak_buf_put(&b, line, (size_t)k);
		walk(&b, s, 1);
	}
	wh_overlays_dump(&b, walk);
	wh_dev_error_dump(&b);
	wh_dev_perf_dump(&b);
	for (uint32_t i = 0; i < H.nwins; i++) {
		wchar_t t[256] = {0};
		GetWindowTextW(wh_nav_window(H.wins[i]), t, 256);
		wstr w = {0};
		wstr_setw(&w, t, -1);
		ak_buf_put(&b, "window ", 7);
		quote_w(&b, &w);
		wstr_free(&w);
		ak_buf_put(&b, ":\n", 2);
		walk(&b, H.wins[i], 1);
		wh_click_region_dump(&b, wh_nav_window(H.wins[i]));
	}
	HMENU bar = GetMenu(H.window);
	if (bar) {
		ak_buf_put(&b, "menu bar:\n", 10);
		int count = GetMenuItemCount(bar);
		for (int i = 0; i < count; i++) {
			wchar_t s[256] = {0};
			GetMenuStringW(bar, (UINT)i, s, 256, MF_BYPOSITION);
			char *u = wh_utf8(s, -1, NULL);
			ak_buf_printf(&b, "  %s\n", u);
			free(u);
			menu_items(&b, GetSubMenu(bar, i), 2);
		}
	}
	// The pointer's shape the app last set (a view's Cursor; a field's
	// I-beam), wherever the pointer is.
	ak_buf_printf(&b, "cursor: %s\n", wh_cursor_name(GetCursor()));
	char *out = wh_strndup(b.data ? (const char *)b.data : "", b.len);
	if (len) *len = b.len;
	ak_buf_free(&b);
	return out;
}

static void write_dump(const char *path) {
	char *dump_path = wh_alloc(strlen(path) + 5);
	sprintf(dump_path, "%s.txt", path);
	size_t n;
	char *d = wh_dump(&n);
	dsk_write_file(dump_path, d, n);
	free(d);
	free(dump_path);
}

// ---- Finding views ----

// wh_on_screen reports whether n's window shows: it and its ancestors are
// visible, in a window of the app, its center in its parents' (not
// scrolled out of a list's).
bool wh_on_screen(node *n) {
	if (!n->hwnd || !IsWindowVisible(n->hwnd)) return false;
	HWND w = GetAncestor(n->hwnd, GA_ROOT);
	if (w != H.window && !wh_node_of(w)) return false;
	RECT r;
	GetWindowRect(n->hwnd, &r);
	POINT c = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
	for (HWND p = GetParent(n->hwnd); p; p = GetParent(p)) {
		RECT pr;
		GetWindowRect(p, &pr);
		if (!PtInRect(&pr, c)) return false;
		if (p == w) break;
	}
	return true;
}

static bool wstr_is(const wstr *s, const wchar_t *t) { return s->n && wcscmp(s->p, t) == 0; }

static bool shows(node *n, const wchar_t *t) {
	return (n->kind != AK_KIND_TEXT_FIELD && wstr_is(&n->text, t)) ||
		((n->kind == AK_KIND_BUTTON || n->kind == AK_KIND_CHIP || n->kind == AK_KIND_DISCLOSURE) && wstr_is(&n->title, t)) ||
		(n->kind == AK_KIND_TEXT_FIELD && wstr_is(&n->placeholder, t));
}

// showing is the view on screen that shows text (the last made, if
// many): a text, a button's title, a field's placeholder.
static node *showing(const wchar_t *t) {
	node *found = NULL;
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (wh_on_screen(n) && shows(n, t) && (!found || n->id > found->id)) found = n;
	}
	return found;
}

static LPARAM center_in(node *n, node *t) {
	RECT c;
	GetClientRect(n->hwnd, &c);
	POINT p = {c.right / 2, c.bottom / 2};
	MapWindowPoints(n->hwnd, t->hwnd, &p, 1);
	return MAKELPARAM(p.x, p.y);
}

static node *interactive_up(node *n) {
	while (n && !wh_interactive(n)) n = n->parent;
	return n && n->hwnd ? n : NULL;
}

static HWND held_hwnd;
static LPARAM held_at;
// tapped is the field a tap focused: where typing goes when the focus
// isn't the app's (a program the user didn't start isn't the foreground).
static HWND tapped;

static HWND typing_target(void) {
	HWND f = GetFocus();
	if (f) return f;
	return tapped && IsWindow(tapped) ? tapped : NULL;
}

// click sends a click's messages to hwnd at lp (its client's), as the
// message loop dispatches them: the overlays hear of the press first.
static void click(HWND hwnd, LPARAM lp) {
	POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
	ClientToScreen(hwnd, &p);
	MSG m = {hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp, 0, p};
	wh_input_seen(&m);
	wh_overlay_pointer_down(hwnd, p);
	if (!IsWindow(hwnd)) return;
	wh_pressing(true);
	SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
	wh_pressing(false);
	if (IsWindow(hwnd)) SendMessageW(hwnd, WM_LBUTTONUP, 0, lp);
}

// tap clicks what shows text: the mouse's messages, at its center, to the
// window under it that takes them.
static bool tap(const wchar_t *t) {
	if (wh_dev_error_tap(t)) return true;  // asuka dev's error overlay, over the rest
	node *found = showing(t);
	if (!found && (wh_tap_nav(t) || wh_tap_overlay(t))) return true;  // a tab, a header's or command bar's button; a snackbar's, a dialog's...
	if (!found) return false;
	if (found->kind == AK_KIND_TEXT_FIELD) {
		SetFocus(found->ctl);
		tapped = found->ctl;
		return true;
	}
	if (found->kind == AK_KIND_BUTTON || found->kind == AK_KIND_TOGGLE) {
		RECT r;
		GetWindowRect(found->ctl, &r);
		wh_overlay_pointer_down(found->ctl, (POINT){(r.left + r.right) / 2, (r.top + r.bottom) / 2});
		if (!IsWindow(found->ctl)) return true;
		wh_pressing(true);
		SendMessageW(found->ctl, BM_CLICK, 0, 0);
		wh_pressing(false);
		return true;
	}
	node *target = interactive_up(found);
	if (!target) return false;
	LPARAM lp = center_in(found, target);
	click(target->hwnd, lp);
	return true;
}

// tap_at clicks at x, y (points) in the window in use's root, on the
// window there as Windows hit-tests it (through what lets the mouse
// through, onto a modal overlay), as the message loop gets a click.
static bool tap_at(const char *arg) {
	float x = 0, y = 0;
	if (sscanf(arg, "%f,%f", &x, &y) != 2) return false;
	HWND top = wh_window_in_use();
	node *r = wh_root_of(top);
	if (!r || !r->hwnd) return false;
	POINT p = {wh_px(r, x), wh_px(r, y)};
	ClientToScreen(r->hwnd, &p);
	HWND w = wh_hit(top, p);
	POINT c = p;
	ScreenToClient(w, &c);
	click(w, MAKELPARAM(c.x, c.y));
	return true;
}


// press_keys presses a shortcut (ctrl+shift+k) in the window in use, as
// the keyboard would: the modifiers held (the thread's key state) while
// the key's down message goes through the message loop's handling.
static void press_keys(const char *spec) {
	char buf[64];
	snprintf(buf, sizeof buf, "%s", spec);
	for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
	uint8_t mods = 0;
	char *key = buf;
	for (char *plus; (plus = strchr(key, '+')) && plus[1];) {
		*plus = 0;
		if (!strcmp(key, "ctrl") || !strcmp(key, "control")) mods |= 8;
		else if (!strcmp(key, "shift")) mods |= 2;
		else if (!strcmp(key, "alt")) mods |= 4;
		key = plus + 1;
	}
	ACCEL a;
	if (!wh_accelerator(key, mods, &a)) {
		wh_log("automation: no key \"%s\"", spec);
		return;
	}
	HWND menu = open_menu_window();
	if (menu) {
		// A menu showing takes the key (arrows, Enter, an access key).
		if (strlen(key) == 1 && !mods) SendMessageW(menu, WM_CHAR, (WPARAM)key[0], 0);
		else SendMessageW(menu, WM_KEYDOWN, a.key, 0);
		return;
	}
	BYTE saved[256], st[256];
	GetKeyboardState(saved);
	memcpy(st, saved, sizeof st);
	if (a.fVirt & FCONTROL) st[VK_CONTROL] = 0x80;
	if (a.fVirt & FSHIFT) st[VK_SHIFT] = 0x80;
	if (a.fVirt & FALT) st[VK_MENU] = 0x80;
	SetKeyboardState(st);
	wh_key_pressed();
	HWND target = typing_target();  // the focus, or the field tapped last
	if (!target || !wh_key_window(target)) target = wh_window_in_use();
	MSG m = {target, (a.fVirt & FALT) ? WM_SYSKEYDOWN : WM_KEYDOWN, a.key, 0, 0, {0, 0}};
	if (!wh_pre_translate(&m)) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	SetKeyboardState(saved);
}

static HWND last_window(void) {
	HWND w = wh_window_in_use();
	if (H.nwins && w == H.window) w = wh_nav_window(H.wins[H.nwins - 1]);
	return w;
}

// A scripted hover, run as a mouse move is: once the core has returned,
// its reports each a call into the core of its own (whose flush may
// destroy the views being reported).
typedef struct {
	HWND top;
	POINT p;
} hover_job;

static void hover_now(void *ctx) {
	hover_job *j = ctx;
	wh_hover_at(j->top, j->p);
	wh_hover_moves(j->top, j->p);
	free(j);
}

static void hover_later(HWND top, POINT p) {
	hover_job *j = wh_alloc(sizeof *j);
	j->top = top, j->p = p;
	wh_after_core(hover_now, j);
}

static bool mouse_step(const char *verb, const char *arg, const wchar_t *warg);
static bool real_step(const char *verb, const char *arg, const wchar_t *warg);

static void step(const char *verb, const char *arg) {
	wchar_t *warg = wh_wide(arg, (int)strlen(arg));
	if (!strcmp(verb, "hover")) {
		if (!*arg) {
			hover_later(NULL, (POINT){0, 0});
		} else {
			node *n = showing(warg);
			if (!n) wh_log("automation: nothing shows \"%s\"", arg);
			else {
				RECT c;
				GetClientRect(n->hwnd, &c);
				POINT p = {c.right / 2, c.bottom / 2};
				ClientToScreen(n->hwnd, &p);
				hover_later(GetAncestor(n->hwnd, GA_ROOT), p);
			}
		}
	} else if (!strcmp(verb, "press")) {
		node *n = showing(warg), *t = interactive_up(n);
		if (!n || !t) wh_log("automation: nothing to press shows \"%s\"", arg);
		else {
			held_hwnd = t->hwnd, held_at = center_in(n, t);
			SendMessageW(held_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, held_at);
		}
	} else if (!strcmp(verb, "release")) {
		if (held_hwnd && IsWindow(held_hwnd)) SendMessageW(held_hwnd, WM_LBUTTONUP, 0, held_at);
		held_hwnd = NULL;
	} else if (!strcmp(verb, "tap")) {
		if (!tap(warg)) wh_log("automation: nothing shows \"%s\"", arg);
	} else if (!strcmp(verb, "click")) {
		// A click at the middle of what shows text, on the window Windows
		// hit-tests there (an overlay over it, modal, takes it).
		node *n = showing(warg);
		if (!n) wh_log("automation: nothing shows \"%s\"", arg);
		else {
			RECT r;
			GetWindowRect(n->hwnd, &r);
			POINT p = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
			HWND w = wh_hit(GetAncestor(n->hwnd, GA_ROOT), p);
			POINT c = p;
			ScreenToClient(w, &c);
			click(w, MAKELPARAM(c.x, c.y));
		}
	} else if (!strcmp(verb, "tapat")) {
		if (!tap_at(arg)) wh_log("automation: no point \"%s\"", arg);
	} else if (!strcmp(verb, "focus")) {
		// The keyboard's focus to what shows text, as the app's Focus does.
		node *n = showing(warg);
		if (!n) wh_log("automation: nothing shows \"%s\"", arg);
		else {
			for (node *x = n; x; x = x->parent)
				if (wh_takes_focus(x)) {
					SetFocus(x->ctl ? x->ctl : x->hwnd);
					break;
				}
		}
	} else if (!strcmp(verb, "type")) {
		HWND f = typing_target();
		if (f)
			for (wchar_t *p = warg; *p; p++) SendMessageW(f, WM_CHAR, *p, 0);
	} else if (!strcmp(verb, "submit")) {
		HWND f = typing_target();
		if (f) SendMessageW(f, WM_KEYDOWN, VK_RETURN, 0);
	} else if (!strcmp(verb, "back")) {
		wh_back(H.nsheets ? H.sheets[H.nsheets - 1] : H.root);
	} else if (!strcmp(verb, "key")) {
		press_keys(arg);
	} else if (!strcmp(verb, "open")) {
		if (!wh_open_bar_menu(warg)) wh_log("automation: no menu \"%s\"", arg);
	} else if (!strcmp(verb, "menu")) {
		if (!wh_choose_menu_item(warg)) wh_log("automation: no menu item \"%s\"", arg);
	} else if (!strcmp(verb, "fault")) {
		// An access violation nothing handles: what the crash record's
		// exception filter (crash.c) reports.
		*(volatile int *)(uintptr_t)8 = 1;
	} else if (!strcmp(verb, "close")) {
		SendMessageW(last_window(), WM_CLOSE, 0, 0);
	} else if (!strcmp(verb, "dpi")) {
		// As if the window went to a monitor at this DPI: Windows'
		// WM_DPICHANGED, with the size it suggests (its client area's
		// points kept). The frame, drawn by Windows, keeps its DPI.
		HWND w = last_window();
		int dpi = atoi(arg);
		if (dpi >= 48) {
			RECT cr, wr;
			GetClientRect(w, &cr);
			GetWindowRect(w, &wr);
			float k = (float)wh_dpi(w) / 96;
			node *f = wh_node_of(w);
			if (f && f->k > 0) k = f->k;
			else if (w == H.window) k = H.scale;
			float nk = (float)dpi / 96;
			RECT s = {wr.left, wr.top, 0, 0};
			s.right = s.left + (wr.right - wr.left) - cr.right + (LONG)((float)cr.right / k * nk + 0.5f);
			s.bottom = s.top + (wr.bottom - wr.top) - cr.bottom + (LONG)((float)cr.bottom / k * nk + 0.5f);
			SendMessageW(w, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&s);
		}
	} else if (!strcmp(verb, "resize")) {
		int w = 0, h = 0;
		sscanf(arg, "%d,%d", &w, &h);
		RECT r = {0, 0, wh_px(NULL, (float)w), wh_px(NULL, (float)h)};
		wh_adjust_window_rect(&r, (DWORD)GetWindowLongPtrW(H.window, GWL_STYLE), 0, H.dpi);
		SetWindowPos(H.window, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	} else if (!strcmp(verb, "scroll")) {
		// The innermost scroll view showing, to a y.
		float y = (float)atof(arg);
		node *s = NULL;
		uint32_t i = 0;
		void *v;
		while (wh_map_next(&H.nodes, &i, NULL, &v)) {
			node *n = v;
			if ((n->kind == AK_KIND_SCROLL || n->kind == AK_KIND_LIST) && wh_on_screen(n) && (!s || n->id > s->id)) s = n;
		}
		if (s) wh_scroll_to_offset(s, 0, y);
	} else if (!strcmp(verb, "remove")) {
		// A click on a removable chip's remove button, the chip found by
		// its title.
		HWND h;
		LPARAM lp;
		if (!wh_chip_remove_point(warg, &h, &lp)) wh_log("automation: no removable chip \"%s\"", arg);
		else click(h, lp);
	} else if (!strcmp(verb, "dropdown")) {
		// A click on a Dropdown field's chevron (the field by its
		// placeholder; empty: the first).
		if (!wh_suggest_chevron(warg)) wh_log("automation: no dropdown field \"%s\"", arg);
	} else if (!strcmp(verb, "pickdate")) {
		// pickdate:start,2026-12-01: a day chosen on a DateRangePicker's
		// picker (start or end).
		char which[8] = {0};
		const char *comma = strchr(arg, ',');
		if (comma && comma - arg < (ptrdiff_t)sizeof which) memcpy(which, arg, (size_t)(comma - arg));
		if (!comma || !wh_range_pick(which, comma + 1)) wh_log("automation: no date range picker for \"%s\"", arg);
	} else if (!strcmp(verb, "calday")) {
		// A click on a day of an inline DateRangePicker's calendar.
		if (!wh_range_click_day(atoi(arg))) wh_log("automation: no calendar day \"%s\"", arg);
	} else if (!strcmp(verb, "escape")) {
		if (open_menu_window()) EndMenu();
		else if (H.nsheets) wh_cancel_sheet(wh_node_of(wh_nav_window(H.sheets[H.nsheets - 1])));
	} else if (mouse_step(verb, arg, warg)) {
	} else if (real_step(verb, arg, warg)) {
	} else if (wh_interaction_step(verb, warg)) {
	} else if (!strcmp(verb, "snap")) {
		wh_snapshot(arg);
		if (getenv("ASUKA_DUMP")) write_dump(arg);
	}
	free(warg);
}

// ---- The mouse held down, moved and let go of ----

static POINT pointer;  // where the scripted mouse is, on the screen
static HWND pointer_on;  // the window it went down on

static bool center_of(const char *arg, const wchar_t *warg, POINT *p) {
	node *n = showing(warg);
	if (!n) {
		wh_log("automation: nothing shows \"%s\"", arg);
		return false;
	}
	RECT r;
	GetWindowRect(n->hwnd, &r);
	*p = (POINT){(r.left + r.right) / 2, (r.top + r.bottom) / 2};
	return true;
}

// mouse_to sends a mouse message at screen point p to the window that
// has the mouse (the one it went down on), in its client's coordinates.
static void mouse_to(UINT m, WPARAM keys, POINT p) {
	HWND w = GetCapture();
	if (!w || GetWindowThreadProcessId(w, NULL) != GetCurrentThreadId()) w = pointer_on;
	if (!w || !IsWindow(w)) return;
	POINT c = p;
	ScreenToClient(w, &c);
	SendMessageW(w, m, keys, MAKELPARAM(c.x, c.y));
}

// mouse_step runs the steps that hold the mouse down and move it:
//
//   - down:<text>: the button down at the middle of what shows text, on
//     the window Windows hit-tests there.
//   - move:<text>, moveby:<dx>,<dy> (points): the mouse moved with the
//     button held, in ten moves, to what shows text, or by that much.
//   - up: the button let go of where the mouse is.
//   - dblclick:<text>: a double click there (its second press Windows's
//     WM_LBUTTONDBLCLK).
static bool mouse_step(const char *verb, const char *arg, const wchar_t *warg) {
	if (!strcmp(verb, "down") || !strcmp(verb, "dblclick")) {
		POINT p;
		if (!center_of(arg, warg, &p)) return true;
		node *n = showing(warg);
		HWND w = wh_hit(GetAncestor(n->hwnd, GA_ROOT), p);
		POINT c = p;
		ScreenToClient(w, &c);
		LPARAM lp = MAKELPARAM(c.x, c.y);
		pointer = p, pointer_on = w;
		if (!strcmp(verb, "down")) {
			wh_overlay_pointer_down(w, p);
			if (!IsWindow(w)) return true;
			wh_pressing(true);
			SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, lp);
			wh_pressing(false);
			return true;
		}
		click(w, lp);
		if (!IsWindow(w)) return true;
		wh_pressing(true);
		SendMessageW(w, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
		wh_pressing(false);
		if (IsWindow(w)) SendMessageW(w, WM_LBUTTONUP, 0, lp);
		return true;
	}
	if (!strcmp(verb, "move") || !strcmp(verb, "moveby")) {
		POINT to = pointer;
		if (!strcmp(verb, "move")) {
			if (!center_of(arg, warg, &to)) return true;
		} else {
			float dx = 0, dy = 0;
			sscanf(arg, "%f,%f", &dx, &dy);
			to.x += wh_px(NULL, dx), to.y += wh_px(NULL, dy);
		}
		POINT from = pointer;
		for (int i = 1; i <= 10; i++) {
			POINT p = {from.x + (to.x - from.x) * i / 10, from.y + (to.y - from.y) * i / 10};
			pointer = p;
			mouse_to(WM_MOUSEMOVE, MK_LBUTTON, p);
		}
		return true;
	}
	if (!strcmp(verb, "up")) {
		mouse_to(WM_LBUTTONUP, 0, pointer);
		pointer_on = NULL;
		return true;
	}
	return false;
}

// ---- The mouse's own input (SendInput), at a table's rows ----

// real_click clicks at screen point p as the mouse does, through the
// system's input queue (a double click's second press Windows's own):
// button 0 the left, 1 the right; mods' Ctrl and Shift held.
static void real_click(POINT p, int button, int clicks, bool ctrl, bool shift) {
	HWND top = wh_window_in_use();
	SetForegroundWindow(top);
	SetCursorPos(p.x, p.y);
	INPUT in[12];
	int n = 0;
	if (ctrl) in[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = VK_CONTROL}};
	if (shift) in[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = VK_SHIFT}};
	for (int i = 0; i < clicks && i < 2; i++) {
		in[n++] = (INPUT){.type = INPUT_MOUSE, .mi = {.dwFlags = button ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN}};
		in[n++] = (INPUT){.type = INPUT_MOUSE, .mi = {.dwFlags = button ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP}};
	}
	if (shift) in[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = VK_SHIFT, .dwFlags = KEYEVENTF_KEYUP}};
	if (ctrl) in[n++] = (INPUT){.type = INPUT_KEYBOARD, .ki = {.wVk = VK_CONTROL, .dwFlags = KEYEVENTF_KEYUP}};
	SendInput((UINT)n, in, sizeof *in);
}

// real_step runs the steps that drive a table and an outline with the
// mouse's own input:
//
//   - row:<n>[,ctrl|,shift], rowdbl:<n>, rowmenu:<n>: a click (with Ctrl
//     or Shift held), a double click, a right click on a Table's row n
//     (scrolled to first);
//   - header:<title>: a click on its column's header;
//   - item:<text>, itemdbl:<text>, itemmenu:<text>, chevron:<text>: a
//     click, a double click, a right click on an Outline's row that says
//     text, or on its disclosure;
//   - pointer:<text>: the mouse over the middle of what shows text.
static bool real_step(const char *verb, const char *arg, const wchar_t *warg) {
	POINT p;
	if (!strcmp(verb, "row") || !strcmp(verb, "rowdbl") || !strcmp(verb, "rowmenu")) {
		if (!wh_table_row_point(atoll(arg), &p)) {
			wh_log("automation: no row \"%s\"", arg);
			return true;
		}
		real_click(p, !strcmp(verb, "rowmenu"), !strcmp(verb, "rowdbl") ? 2 : 1, strstr(arg, "ctrl") != NULL, strstr(arg, "shift") != NULL);
		return true;
	}
	if (!strcmp(verb, "header")) {
		if (!wh_table_header_point(warg, &p)) wh_log("automation: no header \"%s\"", arg);
		else real_click(p, 0, 1, false, false);
		return true;
	}
	if (!strcmp(verb, "item") || !strcmp(verb, "itemdbl") || !strcmp(verb, "itemmenu") || !strcmp(verb, "chevron")) {
		if (!wh_outline_point(warg, !strcmp(verb, "chevron"), &p)) wh_log("automation: no item \"%s\"", arg);
		else real_click(p, !strcmp(verb, "itemmenu"), !strcmp(verb, "itemdbl") ? 2 : 1, false, false);
		return true;
	}
	if (!strcmp(verb, "pointer")) {
		if (center_of(arg, warg, &p)) {
			SetForegroundWindow(wh_window_in_use());
			SetCursorPos(p.x, p.y);
		}
		return true;
	}
	return false;
}

// ---- The script, on a timer ----

static char *script, *snap_path;
static size_t at;
static char *wait_file;

static void finish(void) {
	if (!snap_path) return;
	wh_snapshot(snap_path);
	if (getenv("ASUKA_DUMP")) write_dump(snap_path);
	if (!getenv("ASUKA_SNAPSHOT_STAY")) {
		wh_web_close_all();  // WebView2's browser goes quietly
		fflush(stdout);
		fflush(stderr);
		ExitProcess(0);
	}
}

// parse_duration_ms reads a Go duration (500ms, 2s, 1.5s, 1m30s) in
// milliseconds; 0 if it isn't one.
static double parse_duration_ms(const char *s) {
	double total = 0;
	while (*s) {
		char *end;
		double v = strtod(s, &end);
		if (end == s) return 0;
		s = end;
		double unit;
		if (!strncmp(s, "ns", 2)) unit = 1e-6, s += 2;
		else if (!strncmp(s, "us", 2) || !strncmp(s, "\xc2\xb5s", 3)) unit = 1e-3, s += s[0] == 'u' ? 2 : 3;
		else if (!strncmp(s, "ms", 2)) unit = 1, s += 2;
		else if (*s == 's') unit = 1000, s++;
		else if (*s == 'm') unit = 60000, s++;
		else if (*s == 'h') unit = 3600000, s++;
		else return 0;
		total += v * unit;
	}
	return total;
}

static void next_step(void) {
	if (wait_file) {
		if (GetFileAttributesA(wait_file) == INVALID_FILE_ATTRIBUTES) {
			SetTimer(H.msg_win, TIMER_SCRIPT, 50, NULL);
			return;
		}
		free(wait_file);
		wait_file = NULL;
	}
	while (script && script[at]) {
		size_t end = at;
		while (script[end] && script[end] != ';') end++;
		char *s = wh_strndup(script + at, end - at);
		at = script[end] ? end + 1 : end;
		char *p = s;
		while (*p == ' ') p++;
		size_t l = strlen(p);
		while (l && p[l - 1] == ' ') p[--l] = 0;
		if (!*p) {
			free(s);
			continue;
		}
		char *colon = strchr(p, ':');
		const char *arg = "";
		if (colon) *colon = 0, arg = colon + 1;
		if (!strcmp(p, "wait")) {
			UINT ms = (UINT)parse_duration_ms(arg);
			free(s);
			SetTimer(H.msg_win, TIMER_SCRIPT, ms ? ms : 1, NULL);
			return;
		}
		if (!strcmp(p, "waitfor")) {
			wait_file = wh_strdup(arg);
			free(s);
			SetTimer(H.msg_win, TIMER_SCRIPT, 1, NULL);
			return;
		}
		// The next step's timer first: a step may open a modal loop (a
		// menu), in which the steps go on.
		SetTimer(H.msg_win, TIMER_SCRIPT, 400, NULL);
		wh_core_begin();
		step(p, arg);
		wh_core_end();
		free(s);
		return;
	}
	KillTimer(H.msg_win, TIMER_SCRIPT);
	finish();
}

void wh_automation_timer(void) {
	KillTimer(H.msg_win, TIMER_SCRIPT);
	next_step();
}

// ---- ASUKA_COMMANDS: the app driven from outside while it runs ----
//
// Commands written to dir/asuka-commands.txt, one a line ("snap
// file.png", "tap Add": the script's steps, a space for its colon), are
// run and the file removed; asuka-done.txt is written after them (asuka
// test --gui's screenshots wait for it).

static char *commands_dir;

void wh_commands_timer(void) {
	char *file = dsk_join(commands_dir, "asuka-commands.txt");
	size_t n;
	char *text = dsk_read_file(file, &n);
	if (!text) {
		free(file);
		return;
	}
	DeleteFileA(file);
	free(file);
	for (char *line = text; line && *line;) {
		char *end = strchr(line, '\n');
		if (end) *end = 0;
		char *p = line;
		line = end ? end + 1 : NULL;
		while (*p == ' ' || *p == '\t') p++;
		size_t l = strlen(p);
		while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
		if (!*p) continue;
		char *sp = strchr(p, ' ');
		const char *arg = "";
		if (sp) *sp = 0, arg = sp + 1;
		wh_core_begin();
		step(p, arg);
		wh_core_end();
	}
	free(text);
	char *done = dsk_join(commands_dir, "asuka-done.txt");
	FILE *f = fopen(done, "wb");
	if (f) fclose(f);
	free(done);
}

// wh_automation starts the debug automation, if it's asked for.
void wh_automation(void) {
	char *dir = dsk_getenv("ASUKA_COMMANDS");
	if (dir && *dir) {
		commands_dir = dir;
		SetTimer(H.msg_win, TIMER_COMMANDS, 50, NULL);
	} else {
		free(dir);
	}
	char *s = dsk_getenv("ASUKA_SCRIPT");
	snap_path = dsk_getenv("ASUKA_SNAPSHOT");
	if (!s && !snap_path) return;
	script = s ? s : wh_strdup("");
	at = 0;
	SetTimer(H.msg_win, TIMER_SCRIPT, 1500, NULL);
}
