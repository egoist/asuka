// chip.c: Chip, a compact action, choice or entered item. libadwaita has
// no chip; GNOME's apps show such things as pill buttons, so a chip is a
// GtkButton (an action chip: Press) or a GtkToggleButton (a filter chip:
// Toggled, the toggle's own checked look) in libadwaita's pill style made
// chip sized (theme.c's asuka-chip). In it: its Icon before its title (a
// selected filter chip's checkmark in its place, as Material's), and for
// a Removable chip a flat circular button at its end showing
// window-close-symbolic, its own click target (its gesture claims the
// press: the chip isn't pressed), named for screen readers and in its
// tooltip with the desktop word "remove" and the title. It, Delete or
// Backspace while the chip has the keyboard's focus report Remove.
//
// The chip's view measures it (its natural size, measure.c); the core
// measures it again as its title, icon or state change.
#include "host.h"

#include <stdio.h>
#include <stdlib.h>

void lx_set_a11y_string(gpointer w, int prop, const char *s);
#define A11Y_PROP_LABEL 4
#define CHECK_ICON "object-select-symbolic"
#define REMOVE_ICON "window-close-symbolic"

typedef struct {
	bool filter, selected, removable;
	GtkWidget *button;  // the chip: a GtkButton, or a GtkToggleButton
	GtkWidget *icon, *label, *remove;
	char *remove_name;  // its remove button's: "Remove Ana"
} chip;

static chip *chip_of(node *n) { return n && n->kind == AK_KIND_CHIP ? n->ext : NULL; }

static void free_chip(node *n) {
	chip *c = n->ext;
	free(c->remove_name);
	free(c);
	n->ext = NULL;
}

// show_icon shows the chip's leading icon: a selected filter chip's
// checkmark, else its Icon, else none.
static void show_icon(node *n, chip *c) {
	const char *name = c->filter && c->selected ? CHECK_ICON : !str_empty(n->icon) ? lx_icon_name(n->icon) : NULL;
	if (name && *name) gtk_image_set_from_icon_name((GtkImage *)c->icon, name);
	gtk_widget_set_visible(c->icon, name && *name);
}

// name_remove names the remove button with the platform's word and the
// title, for screen readers and its tooltip.
static void name_remove(node *n, chip *c) {
	if (!c->remove) return;
	const char *word = lx_word("remove");
	size_t len = strlen(word) + strlen(S(n->title)) + 2;
	char *s = malloc(len);
	if (!s) abort();
	if (str_empty(n->title))
		snprintf(s, len, "%s", word);
	else
		snprintf(s, len, "%s %s", word, n->title);
	free(c->remove_name);
	c->remove_name = s;
	lx_set_a11y_string(c->remove, A11Y_PROP_LABEL, s);
	gtk_widget_set_tooltip_text(c->remove, s);
}

// style_chip sets the label's text in the chip's font and color, when it
// sets them (else the theme's).
static void style_chip(node *n, chip *c) {
	gtk_label_set_text((GtkLabel *)c->label, S(n->title));
	if (n->color || n->font_size > 0 || n->text_style || n->font_weight || n->font_traits || n->letter_spacing != 0 || !str_empty(n->font_family) || H.dev_font_scale > 0)
		lx_set_label_attrs(n, c->label);
	else
		gtk_label_set_attributes((GtkLabel *)c->label, NULL);
}

static void on_chip_clicked(GtkButton *b, node *n) {
	(void)b;
	chip *c = chip_of(n);
	if (!c || c->filter || n->dead) return;
	lx_pressed(n);
}

static void on_chip_toggled(GtkToggleButton *b, node *n) {
	chip *c = chip_of(n);
	if (!c || n->updating || n->dead) return;
	c->selected = gtk_toggle_button_get_active(b);
	show_icon(n, c);  // the new state at once
	lx_emit(n, (ak_event){.kind = AK_EVENT_TOGGLED, .b = c->selected});
}

static void on_remove_clicked(GtkButton *b, node *n) {
	(void)b;
	if (!n->dead && chip_of(n)) lx_emit(n, (ak_event){.kind = AK_EVENT_REMOVE});
}

// Delete or Backspace with the keyboard's focus on a removable chip
// removes it.
static gboolean on_chip_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, node *n) {
	(void)k;
	(void)keycode;
	chip *c = chip_of(n);
	if (!c || !c->removable || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) return 0;
	switch (keyval) {
	case 0xffff:  // Delete
	case 0xff9f:  // KP_Delete
	case 0xff08:  // BackSpace
		lx_emit(n, (ak_event){.kind = AK_EVENT_REMOVE});
		return 1;
	}
	return 0;
}

// build_chip makes the chip's button as its Filter and Removable say
// (both sent before it's inserted; again, it's made again).
static void build_chip(node *n, chip *c) {
	GtkWidget *b = c->filter ? gtk_toggle_button_new() : gtk_button_new();
	gtk_widget_add_css_class(b, "pill");
	gtk_widget_add_css_class(b, "asuka-chip");
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	c->icon = gtk_image_new_from_icon_name(CHECK_ICON);
	c->label = gtk_label_new(S(n->title));
	gtk_box_append((GtkBox *)box, c->icon);
	gtk_box_append((GtkBox *)box, c->label);
	c->remove = NULL;
	if (c->removable) {
		GtkWidget *r = gtk_button_new_from_icon_name(REMOVE_ICON);
		gtk_widget_add_css_class(r, "flat");
		gtk_widget_add_css_class(r, "circular");
		gtk_widget_add_css_class(r, "asuka-chip-remove");
		gtk_widget_set_focusable(r, 0);  // the chip has the focus: Delete removes
		gtk_widget_set_valign(r, GTK_ALIGN_CENTER);
		gtk_box_append((GtkBox *)box, r);
		c->remove = r;
		gtk_widget_add_css_class(b, "asuka-chip-removable");
		lx_connect(n, r, "clicked", G_CALLBACK(on_remove_clicked));
	}
	gtk_button_set_child((GtkButton *)b, box);
	c->button = b;
	lx_set_inner(n, b);
	if (c->filter) {
		n->updating = true;
		gtk_toggle_button_set_active((GtkToggleButton *)b, c->selected);
		n->updating = false;
		lx_connect(n, b, "toggled", G_CALLBACK(on_chip_toggled));
	} else {
		lx_connect(n, b, "clicked", G_CALLBACK(on_chip_clicked));
	}
	GtkEventController *k = gtk_event_controller_key_new();
	lx_connect(n, k, "key-pressed", G_CALLBACK(on_chip_key));
	gtk_widget_add_controller(b, k);
	show_icon(n, c);
	style_chip(n, c);
	name_remove(n, c);
	if (n->disabled) gtk_widget_set_sensitive(n->widget, 0);
	lx_update_focusable(n);
}

static bool chip_create(node *n) {
	if (n->kind != AK_KIND_CHIP) return false;
	chip *c = calloc(1, sizeof *c);
	if (!c) abort();
	n->ext = c;
	n->ext_free = free_chip;
	build_chip(n, c);
	return true;
}

static bool chip_prop(node *n, ak_prop p, const ak_value *v) {
	chip *c = chip_of(n);
	if (!c) return false;
	switch (p) {
	case AK_PROP_CHIP_TITLE:
		str_set(&n->title, v->s);
		style_chip(n, c);
		name_remove(n, c);
		return true;
	case AK_PROP_CHIP_ICON:
		str_set(&n->icon, v->s);
		show_icon(n, c);
		return true;
	case AK_PROP_CHIP_FILTER:
		if (c->filter != (v->i != 0)) {
			c->filter = v->i != 0;
			build_chip(n, c);
		}
		return true;
	case AK_PROP_CHIP_SELECTED:
		c->selected = v->i != 0;
		if (c->filter) {
			n->updating = true;
			gtk_toggle_button_set_active((GtkToggleButton *)c->button, c->selected);
			n->updating = false;
		}
		show_icon(n, c);
		return true;
	case AK_PROP_CHIP_REMOVABLE:
		if (c->removable != (v->i != 0)) {
			c->removable = v->i != 0;
			build_chip(n, c);
		}
		return true;
	}
	return false;
}

static void chip_text(node *n) {
	chip *c = chip_of(n);
	if (c) style_chip(n, c);
}

// chip_dump writes what the chip shows beyond its title: a filter's
// state as GTK's toggle has it, its leading icon, its remove button's
// name.
static void chip_dump(node *n, int depth, ak_buf *b) {
	chip *c = chip_of(n);
	if (!c) return;
	for (int i = 0; i <= depth; i++) ak_buf_put(b, "  ", 2);
	ak_buf_printf(b, "chip %s", c->filter ? "filter" : "action");
	if (c->filter) ak_buf_printf(b, " active=%d", gtk_toggle_button_get_active((GtkToggleButton *)c->button) ? 1 : 0);
	if (gtk_widget_get_visible(c->icon)) {
		const char *icon = c->filter && c->selected ? CHECK_ICON : lx_icon_name(n->icon);
		ak_buf_printf(b, " icon=%s", icon);
	}
	if (c->remove) {
		ak_buf_put(b, " remove=", 8);
		ak_fmt_quote(b, ak_cstr(S(c->remove_name)));
	}
	ak_buf_u8(b, '\n');
}

// lx_chip_activate presses a chip as a click does (automation's tap).
bool lx_chip_activate(node *n) {
	chip *c = chip_of(n);
	if (!c) return false;
	gtk_widget_activate(c->button);
	return true;
}

const area lx_area_chip = {
	.name = "chip",
	.create = chip_create,
	.prop = chip_prop,
	.text = chip_text,
	.dump = chip_dump,
};
