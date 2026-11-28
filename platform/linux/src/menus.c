// menus.c: menus are GMenu models in GtkPopoverMenus: a button's drops
// from it, a view's LongPress menu is its context menu (a secondary
// click, a long press) at the pointer. Items are actions of a group on
// the view ("asuka.item<number>"), disabled ones disabled, checked ones
// stateful (a check by the item); choosing one sends Action with its
// number. A submenu is a GMenu submenu, a section (MenuInline) a GMenu
// section, under its title as its label.
//
// The app's menus (MenuBar): GNOME's apps have no menu bar but a main
// menu, the header bar's last button; each of the app's menus is a
// submenu of it (its items, when it has one), and an item with a submenu
// opens one of its own. The items' shortcuts are the application's
// accelerators, in every window. A chosen item sends Action on the root
// with its number among all the menus' items; a Quit item quits the app
// as the user's quit does (the app asked first), and takes Ctrl+Q; a
// Close item closes the window in use (Ctrl+W); Undo and Redo items are
// undo.c's (Ctrl+Z, Ctrl+Shift+Z).
#include "host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

void lx_listen_long_press(node *n);
void lx_listen_click(node *n);
void lx_popup_at(node *n, GMenuModel *model, double x, double y);
const char *lx_word_app(const char *key, const char *app);
const char *lx_undo_title(bool redo);
void lx_undo_item(bool redo, GSimpleAction *a, bool disabled);
bool lx_undo_enabled(bool redo);
void lx_undo_choose(bool redo, int64_t index);

void lx_menu_changed(node *n) {
	if (!n->widget || !n->has_menu) return;
	if (n->menu_trigger == AK_MENU_TRIGGER_LONG_PRESS) lx_listen_long_press(n);
	if (n->menu_trigger == AK_MENU_TRIGGER_TAP && n->kind != AK_KIND_BUTTON) {
		lx_listen_click(n);
		lx_make_pressable(n);
	}
}

static void item_icon(GMenuItem *item, const char *symbol) {
	if (str_empty(symbol)) return;
	const char *name = lx_icon_name(symbol);
	if (!*name) return;
	GIcon *icon = g_themed_icon_new(name);
	g_menu_item_set_icon(item, icon);
	g_object_unref(icon);
}

// lx_menu_model builds a GMenu of actions, their items naming the actions
// prefix+number (lx_insert_menu_actions makes them).
GMenuModel *lx_menu_model(const action_vec *actions, const char *prefix) {
	GMenu *m = g_menu_new(), *section = g_menu_new();
	int items = 0;  // in section
#define FLUSH()                                                     \
	do {                                                            \
		if (items > 0) g_menu_append_section(m, NULL, (GMenuModel *)section); \
		g_object_unref(section);                                    \
		section = g_menu_new();                                     \
		items = 0;                                                  \
	} while (0)
	for (uint32_t i = 0; i < actions->len; i++) {
		const action *a = &actions->data[i];
		if (action_is(a, AK_MENU_SUBMENU | AK_MENU_INLINE)) {
			// A section of its own, under its title.
			FLUSH();
			GMenuModel *sub = lx_menu_model(&a->items, prefix);
			g_menu_append_section(m, str_empty(a->title) ? NULL : a->title, sub);
			g_object_unref(sub);
			continue;
		}
		if (action_is(a, AK_MENU_SUBMENU)) {
			GMenuModel *sub = lx_menu_model(&a->items, prefix);
			GMenuItem *item = g_menu_item_new_submenu(S(a->title), sub);
			g_object_unref(sub);
			item_icon(item, a->icon);
			g_menu_append_item(section, item);
			g_object_unref(item);
			items++;
			continue;
		}
		// GTK's menus have no destructive style: a section of its own, as
		// GNOME's apps set a destructive item apart.
		if (a->role == AK_ALERT_BUTTON_ROLE_DESTRUCTIVE && items > 0) FLUSH();
		char detailed[64];
		snprintf(detailed, sizeof detailed, "%s%lld", prefix, (long long)a->index);
		GMenuItem *item = g_menu_item_new(S(a->title), detailed);
		item_icon(item, a->icon);
		g_menu_append_item(section, item);
		g_object_unref(item);
		items++;
	}
	FLUSH();
#undef FLUSH
	g_object_unref(section);
	return (GMenuModel *)m;
}

typedef struct {
	ak_id id;
	int64_t index;
} menu_act;

static void on_item(GSimpleAction *act, GVariant *param, gpointer data) {
	(void)act;
	(void)param;
	menu_act *m = data;
	node *n = node_get(m->id);
	if (n) lx_emit(n, (ak_event){.kind = AK_EVENT_ACTION, .i = m->index});
}

static void add_actions(GSimpleActionGroup *g, ak_id id, const action_vec *as) {
	for (uint32_t i = 0; i < as->len; i++) {
		const action *a = &as->data[i];
		if (action_is(a, AK_MENU_SUBMENU)) {
			add_actions(g, id, &a->items);
			continue;
		}
		char name[48];
		snprintf(name, sizeof name, "item%lld", (long long)a->index);
		GSimpleAction *act = action_is(a, AK_MENU_CHECKED) ? g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(1)) : g_simple_action_new(name, NULL);
		g_simple_action_set_enabled(act, !action_is(a, AK_MENU_DISABLED));
		menu_act *m = malloc(sizeof *m);
		if (!m) abort();
		m->id = id;
		m->index = a->index;
		g_signal_connect_data(act, "activate", G_CALLBACK(on_item), m, (GClosureNotify)(void (*)(void))free, (GConnectFlags)0);
		g_action_map_add_action((GActionMap *)g, (GAction *)act);
		g_object_unref(act);
	}
}

// lx_insert_menu_actions gives widget the actions of a menu's items, at
// any depth, as the group named group: "item<number>", choosing it sends
// Action with its number to the node with id.
void lx_insert_menu_actions(GtkWidget *widget, const char *group, ak_id id, const action_vec *items) {
	GSimpleActionGroup *g = g_simple_action_group_new();
	add_actions(g, id, items);
	gtk_widget_insert_action_group(widget, group, (GActionGroup *)g);
	g_object_unref(g);
}

// lx_popup_menu shows n's menu: from the view (a button's, x < 0) or at a
// point in it (a context menu).
void lx_popup_menu(node *n, double x, double y) {
	if (!n->has_menu || !n->widget) return;
	lx_insert_menu_actions(n->widget, "asuka", n->id, &n->menu);
	GMenuModel *model = lx_menu_model(&n->menu, "asuka.item");
	lx_popup_at(n, model, x, y);
	g_object_unref(model);
}

// ---- The app's menus ----

typedef struct {
	int index;
	uint8_t role, flags;
	char *title;
} app_item;

static AK_VEC(app_item) app_items;  // the app menu's items, flattened

// lx_key_gdk_name is a key as GDK names it ("" none).
void lx_key_gdk_name(const char *key, char *out, size_t n) {
	static const char *const named[][2] = {{"enter", "Return"}, {"escape", "Escape"}, {"tab", "Tab"}, {"space", "space"},
		{"delete", "Delete"}, {"backspace", "BackSpace"}, {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
		{"home", "Home"}, {"end", "End"}, {"pageup", "Page_Up"}, {"pagedown", "Page_Down"},
		{",", "comma"}, {".", "period"}, {"/", "slash"}, {"\\", "backslash"}, {";", "semicolon"}, {"'", "apostrophe"},
		{"[", "bracketleft"}, {"]", "bracketright"}, {"-", "minus"}, {"=", "equal"}, {"`", "grave"}, {"+", "plus"}};
	out[0] = 0;
	for (size_t i = 0; i < AK_LEN(named); i++)
		if (!strcmp(key, named[i][0])) {
			snprintf(out, n, "%s", named[i][1]);
			return;
		}
	if ((key[0] == 'f' || key[0] == 'F') && key[1] && isdigit((unsigned char)key[1])) {
		bool digits = true;
		for (const char *p = key + 1; *p; p++) digits &= isdigit((unsigned char)*p) != 0;
		if (digits) {
			snprintf(out, n, "F%s", key + 1);
			return;
		}
	}
	if (g_utf8_strlen(key, -1) == 1) {
		char *l = g_utf8_strdown(key, -1);
		snprintf(out, n, "%s", l);
		g_free(l);
	}
}

// lx_accelerator is a shortcut in GTK's accelerator syntax:
// <Control><Shift>s (NULL: none; free it).
char *lx_accelerator(const char *key, uint8_t mods) {
	if (str_empty(key)) return NULL;
	char name[32];
	lx_key_gdk_name(key, name, sizeof name);
	if (!name[0]) return NULL;
	char buf[96];
	snprintf(buf, sizeof buf, "%s%s%s%s", (mods & 1) || (mods & 8) ? "<Control>" : "", mods & 2 ? "<Shift>" : "", mods & 4 ? "<Alt>" : "", name);
	return strdup(buf);
}

// item_title is what an item says: its title, or its role's.
static const char *item_title(const ak_menu_entry *it, char *buf, size_t n) {
	if (!it->title.len && it->role == AK_MENU_ROLE_QUIT) return lx_word_app("quit", H.app_name);
	if (!it->title.len && it->role == AK_MENU_ROLE_CLOSE) return lx_word("close");
	if (!it->title.len && (it->role == AK_MENU_ROLE_UNDO || it->role == AK_MENU_ROLE_REDO)) return lx_undo_title(it->role == AK_MENU_ROLE_REDO);
	size_t k = it->title.len < n - 1 ? it->title.len : n - 1;
	memcpy(buf, it->title.p, k);
	buf[k] = 0;
	return buf;
}

static void close_active(void *ctx) {
	(void)ctx;
	GtkWindow *w = gtk_application_get_active_window(H.app);
	if (w) gtk_window_close(w);
}

static void quit_now(void *ctx) {
	(void)ctx;
	lx_request_quit();
}

// choose_app_item is the app menu's item chosen: Action on the root, or
// the app quitting for a Quit item.
static void choose_app_item(int index, uint8_t role) {
	if (role == AK_MENU_ROLE_QUIT) {
		lx_after_core(quit_now, NULL);
		return;
	}
	if (role == AK_MENU_ROLE_CLOSE) {
		// The window in use, as its close button closes it.
		lx_after_core(close_active, NULL);
		return;
	}
	if (role == AK_MENU_ROLE_UNDO || role == AK_MENU_ROLE_REDO) {
		// A focused text's own, else the app's.
		lx_undo_choose(role == AK_MENU_ROLE_REDO, index);
		return;
	}
	if (H.root) lx_dispatch(&(ak_event){.id = H.root->id, .kind = AK_EVENT_ACTION, .i = index});
}

static void on_app_item(GSimpleAction *act, GVariant *param, gpointer data) {
	(void)act;
	(void)param;
	uintptr_t v = (uintptr_t)data;
	choose_app_item((int)(v >> 8), (uint8_t)(v & 0xff));
}

// app_menu_model builds a GMenu of the app's items (entries first..first+
// count at depth): sections between separators, a submenu for an item
// with one, each item an action of the application's
// ("app.asuka-menu-<number>").
static GMenuModel *app_menu_model(const ak_menu_entry *es, uint32_t n, uint32_t first, uint32_t count) {
	GMenu *m = g_menu_new(), *section = g_menu_new();
	uint32_t i = first;
	for (uint32_t k = 0; k < count && i < n; k++) {
		const ak_menu_entry *it = &es[i];
		uint32_t next = i + 1;
		// Skip the entry's submenu entries (depth first) to its next sibling.
		while (next < n && es[next].depth > it->depth) next++;
		if (it->flags & AK_MENU_SEPARATOR) {
			g_menu_append_section(m, NULL, (GMenuModel *)section);
			g_object_unref(section);
			section = g_menu_new();
			i = next;
			continue;
		}
		char tb[256];
		const char *title = item_title(it, tb, sizeof tb);
		if (it->flags & AK_MENU_SUBMENU) {
			GMenuModel *sub = app_menu_model(es, n, i + 1, it->count);
			g_menu_append_submenu(section, title, sub);
			g_object_unref(sub);
			i = next;
			continue;
		}
		char name[48], detailed[64];
		snprintf(name, sizeof name, "asuka-menu-%u", it->index);
		snprintf(detailed, sizeof detailed, "app.%s", name);
		GSimpleAction *a = (it->flags & AK_MENU_CHECKED) ? g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(1)) : g_simple_action_new(name, NULL);
		bool undo = it->role == AK_MENU_ROLE_UNDO || it->role == AK_MENU_ROLE_REDO;
		g_simple_action_set_enabled(a, !(it->flags & AK_MENU_DISABLED));
		if (undo) lx_undo_item(it->role == AK_MENU_ROLE_REDO, a, (it->flags & AK_MENU_DISABLED) != 0);  // enabled as undo.c says
		g_signal_connect(a, "activate", G_CALLBACK(on_app_item), (gpointer)(((uintptr_t)it->index << 8) | it->role));
		g_action_map_add_action((GActionMap *)H.app, (GAction *)a);
		g_object_unref(a);
		ak_vec_push(&H.app_actions, strdup(name));
		char key[32];
		size_t kn = it->key.len < sizeof key - 1 ? it->key.len : sizeof key - 1;
		memcpy(key, it->key.p, kn);
		key[kn] = 0;
		uint8_t mods = it->mods;
		if (it->role == AK_MENU_ROLE_QUIT && !kn) {
			snprintf(key, sizeof key, "q");
			mods = 1;
		}
		if (it->role == AK_MENU_ROLE_CLOSE && !kn) {
			snprintf(key, sizeof key, "w");
			mods = 1;
		}
		if (undo && !kn) {
			snprintf(key, sizeof key, "z");
			mods = it->role == AK_MENU_ROLE_REDO ? 3 : 1;
		}
		char *accel = lx_accelerator(key, mods);
		if (accel && (undo || !(it->flags & AK_MENU_DISABLED))) {  // a disabled Undo leaves Ctrl+Z to the focused widget
			const char *list[2] = {accel, NULL};
			gtk_application_set_accels_for_action(H.app, detailed, list);
		}
		free(accel);
		GMenuItem *item = g_menu_item_new(title, detailed);
		g_menu_append_item(section, item);
		g_object_unref(item);
		i = next;
	}
	g_menu_append_section(m, NULL, (GMenuModel *)section);
	g_object_unref(section);
	return (GMenuModel *)m;
}

static ak_buf menu_bar;  // the MenuBar as last set (undo.c's names change its titles)

void lx_menu_rebuild(void);
void lx_menu_rebuild(void) {
	ak_buf b = menu_bar;
	menu_bar = (ak_buf){0};
	lx_set_menu_bar((ak_span){(const char *)b.data, b.len});
	ak_buf_free(&b);
}

void lx_set_menu_bar(ak_span b) {
	if ((const uint8_t *)b.p != menu_bar.data) {
		ak_buf_reset(&menu_bar);
		ak_buf_put(&menu_bar, b.p, b.len);
	}
	lx_undo_item(false, NULL, false);
	lx_undo_item(true, NULL, false);
	// The previous menus' actions and accelerators go.
	for (uint32_t i = 0; i < H.app_actions.len; i++) {
		char detailed[64];
		snprintf(detailed, sizeof detailed, "app.%s", H.app_actions.data[i]);
		const char *none[1] = {NULL};
		gtk_application_set_accels_for_action(H.app, detailed, none);
		g_action_map_remove_action((GActionMap *)H.app, H.app_actions.data[i]);
		free(H.app_actions.data[i]);
	}
	H.app_actions.len = 0;
	for (uint32_t i = 0; i < app_items.len; i++) free(app_items.data[i].title);
	app_items.len = 0;
	ak_menu_title_vec menus = {0};
	ak_menu_entry_vec items = {0};
	ak_decode_menu_bar((const uint8_t *)b.p, b.len, &menus, &items);
	for (uint32_t i = 0; i < items.len; i++) {
		char tb[256];
		app_item a = {(int)items.data[i].index, items.data[i].role, items.data[i].flags, strdup(item_title(&items.data[i], tb, sizeof tb))};
		ak_vec_push(&app_items, a);
	}
	if (H.app_menu) {
		g_object_unref(H.app_menu);
		H.app_menu = NULL;
	}
	if (menus.len) {
		GMenu *root = g_menu_new();
		for (uint32_t i = 0; i < menus.len; i++) {
			// The menu's own items: those at depth 0 among its entries.
			uint32_t top = 0;
			for (uint32_t k = menus.data[i].first; k < menus.data[i].first + menus.data[i].total && k < items.len; k++)
				if (items.data[k].depth == 0) top++;
			GMenuModel *sub = app_menu_model(items.data, items.len, menus.data[i].first, top);
			if (menus.len == 1) {
				g_menu_append_section(root, NULL, sub);
			} else {
				char *t = str_dup(menus.data[i].title);
				g_menu_append_submenu(root, t, sub);
				free(t);
			}
			g_object_unref(sub);
		}
		H.app_menu = (GMenuModel *)root;
	}
	ak_vec_free(&menus);
	ak_vec_free(&items);
	H.menus_version++;
	lx_queue_chrome();
}

// lx_activate_app_item chooses the app menu's item with a title, as a
// click on it would.
bool lx_activate_app_item(const char *title) {
	for (uint32_t i = 0; i < app_items.len; i++) {
		app_item *it = &app_items.data[i];
		if (strcmp(it->title, title) || (it->flags & (AK_MENU_SEPARATOR | AK_MENU_SUBMENU))) continue;
		if (it->role == AK_MENU_ROLE_UNDO || it->role == AK_MENU_ROLE_REDO) {
			if (!lx_undo_enabled(it->role == AK_MENU_ROLE_REDO)) return false;
		} else if (it->flags & AK_MENU_DISABLED) {
			return false;
		}
		choose_app_item(it->index, it->role);
		return true;
	}
	return false;
}
