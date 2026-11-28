// automation.c: debug automation, as on macOS and Windows: ASUKA_SCRIPT
// runs steps (tap:<text>, type:<text>, submit, back, escape, scroll:<y>,
// resize:<w>,<h>, key:<shortcut>, menu:<item title>, tab:<title>, close,
// wait:<d>, waitfor:<file>, snap:<file>; on X11, real input through the X
// server's XTest, as a user's: click:<text> (a press of the mouse's
// button on the view showing text), clickat:<x>,<y> (in the main
// window's root), press:<keys> (shift+tab, b, space), dclick:<text> (a
// double click on it), drag:<text>,<dx>,<dy>[,hold] (the button held from
// its middle, moved by dx, dy in steps, let go, or held until release),
// wheel:<text>,<notches>[,ctrl]
// (the wheel turned over it, up for notches above 0, Ctrl held)), then
// ASUKA_SNAPSHOT=file.png
// writes the window as a PNG (ASUKA_DUMP=1: its view tree beside it) and
// quits. ASUKA_COMMANDS=dir drives the app from outside while it runs, as
// the phones' debug hosts do (the e2e tests): commands written to
// dir/asuka-commands.txt, one a line, are run and the file removed: tap,
// type (the focused field, else the first), submit, back, escape, tab,
// scroll, and dump, which writes what the screen shows (each text,
// field's text, button's title or accessibility label, a line each, in
// creation order; then asuka dev's error overlay's) to dir/asuka-dump.txt.
//
// The steps run on the main loop, between its turns: a step, then 400 ms
// for animations to settle (and, while asuka dev's error overlay fades
// in, until it has).
#include "host.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

void lx_each_node(void (*f)(node *n, void *ctx), void *ctx);
bool lx_go_back_front(void);
void lx_focus_field_by(node *n);
node *lx_field_of_widget(GtkWidget *w);

static char **steps;
static size_t step_count, step_at;
static char *snap_path;
static gint64 waiting_until;

// ---- Finding what to act on ----

typedef struct {
	const char *text;
	node *found;
	bool first;  // the lowest ID (else the highest)
	ak_view_kind kind;
} find_ctx;

static bool mapped(node *n) { return n->widget && gtk_widget_get_mapped(n->widget); }

static void find_text(node *n, void *data) {
	find_ctx *c = data;
	if (!mapped(n)) return;
	if (str_eq(n->text, c->text) || (n->kind == AK_KIND_BUTTON && str_eq(n->title, c->text)) || str_eq(n->placeholder, c->text) || str_eq(n->a11y_label, c->text))
		if (!c->found || n->id > c->found->id) c->found = n;
}

// find_text_node is the node on screen that shows text: a text, a
// button's title, a field's placeholder, an accessibility label.
static node *find_text_node(const char *text) {
	if (!text || !*text) return NULL;
	find_ctx c = {text, NULL, false, 0};
	lx_each_node(find_text, &c);
	return c.found;
}

// tap presses the node, or the nearest one around it that presses.
static void tap(node *n) {
	for (node *p = n; p; p = p->parent) {
		if (p->kind == AK_KIND_BUTTON || p->kind == AK_KIND_TOGGLE) {
			gtk_widget_activate(p->inner);
			return;
		}
		if (p->kind == AK_KIND_TEXT_FIELD) {
			lx_focus_field(p);
			return;
		}
		if (listening(p, AK_EVENT_PRESS) || p->has_menu) {
			lx_press(p);
			return;
		}
	}
}

static void find_widgets(GtkWidget *w, const char *text, widget_vec *out) {
	if (!gtk_widget_get_mapped(w)) return;
	if (AK_IS(w, gtk_label_get_type) && str_eq(gtk_label_get_text((GtkLabel *)w), text)) {
		ak_vec_push(out, w);
	} else {
		char *t = gtk_widget_get_tooltip_text(w);
		if (t && strcmp(t, text) == 0) ak_vec_push(out, w);
		g_free(t);
	}
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) find_widgets(c, text, out);
}

// tap_widget activates the GTK widget in a window of the app's (or a
// dialog over one) whose label or tooltip is text: a button, a list box's
// row, a check button, the nearest of them around a label.
static bool tap_widget(const char *text) {
	GListModel *tops = gtk_window_get_toplevels();
	widget_vec found = {0};
	for (guint i = 0; i < g_list_model_get_n_items(tops); i++) {
		GtkWidget *w = g_list_model_get_item(tops, i);
		if (w && gtk_widget_get_mapped(w)) find_widgets(w, text, &found);
		if (w) g_object_unref(w);
	}
	bool done = false;
	for (uint32_t i = 0; i < found.len && !done; i++) {
		for (GtkWidget *w = found.data[i]; w && !done; w = gtk_widget_get_parent(w)) {
			if (AK_IS(w, gtk_button_get_type)) {
				g_signal_emit_by_name(w, "clicked");
				done = true;
			} else if (AK_IS(w, gtk_list_box_row_get_type)) {
				if (!gtk_list_box_row_get_activatable((GtkListBoxRow *)w)) break;  // a section's title: not this one
				gtk_widget_activate(w);
				done = true;
			} else if (AK_IS(w, gtk_check_button_get_type) || AK_IS(w, gtk_switch_get_type)) {
				gtk_widget_activate(w);
				done = true;
			}
		}
	}
	ak_vec_free(&found);
	return done;
}

static node *focused_field(void) {
	for (uint32_t i = 0; i < H.wins.len; i++) {
		GtkWidget *f = gtk_window_get_focus(H.wins.data[i]->window);
		for (; f; f = gtk_widget_get_parent(f)) {
			node *n = lx_field_of_widget(f);
			if (n) return n;
		}
	}
	return NULL;
}

static void first_field(node *n, void *data) {
	find_ctx *c = data;
	if (n->kind == AK_KIND_TEXT_FIELD && mapped(n) && (!c->found || n->id < c->found->id)) c->found = n;
}

static void sheet_or_alert(node *n, void *data) {
	find_ctx *c = data;
	if ((n->kind == AK_KIND_SHEET || n->kind == AK_KIND_ALERT) && n->nav && n->nav->dialog && !n->nav->gone) c->found = n;
}

// escape closes the top dialog, or dismisses asuka dev's error overlay.
static void escape(void) {
	if (lx_dev_error_escape()) return;
	find_ctx c = {0};
	lx_each_node(sheet_or_alert, &c);
	if (c.found) adw_dialog_close(c.found->nav->dialog);
}

typedef struct {
	double y;
	bool done;
} scroll_ctx;

static void scroll_first(node *n, void *data) {
	scroll_ctx *c = data;
	if (c->done || !mapped(n)) return;
	if (n->scroll) {
		lx_scroll_to(n, 0, c->y, false);
		c->done = true;
	} else if (n->kind == AK_KIND_LIST) {
		lx_list_scroll_to_offset(n, 0, c->y, false);
		c->done = true;
	}
}

typedef struct {
	const char *title;
	bool done;
} tab_ctx;

static void choose_tab(node *n, void *data) {
	tab_ctx *c = data;
	if (c->done || n->kind != AK_KIND_TABS || !n->nav || !mapped(n)) return;
	for (uint32_t i = 0; i < n->children.len; i++) {
		node *t = n->children.data[i];
		if (str_eq(t->title, c->title) && t->widget) {
			adw_view_stack_set_visible_child((AdwViewStack *)n->nav->view, t->widget);
			c->done = true;
			return;
		}
	}
}

// xdotool_key types a key into the focused window through xdotool (X11:
// Xvfb in tests and automation runs), for the shortcuts a window's
// controllers take (F5, Alt+Left) rather than the app's menus.
static bool xdotool_key(const char *shortcut) {
	static const char *const names[][2] = {{"enter", "Return"}, {"escape", "Escape"}, {"esc", "Escape"}, {"tab", "Tab"}, {"space", "space"},
		{"delete", "Delete"}, {"backspace", "BackSpace"}, {"left", "Left"}, {"right", "Right"}, {"up", "Up"}, {"down", "Down"},
		{"home", "Home"}, {"end", "End"}, {"pageup", "Prior"}, {"pagedown", "Next"}};
	ak_buf b = {0};
	char *copy = strdup(shortcut);
	for (char *part = strtok(copy, "+"); part; part = strtok(NULL, "+")) {
		if (b.len) ak_buf_u8(&b, '+');
		const char *out = part;
		char up[16];
		for (size_t i = 0; i < AK_LEN(names); i++)
			if (strcasecmp(part, names[i][0]) == 0) out = names[i][1];
		if (!strcasecmp(part, "cmd") || !strcasecmp(part, "primary")) {
			out = "ctrl";
		} else if (out == part && strlen(part) > 1 && strlen(part) < sizeof up && (part[0] == 'f' || part[0] == 'F')) {
			for (size_t i = 0; part[i]; i++) up[i] = (char)toupper((unsigned char)part[i]);
			up[strlen(part)] = 0;
			out = up;
		}
		ak_buf_put(&b, out, strlen(out));
	}
	free(copy);
	// Asynchronously: the key comes back through the main loop this step
	// runs on.
	char *argv[] = {"xdotool", "key", "--clearmodifiers", (char *)lx_buf_cstr(&b), NULL};
	GError *err = NULL;
	bool ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
	if (err) g_error_free(err);
	ak_buf_free(&b);
	return ok;
}

// key presses a shortcut (ctrl+shift+k, f5, alt+left): a view's Keys
// that have it, else an action of the app's that has it as its
// accelerator, else the key itself through xdotool, on X11.
static bool key(const char *shortcut) {
	uint8_t mods = 0;
	const char *k = shortcut;
	for (;;) {
		const char *plus = strchr(k, '+');
		if (!plus || !plus[1]) break;
		size_t n = (size_t)(plus - k);
		if ((n == 4 && !strncasecmp(k, "ctrl", 4)) || (n == 7 && !strncasecmp(k, "control", 7)) || (n == 3 && !strncasecmp(k, "cmd", 3)) || (n == 7 && !strncasecmp(k, "primary", 7)))
			mods |= 1;
		else if (n == 5 && !strncasecmp(k, "shift", 5))
			mods |= 2;
		else if ((n == 3 && !strncasecmp(k, "alt", 3)) || (n == 6 && !strncasecmp(k, "option", 6)))
			mods |= 4;
		k = plus + 1;
	}
	char lower[64];
	size_t i = 0;
	for (; k[i] && i < sizeof lower - 1; i++) lower[i] = (char)tolower((unsigned char)k[i]);
	lower[i] = 0;
	if (lx_key_to_nodes(lower, mods)) return true;
	char *accel = lx_accelerator(lower, mods);
	if (accel && H.app) {
		char **names = gtk_application_get_actions_for_accel(H.app, accel);
		if (names && names[0]) {
			const char *dot = strchr(names[0], '.');
			g_action_group_activate_action((GActionGroup *)H.app, dot ? dot + 1 : names[0], NULL);
			g_strfreev(names);
			free(accel);
			return true;
		}
		g_strfreev(names);
	}
	free(accel);
	return xdotool_key(shortcut);
}

typedef struct {
	const char *title;
	bool done;
} pick_ctx;

// pick_item chooses a Picker's item of the title, as the user does from
// its open list.
static void pick_item(node *n, void *data) {
	pick_ctx *c = data;
	if (c->done || n->kind != AK_KIND_PICKER || !n->inner || !gtk_widget_get_mapped(n->widget)) return;
	for (uint32_t i = 0; i < n->items.len; i++)
		if (!strcmp(n->items.data[i], c->title)) {
			GtkWidget *drop = gtk_widget_get_first_child(n->inner);  // the overlay's child: the drop-down
			if (drop) gtk_drop_down_set_selected((GtkDropDown *)drop, i);
			c->done = true;
			return;
		}
}

static void write_dump(const char *path);

// ---- Real input (X11's XTest) ----

bool lx_is_x11(GdkDisplay *d);

static void *x_display(void) {
	GdkDisplay *d = gdk_display_get_default();
	if (!lx_is_x11(d) || !lx_lib_load(ASUKA_LIB_XTST) || !XTestFakeButtonEvent || !XTestFakeMotionEvent || !XTestFakeKeyEvent) return NULL;
	return gdk_x11_display_get_xdisplay(d);
}

// screen_point is where x, y of w is on the X screen.
static bool screen_point(GtkWidget *w, double x, double y, int *sx, int *sy) {
	GtkNative *nat = gtk_widget_get_native(w);
	void *dpy = x_display();
	if (!nat || !dpy) return false;
	graphene_point_t p = {(float)x, (float)y}, q;
	if (!gtk_widget_compute_point(w, (GtkWidget *)nat, &p, &q)) return false;
	double tx = 0, ty = 0;
	gtk_native_get_surface_transform(nat, &tx, &ty);
	GdkSurface *s = gtk_native_get_surface(nat);
	int scale = gdk_surface_get_scale_factor(s);
	unsigned long child;
	return XTranslateCoordinates(dpy, gdk_x11_surface_get_xid(s), gdk_x11_display_get_xrootwindow(gdk_display_get_default()),
		(int)((q.x + tx) * scale), (int)((q.y + ty) * scale), sx, sy, &child) != 0;
}

// real_click presses and lets go of the mouse's primary button at x, y of
// w, through the X server.
static bool real_click(GtkWidget *w, double x, double y) {
	void *dpy = x_display();
	int sx, sy;
	if (!dpy || !screen_point(w, x, y, &sx, &sy)) return false;
	XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
	XTestFakeButtonEvent(dpy, 1, 1, 30);
	XTestFakeButtonEvent(dpy, 1, 0, 30);
	XFlush(dpy);
	return true;
}

// node_middle is where the middle of the node showing text is on the X
// screen (its view's).
static bool node_middle(const char *text, size_t n, int *sx, int *sy, GtkWidget **w, double *mx, double *my) {
	char *t = strndup(text, n);
	node *nd = find_text_node(t);
	free(t);
	if (!nd || !x_display()) return false;
	float width, height;
	lx_size(nd, nd->widget, &width, &height);
	*w = nd->widget;
	*mx = width / 2;
	*my = height / 2;
	return screen_point(nd->widget, width / 2, height / 2, sx, sy);
}

// real_dclick double clicks the mouse's primary button at x, y of w.
static bool real_dclick(GtkWidget *w, double x, double y) {
	void *dpy = x_display();
	int sx, sy;
	if (!dpy || !screen_point(w, x, y, &sx, &sy)) return false;
	XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
	for (int i = 0; i < 2; i++) {
		XTestFakeButtonEvent(dpy, 1, 1, 30);
		XTestFakeButtonEvent(dpy, 1, 0, 30);
	}
	XFlush(dpy);
	return true;
}

// real_drag holds the primary button at x, y of w, moves the pointer by
// dx, dy in 20 steps, rests, and lets go (unless hold).
static bool real_drag(GtkWidget *w, double x, double y, double dx, double dy, bool hold) {
	void *dpy = x_display();
	int sx, sy;
	if (!dpy || !screen_point(w, x, y, &sx, &sy)) return false;
	int scale = gdk_surface_get_scale_factor(gtk_native_get_surface(gtk_widget_get_native(w)));
	XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
	XTestFakeButtonEvent(dpy, 1, 1, 30);
	for (int i = 1; i <= 20; i++) XTestFakeMotionEvent(dpy, -1, sx + (int)(dx * scale * i / 20), sy + (int)(dy * scale * i / 20), 25);
	if (!hold) XTestFakeButtonEvent(dpy, 1, 0, 150);
	XFlush(dpy);
	return true;
}

// real_release lets go of the primary button.
static bool real_release(void) {
	void *dpy = x_display();
	if (!dpy) return false;
	XTestFakeButtonEvent(dpy, 1, 0, 30);
	XFlush(dpy);
	return true;
}

// real_wheel turns the wheel notches (up above 0) at x, y of w, with Ctrl
// held if ctrl.
static bool real_wheel(GtkWidget *w, double x, double y, int notches, bool ctrl) {
	void *dpy = x_display();
	int sx, sy;
	if (!dpy || !screen_point(w, x, y, &sx, &sy)) return false;
	unsigned int code = ctrl ? XKeysymToKeycode(dpy, XStringToKeysym("Control_L")) : 0;
	XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
	if (code) XTestFakeKeyEvent(dpy, code, 1, 20);
	unsigned int button = notches > 0 ? 4 : 5;
	for (int i = 0; i < abs(notches); i++) {
		XTestFakeButtonEvent(dpy, button, 1, 40);
		XTestFakeButtonEvent(dpy, button, 0, 10);
	}
	if (code) XTestFakeKeyEvent(dpy, code, 0, 40);
	XFlush(dpy);
	return true;
}

// split_tail splits "text,a,b": the text's length, and the n numbers
// after its last n commas.
static bool split_tail(const char *arg, int n, size_t *len, double *v) {
	size_t end = strlen(arg);
	for (int i = n - 1; i >= 0; i--) {
		const char *c = NULL;
		for (size_t k = end; k-- > 0;)
			if (arg[k] == ',') {
				c = arg + k;
				break;
			}
		if (!c) return false;
		v[i] = strtod(c + 1, NULL);
		end = (size_t)(c - arg);
	}
	*len = end;
	return true;
}

// real_keys types a key with its modifiers (shift+tab, ctrl+k, b) through
// the X server, to the window the pointer is put over.
static bool real_keys(const char *spec) {
	void *dpy = x_display();
	if (!dpy || !H.main) return false;
	static const char *const names[][2] = {{"enter", "Return"}, {"escape", "Escape"}, {"esc", "Escape"}, {"tab", "Tab"}, {"space", "space"},
		{"delete", "Delete"}, {"backspace", "BackSpace"}, {"left", "Left"}, {"right", "Right"}, {"up", "Up"}, {"down", "Down"},
		{"home", "Home"}, {"end", "End"}, {"pageup", "Prior"}, {"pagedown", "Next"}, {"ctrl", "Control_L"}, {"shift", "Shift_L"}, {"alt", "Alt_L"}};
	unsigned int codes[8];
	int n = 0;
	char *copy = strdup(spec);
	for (char *part = strtok(copy, "+"); part && n < 8; part = strtok(NULL, "+")) {
		const char *name = part;
		for (size_t i = 0; i < AK_LEN(names); i++)
			if (!strcasecmp(part, names[i][0])) name = names[i][1];
		unsigned long sym = XStringToKeysym(name);
		unsigned int code = sym ? XKeysymToKeycode(dpy, sym) : 0;
		if (!code) {
			free(copy);
			return false;
		}
		codes[n++] = code;
	}
	free(copy);
	// The keys go where X's focus is: the window under the pointer.
	int sx, sy;
	if (H.main->root && screen_point(H.main->content, 1, 1, &sx, &sy)) XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
	for (int i = 0; i < n; i++) XTestFakeKeyEvent(dpy, codes[i], 1, 20);
	for (int i = n; i-- > 0;) XTestFakeKeyEvent(dpy, codes[i], 0, 20);
	XFlush(dpy);
	return true;
}

static bool step(const char *s, char *err, size_t errn) {
	const char *colon = strchr(s, ':');
	char verb[32];
	size_t vn = colon ? (size_t)(colon - s) : strlen(s);
	if (vn >= sizeof verb) vn = sizeof verb - 1;
	memcpy(verb, s, vn);
	verb[vn] = 0;
	const char *arg = colon ? colon + 1 : "";
	if (!strcmp(verb, "tap")) {
		// asuka dev's error overlay, over everything (and modal).
		if (lx_dev_error_tap(arg)) return true;
		node *n = find_text_node(arg);
		if (n) {
			tap(n);
			return true;
		}
		// A header bar's button, a sidebar's row, a dialog's button: GTK's
		// own widgets, by their label or tooltip.
		if (tap_widget(arg)) return true;
		// A Picker's item, chosen as from its open list.
		pick_ctx pc = {arg, false};
		lx_each_node(pick_item, &pc);
		if (pc.done) return true;
		snprintf(err, errn, "nothing shows \"%s\"", arg);
		return false;
	}
	if (!strcmp(verb, "click")) {
		node *n = find_text_node(arg);
		float w, h;
		if (n) lx_size(n, n->widget, &w, &h);
		if (n && real_click(n->widget, w / 2, h / 2)) return true;
		snprintf(err, errn, n ? "no X11 XTest to click with" : "nothing shows \"%s\"", arg);
		return false;
	}
	if (!strcmp(verb, "clickat")) {
		double x = 0, y = 0;
		sscanf(arg, "%lf,%lf", &x, &y);
		if (H.main && H.main->root && H.main->root->widget && real_click(H.main->root->widget, x, y)) return true;
		snprintf(err, errn, "no X11 XTest to click with");
		return false;
	}
	if (!strcmp(verb, "dclick") || !strcmp(verb, "drag") || !strcmp(verb, "wheel")) {
		double v[3] = {0};
		size_t len = strlen(arg);
		bool flag = false;  // a drag's hold, a wheel's ctrl
		bool ok = true;
		const char *suffix = !strcmp(verb, "drag") ? ",hold" : ",ctrl";
		flag = len > 5 && !strcmp(arg + len - 5, suffix);
		if (strcmp(verb, "dclick")) {
			char *a = strndup(arg, flag ? len - 5 : len);
			ok = split_tail(a, !strcmp(verb, "drag") ? 2 : 1, &len, v);
			free(a);
		}
		int sx, sy;
		GtkWidget *w = NULL;
		double mx = 0, my = 0;
		if (!ok || !node_middle(arg, len, &sx, &sy, &w, &mx, &my)) {
			snprintf(err, errn, ok ? "nothing shows it, or no X11 XTest" : "a step needs its text and numbers");
			return false;
		}
		if (!strcmp(verb, "dclick")) return real_dclick(w, mx, my);
		if (!strcmp(verb, "drag")) return real_drag(w, mx, my, v[0], v[1], flag);
		return real_wheel(w, mx, my, (int)v[0], flag);
	}
	if (!strcmp(verb, "release")) {
		if (real_release()) return true;
		snprintf(err, errn, "no X11 XTest to let go with");
		return false;
	}
	if (!strcmp(verb, "press")) {
		if (real_keys(arg)) return true;
		snprintf(err, errn, "%s: no X11 XTest, or a key it hasn't", arg);
		return false;
	}
	if (!strcmp(verb, "key")) {
		if (key(arg)) return true;
		snprintf(err, errn, "%s: not an app shortcut, and xdotool isn't installed to press it", arg);
		return false;
	}
	if (!strcmp(verb, "type")) {
		node *n = focused_field();
		if (!n) {
			snprintf(err, errn, "no field has the focus");
			return false;
		}
		lx_insert_text(n, ak_cstr(arg));
		return true;
	}
	if (!strcmp(verb, "submit")) {
		node *n = focused_field();
		if (n) lx_emit(n, (ak_event){.kind = AK_EVENT_SUBMIT});
		return true;
	}
	if (!strcmp(verb, "back")) {
		if (lx_go_back_front()) return true;
		snprintf(err, errn, "no screen to go back from");
		return false;
	}
	if (!strcmp(verb, "escape")) {
		escape();
		return true;
	}
	if (!strcmp(verb, "scroll")) {
		scroll_ctx c = {strtod(arg, NULL), false};
		lx_each_node(scroll_first, &c);
		return true;
	}
	if (!strcmp(verb, "resize")) {
		int w = 0, h = 0;
		sscanf(arg, "%d,%d", &w, &h);
		gtk_window_set_default_size(H.main->window, w, h + AK_HEADER_HEIGHT);
		return true;
	}
	if (!strcmp(verb, "menu")) {
		if (lx_activate_app_item(arg)) return true;
		snprintf(err, errn, "no menu item \"%s\"", arg);
		return false;
	}
	if (!strcmp(verb, "close")) {
		win *w = H.active_win ? H.active_win : H.main;
		gtk_window_close(w->window);
		return true;
	}
	if (!strcmp(verb, "snap")) {
		// The window as a PNG, and with ASUKA_DUMP its views beside it
		// (file.txt), as the end's snapshot.
		lx_snapshot(arg);
		const char *dump = getenv("ASUKA_DUMP");
		if (dump && *dump) {
			char p[4096];
			snprintf(p, sizeof p, "%s.txt", arg);
			write_dump(p);
		}
		return true;
	}
	if (!strcmp(verb, "tab")) {
		tab_ctx c = {arg, false};
		lx_each_node(choose_tab, &c);
		if (c.done) return true;
		snprintf(err, errn, "no tab \"%s\"", arg);
		return false;
	}
	snprintf(err, errn, "unknown step");
	return false;
}

// ---- Snapshots and the dump ----

// lx_snapshot_window writes a window (its header bar and content) as a
// PNG, rendered by its own renderer.
void lx_snapshot_window(GtkWindow *window, const char *path) {
	GdkPaintable *paint = gtk_widget_paintable_new((GtkWidget *)window);
	int width = gtk_widget_get_width((GtkWidget *)window), height = gtk_widget_get_height((GtkWidget *)window);
	GtkSnapshot *s = gtk_snapshot_new();
	gdk_paintable_snapshot(paint, (GdkSnapshot *)s, width, height);
	GskRenderNode *nd = gtk_snapshot_free_to_node(s);
	g_object_unref(paint);
	if (!nd) return;
	GskRenderer *r = gtk_native_get_renderer((GtkNative *)window);
	graphene_rect_t vp = {{0, 0}, {(float)width, (float)height}};
	GdkTexture *t = r ? gsk_renderer_render_texture(r, nd, &vp) : NULL;
	gsk_render_node_unref(nd);
	if (!t) return;
	gdk_texture_save_to_png(t, path);
	g_object_unref(t);
}

// lx_snapshot writes the main window (a path with ".window" in it: the
// main window still), then the app's other windows beside it.
void lx_snapshot(const char *path) {
	lx_snapshot_window(H.main->window, path);
	int i = 0;
	for (uint32_t k = 0; k < H.wins.len; k++) {
		win *x = H.wins.data[k];
		if (x == H.main || x->closing) continue;
		i++;
		char p[4096];
		size_t n = strlen(path);
		if (n > 4 && !strcmp(path + n - 4, ".png")) n -= 4;
		if (i > 1)
			snprintf(p, sizeof p, "%.*s.window%d.png", (int)n, path, i);
		else
			snprintf(p, sizeof p, "%.*s.window.png", (int)n, path);
		lx_snapshot_window(x->window, p);
	}
}

static void quote(ak_buf *b, const char *s) { ak_fmt_quote(b, ak_cstr(s)); }

// lx_dump_node describes a node and what's in it: its kind, frame and
// what it shows, a line each, indented by depth.
void lx_dump_node(node *n, int depth, ak_buf *b) {
	for (int i = 0; i < depth; i++) ak_buf_put(b, "  ", 2);
	const char *kind = n->kind < AK_KIND_COUNT && ak_kind_names[n->kind] ? ak_kind_names[n->kind] : "Cell";
	ak_buf_printf(b, "%u %s [%.0f,%.0f %.0fx%.0f]", n->id, kind, n->frame[0], n->frame[1], n->frame[2], n->frame[3]);
	if (n->hidden) ak_buf_put(b, " hidden", 7);
	if (n->widget && !gtk_widget_get_mapped(n->widget)) ak_buf_put(b, " unmapped", 9);
	if (n->rtl) ak_buf_put(b, " rtl", 4);
	// The keyboard's focus, as GTK has it.
	if (n->focusable) ak_buf_put(b, " focusable", 10);
	if (n->focus_trap) ak_buf_put(b, " trap", 5);
	if (n->widget && gtk_widget_has_css_class) {
		GtkWidget *c = lx_control_of(n);
		if (c && gtk_widget_has_css_class(c, "asuka-no-focus-ring")) ak_buf_put(b, " noring", 7);
	}
	win *w = n->widget ? lx_win_of(n) : NULL;
	if (w && w->window) {
		GtkWidget *f = gtk_window_get_focus(w->window);
		while (f && !lx_view_node(f)) f = gtk_widget_get_parent(f);
		if (f && lx_view_node(f) == n) {
			ak_buf_put(b, " focused", 8);
			if (gtk_window_get_focus_visible && gtk_window_get_focus_visible(w->window)) ak_buf_put(b, "-visible", 8);
		}
	}
	lx_a11y_dump(n, b);
	if (!str_empty(n->text)) {
		ak_buf_u8(b, ' ');
		quote(b, n->text);
	}
	if (!str_empty(n->title)) {
		ak_buf_put(b, " title=", 7);
		quote(b, n->title);
	}
	ak_buf_u8(b, '\n');
	// A click-through window's input region: its views that take clicks.
	if (n->win && n->win->desk.through && n == (n->win->node ? n->win->node : n->win->root))
		for (size_t i = 0; i + 3 < n->win->desk.region.len; i += 4) {
			for (int d = 0; d <= depth; d++) ak_buf_put(b, "  ", 2);
			const float *r = n->win->desk.region.data + i;
			ak_buf_printf(b, "click region %.0f %.0f %.0f %.0f\n", r[0], r[1], r[2], r[3]);
		}
	for (uint32_t i = 0; i < n->children.len; i++) lx_dump_node(n->children.data[i], depth + 1, b);
	for (size_t i = 0; i < lx_area_count; i++)
		if (lx_areas[i]->dump) lx_areas[i]->dump(n, depth, b);
}

static void dump_floating(node *n, void *data) {
	if (!n->parent && n != H.root && (n->kind == AK_KIND_SHEET || n->kind == AK_KIND_WINDOW)) lx_dump_node(n, 0, data);
}

static void write_dump(const char *path) {
	ak_buf b = {0};
	if (H.root) lx_dump_node(H.root, 0, &b);
	lx_each_node(dump_floating, &b);
	lx_dev_error_dump(&b);
	lx_dev_perf_dump(&b);
	dsk_write_file(path, b.data ? (const char *)b.data : "", b.len);
	ak_buf_free(&b);
}

typedef struct {
	node_vec shown;
} texts_ctx;

static void screen_text(node *n, void *data) {
	texts_ctx *c = data;
	if (!n->widget || n->hidden || !gtk_widget_get_mapped(n->widget)) return;
	if (!str_empty(n->text) || (n->kind == AK_KIND_BUTTON && !str_empty(n->title)) || !str_empty(n->a11y_label)) ak_vec_push(&c->shown, n);
}

static int by_id(const void *a, const void *b) {
	ak_id x = (*(node *const *)a)->id, y = (*(node *const *)b)->id;
	return x < y ? -1 : x > y;
}

// write_texts writes what the screen shows: each mapped node's text, else
// a button's title, else its accessibility label, a line each, in
// creation order.
static void write_texts(const char *path) {
	texts_ctx c = {0};
	lx_each_node(screen_text, &c);
	if (c.shown.len > 1) qsort(c.shown.data, c.shown.len, sizeof *c.shown.data, by_id);
	ak_buf b = {0};
	for (uint32_t i = 0; i < c.shown.len; i++) {
		node *n = c.shown.data[i];
		const char *t = n->text;
		if (str_empty(t) && n->kind == AK_KIND_BUTTON) t = n->title;
		if (str_empty(t)) t = n->a11y_label;
		for (const char *p = S(t); *p; p++) ak_buf_u8(&b, *p == '\n' ? ' ' : (uint8_t)*p);
		ak_buf_u8(&b, '\n');
	}
	ak_vec_free(&c.shown);
	lx_dev_error_texts(&b);
	dsk_write_file(path, b.data ? (const char *)b.data : "", b.len);  // whole, then moved in place
	ak_buf_free(&b);
}

// ---- The script ----

// settle lets what's coming in finish first (asuka dev's error overlay
// fades in): the main loop runs until it has, for up to 5 s.
static void settle(void) {
	gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	while (lx_dev_error_settling() && g_get_monotonic_time() < until)
		if (!g_main_context_iteration(NULL, 0)) g_usleep(2000);
}

static gboolean next_step(gpointer data);

static void schedule(guint ms) { g_timeout_add(ms, next_step, NULL); }

static gboolean next_step(gpointer data) {
	(void)data;
	if (waiting_until) {
		// waitfor: a file, polled for up to a minute.
		const char *p = steps[step_at] + strlen("waitfor:");
		struct stat st;
		if (stat(p, &st) != 0 && g_get_monotonic_time() < waiting_until) return 1;
		waiting_until = 0;
		step_at++;
		schedule(1);
		return 0;
	}
	if (step_at < step_count) {
		char *s = steps[step_at];
		while (*s == ' ') s++;
		if (!strncmp(s, "wait:", 5)) {
			// A duration, as Go writes one: 500ms, 2s.
			double v = strtod(s + 5, NULL);
			const char *unit = s + 5;
			while (*unit && (isdigit((unsigned char)*unit) || *unit == '.')) unit++;
			guint ms = !strcmp(unit, "ms") ? (guint)v : !strcmp(unit, "m") ? (guint)(v * 60000) : (guint)(v * 1000);
			step_at++;
			schedule(ms ? ms : 1);
			return 0;
		}
		if (!strncmp(s, "waitfor:", 8)) {
			memmove(steps[step_at], s, strlen(s) + 1);
			waiting_until = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
			g_timeout_add(20, next_step, NULL);
			return 0;
		}
		char err[256];
		settle();
		lx_enter_begin();
		bool ok = step(s, err, sizeof err);
		lx_enter_end();
		if (!ok) fprintf(stderr, "asuka: script: %s: %s\n", s, err);
		step_at++;
		schedule(400);  // animations settle
		return 0;
	}
	if (!snap_path) return 0;
	settle();
	lx_snapshot(snap_path);
	const char *dump = getenv("ASUKA_DUMP");
	if (dump && *dump) {
		char p[4096];
		snprintf(p, sizeof p, "%s.txt", snap_path);
		write_dump(p);
	}
	const char *stay = getenv("ASUKA_SNAPSHOT_STAY");
	if (!stay || !*stay) {
		lx_routes_flush();  // as a quit writes them
		exit(0);
	}
	return 0;
}

// ---- ASUKA_COMMANDS ----

static char *commands_dir;

static gboolean poll_commands(gpointer data) {
	(void)data;
	char *file = dsk_join(commands_dir, "asuka-commands.txt");
	size_t n;
	char *text = dsk_read_file(file, &n);
	if (!text) {
		free(file);
		return 1;
	}
	remove(file);
	free(file);
	// strtok_r: the main loop may run between the lines (settle).
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		while (*line == ' ' || *line == '\t') line++;
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
		if (!len) continue;
		char *sp = strchr(line, ' ');
		char verb[32];
		size_t vn = sp ? (size_t)(sp - line) : len;
		if (vn >= sizeof verb) vn = sizeof verb - 1;
		memcpy(verb, line, vn);
		verb[vn] = 0;
		const char *arg = sp ? sp + 1 : "";
		char err[256] = "";
		bool ok = true;
		settle();
		lx_enter_begin();
		if (!strcmp(verb, "dump")) {
			char *p = dsk_join(commands_dir, "asuka-dump.txt");
			write_texts(p);
			free(p);
		} else {
			if (!strcmp(verb, "type") && !focused_field()) {
				find_ctx c = {0};
				lx_each_node(first_field, &c);
				if (c.found) lx_focus_field(c.found);
			}
			char s[1024];
			snprintf(s, sizeof s, "%s:%s", verb, arg);
			ok = step(s, err, sizeof err);
		}
		lx_enter_end();
		if (!ok) fprintf(stderr, "asuka: automation: %s: %s\n", line, err);
		// As the script's steps: a button takes no second activation while
		// it shows the first.
		if (strcmp(verb, "dump") != 0) g_usleep(400000);
	}
	free(text);
	return 1;
}

void lx_automation(void) {
	const char *dir = getenv("ASUKA_COMMANDS");
	if (dir && *dir) {
		commands_dir = strdup(dir);
		g_timeout_add(50, poll_commands, NULL);
	}
	const char *script = getenv("ASUKA_SCRIPT"), *snap = getenv("ASUKA_SNAPSHOT");
	if ((!script || !*script) && (!snap || !*snap)) return;
	if (snap && *snap) snap_path = strdup(snap);
	if (script && *script) {
		char *copy = strdup(script);
		for (char *s = strtok(copy, ";"); s; s = strtok(NULL, ";")) {
			steps = realloc(steps, (step_count + 1) * sizeof *steps);
			if (!steps) abort();
			steps[step_count++] = strdup(s);
		}
		free(copy);
	}
	schedule(1200);  // the window shows, images load
}
