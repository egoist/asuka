// snackbar.c: a Snackbar on Windows. Win32 has none, so it's an overlay at
// the bottom of the window it's on, drawn as Fluent's InfoBar: a card in
// the layer color with its stroke and 8 point corners, the message, a
// standard button for its action and a subtle close button (the host's own
// buttons, drawn as Fluent's in custom draw). It slides up in, and down out
// (it just shows and goes with animations off); its time waits while the
// mouse is over it or the keyboard's focus is in it; screen readers hear it
// as a UI Automation notification, and reach its buttons, which are BUTTON
// controls.
#include "nav.h"

struct wh_snack {
	HWND owner;   // the top-level window it's on
	HWND action;  // BUTTON: its action (NULL: none)
	HWND close;   // BUTTON: closes it
	int32_t duration;  // AK_SNACKBAR_DURATION_*
	int left_ms;       // of its time
	bool counting;     // its time's timer runs
	bool sliding;      // its slide's timer runs
	double slide_at;   // when the slide began (seconds)
	int slide_from, slide_to;
	bool out;      // sliding out
	int offset;    // px below its place, while it slides
	RECT text;     // the message's, in its window (px)
	RECT frame;    // its place in its owner's client area (px)
	bool done;     // reported (its action, its time, closed) or destroyed
	bool dropped;  // its window let go of
};

// Its measures, in points.
#define SNACK_MARGIN 24   // from the window's edges
#define SNACK_MAX_W 560   // at most this wide
#define SNACK_PAD_L 16
#define SNACK_PAD_R 8
#define SNACK_PAD_Y 14
#define SNACK_GAP 12
#define SNACK_MIN_H 48
#define SNACK_BUTTON_H 32
#define SNACK_CLOSE_W 32
#define SNACK_RADIUS 8
#define SNACK_SLIDE 0.2  // seconds

// Its window's timers: its time (each second), its slide (TIMER_SNACK's
// neighbor, on the snackbar's own window).
#define TIMER_SNACK_SLIDE (TIMER_SNACK + 100)

#define SPI_GETMESSAGEDURATION_ 0x2016  // seconds

// The snackbars showing, in the order they came.
static node **snacks;
static uint32_t nsnacks;

static wh_snack *snack_of(node *n) {
	if (!n->snack) n->snack = wh_alloc(sizeof *n->snack);
	return n->snack;
}

static void place_snack(node *n);
static void snack_button(node *n);

void wh_set_snack_prop(node *n, uint16_t p, const ak_value *v) {
	wh_snack *s = snack_of(n);
	switch (p) {
	case AK_PROP_SNACKBAR_MESSAGE:
		wstr_set(&n->message, v->s);
		wstr_set(&n->a11y_label, v->s);  // its group's name
		if (s->owner) {
			place_snack(n);
			InvalidateRect(n->hwnd, NULL, FALSE);
		}
		break;
	case AK_PROP_SNACKBAR_ACTION:
		wstr_set(&n->title, v->s);
		if (s->owner) {
			snack_button(n);
			place_snack(n);
		}
		break;
	case AK_PROP_SNACKBAR_DURATION:
		s->duration = (int32_t)v->i;
		break;
	case AK_PROP_SNACKBAR_WINDOW:
		n->nav->on = (uint32_t)v->i;
		break;
	}
}

// tooltip gives a window of the snackbar's a tooltip; NULL: none.
static void tooltip(HWND hwnd, const wchar_t *text) {
	if (!hwnd) return;
	if (!H.tips) {
		H.tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, L"", WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, H.window, NULL, GetModuleHandleW(NULL), NULL);
		if (H.tips) SendMessageW(H.tips, TTM_SETMAXTIPWIDTH, 0, 400);
	}
	if (!H.tips) return;
	TTTOOLINFOW ti = {sizeof ti};
	ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
	ti.hwnd = hwnd;
	ti.uId = (UINT_PTR)hwnd;
	ti.hinst = GetModuleHandleW(NULL);
	SendMessageW(H.tips, TTM_DELTOOLW, 0, (LPARAM)&ti);
	if (!text) return;
	ti.lpszText = (wchar_t *)text;  // the control copies it
	SendMessageW(H.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static LRESULT snack_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled);
static void slide_snack(node *n, bool out);
static void start_snack(node *n);
static void report(node *n, int kind);

static void push_snack(node *n) {
	snacks = wh_realloc(snacks, (nsnacks + 1) * sizeof *snacks);
	snacks[nsnacks++] = n;
}

static void pull_snack(node *n) {
	for (uint32_t i = 0; i < nsnacks; i++)
		if (snacks[i] == n) {
			memmove(snacks + i, snacks + i + 1, (nsnacks - i - 1) * sizeof *snacks);
			nsnacks--;
			return;
		}
}

// snack_travel is how far it slides: from below the window's edge.
static int snack_travel(node *n) { return wh_px(n, SNACK_MIN_H + SNACK_MARGIN + 8); }

// wh_present_snack shows it at the bottom of the window it's declared in
// (wh_present_on: the main window, a Window, the sheet over it).
void wh_present_snack(node *n) {
	wh_snack *s = snack_of(n);
	if (s->owner || s->done || n->dying || !n->hwnd) return;
	HWND owner = wh_present_on(n->nav->on);
	if (!owner) owner = H.window;  // its Window never opened
	if (!owner) return;
	s->owner = owner;
	if (owner != H.window) n->k = (float)wh_dpi(owner) / 96;
	n->host_msg = snack_message;
	SetParent(n->hwnd, owner);
	SetWindowLongPtrW(n->hwnd, GWL_EXSTYLE, GetWindowLongPtrW(n->hwnd, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
	s->close = CreateWindowExW(0, L"BUTTON", wh_word("close"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, n->hwnd, (HMENU)2,
		GetModuleHandleW(NULL), NULL);
	SendMessageW(s->close, WM_UPDATEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
	tooltip(s->close, wh_word("close"));
	wh_bind(n, s->close);
	snack_button(n);
	push_snack(n);

	bool animate = !H.env.env.reduce_motion;
	if (animate) s->offset = snack_travel(n);
	place_snack(n);
	ShowWindow(n->hwnd, SW_SHOWNOACTIVATE);
	if (animate) slide_snack(n, false);
	// Screen readers hear it: its message, and its action.
	int len;
	char *msg = wh_utf8(wstr_c(&n->message), n->message.n, &len);
	if (n->title.n) {
		int tl;
		char *t = wh_utf8(wstr_c(&n->title), n->title.n, &tl);
		char *both = wh_alloc((size_t)len + (size_t)tl + 3);
		memcpy(both, msg, (size_t)len);
		memcpy(both + len, ", ", 2);
		memcpy(both + len + 2, t, (size_t)tl);
		wh_announce(both, (size_t)len + 2 + (size_t)tl);
		free(both);
		free(t);
	} else {
		wh_announce(msg, (size_t)len);
	}
	free(msg);
	start_snack(n);
}

// snack_button makes, retitles or removes its action's button.
static void snack_button(node *n) {
	wh_snack *s = n->snack;
	if (!n->title.n) {
		if (s->action) {
			wh_unbind(s->action);
			DestroyWindow(s->action);
			s->action = NULL;
		}
		return;
	}
	if (!s->action) {
		s->action = CreateWindowExW(0, L"BUTTON", wstr_c(&n->title), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, n->hwnd, (HMENU)1,
			GetModuleHandleW(NULL), NULL);
		SendMessageW(s->action, WM_SETFONT, (WPARAM)wh_body_font(n), 0);
		SendMessageW(s->action, WM_UPDATEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
		wh_bind(n, s->action);
		return;
	}
	SetWindowTextW(s->action, wstr_c(&n->title));
	InvalidateRect(s->action, NULL, FALSE);
}

// place_snack lays it out: as wide as its message needs, up to
// SNACK_MAX_W and its window's width, centered at the bottom; the message,
// its button and the close button (mirrored right to left).
static void place_snack(node *n) {
	wh_snack *s = n->snack;
	if (!s || !s->owner || !n->hwnd) return;
	RECT cr;
	GetClientRect(s->owner, &cr);
	int max_w = wh_mini(wh_px(n, SNACK_MAX_W), cr.right - 2 * wh_px(n, SNACK_MARGIN));
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_body_font(n));
	int bw = 0;
	if (s->action) bw = wh_text_extent(dc, wstr_c(&n->title), n->title.n).cx + wh_px(n, 24);  // Fluent's button: 12 points each side
	int fixed = wh_px(n, SNACK_PAD_L) + wh_px(n, SNACK_GAP) + wh_px(n, SNACK_CLOSE_W) + wh_px(n, SNACK_PAD_R);
	if (bw > 0) fixed += bw + wh_px(n, 8);
	int avail = wh_maxi(max_w - fixed, wh_px(n, 60));
	RECT tr = {0, 0, avail, 0};
	DrawTextW(dc, wstr_c(&n->message), n->message.n, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
	SelectObject(dc, old);
	int tw = wh_mini(tr.right - tr.left, avail), th = tr.bottom - tr.top;
	int height = wh_maxi(wh_maxi(wh_px(n, SNACK_MIN_H), th + 2 * wh_px(n, SNACK_PAD_Y)), wh_px(n, SNACK_BUTTON_H) + wh_px(n, 16));
	int width = tw + fixed;
	int x = (cr.right - width) / 2;
	int y = cr.bottom - wh_px(n, SNACK_MARGIN) - height;
	s->frame = (RECT){x, y, x + width, y + height};
	SetWindowPos(n->hwnd, HWND_TOP, x, y + s->offset, width, height, SWP_NOACTIVATE);

	bool rtl = wh_is_rtl(n);
#define AT(left, w) (rtl ? width - (left) - (w) : (left))
	s->text.left = AT(wh_px(n, SNACK_PAD_L), tw);
	s->text.top = (height - th) / 2;
	s->text.right = s->text.left + tw, s->text.bottom = s->text.top + th;
	int bh = wh_px(n, SNACK_BUTTON_H);
	int next = wh_px(n, SNACK_PAD_L) + tw + wh_px(n, SNACK_GAP);
	if (s->action) SetWindowPos(s->action, NULL, AT(next, bw), (height - bh) / 2, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
	int cw = wh_px(n, SNACK_CLOSE_W);
	SetWindowPos(s->close, NULL, AT(width - wh_px(n, SNACK_PAD_R) - cw, cw), (height - cw) / 2, cw, cw, SWP_NOZORDER | SWP_NOACTIVATE);
#undef AT
	InvalidateRect(n->hwnd, NULL, FALSE);
}

// wh_place_snacks lays out the snackbars showing: a window resized.
void wh_place_snacks(void) {
	for (uint32_t i = 0; i < nsnacks; i++)
		if (!snacks[i]->snack->done) place_snack(snacks[i]);
}

// wh_paint_snack draws its card: the layer color, its stroke, its corners.
void wh_paint_snack(node *n, gfx g, float w, float h) {
	float k = wh_scale_of(n);
	const palette *p = &H.theme;
	float r = SNACK_RADIUS * k;
	g_fill_round_rect(g, rgba_argb(p->layer), 0, 0, w, h, r);
	g_stroke_round_rect(g, rgba_argb(p->card_stroke), wh_maxf(k, 1), 0.5f, 0.5f, w - 1, h - 1, r);
}

// wh_paint_snack_text draws its message.
void wh_paint_snack_text(node *n, HDC dc) {
	if (!n->snack) return;
	RECT tr = n->snack->text;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(rgba_over(H.theme.label, H.theme.layer)));
	HGDIOBJ old = SelectObject(dc, wh_body_font(n));
	DrawTextW(dc, wstr_c(&n->message), n->message.n, &tr, DT_WORDBREAK | DT_NOPREFIX | wh_start_side(wh_is_rtl(n)));
	SelectObject(dc, old);
}

bool wh_snack_is_close(node *n, HWND ctl) { return n->snack && ctl && ctl == n->snack->close; }

// wh_snack_clicked is its buttons' BN_CLICKED.
void wh_snack_clicked(node *n, HWND ctl) {
	if (wh_snack_is_close(n, ctl)) report(n, AK_EVENT_DISMISSED);
	else report(n, AK_EVENT_ACTION);  // Action's Int: 0, its one action
}

static void stop_snack(node *n) {
	wh_snack *s = n->snack;
	if (s->counting && n->hwnd) KillTimer(n->hwnd, TIMER_SNACK);
	s->counting = false;
}

static void leave_snack(node *n);

// report tells the core it went (its action, its time running out,
// closed), and takes it out; the core destroys it.
static void report(node *n, int kind) {
	wh_snack *s = n->snack;
	if (!s || s->done) return;
	s->done = true;
	stop_snack(n);
	leave_snack(n);
	wh_emit_kind(n, kind);
}

// start_snack starts its time: four seconds, ten for SnackbarLong.
// Windows' "Dismiss notifications after this amount of time" setting
// (SPI_GETMESSAGEDURATION) lengthens it, as UWP's notifications do; with
// a screen reader on, one with a button stays until it's closed, and one
// without stays long.
static void start_snack(node *n) {
	wh_snack *s = n->snack;
	if (s->duration == AK_SNACKBAR_DURATION_INDEFINITE) return;
	int ms = s->duration == AK_SNACKBAR_DURATION_LONG ? 10000 : 4000;
	ULONG secs = 0;
	if (SystemParametersInfoW(SPI_GETMESSAGEDURATION_, 0, &secs, 0) && secs > 5) ms = wh_maxi(ms, (int)secs * 1000);
	if (H.env.env.screen_reader) {
		if (n->title.n) return;
		ms = wh_maxi(ms, 10000);
	}
	s->left_ms = ms;
	s->counting = true;
	SetTimer(n->hwnd, TIMER_SNACK, 1000, NULL);
}

// held reports whether the mouse is over it or the keyboard's focus in it:
// its time waits meanwhile.
static bool held(node *n) {
	HWND f = GetFocus();
	if (f && (f == n->hwnd || IsChild(n->hwnd, f))) return true;
	POINT p;
	RECT r;
	GetCursorPos(&p);
	GetWindowRect(n->hwnd, &r);
	return PtInRect(&r, p);
}

// slide_snack slides it in (up to its place) or out (down past the
// window's edge, then it's gone), easing out, over SNACK_SLIDE.
static void slide_snack(node *n, bool out) {
	wh_snack *s = n->snack;
	s->out = out;
	s->slide_from = s->offset;
	s->slide_to = out ? wh_maxi(snack_travel(n), (s->frame.bottom - s->frame.top) + wh_px(n, SNACK_MARGIN)) : 0;
	s->slide_at = wh_seconds();
	s->sliding = true;
	SetTimer(n->hwnd, TIMER_SNACK_SLIDE, 15, NULL);
}

static void drop_snack(node *n);

static void slide_step(node *n) {
	wh_snack *s = n->snack;
	double t = fmin((wh_seconds() - s->slide_at) / SNACK_SLIDE, 1);
	double e = s->out ? t * t * t : 1 - pow(1 - t, 3);  // ease in as it leaves, out as it comes
	s->offset = s->slide_from + (int)lround((s->slide_to - s->slide_from) * e);
	SetWindowPos(n->hwnd, NULL, s->frame.left, s->frame.top + s->offset, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
	if (t >= 1) {
		KillTimer(n->hwnd, TIMER_SNACK_SLIDE);
		s->sliding = false;
		if (s->out) drop_snack(n);
	}
}

// snack_message is its window's own messages: it takes clicks (rather than
// letting them through to what's under it, as a view that doesn't press
// does), and its timers.
static LRESULT snack_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	(void)lp;
	*handled = false;
	wh_snack *s = n->snack;
	switch (m) {
	case WM_NCHITTEST:
		*handled = true;
		return HTCLIENT;
	case WM_TIMER:
		if (wp == TIMER_SNACK) {
			*handled = true;
			if (s->done || held(n)) return 0;
			s->left_ms -= 1000;
			if (s->left_ms <= 0) report(n, AK_EVENT_DISMISSED);
			return 0;
		}
		if (wp == TIMER_SNACK_SLIDE) {
			*handled = true;
			slide_step(n);
			return 0;
		}
		break;
	}
	return 0;
}

static void drop_later(void *ctx) { drop_snack(ctx); }

// leave_snack takes it out: sliding, or at once with animations off.
static void leave_snack(node *n) {
	wh_snack *s = n->snack;
	if (!s->owner || s->out) return;
	if (s->action) EnableWindow(s->action, FALSE);
	EnableWindow(s->close, FALSE);
	if (H.env.env.reduce_motion || !IsWindowVisible(n->hwnd)) {
		// Gone at once, after the message in hand (its button's click).
		s->out = true;
		ShowWindow(n->hwnd, SW_HIDE);
		wh_post(drop_later, n);
		return;
	}
	slide_snack(n, true);
}

// free_snack lets go of a destroyed snackbar's node.
static void free_snack(node *n) {
	for (uint32_t i = 0; i < n->nkids; i++) n->kids[i]->parent = NULL;
	wh_release(n);
	free(n->snack);
	n->snack = NULL;
	free(n->nav);
	n->nav = NULL;
	wh_free_node(n);
}

// drop_snack lets its window go, once it's out; and its node, once the
// core destroyed it.
static void drop_snack(node *n) {
	wh_snack *s = n->snack;
	if (s->dropped) return;
	s->dropped = true;
	pull_snack(n);
	if (n->hwnd) {
		KillTimer(n->hwnd, TIMER_SNACK_SLIDE);
		KillTimer(n->hwnd, TIMER_SNACK);
	}
	s->sliding = s->counting = false;
	if (s->action) tooltip(s->action, NULL);
	if (s->close) tooltip(s->close, NULL);
	if (n->hwnd) {
		HWND hw = n->hwnd;
		wh_forget_tree(hw);
		DestroyWindow(hw);
		n->hwnd = NULL;
	}
	s->action = s->close = NULL;
	if (n->dying) free_snack(n);
}

// wh_destroy_snack is the core's Destroy: out, animated.
void wh_destroy_snack(node *n) {
	wh_snack *s = snack_of(n);
	s->done = true;
	if (!n->hwnd && !s->dropped) {
		free_snack(n);
		return;
	}
	stop_snack(n);
	if (!s->owner || s->dropped) {
		if (!s->dropped) drop_snack(n);  // never shown: drop frees it
		else free_snack(n);
		return;
	}
	leave_snack(n);
}

bool wh_tap_overlay(const wchar_t *t) { return wh_snack_tap(t) || wh_alert_dialog_tap(t) || wh_pane_tap(t) || wh_combo_tap(t) || wh_suggest_tap(t); }

// wh_snack_tap is automation's tap on a snackbar showing: its action's
// title, or Close.
bool wh_snack_tap(const wchar_t *t) {
	for (uint32_t i = nsnacks; i-- > 0;) {
		node *n = snacks[i];
		wh_snack *s = n->snack;
		if (s->done || !n->hwnd || !IsWindowVisible(n->hwnd)) continue;
		if (s->action && n->title.n && !wcscmp(wstr_c(&n->title), t)) {
			SendMessageW(s->action, BM_CLICK, 0, 0);
			return true;
		}
		if (s->close && !wcscmp(wh_word("close"), t)) {
			SendMessageW(s->close, BM_CLICK, 0, 0);
			return true;
		}
	}
	return false;
}
