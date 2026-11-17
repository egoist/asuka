// focus.c: the keyboard's focus, as the core follows it, and what follows
// it: the focused and focusVisible states, a container's focus keys
// (focusable, pressKeepsFocus, hideFocusRing, trapFocus, restoreFocus,
// autofocus), and the accessibility keys that name other views
// (a11yControls, a11yLabelledBy, a11yDescribedBy).
//
// The host reports FocusChange for every view that takes the keyboard's
// focus or loses it (a field reports Focus and Blur): the core keeps the
// view that has it, per root, and its states follow:
//
//   - focused: the view and every view around it (CSS's :focus and
//     :focus-within), so a field's frame styles itself while the field
//     inside it has the focus;
//   - focusVisible: the same views, while the focus came from the
//     keyboard (or is in a field, which shows its caret): CSS's
//     :focus-visible, what a focus ring draws for.
//
// States are kept only on views that have their interaction state (ix):
// a view that asks for one later starts from where the focus is
// (ak_focus_state_now). A focus change allocates nothing.
#include "ui/ui_ext.h"

// within reports whether v is a or inside it (its parents, up to the
// layout root it's in).
static bool within(const ak_view *v, const ak_view *a) {
	for (; v; v = v->parent)
		if (v == a) return true;
	return false;
}

static bool focus_bit(uint8_t bit) { return bit == AK_STATE_FOCUSED || bit == AK_STATE_FOCUS_VISIBLE; }

// focus_of is the view with the focus in x (NULL: none, or gone).
static ak_view *focus_of(const ak_rext *x) {
	if (!x || !x->focus) return NULL;
	ak_view *v = ak_view_of(x->focus);
	return v && !v->dead ? v : NULL;
}

bool ak_focus_state_now(ak_view *e, uint8_t bit) {
	if (!focus_bit(bit) || !e->r) return false;
	ak_rext *x = e->r->ext;
	ak_view *f = focus_of(x);
	if (!f || !within(f, e)) return false;
	return bit == AK_STATE_FOCUSED || x->focus_visible;
}

// chain sets the focus states of f's chain (f and the views around it):
// on, visible as vis; or off, for the views not around keep (NULL: all).
static void chain(ak_view *f, bool on, bool vis, const ak_view *keep) {
	for (ak_view *v = f; v; v = v->parent) {
		if (!v->ix || v->dead) continue;
		if (on) {
			ak_ix_set_state(v, AK_STATE_FOCUSED, true);
			ak_ix_set_state(v, AK_STATE_FOCUS_VISIBLE, vis);
		} else if (!keep || !within(keep, v)) {
			ak_ix_set_state(v, AK_STATE_FOCUSED, false);
			ak_ix_set_state(v, AK_STATE_FOCUS_VISIBLE, false);
		}
	}
}

static void call_change(ak_view *e, bool on) {
	if (e->dead) return;
	uint16_t ev = ak_ext_ev(e, AK_EV_VIEW_FOCUS_CHANGE);
	if (!ev || !ak_view_handler(e, ev)) return;
	ak_val arg = {.i = on};
	ak_view_call(e, ev, &arg, 1);
}

void ak_focus_report(ak_view *e, int how) {
	if (!e || !e->r || !e->real) return;
	ak_rext *x = ak_rext_of(e->r);
	if (how == 0) {
		if (x->focus != e->id) return;  // another took it already
		x->focus = 0;
		chain(e, false, false, NULL);
		ak_overlay_focus_moved(e->r, NULL);
		call_change(e, false);
		return;
	}
	bool vis = how == 2 || e->el == AK_EL_TEXT_FIELD;
	ak_view *old = focus_of(x);
	if (old == e && x->focus_visible == vis) return;
	ak_retain(e);
	x->focus = e->id;
	x->focus_visible = vis;
	if (old && old != e) chain(old, false, false, e);
	chain(e, true, vis, NULL);
	if (old != e) {
		ak_overlay_focus_moved(e->r, e);
		if (old) call_change(old, false);
		call_change(e, true);
	}
	ak_release(e);
}

void ak_focus_destroyed(ak_view *e) {
	ak_rext *x = e->r ? e->r->ext : NULL;
	if (!x || !x->focus || x->focus != e->id) return;
	x->focus = 0;
	chain(e, false, false, NULL);
}

ak_view *ak_focus_current(ak_ui_root *r) { return focus_of(r ? r->ext : NULL); }

void ak_focus_forget(ak_ui_root *r) {
	ak_rext *x = r ? r->ext : NULL;
	if (!x) return;
	x->focus = 0;
	ak_vec_free(&x->rels);
}

// ---- A view that can take the focus ----

static const ak_val *ckey(const ak_view *e, uint16_t view_key) {
	uint16_t k = ak_ext_key(e, view_key);
	return k ? ak_style_get(e, k) : NULL;
}

static bool ckey_on(const ak_view *e, uint16_t view_key) {
	const ak_val *v = ckey(e, view_key);
	return v && v->i;
}

// takes_focus reports whether the host gives v the keyboard's focus: a
// focusable, pressable or adjustable container, a field, a control.
static bool takes_focus(const ak_view *v) {
	if (!v->real || v->dead) return false;
	switch (v->el) {
	case AK_EL_TEXT_FIELD:
	case AK_EL_BUTTON:
	case AK_EL_TOGGLE:
	case AK_EL_SLIDER:
	case AK_EL_STEPPER:
	case AK_EL_SEGMENTED:
	case AK_EL_PICKER:
	case AK_EL_WHEEL:
	case AK_EL_COLOR_PICKER:
	case AK_EL_DATE_PICKER:
		return !ak_style_get(v, AK_KEY_DISABLED) || !ak_style_get(v, AK_KEY_DISABLED)->i;
	}
	if (ak_ext_key(v, AK_KEY_VIEW_FOCUSABLE) == 0) return false;
	const ak_val *d = ak_style_get(v, AK_KEY_DISABLED);
	if (d && d->i) return false;
	return ckey_on(v, AK_KEY_VIEW_FOCUSABLE) || ak_view_listening(v, AK_EVENT_PRESS) || ak_view_listening(v, AK_EVENT_ADJUST);
}

// first_focusable is v if it takes the focus, else the first view inside
// it that does, in the tree's order.
static ak_view *first_focusable(ak_view *v) {
	if (v->dead) return NULL;
	if (takes_focus(v)) return v;
	for (uint32_t i = 0; i < v->kids.len; i++) {
		ak_view *f = first_focusable(v->kids.data[i]);
		if (f) return f;
	}
	return NULL;
}

void ak_focus_autofocus(ak_view *e) {
	ak_view *t = first_focusable(e);
	if (t) ak_batch_command(t->r->batch, t->id, AK_CMD_FOCUS, NULL, 0);
}

// restore_fn gives the focus back to the view that had it when a
// restoreFocus view came, as its owner is disposed.
static void restore_fn(void *ctx) {
	ak_view *e = ctx;
	ak_ix *ix = e->ix;
	ak_view *t = ix ? ix->restore : NULL;
	if (ix) ix->restore = NULL;
	if (t) {
		// Only if the focus is still in what's going (or nowhere): a view
		// the user moved the focus to keeps it.
		ak_view *now = ak_focus_current(t->r);
		if (!t->dead && t->real && (!now || within(now, e) || now->dead)) ak_ui_focus(t);
		ak_release(t);
	}
	ak_release(e);
}

typedef struct {
	ak_view *e;
} restore_ctx;

static void restore_hook(void *ctx) { ak_on_cleanup(restore_fn, ak_retain(((restore_ctx *)ctx)->e)); }

static void restore_arm(ak_view *e) {
	ak_ix *ix = ak_ix_of(e);
	if (ix->restore || !e->owner || ak_owner_disposed(e->owner)) return;
	ak_view *f = ak_focus_current(e->r);
	if (!f || within(f, e)) return;
	ix->restore = ak_retain(f);
	restore_ctx c = {e};
	ak_with_owner(e->owner, restore_hook, &c);
}

static void send_bool(ak_view *e, ak_prop p, bool on, const ak_val *prev) {
	if (!on && !prev) return;
	if (!e->real && (e->cls->flags & AK_ELF_FLATTEN)) {
		if (!on) return;
		ak_view_make_real(e);
	}
	if (e->real) ak_batch_set_bool(ak_view_batch(e), e->id, p, on);
}

bool ak_focus_container_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	if (key == ak_ext_key(e, AK_KEY_VIEW_FOCUSABLE)) send_bool(e, AK_PROP_FOCUSABLE, v.i != 0, prev);
	else if (key == ak_ext_key(e, AK_KEY_VIEW_PRESS_KEEPS_FOCUS)) send_bool(e, AK_PROP_NO_FOCUS_ON_PRESS, v.i != 0, prev);
	else if (key == ak_ext_key(e, AK_KEY_VIEW_HIDE_FOCUS_RING)) send_bool(e, AK_PROP_NO_FOCUS_RING, v.i != 0 || ak_ix_styles_state(e, AK_STATE_FOCUS_VISIBLE), prev);
	else if (key == ak_ext_key(e, AK_KEY_VIEW_TRAP_FOCUS)) send_bool(e, AK_PROP_FOCUS_TRAP, v.i != 0, prev);
	else if (key == ak_ext_key(e, AK_KEY_VIEW_RESTORE_FOCUS)) {
		if (v.i) restore_arm(e);
	} else if (key == ak_ext_key(e, AK_KEY_VIEW_AUTOFOCUS)) {
		if (v.i && !(prev && prev->i)) {
			if (!e->real && (e->cls->flags & AK_ELF_FLATTEN)) ak_view_make_real(e);  // its first frame says it's placed
			e->autofocus = true;
		}
	} else {
		return false;
	}
	return true;
}

void ak_focus_ring_styled(ak_view *e) {
	if (!e->real && (e->cls->flags & AK_ELF_FLATTEN)) return;  // sent when it gets its view (ak_focus_view_made)
	if (e->real) ak_batch_set_bool(ak_view_batch(e), e->id, AK_PROP_NO_FOCUS_RING, true);
}

void ak_focus_view_made(ak_view *e) {
	if (ak_ext_key(e, AK_KEY_VIEW_FOCUSABLE) == 0) return;
	if (ckey_on(e, AK_KEY_VIEW_HIDE_FOCUS_RING)) return;  // sent with its keys
	if (ak_ix_styles_state(e, AK_STATE_FOCUS_VISIBLE)) ak_batch_set_bool(ak_view_batch(e), e->id, AK_PROP_NO_FOCUS_RING, true);
}

// ---- Accessibility relations ----

// The relations' keys, and the props they're sent as.
static const uint16_t rel_keys[3] = {AK_KEY_A11Y_CONTROLS, AK_KEY_A11Y_LABELLED_BY, AK_KEY_A11Y_DESCRIBED_BY};
static const ak_prop rel_props[3] = {AK_PROP_A11Y_CONTROLS, AK_PROP_A11Y_LABELLED_BY, AK_PROP_A11Y_DESCRIBED_BY};

static int rel_index(uint16_t key) {
	for (int i = 0; i < 3; i++)
		if (rel_keys[i] == key) return i;
	return -1;
}

bool ak_focus_rel_key(uint16_t key) { return rel_index(key) >= 0; }

// rel_id is the native view a relation's Ref holds now: 0 for none, or
// one with no view (yet).
static ak_id rel_id(const ak_val *v) {
	if (!v || !v->p) return 0;
	ak_val held = ak_vm_cell_get(v->p, false);
	const ak_view *t = held.p;
	if (!t || ((const ak_obj *)t)->tag != AK_TAG_VIEW || t->dead) return 0;
	// A view that makes none of its own is labelled by the first that does.
	while (t && !t->real && t->kids.len) t = t->kids.data[0];
	return t && t->real ? t->id : 0;
}

static void rel_send(ak_view *e) {
	ak_ix *ix = ak_ix_of(e);
	for (int i = 0; i < 3; i++) {
		ak_id id = rel_id(ak_style_get(e, rel_keys[i]));
		if (id == ix->rel_sent[i]) continue;
		ix->rel_sent[i] = id;
		if (e->real) ak_batch_set_int(ak_view_batch(e), e->id, rel_props[i], (int64_t)id);
	}
}

void ak_focus_rel_set(ak_view *e, uint16_t key) {
	AK_UNUSED(key);
	if (!e->real && (e->cls->flags & AK_ELF_FLATTEN)) ak_view_make_real(e);
	if (!e->real) return;
	ak_rext *x = ak_rext_of(e->r);
	bool known = false;
	for (uint32_t i = 0; i < x->rels.len && !known; i++) known = x->rels.data[i] == e->id;
	if (!known) ak_vec_push(&x->rels, e->id);
	rel_send(e);
}

void ak_focus_rels_flush(ak_ui_root *r) {
	ak_rext *x = r->ext;
	if (!x) return;
	uint32_t live = 0;
	for (uint32_t i = 0; i < x->rels.len; i++) {
		ak_view *e = ak_view_of(x->rels.data[i]);
		if (!e || e->dead) continue;
		x->rels.data[live++] = x->rels.data[i];
		rel_send(e);
	}
	x->rels.len = live;
}
