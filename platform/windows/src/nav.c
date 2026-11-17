// nav.c: navigation on Windows:
//
//   - A Stack shows its top Screen under a header bar of its own: a back
//     button (Alt+Left too) while there's a screen to go back to, the
//     screen's title, its bar buttons on the right and its search field.
//   - Tabs are a bar of tabs on top (WinUI's top NavigationView: an icon
//     and a title each, an accent pill under the selected one).
//   - A Sheet is a modal window over the window it's declared in (its
//     owner disabled meanwhile); closing it or Escape dismisses it. A
//     FullScreen one is a borderless window over all of its window's
//     content, following it as it moves and resizes.
//   - An Alert is a task dialog; an action sheet's buttons are command
//     links. One with text fields is the host's own dialog
//     (alertdialog.c).
//   - A Window is a top-level window of its own (appwindow.c's part here).
//
// Screens, sheets and Windows are sized by the host, which reports each
// one's Environment.
#include "nav.h"

static wh_nav *nav_new(void) {
	wh_nav *v = wh_alloc(sizeof *v);
	v->hot = -1;
	return v;
}

static node *make_header(node *stack);
static node *make_tab_bar(node *tabs);

bool wh_create_nav(node *n) {
	switch (n->kind) {
	case AK_KIND_STACK:
		n->hwnd = wh_make_view(0);
		n->nav = nav_new();
		n->nav->bar = make_header(n);
		return true;
	case AK_KIND_SCREEN:
		n->hwnd = wh_make_view(0);
		n->nav = nav_new();
		n->nav->back_gesture = true;
		return true;
	case AK_KIND_TABS:
		n->hwnd = wh_make_view(0);
		n->nav = nav_new();
		n->nav->bar = make_tab_bar(n);
		return true;
	case AK_KIND_SHEET:
	case AK_KIND_WINDOW:
	case AK_KIND_POPOVER:
		n->hwnd = wh_make_view(0);
		n->nav = nav_new();
		return true;
	case AK_KIND_ALERT:
		n->nav = nav_new();
		return true;
	case AK_KIND_SNACKBAR:
		n->hwnd = wh_make_view(0);
		ShowWindow(n->hwnd, SW_HIDE);  // until it's presented, placed
		n->nav = nav_new();
		return true;
	}
	return false;
}

// wh_nav_sized_by_host reports whether the host sizes n (a screen, a
// sheet's content), not the core.
bool wh_nav_sized_by_host(node *n) {
	return n->kind == AK_KIND_SCREEN || n->kind == AK_KIND_SHEET || n->kind == AK_KIND_WINDOW || n->kind == AK_KIND_POPOVER;
}

HWND wh_nav_bar_window(node *p) { return p->nav && p->nav->bar && p->nav->bar->hwnd ? p->nav->bar->hwnd : NULL; }

wh_wctl *wh_nav_ctl(node *n) { return n && n->nav ? n->nav->ctl : NULL; }

HWND wh_nav_window(node *n) { return n && n->nav ? n->nav->window : NULL; }

bool wh_nav_flyout(node *n, ak_flyout *f) {
	if (!n->nav || !n->nav->has_flyout) return false;
	*f = n->nav->flyout;
	return true;
}

void wh_nav_min_size(node *n, float out[2]) {
	out[0] = n->nav ? n->nav->min_size[0] : 0;
	out[1] = n->nav ? n->nav->min_size[1] : 0;
}

void wh_nav_fitted(node *n, float w, float h) {
	n->nav->size[0] = w, n->nav->size[1] = h;
	n->nav->fitted = true;
}

void wh_nav_set_direction(node *n, uint8_t d) {
	if (!n->nav) return;
	if (n->nav->bar) n->nav->bar->dir = d;
	if (n->nav->foot) n->nav->foot->dir = d;
}

void wh_nav_window_dark(node *n, bool dark) {
	if (!n->nav) return;
	HWND w = n->kind == AK_KIND_ALERT ? wh_alert_dialog_window(n) : n->nav->window;
	if (!w) return;
	wh_set_dark_title_bar(w, dark);
	if (n->kind == AK_KIND_ALERT) RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

void wh_nav_window_redraw(node *n) {
	if (n->nav && n->nav->window) RedrawWindow(n->nav->window, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME);
}

void wh_nav_say_words(node *n) {
	if (n->kind == KIND_HEADER && n->nav->back) SetWindowTextW(n->nav->back, wh_word("back"));
	if (n->kind == KIND_COMMAND_BAR && n->nav->cmd) SetWindowTextW(n->nav->cmd->more, wh_word("moreOptions"));
}

static void kids_insert(node *p, node *c, uint32_t i) {
	if (p->nkids == p->capkids) {
		p->capkids = p->capkids ? p->capkids * 2 : 4;
		p->kids = wh_realloc(p->kids, p->capkids * sizeof *p->kids);
	}
	memmove(p->kids + i + 1, p->kids + i, (p->nkids - i) * sizeof *p->kids);
	p->kids[i] = c;
	p->nkids++;
}

static void kids_remove(node *p, node *c) {
	for (uint32_t i = 0; i < p->nkids; i++)
		if (p->kids[i] == c) {
			memmove(p->kids + i, p->kids + i + 1, (p->nkids - i - 1) * sizeof *p->kids);
			p->nkids--;
			return;
		}
}

// wh_fill_screens sizes the Screens in a host-sized view (a screen
// component's Screen, in the Screen its Stack made for it; the root's) to fill it,
// and reports their environments.
void wh_fill_screens(node *p) {
	if (!p->hwnd) return;
	RECT r;
	GetClientRect(p->hwnd, &r);
	for (uint32_t i = 0; i < p->nkids; i++) {
		node *c = p->kids[i];
		if (c->kind != AK_KIND_SCREEN || !c->hwnd || c->dying) continue;
		SetWindowPos(c->hwnd, NULL, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
		wh_report_screen(c, wh_pt(p, r.right), wh_pt(p, r.bottom));
	}
}

bool wh_insert_nav(node *p, node *c, uint32_t index) {
	if (c->kind == AK_KIND_SCREEN && (wh_nav_sized_by_host(p) || p == H.root)) {
		// A Screen in a screen (a sheet, a Window) or at the root: the
		// host sizes it to fill it, as it does the one around it.
		uint32_t i = index < p->nkids ? index : p->nkids;
		kids_insert(p, c, i);
		c->parent = p;
		if (c->hwnd && p->hwnd) {
			SetParent(c->hwnd, p->hwnd);
			wh_adopt(p, c);
			ShowWindow(c->hwnd, SW_SHOWNA);
			wh_fill_screens(p);
		}
		return true;
	}
	if (c->kind != AK_KIND_SCREEN || (p->kind != AK_KIND_STACK && p->kind != AK_KIND_TABS)) return false;
	uint32_t i = index < p->nkids ? index : p->nkids;
	node *top = wh_top_screen(p);
	kids_insert(p, c, i);
	c->parent = p;
	SetParent(c->hwnd, p->hwnd);
	wh_adopt(p, c);
	SetWindowPos(c->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (p->kind == AK_KIND_STACK) {
		if (top && wh_top_screen(p) == c) ShowWindow(top->hwnd, SW_HIDE);
		if (wh_top_screen(p) != c) ShowWindow(c->hwnd, SW_HIDE);
	} else {
		if ((int)i != p->selected) ShowWindow(c->hwnd, SW_HIDE);
		InvalidateRect(p->nav->bar->hwnd, NULL, FALSE);
	}
	wh_layout_screens(p);
	wh_update_header(p);
	return true;
}

bool wh_remove_nav(node *p, node *c) {
	if (p->kind != AK_KIND_STACK && p->kind != AK_KIND_TABS) return false;
	if (c->hwnd) {
		ShowWindow(c->hwnd, SW_HIDE);
		SetParent(c->hwnd, H.park);
	}
	wh_show_screens(p);
	wh_update_header(p);
	return true;
}

// wh_destroy_nav destroys a screen, sheet or alert; false: an ordinary
// destroy.
bool wh_destroy_nav(node *n) {
	if (n->kind == AK_KIND_STACK && n->nav && n->nav->foot) wh_forget_command_bar(n);  // it goes with its window
	if (!n->nav || n->kind == AK_KIND_STACK || n->kind == AK_KIND_TABS) return false;
	switch (n->kind) {
	case AK_KIND_SCREEN: {
		node *p = n->parent;
		if (p) {
			kids_remove(p, n);
			n->parent = NULL;
		}
		for (uint32_t i = 0; i < n->nkids; i++) n->kids[i]->parent = NULL;
		wh_release(n);
		if (p) {
			wh_show_screens(p);
			wh_layout_screens(p);
			wh_update_header(p);
		}
		wh_free_node(n);
		return true;
	}
	case AK_KIND_SHEET:
	case AK_KIND_WINDOW:
	case AK_KIND_POPOVER:
		n->dying = true;
		if (n->kind == AK_KIND_SHEET) wh_close_sheet(n);
		else if (n->kind == AK_KIND_POPOVER) wh_close_popover(n);
		else wh_close_window(n);
		for (uint32_t i = 0; i < n->nkids; i++) n->kids[i]->parent = NULL;
		wh_release(n);
		wh_free_node(n);
		return true;
	case AK_KIND_ALERT:
		n->dying = true;
		wh_close_alert_dialog(n);
		if (!n->nav->dialog) wh_free_node(n);  // a task dialog open frees it as it returns
		return true;
	case AK_KIND_SNACKBAR:
		n->dying = true;
		wh_destroy_snack(n);
		return true;
	}
	return false;
}

node *wh_top_screen(node *stack) {
	for (uint32_t i = stack->nkids; i-- > 0;)
		if (!stack->kids[i]->dying) return stack->kids[i];
	return NULL;
}

uint32_t wh_live_screens(node *stack) {
	uint32_t live = 0;
	for (uint32_t i = 0; i < stack->nkids; i++)
		if (!stack->kids[i]->dying) live++;
	return live;
}

// wh_show_screens shows a stack's top screen, or the tabs' selected page,
// and hides the others.
void wh_show_screens(node *p) {
	node *shown = NULL;
	if (p->kind == AK_KIND_STACK) shown = wh_top_screen(p);
	else if (p->kind == AK_KIND_TABS && p->selected >= 0 && (uint32_t)p->selected < p->nkids) shown = p->kids[p->selected];
	for (uint32_t i = 0; i < p->nkids; i++) {
		node *c = p->kids[i];
		if (c->hwnd) ShowWindow(c->hwnd, c == shown ? SW_SHOWNA : SW_HIDE);
	}
}

static bool header_shown(node *stack) {
	node *s = wh_top_screen(stack);
	if (!s) return false;
	return s->title.n || s->acts.n || s->nav->search.n || wh_live_screens(stack) > 1;
}

static float bar_height(node *p) {
	if (p->kind == AK_KIND_STACK) return header_shown(p) ? HEADER_HEIGHT : 0;
	if (p->kind == AK_KIND_TABS) return TAB_BAR_HEIGHT;
	return 0;
}

static void layout_header(node *b);

// wh_report_screen sends a screen's or sheet's environment if it changed.
void wh_report_screen(node *n, float w, float h) {
	if (!n->nav || w <= 0) return;
	wh_env env = H.env;
	env.env.width = w, env.env.height = h;
	env.env.scale = wh_scale_of(n);
	env.env.safe_top = env.env.safe_right = env.env.safe_bottom = env.env.safe_left = 0;
	env.env.width_class = ak_width_class_of(w), env.env.height_class = ak_height_class_of(h);
	if (n->kind == AK_KIND_WINDOW) env.env.safe_top = wh_safe_top(n->nav->ctl);  // under a hidden title bar's buttons
	if (n->nav->reported && !memcmp(&env.env, &n->nav->env.env, offsetof(ak_environment, locale)) && !strcmp(env.locale, n->nav->env.locale) &&
		!strcmp(env.time_zone, n->nav->env.time_zone) && !strcmp(env.languages, n->nav->env.languages) &&
		env.env.first_weekday == n->nav->env.env.first_weekday && env.env.hour_cycle == n->nav->env.env.hour_cycle)
		return;
	n->nav->env = env, n->nav->reported = true;
	wh_report_environment(n, &n->nav->env);
}

// wh_layout_screens sizes a stack's screens, or a tabs' pages, to it under
// its bar, and reports their environments.
void wh_layout_screens(node *n) {
	if (!n->hwnd || !n->nav) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	int bar = wh_px(n, bar_height(n));
	node *b = n->nav->bar;
	if (b && b->hwnd) {
		SetWindowPos(b->hwnd, HWND_TOP, 0, 0, r.right, bar, SWP_NOACTIVATE | SWP_NOCOPYBITS);
		if (n->kind == AK_KIND_STACK) layout_header(b);
	}
	// A stack's command bar, under its screens.
	int foot = wh_px(n, wh_cmd_bar_height_of(n));
	node *f = n->nav->foot;
	if (f && f->hwnd && foot > 0) {
		SetWindowPos(f->hwnd, HWND_TOP, 0, wh_maxi(r.bottom - foot, 0), r.right, foot, SWP_NOACTIVATE | SWP_NOCOPYBITS);
		wh_layout_command_bar(f);
	}
	int ch = wh_maxi(r.bottom - bar - foot, 0);
	for (uint32_t i = 0; i < n->nkids; i++) {
		node *c = n->kids[i];
		if (!c->hwnd) continue;
		SetWindowPos(c->hwnd, NULL, 0, bar, r.right, ch, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
		wh_report_screen(c, wh_pt(n, r.right), wh_pt(n, ch));
	}
}

// wh_layout_nav sizes every host-sized view again (the window resized).
void wh_layout_nav(void) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (n->kind == AK_KIND_STACK || n->kind == AK_KIND_TABS) wh_layout_screens(n);
		else if ((n->kind == AK_KIND_SHEET || n->kind == AK_KIND_WINDOW) && n->nav->window) {
			RECT r;
			GetClientRect(n->nav->window, &r);
			wh_report_screen(n, wh_pt(n, r.right), wh_pt(n, r.bottom));
		}
	}
	wh_place_snacks();
	wh_overlays_environment();
}

static void set_window_text(node *n);

void wh_set_nav_prop(node *n, uint16_t p, const ak_value *v) {
	const uint8_t *b = (const uint8_t *)v->s.p;
	switch (n->kind) {
	case AK_KIND_SCREEN:
		switch (p) {
		case AK_PROP_SCREEN_TITLE: wstr_set(&n->title, v->s); break;
		case AK_PROP_SCREEN_ACTIONS: actions_decode(&n->acts, b, v->s.len); break;
		case AK_PROP_SCREEN_TOOLBAR: actions_decode(&n->nav->toolbar, b, v->s.len); break;
		case AK_PROP_SCREEN_PRIMARY: actions_decode(&n->nav->primary, b, v->s.len); break;
		case AK_PROP_SCREEN_BACK_GESTURE: n->nav->back_gesture = v->i != 0; break;
		case AK_PROP_SCREEN_ICON:
			free(n->icon);
			n->icon = wh_strndup(v->s.p, v->s.len);
			break;
		case AK_PROP_SCREEN_BADGE:
			// The tab bar draws it again, and nothing else changes.
			wstr_set(&n->badge, v->s);
			if (n->parent && n->parent->kind == AK_KIND_TABS) InvalidateRect(n->parent->nav->bar->hwnd, NULL, FALSE);
			return;
		case AK_PROP_SCREEN_SEARCH: wstr_set(&n->nav->search, v->s); break;
		case AK_PROP_SCREEN_SEARCH_TEXT: wstr_set(&n->nav->search_text, v->s); break;
		}
		if (n->parent) {
			wh_update_header(n->parent);
			if (n->parent->kind == AK_KIND_TABS) InvalidateRect(n->parent->nav->bar->hwnd, NULL, FALSE);
		}
		break;
	case AK_KIND_TABS:
		if (p == AK_PROP_TABS_SELECTED) {
			n->selected = (int)v->i;
			wh_show_screens(n);
			wh_layout_screens(n);
			InvalidateRect(n->nav->bar->hwnd, NULL, FALSE);
		}
		break;
	case AK_KIND_SHEET:
		if (p == AK_PROP_SHEET_DETENTS) n->detents = v->i;
		else if (p == AK_PROP_SHEET_WINDOW) n->nav->on = (uint32_t)v->i;
		else if (p == AK_PROP_SHEET_FULL_SCREEN) n->nav->full = v->i != 0;
		break;
	case AK_KIND_POPOVER:
		wh_set_popover_prop(n, p, v);
		break;
	case AK_KIND_WINDOW:
		switch (p) {
		case AK_PROP_WINDOW_TITLE:
			wstr_set(&n->title, v->s);
			set_window_text(n);
			break;
		case AK_PROP_WINDOW_CONFIG: {
			free(n->nav->cfg);
			n->nav->cfg = (uint8_t *)wh_strndup(v->s.p, v->s.len);
			n->nav->cfg_len = v->s.len;
			ak_window_config c = ak_decode_window_config(n->nav->cfg, n->nav->cfg_len);
			n->nav->size[0] = c.width, n->nav->size[1] = c.height;
			n->nav->min_size[0] = c.min_width, n->nav->min_size[1] = c.min_height;
			break;
		}
		case AK_PROP_WINDOW_FLYOUT:
			if (ak_decode_flyout(b, v->s.len, &n->nav->flyout)) {
				n->nav->has_flyout = true;
				if (n->nav->ctl && n->nav->window) wh_place_flyout(n->nav->ctl);
			}
			break;
		case AK_PROP_WINDOW_EDITED:
			n->nav->edited = v->i != 0;
			set_window_text(n);
			break;
		}
		break;
	case AK_KIND_ALERT:
		switch (p) {
		case AK_PROP_ALERT_TITLE: wstr_set(&n->title, v->s); break;
		case AK_PROP_ALERT_MESSAGE: wstr_set(&n->message, v->s); break;
		case AK_PROP_ALERT_BUTTONS:
			actions_decode(&n->acts, b, v->s.len);
			// Showing, its buttons follow Disabled (a task dialog's titles
			// are its own once it's made).
			if (n->nav->dialog)
				for (uint32_t i = 0; i < n->acts.n; i++)
					SendMessageW(n->nav->dialog, TDM_ENABLE_BUTTON, n->acts.v[i].role == AK_ALERT_BUTTON_ROLE_CANCEL ? IDCANCEL : 100 + (int)i,
						!(n->acts.v[i].flags & AK_MENU_DISABLED));
			wh_update_alert_dialog(n);
			break;
		case AK_PROP_ALERT_FIELDS:
			free(n->nav->fields);
			n->nav->fields = (uint8_t *)wh_strndup(v->s.p, v->s.len);
			n->nav->fields_len = v->s.len;
			break;
		case AK_PROP_ALERT_STYLE: n->alert_style = (int32_t)v->i; break;
		case AK_PROP_ALERT_WINDOW: n->nav->on = (uint32_t)v->i; break;
		}
		break;
	case AK_KIND_SNACKBAR:
		wh_set_snack_prop(n, p, v);
		break;
	}
}

// ---- A stack's header ----

static node *make_header(node *stack) {
	node *b = wh_node_new(wh_host_id(), KIND_HEADER);
	b->hwnd = wh_make_view(0);
	b->nav = nav_new();
	b->nav->stack = stack;
	b->k = stack->k, b->dir = stack->dir;
	SetParent(b->hwnd, stack->hwnd);
	b->nav->back = CreateWindowExW(0, L"BUTTON", wh_word("back"), WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, b->hwnd, (HMENU)1, GetModuleHandleW(NULL), NULL);
	SendMessageW(b->nav->back, WM_UPDATEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
	wh_node_add(b);
	wh_bind(b, b->nav->back);
	return b;
}

// wh_update_header shows the top screen's title and buttons in its stack's
// header.
void wh_update_header(node *stack) {
	if (stack->kind != AK_KIND_STACK || !stack->nav) return;
	node *b = stack->nav->bar;
	node *screen = wh_top_screen(stack);
	b->nav->screen = screen;
	bool shown = header_shown(stack);
	if (!b->hwnd) return;
	const actions *acts = screen ? &screen->acts : NULL;
	uint32_t want = acts ? acts->n : 0;
	while (b->nav->nbuttons > want) {
		HWND btn = b->nav->buttons[--b->nav->nbuttons];
		wh_unbind(btn);
		DestroyWindow(btn);
	}
	while (b->nav->nbuttons < want) {
		HWND btn = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, b->hwnd,
			(HMENU)(INT_PTR)(b->nav->nbuttons + 2), GetModuleHandleW(NULL), NULL);
		SendMessageW(btn, WM_UPDATEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
		wh_bind(b, btn);
		b->nav->buttons = wh_realloc(b->nav->buttons, (b->nav->nbuttons + 1) * sizeof *b->nav->buttons);
		b->nav->buttons[b->nav->nbuttons++] = btn;
	}
	for (uint32_t i = 0; i < want; i++) {
		SetWindowTextW(b->nav->buttons[i], acts->v[i].title);
		EnableWindow(b->nav->buttons[i], !(acts->v[i].flags & AK_MENU_DISABLED));
		InvalidateRect(b->nav->buttons[i], NULL, FALSE);
	}
	wh_update_search(b, screen);
	wh_update_command_bar(stack);
	ShowWindow(b->nav->back, wh_live_screens(stack) > 1 ? SW_SHOWNA : SW_HIDE);
	ShowWindow(b->hwnd, shown ? SW_SHOWNA : SW_HIDE);
	wh_layout_screens(stack);
	InvalidateRect(b->hwnd, NULL, FALSE);
}

static int across(RECT r, bool rtl, int x, int w) { return rtl ? r.right - x - w : x; }

// layout_header places the back button on the left and the bar buttons on
// the right (the other way round, right to left), 40 points square, or as
// wide as their titles.
static void layout_header(node *b) {
	RECT r;
	GetClientRect(b->hwnd, &r);
	int size = wh_px(b, 36), y = (r.bottom - size) / 2;
	bool rtl = wh_is_rtl(b);
	UINT f = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
	SetWindowPos(b->nav->back, NULL, across(r, rtl, wh_px(b, 6), wh_px(b, 40)), y, wh_px(b, 40), size, f);
	int x = r.right - wh_px(b, 8);
	node *screen = b->nav->screen;
	for (uint32_t i = b->nav->nbuttons; i-- > 0;) {
		int w = wh_px(b, 40);
		if (screen && i < screen->acts.n && !wh_symbol_glyph(screen->acts.v[i].icon)) {
			HDC dc = H.measure_dc;
			HGDIOBJ old = SelectObject(dc, wh_body_font(b));
			w = wh_text_extent(dc, screen->acts.v[i].title, (int)wcslen(screen->acts.v[i].title)).cx + wh_px(b, 24);
			SelectObject(dc, old);
		}
		x -= w;
		SetWindowPos(b->nav->buttons[i], NULL, across(r, rtl, x, w), y, w, size, f);
		x -= wh_px(b, 4);
	}
	wh_layout_search(b, x, rtl, r.right);
}

// wh_header_button is what a header's button shows: the back arrow, or a
// bar button's icon or title.
uint16_t wh_header_button(node *n, HWND ctl, bool rtl, wchar_t *title, int cap) {
	wh_nav *nv = n->nav;
	if (ctl == nv->back) return rtl ? 0xE72A : 0xE72B;  // Forward (the way back goes, right to left), Back
	for (uint32_t i = 0; i < nv->nbuttons; i++) {
		if (nv->buttons[i] != ctl || !nv->screen || i >= nv->screen->acts.n) continue;
		const action *a = &nv->screen->acts.v[i];
		uint16_t g = wh_directed_glyph(a->icon, rtl);
		if (g) return g;
		wcsncpy(title, a->title, (size_t)cap - 1);
		title[cap - 1] = 0;
		return 0;
	}
	return 0;
}

static void pop_stack(node *stack);

void wh_header_command(node *b, HWND ctl) {
	if (ctl == b->nav->back) {
		pop_stack(b->nav->stack);
		return;
	}
	node *screen = b->nav->screen;
	for (uint32_t i = 0; i < b->nav->nbuttons; i++) {
		if (b->nav->buttons[i] != ctl || !screen || i >= screen->acts.n) continue;
		const action *a = &screen->acts.v[i];
		if (a->flags & AK_MENU_SUBMENU) {
			// A bar button with children opens them as a menu, below it.
			actions sub = {a->items, a->count};
			wh_pop_up_actions(screen, ctl, b, &sub, 0);
			return;
		}
		wh_emit_int(screen, AK_EVENT_ACTION, a->index);
	}
}

static font_spec title_spec(node *b) {
	font_spec s = wh_spec(AK_TEXT_STYLE_TITLE2, 0, 0, NULL);
	s.k = wh_scale_of(b);
	return s;
}

static RECT flip_in(RECT r, RECT outer) {
	LONG l = outer.left + outer.right - r.right, rr = outer.left + outer.right - r.left;
	r.left = l, r.right = rr;
	return r;
}

// wh_paint_header draws a hairline under the bar (g), and the screen's
// title beside the back button in Fluent's Subtitle (dc).
void wh_paint_header(node *b, HDC dc, gfx g, RECT r) {
	node *stack = b->nav->stack, *screen = b->nav->screen;
	float k = wh_scale_of(b);
	if (g) {
		g_fill_rect(g, rgba_argb(H.theme.separator), 0, (float)r.bottom - wh_maxf(k, 1), (float)r.right, wh_maxf(k, 1));
		return;
	}
	if (!screen || !screen->title.n) return;
	int x = wh_live_screens(stack) > 1 ? wh_px(b, 52) : wh_px(b, 16);
	HGDIOBJ old = SelectObject(dc, wh_font(title_spec(b)));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(H.theme.label));
	bool rtl = wh_is_rtl(b);
	RECT tr = {x, 0, r.right - wh_px(b, 16), r.bottom};
	if (rtl) tr = flip_in(tr, r);
	HWND edges[64];
	uint32_t ne = 0;
	for (uint32_t i = 0; i < b->nav->nbuttons && ne < 63; i++) edges[ne++] = b->nav->buttons[i];
	node *f = b->nav->field;
	if (f && f->hwnd && IsWindowVisible(f->hwnd)) edges[ne++] = f->hwnd;
	for (uint32_t i = 0; i < ne; i++) {
		RECT br = wh_rect_in(edges[i], b->hwnd);
		if (rtl) tr.left = wh_maxi(tr.left, br.right + wh_px(b, 8));
		else tr.right = wh_mini(tr.right, br.left - wh_px(b, 8));
	}
	DrawTextW(dc, screen->title.p, screen->title.n, &tr, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | wh_start_side(rtl));
	SelectObject(dc, old);
}

// pop_stack is the back button: the stack's top screen is popped (the
// core destroys it, told with Popped).
static void pop_stack(node *stack) {
	node *top = wh_top_screen(stack);
	if (!top || wh_live_screens(stack) < 2) return;
	if (top->intercept_back) {
		wh_emit_kind(top, AK_EVENT_BACK);
		return;
	}
	top->dying = true;
	wh_show_screens(stack);
	wh_update_header(stack);
	wh_emit_int(stack, AK_EVENT_POPPED, top->id);
}

// active_stack finds the innermost stack on screen under root that can
// pop: through each stack's top screen and each tabs' selected page.
static void walk_active(node *n, node *root, node **found) {
	switch (n->kind) {
	case AK_KIND_STACK: {
		if (wh_live_screens(n) > 1) *found = n;
		node *t = wh_top_screen(n);
		if (t) walk_active(t, root, found);
		return;
	}
	case AK_KIND_TABS:
		if (n->selected >= 0 && (uint32_t)n->selected < n->nkids) walk_active(n->kids[n->selected], root, found);
		return;
	case AK_KIND_SHEET:
	case AK_KIND_ALERT:
	case AK_KIND_WINDOW:
	case AK_KIND_POPOVER:
		if (n != root) return;
	}
	for (uint32_t i = 0; i < n->nkids; i++) walk_active(n->kids[i], root, found);
}

// wh_back pops the innermost shown stack that can pop in a window
// (Alt+Left): under root, the root or a sheet or Window.
void wh_back(node *root) {
	if (root && wh_overlay_back(root)) return;  // the topmost overlay that takes it, first
	node *s = NULL;
	if (root) walk_active(root, root, &s);
	if (s) pop_stack(s);
}

// ---- A tab bar ----

static LRESULT tab_bar_message(node *b, UINT m, WPARAM wp, LPARAM lp, bool *handled);

static node *make_tab_bar(node *tabs) {
	node *b = wh_node_new(wh_host_id(), KIND_TAB_BAR);
	b->hwnd = wh_make_view(WS_TABSTOP);
	b->nav = nav_new();
	b->nav->tabs = tabs;
	b->k = tabs->k, b->dir = tabs->dir;
	b->host_msg = tab_bar_message;
	SetParent(b->hwnd, tabs->hwnd);
	wh_node_add(b);
	return b;
}

// tab_rects are the tabs' places in the bar: each its icon and title,
// padded, from the left (the right, right to left).
static uint32_t tab_rects(node *b, RECT *out, uint32_t cap) {
	node *tabs = b->nav->tabs;
	RECT r;
	GetClientRect(b->hwnd, &r);
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, wh_body_font(b));
	int x = wh_px(b, 8);
	uint32_t n = 0;
	for (uint32_t i = 0; i < tabs->nkids && n < cap; i++) {
		node *s = tabs->kids[i];
		int w = wh_text_extent(dc, wstr_c(&s->title), s->title.n).cx + wh_px(b, 24);
		if (wh_symbol_glyph(s->icon)) w += wh_px(b, 16 + 8);
		int bw, bh;
		wh_badge_size(b, &s->badge, &bw, &bh);
		if (bw > 0) w += bw + wh_px(b, 6);
		out[n++] = (RECT){x, wh_px(b, 6), x + w, r.bottom - wh_px(b, 6)};
		x += w + wh_px(b, 4);
	}
	SelectObject(dc, old);
	if (wh_is_rtl(b))
		for (uint32_t i = 0; i < n; i++) out[i] = flip_in(out[i], r);
	return n;
}

static LRESULT tab_bar_message(node *b, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	node *tabs = b->nav->tabs;
	*handled = true;
	RECT rects[64];
	int i;
	switch (m) {
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN: {
		uint32_t n = tab_rects(b, rects, 64);
		POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		i = -1;
		for (uint32_t k = 0; k < n; k++)
			if (p.x >= rects[k].left && p.x < rects[k].right && p.y >= 0) i = (int)k;
		if (m == WM_LBUTTONDOWN) {
			if (i >= 0) wh_select_tab(tabs, i);
		} else if (i != b->nav->hot) {
			b->nav->hot = i;
			TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, b->hwnd, 0};
			TrackMouseEvent(&t);
			InvalidateRect(b->hwnd, NULL, FALSE);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		b->nav->hot = -1;
		InvalidateRect(b->hwnd, NULL, FALSE);
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS;
	case WM_KEYDOWN: {
		// The arrows go the way they point: the next tab is to the left,
		// right to left.
		int step = wh_is_rtl(b) ? -1 : 1;
		if (wp == VK_LEFT || wp == VK_RIGHT) {
			wh_select_tab(tabs, tabs->selected + (wp == VK_LEFT ? -step : step));
			wh_announce_choice(b);
			return 0;
		}
		break;
	}
	}
	*handled = false;
	return 0;
}

void wh_select_tab(node *tabs, int i) {
	if (i < 0 || (uint32_t)i >= tabs->nkids || i == tabs->selected) return;
	tabs->selected = i;
	wh_show_screens(tabs);
	wh_layout_screens(tabs);
	InvalidateRect(tabs->nav->bar->hwnd, NULL, FALSE);
	wh_emit_int(tabs, AK_EVENT_SELECTED, i);
}

static RECT tab_badge(node *b, node *s, RECT tr) {
	bool rtl = wh_is_rtl(b);
	int end = rtl ? tr.left + wh_px(b, 12) : tr.right - wh_px(b, 12);
	return wh_badge_at(b, &s->badge, end, tr.top, tr.bottom, rtl);
}

// wh_paint_tab_bar draws the tabs: under the mouse a highlight, under the
// selected one an accent pill (g); the titles, the selected one's in the
// text color and the others' secondary (dc); a hairline under the bar.
void wh_paint_tab_bar(node *b, HDC dc, gfx g, RECT r) {
	node *tabs = b->nav->tabs;
	float k = wh_scale_of(b);
	const palette *p = &H.theme;
	RECT rects[64];
	uint32_t n = tab_rects(b, rects, 64);
	if (g) {
		for (uint32_t i = 0; i < n; i++) {
			RECT tr = rects[i];
			if ((int)i == b->nav->hot) g_fill_round_rect(g, rgba_argb(p->subtle_hot), (float)tr.left, (float)tr.top, (float)(tr.right - tr.left), (float)(tr.bottom - tr.top), 4 * k);
			node *s = tabs->kids[i];
			if (s->badge.n) wh_paint_badge(b, NULL, g, tab_badge(b, s, tr), &s->badge);
			if ((int)i == tabs->selected) {
				float pw = 16 * k, ph = 3 * k, cx = (float)(tr.left + tr.right) / 2;
				g_fill_round_rect(g, rgba_argb(p->accent), cx - pw / 2, (float)tr.bottom - ph, pw, ph, ph / 2);
				if (b->focused && wh_keyboard_cues(b->hwnd))
					g_stroke_round_rect(g, rgba_argb(p->label), 2 * k, (float)tr.left, (float)tr.top, (float)(tr.right - tr.left), (float)(tr.bottom - tr.top), 4 * k);
			}
		}
		g_fill_rect(g, rgba_argb(p->separator), 0, (float)r.bottom - wh_maxf(k, 1), (float)r.right, wh_maxf(k, 1));
		return;
	}
	HFONT font = wh_body_font(b);
	SetBkMode(dc, TRANSPARENT);
	bool rtl = wh_is_rtl(b);
	for (uint32_t i = 0; i < n; i++) {
		node *s = tabs->kids[i];
		RECT tr = rects[i];
		SetTextColor(dc, rgba_ref(rgba_over((int)i == tabs->selected ? p->label : p->secondary, p->bg)));
		int x = tr.left + wh_px(b, 12);
		wchar_t glyph = wh_directed_glyph(s->icon, rtl);
		if (glyph) {
			HGDIOBJ old = SelectObject(dc, wh_icon_font(b, 16 * wh_font_scale()));
			RECT gr = {x, tr.top, x + wh_px(b, 16), tr.bottom};
			if (rtl) gr = flip_in(gr, tr);
			DrawTextW(dc, &glyph, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			SelectObject(dc, old);
			x += wh_px(b, 16 + 8);
		}
		HGDIOBJ old = SelectObject(dc, font);
		RECT lr = {x, tr.top, tr.right, tr.bottom};
		if (rtl) lr = flip_in(lr, tr);
		DrawTextW(dc, wstr_c(&s->title), s->title.n, &lr, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | wh_start_side(rtl));
		SelectObject(dc, old);
		if (s->badge.n) wh_paint_badge(b, dc, NULL, tab_badge(b, s, tr), &s->badge);
	}
}

// For the automation's tap: a tab, a header's bar button, a command bar's
// button showing text.
bool wh_tap_nav(const wchar_t *text) {
	uint32_t i = 0;
	void *v;
	node *tab = NULL;
	int tab_index = -1;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (!wh_on_screen(n)) continue;
		if (n->kind == AK_KIND_TABS)
			for (uint32_t k = 0; k < n->nkids; k++)
				if (n->kids[k]->title.n && !wcscmp(n->kids[k]->title.p, text)) tab = n, tab_index = (int)k;
		if (n->kind == KIND_COMMAND_BAR && wh_tap_command_bar(n, text)) return true;
		if (n->kind == KIND_HEADER && n->nav->screen) {
			node *s = n->nav->screen;
			for (uint32_t k = 0; k < s->acts.n && k < n->nav->nbuttons; k++)
				if (!wcscmp(s->acts.v[k].title, text)) {
					SendMessageW(n->nav->buttons[k], BM_CLICK, 0, 0);
					return true;
				}
		}
	}
	if (tab) {
		wh_select_tab(tab, tab_index);
		return true;
	}
	return false;
}

// ---- Badges: Fluent's InfoBadge (an accent pill, 16 points high, its
// value in caption text) ----

static HFONT badge_font(node *n) {
	font_spec s = {.size = 11 * wh_font_scale(), .k = wh_scale_of(n), .weight = 400};
	return wh_font(s);
}

void wh_badge_size(node *n, const wstr *badge, int *w, int *h) {
	*w = *h = 0;
	if (!badge->n) return;
	HDC dc = H.measure_dc;
	HGDIOBJ old = SelectObject(dc, badge_font(n));
	int tw = wh_text_extent(dc, badge->p, badge->n).cx;
	SelectObject(dc, old);
	*h = wh_px(n, 16 * wh_font_scale());
	*w = wh_maxi(*h, tw + wh_px(n, 8));
}

RECT wh_badge_at(node *n, const wstr *badge, int end, int top, int bottom, bool rtl) {
	int w, h;
	wh_badge_size(n, badge, &w, &h);
	int y = (top + bottom - h) / 2;
	return rtl ? (RECT){end, y, end + w, y + h} : (RECT){end - w, y, end, y + h};
}

void wh_paint_badge(node *n, HDC dc, gfx g, RECT r, const wstr *badge) {
	const palette *p = &H.theme;
	if (g) {
		float h = (float)(r.bottom - r.top);
		g_fill_round_rect(g, rgba_argb(p->accent), (float)r.left, (float)r.top, (float)(r.right - r.left), h, h / 2);
		return;
	}
	HGDIOBJ old = SelectObject(dc, badge_font(n));
	SetTextColor(dc, rgba_ref(rgba_over(p->on_accent, p->accent)));
	DrawTextW(dc, badge->p, badge->n, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old);
}

// ---- A screen's search field: in its stack's header, as WinUI puts an
// AutoSuggestBox in a page's command bar: the host's own TextField, up to
// 280 points wide, the search glyph inside it. Typing is the screen's
// TextChanged, Enter its Submit; SearchText sets its text. ----

#define SEARCH_WIDTH 280
#define SEARCH_ICON 32

static void search_inset(node *f, bool rtl) {
	f->inset[0] = f->inset[2] = 0;
	f->inset[1] = rtl ? 0 : SEARCH_ICON - 10;
	f->inset[3] = rtl ? SEARCH_ICON - 10 : 0;
}

void wh_update_search(node *b, node *screen) {
	bool want = screen && screen->nav->search.n;
	node *f = b->nav->field;
	if (!want) {
		if (f && f->hwnd) {
			if (GetFocus() == f->ctl) SetFocus(b->nav->stack->hwnd);  // the focus doesn't stay in a hidden field
			ShowWindow(f->hwnd, SW_HIDE);
		}
		return;
	}
	if (!f) {
		f = wh_node_new(wh_host_id(), AK_KIND_TEXT_FIELD);
		f->k = b->k, f->dir = b->dir;
		f->search_of = b;
		search_inset(f, wh_is_rtl(b));
		wh_make_field(f);
		wh_node_add(f);
		SetParent(f->hwnd, b->hwnd);
		b->nav->field = f;
	}
	if (f->placeholder.n != screen->nav->search.n || wcscmp(wstr_c(&f->placeholder), wstr_c(&screen->nav->search))) {
		wstr_setw(&f->placeholder, screen->nav->search.p, screen->nav->search.n);
		SendMessageW(f->ctl, EM_SETCUEBANNER, TRUE, (LPARAM)f->placeholder.p);
	}
	if (f->text.n != screen->nav->search_text.n || wcscmp(wstr_c(&f->text), wstr_c(&screen->nav->search_text))) {
		wstr_setw(&f->text, wstr_c(&screen->nav->search_text), screen->nav->search_text.n);
		f->setting_text = true;
		SetWindowTextW(f->ctl, wstr_c(&f->text));
		f->setting_text = false;
	}
	ShowWindow(f->hwnd, SW_SHOWNA);
}

// wh_layout_search places the field before the bar buttons, which end at
// x (pixels from the leading edge).
void wh_layout_search(node *b, int x, bool rtl, int width) {
	node *f = b->nav->field;
	if (!f || !f->hwnd) return;
	// The title keeps 120 points, after the back button.
	int start = wh_px(b, 16 + 120);
	if (wh_live_screens(b->nav->stack) > 1) start += wh_px(b, 36);
	int w = wh_mini(wh_px(b, SEARCH_WIDTH), x - wh_px(b, 4) - start);
	if (w < wh_px(b, 120)) w = wh_maxi(x - wh_px(b, 4) - wh_px(b, 52), 1);  // a narrow window: the field over the title
	int h = wh_px(b, 32);
	RECT r;
	GetClientRect(b->hwnd, &r);
	f->dir = b->dir;
	search_inset(f, rtl);
	int left = x - wh_px(b, 4) - w;
	SetWindowPos(f->hwnd, NULL, rtl ? width - left - w : left, (r.bottom - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	wh_layout_field(f);
}

void wh_paint_search_icon(node *f, HDC dc, RECT r) {
	bool rtl = wh_is_rtl(f);
	RECT gr = {r.right - wh_px(f, SEARCH_ICON), r.top, r.right - wh_px(f, 4), r.bottom};
	if (rtl) gr = flip_in(gr, r);
	HGDIOBJ old = SelectObject(dc, wh_icon_font(f, 14 * wh_font_scale()));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(H.theme.secondary));
	wchar_t g = 0xE721;  // Search
	DrawTextW(dc, &g, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old);
}

// wh_search_event is an event of a header's search field: typing is the
// header's screen's TextChanged, Enter its Submit.
void wh_search_event(node *f, ak_event *e) {
	node *screen = f->search_of->nav->screen;
	if (!screen) return;
	if (e->kind == AK_EVENT_TEXT_CHANGED) {
		wstr_set(&screen->nav->search_text, e->s);
		wh_emit(screen, e);
	} else if (e->kind == AK_EVENT_SUBMIT) {
		wh_emit(screen, e);
	}
}

// ---- Sheets ----

static RECT frame_rect(float w, float h, DWORD style, int dpi) {
	float k = (float)dpi / 96;
	RECT r = {0, 0, (LONG)lroundf(w * k), (LONG)lroundf(h * k)};
	wh_adjust_window_rect(&r, style, 0, dpi);
	return r;
}

// wh_top_sheet is the top sheet shown over window w, else w: what takes
// the user's input there.
HWND wh_top_sheet(HWND w) {
	for (bool over = true; over;) {
		over = false;
		for (uint32_t i = H.nsheets; i-- > 0;) {
			node *s = H.sheets[i];
			if (s->nav->owner == w && s->nav->window) {
				w = s->nav->window, over = true;
				break;
			}
		}
	}
	return w;
}

// wh_present_on is the window a sheet or an alert declared in window
// node id goes on: the main window's for the root, a Window node's own;
// over that, the top sheet shown on it. NULL: a Window not open yet.
HWND wh_present_on(uint32_t id) {
	HWND w = H.window;
	node *t = wh_node(id);
	if (t && t->kind == AK_KIND_WINDOW && !t->dying && !(w = t->nav->window)) return NULL;
	return wh_top_sheet(w);
}

// cover_rect is where a full-screen sheet over owner goes, in screen
// pixels: all of owner's client area under its title bar.
static RECT cover_rect(HWND owner) {
	RECT cr;
	GetClientRect(owner, &cr);
	int top = 0;
	wh_wctl *c = wh_ctl_of(owner);
	if (c) top = (int)lroundf(wh_safe_top(c) * (float)wh_dpi(owner) / 96);
	POINT a = {0, wh_mini(top, cr.bottom)}, b = {cr.right, cr.bottom};
	ClientToScreen(owner, &a);
	ClientToScreen(owner, &b);
	return (RECT){wh_mini(a.x, b.x), wh_mini(a.y, b.y), wh_maxi(a.x, b.x), wh_maxi(a.y, b.y)};
}

// round_cover gives a full-screen sheet its window's bottom corners
// (Windows 11 rounds a window's); none for a maximized or full-screen one.
static void round_cover(node *n) {
	HWND w = n->nav->window, owner = n->nav->owner;
	if (!w) return;
	wh_wctl *c = wh_ctl_of(owner);
	if (!wh_windows11() || IsZoomed(owner) || (c && c->full)) {
		SetWindowRgn(w, NULL, TRUE);
		return;
	}
	RECT r;
	GetWindowRect(w, &r);
	int d = (int)lroundf(16 * (float)wh_dpi(w) / 96);  // an 8 point radius, as DWM's
	SetWindowRgn(w, CreateRoundRectRgn(0, -d, r.right - r.left + 1, r.bottom - r.top + 1, d, d), TRUE);
}

void wh_fit_covers(HWND owner) {
	if (!owner || IsIconic(owner)) return;
	for (uint32_t i = 0; i < H.nsheets; i++) {
		node *s = H.sheets[i];
		if (!s->nav->full || s->nav->owner != owner || !s->nav->window) continue;
		RECT r = cover_rect(owner), now;
		GetWindowRect(s->nav->window, &now);
		if (!EqualRect(&r, &now)) SetWindowPos(s->nav->window, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

void wh_round_covers(HWND owner) {
	for (uint32_t i = 0; i < H.nsheets; i++)
		if (H.sheets[i]->nav->full && H.sheets[i]->nav->owner == owner) round_cover(H.sheets[i]);
}

// open_sheet shows a sheet as a modal window centered over the window
// it's declared in: as wide as fits (320 to 560 points), as tall as it
// less a margin, or half of it for a medium sheet.
static void open_sheet(node *n) {
	HWND owner = wh_present_on(n->nav->on);
	if (!owner) owner = H.window;  // its Window never opened
	int dpi = wh_dpi(owner);
	float k = (float)dpi / 96;
	RECT cr;
	GetClientRect(owner, &cr);
	float cw = (float)cr.right / k, ch = (float)cr.bottom / k;
	float w = wh_minf(wh_maxf(cw - 80, 320), 560), ht = wh_maxf(ch - 100, 240);
	if (n->detents == 1) ht = wh_maxf(ch / 2, 240);  // medium only
	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
	RECT r = frame_rect(w, ht, style, dpi), wr;
	GetWindowRect(owner, &wr);
	int x = wr.left + ((wr.right - wr.left) - (r.right - r.left)) / 2, y = wr.top + ((wr.bottom - wr.top) - (r.bottom - r.top)) / 2;
	if (n->nav->full) {
		// No frame: it's its window's content, in pixels where that is.
		style = WS_POPUP | WS_CLIPCHILDREN;
		r = cover_rect(owner);
		x = r.left, y = r.top;
	}
	HWND win = CreateWindowExW(wh_top_level_style(), WH_CLASS_WINDOW, H.app_namew, style, x, y, r.right - r.left, r.bottom - r.top, owner, NULL, GetModuleHandleW(NULL), NULL);
	int d = wh_dpi(win);
	if (d != dpi && !n->nav->full) {
		// It opened on a monitor of another DPI.
		dpi = d, k = (float)d / 96;
		r = frame_rect(w, ht, style, dpi);
		SetWindowPos(win, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	node *sw = wh_node_new(0, KIND_SHEET_WINDOW);
	sw->hwnd = win;
	sw->nav = nav_new();
	sw->parent = n;
	sw->k = k;
	wh_bind(sw, win);
	wh_accept_drops(win);
	wh_set_dark_title_bar(win, H.dark);
	n->nav->window = win, n->nav->owner = owner;
	n->presented = true;
	if (n->nav->full) {
		DWORD pref = 1;  // DWMWCP_DONOTROUND: its top meets its window's title bar
		DwmSetWindowAttribute(win, 33, &pref, sizeof pref);
		round_cover(n);
	}
	SetParent(n->hwnd, win);
	wh_rescale(n->hwnd, k);
	GetClientRect(win, &cr);
	SetWindowPos(n->hwnd, NULL, 0, 0, cr.right, cr.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	H.sheets = wh_realloc(H.sheets, (H.nsheets + 1) * sizeof *H.sheets);
	H.sheets[H.nsheets++] = n;
	wh_report_screen(n, wh_pt(n, cr.right), wh_pt(n, cr.bottom));
	EnableWindow(owner, FALSE);
	ShowWindow(win, wh_snapshotting() ? SW_SHOWNOACTIVATE : SW_SHOW);
}

static void fill(HDC dc, HWND w, rgba c) {
	RECT r;
	GetClientRect(w, &r);
	HBRUSH b = CreateSolidBrush(rgba_ref(c));
	FillRect(dc, &r, b);
	DeleteObject(b);
}

LRESULT wh_sheet_message(node *sw, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	node *n = sw->parent;
	*handled = true;
	if (m == WM_MOVE || m == WM_WINDOWPOSCHANGED) wh_fit_covers(sw->hwnd);
	switch (m) {
	case WM_SIZE:
		wh_fit_covers(sw->hwnd);
		if (n && n->nav->full) round_cover(n);
		if (n && n->hwnd) {
			RECT cr;
			GetClientRect(sw->hwnd, &cr);
			SetWindowPos(n->hwnd, NULL, 0, 0, cr.right, cr.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
			wh_report_screen(n, wh_pt(n, cr.right), wh_pt(n, cr.bottom));
		}
		wh_place_snacks();
		return 0;
	case WM_ERASEBKGND:
		fill((HDC)wp, sw->hwnd, H.theme.bg);
		return 1;
	case WM_PRINTCLIENT:
		fill((HDC)wp, sw->hwnd, H.theme.bg);
		return 0;
	case WM_CLOSE:
		wh_cancel_sheet(sw);
		return 0;
	case WM_DPICHANGED: {
		RECT r = *(RECT *)lp;
		if (n && n->nav->full) r = cover_rect(n->nav->owner);  // where its window's content is
		wh_window_dpi_changed(sw, LOWORD(wp), &r);
		return 0;
	}
	case WM_ACTIVATE:
		if (LOWORD(wp)) H.in_use = sw->hwnd;
		break;
	}
	*handled = false;
	return 0;
}

// wh_cancel_sheet is the user dismissing a sheet (its close button,
// Escape): Dismissed, and the core destroys it.
void wh_cancel_sheet(node *sw) {
	node *n = sw ? sw->parent : NULL;
	if (!n || n->dying) return;
	if (n->intercept_back) wh_emit_kind(n, AK_EVENT_BACK);
	else wh_emit_kind(n, AK_EVENT_DISMISSED);
}

void wh_close_sheet(node *n) {
	HWND win = n->nav->window;
	if (!win) return;
	for (uint32_t i = 0; i < H.nsheets; i++)
		if (H.sheets[i] == n) {
			memmove(H.sheets + i, H.sheets + i + 1, (H.nsheets - i - 1) * sizeof *H.sheets);
			H.nsheets--;
			break;
		}
	// The owner comes back before the sheet goes, so it's activated.
	EnableWindow(n->nav->owner, TRUE);
	SetParent(n->hwnd, H.park);
	node *sw = wh_node_of(win);
	wh_forget_tree(win);
	DestroyWindow(win);
	if (sw && sw->kind == KIND_SHEET_WINDOW) {
		free(sw->nav);
		sw->nav = NULL;
		wh_free_node(sw);
	}
	n->nav->window = NULL;
}

// ---- Alerts: task dialogs ----

typedef struct {
	uint32_t id;
} alert_job;

static HRESULT CALLBACK task_callback(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data) {
	(void)wp, (void)lp;
	if (msg == TDN_CREATED || msg == TDN_DIALOG_CONSTRUCTED) {
		node *n = wh_node((uint32_t)data);
		if (n) n->nav->dialog = hwnd;
		if (n)
			for (uint32_t i = 0; i < n->acts.n; i++)
				if (n->acts.v[i].flags & AK_MENU_DISABLED)
					SendMessageW(hwnd, TDM_ENABLE_BUTTON, n->acts.v[i].role == AK_ALERT_BUTTON_ROLE_CANCEL ? IDCANCEL : 100 + (int)i, FALSE);
		wh_set_dark_title_bar(hwnd, H.dark);
	}
	if (msg == TDN_DESTROYED) {
		node *n = wh_node((uint32_t)data);
		if (n) n->nav->dialog = NULL;
	}
	return S_OK;
}

// show_alert shows an alert as a task dialog and reports the button
// chosen. It's a modal loop, so it runs once the core has returned.
static void show_alert(void *ctx) {
	uint32_t id = (uint32_t)(uintptr_t)ctx;
	node *n = wh_node(id);
	if (!n || n->dying) return;
	if (n->nav->fields_len >= 4 && wh_u32(n->nav->fields) > 0 && n->alert_style == AK_ALERT_STYLE_ALERT) {
		wh_open_alert_dialog(n);  // a task dialog holds no fields
		return;
	}
	uint32_t nb = n->acts.n;
	TASKDIALOG_BUTTON *buttons = wh_alloc((nb + 1) * sizeof *buttons);
	int cancel = -1;
	for (uint32_t i = 0; i < nb; i++) {
		if (n->acts.v[i].role == AK_ALERT_BUTTON_ROLE_CANCEL) cancel = (int)i;
		buttons[i].nButtonID = n->acts.v[i].role == AK_ALERT_BUTTON_ROLE_CANCEL ? IDCANCEL : 100 + (int)i;
		buttons[i].pszButtonText = n->acts.v[i].title;
	}
	if (!nb) {
		buttons[0] = (TASKDIALOG_BUTTON){100, wh_word("ok")};
		nb = 1;
	}
	TASKDIALOGCONFIG c = {sizeof c};
	c.dwFlags = TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
	if (cancel >= 0) c.dwFlags |= TDF_ALLOW_DIALOG_CANCELLATION;
	if (n->alert_style == AK_ALERT_STYLE_ACTION_SHEET) c.dwFlags |= TDF_USE_COMMAND_LINKS;
	if (H.rtl) c.dwFlags |= TDF_RTL_LAYOUT;
	HWND owner = wh_present_on(n->nav->on);  // the window it's declared in
	c.hwndParent = owner ? owner : H.window;
	c.pszWindowTitle = H.app_namew;
	wchar_t *title = _wcsdup(wstr_c(&n->title)), *content = _wcsdup(wstr_c(&n->message));
	c.pszMainInstruction = title;
	c.pszContent = content;
	c.cButtons = nb;
	c.pButtons = buttons;
	c.nDefaultButton = buttons[0].nButtonID;
	c.pfCallback = task_callback;
	c.lpCallbackData = id;
	int got = 0;
	H.modal++;
	HRESULT hr = wh_task_dialog(&c, &got);
	H.modal--;
	free(buttons);
	free(title);
	free(content);
	n = wh_node(id);
	if (!n) return;  // destroyed meanwhile: freed as it returned
	if (n->dying) {
		n->nav->dialog = NULL;
		return;
	}
	if (FAILED(hr)) got = 0;
	if (got == IDCANCEL && cancel >= 0) wh_emit_int(n, AK_EVENT_ACTION, cancel);
	else if (got >= 100 && (uint32_t)(got - 100) < n->acts.n) wh_emit_int(n, AK_EVENT_ACTION, got - 100);
	else wh_emit_kind(n, AK_EVENT_DISMISSED);
}

// ---- Presenting ----

static void open_sheet_later(void *ctx) {
	node *p = wh_node((uint32_t)(uintptr_t)ctx);
	if (p && !p->dying && !p->nav->window) open_sheet(p);
}

static void open_window_later(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (n && !n->dying) wh_open_window(n);
}

static void open_popover_later(void *ctx) {
	node *p = wh_node((uint32_t)(uintptr_t)ctx);
	if (p && !p->dying) wh_open_popover(p);
}

static void present_snack_later(void *ctx) {
	node *s = wh_node((uint32_t)(uintptr_t)ctx);
	if (s && !s->dying) wh_present_snack(s);
}

// wh_present shows a sheet, a Window, a popover, an alert or a snackbar.
void wh_present(node *n) {
	void *id = (void *)(uintptr_t)n->id;
	switch (n->kind) {
	case AK_KIND_SHEET:
		if (n->nav->window) return;
		// Its Window opens once the batch is applied: the sheet goes on it
		// then.
		if (!wh_present_on(n->nav->on)) wh_after_core(open_sheet_later, id);
		else open_sheet(n);
		break;
	case AK_KIND_WINDOW: {
		// A child of a Window comes before its parent in the batch that
		// opens them both: it opens once the batch is applied.
		ak_window_config c = ak_decode_window_config(n->nav->cfg, n->nav->cfg_len);
		node *p = wh_node(c.parent);
		if ((c.flags & (AK_WINDOW_CHILD | AK_WINDOW_FLYOUT)) && p && p->kind == AK_KIND_WINDOW && !p->nav->window) wh_after_core(open_window_later, id);
		else wh_open_window(n);
		break;
	}
	case AK_KIND_POPOVER:
		wh_after_core(open_popover_later, id);  // its anchor, made with it, is placed once the batch is
		break;
	case AK_KIND_ALERT:
		wh_after_core(show_alert, id);
		break;
	case AK_KIND_SNACKBAR:
		wh_after_core(present_snack_later, id);  // its window, opened in the same batch, shows once it's applied
		break;
	case AK_KIND_OVERLAY:
		wh_present_overlay(n);
		break;
	}
}

// ---- Windows (ui.Window): each a top-level window of its own ----

static void set_window_text(node *n) {
	if (!n->nav->window) return;
	wchar_t t[600];
	swprintf(t, WH_LEN(t), L"%ls%ls", n->nav->edited ? L"*" : L"", n->title.n ? n->title.p : H.app_namew);
	SetWindowTextW(n->nav->window, t);
}

HWND wh_parent_window(uint32_t id) {
	node *p = wh_node(id);
	if (p && p->kind == AK_KIND_WINDOW && p->nav && p->nav->window) return p->nav->window;
	return H.window;
}

// cascade places a window of w by h pixels down and right of the window
// in use (down and left, right to left), past the app's windows there,
// inside the work area.
static void cascade(int w, int h, int *x, int *y) {
	HWND from = wh_window_in_use();
	RECT wr, work = wh_work_area(from);
	GetWindowRect(from, &wr);
	int step = 32 * wh_dpi(from) / 96;
	*x = wr.left + step, *y = wr.top + step;
	int dx = step;
	if (H.rtl) *x = wr.right - step - w, dx = -step;
	for (int k = 0; k < 32; k++) {
		bool taken = false;
		RECT r;
		GetWindowRect(H.window, &r);
		taken = r.left == *x && r.top == *y;
		for (uint32_t i = 0; i < H.nwins && !taken; i++) {
			GetWindowRect(H.wins[i]->nav->window, &r);
			taken = r.left == *x && r.top == *y;
		}
		if (!taken) break;
		*x += dx, *y += step;
	}
	if (*x + w > work.right) *x = wh_maxi(work.left, work.right - w);
	*x = wh_maxi(*x, work.left);
	if (*y + h > work.bottom) *y = wh_maxi(work.top, work.bottom - h);
}

static void window_opened(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (!n || !n->nav->ctl) return;
	wh_wctl *c = n->nav->ctl;
	if (c->wait_fit) wh_fit_window(c, c->fit[0], c->fit[1]);  // in case no Fit came: it shows at its size
	wh_report_state(c);
}

// wh_open_window opens a Window's window with its options: its content's
// size (800 by 600 points if the app gives none), its abilities and title
// bar; where it was left (its StateKey), at its X and Y, centered, or
// cascaded from the window in use. A child is owned by its parent's
// window, a modal one disabling it. It shows once the main window does.
void wh_open_window(node *n) {
	if (n->nav->window) return;
	float w = n->nav->size[0] > 0 ? n->nav->size[0] : 800, ht = n->nav->size[1] > 0 ? n->nav->size[1] : 600;
	wh_wctl *c = wh_alloc(sizeof *c);
	c->n = n;
	wh_ctl_set_config(c, n->nav->cfg ? n->nav->cfg : (const uint8_t *)"", n->nav->cfg_len);
	c->key = c->cfg.state_key.len ? wh_strndup(c->cfg.state_key.p, c->cfg.state_key.len) : NULL;
	if (n->nav->fitted) c->fit[0] = w, c->fit[1] = ht, c->fitted = true;  // laid out before it opened
	c->wait_fit = wh_ctl_has(c, AK_WINDOW_FIT_CONTENT) && !c->fitted;
	DWORD style = wh_ctl_style(c);
	HWND from = wh_window_in_use();
	RECT r = frame_rect(w, ht, style, wh_dpi(from));
	int x, y;
	cascade(r.right - r.left, r.bottom - r.top, &x, &y);
	ak_flyout f;
	if (wh_ctl_has(c, AK_WINDOW_CHILD)) c->owner = wh_parent_window(c->cfg.parent);
	else if (wh_nav_flyout(n, &f) && wh_ctl_has(c, AK_WINDOW_FLYOUT) && !f.tray) c->owner = wh_parent_window(c->cfg.parent);  // above the window it hangs from
	const wchar_t *cls = wh_frameless(c) ? WH_CLASS_FLYOUT : WH_CLASS_WINDOW;
	HWND win = CreateWindowExW(wh_top_level_style() | wh_ex_style(c), cls, L"", style, x, y, r.right - r.left, r.bottom - r.top, c->owner, NULL, GetModuleHandleW(NULL), NULL);
	c->hwnd = win;
	node *fw = wh_node_new(0, KIND_WINDOW_FRAME);
	fw->hwnd = win;
	fw->nav = nav_new();
	fw->parent = n;
	fw->k = (float)wh_dpi(win) / 96;  // the DPI of the monitor it opened on
	c->frame = fw;
	wh_bind(fw, win);
	wh_map_put(&H.ctls, (uint64_t)(uintptr_t)win, c);
	n->nav->ctl = c;
	n->nav->window = win;
	set_window_text(n);
	wh_accept_drops(win);
	wh_set_dark_title_bar(win, H.dark);
	n->presented = true;
	SetParent(n->hwnd, win);
	wh_rescale(n->hwnd, fw->k);
	wh_configure(c);
	wh_set_window_menu(c);  // its own, if it shows the app's: before its size, which the bar takes from
	RECT cr;
	GetClientRect(win, &cr);
	SetWindowPos(n->hwnd, NULL, 0, 0, cr.right, cr.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	wh_size_content(c, w, ht);  // its frame as configured, at its DPI
	if (!wh_restore_placement(c)) {
		if (wh_ctl_has(c, AK_WINDOW_POSITIONED)) wh_move_to(c, c->cfg.x, c->cfg.y);
		else if (c->owner && wh_is_frameless(c->owner)) wh_center_over(c, c->owner);  // a dialog of a panel, over it
		else if (wh_ctl_has(c, AK_WINDOW_CENTERED)) wh_center(c, from);
	}
	H.wins = wh_realloc(H.wins, (H.nwins + 1) * sizeof *H.wins);
	H.wins[H.nwins++] = n;
	GetClientRect(win, &cr);
	wh_report_screen(n, wh_pt(n, cr.right), wh_pt(n, cr.bottom));
	if (wh_ctl_has(c, AK_WINDOW_MODAL) && c->owner) EnableWindow(c->owner, FALSE);
	if (H.started && !wh_ctl_has(c, AK_WINDOW_HIDDEN)) wh_show_ctl(c, true);
	wh_after_core(window_opened, (void *)(uintptr_t)n->id);
}

// wh_window_dpi_changed follows a Window or a sheet (f, its top-level
// window's node) to a monitor of another DPI: its views at the new scale,
// the size Windows suggests, its Environment reported again.
void wh_window_dpi_changed(node *f, int dpi, const RECT *s) {
	node *n = f->parent;
	f->k = (float)dpi / 96;
	if (n && n->hwnd) wh_rescale(n->hwnd, f->k);
	SetWindowPos(f->hwnd, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	if (n && n->hwnd) {
		RECT cr;
		GetClientRect(f->hwnd, &cr);
		SetWindowPos(n->hwnd, NULL, 0, 0, cr.right, cr.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
		wh_report_screen(n, wh_pt(n, cr.right), wh_pt(n, cr.bottom));
	}
	RedrawWindow(f->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

// wh_root_of is the node a top-level window of the app's shows: the root,
// a sheet, a Window.
node *wh_root_of(HWND w) {
	if (w == H.window) return H.root;
	node *f = wh_node_of(w);
	if (f && (f->kind == KIND_SHEET_WINDOW || f->kind == KIND_WINDOW_FRAME)) return f->parent;
	return NULL;
}

// wh_close_window closes a Window's window, and the sheets over it: its
// placement kept first, and a modal one's owner enabled again before it
// goes, so that the owner is activated.
void wh_close_window(node *n) {
	HWND win = n->nav->window;
	if (!win) return;
	wh_wctl *c = n->nav->ctl;
	for (uint32_t i = 0; i < H.nwins; i++)
		if (H.wins[i] == n) {
			memmove(H.wins + i, H.wins + i + 1, (H.nwins - i - 1) * sizeof *H.wins);
			H.nwins--;
			break;
		}
	for (uint32_t i = H.nsheets; i-- > 0;) {
		node *s = H.sheets[i];
		if (s->nav->owner == win) {
			wh_close_sheet(s);
			wh_emit_kind(s, AK_EVENT_DISMISSED);
		}
	}
	if (c) {
		wh_save_placement(c);
		if (wh_ctl_has(c, AK_WINDOW_MODAL) && c->owner && IsWindow(c->owner)) EnableWindow(c->owner, TRUE);
		n->nav->ctl = NULL;
	}
	// The window it belongs to (or the main one) is the one in use next,
	// not another app's; a panel or a tray icon's flyout belongs to none.
	if (GetForegroundWindow() == win && !wh_snapshotting() && !(c && wh_frameless(c) && !c->owner)) {
		HWND next = c && c->owner && c->owner != H.park && IsWindow(c->owner) ? c->owner : H.window;
		SetForegroundWindow(next);
	}
	SetParent(n->hwnd, H.park);
	node *fw = wh_node_of(win);
	wh_forget_tree(win);
	if (IsWindow(win)) DestroyWindow(win);
	if (c) wh_wctl_free(c);
	if (fw && fw->kind == KIND_WINDOW_FRAME) {
		free(fw->nav);
		fw->nav = NULL;
		wh_free_node(fw);
	}
	n->nav->window = NULL;
	if (H.in_use == win) H.in_use = NULL;
}

LRESULT wh_window_frame_message(node *fw, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	node *n = fw->parent;
	wh_wctl *c = wh_ctl_of(fw->hwnd);
	bool h;
	if (c) {
		LRESULT r = wh_ctl_message(c, m, wp, lp, &h);
		if (h) {
			*handled = true;
			return r;
		}
	}
	*handled = true;
	switch (m) {
	case WM_SIZE:
		if (n && n->hwnd) {
			RECT cr;
			GetClientRect(fw->hwnd, &cr);
			SetWindowPos(n->hwnd, NULL, 0, 0, cr.right, cr.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
			wh_report_screen(n, wh_pt(n, cr.right), wh_pt(n, cr.bottom));
		}
		wh_place_snacks();
		return 0;
	case WM_ERASEBKGND:
		fill((HDC)wp, fw->hwnd, wh_bg_of(fw->hwnd));
		return 1;
	case WM_PRINTCLIENT:
		fill((HDC)wp, fw->hwnd, wh_bg_of(fw->hwnd));
		return 0;
	case WM_DPICHANGED:
		wh_window_dpi_changed(fw, LOWORD(wp), (RECT *)lp);
		return 0;
	case WM_ACTIVATE:
		if (LOWORD(wp)) H.in_use = fw->hwnd;
		break;
	case WM_CLOSE:
		if (n && !n->dying) wh_emit_kind(n, AK_EVENT_DISMISSED);  // the core closes it
		return 0;
	}
	*handled = false;
	return 0;
}

// wh_set_window_title is the main window's title; "": the app's name.
void wh_set_window_title(const char *title, size_t len) {
	if (!len) {
		SetWindowTextW(H.window, H.app_namew);
		return;
	}
	wchar_t *w = wh_wide(title, (int)len);
	SetWindowTextW(H.window, w);
	free(w);
}

static void fit_main_later(void *ctx) {
	(void)ctx;
	wh_wctl *c = H.main_ctl;
	if (c->wait_fit) wh_fit_window(c, c->fit[0], c->fit[1]);
}

// wh_window_options applies WindowOptions to the main window, before it
// shows: its title, its least and most sizes, its abilities and title bar,
// its client size, centered in the work area or at its X and Y; where it
// was left (its StateKey, "main" by default) wins over them as it shows.
void wh_window_options(node *n, const uint8_t *args, size_t len) {
	wh_wctl *c = H.main_ctl;
	c->n = n;
	wh_ctl_set_config(c, args, len);
	if (wh_frameless(c) && !c->cfg.state_key.len) {
		free(c->key);
		c->key = NULL;  // a panel shows where the cursor is, unless the app keeps its place
	}
	if (wh_ctl_has(c, AK_WINDOW_FIT_CONTENT) && !c->fitted) {
		c->wait_fit = true;  // shown once its content is laid out
		wh_after_core(fit_main_later, NULL);
	}
	H.min_size[0] = c->cfg.min_width, H.min_size[1] = c->cfg.min_height;
	if (c->cfg.title.len) wh_set_window_title(c->cfg.title.p, c->cfg.title.len);
	wh_configure(c);
	RECT cr, wr;
	GetClientRect(H.window, &cr);
	float w = c->cfg.width > 0 ? c->cfg.width : wh_pt(NULL, cr.right), ht = c->cfg.height > 0 ? c->cfg.height : wh_pt(NULL, cr.bottom);
	wh_clamp_size(c, &w, &ht);
	// The frame as it is (a menu bar too, or no caption), around the new
	// client area.
	GetWindowRect(H.window, &wr);
	int fw = (wr.right - wr.left) - cr.right, fh = (wr.bottom - wr.top) - cr.bottom;
	int pw = wh_px(NULL, w) + fw, ph = wh_px(NULL, ht) + fh;
	RECT work = wh_work_area(H.window);
	ph = wh_mini(ph, work.bottom - work.top);
	pw = wh_mini(pw, work.right - work.left);
	SetWindowPos(H.window, NULL, work.left + ((work.right - work.left) - pw) / 2, work.top + ((work.bottom - work.top) - ph) / 2, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	if (wh_ctl_has(c, AK_WINDOW_POSITIONED)) wh_move_to(c, c->cfg.x, c->cfg.y);
	wh_environment_changed();  // a hidden title bar's room
}
