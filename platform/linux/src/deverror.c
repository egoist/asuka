// deverror.c: asuka dev's error overlay (the DevError command, to the
// root; ops/deverror.go, the core's devoverlay.c): an error the app
// didn't catch, a module that didn't load, a build that failed. It's
// GTK's and libadwaita's own widgets, not the app's views (the app's
// tree may be what's broken): an AdwDialog over the root's window, as big
// as the window and sized again with it, presented after the app's own
// sheets and alerts (the dialog host draws the last presented on top),
// and presented again whenever one of the app's comes over it. In it, the
// title (small, in the error color; the warning color for a build), the
// message, the lines of the source where it went wrong in a card and the
// stack, selectable, scrolling when long; under them Dismiss and, when the
// core offers it, Reload. They send DevErrorAction to the root, as Escape
// does Dismiss; the core removes it (DevErrorClear) or sends what shows
// next.
#include "host.h"

#include <math.h>
#include <stdlib.h>

static struct {
	AdwDialog *dialog;  // NULL: nothing shows
	ak_id root;
	win *w;
	uint8_t flags;
	char *title, *message, *source, *details;
	GtkWidget *content;  // the dialog's child
	GtkWidget *title_l, *message_l, *card, *source_l, *details_l, *dismiss, *reload;
	int above;  // the widgets after the dialog's, up to its window, as presented
	bool touched;  // pressed or typed in since it was presented
	GtkWindow *watched;  // the window whose focus it watches
} E;

static void act(int64_t a) {
	if (!E.dialog) return;
	ak_event ev = {.id = E.root, .kind = AK_EVENT_DEV_ERROR_ACTION, .i = a};
	lx_dispatch(&ev);
}

static void on_dismiss(GtkButton *b, gpointer data) {
	(void)b, (void)data;
	act(AK_DEV_ERROR_ACTION_DISMISS);
}

static void on_reload(GtkButton *b, gpointer data) {
	(void)b, (void)data;
	act(AK_DEV_ERROR_ACTION_RELOAD);
}

// Escape (and a bottom sheet swiped away): the dialog can't close, it asks.
static void on_close_attempt(AdwDialog *d, gpointer data) {
	(void)d, (void)data;
	act(AK_DEV_ERROR_ACTION_DISMISS);
}

// closed: gone with its window, not by the core's word.
static void on_closed(AdwDialog *d, gpointer data) {
	(void)data;
	if (d != E.dialog) return;
	g_object_unref(E.dialog);
	E.dialog = NULL;
}

static bool offers_reload(void) { return E.flags & AK_DEV_ERROR_RELOAD; }

static GtkWidget *label(const char *cls, bool mono) {
	GtkWidget *l = gtk_label_new("");
	gtk_label_set_xalign((GtkLabel *)l, 0);
	gtk_label_set_selectable((GtkLabel *)l, 1);
	if (!mono) {
		gtk_label_set_wrap((GtkLabel *)l, 1);
		gtk_label_set_wrap_mode((GtkLabel *)l, PANGO_WRAP_WORD_CHAR);
	} else {
		gtk_widget_add_css_class(l, "monospace");
	}
	if (cls) gtk_widget_add_css_class(l, cls);
	return l;
}

static void margins(GtkWidget *w, int top, int side, int bottom) {
	gtk_widget_set_margin_top(w, top);
	gtk_widget_set_margin_bottom(w, bottom);
	gtk_widget_set_margin_start(w, side);
	gtk_widget_set_margin_end(w, side);
}

// size makes the dialog as big as its window.
static void size(void) {
	if (!E.dialog || !E.w) return;
	int ww = gtk_widget_get_width((GtkWidget *)E.w->window), wh = gtk_widget_get_height((GtkWidget *)E.w->window);
	if (ww <= 0 || wh <= 0) gtk_window_get_default_size(E.w->window, &ww, &wh);  // not shown yet
	adw_dialog_set_content_width(E.dialog, ww > 1 ? ww : 1);
	adw_dialog_set_content_height(E.dialog, wh > 1 ? wh : 1);
}

// above counts the widgets drawn after the dialog's, from it up to its
// window: the dialogs presented after it (the dialog host's later
// children) among them.
static int above(void) {
	int n = 0;
	GtkWidget *top = E.w ? (GtkWidget *)E.w->window : NULL;
	for (GtkWidget *x = (GtkWidget *)E.dialog; x && x != top; x = gtk_widget_get_parent(x))
		for (GtkWidget *s = gtk_widget_get_next_sibling(x); s; s = gtk_widget_get_next_sibling(s)) n++;
	return n;
}

// unselect takes back the selection a label made as it took the focus.
static gboolean unselect(gpointer data) {
	(void)data;
	if (E.dialog && !E.touched) {
		GtkWidget *labels[] = {E.title_l, E.message_l, E.source_l, E.details_l};
		for (size_t i = 0; i < sizeof labels / sizeof *labels; i++) gtk_label_select_region((GtkLabel *)labels[i], 0, 0);
	}
	return 0;
}

// on_focus: the window's focus moved. Presented over another dialog, the
// dialog host focuses the overlay's first label, which selects all it
// shows: until the user presses or types in it, the focus that lands on a
// label goes to its button instead, and the label selects nothing (once
// it's done taking the focus).
static void on_focus(GObject *o, GParamSpec *ps, gpointer data) {
	(void)ps, (void)data;
	if (!E.dialog || E.touched) return;
	GtkWidget *f = gtk_window_get_focus((GtkWindow *)o);
	if (f == E.title_l || f == E.message_l || f == E.source_l || f == E.details_l) {
		gtk_widget_grab_focus(offers_reload() ? E.reload : E.dismiss);
		g_idle_add_full(G_PRIORITY_DEFAULT, unselect, NULL, NULL);
	}
}

static void on_press(GtkGestureClick *g, int n, double x, double y, gpointer data) {
	(void)g, (void)n, (void)x, (void)y, (void)data;
	E.touched = true;
}

static gboolean on_key(GtkEventControllerKey *k, guint key, guint code, GdkModifierType mods, gpointer data) {
	(void)k, (void)key, (void)code, (void)mods, (void)data;
	E.touched = true;
	return 0;
}

// build makes the dialog's widgets, which fill sets to what shows.
static GtkWidget *build(void) {
	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	margins(body, 24, 24, 24);
	E.title_l = label("caption-heading", false);
	gtk_box_append((GtkBox *)body, E.title_l);
	E.message_l = label("title-3", false);
	gtk_box_append((GtkBox *)body, E.message_l);
	// The source: a card, scrolling sideways, the lines as they are.
	E.source_l = label(NULL, true);
	margins(E.source_l, 12, 14, 12);
	E.card = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy((GtkScrolledWindow *)E.card, GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_propagate_natural_height((GtkScrolledWindow *)E.card, 1);
	gtk_scrolled_window_set_child((GtkScrolledWindow *)E.card, E.source_l);
	gtk_widget_add_css_class(E.card, "card");
	gtk_widget_set_overflow(E.card, GTK_OVERFLOW_HIDDEN);
	gtk_box_append((GtkBox *)body, E.card);
	// The details: the stack, or the build's other errors, wrapped.
	E.details_l = label("dim-label", true);
	gtk_label_set_wrap((GtkLabel *)E.details_l, 1);
	gtk_label_set_wrap_mode((GtkLabel *)E.details_l, PANGO_WRAP_WORD_CHAR);
	gtk_box_append((GtkBox *)body, E.details_l);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy((GtkScrolledWindow *)scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child((GtkScrolledWindow *)scroll, body);
	gtk_widget_set_vexpand(scroll, 1);

	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_set_homogeneous((GtkBox *)bar, 1);
	margins(bar, 12, 24, 12);
	E.dismiss = gtk_button_new_with_label("Dismiss");
	g_signal_connect(E.dismiss, "clicked", on_dismiss, NULL);
	gtk_box_append((GtkBox *)bar, E.dismiss);
	E.reload = gtk_button_new_with_label("Reload");
	gtk_widget_add_css_class(E.reload, "suggested-action");
	g_signal_connect(E.reload, "clicked", on_reload, NULL);
	gtk_box_append((GtkBox *)bar, E.reload);

	GtkWidget *tv = adw_toolbar_view_new();
	GtkGesture *press = gtk_gesture_click_new();
	gtk_gesture_single_set_button((GtkGestureSingle *)press, 0);
	gtk_event_controller_set_propagation_phase((GtkEventController *)press, GTK_PHASE_CAPTURE);
	g_signal_connect(press, "pressed", on_press, NULL);
	gtk_widget_add_controller(tv, (GtkEventController *)press);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", on_key, NULL);
	gtk_widget_add_controller(tv, keys);
	adw_toolbar_view_set_content((AdwToolbarView *)tv, scroll);
	adw_toolbar_view_add_bottom_bar((AdwToolbarView *)tv, bar);
	adw_toolbar_view_set_bottom_bar_style((AdwToolbarView *)tv, ADW_TOOLBAR_RAISED_BORDER);
	return tv;
}

// fill sets the dialog's widgets to what E holds.
static void fill(void) {
	adw_dialog_set_title(E.dialog, S(E.title));
	gtk_label_set_text((GtkLabel *)E.title_l, S(E.title));
	bool failed = E.flags & AK_DEV_ERROR_BUILD;  // a build's: a warning's color
	gtk_widget_remove_css_class(E.title_l, failed ? "error" : "warning");
	gtk_widget_add_css_class(E.title_l, failed ? "warning" : "error");
	gtk_label_set_text((GtkLabel *)E.message_l, S(E.message));
	gtk_label_set_text((GtkLabel *)E.source_l, S(E.source));
	gtk_widget_set_visible(E.card, !str_empty(E.source));
	gtk_label_set_text((GtkLabel *)E.details_l, S(E.details));
	gtk_widget_set_visible(E.details_l, !str_empty(E.details));
	if (gtk_widget_get_visible(E.reload) != offers_reload()) {
		gtk_widget_set_visible(E.reload, offers_reload());
		// The focus on its button, not a label (which would select all it
		// shows).
		adw_dialog_set_focus(E.dialog, offers_reload() ? E.reload : E.dismiss);
	}
}

// drop closes the dialog without telling the core (it said so, or it's
// presented again).
static void drop(void) {
	if (!E.dialog) return;
	AdwDialog *d = E.dialog;
	E.dialog = NULL;
	g_signal_handlers_disconnect_matched(d, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, (gpointer)on_closed, NULL);
	g_signal_handlers_disconnect_matched(d, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, (gpointer)on_close_attempt, NULL);
	adw_dialog_force_close(d);
	g_object_unref(d);
}

static void present(void) {
	drop();
	if (!E.w) return;
	AdwDialog *d = adw_dialog_new();
	g_object_ref_sink(d);
	adw_dialog_set_can_close(d, 0);
	g_signal_connect(d, "close-attempt", on_close_attempt, NULL);
	g_signal_connect(d, "closed", on_closed, NULL);
	E.dialog = d;
	E.content = build();
	adw_dialog_set_child(d, E.content);
	fill();
	size();
	if (E.watched != E.w->window) {
		if (E.watched) g_signal_handlers_disconnect_matched(E.watched, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, (gpointer)on_focus, NULL);
		E.watched = E.w->window;
		g_signal_connect(E.watched, "notify::focus-widget", on_focus, NULL);
	}
	E.touched = false;
	adw_dialog_present(d, (GtkWidget *)E.w->window);
	adw_dialog_set_focus(d, offers_reload() ? E.reload : E.dismiss);
	E.above = above();
}

void lx_dev_error(node *n, ak_span args) {
	ak_dev_error e;
	if (!ak_decode_dev_error((const uint8_t *)args.p, args.len, &e)) return;
	win *w = lx_win_for(n);
	if (!w) w = H.main;
	if (!w) return;
	E.root = n->id;
	E.flags = e.flags;
	str_set(&E.title, e.title);
	str_set(&E.message, e.message);
	str_set(&E.source, e.source);
	str_set(&E.details, e.details);
	// Shown already, on top, over the same window: what it shows changes
	// in place (the source of an app's error comes a moment after it).
	if (E.dialog && E.w == w && above() <= E.above) {
		fill();
		return;
	}
	E.w = w;
	present();
}

void lx_dev_error_clear(void) {
	drop();
	E.w = NULL;
}

// lx_dev_error_raise presents the overlay again if a dialog of the app's
// came over it.
void lx_dev_error_raise(void) {
	if (E.dialog && above() > E.above) present();
}

// lx_dev_error_resized: a window resized, maybe the overlay's.
void lx_dev_error_resized(win *w) {
	if (E.dialog && E.w == w) size();
}

// lx_dev_error_closing: a window goes, maybe the overlay's.
void lx_dev_error_closing(win *w) {
	if (E.w == w) lx_dev_error_clear();
	if (E.watched == w->window) E.watched = NULL;
}

// lx_dev_error_over: the overlay shows over w, which keeps its keys from
// the app's views.
bool lx_dev_error_over(win *w) { return E.dialog && E.w == w; }

// ---- Automation ----

static bool fading(GtkWidget *w) {
	if (!gtk_widget_get_mapped(w)) return false;
	if (gtk_widget_get_opacity(w) < 1) return true;
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
		if (fading(c)) return true;
	return false;
}

// scaled: its content is drawn at other than its size (libadwaita's
// dialog scales its sheet in).
static bool scaled(void) {
	graphene_rect_t r;
	if (!gtk_widget_get_mapped(E.content) || !gtk_widget_compute_bounds(E.content, (GtkWidget *)E.w->window, &r)) return false;
	return fabsf(r.size.width - (float)gtk_widget_get_width(E.content)) > 0.5f || fabsf(r.size.height - (float)gtk_widget_get_height(E.content)) > 0.5f;
}

// lx_dev_error_settling: the overlay is still coming in (libadwaita's
// dialog fades its dimming and its sheet in, their opacity from 0 to 1,
// and scales the sheet to its size): automation waits, so that a step or
// a snapshot sees it as the user does once it's there.
bool lx_dev_error_settling(void) { return E.dialog && E.w && (fading((GtkWidget *)E.dialog) || scaled()); }

// lx_dev_error_tap presses the overlay's button titled text.
bool lx_dev_error_tap(const char *text) {
	if (!E.dialog) return false;
	if (str_eq(text, "Dismiss")) {
		g_signal_emit_by_name(E.dismiss, "clicked");
		return true;
	}
	if (offers_reload() && str_eq(text, "Reload")) {
		g_signal_emit_by_name(E.reload, "clicked");
		return true;
	}
	return false;
}

// lx_dev_error_escape is Escape over the overlay: Dismiss.
bool lx_dev_error_escape(void) {
	if (!E.dialog) return false;
	act(AK_DEV_ERROR_ACTION_DISMISS);
	return true;
}

// lx_dev_error_dump writes what the overlay shows for ASUKA_DUMP, a line
// each.
void lx_dev_error_dump(ak_buf *b) {
	if (!E.dialog) return;
	// covered: a dialog of the app's came over it, and it wasn't raised.
	ak_buf_printf(b, "DevError%s%s%s ", E.flags & AK_DEV_ERROR_BUILD ? " build" : "", E.flags & AK_DEV_ERROR_RELOAD ? " reload" : "",
		above() > E.above ? " covered" : "");
	ak_fmt_quote(b, ak_cstr(S(E.title)));
	ak_buf_printf(b, "\n  message ");
	ak_fmt_quote(b, ak_cstr(S(E.message)));
	if (!str_empty(E.source)) {
		ak_buf_printf(b, "\n  source ");
		ak_fmt_quote(b, ak_cstr(E.source));
	}
	if (!str_empty(E.details)) {
		ak_buf_printf(b, "\n  details ");
		ak_fmt_quote(b, ak_cstr(E.details));
	}
	ak_buf_printf(b, "\n  Button title=");
	ak_fmt_quote(b, ak_cstr("Dismiss"));
	if (offers_reload()) {
		ak_buf_printf(b, "\n  Button title=");
		ak_fmt_quote(b, ak_cstr("Reload"));
	}
	ak_buf_u8(b, '\n');
}

static void put_lines(ak_buf *b, const char *s) {
	if (str_empty(s)) return;
	ak_buf_put(b, s, strlen(s));
	if (s[strlen(s) - 1] != '\n') ak_buf_u8(b, '\n');
}

// lx_dev_error_texts writes what the overlay shows for ASUKA_COMMANDS'
// dump: its title, message, source and details, a line each of theirs,
// and its buttons' titles.
void lx_dev_error_texts(ak_buf *b) {
	if (!E.dialog) return;
	put_lines(b, E.title);
	put_lines(b, E.message);
	put_lines(b, E.source);
	put_lines(b, E.details);
	put_lines(b, "Dismiss");
	if (offers_reload()) put_lines(b, "Reload");
}
