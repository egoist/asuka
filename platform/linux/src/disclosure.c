// disclosure.c: Disclosure, a disclosure's header: a GtkExpander with no
// child, its label the Title and its arrow before it (GTK's, turning as
// it expands, mirrored right to left by GTK as it draws it). The content
// under it is the core's (views beside this one in the core's column), so
// the expander only shows its header and reports the user's toggling
// (a click, Space or Return with the focus on it) with Toggled, its new
// state shown at once. GTK's expander tells screen readers it's a button,
// expanded or collapsed.
#include "host.h"

static GtkExpander *expander_of(node *n) {
	return n && n->kind == AK_KIND_DISCLOSURE && n->inner && AK_IS(n->inner, gtk_expander_get_type) ? (GtkExpander *)n->inner : NULL;
}

static void on_expanded(GObject *o, GParamSpec *ps, node *n) {
	(void)ps;
	if (n->updating || n->dead) return;
	bool on = gtk_expander_get_expanded((GtkExpander *)o);
	n->value = on;
	lx_emit(n, (ak_event){.kind = AK_EVENT_TOGGLED, .b = on});
}

static void style_disclosure(node *n) {
	GtkExpander *e = expander_of(n);
	GtkWidget *l = e ? gtk_expander_get_label_widget(e) : NULL;
	if (!l || !AK_IS(l, gtk_label_get_type)) return;
	if (n->color || n->font_size > 0 || n->text_style || n->font_weight || n->font_traits || n->letter_spacing != 0 || !str_empty(n->font_family) || H.dev_font_scale > 0)
		lx_set_label_attrs(n, l);
	else
		gtk_label_set_attributes((GtkLabel *)l, NULL);
}

static bool disclosure_create(node *n) {
	if (n->kind != AK_KIND_DISCLOSURE) return false;
	GtkWidget *e = gtk_expander_new("");
	gtk_widget_set_valign(e, GTK_ALIGN_CENTER);
	lx_set_inner(n, e);
	lx_connect(n, e, "notify::expanded", G_CALLBACK(on_expanded));
	return true;
}

static bool disclosure_prop(node *n, ak_prop p, const ak_value *v) {
	GtkExpander *e = expander_of(n);
	if (!e) return false;
	switch (p) {
	case AK_PROP_DISCLOSURE_TITLE: {
		str_set(&n->title, v->s);
		gtk_expander_set_label(e, S(n->title));
		style_disclosure(n);
		return true;
	}
	case AK_PROP_DISCLOSURE_EXPANDED:
		n->value = v->i != 0;
		n->updating = true;
		gtk_expander_set_expanded(e, v->i != 0);
		n->updating = false;
		return true;
	}
	return false;
}

// expanded_state finds the accessible in the expander that tells screen
// readers its state (its title, a button): 1 expanded, 0 collapsed, -1
// none says.
static int expanded_state(GtkWidget *w) {
	enum { EXPANDED = 3 };
	if (gtk_test_accessible_has_state && gtk_test_accessible_has_state((GtkAccessible *)w, (GtkAccessibleState)EXPANDED)) {
		char *e = gtk_test_accessible_check_state((GtkAccessible *)w, (GtkAccessibleState)EXPANDED, 1);
		if (e) g_free(e);
		return e ? 0 : 1;
	}
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
		int s = expanded_state(c);
		if (s >= 0) return s;
	}
	return -1;
}

static void disclosure_dump(node *n, int depth, ak_buf *b) {
	GtkExpander *e = expander_of(n);
	if (!e) return;
	for (int i = 0; i <= depth; i++) ak_buf_put(b, "  ", 2);
	int s = expanded_state((GtkWidget *)e);
	ak_buf_printf(b, "expander expanded=%d a11y-expanded=%s\n", gtk_expander_get_expanded(e) ? 1 : 0, s < 0 ? "none" : s ? "true" : "false");
}

const area lx_area_disclosure = {
	.name = "disclosure",
	.create = disclosure_create,
	.prop = disclosure_prop,
	.text = style_disclosure,
	.dump = disclosure_dump,
};
