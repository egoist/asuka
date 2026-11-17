// window.c: windows: the Window element,
// a desktop window of the app's own while it's open, the main window
// (the app block's Window: its keys are the root's options), window
// handles (the commands, WindowAction, and the state, WindowState, of a
// Window or the main window), close requests and what asks before a
// window closes (on closeRequest, WindowCloseGuard, onCloseRequest), and
// a window's keys (WindowKeys).
//
// A handle is the Window element itself (Window(…) as w), or none: the
// window the calling code is in (a Window's content finds its Window as
// a context), else the main window. On a phone there are no windows: the
// commands go to the root, which its host ignores.
#include "ui/ui_ext.h"

static const ak_val *key_of(const ak_view *e, uint16_t key) { return ak_style_get(e, key); }

static void send_str(ak_view *e, ak_prop p, ak_val v) {
	const ak_str *s = v.p;
	ak_batch_set_string(e->r->batch, e->id, p, s ? s->data : "", s ? s->len : 0);
}

static void unlisten(ak_view *e, ak_event_kind ev) {
	if (!ak_view_listening(e, ev)) return;
	e->listens[ev / 64] &= ~(UINT64_C(1) << (ev % 64));
	if (e->real && !e->dead) ak_batch_unlisten(e->r->batch, e->id, ev);
}

// ---- Handles ----

// What asks before a window closes: a closure (onCloseRequest's: false
// keeps it open), or a WindowCloseGuard.
typedef struct {
	ak_closure *c;  // retained; NULL for a guard
	ak_view *guard; // not retained: it's removed as the guard goes
} ask_t;

// A window's handle: what asks before it closes, and its state as the
// host reports it while the core listens.
typedef struct {
	AK_VEC(ask_t) asks;
	ak_window_state st;
	ak_node node;
	bool watch;  // its state is followed
	bool known;  // the host reported it
	ak_view *main;  // the root's: the app's main Window element, if any (not retained)
} handle_t;

static void handle_free(handle_t *h) {
	for (uint32_t i = 0; i < h->asks.len; i++) ak_release(h->asks.data[i].c);
	ak_vec_free(&h->asks);
	ak_signal_drop(&h->node);
}

typedef struct {
	ak_closure *build;
	ak_view *win;     // the window, while it's open (retained); the main window's is the root
	ak_owner *owner;  // its content's
	handle_t h;
	ak_window_config cfg;  // as it opened (a window sized to its content's least and most)
	ak_buf flyout;         // the Flyout prop as last sent
	bool main;             // the app's main window
	bool main_built;       // the main window's content is built
	bool main_open;        // the main window shows (as last sent)
} window_t;

static handle_t *main_handle(ak_ui_root *r) {
	if (!r->el->impl) r->el->impl = ak_alloc(sizeof(handle_t));
	return r->el->impl;
}

void ak_window_root_drop(ak_view *e) {
	handle_t *h = e->impl;
	if (!h) return;
	handle_free(h);
	ak_free(h);
	e->impl = NULL;
}

static bool is_main(const ak_view *w) { return w && w->el == AK_EL_WINDOW && ((window_t *)w->impl)->main; }

// The view a handle's commands go to: the open window's, or the root's
// (the main window); NULL for a closed Window.
static ak_view *handle_view(ak_view *w) {
	if (!w) return NULL;
	if (w->el == AK_EL_WINDOW) return is_main(w) ? w->r->el : ((window_t *)w->impl)->win;
	return w;
}

static handle_t *handle_of(ak_view *w) { return w->el == AK_EL_WINDOW ? &((window_t *)w->impl)->h : main_handle(w->r); }

static const ak_el_class cls_win_view;

// target is the handle w names: a Window element (the main one: the
// root), a view in a window (that window), or none: the window the
// calling code is in, else the main window.
static ak_view *target(ak_view *w) {
	if (w && w->el == AK_EL_WINDOW) return is_main(w) ? w->r->el : w;
	if (w) {
		ak_view *x = w;
		for (int i = 0; i < 1 << 16 && x; i++) {
			if (x->cls == &cls_win_view && x->up) return x->up;  // a Window's view: its element
			x = x->parent ? x->parent : x->up;
		}
		return w->r->el;
	}
	void *v = NULL;
	if (ak_context_lookup(ak_window_key, &v)) {
		ak_view *e = v;
		return is_main(e) ? e->r->el : e;
	}
	return ak_ui_current()->el;
}

ak_view *ak_window_of(ak_view *e) {
	if (!e) return NULL;
	return handle_view(target(e));
}

static void window_here(void *ctx) { *(ak_view **)ctx = handle_view(target(NULL)); }

// ak_window_under is the window code under owner o runs in, by its
// context (a Window's content finds its Window), before its views are
// in that window: the root's view or the Window's.
ak_view *ak_window_under(ak_owner *o) {
	ak_view *v = NULL;
	if (o && !ak_owner_disposed(o)) ak_with_owner(o, window_here, &v);
	return v;
}

// The Window element whose handlers a handle runs: a Window, or the
// main window's (NULL when the app has none).
static ak_view *handlers_of(ak_view *w) { return w->el == AK_EL_WINDOW ? w : main_handle(w->r)->main; }

// sync_listen listens for what the open window's view must report: its
// close requests while something asks, its state while it's followed.
static void sync_listen(ak_view *w) {
	ak_view *v = handle_view(w);
	if (!v || v->dead) return;
	handle_t *h = handle_of(w);
	ak_view *el = handlers_of(w);
	bool asks = h->asks.len > 0 || (el && !el->dead && ak_view_handler(el, AK_EV_WINDOW_CLOSE_REQUEST));
	if (asks) ak_view_listen(v, AK_EVENT_CLOSE_REQUEST);
	else unlisten(v, AK_EVENT_CLOSE_REQUEST);
	if (h->watch) ak_view_listen(v, AK_EVENT_WINDOW_STATE);
}

static void watch(ak_view *w) {
	handle_t *h = handle_of(w);
	if (h->watch) return;
	h->watch = true;
	sync_listen(w);
}

static void window_close(ak_view *w);

// destroy closes the window now, without asking.
static void destroy(ak_view *w) {
	if (w->el == AK_EL_WINDOW) {
		window_t *wt = w->impl;
		if (!wt->win) return;
		ak_retain(w);
		window_close(w);
		ak_val f = {.i = 0};
		ak_view_call(w, AK_EV_WINDOW_CHANGE, &f, 1);
		ak_view_call(w, AK_EV_WINDOW_CLOSE, NULL, 0);
		ak_release(w);
		return;
	}
	uint8_t a = AK_WINDOW_ACTION_CLOSE;
	ak_batch_command(w->r->batch, w->id, AK_CMD_WINDOW_ACTION, &a, 1);
}

static bool guard_asks(ak_view *g);

// allow asks what asks before the window closes; all must agree.
static bool allow(ak_view *w) {
	handle_t *h = handle_of(w);
	uint32_t n = h->asks.len;
	if (!n) return true;
	// A copy: an ask may add or remove others.
	ask_t *as = ak_alloc(n * sizeof *as);
	for (uint32_t i = 0; i < n; i++) {
		as[i] = h->asks.data[i];
		if (as[i].c) ak_retain(as[i].c);
		if (as[i].guard) ak_retain(as[i].guard);
	}
	bool ok = true;
	for (uint32_t i = 0; i < n; i++) {
		if (as[i].c) {
			ak_val v = ak_call(as[i].c, NULL, 0);
			if (ak_error_pending()) {
				ak_ui_raise_pending(NULL);
				ok = false;
			} else if (!v.i) {
				ok = false;
			}
			ak_release(as[i].c);
		} else {
			if (!as[i].guard->dead && guard_asks(as[i].guard)) ok = false;
			ak_release(as[i].guard);
		}
	}
	ak_free(as);
	return ok;
}

// close_requested is the user closing the window (or w.close()): what
// asks first, then its closeRequest handler (it stays open), else it
// closes.
static void close_requested(ak_view *w) {
	if (!allow(w)) return;
	ak_view *el = handlers_of(w);
	if (el && !el->dead && ak_view_handler(el, AK_EV_WINDOW_CLOSE_REQUEST)) {
		// The window stays: its handler closes it, or not.
		ak_view_call(el, AK_EV_WINDOW_CLOSE_REQUEST, NULL, 0);
		return;
	}
	destroy(w);
}

static void main_visible(ak_ui_root *r, bool visible);

static void set_state(ak_view *w, const ak_event *ev) {
	handle_t *h = handle_of(w);
	bool was = h->known && (h->st.flags & AK_WINDOW_STATE_VISIBLE);
	ak_window_state st = ak_decode_window_state((const uint8_t *)ev->s.p, ev->s.len);
	bool same = h->known && memcmp(&st, &h->st, sizeof st) == 0;
	h->st = st;
	h->known = true;
	if (!same) ak_signal_write(&h->node);
	bool now = (st.flags & AK_WINDOW_STATE_VISIBLE) != 0;
	if (w->el != AK_EL_WINDOW && was != now) main_visible(w->r, now);
}

// The main window's events, to the root view.
void ak_window_event(ak_view *e, const ak_event *ev) {
	if (ev->kind == AK_EVENT_CLOSE_REQUEST) close_requested(e);
	else if (ev->kind == AK_EVENT_WINDOW_STATE) set_state(e, ev);
}

typedef struct {
	ak_view *w;  // retained
	ask_t a;
} ask_reg;

static void ask_remove(void *ctx) {
	ask_reg *g = ctx;
	handle_t *h = handle_of(g->w);
	for (uint32_t i = 0; i < h->asks.len; i++) {
		if (h->asks.data[i].c == g->a.c && h->asks.data[i].guard == g->a.guard) {
			memmove(h->asks.data + i, h->asks.data + i + 1, (h->asks.len - i - 1) * sizeof(ask_t));
			h->asks.len--;
			ak_release(g->a.c);
			break;
		}
	}
	if (!g->w->dead) sync_listen(g->w);
	ak_release(g->w);
	ak_free(g);
}

static void add_ask(ak_view *w, ask_t a) {
	handle_t *h = handle_of(w);
	if (a.c) ak_retain(a.c);
	ak_vec_push(&h->asks, a);
	sync_listen(w);
	ask_reg *g = ak_alloc(sizeof *g);
	g->w = ak_retain(w);
	g->a = a;
	ak_on_cleanup(ask_remove, g);
}

// ak_ui_on_close_request asks f when the user closes the window the
// calling component is in, while the component lives: false keeps it
// open.
void ak_ui_on_close_request(ak_closure *f) { add_ask(target(NULL), (ask_t){f, NULL}); }

static bool state_bit(const handle_t *h, uint8_t bit) { return (h->st.flags & bit) != 0; }

// ak_ui_window_action does a WindowAction (AK_WINDOW_ACTION_*) on w's
// window: x, y are a Size's or a Position's. Close is the user's closing
// (asking first). A closed Window opens for Show and Focus (its change
// handler is told), and ignores the others.
void ak_ui_window_action(ak_view *w, int64_t action, float x, float y) {
	w = target(w);
	ak_view *v = handle_view(w);
	if (action == AK_WINDOW_ACTION_CLOSE) {
		if (v) close_requested(w);
		return;
	}
	if (!v) {
		if (action == AK_WINDOW_ACTION_SHOW || action == AK_WINDOW_ACTION_FOCUS) {
			ak_val t = {.i = 1};
			ak_view_call(w, AK_EV_WINDOW_CHANGE, &t, 1);
		}
		return;
	}
	ak_buf *b = ak_batch_command_begin(v->r->batch, v->id, AK_CMD_WINDOW_ACTION);
	ak_append_window_action(b, (uint8_t)action, x, y);
	ak_batch_command_end(v->r->batch);
	// The main window shown or hidden by the app: its open follows.
	if (v == v->r->el && (action == AK_WINDOW_ACTION_SHOW || action == AK_WINDOW_ACTION_FOCUS || action == AK_WINDOW_ACTION_HIDE)) {
		ak_view *m = main_handle(v->r)->main;
		if (m && !m->dead) ((window_t *)m->impl)->main_open = action != AK_WINDOW_ACTION_HIDE;
	}
}

// ak_ui_window_close closes w's window now, without asking: what a
// closeRequest that said no calls once the user has chosen.
void ak_ui_window_close(ak_view *w) {
	w = target(w);
	if (handle_view(w)) destroy(w);
}

static const uint8_t state_kinds[] = {AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_BOOL, AK_K_BOOL,
                                      AK_K_BOOL, AK_K_BOOL, AK_K_BOOL, AK_K_BOOL, AK_K_BOOL};
static const char *const state_names[] = {"x", "y", "width", "height", "visible", "focused", "minimized", "maximized", "fullscreen",
                                          "alwaysOnTop", "clickThrough"};
static const ak_shape *const no_shapes[11];
const ak_shape ak_ui_shape_window_state = {"WindowState", 11, state_kinds, no_shapes, state_names, 0, NULL, false, NULL};

// followed is w's handle, its state followed from now on and read: a
// binding that reads it follows it. The host reports it as the core
// starts listening.
static handle_t *followed(ak_view *w) {
	watch(w);
	handle_t *h = handle_of(w);
	ak_signal_read(&h->node);
	return h;
}

// ak_ui_window_state is w's window's state (WindowState), read.
ak_val ak_ui_window_state(ak_view *w) {
	const handle_t *h = followed(target(w));
	ak_struct *s = ak_struct_new(&ak_ui_shape_window_state);
	const ak_window_state *st = &h->st;
	s->f[0].f = st->x, s->f[1].f = st->y, s->f[2].f = st->width, s->f[3].f = st->height;
	static const uint8_t bits[] = {AK_WINDOW_STATE_VISIBLE, AK_WINDOW_STATE_FOCUSED, AK_WINDOW_STATE_MINIMIZED, AK_WINDOW_STATE_MAXIMIZED,
	                               AK_WINDOW_STATE_FULLSCREEN, AK_WINDOW_STATE_ON_TOP, AK_WINDOW_STATE_CLICK_THROUGH};
	for (int i = 0; i < 7; i++) s->f[4 + i].i = (st->flags & bits[i]) != 0;
	return (ak_val){.p = s};
}

// ak_ui_window_flag is one of w's window's state bits
// (AK_WINDOW_STATE_*), read.
bool ak_ui_window_flag(ak_view *w, int64_t bit) {
	// A view's focused (not a window's, the root's or a Window's view):
	// the keyboard's focus is in it.
	if (bit == 2 && w && w->el != AK_EL_WINDOW && w != w->r->el && w->cls != &cls_win_view) return ak_ui_view_state(w, AK_STATE_FOCUSED);
	return state_bit(followed(target(w)), (uint8_t)bit);
}

static const uint8_t pair_kinds[] = {AK_K_FLOAT, AK_K_FLOAT};
static const ak_shape pair_shape = {"(float,float)", 2, pair_kinds, no_shapes, NULL, 0, NULL, false, NULL};

// ak_ui_window_pair is w's window's position (its frame's top left on
// the screen, which 0) or its content's size (which 1), read.
ak_val ak_ui_window_pair(ak_view *w, int64_t which) {
	const handle_t *h = followed(target(w));
	ak_struct *s = ak_struct_new(&pair_shape);
	s->f[0].f = which ? h->st.width : h->st.x;
	s->f[1].f = which ? h->st.height : h->st.y;
	return (ak_val){.p = s};
}

// ---- Windows' options ----

static float kfloat(const ak_view *w, uint16_t key) {
	const ak_val *v = key_of(w, key);
	return v ? (float)v->f : 0;
}

static bool kbool(const ak_view *w, uint16_t key, bool def) {
	const ak_val *v = key_of(w, key);
	return v ? v->i != 0 : def;
}

// config_of is w's options as its keys say (the state key's span is the
// key's string's).
static ak_window_config config_of(const ak_view *w) {
	ak_window_config c = {0};
	c.width = kfloat(w, AK_KEY_WINDOW_WIDTH), c.height = kfloat(w, AK_KEY_WINDOW_HEIGHT);
	c.min_width = kfloat(w, AK_KEY_WINDOW_MIN_WIDTH), c.min_height = kfloat(w, AK_KEY_WINDOW_MIN_HEIGHT);
	c.max_width = kfloat(w, AK_KEY_WINDOW_MAX_WIDTH), c.max_height = kfloat(w, AK_KEY_WINDOW_MAX_HEIGHT);
	c.x = kfloat(w, AK_KEY_WINDOW_X), c.y = kfloat(w, AK_KEY_WINDOW_Y);
	static const struct {
		uint16_t key;
		bool def;  // its default; the flag is set when it's the other
		uint32_t flags;
	} bools[] = {
		{AK_KEY_WINDOW_RESIZABLE, true, AK_WINDOW_FIXED_SIZE},
		{AK_KEY_WINDOW_MINIMIZABLE, true, AK_WINDOW_NO_MINIMIZE},
		{AK_KEY_WINDOW_MAXIMIZABLE, true, AK_WINDOW_NO_MAXIMIZE},
		{AK_KEY_WINDOW_CLOSABLE, true, AK_WINDOW_NO_CLOSE},
		{AK_KEY_WINDOW_ALWAYS_ON_TOP, false, AK_WINDOW_ALWAYS_ON_TOP},
		{AK_KEY_WINDOW_HIDDEN, false, AK_WINDOW_HIDDEN},
		{AK_KEY_WINDOW_SKIP_TASKBAR, false, AK_WINDOW_SKIP_TASKBAR},
		{AK_KEY_WINDOW_TRANSPARENT, false, AK_WINDOW_TRANSPARENT},
		{AK_KEY_WINDOW_MAXIMIZED, false, AK_WINDOW_MAXIMIZED},
		{AK_KEY_WINDOW_FULL_SCREEN, false, AK_WINDOW_FULLSCREEN},
		{AK_KEY_WINDOW_CENTER, false, AK_WINDOW_CENTERED},
		{AK_KEY_WINDOW_CHILD, false, AK_WINDOW_CHILD},
		{AK_KEY_WINDOW_MODAL, false, AK_WINDOW_CHILD | AK_WINDOW_MODAL},
		{AK_KEY_WINDOW_PANEL, false,
		 AK_WINDOW_PANEL | AK_WINDOW_ALWAYS_ON_TOP | AK_WINDOW_SKIP_TASKBAR | AK_WINDOW_NO_MINIMIZE | AK_WINDOW_NO_MAXIMIZE},
		{AK_KEY_WINDOW_FIT_CONTENT, false, AK_WINDOW_FIT_CONTENT | AK_WINDOW_FIXED_SIZE},
		{AK_KEY_WINDOW_CLICK_THROUGH, false, AK_WINDOW_CLICK_THROUGH},
		{AK_KEY_WINDOW_MENU_BAR, false, AK_WINDOW_MENU_BAR},
	};
	for (size_t i = 0; i < AK_LEN(bools); i++)
		if (kbool(w, bools[i].key, bools[i].def) != bools[i].def) c.flags |= bools[i].flags;
	if (c.x != 0 || c.y != 0) c.flags |= AK_WINDOW_POSITIONED;
	const ak_val *fl = key_of(w, AK_KEY_WINDOW_FLYOUT);
	if (fl && fl->p) {
		c.flags |= AK_WINDOW_FLYOUT | AK_WINDOW_SKIP_TASKBAR | AK_WINDOW_NO_MINIMIZE | AK_WINDOW_NO_MAXIMIZE;
		c.flags &= ~(uint32_t)(AK_WINDOW_PANEL | AK_WINDOW_CHILD | AK_WINDOW_MODAL | AK_WINDOW_CENTERED | AK_WINDOW_POSITIONED |
		                       AK_WINDOW_MAXIMIZED | AK_WINDOW_FULLSCREEN);
	}
	const ak_val *tb = key_of(w, AK_KEY_WINDOW_TITLE_BAR), *bg = key_of(w, AK_KEY_WINDOW_BACKGROUND), *k = key_of(w, AK_KEY_WINDOW_STATE_KEY);
	if (tb) c.title_bar = (uint8_t)tb->i;
	if (bg) c.background = (ak_color)bg->i;
	if (k && k->p) c.state_key = ak_span_str(k->p);
	return c;
}

// send_flyout sends a flyout's anchor and placement (a Flyout struct:
// anchor (Anchor: tray, x, y, width, height), placement, gap), when its
// encoding changed.
static void send_flyout(ak_view *w) {
	window_t *wt = w->impl;
	const ak_val *v = key_of(w, AK_KEY_WINDOW_FLYOUT);
	if (!wt->win || !v || !v->p) return;
	const ak_struct *f = v->p;
	const ak_struct *a = f->f[0].p;
	ak_flyout fo = {.placement = (uint8_t)f->f[1].i, .gap = (float)f->f[2].f};
	if (a) fo.tray = (uint32_t)a->f[0].i, fo.x = (float)a->f[1].f, fo.y = (float)a->f[2].f, fo.width = (float)a->f[3].f,
	fo.height = (float)a->f[4].f;
	ak_buf b = {0};
	ak_append_flyout(&b, &fo);
	if (b.len == wt->flyout.len && memcmp(b.data, wt->flyout.data, b.len) == 0) {
		ak_buf_free(&b);
		return;
	}
	ak_buf_free(&wt->flyout);
	wt->flyout = b;
	ak_batch_set_bytes(w->r->batch, wt->win->id, AK_PROP_WINDOW_FLYOUT, b.data, b.len);
}

// ---- The Window element ----

static void win_view_event(ak_view *e, const ak_event *ev);

static void win_view_adjust(ak_view *e, ak_lstyle *ls) {
	ls->flex_direction = AK_LCOLUMN;
	ak_prov *p = e->prov;
	ak_view *w = e->up;
	if (!p || !p->fit_win || !w) {
		ls->width = ak_lpct(100), ls->height = ak_lpct(100);
		return;
	}
	// Sized to its content, within its least and most size.
	const ak_window_config *c = &((window_t *)w->impl)->cfg;
	if (c->min_width > 0) ls->min_width = ak_lpt(c->min_width);
	if (c->min_height > 0) ls->min_height = ak_lpt(c->min_height);
	if (c->max_width > 0) ls->max_width = ak_lpt(c->max_width);
	if (c->max_height > 0) ls->max_height = ak_lpt(c->max_height);
}

static const ak_el_class cls_win_view = {.name = "Window", .kind = AK_KIND_WINDOW, .flags = AK_ELF_CHILDREN, .event = win_view_event,
                                         .adjust = win_view_adjust};

static void win_view_event(ak_view *e, const ak_event *ev) {
	ak_view *w = e->up;  // its element
	if (!w || w->dead || ((window_t *)w->impl)->win != e) return;
	switch (ev->kind) {
	case AK_EVENT_DISMISSED: destroy(w); break;  // the user closed it
	case AK_EVENT_CLOSE_REQUEST: close_requested(w); break;
	case AK_EVENT_WINDOW_STATE: set_state(w, ev); break;
	case AK_EVENT_LOCATION: ak_router_location(e, ev); break;  // its route, restored by the host
	}
}

static void window_close(ak_view *w) {
	window_t *wt = w->impl;
	if (!wt->win) return;
	ak_view *v = wt->win;
	wt->win = NULL;
	wt->flyout.len = 0;
	wt->h.known = false;
	if (wt->owner) {
		ak_owner *o = wt->owner;
		wt->owner = NULL;
		ak_owner_dispose(o);
	}
	ak_view_destroy(v);
	ak_release(v);
}

static void window_open(ak_view *w) {
	window_t *wt = w->impl;
	if (wt->main || wt->win || !wt->build || w->dead || !w->finalized) return;
	ak_ui_root *r = w->r;
	// The window it's declared in: its parent, for a child window.
	void *pv = NULL;
	ak_view *parent = ak_context_lookup(ak_window_key, &pv) ? handle_view(pv) : NULL;
	if (!parent) parent = r->el;
	ak_window_config c = config_of(w);
	if (c.flags & (AK_WINDOW_CHILD | AK_WINDOW_FLYOUT)) c.parent = parent->id;
	else c.flags &= ~(uint32_t)(AK_WINDOW_CHILD | AK_WINDOW_MODAL);
	wt->cfg = c;
	wt->cfg.state_key = ak_cstr("");  // the span is the key's
	ak_view *e = ak_view_new(r, &cls_win_view, 0, true);
	ak_prov *p = ak_prov_new(r);
	p->fit_win = (c.flags & AK_WINDOW_FIT_CONTENT) != 0;
	e->up = w;
	ak_prov_attach(e, p);
	ak_release(p);
	e->finalized = true;
	wt->win = e;
	ak_buf *b = ak_batch_set_bytes_begin(r->batch, e->id, AK_PROP_WINDOW_CONFIG);
	ak_append_window_config(b, &c);
	ak_batch_set_bytes_end(r->batch);
	send_flyout(w);
	ak_view_listen(e, AK_EVENT_DISMISSED);
	sync_listen(w);
	const ak_val *t = key_of(w, AK_KEY_WINDOW_TITLE), *ed = key_of(w, AK_KEY_WINDOW_EDITED), *fl = key_of(w, AK_KEY_WINDOW_FILE);
	if (t) send_str(e, AK_PROP_WINDOW_TITLE, *t);
	if (ed && ed->i) ak_batch_set_bool(r->batch, e->id, AK_PROP_WINDOW_EDITED, true);
	if (fl) send_str(e, AK_PROP_WINDOW_FILE, *fl);
	// Opened before its content is built: a Window declared in it, a
	// child of it, opens after it.
	ak_batch_command(r->batch, e->id, AK_CMD_PRESENT, NULL, 0);
	wt->owner = ak_root_under(w->owner);
	ak_owner_set_context(wt->owner, ak_window_key, w);
	ak_owner_set_context(wt->owner, ak_env_key, p);
	ak_view *v = ak_build_under(r, wt->owner, wt->build, NULL, 0);
	if (v) {
		ak_view *k = ak_only_kid(r, v);
		ak_view_append(e, k);
		ak_release(k);
	}
}

// ---- The main window ----

// main_shows is whether the main window shows as its keys say: unless
// its open is false.
static bool main_shows(const ak_view *w) { return kbool(w, AK_KEY_WINDOW_OPEN, true); }

static void send_title(ak_ui_root *r, ak_val v) {
	const ak_str *s = v.p;
	ak_batch_command(r->batch, r->el->id, AK_CMD_WINDOW_TITLE, (const uint8_t *)(s ? s->data : ""), s ? s->len : 0);
}

// main_open sends the main window's options (WindowOptions, to the root)
// and builds its content in the root.
static void main_open(ak_view *w) {
	window_t *wt = w->impl;
	if (wt->main_built || w->dead || !w->finalized) return;
	ak_ui_root *r = w->r;
	handle_t *h = main_handle(r);
	if (h->main && h->main != w && !h->main->dead) ak_panic("ui: an app has one main window");
	h->main = w;
	wt->main_built = true;
	ak_window_config c = config_of(w);
	c.flags &= ~(uint32_t)(AK_WINDOW_CHILD | AK_WINDOW_MODAL | AK_WINDOW_FLYOUT);
	wt->main_open = main_shows(w) && !(c.flags & AK_WINDOW_HIDDEN);
	if (!wt->main_open) c.flags |= AK_WINDOW_HIDDEN;
	wt->cfg = c;
	wt->cfg.state_key = ak_cstr("");
	const ak_val *t = key_of(w, AK_KEY_WINDOW_TITLE);
	const ak_str *title = t ? t->p : NULL;
	c.title = title ? ak_span_str(title) : ak_cstr("");
	ak_buf *b = ak_batch_command_begin(r->batch, r->el->id, AK_CMD_WINDOW_OPTIONS);
	ak_append_window_config(b, &c);
	ak_batch_command_end(r->batch);
	ak_ui_root_fit(r, (c.flags & AK_WINDOW_FIT_CONTENT) != 0, c.min_width, c.min_height, c.max_width, c.max_height);
	// Its open follows the host's: the user closing it, in an app that
	// keeps running, hides it.
	if (ak_view_handler(w, AK_EV_WINDOW_CHANGE)) watch(r->el);
	sync_listen(r->el);
	wt->owner = ak_root_under(w->owner);
	ak_owner_set_context(wt->owner, ak_window_key, w);
	ak_view *v = ak_build_under(r, wt->owner, wt->build, NULL, 0);
	if (v) {
		ak_view *k = ak_only_kid(r, v);
		ak_view_append(w, k);
		ak_release(k);
	}
}

// main_visible hears the main window shown or hidden by the host (the
// user closing it, the app opened again): its open follows.
static void main_visible(ak_ui_root *r, bool visible) {
	ak_view *w = main_handle(r)->main;
	if (!w || w->dead) return;
	window_t *wt = w->impl;
	if (wt->main_open == visible) return;
	wt->main_open = visible;
	ak_val v = {.i = visible};
	ak_view_call(w, AK_EV_WINDOW_CHANGE, &v, 1);
}

static void main_set(ak_view *w, uint16_t key, ak_val v) {
	window_t *wt = w->impl;
	ak_ui_root *r = w->r;
	if (!wt->main_built) return;  // its options say it as it opens
	switch (key) {
	case AK_KEY_WINDOW_OPEN:
		if ((v.i != 0) == wt->main_open) break;
		wt->main_open = v.i != 0;
		ak_ui_window_action(w, v.i ? AK_WINDOW_ACTION_SHOW : AK_WINDOW_ACTION_HIDE, 0, 0);
		break;
	case AK_KEY_WINDOW_TITLE: send_title(r, v); break;
	case AK_KEY_WINDOW_ALWAYS_ON_TOP:
		ak_ui_window_action(w, v.i ? AK_WINDOW_ACTION_ALWAYS_ON_TOP : AK_WINDOW_ACTION_NOT_ALWAYS_ON_TOP, 0, 0);
		break;
	case AK_KEY_WINDOW_CLICK_THROUGH:
		ak_ui_window_action(w, v.i ? AK_WINDOW_ACTION_CLICK_THROUGH : AK_WINDOW_ACTION_NOT_CLICK_THROUGH, 0, 0);
		break;
	}
}

// ak_ui_main_window makes w (a Window: the app block's) the main
// window, before its keys are set.
void ak_ui_main_window(ak_view *w) {
	if (w->el != AK_EL_WINDOW) ak_panic("ui: the main window is a Window");
	((window_t *)w->impl)->main = true;
}

// ---- The element's class ----

static void window_cleanup(void *ctx) {
	ak_view *w = ctx;
	window_t *wt = w->impl;
	if (wt->main) {
		handle_t *h = w->r->el ? w->r->el->impl : NULL;
		if (h && h->main == w) h->main = NULL;
		if (wt->owner) {
			ak_owner *o = wt->owner;
			wt->owner = NULL;
			ak_owner_dispose(o);
		}
	} else {
		window_close(w);
	}
	ak_release(w);
}

static void window_init(ak_view *e) {
	e->impl = ak_alloc(sizeof(window_t));
	ak_on_cleanup(window_cleanup, ak_retain(e));
}

static void window_drop(ak_view *e) {
	window_t *wt = e->impl;
	if (!wt) return;
	if (wt->owner) ak_owner_dispose(wt->owner);
	if (wt->win) {
		ak_view_destroy(wt->win);
		ak_release(wt->win);
	}
	ak_release(wt->build);
	ak_buf_free(&wt->flyout);
	handle_free(&wt->h);
	ak_free(wt);
}

static void window_set(ak_view *w, uint16_t key, ak_val v, const ak_val *prev) {
	window_t *wt = w->impl;
	if (wt->main) {
		main_set(w, key, v);
		return;
	}
	ak_view *e = wt->win;
	switch (key) {
	case AK_KEY_WINDOW_OPEN:
		if (v.i) window_open(w);
		else window_close(w);
		break;
	case AK_KEY_WINDOW_TITLE:
		if (e) send_str(e, AK_PROP_WINDOW_TITLE, v);
		break;
	case AK_KEY_WINDOW_EDITED:
		if (e && (prev || v.i)) ak_batch_set_bool(w->r->batch, e->id, AK_PROP_WINDOW_EDITED, v.i != 0);
		break;
	case AK_KEY_WINDOW_FILE:
		if (e) send_str(e, AK_PROP_WINDOW_FILE, v);
		break;
	case AK_KEY_WINDOW_FLYOUT: send_flyout(w); break;  // placed again
	case AK_KEY_WINDOW_ALWAYS_ON_TOP:
		if (e) ak_ui_window_action(w, v.i ? AK_WINDOW_ACTION_ALWAYS_ON_TOP : AK_WINDOW_ACTION_NOT_ALWAYS_ON_TOP, 0, 0);
		break;
	case AK_KEY_WINDOW_CLICK_THROUGH:
		if (e) ak_ui_window_action(w, v.i ? AK_WINDOW_ACTION_CLICK_THROUGH : AK_WINDOW_ACTION_NOT_CLICK_THROUGH, 0, 0);
		break;
	}
}

static void window_finalize(ak_view *w) {
	window_t *wt = w->impl;
	if (wt->main) {
		if (wt->build) main_open(w);
		return;
	}
	const ak_val *o = key_of(w, AK_KEY_WINDOW_OPEN);
	if (o && o->i) window_open(w);
}

static void window_lazy(ak_view *w, ak_closure *build) {
	window_t *wt = w->impl;
	ak_retain(build);
	ak_release(wt->build);
	wt->build = build;
	window_finalize(w);
}

const ak_el_class ak_class_window = {.name = "Window", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT | AK_ELF_FLATTEN | AK_ELF_CHILDREN,
                                     .init = window_init, .set = window_set, .finalize = window_finalize, .lazy = window_lazy,
                                     .drop = window_drop};

// ---- WindowCloseGuard ----

// guard_asks runs a guard: while its when holds, its closeRequest runs
// and the window stays (true).
static bool guard_asks(ak_view *g) {
	if (!kbool(g, AK_KEY_WINDOW_CLOSE_GUARD_WHEN, true)) return false;
	ak_view_call(g, AK_EV_WINDOW_CLOSE_GUARD_CLOSE_REQUEST, NULL, 0);
	return true;
}

static void guard_init(ak_view *e) { add_ask(target(NULL), (ask_t){NULL, e}); }

static const ak_el_class cls_guard = {.name = "WindowCloseGuard", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT, .init = guard_init};

// ---- WindowKeys ----

typedef struct {
	struct ak_keyreg *reg;
} wkeys_t;

// ak_keys_window is where keys registered now go (WindowKeys',
// ui.use_keys'): the view of the Window the calling code is built in, or
// the root for the main window.
ak_view *ak_keys_window(void) {
	ak_view *w = target(NULL);
	ak_view *v = handle_view(w);
	return v ? v : w->r->el;
}

static void wkeys_init(ak_view *e) {
	wkeys_t *k = e->impl = ak_alloc(sizeof(wkeys_t));
	ak_keys_register(ak_keys_window(), NULL, &k->reg);
}

static void wkeys_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(prev);
	wkeys_t *k = e->impl;
	if (key == AK_KEY_WINDOW_KEYS_KEYS && k->reg) ak_keys_update(k->reg, v.p);
}

static void wkeys_drop(ak_view *e) {
	wkeys_t *k = e->impl;
	if (k && k->reg) ak_keys_forget(k->reg);
	ak_free(k);
}

static const ak_el_class cls_wkeys = {.name = "WindowKeys", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT, .init = wkeys_init,
                                      .set = wkeys_set, .drop = wkeys_drop};

const ak_el_class *ak_window_class_of(uint16_t el) {
	switch (el) {
	case AK_EL_WINDOW_CLOSE_GUARD: return &cls_guard;
	case AK_EL_WINDOW_KEYS: return &cls_wkeys;
	}
	return NULL;
}

// ak_window_view reports whether e is a Window's view (its keys are its
// WindowKeys').
bool ak_window_view(const ak_view *e) { return e->cls == &cls_win_view; }
