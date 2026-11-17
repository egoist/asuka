// focus.c: the keyboard's focus as the core follows it (RFC 0010).
//
// Every view that takes the keyboard's focus or loses it reports
// FocusChange (a TextField its Focus and Blur instead, controls.c): 1
// when it took it from a press or the app's Focus, 2 from the keyboard
// (Tab, or Focus right after a key: what a focus ring shows for), 0 when
// it lost it. A thread's CBT hook sees each focus change before it's made
// (HCBT_SETFOCUS), whoever makes it (SetFocus, a control's own click, a
// dialog's); the views it moved between are reported as a view gets it
// (WM_SETFOCUS) or, at the latest, before what follows from the input
// that moved it (a press's Press: wh_focus_flush), else once the message
// that moved it is handled. A press doesn't move the focus to a view with
// NoFocusOnPress: the hook refuses it while a press is dispatched.
//
// Tab and Shift+Tab move the focus in the views' order (the tree's: a
// view's children in their order, then the overlays over a root in the
// order they were presented; the host's own windows, a stack's header or
// a command bar, where they are), among the tab stops that show: Focusable
// views, pressable, adjustable ones, controls. In a FocusTrap (the
// innermost one the focus is in) they cycle within it; with a modal
// overlay showing, within the overlay. A control that takes Tab itself (a
// multiline field) keeps it, as in a dialog.
#include "win.h"

static bool key_last;      // the last input was a key: focus that comes now shows
static int pressing;       // a press being dispatched
static bool queued, queued_key;
static uint32_t reported;  // the view FocusChange last said has the focus
static HHOOK cbt;

// wh_input_seen notes a message of the user's input: a key or a press.
void wh_input_seen(const MSG *m) {
	switch (m->message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		key_last = true;
		break;
	case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
	case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
	case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_POINTERDOWN:
		key_last = false;
		break;
	}
}

bool wh_is_press_message(UINT m) {
	switch (m) {
	case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
	case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
	case WM_POINTERDOWN:
		return true;
	}
	return false;
}

// wh_pressing brackets a press's dispatch (on, then off).
void wh_pressing(bool on) {
	if (on) {
		pressing++;
		key_last = false;
	} else if (pressing > 0) {
		pressing--;
	}
}

void wh_key_pressed(void) { key_last = true; }

// focus_node is the view a window with the focus is (a control's: its
// view's); NULL for the host's own windows.
static node *focus_node(HWND h) {
	for (HWND w = h; w; w = GetParent(w)) {
		node *n = wh_node_of(w);
		if (n) return n->id && !(n->id & AK_HOST_ID_BIT) ? n : NULL;
		if (!(GetWindowLongPtrW(w, GWL_STYLE) & WS_CHILD)) break;
	}
	return NULL;
}

node *wh_focused_node(void) { return focus_node(GetFocus()); }

static void change(node *n, int64_t how) {
	if (!n || n->kind == AK_KIND_TEXT_FIELD) return;  // a field reports Focus and Blur
	ak_event e = {.id = n->id, .kind = AK_EVENT_FOCUS_CHANGE, .i = how};
	wh_dispatch(&e);
}

static void report(void *ctx) {
	(void)ctx;
	if (!queued) return;  // flushed already
	queued = false;
	HWND f = GetFocus();
	if (!f) return;  // another app's: the app's focus stays where it was, as its window's
	node *n = focus_node(f);
	uint32_t id = n ? n->id : 0;
	if (id == reported) return;
	node *was = reported ? wh_node(reported) : NULL;
	reported = id;
	change(was, 0);
	change(n, queued_key ? 2 : 1);
}

static LRESULT CALLBACK cbt_proc(int code, WPARAM wp, LPARAM lp) {
	if (code == HCBT_SETFOCUS) {
		HWND to = (HWND)wp;
		node *n = to ? focus_node(to) : NULL;
		if (pressing && n && n->no_focus_on_press) return 1;  // a press keeps the focus where it was
		queued_key = key_last;
		if (!queued) {
			queued = true;
			wh_post(report, NULL);
		}
	}
	return CallNextHookEx(cbt, code, wp, lp);
}

// wh_focus_flush reports a focus change waiting to be, now: before what
// follows from the input that moved it.
void wh_focus_flush(void) {
	if (queued) report(NULL);
}

void wh_focus_start(void) {
	if (!cbt) cbt = SetWindowsHookExW(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
}

// wh_focus_forget forgets a view going: its focus goes with it, unreported.
void wh_focus_forget(node *n) {
	if (n->id == reported) reported = 0;
}

// ---- Tab ----

typedef struct {
	HWND w;
	int group;          // 0 the host's windows over the content, 1 the views, 2 the host's under it, 3 overlays
	double key, k2, k3;
} kid;

static int by_order(const void *a, const void *b) {
	const kid *x = a, *y = b;
	if (x->group != y->group) return x->group < y->group ? -1 : 1;
	if (x->key != y->key) return x->key < y->key ? -1 : 1;
	if (x->k2 != y->k2) return x->k2 < y->k2 ? -1 : 1;
	return x->k3 < y->k3 ? -1 : x->k3 > y->k3;
}

// kids_in_order are p's child windows in the views' order: the views the
// core put in it by their index, the overlays over it as presented, the
// host's own windows by where they are (above the middle: before the
// views; below: after).
static uint32_t kids_in_order(HWND p, kid **out) {
	uint32_t cap = 16, n = 0;
	kid *v = wh_alloc(cap * sizeof *v);
	node *pn = wh_node_of(p);
	if (pn && pn->hwnd != p) pn = NULL;
	RECT pr;
	GetClientRect(p, &pr);
	bool rtl = wh_is_rtl(pn);
	int z = 0;
	for (HWND c = GetWindow(p, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT), z++) {
		if (n == cap) v = wh_realloc(v, (cap *= 2) * sizeof *v);
		kid k = {.w = c, .k3 = z};
		node *cn = wh_node_of(c);
		if (cn && cn->hwnd != c) cn = NULL;
		uint32_t at = 0;
		bool core_kid = false;
		if (cn && pn && cn->parent == pn) {
			for (; at < pn->nkids; at++)
				if (pn->kids[at] == cn) break;
			core_kid = at < pn->nkids;
		}
		if (cn && cn->kind == AK_KIND_OVERLAY) {
			k.group = 3, k.key = wh_overlay_seq(cn);
		} else if (core_kid || (cn && cn == H.root)) {
			k.group = 1, k.key = at;
		} else {
			RECT r = wh_rect_in(c, p);
			double mid = (r.top + r.bottom) / 2.0;
			k.group = mid < pr.bottom / 2.0 ? 0 : 2;
			k.key = r.top, k.k2 = rtl ? -r.right : r.left;
		}
		v[n++] = k;
	}
	qsort(v, n, sizeof *v, by_order);
	*out = v;
	return n;
}

typedef struct {
	HWND *v;
	uint32_t n, cap;
} stops;

static void add_stops(HWND p, stops *s) {
	kid *ks;
	uint32_t n = kids_in_order(p, &ks);
	for (uint32_t i = 0; i < n; i++) {
		HWND c = ks[i].w;
		LONG_PTR st = GetWindowLongPtrW(c, GWL_STYLE), ex = GetWindowLongPtrW(c, GWL_EXSTYLE);
		if (!(st & WS_VISIBLE) || (st & WS_DISABLED)) continue;
		if (st & WS_TABSTOP) {
			if (s->n == s->cap) s->v = wh_realloc(s->v, (s->cap = s->cap ? s->cap * 2 : 32) * sizeof *s->v);
			s->v[s->n++] = c;
		}
		if (ex & WS_EX_CONTROLPARENT) add_stops(c, s);
	}
	free(ks);
}

// trap_of is the innermost FocusTrap view the focus (in f) is in.
static node *trap_of(node *f) {
	for (node *x = f; x; x = x->parent)
		if (x->focus_trap && x->hwnd) return x;
	return NULL;
}

// wh_tab_key moves the focus as Tab or Shift+Tab does; false: not Tab, or
// a key the focused control takes.
bool wh_tab_key(MSG *m) {
	if (m->message != WM_KEYDOWN || m->wParam != VK_TAB || wh_key_down(VK_CONTROL) || wh_key_down(VK_MENU)) return false;
	HWND top = wh_key_window(m->hwnd);
	if (!top) return false;
	HWND f = GetFocus();
	if (f && GetAncestor(f, GA_ROOT) != top) f = NULL;
	if (f && (SendMessageW(f, WM_GETDLGCODE, VK_TAB, (LPARAM)m) & (DLGC_WANTTAB | DLGC_WANTALLKEYS))) return false;
	HWND scope = top;
	node *trap = f ? trap_of(focus_node(f)) : NULL;
	node *modal = wh_modal_overlay(top);
	if (trap && (!modal || IsChild(modal->hwnd, trap->hwnd))) scope = trap->hwnd;
	else if (modal) scope = modal->hwnd;
	stops s = {0};
	add_stops(scope, &s);
	if (!s.n) {
		free(s.v);
		return scope != top;  // a trap with nothing to go to keeps it
	}
	bool back = wh_key_down(VK_SHIFT);
	int64_t at = -1;
	for (uint32_t i = 0; i < s.n && f; i++)
		if (s.v[i] == f || IsChild(s.v[i], f)) at = i;
	int64_t n = s.n;
	int64_t next = at < 0 ? (back ? n - 1 : 0) : ((at + (back ? -1 : 1)) % n + n) % n;
	HWND to = s.v[next];
	free(s.v);
	key_last = true;
	SendMessageW(top, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);  // focus shows, as a dialog's
	SetFocus(to);
	if (SendMessageW(to, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessageW(to, EM_SETSEL, 0, -1);
	return true;
}
