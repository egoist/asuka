// focus.c: the keyboard's focus on any view (RFC 0010).
//
// A view takes the focus when it's Focusable, pressable (it listens for
// Press) or has screen readers' actions or adjustments: its AsukaView is
// focusable in GTK's sense, a Tab stop in GTK's order, and a primary
// press focuses it unless NoFocusOnPress (focus-on-click). Its ring is
// GTK's :focus-visible outline (theme.c's asuka-focus-ring), gone with
// NoFocusRing (asuka-no-focus-ring, on a control too).
//
// Where the focus is, each window says (its focus-widget): the node it's
// in is the nearest view out from that widget. As it changes, the view
// that had it gets FocusChange 0 and the one that has it 1, or 2 when the
// last input was a key (Tab, an arrow, a key that opened what took it:
// CSS's :focus-visible), not a pointer's press; fields say Focus and Blur
// themselves (field.c). FocusChange goes whether or not the core listens.
//
// A focus trap (FocusTrap) keeps GTK's focus movement inside its view:
// when GTK finds nothing more to focus in it (its focus vfunc fails) while
// the focus is in it, Tab wraps to its first view (Shift+Tab its last),
// as a window wraps, and an arrow key stays.
//
// KeyText: a printable key, without Ctrl, Alt or Super, typed while the
// view itself has the focus, that no Keys took (keys.c's controller is in
// the capture phase, ahead of this one): Space only when it doesn't press
// the view.
#include "host.h"

#include <stdlib.h>

bool lx_view_is_pressable(node *n);
bool lx_a11y_takes_focus(node *n);
GtkWidget *lx_field_widget(node *n);

#define FOCUS_RING "asuka-focus-ring"
#define NO_FOCUS_RING "asuka-no-focus-ring"

// is_control reports whether n's kind is a control of GTK's, its widget
// the inner one.
static bool is_control(const node *n) {
	switch (n->kind) {
	case AK_KIND_BUTTON:
	case AK_KIND_TOGGLE:
	case AK_KIND_SLIDER:
	case AK_KIND_STEPPER:
	case AK_KIND_SEGMENTED:
	case AK_KIND_PICKER:
	case AK_KIND_WHEEL:
	case AK_KIND_COLOR_PICKER:
	case AK_KIND_DATE_PICKER:
		return true;
	}
	return false;
}

// lx_control_of is the widget that takes the focus for n: a field's
// editable, a control's GTK widget, else n's view.
GtkWidget *lx_control_of(node *n) {
	if (n->kind == AK_KIND_TEXT_FIELD) {
		GtkWidget *f = lx_field_widget(n);
		if (f) return f;
	}
	if (is_control(n) && n->inner) return n->inner;
	return n->widget;
}

static void set_class(GtkWidget *w, const char *c, bool on) {
	if (on)
		gtk_widget_add_css_class(w, c);
	else
		gtk_widget_remove_css_class(w, c);
}

// lx_update_focusable makes n's view take the focus, or not, as its props
// and listeners say.
void lx_update_focusable(node *n) {
	if (!n->widget) return;
	if (is_control(n) || n->kind == AK_KIND_TEXT_FIELD) {
		GtkWidget *c = lx_control_of(n);
		if (!c) return;
		gtk_widget_set_focus_on_click(c, !n->no_focus_on_press);
		set_class(c, NO_FOCUS_RING, n->no_focus_ring);
		return;
	}
	bool takes = n->focusable || lx_view_is_pressable(n) || lx_a11y_takes_focus(n);
	gtk_widget_set_focusable(n->widget, takes);
	gtk_widget_set_focus_on_click(n->widget, takes && !n->no_focus_on_press);
	set_class(n->widget, FOCUS_RING, takes && !n->no_focus_ring);
	set_class(n->widget, NO_FOCUS_RING, n->no_focus_ring);
}

void lx_set_focus_prop(node *n, ak_prop p, const ak_value *v) {
	bool on = v->i != 0;
	switch (p) {
	case AK_PROP_FOCUSABLE: n->focusable = on; break;
	case AK_PROP_NO_FOCUS_ON_PRESS: n->no_focus_on_press = on; break;
	case AK_PROP_NO_FOCUS_RING: n->no_focus_ring = on; break;
	case AK_PROP_FOCUS_TRAP: n->focus_trap = on; return;
	default: return;
	}
	lx_update_focusable(n);
}

// ---- Where the focus is ----

// node_of is the node a focus widget is in: the nearest view out from it
// (NULL: none, a widget of the window's own).
static node *node_of(GtkWidget *f) {
	for (; f; f = gtk_widget_get_parent(f)) {
		node *n = lx_view_node(f);
		if (n && !n->dead) return n;
	}
	return NULL;
}

static void send_change(node *n, int how) {
	// A view of the core's (not a list's cell the host made), not a field.
	if (!n || n->dead || n->kind >= AK_KIND_COUNT || n->kind == AK_KIND_TEXT_FIELD || n->list_owner) return;
	lx_dispatch(&(ak_event){.id = n->id, .kind = AK_EVENT_FOCUS_CHANGE, .i = how});
}

// lx_focus_report says where w's focus went, if it moved to another node.
void lx_focus_report(win *w) {
	if (!w || H.focus_moving || !w->window) return;
	node *now = node_of(gtk_window_get_focus(w->window));
	node *was = node_get(w->focused);
	if (now == was) return;
	w->focused = now ? now->id : 0;
	send_change(was, 0);
	send_change(now, H.key_input ? 2 : 1);
}

static void on_focus_widget(GObject *o, GParamSpec *ps, gpointer data) {
	(void)o;
	(void)ps;
	lx_focus_report(data);
}

// focus_on_press gives the focus to the view a primary press at x, y of
// window lands in that takes it, the nearest out from where it landed,
// unless NoFocusOnPress; a control of GTK's on the way focuses itself.
static void focus_on_press(GtkWidget *window, double x, double y) {
	GtkWidget *hit = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
	for (GtkWidget *w = hit; w && w != window; w = gtk_widget_get_parent(w)) {
		if (!gtk_widget_get_focusable(w)) continue;
		node *n = lx_view_node(w);
		if (n && !n->dead && n->widget == w && !is_control(n) && n->kind != AK_KIND_TEXT_FIELD && !n->no_focus_on_press && gtk_widget_is_sensitive(w))
			gtk_widget_grab_focus(w);
		return;
	}
}

// on_window_press, in the capture phase: a pointer's press, ahead of its
// target: the overlays hear of it (OutsidePress), and the focus it moves
// (to a focusable view in it) isn't visible.
static void on_window_press(GtkGestureClick *g, int count, double x, double y, gpointer data) {
	(void)count;
	(void)data;
	H.key_input = false;
	GtkWidget *window = gtk_event_controller_get_widget((GtkEventController *)g);
	GdkEvent *ev = gtk_event_controller_get_current_event((GtkEventController *)g);
	GdkSurface *s = ev && gdk_event_get_surface ? gdk_event_get_surface(ev) : NULL;
	bool own = !s || s == gtk_native_get_surface((GtkNative *)window);
	lx_enter_begin();
	lx_overlays_outside_press(window, s, x, y);
	if (own && gtk_gesture_single_get_current_button((GtkGestureSingle *)g) <= 1) focus_on_press(window, x, y);
	lx_enter_end();
}

// lx_watch_focus follows w's focus, and its pointer's presses.
void lx_watch_focus(win *w) {
	g_signal_connect(w->window, "notify::focus-widget", G_CALLBACK(on_focus_widget), w);
	GtkGesture *g = gtk_gesture_click_new();
	gtk_gesture_single_set_button((GtkGestureSingle *)g, 0);
	gtk_event_controller_set_propagation_phase((GtkEventController *)g, GTK_PHASE_CAPTURE);
	if (gtk_event_controller_set_propagation_limit) gtk_event_controller_set_propagation_limit((GtkEventController *)g, GTK_LIMIT_NONE);  // a popover's too
	g_signal_connect(g, "pressed", G_CALLBACK(on_window_press), w);
	gtk_widget_add_controller((GtkWidget *)w->window, (GtkEventController *)g);
}

// grab_first focuses w, or the first widget in it that takes the focus.
static bool grab_first(GtkWidget *w) {
	if (gtk_widget_grab_focus(w)) return true;
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
		if (gtk_widget_get_visible(c) && grab_first(c)) return true;
	return false;
}

static bool inside(GtkWidget *w, GtkWidget *a) { return w && a && (w == a || gtk_widget_is_ancestor(w, a)); }

// lx_focus_command is Focus (focus) or Blur on n.
void lx_focus_command(node *n, bool focus) {
	if (focus) {
		if (n->kind == AK_KIND_TEXT_FIELD) {
			lx_focus_field(n);
			return;
		}
		GtkWidget *c = lx_control_of(n);
		if (c) grab_first(c);
		return;
	}
	win *w = lx_win_of(n);
	if (!w || !n->widget) return;
	GtkWidget *f = gtk_window_get_focus(w->window);
	if (inside(f, n->widget) || (n->kind == AK_KIND_TEXT_FIELD && inside(f, lx_control_of(n)))) gtk_window_set_focus(w->window, NULL);
}

// ---- A focus trap ----

// lx_trap_focus: GTK found nothing more to focus in w in direction d.
// True when a trap keeps it in: Tab wraps around, an arrow stays.
gboolean lx_trap_focus(GtkWidget *w, GtkDirectionType d) {
	node *n = lx_view_node(w);
	if (!n || !n->focus_trap || H.focus_moving) return 0;
	GtkRoot *r = gtk_widget_get_root(w);
	if (!r || !AK_IS(r, gtk_window_get_type)) return 0;
	GtkWindow *window = (GtkWindow *)r;
	GtkWidget *f = gtk_window_get_focus(window);
	if (!inside(f, w)) return 0;  // coming in from outside: as GTK goes
	if (d != GTK_DIR_TAB_FORWARD && d != GTK_DIR_TAB_BACKWARD) return 1;
	// As GtkWindow wraps: from nothing focused, into the trap. Where the
	// focus lands is reported once, not the nothing in between.
	H.focus_moving = true;
	g_object_ref(f);
	gtk_window_set_focus(window, NULL);
	if (!gtk_widget_child_focus(w, d)) gtk_window_set_focus(window, f);
	g_object_unref(f);
	H.focus_moving = false;
	lx_focus_report(lx_win_by_window(window));
	return 1;
}

// ---- KeyText ----

static gboolean on_key_text(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, node *n) {
	(void)k;
	(void)keycode;
	if (n->dead || !n->widget) return 0;
	if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_HYPER_MASK | GDK_META_MASK)) return 0;
	GtkRoot *r = gtk_widget_get_root(n->widget);
	if (!r || !AK_IS(r, gtk_window_get_type) || gtk_window_get_focus((GtkWindow *)r) != n->widget) return 0;  // the view's own focus
	gunichar c = gdk_keyval_to_unicode(keyval);
	if (c < 0x20 || c == 0x7f) return 0;
	if (c == ' ' && listening(n, AK_EVENT_PRESS)) return 0;  // Space presses it
	char utf8[8];
	int len = g_unichar_to_utf8(c, utf8);
	lx_emit(n, (ak_event){.kind = AK_EVENT_KEY_TEXT, .s = ak_strn(utf8, (size_t)len)});
	return 1;
}

void lx_listen_key_text(node *n) {
	if (n->key_text || !n->widget) return;
	GtkEventController *k = gtk_event_controller_key_new();
	n->key_text = k;
	lx_connect(n, k, "key-pressed", G_CALLBACK(on_key_text));
	gtk_widget_add_controller(n->widget, k);
}
