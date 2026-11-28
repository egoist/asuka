// scroll.c: scrolling a Scroll or a List (the wheel, the keys, the scroll
// bar), the scroll bars as indicators over the content (a thin rounded
// thumb that grows under the mouse and drags), refreshing with F5, and
// paging Scrolls and PageControls.
#include "nav.h"

struct wh_scrollbar {
	node *n;     // the host node of the thumb's window
	node *view;  // the Scroll or List
	bool horizontal, visible, dragging;
	int drag_at;
	float drag_from;
	RECT thumb;
};

struct wh_refresher {
	bool refreshing;
	node *spinner;  // the disc and its ring
};

// ---- Where a scroll view is ----

// extent_along is a scroll view's content, viewport and position along
// an axis, in points.
static void extent_along(node *n, bool horizontal, float *content, float *view, float *pos) {
	RECT r;
	GetClientRect(n->hwnd, &r);
	if (n->list) *content = wh_list_content(n), *view = wh_pt(n, r.bottom), *pos = wh_list_offset(n);
	else if (horizontal) *content = n->content_w, *view = wh_pt(n, r.right), *pos = n->scroll_x;
	else *content = n->content_h, *view = wh_pt(n, r.bottom), *pos = n->scroll_y;
}

// extent is a scroll view's content, viewport and position along its
// axis, in points.
static void extent(node *n, float *content, float *view, float *pos, bool *horizontal) {
	*horizontal = !n->list && n->horizontal;
	extent_along(n, *horizontal, content, view, pos);
}

// overflows reports whether a zoomable Scroll's content goes past it
// across its axis too (zoomed in, it scrolls both ways): its second
// thumb's.
static bool overflows_across(node *n) {
	if (!wh_zoomable(n)) return false;
	float content, view, pos;
	extent_along(n, !n->horizontal, &content, &view, &pos);
	return content > view + 0.5f && view > 0;
}

// ---- The scroll bar ----

static LRESULT bar_message(node *t, UINT m, WPARAM wp, LPARAM lp, bool *handled);

static wh_scrollbar *new_bar(node *n) {
	node *t = wh_node_new(wh_host_id(), KIND_SCROLL_BAR);
	t->hwnd = wh_make_view(0);
	t->k = n->k;
	SetParent(t->hwnd, n->hwnd);
	ShowWindow(t->hwnd, SW_HIDE);
	t->host_msg = bar_message;
	wh_scrollbar *b = wh_alloc(sizeof *b);
	b->n = t, b->view = n;
	t->sbar = b;
	wh_node_add(t);
	return b;
}

static wh_scrollbar *bar_of(node *n) {
	if (!n->bar) n->bar = new_bar(n);
	return n->bar;
}

// across_of is a zoomable Scroll's second thumb, across its axis.
static wh_scrollbar *across_of(node *n) {
	if (!n->bar2) n->bar2 = new_bar(n);
	return n->bar2;
}

void wh_bars_reset(node *n) {
	if (n->bar) memset(&n->bar->thumb, 0, sizeof n->bar->thumb);
	if (n->bar2) memset(&n->bar2->thumb, 0, sizeof n->bar2->thumb);
}

static void place_bar(node *n, wh_scrollbar *b, bool horizontal);

// wh_update_bars places a scroll view's thumbs at its position, showing
// them if it's been scrolling or under the mouse lately: its axis's, and
// a zoomed Scroll's across it.
void wh_update_bars(node *n) {
	if (!n->hwnd) return;
	if (n->bar2 || overflows_across(n)) place_bar(n, across_of(n), !n->horizontal);
	place_bar(n, n->bar, !n->list && n->horizontal);
}

static void place_bar(node *n, wh_scrollbar *b, bool horizontal) {
	float content, view, pos;
	extent_along(n, horizontal, &content, &view, &pos);
	bool across = b && b == n->bar2;
	if (content <= view + 0.5f || view <= 0 || (across && !wh_zoomable(n))) {
		if (b) {
			b->visible = false;
			ShowWindow(b->n->hwnd, SW_HIDE);
		}
		return;
	}
	if (!b) b = bar_of(n);
	b->horizontal = horizontal;
	if (!b->visible) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	int thick = wh_px(n, b->n->hot || b->dragging ? 8 : 4), inset = wh_px(n, 2);
	int along = horizontal ? r.right : r.bottom;
	int length = wh_maxi((int)((float)along * view / content), wh_px(n, 24));
	int at = (int)((float)(along - length) * pos / (content - view));
	RECT t;
	if (horizontal) t = (RECT){at, r.bottom - thick - inset, at + length, r.bottom - inset};
	else {
		t = (RECT){r.right - thick - inset, at, r.right - inset, at + length};
		if (wh_is_rtl(n)) t.left = inset, t.right = inset + thick;  // on the left, as a mirrored window's
	}
	if (!EqualRect(&t, &b->thumb)) {
		b->thumb = t;
		SetWindowRgn(b->n->hwnd, CreateRoundRectRgn(0, 0, t.right - t.left + 1, t.bottom - t.top + 1, thick, thick), TRUE);
	}
	SetWindowPos(b->n->hwnd, HWND_TOP, t.left, t.top, t.right - t.left, t.bottom - t.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

// show_bars shows a scroll view's thumb for a moment (it scrolled, or the
// mouse moved over it).
static void show_bars(node *n) {
	if (n->paging) return;  // a pager has no scroll bar
	float content, view, pos;
	bool horizontal;
	extent(n, &content, &view, &pos, &horizontal);
	bool across = overflows_across(n);
	if (content <= view + 0.5f && !across) return;
	SetTimer(n->hwnd, TIMER_BARS, 1200, NULL);
	bool more = false;
	if (content > view + 0.5f && !bar_of(n)->visible) more = bar_of(n)->visible = true;
	if (across && !across_of(n)->visible) more = across_of(n)->visible = true;
	if (more) wh_update_bars(n);
}

void wh_show_bars(node *n) { show_bars(n); }

// wh_hide_bars hides the thumb once the mouse has left it alone.
void wh_hide_bars(node *n) {
	if (n->hwnd) KillTimer(n->hwnd, TIMER_BARS);
	wh_scrollbar *bs[2] = {n->bar, n->bar2};
	for (int i = 0; i < 2; i++)
		if (bs[i] && (bs[i]->n->hot || bs[i]->dragging)) return;  // the mouse on one: both stay
	for (int i = 0; i < 2; i++) {
		if (!bs[i]) continue;
		bs[i]->visible = false;
		ShowWindow(bs[i]->n->hwnd, SW_HIDE);
	}
}

// wh_mouse_over shows the thumbs of the scroll views the mouse moves in.
void wh_mouse_over(HWND hwnd) {
	for (HWND w = hwnd; w && w != H.window; w = GetParent(w)) {
		node *n = wh_node_of(w);
		if (n && n->hwnd == w && (n->kind == AK_KIND_SCROLL || n->list)) show_bars(n);
	}
}

static LRESULT bar_message(node *t, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	(void)wp;
	wh_scrollbar *b = t->sbar;
	node *n = b->view;
	*handled = true;
	switch (m) {
	case WM_NCHITTEST:
		return HTCLIENT;
	case WM_MOUSEMOVE:
		if (!t->hot) {
			t->hot = true;
			TRACKMOUSEEVENT tm = {sizeof tm, TME_LEAVE, t->hwnd, 0};
			TrackMouseEvent(&tm);
			memset(&b->thumb, 0, sizeof b->thumb);
			wh_update_bars(n);
		}
		if (b->dragging) {
			POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			ClientToScreen(t->hwnd, &p);
			int at = b->horizontal ? p.x : p.y;
			float content, view, pos;
			extent_along(n, b->horizontal, &content, &view, &pos);
			RECT r;
			GetClientRect(n->hwnd, &r);
			int along = b->horizontal ? r.right : r.bottom;
			float d = wh_pt(n, at - b->drag_at) * content / wh_maxf(wh_pt(n, along), 1);
			if (b->horizontal) wh_scroll_to_offset(n, b->drag_from + d, n->scroll_y);
			else wh_scroll_to_offset(n, n->scroll_x, b->drag_from + d);
		}
		return 0;
	case WM_MOUSELEAVE:
		t->hot = false;
		memset(&b->thumb, 0, sizeof b->thumb);
		wh_update_bars(n);
		SetTimer(n->hwnd, TIMER_BARS, 1200, NULL);
		return 0;
	case WM_LBUTTONDOWN: {
		SetCapture(t->hwnd);
		b->dragging = true;
		POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		ClientToScreen(t->hwnd, &p);
		float content, view, pos;
		extent_along(n, b->horizontal, &content, &view, &pos);
		b->drag_at = b->horizontal ? p.x : p.y, b->drag_from = pos;
		return 0;
	}
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		if (b->dragging) {
			b->dragging = false;
			if (GetCapture() == t->hwnd) ReleaseCapture();
			SetTimer(n->hwnd, TIMER_BARS, 1200, NULL);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(t->hwnd, &ps);
		rgba c = H.dark ? rgba_hex(0x9A9A9A) : rgba_hex(0x8A8A8A);
		if (t->hot || b->dragging) c = H.dark ? rgba_hex(0xCECECE) : rgba_hex(0x6E6E6E);
		RECT r;
		GetClientRect(t->hwnd, &r);
		HBRUSH br = CreateSolidBrush(rgba_ref(c));
		FillRect(dc, &r, br);
		DeleteObject(br);
		EndPaint(t->hwnd, &ps);
		return 0;
	}
	}
	*handled = false;
	return 0;
}

// ---- Scrolling ----

// wh_scroll_to_offset scrolls a Scroll to a point of its content
// (clamped).
void wh_scroll_to_offset(node *n, float x, float y) {
	if (n->list) {
		wh_list_scroll(n, y);
		return;
	}
	if (n->kind != AK_KIND_SCROLL) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	float vw = wh_pt(n, r.right), vh = wh_pt(n, r.bottom);
	x = wh_clampf(x, 0, wh_maxf(n->content_w - vw, 0));
	y = wh_clampf(y, 0, wh_maxf(n->content_h - vh, 0));
	if (!n->horizontal && !wh_zoomable(n)) x = 0;  // zoomable, it scrolls both ways
	if (x == n->scroll_x && y == n->scroll_y) return;
	n->scroll_x = x, n->scroll_y = y;
	n->scroll_end = wh_maxf(n->content_w - vw, 0) - x;
	for (uint32_t i = 0; i < n->nkids; i++) wh_place_frame(n->kids[i]);
	show_bars(n);
	wh_update_bars(n);
	ak_event e = {.kind = AK_EVENT_SCROLLED, .x = x, .y = y};
	wh_emit(n, &e);
}

void wh_scroll_by(node *n, float dx, float dy) {
	if (n->list) wh_list_scroll(n, wh_list_offset(n) + dy);
	else wh_scroll_to_offset(n, n->scroll_x + dx, n->scroll_y + dy);
}

// wh_wheel scrolls by a wheel's delta: the system's lines per notch, a
// line 16 points. It reports false at the end, so an outer scroll view
// takes it.
bool wh_wheel(node *n, bool horizontal, int delta) {
	UINT lines = 3;
	SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
	if (lines == 0 || lines > 100) lines = 3;
	float d = -(float)delta / WHEEL_DELTA * (float)lines * 16;
	if (horizontal) d = -d;
	// A vertical wheel scrolls a horizontal view only with Shift (else the
	// view it's in), and a horizontal one only a horizontal view.
	if (wh_key_down(VK_SHIFT) && !horizontal) {
		horizontal = true;
		if (wh_is_rtl(n)) d = -d;  // down the wheel is on along the content, to the left
	}
	if (horizontal != n->horizontal && !wh_zoomable(n)) return false;  // a zoomable Scroll's content scrolls both ways
	float bx = n->list ? 0 : n->scroll_x, by = n->list ? wh_list_offset(n) : n->scroll_y;
	if (horizontal) wh_scroll_by(n, d, 0);
	else wh_scroll_by(n, 0, d);
	float ax = n->list ? 0 : n->scroll_x, ay = n->list ? wh_list_offset(n) : n->scroll_y;
	return ax != bx || ay != by;
}

// wh_scroll_bar_message follows a Win32 scroll bar's arrows, track and
// thumb (a view's own, should it have one).
void wh_scroll_bar_message(node *n, bool horizontal, int code) {
	SCROLLINFO si = {sizeof si, SIF_ALL};
	GetScrollInfo(n->hwnd, horizontal ? SB_HORZ : SB_VERT, &si);
	float line = 40, page = wh_pt(n, (int)si.nPage), pos = wh_pt(n, si.nPos);
	switch (code) {
	case SB_LINEUP: pos -= line; break;
	case SB_LINEDOWN: pos += line; break;
	case SB_PAGEUP: pos -= page; break;
	case SB_PAGEDOWN: pos += page; break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: pos = wh_pt(n, si.nTrackPos); break;
	case SB_TOP: pos = 0; break;
	case SB_BOTTOM: pos = 1e9f; break;
	default: return;
	}
	if (n->list) wh_list_scroll(n, pos);
	else if (horizontal) wh_scroll_to_offset(n, pos, n->scroll_y);
	else wh_scroll_to_offset(n, n->scroll_x, pos);
}

// wh_update_scroll takes a Scroll's new size or content size: the position
// clamped, the scroll bar shown when the content overflows. A horizontal
// one going right to left starts at its trailing end, and keeps its
// distance from that end as they change.
void wh_update_scroll(node *n) {
	if (!n->hwnd) return;
	float z = wh_scroll_zoom(n);
	if (n->nkids) n->content_w = n->kids[0]->frame[2] * z, n->content_h = n->kids[0]->frame[3] * z;
	wh_update_bars(n);
	RECT r;
	GetClientRect(n->hwnd, &r);
	float vw = wh_pt(n, r.right), vh = wh_pt(n, r.bottom);
	float x = wh_clampf(n->scroll_x, 0, wh_maxf(n->content_w - vw, 0)), y = wh_clampf(n->scroll_y, 0, wh_maxf(n->content_h - vh, 0));
	bool follows_end = n->horizontal && wh_is_rtl(n);
	if (follows_end) x = wh_maxf(n->content_w - vw - n->scroll_end, 0);
	if (x != n->scroll_x || y != n->scroll_y) {
		n->scroll_x = x, n->scroll_y = y;
		for (uint32_t i = 0; i < n->nkids; i++) wh_place_frame(n->kids[i]);
		wh_update_bars(n);
		if (follows_end) {
			ak_event e = {.kind = AK_EVENT_SCROLLED, .x = x, .y = y};
			wh_emit(n, &e);
		}
	}
}

// wh_restart_scroll puts a horizontal Scroll at its start in a new
// direction: its right end, right to left; its left, left to right.
void wh_restart_scroll(node *n) {
	if (!n->horizontal || !n->hwnd) return;
	n->scroll_end = 0;
	if (wh_is_rtl(n)) wh_update_scroll(n);
	else wh_scroll_to_offset(n, 0, n->scroll_y);
}

// ---- Paging Scrolls and PageControls, as WinUI's FlipView and
// PipsPager ----

#define PIP_CELL 12.0f
#define PIP_ON 6.0f
#define PIP_OFF 4.0f
#define PIPS_HIGH 24.0f

// wh_page_wheel turns a paging Scroll's wheel into pages: a page per notch
// (a precision touchpad's small steps add up to one).
bool wh_page_wheel(node *n, bool horizontal, int delta) {
	bool tilt = horizontal;  // a tilted wheel, or a touchpad's sideways swipe
	if (wh_key_down(VK_SHIFT) && !horizontal) horizontal = true;
	if (horizontal != n->horizontal) return false;
	n->wheel_sum += delta;
	if (n->wheel_sum > -WHEEL_DELTA && n->wheel_sum < WHEEL_DELTA) return true;
	// Pages are physical, from the left or top. Down the wheel is the next
	// page along the content (to the left, right to left); a tilt to the
	// right is the page to the right.
	int step = n->wheel_sum > 0 ? -1 : 1;
	if (tilt) step = -step;
	else if (horizontal && wh_is_rtl(n)) step = -step;
	n->wheel_sum = 0;
	float content, view, pos;
	bool h;
	extent(n, &content, &view, &pos, &h);
	if (view <= 0) return true;
	int page = (int)lroundf(pos / view) + step;
	page = wh_mini(wh_maxi(page, 0), wh_maxi((int)ceilf(content / view) - 1, 0));
	float to = (float)page * view;
	if (horizontal) wh_scroll_to_offset(n, to, 0);
	else wh_scroll_to_offset(n, 0, to);
	wh_emit_int(n, AK_EVENT_PAGED, page);
	return true;
}

void wh_measure_pips(node *n, float *w, float *h) {
	*w = (float)n->pages * PIP_CELL + PIP_CELL;
	*h = PIPS_HIGH;
}

int wh_pip_at(node *n, int x) {
	if (!n->pages) return -1;
	RECT r;
	GetClientRect(n->hwnd, &r);
	float w = wh_pt(n, r.right);
	int i = (int)floorf((wh_pt(n, x) - (w - (float)n->pages * PIP_CELL) / 2) / PIP_CELL);
	if (wh_is_rtl(n)) i = n->pages - 1 - i;
	return wh_mini(wh_maxi(i, 0), n->pages - 1);
}

void wh_select_pip(node *n, int i) {
	if (i < 0 || i >= n->pages || i == n->selected) return;
	n->selected = i;
	wh_redraw(n);
	wh_emit_int(n, AK_EVENT_SELECTED, i);
}

void wh_paint_pips(node *n, gfx g, float w, float h) {
	float k = wh_scale_of(n);
	float x0 = (w - (float)n->pages * PIP_CELL * k) / 2;
	bool rtl = wh_is_rtl(n);
	for (int i = 0; i < n->pages; i++) {
		int slot = rtl ? n->pages - 1 - i : i;
		float d = i == n->selected ? PIP_ON * k : PIP_OFF * k;
		rgba c = i == n->selected ? H.theme.secondary : H.theme.tertiary;
		float cx = x0 + ((float)slot + 0.5f) * PIP_CELL * k;
		g_fill_ellipse(g, rgba_argb(c), cx - d / 2, h / 2 - d / 2, d, d);
	}
}

// ---- Refreshing: F5, the Windows way: the refreshable Scroll or List
// the focus is in, or else the innermost one on screen in the window; a
// spinner on a raised disc at the top of its content while it refreshes,
// as WinUI's RefreshContainer shows its indicator ----

static void drop_spinner(node *n) {
	if (!n->refresh || !n->refresh->spinner) return;
	node *s = n->refresh->spinner;
	n->refresh->spinner = NULL;
	if (s->hwnd) {
		HWND hw = s->hwnd;
		wh_forget_tree(hw);
		DestroyWindow(hw);
	}
	wh_free_node(s);
}

void wh_set_refreshable(node *n, bool on) {
	if (on && !n->refresh) n->refresh = wh_alloc(sizeof *n->refresh);
	else if (!on && n->refresh) {
		wh_set_refreshing(n, false);
		drop_spinner(n);
		free(n->refresh);
		n->refresh = NULL;
	}
}

void wh_set_refreshing(node *n, bool on) {
	wh_refresher *r = n->refresh;
	if (!r || r->refreshing == on || !n->hwnd) return;
	r->refreshing = on;
	if (!on) {
		node *s = r->spinner;
		if (s && s->hwnd) {
			s->animating = false;
			wh_spin(s);
			ShowWindow(s->hwnd, SW_HIDE);
		}
		return;
	}
	node *s = r->spinner;
	if (!s) {
		s = wh_node_new(wh_host_id(), AK_KIND_ACTIVITY);
		s->hwnd = wh_make_view(0);
		s->k = n->k, s->dir = n->dir;
		s->background = ak_semantic_color(AK_SEMANTIC_COLOR_SECONDARY_BACKGROUND);
		s->border_width = 1;
		s->border_color = ak_semantic_color(AK_SEMANTIC_COLOR_SEPARATOR);
		wh_bind(s, s->hwnd);
		SetParent(s->hwnd, n->hwnd);
		r->spinner = s;
	}
	const float size = 36, top = 12;  // points
	float k = wh_scale_of(n);
	s->k = n->k;
	s->corner_radius = size / 2;
	s->inset[0] = s->inset[1] = s->inset[2] = s->inset[3] = 8;
	RECT cr;
	GetClientRect(n->hwnd, &cr);
	int px = (int)(size * k + 0.5f);
	SetWindowPos(s->hwnd, HWND_TOP, (cr.right - px) / 2, (int)(top * k + 0.5f), px, px, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	s->animating = true;
	wh_spin(s);
	InvalidateRect(s->hwnd, NULL, FALSE);
}

static bool refresh(node *n) {
	if (!n->refresh || n->refresh->refreshing || !node_listening(n, AK_EVENT_REFRESH)) return false;
	wh_set_refreshing(n, true);  // the core's handler says when it's done
	wh_emit_kind(n, AK_EVENT_REFRESH);
	return true;
}

static void walk_refresh(node *n, node *root, node **found) {
	if (n->hidden || (n->hwnd && !IsWindowVisible(n->hwnd))) return;
	if (n->refresh) *found = n;
	switch (n->kind) {
	case AK_KIND_STACK: {
		node *t = wh_top_screen(n);
		if (t) walk_refresh(t, root, found);
		return;
	}
	case AK_KIND_TABS:
		if (n->selected >= 0 && (uint32_t)n->selected < n->nkids) walk_refresh(n->kids[n->selected], root, found);
		return;
	case AK_KIND_SHEET:
	case AK_KIND_ALERT:
	case AK_KIND_WINDOW:
	case AK_KIND_POPOVER:
		if (n != root) return;
	}
	for (uint32_t i = 0; i < n->nkids; i++) walk_refresh(n->kids[i], root, found);
}

bool wh_refresh_key(MSG *m) {
	for (node *c = wh_node_of(GetFocus()); c; c = c->parent)
		if (c->refresh) return refresh(c);
	node *root = wh_root_of(GetAncestor(m->hwnd, GA_ROOT));
	if (!root) return false;
	node *found = NULL;
	walk_refresh(root, root, &found);
	return found && refresh(found);
}

// ---- Zooming a Scroll's content (MinZoom, MaxZoom): Ctrl and the wheel
// (a precision touchpad's pinch), a touch screen's pinch (gesture.c), a
// double click, ZoomTo. Its content is drawn at the zoom as a Transform's
// scale draws a view's (its tree's k, so text, images and controls are
// drawn at it, not magnified), its frame the zoomed one's, scrolled both
// ways. A web view or a video in it is placed and sized at the zoom, but
// its page isn't zoomed (WebView2 draws it). ----

#define ZOOM_TIME 0.25  // seconds a double click's zoom, or an animated ZoomTo, takes

bool wh_zoomable(const node *n) { return n->kind == AK_KIND_SCROLL && n->max_zoom > n->min_zoom; }

float wh_scroll_zoom(const node *n) { return n->kind == AK_KIND_SCROLL && n->zoom_scale > 0 ? n->zoom_scale : 1; }

static float clamp_zoom(const node *n, float z) { return wh_clampf(z, n->min_zoom, wh_maxf(n->max_zoom, n->min_zoom)); }

// wh_zoom_child gives a Scroll's content c the Scroll's zoom (with its
// transform's scale), or, once it's taken out (its parent no longer the
// Scroll), its own again; rescale: its tree drawn at it now (else when
// it's adopted).
void wh_zoom_child(node *scroll, node *c, bool rescale) {
	float z = wh_xform_zoom(c) * (c->parent == scroll ? wh_scroll_zoom(scroll) : 1);
	if (fabsf(z - 1) < 1e-6f) z = 0;
	if ((z == 0) != (c->zoom == 0)) H.zoomed += z == 0 ? -1 : 1;
	if (z == c->zoom) return;
	c->zoom = z;
	if (rescale && c->hwnd && c->parent == scroll) wh_rescale(c->hwnd, scroll->k);
}

static void viewport_pt(node *n, float *vw, float *vh) {
	RECT r;
	GetClientRect(n->hwnd, &r);
	*vw = wh_pt(n, r.right), *vh = wh_pt(n, r.bottom);
}

// wh_zoom_around zooms n's content to z (within MinZoom and MaxZoom),
// keeping the content's point at ax, ay (points in the view) where it is;
// user: the user's zoom, reported (Zoomed).
void wh_zoom_around(node *n, float z, float ax, float ay, bool user) {
	if (!n->hwnd || n->kind != AK_KIND_SCROLL) return;
	z = clamp_zoom(n, z);
	float old = wh_scroll_zoom(n);
	if (z == old) {
		if (user) wh_emit_float(n, AK_EVENT_ZOOMED, z);
		return;
	}
	float cx = (n->scroll_x + ax) / old, cy = (n->scroll_y + ay) / old;
	n->zoom_scale = z;
	float vw, vh;
	viewport_pt(n, &vw, &vh);
	if (n->nkids) n->content_w = n->kids[0]->frame[2] * z, n->content_h = n->kids[0]->frame[3] * z;
	n->scroll_x = wh_clampf(cx * z - ax, 0, wh_maxf(n->content_w - vw, 0));
	n->scroll_y = wh_clampf(cy * z - ay, 0, wh_maxf(n->content_h - vh, 0));
	n->scroll_end = wh_maxf(n->content_w - vw, 0) - n->scroll_x;
	for (uint32_t i = 0; i < n->nkids; i++) {
		node *c = n->kids[i];
		float was = wh_own_zoom(c);
		wh_zoom_child(n, c, true);  // placed as it's rescaled
		if (wh_own_zoom(c) == was) wh_place_frame(c);
	}
	show_bars(n);
	wh_bars_reset(n);
	wh_update_bars(n);
	ak_event e = {.kind = AK_EVENT_SCROLLED, .x = n->scroll_x, .y = n->scroll_y};
	wh_emit(n, &e);
	if (user) wh_emit_float(n, AK_EVENT_ZOOMED, z);
}

// wh_set_zoom_bounds takes MinZoom or MaxZoom: the zoom shown kept within
// them (silently), and a touch screen's pinch asked for.
void wh_set_zoom_bounds(node *n, uint16_t p, float v) {
	if (p == AK_PROP_SCROLL_MIN_ZOOM) n->min_zoom = v > 0 ? v : 1;
	else n->max_zoom = v > 0 ? v : 1;
	if (!n->hwnd) return;
	wh_allow_gestures(n);
	float z = wh_scroll_zoom(n);
	if (clamp_zoom(n, z) != z) {
		float vw, vh;
		viewport_pt(n, &vw, &vh);
		KillTimer(n->hwnd, TIMER_ZOOM);
		wh_zoom_around(n, z, vw / 2, vh / 2, false);
	}
}

// wh_zoom_wheel zooms with Ctrl and the wheel (a precision touchpad's
// pinch arrives as it), 10% a notch, around the pointer.
bool wh_zoom_wheel(node *n, int delta, LPARAM lp) {
	if (!wh_zoomable(n) || !n->hwnd) return false;
	KillTimer(n->hwnd, TIMER_ZOOM);
	POINT at = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
	ScreenToClient(n->hwnd, &at);
	float z = wh_scroll_zoom(n) * (float)pow(1.1, (double)delta / WHEEL_DELTA);
	wh_zoom_around(n, z, wh_pt(n, at.x), wh_pt(n, at.y), true);
	return true;
}

static void zoom_animated(node *n, float z, float ax, float ay, bool user, bool animated) {
	z = clamp_zoom(n, z);
	if (!animated || H.env.env.reduce_motion || !IsWindowVisible(n->hwnd)) {
		KillTimer(n->hwnd, TIMER_ZOOM);
		wh_zoom_around(n, z, ax, ay, user);
		return;
	}
	n->zoom_from = wh_scroll_zoom(n), n->zoom_to = z, n->zoom_ax = ax, n->zoom_ay = ay;
	n->zoom_t0 = wh_seconds(), n->zoom_user = user;
	SetTimer(n->hwnd, TIMER_ZOOM, 16, NULL);
}

// wh_zoom_step moves a zoom under way on: eased out, over ZOOM_TIME.
void wh_zoom_step(node *n) {
	double t = (wh_seconds() - n->zoom_t0) / ZOOM_TIME;
	if (t >= 1 || !n->hwnd) {
		if (n->hwnd) KillTimer(n->hwnd, TIMER_ZOOM);
		wh_zoom_around(n, n->zoom_to, n->zoom_ax, n->zoom_ay, n->zoom_user);
		return;
	}
	double e = 1 - pow(1 - t, 3);
	wh_zoom_around(n, n->zoom_from + (n->zoom_to - n->zoom_from) * (float)e, n->zoom_ax, n->zoom_ay, n->zoom_user);
}

// wh_zoom_double_click zooms a zoomable Scroll in to twice MinZoom
// (MaxZoom at most) around a double click (lp in hwnd), or back out to
// MinZoom when it's zoomed in; false: n doesn't zoom.
bool wh_zoom_double_click(node *n, HWND hwnd, LPARAM lp) {
	if (!wh_zoomable(n) || !n->hwnd) return false;
	POINT at = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
	MapWindowPoints(hwnd, n->hwnd, &at, 1);
	float z = wh_scroll_zoom(n), to = z > n->min_zoom + 0.01f ? n->min_zoom : clamp_zoom(n, n->min_zoom * 2);
	zoom_animated(n, to, wh_pt(n, at.x), wh_pt(n, at.y), true, true);
	return true;
}

// wh_zoom_to is ZoomTo: around the middle of what it shows, not reported.
void wh_zoom_to(node *n, float z, bool animated) {
	if (!n->hwnd || n->kind != AK_KIND_SCROLL) return;
	float vw, vh;
	viewport_pt(n, &vw, &vh);
	zoom_animated(n, z, vw / 2, vh / 2, false, animated);
}

// wh_zoom_hand reports whether a drag of n moves its zoomed content (a
// photo viewer's hand): a zoomable Scroll zoomed in.
bool wh_zoom_hand(const node *n) { return wh_zoomable(n) && wh_scroll_zoom(n) > n->min_zoom + 0.001f; }
