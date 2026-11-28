// suggest.c: a TextField's Suggestions: the list under the field (an
// AutoSuggestBox's, a ComboBox's), and a Dropdown field's chevron (WinUI's
// editable ComboBox).
//
// The field stays the host's EDIT in Fluent's frame (controls.c), so its
// typing, selection, pasting and undo are a field's. A combo box with
// CBS_DROPDOWN would bring its own list, but it rewrites its edit's text
// and selection as the list drops, and selects by prefix as one types,
// where the core's list is already filtered (shown as it is) and a choice
// must not read as typing. The list is a popup of the host's, drawn as the
// Picker's list (combo.c): an owner-drawn listbox on the flyout surface,
// WinUI's ComboBoxItem metrics, the item under the pointer or the keyboard
// on the subtle fill, a Dropdown field's chosen item (its text) with the
// accent pill, DWM's rounded corners and border on Windows 11, a contrast
// theme's system colors. It shows under the field (above it near the
// screen's bottom), as wide, while the field has the keyboard's focus,
// once the user has typed since it took the focus or since a choice; it
// follows the suggestions as they change, and goes when they're none.
// Up and Down move in it, Enter chooses, Escape closes it; a click
// chooses (the list never takes the focus: the field keeps it). A Dropdown
// field's chevron (E70D) at its end opens and closes it with all the
// suggestions, as do Alt+Down and F4.
//
// Choosing puts the item's text in the field, the cursor at its end,
// reported with Suggestion, not TextChanged. Screen readers hear the item
// the arrows move to (a notification); the list is a listbox, its items
// list items.
#include "win.h"

#define LIST_CLASS L"AsukaSuggestions"

// WinUI's metrics (points), as the Picker's list's.
#define LINE 20
#define ITEM_MAR_X 5
#define ITEM_MAR_Y 2
#define ITEM_PAD_X 11
#define PAD_TOP 5
#define PAD_BOTTOM 7
#define LIST_PAD_Y 4
#define LIST_GAP 2        // between the field and the list
#define LIST_ITEMS 8      // the most it shows before it scrolls
#define PILL_WIDTH 3
#define PILL_HEIGHT 16
#define CHEVRON_COL 38    // a Dropdown field's chevron's column
#define CHEVRON_END 14
#define CHEVRON_SIZE 12

struct wh_suggest {
	wchar_t **items;
	uint32_t n;
	bool dropdown;
	bool typed;      // typed since the focus came, or since a choice
	bool open;       // the list shows
	bool by_button;  // opened by the chevron (or Alt+Down): all of them, typed or not
	int hot;         // the item highlighted (-1: none)
	int pressed;     // the item the mouse went down on (-1: none)
	HWND popup, list;
	bool chev_hot, chev_pressed;
	POINT moved;     // where the mouse was last over the list (on the screen)
	uint32_t id;     // its field's
};

static void close_list(node *n);
static void choose(node *n, int i);

static rgba sys_rgba(int i) {
	COLORREF c = GetSysColor(i);
	return (rgba){GetRValue(c), GetGValue(c), GetBValue(c), 255};
}

static wh_suggest *of(node *n) {
	if (!n->suggest) {
		n->suggest = wh_alloc(sizeof *n->suggest);
		n->suggest->hot = n->suggest->pressed = -1;
		n->suggest->id = n->id;
	}
	return n->suggest;
}

static node *field_of_list(HWND hwnd) {
	uint32_t id = (uint32_t)GetWindowLongPtrW(GetAncestor(hwnd, GA_ROOT) == hwnd ? hwnd : GetParent(hwnd), GWLP_USERDATA);
	node *n = id ? wh_node(id) : NULL;
	return n && n->suggest && n->suggest->popup ? n : NULL;
}

// ---- Measuring ----

static int line_height(node *n) {
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	return wh_maxi(tm.tmHeight, wh_px(n, LINE * wh_font_scale()));
}

static int item_height(node *n) { return line_height(n) + wh_px(n, 2 * ITEM_MAR_Y + PAD_TOP + PAD_BOTTOM); }

// ---- The list's window ----

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp);

static void register_class(void) {
	static bool done;
	if (done) return;
	done = true;
	WNDCLASSEXW c = {sizeof c};
	c.style = CS_DROPSHADOW;
	c.lpfnWndProc = popup_proc;
	c.hInstance = GetModuleHandleW(NULL);
	c.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	c.lpszClassName = LIST_CLASS;
	RegisterClassExW(&c);
}

static bool chosen_item(node *n, int i) {
	wh_suggest *s = n->suggest;
	return s->dropdown && i >= 0 && (uint32_t)i < s->n && n->text.n && !wcscmp(s->items[i], n->text.p);
}

// draw_item draws a list item as the Picker's are (combo.c): its plate
// under the pointer or the keyboard, a Dropdown's chosen item's pill.
static void draw_item(node *n, DRAWITEMSTRUCT *d) {
	wh_suggest *s = n->suggest;
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	rgba bg = wh_combo_list_bg();
	FillRect(dc, &r, wh_brush(bg));
	int i = (int)d->itemID;
	if (i < 0 || (uint32_t)i >= s->n) return;
	float k = wh_scale_of(n);
	bool hc = wh_high_contrast(), lit = i == s->hot, chosen = chosen_item(n, i);
	rgba text = wh_combo_list_text(), plate = {0}, pill = H.theme.accent;
	if (hc) {
		if (lit) plate = sys_rgba(COLOR_HIGHLIGHT), text = sys_rgba(COLOR_HIGHLIGHTTEXT);
		pill = lit ? sys_rgba(COLOR_HIGHLIGHTTEXT) : sys_rgba(COLOR_HIGHLIGHT);
	} else {
		if (lit || chosen) plate = rgba_over(i == s->pressed ? H.theme.subtle_pressed : H.theme.subtle_hot, bg);
		rgba c;
		if (wh_resolve(n->color, &c) && c.a > 0) pill = rgba_over(c, bg);
	}
	bool rtl = wh_is_rtl(n);
	RECT pr = {r.left + wh_px(n, ITEM_MAR_X), r.top + wh_px(n, ITEM_MAR_Y), r.right - wh_px(n, ITEM_MAR_X), r.bottom - wh_px(n, ITEM_MAR_Y)};
	if (plate.a > 0 || chosen) {
		gfx g = g_new(dc);
		if (plate.a > 0) g_fill_round_rect(g, rgba_argb(plate), (float)pr.left, (float)pr.top, (float)(pr.right - pr.left), (float)(pr.bottom - pr.top), 4 * k);
		if (chosen) {
			int pw = wh_px(n, PILL_WIDTH), ph = wh_px(n, PILL_HEIGHT);
			int px = rtl ? pr.right - pw : pr.left, py = pr.top + ((pr.bottom - pr.top) - ph) / 2;
			g_fill_round_rect(g, rgba_argb(pill), (float)px, (float)py, (float)pw, (float)ph, (float)pw / 2);
		}
		g_close(g);
	}
	rgba under = plate.a > 0 ? plate : bg;
	int pad = wh_px(n, ITEM_PAD_X);
	RECT tr = {pr.left + pad, pr.top, pr.right - pad, pr.bottom};
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(rgba_over(text, under)));
	DrawTextW(dc, s->items[i], -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (rtl ? DT_RIGHT : DT_LEFT) | wh_text_direction(rtl));
	SelectObject(dc, old);
}

static void set_hot(node *n, int i, bool announce) {
	wh_suggest *s = n->suggest;
	if (i < -1 || i >= (int)s->n) i = -1;
	if (i == s->hot) return;
	s->hot = i;
	if (s->list) {
		SendMessageW(s->list, LB_SETCURSEL, (WPARAM)i, 0);  // its selection: the item screen readers find
		if (i >= 0) {
			int top = (int)SendMessageW(s->list, LB_GETTOPINDEX, 0, 0);
			if (i < top) SendMessageW(s->list, LB_SETTOPINDEX, (WPARAM)i, 0);
			else if (i >= top + LIST_ITEMS) SendMessageW(s->list, LB_SETTOPINDEX, (WPARAM)(i - LIST_ITEMS + 1), 0);
		}
		InvalidateRect(s->list, NULL, FALSE);
	}
	if (announce && i >= 0) {
		int len;
		char *u = wh_utf8(s->items[i], -1, &len);
		wh_announce(u, (size_t)len);
		free(u);
	}
}

// list_message is the listbox's subclass: the mouse chooses without
// taking the focus from the field.
static LRESULT CALLBACK list_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
	if (m == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, list_message, id);
		return DefSubclassProc(hwnd, m, wp, lp);
	}
	node *n = wh_node((uint32_t)data);
	if (!n || !n->suggest || n->suggest->list != hwnd) return DefSubclassProc(hwnd, m, wp, lp);
	wh_suggest *s = n->suggest;
	switch (m) {
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK: {
		LRESULT at = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
		if (HIWORD(at)) return 0;  // outside the items
		s->pressed = LOWORD(at);
		set_hot(n, s->pressed, false);
		SetCapture(hwnd);
		InvalidateRect(hwnd, NULL, FALSE);
		return 0;
	}
	case WM_MOUSEMOVE: {
		// Only the mouse moving: not the list showing under it.
		POINT at_screen;
		GetCursorPos(&at_screen);
		if (s->pressed < 0 && at_screen.x == s->moved.x && at_screen.y == s->moved.y) return 0;
		s->moved = at_screen;
		TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
		TrackMouseEvent(&t);
		LRESULT at = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
		set_hot(n, HIWORD(at) ? -1 : LOWORD(at), false);
		return 0;
	}
	case WM_MOUSELEAVE:
		if (s->pressed < 0) set_hot(n, -1, false);
		return 0;
	case WM_LBUTTONUP: {
		int was = s->pressed;
		s->pressed = -1;
		if (GetCapture() == hwnd) ReleaseCapture();
		LRESULT at = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
		RECT cr;
		GetClientRect(hwnd, &cr);
		POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		if (was >= 0 && !HIWORD(at) && LOWORD(at) == was && PtInRect(&cr, p)) choose(n, was);
		else InvalidateRect(hwnd, NULL, FALSE);
		return 0;
	}
	case WM_ERASEBKGND: {
		RECT cr;
		GetClientRect(hwnd, &cr);
		FillRect((HDC)wp, &cr, wh_brush(wh_combo_list_bg()));
		return 1;
	}
	case WM_KEYDOWN: case WM_CHAR:
		return 0;  // the field has the keyboard
	}
	return DefSubclassProc(hwnd, m, wp, lp);
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	node *n = field_of_list(hwnd);
	switch (m) {
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_DRAWITEM:
		if (n) {
			draw_item(n, (DRAWITEMSTRUCT *)lp);
			return TRUE;
		}
		break;
	case WM_MEASUREITEM:
		if (n) {
			((MEASUREITEMSTRUCT *)lp)->itemHeight = (UINT)item_height(n);
			return TRUE;
		}
		break;
	case WM_CTLCOLORLISTBOX: {
		rgba bg = wh_combo_list_bg();
		SetBkColor((HDC)wp, rgba_ref(bg));
		SetTextColor((HDC)wp, rgba_ref(wh_combo_list_text()));
		return (LRESULT)wh_brush(bg);
	}
	case WM_ERASEBKGND: {
		RECT cr;
		GetClientRect(hwnd, &cr);
		FillRect((HDC)wp, &cr, wh_brush(wh_combo_list_bg()));
		return 1;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		FillRect(dc, &ps.rcPaint, wh_brush(wh_combo_list_bg()));
		if (!wh_windows11()) {
			// Windows 10's hairline (DWM draws Windows 11's border).
			RECT cr;
			GetClientRect(hwnd, &cr);
			FrameRect(dc, &cr, wh_brush(H.dark ? rgba_hex(0x3E3E3E) : rgba_hex(0xE5E5E5)));
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	}
	return DefWindowProcW(hwnd, m, wp, lp);
}

static DWORD list_ex(node *n) { return wh_is_rtl(n) ? WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR : 0; }

static void make_popup(node *n) {
	wh_suggest *s = n->suggest;
	if (s->popup) return;
	register_class();
	HWND owner = GetAncestor(n->hwnd, GA_ROOT);
	s->popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, LIST_CLASS, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0, owner, NULL,
		GetModuleHandleW(NULL), NULL);
	if (!s->popup) return;
	SetWindowLongPtrW(s->popup, GWLP_USERDATA, (LONG_PTR)n->id);
	s->list = CreateWindowExW(list_ex(n), L"LISTBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0, s->popup,
		(HMENU)1, GetModuleHandleW(NULL), NULL);
	SendMessageW(s->list, WM_SETFONT, (WPARAM)wh_node_font(n), 0);
	SetWindowSubclass(s->list, list_message, 1, (DWORD_PTR)n->id);
	wh_allow_dark(s->list, H.dark);
	SetWindowTheme(s->list, H.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
	if (n->a11y_label.n) wh_set_accessible_name(s->list, n->a11y_label.p);
	else if (n->placeholder.n) wh_set_accessible_name(s->list, n->placeholder.p);
	if (wh_windows11()) {
		DWORD pref = 2;  // DWMWCP_ROUND
		DwmSetWindowAttribute(s->popup, 33, &pref, sizeof pref);
		COLORREF c = rgba_ref(wh_high_contrast() ? sys_rgba(COLOR_WINDOWTEXT) : H.dark ? rgba_hex(0x3E3E3E) : rgba_hex(0xE5E5E5));
		DwmSetWindowAttribute(s->popup, 34, &c, sizeof c);
	}
}

static void drop_popup(wh_suggest *s) {
	if (s->list) {
		wh_clear_accessible_name(s->list);
		RemoveWindowSubclass(s->list, list_message, 1);
	}
	if (s->popup) DestroyWindow(s->popup);
	s->popup = s->list = NULL;
	s->open = false;
}

// fill puts the suggestions in the listbox.
static void fill(node *n) {
	wh_suggest *s = n->suggest;
	if (!s->list) return;
	SendMessageW(s->list, WM_SETREDRAW, FALSE, 0);
	SendMessageW(s->list, LB_RESETCONTENT, 0, 0);
	SendMessageW(s->list, LB_SETITEMHEIGHT, 0, item_height(n));
	for (uint32_t i = 0; i < s->n; i++) SendMessageW(s->list, LB_ADDSTRING, 0, (LPARAM)s->items[i]);
	SendMessageW(s->list, LB_SETCURSEL, (WPARAM)s->hot, 0);
	SendMessageW(s->list, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(s->list, NULL, TRUE);
}

// place puts the list under the field (above it if there's no room
// below), as wide as it.
static void place(node *n) {
	wh_suggest *s = n->suggest;
	if (!s->popup || !n->hwnd) return;
	RECT f;
	GetWindowRect(n->hwnd, &f);
	int ih = item_height(n), pad = wh_px(n, LIST_PAD_Y), gap = wh_px(n, LIST_GAP);
	int shown = (int)wh_mini((int)s->n, LIST_ITEMS);
	int h = shown * ih + 2 * pad, w = f.right - f.left;
	RECT work = wh_monitor_work_area(f);
	int y = f.bottom + gap;
	if (y + h > work.bottom && f.top - gap - h >= work.top) y = f.top - gap - h;
	SetWindowPos(s->popup, NULL, f.left, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
	SetWindowPos(s->list, NULL, 0, pad, w, h - 2 * pad, SWP_NOZORDER | SWP_NOACTIVATE);
}

static bool has_focus(node *n) { return n->ctl && (GetFocus() == n->ctl || n->focused); }

// update shows or hides the list as the field is: focused, typed into
// (or opened by its chevron), with suggestions.
static void update(node *n) {
	wh_suggest *s = n->suggest;
	if (!s) return;
	bool want = s->n > 0 && (s->typed || s->by_button) && has_focus(n) && !n->disabled && n->hwnd && IsWindowVisible(n->hwnd);
	if (!want) {
		if (s->open) close_list(n);
		return;
	}
	make_popup(n);
	if (!s->popup) return;
	fill(n);
	place(n);
	if (!s->open) {
		s->open = true;
		GetCursorPos(&s->moved);
		// The theme as it is now (dark or light, a contrast theme).
		wh_allow_dark(s->list, H.dark);
		SetWindowTheme(s->list, H.dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
		if (wh_windows11()) {
			COLORREF c = rgba_ref(wh_high_contrast() ? sys_rgba(COLOR_WINDOWTEXT) : H.dark ? rgba_hex(0x3E3E3E) : rgba_hex(0xE5E5E5));
			DwmSetWindowAttribute(s->popup, 34, &c, sizeof c);  // DWMWA_BORDER_COLOR
		}
		ShowWindow(s->popup, SW_SHOWNOACTIVATE);
		NotifyWinEvent(EVENT_OBJECT_SHOW, s->list, OBJID_CLIENT, CHILDID_SELF);
		wh_redraw(n);
	}
}

static void close_list(node *n) {
	wh_suggest *s = n->suggest;
	if (!s) return;
	s->by_button = false;
	s->hot = s->pressed = -1;
	if (!s->open) return;
	s->open = false;
	if (s->popup) ShowWindow(s->popup, SW_HIDE);
	wh_redraw(n);
}

// choose puts item i's text in the field (not as typing), the cursor at
// its end, and reports it.
static void choose(node *n, int i) {
	wh_suggest *s = n->suggest;
	if (!s || i < 0 || (uint32_t)i >= s->n || !n->ctl) return;
	wstr_setw(&n->text, s->items[i], -1);
	n->setting_text = true;
	SetWindowTextW(n->ctl, n->text.p ? n->text.p : L"");
	n->setting_text = false;
	SendMessageW(n->ctl, EM_SETSEL, (WPARAM)n->text.n, (LPARAM)n->text.n);
	s->typed = false;
	close_list(n);
	wh_check_selection(n);
	wh_emit_int(n, AK_EVENT_SUGGESTION, i);
}

// ---- The field's side ----

void wh_suggest_prop(node *n, uint16_t p, const ak_value *v) {
	wh_suggest *s = of(n);
	if (p == AK_PROP_TEXT_FIELD_SUGGESTIONS) {
		wh_free_strings(s->items, s->n);
		s->n = wh_decode_strings(&s->items, (const uint8_t *)v->s.p, v->s.len);
		s->hot = s->pressed = -1;
		if (s->open || s->typed || s->by_button) update(n);
	} else if (p == AK_PROP_TEXT_FIELD_DROPDOWN) {
		s->dropdown = v->i != 0;
		wh_layout_field(n);
		wh_redraw(n);
	}
}

void wh_suggest_release(node *n) {
	wh_suggest *s = n->suggest;
	if (!s) return;
	drop_popup(s);
	wh_free_strings(s->items, s->n);
	free(s);
	n->suggest = NULL;
}

bool wh_suggest_dropdown(const node *n) { return n->suggest && n->suggest->dropdown; }

// wh_suggest_field follows a field's EDIT: typing (EN_CHANGE, after its
// TextChanged went), the focus coming and going.
void wh_suggest_field(node *n, UINT code) {
	wh_suggest *s = n->suggest;
	if (!s) return;
	switch (code) {
	case EN_CHANGE:
		s->typed = true;
		update(n);
		break;
	case EN_SETFOCUS:
		s->typed = false;
		break;
	case EN_KILLFOCUS:
		s->typed = false;
		close_list(n);
		break;
	}
}

void wh_suggest_disabled(node *n) {
	if (n->suggest && n->disabled) close_list(n);
}

// toggle opens the list of all the suggestions (the chevron, Alt+Down),
// or closes it.
static void toggle(node *n) {
	wh_suggest *s = n->suggest;
	if (!s || n->disabled) return;
	if (s->open) {
		close_list(n);
		return;
	}
	if (n->ctl && GetFocus() != n->ctl) SetFocus(n->ctl);
	s->by_button = true;
	update(n);
	if (s->open && n->text.n)
		for (uint32_t i = 0; i < s->n; i++)
			if (!wcscmp(s->items[i], n->text.p)) set_hot(n, (int)i, false);
}

// wh_suggest_key takes a key for a field whose list shows (or a Dropdown
// field's Alt+Down and F4), before the window's shortcuts: Up and Down
// move in the list, Enter chooses, Escape closes it.
bool wh_suggest_key(MSG *m) {
	if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return false;
	node *n = wh_node_of(m->hwnd);
	if (!n || !n->suggest || n->ctl != m->hwnd || n->kind != AK_KIND_TEXT_FIELD) return false;
	wh_suggest *s = n->suggest;
	bool alt = m->message == WM_SYSKEYDOWN || wh_key_down(VK_MENU);
	if (s->dropdown && (m->wParam == VK_F4 || (alt && (m->wParam == VK_DOWN || m->wParam == VK_UP)))) {
		toggle(n);
		return true;
	}
	if (!s->open || m->message != WM_KEYDOWN) return false;
	switch (m->wParam) {
	case VK_DOWN:
		set_hot(n, s->hot + 1 < (int)s->n ? s->hot + 1 : s->hot, true);
		return true;
	case VK_UP:
		set_hot(n, s->hot < 0 ? (int)s->n - 1 : s->hot - 1, true);  // from the field, the last; from the first, the field
		return true;
	case VK_PRIOR:
		set_hot(n, wh_maxi(s->hot - LIST_ITEMS, 0), true);
		return true;
	case VK_NEXT:
		set_hot(n, wh_mini(s->hot + LIST_ITEMS, (int)s->n - 1), true);
		return true;
	case VK_RETURN:
		if (s->hot >= 0) {
			choose(n, s->hot);
			return true;
		}
		close_list(n);
		return false;  // the field's Submit
	case VK_ESCAPE:
		close_list(n);
		s->typed = false;
		return true;
	case VK_TAB:
		close_list(n);
		return false;
	}
	return false;
}

// wh_suggest_pointer_down closes a list a press lands outside of (not in
// it, nor its field).
void wh_suggest_pointer_down(HWND target) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		wh_suggest *s = n->suggest;
		if (!s || !s->open) continue;
		if (target == s->popup || target == s->list || target == n->ctl || target == n->hwnd) continue;
		close_list(n);
	}
}

// wh_suggest_follow keeps the lists showing under their fields as the
// window moves or the field is placed again.
void wh_suggest_follow(node *only) {
	if (only) {
		if (only->suggest && only->suggest->open) place(only);
		return;
	}
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (n->suggest && n->suggest->open) {
			if (!n->hwnd || !IsWindowVisible(n->hwnd)) close_list(n);
			else place(n);
		}
	}
}

// ---- A Dropdown field's chevron ----

static RECT chevron_rect(node *n) {
	RECT r;
	GetClientRect(n->hwnd, &r);
	int col = wh_px(n, CHEVRON_COL);
	if (wh_is_rtl(n)) r.right = r.left + col;
	else r.left = r.right - col;
	return r;
}

// wh_suggest_message is a Dropdown field's view's mouse: its chevron.
LRESULT wh_suggest_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = false;
	wh_suggest *s = n->suggest;
	if (!s || !s->dropdown) return 0;
	POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
	RECT c = chevron_rect(n);
	switch (m) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		*handled = true;
		if (n->disabled) return 0;
		if (PtInRect(&c, p)) {
			s->chev_pressed = true;
			wh_redraw(n);
			toggle(n);
		} else if (n->ctl) {
			SetFocus(n->ctl);  // the frame's padding: the field
		}
		return 0;
	case WM_LBUTTONUP:
		if (s->chev_pressed) {
			s->chev_pressed = false;
			wh_redraw(n);
		}
		*handled = true;
		return 0;
	case WM_MOUSEMOVE: {
		bool hot = PtInRect(&c, p) != 0;
		if (hot != s->chev_hot) {
			s->chev_hot = hot;
			TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, n->hwnd, 0};
			TrackMouseEvent(&t);
			wh_redraw(n);
		}
		*handled = true;
		return 0;
	}
	case WM_MOUSELEAVE:
		if (s->chev_hot || s->chev_pressed) {
			s->chev_hot = s->chev_pressed = false;
			wh_redraw(n);
		}
		return 0;
	}
	(void)wp;
	return 0;
}

// wh_paint_dropdown draws a Dropdown field's chevron: its column's subtle
// fill under the pointer (g), the glyph (dc), a point lower pressed.
void wh_paint_dropdown(node *n, HDC dc, gfx g, RECT r) {
	wh_suggest *s = n->suggest;
	if (!s || !s->dropdown) return;
	(void)r;
	RECT c = chevron_rect(n);
	float k = wh_scale_of(n);
	bool hc = wh_high_contrast();
	if (g) {
		if ((s->chev_hot || s->chev_pressed || s->open) && !n->disabled) {
			float in = 4 * k;
			rgba fill = hc ? sys_rgba(COLOR_HIGHLIGHT) : s->chev_pressed || s->open ? H.theme.subtle_pressed : H.theme.subtle_hot;
			g_fill_round_rect(g, rgba_argb(fill), (float)c.left + in, (float)c.top + in, (float)(c.right - c.left) - 2 * in, (float)(c.bottom - c.top) - 2 * in, 3 * k);
		}
		return;
	}
	wchar_t ch = 0xE70D;
	rgba col = n->disabled ? H.theme.disabled : H.theme.secondary;
	if (hc) col = n->disabled ? sys_rgba(COLOR_GRAYTEXT) : s->chev_hot ? sys_rgba(COLOR_HIGHLIGHTTEXT) : sys_rgba(COLOR_WINDOWTEXT);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(col));
	HGDIOBJ old = SelectObject(dc, wh_icon_font(n, CHEVRON_SIZE));
	int gs = wh_px(n, CHEVRON_SIZE), end = wh_px(n, CHEVRON_END);
	int gx = wh_is_rtl(n) ? c.left + end : c.right - end - gs;
	RECT gr = {gx, c.top, gx + gs, c.bottom};
	if (s->chev_pressed) gr.top += wh_px(n, 1), gr.bottom += wh_px(n, 1);
	DrawTextW(dc, &ch, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old);
}

// wh_suggest_end is the room a Dropdown field's chevron takes at its end,
// in points (0: none).
float wh_suggest_end(const node *n) { return wh_suggest_dropdown(n) ? CHEVRON_COL : 0; }

// ---- Automation ----

HWND wh_suggest_popup(void) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (n->suggest && n->suggest->open && n->suggest->popup) return n->suggest->popup;
	}
	return NULL;
}

// wh_suggest_tap clicks the item that says t in a list showing, as the
// mouse does.
bool wh_suggest_tap(const wchar_t *t) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		wh_suggest *s = n->suggest;
		if (!s || !s->open || !s->list) continue;
		for (uint32_t k = 0; k < s->n; k++) {
			if (wcscmp(s->items[k], t)) continue;
			SendMessageW(s->list, LB_SETTOPINDEX, k, 0);
			RECT r;
			if (SendMessageW(s->list, LB_GETITEMRECT, k, (LPARAM)&r) == LB_ERR) return false;
			LPARAM lp = MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);
			SendMessageW(s->list, WM_LBUTTONDOWN, MK_LBUTTON, lp);
			SendMessageW(s->list, WM_LBUTTONUP, 0, lp);
			return true;
		}
	}
	return false;
}

// wh_suggest_chevron clicks a Dropdown field's chevron, the field found
// by its placeholder.
bool wh_suggest_chevron(const wchar_t *placeholder) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (!wh_suggest_dropdown(n) || !n->hwnd || !wh_on_screen(n)) continue;
		if (*placeholder && (!n->placeholder.n || wcscmp(n->placeholder.p, placeholder))) continue;
		RECT c = chevron_rect(n);
		LPARAM lp = MAKELPARAM((c.left + c.right) / 2, (c.top + c.bottom) / 2);
		SendMessageW(n->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
		SendMessageW(n->hwnd, WM_LBUTTONUP, 0, lp);
		return true;
	}
	return false;
}

void wh_suggest_dump(node *n, ak_buf *b) {
	wh_suggest *s = n->suggest;
	if (!s) return;
	ak_buf_printf(b, " suggestions=%u", s->n);
	if (s->dropdown) ak_buf_printf(b, " dropdown");
	if (!s->open) return;
	ak_buf_put(b, " list=[", 7);
	for (uint32_t i = 0; i < s->n; i++) {
		if (i) ak_buf_u8(b, ',');
		int len;
		char *u = wh_utf8(s->items[i], -1, &len);
		ak_fmt_quote(b, ak_strn(u, (size_t)len));
		free(u);
	}
	ak_buf_u8(b, ']');
	if (s->hot >= 0) ak_buf_printf(b, " highlighted=%d", s->hot);
}
