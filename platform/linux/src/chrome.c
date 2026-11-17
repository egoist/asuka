// chrome.c: what a window's header bar shows: the innermost stack on
// screen's back button (while there's a screen to go back to), its top
// screen's title, bar buttons and search, the tabs' switcher on a wide
// window, and the app's menu (MenuBar) as GNOME's main menu.
//
// A screen's search: a search button in the header bar and, under it,
// GTK's search bar with its entry, as GNOME's apps search; typing anywhere
// in the window starts it. Typing is the screen's TextChanged, Enter its
// Submit.
#include "host.h"

#include <stdlib.h>

struct search_field {
	GtkWidget *bar, *entry;
	node *screen;
	bool updating;
};

const char *lx_word(const char *key);
void lx_place_header(win *w, node *split);
GtkWidget *lx_tabs_switcher(node *tabs);
bool lx_tabs_narrow(node *tabs);
void lx_watch_preedit(GtkWidget *text);

static void search_set_text(search_field *s, const char *t) {
	if (str_eq(gtk_editable_get_text((GtkEditable *)s->entry), t)) return;
	s->updating = true;
	gtk_editable_set_text((GtkEditable *)s->entry, S(t));
	s->updating = false;
}

void lx_search_set_text(search_field *s, const char *t);
void lx_search_set_text(search_field *s, const char *t) { search_set_text(s, t); }

static void on_search_changed(GtkSearchEntry *e, gpointer data) {
	search_field *s = data;
	if (s->updating || !s->screen || s->screen->dead) return;
	const char *t = gtk_editable_get_text((GtkEditable *)e);
	str_setc(&s->screen->nav->search_tx, t);
	lx_emit(s->screen, (ak_event){.kind = AK_EVENT_TEXT_CHANGED, .s = ak_cstr(t)});
}

static void on_search_activate(GtkSearchEntry *e, gpointer data) {
	(void)e;
	search_field *s = data;
	if (s->screen && !s->screen->dead) lx_emit(s->screen, (ak_event){.kind = AK_EVENT_SUBMIT});
}

static void on_search_toggle(GtkToggleButton *b, gpointer data) {
	gtk_search_bar_set_search_mode((GtkSearchBar *)data, gtk_toggle_button_get_active(b));
}

static void on_search_mode(GObject *bar, GParamSpec *ps, gpointer data) {
	(void)ps;
	gtk_toggle_button_set_active((GtkToggleButton *)data, gtk_search_bar_get_search_mode((GtkSearchBar *)bar));
}

// search_for is the window's search button, its bar set for the screen.
static GtkWidget *search_for(win *w, node *screen) {
	search_field *s = w->search;
	if (!s) {
		s = calloc(1, sizeof *s);
		if (!s) abort();
		w->search = s;
		s->bar = gtk_search_bar_new();
		s->entry = gtk_search_entry_new();
		gtk_search_bar_set_child((GtkSearchBar *)s->bar, s->entry);
		gtk_search_bar_connect_entry((GtkSearchBar *)s->bar, (GtkEditable *)s->entry);
		lx_watch_preedit((GtkWidget *)gtk_editable_get_delegate((GtkEditable *)s->entry));
		gtk_search_bar_set_key_capture_widget((GtkSearchBar *)s->bar, (GtkWidget *)w->window);
		adw_toolbar_view_add_top_bar(w->toolbar, s->bar);
		g_signal_connect(s->entry, "search-changed", G_CALLBACK(on_search_changed), s);
		g_signal_connect(s->entry, "activate", G_CALLBACK(on_search_activate), s);
	}
	if (s->screen != screen) {
		s->screen = screen;
		screen->nav->search = s;
		gtk_search_entry_set_placeholder_text((GtkSearchEntry *)s->entry, S(screen->nav->search_ph));
		search_set_text(s, screen->nav->search_tx);
		gtk_search_bar_set_search_mode((GtkSearchBar *)s->bar, !str_empty(screen->nav->search_tx));
	}
	GtkWidget *b = gtk_toggle_button_new();
	gtk_button_set_icon_name((GtkButton *)b, "system-search-symbolic");
	gtk_widget_set_tooltip_text(b, lx_word("search"));
	gtk_toggle_button_set_active((GtkToggleButton *)b, gtk_search_bar_get_search_mode((GtkSearchBar *)s->bar));
	g_signal_connect(b, "toggled", G_CALLBACK(on_search_toggle), s->bar);
	g_signal_connect_data(s->bar, "notify::search-mode-enabled", G_CALLBACK(on_search_mode), b, NULL, (GConnectFlags)0);
	gtk_widget_set_visible(s->bar, 1);
	return b;
}

static void hide_search(win *w) {
	search_field *s = w->search;
	if (!s) return;
	if (s->screen && s->screen->nav) s->screen->nav->search = NULL;
	s->screen = NULL;
	gtk_search_bar_set_search_mode((GtkSearchBar *)s->bar, 0);
	gtk_widget_set_visible(s->bar, 0);
}

// lx_forget_search lets a window's search let go of a screen going.
void lx_forget_search(node *screen);
void lx_forget_search(node *screen) {
	for (uint32_t i = 0; i < H.wins.len; i++) {
		search_field *s = H.wins.data[i]->search;
		if (s && s->screen == screen) s->screen = NULL;
	}
}

// ---- The header bar ----

static void chrome_reset(chrome_t *c) {
	free(c->title);
	free(c->search);
	actions_free(&c->actions);
	ak_vec_free(&c->packed);
	memset(c, 0, sizeof *c);
}

static void chrome_take(chrome_t *dst, const chrome_t *src) {
	dst->back = src->back;
	str_setc(&dst->title, src->title);
	actions_copy(&dst->actions, &src->actions);
	dst->screen = src->screen;
	dst->stack = src->stack;
	dst->tabs = src->tabs;
	dst->split = src->split;
	str_setc(&dst->search, src->search);
	dst->menus = src->menus;
}

static bool chrome_same(const chrome_t *a, const chrome_t *b) {
	return a->back == b->back && str_eq(a->title, b->title) && actions_same(&a->actions, &b->actions) && a->screen == b->screen &&
		a->stack == b->stack && a->tabs == b->tabs && str_eq(a->search, b->search) && a->menus == b->menus && a->split == b->split;
}

// The finder fills a chrome_t whose strings and actions point at the
// nodes' own (borrowed: copied only into a window's chrome).
typedef struct {
	bool back;
	const char *title;
	const action_vec *actions;
	node *screen, *stack, *tabs, *split;
	const char *search;
} found;

static void find(node *n, found *c);

static void find1(node *n, found *c) {
	for (uint32_t i = 0; i < n->children.len; i++) {
		node *x = n->children.data[i];
		if (x->hidden) continue;
		found inner = {0};
		find(x, &inner);
		if (inner.stack) {
			node *tabs = c->tabs;
			*c = inner;
			if (!c->tabs && !c->back) c->tabs = tabs;
			return;
		}
		if (inner.split && !c->split) c->split = inner.split;
		if (inner.tabs && !c->tabs) {
			c->tabs = inner.tabs;
			if (!c->screen) {
				c->title = inner.title;
				c->actions = inner.actions;
				c->screen = inner.screen;
				c->search = inner.search;
			}
		}
	}
}

static bool live_page(node *x) { return x->nav && x->nav->page && !x->nav->gone; }

// find walks what's on screen from n: a stack's top screen, the tabs'
// selected one, a node's children.
static void find(node *n, found *c) {
	if (!n) return;
	switch (n->kind) {
	case AK_KIND_STACK: {
		if (!n->children.len) return;
		node *top = n->children.data[n->children.len - 1];
		for (uint32_t i = n->children.len; i-- > 0;)
			if (live_page(n->children.data[i])) {
				top = n->children.data[i];
				break;
			}
		int depth = 0;
		for (uint32_t i = 0; i < n->children.len; i++) depth += live_page(n->children.data[i]);
		c->stack = n;
		c->screen = top;
		c->back = depth > 1;
		c->title = top->title;
		c->actions = &top->actions;
		c->search = top->nav ? top->nav->search_ph : NULL;
		find1(top, c);
		return;
	}
	case AK_KIND_TABS:
		if (!lx_tabs_narrow(n) && !c->stack) c->tabs = n;
		if (n->selected >= 0 && (uint32_t)n->selected < n->children.len) {
			node *s = n->children.data[n->selected];
			if (!c->screen) {
				c->title = s->title;
				c->actions = &s->actions;
				c->screen = s;
				c->search = s->nav ? s->nav->search_ph : NULL;
			}
			find1(s, c);
		}
		return;
	case AK_KIND_SHEET:
	case AK_KIND_WINDOW:
	case AK_KIND_ALERT:
	case AK_KIND_POPOVER:
		return;  // their own
	case AK_KIND_SPLIT:
		// The detail's chrome, in a header bar over it (split.c).
		find1(n, c);
		c->split = n;
		return;
	}
	find1(n, c);
}

bool lx_shows_menu(win *w) { return w == H.main || (w->cfg.flags & AK_WINDOW_MENU_BAR); }

typedef struct {
	ak_id screen;
	int64_t index;
} bar_action;

static void bar_action_free(gpointer data, GClosure *c) {
	(void)c;
	free(data);
}

static void on_bar_button(GtkButton *b, gpointer data) {
	(void)b;
	bar_action *a = data;
	node *s = node_get(a->screen);
	if (s) lx_emit(s, (ak_event){.kind = AK_EVENT_ACTION, .i = a->index});
}

static void on_back(GtkButton *b, gpointer data) {
	(void)b;
	if (lx_overlay_back()) return;  // the top overlay that takes it, first
	node *stack = node_get((ak_id)(uintptr_t)data);
	if (stack) lx_go_back(stack);
}

static void update_win_chrome(win *w) {
	found f = {0};
	find(w->root, &f);
	if (w->node)
		for (uint32_t i = 0; i < w->node->children.len; i++) find(w->node->children.data[i], &f);
	chrome_t c = {0};
	c.back = f.back;
	c.title = f.title ? strdup(f.title) : NULL;
	if (f.actions) actions_copy(&c.actions, f.actions);
	c.screen = f.screen;
	c.stack = f.stack;
	c.tabs = f.tabs;
	c.split = f.split;
	c.search = f.search ? strdup(f.search) : NULL;
	if (lx_shows_menu(w)) c.menus = H.menus_version;
	lx_place_header(w, c.split);
	if (chrome_same(&c, &w->chrome)) {
		chrome_reset(&c);
		return;
	}
	for (uint32_t i = 0; i < w->chrome.packed.len; i++) adw_header_bar_remove(w->header, w->chrome.packed.data[i]);
	w->chrome.packed.len = 0;
	GtkWidget *sw = c.tabs ? lx_tabs_switcher(c.tabs) : NULL;
	adw_header_bar_set_title_widget(w->header, sw && !c.back ? sw : (GtkWidget *)w->title);
	widget_vec packed = {0};
	if (c.back) {
		GtkWidget *b = gtk_button_new();
		gtk_button_set_icon_name((GtkButton *)b, "go-previous-symbolic");
		gtk_widget_set_tooltip_text(b, lx_word("back"));
		g_signal_connect(b, "clicked", G_CALLBACK(on_back), (gpointer)(uintptr_t)c.stack->id);
		adw_header_bar_pack_start(w->header, b);
		ak_vec_push(&packed, b);
	}
	if (lx_shows_menu(w) && H.app_menu) {
		GtkWidget *mb = gtk_menu_button_new();
		gtk_menu_button_set_icon_name((GtkMenuButton *)mb, "open-menu-symbolic");
		gtk_menu_button_set_menu_model((GtkMenuButton *)mb, H.app_menu);
		gtk_widget_set_tooltip_text(mb, "Main Menu");
		adw_header_bar_pack_end(w->header, mb);
		ak_vec_push(&packed, mb);
	}
	node *screen = c.screen;
	for (uint32_t i = c.actions.len; i-- > 0;) {
		action *a = &c.actions.data[i];
		GtkWidget *b;
		if (action_is(a, AK_MENU_SUBMENU) && screen) {
			// A bar button with children opens them as a menu.
			b = gtk_menu_button_new();
			if (!str_empty(a->icon)) {
				gtk_menu_button_set_icon_name((GtkMenuButton *)b, lx_icon_name(a->icon));
				gtk_widget_set_tooltip_text(b, S(a->title));
			} else {
				gtk_menu_button_set_label((GtkMenuButton *)b, S(a->title));
			}
			lx_insert_menu_actions(b, "asukabar", screen->id, &a->items);
			GMenuModel *model = lx_menu_model(&a->items, "asukabar.item");
			gtk_menu_button_set_menu_model((GtkMenuButton *)b, model);
			g_object_unref(model);
		} else {
			b = gtk_button_new();
			if (!screen) {
				// No screen to report to.
			}
			if (!str_empty(a->icon)) {
				gtk_button_set_icon_name((GtkButton *)b, lx_icon_name(a->icon));
				gtk_widget_set_tooltip_text(b, S(a->title));
			} else {
				gtk_button_set_label((GtkButton *)b, S(a->title));
			}
			bar_action *ba = malloc(sizeof *ba);
			if (!ba) abort();
			ba->screen = screen->id;
			ba->index = a->index;
			g_signal_connect_data(b, "clicked", G_CALLBACK(on_bar_button), ba, bar_action_free, (GConnectFlags)0);
		}
		if (a->role == AK_ALERT_BUTTON_ROLE_DESTRUCTIVE) gtk_widget_add_css_class(b, "destructive-action");
		gtk_widget_set_sensitive(b, !action_is(a, AK_MENU_DISABLED));
		adw_header_bar_pack_end(w->header, b);
		ak_vec_push(&packed, b);
	}
	if (!str_empty(c.search) && screen) {
		GtkWidget *bt = search_for(w, screen);
		adw_header_bar_pack_end(w->header, bt);
		ak_vec_push(&packed, bt);
	} else {
		hide_search(w);
	}
	chrome_reset(&w->chrome);
	chrome_take(&w->chrome, &c);
	w->chrome.packed = packed;
	chrome_reset(&c);
	lx_update_title(w);
}

void lx_update_chrome(void) {
	if (H.main) update_win_chrome(H.main);
	for (uint32_t i = 0; i < H.wins.len; i++)
		if (H.wins.data[i] != H.main && !H.wins.data[i]->closing) update_win_chrome(H.wins.data[i]);
}

static void chrome_now(void *ctx) {
	(void)ctx;
	H.chrome_queued = false;
	lx_update_chrome();
}

// lx_queue_chrome updates the header bars once the batch (or the core's
// call) is done.
void lx_queue_chrome(void) {
	if (H.chrome_queued) return;
	H.chrome_queued = true;
	lx_after_core(chrome_now, NULL);
}

// lx_forget_chrome drops what windows' chrome names of a node going.
void lx_forget_chrome(node *n);
void lx_forget_chrome(node *n) {
	for (uint32_t i = 0; i < H.wins.len; i++) {
		chrome_t *c = &H.wins.data[i]->chrome;
		if (c->screen == n) c->screen = NULL;
		if (c->stack == n) c->stack = NULL;
		if (c->tabs == n) c->tabs = NULL;
		if (c->split == n) c->split = NULL;
	}
}

void lx_batch_applied(void) {
	lx_update_chrome();
	lx_modal_refresh();
}

// lx_go_back_front goes back in the innermost stack on screen in the
// window in use (automation's back).
bool lx_go_back_front(void) {
	if (lx_overlay_back()) return true;  // the top overlay that takes it, first
	win *w = H.active_win ? H.active_win : H.main;
	found f = {0};
	find(w->root, &f);
	return lx_go_back(f.stack);
}

// lx_go_back is the header's back button, or Alt+Left: the stack's top
// screen asks the core when it takes going back itself, else it's popped.
bool lx_go_back(node *stack) {
	if (!stack || stack->children.len < 2) return false;
	node *top = stack->children.data[stack->children.len - 1];
	if (top->intercept_back) {
		lx_emit(top, (ak_event){.kind = AK_EVENT_BACK});
		return true;
	}
	adw_navigation_view_pop((AdwNavigationView *)stack->nav->view);
	return true;
}

// lx_search_entry is a window's search entry and the screen it searches.
GtkWidget *lx_search_entry(win *w, node **screen);
GtkWidget *lx_search_entry(win *w, node **screen) {
	*screen = w->search ? w->search->screen : NULL;
	return w->search ? w->search->entry : NULL;
}
