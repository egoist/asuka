// controls.c: the common controls drawn as Windows 11 draws its own
// (WinUI's Fluent styles) through custom draw, keeping their keyboard,
// focus and accessibility: BUTTON for buttons (Filled is the accent
// button), a BUTTON check box drawn as a toggle switch, a trackbar for
// sliders; fields (EDITs in a frame the host draws), the stepper and the
// segmented control.
#include "host.h"
#include "parts.h"

// button_glyph is the icon and title a host-made button shows.
static uint16_t button_glyph(node *n, HWND ctl, wchar_t *title, int cap) {
	title[0] = 0;
	switch (n->kind) {
	case AK_KIND_STEPPER:
		return ctl == n->ctl ? 0xE949 : 0xE948;  // Remove, Add
	case KIND_HEADER:
		return wh_header_button(n, ctl, wh_is_rtl(n), title, cap);
	case KIND_COMMAND_BAR:
		return wh_command_bar_button(n, ctl, wh_is_rtl(n), title, cap);
	case AK_KIND_BUTTON:
		wcsncpy(title, wstr_c(&n->title), (size_t)cap - 1);
		title[cap - 1] = 0;
		return wh_directed_glyph(n->icon, wh_is_rtl(n));
	case KIND_ALERT_WINDOW:
		GetWindowTextW(ctl, title, cap);  // an alert's dialog's button
		return 0;
	case AK_KIND_SNACKBAR:
		if (wh_snack_is_close(n, ctl)) return 0xE711;  // Cancel: an ×
		wcsncpy(title, wstr_c(&n->title), (size_t)cap - 1);
		title[cap - 1] = 0;
		return 0;
	}
	return 0;
}

LRESULT wh_draw_button(node *n, NMCUSTOMDRAW *cd) {
	if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
	HDC dc = cd->hdc;
	RECT r = cd->rc;
	HWND ctl = cd->hdr.hwndFrom;
	wh_paint_behind(ctl, dc);
	gfx g = g_new(dc);
	float k = wh_scale_of(n);
	float w = (float)(r.right - r.left), ht = (float)(r.bottom - r.top);
	bool hot = cd->uItemState & CDIS_HOT, pressed = cd->uItemState & CDIS_SELECTED;
	bool disabled = (cd->uItemState & CDIS_DISABLED) || n->disabled;
	bool focus = (cd->uItemState & CDIS_FOCUS) && (cd->uItemState & CDIS_SHOWKEYBOARDCUES) && !n->no_focus_ring;
	const palette *p = &H.theme;
	float radius = 4 * k;
	if (n->corner_radius > 0 && n->kind == AK_KIND_BUTTON) radius = wh_minf(n->corner_radius * k, wh_minf(w / 2, ht / 2));

	if (n->kind == AK_KIND_COLOR_PICKER) {
		rgba c;
		if (!wh_resolve(n->color_value, &c)) c = p->accent;
		g_fill_round_rect(g, rgba_argb(hot ? p->control_hot : p->control), 0, 0, w, ht, radius);
		g_stroke_round_rect(g, rgba_argb(p->control_stroke), wh_maxf(k, 1), 0, 0, w, ht, radius);
		float in = 4 * k;
		g_fill_round_rect(g, rgba_argb(c), in, in, w - 2 * in, ht - 2 * in, radius / 2);
		g_close(g);
		return CDRF_SKIPDEFAULT;
	}

	int variant = AK_BUTTON_VARIANT_BORDERED;
	if (n->kind == AK_KIND_BUTTON) {
		variant = n->variant;
		// No Liquid Glass on Windows: the gray and the accent buttons.
		if (variant == AK_BUTTON_VARIANT_GLASS) variant = AK_BUTTON_VARIANT_GRAY;
		else if (variant == AK_BUTTON_VARIANT_GLASS_PROMINENT) variant = AK_BUTTON_VARIANT_FILLED;
	}
	if (n->kind == KIND_HEADER || (n->kind == AK_KIND_SNACKBAR && wh_snack_is_close(n, ctl))) variant = AK_BUTTON_VARIANT_PLAIN;
	if (n->kind == KIND_COMMAND_BAR) variant = wh_command_bar_is_primary(n, ctl) ? AK_BUTTON_VARIANT_FILLED : AK_BUTTON_VARIANT_PLAIN;
	if (n->kind == KIND_ALERT_WINDOW && wh_alert_dialog_default_button(n, ctl)) variant = AK_BUTTON_VARIANT_FILLED;
	rgba text = p->label, fill = {0};
	bool stroke = false;
	switch (variant) {
	case AK_BUTTON_VARIANT_FILLED:
		fill = p->accent, text = p->on_accent;
		if (hot) fill = rgba_over(p->accent_hot, wh_backdrop(n));
		if (pressed) {
			fill = rgba_over(p->accent_pressed, wh_backdrop(n));
			text = rgba_over(rgba_alpha(text, 0.8f), fill);
		}
		if (disabled) fill = rgba_over(rgba_alpha(p->disabled, 0.6f), wh_backdrop(n)), text = p->bg;
		break;
	case AK_BUTTON_VARIANT_PLAIN:
		text = p->accent_text;
		if (n->kind == KIND_HEADER || n->kind == KIND_COMMAND_BAR || n->kind == AK_KIND_SNACKBAR) text = p->label;
		if (hot) fill = p->subtle_hot;
		if (pressed) {
			fill = p->subtle_pressed;
			text = rgba_over(rgba_alpha(text, 0.8f), wh_backdrop(n));
		}
		break;
	case AK_BUTTON_VARIANT_TINTED:
		fill = rgba_alpha(p->accent, 0.15f), text = p->accent_text;
		if (hot) fill = rgba_alpha(p->accent, 0.2f);
		if (pressed) fill = rgba_alpha(p->accent, 0.1f);
		break;
	default:  // Gray, Bordered: Fluent's standard button
		fill = p->control, stroke = true;
		if (hot) fill = p->control_hot;
		if (pressed) fill = p->control_pressed, text = p->secondary;
	}
	if (n->kind == AK_KIND_BUTTON) {
		rgba c;
		if (wh_resolve(n->background, &c)) fill = c, stroke = false;
		if (wh_resolve(n->color, &c)) text = c;
	}
	if (disabled && variant != AK_BUTTON_VARIANT_FILLED) text = p->disabled;
	if (fill.a > 0) g_fill_round_rect(g, rgba_argb(fill), 0, 0, w, ht, radius);
	if (stroke) g_stroke_round_rect(g, rgba_argb(p->control_stroke), wh_maxf(k, 1), 0, 0, w, ht, radius);
	if (focus) wh_focus_ring(n, g, w, ht, radius);
	g_close(g);  // before GDI draws the title

	wchar_t title[512];
	wchar_t glyph = button_glyph(n, ctl, title, (int)WH_LEN(title));
	rgba under = fill.a < 255 ? rgba_over(fill, wh_backdrop(n)) : fill;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(rgba_over(text, under)));
	HFONT font = n->kind == AK_KIND_BUTTON ? wh_node_font(n) : wh_body_font(n);
	int tl = (int)wcslen(title);
	HGDIOBJ old = SelectObject(dc, font);
	SIZE ts = wh_text_extent(dc, title, tl);
	int gw = 0;
	if (glyph) {
		SelectObject(dc, wh_icon_font(n, 16 * wh_font_scale()));
		gw = wh_text_extent(dc, &glyph, 1).cx;
	}
	int gap = gw > 0 && ts.cx > 0 ? wh_px(n, 8) : 0;
	// The icon before the title: on its right, right to left.
	bool rtl = wh_is_rtl(n);
	int left = r.left + ((r.right - r.left) - (gw + gap + ts.cx)) / 2;
	int gx = left, x = left + gw + gap;
	if (rtl) gx = left + ts.cx + gap, x = left;
	if (glyph) {
		RECT gr = {gx, r.top, gx + gw, r.bottom};
		DrawTextW(dc, &glyph, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, font);
	}
	if (ts.cx > 0) {
		int y = r.top + ((r.bottom - r.top) - ts.cy) / 2;
		RECT tr = {x, y, wh_mini(x + ts.cx, r.right - wh_px(n, 4)), y + ts.cy};
		if (x < r.left + wh_px(n, 4)) tr.left = r.left + wh_px(n, 4);
		DrawTextW(dc, title, tl, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | wh_start_side(rtl));
	}
	SelectObject(dc, old);
	return CDRF_SKIPDEFAULT;
}

// draw_checkbox draws Fluent's check box: a 20 by 20 square, rounded 4,
// outlined and empty off, filled with the accent and a checkmark on.
static void draw_checkbox(node *n, gfx g, RECT r, bool on, bool hot, bool pressed, bool disabled) {
	float k = wh_scale_of(n);
	const palette *p = &H.theme;
	float s = 20 * k, x = 0, y = ((float)(r.bottom - r.top) - s) / 2;
	if (wh_is_rtl(n)) x = (float)(r.right - r.left) - s;
	rgba accent = p->accent, c;
	if (wh_resolve(n->color, &c)) accent = c;
	if (on) {
		rgba fill = accent;
		if (disabled) fill = p->disabled;
		else if (pressed) fill = rgba_over(p->accent_pressed, wh_backdrop(n));
		else if (hot) fill = rgba_over(p->accent_hot, wh_backdrop(n));
		g_fill_round_rect(g, rgba_argb(fill), x, y, s, s, 4 * k);
		GpPen *pen = NULL;
		GdipCreatePen1(rgba_argb(p->on_accent), 1.5f * k, UnitPixel, &pen);
		GdipSetPenLineCap197819(pen, LineCapRound, LineCapRound, DashCapFlat);
		GpPath *path = g_path();
		GdipAddPathLine(path, x + 5 * k, y + 10.5f * k, x + 8.5f * k, y + 14 * k);
		GdipAddPathLine(path, x + 8.5f * k, y + 14 * k, x + 15 * k, y + 6.5f * k);
		GdipDrawPath(g, pen, path);
		GdipDeletePath(path);
		GdipDeletePen(pen);
		return;
	}
	rgba strong = disabled ? p->disabled : p->secondary;
	if (hot || pressed) g_fill_round_rect(g, rgba_argb(p->subtle_hot), x, y, s, s, 4 * k);
	g_stroke_round_rect(g, rgba_argb(strong), wh_maxf(k, 1), x, y, s, s, 4 * k);
}

// wh_draw_toggle draws a check box as Fluent's toggle switch: a 40 by 20
// pill, outlined and empty off, filled with the accent on, its knob
// sliding to the end (the left, right to left).
LRESULT wh_draw_toggle(node *n, NMCUSTOMDRAW *cd) {
	if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
	HDC dc = cd->hdc;
	RECT r = cd->rc;
	wh_paint_behind(cd->hdr.hwndFrom, dc);
	gfx g = g_new(dc);
	float k = wh_scale_of(n);
	const palette *p = &H.theme;
	bool hot = cd->uItemState & CDIS_HOT, pressed = cd->uItemState & CDIS_SELECTED;
	bool disabled = (cd->uItemState & CDIS_DISABLED) || n->disabled;
	bool focus = (cd->uItemState & CDIS_FOCUS) && (cd->uItemState & CDIS_SHOWKEYBOARDCUES) && !n->no_focus_ring;
	bool on = SendMessageW(cd->hdr.hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED;
	if (n->checkbox) {
		draw_checkbox(n, g, r, on, hot, pressed, disabled);
		if (focus) {
			float s = 20 * k, x = wh_is_rtl(n) ? (float)(r.right - r.left) - s : 0;
			g_stroke_round_rect(g, rgba_argb(p->label), 2 * k, x - 3 * k, ((float)(r.bottom - r.top) - s) / 2 - 3 * k, s + 6 * k, s + 6 * k, 7 * k);
		}
		g_close(g);
		return CDRF_SKIPDEFAULT;
	}
	float tw = 40 * k, th = 20 * k, x = 0, y = ((float)(r.bottom - r.top) - th) / 2;
	bool rtl = wh_is_rtl(n);
	if (rtl) x = (float)(r.right - r.left) - tw;  // the switch at the view's start
	float knob = hot ? 14 * k : 12 * k;
	// The knob at its start and its end, swapped right to left.
	float at_start = x + th / 2 - knob / 2, at_end = x + tw - th / 2 - knob / 2;
	float knob_off = rtl ? at_end : at_start, knob_on = rtl ? at_start : at_end;
	rgba strong = disabled ? p->disabled : p->secondary;
	rgba accent = p->accent, c;
	if (wh_resolve(n->color, &c)) accent = c;
	if (on) {
		rgba fill = accent;
		if (hot) fill = rgba_over(p->accent_hot, wh_backdrop(n));
		if (pressed) fill = rgba_over(p->accent_pressed, wh_backdrop(n));
		if (disabled) fill = p->disabled;
		g_fill_round_rect(g, rgba_argb(fill), x, y, tw, th, th / 2);
		g_fill_ellipse(g, rgba_argb(p->on_accent), knob_on, y + th / 2 - knob / 2, knob, knob);
	} else {
		if (hot) g_fill_round_rect(g, rgba_argb(p->subtle_hot), x, y, tw, th, th / 2);
		g_stroke_round_rect(g, rgba_argb(strong), wh_maxf(k, 1), x, y, tw, th, th / 2);
		g_fill_ellipse(g, rgba_argb(strong), knob_off, y + th / 2 - knob / 2, knob, knob);
	}
	if (focus) g_stroke_round_rect(g, rgba_argb(p->label), 2 * k, x - 3 * k, y - 3 * k, tw + 6 * k, th + 6 * k, th / 2 + 3 * k);
	g_close(g);
	return CDRF_SKIPDEFAULT;
}

// wh_draw_slider draws a trackbar as Fluent's slider: a 4-point track
// filled with the accent up to a round thumb, its accent dot growing under
// the mouse.
LRESULT wh_draw_slider(node *n, NMCUSTOMDRAW *cd) {
	if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
	if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
	HWND ctl = cd->hdr.hwndFrom;
	HDC dc = cd->hdc;
	float k = wh_scale_of(n);
	const palette *p = &H.theme;
	RECT thumb, client;
	SendMessageW(ctl, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
	GetClientRect(ctl, &client);
	float cx = (float)(thumb.left + thumb.right) / 2, cy = (float)client.bottom / 2;
	rgba accent = p->accent, c;
	if (wh_resolve(n->color, &c)) accent = c;
	if (n->disabled) accent = p->disabled;
	if (cd->dwItemSpec == TBCD_CHANNEL) {
		wh_paint_behind(ctl, dc);
		gfx g = g_new(dc);
		float th = 4 * k, rr = 10 * k, x0 = rr, x1 = (float)client.right - rr;
		g_fill_round_rect(g, rgba_argb(p->track), x0, cy - th / 2, x1 - x0, th, th / 2);
		if (wh_is_rtl(n)) {
			if (cx < x1) g_fill_round_rect(g, rgba_argb(accent), cx, cy - th / 2, x1 - cx, th, th / 2);
		} else if (cx > x0) {
			g_fill_round_rect(g, rgba_argb(accent), x0, cy - th / 2, cx - x0, th, th / 2);
		}
		g_close(g);
	} else if (cd->dwItemSpec == TBCD_THUMB) {
		gfx g = g_new(dc);
		float d = 20 * k;
		g_fill_ellipse(g, rgba_argb(p->knob), cx - d / 2, cy - d / 2, d, d);
		g_draw_arc(g, rgba_argb(p->control_stroke), wh_maxf(k, 1), 0, cx - d / 2 + k / 2, cy - d / 2 + k / 2, d - k, d - k, 0, 360);
		float inner = 10 * k;
		if (cd->uItemState & CDIS_SELECTED) inner = 8 * k;
		else if (cd->uItemState & CDIS_HOT) inner = 12 * k;
		g_fill_ellipse(g, rgba_argb(accent), cx - inner / 2, cy - inner / 2, inner, inner);
		if ((cd->uItemState & CDIS_FOCUS) && wh_keyboard_cues(ctl) && !n->no_focus_ring)
			g_draw_arc(g, rgba_argb(p->label), 2 * k, 0, cx - d / 2 - 2 * k, cy - d / 2 - 2 * k, d + 4 * k, d + 4 * k, 0, 360);
		g_close(g);
	}
	return CDRF_SKIPDEFAULT;
}

// ---- Text fields: an EDIT without a border in a view that draws
// Fluent's text box around it ----

void wh_make_field(node *n) {
	n->hwnd = wh_make_view(0);
	wh_make_edit(n);
}

// edit_align is an EDIT's style for a field's alignment in a direction.
static DWORD edit_align(const node *n, bool rtl) {
	if (n->text_align == AK_TEXT_ALIGN_CENTER) return ES_CENTER;
	if (wh_aligned_right(n->text_align, rtl)) return ES_RIGHT;
	return ES_LEFT;
}

// edit_text is the field's text with Windows's line breaks (free it).
static wchar_t *edit_text(const node *n, int *len) {
	const wchar_t *s = wstr_c(&n->text);
	int k = 0, extra = 0;
	if (n->multiline && !n->rich_text)
		for (int i = 0; i < n->text.n; i++)
			if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) extra++;
	wchar_t *out = wh_alloc(((size_t)n->text.n + (size_t)extra + 1) * sizeof *out);
	for (int i = 0; i < n->text.n; i++) {
		if (extra && s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out[k++] = '\r';
		out[k++] = s[i];
	}
	out[k] = 0;
	if (len) *len = k;
	return out;
}

static void set_edit_text(node *n) {
	wchar_t *t = edit_text(n, NULL);
	n->setting_text = true;
	SetWindowTextW(n->ctl, t);
	n->setting_text = false;
	free(t);
}

static bool moves_selection(UINT m, WPARAM w) {
	switch (m) {
	case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
	case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO: case EM_REPLACESEL:
		return true;
	case WM_MOUSEMOVE:
		return (w & MK_LBUTTON) != 0;
	}
	return false;
}

static UINT placeholder_side(const node *n) {
	bool rtl = wh_is_rtl(n);
	UINT f = wh_text_direction(rtl);
	if (n->text_align == AK_TEXT_ALIGN_CENTER) f |= DT_CENTER;
	else if (wh_aligned_right(n->text_align, rtl)) f |= DT_RIGHT;
	return f;
}

// edit_message is an EDIT's subclass: return submits a single-line field
// (and doesn't beep), and Ctrl+A selects all.
static LRESULT edit_message(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
	node *n = wh_node_of(hwnd);
	if (!n) return wh_def_subclass(hwnd, m, w, l);
	if (m == WM_PASTE && wh_paste_content(n)) {
		wh_check_selection(n);
		return 0;
	}
	bool check = moves_selection(m, w);
	LRESULT r;
	switch (m) {
	case WM_GETDLGCODE:
		r = wh_def_subclass(hwnd, m, w, l);
		if (l && ((MSG *)l)->message == WM_KEYDOWN && ((MSG *)l)->wParam == VK_RETURN) r |= DLGC_WANTALLKEYS;
		return r;
	case WM_KEYDOWN:
		if (w == VK_RETURN && !n->multiline) {
			wh_emit_kind(n, AK_EVENT_SUBMIT);
			return 0;
		}
		if (w == 'A' && wh_key_down(VK_CONTROL)) {
			SendMessageW(hwnd, EM_SETSEL, 0, -1);
			wh_check_selection(n);
			return 0;
		}
		break;
	case WM_CHAR:
		if (((w == '\r' || w == '\n') && !n->multiline) || w == 1) return 0;  // Ctrl+A's character beeps
		break;
	case WM_PAINT:
		if (n->multiline && n->placeholder.n && !n->text.n) {
			// A multiline EDIT has no cue banner: the placeholder is drawn
			// over it while it's empty.
			r = wh_def_subclass(hwnd, m, w, l);
			HDC dc = GetDC(hwnd);
			HGDIOBJ old = SelectObject(dc, wh_node_font(n));
			SetBkMode(dc, TRANSPARENT);
			rgba bg = n->plain ? wh_backdrop(n) : H.theme.field_bg;
			SetTextColor(dc, rgba_ref(rgba_over(H.theme.secondary, bg)));
			RECT cr;
			GetClientRect(hwnd, &cr);
			DrawTextW(dc, n->placeholder.p, n->placeholder.n, &cr, DT_NOPREFIX | DT_WORDBREAK | placeholder_side(n));
			SelectObject(dc, old);
			ReleaseDC(hwnd, dc);
			return r;
		}
		break;
	}
	r = wh_def_subclass(hwnd, m, w, l);
	if (check) wh_check_selection(n);
	return r;
}

void wh_make_edit(node *n) {
	DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
	if (n->multiline) style |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
	else style |= ES_AUTOHSCROLL;
	if (n->keyboard == AK_KEYBOARD_NUMBER) style |= ES_NUMBER;
	n->edit_rtl = wh_is_rtl(n);
	n->edit_side = edit_align(n, n->edit_rtl);
	style |= n->edit_side;
	DWORD ex = n->edit_rtl ? WS_EX_RTLREADING : 0;
	if (n->rich_text) style |= ES_NOHIDESEL;
	HWND old = n->ctl;
	n->ctl = CreateWindowExW(ex, wh_rich_field_class(n), L"", style, 0, 0, 0, 0, n->hwnd, (HMENU)1, GetModuleHandleW(NULL), NULL);
	SendMessageW(n->ctl, WM_SETFONT, (WPARAM)wh_node_font(n), 0);
	SendMessageW(n->ctl, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
	wh_bind(n, n->ctl);
	if (old) {
		wh_clear_accessible_name(old);  // n->ctl names the replacement
		wh_forget_tree(old);
		DestroyWindow(old);
		wh_bind(n, n->ctl);
	}
	set_edit_text(n);
	if (n->placeholder.n) SendMessageW(n->ctl, EM_SETCUEBANNER, TRUE, (LPARAM)n->placeholder.p);
	if (n->secure) SendMessageW(n->ctl, EM_SETPASSWORDCHAR, 0x25CF, 0);
	if (n->disabled) EnableWindow(n->ctl, FALSE);
	wh_subclass(n->ctl, edit_message);
	wh_setup_rich_edit(n);
	if (n->a11y_label.n) wh_set_accessible_name(n->ctl, n->a11y_label.p);
	wh_layout_field(n);
}

// wh_align_field follows a field's alignment and direction. An EDIT takes
// both as it's made, so a new one makes it again, with what it had.
void wh_align_field(node *n) {
	if (!n->ctl) return;
	bool rtl = wh_is_rtl(n);
	if (rtl == n->edit_rtl && edit_align(n, rtl) == n->edit_side) return;
	bool focused = GetFocus() == n->ctl;
	DWORD s = 0, e = 0;
	SendMessageW(n->ctl, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
	wh_make_edit(n);
	wh_theme_control(n);
	SendMessageW(n->ctl, EM_SETSEL, s, e);
	if (focused) SetFocus(n->ctl);
}

// ---- Selections: an EDIT counts UTF-16 units as the core does, but a
// multiline one's lines end in \r\n where the core's end in \n ----

static int edit_offset(const node *n, int u) {
	if (!n->multiline || n->rich_text) return u;  // a rich edit's line breaks are one \r
	int lines = 0;
	for (int i = 0; i < n->text.n && i < u; i++)
		if (n->text.p[i] == '\n') lines++;
	return u + lines;
}

static int core_offset(const node *n, int e) {
	if (!n->multiline || n->rich_text) return e;
	int units = 0, edit = 0;
	for (int i = 0; i < n->text.n; i++) {
		int step = n->text.p[i] == '\n' ? 2 : 1;
		if (edit + step > e) break;
		edit += step;
		units++;
	}
	return units;
}

static void set_selection(node *n, const uint8_t *b, size_t len) {
	if (len < 8 || !n->ctl) return;
	uint32_t s = wh_u32(b), e = wh_u32(b + 4), total = (uint32_t)n->text.n;
	if (s > total) s = total;
	if (e > total) e = total;
	n->selection[0] = s, n->selection[1] = e;
	SendMessageW(n->ctl, EM_SETSEL, (WPARAM)edit_offset(n, (int)s), (LPARAM)edit_offset(n, (int)e));
}

static void report_selection(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (!n) return;
	n->sel_queued = false;
	if (!n->ctl) return;
	DWORD es = 0, ee = 0;
	SendMessageW(n->ctl, EM_GETSEL, (WPARAM)&es, (LPARAM)&ee);
	uint32_t sel[2] = {(uint32_t)core_offset(n, (int)es), (uint32_t)core_offset(n, (int)ee)};
	if (sel[0] == n->selection[0] && sel[1] == n->selection[1]) return;
	n->selection[0] = sel[0], n->selection[1] = sel[1];
	uint8_t out[8];
	memcpy(out, &sel[0], 4);
	memcpy(out + 4, &sel[1], 4);
	wh_emit_bytes(n, AK_EVENT_SELECTION_CHANGED, out, 8);
}

// wh_check_selection reports the field's selection after what may have
// moved it (a key, a click, a paste), once its text change is reported,
// unless it's where the core put it.
void wh_check_selection(node *n) {
	if (n->sel_queued || !node_listening(n, AK_EVENT_SELECTION_CHANGED)) return;
	n->sel_queued = true;
	wh_post(report_selection, (void *)(uintptr_t)n->id);
}

// wh_insert_text puts text in place of the selection as typing does:
// undoably, and reported as typing is.
void wh_insert_text(node *n, const char *s, size_t len) {
	if (!n->ctl) return;
	wchar_t *w = wh_wide(s, (int)len);
	if (n->multiline && !n->rich_text) {
		// \n to \r\n, \r\n kept.
		size_t wl = wcslen(w), k = 0;
		wchar_t *o = wh_alloc((wl * 2 + 1) * sizeof *o);
		for (size_t i = 0; i < wl; i++) {
			if (w[i] == '\n' && (i == 0 || w[i - 1] != '\r')) o[k++] = '\r';
			o[k++] = w[i];
		}
		free(w);
		w = o;
	}
	SendMessageW(n->ctl, EM_REPLACESEL, TRUE, (LPARAM)w);
	free(w);
	wh_check_selection(n);
}

void wh_set_field(node *n, uint16_t p, const ak_value *v) {
	switch (p) {
	case AK_PROP_TEXT_FIELD_VALUE:
		wstr_set(&n->text, v->s);
		set_edit_text(n);
		break;
	case AK_PROP_TEXT_FIELD_PLACEHOLDER:
		wstr_set(&n->placeholder, v->s);
		SendMessageW(n->ctl, EM_SETCUEBANNER, TRUE, (LPARAM)wstr_c(&n->placeholder));
		break;
	case AK_PROP_TEXT_FIELD_SECURE:
		n->secure = v->i != 0;
		SendMessageW(n->ctl, EM_SETPASSWORDCHAR, n->secure ? 0x25CF : 0, 0);
		InvalidateRect(n->ctl, NULL, FALSE);
		break;
	case AK_PROP_TEXT_FIELD_SELECTION:
		set_selection(n, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_TEXT_FIELD_PASTE_TYPES:
		wh_free_cstrings(n->paste_types, n->npaste_types);
		n->npaste_types = wh_decode_cstrings(&n->paste_types, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_TEXT_FIELD_RICH:
		wh_set_rich_text(n, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_TEXT_FIELD_PLAIN:
		n->plain = v->i != 0;
		wh_layout_field(n);
		InvalidateRect(n->hwnd, NULL, FALSE);
		break;
	case AK_PROP_TEXT_FIELD_MULTILINE:
		if ((v->i != 0) != n->multiline) {
			n->multiline = v->i != 0;
			wh_make_edit(n);
			wh_theme_control(n);
		}
		break;
	case AK_PROP_TEXT_FIELD_KEYBOARD: {
		n->keyboard = (int32_t)v->i;
		LONG_PTR style = GetWindowLongPtrW(n->ctl, GWL_STYLE) & ~(LONG_PTR)ES_NUMBER;
		if (n->keyboard == AK_KEYBOARD_NUMBER) style |= ES_NUMBER;
		SetWindowLongPtrW(n->ctl, GWL_STYLE, style);
		break;
	}
	}
}

// wh_field_command is a field's EDIT's WM_COMMAND notification.
void wh_field_command(node *n, HWND ctl, UINT code) {
	switch (code) {
	case EN_CHANGE: {
		if (n->setting_text) return;
		int len = GetWindowTextLengthW(ctl);
		wchar_t *s = wh_alloc(((size_t)len + 1) * sizeof *s);
		if (n->rich_text) len = wh_visible_text(n, s, len + 1);  // not a link's hidden field instruction
		else len = GetWindowTextW(ctl, s, len + 1);
		// \r\n (an EDIT's) and \r (a rich edit's) to \n.
		int k = 0;
		for (int i = 0; i < len; i++) {
			if (s[i] == '\r' && n->multiline && !n->rich_text && i + 1 < len && s[i + 1] == '\n') continue;
			s[k++] = s[i] == '\r' && n->rich_text ? '\n' : s[i];
		}
		wstr_setw(&n->text, s, k);
		free(s);
		wh_emit_wstr(n, AK_EVENT_TEXT_CHANGED, n->text.p, n->text.n);
		wh_rich_edited(n);
		break;
	}
	case EN_SETFOCUS:
		n->focused = true;
		wh_redraw(n);
		wh_emit_kind(n, AK_EVENT_FOCUS);
		break;
	case EN_KILLFOCUS:
		n->focused = false;
		wh_redraw(n);
		wh_emit_kind(n, AK_EVENT_BLUR);
		break;
	}
}

// wh_layout_field places a view's control: a field's EDIT inside its frame
// (padded as Fluent's text box: 10 points across; one line centered), the
// other controls filling their view.
void wh_layout_field(node *n) {
	if (!n->ctl || !n->hwnd) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	switch (n->kind) {
	case AK_KIND_TEXT_FIELD: {
		r = wh_inset_rect(n, r);  // inside its padding (ContentInset)
		HDC dc = H.measure_dc;
		HGDIOBJ old = SelectObject(dc, wh_node_font(n));
		TEXTMETRICW tm;
		GetTextMetricsW(dc, &tm);
		SelectObject(dc, old);
		int x = n->plain ? 0 : wh_px(n, 10), y = n->plain ? 0 : wh_px(n, 6);
		int w = wh_maxi(r.right - r.left - 2 * x, 1);
		if (n->multiline) {
			SetWindowPos(n->ctl, NULL, r.left + x, r.top + y, w, wh_maxi(r.bottom - r.top - 2 * y, 1), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
		} else {
			int lh = tm.tmHeight;
			SetWindowPos(n->ctl, NULL, r.left + x, r.top + (r.bottom - r.top - lh) / 2, w, lh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
		}
		break;
	}
	case AK_KIND_WHEEL:
	case AK_KIND_PICKER:
		wh_layout_combo(n, r);  // the box fills the view; the list drops below it
		break;
	case AK_KIND_DATE_PICKER:
		wh_layout_date(n, r);
		break;
	case AK_KIND_SLIDER:
		SendMessageW(n->ctl, TBM_SETTHUMBLENGTH, (WPARAM)wh_px(n, 20), 0);
		SetWindowPos(n->ctl, NULL, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
		break;
	default:
		SetWindowPos(n->ctl, NULL, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	}
}

// wh_paint_field draws Fluent's text box: a rounded, filled frame with a
// darker bottom edge, the accent's under the focused field.
void wh_paint_field(node *n, gfx g, float w, float ht) {
	if (n->plain) return;  // the text alone, on what's behind it
	float k = wh_scale_of(n);
	const palette *p = &H.theme;
	bool ring = n->focused && !n->no_focus_ring;  // the app draws its own focus with NoFocusRing
	rgba bg = ring ? p->field_focus_bg : p->field_bg;
	float r = n->corner_radius > 0 ? n->corner_radius * k : 4 * k;
	g_fill_round_rect(g, rgba_argb(bg), 0, 0, w, ht, r);
	g_stroke_round_rect(g, rgba_argb(p->control_stroke), wh_maxf(k, 1), 0, 0, w, ht, r);
	GpPath *path = g_path();
	g_path_round_rect(path, 0, 0, w, ht, r);
	g_clip_path(g, path);
	rgba edge = rgba_alpha(p->secondary, 0.7f);
	float line = wh_maxf(k, 1);
	if (ring) edge = p->accent, line = 2 * k;
	g_fill_rect(g, rgba_argb(edge), 0, ht - line, w, line);
	g_reset_clip(g);
	GdipDeletePath(path);
}

// ---- Stepper: two standard buttons, minus and plus ----

void wh_layout_stepper(node *n) {
	RECT r;
	GetClientRect(n->hwnd, &r);
	int gap = wh_px(n, 4), bw = (r.right - gap) / 2;
	// Minus, then plus: from the right, right to left.
	HWND first = n->ctl, second = n->ctl2;
	if (wh_is_rtl(n)) first = n->ctl2, second = n->ctl;
	SetWindowPos(first, NULL, 0, 0, bw, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	SetWindowPos(second, NULL, bw + gap, 0, r.right - bw - gap, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

// wh_name_stepper names a Stepper's buttons: they draw a glyph, their
// text is what a screen reader says.
void wh_name_stepper(node *n) {
	if (n->ctl) SetWindowTextW(n->ctl, wh_word("decrease"));
	if (n->ctl2) SetWindowTextW(n->ctl2, wh_word("increase"));
}

// ---- Segmented: the host's own control, drawn as the Windows Community
// Toolkit's Segmented: a track whose selected segment is raised, with an
// accent pill under its title ----

int wh_segment_at(node *n, int x) {
	if (!n->nitems) return -1;
	RECT r;
	GetClientRect(n->hwnd, &r);
	int i = x * (int)n->nitems / wh_maxi(r.right, 1);
	i = wh_maxi(wh_mini(i, (int)n->nitems - 1), 0);
	if (wh_is_rtl(n)) i = (int)n->nitems - 1 - i;  // the first on the right
	return i;
}

void wh_select_segment(node *n, int i) {
	if (i < 0 || i >= (int)n->nitems || i == n->selected) return;
	n->selected = i;
	wh_redraw(n);
	wh_emit_int(n, AK_EVENT_SELECTED, i);
}

// wh_paint_segmented draws the track and the raised segment (g), or the
// titles (dc).
void wh_paint_segmented(node *n, HDC dc, gfx g, RECT r) {
	float k = wh_scale_of(n);
	const palette *p = &H.theme;
	float w = (float)(r.right - r.left), ht = (float)(r.bottom - r.top);
	int count = (int)n->nitems;
	float sw = w / (float)wh_maxi(count, 1);
	bool rtl = wh_is_rtl(n);
#define PLACE(i) ((float)(rtl ? count - 1 - (i) : (i)) * sw)
	if (g) {
		rgba track = H.dark ? (rgba){255, 255, 255, 0x0F} : (rgba){0, 0, 0, 0x0F};
		g_fill_round_rect(g, rgba_argb(track), 0, 0, w, ht, 5 * k);
		g_stroke_round_rect(g, rgba_argb(p->stroke), wh_maxf(k, 1), 0, 0, w, ht, 5 * k);
		if (n->selected >= 0 && n->selected < count) {
			float x = PLACE(n->selected), in = 3 * k;
			rgba sel = H.dark ? rgba_hex(0x3A3A3A) : p->control;
			g_fill_round_rect(g, rgba_argb(sel), x + in, in, sw - 2 * in, ht - 2 * in, 3 * k);
			g_stroke_round_rect(g, rgba_argb(p->stroke), wh_maxf(k, 1), x + in, in, sw - 2 * in, ht - 2 * in, 3 * k);
			rgba accent = p->accent, c;
			if (wh_resolve(n->color, &c)) accent = c;
			float pw = 16 * k, ph = 3 * k;
			g_fill_round_rect(g, rgba_argb(accent), x + sw / 2 - pw / 2, ht - in - ph - 1 * k, pw, ph, ph / 2);
		}
		return;
	}
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	SetBkMode(dc, TRANSPARENT);
	for (int i = 0; i < count; i++) {
		float x = PLACE(i);
		rgba color = i == n->selected ? p->label : p->secondary;
		if (n->disabled) color = p->disabled;
		SetTextColor(dc, rgba_ref(rgba_over(color, wh_backdrop(n))));
		RECT tr = {(LONG)x + wh_px(n, 6), r.top, (LONG)(x + sw) - wh_px(n, 6), r.bottom - wh_px(n, 2)};
		DrawTextW(dc, n->items[i], -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | wh_text_direction(rtl));
	}
	SelectObject(dc, old);
#undef PLACE
}
