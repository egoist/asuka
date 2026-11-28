// menu.c: the app's menus (MenuBar): a menu bar (an HMENU) on the main
// window, made again each time they change, and an accelerator table for
// their shortcuts, which the message loop translates for any of the app's
// windows; popup menus of actions (a view's Menu, a bar's buttons); and
// owner-drawn menus, for a destructive item's red and a section's header,
// drawn as the system draws a themed menu.
#include "win.h"

#include <vssym32.h>

#define MENU_BASE 0x100  // the command ID of the menus' first item

// ---- The app's menus, flattened ----

typedef struct {
	wchar_t *title;
	char key[24];
	uint8_t mods, role, flags;
	uint32_t index;      // its number among all the menus' items
	uint32_t depth, count;
	uint32_t menu;  // its menu bar menu
} bar_item;

static struct {
	wchar_t **titles;  // the menus'
	uint32_t ntitles;
	uint32_t *first, *total;
	bar_item *items;
	uint32_t nitems;
	HACCEL accel;
} bar;

enum { KM_PRIMARY = 1, KM_SHIFT = 2, KM_ALT = 4, KM_CONTROL = 8 };
#define MENU_HEADER 0x80  // a section's title among the items of actions: a header, not chosen

// ---- Owner-drawn menus ----

typedef struct owner_menu owner_menu;
typedef struct {
	owner_menu *menu;
	wchar_t *text, *key;
	bool separator, destructive, submenu, header;
} owner_item;

struct owner_menu {
	HMENU menu;
	owner_item **items;
	uint32_t n;
};

static wh_map owned;  // owner items by their item data
static uintptr_t owned_next;
static owner_menu **owned_menus;
static uint32_t nowned_menus;
static owner_menu **bar_owned;
static uint32_t nbar_owned;

typedef struct {
	wchar_t *text;
	bool separator, destructive, submenu, header;
} menu_item;

static owner_menu *owner_draw(HMENU m, const menu_item *items, uint32_t n, UINT order) {
	owner_menu *om = wh_alloc(sizeof *om);
	om->menu = m;
	om->items = wh_alloc((n ? n : 1) * sizeof *om->items);
	om->n = n;
	for (uint32_t i = 0; i < n; i++) {
		owner_item *it = wh_alloc(sizeof *it);
		it->menu = om;
		const wchar_t *tab = items[i].text ? wcschr(items[i].text, '\t') : NULL;
		size_t tl = items[i].text ? (tab ? (size_t)(tab - items[i].text) : wcslen(items[i].text)) : 0;
		it->text = wh_alloc((tl + 1) * sizeof(wchar_t));
		if (tl) memcpy(it->text, items[i].text, tl * sizeof(wchar_t));
		it->key = _wcsdup(tab ? tab + 1 : L"");
		it->separator = items[i].separator, it->destructive = items[i].destructive;
		it->submenu = items[i].submenu, it->header = items[i].header;
		om->items[i] = it;
		uintptr_t id = ++owned_next;
		wh_map_put(&owned, id, it);
		MENUITEMINFOW mi = {sizeof mi};
		mi.fMask = MIIM_FTYPE | MIIM_DATA;
		mi.fType = MFT_OWNERDRAW | order | (it->separator ? MFT_SEPARATOR : 0);
		mi.dwItemData = id;
		SetMenuItemInfoW(m, i, TRUE, &mi);
	}
	owned_menus = wh_realloc(owned_menus, (nowned_menus + 1) * sizeof *owned_menus);
	owned_menus[nowned_menus++] = om;
	return om;
}

// forget lets go of an owner-drawn menu's items (it's gone).
static void forget(owner_menu *om) {
	if (!om) return;
	uint32_t i = 0;
	uint64_t k;
	void *v;
	while (wh_map_next(&owned, &i, &k, &v))
		if (((owner_item *)v)->menu == om) wh_map_del(&owned, k);
	for (uint32_t j = 0; j < om->n; j++) {
		free(om->items[j]->text);
		free(om->items[j]->key);
		free(om->items[j]);
	}
	for (uint32_t j = 0; j < nowned_menus; j++)
		if (owned_menus[j] == om) {
			memmove(owned_menus + j, owned_menus + j + 1, (nowned_menus - j - 1) * sizeof *owned_menus);
			nowned_menus--;
			break;
		}
	free(om->items);
	free(om);
}

// ---- Keys and their names ----

static const struct {
	const char *name;
	WORD vk;
	const wchar_t *text;
} named[] = {
	{"enter", VK_RETURN, L"Enter"}, {"escape", VK_ESCAPE, L"Esc"}, {"tab", VK_TAB, L"Tab"},
	{"space", VK_SPACE, L"Space"}, {"delete", VK_DELETE, L"Del"}, {"backspace", VK_BACK, L"Backspace"},
	{"up", VK_UP, L"Up"}, {"down", VK_DOWN, L"Down"}, {"left", VK_LEFT, L"Left"}, {"right", VK_RIGHT, L"Right"},
	{"home", VK_HOME, L"Home"}, {"end", VK_END, L"End"}, {"pageup", VK_PRIOR, L"PgUp"}, {"pagedown", VK_NEXT, L"PgDn"},
};

static int function_key(const char *k) {
	if (k[0] != 'f' || !k[1] || strlen(k) > 3) return 0;
	int n = 0;
	for (const char *c = k + 1; *c; c++) {
		if (*c < '0' || *c > '9') return 0;
		n = n * 10 + (*c - '0');
	}
	return n >= 1 && n <= 12 ? n : 0;
}

static void lower(char *out, const char *s, size_t cap) {
	size_t i = 0;
	for (; s[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)s[i]);
	out[i] = 0;
}

// shortcut_text is how a menu item shows its shortcut: "Ctrl+Shift+K",
// "Ctrl+Enter", "F5" (Primary and Control are both Ctrl).
static void shortcut_text(wchar_t *out, size_t cap, const char *key, uint8_t mods) {
	out[0] = 0;
	if (!key[0]) return;
	char k[24];
	lower(k, key, sizeof k);
	wchar_t name[32];
	name[0] = 0;
	for (size_t i = 0; i < WH_LEN(named); i++)
		if (!strcmp(k, named[i].name)) wcscpy(name, named[i].text);
	if (!name[0]) {
		wchar_t *w = wh_wide(function_key(k) ? k : key, (int)strlen(key));
		for (wchar_t *p = w; *p; p++) *p = towupper(*p);
		wcsncpy(name, w, 31);
		name[31] = 0;
		free(w);
	}
	swprintf(out, cap, L"%ls%ls%ls%ls", mods & (KM_PRIMARY | KM_CONTROL) ? L"Ctrl+" : L"", mods & KM_SHIFT ? L"Shift+" : L"",
		mods & KM_ALT ? L"Alt+" : L"", name);
}

// wh_accelerator is a shortcut's accelerator: a character's virtual key on
// the keyboard's layout (with Shift if it takes it, as + does), or a named
// key's.
bool wh_accelerator(const char *key, uint8_t mods, ACCEL *a) {
	if (!key || !key[0]) return false;
	memset(a, 0, sizeof *a);
	a->fVirt = FVIRTKEY | FNOINVERT;
	char k[24];
	lower(k, key, sizeof k);
	bool found = false;
	for (size_t i = 0; i < WH_LEN(named); i++)
		if (!strcmp(k, named[i].name)) a->key = named[i].vk, found = true;
	if (!found && function_key(k)) a->key = (WORD)(VK_F1 + function_key(k) - 1), found = true;
	if (!found) {
		wchar_t *w = wh_wide(k, (int)strlen(k));
		size_t n = wcslen(w);
		if (n != 1) {
			free(w);
			return false;
		}
		wchar_t r = w[0];
		free(w);
		if (r >= 'a' && r <= 'z') a->key = (WORD)(r - 'a' + 'A');
		else if (r >= '0' && r <= '9') a->key = (WORD)r;
		else {
			SHORT s = VkKeyScanW(r);
			if (s == -1) return false;
			a->key = (WORD)(s & 0xFF);
			if (s & 0x100) a->fVirt |= FSHIFT;
		}
	}
	if (mods & (KM_PRIMARY | KM_CONTROL)) a->fVirt |= FCONTROL;
	if (mods & KM_SHIFT) a->fVirt |= FSHIFT;
	if (mods & KM_ALT) a->fVirt |= FALT;
	return true;
}

// access_keys gives each title an access key (Alt and the underlined
// letter): its first letter or digit no title before it took. A title's
// own & is shown as one. The titles are replaced.
static void access_keys(wchar_t **titles, uint32_t n) {
	wchar_t used[512];
	uint32_t nused = 0;
	for (uint32_t i = 0; i < n; i++) {
		const wchar_t *t = titles[i];
		size_t l = wcslen(t);
		wchar_t *s = wh_alloc((2 * l + 2) * sizeof *s);
		size_t k = 0;
		bool marked = false;
		for (size_t j = 0; j < l; j++) {
			wchar_t c = t[j];
			if (!marked && (IsCharAlphaNumericW(c))) {
				wchar_t lc = towlower(c);
				bool taken = false;
				for (uint32_t u = 0; u < nused; u++) taken |= used[u] == lc;
				if (!taken) {
					if (nused < WH_LEN(used)) used[nused++] = lc;
					s[k++] = '&';
					marked = true;
				}
			}
			if (c == '&') s[k++] = '&';
			s[k++] = c;
		}
		s[k] = 0;
		free(titles[i]);
		titles[i] = s;
	}
}

static wchar_t *dup_escaped(const wchar_t *t) {
	size_t l = wcslen(t), k = 0;
	wchar_t *s = wh_alloc((2 * l + 1) * sizeof *s);
	for (size_t j = 0; j < l; j++) {
		if (t[j] == '&') s[k++] = '&';
		s[k++] = t[j];
	}
	return s;
}

// ---- Building menus ----

// A menu to build: its items as the menus of the menu bar and of actions
// share them.
typedef struct mitem {
	wchar_t *title;
	char key[24];
	uint8_t mods, role, flags;
	int64_t index;  // -1: none (a header)
	struct mitem *items;
	uint32_t count;
} mitem;

typedef struct {
	int base;
	UINT order;  // MFT_RIGHTORDER, right to left
	bool shortcuts;
	owner_menu **owned;
	uint32_t nowned;
} builder;

static bool undo_role(uint8_t r) { return r == AK_MENU_ROLE_UNDO || r == AK_MENU_ROLE_REDO; }

// undo_key is an Undo or Redo item's shortcut where it has none of its
// own: Ctrl+Z, Ctrl+Y (Windows'; Ctrl+Shift+Z redoes too).
static const char *undo_key(uint8_t role, const char *key, uint8_t *mods) {
	if (key[0] || !undo_role(role)) return key;
	*mods = KM_PRIMARY;
	return role == AK_MENU_ROLE_UNDO ? "z" : "y";
}

// item_title is what an item says: a quit item's own title, else the
// platform's; an Undo or Redo item's names the change ("Undo Rename").
static const wchar_t *item_title(const mitem *it, wchar_t *buf, size_t cap) {
	if (!it->title[0] && undo_role(it->role)) return wh_undo_title(it->role == AK_MENU_ROLE_REDO, buf, cap);
	if (!it->title[0] && it->role == AK_MENU_ROLE_QUIT) {
		wh_word_app(buf, cap, "quit");
		return buf;
	}
	if (!it->title[0] && it->role == AK_MENU_ROLE_CLOSE) return L"Close";
	return it->title;
}

static HMENU popup(builder *b, const mitem *items, uint32_t n) {
	HMENU pop = CreatePopupMenu();
	wchar_t **names = wh_alloc((n ? n : 1) * sizeof *names);
	uint32_t nn = 0;
	for (uint32_t i = 0; i < n; i++)
		if (!(items[i].flags & AK_MENU_SEPARATOR) && !(items[i].flags & MENU_HEADER)) {
			wchar_t buf[256];
			names[nn++] = _wcsdup(item_title(&items[i], buf, WH_LEN(buf)));
		}
	access_keys(names, nn);
	uint32_t k = 0;
	menu_item *drawn = wh_alloc((n ? n : 1) * sizeof *drawn);
	bool owner = false;  // a destructive item or a header: the menu is owner-drawn
	for (uint32_t i = 0; i < n; i++) {
		const mitem *it = &items[i];
		if (it->flags & AK_MENU_SEPARATOR) {
			AppendMenuW(pop, MF_SEPARATOR, 0, NULL);
			drawn[i] = (menu_item){.separator = true};
			continue;
		}
		if (it->flags & MENU_HEADER) {
			// A section's title: drawn as a header, and not chosen.
			wchar_t *text = dup_escaped(it->title);
			AppendMenuW(pop, MF_STRING | MF_GRAYED | b->order, 0, text);
			drawn[i] = (menu_item){.text = text, .header = true};
			owner = true;
			continue;
		}
		UINT flags = MF_STRING | b->order;
		bool off = undo_role(it->role) ? !wh_undo_enabled(it->role == AK_MENU_ROLE_REDO) : (it->flags & AK_MENU_DISABLED) != 0;
		if (off) flags |= MF_GRAYED;
		wchar_t *text = names[k++];
		if (it->count) {
			AppendMenuW(pop, flags | MF_POPUP, (UINT_PTR)popup(b, it->items, it->count), text);
			drawn[i] = (menu_item){.text = _wcsdup(text), .submenu = true};
			continue;
		}
		if (it->flags & AK_MENU_CHECKED) flags |= MF_CHECKED;
		wchar_t sc[64];
		const char *key = it->key;
		uint8_t mods = it->mods;
		if (!key[0] && it->role == AK_MENU_ROLE_CLOSE) key = "w", mods = KM_PRIMARY;  // Close's Ctrl+W
		key = undo_key(it->role, key, &mods);
		shortcut_text(sc, WH_LEN(sc), key, mods);
		wchar_t full[600];
		if (sc[0] && b->shortcuts) swprintf(full, WH_LEN(full), L"%ls\t%ls", text, sc);
		else swprintf(full, WH_LEN(full), L"%ls", text);
		AppendMenuW(pop, flags, (UINT_PTR)(b->base + it->index), full);
		bool d = (it->flags & AK_MENU_DESTRUCTIVE) != 0;
		drawn[i] = (menu_item){.text = _wcsdup(full), .destructive = d};
		owner |= d;
	}
	if (owner) {
		b->owned = wh_realloc(b->owned, (b->nowned + 1) * sizeof *b->owned);
		b->owned[b->nowned++] = owner_draw(pop, drawn, n, b->order);
	}
	for (uint32_t i = 0; i < n; i++) free(drawn[i].text);
	free(drawn);
	wh_free_strings(names, nn);
	return pop;
}

// ---- The menu bar ----

static mitem *bar_level(uint32_t first, uint32_t count, uint32_t *used);

// bar_menu is a menu bar menu's items, as items to build.
static mitem *bar_level(uint32_t at, uint32_t count, uint32_t *next) {
	mitem *v = wh_alloc((count ? count : 1) * sizeof *v);
	uint32_t i = at;
	for (uint32_t k = 0; k < count && i < bar.nitems; k++) {
		bar_item *b = &bar.items[i];
		v[k].title = b->title;
		memcpy(v[k].key, b->key, sizeof v[k].key);
		v[k].mods = b->mods, v[k].role = b->role, v[k].flags = b->flags, v[k].index = b->index;
		i++;
		if (b->flags & AK_MENU_SUBMENU) {
			v[k].count = b->count;
			v[k].items = bar_level(i, b->count, &i);
		}
	}
	*next = i;
	return v;
}

static void free_level(mitem *v, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) free_level(v[i].items, v[i].count);
	free(v);
}

static HMENU new_menu_bar(void) {
	UINT order = H.rtl ? MFT_RIGHTORDER : 0;
	HMENU m = CreateMenu();
	wchar_t **titles = wh_alloc((bar.ntitles ? bar.ntitles : 1) * sizeof *titles);
	for (uint32_t i = 0; i < bar.ntitles; i++) titles[i] = _wcsdup(bar.titles[i]);
	access_keys(titles, bar.ntitles);
	builder b = {.base = MENU_BASE, .order = order, .shortcuts = true};
	for (uint32_t i = 0; i < bar.ntitles; i++) {
		// The menu's own items: those of depth 0 in its range.
		uint32_t count = 0;
		for (uint32_t j = bar.first[i]; j < bar.first[i] + bar.total[i]; j++)
			if (bar.items[j].depth == 0) count++;
		uint32_t next;
		mitem *items = bar_level(bar.first[i], count, &next);
		AppendMenuW(m, MF_POPUP | order, (UINT_PTR)popup(&b, items, count), titles[i]);
		free_level(items, count);
	}
	bar_owned = wh_realloc(bar_owned, (nbar_owned + b.nowned + 1) * sizeof *bar_owned);
	for (uint32_t i = 0; i < b.nowned; i++) bar_owned[nbar_owned++] = b.owned[i];
	free(b.owned);
	wh_free_strings(titles, bar.ntitles);
	return m;
}

void wh_set_window_menu(wh_wctl *c) {
	if (c->main || !c->hwnd || !(c->cfg.flags & AK_WINDOW_MENU_BAR)) return;
	HMENU m = bar.ntitles && !c->hidden ? new_menu_bar() : NULL;
	HMENU old = GetMenu(c->hwnd);
	SetMenu(c->hwnd, m);
	if (old) DestroyMenu(old);
	DrawMenuBar(c->hwnd);
}

// wh_build_menu_bar makes the menu bar and the accelerators from the
// menus, in the app's direction.
void wh_build_menu_bar(void) {
	for (uint32_t i = 0; i < nbar_owned; i++) forget(bar_owned[i]);
	nbar_owned = 0;
	HMENU m = NULL;
	// A hidden title bar's main window has no menu bar (Windows would draw
	// it under a caption that isn't there), nor has a panel.
	if (bar.ntitles && (!H.main_ctl || (!H.main_ctl->hidden && !wh_frameless(H.main_ctl)))) m = new_menu_bar();
	// A menu bar coming or going before the window shows keeps its client
	// size, and the window centered.
	RECT before, after;
	GetClientRect(H.window, &before);
	HMENU old = GetMenu(H.window);
	SetMenu(H.window, m);
	if (old) DestroyMenu(old);
	DrawMenuBar(H.window);
	if (!IsWindowVisible(H.window)) {
		GetClientRect(H.window, &after);
		int dh = before.bottom - after.bottom;
		if (dh) {
			RECT wr;
			GetWindowRect(H.window, &wr);
			SetWindowPos(H.window, NULL, wr.left, wr.top - dh / 2, wr.right - wr.left, wr.bottom - wr.top + dh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
		}
	}
	// The Windows that show the menu bar too (a document's).
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.ctls, &i, NULL, &v)) wh_set_window_menu(v);
	if (bar.accel) DestroyAcceleratorTable(bar.accel);
	bar.accel = NULL;
	ACCEL *accels = wh_alloc((2 * bar.nitems + 1) * sizeof *accels);
	int na = 0;
	for (uint32_t j = 0; j < bar.nitems; j++) {
		bar_item *it = &bar.items[j];
		// An Undo or Redo item's is enabled as the keyboard's field or the
		// app's stack says (WM_INITMENUPOPUP, which accelerators send too).
		uint8_t off = undo_role(it->role) ? 0 : AK_MENU_DISABLED;
		if (it->flags & (AK_MENU_SEPARATOR | off | AK_MENU_SUBMENU)) continue;
		const char *key = it->key;
		uint8_t mods = it->mods;
		if (!key[0] && it->role == AK_MENU_ROLE_CLOSE) key = "w", mods = KM_PRIMARY;
		bool own = key[0] != 0;
		key = undo_key(it->role, key, &mods);
		if (wh_accelerator(key, mods, &accels[na])) accels[na++].cmd = (WORD)(MENU_BASE + it->index);
		if (!own && it->role == AK_MENU_ROLE_REDO && wh_accelerator("z", KM_PRIMARY | KM_SHIFT, &accels[na])) accels[na++].cmd = (WORD)(MENU_BASE + it->index);
	}
	if (na) bar.accel = CreateAcceleratorTableW(accels, na);
	free(accels);
}

// wh_set_menu_bar takes the MenuBar command's menus and makes the menu
// bar and the accelerators again.
void wh_set_menu_bar(const uint8_t *b, size_t len) {
	for (uint32_t i = 0; i < bar.nitems; i++) free(bar.items[i].title);
	free(bar.items);
	wh_free_strings(bar.titles, bar.ntitles);
	free(bar.first);
	free(bar.total);
	bar.items = NULL, bar.nitems = 0, bar.titles = NULL, bar.ntitles = 0, bar.first = bar.total = NULL;
	ak_menu_title_vec menus = {0};
	ak_menu_entry_vec items = {0};
	ak_decode_menu_bar(b, len, &menus, &items);
	bar.ntitles = menus.len;
	bar.titles = wh_alloc((menus.len ? menus.len : 1) * sizeof *bar.titles);
	bar.first = wh_alloc((menus.len ? menus.len : 1) * sizeof *bar.first);
	bar.total = wh_alloc((menus.len ? menus.len : 1) * sizeof *bar.total);
	for (uint32_t i = 0; i < menus.len; i++) {
		bar.titles[i] = wh_wspan(menus.data[i].title);
		bar.first[i] = menus.data[i].first, bar.total[i] = menus.data[i].total;
	}
	bar.nitems = items.len;
	bar.items = wh_alloc((items.len ? items.len : 1) * sizeof *bar.items);
	for (uint32_t i = 0; i < items.len; i++) {
		const ak_menu_entry *e = &items.data[i];
		bar_item *it = &bar.items[i];
		it->title = wh_wspan(e->title);
		size_t kl = e->key.len < sizeof it->key - 1 ? e->key.len : sizeof it->key - 1;
		memcpy(it->key, e->key.p, kl);
		it->key[kl] = 0;
		it->mods = e->mods, it->role = e->role, it->flags = e->flags;
		it->index = e->index, it->depth = e->depth, it->count = e->count, it->menu = e->menu;
	}
	ak_vec_free(&menus);
	ak_vec_free(&items);
	wh_build_menu_bar();
}

// wh_menu_command is a menu item chosen, clicked or by its accelerator,
// in one of the app's windows: Action on the root, unless it's off; a
// quit item quits.
bool wh_menu_command(int id) {
	int i = id - MENU_BASE;
	if (i < 0 || (uint32_t)i >= bar.nitems) return false;
	bar_item *it = &bar.items[i];
	if (undo_role(it->role) && !(it->flags & (AK_MENU_SEPARATOR | AK_MENU_SUBMENU))) {
		wh_undo_chosen(it->role == AK_MENU_ROLE_REDO, it->index);  // a field's typing, or the app's change
		return true;
	}
	if (it->flags & (AK_MENU_SEPARATOR | AK_MENU_DISABLED | AK_MENU_SUBMENU)) return true;
	if (it->role == AK_MENU_ROLE_QUIT) wh_quit(false);
	else if (it->role == AK_MENU_ROLE_CLOSE) PostMessageW(wh_window_in_use(), WM_CLOSE, 0, 0);  // as its close button
	else if (H.root) wh_emit_int(H.root, AK_EVENT_ACTION, it->index);
	return true;
}

bool wh_accelerate(HWND win, MSG *m) { return bar.accel && TranslateAcceleratorW(win, bar.accel, m); }

// For the automation: a menu bar's item, chosen by its title.
bool wh_choose_menu_item(const wchar_t *title) {
	for (uint32_t i = 0; i < bar.nitems; i++) {
		wchar_t buf[256];
		const wchar_t *t = bar.items[i].title;
		if (!t[0] && undo_role(bar.items[i].role)) t = wh_undo_title(bar.items[i].role == AK_MENU_ROLE_REDO, buf, WH_LEN(buf));
		if (!wcscmp(t, title) && !(bar.items[i].flags & AK_MENU_SUBMENU)) {
			SendMessageW(H.window, WM_COMMAND, (WPARAM)(MENU_BASE + bar.items[i].index), 0);
			return true;
		}
	}
	return false;
}

// refresh_undo says what a menu's Undo and Redo items do now, as it opens
// (or an accelerator checks it): their names, enabled or not.
static void refresh_undo(HMENU m) {
	int count = GetMenuItemCount(m);
	for (int i = 0; i < count; i++) {
		MENUITEMINFOW mi = {sizeof mi, MIIM_ID | MIIM_FTYPE | MIIM_DATA};
		if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mi)) continue;
		int k = (int)mi.wID - MENU_BASE;
		if (k < 0 || (uint32_t)k >= bar.nitems) continue;
		bar_item *it = &bar.items[k];
		if (!undo_role(it->role) || (it->flags & AK_MENU_SUBMENU)) continue;
		bool redo = it->role == AK_MENU_ROLE_REDO;
		wchar_t buf[256], sc[64], full[600];
		const wchar_t *title = it->title[0] ? it->title : wh_undo_title(redo, buf, WH_LEN(buf));
		uint8_t mods = it->mods;
		const char *key = undo_key(it->role, it->key, &mods);
		shortcut_text(sc, WH_LEN(sc), key, mods);
		wchar_t *esc = dup_escaped(title);
		swprintf(full, WH_LEN(full), sc[0] ? L"&%ls\t%ls" : L"&%ls", esc, sc);
		free(esc);
		MENUITEMINFOW set = {sizeof set, MIIM_STATE};
		set.fState = wh_undo_enabled(redo) ? MFS_ENABLED : MFS_GRAYED;
		if (mi.fType & MFT_OWNERDRAW) {
			owner_item *o = wh_map_get(&owned, mi.dwItemData);
			if (o) {
				free(o->text);
				o->text = _wcsdup(full);
				wchar_t *tab = wcschr(o->text, '\t');
				if (tab) *tab = 0;
			}
		} else {
			set.fMask |= MIIM_STRING;
			set.dwTypeData = full;
		}
		SetMenuItemInfoW(m, (UINT)i, TRUE, &set);
	}
}

static void menu_loop(bool enter);

typedef struct {
	int index;
	RECT r;
} open_job;

static void open_now(void *ctx) {
	open_job *j = ctx;
	HMENU m = GetMenu(H.window);
	if (m) {
		menu_loop(true);
		int got = TrackPopupMenu(GetSubMenu(m, j->index), TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, j->r.left, j->r.bottom, 0, H.window, NULL);
		menu_loop(false);
		if (got > 0) wh_menu_command(got);
	}
	free(j);
}

// For the automation: a menu bar's menu, below its title, as clicking it
// shows it.
bool wh_open_bar_menu(const wchar_t *title) {
	HMENU m = GetMenu(H.window);
	for (uint32_t i = 0; i < bar.ntitles && m; i++) {
		if (wcscmp(bar.titles[i], title) != 0) continue;
		open_job *j = wh_alloc(sizeof *j);
		j->index = (int)i;
		GetMenuItemRect(H.window, m, i, &j->r);
		wh_after_core(open_now, j);
		return true;
	}
	return false;
}

// ---- The dark menu bar: Windows draws it light whatever the theme, so
// the window draws it (uxtheme's undocumented UAH messages, as Notepad++
// and MyGo do) ----

#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
#define DARK_BAR 0x202020
#define DARK_HOT 0x2D2D2D
#define DARK_OPEN 0x333333
#define DARK_TEXT 0xFFFFFF
#define DARK_DISABLED 0x6D6D6D

typedef struct {
	HMENU menu;
	HDC dc;
	DWORD flags;
} uah_menu;

typedef struct {
	DRAWITEMSTRUCT draw;
	uah_menu menu;
	int position;
} uah_draw_item;

static bool menu_bar_rect(HWND hwnd, RECT *out) {
	MENUBARINFO mbi = {sizeof mbi};
	if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return false;
	RECT wr;
	GetWindowRect(hwnd, &wr);
	*out = mbi.rcBar;
	OffsetRect(out, -wr.left, -wr.top);
	return true;
}

static void fill(HDC dc, RECT r, COLORREF c) {
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &r, b);
	DeleteObject(b);
}

static bool mirrored_dc(HDC dc) { return (GetLayout(dc) & LAYOUT_RTL) != 0; }

static void draw_bar_item(uah_draw_item *d) {
	HDC dc = d->draw.hDC;
	RECT r = d->draw.rcItem;
	UINT state = d->draw.itemState;
	COLORREF plate = DARK_BAR, fore = DARK_TEXT;
	if (state & (ODS_GRAYED | ODS_DISABLED)) fore = DARK_DISABLED;
	else if (state & ODS_SELECTED) plate = DARK_OPEN;
	else if (state & ODS_HOTLIGHT) plate = DARK_HOT;
	fill(dc, r, DARK_BAR);
	if (plate != DARK_BAR) {
		float k = H.scale;
		if (wh_windows11() && mirrored_dc(dc)) {
			// A mirrored window's DC: GDI+ doesn't follow it.
			int in = (int)(2 * k + 0.5f), rr = (int)(4 * k + 0.5f) * 2;
			HBRUSH b = CreateSolidBrush(plate);
			HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
			RoundRect(dc, r.left + in, r.top + in, r.right - in + 1, r.bottom - in + 1, rr, rr);
			SelectObject(dc, ob);
			SelectObject(dc, op);
			DeleteObject(b);
		} else if (wh_windows11()) {
			gfx g = g_new(dc);
			ARGB c = 0xFF000000u | (ARGB)GetRValue(plate) << 16 | (ARGB)GetGValue(plate) << 8 | GetBValue(plate);
			g_fill_round_rect(g, c, (float)r.left + 2 * k, (float)r.top + 2 * k, (float)(r.right - r.left) - 4 * k, (float)(r.bottom - r.top) - 4 * k, 4 * k);
			g_close(g);
		} else {
			fill(dc, r, plate);
		}
	}
	UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE | wh_text_direction(H.rtl);
	if (state & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, fore);
	wchar_t s[256] = {0};
	GetMenuStringW(d->menu.menu, (UINT)d->position, s, 256, MF_BYPOSITION);
	DrawTextW(dc, s, -1, &r, flags);
}

LRESULT wh_menu_bar_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = false;
	if (m != WM_UAHDRAWMENU && m != WM_UAHDRAWMENUITEM && m != WM_NCPAINT && m != WM_NCACTIVATE) return 0;
	if (!H.dark || !GetMenu(hwnd)) return 0;
	*handled = true;
	RECT r;
	if (m == WM_UAHDRAWMENU) {
		uah_menu *um = (uah_menu *)lp;
		if (menu_bar_rect(hwnd, &r)) fill(um->dc, r, DARK_BAR);
		return 0;
	}
	if (m == WM_UAHDRAWMENUITEM) {
		draw_bar_item((uah_draw_item *)lp);
		return 0;
	}
	LRESULT res = DefWindowProcW(hwnd, m, wp, lp);
	if (menu_bar_rect(hwnd, &r)) {
		HDC dc = GetWindowDC(hwnd);
		if (dc) {
			// Over the light line the frame draws under the bar.
			fill(dc, (RECT){r.left, r.bottom, r.right, r.bottom + 1}, DARK_BAR);
			ReleaseDC(hwnd, dc);
		}
	}
	return res;
}

// ---- Owner-drawn items, drawn as the system draws a themed menu's ----

typedef struct {
	HTHEME theme, sep_theme, check_theme, alt;
	HFONT font;
	float k;
	SIZE check;
	MARGINS check_m, check_bg_m, item_m, arrow_m;
	SIZE arrow;
	int sep;
	COLORREF text, disabled;
	int dpi;
	bool dark;
} menu_style;

static menu_style *styles;
static uint32_t nstyles;

static MARGINS margins(HTHEME t, int part, int prop) {
	MARGINS m = {0};
	GetThemeMargins(t, H.measure_dc, part, 0, prop, NULL, &m);
	return m;
}

static menu_style *style(HWND hwnd, int dpi) {
	for (uint32_t i = 0; i < nstyles; i++)
		if (styles[i].dpi == dpi && styles[i].dark == H.dark) return &styles[i];
	styles = wh_realloc(styles, (nstyles + 1) * sizeof *styles);
	menu_style *s = &styles[nstyles++];
	memset(s, 0, sizeof *s);
	s->dpi = dpi, s->dark = H.dark, s->k = (float)dpi / 96;
	const wchar_t *cls = L"Menu";
	if (wh_windows11() && H.dark) cls = L"DarkMode_ImmersiveStart::Menu";
	else if (wh_windows11()) cls = L"ImmersiveStart::Menu";
	else if (H.dark) cls = L"DarkMode::Menu";
	s->theme = wh_open_theme(hwnd, cls, dpi);
	if (!s->theme) s->theme = wh_open_theme(hwnd, L"Menu", dpi);
	// As the system's menus on Windows 11 are: in light, the separator is
	// the plain Menu class's; in dark, the check mark and the disabled
	// gray are DarkMode::Menu's.
	s->sep_theme = s->check_theme = s->theme;
	if (wh_windows11()) {
		HTHEME t = wh_open_theme(hwnd, H.dark ? L"DarkMode::Menu" : L"Menu", dpi);
		if (t) {
			s->alt = t;
			if (H.dark) s->check_theme = t;
			else s->sep_theme = t;
		}
	}
	NONCLIENTMETRICSW ncm = {sizeof ncm};
	SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, (UINT)dpi);
	s->font = CreateFontIndirectW(&ncm.lfMenuFont);
	HDC dc = H.measure_dc;
	GetThemePartSize(s->theme, dc, MENU_POPUPCHECK, MC_CHECKMARKNORMAL, NULL, TS_TRUE, &s->check);
	s->check_m = margins(s->theme, MENU_POPUPCHECK, TMT_CONTENTMARGINS);
	s->check_bg_m = margins(s->theme, MENU_POPUPCHECKBACKGROUND, TMT_CONTENTMARGINS);
	s->item_m = margins(s->theme, MENU_POPUPITEM, TMT_CONTENTMARGINS);
	GetThemePartSize(s->theme, dc, MENU_POPUPSUBMENU, 0, NULL, TS_TRUE, &s->arrow);
	s->arrow_m = margins(s->theme, MENU_POPUPSUBMENU, TMT_CONTENTMARGINS);
	SIZE sep = {0};
	GetThemePartSize(s->theme, dc, MENU_POPUPSEPARATOR, 0, NULL, TS_TRUE, &sep);
	s->sep = sep.cy;
	if (FAILED(GetThemeColor(s->theme, MENU_POPUPITEM, MPI_NORMAL, TMT_TEXTCOLOR, &s->text))) s->text = GetSysColor(COLOR_MENUTEXT);
	if (FAILED(GetThemeColor(s->check_theme, MENU_POPUPITEM, MPI_DISABLED, TMT_TEXTCOLOR, &s->disabled))) s->disabled = GetSysColor(COLOR_GRAYTEXT);
	return s;
}

void wh_clear_menu_styles(void) {
	for (uint32_t i = 0; i < nstyles; i++) {
		if (styles[i].theme) CloseThemeData(styles[i].theme);
		if (styles[i].alt) CloseThemeData(styles[i].alt);
		DeleteObject(styles[i].font);
	}
	free(styles);
	styles = NULL;
	nstyles = 0;
}

// The metrics, in pixels, as the system lays out a themed menu's items.
static int spx(const menu_style *s, int v) { return (int)((float)v * s->k + 0.5f); }
static int inset(const menu_style *s) { return spx(s, 3); }
static int check_left(const menu_style *s) { return inset(s) + spx(s, s->check_m.cxLeftWidth); }
static int item_height(const menu_style *s) { return s->check.cy + spx(s, s->check_m.cyTopHeight + s->check_m.cyBottomHeight); }
static int sep_height(const menu_style *s) { return s->sep + spx(s, s->item_m.cxLeftWidth + s->item_m.cxRightWidth); }
static int right_col(const menu_style *s) { return inset(s) + spx(s, s->arrow_m.cxLeftWidth) + s->arrow.cx + spx(s, s->arrow_m.cxRightWidth); }
static int text_left(const menu_style *s) {
	return check_left(s) + s->check.cx + spx(s, s->check_m.cxRightWidth) + spx(s, s->check_bg_m.cxRightWidth) + spx(s, s->item_m.cxLeftWidth);
}

// strip_access is a title as it shows: & marks dropped, && one &.
static int strip_access(const wchar_t *s, wchar_t *out) {
	int k = 0;
	for (int i = 0; s[i]; i++) {
		if (s[i] == '&') {
			if (s[i + 1] == '&') i++;
			else continue;
		}
		out[k++] = s[i];
	}
	out[k] = 0;
	return k;
}

static rgba menu_background(void) { return H.dark ? rgba_hex(0x2B2B2B) : rgba_hex(0xF9F9F9); }

static void menu_focus(HDC dc, RECT r, float k) {
	BYTE c = H.dark ? 255 : 0;
	float w = wh_maxf(1, (float)(int)(k / 2 + 0.5f));
	if (mirrored_dc(dc)) {
		HPEN pen = CreatePen(PS_SOLID, (int)w, RGB(c, c, c));
		HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
		int rr = (int)(1.75f * k + 0.5f) * 2;
		RoundRect(dc, r.left, r.top, r.right, r.bottom, rr, rr);
		SelectObject(dc, op);
		SelectObject(dc, ob);
		DeleteObject(pen);
		return;
	}
	gfx g = g_new(dc);
	g_stroke_round_rect(g, 0xFF000000u | (ARGB)c << 16 | (ARGB)c << 8 | c, w, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), 1.75f * k);
	g_close(g);
}

static void draw_item(const menu_style *s, DRAWITEMSTRUCT *d, owner_item *it) {
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	UINT state = d->itemState;
	fill(dc, r, rgba_ref(menu_background()));
	if (it->separator) {
		int top = r.top + ((r.bottom - r.top) - s->sep) / 2;
		RECT sr = {r.left + spx(s, 6), top, r.right - spx(s, 6), top + s->sep};
		DrawThemeBackground(s->sep_theme, dc, MENU_POPUPSEPARATOR, 0, &sr, NULL);
		return;
	}
	bool disabled = (state & (ODS_GRAYED | ODS_DISABLED)) != 0;
	RECT sel = {r.left + inset(s), r.top, r.right - inset(s), r.bottom};
	DrawThemeBackground(s->theme, dc, MENU_POPUPITEM, MPI_NORMAL, &sel, NULL);
	if ((state & ODS_SELECTED) && !it->header) {
		if (state & ODS_NOFOCUSRECT) DrawThemeBackground(s->theme, dc, MENU_POPUPITEM, disabled ? MPI_DISABLEDHOT : MPI_HOT, &sel, NULL);  // under the mouse
		else menu_focus(dc, sel, s->k);  // from the keyboard
	}
	if (state & ODS_CHECKED) {
		int top = r.top + ((r.bottom - r.top) - s->check.cy) / 2, left = r.left + check_left(s);
		RECT cr = {left, top, left + s->check.cx, top + s->check.cy};
		DrawThemeBackground(s->check_theme, dc, MENU_POPUPCHECK, disabled ? MC_CHECKMARKDISABLED : MC_CHECKMARKNORMAL, &cr, NULL);
	}
	COLORREF color = disabled ? s->disabled : s->text;
	HGDIOBJ old = SelectObject(dc, s->font);
	SetBkMode(dc, TRANSPARENT);
	// A destructive item's title is red, its shortcut as the others'.
	SetTextColor(dc, it->destructive && !disabled ? rgba_ref(H.theme.red) : color);
	bool rtl = mirrored_dc(dc);  // a mirrored menu's DC goes the other way by itself
	UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT | wh_text_direction(rtl);
	if (state & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
	RECT tr = {r.left + text_left(s), r.top, r.right - right_col(s), r.bottom};
	DrawTextW(dc, it->text, -1, &tr, flags);
	if (it->key[0]) {
		SetTextColor(dc, color);
		DrawTextW(dc, it->key, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX | wh_text_direction(rtl));
	}
	SelectObject(dc, old);
	if (it->submenu) {
		// The submenu's arrow, in its column, as the visual style draws
		// it; the item's rectangle kept from the system, which would draw
		// a classic arrow over it.
		int right = r.right - inset(s) - spx(s, s->arrow_m.cxRightWidth), top = r.top + ((r.bottom - r.top) - s->arrow.cy) / 2;
		RECT ar = {right - s->arrow.cx, top, right, top + s->arrow.cy};
		DrawThemeBackground(s->theme, dc, MENU_POPUPSUBMENU, disabled ? MSM_DISABLED : MSM_NORMAL, &ar, NULL);
		ExcludeClipRect(dc, r.left, r.top, r.right, r.bottom);
	}
}

LRESULT wh_menu_item_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = false;
	switch (m) {
	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
		owner_item *it = mi->CtlType == ODT_MENU ? wh_map_get(&owned, mi->itemData) : NULL;
		if (!it) return 0;
		*handled = true;
		int dpi = wh_dpi(hwnd);
		menu_style *s = style(hwnd, dpi);
		if (it->separator) {
			mi->itemHeight = (UINT)sep_height(s);
			return TRUE;
		}
		HDC dc = H.measure_dc;
		HGDIOBJ old = SelectObject(dc, s->font);
		int tw = 0, kw = 0;
		for (uint32_t i = 0; i < it->menu->n; i++) {
			owner_item *x = it->menu->items[i];
			if (x->separator) continue;
			wchar_t buf[600];
			int n = strip_access(x->text, buf);
			tw = wh_maxi(tw, wh_text_extent(dc, buf, n).cx);
			if (x->key[0]) kw = wh_maxi(kw, wh_text_extent(dc, x->key, (int)wcslen(x->key)).cx);
		}
		SelectObject(dc, old);
		int w = text_left(s) + tw + right_col(s);
		if (kw > 0) w += spx(s, 24) + kw;
		// The system adds the check mark's width to an owner-drawn item's
		// (less 3 pixels, as measured on Windows 11).
		mi->itemWidth = (UINT)wh_maxi(w - (wh_system_metric(SM_CXMENUCHECK, dpi) - 3), 0);
		mi->itemHeight = (UINT)item_height(s);
		return TRUE;
	}
	case WM_DRAWITEM: {
		DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
		owner_item *it = d->CtlType == ODT_MENU ? wh_map_get(&owned, d->itemData) : NULL;
		if (!it) return 0;
		*handled = true;
		draw_item(style(hwnd, wh_dpi(hwnd)), d, it);
		return TRUE;
	}
	case WM_INITMENUPOPUP:
		refresh_undo((HMENU)wp);
		// Its background, light or dark as the app is now.
		for (uint32_t i = 0; i < nowned_menus; i++)
			if (owned_menus[i]->menu == (HMENU)wp) {
				MENUINFO mi = {sizeof mi, MIM_BACKGROUND};
				mi.hbrBack = wh_brush(menu_background());
				SetMenuInfo((HMENU)wp, &mi);
			}
		return 0;
	case WM_INITMENU:
	case WM_ENTERMENULOOP:
	case WM_EXITMENULOOP:
		menu_loop(m != WM_EXITMENULOOP);
		return 0;
	case WM_MENUCHAR:
		// The items keep their text, so the system takes their access keys
		// itself; a key none takes is ignored, the menu staying open as a
		// themed one does.
		for (uint32_t i = 0; i < nowned_menus; i++)
			if (owned_menus[i]->menu == (HMENU)lp) {
				*handled = true;
				return MAKELRESULT(0, MNC_IGNORE);
			}
		return 0;
	}
	return 0;
}

// ---- Menu windows: the system draws an owner-drawn menu's window a
// classic frame, so it's drawn as Windows 11's menus are: the menu's
// background, with DWM's rounded corners and border ----

static HHOOK cbt;

static owner_menu *menu_of(HWND hwnd) {
	HMENU m = (HMENU)SendMessageW(hwnd, MN_GETHMENU, 0, 0);
	for (uint32_t i = 0; i < nowned_menus; i++)
		if (owned_menus[i]->menu == m) return owned_menus[i];
	return NULL;
}

static rgba flyout_border(void) { return H.dark ? rgba_hex(0x3E3E3E) : rgba_hex(0xE5E5E5); }

static void round_menu(HWND hwnd) {
	if (!wh_windows11()) return;
	DWORD pref = 3;  // DWMWCP_ROUNDSMALL
	DwmSetWindowAttribute(hwnd, 33, &pref, sizeof pref);
	COLORREF c = rgba_ref(flyout_border());
	DwmSetWindowAttribute(hwnd, 34, &c, sizeof c);  // DWMWA_BORDER_COLOR
}

static void paint_menu_frame(HWND hwnd, HDC dc) {
	RECT wr, cr;
	GetWindowRect(hwnd, &wr);
	GetClientRect(hwnd, &cr);
	int w = wr.right - wr.left, h = wr.bottom - wr.top;
	POINT p = {0, 0};
	ClientToScreen(hwnd, &p);
	if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) p.x -= cr.right;  // a mirrored window's client origin is its top right
	RECT c = {p.x - wr.left, p.y - wr.top, 0, 0};
	c.right = c.left + cr.right, c.bottom = c.top + cr.bottom;
	COLORREF bg = rgba_ref(menu_background());
	fill(dc, (RECT){0, 0, w, c.top}, bg);
	fill(dc, (RECT){0, c.bottom, w, h}, bg);
	fill(dc, (RECT){0, c.top, c.left, c.bottom}, bg);
	fill(dc, (RECT){c.right, c.top, w, c.bottom}, bg);
}

static LRESULT menu_window_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	switch (m) {
	case WM_NCPAINT:
	case WM_PRINT:
		if (menu_of(hwnd)) {
			if (m == WM_PRINT) {
				LRESULT r = wh_def_subclass(hwnd, m, wp, lp);
				paint_menu_frame(hwnd, (HDC)wp);
				return r;
			}
			round_menu(hwnd);
			HDC dc = GetWindowDC(hwnd);
			paint_menu_frame(hwnd, dc);
			ReleaseDC(hwnd, dc);
			return 0;
		}
		break;
	case WM_NCCREATE:
		round_menu(hwnd);
		break;
	case WM_NCDESTROY:
		wh_unsubclass(hwnd);
		break;
	}
	return wh_def_subclass(hwnd, m, wp, lp);
}

static LRESULT CALLBACK cbt_proc(int code, WPARAM wp, LPARAM lp) {
	if (code == HCBT_CREATEWND) {
		wchar_t cls[16];
		if (GetClassNameW((HWND)wp, cls, 16) && !wcscmp(cls, L"#32768")) wh_subclass((HWND)wp, menu_window_message);
	}
	return CallNextHookEx(cbt, code, wp, lp);
}

// menu_loop starts and ends following the menu windows the system makes
// while a menu is open.
static void menu_loop(bool enter) {
	if (!enter) {
		if (cbt) UnhookWindowsHookEx(cbt);
		cbt = NULL;
		return;
	}
	if (nowned_menus && !cbt) cbt = SetWindowsHookExW(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
}

// ---- Menus of actions: a view's Menu, a bar's buttons ----

// action_items are a menu's items for actions: a section's set off by
// separators, under a header with its title; none at the ends or doubled.
static void add_actions(mitem **out, uint32_t *n, uint32_t *cap, const action *as, uint32_t count);

static void push_item(mitem **out, uint32_t *n, uint32_t *cap, mitem it) {
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 8;
		*out = wh_realloc(*out, *cap * sizeof **out);
	}
	(*out)[(*n)++] = it;
}

static mitem *action_items(const action *as, uint32_t count, uint32_t *outn) {
	mitem *out = NULL;
	uint32_t n = 0, cap = 0;
	add_actions(&out, &n, &cap, as, count);
	// No separator at the ends, nor one after another.
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		if ((out[i].flags & AK_MENU_SEPARATOR) && (k == 0 || (out[k - 1].flags & AK_MENU_SEPARATOR))) continue;
		out[k++] = out[i];
	}
	if (k && (out[k - 1].flags & AK_MENU_SEPARATOR)) k--;
	*outn = k;
	return out;
}

static void add_actions(mitem **out, uint32_t *n, uint32_t *cap, const action *as, uint32_t count) {
	for (uint32_t i = 0; i < count; i++) {
		const action *a = &as[i];
		if (a->role == AK_ALERT_BUTTON_ROLE_CANCEL) continue;
		if ((a->flags & (AK_MENU_SUBMENU | AK_MENU_INLINE)) == (AK_MENU_SUBMENU | AK_MENU_INLINE)) {
			push_item(out, n, cap, (mitem){.title = L"", .flags = AK_MENU_SEPARATOR, .index = -1});
			if (a->title[0]) push_item(out, n, cap, (mitem){.title = a->title, .flags = MENU_HEADER | AK_MENU_DISABLED, .index = -1});
			add_actions(out, n, cap, a->items, a->count);
			push_item(out, n, cap, (mitem){.title = L"", .flags = AK_MENU_SEPARATOR, .index = -1});
			continue;
		}
		mitem it = {.title = a->title, .index = a->index, .flags = a->flags & (AK_MENU_DISABLED | AK_MENU_CHECKED)};
		if (a->role == AK_ALERT_BUTTON_ROLE_DESTRUCTIVE) it.flags |= AK_MENU_DESTRUCTIVE;
		if (a->flags & AK_MENU_SUBMENU) {
			it.flags |= AK_MENU_SUBMENU;
			it.items = action_items(a->items, a->count, &it.count);
			if (!it.count) it.flags |= AK_MENU_DISABLED;  // nothing to open
		}
		push_item(out, n, cap, it);
	}
}

static void free_action_items(mitem *v, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) free_action_items(v[i].items, v[i].count);
	free(v);
}

// action_menu is a popup menu of actions (but the cancel one), the command
// of each item 1 + its number; the owner-drawn ones are returned, to
// forget once the menu's gone.
static HMENU action_menu(const actions *a, bool rtl, builder *b) {
	uint32_t n;
	mitem *items = action_items(a->v, a->n, &n);
	b->base = 1, b->order = rtl ? MFT_RIGHTORDER : 0;
	HMENU m = popup(b, items, n);
	free_action_items(items, n);
	return m;
}

// track runs a popup menu of actions at p, reporting the item chosen (its
// number), or -1.
int64_t wh_track_actions(const actions *a, bool rtl, POINT p, HWND owner) {
	builder b = {0};
	HMENU m = action_menu(a, rtl, &b);
	UINT flags = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
	if (rtl) flags |= TPM_LAYOUTRTL;  // from the point leftward, itself mirrored
	H.modal++;
	if (b.nowned) menu_loop(true);  // before the menu window is made
	int got = TrackPopupMenu(m, flags, p.x, p.y, 0, owner, NULL);
	menu_loop(false);
	H.modal--;
	DestroyMenu(m);
	for (uint32_t i = 0; i < b.nowned; i++) forget(b.owned[i]);
	free(b.owned);
	return got > 0 ? got - 1 : -1;
}

typedef struct {
	uint32_t id, at;
	HWND anchor;
	bool below;
	int64_t base;
	actions a;  // a copy, for a target's own menus
	bool own;   // the target's Menu prop
} pop_job;

static void pop_now(void *ctx) {
	pop_job *j = ctx;
	node *n = wh_node(j->id), *at = wh_node(j->at);
	const actions *a = j->own ? (n ? &n->menu : NULL) : &j->a;
	if (n && at && a && a->n && j->anchor && IsWindow(j->anchor)) {
		bool rtl = wh_is_rtl(at);
		POINT p;
		GetCursorPos(&p);
		if (j->below) {
			RECT r;
			GetWindowRect(j->anchor, &r);
			p = (POINT){rtl ? r.right : r.left, r.bottom + wh_px(at, 4)};
		}
		int64_t got = wh_track_actions(a, rtl, p, GetAncestor(j->anchor, GA_ROOT));
		if (got >= 0 && (n = wh_node(j->id))) wh_emit_int(n, AK_EVENT_ACTION, j->base + got);
	}
	if (!j->own) actions_free(&j->a);
	free(j);
}

// wh_pop_up shows a view's menu: below the view (a button's), or at the
// mouse (a context menu). The menu runs a modal loop, so it shows once the
// core has returned; the item chosen is reported with Action.
void wh_pop_up(node *n, HWND anchor, bool below) {
	pop_job *j = wh_alloc(sizeof *j);
	j->id = j->at = n->id, j->anchor = anchor, j->below = below, j->own = true;
	wh_after_core(pop_now, j);
}

static void copy_actions(actions *dst, const actions *src);

static action *copy_level(const action *v, uint32_t n) {
	action *out = wh_alloc((n ? n : 1) * sizeof *out);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = v[i];
		out[i].title = _wcsdup(v[i].title);
		out[i].icon = wh_strdup(v[i].icon);
		out[i].items = copy_level(v[i].items, v[i].count);
	}
	return out;
}

static void copy_actions(actions *dst, const actions *src) {
	dst->v = copy_level(src->v, src->n);
	dst->n = src->n;
}

// wh_pop_up_actions shows actions as a menu below anchor, a control of
// the node at (its direction's); the item chosen is reported to target
// with Action, base + its number. It shows once the core has returned.
void wh_pop_up_actions(node *target, HWND anchor, node *at, const actions *a, int64_t base) {
	pop_job *j = wh_alloc(sizeof *j);
	j->id = target->id, j->at = at->id, j->anchor = anchor, j->below = true, j->base = base;
	copy_actions(&j->a, a);
	wh_after_core(pop_now, j);
}

// ---- Menus of ops/menu.go's items: a tray icon's ----

// entry_level is a menu's items, from its decoded entries (depth first),
// as items to build; their titles go in *titles, to free after.
static mitem *entry_level(const ak_menu_entry_vec *e, uint32_t at, uint32_t count, uint32_t *next, wchar_t ***titles, uint32_t *ntitles) {
	mitem *v = wh_alloc((count ? count : 1) * sizeof *v);
	uint32_t i = at;
	for (uint32_t k = 0; k < count && i < e->len; k++) {
		const ak_menu_entry *it = &e->data[i];
		wchar_t *t = wh_wspan(it->title);
		*titles = wh_realloc(*titles, (*ntitles + 1) * sizeof **titles);
		(*titles)[(*ntitles)++] = t;
		v[k].title = t;
		size_t kl = it->key.len < sizeof v[k].key - 1 ? it->key.len : sizeof v[k].key - 1;
		memcpy(v[k].key, it->key.p, kl);
		v[k].key[kl] = 0;
		v[k].mods = it->mods, v[k].role = it->role, v[k].flags = it->flags, v[k].index = it->index;
		i++;
		if (it->flags & AK_MENU_SUBMENU) {
			v[k].count = it->count;
			v[k].items = entry_level(e, i, it->count, &i, titles, ntitles);
		}
	}
	*next = i;
	return v;
}

// wh_track_menu runs a popup menu of a menu's items (ops/menu.go's, as
// the tray's icons have them) at p, owned by owner, as the system's tray
// menus open: above and from the point (leftward, right to left). It
// reports the item chosen (its number), or -1.
int64_t wh_track_menu(const uint8_t *menu, size_t len, POINT p, HWND owner) {
	ak_menu_entry_vec e = {0};
	if (!ak_decode_menu(menu, len, &e) || !e.len) {
		ak_vec_free(&e);
		return -1;
	}
	uint32_t count = 0;
	for (uint32_t i = 0; i < e.len; i++)
		if (e.data[i].depth == 0) count++;
	wchar_t **titles = NULL;
	uint32_t ntitles = 0, next;
	mitem *items = entry_level(&e, 0, count, &next, &titles, &ntitles);
	builder b = {.base = 1, .order = H.rtl ? MFT_RIGHTORDER : 0};
	HMENU m = popup(&b, items, count);
	free_level(items, count);
	wh_free_strings(titles, ntitles);
	ak_vec_free(&e);
	UINT flags = TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD;
	if (H.rtl) flags |= TPM_RIGHTALIGN | TPM_LAYOUTRTL;
	// The menu closes when the user clicks away only if its owner is the
	// foreground window.
	SetForegroundWindow(owner);
	H.modal++;
	if (b.nowned) menu_loop(true);
	int got = TrackPopupMenu(m, flags, p.x, p.y, 0, owner, NULL);
	menu_loop(false);
	H.modal--;
	PostMessageW(owner, WM_NULL, 0, 0);
	DestroyMenu(m);
	for (uint32_t i = 0; i < b.nowned; i++) forget(b.owned[i]);
	free(b.owned);
	return got > 0 ? got - 1 : -1;
}
