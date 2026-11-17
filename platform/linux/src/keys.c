// keys.c: keys (the Keys prop). Each window of the app's, and each
// popover (GTK takes a popover's keys from its focus widget up to it, not
// through the window), has a key controller of the host's in the capture
// phase, ahead of the window's own and the application's accelerators. A
// key going down, and each repeat, starts at the window's focus widget's
// node (the nearest widget out from it that is a node's view; the header
// bar's search entry is its screen's), or the window's root where no node
// has the focus, and goes out through the nodes' parents, and on from a
// node with none where it shows: a sheet's to its window's root, a
// popover's to its anchor's root. The first node whose Keys have the key
// takes it: the controller stops it, and the node gets Key with the
// key's index. Not while an input method composes text in the focused
// widget.
//
// A key is matched as it types with no modifiers: the named keys by the
// keyval (KP_ variants too; ISO_Left_Tab is tab), a character by the
// keyval of the key's level 0 in the event's layout (Shift+a is a with
// Shift), lower case; a key typed with AltGr (level 2 and up) is the
// character it types. Ctrl is primary, and the control modifier too;
// Super and Hyper match no Keys.
#include "host.h"

#include <stdlib.h>

#define KEY_ESCAPE 0xff1b

node *lx_popover_anchor(node *n);

// The Keys prop.
void lx_set_keys(node *n, ak_span b) {
	ak_buf_reset(&n->keys_buf);
	ak_buf_put(&n->keys_buf, b.p, b.len);
	n->keys.len = 0;
	ak_decode_keys(n->keys_buf.data, n->keys_buf.len, &n->keys);
	for (uint32_t i = 0; i < n->keys.len; i++)
		if (n->keys.data[i].mods & 8)  // control: Ctrl, as primary is
			n->keys.data[i].mods = (uint8_t)((n->keys.data[i].mods & ~8) | 1);
}

// key_out is where a key goes on from n, not taking it: n's parent; from a
// node the core leaves without one, where it shows. The root and a Window
// node are ends (NULL).
static node *key_out(node *n) {
	if (n->parent) return n->parent;
	switch (n->kind) {
	case AK_KIND_SHEET: {
		win *w = lx_declared_win(n);
		if (w && w->root && w->root != n) return w->root;
		break;
	}
	case AK_KIND_POPOVER: {
		node *a = lx_popover_anchor(n);
		while (a && a->parent) a = a->parent;
		if (a && a != n) return a;
		break;
	}
	default:
		return NULL;
	}
	return H.main ? H.main->root : NULL;
}

static bool take_key(node *n, ak_span name, uint8_t mods) {
	int i = ak_match_key(n->keys.data, n->keys.len, name, mods);
	if (i < 0) return false;
	lx_dispatch(&(ak_event){.id = n->id, .kind = AK_EVENT_KEY, .i = i});
	return true;
}

static bool route_key(node *n, ak_span name, uint8_t mods) {
	for (; n; n = key_out(n))
		if (take_key(n, name, mods)) return true;
	return false;
}

GtkWidget *lx_search_entry(win *w, node **screen);

// focus_node is the node the keyboard's focus is in, in native: the
// nearest node's view out from focus, else start.
static node *focus_node(win *w, GtkWidget *native, GtkWidget *focus, node *start) {
	node *screen;
	GtkWidget *search = lx_search_entry(w, &screen);
	for (GtkWidget *f = focus; f && f != native; f = gtk_widget_get_parent(f)) {
		node *n = lx_view_node(f);
		if (n && !n->dead) return n;
		if (search && f == search && screen) return screen;
	}
	return start;
}

static bool inside(GtkWidget *w, GtkWidget *native) {
	for (; w; w = gtk_widget_get_parent(w))
		if (w == native) return true;
	return false;
}

// named_key is a keyval's name among the Keys prop's named keys, or NULL.
static const char *named_key(guint keyval) {
	switch (keyval) {
	case 0xff0d: case 0xff8d: case 0xfe34: return "enter";
	case KEY_ESCAPE: return "escape";
	case 0xff09: case 0xfe20: case 0xff89: return "tab";
	case 0x20: case 0xff80: return "space";
	case 0xffff: case 0xff9f: return "delete";
	case 0xff08: return "backspace";
	case 0xff52: case 0xff97: return "up";
	case 0xff54: case 0xff99: return "down";
	case 0xff51: case 0xff96: return "left";
	case 0xff53: case 0xff98: return "right";
	case 0xff50: case 0xff95: return "home";
	case 0xff57: case 0xff9c: return "end";
	case 0xff55: case 0xff9a: return "pageup";
	case 0xff56: case 0xff9b: return "pagedown";
	}
	static const char *const f[12] = {"f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12"};
	if (keyval >= 0xffbe && keyval <= 0xffc9) return f[keyval - 0xffbe];
	return NULL;
}

// key_of is a key event's key as the Keys prop names it (a lower case
// character into buf, or a named key; empty: neither) and its modifiers.
static ak_span key_of(GtkWidget *window, GdkEvent *ev, guint keyval, guint keycode, GdkModifierType state, uint8_t *mods, char buf[8]) {
	*mods = 0;
	if (state & GDK_CONTROL_MASK) *mods |= 1;
	if (state & GDK_SHIFT_MASK) *mods |= 2;
	if (state & (GDK_ALT_MASK | GDK_META_MASK)) *mods |= 4;
	if (state & (GDK_SUPER_MASK | GDK_HYPER_MASK)) *mods |= 16;  // no Keys have it
	const char *name = named_key(keyval);
	if (name) return ak_cstr(name);
	gunichar r = 0;
	if (ev && gdk_key_event_get_level(ev) >= 2) {
		// AltGr's: the character it types, with any Shift it took.
		r = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
		if (gdk_key_event_get_consumed_modifiers(ev) & GDK_SHIFT_MASK) *mods &= (uint8_t)~2;
	} else {
		// The key's level 0, in the event's layout.
		guint kv = 0;
		int group = ev ? (int)gdk_key_event_get_layout(ev) : 0, eff = 0, level = 0;
		GdkModifierType consumed = 0;
		if (gdk_display_translate_key(gtk_widget_get_display(window), keycode, 0, group, &kv, &eff, &level, &consumed))
			r = gdk_keyval_to_unicode(gdk_keyval_to_lower(kv));
		if (!r) r = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));  // a keypad's digit with Num Lock
	}
	if (r < 0x20 || r == 0x7f) return (ak_span){0};
	char utf8[8];
	int n = g_unichar_to_utf8(r, utf8);
	utf8[n] = 0;
	return ak_key_name(ak_strn(utf8, (size_t)n), buf);
}

typedef struct {
	GtkWidget *native;
	win *w;      // a window's: its own
	node *pop;   // a popover's node
} key_route;

static gboolean on_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
	key_route *kr = data;
	win *w = kr->w;
	node *start;
	H.key_input = true;  // focus that comes now is visible (focus.c)
	if (w) {
		start = w->root;
	} else {
		if (!kr->pop || kr->pop->dead) return 0;
		w = lx_win_of(kr->pop);
		start = kr->pop;
	}
	if (!w || w->closing) return 0;
	GdkEvent *ev = gtk_event_controller_get_current_event((GtkEventController *)k);
	if (ev && gdk_key_event_is_modifier(ev)) return 0;
	GtkWidget *focus = gtk_window_get_focus(w->window);
	if (focus && kr->native != (GtkWidget *)w->window && focus != kr->native && !inside(focus, kr->native)) focus = NULL;
	if (focus && lx_composing(focus)) return 0;
	uint8_t mods;
	char buf[8];
	ak_span name = key_of(kr->native, ev, keyval, keycode, state, &mods, buf);
	if (!name.len) return 0;
	lx_enter_begin();
	bool took = route_key(focus_node(w, kr->native, focus, start), name, mods);
	lx_enter_end();
	return took;
}

// lx_key_controller adds a capture phase key controller routing keys to
// the nodes' Keys to native (a window, or a popover's).
void lx_key_controller(GtkWidget *native, win *w, node *pop);
void lx_key_controller(GtkWidget *native, win *w, node *pop) {
	key_route *kr = calloc(1, sizeof *kr);
	if (!kr) abort();
	kr->native = native;
	kr->w = w;
	kr->pop = pop;
	GtkEventController *k = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
	g_signal_connect_data(k, "key-pressed", G_CALLBACK(on_key), kr, (GClosureNotify)(void (*)(void))free, (GConnectFlags)0);
	gtk_widget_add_controller(native, k);
	if (w) w->keys = k;
}

void lx_route_keys(win *w) { lx_key_controller((GtkWidget *)w->window, w, NULL); }

// lx_key_to_nodes gives a key (lower case: down, k; mods as the Keys
// prop's) to the Keys of the views in the window in use, as the keyboard
// would: an automation run's key step.
bool lx_key_to_nodes(const char *key, uint8_t mods) {
	if (!strcmp(key, "esc")) key = "escape";
	win *w = H.active_win;
	if (!w || w->closing) w = H.main;
	if (!w) return false;
	GtkWidget *focus = gtk_window_get_focus(w->window);
	if (focus && lx_composing(focus)) return false;
	char buf[8];
	return route_key(focus_node(w, (GtkWidget *)w->window, focus, w->root), ak_key_name(ak_cstr(key), buf), mods);
}
