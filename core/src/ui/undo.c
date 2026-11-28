// undo.c: the app's undo stack (the prelude's undo: undo.register,
// undo.group, undo.undo, undo.redo, canUndo, canRedo, undoName,
// redoName, clear), as NSUndoManager's and UndoManager's are, which the
// platform's Edit > Undo and Redo, their shortcuts and a phone's shake
// reach through the root's Undo events; a field being edited keeps its
// own typing's.
//
// An entry is a change the app registered: its name ("Rename") and the
// functions that undo and redo it; a group's entry is the changes its
// block registered, undone last first and redone first first. Undoing
// moves the entry to the redo stack, redoing back; a new change clears
// the redo stack. What the functions register as they run is theirs, not
// the stack's. Each change goes to the host as UndoState (enabling and
// naming its Undo and Redo), and to the MenuBar's undo and redo items.
#include "ui/ui_int.h"
#include "vm/vm.h"

typedef struct {
	ak_closure *undo, *redo;  // retained
	ak_owner *owner;          // where it was registered (retained)
} uaction;

typedef struct {
	ak_str *name;  // retained
	AK_VEC(uaction) acts;
} uentry;

typedef AK_VEC(uentry) uentry_vec;

typedef struct ak_undo {
	uentry_vec done, undone;
	ak_node sig;  // the stacks changed: canUndo's and the rest's readers
	int group;    // groups open
	uentry open;  // the outermost group's changes
	bool running; // an entry's functions run: what they register isn't the stack's
	bool sent;
	ak_buf state_sent;
} ak_undo;

// The most the stack keeps: the oldest go first.
#define UNDO_LEVELS 1000

static ak_undo *undo_of(ak_ui_root *r) {
	if (!r) return NULL;
	if (!r->undo) {
		r->undo = ak_alloc(sizeof(ak_undo));
		ak_signal_init(&r->undo->sig);
	}
	return r->undo;
}

static void entry_free(uentry *e) {
	for (uint32_t i = 0; i < e->acts.len; i++) {
		ak_release(e->acts.data[i].undo);
		ak_release(e->acts.data[i].redo);
		if (e->acts.data[i].owner) ak_owner_release(e->acts.data[i].owner);
	}
	ak_vec_free(&e->acts);
	ak_release(e->name);
	e->name = NULL;
}

static void stack_clear(uentry_vec *s) {
	for (uint32_t i = 0; i < s->len; i++) entry_free(&s->data[i]);
	s->len = 0;
}

void ak_undo_free(ak_ui_root *r) {
	ak_undo *u = r->undo;
	if (!u) return;
	r->undo = NULL;
	stack_clear(&u->done);
	stack_clear(&u->undone);
	ak_vec_free(&u->done);
	ak_vec_free(&u->undone);
	entry_free(&u->open);
	ak_signal_drop(&u->sig);
	ak_buf_free(&u->state_sent);
	ak_free(u);
}

static const ak_str *top_name(const uentry_vec *s) { return s->len ? s->data[s->len - 1].name : NULL; }

// changed tells the readers, the host and the menu bar.
static void changed(ak_ui_root *r, ak_undo *u) {
	ak_signal_write(&u->sig);
	if (!r->el || r->el->dead) return;
	ak_buf b = {0};
	const ak_str *un = top_name(&u->done), *rn = top_name(&u->undone);
	ak_buf_u8(&b, (uint8_t)((u->done.len ? 1 : 0) | (u->undone.len ? 2 : 0)));
	ak_buf_str(&b, un ? un->data : "", un ? un->len : 0);
	ak_buf_str(&b, rn ? rn->data : "", rn ? rn->len : 0);
	if (!u->sent || b.len != u->state_sent.len || memcmp(b.data, u->state_sent.data, b.len) != 0) {
		if (!u->sent) ak_view_listen(r->el, AK_EVENT_UNDO);
		u->sent = true;
		u->state_sent.len = 0;
		ak_buf_put(&u->state_sent, b.data, b.len);
		ak_batch_command(r->batch, r->el->id, AK_CMD_UNDO_STATE, b.data, b.len);
		ak_menubar_resync(r);
	}
	ak_buf_free(&b);
}

static void push_done(ak_undo *u, uentry e) {
	if (u->done.len >= UNDO_LEVELS) {
		entry_free(&u->done.data[0]);
		memmove(u->done.data, u->done.data + 1, (u->done.len - 1) * sizeof(uentry));
		u->done.len--;
	}
	ak_vec_push(&u->done, e);
}

void ak_undo_register(ak_str *name, ak_closure *undo, ak_closure *redo) {
	ak_ui_root *r = ak_ui.root;
	ak_undo *u = undo_of(r);
	if (!u || u->running || !undo) return;
	ak_owner *o = ak_current_owner();
	if (o) ak_owner_retain(o);
	uaction a = {ak_retain(undo), redo ? ak_retain(redo) : NULL, o};
	if (u->group) {
		if (!u->open.name || !u->open.name->len) {
			ak_release(u->open.name);
			u->open.name = name ? ak_retain(name) : NULL;
		}
		ak_vec_push(&u->open.acts, a);
		return;
	}
	uentry e = {name ? ak_retain(name) : NULL, {0}};
	ak_vec_push(&e.acts, a);
	push_done(u, e);
	stack_clear(&u->undone);
	changed(r, u);
}

void ak_undo_group(ak_str *name, ak_closure *body) {
	ak_ui_root *r = ak_ui.root;
	ak_undo *u = undo_of(r);
	if (!body) return;
	if (!u || u->running) {
		ak_val v = ak_call(body, NULL, 0);
		if (!ak_error_pending()) ak_release(v.p);
		return;
	}
	if (u->group++ == 0) {
		entry_free(&u->open);
		u->open.name = name && name->len ? ak_retain(name) : NULL;
	}
	ak_retain(body);
	ak_val v = ak_call(body, NULL, 0);
	if (!ak_error_pending()) ak_release(v.p);
	ak_release(body);
	if (--u->group > 0) return;
	// What the block registered is one change, even when it threw.
	if (!u->open.acts.len) {
		entry_free(&u->open);
		return;
	}
	push_done(u, u->open);
	u->open = (uentry){0};
	stack_clear(&u->undone);
	changed(r, u);
}

static void run_one(ak_closure *f, ak_owner *o) {
	if (!f) return;
	ak_retain(f);
	ak_val v = ak_ui_call_under(o && !ak_owner_disposed(o) ? o : NULL, f, NULL, 0);
	if (!ak_error_pending()) ak_release(v.p);
	else ak_ui_raise_pending(o);
	ak_release(f);
}

// step undoes the newest change (redo: redoes the newest undone).
static bool step(ak_ui_root *r, bool redo) {
	ak_undo *u = r ? r->undo : NULL;
	if (!u || u->running || u->group) return false;
	uentry_vec *from = redo ? &u->undone : &u->done, *to = redo ? &u->done : &u->undone;
	if (!from->len) return false;
	uentry e = from->data[--from->len];
	u->running = true;
	if (redo)
		for (uint32_t i = 0; i < e.acts.len; i++) run_one(e.acts.data[i].redo, e.acts.data[i].owner);
	else
		for (uint32_t i = e.acts.len; i-- > 0;) run_one(e.acts.data[i].undo, e.acts.data[i].owner);
	u->running = false;
	if (r->undo != u) {  // the root went while they ran
		entry_free(&e);
		return true;
	}
	ak_vec_push(to, e);
	changed(r, u);
	return true;
}

bool ak_undo_undo(void) { return step(ak_ui.root, false); }
bool ak_undo_redo(void) { return step(ak_ui.root, true); }

void ak_undo_clear(void) {
	ak_ui_root *r = ak_ui.root;
	ak_undo *u = r ? r->undo : NULL;
	if (!u || (!u->done.len && !u->undone.len)) return;
	stack_clear(&u->done);
	stack_clear(&u->undone);
	changed(r, u);
}

bool ak_undo_peek(const ak_ui_root *r, bool redo) {
	const ak_undo *u = r ? r->undo : NULL;
	return u && (redo ? u->undone.len : u->done.len) > 0;
}

// The readers' (tracked).
static ak_undo *read_undo(void) {
	ak_undo *u = undo_of(ak_ui.root);
	if (u) ak_signal_read(&u->sig);
	return u;
}

bool ak_undo_can(bool redo) {
	ak_undo *u = read_undo();
	return u && (redo ? u->undone.len : u->done.len) > 0;
}

ak_str *ak_undo_name(bool redo) {
	ak_undo *u = read_undo();
	const ak_str *n = u ? top_name(redo ? &u->undone : &u->done) : NULL;
	return n ? ak_retain((void *)n) : ak_str_empty();
}

// ak_undo_event hears the host's Undo on the root: 0 undo, 1 redo.
void ak_undo_event(ak_ui_root *r, int64_t which) { step(r, which == 1); }

// ---- Builtins (CALLB) ----
//
//	undo.register   name: string, undo: fn(), redo: fn()   -
//	undo.group      name: string, body: fn()               -
//	undo.undo       -                                      bool
//	undo.redo       -                                      bool
//	undo.can_undo   -                                      bool (tracked)
//	undo.can_redo   -                                      bool (tracked)
//	undo.undo_name  -                                      string (tracked)
//	undo.redo_name  -                                      string (tracked)
//	undo.clear      -                                      -

static ak_val b_register(ak_val *a, uint32_t n) {
	if (n >= 3) ak_undo_register(a[0].p, a[1].p, a[2].p);
	return (ak_val){0};
}

static ak_val b_group(ak_val *a, uint32_t n) {
	if (n >= 2) ak_undo_group(a[0].p, a[1].p);
	return (ak_val){0};
}

static ak_val b_undo(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_undo_undo()};
}

static ak_val b_redo(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_undo_redo()};
}

static ak_val b_can_undo(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_undo_can(false)};
}

static ak_val b_can_redo(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_undo_can(true)};
}

static ak_val b_undo_name(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_undo_name(false)};
}

static ak_val b_redo_name(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_undo_name(true)};
}

static ak_val b_clear(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_undo_clear();
	return (ak_val){0};
}

void ak_undo_builtins_init(void) {
	static const struct {
		const char *name;
		ak_builtin_fn fn;
	} t[] = {
		{"undo.register", b_register}, {"undo.group", b_group},       {"undo.undo", b_undo},
		{"undo.redo", b_redo},         {"undo.can_undo", b_can_undo}, {"undo.can_redo", b_can_redo},
		{"undo.undo_name", b_undo_name}, {"undo.redo_name", b_redo_name}, {"undo.clear", b_clear},
	};
	for (size_t i = 0; i < AK_LEN(t); i++) ak_builtin_register(t[i].name, t[i].fn);
}
