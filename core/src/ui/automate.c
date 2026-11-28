// automate.c: asuka dev's automation (AK_DEV_AUTOMATE): commands run
// against the app's views as the core has them, on any platform's player,
// for asuka test --gui's flows. A command does what the host would report
// the user doing: tap sends the view's Press, type its field's text
// change (and the text to the field, which shows it), submit its Submit,
// back takes the top screen off the deepest stack, tab chooses a tab by
// its title, scroll scrolls the List that shows a page down (a List has
// views only for the rows the host has bound, those on the screen). Only
// what shows counts: a Stack's top screen, Tabs' selected tab. dump says
// the texts showing; see and notsee check for one. A text is matched all
// of it, a * in it any characters ("Count: *"). Each command answers a
// line: ok, the dump's texts, or "error: why".
//
// The gestures are what a platform's recognizers would report of them:
// doubletap (a double tap, or a zoomable Scroll's zoom), longpress, swipe
// (a row's swipe actions shown, which the next tap chooses from; a
// swipe's handler; a pan; a paging Scroll's next page; a Scroll or List
// scrolled), drag (a List's row moved, a drag dropped on a view that
// takes it, a pan), pinch (a zoomable Scroll's zoom, a pinch's handler).
// focus picks a field, hidekeyboard blurs it; check and value read a
// view's state; openlink, background and foreground are the app's, as the
// system reports them; mask hides views (their opacity) for a
// screenshot, unmask shows them again.
#include "ui/ui_ext.h"
#include "rt/sched.h"
#include "ops/payload.h"

#include <stdio.h>
#include <stdlib.h>

static bool listens(const ak_view *e, uint16_t k) { return k < 128 && (e->listens[k / 64] >> (k % 64)) & 1; }

static bool is(const ak_str *s, ak_span t) { return s && s->len == t.len && memcmp(s->data, t.p, t.len) == 0; }


// The views in the tree's order (the inspector's: the roots, each view's
// children, a List's cells), as a walk calls f until it returns true; a
// Stack's top screen alone, the one that shows.
typedef bool (*visit)(ak_view *e, void *ctx);

typedef struct {
	visit f;
	void *ctx;
	bool stop;
	int depth;
} walk;

static void walk_view(walk *w, ak_view *e);

// A row the list no longer has (deleted, the host not yet told) shows nothing.
static void walk_cell(ak_view *c, void *ctx) {
	if (ak_list_cell_live(c)) walk_view(ctx, c);
}

static void walk_view(walk *w, ak_view *e) {
	if (w->stop || !e || e->dead || w->depth > 256) return;
	if (w->f(e, w->ctx)) {
		w->stop = true;
		return;
	}
	w->depth++;
	// A Stack shows its top screen, Tabs its selected tab: the others
	// aren't on the screen.
	uint32_t from = 0, to = e->kids.len;
	if (e->el == AK_EL_STACK && e->kids.len) from = e->kids.len - 1;
	int64_t sel = ak_tabs_selected(e);
	if (sel >= 0 && sel < (int64_t)e->kids.len) from = (uint32_t)sel, to = (uint32_t)sel + 1;
	for (uint32_t i = from; i < to && !w->stop; i++) walk_view(w, e->kids.data[i]);
	if (e->kind == AK_KIND_LIST && e->el == AK_EL_LIST) ak_list_each_cell(e, walk_cell, w);
	w->depth--;
}

static void walk_all(visit f, void *ctx) {
	ak_ui_root *r = ak_ui.root;
	if (!r) return;
	walk w = {f, ctx, false, 0};
	walk_view(&w, r->el);
	for (uint32_t i = 0; i < r->provs.len && !w.stop; i++) {
		ak_view *v = r->provs.data[i];
		if (v && !v->dead && !v->parent && v != r->el) walk_view(&w, v);
	}
}

// ---- Finding ----

typedef struct {
	ak_span text;
	ak_view *found;
} find;

// glob matches s against p, all of it: a * in p is any run of characters
// (none too), the rest itself.
static bool glob(const char *p, uint32_t pn, const char *s, uint32_t sn) {
	uint32_t pi = 0, si = 0, star = UINT32_MAX, mark = 0;
	while (si < sn) {
		if (pi < pn && p[pi] == '*') {
			star = pi++;
			mark = si;
		} else if (pi < pn && p[pi] == s[si]) {
			pi++, si++;
		} else if (star != UINT32_MAX) {
			pi = star + 1;
			si = ++mark;
		} else {
			return false;
		}
	}
	while (pi < pn && p[pi] == '*') pi++;
	return pi == pn;
}

static bool matches(const ak_str *s, ak_span t) { return s && glob(t.p, t.len, s->data, s->len); }

// key_str is a view's string key, as last set (NULL: none).
static const ak_str *key_str(const ak_view *e, uint16_t key) {
	const ak_val *v = ak_style_get(e, key);
	return v && v->p && ((ak_obj *)v->p)->tag == AK_TAG_STR ? v->p : NULL;
}

static bool find_text(ak_view *e, void *ctx) {
	find *f = ctx;
	// What it shows, or what a screen reader says of it (a Button with
	// only an icon: its label), or a field's placeholder.
	if (matches(e->text, f->text) || matches(e->text2, f->text) || matches(key_str(e, AK_KEY_LABEL), f->text) ||
	    (e->kind == AK_KIND_TEXT_FIELD && matches(key_str(e, AK_KEY_TEXT_FIELD_PLACEHOLDER), f->text))) {
		f->found = e;
		return true;
	}
	return false;
}

// shown is the view whose text (a Text's, a Button's title) is t, all of
// it; a * in t is any characters ("Count: *").
static ak_view *shown(ak_span t) {
	find f = {t, NULL};
	walk_all(find_text, &f);
	return f.found;
}

static bool first_field(ak_view *e, void *ctx) {
	if (e->kind == AK_KIND_TEXT_FIELD) {
		*(ak_view **)ctx = e;
		return true;
	}
	return false;
}

// ---- State ----

// The field typed into last (a tap on one picks it), its ID: a view
// that went is looked up again.
static ak_id typing;

static ak_view *field(void) {
	ak_view *f = typing ? ak_view_of(typing) : NULL;
	if (f && !f->dead) return f;
	f = NULL;
	walk_all(first_field, &f);
	return f;
}

static void send(const ak_view *e, uint16_t kind, ak_span s) { ak_ui_dispatch_event(&(ak_event){.id = e->id, .kind = kind, .s = s}); }

// ---- Commands ----

// The row whose swipe actions a swipe showed (0: none), and which side's:
// the next tap may choose one, as on the screen.
static ak_id revealed;
static bool revealed_leading;

static void tap(ak_span arg, ak_buf *out) {
	ak_view *cell = revealed ? ak_view_of(revealed) : NULL;
	revealed = 0;
	if (cell && !cell->dead) {
		int64_t k = ak_list_cell_action(cell, revealed_leading, arg);
		if (k != INT64_MIN) {
			ak_ui_dispatch_event(&(ak_event){.id = cell->id, .kind = AK_EVENT_ACTION, .i = k});
			ak_buf_put(out, "ok\n", 3);
			return;
		}
	}
	ak_view *v = shown(arg);
	if (!v) {
		ak_buf_printf(out, "error: nothing shows \"%.*s\"\n", (int)arg.len, arg.p);
		return;
	}
	// What takes the press: the view, or the nearest around it that does
	// (a Text in a pressable View, a Button's label).
	for (ak_view *e = v; e; e = e->parent ? e->parent : e->up) {
		if (e->kind == AK_KIND_TEXT_FIELD) {
			typing = e->id;
			send(e, AK_EVENT_FOCUS, (ak_span){0});
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		if (listens(e, AK_EVENT_PRESS)) {
			send(e, AK_EVENT_PRESS, (ak_span){0});
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		if (e->el == AK_EL_TOGGLE && listens(e, AK_EVENT_TOGGLED)) {
			// Flipped, as a tap flips the switch; the core sends the host
			// the new value (it's controlled).
			const ak_val *on = ak_style_get(e, AK_KEY_TOGGLE_ON);
			bool now = on && on->i;
			if (e->r && !e->r->disposed) ak_batch_set_bool(e->r->batch, e->id, AK_PROP_TOGGLE_ON, !now);
			ak_ui_dispatch_event(&(ak_event){.id = e->id, .kind = AK_EVENT_TOGGLED, .b = !now});
			ak_buf_put(out, "ok\n", 3);
			return;
		}
	}
	ak_buf_printf(out, "error: \"%.*s\" shows, but nothing around it takes a press\n", (int)arg.len, arg.p);
}

static void type(ak_span arg, ak_buf *out) {
	ak_view *f = field();
	if (!f) {
		ak_buf_put(out, "error: no text field\n", 21);
		return;
	}
	typing = f->id;
	// The host's field shows it, as the user's typing would have.
	if (f->r && !f->r->disposed) ak_batch_set_string(f->r->batch, f->id, AK_PROP_TEXT_FIELD_VALUE, arg.p, arg.len);
	send(f, AK_EVENT_TEXT_CHANGED, arg);
	ak_buf_put(out, "ok\n", 3);
}

static void submit(ak_buf *out) {
	ak_view *f = field();
	if (!f) {
		ak_buf_put(out, "error: no text field\n", 21);
		return;
	}
	send(f, AK_EVENT_SUBMIT, (ak_span){0});
	ak_buf_put(out, "ok\n", 3);
}

typedef struct {
	ak_view *stack;
	int depth;
	int best;
} deepest;

static bool find_stack(ak_view *e, void *ctx) {
	deepest *d = ctx;
	int depth = 0;
	for (ak_view *p = e; p; p = p->parent) depth++;
	if (ak_nav_screens(e) > 1 && depth >= d->best) {
		d->stack = e;
		d->best = depth;
	}
	return false;
}

static void back(ak_buf *out) {
	deepest d = {NULL, 0, -1};
	walk_all(find_stack, &d);
	if (!d.stack) {
		ak_buf_put(out, "error: no screen to go back from\n", 33);
		return;
	}
	ak_batch_begin();
	ak_nav_pop_top(d.stack);
	ak_sched_run();
	ak_batch_end();
	ak_ui_flush();
	ak_buf_put(out, "ok\n", 3);
}

// scroll: the first List that shows, a page further down: to the row
// after the last the host has bound, as many on as it binds.
static bool first_list(ak_view *e, void *ctx) {
	if (e->kind == AK_KIND_LIST && e->el == AK_EL_LIST) {
		*(ak_view **)ctx = e;
		return true;
	}
	return false;
}

static void scroll(ak_buf *out) {
	ak_view *l = NULL;
	walk_all(first_list, &l);
	if (!l) {
		ak_buf_put(out, "error: no list\n", 15);
		return;
	}
	uint32_t n;
	int64_t last = ak_list_bound(l, &n), count = ak_list_count(l);
	if (last >= count - 1) {
		ak_buf_put(out, "error: the list is at its end\n", 30);
		return;
	}
	int64_t to = last + (n > 1 ? n - 1 : 1);
	if (to > count - 1) to = count - 1;
	ak_batch_begin();
	ak_ui_scroll_to_item(l, to, false);
	ak_batch_end();
	ak_ui_flush();
	ak_buf_put(out, "ok\n", 3);
}

// tab: the Tab titled arg, in any Tabs (shown or not), and its index.
typedef struct {
	ak_span title;
	ak_view *tabs;
	int64_t index;
} tab_find;

static bool find_tab(ak_view *e, void *ctx) {
	tab_find *t = ctx;
	if (e->el != AK_EL_TABS) return false;
	for (uint32_t i = 0; i < e->kids.len; i++) {
		const ak_val *v = ak_style_get(e->kids.data[i], AK_KEY_TAB_TITLE);
		if (v && is(v->p, t->title)) {
			t->tabs = e;
			t->index = i;
			return true;
		}
	}
	return false;
}

static void tab(ak_span arg, ak_buf *out) {
	tab_find t = {arg, NULL, -1};
	walk_all(find_tab, &t);
	if (!t.tabs) {
		ak_buf_printf(out, "error: no tab \"%.*s\"\n", (int)arg.len, arg.p);
		return;
	}
	ak_tabs_select(t.tabs, t.index);
	ak_buf_put(out, "ok\n", 3);
}

// ---- Gestures ----

static ak_view *outer(const ak_view *e) { return e->parent ? e->parent : e->up; }

static void not_shown(ak_span t, ak_buf *out) { ak_buf_printf(out, "error: nothing shows \"%.*s\"\n", (int)t.len, t.p); }

// cut splits "a\tb" at its tab.
static bool cut(ak_span s, ak_span *a, ak_span *b) {
	for (uint32_t i = 0; i < s.len; i++) {
		if (s.p[i] != '\t') continue;
		*a = (ak_span){s.p, i};
		*b = (ak_span){s.p + i + 1, s.len - i - 1};
		return true;
	}
	return false;
}

static bool number(ak_span s, double *v) {
	char b[64];
	if (!s.len || s.len >= sizeof b) return false;
	memcpy(b, s.p, s.len);
	b[s.len] = 0;
	char *end;
	*v = strtod(b, &end);
	return *end == 0 && *v == *v;
}

// pair reads "x,y".
static bool pair(ak_span s, float *x, float *y) {
	for (uint32_t i = 0; i < s.len; i++) {
		double a, b;
		if (s.p[i] == ',' && number((ak_span){s.p, i}, &a) && number((ak_span){s.p + i + 1, s.len - i - 1}, &b)) {
			*x = (float)a, *y = (float)b;
			return true;
		}
	}
	return false;
}

// frame is a view's frame as laid out, in its parent's (0s: none yet).
static ak_lframe frame(const ak_view *v) {
	if (!v->box) return (ak_lframe){0};
	ak_lframe f = ak_lnode_frame(v->box);
	if (f.x != f.x || f.y != f.y || f.w != f.w || f.h != f.h) return (ak_lframe){0};
	return f;
}

// center is a view's middle in the window, as laid out (a scroll view's
// offset aside, a list's row at the list's top).
static void center(const ak_view *v, float *x, float *y) {
	ak_lframe f = frame(v);
	*x = f.w / 2, *y = f.h / 2;
	for (const ak_view *e = v; e; e = outer(e)) {
		f = frame(e);
		*x += f.x, *y += f.y;
	}
}

static void gesture(ak_view *e, ak_event_kind k, uint8_t state, float dx, float dy, float scale) {
	ak_lframe f = frame(e);
	ak_gesture g = {.state = state, .x = f.w / 2 + dx, .y = f.h / 2 + dy, .translation_x = dx, .translation_y = dy, .scale = scale};
	ak_ui_dispatch_event(&(ak_event){.id = e->id, .kind = k, .g = g});
}

// pan drags e by dx, dy: began, moved, ended, as a pan recognizer reports.
static void pan(ak_view *e, float dx, float dy) {
	gesture(e, AK_EVENT_PAN, AK_GESTURE_STATE_BEGAN, 0, 0, 1);
	gesture(e, AK_EVENT_PAN, AK_GESTURE_STATE_CHANGED, dx, dy, 1);
	gesture(e, AK_EVENT_PAN, AK_GESTURE_STATE_ENDED, dx, dy, 1);
}

static void doubletap(ak_span arg, ak_buf *out) {
	ak_view *v = shown(arg);
	if (!v) return not_shown(arg, out);
	for (ak_view *e = v; e; e = outer(e)) {
		if (ak_scroll_zoomable(e)) {
			ak_batch_begin();
			ak_scroll_double_tap(e);
			ak_batch_end();
			ak_ui_flush();
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		if (listens(e, AK_EVENT_DOUBLE_TAP)) {
			ak_lframe f = frame(e);
			ak_ui_dispatch_event(&(ak_event){.id = e->id, .kind = AK_EVENT_DOUBLE_TAP, .x = f.w / 2, .y = f.h / 2});
			ak_buf_put(out, "ok\n", 3);
			return;
		}
	}
	ak_buf_printf(out, "error: \"%.*s\" shows, but nothing around it takes a double tap\n", (int)arg.len, arg.p);
}

static void longpress(ak_span arg, ak_buf *out) {
	ak_view *v = shown(arg);
	if (!v) return not_shown(arg, out);
	for (ak_view *e = v; e; e = outer(e)) {
		if (listens(e, AK_EVENT_LONG_PRESS)) {
			send(e, AK_EVENT_LONG_PRESS, (ak_span){0});
			ak_buf_put(out, "ok\n", 3);
			return;
		}
	}
	ak_buf_printf(out, "error: \"%.*s\" shows, but nothing around it takes a long press\n", (int)arg.len, arg.p);
}

static bool first_scrollable(ak_view *e, void *ctx) {
	if (e->el == AK_EL_SCROLL || (e->kind == AK_KIND_LIST && e->el == AK_EL_LIST)) {
		*(ak_view **)ctx = e;
		return true;
	}
	return false;
}

// list_page scrolls a List a page on (down) or back (up), as a swipe does.
static bool list_page(ak_view *l, bool on) {
	uint32_t n;
	int64_t last = ak_list_bound(l, &n), count = ak_list_count(l), to;
	if (on) {
		if (last >= count - 1) return false;
		to = last + (n > 1 ? n - 1 : 1);
		if (to > count - 1) to = count - 1;
	} else {
		int64_t first = last - (int64_t)n + 1;
		if (first <= 0) return false;
		to = first - (n > 1 ? n - 1 : 1);
		if (to < 0) to = 0;
	}
	ak_batch_begin();
	ak_ui_scroll_to_item(l, to, false);
	ak_batch_end();
	ak_ui_flush();
	return true;
}

// swipe: a finger moving dir (left, right, up, down) across the view
// showing the text, or the screen's first Scroll or List.
static void swipe(ak_span arg, ak_buf *out) {
	uint32_t sp = 0;
	while (sp < arg.len && arg.p[sp] != ' ') sp++;
	ak_span dir = {arg.p, sp}, what = {arg.p + (sp < arg.len ? sp + 1 : sp), sp < arg.len ? arg.len - sp - 1 : 0};
	int d = -1;  // a direction's bit: 0 right, 1 left, 2 up, 3 down (as Swipe's)
	static const char *const dirs[] = {"right", "left", "up", "down"};
	for (int i = 0; i < 4; i++)
		if (dir.len == strlen(dirs[i]) && memcmp(dir.p, dirs[i], dir.len) == 0) d = i;
	if (d < 0) {
		ak_buf_printf(out, "error: swipe %.*s: which way? (left, right, up, down)\n", (int)dir.len, dir.p);
		return;
	}
	ak_view *v = NULL;
	if (what.len) {
		if (!(v = shown(what))) return not_shown(what, out);
	} else {
		walk_all(first_scrollable, &v);
		if (!v) {
			ak_buf_put(out, "error: nothing to swipe: no scroll view or list shows (swipe left on <text>)\n", 77);
			return;
		}
	}
	bool across = d < 2;
	float dx = d == 0 ? 1 : d == 1 ? -1 : 0, dy = d == 2 ? -1 : d == 3 ? 1 : 0;
	for (ak_view *e = v; e; e = outer(e)) {
		// A row's swipe actions: the trailing ones a swipe toward the
		// leading edge shows (left, right to left: right).
		ak_view *cell = across ? ak_list_cell_around(e, NULL, NULL) : NULL;
		if (cell == e) {
			bool leading = (d == 0) != ak_view_rtl(cell);
			if (ak_list_cell_action(cell, leading, (ak_span){0}) != INT64_MIN) {
				revealed = cell->id, revealed_leading = leading;
				ak_buf_put(out, "ok\n", 3);
				return;
			}
		}
		if (listens(e, AK_EVENT_SWIPE)) {
			ak_ui_dispatch_event(&(ak_event){.id = e->id, .kind = AK_EVENT_SWIPE, .i = INT64_C(1) << d});
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		if (listens(e, AK_EVENT_PAN)) {
			ak_lframe f = frame(e);
			float w = f.w > 0 ? f.w : 200, h = f.h > 0 ? f.h : 200;
			pan(e, dx * w * 0.6f, dy * h * 0.6f);
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		if (e->el == AK_EL_SCROLL && ak_scroll_horizontal(e) == across) {
			// The content moves with the finger: a swipe left shows what's
			// to the right (in reading order, right to left: before).
			bool ok;
			ak_batch_begin();
			if (ak_scroll_paging(e)) {
				int delta = across ? ((d == 1) != ak_view_rtl(e) ? 1 : -1) : (d == 2 ? 1 : -1);
				ok = ak_scroll_page_by(e, delta);
			} else {
				ak_lframe f = frame(e);
				float w = f.w * 0.8f, h = f.h * 0.8f;
				ok = ak_scroll_by(e, -dx * w * (ak_view_rtl(e) && across ? -1 : 1), -dy * h);
			}
			ak_batch_end();
			ak_ui_flush();
			if (ok) ak_buf_put(out, "ok\n", 3);
			else ak_buf_put(out, "error: the scroll view is at its end\n", 37);
			return;
		}
		if (e->kind == AK_KIND_LIST && e->el == AK_EL_LIST && !across) {
			if (list_page(e, d == 2)) ak_buf_put(out, "ok\n", 3);
			else ak_buf_put(out, "error: the list is at its end\n", 30);
			return;
		}
	}
	ak_buf_printf(out, "error: nothing there takes a swipe %.*s\n", (int)dir.len, dir.p);
}

static ak_view *dragger(ak_view *v) {
	for (ak_view *e = v; e; e = outer(e))
		if (listens(e, AK_EVENT_DRAGGING)) return e;
	return NULL;
}

static ak_view *dropper(ak_view *v) {
	for (ak_view *e = v; e; e = outer(e))
		if (listens(e, AK_EVENT_DROP)) return e;
	return NULL;
}

// drop drags src's items onto target, as a drag and drop within the app
// reports them: the drag begins, moves over the target, drops, ends.
static void drop(ak_view *src, ak_view *target) {
	ak_id sid = src->id, tid = target->id;
	ak_lframe f = frame(target);
	float x = f.w / 2, y = f.h / 2;
	ak_ui_dispatch_event(&(ak_event){.id = sid, .kind = AK_EVENT_DRAGGING, .i = AK_DRAG_PHASE_BEGAN});
	ak_ui_dispatch_event(&(ak_event){.id = tid, .kind = AK_EVENT_DRAG_OVER, .g = {.state = AK_GESTURE_STATE_BEGAN, .x = x, .y = y}});
	ak_buf b = {0};
	ak_append_drop(&b, &(ak_drop){.x = x, .y = y, .source = sid}, NULL, 0);
	ak_ui_dispatch_event(&(ak_event){.id = tid, .kind = AK_EVENT_DROP, .s = {(const char *)b.data, (uint32_t)b.len}});
	ak_buf_free(&b);
	ak_ui_dispatch_event(&(ak_event){.id = tid, .kind = AK_EVENT_DRAG_OVER, .g = {.state = AK_GESTURE_STATE_CANCELLED, .x = x, .y = y}});
	ak_ui_dispatch_event(&(ak_event){.id = sid, .kind = AK_EVENT_DRAGGING, .i = AK_DRAG_PHASE_DROPPED});
}

// drag: the view showing from to a place: "@x,y" a point in the window,
// "+dx,dy" that far, else the view showing that text.
static void drag(ak_span arg, ak_buf *out) {
	ak_span from, to;
	if (!cut(arg, &from, &to) || !from.len || !to.len) {
		ak_buf_put(out, "error: drag <text>\\t<text, @x,y or +dx,dy>\n", 44);
		return;
	}
	ak_view *v = shown(from);
	if (!v) return not_shown(from, out);
	float sx, sy, tx, ty;
	center(v, &sx, &sy);
	if (to.p[0] == '@' || to.p[0] == '+') {
		if (!pair((ak_span){to.p + 1, to.len - 1}, &tx, &ty)) {
			ak_buf_printf(out, "error: drag to %.*s: want x,y\n", (int)to.len - 1, to.p + 1);
			return;
		}
		if (to.p[0] == '+') tx += sx, ty += sy;
	} else {
		ak_view *t = shown(to);
		if (!t) return not_shown(to, out);
		// A List's row onto another of its rows: moved there.
		ak_view *la = NULL, *lb = NULL;
		int64_t ia = -1, ib = -1;
		if (ak_list_cell_around(v, &la, &ia) && ak_list_cell_around(t, &lb, &ib) && la == lb && ak_list_reorders(la)) {
			ak_batch_begin();
			bool ok = ak_list_move_row(la, ia, ib);
			ak_batch_end();
			ak_ui_flush();
			if (ok) ak_buf_put(out, "ok\n", 3);
			else ak_buf_put(out, "error: the row didn't move\n", 27);
			return;
		}
		// What's dragged onto a view that takes drops.
		ak_view *src = dragger(v), *target = dropper(t);
		if (src && target) {
			drop(src, target);
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		center(t, &tx, &ty);
	}
	for (ak_view *e = v; e; e = outer(e)) {
		if (listens(e, AK_EVENT_PAN)) {
			pan(e, tx - sx, ty - sy);
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		if (e->el == AK_EL_SCROLL) {
			ak_batch_begin();
			bool ok = ak_scroll_by(e, sx - tx, sy - ty);
			ak_batch_end();
			ak_ui_flush();
			if (ok) ak_buf_put(out, "ok\n", 3);
			else ak_buf_put(out, "error: the scroll view is at its end\n", 37);
			return;
		}
	}
	ak_buf_printf(out, "error: \"%.*s\" shows, but nothing around it takes a drag\n", (int)from.len, from.p);
}

// pinch: two fingers apart (scale > 1) or together on the view showing the
// text: a zoomable Scroll zooms by scale, a pinch's handler hears it.
static void pinch(ak_span arg, ak_buf *out) {
	ak_span what, by;
	double scale;
	if (!cut(arg, &what, &by) || !number(by, &scale) || scale <= 0) {
		ak_buf_put(out, "error: pinch <text>\\t<scale>\n", 30);
		return;
	}
	ak_view *v = shown(what);
	if (!v) return not_shown(what, out);
	for (ak_view *e = v; e; e = outer(e)) {
		if (ak_scroll_zoomable(e)) {
			ak_batch_begin();
			ak_ui_zoom_to(e, ak_scroll_zoom(e) * (float)scale, true);
			ak_batch_end();
			ak_ui_flush();
			ak_buf_put(out, "ok\n", 3);
			return;
		}
		if (listens(e, AK_EVENT_PINCH)) {
			gesture(e, AK_EVENT_PINCH, AK_GESTURE_STATE_BEGAN, 0, 0, 1);
			gesture(e, AK_EVENT_PINCH, AK_GESTURE_STATE_CHANGED, 0, 0, (float)scale);
			gesture(e, AK_EVENT_PINCH, AK_GESTURE_STATE_ENDED, 0, 0, (float)scale);
			ak_buf_put(out, "ok\n", 3);
			return;
		}
	}
	ak_buf_printf(out, "error: \"%.*s\" shows, but nothing around it takes a pinch\n", (int)what.len, what.p);
}

// ---- Fields ----

typedef struct {
	ak_span text;
	ak_view *found;
} field_find;

static bool find_field(ak_view *e, void *ctx) {
	field_find *f = ctx;
	if (e->kind != AK_KIND_TEXT_FIELD) return false;
	if (matches(key_str(e, AK_KEY_TEXT_FIELD_PLACEHOLDER), f->text) || matches(key_str(e, AK_KEY_LABEL), f->text) ||
	    matches(ak_field_value(e), f->text) || matches(e->text, f->text)) {
		f->found = e;
		return true;
	}
	return false;
}

// a_field is the text field that shows t: its placeholder, label or text;
// else the field around the view that shows it (a label beside it in a
// row of a form).
static ak_view *a_field(ak_span t) {
	field_find f = {t, NULL};
	walk_all(find_field, &f);
	if (f.found) return f.found;
	ak_view *v = shown(t);
	int up = 0;
	for (ak_view *e = v; e && up < 3; e = outer(e), up++) {
		ak_view *in = NULL;
		walk_view(&(walk){first_field, &in, false, 0}, e);
		if (in) return in;
	}
	return NULL;
}

static void focus(ak_span arg, ak_buf *out) {
	ak_view *f = a_field(arg);
	if (!f) {
		ak_buf_printf(out, "error: no text field shows \"%.*s\"\n", (int)arg.len, arg.p);
		return;
	}
	typing = f->id;
	send(f, AK_EVENT_FOCUS, (ak_span){0});
	ak_buf_put(out, "ok\n", 3);
}

static bool focused_field(ak_view *e, void *ctx) {
	if (ak_field_focused(e)) {
		*(ak_view **)ctx = e;
		return true;
	}
	return false;
}

// hidekeyboard: the field with the keyboard lets it go, as the keyboard's
// dismiss key does.
static void hidekeyboard(ak_buf *out) {
	ak_view *f = NULL;
	walk_all(focused_field, &f);
	if (f) {
		ak_retain(f);
		ak_batch_begin();
		ak_ui_blur(f);
		ak_batch_end();
		ak_ui_flush();
		if (!f->dead && ak_field_focused(f)) send(f, AK_EVENT_BLUR, (ak_span){0});
		ak_release(f);
	}
	ak_buf_put(out, "ok\n", 3);
}

// value: the field (or slider, stepper) that the text names shows the
// pattern.
static void value(ak_span arg, ak_buf *out) {
	ak_span what, want;
	if (!cut(arg, &what, &want)) {
		ak_buf_put(out, "error: value <field>\\t<text>\n", 30);
		return;
	}
	ak_view *f = a_field(what);
	const ak_str *now = f ? ak_field_value(f) : NULL;
	char num[64];
	if (!f) {
		// A slider's or stepper's, by the text around it.
		ak_view *v = shown(what);
		for (ak_view *e = v; e && !f; e = outer(e)) {
			const ak_val *x = e->el == AK_EL_SLIDER ? ak_style_get(e, AK_KEY_SLIDER_VALUE) : e->el == AK_EL_STEPPER ? ak_style_get(e, AK_KEY_STEPPER_VALUE) : NULL;
			if (!x) continue;
			int n = snprintf(num, sizeof num, "%g", x->f);
			if (n > 0 && glob(want.p, want.len, num, (uint32_t)n)) {
				ak_buf_put(out, "ok\n", 3);
			} else {
				ak_buf_printf(out, "error: \"%.*s\" is %s\n", (int)what.len, what.p, num);
			}
			return;
		}
		ak_buf_printf(out, "error: no field shows \"%.*s\"\n", (int)what.len, what.p);
		return;
	}
	ak_span s = now ? (ak_span){now->data, now->len} : (ak_span){"", 0};
	if (glob(want.p, want.len, s.p, s.len)) ak_buf_put(out, "ok\n", 3);
	else ak_buf_printf(out, "error: \"%.*s\" has \"%.*s\"\n", (int)what.len, what.p, (int)s.len, s.p);
}

// ---- State ----

static bool key_on(const ak_view *e, uint16_t key) {
	const ak_val *v = key ? ak_style_get(e, key) : NULL;
	return v && v->i;
}

typedef struct {
	ak_span text;
	int selected;  // -1: no segment or tab is titled it
} titled;

static bool find_titled(ak_view *e, void *ctx) {
	titled *t = ctx;
	if (e->el == AK_EL_SEGMENTED) {
		const ak_val *segs = ak_style_get(e, AK_KEY_SEGMENTED_SEGMENTS), *sel = ak_style_get(e, AK_KEY_SEGMENTED_SELECTED);
		const ak_arr *a = segs ? segs->p : NULL;
		for (uint32_t i = 0; a && i < a->len; i++) {
			if (matches(ak_arr_vals(a)[i].p, t->text)) {
				t->selected = sel && sel->i == (int64_t)i;
				return true;
			}
		}
	}
	if (e->el == AK_EL_TABS) {
		for (uint32_t i = 0; i < e->kids.len; i++) {
			if (matches(key_str(e->kids.data[i], AK_KEY_TAB_TITLE), t->text)) {
				t->selected = ak_tabs_selected(e) == (int64_t)i;
				return true;
			}
		}
	}
	return false;
}

// selected: whether what shows the text is on or chosen (a toggle, a
// segment, a tab, a row, a view said selected); -1 unknown.
static int selected(ak_view *v, ak_span t) {
	titled tt = {t, -1};
	walk_all(find_titled, &tt);
	if (tt.selected >= 0) return tt.selected;
	for (ak_view *e = v; e; e = outer(e)) {
		if (e->el == AK_EL_TOGGLE) return key_on(e, AK_KEY_TOGGLE_ON);
		if (ak_style_get(e, AK_KEY_A11Y_SELECTED)) return key_on(e, AK_KEY_A11Y_SELECTED);
		uint16_t k = ak_ext_key(e, AK_KEY_VIEW_SELECTED);
		if (k && ak_style_get(e, k)) return key_on(e, k);
		ak_view *cell = ak_list_cell_around(e, NULL, NULL);
		if (cell == e) return ak_list_cell_selected(cell);
	}
	return -1;
}

static bool disabled(const ak_view *v) {
	for (const ak_view *e = v; e; e = outer(e))
		if (key_on(e, AK_KEY_DISABLED)) return true;
	return false;
}

// check: enabled, disabled, selected or unselected, what shows the text.
static void check(ak_span arg, ak_buf *out) {
	uint32_t sp = 0;
	while (sp < arg.len && arg.p[sp] != ' ') sp++;
	ak_span what = {arg.p, sp}, t = {arg.p + (sp < arg.len ? sp + 1 : sp), sp < arg.len ? arg.len - sp - 1 : 0};
#define IS(s) (what.len == sizeof s - 1 && memcmp(what.p, s, what.len) == 0)
	bool want_on = IS("enabled") || IS("selected");
	if (!want_on && !IS("disabled") && !IS("unselected")) {
		ak_buf_printf(out, "error: check %.*s: enabled, disabled, selected or unselected\n", (int)what.len, what.p);
		return;
	}
	bool sel = IS("selected") || IS("unselected");
#undef IS
	ak_view *v = shown(t);
	titled tt = {t, -1};
	if (!v && sel) walk_all(find_titled, &tt);
	if (!v && tt.selected < 0) return not_shown(t, out);
	if (sel) {
		int on = v ? selected(v, t) : tt.selected;
		if (on < 0) {
			ak_buf_printf(out, "error: \"%.*s\" isn't something that's selected or not\n", (int)t.len, t.p);
		} else if ((on == 1) == want_on) {
			ak_buf_put(out, "ok\n", 3);
		} else {
			ak_buf_printf(out, "error: \"%.*s\" is %s\n", (int)t.len, t.p, on ? "selected" : "not selected");
		}
		return;
	}
	bool on = !disabled(v);
	if (on == want_on) ak_buf_put(out, "ok\n", 3);
	else ak_buf_printf(out, "error: \"%.*s\" is %s\n", (int)t.len, t.p, on ? "enabled" : "disabled");
}

// ---- The app's: a link it's opened with, the background ----

static void root_event(const ak_event *ev) {
	ak_buf b = {0};
	ak_event_append(&b, ev);
	ak_ui_dispatch(b.data, b.len);
	ak_buf_free(&b);
}

static ak_id root_id(void) { return ak_ui.root && ak_ui.root->el ? ak_ui.root->el->id : 0; }

// ---- Masks ----

// The views a mask hid: their opacity back when it goes.
static AK_VEC(ak_id) masked;

static void mask(ak_span arg, ak_buf *out) {
	ak_view *v = shown(arg);
	if (!v) return not_shown(arg, out);
	ak_view *a = NULL;
	float rect[4];
	if (!v->real && (!ak_view_native_rect(v, &a, rect) || !a)) {
		ak_buf_printf(out, "error: \"%.*s\" has no view of its own to hide\n", (int)arg.len, arg.p);
		return;
	}
	if (!v->real) v = a;
	if (!v->r || v->r->disposed) return not_shown(arg, out);
	ak_batch_begin();
	ak_batch_set_float(v->r->batch, v->id, AK_PROP_OPACITY, 0);
	ak_batch_end();
	ak_ui_flush();
	ak_vec_push(&masked, v->id);
	ak_buf_put(out, "ok\n", 3);
}

static void unmask(ak_buf *out) {
	ak_batch_begin();
	for (uint32_t i = 0; i < masked.len; i++) {
		ak_view *v = ak_view_of(masked.data[i]);
		if (!v || v->dead || !v->r || v->r->disposed) continue;
		const ak_val *o = ak_style_get(v, AK_KEY_OPACITY);
		ak_batch_set_float(v->r->batch, v->id, AK_PROP_OPACITY, o ? (float)o->f : 1);
	}
	ak_batch_end();
	ak_ui_flush();
	ak_vec_free(&masked);
	ak_buf_put(out, "ok\n", 3);
}

typedef struct {
	ak_buf *out;
} dumping;

static void line(ak_buf *b, const ak_str *s) {
	if (!s || !s->len) return;
	for (uint32_t i = 0; i < s->len; i++) ak_buf_u8(b, s->data[i] == '\n' ? ' ' : s->data[i]);
	ak_buf_u8(b, '\n');
}

static bool dump_view(ak_view *e, void *ctx) {
	dumping *d = ctx;
	line(d->out, e->text);
	line(d->out, e->text2);
	return false;
}

void ak_ui_automate(ak_span commands, ak_buf *out) {
	uint32_t i = 0;
	while (i < commands.len) {
		uint32_t j = i;
		while (j < commands.len && commands.p[j] != '\n') j++;
		ak_span l = {commands.p + i, j - i};
		i = j + 1;
		while (l.len && (l.p[l.len - 1] == '\r' || l.p[l.len - 1] == ' ')) l.len--;
		if (!l.len) continue;
		uint32_t sp = 0;
		while (sp < l.len && l.p[sp] != ' ') sp++;
		ak_span verb = {l.p, sp}, arg = {l.p + (sp < l.len ? sp + 1 : sp), sp < l.len ? l.len - sp - 1 : 0};
#define VERB(s) (verb.len == sizeof s - 1 && memcmp(verb.p, s, verb.len) == 0)
		// A tap after a swipe may choose the row's swipe action; anything
		// else puts the row back.
		if (!VERB("tap")) revealed = 0;
		if (VERB("tap")) tap(arg, out);
		else if (VERB("doubletap")) doubletap(arg, out);
		else if (VERB("longpress")) longpress(arg, out);
		else if (VERB("swipe")) swipe(arg, out);
		else if (VERB("drag")) drag(arg, out);
		else if (VERB("pinch")) pinch(arg, out);
		else if (VERB("focus")) focus(arg, out);
		else if (VERB("hidekeyboard")) hidekeyboard(out);
		else if (VERB("check")) check(arg, out);
		else if (VERB("value")) value(arg, out);
		else if (VERB("mask")) mask(arg, out);
		else if (VERB("unmask")) unmask(out);
		else if (VERB("openlink")) {
			root_event(&(ak_event){.id = root_id(), .kind = AK_EVENT_INCOMING_URL, .s = arg});
			ak_buf_put(out, "ok\n", 3);
		} else if (VERB("background") || VERB("foreground")) {
			root_event(&(ak_event){.id = root_id(), .kind = AK_EVENT_LIFECYCLE, .i = VERB("background") ? AK_LIFECYCLE_BACKGROUND : AK_LIFECYCLE_FOREGROUND});
			ak_buf_put(out, "ok\n", 3);
		} else if (VERB("type")) type(arg, out);
		else if (VERB("submit")) submit(out);
		else if (VERB("back")) back(out);
		else if (VERB("tab")) tab(arg, out);
		else if (VERB("scroll")) scroll(out);
		else if (VERB("dump")) {
			dumping d = {out};
			walk_all(dump_view, &d);
		} else if (VERB("see")) {
			if (shown(arg)) ak_buf_put(out, "ok\n", 3);
			else ak_buf_printf(out, "error: nothing shows \"%.*s\"\n", (int)arg.len, arg.p);
		} else if (VERB("notsee")) {
			if (!shown(arg)) ak_buf_put(out, "ok\n", 3);
			else ak_buf_printf(out, "error: \"%.*s\" shows\n", (int)arg.len, arg.p);
		} else {
			ak_buf_printf(out, "error: no command %.*s (tap, doubletap, longpress, swipe, drag, pinch, type, focus, submit, hidekeyboard, back, tab, scroll, see, notsee, check, value, openlink, background, foreground, mask, unmask, dump)\n", (int)verb.len, verb.p);
		}
#undef VERB
	}
}
