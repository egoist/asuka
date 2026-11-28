// a11y.c: screen readers (Orca) read GTK's accessibility tree over
// AT-SPI. A node's view takes its role (a pressable view a button, a
// header a heading), its label (A11yLabel; a text's text) and description
// (the hint), and is hidden with A11yHidden.
//
// A screen reader's actions on a view (A11yActions) and adjustable views
// (A11yAdjustable): GTK has no accessible actions of an app's own, but its
// AT-SPI bridge gives a widget that has action groups of its own the
// Action interface, listing their enabled actions without a parameter by
// their names. So a view's actions are an action group of its own,
// "action", each named after its title as GAction names may be ("action.
// Mark-as-read"): Orca lists them and does them. The view says it has a
// popup and its key ("Shift+F10"), and its actions are its context menu
// (a secondary click, a long press on a touchscreen, the Menu key,
// Shift+F10); the view is a tab stop. An adjustable view is a slider to
// screen readers and takes the arrow keys (up and the reading direction's
// forward one step up, the others down), each sending Adjust.
//
// RFC 0010's roles map to GtkAccessibleRole's (by their nicks), and its
// states to GTK's: A11yChecked to GTK_ACCESSIBLE_STATE_CHECKED (a
// checkable role's None as false), A11yExpanded to EXPANDED, A11ySelected
// to SELECTED, A11yValue and A11yRange to the VALUE_TEXT, VALUE_MIN,
// VALUE_MAX and VALUE_NOW properties, A11yModal to MODAL, and the views
// that control, label or describe another to the CONTROLS, LABELLED_BY
// and DESCRIBED_BY relations. GTK's AT-SPI bridge has no modal of its
// own, so while a modal view shows (A11yModal, or a Modal overlay) the
// views beside it and around it, up to its layout root and the overlays
// under it, are hidden from screen readers (HIDDEN), as aria-modal's
// inert rest is.
#include "host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

enum {
	A11Y_PROP_DESCRIPTION = 1,
	A11Y_PROP_HAS_POPUP = 2,
	A11Y_PROP_KEY_SHORTCUTS = 3,
	A11Y_PROP_LABEL = 4,
	A11Y_PROP_MODAL = 6,
	A11Y_PROP_VALUE_MAX = 15,
	A11Y_PROP_VALUE_MIN = 16,
	A11Y_PROP_VALUE_NOW = 17,
	A11Y_PROP_VALUE_TEXT = 18,
	A11Y_STATE_CHECKED = 1,
	A11Y_STATE_EXPANDED = 3,
	A11Y_STATE_HIDDEN = 4,
	A11Y_STATE_SELECTED = 7,
	A11Y_REL_CONTROLS = 5,
	A11Y_REL_DESCRIBED_BY = 6,
	A11Y_REL_LABELLED_BY = 10,
	A11Y_TRISTATE_FALSE = 0,
	A11Y_TRISTATE_TRUE = 1,
	A11Y_TRISTATE_MIXED = 2,
};

const char *lx_symbol_label(const char *symbol);
GtkWidget *lx_field_widget(node *n);
bool lx_view_is_pressable(node *n);

struct a11y_state {
	AK_VEC(char *) actions;
	AK_VEC(char *) names;  // the actions' GAction names
	bool adjustable;
	bool wired;  // its keys and gestures added (actions, adjustable)
	// RFC 0010's, as set.
	int32_t checked, expanded;
	bool selected, modal, has_range;
	char *value;
	double range[3];  // min, max, now
	ak_id rel[3];     // controls, labelled by, described by
};

static const int rel_kinds[3] = {A11Y_REL_CONTROLS, A11Y_REL_LABELLED_BY, A11Y_REL_DESCRIBED_BY};

void lx_set_role(gpointer w, const char *nick);
void lx_set_a11y_string(gpointer w, int prop, const char *s);

// a11y_get is n's state, made with nothing added to its view.
static a11y_state *a11y_get(node *n) {
	if (!n->a11y) {
		n->a11y = calloc(1, sizeof *n->a11y);
		if (!n->a11y) abort();
	}
	return n->a11y;
}

// lx_a11y_takes_focus reports whether n's screen readers' actions or
// adjustments make it a Tab stop.
bool lx_a11y_takes_focus(node *n);
bool lx_a11y_takes_focus(node *n) { return n->a11y && n->a11y->wired; }

// a11y_widget is the accessible that speaks for n: a field's or toggle's
// control, else its view.
static GtkWidget *a11y_widget(node *n) {
	if (n->kind == AK_KIND_TEXT_FIELD) {
		GtkWidget *f = lx_field_widget(n);
		if (f) return f;
	}
	if (n->kind == AK_KIND_TOGGLE && n->inner) return n->inner;
	return n->widget;
}

// target_widget is what a relation names for n: a text's label, a
// field's or toggle's control, else its view.
static GtkWidget *target_widget(node *n) {
	if (n->kind == AK_KIND_TEXT && n->inner) return n->inner;
	return a11y_widget(n);
}

static bool checkable(int32_t role) {
	switch (role) {
	case AK_ROLE_CHECKBOX:
	case AK_ROLE_RADIO:
	case AK_ROLE_SWITCH:
	case AK_ROLE_MENU_ITEM_CHECKBOX:
	case AK_ROLE_MENU_ITEM_RADIO:
		return true;
	}
	return false;
}

static bool selectable(int32_t role) {
	switch (role) {
	case AK_ROLE_TAB:
	case AK_ROLE_OPTION:
		return true;
	}
	return false;
}

// set_int puts v in a value GTK made for a state: a boolean or an int.
// GTK reads a tristate (checked) as an int though it makes its value an
// enum's: an int it is.
static void set_int(GValue *val, int v) {
	if (val->g_type == G_TYPE_BOOLEAN) {
		g_value_set_boolean(val, v != 0);
		return;
	}
	if (val->g_type != G_TYPE_INT) {
		if (val->g_type) g_value_unset(val);
		g_value_init(val, G_TYPE_INT);
	}
	g_value_set_int(val, v);
}

// update_state sets a state of w's to v (-1: none).
static void update_state(gpointer w, int st, int v) {
	GtkAccessibleState s = (GtkAccessibleState)st;
	if (v < 0) {
		if (gtk_accessible_reset_state) gtk_accessible_reset_state((GtkAccessible *)w, s);
		return;
	}
	GValue val = {0};
	if (gtk_accessible_state_init_value)
		gtk_accessible_state_init_value(s, &val);
	else
		g_value_init(&val, G_TYPE_BOOLEAN);
	set_int(&val, v);
	gtk_accessible_update_state_value((GtkAccessible *)w, 1, &s, &val);
	g_value_unset(&val);
}

static void update_double(gpointer w, int prop, double d) {
	GtkAccessibleProperty p = (GtkAccessibleProperty)prop;
	GValue val = {0};
	g_value_init(&val, G_TYPE_DOUBLE);
	g_value_set_double(&val, d);
	gtk_accessible_update_property_value((GtkAccessible *)w, 1, &p, &val);
	g_value_unset(&val);
}

static void update_bool_prop(gpointer w, int prop, bool b) {
	GtkAccessibleProperty p = (GtkAccessibleProperty)prop;
	GValue val = {0};
	g_value_init(&val, G_TYPE_BOOLEAN);
	g_value_set_boolean(&val, b);
	gtk_accessible_update_property_value((GtkAccessible *)w, 1, &p, &val);
	g_value_unset(&val);
}

// update_relation names the view (0: none) a relation of w's is to. The
// list given is GTK's to keep.
static void update_relation(gpointer w, int rel, ak_id id) {
	GtkAccessibleRelation r = (GtkAccessibleRelation)rel;
	node *t = id ? node_get(id) : NULL;
	GtkWidget *tw = t && !t->dead ? target_widget(t) : NULL;
	if (!tw) {
		if (gtk_accessible_reset_relation) gtk_accessible_reset_relation((GtkAccessible *)w, r);
		return;
	}
	if (!gtk_accessible_update_relation_value || !gtk_accessible_relation_init_value) return;
	GValue val = {0};
	gtk_accessible_relation_init_value(r, &val);
	if (val.g_type != G_TYPE_POINTER) {
		if (val.g_type) g_value_unset(&val);
		return;
	}
	g_value_set_pointer(&val, g_list_prepend(NULL, tw));
	gtk_accessible_update_relation_value((GtkAccessible *)w, 1, &r, &val);
	g_value_unset(&val);
}

static void apply_checked(node *n, a11y_state *a) {
	GtkWidget *w = a11y_widget(n);
	switch (a->checked) {
	case AK_A11Y_CHECKED_UNCHECKED: update_state(w, A11Y_STATE_CHECKED, A11Y_TRISTATE_FALSE); break;
	case AK_A11Y_CHECKED_CHECKED: update_state(w, A11Y_STATE_CHECKED, A11Y_TRISTATE_TRUE); break;
	case AK_A11Y_CHECKED_MIXED: update_state(w, A11Y_STATE_CHECKED, A11Y_TRISTATE_MIXED); break;
	default: update_state(w, A11Y_STATE_CHECKED, checkable(n->role) ? A11Y_TRISTATE_FALSE : -1);
	}
}

static void apply_selected(node *n, a11y_state *a) {
	update_state(a11y_widget(n), A11Y_STATE_SELECTED, a->selected ? 1 : selectable(n->role) ? 0 : -1);
}

// ---- Modal views ----

static AK_VEC(ak_id) modals;       // the views that were ever modal
static AK_VEC(ak_id) hidden_now;   // the views modal views hide
static AK_VEC(ak_id) hidden_next;  // scratch
static bool modal_dirty;

void lx_modal_changed(void) { modal_dirty = true; }

// apply_hidden hides n from screen readers while the app or a modal view
// says.
static void apply_hidden(node *n) {
	if (!n->widget) return;
	GValue val = {0};
	g_value_init(&val, G_TYPE_BOOLEAN);
	g_value_set_boolean(&val, n->a11y_hidden || n->modal_hidden > 0);
	GtkAccessibleState st = (GtkAccessibleState)A11Y_STATE_HIDDEN;
	gtk_accessible_update_state_value((GtkAccessible *)n->widget, 1, &st, &val);
	g_value_unset(&val);
}

static bool is_modal(node *n) {
	if (!n || n->dead || !n->widget || n->hidden) return false;
	if (n->kind == AK_KIND_OVERLAY) return lx_overlay_modal(n);
	return n->a11y && n->a11y->modal;
}

// hide_around adds what a modal view m hides: the views beside it and
// beside each view around it, up to its layout root; for an overlay, its
// root's views and the overlays under it, and on from its root.
static void hide_around(node *m) {
	for (node *v = m; v;) {
		node *p = v->parent;
		if (p) {
			for (uint32_t i = 0; i < p->children.len; i++)
				if (p->children.data[i] != v) ak_vec_push(&hidden_next, p->children.data[i]->id);
			v = p;
			continue;
		}
		if (v->kind != AK_KIND_OVERLAY || !v->widget) break;
		GtkWidget *rw = lx_overlay_root_widget(v);
		node *r = rw ? lx_view_node(rw) : NULL;
		if (!r || r == v) break;
		for (uint32_t i = 0; i < r->children.len; i++) ak_vec_push(&hidden_next, r->children.data[i]->id);
		for (GtkWidget *x = gtk_widget_get_prev_sibling(v->widget); x; x = gtk_widget_get_prev_sibling(x)) {
			node *o = lx_view_node(x);
			if (o && o->kind == AK_KIND_OVERLAY) ak_vec_push(&hidden_next, o->id);
		}
		v = r;
	}
}

static int by_id(const void *a, const void *b) {
	ak_id x = *(const ak_id *)a, y = *(const ak_id *)b;
	return x < y ? -1 : x > y;
}

// lx_modal_refresh hides from screen readers what the modal views showing
// hide, and shows again what they no longer do: after each batch, while
// any view is modal or was.
void lx_modal_refresh(void) {
	if (!modal_dirty && !modals.len && !hidden_now.len) return;
	modal_dirty = false;
	hidden_next.len = 0;
	uint32_t live = 0;
	for (uint32_t i = 0; i < modals.len; i++) {
		node *m = node_get(modals.data[i]);
		if (!m || m->dead) continue;
		modals.data[live++] = modals.data[i];
		if (is_modal(m)) hide_around(m);
	}
	modals.len = live;
	if (hidden_next.len) qsort(hidden_next.data, hidden_next.len, sizeof(ak_id), by_id);
	if (hidden_next.len == hidden_now.len && (!hidden_now.len || !memcmp(hidden_next.data, hidden_now.data, hidden_now.len * sizeof(ak_id)))) return;
	for (uint32_t i = 0; i < hidden_now.len; i++) {
		node *n = node_get(hidden_now.data[i]);
		if (n && n->modal_hidden) n->modal_hidden--;
	}
	for (uint32_t i = 0; i < hidden_next.len; i++) {
		node *n = node_get(hidden_next.data[i]);
		if (n) n->modal_hidden++;
	}
	for (uint32_t i = 0; i < hidden_now.len; i++) {
		node *n = node_get(hidden_now.data[i]);
		if (n && !n->dead) apply_hidden(n);
	}
	for (uint32_t i = 0; i < hidden_next.len; i++) {
		node *n = node_get(hidden_next.data[i]);
		if (n && !n->dead) apply_hidden(n);
	}
	// The next set is now's; now's memory the next scratch.
	ak_id *d = hidden_now.data;
	uint32_t cap = hidden_now.cap;
	hidden_now.data = hidden_next.data, hidden_now.len = hidden_next.len, hidden_now.cap = hidden_next.cap;
	hidden_next.data = d, hidden_next.len = 0, hidden_next.cap = cap;
}

// lx_modal_watch: n may be modal (an A11yModal view, an Overlay).
void lx_modal_watch(node *n) {
	for (uint32_t i = 0; i < modals.len; i++)
		if (modals.data[i] == n->id) return;
	ak_vec_push(&modals, n->id);
	modal_dirty = true;
}

static const char *const role_nicks[AK_ROLE_COUNT] = {
	[AK_ROLE_BUTTON] = "button",
	[AK_ROLE_HEADER] = "heading",
	[AK_ROLE_IMAGE] = "img",
	[AK_ROLE_LINK] = "link",
	[AK_ROLE_TEXT] = "label",
	[AK_ROLE_SEARCH] = "search",
	[AK_ROLE_CHECKBOX] = "checkbox",
	[AK_ROLE_RADIO] = "radio",
	[AK_ROLE_RADIO_GROUP] = "radio-group",
	[AK_ROLE_SWITCH] = "switch",
	[AK_ROLE_TAB] = "tab",
	[AK_ROLE_TAB_LIST] = "tab-list",
	[AK_ROLE_TAB_PANEL] = "tab-panel",
	[AK_ROLE_SLIDER] = "slider",
	[AK_ROLE_SPIN_BUTTON] = "spin-button",
	[AK_ROLE_PROGRESS_BAR] = "progress-bar",
	[AK_ROLE_METER] = "meter",
	[AK_ROLE_MENU] = "menu",
	[AK_ROLE_MENU_BAR] = "menu-bar",
	[AK_ROLE_MENU_ITEM] = "menu-item",
	[AK_ROLE_MENU_ITEM_CHECKBOX] = "menu-item-checkbox",
	[AK_ROLE_MENU_ITEM_RADIO] = "menu-item-radio",
	[AK_ROLE_LISTBOX] = "list-box",
	[AK_ROLE_OPTION] = "option",
	[AK_ROLE_COMBO_BOX] = "combo-box",
	[AK_ROLE_DIALOG] = "dialog",
	[AK_ROLE_ALERT_DIALOG] = "alert-dialog",
	[AK_ROLE_TOOLTIP] = "tooltip",
	[AK_ROLE_GROUP] = "group",
	[AK_ROLE_SEPARATOR] = "separator",
	[AK_ROLE_TOOLBAR] = "toolbar",
	[AK_ROLE_STATUS] = "status",
};


// lx_set_role sets a view's accessible role by its nick, while GTK still
// lets it (until the accessibility context is realized).
void lx_set_role(gpointer w, const char *nick) {
	GType t = gtk_accessible_role_get_type();
	GEnumClass *c = g_type_class_ref(t);
	GEnumValue *v = c ? g_enum_get_value_by_nick(c, nick) : NULL;
	if (v) {
		GValue val = {0};
		g_value_init(&val, t);
		g_value_set_enum(&val, v->value);
		g_object_set_property(w, "accessible-role", &val);
		g_value_unset(&val);
	}
	if (c) g_type_class_unref(c);
}

void lx_set_a11y_string(gpointer w, int prop, const char *s) {
	GValue val = {0};
	g_value_init(&val, G_TYPE_STRING);
	g_value_set_string(&val, s);
	GtkAccessibleProperty p = (GtkAccessibleProperty)prop;
	gtk_accessible_update_property_value((GtkAccessible *)w, 1, &p, &val);
	g_value_unset(&val);
}

void lx_set_a11y(node *n, ak_prop p, const ak_value *v) {
	if (!n->widget) return;
	switch (p) {
	case AK_PROP_A11Y_HINT: {
		char *s = str_dup(v->s);
		lx_set_a11y_string(n->widget, A11Y_PROP_DESCRIPTION, s);
		free(s);
		break;
	}
	case AK_PROP_A11Y_ROLE: {
		n->role = v->i > 0 && v->i < AK_ROLE_COUNT ? (int32_t)v->i : 0;
		const char *nick = role_nicks[n->role];
		if (nick) lx_set_role(a11y_widget(n), nick);
		// A checkable or selectable role's unset state says so.
		if (n->a11y || checkable(n->role) || selectable(n->role)) {
			apply_checked(n, a11y_get(n));
			apply_selected(n, n->a11y);
		}
		break;
	}
	case AK_PROP_A11Y_HIDDEN:
		n->a11y_hidden = v->i != 0;
		apply_hidden(n);
		break;
	case AK_PROP_A11Y_CHECKED:
		a11y_get(n)->checked = (int32_t)v->i;
		apply_checked(n, n->a11y);
		break;
	case AK_PROP_A11Y_EXPANDED: {
		a11y_state *a = a11y_get(n);
		a->expanded = (int32_t)v->i;
		update_state(a11y_widget(n), A11Y_STATE_EXPANDED, a->expanded == AK_A11Y_EXPANDED_EXPANDED ? 1 : a->expanded == AK_A11Y_EXPANDED_COLLAPSED ? 0 : -1);
		break;
	}
	case AK_PROP_A11Y_SELECTED:
		a11y_get(n)->selected = v->i != 0;
		apply_selected(n, n->a11y);
		break;
	case AK_PROP_A11Y_VALUE: {
		a11y_state *a = a11y_get(n);
		str_set(&a->value, v->s);
		if (*a->value)
			lx_set_a11y_string(a11y_widget(n), A11Y_PROP_VALUE_TEXT, a->value);
		else
			gtk_accessible_reset_property((GtkAccessible *)a11y_widget(n), (GtkAccessibleProperty)A11Y_PROP_VALUE_TEXT);
		break;
	}
	case AK_PROP_A11Y_RANGE: {
		a11y_state *a = a11y_get(n);
		GtkWidget *w = a11y_widget(n);
		a->has_range = v->s.len >= 24;
		if (!a->has_range) {
			static const int props[3] = {A11Y_PROP_VALUE_MIN, A11Y_PROP_VALUE_MAX, A11Y_PROP_VALUE_NOW};
			for (int i = 0; i < 3; i++) gtk_accessible_reset_property((GtkAccessible *)w, (GtkAccessibleProperty)props[i]);
			break;
		}
		ak_reader r = {(const uint8_t *)v->s.p, v->s.len, 0, false};
		for (int i = 0; i < 3; i++) a->range[i] = ak_read_f64(&r);
		update_double(w, A11Y_PROP_VALUE_MIN, a->range[0]);
		update_double(w, A11Y_PROP_VALUE_MAX, a->range[1]);
		update_double(w, A11Y_PROP_VALUE_NOW, a->range[2]);
		break;
	}
	case AK_PROP_A11Y_MODAL: {
		a11y_state *a = a11y_get(n);
		a->modal = v->i != 0;
		update_bool_prop(a11y_widget(n), A11Y_PROP_MODAL, a->modal);
		lx_modal_watch(n);
		break;
	}
	case AK_PROP_A11Y_CONTROLS:
	case AK_PROP_A11Y_LABELLED_BY:
	case AK_PROP_A11Y_DESCRIBED_BY: {
		int i = p == AK_PROP_A11Y_CONTROLS ? 0 : p == AK_PROP_A11Y_LABELLED_BY ? 1 : 2;
		a11y_state *a = a11y_get(n);
		a->rel[i] = (ak_id)v->i;
		update_relation(a11y_widget(n), rel_kinds[i], a->rel[i]);
		break;
	}
	}
}

// ---- What screen readers are told, read back (ASUKA_DUMP) ----

static bool state_is(GtkWidget *w, int st, int v) {
	char *e = gtk_test_accessible_check_state((GtkAccessible *)w, (GtkAccessibleState)st, v);
	if (e) g_free(e);
	return !e;
}

static bool prop_double_is(GtkWidget *w, int prop, double d) {
	char *e = gtk_test_accessible_check_property((GtkAccessible *)w, (GtkAccessibleProperty)prop, d);
	if (e) g_free(e);
	return !e;
}

// lx_a11y_dump writes what n's accessible tells screen readers of its
// role and RFC 0010's states, as GTK has them (read back with its test
// API): " a11y(...)" when the app set any.
void lx_a11y_dump(node *n, ak_buf *b) {
	if (!n->widget || !gtk_test_accessible_check_state || !gtk_accessible_get_accessible_role) return;
	GtkWidget *w = a11y_widget(n);
	GtkAccessible *acc = (GtkAccessible *)w;
	a11y_state *a = n->a11y;
	bool any = n->role || n->a11y_hidden || n->modal_hidden ||
	           (a && (a->checked || a->expanded || a->selected || a->modal || a->has_range || !str_empty(a->value) || a->rel[0] || a->rel[1] || a->rel[2]));
	if (!any) return;
	ak_buf_put(b, " a11y(", 6);
	GEnumClass *c = g_type_class_ref(gtk_accessible_role_get_type());
	GEnumValue *rv = c ? g_enum_get_value(c, gtk_accessible_get_accessible_role(acc)) : NULL;
	ak_buf_printf(b, "role=%s", rv ? rv->value_nick : "?");
	if (c) g_type_class_unref(c);
	if (gtk_test_accessible_has_state(acc, (GtkAccessibleState)A11Y_STATE_CHECKED)) {
		const char *s = state_is(w, A11Y_STATE_CHECKED, A11Y_TRISTATE_TRUE) ? "true" : state_is(w, A11Y_STATE_CHECKED, A11Y_TRISTATE_MIXED) ? "mixed" : "false";
		ak_buf_printf(b, " checked=%s", s);
	}
	if (gtk_test_accessible_has_state(acc, (GtkAccessibleState)A11Y_STATE_EXPANDED))
		ak_buf_printf(b, " expanded=%s", state_is(w, A11Y_STATE_EXPANDED, 1) ? "true" : "false");
	if (gtk_test_accessible_has_state(acc, (GtkAccessibleState)A11Y_STATE_SELECTED))
		ak_buf_printf(b, " selected=%s", state_is(w, A11Y_STATE_SELECTED, 1) ? "true" : "false");
	if (gtk_test_accessible_has_state((GtkAccessible *)n->widget, (GtkAccessibleState)A11Y_STATE_HIDDEN) && state_is(n->widget, A11Y_STATE_HIDDEN, 1))
		ak_buf_put(b, " hidden", 7);
	if (a && !str_empty(a->value) && gtk_test_accessible_has_property(acc, (GtkAccessibleProperty)A11Y_PROP_VALUE_TEXT)) {
		char *e = gtk_test_accessible_check_property(acc, (GtkAccessibleProperty)A11Y_PROP_VALUE_TEXT, a->value);
		if (!e) {
			ak_buf_put(b, " value=", 7);
			ak_fmt_quote(b, ak_cstr(a->value));
		}
		g_free(e);
	}
	if (a && a->has_range && gtk_test_accessible_has_property(acc, (GtkAccessibleProperty)A11Y_PROP_VALUE_NOW) && prop_double_is(w, A11Y_PROP_VALUE_MIN, a->range[0]) &&
	    prop_double_is(w, A11Y_PROP_VALUE_MAX, a->range[1]) && prop_double_is(w, A11Y_PROP_VALUE_NOW, a->range[2]))
		ak_buf_printf(b, " range=%g/%g/%g", a->range[0], a->range[1], a->range[2]);
	if (gtk_test_accessible_has_property(acc, (GtkAccessibleProperty)A11Y_PROP_MODAL)) {
		char *e = gtk_test_accessible_check_property(acc, (GtkAccessibleProperty)A11Y_PROP_MODAL, 1);
		if (!e) ak_buf_put(b, " modal", 6);
		g_free(e);
	}
	static const char *const rel_names[3] = {"controls", "labelledby", "describedby"};
	for (int i = 0; a && i < 3; i++)
		if (a->rel[i] && gtk_test_accessible_has_relation(acc, (GtkAccessibleRelation)rel_kinds[i])) ak_buf_printf(b, " %s=%u", rel_names[i], a->rel[i]);
	ak_buf_u8(b, ')');
}


// lx_update_a11y labels the native control, or the view for content the
// host draws.
void lx_update_a11y(node *n) {
	if (!n->widget) return;
	if (n->kind == AK_KIND_TEXT_FIELD || n->kind == AK_KIND_TOGGLE) {
		GtkWidget *w = n->kind == AK_KIND_TEXT_FIELD ? lx_field_widget(n) : n->inner;
		if (w) {
			if (str_empty(n->a11y_label))
				gtk_accessible_reset_property((GtkAccessible *)w, (GtkAccessibleProperty)A11Y_PROP_LABEL);
			else
				lx_set_a11y_string(w, A11Y_PROP_LABEL, n->a11y_label);
		}
		return;  // keep the control's native role; the wrapper has no label
	}
	const char *label = n->a11y_label;
	if (str_empty(label) && n->kind == AK_KIND_TEXT) return;  // the label inside says it
	if (str_empty(label) && n->kind == AK_KIND_SYMBOL) label = lx_symbol_label(n->symbol);
	if (!str_empty(label)) lx_set_a11y_string(n->widget, A11Y_PROP_LABEL, label);
	if (lx_view_is_pressable(n) && n->kind == AK_KIND_VIEW && !n->role) lx_set_role(n->widget, "button");
}

// lx_announce has a screen reader read text out.
void lx_announce(ak_span text) {
	if (!gtk_accessible_announce || !H.main) return;
	char *t = str_dup(text);
	gtk_accessible_announce((GtkAccessible *)H.main->window, t, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
	free(t);
}

// ---- Actions and adjustable views ----

static void free_names(a11y_state *a) {
	for (uint32_t i = 0; i < a->actions.len; i++) free(a->actions.data[i]);
	for (uint32_t i = 0; i < a->names.len; i++) free(a->names.data[i]);
	a->actions.len = a->names.len = 0;
}

void lx_release_a11y(node *n) {
	if (!n->a11y) return;
	free(n->a11y->value);
	free_names(n->a11y);
	ak_vec_free(&n->a11y->actions);
	ak_vec_free(&n->a11y->names);
	free(n->a11y);
	n->a11y = NULL;
}

static bool actions_menu(node *n, double x, double y);

static gboolean on_a11y_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, node *n) {
	(void)k;
	(void)keycode;
	a11y_state *a = n->a11y;
	if (a->adjustable && !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
		int64_t by = 0;
		switch (keyval) {
		case 0xff52: case 0xff97: by = 1; break;   // up
		case 0xff54: case 0xff99: by = -1; break;  // down
		case 0xff53: case 0xff98: by = n->rtl ? -1 : 1; break;  // right
		case 0xff51: case 0xff96: by = n->rtl ? 1 : -1; break;  // left
		}
		if (by) {
			lx_emit(n, (ak_event){.kind = AK_EVENT_ADJUST, .i = by});
			return 1;
		}
	}
	if (keyval == 0xff67 || (keyval == 0xffc7 && (state & GDK_SHIFT_MASK))) return actions_menu(n, -1, -1);  // Menu, Shift+F10
	return 0;
}

static void on_a11y_secondary(GtkGestureClick *g, int count, double x, double y, node *n) {
	(void)count;
	if (actions_menu(n, x, y)) gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void on_a11y_long(GtkGestureLongPress *g, double x, double y, node *n) {
	(void)g;
	actions_menu(n, x, y);
}

// a11y_of is n's state, its controllers added: a Tab stop (focus.c).
static a11y_state *a11y_of(node *n) {
	a11y_state *a = a11y_get(n);
	if (a->wired || !n->widget) return a;
	a->wired = true;
	GtkWidget *w = n->widget;
	lx_update_focusable(n);
	GtkEventController *keys = gtk_event_controller_key_new();
	lx_connect(n, keys, "key-pressed", G_CALLBACK(on_a11y_key));
	gtk_widget_add_controller(w, keys);
	GtkGesture *secondary = gtk_gesture_click_new();
	gtk_gesture_single_set_button((GtkGestureSingle *)secondary, 3);
	lx_connect(n, secondary, "pressed", G_CALLBACK(on_a11y_secondary));
	gtk_widget_add_controller(w, (GtkEventController *)secondary);
	GtkGesture *lng = gtk_gesture_long_press_new();
	gtk_gesture_single_set_touch_only((GtkGestureSingle *)lng, 1);
	lx_connect(n, lng, "pressed", G_CALLBACK(on_a11y_long));
	gtk_widget_add_controller(w, (GtkEventController *)lng);
	return a;
}

static gboolean unparent_later(gpointer w) {
	gtk_widget_unparent(w);
	return 0;
}

static void on_popup_closed(GtkPopover *pop, gpointer d) {
	(void)d;
	// Unparented once its action (which runs after it closes) has.
	g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, unparent_later, pop, NULL);
}

// lx_popup_at shows a popover menu of model on n's view: at x, y in it,
// or (x < 0, from the keyboard) under it.
void lx_popup_at(node *n, GMenuModel *model, double x, double y);
void lx_popup_at(node *n, GMenuModel *model, double x, double y) {
	GtkWidget *pop = gtk_popover_menu_new_from_model(model);
	gtk_widget_set_parent(pop, n->widget);
	if (x >= 0) {
		GdkRectangle r = {(int)x, (int)y, 1, 1};
		gtk_popover_set_pointing_to((GtkPopover *)pop, &r);
		gtk_popover_set_has_arrow((GtkPopover *)pop, 0);
		gtk_widget_set_halign(pop, n->rtl ? GTK_ALIGN_END : GTK_ALIGN_START);
	} else {
		gtk_popover_set_position((GtkPopover *)pop, GTK_POS_BOTTOM);
	}
	g_signal_connect(pop, "closed", G_CALLBACK(on_popup_closed), NULL);
	gtk_popover_popup((GtkPopover *)pop);
}

// actions_menu shows a view's actions as a popover menu. The action
// chosen sends A11yAction. A view with a context menu of its own shows
// that.
static bool actions_menu(node *n, double x, double y) {
	a11y_state *a = n->a11y;
	if (!a || !a->actions.len || !n->widget || n->has_menu) return false;
	GMenu *model = g_menu_new();
	for (uint32_t i = 0; i < a->actions.len && i < a->names.len; i++) {
		char detailed[256];
		snprintf(detailed, sizeof detailed, "action.%s", a->names.data[i]);
		GMenuItem *item = g_menu_item_new(a->actions.data[i], detailed);
		g_menu_append_item(model, item);
		g_object_unref(item);
	}
	lx_popup_at(n, (GMenuModel *)model, x, y);
	g_object_unref(model);
	return true;
}

// lx_action_name is a title as a GAction name (ASCII letters, digits and
// '-'), in name (cap bytes): a space or a dot is a hyphen, other
// characters go; a title left with none its number (i + 1). action_names
// are a view's, each its own.
void lx_action_name(const char *title, unsigned i, char *name, size_t cap) {
	const char *t = S(title);
	while (*t == ' ') t++;
	size_t k = 0;
	bool hyphen = false;
	for (; *t && k + 8 < cap; t++) {
		unsigned char c = (unsigned char)*t;
		if (c < 128 && isalnum(c)) {
			name[k++] = (char)c;
			hyphen = false;
		} else if ((c == ' ' || c == '.' || c == '-' || c == '_' || c == '/') && k > 0 && !hyphen) {
			name[k++] = '-';
			hyphen = true;
		}
	}
	while (k > 0 && name[k - 1] == '-') k--;
	name[k] = 0;
	if (!k) snprintf(name, cap, "%u", i + 1);
}

static void action_names(a11y_state *a) {
	for (uint32_t i = 0; i < a->actions.len; i++) {
		char name[128];
		lx_action_name(a->actions.data[i], i, name, sizeof name);
		char unique[160];
		snprintf(unique, sizeof unique, "%s", name);
		for (int n = 2;; n++) {
			bool seen = false;
			for (uint32_t j = 0; j < a->names.len; j++) seen |= !strcmp(a->names.data[j], unique);
			if (!seen) break;
			snprintf(unique, sizeof unique, "%s-%d", name, n);
		}
		ak_vec_push(&a->names, strdup(unique));
	}
}

typedef struct {
	ak_id id;
	int index;
} a11y_act;

static void on_a11y_action(GSimpleAction *act, GVariant *param, gpointer data) {
	(void)act;
	(void)param;
	a11y_act *a = data;
	node *n = node_get(a->id);
	if (n) lx_emit(n, (ak_event){.kind = AK_EVENT_A11Y_ACTION, .i = a->index});
}

// expose_actions makes a view's actions its "action" group, and says it
// has them.
static void expose_actions(node *n, a11y_state *a) {
	if (!n->widget) return;
	for (uint32_t i = 0; i < a->names.len; i++) free(a->names.data[i]);
	a->names.len = 0;
	action_names(a);
	if (!a->actions.len) {
		gtk_widget_insert_action_group(n->widget, "action", NULL);
		gtk_accessible_reset_property((GtkAccessible *)n->widget, (GtkAccessibleProperty)A11Y_PROP_HAS_POPUP);
		gtk_accessible_reset_property((GtkAccessible *)n->widget, (GtkAccessibleProperty)A11Y_PROP_KEY_SHORTCUTS);
		return;
	}
	// GTK names no generic view: a group, so its label is read with its
	// actions.
	if (n->kind == AK_KIND_VIEW && !a->adjustable && !n->role) lx_set_role(n->widget, "group");
	GSimpleActionGroup *group = g_simple_action_group_new();
	for (uint32_t i = 0; i < a->names.len; i++) {
		GSimpleAction *act = g_simple_action_new(a->names.data[i], NULL);
		a11y_act *d = malloc(sizeof *d);
		if (!d) abort();
		d->id = n->id;
		d->index = (int)i;
		g_signal_connect_data(act, "activate", G_CALLBACK(on_a11y_action), d, (GClosureNotify)(void (*)(void))free, (GConnectFlags)0);
		g_action_map_add_action((GActionMap *)group, (GAction *)act);
		g_object_unref(act);
	}
	gtk_widget_insert_action_group(n->widget, "action", (GActionGroup *)group);  // the view holds it
	g_object_unref(group);
	GValue val = {0};
	g_value_init(&val, G_TYPE_BOOLEAN);
	g_value_set_boolean(&val, 1);
	GtkAccessibleProperty p = (GtkAccessibleProperty)A11Y_PROP_HAS_POPUP;
	gtk_accessible_update_property_value((GtkAccessible *)n->widget, 1, &p, &val);
	g_value_unset(&val);
	lx_set_a11y_string(n->widget, A11Y_PROP_KEY_SHORTCUTS, "Shift+F10");
}

void lx_set_a11y_actions(node *n, ak_span b) {
	a11y_state *a = a11y_of(n);
	for (uint32_t i = 0; i < a->actions.len; i++) free(a->actions.data[i]);
	a->actions.len = 0;
	ak_str_vec ss = {0};
	ak_decode_strings((const uint8_t *)b.p, b.len, &ss);
	for (uint32_t i = 0; i < ss.len; i++) ak_vec_push(&a->actions, str_dup(ss.data[i]));
	ak_vec_free(&ss);
	expose_actions(n, a);
}

void lx_set_adjustable(node *n, bool on) {
	a11y_state *a = a11y_of(n);
	a->adjustable = on;
	if (on && n->widget && !n->role) lx_set_role(n->widget, "slider");
}
