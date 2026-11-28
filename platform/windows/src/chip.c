// chip.c: Chip and Disclosure, views the host draws itself as WinUI 3
// draws its own controls, keeping a control's keyboard, focus and UI
// Automation.
//
// A Chip is Fluent's ToggleButton (a filter chip) or Button (an action
// chip, an input chip) in a pill (its corners half its height): the
// control fill and stroke of the standard button, hover and pressed
// fills, the accent fill and the on-accent text while a filter chip is
// selected, a checkmark (E73E) before its title then (in its Icon's
// place, as Material's does), the disabled colors, and the keyboard's
// focus visual around the pill. A Removable chip has a remove button at
// its end, an ✕ (E711) with a subtle circle under the pointer: a window
// of its own (KIND_CHIP_REMOVE), so screen readers find it as a button
// ("Remove Ana": the desktop word remove and the title) that Invoke
// presses. Space and Enter press a chip with the keyboard's focus,
// Delete and Backspace remove a removable one. UI Automation: a filter
// chip is a button with the Toggle pattern, an action chip one with
// Invoke.
//
// A Disclosure is the header of WinUI's Expander: a card (the card fill
// and stroke, 4 corners, 48 tall) with the title at its start and the
// chevron's button at its end (32 square, a subtle fill under the
// pointer), the chevron pointing down while it's collapsed and up while
// it's expanded, turning between the two as WinUI's does (at once with
// Reduce Motion). Space and Enter flip it; UI Automation: a button with
// the ExpandCollapse pattern.
//
// A contrast theme draws both in the system's colors, as WinUI does:
// ButtonFace and ButtonText, Highlight and HighlightText under the
// pointer and for a selected chip, GrayText disabled. Right to left,
// everything inside is mirrored (the chip's glyph after its title on the
// right, the remove button on the left; the disclosure's title on the
// right, its chevron on the left); its frame is the core's.
#include "win.h"

// A chip's metrics, in points.
#define CHIP_HEIGHT 32
#define CHIP_PAD 12          // its ends' padding
#define CHIP_GLYPH 16        // a leading glyph (its icon, a filter chip's checkmark)
#define CHIP_GAP 8           // between the glyph and the title
#define CHIP_REMOVE 24       // the remove button's square
#define CHIP_REMOVE_GAP 4    // between the title and the remove button
#define CHIP_REMOVE_END 4    // after the remove button
#define CHIP_REMOVE_GLYPH 10

// The Expander header's (WinUI's ExpanderHeader*), in points.
#define DISC_MIN_HEIGHT 48
#define DISC_PAD 16          // the title's padding at the start
#define DISC_CHEVRON 32      // the chevron's button
#define DISC_CHEVRON_END 8   // its margin at the end
#define DISC_CHEVRON_GAP 20  // between the title and it
#define DISC_TURN 0.167      // seconds the chevron takes to turn

static bool high_contrast(void) { return wh_high_contrast(); }

static rgba sys_rgba(int i) {
	COLORREF c = GetSysColor(i);
	return (rgba){GetRValue(c), GetGValue(c), GetBValue(c), 255};
}

static rgba on_color(rgba fill) {
	double lum = 0.299 * fill.r + 0.587 * fill.g + 0.114 * fill.b;
	return lum > 160 ? rgba_hex(0x000000) : rgba_hex(0xFFFFFF);
}

static void track_leave(HWND hwnd) {
	TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
	TrackMouseEvent(&t);
}

static bool inside(HWND hwnd, LPARAM lp) {
	RECT r;
	GetClientRect(hwnd, &r);
	POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
	return PtInRect(&r, p) != 0;
}

static bool selected_chip(const node *n) { return n->kind == AK_KIND_CHIP && n->chip_filter && n->on; }

// ---- Pressing: the mouse, the keyboard, a screen reader ----

static void start_turn(node *n);

// activate is a chip's or a disclosure's press: a filter chip toggles (it
// shows its new state at once), a disclosure flips, an action chip
// presses.
static void activate(node *n) {
	if (n->disabled || n->dead) return;
	wh_focus_flush();
	if (n->kind == AK_KIND_DISCLOSURE || (n->kind == AK_KIND_CHIP && n->chip_filter)) {
		bool was = n->on;
		n->on = !n->on;
		if (n->kind == AK_KIND_DISCLOSURE) {
			start_turn(n);
			wh_uia_expand_changed(n, was);
		} else {
			wh_uia_toggle_changed(n, was);
		}
		wh_redraw(n);
		wh_emit_bool(n, AK_EVENT_TOGGLED, n->on);
		return;
	}
	wh_press(n, n->hwnd);
}

static void remove_chip(node *chip) {
	if (!chip || chip->dead || chip->disabled || !chip->chip_removable) return;
	wh_focus_flush();
	wh_emit_kind(chip, AK_EVENT_REMOVE);
}

bool wh_own_press(node *n) {
	switch (n->kind) {
	case AK_KIND_CHIP:
	case AK_KIND_DISCLOSURE:
		activate(n);
		return true;
	case KIND_CHIP_REMOVE:
		remove_chip(n->chip_peer);
		return true;
	}
	return false;
}

bool wh_own_invokes(const node *n) {
	if (n->kind == KIND_CHIP_REMOVE) return n->chip_peer && !n->chip_peer->disabled;
	return n->kind == AK_KIND_CHIP && !n->chip_filter && !n->disabled && (node_listening(n, AK_EVENT_PRESS) || n->menu.n);
}

// own_message is a chip's, a disclosure's and a remove button's mouse and
// keyboard, before a view's own handling.
static LRESULT own_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = true;
	HWND hwnd = n->hwnd;
	node *chip = n->kind == KIND_CHIP_REMOVE ? n->chip_peer : n;
	switch (m) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (!chip || chip->disabled) return 0;
		if (n->kind != KIND_CHIP_REMOVE && !n->no_focus_on_press && GetFocus() != hwnd) SetFocus(hwnd);
		SetCapture(hwnd);
		n->tracking = true;
		n->pressed = true;
		if (n->kind != KIND_CHIP_REMOVE) wh_emit_bool(n, AK_EVENT_PRESSED, true);
		wh_redraw(chip);
		return 0;
	case WM_MOUSEMOVE: {
		if (!n->hot) {
			n->hot = true;
			track_leave(hwnd);
			if (chip) wh_redraw(chip);
		}
		if (n->tracking) {
			bool in = inside(hwnd, lp);
			if (in != n->pressed) {
				n->pressed = in;
				if (chip) wh_redraw(chip);
			}
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		if (n->hot) {
			n->hot = false;
			if (chip) wh_redraw(chip);
		}
		return 0;
	case WM_LBUTTONUP: {
		bool fire = n->tracking && n->pressed && inside(hwnd, lp);
		n->tracking = false;
		if (GetCapture() == hwnd) {
			n->releasing = true;
			ReleaseCapture();
			n->releasing = false;
		}
		if (n->pressed) {
			n->pressed = false;
			if (n->kind != KIND_CHIP_REMOVE) wh_emit_bool(n, AK_EVENT_PRESSED, false);
			if (chip && !chip->dead) wh_redraw(chip);
		}
		if (fire) wh_own_press(n);
		return 0;
	}
	case WM_CAPTURECHANGED:
		if (n->tracking && (HWND)lp != hwnd && !n->releasing) {
			n->tracking = n->pressed = false;
			if (chip) wh_redraw(chip);
		}
		return 0;
	case WM_GETDLGCODE: {
		LRESULT code = 0;
		if (lp) {
			MSG *msg = (MSG *)lp;
			if (msg->message == WM_KEYDOWN &&
				(msg->wParam == VK_RETURN || msg->wParam == VK_SPACE || (n->chip_removable && (msg->wParam == VK_DELETE || msg->wParam == VK_BACK))))
				code |= DLGC_WANTALLKEYS;
			if (msg->message == WM_CHAR && msg->wParam == ' ') code |= DLGC_WANTCHARS;
		}
		return code;
	}
	case WM_KEYDOWN:
		if (n->kind == KIND_CHIP_REMOVE) break;
		if (wp == VK_RETURN) {
			activate(n);
			return 0;
		}
		if (wp == VK_SPACE) {
			if (!n->space_down && !n->disabled) {
				n->space_down = n->pressed = true;
				wh_redraw(n);
			}
			return 0;
		}
		if ((wp == VK_DELETE || wp == VK_BACK) && n->kind == AK_KIND_CHIP && n->chip_removable) {
			remove_chip(n);
			return 0;
		}
		break;
	case WM_KEYUP:
		if (wp == VK_SPACE && n->space_down) {
			n->space_down = n->pressed = false;
			wh_redraw(n);
			activate(n);
			return 0;
		}
		break;
	case WM_CHAR:
		if (wp == ' ' || wp == '\b') return 0;
		break;
	case WM_KILLFOCUS:
		if (n->space_down) {
			n->space_down = n->pressed = false;
			wh_redraw(n);
		}
		break;
	case WM_SIZE:
		if (n->kind == AK_KIND_CHIP) wh_layout_chip(n);
		break;
	case WM_TIMER:
		if (wp == TIMER_CHEVRON && n->kind == AK_KIND_DISCLOSURE) {
			double target = n->on ? 1 : 0, step = 1.0 / 60 / DISC_TURN;
			if (fabs(target - n->phase) <= step) {
				n->phase = target;
				KillTimer(hwnd, TIMER_CHEVRON);
			} else {
				n->phase += target > n->phase ? step : -step;
			}
			wh_redraw(n);
			return 0;
		}
		break;
	}
	*handled = false;
	return 0;
}

// ---- Chip ----

static uint16_t lead_glyph(const node *n) {
	if (selected_chip(n)) return 0xE73E;  // CheckMark
	return n->icon && *n->icon ? wh_directed_glyph(n->icon, wh_is_rtl(n)) : 0;
}

static void name_remove(node *n) {
	node *rm = n->chip_peer;
	if (!rm) return;
	wchar_t name[600];
	swprintf(name, WH_LEN(name), L"%ls %ls", wh_word("remove"), wstr_c(n->a11y_label.n ? &n->a11y_label : &n->title));
	wstr_setw(&rm->a11y_label, name, -1);
	wh_tooltip_on(rm->hwnd, wh_word("remove"));
}

static void make_remove(node *n) {
	if (n->chip_peer || !n->hwnd) return;
	node *rm = wh_node_new(wh_host_id(), KIND_CHIP_REMOVE);
	rm->hwnd = wh_make_view(0);
	rm->k = n->k, rm->dir = n->dir;
	rm->a11y_role = AK_ROLE_BUTTON;
	rm->host_msg = own_message;
	rm->chip_peer = n;
	n->chip_peer = rm;
	SetParent(rm->hwnd, n->hwnd);
	wh_node_add(rm);
	name_remove(n);
	wh_layout_chip(n);
}

static void drop_remove(node *n) {
	node *rm = n->chip_peer;
	if (!rm) return;
	n->chip_peer = NULL;
	rm->chip_peer = NULL;
	wh_map_del(&H.nodes, rm->id);
	if (rm->hwnd) {
		HWND h = rm->hwnd;
		wh_tooltip_on(h, NULL);
		wh_forget_tree(h);
		DestroyWindow(h);
	}
	rm->hwnd = NULL;
	wh_free_node(rm);
}

void wh_make_chip(node *n) {
	n->hwnd = wh_make_view(WS_TABSTOP);
	n->host_msg = own_message;
}

void wh_make_disclosure(node *n) {
	n->hwnd = wh_make_view(WS_TABSTOP);
	n->host_msg = own_message;
}

// wh_own_release lets go of what a chip or a disclosure has besides its
// window: a chip's remove button.
void wh_own_release(node *n) {
	if (n->kind == AK_KIND_CHIP) drop_remove(n);
	if (n->kind == AK_KIND_DISCLOSURE && n->hwnd) KillTimer(n->hwnd, TIMER_CHEVRON);
}

void wh_set_chip_prop(node *n, uint16_t p, const ak_value *v) {
	switch (p) {
	case AK_PROP_CHIP_TITLE:
		wstr_set(&n->title, v->s);
		name_remove(n);
		break;
	case AK_PROP_CHIP_ICON:
		free(n->icon);
		n->icon = v->s.len ? wh_strndup(v->s.p, v->s.len) : NULL;
		if (n->icon && !wh_symbol_glyph(n->icon)) wh_warn_symbol(n->icon);
		break;
	case AK_PROP_CHIP_FILTER:
		n->chip_filter = v->i != 0;
		break;
	case AK_PROP_CHIP_SELECTED: {
		bool was = n->on;
		n->on = v->i != 0;
		if (was != n->on) wh_uia_toggle_changed(n, was);
		break;
	}
	case AK_PROP_CHIP_REMOVABLE:
		n->chip_removable = v->i != 0;
		if (n->chip_removable) make_remove(n);
		else drop_remove(n);
		break;
	}
	wh_redraw(n);
}

// wh_chip_label follows a chip's A11yLabel: its remove button's name.
void wh_chip_label(node *n) { name_remove(n); }

// wh_layout_chip places a chip's remove button at its end.
void wh_layout_chip(node *n) {
	node *rm = n->chip_peer;
	if (!rm || !rm->hwnd || !n->hwnd) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	int s = wh_px(n, CHIP_REMOVE), end = wh_px(n, CHIP_REMOVE_END);
	int x = wh_is_rtl(n) ? end : r.right - end - s;
	rm->k = n->k;
	SetWindowPos(rm->hwnd, NULL, x, (r.bottom - s) / 2, s, s, SWP_NOZORDER | SWP_NOACTIVATE);
}

void wh_measure_chip(node *n, float *w, float *h) {
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	SIZE t = wh_text_extent(dc, wstr_c(&n->title), n->title.n);
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	float x = CHIP_PAD + wh_pt(n, t.cx);
	x += n->chip_removable ? CHIP_REMOVE_GAP + CHIP_REMOVE + CHIP_REMOVE_END : CHIP_PAD;
	if (lead_glyph(n)) x += CHIP_GLYPH * wh_font_scale() + (t.cx > 0 ? CHIP_GAP : 0);
	*w = x;
	*h = wh_maxf(wh_pt(n, tm.tmHeight) + 12, CHIP_HEIGHT);
}

typedef struct {
	rgba fill, stroke, text;
	bool stroke_on;
} chip_look;

// look_of is a chip's colors as it is (hot, pressed, selected, disabled):
// Fluent's standard button's, or its accent button's while a filter chip
// is selected; the app's Color tints the accent, its Background fills it.
static chip_look look_of(node *n) {
	const palette *p = &H.theme;
	bool hot = n->hot || (n->chip_peer && n->chip_peer->hot), pressed = n->pressed;
	bool sel = selected_chip(n);
	chip_look l;
	if (high_contrast()) {
		rgba face = sys_rgba(COLOR_BTNFACE), text = sys_rgba(COLOR_BTNTEXT);
		l = (chip_look){face, text, text, true};
		if (n->disabled) {
			l.stroke = l.text = sys_rgba(COLOR_GRAYTEXT);
		} else if (sel || hot || pressed) {
			l.fill = sys_rgba(COLOR_HIGHLIGHT), l.text = sys_rgba(COLOR_HIGHLIGHTTEXT);
			l.stroke = sel ? l.fill : text;
		}
		return l;
	}
	rgba under = wh_backdrop(n), c;
	if (sel) {
		rgba accent = p->accent;
		if (wh_resolve(n->color, &c) && c.a > 0) accent = rgba_over(c, under);
		l = (chip_look){accent, accent, on_color(accent), false};
		if (n->disabled) l.fill = rgba_over(rgba_alpha(p->disabled, 0.6f), under), l.text = p->bg;
		else if (pressed) l.fill = rgba_over(rgba_alpha(accent, 0.8f), under), l.text = rgba_over(rgba_alpha(l.text, 0.8f), l.fill);
		else if (hot) l.fill = rgba_over(rgba_alpha(accent, 0.9f), under);
	} else {
		l = (chip_look){p->control, p->control_stroke, p->label, true};
		if (n->disabled) l.fill = rgba_over(H.dark ? (rgba){255, 255, 255, 0x0B} : (rgba){0xF9, 0xF9, 0xF9, 0x4D}, under), l.text = p->disabled;
		else if (pressed) l.fill = p->control_pressed, l.text = p->secondary;
		else if (hot) l.fill = p->control_hot;
	}
	if (wh_resolve(n->background, &c) && c.a > 0) l.fill = rgba_over(c, under), l.stroke_on = false, l.text = on_color(l.fill);
	return l;
}

// wh_paint_chip draws a chip: its pill (g), or its glyph and title (dc).
void wh_paint_chip(node *n, HDC dc, gfx g, RECT r) {
	float k = wh_scale_of(n);
	float w = (float)(r.right - r.left), ht = (float)(r.bottom - r.top);
	chip_look l = look_of(n);
	if (g) {
		float rad = ht / 2;
		g_fill_round_rect(g, rgba_argb(l.fill), 0, 0, w, ht, rad);
		if (l.stroke_on) g_stroke_round_rect(g, rgba_argb(l.stroke), wh_maxf(k, 1), k / 2, k / 2, w - k, ht - k, rad - k / 2);
		if (n->focused && !n->no_focus_ring && wh_keyboard_cues(n->hwnd))
			g_stroke_round_rect(g, rgba_argb(high_contrast() ? sys_rgba(COLOR_WINDOWTEXT) : H.theme.label), 2 * k, k, k, w - 2 * k, ht - 2 * k, rad - k);
		return;
	}
	bool rtl = wh_is_rtl(n);
	uint16_t glyph = lead_glyph(n);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(l.text));
	HFONT font = wh_node_font(n);
	HGDIOBJ old = SelectObject(dc, font);
	SIZE ts = wh_text_extent(dc, wstr_c(&n->title), n->title.n);
	int pad = wh_px(n, CHIP_PAD), end = n->chip_removable ? wh_px(n, CHIP_REMOVE_GAP + CHIP_REMOVE + CHIP_REMOVE_END) : pad;
	int gw = glyph ? wh_px(n, CHIP_GLYPH * wh_font_scale()) : 0, gap = glyph && ts.cx > 0 ? wh_px(n, CHIP_GAP) : 0;
	// From the start: the glyph, then the title (the other way right to left).
	int x = r.left + pad;
	int avail = (r.right - r.left) - pad - end - gw - gap;
	int tw = wh_mini(ts.cx, wh_maxi(avail, 0));
	int gx = rtl ? r.right - pad - gw : x;
	int tx = rtl ? r.right - pad - gw - gap - tw : x + gw + gap;
	if (glyph) {
		wchar_t ch = (wchar_t)glyph;
		SelectObject(dc, wh_icon_font(n, CHIP_GLYPH * wh_font_scale()));
		RECT gr = {gx, r.top, gx + gw, r.bottom};
		DrawTextW(dc, &ch, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, font);
	}
	if (tw > 0) {
		RECT tr = {tx, r.top, tx + tw, r.bottom};
		DrawTextW(dc, wstr_c(&n->title), n->title.n, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | wh_start_side(rtl) | wh_text_direction(rtl));
	}
	SelectObject(dc, old);
}

// wh_paint_chip_remove draws a chip's remove button over the chip (drawn
// behind it): its ✕ (as Segoe Fluent's Cancel, two strokes: a glyph this
// small comes out of ClearType in color fringes), on a subtle circle under
// the pointer.
void wh_paint_chip_remove(node *rm, HDC dc, gfx g, RECT r) {
	node *n = rm->chip_peer;
	if (!n || !g) return;
	(void)dc;
	float k = wh_scale_of(n);
	float w = (float)(r.right - r.left), ht = (float)(r.bottom - r.top);
	chip_look l = look_of(n);
	if ((rm->hot || rm->pressed) && !n->disabled) {
		// The title's color, faint: on the control fill as a subtle fill
		// is, and on the accent too.
		bool strong = selected_chip(n) || high_contrast();
		rgba c = rgba_alpha(l.text, strong ? (rm->pressed ? 0.12f : 0.18f) : (rm->pressed ? 0.06f : 0.09f));
		float d = wh_minf(w, ht);
		g_fill_ellipse(g, rgba_argb(c), (w - d) / 2, (ht - d) / 2, d, d);
	}
	float half = CHIP_REMOVE_GLYPH * wh_font_scale() * k / 2 * 0.8f, cx = w / 2, cy = ht / 2, line = wh_maxf(k, 1);
	g_draw_line(g, rgba_argb(l.text), line, cx - half, cy - half, cx + half, cy + half);
	g_draw_line(g, rgba_argb(l.text), line, cx + half, cy - half, cx - half, cy + half);
}

// ---- Disclosure ----

void wh_set_disclosure_prop(node *n, uint16_t p, const ak_value *v) {
	switch (p) {
	case AK_PROP_DISCLOSURE_TITLE:
		wstr_set(&n->title, v->s);
		break;
	case AK_PROP_DISCLOSURE_EXPANDED: {
		bool was = n->on;
		n->on = v->i != 0;
		if (was != n->on) {
			start_turn(n);
			wh_uia_expand_changed(n, was);
		}
		break;
	}
	}
	wh_redraw(n);
}

// start_turn turns the chevron to the disclosure's state: at once while
// it doesn't show or motion is reduced.
static void start_turn(node *n) {
	double target = n->on ? 1 : 0;
	if (!n->hwnd || !IsWindowVisible(n->hwnd) || H.env.env.reduce_motion) {
		n->phase = target;
		if (n->hwnd) KillTimer(n->hwnd, TIMER_CHEVRON);
		return;
	}
	SetTimer(n->hwnd, TIMER_CHEVRON, 16, NULL);
}

void wh_measure_disclosure(node *n, float cw, float *w, float *h) {
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	SIZE t = wh_text_extent(dc, wstr_c(&n->title), n->title.n);
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	*w = DISC_PAD + wh_pt(n, t.cx) + DISC_CHEVRON_GAP + DISC_CHEVRON + DISC_CHEVRON_END;
	if (cw == cw) *w = cw;
	*h = wh_maxf(wh_pt(n, tm.tmHeight) + 2 * 12, DISC_MIN_HEIGHT);
}

// wh_paint_disclosure draws an Expander's header: its card and chevron
// (g), or its title (dc).
void wh_paint_disclosure(node *n, HDC dc, gfx g, RECT r) {
	float k = wh_scale_of(n);
	float w = (float)(r.right - r.left), ht = (float)(r.bottom - r.top);
	bool rtl = wh_is_rtl(n), hc = high_contrast();
	const palette *p = &H.theme;
	rgba under = wh_backdrop(n), text = p->label, fill = rgba_over(p->card, under), stroke = p->card_stroke, c;
	if (hc) {
		fill = sys_rgba(COLOR_BTNFACE), stroke = text = sys_rgba(COLOR_BTNTEXT);
		if (n->disabled) stroke = text = sys_rgba(COLOR_GRAYTEXT);
		else if (n->hot || n->pressed) fill = sys_rgba(COLOR_HIGHLIGHT), text = sys_rgba(COLOR_HIGHLIGHTTEXT);
	} else if (n->disabled) {
		text = p->disabled;
	}
	if (wh_resolve(n->color, &c) && c.a > 0 && !hc) text = c;
	float cs = DISC_CHEVRON * k, cx = rtl ? DISC_CHEVRON_END * k : w - DISC_CHEVRON_END * k - cs, cy = (ht - cs) / 2;
	if (g) {
		float rad = n->corner_radius > 0 ? n->corner_radius * k : 4 * k;
		if (!wh_resolve(n->background, &c) || c.a == 0) {
			g_fill_round_rect(g, rgba_argb(fill), 0, 0, w, ht, rad);
			g_stroke_round_rect(g, rgba_argb(stroke), wh_maxf(k, 1), k / 2, k / 2, w - k, ht - k, rad);
		}
		if (!hc && !n->disabled && (n->hot || n->pressed)) {
			// The chevron's button's subtle fill (SubtleFillColorSecondary,
			// Tertiary pressed), as WinUI's header shows under the pointer.
			g_fill_round_rect(g, rgba_argb(n->pressed ? p->subtle_pressed : p->subtle_hot), cx, cy, cs, cs, 4 * k);
		}
		// The chevron: down collapsed, up expanded, turning between.
		GpPen *pen = NULL;
		GdipCreatePen1(rgba_argb(text), wh_maxf(k, 1), UnitPixel, &pen);
		GdipSetPenLineCap197819(pen, LineCapRound, LineCapRound, DashCapFlat);
		GdipSetPenLineJoin(pen, LineJoinRound);
		double a = n->phase * M_PI, ca = cos(a), sa = sin(a);
		float mx = cx + cs / 2, my = cy + cs / 2 + (n->pressed ? k : 0);
		float pts[3][2] = {{-4.5f, -2.25f}, {0, 2.25f}, {4.5f, -2.25f}};
		GpPointF q[3];
		for (int i = 0; i < 3; i++) {
			float x = pts[i][0] * k, y = pts[i][1] * k;
			q[i] = (GpPointF){mx + (float)(x * ca - y * sa), my + (float)(x * sa + y * ca)};
		}
		GdipDrawLines(g, pen, q, 3);
		GdipDeletePen(pen);
		if (n->focused && !n->no_focus_ring && wh_keyboard_cues(n->hwnd))
			g_stroke_round_rect(g, rgba_argb(hc ? sys_rgba(COLOR_WINDOWTEXT) : p->label), 2 * k, k, k, w - 2 * k, ht - 2 * k, rad);
		return;
	}
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(text));
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	int pad = wh_px(n, DISC_PAD), room = wh_px(n, DISC_CHEVRON_GAP + DISC_CHEVRON + DISC_CHEVRON_END);
	RECT tr = {r.left + pad, r.top, r.right - room, r.bottom};
	if (rtl) tr.left = r.left + room, tr.right = r.right - pad;
	DrawTextW(dc, wstr_c(&n->title), n->title.n, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | wh_start_side(rtl) | wh_text_direction(rtl));
	SelectObject(dc, old);
}

// ---- Automation ----

// wh_own_dump adds what a chip and a disclosure are to the view tree's
// line (automation.c).
void wh_own_dump(node *n, ak_buf *b) {
	if (n->kind == AK_KIND_CHIP) {
		if (n->chip_filter) ak_buf_printf(b, n->on ? " selected" : " unselected");
		if (n->chip_removable) ak_buf_printf(b, " removable");
		if (n->icon) ak_buf_printf(b, " icon=%s", n->icon);
	} else if (n->kind == AK_KIND_DISCLOSURE) {
		ak_buf_printf(b, n->on ? " expanded" : " collapsed");
	}
}

// wh_chip_remove_point is a removable chip's remove button's middle on
// the screen, the chip found by its title (automation's remove step).
bool wh_chip_remove_point(const wchar_t *title, HWND *hwnd, LPARAM *lp) {
	uint32_t i = 0;
	void *v;
	node *found = NULL;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (n->kind == AK_KIND_CHIP && n->chip_peer && n->chip_peer->hwnd && n->title.n && !wcscmp(n->title.p, title) && wh_on_screen(n) &&
			(!found || n->id > found->id))
			found = n;
	}
	if (!found) return false;
	RECT r;
	GetClientRect(found->chip_peer->hwnd, &r);
	*hwnd = found->chip_peer->hwnd;
	*lp = MAKELPARAM(r.right / 2, r.bottom / 2);
	return true;
}
