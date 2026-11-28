// nav.c: navigation with libadwaita's widgets: a Stack is an
// AdwNavigationView (its pages slide, a swipe or Alt+Left goes back), each
// Screen a page; Tabs an AdwViewStack with a view switcher (in the header
// bar on a wide window, a bar at the bottom of a narrow one); a Sheet an
// AdwDialog (a floating dialog, a bottom sheet in a narrow window), a
// FullScreen one an AdwDialog as big as its window; an Alert an
// AdwAlertDialog; a Snackbar an AdwToast; a Window a window of its own.
//
// A screen's Toolbar and Primary action are a GtkActionBar at the bottom
// of its page, as GNOME's apps put a view's actions: the page's child is
// an AdwToolbarView with the screen's view as its content and the action
// bar as its bottom bar.
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void lx_set_a11y_string(gpointer w, int prop, const char *s);
const char *lx_badged(const char *title, const char *badge);
const char *lx_word(const char *key);
void lx_search_set_text(search_field *s, const char *t);
void lx_forget_search(node *screen);
void lx_forget_chrome(node *n);
void lx_insert_split(node *p, node *c, uint32_t index);
void lx_remove_split(node *p, node *c);
void lx_watch_preedit(GtkWidget *text);
#define A11Y_PROP_LABEL 4

static nav *nav_of(node *n) {
	if (!n->nav) {
		n->nav = calloc(1, sizeof *n->nav);
		if (!n->nav) abort();
	}
	return n->nav;
}

void lx_nav_free(node *n) {
	nav *v = n->nav;
	if (!v) return;
	free(v->search_ph);
	free(v->search_tx);
	free(v->badge);
	ak_buf_free(&v->fields_buf);
	ak_buf_free(&v->detents);
	ak_vec_free(&v->fields);
	ak_vec_free(&v->rows);
	actions_free(&v->toolbar);
	actions_free(&v->primary);
	free(v);
	n->nav = NULL;
}

// ---- Layout roots ----

// lx_root_allocated: a screen, sheet, window or popover was sized; the
// core lays it out at that size.
static bool sheet_fit_only(node *n);
static void sheet_room(node *n, float *w, float *h);
static void follow_height(node *n);

void lx_root_allocated(node *n, int width, int height) {
	nav *v = nav_of(n);
	float w = (float)width, h = (float)height;
	if (n->kind == AK_KIND_SHEET) {
		follow_height(n);
		// As tall as its content: its room, which the core lays it out in
		// (and sizes it to with Fit), not what it's sized to.
		if (sheet_fit_only(n)) sheet_room(n, &w, &h);
	}
	if (v->env_w == w && v->env_h == h) return;
	v->env_w = w;
	v->env_h = h;
	ak_event ev = {.id = n->id, .kind = AK_EVENT_ENVIRONMENT};
	lx_root_env(n, &ev.env);
	lx_dispatch(&ev);
}

// lx_root_env is a host-sized root's environment: its window's, at its
// size.
void lx_root_env(node *n, ak_environment *env) {
	if (n->kind == AK_KIND_OVERLAY) {
		lx_overlay_env(n, env);  // its root's, at its size
		return;
	}
	*env = H.env;
	win *w = lx_win_of(n);
	if (w && w != H.main) lx_win_env(w, env);
	if (n->nav) {
		env->width = n->nav->env_w;
		env->height = n->nav->env_h;
		env->width_class = ak_width_class_of(env->width);
		env->height_class = ak_height_class_of(env->height);
	}
}

void lx_each_node(void (*f)(node *n, void *ctx), void *ctx);

static void root_env_again(node *n, void *ctx) {
	(void)ctx;
	if (lx_is_layout_root(n) && n->nav && (n->nav->env_w > 0 || n->nav->env_h > 0)) {
		ak_event ev = {.id = n->id, .kind = AK_EVENT_ENVIRONMENT};
		lx_root_env(n, &ev.env);
		lx_dispatch(&ev);
	}
}

// lx_roots_env_changed reports the environment again to every root the
// host sizes (the style or the text size changed).
void lx_roots_env_changed(void) { lx_each_node(root_env_again, NULL); }

// lx_win_of is the window a node is in.
win *lx_win_of(node *n) {
	for (node *p = n; p; p = p->parent) {
		if (p->win) return p->win;
		if (H.main && p == H.main->root) return H.main;
	}
	if (n->widget) {
		GtkRoot *r = gtk_widget_get_root(n->widget);
		win *w = r ? lx_win_by_window(r) : NULL;
		if (w) return w;
	}
	return H.main;
}

GtkWidget *lx_page_child(node *n) { return n->nav && n->nav->page_child ? n->nav->page_child : n->widget; }

// ---- Making them ----

static void page_popped(node *stack, gpointer page);

static void on_popped(AdwNavigationView *v, AdwNavigationPage *page, node *n) {
	(void)v;
	page_popped(n, page);
}

static void on_visible_page(GObject *o, GParamSpec *ps, node *n) {
	(void)o;
	(void)ps;
	(void)n;
	lx_queue_chrome();
}

static int tab_index(node *tabs, GtkWidget *child) {
	for (uint32_t i = 0; i < tabs->children.len; i++)
		if (tabs->children.data[i]->widget == child) return (int)i;
	return -1;
}

static void on_visible_tab(GObject *stack, GParamSpec *ps, node *n) {
	(void)ps;
	if (n->updating) return;
	int i = tab_index(n, adw_view_stack_get_visible_child((AdwViewStack *)stack));
	if (i >= 0 && i != n->selected) {
		n->selected = i;
		lx_emit(n, (ak_event){.kind = AK_EVENT_SELECTED, .i = i});
	}
	lx_queue_chrome();
}

static void on_sheet_closed(AdwDialog *d, node *n) {
	(void)d;
	if (n->nav->popping) return;
	n->nav->gone = true;
	lx_emit(n, (ak_event){.kind = AK_EVENT_DISMISSED});
}

static void on_close_attempt(AdwDialog *d, node *n) {
	(void)d;
	// Escape or a swipe down: the core decides while it intercepts; a
	// sheet that can't be dismissed stays (Escape is on_sheet_key's).
	if (n->intercept_back) lx_emit(n, (ak_event){.kind = AK_EVENT_BACK});
}

#define KEY_ESCAPE 0xff1b

// on_sheet_key: Escape dismisses a sheet that can't be dragged away
// (NoDismiss), as a full-screen one's does, unless it intercepts (then
// close-attempt asks the core).
static gboolean on_sheet_key(GtkEventControllerKey *k, guint key, guint code, GdkModifierType mods, node *n) {
	(void)k, (void)code, (void)mods;
	if (key != KEY_ESCAPE || n->dead || !n->nav || n->nav->gone || n->intercept_back || !n->nav->no_dismiss) return 0;
	if (n->nav->dialog) adw_dialog_force_close(n->nav->dialog);  // closed: Dismissed
	return 1;
}

static gboolean on_window_close(GtkWindow *window, node *n) {
	(void)window;
	win *w = n->win;
	if (!w || w->closing) return 0;
	// The core closes it, or asks the app first (CloseRequest).
	if (listening(n, AK_EVENT_CLOSE_REQUEST)) {
		lx_emit(n, (ak_event){.kind = AK_EVENT_CLOSE_REQUEST});
	} else {
		lx_emit(n, (ak_event){.kind = AK_EVENT_DISMISSED});
		lx_closed_window();
	}
	return 1;
}

bool lx_create_nav(node *n) {
	switch (n->kind) {
	case AK_KIND_STACK: {
		GtkWidget *v = adw_navigation_view_new();
		nav_of(n)->view = v;
		lx_set_inner(n, v);
		lx_connect(n, v, "popped", G_CALLBACK(on_popped));
		lx_connect(n, v, "notify::visible-page", G_CALLBACK(on_visible_page));
		return true;
	}
	case AK_KIND_SCREEN:
	case AK_KIND_ALERT:
	case AK_KIND_SNACKBAR:
		nav_of(n);
		return true;
	case AK_KIND_TABS: {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget *stack = adw_view_stack_new();
		gtk_widget_set_vexpand(stack, 1);
		GtkWidget *bar = adw_view_switcher_bar_new();
		adw_view_switcher_bar_set_stack((AdwViewSwitcherBar *)bar, (AdwViewStack *)stack);
		gtk_box_append((GtkBox *)box, stack);
		gtk_box_append((GtkBox *)box, bar);
		GtkWidget *sw = adw_view_switcher_new();
		g_object_ref_sink(sw);
		adw_view_switcher_set_stack((AdwViewSwitcher *)sw, (AdwViewStack *)stack);
		adw_view_switcher_set_policy((AdwViewSwitcher *)sw, ADW_VIEW_SWITCHER_POLICY_WIDE);
		nav *v = nav_of(n);
		v->view = stack;
		v->bar = bar;
		v->switcher = sw;
		lx_set_inner(n, box);
		lx_connect(n, stack, "notify::visible-child", G_CALLBACK(on_visible_tab));
		return true;
	}
	case AK_KIND_SHEET: {
		AdwDialog *d = adw_dialog_new();
		g_object_ref_sink(d);
		nav_of(n)->dialog = d;
		adw_dialog_set_child(d, n->widget);
		lx_connect(n, d, "closed", G_CALLBACK(on_sheet_closed));
		lx_connect(n, d, "close-attempt", G_CALLBACK(on_close_attempt));
		GtkEventController *keys = gtk_event_controller_key_new();
		gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
		lx_connect(n, keys, "key-pressed", G_CALLBACK(on_sheet_key));
		gtk_widget_add_controller((GtkWidget *)d, keys);
		n->nav->sent_height = -1;
		return true;
	}
	case AK_KIND_WINDOW: {
		win *w = lx_new_win((GtkWindow *)adw_window_new());
		w->node = n;
		n->win = w;
		nav_of(n);
		lx_win_set_root(w, n);
		lx_connect(n, w->window, "close-request", G_CALLBACK(on_window_close));
		return true;
	}
	}
	return false;
}

// ---- A screen's bottom bar ----

typedef struct {
	AdwToolbarView *view;  // a reference of the host's
	GtkWidget *bar;        // its GtkActionBar
	widget_vec packed;
} screen_bar;

typedef struct {
	ak_id screen;
	int64_t index;
} bar_click;

static void free_click(gpointer data, GClosure *c) {
	(void)c;
	free(data);
}

static void on_bar_click(GtkButton *b, gpointer data) {
	(void)b;
	bar_click *k = data;
	node *n = node_get(k->screen);
	if (n) lx_emit(n, (ak_event){.kind = AK_EVENT_ACTION, .i = k->index});
}

// bar_button makes a button of an action bar: its icon (its title as its
// tooltip and what a screen reader says), else its title; one with
// children opens them as a menu. The primary one is a suggested-action
// button with its icon beside its title.
static GtkWidget *bar_button(node *screen, const action *a, bool primary) {
	GtkWidget *b;
	if (action_is(a, AK_MENU_SUBMENU)) {
		b = gtk_menu_button_new();
		if (!str_empty(a->icon) && !primary) {
			gtk_menu_button_set_icon_name((GtkMenuButton *)b, lx_icon_name(a->icon));
			gtk_widget_set_tooltip_text(b, S(a->title));
		} else {
			gtk_menu_button_set_label((GtkMenuButton *)b, S(a->title));
		}
		lx_insert_menu_actions(b, "asukatool", screen->id, &a->items);
		GMenuModel *model = lx_menu_model(&a->items, "asukatool.item");
		gtk_menu_button_set_menu_model((GtkMenuButton *)b, model);
		g_object_unref(model);
	} else {
		b = gtk_button_new();
		if (!str_empty(a->icon) && primary) {
			GtkWidget *c = adw_button_content_new();
			adw_button_content_set_icon_name((AdwButtonContent *)c, lx_icon_name(a->icon));
			adw_button_content_set_label((AdwButtonContent *)c, S(a->title));
			gtk_button_set_child((GtkButton *)b, c);
		} else if (!str_empty(a->icon)) {
			gtk_button_set_icon_name((GtkButton *)b, lx_icon_name(a->icon));
			gtk_widget_set_tooltip_text(b, S(a->title));
		} else {
			gtk_button_set_label((GtkButton *)b, S(a->title));
		}
		bar_click *k = malloc(sizeof *k);
		if (!k) abort();
		k->screen = screen->id;
		k->index = a->index;
		g_signal_connect_data(b, "clicked", G_CALLBACK(on_bar_click), k, free_click, (GConnectFlags)0);
	}
	lx_set_a11y_string(b, A11Y_PROP_LABEL, S(a->title));
	if (primary)
		gtk_widget_add_css_class(b, "suggested-action");
	else if (a->role == AK_ALERT_BUTTON_ROLE_DESTRUCTIVE)
		gtk_widget_add_css_class(b, "destructive-action");
	gtk_widget_set_sensitive(b, !action_is(a, AK_MENU_DISABLED));
	return b;
}

// set_screen_bar shows a screen's Toolbar and Primary in its bottom bar,
// which is made with the first, and hidden (sliding down) while there are
// none.
static void set_screen_bar(node *n) {
	nav *v = n->nav;
	screen_bar *b = v->bottom;
	if (!b) {
		if (!v->toolbar.len && !v->primary.len) return;
		b = calloc(1, sizeof *b);
		if (!b) abort();
		b->view = (AdwToolbarView *)adw_toolbar_view_new();
		g_object_ref_sink(b->view);
		b->bar = gtk_action_bar_new();
		adw_toolbar_view_add_bottom_bar(b->view, b->bar);
		// The view goes in the toolbar view, which takes its place in the page.
		g_object_ref(n->widget);
		if (gtk_widget_get_parent(n->widget)) lx_detach(n->widget);
		adw_toolbar_view_set_content(b->view, n->widget);
		g_object_unref(n->widget);
		v->bottom = b;
		v->page_child = (GtkWidget *)b->view;
		if (v->page && n->parent && n->parent->kind == AK_KIND_STACK) adw_navigation_page_set_child((AdwNavigationPage *)v->page, (GtkWidget *)b->view);
	}
	for (uint32_t i = 0; i < b->packed.len; i++) gtk_action_bar_remove((GtkActionBar *)b->bar, b->packed.data[i]);
	b->packed.len = 0;
	for (uint32_t i = 0; i < v->toolbar.len; i++) {
		GtkWidget *w = bar_button(n, &v->toolbar.data[i], false);
		gtk_action_bar_pack_start((GtkActionBar *)b->bar, w);
		ak_vec_push(&b->packed, w);
	}
	if (v->primary.len) {
		GtkWidget *w = bar_button(n, &v->primary.data[0], true);
		gtk_action_bar_pack_end((GtkActionBar *)b->bar, w);
		ak_vec_push(&b->packed, w);
	}
	gtk_action_bar_set_revealed((GtkActionBar *)b->bar, b->packed.len > 0);
}

static void release_screen_bar(node *n) {
	if (!n->nav || !n->nav->bottom) return;
	screen_bar *b = n->nav->bottom;
	n->nav->bottom = NULL;
	n->nav->page_child = NULL;
	if (n->widget && gtk_widget_get_parent(n->widget) == (GtkWidget *)b->view) adw_toolbar_view_set_content(b->view, NULL);
	g_object_unref(b->view);
	ak_vec_free(&b->packed);
	free(b);
}

// ---- Snackbars ----
//
// An AdwToast in the AdwToastOverlay around the content of the window
// it's on: its title the message, its button the action, its timeout the
// duration. libadwaita shows one toast at a time, slides it in and out,
// gives it a close button and has screen readers announce it (from 1.6;
// before, the host announces it).

static void set_toast_button(AdwToast *t, const char *label) { adw_toast_set_button_label(t, str_empty(label) ? NULL : label); }

// snack_seconds is the toast's timeout: four seconds, ten for Long, none
// for Indefinite; with a screen reader on, one with a button stays until
// it's closed, one without stays long.
static guint snack_seconds(node *n) {
	guint s = 4;
	switch (n->nav->duration) {
	case AK_SNACKBAR_DURATION_INDEFINITE:
		return 0;
	case AK_SNACKBAR_DURATION_LONG:
		s = 10;
		break;
	}
	if (H.screen_reader) {
		if (!str_empty(n->title)) return 0;
		if (s < 10) s = 10;
	}
	return s;
}

static void on_toast_button(AdwToast *t, node *n) {
	(void)t;
	if (n->nav->gone || n->nav->popping) return;
	n->nav->gone = true;
	lx_emit(n, (ak_event){.kind = AK_EVENT_ACTION});  // its one action: 0
}

static void on_toast_dismissed(AdwToast *t, node *n) {
	(void)t;
	if (n->nav->gone || n->nav->popping) return;
	n->nav->gone = true;
	lx_emit(n, (ak_event){.kind = AK_EVENT_DISMISSED});
}

static void present_snack(node *n) {
	win *w = lx_declared_win(n);
	if (n->nav->toast || n->nav->gone || !w || !w->toasts) return;
	AdwToast *t = adw_toast_new(S(n->message));
	g_object_ref(t);  // the overlay takes one; this one is the node's
	n->nav->toast = t;
	if (adw_toast_set_use_markup) adw_toast_set_use_markup(t, 0);  // the message is text
	if (!str_empty(n->title)) set_toast_button(t, n->title);
	adw_toast_set_timeout(t, snack_seconds(n));
	lx_connect(n, t, "button-clicked", G_CALLBACK(on_toast_button));
	// Its time ran out, or the user closed it.
	lx_connect(n, t, "dismissed", G_CALLBACK(on_toast_dismissed));
	adw_toast_overlay_add_toast(w->toasts, t);
	if (!adw_get_minor_version || adw_get_minor_version() < 6) {
		char *text = str_empty(n->title) ? strdup(S(n->message)) : g_strdup_printf("%s, %s", S(n->message), n->title);
		lx_announce(ak_cstr(text));
		if (str_empty(n->title))
			free(text);
		else
			g_free(text);
	}
}

static void destroy_snack(node *n) {
	AdwToast *t = n->nav->toast;
	if (!t) return;
	n->nav->popping = true;
	g_signal_handlers_disconnect_matched(t, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
	if (!n->nav->gone) adw_toast_dismiss(t);
	g_object_unref(t);
	n->nav->toast = NULL;
}

static void set_snack_prop(node *n, ak_prop p, const ak_value *v) {
	AdwToast *t = n->nav->toast;
	switch (p) {
	case AK_PROP_SNACKBAR_MESSAGE:
		str_set(&n->message, v->s);
		if (t) adw_toast_set_title(t, n->message);
		break;
	case AK_PROP_SNACKBAR_ACTION:
		str_set(&n->title, v->s);
		if (t) set_toast_button(t, n->title);
		break;
	case AK_PROP_SNACKBAR_DURATION:
		n->nav->duration = (int32_t)v->i;
		break;
	case AK_PROP_SNACKBAR_WINDOW:
		n->nav->window = (ak_id)v->i;
		break;
	}
}

// ---- Props ----

static void show_tab_badge(node *n);
static void label_tabs(node *tabs);
static void select_tab(node *n);
static void size_sheet(node *n);
static void sheet_corners(node *n);
static void sheet_grabber(node *n);

void lx_set_dialog_can_close(node *n) {
	if (n->kind == AK_KIND_SHEET && n->nav && n->nav->dialog)
		// Escape asks the core (close-attempt) rather than closing it; a
		// sheet that can't be dismissed isn't swiped away, and its Escape
		// is its key controller's (on_sheet_key).
		adw_dialog_set_can_close(n->nav->dialog, !n->intercept_back && !n->nav->no_dismiss);
}

static bool in_tabs(node *n) { return n->nav->page && n->parent && n->parent->kind == AK_KIND_TABS; }

bool lx_set_nav_prop(node *n, ak_prop p, const ak_value *v) {
	switch (n->kind) {
	case AK_KIND_SCREEN: {
		nav *nv = n->nav;
		switch (p) {
		case AK_PROP_SCREEN_TITLE:
			str_set(&n->title, v->s);
			if (nv->page) {
				if (in_tabs(n)) {
					adw_view_stack_page_set_title((AdwViewStackPage *)nv->page, n->title);
					label_tabs(n->parent);
				} else {
					adw_navigation_page_set_title((AdwNavigationPage *)nv->page, n->title);
				}
			}
			break;
		case AK_PROP_SCREEN_ACTIONS:
			actions_decode(&n->actions, v->s, 0);
			break;
		case AK_PROP_SCREEN_TOOLBAR:
			actions_decode(&nv->toolbar, v->s, AK_SCREEN_TOOLBAR_ACTIONS);
			set_screen_bar(n);
			break;
		case AK_PROP_SCREEN_PRIMARY:
			actions_decode(&nv->primary, v->s, AK_SCREEN_PRIMARY_ACTION);
			set_screen_bar(n);
			break;
		case AK_PROP_SCREEN_ICON:
			str_set(&n->icon, v->s);
			if (in_tabs(n)) adw_view_stack_page_set_icon_name((AdwViewStackPage *)nv->page, lx_icon_name(n->icon));
			break;
		case AK_PROP_SCREEN_BADGE:
			str_set(&nv->badge, v->s);
			if (in_tabs(n)) {
				show_tab_badge(n);
				label_tabs(n->parent);
			}
			break;
		case AK_PROP_SCREEN_SEARCH:
			str_set(&nv->search_ph, v->s);
			break;
		case AK_PROP_SCREEN_SEARCH_TEXT:
			str_set(&nv->search_tx, v->s);
			if (nv->search) lx_search_set_text(nv->search, nv->search_tx);
			break;
		case AK_PROP_SCREEN_BACK_GESTURE:
			if (nv->page && n->parent && n->parent->kind == AK_KIND_STACK)
				adw_navigation_page_set_can_pop((AdwNavigationPage *)nv->page, v->i != 0 && !n->intercept_back);
			break;
		}
		lx_queue_chrome();
		return true;
	}
	case AK_KIND_TABS:
		if (p == AK_PROP_TABS_SELECTED) {
			n->selected = (int)v->i;
			select_tab(n);
		}
		return true;
	case AK_KIND_SHEET:
		switch (p) {
		case AK_PROP_SHEET_DETENTS:
			n->nav->detents.len = 0;
			ak_buf_put(&n->nav->detents, v->s.p, v->s.len);
			size_sheet(n);
			break;
		case AK_PROP_SHEET_DETENT:
			n->nav->detent = (int32_t)v->i;
			size_sheet(n);  // moved there
			break;
		case AK_PROP_SHEET_UNDIMMED:
			n->nav->undimmed = (int32_t)v->i;
			size_sheet(n);
			break;
		case AK_PROP_SHEET_CORNER_RADIUS:
			n->nav->corner = (float)v->f;
			sheet_corners(n);
			break;
		case AK_PROP_SHEET_GRABBER:
			n->nav->grabber = v->i != 0;
			if (n->nav->panel) sheet_grabber(n);
			break;
		case AK_PROP_SHEET_NO_DISMISS:
			n->nav->no_dismiss = v->i != 0;
			lx_set_dialog_can_close(n);
			break;
		case AK_PROP_SHEET_WINDOW:
			n->nav->window = (ak_id)v->i;
			break;
		case AK_PROP_SHEET_FULL_SCREEN:
			n->nav->full = v->i != 0;
			// Floating, so a narrow window doesn't make it a bottom sheet,
			// which shows the window above it.
			if (n->nav->full) adw_dialog_set_presentation_mode(n->nav->dialog, ADW_DIALOG_FLOATING);
			size_sheet(n);
			break;
		}
		return true;
	case AK_KIND_ALERT:
		switch (p) {
		case AK_PROP_ALERT_TITLE:
			str_set(&n->title, v->s);
			break;
		case AK_PROP_ALERT_MESSAGE:
			str_set(&n->message, v->s);
			break;
		case AK_PROP_ALERT_BUTTONS:
			actions_decode(&n->actions, v->s, 0);
			if (n->nav->dialog && !n->nav->gone) {
				// Showing: its buttons follow their titles and Disabled.
				for (uint32_t i = 0; i < n->actions.len; i++) {
					char id[16];
					snprintf(id, sizeof id, "%u", i);
					adw_alert_dialog_set_response_label((AdwAlertDialog *)n->nav->dialog, id, S(n->actions.data[i].title));
					adw_alert_dialog_set_response_enabled((AdwAlertDialog *)n->nav->dialog, id, !action_is(&n->actions.data[i], AK_MENU_DISABLED));
				}
			}
			break;
		case AK_PROP_ALERT_STYLE:
			n->alert_style = (int32_t)v->i;
			break;
		case AK_PROP_ALERT_WINDOW:
			n->nav->window = (ak_id)v->i;
			break;
		case AK_PROP_ALERT_FIELDS:
			ak_buf_reset(&n->nav->fields_buf);
			ak_buf_put(&n->nav->fields_buf, v->s.p, v->s.len);
			n->nav->fields.len = 0;
			ak_decode_alert_fields(n->nav->fields_buf.data, n->nav->fields_buf.len, &n->nav->fields);
			break;
		}
		return true;
	case AK_KIND_SNACKBAR:
		set_snack_prop(n, p, v);
		return true;
	case AK_KIND_WINDOW: {
		win *w = n->win;
		switch (p) {
		case AK_PROP_WINDOW_TITLE:
			str_set(&w->name, v->s);
			lx_update_title(w);
			break;
		case AK_PROP_WINDOW_CONFIG: {
			ak_window_config c = ak_decode_window_config((const uint8_t *)v->s.p, v->s.len);
			lx_configure(w, &c);
			break;
		}
		case AK_PROP_WINDOW_FLYOUT:
			lx_set_flyout(w, v->s);
			break;
		case AK_PROP_WINDOW_EDITED:
			w->edited = v->i != 0;
			lx_update_title(w);
			break;
		case AK_PROP_WINDOW_FILE:
			str_set(&w->file, v->s);
			lx_update_title(w);
			break;
		}
		return true;
	}
	}
	return false;
}

// ---- Stacks ----

static void replace_pages(node *p) {
	AdwNavigationPage *stack[64];
	AdwNavigationPage **pages = p->children.len <= 64 ? stack : malloc(p->children.len * sizeof *pages);
	int n = 0;
	for (uint32_t i = 0; i < p->children.len; i++) {
		node *c = p->children.data[i];
		if (c->nav && c->nav->page && !c->nav->gone) pages[n++] = c->nav->page;
	}
	p->nav->popping = true;
	adw_navigation_view_replace((AdwNavigationView *)p->nav->view, n ? pages : NULL, n);
	p->nav->popping = false;
	p->nav->pending = 0;
	if (pages != stack) free(pages);
}

// drop_page takes a screen's page out of its stack: popped (animated)
// when it's the top, else at once.
static void drop_page(node *p, node *c) {
	if (!c->nav || !c->nav->page) return;
	gpointer page = c->nav->page;
	c->nav->page = NULL;
	if (c->nav->gone) return;  // the user popped it
	if (adw_navigation_view_get_visible_page((AdwNavigationView *)p->nav->view) == page) {
		p->nav->popping = true;
		adw_navigation_view_pop((AdwNavigationView *)p->nav->view);
		p->nav->popping = false;
	} else {
		replace_pages(p);
	}
	lx_queue_chrome();
}

// page_popped: the user went back (the back button, a swipe, Escape), or
// the host popped.
static void page_popped(node *stack, gpointer page) {
	lx_queue_chrome();
	if (stack->nav->popping) return;
	for (uint32_t i = 0; i < stack->children.len; i++) {
		node *c = stack->children.data[i];
		if (c->nav && c->nav->page == page) {
			c->nav->gone = true;
			lx_emit(stack, (ak_event){.kind = AK_EVENT_POPPED, .i = c->id});
			return;
		}
	}
}

// ---- Tabs ----

// show_tab_badge shows a tab's badge on its page, which the view switchers
// show: a count as its badge-number, other text as needs-attention's dot.
static void show_tab_badge(node *n) {
	int32_t num = 0;
	if (!ak_badge_number(ak_cstr(S(n->nav->badge)), &num) || num < 0) num = 0;
	adw_view_stack_page_set_badge_number((AdwViewStackPage *)n->nav->page, (guint)num);
	adw_view_stack_page_set_needs_attention((AdwViewStackPage *)n->nav->page, !str_empty(n->nav->badge));
}

static void collect_toggles(GtkWidget *w, widget_vec *out) {
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
		if (AK_IS(c, gtk_toggle_button_get_type)) {
			ak_vec_push(out, c);
			continue;
		}
		collect_toggles(c, out);
	}
}

// label_tabs labels the view switchers' buttons, a tab's each, for a
// screen reader: its title and its badge, a count as so many items
// ("Inbox, 3 items").
static void label_tabs(node *tabs) {
	if (!tabs->nav) return;
	GtkWidget *sws[2] = {tabs->nav->switcher, tabs->nav->bar};
	for (int k = 0; k < 2; k++) {
		if (!sws[k]) continue;
		widget_vec buttons = {0};
		collect_toggles(sws[k], &buttons);
		node_vec pages = {0};
		for (uint32_t i = 0; i < tabs->children.len; i++)
			if (tabs->children.data[i]->nav && tabs->children.data[i]->nav->page) ak_vec_push(&pages, tabs->children.data[i]);
		if (buttons.len == pages.len)  // else the switcher makes its buttons as it's shown
			for (uint32_t i = 0; i < buttons.len; i++)
				lx_set_a11y_string(buttons.data[i], A11Y_PROP_LABEL, lx_badged(pages.data[i]->title, pages.data[i]->nav->badge));
		ak_vec_free(&buttons);
		ak_vec_free(&pages);
	}
}

static void select_tab(node *n) {
	if (n->selected < 0 || (uint32_t)n->selected >= n->children.len) return;
	node *c = n->children.data[n->selected];
	n->updating = true;
	adw_view_stack_set_visible_child((AdwViewStack *)n->nav->view, c->widget);
	n->updating = false;
	lx_queue_chrome();
}

// lx_tabs_resized puts the switcher in the header (wide) or at the bottom
// (narrow, under 550 points: libadwaita's breakpoint for it).
void lx_tabs_resized(node *n) {
	bool narrow = n->frame[2] < 550;
	if (narrow != n->nav->narrow) {
		n->nav->narrow = narrow;
		lx_queue_chrome();
	}
	adw_view_switcher_bar_set_reveal((AdwViewSwitcherBar *)n->nav->bar, narrow);
}

static void label_tabs_of(node *n, void *ctx) {
	(void)ctx;
	if (n->kind == AK_KIND_TABS) label_tabs(n);
}

void lx_label_tabs_all(void);
void lx_label_tabs_all(void) { lx_each_node(label_tabs_of, NULL); }

GtkWidget *lx_tabs_switcher(node *tabs) { return tabs->nav ? tabs->nav->switcher : NULL; }
bool lx_tabs_narrow(node *tabs) { return tabs->nav && tabs->nav->narrow; }

// ---- The tree ----

bool lx_insert_nav(node *p, node *c, uint32_t index) {
	switch (p->kind) {
	case AK_KIND_SPLIT:
		lx_insert_split(p, c, index);
		return true;
	case AK_KIND_STACK: {
		lx_add_child(p, c, index);
		if (!c->nav) nav_of(c);
		AdwNavigationPage *page = adw_navigation_page_new(lx_page_child(c), S(c->title));
		c->nav->page = page;
		char tag[16];
		snprintf(tag, sizeof tag, "%u", c->id);
		adw_navigation_page_set_tag(page, tag);
		if (c->intercept_back) adw_navigation_page_set_can_pop(page, 0);
		if (!adw_navigation_view_get_visible_page((AdwNavigationView *)p->nav->view) || p->nav->pending == -2)
			replace_pages(p);  // the first screen, or a Replace: at once
		else
			adw_navigation_view_push((AdwNavigationView *)p->nav->view, page);
		lx_queue_chrome();
		return true;
	}
	case AK_KIND_TABS: {
		lx_add_child(p, c, index);
		if (!c->nav) nav_of(c);
		char name[16];
		snprintf(name, sizeof name, "%u", c->id);
		c->nav->page = adw_view_stack_add_titled_with_icon((AdwViewStack *)p->nav->view, c->widget, name, S(c->title), lx_icon_name(S(c->icon)));
		if (!str_empty(c->nav->badge)) show_tab_badge(c);
		label_tabs(p);
		select_tab(p);
		lx_queue_chrome();
		return true;
	}
	}
	return false;
}

bool lx_remove_nav(node *p, node *c) {
	switch (p->kind) {
	case AK_KIND_SPLIT:
		lx_remove_split(p, c);
		return true;
	case AK_KIND_STACK:
		drop_page(p, c);
		return true;
	case AK_KIND_TABS:
		if (c->nav && c->nav->page) {
			adw_view_stack_remove((AdwViewStack *)p->nav->view, c->widget);
			c->nav->page = NULL;
			label_tabs(p);
		}
		lx_queue_chrome();
		return true;
	}
	return false;
}

void lx_save_window(win *w);
static void sheet_forget(node *n);

// lx_destroy_nav handles the destruction of a navigation node: a sheet or
// alert dismissed, a window closed.
bool lx_destroy_nav(node *n) {
	lx_forget_chrome(n);
	switch (n->kind) {
	case AK_KIND_SHEET:
		sheet_forget(n);
		if (n->nav->dialog) {
			AdwDialog *d = n->nav->dialog;
			n->nav->popping = true;
			g_signal_handlers_disconnect_matched(d, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
			if (!n->nav->gone) adw_dialog_force_close(d);
			adw_dialog_set_child(d, NULL);
			g_object_unref(d);
			n->nav->dialog = NULL;
		}
		break;
	case AK_KIND_ALERT:
		if (n->nav->dialog) {
			n->nav->popping = true;
			g_signal_handlers_disconnect_matched(n->nav->dialog, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
			if (!n->nav->gone) adw_dialog_force_close(n->nav->dialog);
			n->nav->dialog = NULL;
		}
		break;
	case AK_KIND_SNACKBAR:
		destroy_snack(n);
		break;
	case AK_KIND_WINDOW: {
		win *w = n->win;
		if (w) {
			lx_save_window(w);
			lx_drop_window_css(w);
			w->closing = true;
			lx_drop_desk(w);
			g_signal_handlers_disconnect_matched(w->window, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
			if (w->root && gtk_widget_get_parent(w->root->widget) == w->content) gtk_widget_unparent(w->root->widget);
			lx_free_win(w);
			gtk_window_destroy(w->window);
			n->win = NULL;
		}
		break;
	}
	case AK_KIND_TABS:
		if (n->nav->switcher) {
			g_object_unref(n->nav->switcher);
			n->nav->switcher = NULL;
		}
		break;
	case AK_KIND_SCREEN:
		lx_forget_search(n);
		break;
	}
	if (n->kind == AK_KIND_SCREEN && n->parent && n->parent->kind == AK_KIND_STACK) {
		node *p = n->parent;
		drop_page(p, n);
		for (uint32_t i = 0; i < p->children.len; i++)
			if (p->children.data[i] == n) {
				memmove(p->children.data + i, p->children.data + i + 1, (p->children.len - i - 1) * sizeof(node *));
				p->children.len--;
				break;
			}
		n->parent = NULL;
		for (uint32_t i = 0; i < n->children.len; i++) n->children.data[i]->parent = NULL;
		release_screen_bar(n);
		lx_release(n);
		return true;
	}
	if (n->kind == AK_KIND_SCREEN) release_screen_bar(n);
	return false;
}

// ---- Presenting ----

void lx_present_popover(node *n);
static void present_alert(node *n, GtkWidget *parent);
static void present_sheet(node *n);

// lx_present shows a sheet or alert over the window it's declared in (its
// Window prop), or opens a Window or a popover.
void lx_present(node *n) {
	switch (n->kind) {
	case AK_KIND_SHEET:
		present_sheet(n);
		lx_dev_error_raise();  // asuka dev's error overlay stays on top
		break;
	case AK_KIND_ALERT:
		present_alert(n, lx_declared_win(n)->content);
		lx_dev_error_raise();
		break;
	case AK_KIND_SNACKBAR:
		present_snack(n);
		break;
	case AK_KIND_WINDOW:
		lx_open_window(n);
		break;
	case AK_KIND_POPOVER:
		lx_present_popover(n);
		break;
	}
}

typedef struct {
	win *w;
} covers_ctx;

static void size_cover(node *n, void *ctx) {
	covers_ctx *c = ctx;
	if (n->kind == AK_KIND_SHEET && n->nav && (n->nav->dialog || n->nav->panel) && lx_declared_win(n) == c->w) size_sheet(n);
}

// lx_size_covers sizes the sheets on w again (it resized): a full-screen
// one's its size, the others' detents its share.
void lx_size_covers(win *w) {
	covers_ctx c = {w};
	lx_each_node(size_cover, &c);
}

// lx_declared_win is the window a sheet or an alert is declared in (its
// Window prop), the main window if it has none, or that one closed.
win *lx_declared_win(node *n) {
	if (n->nav) {
		node *x = node_get(n->nav->window);
		if (x) {
			if (x->win && !x->win->closing) return x->win;
			if (H.main && x == H.main->root) return H.main;
		}
	}
	return H.main;
}

// ---- Sheets: detents ----
//
// A sheet rests at its detent's height: Detent's, else its shortest.
// Large is most of its window's height (nine tenths), medium half of it
// (at least 200 points), height its points, fraction its share of large,
// fit its content's (the core's Fit), each at most large. A modal sheet
// is an AdwDialog of that content height, which libadwaita floats, or
// shows as a bottom sheet in a narrow window; it isn't dragged between
// detents (libadwaita's dialogs aren't), Detent moves it. An undimmed one
// (Undimmed) is the host's own panel instead, at the bottom of its
// window's root view over the app (libadwaita's AdwBottomSheet would need
// to hold the window's content, and has no detents): the window behind
// takes clicks and keys while it's at the undimmed detent or lower; above
// that a scrim dims it and takes them. The panel is dragged between its
// detents (Selected), down past its shortest to dismiss it, and slides
// to a detent set.

typedef struct sheet_panel {
	GtkWidget *box;     // the panel: its grabber over the sheet's view
	GtkWidget *handle;  // its grabber, shown with Grabber
	GtkWidget *scrim;   // over the window, under the panel: dims and takes presses above the undimmed detent
	GtkWidget *root;    // the root view they're in (a reference)
	float h;            // its height now
	float from, to;     // a slide's
	gint64 start;
	guint tick;
	float drag_h;  // its height as a drag began
	double press_x, press_y, press_root;  // where it began: in the panel, and in the root
	bool pressed, dragging;
} sheet_panel;

static uint32_t detent_count(const nav *v) {
	uint32_t c = 0;
	if (v->detents.len >= 4) memcpy(&c, v->detents.data, 4);
	return v->detents.len == 4 + (size_t)5 * c ? c : 0;
}

static uint8_t detent_kind(const nav *v, uint32_t i, float *value) {
	const uint8_t *p = v->detents.data + 4 + (size_t)5 * i;
	memcpy(value, p + 1, 4);
	return p[0];
}

static bool sheet_fit_only(node *n) {
	nav *v = n->nav;
	float x;
	return v && !v->full && detent_count(v) == 1 && detent_kind(v, 0, &x) == AK_DETENT_KIND_FIT;
}

// sheet_window is the size of the window a sheet is on (its content's).
static void sheet_window(node *n, float *ww, float *wh) {
	win *w = lx_declared_win(n);
	*ww = w ? w->env_w : 0, *wh = w ? w->env_h : 0;
	if (*ww <= 0 || *wh <= 0) *ww = 800, *wh = 600;  // not shown yet
}

static float sheet_large(node *n) {
	float ww, wh;
	sheet_window(n, &ww, &wh);
	return fmaxf(wh * 0.9f, fminf(200, wh));
}

static float sheet_width(node *n) {
	float ww, wh;
	sheet_window(n, &ww, &wh);
	return ww < 600 ? ww : 600;  // as wide as libadwaita's dialogs, at most
}

// sheet_room is what a sheet as tall as its content may take: its width
// and large.
static void sheet_room(node *n, float *w, float *h) {
	*w = sheet_width(n);
	*h = sheet_large(n);
}

static float detent_height(node *n, uint32_t i) {
	nav *v = n->nav;
	float ww, wh, x, large = sheet_large(n), h = large;
	sheet_window(n, &ww, &wh);
	switch (detent_kind(v, i, &x)) {
	case AK_DETENT_KIND_MEDIUM: h = fmaxf(wh * 0.5f, fminf(200, wh)); break;
	case AK_DETENT_KIND_FIT: h = v->fit_h > 0 ? v->fit_h : large; break;
	case AK_DETENT_KIND_HEIGHT: h = x; break;
	case AK_DETENT_KIND_FRACTION: h = large * x; break;
	}
	return h > large ? large : h < 1 ? 1 : h;
}

// sheet_at is the index of the detent a sheet is at.
static uint32_t sheet_at(node *n) {
	nav *v = n->nav;
	uint32_t c = detent_count(v);
	if (v->detent >= 1 && (uint32_t)v->detent <= c) return (uint32_t)v->detent - 1;
	uint32_t at = 0;
	for (uint32_t i = 1; i < c; i++)
		if (detent_height(n, i) < detent_height(n, at)) at = i;
	return at;
}

static float sheet_rest(node *n) { return detent_count(n->nav) ? detent_height(n, sheet_at(n)) : sheet_large(n); }

// undimmed_height is the height of its tallest undimmed detent (< 0:
// none).
static float undimmed_height(node *n) {
	nav *v = n->nav;
	return v->undimmed >= 1 && (uint32_t)v->undimmed <= detent_count(v) ? detent_height(n, (uint32_t)v->undimmed - 1) : -1;
}

// ---- Its height, as it moves (SheetHeight) ----

// shown_height is how far a sheet's top is from its window's bottom.
static float shown_height(node *n) {
	nav *v = n->nav;
	if (v->panel) return v->panel->h;
	win *w = lx_declared_win(n);
	if (!w || !gtk_widget_get_mapped(n->widget)) return 0;
	graphene_point_t p = {0, 0}, out;
	if (!gtk_widget_compute_point(n->widget, w->content, &p, &out)) return 0;
	float h = (float)gtk_widget_get_height(w->content) - out.y;
	return h > 0 ? h : 0;
}

static void send_height(node *n) {
	nav *v = n->nav;
	float h = roundf(shown_height(n) * 2) / 2;
	if (h == v->sent_height) {
		v->height_still++;
		return;
	}
	v->height_still = 0;
	v->sent_height = h;
	lx_emit(n, (ak_event){.kind = AK_EVENT_SHEET_HEIGHT, .f = h});
}

static gboolean height_tick(GtkWidget *w, GdkFrameClock *c, gpointer data) {
	(void)w, (void)c;
	node *n = node_get((ak_id)(uintptr_t)data);
	if (!n || n->dead || !n->nav || n->nav->gone) return 0;
	send_height(n);
	// Still for half a second (libadwaita's animations are shorter): it
	// settled; a resize or a move follows it again.
	if (n->nav->height_still < 30) return 1;
	n->nav->height_tick = 0;
	return 0;
}

// follow_height follows a sheet's height frame by frame while the core
// listens, until it settles.
static void follow_height(node *n) {
	nav *v = n->nav;
	if (!v || !listening(n, AK_EVENT_SHEET_HEIGHT) || v->gone) return;
	v->height_still = 0;
	if (!v->height_tick) v->height_tick = gtk_widget_add_tick_callback(n->widget, height_tick, (gpointer)(uintptr_t)n->id, NULL);
}

// ---- Its corners ----

// sheet_corners rounds a sheet's corners to CornerRadius: a panel's top
// ones, a dialog's sheet's (libadwaita's sheet node, in the dialog around
// its child: all four floating, the top ones as a bottom sheet).
static void sheet_corners(node *n) {
	nav *v = n->nav;
	char cls[32];
	snprintf(cls, sizeof cls, "asuka-sheet-%u", n->id);
	GtkWidget *target = v->panel ? v->panel->box : NULL;
	if (!target && v->dialog) {
		for (GtkWidget *x = gtk_widget_get_parent(n->widget); x && x != (GtkWidget *)v->dialog; x = gtk_widget_get_parent(x)) {
			const char *name = gtk_widget_get_css_name ? gtk_widget_get_css_name(x) : NULL;
			if (name && strcmp(name, "sheet") == 0) {
				target = x;
				break;
			}
		}
	}
	if (v->corner_sheet && v->corner_sheet != target) {
		gtk_widget_remove_css_class(v->corner_sheet, cls);
		g_object_unref(v->corner_sheet);
		v->corner_sheet = NULL;
	}
	if (v->corner <= 0 || !target) return;
	if (!v->corner_css) {
		v->corner_css = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gtk_widget_get_display(n->widget), (GtkStyleProvider *)v->corner_css, 800);
	}
	char css[256];
	float r = v->corner;
	snprintf(css, sizeof css,
	         ".%s { border-radius: %gpx; }\n"
	         ".%s.asuka-sheet-panel, bottom-sheet .%s { border-radius: %gpx %gpx 0 0; }\n",
	         cls, r, cls, cls, r, r);
	gtk_css_provider_load_from_string(v->corner_css, css);
	if (v->corner_sheet != target) {
		v->corner_sheet = g_object_ref(target);
		gtk_widget_add_css_class(target, cls);
	}
}

// ---- The undimmed panel ----

static node *panel_node(gpointer data) {
	node *n = node_get((ak_id)(uintptr_t)data);
	return n && !n->dead && n->nav && n->nav->panel ? n : NULL;
}

node *lx_sheet_panel_of(GtkWidget *w) {
	gpointer id = g_object_get_data((GObject *)w, "asuka-sheet");
	return id ? panel_node(id) : NULL;
}

// scrim_dim is how dim the scrim is at the panel's height: none at the
// undimmed detent or lower, all of it at the next one up.
static float scrim_dim(node *n) {
	float und = undimmed_height(n), h = n->nav->panel->h, next = sheet_large(n);
	if (und < 0) return 1;
	if (h <= und) return 0;
	uint32_t c = detent_count(n->nav);
	for (uint32_t i = 0; i < c; i++) {
		float d = detent_height(n, i);
		if (d > und + 0.5f && d < next) next = d;
	}
	float t = next > und ? (h - und) / (next - und) : 1;
	return t < 0 ? 0 : t > 1 ? 1 : t;
}

void lx_allocate_sheet_panel(node *n, GtkWidget *w, int width, int height) {
	sheet_panel *p = n->nav->panel;
	if (w == p->scrim) {
		float dim = scrim_dim(n);
		gtk_widget_set_opacity(w, dim);
		gtk_widget_set_can_target(w, dim > 0.01f);
		lx_allocate(w, width, height, 0, 0, NULL);
		return;
	}
	float pw = width < 600 ? (float)width : 600, h = p->h < height ? p->h : (float)height;
	lx_allocate(w, (int)pw, (int)ceilf(h), ((float)width - pw) / 2, (float)height - h, NULL);
}

static void panel_moved(node *n) {
	sheet_panel *p = n->nav->panel;
	gtk_widget_queue_allocate(p->root);
	follow_height(n);
}

static gboolean slide_tick(GtkWidget *w, GdkFrameClock *c, gpointer data) {
	(void)w;
	node *n = panel_node(data);
	if (!n) return 0;
	sheet_panel *p = n->nav->panel;
	gint64 now = gdk_frame_clock_get_frame_time(c);
	if (!p->start) p->start = now;
	float t = (float)(now - p->start) / 250000.0f;  // a quarter of a second
	if (t >= 1 || H.env.reduce_motion) t = 1;
	float e = 1 - (1 - t) * (1 - t) * (1 - t);  // ease out
	p->h = p->from + (p->to - p->from) * e;
	panel_moved(n);
	if (t < 1) return 1;
	p->tick = 0;
	return 0;
}

// slide_to slides the panel to height h.
static void slide_to(node *n, float h) {
	sheet_panel *p = n->nav->panel;
	if (p->dragging) return;
	if (p->tick && p->to == h) return;
	p->from = p->h, p->to = h, p->start = 0;
	if (p->h == h) {
		panel_moved(n);
		return;
	}
	if (!p->tick) p->tick = gtk_widget_add_tick_callback(p->box, slide_tick, (gpointer)(uintptr_t)n->id, NULL);
}

// panel_dismiss is a panel dismissed by the user: the core destroys it,
// or, while it intercepts, decides.
static void panel_dismiss(node *n) {
	if (n->intercept_back) {
		lx_emit(n, (ak_event){.kind = AK_EVENT_BACK});
		slide_to(n, sheet_rest(n));
		return;
	}
	n->nav->gone = true;
	lx_emit(n, (ak_event){.kind = AK_EVENT_DISMISSED});
}

// The panel's drag watches every press in it (capture), and takes it
// (claiming it from a button under it) once it has moved vertically: a
// sheet is dragged from anywhere on it, as a phone's is. The pointer's
// travel is measured in the root's coordinates, the panel moving under
// it.

// root_y is where a point of the panel, as the gesture has it, is in
// the root.
static double root_y(sheet_panel *p, double x, double y) {
	graphene_point_t a = {(float)x, (float)y}, out;
	return gtk_widget_compute_point(p->box, p->root, &a, &out) ? out.y : y;
}

static void on_panel_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	(void)g;
	node *n = panel_node(data);
	if (!n) return;
	sheet_panel *p = n->nav->panel;
	p->pressed = true;
	p->press_x = x, p->press_y = y;
	p->press_root = root_y(p, x, y);
}

static void on_panel_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	node *n = panel_node(data);
	if (!n || !n->nav->panel->pressed) return;
	sheet_panel *p = n->nav->panel;
	double travel = root_y(p, p->press_x + dx, p->press_y + dy) - p->press_root;
	if (!p->dragging) {
		if (fabs(travel) < 8 || fabs(travel) < fabs(dx)) return;  // not yet a vertical drag
		gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
		if (p->tick) {
			gtk_widget_remove_tick_callback(p->box, p->tick);
			p->tick = 0;
		}
		p->dragging = true;
		p->drag_h = p->h;
	}
	float h = p->drag_h - (float)travel, large = sheet_large(n);
	if (h > large) h = large + (h - large) / 4;  // past large: it resists
	p->h = h > 0 ? h : 0;
	panel_moved(n);
}

static void on_panel_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	(void)g, (void)dx, (void)dy;
	node *n = panel_node(data);
	if (!n) return;
	nav *v = n->nav;
	sheet_panel *p = v->panel;
	p->pressed = false;
	if (!p->dragging) return;  // a press, not a drag: its view's
	p->dragging = false;
	uint32_t c = detent_count(v);
	float shortest = c ? detent_height(n, 0) : sheet_large(n);
	for (uint32_t i = 1; i < c; i++) shortest = fminf(shortest, detent_height(n, i));
	if (!v->no_dismiss && p->h < shortest * 0.5f) {
		panel_dismiss(n);
		return;
	}
	// The nearest detent; its index is the user's choice.
	uint32_t at = sheet_at(n);
	float best = -1;
	for (uint32_t i = 0; i < c; i++) {
		float d = fabsf(detent_height(n, i) - p->h);
		if (best < 0 || d < best) best = d, at = i;
	}
	if (c && at != sheet_at(n)) {
		v->detent = (int32_t)at + 1;
		lx_emit(n, (ak_event){.kind = AK_EVENT_SELECTED, .i = at});
	}
	slide_to(n, sheet_rest(n));
}

// on_panel_drag_cancel: another gesture took the press (a scroll in the
// sheet); a drag under way settles where it was.
static void on_panel_drag_cancel(GtkGesture *g, gpointer s, gpointer data) {
	(void)g, (void)s;
	node *n = panel_node(data);
	if (!n) return;
	sheet_panel *p = n->nav->panel;
	bool was = p->dragging;
	p->pressed = p->dragging = false;
	if (was) slide_to(n, sheet_rest(n));
}

static void on_scrim_press(GtkGestureClick *g, int count, double x, double y, gpointer data) {
	(void)count, (void)x, (void)y;
	node *n = panel_node(data);
	if (!n) return;
	gtk_gesture_set_state((GtkGesture *)g, GTK_EVENT_SEQUENCE_CLAIMED);
	if (!n->nav->no_dismiss || n->intercept_back) panel_dismiss(n);
}

static gboolean on_panel_key(GtkEventControllerKey *k, guint key, guint code, GdkModifierType mods, gpointer data) {
	(void)k, (void)code, (void)mods;
	node *n = panel_node(data);
	if (!n || key != KEY_ESCAPE) return 0;
	panel_dismiss(n);  // Escape dismisses it, as a dialog's does
	return 1;
}

static void sheet_grabber(node *n) {
	sheet_panel *p = n->nav->panel;
	if (p) gtk_widget_set_visible(p->handle, n->nav->grabber);
}

// present_panel shows an undimmed sheet as a panel over its window's
// root view, sliding up to its detent.
static void present_panel(node *n) {
	nav *v = n->nav;
	win *w = lx_declared_win(n);
	GtkWidget *root = w && w->root ? w->root->widget : NULL;
	if (!root) return;
	sheet_panel *p = calloc(1, sizeof *p);
	if (!p) abort();
	v->panel = p;
	gpointer id = (gpointer)(uintptr_t)n->id;
	p->root = g_object_ref(root);
	p->scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(p->scrim, "asuka-sheet-scrim");
	g_object_set_data((GObject *)p->scrim, "asuka-sheet", id);
	gtk_widget_set_opacity(p->scrim, 0);
	gtk_widget_set_can_target(p->scrim, 0);
	GtkGesture *press = gtk_gesture_click_new();
	gtk_gesture_single_set_button((GtkGestureSingle *)press, 0);
	g_signal_connect(press, "pressed", G_CALLBACK(on_scrim_press), id);
	gtk_widget_add_controller(p->scrim, (GtkEventController *)press);
	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(p->box, "asuka-sheet-panel");
	gtk_widget_set_overflow(p->box, GTK_OVERFLOW_HIDDEN);
	g_object_set_data((GObject *)p->box, "asuka-sheet", id);
	p->handle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(p->handle, "asuka-sheet-handle");
	gtk_widget_set_halign(p->handle, GTK_ALIGN_CENTER);
	gtk_box_append((GtkBox *)p->box, p->handle);
	sheet_grabber(n);
	adw_dialog_set_child(v->dialog, NULL);  // its view goes in the panel
	gtk_widget_set_vexpand(n->widget, 1);
	gtk_box_append((GtkBox *)p->box, n->widget);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_panel_drag_begin), id);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_panel_drag_update), id);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_panel_drag_end), id);
	g_signal_connect(drag, "cancel", G_CALLBACK(on_panel_drag_cancel), id);
	gtk_event_controller_set_propagation_phase((GtkEventController *)drag, GTK_PHASE_CAPTURE);
	gtk_widget_add_controller(p->box, (GtkEventController *)drag);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_panel_key), id);
	gtk_widget_add_controller(p->box, keys);
	gtk_widget_insert_before(p->scrim, root, NULL);  // over the app, under its overlays as they come
	gtk_widget_insert_before(p->box, root, NULL);
	sheet_corners(n);
	slide_to(n, sheet_rest(n));
}

static void drop_panel(node *n) {
	sheet_panel *p = n->nav->panel;
	if (!p) return;
	n->nav->panel = NULL;
	if (p->tick) gtk_widget_remove_tick_callback(p->box, p->tick);
	if (gtk_widget_get_parent(n->widget) == p->box) gtk_box_remove((GtkBox *)p->box, n->widget);
	gtk_widget_unparent(p->box);
	gtk_widget_unparent(p->scrim);
	gtk_widget_queue_allocate(p->root);
	g_object_unref(p->root);
	free(p);
}

// sheet_forget lets go of what a sheet holds as it goes.
static void sheet_forget(node *n) {
	nav *v = n->nav;
	if (v->height_tick) {
		gtk_widget_remove_tick_callback(n->widget, v->height_tick);
		v->height_tick = 0;
	}
	if (v->corner_sheet) {
		char cls[32];
		snprintf(cls, sizeof cls, "asuka-sheet-%u", n->id);
		gtk_widget_remove_css_class(v->corner_sheet, cls);
		g_object_unref(v->corner_sheet);
		v->corner_sheet = NULL;
	}
	if (v->corner_css) {
		gtk_style_context_remove_provider_for_display(gtk_widget_get_display(n->widget), (GtkStyleProvider *)v->corner_css);
		g_object_unref(v->corner_css);
		v->corner_css = NULL;
	}
	drop_panel(n);
}

// ---- Sizing and presenting ----

// size_sheet sizes a sheet at its detent: a dialog's content height, a
// panel's slide; a full-screen one, as big as its whole window.
static void size_sheet(node *n) {
	nav *v = n->nav;
	if (v->panel) {
		slide_to(n, sheet_rest(n));
		return;
	}
	AdwDialog *d = v->dialog;
	if (!d) return;
	win *w = lx_declared_win(n);
	if (v->full) {
		int ww = gtk_widget_get_width((GtkWidget *)w->window), wh = gtk_widget_get_height((GtkWidget *)w->window);
		if (ww <= 0 || wh <= 0) gtk_window_get_default_size(w->window, &ww, &wh);  // not shown yet
		adw_dialog_set_content_width(d, ww > 1 ? ww : 1);
		adw_dialog_set_content_height(d, wh > 1 ? wh : 1);
		return;
	}
	float width = sheet_width(n), height = sheet_rest(n);
	if (sheet_fit_only(n) && v->fit_w > 0) {
		// As tall as its content: the size the core laid it out at (wider
		// than a sheet when its content can't be narrower).
		float ww, wh;
		sheet_window(n, &ww, &wh);
		width = v->fit_w < ww ? v->fit_w : ww;
	}
	adw_dialog_set_content_width(d, lx_window_size(width));
	adw_dialog_set_content_height(d, lx_window_size(height));
	if (v->corner > 0) sheet_corners(n);  // a narrower window may have made it a bottom sheet
	follow_height(n);
}

static void show_sheet(node *n);

void lx_sheet_action(node *n, ak_span args) {
	float w, h;
	nav *v = n->nav;
	if (!v || args.len < 9 || (uint8_t)args.p[0] != AK_WINDOW_ACTION_FIT) return;
	memcpy(&w, args.p + 1, 4);
	memcpy(&h, args.p + 5, 4);
	if (w == v->fit_w && h == v->fit_h && !v->fit_pending) return;
	v->fit_w = w, v->fit_h = h;
	if (v->fit_pending) {
		v->fit_pending = false;
		show_sheet(n);  // its first Fit: it shows at its content's size
		return;
	}
	size_sheet(n);  // its fit detent's height
}

// show_sheet shows a sheet over the window it's declared in: an
// undimmed one as a panel, the others as a dialog.
static void show_sheet(node *n) {
	nav *v = n->nav;
	if (v->undimmed > 0 && !v->full) {
		present_panel(n);
		return;
	}
	size_sheet(n);
	adw_dialog_present(v->dialog, lx_declared_win(n)->content);
	sheet_corners(n);
	follow_height(n);
}

bool lx_sheet_escape(node *n) {
	nav *v = n->nav;
	if (!v || v->gone) return false;
	if (v->panel) {
		panel_dismiss(n);
		return true;
	}
	if (v->no_dismiss && !n->intercept_back && v->dialog) {
		adw_dialog_force_close(v->dialog);
		return true;
	}
	return false;
}

void lx_sheet_dump(node *n, ak_buf *b) {
	nav *v = n->nav;
	if (!v || !n->widget) return;
	ak_buf_printf(b, " sheet=%dx%d detent=%u height=%.0f", gtk_widget_get_width(n->widget), gtk_widget_get_height(n->widget),
	              detent_count(v) ? sheet_at(n) : 0, shown_height(n));
	if (v->panel) ak_buf_printf(b, " panel dim=%.2f", (double)gtk_widget_get_opacity(v->panel->scrim));
}

// present_sheet presents a sheet. One as tall as its content is told its
// room first, and shows once the core's Fit says its size (as a Mac's
// sheet does), not at another size before it.
static void present_sheet(node *n) {
	nav *v = n->nav;
	if (sheet_fit_only(n) && v->fit_h <= 0) {
		float w, h;
		sheet_room(n, &w, &h);
		v->env_w = w, v->env_h = h;
		v->fit_pending = true;
		ak_event ev = {.id = n->id, .kind = AK_EVENT_ENVIRONMENT};
		lx_root_env(n, &ev.env);
		lx_dispatch(&ev);  // laid out after this batch: its Fit comes
		return;
	}
	show_sheet(n);
}

// ---- Alerts ----

static void on_alert_response(AdwAlertDialog *d, const char *resp, node *n) {
	(void)d;
	n->nav->gone = true;
	char *end;
	long i = strtol(resp, &end, 10);
	if (*resp && !*end && i >= 0 && (uint32_t)i < n->actions.len) {
		lx_emit(n, (ak_event){.kind = AK_EVENT_ACTION, .i = i});
		return;
	}
	lx_emit(n, (ak_event){.kind = AK_EVENT_DISMISSED});
}

static void on_row_changed(GtkEditable *row, node *n) {
	if (n->dead || n->nav->gone) return;
	for (uint32_t i = 0; i < n->nav->rows.len; i++)
		if ((GtkEditable *)n->nav->rows.data[i] == row) {
			ak_buf b = {0};
			ak_append_field_changed(&b, i, ak_cstr(gtk_editable_get_text(row)));
			lx_emit(n, (ak_event){.kind = AK_EVENT_FIELD_CHANGED, .s = ak_strn(b.data, b.len)});
			ak_buf_free(&b);
			return;
		}
}

static void on_row_activated(AdwEntryRow *row, gpointer next) {
	(void)row;
	gtk_widget_grab_focus(next);
}

// field_purpose is what an alert's field tells the input method: its
// keyboard and content type, as a TextField's.
static void field_purpose(const ak_alert_field *f, GtkInputPurpose *purpose, GtkInputHints *hints) {
	*purpose = GTK_INPUT_PURPOSE_FREE_FORM;
	switch (f->keyboard) {
	case AK_KEYBOARD_NUMBER: *purpose = GTK_INPUT_PURPOSE_DIGITS; break;
	case AK_KEYBOARD_DECIMAL: *purpose = GTK_INPUT_PURPOSE_NUMBER; break;
	case AK_KEYBOARD_EMAIL: *purpose = GTK_INPUT_PURPOSE_EMAIL; break;
	case AK_KEYBOARD_URL: *purpose = GTK_INPUT_PURPOSE_URL; break;
	case AK_KEYBOARD_PHONE: *purpose = GTK_INPUT_PURPOSE_PHONE; break;
	}
	*hints = GTK_INPUT_HINT_UPPERCASE_SENTENCES;
	ak_span ct = f->content_type;
	if (ak_span_eqc(ct, "oneTimeCode")) {
		*purpose = GTK_INPUT_PURPOSE_PIN;
		*hints = GTK_INPUT_HINT_NO_SPELLCHECK;
	} else if (ak_span_eqc(ct, "email")) {
		*purpose = GTK_INPUT_PURPOSE_EMAIL;
		*hints = GTK_INPUT_HINT_NO_SPELLCHECK;
	} else if (ak_span_eqc(ct, "phone")) {
		*purpose = GTK_INPUT_PURPOSE_PHONE;
	} else if (ak_span_eqc(ct, "name") || ak_span_eqc(ct, "givenName") || ak_span_eqc(ct, "familyName")) {
		*purpose = GTK_INPUT_PURPOSE_NAME;
		*hints = GTK_INPUT_HINT_UPPERCASE_WORDS;
	} else if (ak_span_eqc(ct, "cardNumber")) {
		*purpose = GTK_INPUT_PURPOSE_DIGITS;
		*hints = GTK_INPUT_HINT_NO_SPELLCHECK;
	} else if (ak_span_eqc(ct, "postalCode") || ak_span_eqc(ct, "username")) {
		*hints = GTK_INPUT_HINT_NO_SPELLCHECK;
	} else if (ak_span_eqc(ct, "url")) {
		*purpose = GTK_INPUT_PURPOSE_URL;
		*hints = GTK_INPUT_HINT_NO_SPELLCHECK;
	}
	if (*purpose == GTK_INPUT_PURPOSE_EMAIL || *purpose == GTK_INPUT_PURPOSE_URL) *hints = GTK_INPUT_HINT_NO_SPELLCHECK;
}

// alert_fields gives an alert its text fields, as libadwaita's dialogs
// have them: a boxed list of entry rows (password entry rows for
// passwords), each titled with its placeholder, under the message. Typing
// reports FieldChanged; activating a row before the last goes on to the
// next, and the last activates the default response. It returns the
// first row, which takes the focus.
static GtkWidget *alert_fields(node *n, AdwAlertDialog *d) {
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode((GtkListBox *)list, 0 /* GTK_SELECTION_NONE */);
	gtk_widget_add_css_class(list, "boxed-list");
	nav *v = n->nav;
	v->rows.len = 0;
	for (uint32_t i = 0; i < v->fields.len; i++) {
		const ak_alert_field *f = &v->fields.data[i];
		GtkWidget *row;
		if (f->flags & AK_ALERT_FIELD_SECURE) {
			row = adw_password_entry_row_new();
		} else {
			row = adw_entry_row_new();
			GtkInputPurpose purpose;
			GtkInputHints hints;
			field_purpose(f, &purpose, &hints);
			adw_entry_row_set_input_purpose((AdwEntryRow *)row, purpose);
			adw_entry_row_set_input_hints((AdwEntryRow *)row, hints);
		}
		char *ph = str_dup(f->placeholder), *tx = str_dup(f->text);
		adw_preferences_row_set_title((AdwPreferencesRow *)row, ph);
		gtk_editable_set_text((GtkEditable *)row, tx);
		free(ph);
		free(tx);
		ak_vec_push(&v->rows, row);
		gtk_list_box_append((GtkListBox *)list, row);
	}
	for (uint32_t i = 0; i < v->rows.len; i++) {
		GtkWidget *row = v->rows.data[i];
		lx_connect(n, row, "changed", G_CALLBACK(on_row_changed));
		if (i + 1 < v->rows.len)
			g_signal_connect(row, "entry-activated", G_CALLBACK(on_row_activated), v->rows.data[i + 1]);
		else
			adw_entry_row_set_activates_default((AdwEntryRow *)row, 1);
	}
	adw_alert_dialog_set_extra_child(d, list);
	return v->rows.len ? v->rows.data[0] : NULL;
}

// present_alert shows an AdwAlertDialog of the alert's buttons: the first
// default button suggested, a destructive one red, Escape the cancel
// button; a button sends Action with its index, Escape (with no cancel
// button) Dismissed.
static void present_alert(node *n, GtkWidget *parent) {
	AdwAlertDialog *d = (AdwAlertDialog *)adw_alert_dialog_new(S(n->title), S(n->message));
	n->nav->dialog = (AdwDialog *)d;
	char close_id[16] = "close";
	bool suggested = false;
	for (uint32_t i = 0; i < n->actions.len; i++) {
		action *a = &n->actions.data[i];
		char id[16];
		snprintf(id, sizeof id, "%u", i);
		adw_alert_dialog_add_response(d, id, S(a->title));
		if (action_is(a, AK_MENU_DISABLED)) adw_alert_dialog_set_response_enabled(d, id, 0);
		switch (a->role) {
		case AK_ALERT_BUTTON_ROLE_DESTRUCTIVE:
			adw_alert_dialog_set_response_appearance(d, id, ADW_RESPONSE_DESTRUCTIVE);
			break;
		case AK_ALERT_BUTTON_ROLE_CANCEL:
			snprintf(close_id, sizeof close_id, "%s", id);
			break;
		default:
			if (!suggested && n->alert_style == AK_ALERT_STYLE_ALERT) {
				adw_alert_dialog_set_response_appearance(d, id, ADW_RESPONSE_SUGGESTED);
				adw_alert_dialog_set_default_response(d, id);
				suggested = true;
			}
		}
	}
	if (!n->actions.len) {
		adw_alert_dialog_add_response(d, "0", lx_word("ok"));
		snprintf(close_id, sizeof close_id, "0");
	}
	adw_alert_dialog_set_close_response(d, close_id);
	GtkWidget *first = NULL;
	if (n->alert_style == AK_ALERT_STYLE_ALERT && n->nav->fields.len) first = alert_fields(n, d);
	lx_connect(n, d, "response", G_CALLBACK(on_alert_response));
	adw_dialog_present((AdwDialog *)d, parent);
	if (first) adw_dialog_set_focus((AdwDialog *)d, first);
}

// ---- Windows ----

// lx_open_window shows a Window node's window at its size. Where it goes
// is the desktop's to say, unless the app says (X11: desk.c). One opened
// hidden is laid out at its size first, its environment reported as if
// shown, and shown by a Show or Focus.
void lx_open_window(node *n) {
	win *w = n->win;
	if (!w->configured) {
		ak_window_config c = {0};
		lx_configure(w, &c);
	}
	gtk_window_set_application(w->window, H.app);
	if (w->hidden) {
		int dw = 0, dh = 0;
		gtk_window_get_default_size(w->window, &dw, &dh);
		int h = dh - lx_header_room(w);
		lx_root_allocated(n, dw, h > 0 ? h : 0);
		return;
	}
	lx_show_win(w, true);
}
