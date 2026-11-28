// suggest.c: a TextField's Suggestions (an autocomplete, a combo box), as
// GTK's own entry completion shows its list (GtkEmojiCompletion's way): a
// GtkPopover under the field, as wide as it, that neither hides itself
// on a click elsewhere nor takes the keyboard's focus, holding a
// GtkListView of the suggestions as the core sent them (already
// filtered: shown as they are, in order). It shows while the entry has
// the focus and the list isn't empty, after the user typed (not on the
// focus alone, nor after a choice until they type again). A key
// controller on the entry, ahead of its own, moves in it with Up and
// Down, chooses with Return (Enter), closes it with Escape; a click
// chooses. Choosing puts the suggestion in the field, the cursor at its
// end, without TextChanged, reports Suggestion with its index and closes
// the list. Dropdown: the entry's secondary icon (pan-down-symbolic, a
// combo box's) opens the list of all of them, typed or not, and closes
// it; so does Alt+Down (and Down while it's closed). Screen readers hear
// the entry as a list autocomplete with a popup, expanded while it shows.
#include "host.h"

#include <stdlib.h>

GtkWidget *lx_field_widget(node *n);
void lx_field_put_text(node *n, const char *s);

#define NONE 0xffffffffu  // GTK_INVALID_LIST_POSITION
#define MAX_HEIGHT 300

enum {
	A11Y_PROP_AUTOCOMPLETE = 0,
	A11Y_PROP_HAS_POPUP = 2,
	A11Y_STATE_EXPANDED = 3,
	A11Y_REL_CONTROLS = 5,
	A11Y_AUTOCOMPLETE_LIST = 2,
};

struct suggest {
	AK_VEC(char *) items;  // as last set
	GtkStringList *model;
	GtkSingleSelection *sel;
	GtkWidget *pop, *list;
	GtkWidget *entry;  // the entry it's attached to (a weak pointer: the field's)
	bool dropdown;
	bool typed;  // typed since the focus came or since a choice
};

static suggest *sug_of(node *n) { return n && n->kind == AK_KIND_TEXT_FIELD ? n->suggest : NULL; }

static bool showing(suggest *s) { return s->pop && gtk_widget_get_visible(s->pop); }

static bool entry_focused(suggest *s) {
	if (!s->entry) return false;
	GtkRoot *r = gtk_widget_get_root(s->entry);
	if (!r || !AK_IS(r, gtk_window_get_type)) return false;
	for (GtkWidget *f = gtk_window_get_focus((GtkWindow *)r); f; f = gtk_widget_get_parent(f))
		if (f == s->entry) return true;
	return false;
}

static void set_expanded(suggest *s, bool on) {
	if (!s->entry || !gtk_accessible_state_init_value) return;
	GtkAccessibleState st = (GtkAccessibleState)A11Y_STATE_EXPANDED;
	GValue v = {0};
	gtk_accessible_state_init_value(st, &v);
	if (v.g_type == G_TYPE_BOOLEAN) {
		g_value_set_boolean(&v, on);
		gtk_accessible_update_state_value((GtkAccessible *)s->entry, 1, &st, &v);
	}
	g_value_unset(&v);
}

static void hide(suggest *s) {
	if (showing(s)) gtk_popover_popdown((GtkPopover *)s->pop);
	set_expanded(s, false);
}

static void show(node *n, suggest *s) {
	if (!s->items.len || !s->entry || !n->widget || !gtk_widget_get_mapped(n->widget)) {
		hide(s);
		return;
	}
	int w = gtk_widget_get_width(n->widget);
	gtk_widget_set_size_request(s->pop, w > 0 ? w : -1, -1);
	if (!showing(s)) {
		gtk_single_selection_set_selected(s->sel, NONE);
		gtk_popover_popup((GtkPopover *)s->pop);
	}
	set_expanded(s, true);
}

// choose puts suggestion i in the field and reports it.
static void choose(node *n, suggest *s, guint i) {
	if (i >= s->items.len) return;
	char *t = strdup(s->items.data[i]);
	lx_field_put_text(n, t);
	free(t);
	s->typed = false;
	hide(s);
	lx_emit(n, (ak_event){.kind = AK_EVENT_SUGGESTION, .i = (int64_t)i});
}

static void move(suggest *s, int by) {
	guint count = s->items.len, at = gtk_single_selection_get_selected(s->sel);
	if (!count) return;
	guint next;
	if (at == NONE || at >= count)
		next = by > 0 ? 0 : count - 1;
	else
		next = (guint)(((int)at + by + (int)count) % (int)count);
	gtk_single_selection_set_selected(s->sel, next);
	if (gtk_list_view_scroll_to) gtk_list_view_scroll_to((GtkListView *)s->list, next, GTK_LIST_SCROLL_NONE, NULL);
}

static gboolean on_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, node *n) {
	(void)k;
	(void)keycode;
	suggest *s = sug_of(n);
	if (!s || n->dead || (state & (GDK_CONTROL_MASK | GDK_SUPER_MASK))) return 0;
	bool alt = (state & GDK_ALT_MASK) != 0;
	enum { UP = 0xff52, KP_UP = 0xff97, DOWN = 0xff54, KP_DOWN = 0xff99, RETURN = 0xff0d, KP_ENTER = 0xff8d, ISO_ENTER = 0xfe34, ESCAPE = 0xff1b };
	if (!showing(s)) {
		if ((keyval == DOWN || keyval == KP_DOWN) && s->items.len && (s->dropdown || (alt && s->typed))) {
			show(n, s);
			return 1;
		}
		return 0;
	}
	switch (keyval) {
	case DOWN:
	case KP_DOWN:
		if (alt) return 1;
		move(s, 1);
		return 1;
	case UP:
	case KP_UP:
		if (alt) {
			hide(s);
			return 1;
		}
		move(s, -1);
		return 1;
	case RETURN:
	case KP_ENTER:
	case ISO_ENTER: {
		guint at = gtk_single_selection_get_selected(s->sel);
		if (at == NONE || at >= s->items.len) {
			hide(s);
			return 0;  // the field's Submit
		}
		choose(n, s, at);
		return 1;
	}
	case ESCAPE:
		hide(s);
		return 1;
	}
	return 0;
}

static void on_activate_row(GtkListView *l, guint pos, node *n) {
	(void)l;
	suggest *s = sug_of(n);
	if (s && !n->dead) choose(n, s, pos);
}

static void on_icon(GtkEntry *e, GtkEntryIconPosition pos, node *n) {
	suggest *s = sug_of(n);
	if (!s || n->dead || pos != GTK_ENTRY_ICON_SECONDARY || !s->dropdown) return;
	if (showing(s)) {
		hide(s);
		return;
	}
	if (!entry_focused(s)) gtk_entry_grab_focus_without_selecting(e);
	show(n, s);
}

static void on_setup(GObject *f, GtkListItem *it, gpointer d) {
	(void)f;
	(void)d;
	GtkWidget *l = gtk_label_new(NULL);
	gtk_label_set_xalign((GtkLabel *)l, 0);
	gtk_label_set_ellipsize((GtkLabel *)l, PANGO_ELLIPSIZE_END);
	gtk_list_item_set_child(it, l);
	if (gtk_list_item_set_focusable) gtk_list_item_set_focusable(it, 0);
}

static void on_bind(GObject *f, GtkListItem *it, gpointer d) {
	(void)f;
	(void)d;
	GtkStringObject *o = gtk_list_item_get_item(it);
	gtk_label_set_text((GtkLabel *)gtk_list_item_get_child(it), o ? gtk_string_object_get_string(o) : "");
}

// make builds the popover and its list, in the field's view (which
// presents it as it's laid out).
static void make(node *n, suggest *s) {
	s->model = gtk_string_list_new(NULL);
	s->sel = gtk_single_selection_new((GListModel *)s->model);  // takes the model
	gtk_single_selection_set_autoselect(s->sel, 0);
	gtk_single_selection_set_can_unselect(s->sel, 1);
	GtkListItemFactory *f = gtk_signal_list_item_factory_new();
	g_signal_connect(f, "setup", G_CALLBACK(on_setup), NULL);
	g_signal_connect(f, "bind", G_CALLBACK(on_bind), NULL);
	s->list = gtk_list_view_new((GtkSelectionModel *)s->sel, f);  // takes both
	gtk_list_view_set_single_click_activate((GtkListView *)s->list, 1);
	gtk_widget_set_focusable(s->list, 0);
	gtk_widget_add_css_class(s->list, "asuka-suggestions");
	lx_connect(n, s->list, "activate", G_CALLBACK(on_activate_row));
	GtkWidget *sw = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy((GtkScrolledWindow *)sw, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height((GtkScrolledWindow *)sw, 1);
	gtk_scrolled_window_set_max_content_height((GtkScrolledWindow *)sw, MAX_HEIGHT);
	gtk_scrolled_window_set_child((GtkScrolledWindow *)sw, s->list);
	s->pop = gtk_popover_new();
	gtk_widget_add_css_class(s->pop, "menu");
	gtk_popover_set_autohide((GtkPopover *)s->pop, 0);
	gtk_popover_set_has_arrow((GtkPopover *)s->pop, 0);
	gtk_popover_set_position((GtkPopover *)s->pop, GTK_POS_BOTTOM);
	gtk_widget_set_can_focus(s->pop, 0);
	gtk_popover_set_child((GtkPopover *)s->pop, sw);
	gtk_widget_set_parent(s->pop, n->widget);
}

// lx_suggest_attach puts the list's keys, its dropdown's icon and what
// screen readers hear on the field's entry (made again as the field is).
void lx_suggest_attach(node *n) {
	suggest *s = sug_of(n);
	if (!s) return;
	GtkWidget *e = lx_field_widget(n);
	if (e && !AK_IS(e, gtk_entry_get_type)) e = NULL;  // a password's, a multiline field's: none
	if (e == s->entry) return;
	s->entry = e;
	hide(s);
	if (!e) return;
	GtkEventController *k = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
	lx_connect(n, k, "key-pressed", G_CALLBACK(on_key));
	gtk_widget_add_controller(e, k);
	lx_connect(n, e, "icon-press", G_CALLBACK(on_icon));
	gtk_entry_set_icon_from_icon_name((GtkEntry *)e, GTK_ENTRY_ICON_SECONDARY, s->dropdown ? "pan-down-symbolic" : NULL);
	if (s->dropdown) gtk_entry_set_icon_activatable((GtkEntry *)e, GTK_ENTRY_ICON_SECONDARY, 1);
	// A list autocomplete with a popup, the list it controls.
	GtkAccessibleProperty props[2] = {(GtkAccessibleProperty)A11Y_PROP_AUTOCOMPLETE, (GtkAccessibleProperty)A11Y_PROP_HAS_POPUP};
	GValue vals[2] = {{0}, {0}};
	gtk_accessible_property_init_value(props[0], &vals[0]);
	gtk_accessible_property_init_value(props[1], &vals[1]);
	if (vals[0].g_type && vals[0].g_type != G_TYPE_BOOLEAN && vals[1].g_type == G_TYPE_BOOLEAN) {
		if (vals[0].g_type == G_TYPE_INT)
			g_value_set_int(&vals[0], A11Y_AUTOCOMPLETE_LIST);
		else
			g_value_set_enum(&vals[0], A11Y_AUTOCOMPLETE_LIST);
		g_value_set_boolean(&vals[1], 1);
		gtk_accessible_update_property_value((GtkAccessible *)e, 2, props, vals);
	}
	g_value_unset(&vals[0]);
	g_value_unset(&vals[1]);
	if (gtk_accessible_update_relation_value && gtk_accessible_relation_init_value) {
		GtkAccessibleRelation r = (GtkAccessibleRelation)A11Y_REL_CONTROLS;
		GValue v = {0};
		gtk_accessible_relation_init_value(r, &v);
		if (v.g_type == G_TYPE_POINTER) {
			g_value_set_pointer(&v, g_list_prepend(NULL, s->list));  // GTK's to keep
			gtk_accessible_update_relation_value((GtkAccessible *)e, 1, &r, &v);
		}
		if (v.g_type) g_value_unset(&v);
	}
}

// lx_suggest_typed: the user changed the field's text: the list shows
// (the core's filtered suggestions follow).
void lx_suggest_typed(node *n) {
	suggest *s = sug_of(n);
	if (!s) return;
	s->typed = true;
	if (entry_focused(s)) show(n, s);
}

// lx_suggest_focus: the field took the focus (true) or lost it.
void lx_suggest_focus(node *n, bool in) {
	suggest *s = sug_of(n);
	if (!s) return;
	s->typed = false;
	if (!in) hide(s);
}

static void set_items(node *n, suggest *s, ak_span b) {
	for (uint32_t i = 0; i < s->items.len; i++) free(s->items.data[i]);
	s->items.len = 0;
	ak_str_vec ss = {0};
	ak_decode_strings((const uint8_t *)b.p, b.len, &ss);
	const char **add = calloc(ss.len + 1, sizeof *add);
	if (!add) abort();
	for (uint32_t i = 0; i < ss.len; i++) {
		char *t = str_dup(ss.data[i]);
		ak_vec_push(&s->items, t);
		add[i] = t;
	}
	ak_vec_free(&ss);
	guint had = g_list_model_get_n_items((GListModel *)s->model);
	gtk_string_list_splice(s->model, 0, had, add);  // in place, as it shows
	free(add);
	gtk_single_selection_set_selected(s->sel, NONE);
	if (!s->items.len)
		hide(s);
	else if (showing(s) || (s->typed && entry_focused(s)))
		show(n, s);
}

static bool suggest_prop(node *n, ak_prop p, const ak_value *v) {
	if (n->kind != AK_KIND_TEXT_FIELD || (p != AK_PROP_TEXT_FIELD_SUGGESTIONS && p != AK_PROP_TEXT_FIELD_DROPDOWN)) return false;
	suggest *s = n->suggest;
	if (!s) {
		s = n->suggest = calloc(1, sizeof *s);
		if (!s) abort();
		make(n, s);
		lx_suggest_attach(n);
	}
	if (p == AK_PROP_TEXT_FIELD_SUGGESTIONS) {
		set_items(n, s, v->s);
		return true;
	}
	s->dropdown = v->i != 0;
	if (s->entry) {
		gtk_entry_set_icon_from_icon_name((GtkEntry *)s->entry, GTK_ENTRY_ICON_SECONDARY, s->dropdown ? "pan-down-symbolic" : NULL);
		if (s->dropdown) gtk_entry_set_icon_activatable((GtkEntry *)s->entry, GTK_ENTRY_ICON_SECONDARY, 1);
	}
	if (!s->dropdown && showing(s) && !s->typed) hide(s);
	return true;
}

static void suggest_release(node *n) {
	suggest *s = n->suggest;
	if (!s) return;
	for (uint32_t i = 0; i < s->items.len; i++) free(s->items.data[i]);
	ak_vec_free(&s->items);
	free(s);  // its popover goes with the field's view
	n->suggest = NULL;
}

// suggest_dump writes the list: whether it shows, its items, the one the
// keys are on.
static void suggest_dump(node *n, int depth, ak_buf *b) {
	suggest *s = sug_of(n);
	if (!s) return;
	for (int i = 0; i <= depth; i++) ak_buf_put(b, "  ", 2);
	guint at = gtk_single_selection_get_selected(s->sel);
	ak_buf_printf(b, "suggestions shown=%d dropdown=%d selected=%d items=", showing(s) ? 1 : 0, s->dropdown ? 1 : 0, at == NONE ? -1 : (int)at);
	for (uint32_t i = 0; i < s->items.len; i++) {
		if (i) ak_buf_u8(b, ',');
		ak_fmt_quote(b, ak_cstr(s->items.data[i]));
	}
	ak_buf_u8(b, '\n');
}

const area lx_area_suggest = {
	.name = "suggest",
	.prop = suggest_prop,
	.release = suggest_release,
	.dump = suggest_dump,
};
