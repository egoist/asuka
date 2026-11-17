// combo.c: Wheel and Picker: a drop-down list combo box (CBS_DROPDOWNLIST)
// drawn as WinUI 3's ComboBox, keeping the combo box's keyboard (arrows,
// F4, Alt+Down, typing to search, Escape), focus and UI Automation.
//
// The closed box is the combo box's own WM_PAINT (it's subclassed): the
// control fill and stroke of Fluent's standard button, its bottom edge
// the elevation border's, 4 corners; the chosen item, or a Picker's label
// in the secondary text color while none is chosen; the chevron (E70D in
// Segoe Fluent Icons) at the end; hover and pressed (its list open)
// fills, the disabled colors, and the focus visual from the keyboard.
//
// The list is owner-drawn (CBS_OWNERDRAWFIXED, CBS_HASSTRINGS: the items
// keep their strings): WM_DRAWITEM reaches the box's parent, the host's
// view, which draws each item as WinUI's ComboBoxItem on the flyout
// surface: 5 by 2 in from the list's sides, 11 by 5 and 7 of padding
// around its text, the hovered item's subtle fill, the chosen item's fill
// and its accent pill at the start. The list's window (ComboLBox) is
// subclassed too: 4 more at its top and bottom, its classic frame painted
// over with the surface, and DWM's rounded corners and border on Windows
// 11 (a hairline of its own on Windows 10).
//
// A contrast theme draws both with the system's colors, as WinUI does:
// ButtonFace and ButtonText, Highlight under the pointer; the list in
// Window and WindowText, Highlight for the item under the pointer and the
// chosen item's pill. Right to left, the chevron is on the left, the text
// right-aligned and the pill on the right. Everything is in the pixels of
// the view's window's DPI.
#include "host.h"
#include "win.h"

// WinUI's ComboBox metrics, in points.
#define PAD_START 12  // the text's padding at the start (ComboBoxPadding: 12,5,0,7)
#define PAD_TOP 5
#define PAD_BOTTOM 7
#define GLYPH_COL 38  // the chevron's column at the end
#define GLYPH_END 14  // the chevron's margin at the end
#define GLYPH_SIZE 12
#define MIN_WIDTH 64  // ComboBoxThemeMinWidth
#define MIN_HEIGHT 32
#define LINE 20        // Body's line height
#define ITEM_MAR_X 5   // a ComboBoxItem's margin: 5,2,5,2
#define ITEM_MAR_Y 2
#define ITEM_PAD_X 11  // and its padding: 11,5,11,7
#define LIST_PAD_Y 4   // ComboBoxDropdownContentMargin: 0,4,0,4
#define PILL_WIDTH 3
#define PILL_HEIGHT 16

// A Wheel's or Picker's combo box.
#define COMBO_STYLE (CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP)

#define LIST_SUBCLASS 2  // the list's subclass's ID

static const wchar_t chevron_down = 0xE70D;

static bool high_contrast(void) {
	HIGHCONTRASTW hc = {sizeof hc};
	return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

static rgba sys_rgba(int i) {
	COLORREF c = GetSysColor(i);
	return (rgba){GetRValue(c), GetGValue(c), GetBValue(c), 255};
}

static DWORD ex_style(const node *n) { return wh_is_rtl(n) ? WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR : 0; }

static void fill(HDC dc, RECT r, rgba c) { FillRect(dc, &r, wh_brush(c)); }

// ---- The list's colors: WinUI's flyout surface, light or dark (the
// host's menus', menu.c), or Window and WindowText in a contrast theme ----

rgba wh_combo_list_bg(void) {
	if (high_contrast()) return sys_rgba(COLOR_WINDOW);
	return H.dark ? rgba_hex(0x2B2B2B) : rgba_hex(0xF9F9F9);
}

rgba wh_combo_list_text(void) {
	if (high_contrast()) return sys_rgba(COLOR_WINDOWTEXT);
	return H.theme.label;
}

static rgba list_border(void) {
	if (high_contrast()) return sys_rgba(COLOR_WINDOWTEXT);
	return H.dark ? rgba_hex(0x3E3E3E) : rgba_hex(0xE5E5E5);
}

// ---- Making ----

static LRESULT combo_message(HWND hwnd, UINT m, WPARAM w, LPARAM l);
static LRESULT CALLBACK list_message(HWND hwnd, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data);

static HWND combo_list(HWND cb) {
	COMBOBOXINFO ci = {sizeof ci};
	return SendMessageW(cb, CB_GETCOMBOBOXINFO, 0, (LPARAM)&ci) ? ci.hwndList : NULL;
}

// setup follows a combo box just made: its painting, its pointer, and its
// list's window.
static void setup(node *n) {
	if (!n->ctl) return;
	n->combo_hot = false;
	wh_subclass(n->ctl, combo_message);
	n->combo_list = combo_list(n->ctl);
	if (n->combo_list) {
		SetWindowSubclass(n->combo_list, list_message, LIST_SUBCLASS, (DWORD_PTR)n->id);
		wh_style_combo_list(n);
		// Its frame again, with the padding (WM_NCCALCSIZE).
		SetWindowPos(n->combo_list, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

void wh_make_combo(node *n) {
	wh_make_control(n, L"COMBOBOX", COMBO_STYLE, ex_style(n));
	setup(n);
}

// select_picker selects a Picker's chosen item in its combo box; with none
// (out of range) the box is empty, and shows its label.
static void select_picker(node *n) {
	int i = n->selected;
	if (i < 0 || (uint32_t)i >= n->nitems) i = -1;
	SendMessageW(n->ctl, CB_SETCURSEL, (WPARAM)i, 0);
	InvalidateRect(n->ctl, NULL, FALSE);
}

// label_picker shows a Picker's label in its combo box while none is
// chosen (the box draws it, in the secondary text color, where the combo
// box's cue banner would be), and names it for screen readers (its own
// text is the chosen item).
static void label_picker(node *n) {
	SendMessageW(n->ctl, CB_SETCUEBANNER, 0, (LPARAM)wstr_c(&n->picker_label));
	wh_set_accessible_name(n->ctl, wstr_c(&n->picker_label));
	InvalidateRect(n->ctl, NULL, FALSE);
}

static void add_items(node *n) {
	SendMessageW(n->ctl, CB_RESETCONTENT, 0, 0);
	for (uint32_t i = 0; i < n->nitems; i++) SendMessageW(n->ctl, CB_ADDSTRING, 0, (LPARAM)n->items[i]);
}

// wh_remake_combo makes a combo box again in its direction: a combo box
// takes its extended styles as it's made.
void wh_remake_combo(node *n) {
	HWND old = n->ctl;
	if (!old) return;
	wh_make_control_in(n, L"COMBOBOX", COMBO_STYLE, ex_style(n));
	wh_forget_tree(old);
	DestroyWindow(old);
	wh_bind(n, n->ctl);
	setup(n);
	add_items(n);
	if (n->kind == AK_KIND_PICKER) {
		select_picker(n);
		label_picker(n);
	} else {
		SendMessageW(n->ctl, CB_SETCURSEL, (WPARAM)n->selected, 0);
	}
	if (n->disabled) EnableWindow(n->ctl, FALSE);
	wh_theme_control(n);
	wh_layout_field(n);
}

// wh_set_combo_prop is a Wheel's or Picker's prop (views.c has decoded the
// items into n->items).
void wh_set_combo_prop(node *n, uint16_t p, const ak_value *v) {
	if (!n->ctl) return;
	if (n->kind == AK_KIND_WHEEL) {
		switch (p) {
		case AK_PROP_WHEEL_ITEMS:
			add_items(n);
			SendMessageW(n->ctl, CB_SETCURSEL, (WPARAM)n->selected, 0);
			break;
		case AK_PROP_WHEEL_SELECTED:
			n->selected = (int)v->i;
			SendMessageW(n->ctl, CB_SETCURSEL, (WPARAM)v->i, 0);
			break;
		}
		InvalidateRect(n->ctl, NULL, FALSE);
		return;
	}
	switch (p) {
	case AK_PROP_PICKER_ITEMS:
		add_items(n);
		select_picker(n);
		break;
	case AK_PROP_PICKER_SELECTED:
		n->selected = (int)v->i;
		select_picker(n);
		break;
	case AK_PROP_PICKER_LABEL:
		wstr_set(&n->picker_label, v->s);
		label_picker(n);
		break;
	}
}

// wh_style_combo_list gives a combo box's list the theme's scroll bar and,
// on Windows 11, DWM's rounded corners and border, as WinUI's popups have.
void wh_style_combo_list(node *n) {
	HWND list = n->combo_list;
	if (!list) return;
	wh_allow_dark(list, H.dark);
	SetWindowTheme(list, H.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
	if (wh_windows11()) {
		DWORD pref = 2;  // DWMWCP_ROUND
		DwmSetWindowAttribute(list, 33, &pref, sizeof pref);  // DWMWA_WINDOW_CORNER_PREFERENCE
		COLORREF c = rgba_ref(list_border());
		DwmSetWindowAttribute(list, 34, &c, sizeof c);  // DWMWA_BORDER_COLOR
	}
}

// ---- Measuring and placing ----

// line_height is a combo box's line of text, in pixels: its font's, at
// least Body's 20 points.
static int line_height(node *n) {
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	return wh_maxi(tm.tmHeight, wh_px(n, LINE * wh_font_scale()));
}

// item_height is a list item's height, in pixels: its line, padding and
// margins (36 points in Body).
static int item_height(node *n) { return line_height(n) + wh_px(n, 2 * ITEM_MAR_Y + PAD_TOP + PAD_BOTTOM); }

// wh_layout_combo fills the view with its combo box: the field's height
// (CB_SETITEMHEIGHT -1) is what makes the box as tall as its view, once
// the combo box's own frame is known; the list's items are WinUI's.
void wh_layout_combo(node *n, RECT r) {
	HWND ctl = n->ctl;
	if (!ctl) return;
	int w = r.right - r.left, h = r.bottom - r.top;
	SendMessageW(ctl, CB_SETITEMHEIGHT, 0, item_height(n));
	int field = wh_maxi(h - wh_px(n, 6), 1);
	SendMessageW(ctl, CB_SETITEMHEIGHT, (WPARAM)-1, field);
	// The height past the box is the list's, where the combo box takes it.
	SetWindowPos(ctl, NULL, 0, 0, w, h + wh_px(n, 300), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	RECT wr;
	GetWindowRect(ctl, &wr);
	int got = wr.bottom - wr.top;
	if (got > 0 && got != h) SendMessageW(ctl, CB_SETITEMHEIGHT, (WPARAM)-1, wh_maxi(field + h - got, 1));
	InvalidateRect(ctl, NULL, FALSE);
}

// wh_measure_combo is a Wheel's or Picker's size, in points: its widest
// item (a Picker's label too) with the padding and the chevron's column,
// at least 64 across, and its line with the padding, at least 32 down.
void wh_measure_combo(node *n, float *w, float *h) {
	int widest = 0;
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	for (uint32_t i = 0; i < n->nitems; i++) widest = wh_maxi(widest, wh_text_extent(dc, n->items[i], (int)wcslen(n->items[i])).cx);
	if (n->kind == AK_KIND_PICKER && n->picker_label.n) widest = wh_maxi(widest, wh_text_extent(dc, n->picker_label.p, n->picker_label.n).cx);
	SelectObject(dc, old);
	*w = wh_maxf(PAD_START + wh_pt(n, widest) + GLYPH_COL, MIN_WIDTH);
	*h = wh_maxf(PAD_TOP + wh_pt(n, line_height(n)) + PAD_BOTTOM, MIN_HEIGHT);
}

// ---- The closed box ----

// combo_text is what the closed box shows: the chosen item, or a Picker's
// label (placeholder) while none is.
static const wchar_t *combo_text(node *n, bool *placeholder) {
	*placeholder = false;
	if (n->selected >= 0 && (uint32_t)n->selected < n->nitems) return n->items[n->selected];
	if (n->kind == AK_KIND_PICKER) {
		*placeholder = true;
		return wstr_c(&n->picker_label);
	}
	return L"";
}

typedef struct {
	bool hot, pressed, disabled, focus;
} combo_state;

typedef struct {
	rgba fill, stroke, bottom, text, placeholder, glyph, focus;
} combo_look;

static bool same_rgba(rgba a, rgba b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

// look is WinUI's ComboBox's colors: ControlFillColorDefault, its
// Secondary under the pointer and Tertiary pressed; the elevation border
// (ControlStrokeColorDefault, its bottom ControlStrokeColorSecondary);
// TextFillColorPrimary, the label and the chevron TextFillColorSecondary;
// disabled, ControlFillColorDisabled and TextFillColorDisabled.
static combo_look look(node *n, combo_state s) {
	combo_look l;
	if (high_contrast()) {
		rgba text = sys_rgba(COLOR_BTNTEXT);
		l = (combo_look){sys_rgba(COLOR_BTNFACE), text, text, text, text, text, text};
		if (s.disabled) {
			rgba gray = sys_rgba(COLOR_GRAYTEXT);
			l.stroke = l.bottom = l.text = l.placeholder = l.glyph = gray;
		} else if (s.hot || s.pressed) {
			rgba hl = sys_rgba(COLOR_HIGHLIGHT), on = sys_rgba(COLOR_HIGHLIGHTTEXT);
			l.fill = l.stroke = l.bottom = hl;
			l.text = l.placeholder = l.glyph = on;
		}
		return l;
	}
	const palette *p = &H.theme;
	l = (combo_look){.fill = p->control, .stroke = p->control_stroke, .text = p->label, .placeholder = p->secondary, .glyph = p->secondary, .focus = p->label};
	l.bottom = H.dark ? rgba_over((rgba){255, 255, 255, 0x08}, p->control_stroke) : rgba_over((rgba){0, 0, 0, 0x18}, p->control_stroke);
	if (s.disabled) {
		rgba dis = H.dark ? (rgba){255, 255, 255, 0x0B} : (rgba){0xF9, 0xF9, 0xF9, 0x4D};
		l.fill = rgba_over(dis, wh_backdrop(n));
		l.bottom = l.stroke;
		l.text = l.placeholder = l.glyph = p->disabled;
	} else if (s.pressed) {
		l.fill = p->control_pressed, l.bottom = l.stroke;  // no elevation, pressed
	} else if (s.hot) {
		l.fill = p->control_hot;
	}
	return l;
}

// paint_combo draws the closed box into dc, over what's behind it.
static void paint_combo(node *n, HWND ctl, HDC dc) {
	RECT r;
	GetClientRect(ctl, &r);
	if (r.right <= 0 || r.bottom <= 0) return;
	wh_paint_behind(ctl, dc);
	float k = wh_scale_of(n);
	float w = (float)r.right, ht = (float)r.bottom;
	combo_state st = {
		.hot = n->combo_hot,
		.pressed = SendMessageW(ctl, CB_GETDROPPEDSTATE, 0, 0) != 0,
		.disabled = n->disabled || !IsWindowEnabled(ctl),
		.focus = GetFocus() == ctl && wh_keyboard_cues(ctl) && !n->no_focus_ring,
	};
	combo_look lk = look(n, st);
	float radius = wh_minf(4 * k, wh_minf(w / 2, ht / 2));
	if (n->corner_radius > 0) radius = wh_minf(n->corner_radius * k, wh_minf(w / 2, ht / 2));
	float line = wh_maxf(k, 1);
	gfx g = g_new(dc);
	g_fill_round_rect(g, rgba_argb(lk.fill), 0, 0, w, ht, radius);
	g_stroke_round_rect(g, rgba_argb(lk.stroke), line, 0, 0, w, ht, radius);
	if (!same_rgba(lk.bottom, lk.stroke)) {
		GpPath *path = g_path();
		g_path_round_rect(path, 0, 0, w, ht, radius);
		g_clip_path(g, path);
		g_fill_rect(g, rgba_argb(lk.bottom), 0, ht - line, w, line);
		g_reset_clip(g);
		GdipDeletePath(path);
	}
	if (st.focus) g_stroke_round_rect(g, rgba_argb(lk.focus), 2 * k, 0, 0, w, ht, wh_maxf(radius, 4 * k));
	g_close(g);  // before GDI draws the text

	bool rtl = wh_is_rtl(n);
	SetBkMode(dc, TRANSPARENT);
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	int start = wh_px(n, PAD_START), end = wh_px(n, GLYPH_COL);
	RECT tr = {r.left + start, r.top + wh_px(n, PAD_TOP), r.right - end, r.bottom - wh_px(n, PAD_BOTTOM)};
	if (rtl) tr.left = r.left + end, tr.right = r.right - start;
	if (tr.bottom <= tr.top) tr.top = r.top, tr.bottom = r.bottom;
	bool placeholder;
	const wchar_t *text = combo_text(n, &placeholder);
	if (text[0] && tr.right > tr.left) {
		SetTextColor(dc, rgba_ref(rgba_over(placeholder ? lk.placeholder : lk.text, lk.fill)));
		DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | wh_start_side(rtl));
	}
	// The chevron, in its column at the end; pressed, a point lower.
	SelectObject(dc, wh_icon_font(n, GLYPH_SIZE));
	int gs = wh_px(n, GLYPH_SIZE);
	int gx = rtl ? r.left + wh_px(n, GLYPH_END) : r.right - wh_px(n, GLYPH_END) - gs;
	RECT gr = {gx, r.top, gx + gs, r.bottom};
	if (st.pressed) gr.top += wh_px(n, 1), gr.bottom += wh_px(n, 1);
	SetTextColor(dc, rgba_ref(rgba_over(lk.glyph, lk.fill)));
	DrawTextW(dc, &chevron_down, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old);
}

// combo_message is a Wheel's or Picker's combo box's subclass: it paints
// itself, and repaints as its pointer, focus and list change.
static LRESULT combo_message(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
	node *n = wh_node_of(hwnd);
	if (!n || n->ctl != hwnd) return wh_def_subclass(hwnd, m, w, l);
	switch (m) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT r;
		GetClientRect(hwnd, &r);
		if (r.right > 0 && r.bottom > 0) {
			HDC mem = NULL;
			HPAINTBUFFER pb = BeginBufferedPaint(dc, &r, BPBF_COMPATIBLEBITMAP, NULL, &mem);
			if (pb && mem) {
				paint_combo(n, hwnd, mem);
				EndBufferedPaint(pb, TRUE);
			} else {
				paint_combo(n, hwnd, dc);
			}
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_PRINTCLIENT:
		paint_combo(n, hwnd, (HDC)w);
		return 0;
	case WM_ERASEBKGND:
		return 1;  // WM_PAINT covers it all
	case WM_CTLCOLORLISTBOX: {
		// Its list asks it (the list's owner), which would ask its parent.
		rgba bg = wh_combo_list_bg();
		SetBkColor((HDC)w, rgba_ref(bg));
		SetTextColor((HDC)w, rgba_ref(wh_combo_list_text()));
		return (LRESULT)wh_brush(bg);
	}
	case WM_MOUSEMOVE:
		if (!n->combo_hot) {
			n->combo_hot = true;
			TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
			TrackMouseEvent(&t);
			InvalidateRect(hwnd, NULL, FALSE);
		}
		break;
	case WM_MOUSELEAVE:
		if (n->combo_hot) {
			n->combo_hot = false;
			InvalidateRect(hwnd, NULL, FALSE);
		}
		break;
	case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_UPDATEUISTATE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_CAPTURECHANGED: {
		LRESULT r = wh_def_subclass(hwnd, m, w, l);
		InvalidateRect(hwnd, NULL, FALSE);
		return r;
	}
	}
	return wh_def_subclass(hwnd, m, w, l);
}

// ---- The list ----

// wh_combo_draw_item draws a combo box's list item (WM_DRAWITEM): its
// backplate, under the pointer or the keyboard (ODS_SELECTED) and on the
// chosen item, the chosen item's accent pill at the start, and its text.
// The closed box's field draws itself (combo_message): asked to draw it,
// as a combo box does when its selection changes, the box is repainted.
bool wh_combo_draw_item(node *n, DRAWITEMSTRUCT *d) {
	if (d->itemState & ODS_COMBOBOXEDIT) {
		if (n->ctl) InvalidateRect(n->ctl, NULL, FALSE);
		return true;
	}
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	rgba bg = wh_combo_list_bg();
	fill(dc, r, bg);
	int i = (int)d->itemID;
	if (i < 0 || (uint32_t)i >= n->nitems) return true;
	float k = wh_scale_of(n);
	bool hc = high_contrast();
	bool highlighted = (d->itemState & ODS_SELECTED) != 0;
	bool chosen = i == n->selected;
	rgba text = wh_combo_list_text(), plate = {0}, pill;
	if (hc) {
		if (highlighted) plate = sys_rgba(COLOR_HIGHLIGHT), text = sys_rgba(COLOR_HIGHLIGHTTEXT);
		pill = highlighted ? sys_rgba(COLOR_HIGHLIGHTTEXT) : sys_rgba(COLOR_HIGHLIGHT);
	} else {
		if (highlighted || chosen) plate = rgba_over(H.theme.subtle_hot, bg);  // SubtleFillColorSecondary
		pill = H.theme.accent;  // AccentFillColorDefault
		rgba c;
		if (wh_resolve(n->color, &c) && c.a > 0) pill = rgba_over(c, bg);
	}
	// A mirrored DC's left is its right: GDI follows it, GDI+ doesn't.
	bool mirrored = (GetLayout(dc) & LAYOUT_RTL) != 0;
	bool right = wh_is_rtl(n) != mirrored;  // the start on the DC's right
	RECT pr = {r.left + wh_px(n, ITEM_MAR_X), r.top + wh_px(n, ITEM_MAR_Y), r.right - wh_px(n, ITEM_MAR_X), r.bottom - wh_px(n, ITEM_MAR_Y)};
	rgba under = bg;
	int pw = wh_px(n, PILL_WIDTH), ph = wh_px(n, PILL_HEIGHT);
	int px = right ? pr.right - pw : pr.left;
	int py = pr.top + ((pr.bottom - pr.top) - ph) / 2;
	if (mirrored) {
		HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
		if (plate.a > 0) {
			HBRUSH b = CreateSolidBrush(rgba_ref(plate));
			HGDIOBJ ob = SelectObject(dc, b);
			int rad = wh_px(n, 4);
			// Without a pen, RoundRect's right and bottom edges are a pixel short.
			RoundRect(dc, pr.left, pr.top, pr.right + 1, pr.bottom + 1, 2 * rad, 2 * rad);
			SelectObject(dc, ob);
			DeleteObject(b);
		}
		if (chosen) {
			HBRUSH b = CreateSolidBrush(rgba_ref(pill));
			HGDIOBJ ob = SelectObject(dc, b);
			int rad = wh_maxi(pw / 2, 1);
			RoundRect(dc, px, py, px + pw + 1, py + ph + 1, 2 * rad, 2 * rad);
			SelectObject(dc, ob);
			DeleteObject(b);
		}
		SelectObject(dc, op);
	} else if (plate.a > 0 || chosen) {
		gfx g = g_new(dc);
		if (plate.a > 0) g_fill_round_rect(g, rgba_argb(plate), (float)pr.left, (float)pr.top, (float)(pr.right - pr.left), (float)(pr.bottom - pr.top), 4 * k);
		if (chosen) g_fill_round_rect(g, rgba_argb(pill), (float)px, (float)py, (float)pw, (float)ph, (float)pw / 2);
		g_close(g);
	}
	if (plate.a > 0) under = plate;
	int pad = wh_px(n, ITEM_PAD_X);
	RECT tr = {pr.left + pad, pr.top + wh_px(n, PAD_TOP), pr.right - pad, pr.bottom - wh_px(n, PAD_BOTTOM)};
	if (tr.bottom <= tr.top) tr.top = pr.top, tr.bottom = pr.bottom;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(rgba_over(text, under)));
	DrawTextW(dc, n->items[i], -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (right ? DT_RIGHT : DT_LEFT) | wh_text_direction(wh_is_rtl(n)));
	SelectObject(dc, old);
	return true;
}

// paint_list_frame paints a list window's frame (its classic border) and
// padding in the surface, the scroll bar left as it is: on Windows 11 DWM
// draws the border around it, rounded; before, its hairline is drawn here.
static void paint_list_frame(HWND hwnd, HDC dc) {
	RECT wr, cr;
	GetWindowRect(hwnd, &wr);
	GetClientRect(hwnd, &cr);
	int w = wr.right - wr.left, ht = wr.bottom - wr.top;
	POINT p = {0, 0};
	ClientToScreen(hwnd, &p);
	if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) p.x -= cr.right;  // a mirrored window's client origin is its top right
	RECT c = {p.x - wr.left, p.y - wr.top, 0, 0};
	c.right = c.left + cr.right, c.bottom = c.top + cr.bottom;
	rgba bg = wh_combo_list_bg();
	const int b = 1;  // WS_BORDER's
	fill(dc, (RECT){0, 0, w, c.top}, bg);
	fill(dc, (RECT){0, c.bottom, w, ht}, bg);
	fill(dc, (RECT){0, c.top, b, c.bottom}, bg);
	fill(dc, (RECT){w - b, c.top, w, c.bottom}, bg);
	if (!wh_windows11()) {
		rgba edge = list_border();
		fill(dc, (RECT){0, 0, w, b}, edge);
		fill(dc, (RECT){0, ht - b, w, ht}, edge);
		fill(dc, (RECT){0, 0, b, ht}, edge);
		fill(dc, (RECT){w - b, 0, w, ht}, edge);
	}
}

// list_message is a combo box's list window's subclass: WinUI's 4 points
// above and below the items (its client area inset, its window made that
// much taller as the combo box drops it), its frame painted over with the
// surface, and its background.
static LRESULT CALLBACK list_message(HWND hwnd, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
	if (m == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, list_message, id);
		node *n = wh_node((uint32_t)data);
		if (n && n->combo_list == hwnd) n->combo_list = NULL;
		return DefSubclassProc(hwnd, m, w, l);
	}
	node *n = wh_node((uint32_t)data);
	if (!n || n->combo_list != hwnd) return DefSubclassProc(hwnd, m, w, l);
	switch (m) {
	case WM_NCCALCSIZE: {
		LRESULT r = DefSubclassProc(hwnd, m, w, l);
		RECT *rc = (RECT *)l;  // the client area (NCCALCSIZE_PARAMS's first)
		int pad = wh_px(n, LIST_PAD_Y);
		if (rc->bottom - rc->top > 2 * pad) rc->top += pad, rc->bottom -= pad;
		return r;
	}
	case WM_WINDOWPOSCHANGING: {
		// Sized while hidden is the combo box dropping it (or placing it
		// for later): the padding goes on what it asks for, upward when it
		// opens above the box.
		WINDOWPOS *pos = (WINDOWPOS *)l;
		if (!(pos->flags & SWP_NOSIZE) && pos->cy > 0 && !IsWindowVisible(hwnd)) {
			int extra = 2 * wh_px(n, LIST_PAD_Y);
			if (!(pos->flags & SWP_NOMOVE) && n->ctl) {
				RECT cr;
				GetWindowRect(n->ctl, &cr);
				if (pos->y + pos->cy <= cr.top + 1) pos->y -= extra;
			}
			pos->cy += extra;
		}
		break;
	}
	case WM_NCPAINT: {
		LRESULT r = DefSubclassProc(hwnd, m, w, l);  // the scroll bar
		HDC dc = GetWindowDC(hwnd);
		paint_list_frame(hwnd, dc);
		ReleaseDC(hwnd, dc);
		return r;
	}
	case WM_PRINT: {  // its opening animation (AnimateWindow)
		LRESULT r = DefSubclassProc(hwnd, m, w, l);
		if (l & PRF_NONCLIENT) paint_list_frame(hwnd, (HDC)w);
		return r;
	}
	case WM_ERASEBKGND: {
		RECT cr;
		GetClientRect(hwnd, &cr);
		fill((HDC)w, cr, wh_combo_list_bg());
		return 1;
	}
	}
	return DefSubclassProc(hwnd, m, w, l);
}

// ---- Choosing ----

// chose takes the combo box's selection as the user's choice, once: a
// click in the list sends CBN_CLOSEUP, then CBN_SELCHANGE.
static void chose(node *n, HWND ctl) {
	int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
	InvalidateRect(ctl, NULL, FALSE);
	if (n->combo_list) InvalidateRect(n->combo_list, NULL, FALSE);  // the pill moves
	if (i == n->selected) return;
	n->selected = i;
	wh_emit_int(n, AK_EVENT_SELECTED, i);
}

// wh_combo_command follows a combo box's notifications: the user's choice,
// its list opening and closing.
void wh_combo_command(node *n, HWND ctl, UINT code) {
	switch (code) {
	case CBN_SELCHANGE:
		chose(n, ctl);
		break;
	case CBN_DROPDOWN:
		wh_style_combo_list(n);  // as the theme is now (a contrast theme)
		InvalidateRect(ctl, NULL, FALSE);
		break;
	case CBN_CLOSEUP: {
		// The list closed (Escape puts back what was chosen): the box shows
		// the combo box's choice, and the app has it.
		int cur = n->selected;
		if (cur < 0 || (uint32_t)cur >= n->nitems) cur = -1;
		if ((int)SendMessageW(ctl, CB_GETCURSEL, 0, 0) != cur) chose(n, ctl);
		InvalidateRect(ctl, NULL, FALSE);
		break;
	}
	}
}

// wh_combo_tap is automation's tap on a Wheel's or Picker's item: chosen
// as the list would (its selection, then CBN_SELCHANGE).
bool wh_combo_tap(const wchar_t *t) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if ((n->kind != AK_KIND_WHEEL && n->kind != AK_KIND_PICKER) || !n->ctl || !wh_on_screen(n) || !IsWindowEnabled(n->ctl)) continue;
		for (uint32_t k = 0; k < n->nitems; k++)
			if (!wcscmp(n->items[k], t)) {
				SendMessageW(n->ctl, CB_SETCURSEL, k, 0);
				SendMessageW(GetParent(n->ctl), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(n->ctl), CBN_SELCHANGE), (LPARAM)n->ctl);
				return true;
			}
	}
	return false;
}
