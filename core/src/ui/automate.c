// automate.c: asuka dev's automation (AK_DEV_AUTOMATE): commands run
// against the app's views as the core has them, on any platform's player,
// for asuka test --gui's flows. A command does what the host would report
// the user doing: tap sends the view's Press, type its field's text
// change (and the text to the field, which shows it), submit its Submit,
// back takes the top screen off the deepest stack. dump says the texts
// showing; see and notsee check for one. Each command answers a line: ok,
// the dump's texts, or "error: why".
#include "ui/ui_ext.h"
#include "rt/sched.h"

#include <stdio.h>

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

static void walk_cell(ak_view *c, void *ctx) { walk_view(ctx, c); }

static void walk_view(walk *w, ak_view *e) {
	if (w->stop || !e || e->dead || w->depth > 256) return;
	if (w->f(e, w->ctx)) {
		w->stop = true;
		return;
	}
	w->depth++;
	// A Stack shows its top screen: those under it aren't on the screen.
	uint32_t from = e->el == AK_EL_STACK && e->kids.len ? e->kids.len - 1 : 0;
	for (uint32_t i = from; i < e->kids.len && !w->stop; i++) walk_view(w, e->kids.data[i]);
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
	bool partly;  // the text is in it, not all of it
} find;

static bool contains(const ak_str *s, ak_span t) {
	if (!s || t.len == 0 || s->len < t.len) return false;
	for (uint32_t i = 0; i + t.len <= s->len; i++)
		if (memcmp(s->data + i, t.p, t.len) == 0) return true;
	return false;
}

static bool find_text(ak_view *e, void *ctx) {
	find *f = ctx;
	if (is(e->text, f->text) || is(e->text2, f->text)) {
		f->found = e;
		return true;
	}
	return false;
}

static bool find_part(ak_view *e, void *ctx) {
	find *f = ctx;
	if (contains(e->text, f->text) || contains(e->text2, f->text)) {
		f->found = e;
		f->partly = true;
		return true;
	}
	return false;
}

// shown is the view whose text (a Text's, a Button's title) is t, or that
// has it in its text.
static ak_view *shown(ak_span t) {
	find f = {t, NULL, false};
	walk_all(find_text, &f);
	if (!f.found) walk_all(find_part, &f);
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

static void tap(ak_span arg, ak_buf *out) {
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
		if (VERB("tap")) tap(arg, out);
		else if (VERB("type")) type(arg, out);
		else if (VERB("submit")) submit(out);
		else if (VERB("back")) back(out);
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
			ak_buf_printf(out, "error: no command %.*s (tap, type, submit, back, see, notsee, dump)\n", (int)verb.len, verb.p);
		}
#undef VERB
	}
}
