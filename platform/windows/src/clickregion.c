// clickregion.c: a click-through window's views that still take the
// mouse (TakesClicks: takesClicks). A layered WS_EX_TRANSPARENT window
// lets the mouse through all of it, so while a click-through window has
// such views, a timer follows the cursor (the window gets no messages of
// its own while it's transparent) and the window takes the mouse while
// it's over one of them, the rest of it letting the mouse through. A
// button held down keeps it as it is, so a drag that started on one isn't
// cut off.
#include "win.h"

static wh_map takers;  // node IDs, the views that take clicks
static bool following;

// visible_rect is n's view's part that shows on the screen: its window's
// rect, clipped by its ancestors' (a scroll view's).
static bool visible_rect(node *n, RECT *out) {
	if (!n->hwnd || !IsWindowVisible(n->hwnd)) return false;
	GetWindowRect(n->hwnd, out);
	for (HWND p = GetParent(n->hwnd); p; p = GetParent(p)) {
		RECT pr;
		GetWindowRect(p, &pr);
		if (!IntersectRect(out, out, &pr)) return false;
		if (!(GetWindowLongPtrW(p, GWL_STYLE) & WS_CHILD)) break;
	}
	return true;
}

static bool over_taker(wh_wctl *c, POINT pt) {
	uint32_t i = 0;
	uint64_t k;
	void *v;
	while (wh_map_next(&takers, &i, &k, &v)) {
		node *n = wh_node((uint32_t)k);
		RECT r;
		if (n && !n->dead && n->hwnd && GetAncestor(n->hwnd, GA_ROOT) == c->hwnd && visible_rect(n, &r) && PtInRect(&r, pt)) return true;
	}
	return false;
}

static bool has_takers(wh_wctl *c) {
	uint32_t i = 0;
	uint64_t k;
	void *v;
	while (wh_map_next(&takers, &i, &k, &v)) {
		node *n = wh_node((uint32_t)k);
		if (n && !n->dead && n->hwnd && GetAncestor(n->hwnd, GA_ROOT) == c->hwnd) return true;
	}
	return false;
}

// wh_click_region_tick takes the mouse where the cursor is over a view
// that takes clicks, in each click-through window: TIMER_CLICKS's.
void wh_click_region_tick(void) {
	// What's gone (destroyed) no longer takes clicks.
	uint32_t i = 0;
	uint64_t k;
	void *v;
	uint32_t gone[16], ngone = 0;
	while (wh_map_next(&takers, &i, &k, &v)) {
		node *n = wh_node((uint32_t)k);
		if ((!n || n->dead) && ngone < 16) gone[ngone++] = (uint32_t)k;
	}
	for (uint32_t g = 0; g < ngone; g++) wh_map_del(&takers, gone[g]);
	bool any = false;
	bool held = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
	POINT pt;
	GetCursorPos(&pt);
	i = 0;
	while (wh_map_next(&H.ctls, &i, NULL, &v)) {
		wh_wctl *c = v;
		if (!c->through) continue;
		bool takers_here = has_takers(c);
		any = any || takers_here;
		if (!held) wh_apply_click_through(c, !(takers_here && over_taker(c, pt)));
	}
	if (!any && following) {
		KillTimer(H.msg_win, TIMER_CLICKS);
		following = false;
	}
}

// wh_click_region_update follows the cursor while a click-through window
// has views that take clicks, and stops when none has.
void wh_click_region_update(void) {
	if (!following && takers.len) {
		SetTimer(H.msg_win, TIMER_CLICKS, 16, NULL);
		following = true;
	}
	wh_click_region_tick();
}

// wh_click_region_dump writes a click-through window's views that take
// clicks, where they show in its client area (points): the automation's.
void wh_click_region_dump(ak_buf *b, HWND top) {
	wh_wctl *c = wh_ctl_of(top);
	if (!c || !c->through) return;
	float k = wh_ctl_k(c);
	uint32_t i = 0;
	uint64_t key;
	void *v;
	while (wh_map_next(&takers, &i, &key, &v)) {
		node *n = wh_node((uint32_t)key);
		RECT r;
		if (!n || n->dead || !n->hwnd || GetAncestor(n->hwnd, GA_ROOT) != top || !visible_rect(n, &r)) continue;
		POINT a = {r.left, r.top}, z = {r.right, r.bottom};
		ScreenToClient(top, &a);
		ScreenToClient(top, &z);
		ak_buf_printf(b, "  click region %g %g %g %g\n", (double)lroundf((float)a.x / k), (double)lroundf((float)a.y / k),
		              (double)lroundf((float)(z.x - a.x) / k), (double)lroundf((float)(z.y - a.y) / k));
	}
}

void wh_set_takes_clicks(node *n, bool on) {
	if (on) wh_map_put(&takers, n->id, n);
	else wh_map_del(&takers, n->id);
	wh_click_region_update();
}
