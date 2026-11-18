// desktop.c: the fake host's desktop: windows as a desktop host
// keeps them (the main window, the root's, and each Window's: their
// options, their state, which WindowAction commands change and which it
// reports with WindowState while the core listens, posted), the menu bar
// the core last sent, the "tray" module (icons, the Dock menu, and what
// happens to them through its long poll, "next"), and the quits the app
// asked for.
#include "fake/fake.h"

#include <stdio.h>

typedef struct {
	ak_id id;          // the root's (the main window), or a Window's
	ak_buf cfg;        // its options as sent (WindowOptions' args, a Window's Config)
	ak_buf title;      // the main window's (WindowOptions', WindowTitle)
	ak_window_state st;
	bool closed;       // the main window closed (the app would quit or hide it)
	ak_buf actions;    // its WindowActions' names since read, a space before each
} fwin;

typedef struct {
	uint32_t id;
	uint8_t flags;  // 1 template, 2 clickable
	ak_buf image, symbol, title, tooltip, name, menu;
} ficon;

struct ak_fake_desk {
	AK_VEC(fwin *) wins;
	ak_buf menu_bar;
	bool has_menu_bar;
	AK_VEC(ficon *) icons;
	ak_buf dock;
	uint32_t docks;  // dock calls
	// The tray's "next" waiting for something to happen: its call and
	// root; what happened before it asked, queued (9 bytes each).
	bool waiting;
	uint32_t next_call;
	ak_id next_root;
	ak_buf queue;
	ak_buf quits;  // "quit", "relaunch", a space before each
};

static ak_fake_desk *desk(ak_fake *f) {
	if (!f->desk) f->desk = ak_alloc(sizeof(ak_fake_desk));
	return f->desk;
}

static void icon_free(ficon *ic) {
	ak_buf *bs[] = {&ic->image, &ic->symbol, &ic->title, &ic->tooltip, &ic->name, &ic->menu};
	for (size_t i = 0; i < AK_LEN(bs); i++) ak_buf_free(bs[i]);
	ak_free(ic);
}

static void win_free(fwin *w) {
	ak_buf_free(&w->cfg);
	ak_buf_free(&w->title);
	ak_buf_free(&w->actions);
	ak_free(w);
}

void ak_fake_desk_free(ak_fake *f) {
	ak_fake_desk *d = f->desk;
	if (!d) return;
	for (uint32_t i = 0; i < d->wins.len; i++) win_free(d->wins.data[i]);
	ak_vec_free(&d->wins);
	for (uint32_t i = 0; i < d->icons.len; i++) icon_free(d->icons.data[i]);
	ak_vec_free(&d->icons);
	ak_buf_free(&d->menu_bar);
	ak_buf_free(&d->dock);
	ak_buf_free(&d->queue);
	ak_buf_free(&d->quits);
	ak_free(d);
	f->desk = NULL;
}

static void put(ak_buf *b, const char *s) { ak_buf_put(b, s, strlen(s)); }
static void put_span(ak_buf *b, ak_span s) { ak_buf_put(b, s.p, s.len); }
static ak_span bspan(const ak_buf *b) { return (ak_span){(const char *)b->data, (uint32_t)b->len}; }
static void set_buf(ak_buf *b, const void *p, size_t n) {
	b->len = 0;
	ak_buf_put(b, p, n);
}

// ---- Windows ----

static fwin *find_win(ak_fake *f, ak_id id) {
	ak_fake_desk *d = desk(f);
	for (uint32_t i = 0; i < d->wins.len; i++)
		if (d->wins.data[i]->id == id) return d->wins.data[i];
	return NULL;
}

static fwin *win(ak_fake *f, ak_fake_view *v) {
	fwin *w = find_win(f, v->id);
	if (w) return w;
	w = ak_alloc(sizeof *w);
	w->id = v->id;
	w->st = (ak_window_state){.width = f->env.width, .height = f->env.height, .flags = AK_WINDOW_STATE_VISIBLE | AK_WINDOW_STATE_FOCUSED};
	ak_vec_push(&desk(f)->wins, w);
	return w;
}

static ak_window_config cfg_of(const fwin *w) { return ak_decode_window_config(w->cfg.data, w->cfg.len); }

static void configure(fwin *w) {
	ak_window_config c = cfg_of(w);
	ak_window_state *s = &w->st;
	if (c.width > 0) s->width = c.width;
	if (c.height > 0) s->height = c.height;
	if (c.flags & AK_WINDOW_POSITIONED) s->x = c.x, s->y = c.y;
	static const struct {
		uint32_t c;
		uint8_t s;
	} fl[] = {{AK_WINDOW_MAXIMIZED, AK_WINDOW_STATE_MAXIMIZED}, {AK_WINDOW_FULLSCREEN, AK_WINDOW_STATE_FULLSCREEN},
	          {AK_WINDOW_ALWAYS_ON_TOP, AK_WINDOW_STATE_ON_TOP}, {AK_WINDOW_CLICK_THROUGH, AK_WINDOW_STATE_CLICK_THROUGH}};
	for (size_t i = 0; i < AK_LEN(fl); i++)
		if (c.flags & fl[i].c) s->flags |= fl[i].s;
	if (c.flags & AK_WINDOW_HIDDEN) s->flags &= (uint8_t)~(AK_WINDOW_STATE_VISIBLE | AK_WINDOW_STATE_FOCUSED);
}

// report posts v's window's state, if the core listens for it.
static void report(ak_fake *f, ak_fake_view *v) {
	if (!ak_fake_listens(v, AK_EVENT_WINDOW_STATE)) return;
	fwin *w = v->kind == AK_KIND_WINDOW ? find_win(f, v->id) : win(f, v);
	if (!w) return;  // a Window not presented yet
	ak_buf b = {0};
	ak_append_window_state(&b, &w->st);
	ak_fake_post_event(f, &(ak_event){.id = v->id, .kind = AK_EVENT_WINDOW_STATE, .s = {(const char *)b.data, (uint32_t)b.len}});
	ak_buf_free(&b);
}

// take_focus makes w's window (v's) the one in use, as a desktop has
// one: the window that had the keyboard loses it, reported where the
// core listens.
static void take_focus(ak_fake *f, ak_fake_view *v, fwin *w) {
	ak_fake_desk *d = desk(f);
	for (uint32_t i = 0; i < d->wins.len; i++) {
		fwin *o = d->wins.data[i];
		if (o == w || !(o->st.flags & AK_WINDOW_STATE_FOCUSED)) continue;
		o->st.flags &= (uint8_t)~AK_WINDOW_STATE_FOCUSED;
		ak_fake_view *ov = ak_fake_view_of(f, o->id);
		if (ov) report(f, ov);
	}
	w->st.flags |= AK_WINDOW_STATE_FOCUSED;
	ak_fake_set_key_window(f, v);
}

static const char *const action_names[] = {"show", "hide", "focus", "minimize", "maximize", "restore", "fullScreen", "exitFullScreen",
                                           "center", "size", "position", "alwaysOnTop", "notAlwaysOnTop", "close", "clickThrough",
                                           "notClickThrough", "fit"};

static void action(ak_fake *f, ak_fake_view *v, uint8_t a, float x, float y) {
	fwin *w = win(f, v);
	ak_buf_u8(&w->actions, ' ');
	put(&w->actions, a < AK_LEN(action_names) ? action_names[a] : "?");
	if (a == AK_WINDOW_ACTION_SIZE || a == AK_WINDOW_ACTION_POSITION || a == AK_WINDOW_ACTION_FIT) {
		char n[48];
		snprintf(n, sizeof n, "(%g,%g)", x, y);
		put(&w->actions, n);
	}
	ak_window_state *s = &w->st;
	const uint8_t vis = AK_WINDOW_STATE_VISIBLE, foc = AK_WINDOW_STATE_FOCUSED;
	switch (a) {
	case AK_WINDOW_ACTION_SHOW: s->flags |= vis; break;
	case AK_WINDOW_ACTION_HIDE: s->flags &= (uint8_t)~(vis | foc); break;
	case AK_WINDOW_ACTION_FOCUS:
		s->flags |= vis;
		s->flags &= (uint8_t)~AK_WINDOW_STATE_MINIMIZED;
		take_focus(f, v, w);
		break;
	case AK_WINDOW_ACTION_MINIMIZE:
		s->flags |= AK_WINDOW_STATE_MINIMIZED;
		s->flags &= (uint8_t)~foc;
		break;
	case AK_WINDOW_ACTION_MAXIMIZE: s->flags |= AK_WINDOW_STATE_MAXIMIZED; break;
	case AK_WINDOW_ACTION_RESTORE: s->flags &= (uint8_t)~(AK_WINDOW_STATE_MINIMIZED | AK_WINDOW_STATE_MAXIMIZED); break;
	case AK_WINDOW_ACTION_FULLSCREEN: s->flags |= AK_WINDOW_STATE_FULLSCREEN; break;
	case AK_WINDOW_ACTION_EXIT_FULLSCREEN: s->flags &= (uint8_t)~AK_WINDOW_STATE_FULLSCREEN; break;
	case AK_WINDOW_ACTION_ALWAYS_ON_TOP: s->flags |= AK_WINDOW_STATE_ON_TOP; break;
	case AK_WINDOW_ACTION_NOT_ALWAYS_ON_TOP: s->flags &= (uint8_t)~AK_WINDOW_STATE_ON_TOP; break;
	case AK_WINDOW_ACTION_CLICK_THROUGH: s->flags |= AK_WINDOW_STATE_CLICK_THROUGH; break;
	case AK_WINDOW_ACTION_NOT_CLICK_THROUGH: s->flags &= (uint8_t)~AK_WINDOW_STATE_CLICK_THROUGH; break;
	case AK_WINDOW_ACTION_SIZE:
	case AK_WINDOW_ACTION_FIT: s->width = x, s->height = y; break;
	case AK_WINDOW_ACTION_POSITION: s->x = x, s->y = y; break;
	case AK_WINDOW_ACTION_CLOSE:
		w->closed = true;
		s->flags &= (uint8_t)~(vis | foc);
		break;
	}
	report(f, v);
}

// ak_fake_desk_command keeps what of a command the desktop follows.
void ak_fake_desk_command(ak_fake *f, ak_fake_view *v, ak_cmd c, ak_span args) {
	ak_fake_desk *d = desk(f);
	switch (c) {
	case AK_CMD_WINDOW_OPTIONS: {
		if (v->kind != AK_KIND_VIEW) return;
		fwin *w = win(f, v);
		set_buf(&w->cfg, args.p, args.len);
		ak_window_config cfg = cfg_of(w);
		set_buf(&w->title, cfg.title.p, cfg.title.len);
		configure(w);
		report(f, v);
		return;
	}
	case AK_CMD_WINDOW_TITLE:
		if (v->kind == AK_KIND_VIEW) set_buf(&win(f, v)->title, args.p, args.len);
		return;
	case AK_CMD_PRESENT: {
		if (v->kind != AK_KIND_WINDOW) return;
		fwin *w = win(f, v);
		ak_span cfg = ak_fake_string(v, AK_PROP_WINDOW_CONFIG);
		set_buf(&w->cfg, cfg.p, cfg.len);
		w->st = (ak_window_state){.width = 800, .height = 600, .flags = AK_WINDOW_STATE_VISIBLE};
		configure(w);
		if (w->st.flags & AK_WINDOW_STATE_VISIBLE) take_focus(f, v, w);  // a window opening takes the keyboard
		report(f, v);
		return;
	}
	case AK_CMD_WINDOW_ACTION: {
		if (v->kind != AK_KIND_WINDOW && v->kind != AK_KIND_VIEW) return;
		uint8_t a;
		float x, y;
		if (ak_decode_window_action((const uint8_t *)args.p, args.len, &a, &x, &y)) action(f, v, a, x, y);
		return;
	}
	case AK_CMD_MENU_BAR:
		set_buf(&d->menu_bar, args.p, args.len);
		d->has_menu_bar = true;
		return;
	case AK_CMD_QUIT:
		ak_buf_u8(&d->quits, ' ');
		put(&d->quits, args.len && (args.p[0] & 1) ? "relaunch" : "quit");
		return;
	}
}

// ak_fake_desk_listen hears the core start listening on v: a window's
// state is reported at once.
void ak_fake_desk_listen(ak_fake *f, ak_fake_view *v, ak_event_kind ev) {
	if (ev == AK_EVENT_WINDOW_STATE && (v->kind == AK_KIND_VIEW || v->kind == AK_KIND_WINDOW)) report(f, v);
}

// ak_fake_desk_destroyed hears a view destroyed: a Window's window goes.
void ak_fake_desk_destroyed(ak_fake *f, ak_id id) {
	ak_fake_desk *d = f->desk;
	if (!d) return;
	for (uint32_t i = 0; i < d->wins.len; i++) {
		if (d->wins.data[i]->id != id) continue;
		win_free(d->wins.data[i]);
		d->wins.data[i] = d->wins.data[--d->wins.len];
		return;
	}
}

// ak_fake_window_titled is the window titled title ("": the main window,
// the root), or NULL.
ak_fake_view *ak_fake_window_titled(ak_fake *f, ak_span title) {
	if (!title.len) return ak_fake_root(f);
	ak_fake_view_vec ws = {0};
	ak_fake_of_kind(f, AK_KIND_WINDOW, &ws);
	ak_fake_view *out = NULL;
	for (uint32_t i = 0; i < ws.len && !out; i++)
		if (ak_span_eq(ak_fake_string(ws.data[i], AK_PROP_WINDOW_TITLE), title)) out = ws.data[i];
	ak_vec_free(&ws);
	return out;
}

ak_window_state ak_fake_window_state(ak_fake *f, ak_fake_view *v) { return win(f, v)->st; }

void ak_fake_set_window_state(ak_fake *f, ak_fake_view *v, const ak_window_state *s) {
	fwin *w = win(f, v);
	w->st = *s;
	if (s->flags & AK_WINDOW_STATE_FOCUSED) take_focus(f, v, w);
	if (ak_fake_listens(v, AK_EVENT_WINDOW_STATE)) {
		ak_buf b = {0};
		ak_append_window_state(&b, s);
		ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_WINDOW_STATE, .s = {(const char *)b.data, (uint32_t)b.len}});
		ak_buf_free(&b);
	}
}

void ak_fake_window_actions(ak_fake *f, ak_fake_view *v, ak_buf *out) {
	fwin *w = win(f, v);
	if (w->actions.len) ak_buf_put(out, w->actions.data + 1, w->actions.len - 1);
	w->actions.len = 0;
}

// ak_fake_focus_window makes v's window the one in use, as the user's
// click on it does.
void ak_fake_focus_window(ak_fake *f, ak_fake_view *v) {
	fwin *w = win(f, v);
	bool had = w->st.flags & AK_WINDOW_STATE_FOCUSED;
	take_focus(f, v, w);
	if (!had) report(f, v);
}

// ak_fake_close_window closes v's window as the user does (its close
// button): a window the core asks about (it listens for CloseRequest) is
// asked and stays; otherwise a Window is dismissed (Dismissed) and the
// main window closes (closed: the app would quit, or keep running with it
// hidden).
void ak_fake_close_window(ak_fake *f, ak_fake_view *v) {
	if (ak_fake_listens(v, AK_EVENT_CLOSE_REQUEST)) {
		ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_CLOSE_REQUEST});
		return;
	}
	if (v->kind == AK_KIND_VIEW) {
		fwin *w = win(f, v);
		w->closed = true;
		w->st.flags &= (uint8_t)~(AK_WINDOW_STATE_VISIBLE | AK_WINDOW_STATE_FOCUSED);
		report(f, v);
		return;
	}
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_DISMISSED});
}

static void put_state(ak_buf *b, const ak_window_state *s, bool closed) {
	char n[96];
	snprintf(n, sizeof n, " %gx%g", s->width, s->height);
	put(b, n);
	if (s->x != 0 || s->y != 0) {
		snprintf(n, sizeof n, " at %g,%g", s->x, s->y);
		put(b, n);
	}
	static const char *const bits[] = {"visible", "focused", "minimized", "maximized", "fullScreen", "alwaysOnTop", "clickThrough"};
	for (int i = 0; i < 7; i++)
		if (s->flags & (1 << i)) ak_buf_u8(b, ' '), put(b, bits[i]);
	if (closed) put(b, " closed");
}

static void put_cfg(ak_buf *b, const ak_window_config *c) {
	static const struct {
		uint32_t f;
		const char *name;
	} fl[] = {{AK_WINDOW_PANEL, "panel"}, {AK_WINDOW_FLYOUT, "flyout"}, {AK_WINDOW_FIT_CONTENT, "fit"}, {AK_WINDOW_MODAL, "modal"},
	          {AK_WINDOW_CHILD, "child"}, {AK_WINDOW_FIXED_SIZE, "fixed"}, {AK_WINDOW_NO_MAXIMIZE, "noMaximize"},
	          {AK_WINDOW_NO_MINIMIZE, "noMinimize"}, {AK_WINDOW_NO_CLOSE, "noClose"}, {AK_WINDOW_SKIP_TASKBAR, "skipTaskbar"},
	          {AK_WINDOW_CENTERED, "centered"}, {AK_WINDOW_TRANSPARENT, "transparent"}, {AK_WINDOW_MENU_BAR, "menuBar"}};
	char n[96];
	if (c->min_width || c->min_height || c->max_width || c->max_height) {
		snprintf(n, sizeof n, " min %gx%g max %gx%g", c->min_width, c->min_height, c->max_width, c->max_height);
		put(b, n);
	}
	for (size_t i = 0; i < AK_LEN(fl); i++)
		if (c->flags & fl[i].f) ak_buf_u8(b, ' '), put(b, fl[i].name);
	if (c->state_key.len) put(b, " key="), put_span(b, c->state_key);
}

// ak_fake_windows describes the windows, a line each: the main one (if
// the app has one), then each Window open, by its title.
void ak_fake_windows(ak_fake *f, ak_buf *out) {
	ak_fake_view *r = ak_fake_root(f);
	ak_window_config rc = r ? cfg_of(win(f, r)) : (ak_window_config){0};
	if (r && !(rc.flags & AK_WINDOW_NONE)) {  // an app without a main window: none
		fwin *w = win(f, r);
		put(out, "main \"");
		ak_buf_put(out, w->title.data, w->title.len);
		ak_buf_u8(out, '"');
		put_state(out, &w->st, w->closed);
		put_cfg(out, &rc);
		ak_buf_u8(out, '\n');
	}
	ak_fake_view_vec ws = {0};
	ak_fake_of_kind(f, AK_KIND_WINDOW, &ws);
	for (uint32_t i = 0; i < ws.len; i++) {
		fwin *w = find_win(f, ws.data[i]->id);
		if (!w) continue;
		ak_buf_u8(out, '"');
		put_span(out, ak_fake_string(ws.data[i], AK_PROP_WINDOW_TITLE));
		ak_buf_u8(out, '"');
		put_state(out, &w->st, false);
		ak_window_config c = cfg_of(w);
		put_cfg(out, &c);
		ak_span fl = ak_fake_string(ws.data[i], AK_PROP_WINDOW_FLYOUT);
		ak_flyout fo;
		if (fl.len && ak_decode_flyout((const uint8_t *)fl.p, fl.len, &fo)) {
			char n[96];
			snprintf(n, sizeof n, " anchor=%u placement=%u gap=%g", fo.tray, fo.placement, fo.gap);
			put(out, n);
		}
		ak_buf_u8(out, '\n');
	}
	ak_vec_free(&ws);
}

// ---- Menus ----

static void put_shortcut(ak_buf *b, const ak_menu_entry *e) {
	if (!e->key.len) return;
	put(b, " [");
	if (e->mods & 1) put(b, "primary+");
	if (e->mods & 8) put(b, "control+");
	if (e->mods & 4) put(b, "alt+");
	if (e->mods & 2) put(b, "shift+");
	put_span(b, e->key);
	ak_buf_u8(b, ']');
}

static void put_entry(ak_buf *b, const ak_menu_entry *e) {
	if (e->flags & AK_MENU_SEPARATOR) {
		put(b, "---");
		return;
	}
	put_span(b, e->title);
	put_shortcut(b, e);
	if (e->flags & AK_MENU_DISABLED) put(b, " (disabled)");
	if (e->flags & AK_MENU_CHECKED) put(b, " (checked)");
	if (e->flags & AK_MENU_DESTRUCTIVE) put(b, " (destructive)");
	static const char *const roles[] = {"", " (quit)", " (close)", " (openRecent)"};
	if (e->role && e->role < AK_LEN(roles)) put(b, roles[e->role]);
}

// path_of appends an entry's path: its parents' titles, then its own,
// joined by " > ".
static void path_of(ak_buf *b, const ak_menu_entry *es, uint32_t i) {
	if (es[i].parent != UINT32_MAX) {
		path_of(b, es, es[i].parent);
		put(b, " > ");
	}
	put_entry(b, &es[i]);
}

static void put_menu(ak_buf *out, const char *indent, ak_span prefix, const ak_menu_entry *es, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		put(out, indent);
		if (prefix.len) put_span(out, prefix), put(out, " > ");
		path_of(out, es, i);
		ak_buf_u8(out, '\n');
	}
}

void ak_fake_menu_bar(ak_fake *f, ak_buf *out) {
	ak_fake_desk *d = desk(f);
	if (!d->has_menu_bar) return;
	ak_menu_title_vec ms = {0};
	ak_menu_entry_vec es = {0};
	ak_decode_menu_bar(d->menu_bar.data, d->menu_bar.len, &ms, &es);
	for (uint32_t m = 0; m < ms.len; m++) {
		// A menu's entries' parents are in the whole bar's numbering.
		for (uint32_t i = ms.data[m].first; i < ms.data[m].first + ms.data[m].total; i++) {
			put_span(out, ms.data[m].title);
			put(out, " > ");
			path_of(out, es.data, i);
			ak_buf_u8(out, '\n');
		}
	}
	ak_vec_free(&ms);
	ak_vec_free(&es);
}

// find_path finds the entry whose titles, from the top, are path's parts
// (separated by " > "): its index, or -1.
static int64_t find_path(const ak_menu_entry *es, uint32_t first, uint32_t n, ak_span path) {
	ak_span rest = path;
	uint32_t parent = UINT32_MAX;
	int64_t found = -1;
	while (rest.len) {
		ak_span part = rest;
		for (uint32_t i = 0; i + 2 < rest.len; i++) {
			if (rest.p[i] == ' ' && rest.p[i + 1] == '>' && rest.p[i + 2] == ' ') {
				part = (ak_span){rest.p, i};
				break;
			}
		}
		rest = part.len == rest.len ? (ak_span){rest.p + rest.len, 0} : (ak_span){part.p + part.len + 3, rest.len - part.len - 3};
		found = -1;
		for (uint32_t i = first; i < first + n; i++) {
			if (es[i].parent == parent && !(es[i].flags & AK_MENU_SEPARATOR) && ak_span_eq(es[i].title, part)) {
				found = i;
				break;
			}
		}
		if (found < 0) return -1;
		parent = (uint32_t)found;
	}
	return found;
}

// ak_fake_choose_menu_item chooses the menu bar's item at path ("File >
// Export"), as a click or its shortcut does: a Close item closes the
// window in use, a Quit item quits (the app is asked), as hosts do;
// others are reported. It returns false, saying why in err, when there's
// no such item or it's disabled.
bool ak_fake_choose_menu_item(ak_fake *f, ak_span path, ak_buf *err) {
	ak_fake_desk *d = desk(f);
	ak_menu_title_vec ms = {0};
	ak_menu_entry_vec es = {0};
	if (d->has_menu_bar) ak_decode_menu_bar(d->menu_bar.data, d->menu_bar.len, &ms, &es);
	int64_t found = -1;
	for (uint32_t m = 0; m < ms.len && found < 0; m++) {
		ak_span t = ms.data[m].title;
		if (path.len > t.len + 3 && memcmp(path.p, t.p, t.len) == 0 && memcmp(path.p + t.len, " > ", 3) == 0)
			found = find_path(es.data, ms.data[m].first, ms.data[m].total, (ak_span){path.p + t.len + 3, path.len - t.len - 3});
	}
	bool ok = false;
	if (found < 0) {
		ak_buf_printf(err, "the menu bar has no %.*s: it has\n", (int)path.len, path.p);
		ak_fake_menu_bar(f, err);
	} else if (es.data[found].flags & AK_MENU_DISABLED) {
		ak_buf_printf(err, "the menu bar's %.*s is disabled", (int)path.len, path.p);
	} else if (es.data[found].role == AK_MENU_ROLE_CLOSE) {
		ok = true;
		ak_fake_close_window(f, ak_fake_key_window(f));
	} else if (es.data[found].role == AK_MENU_ROLE_QUIT) {
		ok = true;
		ak_buf_u8(&d->quits, ' ');
		put(&d->quits, "user");
	} else {
		ok = true;
		ak_fake_view *r = ak_fake_root(f);
		if (r && ak_fake_listens(r, AK_EVENT_ACTION)) ak_fake_dispatch(f, &(ak_event){.id = r->id, .kind = AK_EVENT_ACTION, .i = found});
		else ak_buf_printf(err, "the root doesn't listen for the menu bar's Action"), ok = false;
	}
	ak_vec_free(&ms);
	ak_vec_free(&es);
	return ok;
}

void ak_fake_quits(ak_fake *f, ak_buf *out) {
	ak_fake_desk *d = desk(f);
	if (d->quits.len) ak_buf_put(out, d->quits.data + 1, d->quits.len - 1);
}

// ---- The tray ----

static void answer(ak_fake *f, ak_id root, uint32_t call, bool ok, ak_span ans) {
	ak_buf res = {0};
	ak_buf_u32(&res, call);
	ak_buf_u8(&res, ok ? 1 : 0);
	ak_buf_put(&res, ans.p, ans.len);
	ak_fake_post_event(f, &(ak_event){.id = root, .kind = AK_EVENT_RESULT, .s = {(const char *)res.data, (uint32_t)res.len}});
	ak_buf_free(&res);
}

static ficon *find_icon(ak_fake_desk *d, uint32_t id) {
	for (uint32_t i = 0; i < d->icons.len; i++)
		if (d->icons.data[i]->id == id) return d->icons.data[i];
	return NULL;
}

// ak_fake_desk_call answers the "tray" module's calls: set, remove, dock, next (held until something happens), stop.
// It returns false for other modules.
bool ak_fake_desk_call(ak_fake *f, ak_id root, uint32_t call, ak_span module, ak_span method, ak_span args) {
	if (!ak_span_eq(module, ak_cstr("tray"))) return false;
	ak_fake_desk *d = desk(f);
	ak_reader r = {(const uint8_t *)args.p, args.len, 0, false};
	if (ak_span_eq(method, ak_cstr("supported"))) {
		answer(f, root, call, true, ak_cstr("\x01"));
	} else if (ak_span_eq(method, ak_cstr("set"))) {
		uint32_t id = ak_read_u32(&r);
		uint8_t flags = ak_read_u8(&r);
		if (r.err) {
			answer(f, root, call, false, ak_cstr("bad args"));
			return true;
		}
		ficon *ic = find_icon(d, id);
		if (!ic) {
			ic = ak_alloc(sizeof *ic);
			ic->id = id;
			ak_vec_push(&d->icons, ic);
		}
		ic->flags = flags;
		ak_buf *bs[] = {&ic->image, &ic->symbol, &ic->title, &ic->tooltip, &ic->name};
		for (size_t i = 0; i < AK_LEN(bs); i++) {
			ak_span s = ak_read_str(&r);
			set_buf(bs[i], s.p, r.err ? 0 : s.len);
		}
		set_buf(&ic->menu, args.p + r.off, r.err ? 0 : args.len - r.off);
		answer(f, root, call, true, ak_cstr(""));
	} else if (ak_span_eq(method, ak_cstr("remove"))) {
		uint32_t id = ak_read_u32(&r);
		for (uint32_t i = 0; i < d->icons.len; i++) {
			if (d->icons.data[i]->id != id) continue;
			icon_free(d->icons.data[i]);
			memmove(d->icons.data + i, d->icons.data + i + 1, (d->icons.len - i - 1) * sizeof(ficon *));
			d->icons.len--;
			break;
		}
		answer(f, root, call, true, ak_cstr(""));
	} else if (ak_span_eq(method, ak_cstr("dock"))) {
		set_buf(&d->dock, args.p, args.len);
		d->docks++;
		answer(f, root, call, true, ak_cstr(""));
	} else if (ak_span_eq(method, ak_cstr("next"))) {
		if (d->queue.len >= 9) {
			answer(f, root, call, true, (ak_span){(const char *)d->queue.data, 9});
			memmove(d->queue.data, d->queue.data + 9, d->queue.len - 9);
			d->queue.len -= 9;
		} else {
			d->waiting = true;
			d->next_call = call;
			d->next_root = root;
		}
	} else if (ak_span_eq(method, ak_cstr("stop"))) {
		if (d->waiting) {
			d->waiting = false;
			answer(f, d->next_root, d->next_call, false, ak_cstr("stopped"));
		}
		answer(f, root, call, true, ak_cstr(""));
	} else {
		ak_buf m = {0};
		ak_buf_printf(&m, "no module method tray.%.*s", (int)method.len, method.p);
		answer(f, root, call, false, bspan(&m));
		ak_buf_free(&m);
	}
	return true;
}

// happen reports what happened to icon id (0: the Dock menu): kind 1 a
// click, 2 a menu item chosen (its number).
static void happen(ak_fake *f, uint32_t id, uint8_t kind, uint32_t index) {
	ak_fake_desk *d = desk(f);
	uint8_t b[9];
	memcpy(b, &id, 4);
	b[4] = kind;
	memcpy(b + 5, &index, 4);
	if (d->waiting) {
		d->waiting = false;
		answer(f, d->next_root, d->next_call, true, (ak_span){(const char *)b, 9});
	} else {
		ak_buf_put(&d->queue, b, 9);
	}
}

// ak_fake_tray_icon is the icon named name (its "set"'s ID string), or
// NULL.
static ficon *icon_named(ak_fake *f, ak_span name) {
	ak_fake_desk *d = desk(f);
	for (uint32_t i = 0; i < d->icons.len; i++)
		if (ak_span_eq(bspan(&d->icons.data[i]->name), name)) return d->icons.data[i];
	return NULL;
}

bool ak_fake_click_tray(ak_fake *f, ak_span name, ak_buf *err) {
	ficon *ic = icon_named(f, name);
	if (!ic) {
		ak_buf_printf(err, "no tray icon \"%.*s\": there are\n", (int)name.len, name.p);
		ak_fake_trays(f, err);
		return false;
	}
	happen(f, ic->id, 1, 0);
	return true;
}

bool ak_fake_choose_tray_item(ak_fake *f, ak_span name, ak_span path, ak_buf *err) {
	ak_fake_desk *d = desk(f);
	ficon *ic = name.len ? icon_named(f, name) : NULL;
	if (name.len && !ic) {
		ak_buf_printf(err, "no tray icon \"%.*s\"", (int)name.len, name.p);
		return false;
	}
	const ak_buf *menu = ic ? &ic->menu : &d->dock;
	ak_menu_entry_vec es = {0};
	ak_decode_menu(menu->data, menu->len, &es);
	int64_t i = find_path(es.data, 0, es.len, path);
	bool ok = i >= 0 && !(es.data[i].flags & AK_MENU_DISABLED);
	if (i < 0) {
		ak_buf_printf(err, "%s has no item %.*s: it has\n", ic ? "the tray icon's menu" : "the Dock menu", (int)path.len, path.p);
		put_menu(err, "  ", ak_cstr(""), es.data, es.len);
	} else if (!ok) {
		ak_buf_printf(err, "%.*s is disabled", (int)path.len, path.p);
	} else {
		happen(f, ic ? ic->id : 0, 2, (uint32_t)i);
	}
	ak_vec_free(&es);
	return ok;
}

void ak_fake_trays(ak_fake *f, ak_buf *out) {
	ak_fake_desk *d = desk(f);
	for (uint32_t i = 0; i < d->icons.len; i++) {
		const ficon *ic = d->icons.data[i];
		put_span(out, bspan(&ic->name));
		ak_buf_u8(out, ':');
		const struct {
			const char *k;
			const ak_buf *v;
		} fs[] = {{"image", &ic->image}, {"symbol", &ic->symbol}, {"title", &ic->title}, {"tooltip", &ic->tooltip}};
		for (size_t k = 0; k < AK_LEN(fs); k++) {
			if (!fs[k].v->len) continue;
			ak_buf_printf(out, " %s=\"", fs[k].k);
			ak_buf_put(out, fs[k].v->data, fs[k].v->len);
			ak_buf_u8(out, '"');
		}
		if (ic->flags & 1) put(out, " template");
		if (ic->flags & 2) put(out, " clickable");
		ak_buf_u8(out, '\n');
		ak_menu_entry_vec es = {0};
		ak_decode_menu(ic->menu.data, ic->menu.len, &es);
		put_menu(out, "  ", ak_cstr(""), es.data, es.len);
		ak_vec_free(&es);
	}
}

void ak_fake_dock_menu(ak_fake *f, ak_buf *out) {
	ak_fake_desk *d = desk(f);
	ak_menu_entry_vec es = {0};
	ak_decode_menu(d->dock.data, d->dock.len, &es);
	put_menu(out, "", ak_cstr(""), es.data, es.len);
	ak_vec_free(&es);
}

bool ak_fake_tray_following(ak_fake *f) { return desk(f)->waiting; }
uint32_t ak_fake_tray_count(ak_fake *f) { return desk(f)->icons.len; }
