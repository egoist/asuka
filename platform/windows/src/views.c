// views.c: the views the core makes, their props, and the messages of a
// view of the host's own class.
#include "host.h"
#include "win.h"

#define SLIDER_STEPS 10000

// ---- Subclassing: one procedure, the handler in its data ----

typedef LRESULT (*sub_fn)(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK sub_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
	(void)id;
	return ((sub_fn)(void *)data)(hwnd, m, wp, lp);
}

void wh_subclass(HWND hwnd, LRESULT (*fn)(HWND, UINT, WPARAM, LPARAM)) { SetWindowSubclass(hwnd, sub_proc, 1, (DWORD_PTR)(void *)fn); }
void wh_unsubclass(HWND hwnd) { RemoveWindowSubclass(hwnd, sub_proc, 1); }
LRESULT wh_def_subclass(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) { return DefSubclassProc(hwnd, m, wp, lp); }

// ---- Making views ----

// wh_make_view makes a window of the host's own class, which draws
// itself. It waits in the parking window until it's inserted.
HWND wh_make_view(DWORD style) {
	return CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_NOPARENTNOTIFY, WH_CLASS_VIEW, L"",
		WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, H.park, NULL, GetModuleHandleW(NULL), NULL);
}

// wh_make_control makes a common control inside a view of the host's:
// the view is what the core places, and the control never changes parent
// (some controls send their notifications to the parent they were made in).
void wh_make_control(node *n, const wchar_t *cls, DWORD style, DWORD ex) {
	n->hwnd = wh_make_view(0);
	wh_make_control_in(n, cls, style, ex);
}

void wh_make_control_in(node *n, const wchar_t *cls, DWORD style, DWORD ex) {
	n->ctl = CreateWindowExW(ex, cls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, n->hwnd, (HMENU)1, GetModuleHandleW(NULL), NULL);
	if (!n->ctl) wh_fail("couldn't make a %s (%ls)", ak_kind_name(n->kind), cls);
	SendMessageW(n->ctl, WM_SETFONT, (WPARAM)wh_node_font(n), 0);
	// Focus rectangles only after the keyboard is used, as in dialogs.
	SendMessageW(n->ctl, WM_UPDATEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
}

static LRESULT slider_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	// Right to left the trackbar stays as it is, its position the value's
	// mirror, so its keys that mean the value's ends swap.
	if (m == WM_KEYDOWN && (wp == VK_HOME || wp == VK_END)) {
		node *n = wh_node_of(hwnd);
		if (n && wh_is_rtl(n)) wp = VK_HOME + VK_END - wp;
	}
	return wh_def_subclass(hwnd, m, wp, lp);
}

void wh_create(uint32_t id, uint16_t kind) {
	node *n = wh_node_new(id, kind);
	if (wh_create_nav(n)) {
		wh_node_add(n);
		return;
	}
	switch (kind) {
	case AK_KIND_VIEW: case AK_KIND_SELECTABLE: case AK_KIND_TEXT: case AK_KIND_SYMBOL: case AK_KIND_SHAPE:
	case AK_KIND_CANVAS: case AK_KIND_SHADER: case AK_KIND_PROGRESS: case AK_KIND_PROGRESS_RING:
	case AK_KIND_ACTIVITY: case AK_KIND_IMAGE:
		n->hwnd = wh_make_view(0);
		if (kind == AK_KIND_SHAPE) wh_shape_new(n);
		else if (kind == AK_KIND_PROGRESS) n->max = 1;
		else if (kind == AK_KIND_IMAGE) wh_image_new(n);
		else if (kind == AK_KIND_SHADER) wh_shader_new(n);
		break;
	case AK_KIND_SEGMENTED: case AK_KIND_PAGE_CONTROL:
		n->hwnd = wh_make_view(WS_TABSTOP);
		break;
	case AK_KIND_SPLIT:
		wh_make_split(n);
		break;
	case AK_KIND_ACCESSORY:
		// A field's bar over the keyboard: never shown, with no on-screen
		// keyboard to put it on (it waits in the parking window).
		n->hwnd = wh_make_view(0);
		break;
	case AK_KIND_BUTTON:
	case AK_KIND_COLOR_PICKER:
		wh_make_control(n, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, 0);
		break;
	case AK_KIND_TOGGLE:
		wh_make_control(n, L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP, 0);
		break;
	case AK_KIND_SLIDER:
		wh_make_control(n, TRACKBAR_CLASSW, TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH | WS_TABSTOP, 0);
		SendMessageW(n->ctl, TBM_SETRANGEMIN, 0, 0);
		SendMessageW(n->ctl, TBM_SETRANGEMAX, 0, SLIDER_STEPS);
		SendMessageW(n->ctl, TBM_SETLINESIZE, 0, SLIDER_STEPS / 100);
		SendMessageW(n->ctl, TBM_SETPAGESIZE, 0, SLIDER_STEPS / 10);
		n->max = 1;
		wh_subclass(n->ctl, slider_message);
		break;
	case AK_KIND_STEPPER:
		n->hwnd = wh_make_view(0);
		n->min = 0, n->max = 100, n->step = 1;
		for (int i = 0; i < 2; i++) {
			HWND b = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, n->hwnd, (HMENU)(INT_PTR)(i + 1), GetModuleHandleW(NULL), NULL);
			SendMessageW(b, WM_UPDATEUISTATE, MAKELONG(UIS_SET, UISF_HIDEFOCUS), 0);
			if (i == 0) n->ctl = b;
			else n->ctl2 = b;
		}
		wh_name_stepper(n);
		break;
	case AK_KIND_WHEEL:
	case AK_KIND_PICKER:
		wh_make_combo(n);  // drawn as WinUI's ComboBox (combo.c)
		if (kind == AK_KIND_PICKER) n->selected = -1;  // none, till the core says
		break;
	case AK_KIND_DATE_PICKER:
		n->hwnd = wh_make_view(0);
		wh_make_date_controls(n);
		break;
	case AK_KIND_DATE_RANGE_PICKER:
		n->hwnd = wh_make_view(0);
		wh_make_range_controls(n);  // two date pickers, or a month calendar (datepicker.c)
		break;
	case AK_KIND_CHIP:
		wh_make_chip(n);  // drawn as Fluent's toggle button, a pill (chip.c)
		break;
	case AK_KIND_DISCLOSURE:
		wh_make_disclosure(n);  // drawn as WinUI's Expander's header (chip.c)
		break;
	case AK_KIND_TEXT_FIELD:
		wh_make_field(n);
		break;
	case AK_KIND_SCROLL:
		n->hwnd = wh_make_view(0);
		n->min_zoom = n->max_zoom = n->zoom_scale = 1;  // its content doesn't zoom till MaxZoom is more than MinZoom
		break;
	case AK_KIND_LIST:
		wh_list_new(n);
		break;
	case AK_KIND_TABLE:
	case AK_KIND_OUTLINE:
		wh_table_new(n, kind == AK_KIND_OUTLINE);  // a ListView, a TreeView (table.c)
		break;
	case AK_KIND_RICH_TEXT:
		wh_make_rich(n);
		break;
	case AK_KIND_WEB_VIEW:
	case AK_KIND_VIDEO:
		wh_make_web(n);
		break;
	case AK_KIND_NATIVE:
		n->hwnd = wh_make_view(0);  // the app's window goes in it (native.c)
		break;
	case AK_KIND_OVERLAY:
		wh_make_overlay(n);  // over its root, once it's set (overlay.c)
		break;
	default:
		wh_fail("can't show %s yet", ak_kind_name(kind));
	}
	if (!n->hwnd) wh_fail("couldn't make a %s", ak_kind_name(kind));
	wh_node_add(n);
	wh_theme_control(n);
}

// wh_theme_control gives a control the system's dark or light visual
// style where it has one (scroll bars, a combo box's list's).
void wh_theme_control(node *n) {
	const wchar_t *theme = H.dark ? L"DarkMode_Explorer" : L"Explorer";
	switch (n->kind) {
	case AK_KIND_SCROLL:
	case AK_KIND_LIST:
		if (n->hwnd) {
			wh_allow_dark(n->hwnd, H.dark);
			SetWindowTheme(n->hwnd, theme, NULL);
		}
		break;
	case AK_KIND_WHEEL:
	case AK_KIND_PICKER:
		if (n->ctl) {
			wh_allow_dark(n->ctl, H.dark);
			SetWindowTheme(n->ctl, H.dark ? L"DarkMode_CFD" : L"", NULL);
			wh_style_combo_list(n);
			InvalidateRect(n->ctl, NULL, FALSE);
		}
		break;
	case AK_KIND_TEXT_FIELD:
		if (n->ctl && n->multiline) {
			wh_allow_dark(n->ctl, H.dark);
			SetWindowTheme(n->ctl, theme, NULL);
		}
		break;
	case AK_KIND_RICH_TEXT:
		if (n->ctl) {
			wh_allow_dark(n->ctl, H.dark);
			SetWindowTheme(n->ctl, theme, NULL);
			wh_rich_invalidate(n);
		}
		break;
	case AK_KIND_DATE_PICKER:
	case AK_KIND_DATE_RANGE_PICKER:
		wh_theme_date(n);
		break;
	case AK_KIND_TABLE:
	case AK_KIND_OUTLINE:
		wh_table_theme(n);
		break;
	}
}

void wh_redraw_control(node *n) {
	if (n->ctl) InvalidateRect(n->ctl, NULL, FALSE);
	if (n->ctl2) InvalidateRect(n->ctl2, NULL, FALSE);
}

// wh_apply_slider moves the trackbar to the value, in SLIDER_STEPS steps
// between Min and Max.
void wh_apply_slider(node *n) {
	double pos = n->max > n->min ? (n->value - n->min) / (n->max - n->min) * SLIDER_STEPS : 0;
	pos = wh_clampd(pos, 0, SLIDER_STEPS);
	if (wh_is_rtl(n)) pos = SLIDER_STEPS - pos;
	SendMessageW(n->ctl, TBM_SETPOS, TRUE, (LPARAM)lround(pos));
	InvalidateRect(n->ctl, NULL, FALSE);
}

void wh_slider_moved(node *n) {
	double pos = (double)SendMessageW(n->ctl, TBM_GETPOS, 0, 0);
	if (wh_is_rtl(n)) pos = SLIDER_STEPS - pos;
	double v = n->min + pos / SLIDER_STEPS * (n->max - n->min);
	InvalidateRect(n->ctl, NULL, FALSE);
	if (v == n->value) return;
	n->value = v;
	wh_emit_float(n, AK_EVENT_VALUE_CHANGED, v);
}

static void set_items(node *n, const ak_value *v) {
	wh_free_strings(n->items, n->nitems);
	n->nitems = wh_decode_strings(&n->items, (const uint8_t *)v->s.p, v->s.len);
}

void wh_set_own(node *n, uint16_t p, const ak_value *v) {
	switch (n->kind) {
	case AK_KIND_OVERLAY:
		wh_set_overlay_prop(n, p, v);
		break;
	case AK_KIND_VIEW:
		if (p == AK_PROP_VIEW_DROP_ACCEPT) {
			wh_free_cstrings(n->drop_accept, n->ndrop_accept);
			n->ndrop_accept = wh_decode_cstrings(&n->drop_accept, (const uint8_t *)v->s.p, v->s.len);
		} else if (p == AK_PROP_VIEW_DRAG_ITEMS) {
			free(n->drag_items);
			n->drag_items = v->s.len ? (uint8_t *)wh_strndup(v->s.p, v->s.len) : NULL;
			n->drag_items_len = v->s.len;
			// Items with none in them aren't a drag.
			if (n->drag_items_len >= 4 && wh_u32(n->drag_items) == 0) {
				free(n->drag_items);
				n->drag_items = NULL;
				n->drag_items_len = 0;
			}
		} else if (p == AK_PROP_VIEW_TAKES_CLICKS) {
			wh_set_takes_clicks(n, v->i != 0);
		} else if (p == AK_PROP_VIEW_PAN_AXIS) {
			n->pan_axis = (int32_t)v->i;
		} else if (p == AK_PROP_VIEW_PAN_MIN_DISTANCE) {
			n->pan_min = v->f > 0 ? (float)v->f : 0;
		} else if (p == AK_PROP_VIEW_PAN_SIMULTANEOUS) {
			n->pan_simul = v->i != 0;
		}
		break;
	case AK_KIND_TEXT:
		if (p == AK_PROP_TEXT_TEXT) {
			wstr_set(&n->text, v->s);
			wh_text_changed(n);
			wh_redraw(n);
		}
		break;
	case AK_KIND_BUTTON:
		if (p == AK_PROP_BUTTON_TITLE) {
			wstr_set(&n->title, v->s);
			SetWindowTextW(n->ctl, wstr_c(&n->title));
		} else if (p == AK_PROP_BUTTON_VARIANT) {
			n->variant = (int32_t)v->i;
		} else if (p == AK_PROP_BUTTON_ICON) {
			free(n->icon);
			n->icon = wh_strndup(v->s.p, v->s.len);
		}
		wh_redraw_control(n);
		break;
	case AK_KIND_TEXT_FIELD:
		wh_set_field(n, p, v);
		break;
	case AK_KIND_TOGGLE:
		if (p == AK_PROP_TOGGLE_ON) {
			n->on = v->i != 0;
			SendMessageW(n->ctl, BM_SETCHECK, n->on ? BST_CHECKED : BST_UNCHECKED, 0);
		} else if (p == AK_PROP_TOGGLE_CHECKBOX) {
			n->checkbox = v->i != 0;
		}
		wh_redraw_control(n);
		break;
	case AK_KIND_SLIDER:
		if (p == AK_PROP_SLIDER_VALUE) n->value = v->f;
		else if (p == AK_PROP_SLIDER_MIN) n->min = v->f;
		else if (p == AK_PROP_SLIDER_MAX) n->max = v->f;
		wh_apply_slider(n);
		break;
	case AK_KIND_STEPPER:
		if (p == AK_PROP_STEPPER_VALUE) n->value = v->f;
		else if (p == AK_PROP_STEPPER_MIN) n->min = v->f;
		else if (p == AK_PROP_STEPPER_MAX) n->max = v->f;
		else if (p == AK_PROP_STEPPER_STEP) n->step = v->f;
		break;
	case AK_KIND_SCROLL:
		if (p == AK_PROP_SCROLL_HORIZONTAL) {
			n->horizontal = v->i != 0;
			wh_update_scroll(n);
		} else if (p == AK_PROP_SCROLL_PAGING) {
			n->paging = v->i != 0;
			wh_hide_bars(n);
		} else if (p == AK_PROP_SCROLL_REFRESHABLE) {
			wh_set_refreshable(n, v->i != 0);
		} else if (p == AK_PROP_SCROLL_REFRESHING) {
			wh_set_refreshing(n, v->i != 0);
		} else if (p == AK_PROP_SCROLL_MIN_ZOOM || p == AK_PROP_SCROLL_MAX_ZOOM) {
			wh_set_zoom_bounds(n, p, (float)v->f);
		}
		break;
	case AK_KIND_SPLIT:
		wh_set_split_prop(n, p, v);
		break;
	case AK_KIND_SHADER:
		wh_set_shader_prop(n, p, v);
		break;
	case AK_KIND_CANVAS:
		if (p == AK_PROP_CANVAS_COMMANDS) {
			free(n->canvas);
			n->canvas = v->s.len ? (uint8_t *)wh_strndup(v->s.p, v->s.len) : NULL;
			n->canvas_len = v->s.len;
			wh_redraw(n);
		}
		break;
	case AK_KIND_PAGE_CONTROL:
		if (p == AK_PROP_PAGE_CONTROL_COUNT) n->pages = v->i > 0 ? (int)v->i : 0;
		else if (p == AK_PROP_PAGE_CONTROL_PAGE) n->selected = (int)v->i;
		wh_redraw(n);
		break;
	case AK_KIND_LIST:
		wh_list_set_prop(n, p, v);
		break;
	case AK_KIND_TABLE:
	case AK_KIND_OUTLINE:
		wh_table_set_prop(n, p, v);
		break;
	case AK_KIND_ACTIVITY:
		if (p == AK_PROP_ACTIVITY_ANIMATING) {
			n->animating = v->i != 0;
			wh_spin(n);
			wh_redraw(n);
		}
		break;
	case AK_KIND_PROGRESS:
		if (p == AK_PROP_PROGRESS_VALUE) n->value = v->f;
		else if (p == AK_PROP_PROGRESS_INDETERMINATE) {
			n->indeterminate = v->i != 0;
			wh_spin(n);
		}
		wh_redraw(n);
		break;
	case AK_KIND_PROGRESS_RING:
		if (p == AK_PROP_PROGRESS_RING_VALUE) {
			double old = n->value;
			n->value = wh_clampd(v->f, 0, 1);
			// The arc moves to it, as WinUI's ring eases between values; at
			// once when it isn't shown, or motion is reduced.
			if (H.env.env.reduce_motion || !n->hwnd || !IsWindowVisible(n->hwnd)) n->shown = n->value;
			wh_uia_range_changed(n, old * 100, n->value * 100);
		} else if (p == AK_PROP_PROGRESS_RING_INDETERMINATE) {
			n->indeterminate = v->i != 0;
		}
		wh_spin(n);
		wh_redraw(n);
		break;
	case AK_KIND_IMAGE:
		wh_set_image_prop(n, p, v);
		break;
	case AK_KIND_SYMBOL:
		if (p == AK_PROP_SYMBOL_NAME) {
			free(n->symbol);
			n->symbol = wh_strndup(v->s.p, v->s.len);
			if (v->s.len && !wh_symbol_glyph(n->symbol)) wh_warn_symbol(n->symbol);
			wh_redraw(n);
		}
		break;
	case AK_KIND_SEGMENTED:
		if (p == AK_PROP_SEGMENTED_SEGMENTS) set_items(n, v);
		else if (p == AK_PROP_SEGMENTED_SELECTED) n->selected = (int)v->i;
		wh_redraw(n);
		break;
	case AK_KIND_WHEEL:
	case AK_KIND_PICKER:
		if (p == AK_PROP_WHEEL_ITEMS || p == AK_PROP_PICKER_ITEMS) set_items(n, v);
		wh_set_combo_prop(n, p, v);
		break;
	case AK_KIND_DATE_PICKER:
		wh_set_date_prop(n, p, v);
		break;
	case AK_KIND_DATE_RANGE_PICKER:
		wh_set_range_prop(n, p, v);
		break;
	case AK_KIND_CHIP:
		wh_set_chip_prop(n, p, v);
		break;
	case AK_KIND_DISCLOSURE:
		wh_set_disclosure_prop(n, p, v);
		break;
	case AK_KIND_COLOR_PICKER:
		if (p == AK_PROP_COLOR_PICKER_VALUE) n->color_value = (ak_color)v->i;
		else if (p == AK_PROP_COLOR_PICKER_TITLE) wstr_set(&n->title, v->s);
		wh_redraw_control(n);
		break;
	case AK_KIND_RICH_TEXT:
		wh_rich_set_prop(n, p, v);
		break;
	case AK_KIND_SHAPE:
		wh_shape_set_prop(n, p, v);
		break;
	case AK_KIND_WEB_VIEW:
	case AK_KIND_VIDEO:
		wh_web_set_prop(n, p, v);
		break;
	case AK_KIND_NATIVE:
		wh_set_native_prop(n, p, v);
		break;
	default:
		wh_set_nav_prop(n, p, v);
	}
}

void wh_set_common(node *n, uint16_t p, const ak_value *v) {
	switch (p) {
	case AK_PROP_CONTENT_INSET:
		// Its content inside its padding and border: a text's, a field's, a
		// rich text's.
		memcpy(n->inset, v->edges, sizeof n->inset);
		if (n->rich) wh_rich_place(n);
		else if (n->ctl) wh_layout_field(n);
		wh_redraw(n);
		break;
	case AK_PROP_BACKGROUND:
		n->background = (ak_color)v->i;
		wh_redraw(n);
		break;
	case AK_PROP_CORNER_RADIUS:
		n->corner_radius = (float)v->f;
		wh_shape_clip(n);
		wh_redraw(n);
		break;
	case AK_PROP_CORNER_RADII:
		// Each corner its own (none: CornerRadius's).
		memcpy(n->corner_radii, v->edges, sizeof n->corner_radii);
		wh_shape_clip(n);
		wh_redraw(n);
		break;
	case AK_PROP_BORDER_WIDTH:
		n->border_width = (float)v->f;
		wh_redraw(n);
		break;
	case AK_PROP_BORDER_WIDTHS:
		memcpy(n->border_widths, v->edges, sizeof n->border_widths);
		wh_redraw(n);
		break;
	case AK_PROP_BORDER_STYLE:
		n->border_style = (int32_t)v->i;
		wh_redraw(n);
		break;
	case AK_PROP_LETTER_SPACING:
		n->letter_spacing = (float)v->f;
		wh_text_changed(n);
		if (n->rich) wh_rich_invalidate(n);
		break;
	case AK_PROP_LINE_HEIGHT:
		n->line_height = (float)v->f;
		wh_text_changed(n);
		if (n->rich) wh_rich_invalidate(n);
		break;
	case AK_PROP_MINIMUM_SCALE:
		n->min_scale = (float)v->f;
		wh_text_changed(n);
		break;
	case AK_PROP_TRUNCATION:
		n->truncation = (int32_t)v->i;
		wh_text_changed(n);
		break;
	case AK_PROP_FONT_TRAITS:
		n->font_traits = (int32_t)v->i;
		wh_apply_font(n);
		wh_text_changed(n);
		break;
	case AK_PROP_TEXT_SHADOW:
		// Drawn by its parent, under its glyphs (textlay.c).
		if (n->has_text_shadow) wh_invalidate_shadow(n);
		memset(n->text_shadow, 0, sizeof n->text_shadow);
		if (v->s.len >= 20) memcpy(n->text_shadow, v->s.p, 20);
		n->has_text_shadow = wh_u64(n->text_shadow) != 0;
		wh_text_changed(n);
		break;
	case AK_PROP_BORDER_COLOR:
		n->border_color = (ak_color)v->i;
		wh_redraw(n);
		break;
	case AK_PROP_OPACITY:
		// Faded with what's in it (picture.c); none hides it.
		n->opacity = (float)v->f;
		wh_apply_hidden(n);
		wh_update_picture(n);
		wh_redraw(n);
		break;
	case AK_PROP_HIDDEN:
		n->hidden = v->i != 0;
		wh_apply_hidden(n);
		break;
	case AK_PROP_CLIP:
		// Child windows always clip their children to their rectangle; a
		// rounded clip shapes the window (clip.c).
		n->clip = v->i != 0;
		wh_shape_clip(n);
		break;
	case AK_PROP_A11Y_LABEL:
		wstr_set(&n->a11y_label, v->s);
		if (n->ctl && (n->kind == AK_KIND_TEXT_FIELD || n->kind == AK_KIND_TOGGLE)) {
			// These controls keep their native provider, value and role.
			wh_set_accessible_name(n->ctl, wstr_c(&n->a11y_label));
			NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, n->ctl, OBJID_CLIENT, CHILDID_SELF);
		}
		if (n->hwnd && n->kind == AK_KIND_VIEW) SetWindowTextW(n->hwnd, wstr_c(&n->a11y_label));  // its accessible name
		if (n->kind == AK_KIND_CHIP) wh_chip_label(n);  // its remove button's name
		if (n->kind == AK_KIND_DATE_RANGE_PICKER) wh_name_range(n);  // its pickers'
		break;
	case AK_PROP_A11Y_HINT: case AK_PROP_A11Y_ROLE: case AK_PROP_A11Y_HIDDEN: case AK_PROP_A11Y_ACTIONS:
	case AK_PROP_A11Y_ADJUSTABLE:
		wh_set_a11y(n, p, v);
		break;
	case AK_PROP_COLOR:
		n->color = (ak_color)v->i;
		wh_redraw(n);
		if (n->has_text_shadow) wh_text_changed(n);
		if (n->kind == AK_KIND_BUTTON) wh_redraw_control(n);
		if (n->image) wh_image_tint_changed(n);
		break;
	case AK_PROP_TEXT_STYLE:
		n->text_style = (int32_t)v->i;
		wh_apply_font(n);
		break;
	case AK_PROP_FONT_SIZE:
		n->font_size = (float)v->f;
		wh_apply_font(n);
		break;
	case AK_PROP_FONT_WEIGHT:
		n->font_weight = (int32_t)v->i;
		wh_apply_font(n);
		break;
	case AK_PROP_FONT_FAMILY:
		free(n->font_family);
		n->font_family = v->s.len ? wh_strndup(v->s.p, v->s.len) : NULL;
		wh_apply_font(n);
		break;
	case AK_PROP_TEXT_ALIGN:
		n->text_align = (int32_t)v->i;
		if (n->rich) wh_rich_invalidate(n);
		if (n->kind == AK_KIND_TEXT_FIELD) wh_align_field(n);
		wh_redraw(n);
		break;
	case AK_PROP_LAYOUT_DIRECTION:
		wh_set_direction(n, (uint8_t)v->i);
		break;
	case AK_PROP_LINE_LIMIT:
		n->line_limit = (int)v->i;
		if (n->rich) wh_rich_invalidate(n);
		wh_redraw(n);
		break;
	case AK_PROP_DISABLED:
		n->disabled = v->i != 0;
		if (n->ctl) EnableWindow(n->ctl, !n->disabled);
		if (n->ctl2 && (n->kind == AK_KIND_DATE_PICKER || n->kind == AK_KIND_DATE_RANGE_PICKER || n->kind == AK_KIND_STEPPER)) EnableWindow(n->ctl2, !n->disabled);
		if (n->kind == AK_KIND_TEXT_FIELD) wh_suggest_disabled(n);
		wh_redraw(n);
		break;
	case AK_PROP_MENU:
		actions_decode(&n->menu, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_MENU_TRIGGER:
		n->menu_trigger = (int32_t)v->i;
		break;
	case AK_PROP_GRADIENT:
		free(n->gradient);
		n->gradient = NULL;
		n->gradient_len = 0;
		if (n->conic) GdipDisposeImage((GpImage *)n->conic);
		n->conic = NULL;
		if (v->s.len >= 4 && wh_u32((const uint8_t *)v->s.p) != 0) {
			n->gradient = (uint8_t *)wh_strndup(v->s.p, v->s.len);
			n->gradient_len = v->s.len;
		}
		wh_redraw(n);
		break;
	case AK_PROP_SHADOW:
		// Drawn by its parent, around its window.
		wh_invalidate_shadow(n);
		free(n->shadow);
		n->shadow = NULL;
		n->shadow_len = 0;
		if (v->s.len >= 20 && wh_u64((const uint8_t *)v->s.p) != 0) {
			n->shadow = (uint8_t *)wh_strndup(v->s.p, v->s.len);
			n->shadow_len = v->s.len;
		}
		wh_invalidate_shadow(n);
		break;
	case AK_PROP_TRANSITION:
		free(n->transition);
		n->transition = NULL;
		n->transition_len = 0;
		if (v->s.len >= 13) {
			n->transition = (uint8_t *)wh_strndup(v->s.p, v->s.len);
			n->transition_len = v->s.len;
		}
		break;
	case AK_PROP_MATCH:
		wh_set_match(n, v->s);
		break;
	case AK_PROP_TRANSFORM:
		wh_set_transform(n, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_MATERIAL:
		wh_set_material(n, (int32_t)v->i);
		break;
	case AK_PROP_SWIPE_ACTIONS:
	case AK_PROP_LEADING_SWIPE_ACTIONS:
	case AK_PROP_SELECTED:
		if (n->cell) wh_cell_set_prop(n, p, v);
		break;
	case AK_PROP_TOOLTIP:
		wstr_set(&n->tooltip, v->s);
		wh_set_tooltip(n, wstr_c(&n->tooltip));
		break;
	case AK_PROP_INTERCEPT_BACK:
		n->intercept_back = v->i != 0;  // ui.OnBack: the back button and Escape ask the core
		break;
	case AK_PROP_KEYS:
		wh_set_keys(n, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_FOCUSABLE:
		n->focusable = v->i != 0, n->focusable_set = true;
		wh_update_tab_stop(n);
		break;
	case AK_PROP_NO_FOCUS_ON_PRESS:
		n->no_focus_on_press = v->i != 0;
		break;
	case AK_PROP_NO_FOCUS_RING:
		n->no_focus_ring = v->i != 0;
		wh_redraw(n);
		if (n->ctl) wh_redraw_control(n);
		break;
	case AK_PROP_FOCUS_TRAP:
		n->focus_trap = v->i != 0;
		break;
	case AK_PROP_A11Y_CHECKED: case AK_PROP_A11Y_EXPANDED: case AK_PROP_A11Y_SELECTED: case AK_PROP_A11Y_VALUE:
	case AK_PROP_A11Y_RANGE: case AK_PROP_A11Y_MODAL: case AK_PROP_A11Y_CONTROLS: case AK_PROP_A11Y_LABELLED_BY:
	case AK_PROP_A11Y_DESCRIBED_BY:
		wh_set_a11y(n, p, v);
		break;
	case AK_PROP_CURSOR:
		wh_set_cursor_prop(n, (int32_t)v->i);  // the pointer over it (cursor.c)
		break;
	case AK_PROP_ROW_DATA:
		if (n->kind == KIND_TABLE_CELL) wh_table_row_data(n, v);  // the row's text, drawn by its table
		break;
	}
}

// wh_update_tab_stop makes a view of the host's drawing a tab stop while
// it takes the keyboard's focus: Focusable, pressable, adjustable, with
// actions. A control's own tab stop follows Focusable, where it's set.
void wh_update_tab_stop(node *n) {
	if (!n->hwnd) return;
	if (n->ctl) {
		if (!n->focusable_set || n->kind == AK_KIND_STEPPER || n->kind == AK_KIND_DATE_PICKER || n->kind == AK_KIND_DATE_RANGE_PICKER) return;
		LONG_PTR st = GetWindowLongPtrW(n->ctl, GWL_STYLE);
		SetWindowLongPtrW(n->ctl, GWL_STYLE, n->focusable ? st | WS_TABSTOP : st & ~(LONG_PTR)WS_TABSTOP);
		return;
	}
	switch (n->kind) {
	case AK_KIND_SEGMENTED: case AK_KIND_PAGE_CONTROL: case AK_KIND_LIST: case AK_KIND_OVERLAY:
		return;  // their own
	}
	if (n->nav || n->kind == AK_KIND_SPLIT) return;
	if (n->kind == AK_KIND_CHIP || n->kind == AK_KIND_DISCLOSURE) {
		// A control: a tab stop unless Focusable is false.
		LONG_PTR st = GetWindowLongPtrW(n->hwnd, GWL_STYLE);
		bool on = !(n->focusable_set && !n->focusable);
		SetWindowLongPtrW(n->hwnd, GWL_STYLE, on ? st | WS_TABSTOP : st & ~(LONG_PTR)WS_TABSTOP);
		return;
	}
	bool stop = n->focusable || (n->kind == AK_KIND_VIEW && (node_listening(n, AK_EVENT_PRESS) || n->na11y_actions)) || n->adjustable;
	if (n->focusable_set && !n->focusable) stop = false;  // focusable: false
	LONG_PTR st = GetWindowLongPtrW(n->hwnd, GWL_STYLE);
	SetWindowLongPtrW(n->hwnd, GWL_STYLE, stop ? st | WS_TABSTOP : st & ~(LONG_PTR)WS_TABSTOP);
}

// wh_takes_focus reports whether a view takes the keyboard's focus: a tab
// stop (its window, or its control).
bool wh_takes_focus(const node *n) {
	if (!n->hwnd || n->disabled) return false;
	if (GetWindowLongPtrW(n->hwnd, GWL_STYLE) & WS_TABSTOP) return true;
	return n->ctl && (GetWindowLongPtrW(n->ctl, GWL_STYLE) & WS_TABSTOP);
}

// wh_apply_hidden shows or hides n's window: Hidden, or no opacity.
void wh_apply_hidden(node *n) {
	if (!n->hwnd) return;
	bool hide = n->hidden || n->opacity <= 0.01f;
	if (n->nav && n->kind == AK_KIND_SCREEN) return;  // a stack shows its top screen, tabs their page
	if (hide) {
		if (n->tracking || n->space_down) {
			// A view that goes doesn't stay pressed.
			n->space_down = false;
			wh_cancel_press(n);
			if (GetCapture() == n->hwnd) {
				n->releasing = true;
				ReleaseCapture();
				n->releasing = false;
			}
		}
		ShowWindow(n->hwnd, SW_HIDE);
	} else {
		ShowWindow(n->hwnd, SW_SHOWNA);
	}
	if (n->pict) wh_place_picture(n);
	wh_overlays_dirty();  // an anchor hidden, an overlay's child
}

// wh_apply_font sets the font of what draws its own text from n's text
// props.
void wh_apply_font(node *n) {
	if (n->kind == AK_KIND_TEXT) wh_text_changed(n);
	if (n->rich) {
		wh_rich_invalidate(n);
	} else if (n->kind == AK_KIND_TEXT_FIELD || n->kind == AK_KIND_WHEEL || n->kind == AK_KIND_PICKER || n->kind == AK_KIND_DATE_PICKER ||
		n->kind == AK_KIND_DATE_RANGE_PICKER) {
		if (n->ctl) {
			SendMessageW(n->ctl, WM_SETFONT, (WPARAM)wh_node_font(n), TRUE);
			if (n->ctl2) SendMessageW(n->ctl2, WM_SETFONT, (WPARAM)wh_node_font(n), TRUE);
			if (n->kind == AK_KIND_DATE_PICKER || n->kind == AK_KIND_DATE_RANGE_PICKER) wh_theme_date(n);
			wh_layout_field(n);
		}
	} else if (n->kind == AK_KIND_BUTTON && n->ctl) {
		SendMessageW(n->ctl, WM_SETFONT, (WPARAM)wh_node_font(n), TRUE);
	} else if (n->table && n->kind != KIND_TABLE_CELL) {
		wh_table_rescale(n);
	}
	wh_redraw(n);
}

// ---- Direction ----

bool wh_reads_rtl(const node *n) {
	if (n->dir != AK_LAYOUT_DIRECTION_INHERIT) return n->dir == AK_LAYOUT_DIRECTION_RIGHT_TO_LEFT;
	return wh_first_strong_rtl(n->text.p, n->text.n, H.rtl);
}

// wh_first_strong_rtl reports whether text's first strong character reads
// right to left (Unicode's bidi rule P2), or none for text without one.
bool wh_first_strong_rtl(const wchar_t *s, int len, bool none) {
	for (int i = 0; i < len; i++) {
		uint32_t r = s[i];
		if (r >= 0xD800 && r < 0xDC00 && i + 1 < len) r = 0x10000 + ((r - 0xD800) << 10) + (s[++i] - 0xDC00);
		bool rtl_block = (r >= 0x0590 && r <= 0x08FF) || (r >= 0xFB1D && r <= 0xFDFF) || (r >= 0xFE70 && r <= 0xFEFF) ||
			(r >= 0x10800 && r <= 0x10FFF) || (r >= 0x1E800 && r <= 0x1EFFF);
		WORD type = 0;
		wchar_t c = r < 0x10000 ? (wchar_t)r : 0;
		bool letter = c ? (GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & C1_ALPHA)) : true;
		if (rtl_block && letter) return true;
		if (!rtl_block && letter) return false;
	}
	return none;
}

void wh_set_direction(node *n, uint8_t d) {
	bool before = wh_is_rtl(n);
	n->dir = d;
	wh_nav_set_direction(n, d);
	if (n->list) wh_list_set_direction(n, d);
	if (wh_is_rtl(n) != before) wh_direction_changed(n);
}

void wh_direction_changed(node *n) {
	if (n->kind == AK_KIND_TEXT_FIELD) wh_align_field(n);
	else if (n->kind == AK_KIND_WHEEL || n->kind == AK_KIND_PICKER) wh_remake_combo(n);
	else if (n->kind == AK_KIND_SLIDER) wh_apply_slider(n);
	else if (n->kind == AK_KIND_STEPPER) wh_layout_stepper(n);
	else if (n->kind == AK_KIND_CHIP) wh_layout_chip(n);
	else if (n->kind == AK_KIND_DATE_RANGE_PICKER) wh_layout_field(n);
	else if (n->kind == AK_KIND_SCROLL) wh_restart_scroll(n);
	else if (n->kind == AK_KIND_STACK) wh_layout_screens(n);
	else if (n->list) wh_list_rebind(n);
	else if (n->table && n->kind != KIND_TABLE_CELL) wh_table_direction(n);
	else if (n->rich) wh_rich_invalidate(n);
	if (n->bar) wh_bars_reset(n);
	wh_redraw(n);
}

// ---- Controls' notifications ----

void wh_control_command(node *n, HWND ctl, UINT code) {
	wh_focus_flush();  // a control's click focused it first
	switch (n->kind) {
	case AK_KIND_BUTTON:
		if (code == BN_CLICKED) {
			if (n->menu.n && n->menu_trigger == AK_MENU_TRIGGER_TAP) {
				wh_pop_up(n, n->hwnd, true);
				return;
			}
			wh_emit_kind(n, AK_EVENT_PRESS);
		}
		break;
	case AK_KIND_TOGGLE:
		if (code == BN_CLICKED) {
			n->on = SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
			InvalidateRect(ctl, NULL, FALSE);
			wh_emit_bool(n, AK_EVENT_TOGGLED, n->on);
		}
		break;
	case AK_KIND_STEPPER:
		if (code == BN_CLICKED) {
			double step = n->step ? n->step : 1;
			if (ctl == n->ctl) step = -step;
			double v = wh_clampd(n->value + step, n->min, n->max);
			if (v != n->value) {
				n->value = v;
				wh_emit_float(n, AK_EVENT_VALUE_CHANGED, v);
			}
		}
		break;
	case AK_KIND_COLOR_PICKER:
		if (code == BN_CLICKED) wh_pick_color(n);
		break;
	case AK_KIND_TEXT_FIELD:
		wh_field_command(n, ctl, code);
		break;
	case AK_KIND_WHEEL:
	case AK_KIND_PICKER:
		wh_combo_command(n, ctl, code);
		break;
	case KIND_HEADER:
		wh_header_command(n, ctl);
		break;
	case KIND_COMMAND_BAR:
		wh_command_bar_command(n, ctl);
		break;
	case KIND_ALERT_WINDOW:
		wh_alert_dialog_command(n, ctl, code);
		break;
	case AK_KIND_SNACKBAR:
		if (code == BN_CLICKED) wh_snack_clicked(n, ctl);
		break;
	}
}

LRESULT wh_notify(node *n, NMHDR *hdr, LPARAM lp, bool *handled) {
	*handled = false;
	switch (hdr->code) {
	case NM_CUSTOMDRAW:
		switch (n->kind) {
		case AK_KIND_BUTTON: case AK_KIND_COLOR_PICKER: case AK_KIND_STEPPER: case KIND_HEADER: case KIND_COMMAND_BAR:
		case KIND_ALERT_WINDOW: case AK_KIND_SNACKBAR:
			*handled = true;
			return wh_draw_button(n, (NMCUSTOMDRAW *)lp);
		case AK_KIND_TOGGLE:
			*handled = true;
			return wh_draw_toggle(n, (NMCUSTOMDRAW *)lp);
		case AK_KIND_SLIDER:
			*handled = true;
			return wh_draw_slider(n, (NMCUSTOMDRAW *)lp);
		}
		break;
	}
	if (n->table && n->kind != KIND_TABLE_CELL) return wh_table_notify(n, hdr, lp, handled);
	if (n->kind == AK_KIND_DATE_PICKER) return wh_date_notify(n, hdr, lp, handled);
	if (n->kind == AK_KIND_DATE_RANGE_PICKER) return wh_range_notify(n, hdr, lp, handled);
	if (n->kind == AK_KIND_TEXT_FIELD) return wh_field_notify(n, hdr, lp, handled);
	if (n->rich) return wh_rich_notify(n, hdr, lp, handled);
	return 0;
}

// wh_control_colors answers a control's WM_CTLCOLOR*: the colors of its
// text and background, which follow the theme.
HBRUSH wh_control_colors(node *n, HDC dc, UINT m) {
	(void)m;
	rgba bg;
	switch (n->kind) {
	case AK_KIND_TEXT_FIELD:
		bg = n->focused ? H.theme.field_focus_bg : H.theme.field_bg;
		if (n->plain) bg = wh_backdrop(n);  // the text alone, on what's behind it
		SetTextColor(dc, rgba_ref(rgba_over(wh_text_color(n, H.theme.label), bg)));
		SetBkColor(dc, rgba_ref(bg));
		return wh_brush(bg);
	case KIND_ALERT_WINDOW:
		return wh_alert_dialog_colors(dc);
	case AK_KIND_WHEEL:
	case AK_KIND_PICKER:
		bg = wh_combo_list_bg();
		SetTextColor(dc, rgba_ref(wh_combo_list_text()));
		SetBkColor(dc, rgba_ref(bg));
		return wh_brush(bg);
	}
	bg = wh_backdrop(n);
	SetBkColor(dc, rgba_ref(bg));
	SetTextColor(dc, rgba_ref(H.theme.label));
	return wh_brush(bg);
}

// wh_listen adds what an event needs on the native side.
void wh_listen(node *n, int e) {
	if (e == AK_EVENT_LOCATION) {
		wh_listen_location(n);  // where its window's router was left (commands.c)
		return;
	}
	if (!n->hwnd) return;
	if (n->table && n->kind != KIND_TABLE_CELL) wh_table_listen(n, e);
	if (e == AK_EVENT_WINDOW_STATE) {
		wh_listen_window(n);
		return;
	}
	if (e == AK_EVENT_PRESS && n->kind == AK_KIND_VIEW) {
		// A pressable view is a tab stop that Enter and Space press.
		wh_update_tab_stop(n);
	} else if (e == AK_EVENT_PINCH || e == AK_EVENT_ROTATE) {
		wh_allow_gestures(n);  // a touch screen's zoom and rotate (rotate is off by default)
	}
}

// ---- Presses ----

bool wh_pressable(const node *n) {
	return node_listening(n, AK_EVENT_PRESS) || node_listening(n, AK_EVENT_LONG_PRESS) || (n->menu.n > 0 && n->kind == AK_KIND_VIEW);
}

bool wh_tracks_press(const node *n) { return wh_pressable(n) || node_listening(n, AK_EVENT_PRESSED); }
bool wh_draggable(const node *n) { return n->drag_items_len > 0; }
bool wh_draws_states(const node *n) { return node_listening(n, AK_EVENT_PRESSED) || node_listening(n, AK_EVENT_HOVER); }

// wh_interactive: the view takes the mouse; the others let it through to
// what's under them (a text in a pressable row).
bool wh_interactive(const node *n) {
	if (n->kind == AK_KIND_OVERLAY) return wh_overlay_modal(n);  // presses go through it, but for its children's
	if (wh_tracks_press(n) || wh_draggable(n) || node_listening(n, AK_EVENT_PAN) || node_listening(n, AK_EVENT_PINCH) ||
		node_listening(n, AK_EVENT_ROTATE) || node_listening(n, AK_EVENT_SWIPE) || node_listening(n, AK_EVENT_PRESS_POINT) ||
		node_listening(n, AK_EVENT_DOUBLE_TAP) || (n->focusable && !n->ctl))
		return true;
	switch (n->kind) {
	case AK_KIND_SCROLL: case AK_KIND_LIST: case AK_KIND_SEGMENTED: case AK_KIND_TEXT_FIELD: case KIND_HEADER:
	case KIND_TAB_BAR: case KIND_SIDEBAR: case KIND_COMMAND_BAR: case AK_KIND_BUTTON: case AK_KIND_TOGGLE:
	case AK_KIND_SLIDER: case AK_KIND_STEPPER: case AK_KIND_WHEEL: case AK_KIND_PICKER: case AK_KIND_DATE_PICKER:
	case AK_KIND_COLOR_PICKER: case AK_KIND_WEB_VIEW: case AK_KIND_VIDEO: case AK_KIND_PAGE_CONTROL: case AK_KIND_TABLE:
	case AK_KIND_OUTLINE: case AK_KIND_CHIP: case AK_KIND_DISCLOSURE: case KIND_CHIP_REMOVE: case AK_KIND_DATE_RANGE_PICKER:
		return true;
	}
	return false;
}

void wh_set_pressed(node *n, bool on) {
	if (n->pressed == on) return;
	n->pressed = on;
	if (!wh_draws_states(n)) wh_redraw(n);
	wh_emit_bool(n, AK_EVENT_PRESSED, on);
}

// wh_cancel_press ends a press of the mouse without a Press: a pan, a long
// press or a drag took it, or it lost the capture.
void wh_cancel_press(node *n) {
	n->tracking = false;
	if (n->hwnd) KillTimer(n->hwnd, TIMER_LONG_PRESS);
	wh_set_pressed(n, false);
}

// wh_end_press ends a press that fires (Press, then Pressed false, as a
// tap reports them) or not. A menu the press shows comes after.
void wh_end_press(node *n, HWND hwnd, bool fire) {
	if (!fire || (n->menu.n && n->menu_trigger == AK_MENU_TRIGGER_TAP && !node_listening(n, AK_EVENT_PRESS))) wh_set_pressed(n, false);
	if (fire) wh_press(n, hwnd);
	wh_set_pressed(n, false);
}

void wh_press(node *n, HWND hwnd) {
	wh_focus_flush();  // the focus the press moved, reported before it
	if (n->menu.n && n->menu_trigger == AK_MENU_TRIGGER_TAP && !node_listening(n, AK_EVENT_PRESS)) {
		wh_pop_up(n, hwnd, false);
		return;
	}
	wh_emit_kind(n, AK_EVENT_PRESS);
}

void wh_long_press(node *n, HWND hwnd) {
	if (n->menu.n && n->menu_trigger == AK_MENU_TRIGGER_LONG_PRESS) {
		wh_pop_up(n, hwnd, false);
		return;
	}
	wh_emit_kind(n, AK_EVENT_LONG_PRESS);
}

static void secondary_click(node *n, HWND hwnd) {
	if (n->menu.n) {
		wh_pop_up(n, hwnd, false);
		return;
	}
	wh_emit_kind(n, AK_EVENT_LONG_PRESS);
}

static bool inside(HWND hwnd, POINT p) {
	RECT r;
	GetClientRect(hwnd, &r);
	return p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom;
}

// pan reports a drag as a pan (points, from where it started, on the
// screen: the view may move with it; velocity from the last move), and
// its end as a swipe when it was quick. p is in the view, s on the screen.
static void pan(node *n, int state, POINT p, POINT s) {
	int64_t now = wh_now_ns();
	float dt = (float)(now - n->pan_time) / 1e9f;
	ak_event e = {.kind = AK_EVENT_PAN};
	e.g.state = (uint8_t)state;
	e.g.x = wh_pt(n, p.x), e.g.y = wh_pt(n, p.y), e.g.scale = 1;
	e.g.translation_x = wh_pt(n, s.x - n->press_scr.x), e.g.translation_y = wh_pt(n, s.y - n->press_scr.y);
	if (dt > 0) e.g.velocity_x = wh_pt(n, s.x - n->drag_last.x) / dt, e.g.velocity_y = wh_pt(n, s.y - n->drag_last.y) / dt;
	n->drag_last = s, n->pan_time = now;
	ak_gesture g = e.g;
	wh_emit(n, &e);
	if (state == AK_GESTURE_STATE_ENDED && node_listening(n, AK_EVENT_SWIPE)) {
		const float fast = 300;  // points per second
		float dx = g.translation_x, dy = g.translation_y;
		int64_t dir = 0;
		if (fabsf(dx) > fabsf(dy) && dx > 30 && g.velocity_x > fast) dir = 1 << 0;        // right
		else if (fabsf(dx) > fabsf(dy) && dx < -30 && g.velocity_x < -fast) dir = 1 << 1;  // left
		else if (dy < -30 && g.velocity_y < -fast) dir = 1 << 2;                          // up
		else if (dy > 30 && g.velocity_y > fast) dir = 1 << 3;                            // down
		if (dir) wh_emit_int(n, AK_EVENT_SWIPE, dir);
	}
}

static bool pans(const node *n) { return node_listening(n, AK_EVENT_PAN) || node_listening(n, AK_EVENT_SWIPE); }

// scroll_around is the scroll view n is in (a list's row's: the list).
static node *scroll_around(node *n) {
	for (node *p = n->parent; p; p = p->parent) {
		if ((p->kind == AK_KIND_SCROLL && !p->paging) || p->list) return p;
		if (p->cell) return wh_cell_list(p);
	}
	return NULL;
}

// drag_scroll_by moves the content of the scroll view a drag of n
// scrolls with the pointer, from where it was last to s (on the screen).
static void drag_scroll_by(node *n, POINT s) {
	node *sc = n->drag_scroll ? wh_node(n->drag_scroll) : NULL;
	if (!sc || !sc->hwnd) return;
	float dx = wh_pt(sc, s.x - n->drag_last.x), dy = wh_pt(sc, s.y - n->drag_last.y);
	if (sc->list) wh_list_scroll(sc, wh_list_offset(sc) - dy);
	else wh_scroll_to_offset(sc, sc->scroll_x - dx, sc->scroll_y - dy);
}

// press_now fires a Press waiting for a double click's (another click,
// not a double one, came).
static void press_now(node *n, HWND hwnd) {
	if (!n->press_waiting) return;
	n->press_waiting = false;
	KillTimer(hwnd, TIMER_PRESS_WAIT);
	wh_press(n, hwnd);
}

// double_click takes a click's second (WM_LBUTTONDBLCLK: within the
// system's double-click time and distance of the first): a view that
// listens for DoubleTap reports it, and its first click's Press, waiting,
// goes; on a zoomable Scroll (or a view in it that doesn't press), the
// content zooms. False: it's a press like any.
static bool double_click(node *n, HWND hwnd, LPARAM lp) {
	if (n->disabled) return false;
	if (node_listening(n, AK_EVENT_DOUBLE_TAP)) {
		n->press_waiting = false;
		KillTimer(hwnd, TIMER_PRESS_WAIT);
		ak_event e = {.kind = AK_EVENT_DOUBLE_TAP, .x = wh_pt(n, GET_X_LPARAM(lp)), .y = wh_pt(n, GET_Y_LPARAM(lp))};
		wh_emit(n, &e);
		return true;
	}
	for (node *x = n; x && !x->cell; x = x->parent) {
		if (wh_zoom_double_click(x, hwnd, lp)) return true;
		if (wh_tracks_press(x) || node_listening(x, AK_EVENT_DOUBLE_TAP)) return false;
	}
	return false;
}

static void mouse_down(node *n, HWND hwnd, LPARAM lp) {
	if (n->kind == AK_KIND_SEGMENTED) {
		wh_select_segment(n, wh_segment_at(n, GET_X_LPARAM(lp)));
		SetFocus(hwnd);
		return;
	}
	if (n->kind == AK_KIND_PAGE_CONTROL) {
		wh_select_pip(n, wh_pip_at(n, GET_X_LPARAM(lp)));
		return;
	}
	press_now(n, hwnd);  // a click, not a double one, after a click waiting
	// A press focuses a view that takes the focus (a desktop's), unless
	// NoFocusOnPress; it reports where it began first.
	if (!n->disabled && !n->no_focus_on_press && wh_takes_focus(n) && !n->ctl && GetFocus() != hwnd) SetFocus(hwnd);
	if (!n->disabled && node_listening(n, AK_EVENT_PRESS_POINT)) {
		ak_event e = {.kind = AK_EVENT_PRESS_POINT, .x = wh_pt(n, GET_X_LPARAM(lp)), .y = wh_pt(n, GET_Y_LPARAM(lp))};
		wh_emit(n, &e);
	}
	if (n->disabled) return;
	bool armed = wh_list_arm(n, hwnd, lp);  // a row of a list whose rows move: a drag lifts it
	bool hand = wh_zoom_hand(n);            // a zoomed Scroll: a drag moves its content
	if (!wh_tracks_press(n) && !wh_draggable(n) && !pans(n) && !armed && !hand) return;
	SetCapture(hwnd);
	n->tracking = wh_tracks_press(n);
	n->long_fired = n->panning = false;
	n->press_at = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
	n->press_scr = n->press_at;
	ClientToScreen(hwnd, &n->press_scr);
	n->drag_last = n->press_scr, n->pan_time = wh_now_ns();
	n->drag_scroll = hand ? n->id : 0;
	if (node_listening(n, AK_EVENT_LONG_PRESS) || (n->menu.n && n->menu_trigger == AK_MENU_TRIGGER_LONG_PRESS)) SetTimer(hwnd, TIMER_LONG_PRESS, 500, NULL);
	wh_set_pressed(n, n->tracking);
}

static void mouse_move(node *n, HWND hwnd, WPARAM wp, LPARAM lp) {
	if (wh_pressable(n) && !n->hot) {
		n->hot = true;
		TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
		TrackMouseEvent(&t);
		if (!wh_draws_states(n)) wh_redraw(n);
	}
	if (GetCapture() != hwnd || !(wp & MK_LBUTTON)) return;
	POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, s = p;
	ClientToScreen(hwnd, &s);
	if (wh_list_lifted(n)) {
		wh_list_drag_to(n, s);  // a list's row, lifted: it follows the pointer
		return;
	}
	if (n->panning) {
		if (n->drag_scroll) drag_scroll_by(n, s);  // PanSimultaneous: the scroll view around it follows too
		pan(n, AK_GESTURE_STATE_CHANGED, p, s);
		return;
	}
	if (n->drag_scroll) {
		drag_scroll_by(n, s);
		n->drag_last = s;
		return;
	}
	int dpi = wh_dpi_of(n);
	int dx = abs(s.x - n->press_scr.x), dy = abs(s.y - n->press_scr.y);
	bool is_far = dx > wh_system_metric(SM_CXDRAG, dpi) || dy > wh_system_metric(SM_CYDRAG, dpi);
	if (is_far && wh_draggable(n)) {
		wh_list_disarm(n);
		wh_start_drag(n, hwnd);
		return;
	}
	if (pans(n)) {
		// It pans once the pointer is PanMinDistance away (else the
		// system's drag distance), along PanAxis: moved first along the
		// other axis, the drag is the scroll view's around it.
		float min = n->pan_min * wh_scale_of(n);
		bool begins = n->pan_min > 0 ? (float)hypot(dx, dy) >= min : is_far;
		if (begins) {
			wh_list_disarm(n);
			wh_cancel_press(n);
			bool along = n->pan_axis == AK_PAN_AXIS_HORIZONTAL ? dx >= dy : n->pan_axis == AK_PAN_AXIS_VERTICAL ? dy >= dx : true;
			node *sc = scroll_around(n);
			if (!along) {
				n->drag_scroll = sc ? sc->id : 0;
				drag_scroll_by(n, s);
				n->drag_last = s;
				return;
			}
			n->panning = true;
			n->drag_scroll = n->pan_simul && sc ? sc->id : 0;
			if (n->drag_scroll) drag_scroll_by(n, s);
			pan(n, AK_GESTURE_STATE_BEGAN, p, s);
			return;
		}
	} else if (is_far && wh_list_lift(n, s)) {
		return;  // its row lifted: the list has the drag
	}
	if (n->tracking) {
		bool in = inside(hwnd, p);
		if (!in) KillTimer(hwnd, TIMER_LONG_PRESS);
		wh_set_pressed(n, in);
	}
}

static void mouse_up(node *n, HWND hwnd, LPARAM lp) {
	if (wh_list_mouse_up(n, hwnd)) return;  // a lifted row dropped
	bool pressed = n->tracking && n->pressed, panning = n->panning;
	n->tracking = n->panning = false;
	n->drag_scroll = 0;
	if (GetCapture() == hwnd) {
		n->releasing = true;
		ReleaseCapture();
		n->releasing = false;
	}
	KillTimer(hwnd, TIMER_LONG_PRESS);
	POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, s = p;
	ClientToScreen(hwnd, &s);
	if (panning) {
		pan(n, AK_GESTURE_STATE_ENDED, p, s);
		return;
	}
	bool fire = pressed && !n->long_fired && wh_pressable(n) && inside(hwnd, p);
	if (fire && node_listening(n, AK_EVENT_DOUBLE_TAP) && node_listening(n, AK_EVENT_PRESS)) {
		// Its Press waits till a second click can't come (the system's
		// double-click time): a double click is a DoubleTap alone.
		wh_set_pressed(n, false);
		n->press_waiting = true;
		SetTimer(hwnd, TIMER_PRESS_WAIT, GetDoubleClickTime(), NULL);
		return;
	}
	wh_end_press(n, hwnd, fire);
}

// wh_spin runs the timer of an activity ring, an indeterminate progress
// bar or ring, or a ring moving to its value, while it animates.
void wh_spin(node *n) {
	if (!n->hwnd) return;
	if (n->animating || n->indeterminate || (n->kind == AK_KIND_PROGRESS_RING && n->shown != n->value)) SetTimer(n->hwnd, TIMER_SPIN, 16, NULL);
	else KillTimer(n->hwnd, TIMER_SPIN);
}

static bool key(node *n, HWND hwnd, WPARAM vk) {
	if (wh_adjust_key(n, vk)) return true;
	if (n->kind == AK_KIND_SEGMENTED) {
		if (wh_is_rtl(n) && (vk == VK_LEFT || vk == VK_RIGHT)) vk = VK_LEFT + VK_RIGHT - vk;  // the arrows go the way they point
		if (vk == VK_LEFT || vk == VK_UP) {
			wh_select_segment(n, n->selected - 1);
			return true;
		}
		if (vk == VK_RIGHT || vk == VK_DOWN) {
			wh_select_segment(n, n->selected + 1);
			return true;
		}
		return false;
	}
	if (wh_pressable(n)) {
		if (vk == VK_RETURN) {
			wh_press(n, hwnd);  // Enter presses at once
			return true;
		}
		if (vk == VK_SPACE) {
			// Space presses while it's held, and fires as it's let go of.
			if (!n->space_down) {
				n->space_down = true;
				wh_set_pressed(n, true);
			}
			return true;
		}
		return false;
	}
	if (n->list || n->kind == AK_KIND_SCROLL) {
		RECT r;
		GetClientRect(hwnd, &r);
		float page = (float)r.bottom / wh_scale_of(n);
		switch (vk) {
		case VK_UP: wh_scroll_by(n, 0, -40); break;
		case VK_DOWN: wh_scroll_by(n, 0, 40); break;
		case VK_PRIOR: wh_scroll_by(n, 0, -page); break;
		case VK_NEXT: wh_scroll_by(n, 0, page); break;
		case VK_HOME: wh_scroll_by(n, 0, -1e9f); break;
		case VK_END: wh_scroll_by(n, 0, 1e9f); break;
		default: return false;
		}
		return true;
	}
	return false;
}

// key_text reports a character typed in n (the focus's) as KeyText, when
// it listens: a listbox's typeahead. A surrogate pair goes as one.
static bool key_text(node *n, WPARAM wp) {
	if (!node_listening(n, AK_EVENT_KEY_TEXT)) return false;
	wchar_t c = (wchar_t)wp;
	if (c >= 0xD800 && c <= 0xDBFF) {
		n->key_surrogate = c;
		return true;
	}
	if (c < 0x20 || c == 0x7F) return false;
	wchar_t s[2];
	int len = 0;
	if (c >= 0xDC00 && c <= 0xDFFF) {
		if (!n->key_surrogate) return true;
		s[len++] = n->key_surrogate;
	}
	n->key_surrogate = 0;
	s[len++] = c;
	wh_emit_wstr(n, AK_EVENT_KEY_TEXT, s, len);
	return true;
}

// wh_view_message handles the messages of a view of the host's class.
LRESULT wh_view_message(node *n, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = true;
	if (n->host_msg) {
		bool h = false;
		LRESULT r = n->host_msg(n, m, wp, lp, &h);
		if (h) return r;
	}
	switch (m) {
	case WM_GETOBJECT: {
		bool h = false;
		LRESULT r = wh_get_object(n, hwnd, wp, lp, &h);
		if (h) return r;
		break;
	}
	case WM_NCHITTEST:
		// Through to what's under it: under a hidden title bar, the
		// window's caption (a header's empty room too).
		if (!wh_interactive(n) || (n->kind == KIND_HEADER && wh_under_caption(hwnd))) return HTTRANSPARENT;
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		wh_paint_window(n, hwnd);
		return 0;
	case WM_PRINTCLIENT: {
		RECT r;
		GetClientRect(hwnd, &r);
		wh_paint(n, hwnd, (HDC)wp, r);
		return 0;
	}
	case WM_SIZE:
		if ((n->nav && wh_nav_sized_by_host(n)) || n == H.root) wh_fill_screens(n);
		if (n->clip) wh_shape_clip(n);
		wh_overlays_follow(n);  // the overlays over it, if it's a root
		if (n->kind == AK_KIND_STEPPER) wh_layout_stepper(n);
		else if (n->ctl) wh_layout_field(n);
		break;
	case WM_GESTURE:
		if (wh_touch_gesture(n, hwnd, lp)) return 0;
		break;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL: {
		int delta = GET_WHEEL_DELTA_WPARAM(wp);
		if (m == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) && (wh_wheel_pinch(n, hwnd, delta, lp) || wh_zoom_wheel(n, delta, lp))) return 0;
		if (n->kind == AK_KIND_SCROLL && n->paging) {
			if (wh_page_wheel(n, m == WM_MOUSEHWHEEL, delta)) return 0;
			break;
		}
		if ((n->kind == AK_KIND_SCROLL || n->list) && wh_wheel(n, m == WM_MOUSEHWHEEL, delta)) return 0;
		break;
	}
	case WM_VSCROLL:
	case WM_HSCROLL:
		if (!lp && (n->kind == AK_KIND_SCROLL || n->list)) {
			wh_scroll_bar_message(n, m == WM_HSCROLL, LOWORD(wp));
			return 0;
		}
		break;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (wh_select_click(n)) return 0;  // a row of a selecting list
		if (m == WM_LBUTTONDBLCLK && double_click(n, hwnd, lp)) return 0;
		mouse_down(n, hwnd, lp);
		return 0;
	case WM_MOUSEMOVE:
		mouse_move(n, hwnd, wp, lp);
		return 0;
	case WM_LBUTTONUP:
		mouse_up(n, hwnd, lp);
		return 0;
	case WM_MOUSELEAVE:
		if (n->hot) {
			n->hot = false;
			wh_redraw(n);
		}
		return 0;
	case WM_CAPTURECHANGED:
		if (wh_list_capture_lost(n, (HWND)lp)) return 0;  // a row lifted: put back
		if (n->tracking && (HWND)lp != hwnd && !n->releasing) wh_cancel_press(n);
		if ((HWND)lp != hwnd && !n->releasing) n->drag_scroll = 0;
		return 0;
	case WM_RBUTTONUP:
		if (wh_pressable(n)) {
			secondary_click(n, hwnd);
			return 0;
		}
		break;
	case WM_CONTEXTMENU:
		if (!n->menu.n && wh_actions_menu(n, hwnd, lp)) return 0;  // its screen reader actions
		if (wh_pressable(n)) {
			secondary_click(n, hwnd);
			return 0;
		}
		if (n->cell && wh_row_menu(n)) return 0;
		break;
	case WM_TIMER:
		switch (wp) {
		case TIMER_LONG_PRESS:
			KillTimer(hwnd, TIMER_LONG_PRESS);
			if (n->tracking && n->pressed) {
				n->long_fired = true;
				wh_cancel_press(n);
				wh_long_press(n, hwnd);
			}
			break;
		case TIMER_BARS:
			wh_hide_bars(n);
			break;
		case TIMER_SPIN:
			n->phase += 1.0 / 60 / 1.5;
			if (n->kind == AK_KIND_PROGRESS_RING && n->shown != n->value) {
				// Eases to the value: a quarter of the way each frame, then
				// there.
				n->shown += (n->value - n->shown) / 4;
				if (fabs(n->value - n->shown) < 0.002) n->shown = n->value;
				if (!n->indeterminate) wh_spin(n);  // stops once it's there
			}
			InvalidateRect(hwnd, NULL, FALSE);
			break;
		case TIMER_SHADER:
			wh_render_shader(n);
			break;
		case TIMER_PINCH:
			wh_end_wheel_pinch(n, hwnd);
			break;
		case TIMER_PRESS_WAIT:
			// No second click came: the first one's Press.
			KillTimer(hwnd, TIMER_PRESS_WAIT);
			if (n->press_waiting) {
				n->press_waiting = false;
				wh_press(n, hwnd);
			}
			break;
		case TIMER_ZOOM:
			wh_zoom_step(n);
			break;
		case TIMER_REORDER:
		case TIMER_REORDER_HOLD:
			wh_list_timer(n, wp);
			break;
		}
		return 0;
	case WM_SETFOCUS:
		n->focused = true;
		InvalidateRect(hwnd, NULL, FALSE);
		wh_focus_flush();
		break;
	case WM_KILLFOCUS:
		n->focused = false;
		if (n->space_down) {
			// Focus going while Space is held: no press.
			n->space_down = false;
			wh_set_pressed(n, false);
		}
		InvalidateRect(hwnd, NULL, FALSE);
		break;
	case WM_GETDLGCODE: {
		LRESULT code = 0;
		if (n->kind == AK_KIND_SEGMENTED || n->adjustable) code |= DLGC_WANTARROWS;
		if (lp) {
			MSG *msg = (MSG *)lp;
			if (msg->message == WM_KEYDOWN && (msg->wParam == VK_RETURN || msg->wParam == VK_SPACE) && (wh_pressable(n) || n->kind == AK_KIND_SEGMENTED))
				code |= DLGC_WANTALLKEYS;
			if (msg->message == WM_CHAR && msg->wParam == ' ' && wh_pressable(n)) code |= DLGC_WANTCHARS;  // Space's character: the press's
		}
		if (node_listening(n, AK_EVENT_KEY_TEXT)) code |= DLGC_WANTCHARS;  // typeahead
		return code;
	}
	case WM_KEYDOWN:
		if (key(n, hwnd, wp)) return 0;
		break;
	case WM_KEYUP:
		if (wp == VK_SPACE && n->space_down) {
			n->space_down = false;
			wh_end_press(n, hwnd, wh_pressable(n));  // Space let go of: the press fires
			return 0;
		}
		break;
	case WM_CHAR:
		if (wp == ' ' && wh_pressable(n)) return 0;
		if (key_text(n, wp)) return 0;
		break;
	case WM_UPDATEUISTATE:
		InvalidateRect(hwnd, NULL, FALSE);
		break;
	}
	*handled = false;
	return 0;
}

// ---- The color dialog ----

static COLORREF custom_colors[16];

static void pick_color_now(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (!n || !n->hwnd) return;
	rgba c = {0};
	wh_resolve(n->color_value, &c);
	CHOOSECOLORW cc = {sizeof cc};
	cc.hwndOwner = GetAncestor(n->hwnd, GA_ROOT);
	cc.rgbResult = rgba_ref(c);
	cc.lpCustColors = custom_colors;
	cc.Flags = CC_RGBINIT | CC_FULLOPEN;
	H.modal++;
	BOOL ok = ChooseColorW(&cc);
	H.modal--;
	if (!ok || !(n = wh_node((uint32_t)(uintptr_t)ctx))) return;
	ak_color v = ak_rgba(GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult), 255);
	n->color_value = v;
	wh_redraw_control(n);
	wh_emit_int(n, AK_EVENT_COLOR_CHANGED, (int64_t)v);
}

// wh_pick_color shows the color dialog for a color picker, once the core
// has returned (it's a modal loop).
void wh_pick_color(node *n) { wh_after_core(pick_color_now, (void *)(uintptr_t)n->id); }
