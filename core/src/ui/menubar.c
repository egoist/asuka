// menubar.c: the app's menu bar: MenuBar, the
// Menus in it, their MenuItems (MenuItems in an item make a submenu)
// and MenuSeparators. None makes a view: a MenuBar builds its children
// under an owner of its own, outside the tree, and each item it holds is
// marked as its (ak_view's bar), so a change to one, or to what a for or
// an if in it shows, syncs it at the next flush. The menus of the last
// MenuBar shown are the app's: they go to the root as one MenuBar
// command (ops/menu.go's layout), only when their encoding changed; when
// it goes, the one before's do. A chosen item comes back as Action on
// the root with its number among the menus' items, depth first (a
// submenu's after the item that opens it; separators and submenus
// count).
#include "ui/ui_ext.h"

static const char menubar_key[] = "menubar";

typedef struct ak_menus {
	ak_view_vec bars;   // the MenuBars shown, in the order they came (retained)
	ak_view_vec items;  // the items sent, by number: MenuItems, NULL for the rest (retained)
	ak_buf sent, buf;
	bool has_sent;  // menus were sent (none, after, says so)
	bool listens;
} ak_menus;

typedef struct {
	ak_closure *build;
	ak_owner *owner;   // its content's
	ak_view *content;  // what it built (retained): its Menus
} bar_t;

static const ak_el_class cls_bar_menu, cls_item, cls_sep;

static ak_menus *menus_of(ak_ui_root *r) {
	if (!r->menus) r->menus = ak_alloc(sizeof(ak_menus));
	return r->menus;
}

static void release_items(ak_menus *m) {
	for (uint32_t i = 0; i < m->items.len; i++) ak_release(m->items.data[i]);
	m->items.len = 0;
}

void ak_menus_free(ak_ui_root *r) {
	ak_menus *m = r->menus;
	if (!m) return;
	r->menus = NULL;
	release_items(m);
	ak_vec_free(&m->items);
	for (uint32_t i = 0; i < m->bars.len; i++) ak_release(m->bars.data[i]);
	ak_vec_free(&m->bars);
	ak_buf_free(&m->sent);
	ak_buf_free(&m->buf);
	ak_free(m);
}

// ---- Encoding ----

static const ak_val *key_of(const ak_view *e, uint16_t key) { return ak_style_get(e, key); }

static void put_str(ak_buf *b, const ak_val *v) {
	const ak_str *s = v ? v->p : NULL;
	ak_buf_str(b, s ? s->data : "", s ? s->len : 0);
}

static void patch_u32(ak_buf *b, size_t at, uint32_t n) {
	uint8_t le[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
	memcpy(b->data + at, le, 4);
}

// The item kinds a menu holds, through the fragments (for, if) in it.
static bool is_entry(const ak_view *v) { return v->cls == &cls_item || v->cls == &cls_sep; }

static bool has_entries(ak_view *v) {
	for (uint32_t i = 0; i < v->kids.len; i++) {
		ak_view *k = v->kids.data[i];
		if (is_entry(k) || (k->frag && k->cls != &cls_bar_menu && has_entries(k))) return true;
	}
	return false;
}

static void write_menu(ak_menus *m, ak_view *bar, ak_view *parent);

// MenuRole's cases (types.asuka): normal, destructive, quit, close,
// openRecent, undo, redo; the wire's roles are the last five's.
enum { ROLE_NORMAL, ROLE_DESTRUCTIVE, ROLE_QUIT, ROLE_CLOSE, ROLE_OPEN_RECENT, ROLE_UNDO, ROLE_REDO };

static void write_item(ak_menus *m, ak_view *bar, ak_view *v) {
	ak_buf *b = &m->buf;
	ak_vec_push(&m->items, (ak_view *)ak_retain(v));
	if (v->cls == &cls_sep) {
		ak_buf_u8(b, AK_MENU_SEPARATOR);
		ak_buf_str(b, "", 0);
		ak_buf_str(b, "", 0);
		ak_buf_u8(b, 0);
		ak_buf_u8(b, 0);
		return;
	}
	const ak_val *d = key_of(v, AK_KEY_MENU_ITEM_DISABLED), *c = key_of(v, AK_KEY_MENU_ITEM_CHECKED),
	             *role = key_of(v, AK_KEY_MENU_ITEM_ROLE), *sc = key_of(v, AK_KEY_MENU_ITEM_SHORTCUT);
	int64_t ro = role ? role->i : ROLE_NORMAL;
	bool sub = has_entries(v);
	uint8_t flags = 0;
	if (d && d->i) flags |= AK_MENU_DISABLED;
	if (c && c->i) flags |= AK_MENU_CHECKED;
	if (ro == ROLE_DESTRUCTIVE) flags |= AK_MENU_DESTRUCTIVE;
	// The undo stack's: enabled while there's something to undo (the host
	// enables it for a text field being edited too).
	if ((ro == ROLE_UNDO || ro == ROLE_REDO) && !ak_undo_peek(bar->r, ro == ROLE_REDO)) flags |= AK_MENU_DISABLED;
	if (sub) flags |= AK_MENU_SUBMENU;
	ak_buf_u8(b, flags);
	put_str(b, key_of(v, AK_KEY_MENU_ITEM_TITLE));
	// Its shortcut: a Shortcut (key, primary, shift, alt, control, run, mods).
	const ak_struct *s = sc ? sc->p : NULL;
	uint8_t mods = 0;
	if (s) {
		// Lower case, as the hosts match keys.
		const ak_str *ks = s->f[0].p;
		char lower[32];
		uint32_t n = ks && ks->len < sizeof lower ? ks->len : 0;
		for (uint32_t i = 0; i < n; i++) lower[i] = (char)(ks->data[i] >= 'A' && ks->data[i] <= 'Z' ? ks->data[i] + 32 : ks->data[i]);
		char kb[4];
		ak_span k = ak_key_name((ak_span){lower, n}, kb);
		ak_buf_str(b, k.p, k.len);
		mods = ak_shortcut_mods(s);
	} else {
		ak_buf_str(b, "", 0);
	}
	ak_buf_u8(b, mods);
	static const uint8_t wires[] = {0, 0, AK_MENU_ROLE_QUIT, AK_MENU_ROLE_CLOSE, AK_MENU_ROLE_OPEN_RECENT, AK_MENU_ROLE_UNDO, AK_MENU_ROLE_REDO};
	uint8_t wire = ro >= 0 && ro < (int64_t)AK_LEN(wires) ? wires[ro] : 0;
	ak_buf_u8(b, wire);
	if (sub) write_menu(m, bar, v);
}

// count_and_write writes the entries under v (through fragments), and
// returns how many it wrote.
static uint32_t write_entries(ak_menus *m, ak_view *bar, ak_view *v) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < v->kids.len; i++) {
		ak_view *k = v->kids.data[i];
		k->bar = bar;
		if (is_entry(k)) {
			write_item(m, bar, k);
			n++;
		} else if (k->frag && k->cls != &cls_bar_menu) {
			n += write_entries(m, bar, k);
		}
	}
	return n;
}

static void write_menu(ak_menus *m, ak_view *bar, ak_view *parent) {
	size_t at = m->buf.len;
	ak_buf_u32(&m->buf, 0);
	patch_u32(&m->buf, at, write_entries(m, bar, parent));
}

// write_menus writes the Menus under v (through fragments): each its
// title and its menu.
static uint32_t write_menus(ak_menus *m, ak_view *bar, ak_view *v) {
	v->bar = bar;
	if (v->cls == &cls_bar_menu) {
		put_str(&m->buf, key_of(v, AK_KEY_MENU_TITLE));
		write_menu(m, bar, v);
		return 1;
	}
	if (!v->frag) return 0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < v->kids.len; i++) n += write_menus(m, bar, v->kids.data[i]);
	return n;
}

// send sends the menus of the last MenuBar shown, if they changed.
static void send(ak_ui_root *r) {
	ak_menus *m = menus_of(r);
	ak_view *bar = m->bars.len ? m->bars.data[m->bars.len - 1] : NULL;
	if (!bar && !m->has_sent) return;
	release_items(m);
	m->buf.len = 0;
	ak_buf_u32(&m->buf, 0);
	bar_t *bt = bar ? bar->impl : NULL;
	if (bt && bt->content) patch_u32(&m->buf, 0, write_menus(m, bar, bt->content));
	if (m->has_sent && m->sent.len == m->buf.len && memcmp(m->sent.data, m->buf.data, m->buf.len) == 0) return;
	ak_buf t = m->sent;
	m->sent = m->buf;
	m->buf = t;
	m->has_sent = true;
	if (!r->el || r->el->dead) return;
	ak_batch_command(r->batch, r->el->id, AK_CMD_MENU_BAR, m->sent.data, m->sent.len);
	if (!m->listens) {
		m->listens = true;
		ak_view_listen(r->el, AK_EVENT_ACTION);
	}
}

void ak_menubar_resync(ak_ui_root *r) {
	ak_menus *m = r->menus;
	if (m && m->bars.len) ak_view_sync(m->bars.data[m->bars.len - 1]);
}

// ak_menubar_action runs the menu bar's item numbered index: a
// MenuItem's press, unless it's disabled, opens a submenu, or is the
// platform's (a role's, which the host does itself).
void ak_menubar_action(ak_ui_root *r, int64_t index) {
	ak_menus *m = r->menus;
	if (!m || index < 0 || index >= m->items.len) return;
	ak_view *v = m->items.data[index];
	if (v->cls != &cls_item || v->dead || has_entries(v)) return;
	const ak_val *d = key_of(v, AK_KEY_MENU_ITEM_DISABLED), *role = key_of(v, AK_KEY_MENU_ITEM_ROLE);
	if (role && (role->i == ROLE_UNDO || role->i == ROLE_REDO)) {
		// The host undoes a field's typing itself: this is the app's.
		ak_undo_event(r, role->i == ROLE_REDO);
		return;
	}
	if ((d && d->i) || (role && role->i > ROLE_DESTRUCTIVE)) return;
	ak_retain(v);
	ak_view_call(v, AK_EV_MENU_ITEM_PRESS, NULL, 0);
	ak_release(v);
}

// ---- MenuBar ----

static void bar_cleanup(void *ctx) {
	ak_view *e = ctx;
	bar_t *b = e->impl;
	ak_ui_root *r = e->r;
	ak_menus *m = r->menus;
	for (uint32_t i = 0; m && i < m->bars.len; i++) {
		if (m->bars.data[i] != e) continue;
		memmove(m->bars.data + i, m->bars.data + i + 1, (m->bars.len - i - 1) * sizeof(ak_view *));
		m->bars.len--;
		ak_release(e);
		break;
	}
	if (b->owner) {
		ak_owner *o = b->owner;
		b->owner = NULL;
		ak_owner_dispose(o);
	}
	if (b->content) {
		ak_view *c = b->content;
		b->content = NULL;
		ak_view_destroy(c);
		ak_release(c);
	}
	if (m && !r->disposed) send(r);  // the one before's
	ak_release(e);
}

static void bar_init(ak_view *e) {
	e->impl = ak_alloc(sizeof(bar_t));
	e->bar = e;
	ak_vec_push(&menus_of(e->r)->bars, (ak_view *)ak_retain(e));
	ak_on_cleanup(bar_cleanup, ak_retain(e));
}

static void bar_build(ak_view *e) {
	bar_t *b = e->impl;
	if (b->content || !b->build || e->dead || !e->finalized) return;
	b->owner = ak_root_under(e->owner);
	ak_owner_set_context(b->owner, menubar_key, e);
	b->content = ak_build_under(e->r, b->owner, b->build, NULL, 0);
	if (b->content) b->content->bar = e;
	ak_view_sync(e);
}

static void bar_lazy(ak_view *e, ak_closure *build) {
	bar_t *b = e->impl;
	ak_retain(build);
	ak_release(b->build);
	b->build = build;
	bar_build(e);
}

static void bar_sync(ak_view *e) {
	ak_menus *m = e->r->menus;
	if (m && m->bars.len && m->bars.data[m->bars.len - 1] == e) send(e->r);
}

static void bar_drop(ak_view *e) {
	bar_t *b = e->impl;
	if (!b) return;
	if (b->owner) ak_owner_dispose(b->owner);
	if (b->content) {
		ak_view_destroy(b->content);
		ak_release(b->content);
	}
	ak_release(b->build);
	ak_free(b);
}

static const ak_el_class cls_bar = {.name = "MenuBar", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT | AK_ELF_FLATTEN | AK_ELF_CHILDREN,
                                    .init = bar_init, .finalize = bar_build, .lazy = bar_lazy, .sync = bar_sync, .drop = bar_drop};

// A MenuBar's Menu, MenuItem and MenuSeparator: no views, only what the
// bar encodes.
static const ak_el_class cls_bar_menu = {.name = "Menu", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT | AK_ELF_CHILDREN};
static const ak_el_class cls_item = {.name = "MenuItem", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT | AK_ELF_CHILDREN};
static const ak_el_class cls_sep = {.name = "MenuSeparator", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT};

const ak_el_class *ak_menu_class_of(uint16_t el) {
	switch (el) {
	case AK_EL_MENU_BAR: return &cls_bar;
	case AK_EL_MENU_ITEM: return &cls_item;
	case AK_EL_MENU_SEPARATOR: return &cls_sep;
	}
	return NULL;
}

const ak_el_class *ak_menu_class_here(void) {
	void *x = NULL;
	if (ak_context_lookup(menubar_key, &x)) return &cls_bar_menu;
	return ak_el_class_of(AK_EL_MENU);
}
