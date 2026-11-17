// overlay.c: an Overlay (RFC 0010) is a layer over a layout root (the
// main window's root, a Window's, a sheet's or a popover's view) that the
// core lays out at the root's size and places its children in, next to an
// anchor view: std/base's popups (a select's list, a menu, a dialog, a
// tooltip).
//
// Its window is a sibling of its root's, over it, at the root's rectangle
// in their parent (its top-level window), so its children's frames are in
// the root's coordinates and the root's own children never go over it.
// The overlays of a root sit right above it, the one presented last on
// top, and under what the host keeps over the root (a hidden title bar's
// caption buttons). It's sized by the host (its Environment is the
// root's, reported as the root resizes).
//
// It draws nothing of its own. Not Modal, its window's region is its
// children's rectangles (and their shadows): what's outside them is the
// root's, drawn by it and hit by the mouse there; its own view lets the
// mouse through (HTTRANSPARENT), so only its children take presses.
// Modal, its region is the whole root: presses that land on no child of
// it are its own, and what's behind gets nothing.
//
// A press (a mouse button, a pen, a touch) that begins in its root, on no
// view of it, is reported to it as OutsidePress, by the message loop
// before the press reaches its target (wh_overlay_pointer_down). Its
// anchor's frame in the root (AnchorFrame) is reported after each batch
// that lays the views out, a scroll, a resize: once per message at most,
// and never unchanged.
#include "nav.h"
#include "win.h"

typedef struct {
	uint32_t root, anchor;
	bool modal, presented, env_known, anchor_known;
	uint32_t seq;  // the order it was presented in: the last on top
	wh_env env;
	float anchor_sent[5];
} wh_overlay;

static node **overlays;
static uint32_t noverlays, capoverlays;
static uint32_t present_seq;
static bool check_queued;

static wh_overlay *ov(const node *n) { return n && n->kind == AK_KIND_OVERLAY ? n->host_data : NULL; }

static node *root_node(const node *n) {
	wh_overlay *o = ov(n);
	if (!o) return NULL;
	node *r = o->root ? wh_node(o->root) : H.root;
	return r && r->hwnd ? r : NULL;
}

// root_parent is the window the root's view is in, where the overlay's
// goes: NULL while the root isn't shown (a sheet waiting for its window).
static HWND root_parent(const node *r) {
	if (!r || !r->hwnd) return NULL;
	HWND p = GetParent(r->hwnd);
	return p && p != H.park ? p : NULL;
}

static bool attached(const node *n) {
	node *r = root_node(n);
	HWND p = root_parent(r);
	return p && n->hwnd && GetParent(n->hwnd) == p;
}

void wh_make_overlay(node *n) {
	n->hwnd = wh_make_view(0);
	n->host_data = wh_alloc(sizeof(wh_overlay));
	SetWindowRgn(n->hwnd, CreateRectRgn(0, 0, 0, 0), FALSE);  // nothing in it yet
	if (noverlays == capoverlays) {
		capoverlays = capoverlays ? capoverlays * 2 : 8;
		overlays = wh_realloc(overlays, capoverlays * sizeof *overlays);
	}
	overlays[noverlays++] = n;
}

// wh_overlay_gone forgets an overlay being destroyed.
void wh_overlay_gone(node *n) {
	for (uint32_t i = 0; i < noverlays; i++)
		if (overlays[i] == n) {
			memmove(overlays + i, overlays + i + 1, (noverlays - i - 1) * sizeof *overlays);
			noverlays--;
			break;
		}
	free(n->host_data);
	n->host_data = NULL;
}

bool wh_overlay_modal(const node *n) {
	wh_overlay *o = ov(n);
	return o && o->modal;
}

node *wh_overlay_root(const node *n) { return root_node(n); }

uint32_t wh_overlay_seq(const node *n) {
	wh_overlay *o = ov(n);
	return o ? o->seq : 0;
}

// ---- Placing it ----

// report_env reports the overlay's environment: its root's, at the root's
// size, when it changed.
static void report_env(node *n) {
	wh_overlay *o = ov(n);
	node *r = root_node(n);
	if (!o || !r || !o->presented) return;
	RECT cr;
	GetClientRect(r->hwnd, &cr);
	float w = wh_pt(r, cr.right), h = wh_pt(r, cr.bottom);
	if (w <= 0) return;
	wh_env env = r != H.root && r->nav && r->nav->reported ? r->nav->env : H.env;
	env.env.width = w, env.env.height = h;
	env.env.scale = wh_scale_of(r);
	env.env.width_class = ak_width_class_of(w), env.env.height_class = ak_height_class_of(h);
	if (o->env_known && !memcmp(&env.env, &o->env.env, offsetof(ak_environment, locale)) && !strcmp(env.locale, o->env.locale) &&
		!strcmp(env.time_zone, o->env.time_zone) && !strcmp(env.languages, o->env.languages) &&
		env.env.first_weekday == o->env.env.first_weekday && env.env.hour_cycle == o->env.env.hour_cycle)
		return;
	o->env = env, o->env_known = true;
	wh_report_environment(n, &o->env);
}

// fit puts the overlay's window in its root's parent over the root's
// rectangle, at the root's scale.
static void fit(node *n) {
	node *r = root_node(n);
	HWND p = root_parent(r);
	if (!p || !n->hwnd) return;
	if (GetParent(n->hwnd) != p) {
		SetParent(n->hwnd, p);
		SetWindowPos(n->hwnd, r->hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // under the root till it's presented
	}
	if (n->k != r->k) wh_rescale(n->hwnd, r->k);
	RECT rr = wh_rect_in(r->hwnd, p), at = wh_rect_in(n->hwnd, p);
	if (!EqualRect(&rr, &at))
		SetWindowPos(n->hwnd, NULL, rr.left, rr.top, rr.right - rr.left, rr.bottom - rr.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	report_env(n);
}

static bool is_overlay_of(HWND w, const node *r) {
	node *x = wh_node_of(w);
	return x && x->hwnd == w && x->kind == AK_KIND_OVERLAY && root_node(x) == r;
}

// raise puts the overlay on top of its root's overlays, right above the
// root and them, under the windows the host keeps over the root.
static void raise(node *n) {
	node *r = root_node(n);
	if (!attached(n)) return;
	HWND w = GetWindow(r->hwnd, GW_HWNDPREV);
	while (w && (w == n->hwnd || is_overlay_of(w, r))) w = GetWindow(w, GW_HWNDPREV);
	SetWindowPos(n->hwnd, w ? w : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// shape sets the overlay's window's region: the whole root while it's
// modal, else its children's rectangles and their shadows.
static void shape(node *n) {
	wh_overlay *o = ov(n);
	if (!o || !n->hwnd) return;
	if (o->modal) {
		HRGN cur = CreateRectRgn(0, 0, 0, 0);
		int had = GetWindowRgn(n->hwnd, cur);
		DeleteObject(cur);
		if (had != ERROR) SetWindowRgn(n->hwnd, NULL, TRUE);
		return;
	}
	HRGN rg = CreateRectRgn(0, 0, 0, 0);
	for (HWND c = GetWindow(n->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		if (!IsWindowVisible(c)) continue;
		RECT cr = wh_rect_in(c, n->hwnd);
		node *cn = wh_node_of(c);
		if (cn && cn->hwnd == c && cn->shadow_len >= 20 && wh_u64(cn->shadow) != 0) {
			float k = wh_scale_of(cn);
			float blur = wh_f32(cn->shadow + 8), dx = wh_f32(cn->shadow + 12), dy = wh_f32(cn->shadow + 16);
			int pad = (int)ceilf((blur * 1.5f + wh_maxf(fabsf(dx), fabsf(dy))) * k) + 1;
			InflateRect(&cr, pad, pad);
		}
		HRGN one = CreateRectRgnIndirect(&cr);
		CombineRgn(rg, rg, one, RGN_OR);
		DeleteObject(one);
	}
	HRGN cur = CreateRectRgn(0, 0, 0, 0);
	bool same = GetWindowRgn(n->hwnd, cur) != ERROR && EqualRgn(cur, rg);
	DeleteObject(cur);
	if (same) {
		DeleteObject(rg);
		return;
	}
	SetWindowRgn(n->hwnd, rg, TRUE);  // the system owns rg now
}

// ---- Its anchor ----

// anchor_frame is where the overlay's anchor is in its root's
// coordinates (points), and whether any of it shows: in the root's
// window, not hidden, not clipped out by a view around it.
static bool anchor_frame(node *n, float out[5]) {
	wh_overlay *o = ov(n);
	node *a = o && o->anchor ? wh_node(o->anchor) : NULL;
	node *r = root_node(n);
	if (!a || !a->hwnd || !r) return false;
	RECT ar;
	GetWindowRect(a->hwnd, &ar);
	POINT org = {0, 0};
	ClientToScreen(r->hwnd, &org);
	out[0] = wh_pt(r, ar.left - org.x), out[1] = wh_pt(r, ar.top - org.y);
	out[2] = wh_pt(r, ar.right - ar.left), out[3] = wh_pt(r, ar.bottom - ar.top);
	HWND top = GetAncestor(a->hwnd, GA_ROOT);
	bool vis = IsWindowVisible(a->hwnd) && top == GetAncestor(r->hwnd, GA_ROOT);
	RECT v = ar;
	for (HWND p = GetParent(a->hwnd); vis && p; p = GetParent(p)) {
		RECT c;
		GetClientRect(p, &c);
		MapWindowPoints(p, HWND_DESKTOP, (POINT *)&c, 2);
		if (!IntersectRect(&v, &v, &c)) vis = false;
		if (p == top) break;
	}
	out[4] = vis ? 1 : 0;
	return true;
}

static void report_anchor(node *n) {
	wh_overlay *o = ov(n);
	float fr[5];
	if (!o || !o->presented || !node_listening(n, AK_EVENT_ANCHOR_FRAME) || !anchor_frame(n, fr)) return;
	if (o->anchor_known && !memcmp(fr, o->anchor_sent, sizeof fr)) return;
	o->anchor_known = true;
	memcpy(o->anchor_sent, fr, sizeof fr);
	uint8_t b[17];
	memcpy(b, fr, 16);
	b[16] = fr[4] != 0;
	wh_emit_bytes(n, AK_EVENT_ANCHOR_FRAME, b, sizeof b);
}

// check places each overlay, shapes it, and reports where its anchor is.
static void check(void *ctx) {
	(void)ctx;
	check_queued = false;
	uint32_t ids[64];
	uint32_t count = 0;
	for (uint32_t i = 0; i < noverlays && count < WH_LEN(ids); i++) ids[count++] = overlays[i]->id;
	for (uint32_t i = 0; i < count; i++) {
		node *n = wh_node(ids[i]);
		if (!ov(n)) continue;
		fit(n);
		shape(n);
	}
	for (uint32_t i = 0; i < count; i++) {
		node *n = wh_node(ids[i]);  // an AnchorFrame's flush may have destroyed it
		if (ov(n)) report_anchor(n);
	}
}

// wh_overlays_dirty notes that views moved (a batch, a scroll, a resize):
// the overlays are checked once the message being handled is.
void wh_overlays_dirty(void) {
	if (!noverlays || check_queued) return;
	check_queued = true;
	wh_post(check, NULL);
}

// wh_overlays_follow follows a view that resized: the overlays over it,
// if it's a root.
void wh_overlays_follow(node *r) {
	if (!noverlays) return;
	for (uint32_t i = 0; i < noverlays; i++)
		if (root_node(overlays[i]) == r) fit(overlays[i]);
	wh_overlays_dirty();
}

// wh_overlays_environment reports the environment again (it changed).
void wh_overlays_environment(void) {
	for (uint32_t i = 0; i < noverlays; i++) report_env(overlays[i]);
}

// ---- Props, Present ----

void wh_set_overlay_prop(node *n, uint16_t p, const ak_value *v) {
	wh_overlay *o = ov(n);
	if (!o) return;
	switch (p) {
	case AK_PROP_OVERLAY_ROOT:
		o->root = (uint32_t)v->i;
		o->env_known = false;
		fit(n);
		break;
	case AK_PROP_OVERLAY_ANCHOR:
		o->anchor = (uint32_t)v->i;
		o->anchor_known = false;  // reported again
		break;
	case AK_PROP_OVERLAY_MODAL:
		o->modal = v->i != 0;
		shape(n);
		wh_a11y_modal_changed(n);
		break;
	default:
		return;
	}
	wh_overlays_dirty();
}

// wh_present_overlay brings an overlay over its root's others.
void wh_present_overlay(node *n) {
	wh_overlay *o = ov(n);
	if (!o) return;
	o->presented = true;
	o->seq = ++present_seq;
	fit(n);
	raise(n);
	report_env(n);
	wh_overlays_dirty();
}

// ---- Presses, Back ----

// wh_overlay_pointer_down reports a press beginning at screen (on the
// window target) to each overlay over the root it's in that it's not on a
// view of: the topmost first.
void wh_overlay_pointer_down(HWND target, POINT screen) {
	if (!noverlays || !target) return;
	HWND top = GetAncestor(target, GA_ROOT);
	uint32_t ids[64];
	uint32_t count = 0;
	for (uint32_t i = 0; i < noverlays && count < WH_LEN(ids); i++) {
		node *n = overlays[i];
		wh_overlay *o = ov(n);
		node *r = root_node(n);
		if (!o->presented || !n->nkids || !r || !attached(n) || !node_listening(n, AK_EVENT_OUTSIDE_PRESS)) continue;
		if (GetAncestor(r->hwnd, GA_ROOT) != top) continue;
		RECT rr;
		GetWindowRect(r->hwnd, &rr);
		if (!PtInRect(&rr, screen) || IsChild(n->hwnd, target)) continue;
		ids[count++] = n->id;
	}
	// The topmost first.
	for (uint32_t i = 1; i < count; i++)
		for (uint32_t j = i; j > 0 && wh_overlay_seq(wh_node(ids[j])) > wh_overlay_seq(wh_node(ids[j - 1])); j--) {
			uint32_t t = ids[j];
			ids[j] = ids[j - 1], ids[j - 1] = t;
		}
	for (uint32_t i = 0; i < count; i++) {
		node *n = wh_node(ids[i]);
		node *r = root_node(n);
		if (!ov(n) || !r) continue;
		POINT p = screen;
		ScreenToClient(r->hwnd, &p);
		ak_event e = {.kind = AK_EVENT_OUTSIDE_PRESS, .x = wh_pt(r, p.x), .y = wh_pt(r, p.y)};
		wh_emit(n, &e);
	}
}

// topmost is the overlay over root (any root, NULL) in top-level window
// top shown last whose ok holds.
static node *topmost(HWND top, const node *root, bool (*ok)(const node *n)) {
	node *best = NULL;
	for (uint32_t i = 0; i < noverlays; i++) {
		node *n = overlays[i];
		wh_overlay *o = ov(n);
		node *r = root_node(n);
		if (!o->presented || !n->nkids || !r || !attached(n) || !IsWindowVisible(n->hwnd)) continue;
		if ((root && r != root) || (top && GetAncestor(n->hwnd, GA_ROOT) != top)) continue;
		if (ok && !ok(n)) continue;
		if (!best || o->seq > ov(best)->seq) best = n;
	}
	return best;
}

static bool backs(const node *n) { return n->intercept_back && node_listening(n, AK_EVENT_BACK); }

// wh_overlay_back gives going back (Alt+Left) to the topmost overlay over
// root that takes it; false: none does.
bool wh_overlay_back(node *root) {
	node *n = topmost(NULL, root, backs);
	if (!n) return false;
	wh_emit_kind(n, AK_EVENT_BACK);
	return true;
}

// wh_modal_overlay is the topmost modal overlay showing in top-level
// window top, or NULL.
node *wh_modal_overlay(HWND top) { return topmost(top, NULL, wh_overlay_modal); }

// ---- Automation ----

// wh_hit is the window a press at screen lands on in top, as Windows
// hit-tests: the topmost visible, enabled child under it (in its region),
// its children first, past those that answer HTTRANSPARENT.
static HWND hit_in(HWND w, POINT screen) {
	LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE);
	if (!(st & WS_VISIBLE)) return NULL;
	RECT r;
	GetWindowRect(w, &r);
	if (!PtInRect(&r, screen)) return NULL;
	HRGN rg = CreateRectRgn(0, 0, 0, 0);
	bool out = GetWindowRgn(w, rg) != ERROR && !PtInRegion(rg, screen.x - r.left, screen.y - r.top);
	DeleteObject(rg);
	if (out) return NULL;
	if (st & WS_DISABLED) return w;
	POINT c = screen;
	ScreenToClient(w, &c);
	RECT cr;
	GetClientRect(w, &cr);
	if (PtInRect(&cr, c))
		for (HWND k = GetWindow(w, GW_CHILD); k; k = GetWindow(k, GW_HWNDNEXT)) {
			HWND h = hit_in(k, screen);
			if (h) return h;
		}
	if (SendMessageW(w, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y)) == HTTRANSPARENT) return NULL;
	return w;
}

HWND wh_hit(HWND top, POINT screen) {
	HWND w = hit_in(top, screen);
	return w ? w : top;
}

// wh_overlays_dump writes each overlay showing and what's in it.
void wh_overlays_dump(ak_buf *b, void (*walk)(ak_buf *, node *, int)) {
	for (uint32_t i = 0; i < noverlays; i++) {
		node *n = overlays[i];
		wh_overlay *o = ov(n);
		if (!o->presented || !n->nkids) continue;
		// Its place in its parent's z-order: overlays over it, and whether
		// it's over its root.
		node *r = root_node(n);
		int above = 0;
		bool over = false;
		for (HWND w = GetWindow(n->hwnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
			if (is_overlay_of(w, r)) above++;
		for (HWND w = GetWindow(n->hwnd, GW_HWNDNEXT); w && r; w = GetWindow(w, GW_HWNDNEXT))
			if (w == r->hwnd) over = true;
		ak_buf_printf(b, "overlay %u z%d%s%s:\n", o->seq, above, over ? " over" : "", o->modal ? " modal" : "");
		walk(b, n, 1);
	}
}
