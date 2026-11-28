// interaction.c: a view's interaction states and what follows them,
// its gestures and screen readers' actions (a View's), drags and drops.
//
// A view's states (pressed, hovered, dragged, a drag over it) are
// reported by its host as they change, and a style follows them three
// ways:
//
//   - its variants (CSS's :active and :hover): pressed { opacity: 0.6 }
//     in its block, laid over its style while it's in the state;
//   - ui.state(view, state), the state as a value other views' bindings
//     read (a group's hover, a peer's): View as card { … } then
//     card.hovered;
//   - its handlers (on pressedChange, on hoverChange, on dropOver).
//
// A view reports a state once something asks for it: a variant, a read,
// a handler. Each change is reported once, and leaving it when the view's
// owner is disposed while it's in one (what follows it elsewhere doesn't
// stay stuck).
#define _POSIX_C_SOURCE 200809L  // getcwd under -std=c17
#include "ui/ui_ext.h"

#include <stdio.h>

#ifdef _WIN32
#include <direct.h>  // _getcwd
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

// ---- Shapes (types.asuka's) ----

static const ak_shape *const no_shapes[16];

static const uint8_t gesture_kinds[] = {AK_K_INT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const char *const gesture_names[] = {"state", "x", "y", "translationX", "translationY", "velocityX", "velocityY", "scale", "rotation"};
const ak_shape ak_ui_shape_gesture = {"Gesture", 9, gesture_kinds, no_shapes, gesture_names, 0, NULL, false, NULL};

static const uint8_t shared_kinds[] = {AK_K_INT, AK_K_REF, AK_K_REF, AK_K_REF, AK_K_REF, AK_K_INT};
static const char *const shared_names[] = {"kind", "text", "path", "name", "mime", "size"};
const ak_shape ak_ui_shape_shared_item = {"SharedItem", 6, shared_kinds, no_shapes, shared_names, 0, NULL, false, NULL};

static const uint8_t drag_kinds[] = {AK_K_INT, AK_K_REF, AK_K_REF, AK_K_REF, AK_K_REF, AK_K_INT, AK_K_REF};
static const char *const drag_names[] = {"kind", "text", "path", "name", "mime", "size", "value"};
const ak_shape ak_ui_shape_drag_item = {"DragItem", 7, drag_kinds, no_shapes, drag_names, 0, NULL, false, NULL};

static const uint8_t drop_kinds[] = {AK_K_REF, AK_K_FLOAT, AK_K_FLOAT, AK_K_BOOL, AK_K_BOOL};
static const char *const drop_names[] = {"items", "x", "y", "local", "copied"};
static const ak_shape *const drop_shapes[] = {&ak_ui_shape_drag_item, NULL, NULL, NULL, NULL};
const ak_shape ak_ui_shape_drop = {"Drop", 5, drop_kinds, drop_shapes, drop_names, 0, NULL, false, NULL};

// ---- Containers' events and keys ----

// The containers (View, Row, Column, SafeArea) come first among the
// elements, with the same keys and events, numbered in turn.
static int container_index(const ak_view *e) { return e->el >= AK_EL_VIEW && e->el <= AK_EL_SAFE_AREA ? e->el - AK_EL_VIEW : -1; }

uint16_t ak_ext_ev(const ak_view *e, uint16_t view_ev) {
	int i = container_index(e);
	return i < 0 ? 0 : (uint16_t)(view_ev + i * (AK_EV_ROW_PRESS - AK_EV_VIEW_PRESS));
}

uint16_t ak_ext_key(const ak_view *e, uint16_t view_key) {
	int i = container_index(e);
	return i < 0 ? 0 : (uint16_t)(view_key + i * (AK_KEY_ROW_DRAG_ITEMS - AK_KEY_VIEW_DRAG_ITEMS));
}

// view_ev is a container's event as View's (0: not a container's).
static uint16_t view_ev(uint16_t ev) {
	int stride = AK_EV_ROW_PRESS - AK_EV_VIEW_PRESS;
	if (ev < AK_EV_VIEW_PRESS || ev >= AK_EV_VIEW_PRESS + 4 * stride) return 0;
	return (uint16_t)(AK_EV_VIEW_PRESS + (ev - AK_EV_VIEW_PRESS) % stride);
}

ak_event_kind ak_ix_ev_ops(uint16_t ev) {
	switch (view_ev(ev)) {
	case AK_EV_VIEW_PRESSED_CHANGE: return AK_EVENT_PRESSED;
	case AK_EV_VIEW_HOVER_CHANGE: return AK_EVENT_HOVER;
	case AK_EV_VIEW_PAN: return AK_EVENT_PAN;
	case AK_EV_VIEW_PINCH: return AK_EVENT_PINCH;
	case AK_EV_VIEW_ROTATE: return AK_EVENT_ROTATE;
	case AK_EV_VIEW_SWIPE: return AK_EVENT_SWIPE;
	case AK_EV_VIEW_ADJUST: return AK_EVENT_ADJUST;
	case AK_EV_VIEW_KEY_TEXT: return AK_EVENT_KEY_TEXT;
	case AK_EV_VIEW_PRESS_START: return AK_EVENT_PRESS_POINT;
	case AK_EV_VIEW_DOUBLE_TAP: return AK_EVENT_DOUBLE_TAP;
	case AK_EV_VIEW_HOVER: return AK_EVENT_HOVER_MOVE;
	// A drag's and a drop's come with dragItems and dropTypes; FocusChange
	// comes without asking.
	}
	switch (ev) {
	case AK_EV_TEXT_FIELD_RICH_CHANGE: return AK_EVENT_RICH_CHANGED;
	case AK_EV_TEXT_FIELD_FORMATS: return AK_EVENT_FORMATS;
	case AK_EV_RICH_TEXT_LINK: return AK_EVENT_LINK;
	case AK_EV_VIDEO_END: return AK_EVENT_ENDED;
	case AK_EV_VIDEO_LOADED:
	case AK_EV_VIDEO_STATUS:
	case AK_EV_VIDEO_TIME_UPDATE:
	case AK_EV_VIDEO_PICTURE_IN_PICTURE_CHANGE:
	case AK_EV_VIDEO_FULL_SCREEN_CHANGE:
	case AK_EV_VIDEO_RATE_CHANGE:
		return AK_EVENT_VIDEO_STATE;
	case AK_EV_VIDEO_TRACKS:
	case AK_EV_VIDEO_SUBTITLE_CHANGE:
	case AK_EV_VIDEO_AUDIO_TRACK_CHANGE:
		return AK_EVENT_VIDEO_TRACKS;
	case AK_EV_VIDEO_ERROR:
	case AK_EV_WEB_VIEW_ERROR:
		return AK_EVENT_FAILED;
	case AK_EV_WEB_VIEW_LOADED: return AK_EVENT_NAVIGATED;
	case AK_EV_WEB_VIEW_MESSAGE: return AK_EVENT_MESSAGE;
	case AK_EV_WEB_VIEW_STATE: return AK_EVENT_WEB_STATE;
	case AK_EV_WEB_VIEW_REQUEST: return AK_EVENT_REQUEST;
	case AK_EV_WEB_VIEW_DOWNLOAD: return AK_EVENT_DOWNLOAD;
	case AK_EV_WEB_VIEW_NEW_WINDOW: return AK_EVENT_NEW_WINDOW;
	}
	return 0;
}

void ak_ix_on(ak_view *v, uint16_t ev) {
	if (view_ev(ev) == AK_EV_VIEW_ADJUST && v->real) ak_batch_set_bool(ak_view_batch(v), v->id, AK_PROP_A11Y_ADJUSTABLE, true);
	// A web view's request handler has the host report requests.
	if (ev == AK_EV_WEB_VIEW_REQUEST && v->real) ak_batch_set_bool(ak_view_batch(v), v->id, AK_PROP_WEB_VIEW_REQUESTS, true);
}

// ---- Calling closures ----

typedef struct {
	ak_closure *c;
	ak_val *args;
	uint32_t n;
} call_ctx;

static void call_run(void *ctx) {
	call_ctx *x = ctx;
	ak_val v = ak_call(x->c, x->args, x->n);
	if (!ak_error_pending()) ak_release(v.p);  // nothing, or an async closure's future
}

void ak_call_closure(ak_closure *c, ak_owner *o, ak_val *args, uint32_t nargs) {
	if (!c) return;
	ak_retain(c);
	if (o && ak_owner_disposed(o)) o = NULL;
	call_ctx x = {c, args, nargs};
	if (o) ak_with_owner(o, call_run, &x);
	else call_run(&x);
	if (ak_error_pending()) ak_ui_raise_pending(o);
	ak_release(c);
}

// ---- The interaction state ----

ak_ix *ak_ix_of(ak_view *e) {
	if (!e->ix) {
		e->ix = ak_alloc(sizeof(ak_ix));
		for (int i = 0; i < AK_STATE__COUNT; i++) ak_signal_init(&e->ix->sig[i]);
	}
	return e->ix;
}

void ak_ix_free(ak_view *e) {
	ak_ix *ix = e->ix;
	if (!ix) return;
	ak_rootkeys_free(ix->wkeys);
	ak_release(ix->restore);
	for (int i = 0; i < AK_STATE__COUNT; i++) ak_signal_drop(&ix->sig[i]);
	for (uint32_t i = 0; i < ix->lents.len; i++) ak_release_val(ix->lents.data[i].v, (ak_kind)ak_key_kinds[ix->lents.data[i].key]);
	ak_vec_free(&ix->lents);
	ak_vec_free(&ix->layers);
	for (uint32_t i = 0; i < ix->base.len; i++) ak_release_val(ix->base.data[i].v, (ak_kind)ak_key_kinds[ix->base.data[i].key]);
	ak_vec_free(&ix->base);
	for (uint32_t i = 0; i < ix->vdecls.len; i++) ak_release(ix->vdecls.data[i].cond);
	ak_vec_free(&ix->vdecls);
	ak_free(ix->key_order);
	ak_buf_free(&ix->drag_sent);
	ak_buf_free(&ix->actions_sent);
	ak_free(ix);
	e->ix = NULL;
}

static int state_index(uint8_t bit) {
	int i = 0;
	while (bit > 1) bit >>= 1, i++;
	return i;
}

static ak_layer_ent *find_ent(ak_ix *ix, uint16_t layer, uint16_t key) {
	for (uint32_t i = 0; i < ix->lents.len; i++)
		if (ix->lents.data[i].layer == layer && ix->lents.data[i].key == key) return &ix->lents.data[i];
	return NULL;
}

static bool has_layers(const ak_ix *ix, uint16_t key) {
	for (uint32_t i = 0; ix && i < ix->lents.len; i++)
		if (ix->lents.data[i].key == key) return true;
	return false;
}

// on reports whether a layer is laid on: its condition holds, and the
// view is in its state.
static bool layer_on(const ak_ix *ix, const ak_layer *l) { return l->on && (!l->state || (ix->state & l->state)); }

// later reports whether layer a is after layer b in the order.
static bool later(const ak_ix *ix, uint16_t a, uint16_t b) {
	const ak_layer *la = &ix->layers.data[a], *lb = &ix->layers.data[b];
	if (la->tier != lb->tier) return la->tier > lb->tier;
	if (la->state != lb->state) return la->state > lb->state;  // bits in the states' order
	return a > b;
}

static ak_style_ent *find_base(ak_ix *ix, uint16_t key) {
	for (uint32_t i = 0; i < ix->base.len; i++)
		if (ix->base.data[i].key == key) return &ix->base.data[i];
	return NULL;
}

// key_unset is what a key is when it's not set (its prelude default).
static ak_val key_unset(uint16_t key) { return key == AK_KEY_OPACITY ? (ak_val){.f = 1} : (ak_val){0}; }

// apply lays key's value on e: its style's own, with the last of the
// layers on that set it over it. A key its component's caller set takes
// no value from e's own layers: the caller's is over them.
static void apply(ak_view *e, uint16_t key) {
	ak_ix *ix = e->ix;
	ak_style_ent *b = find_base(ix, key);
	ak_val v = b ? b->v : key_unset(key);
	bool outer = key < AK_KEY__STYLE_END && (e->outer[key / 64] >> (key % 64)) & 1;
	const ak_layer_ent *best = NULL;
	for (uint32_t i = 0; i < ix->lents.len; i++) {
		const ak_layer_ent *ent = &ix->lents.data[i];
		if (ent->key != key) continue;
		const ak_layer *l = &ix->layers.data[ent->layer];
		if (!layer_on(ix, l) || (outer && l->tier == AK_LAYER_OWN)) continue;
		if (!best || later(ix, ent->layer, best->layer)) best = ent;
	}
	if (best) v = best->v;
	ak_view *t = e->cls->style_target ? e->cls->style_target(e, key) : e;
	if (t) ak_style_set(t, key, v);
}

// apply_layer applies each key layer sets (a key once: a layer holds a
// key once), or every layer's when layer is -1.
static void apply_layer(ak_view *e, int layer) {
	ak_ix *ix = e->ix;
	for (uint32_t i = 0; i < ix->lents.len; i++) {
		const ak_layer_ent *ent = &ix->lents.data[i];
		if (layer >= 0) {
			if (ent->layer == layer) apply(e, ent->key);
			continue;
		}
		bool seen = false;
		for (uint32_t j = 0; j < i && !seen; j++) seen = ix->lents.data[j].key == ent->key;
		if (!seen) apply(e, ent->key);
	}
}

static void apply_all(ak_view *e) { apply_layer(e, -1); }

bool ak_ix_set_base(ak_view *v, uint16_t key, ak_val value) {
	ak_ix *ix = v->ix;
	if (!has_layers(ix, key)) return false;
	ak_kind k = (ak_kind)ak_key_kinds[key];
	ak_style_ent *b = find_base(ix, key);
	ak_retain_val(value, k);
	if (b) {
		ak_release_val(b->v, k);
		b->v = value;
	} else {
		ak_vec_push(&ix->base, ((ak_style_ent){key, value}));
	}
	apply(v, key);
	return true;
}

// listen_state has e's view report a state: pressed or hovered (a drag's
// states come with its dragItems and dropTypes).
static void listen_state(ak_view *e, uint8_t bit) {
	ak_ix *ix = ak_ix_of(e);
	if (e->dead || (ix->listens & bit)) return;
	if (bit == AK_STATE_FOCUSED || bit == AK_STATE_FOCUS_VISIBLE) {
		// The core follows the focus (focus.c): no event to ask for; start
		// from where it is.
		ix->listens |= bit;
		if (ak_focus_state_now(e, bit)) ix->state |= bit;
		return;
	}
	if (!e->real && (e->cls->flags & AK_ELF_FLATTEN)) ak_view_make_real(e);
	if (!e->real) return;
	if (bit == AK_STATE_PRESSED) ak_view_listen(e, AK_EVENT_PRESSED);
	else if (bit == AK_STATE_HOVERED) ak_view_listen(e, AK_EVENT_HOVER);
	else return;
	ix->listens |= bit;
}

static void drags_forget(ak_id id);
static void set_state(ak_view *e, uint8_t bit, bool on, uint16_t handler);

// leave reports leaving each state e is in, as its owner is disposed.
static void leave(void *ctx) {
	ak_view *e = ctx;
	ak_ix *ix = e->ix;
	if (ix) {
		if (ix->state & AK_STATE_PRESSED) set_state(e, AK_STATE_PRESSED, false, ak_ext_ev(e, AK_EV_VIEW_PRESSED_CHANGE));
		if (ix->state & AK_STATE_HOVERED) set_state(e, AK_STATE_HOVERED, false, ak_ext_ev(e, AK_EV_VIEW_HOVER_CHANGE));
		if (ix->state & AK_STATE_DRAG_OVER) set_state(e, AK_STATE_DRAG_OVER, false, ak_ext_ev(e, AK_EV_VIEW_DROP_OVER));
		if (ix->state & AK_STATE_DRAGGING) set_state(e, AK_STATE_DRAGGING, false, 0);  // dragPhase hears only the platform's end
		if (ix->state & AK_STATE_FOCUSED) set_state(e, AK_STATE_FOCUSED, false, 0);
		if (ix->state & AK_STATE_FOCUS_VISIBLE) set_state(e, AK_STATE_FOCUS_VISIBLE, false, 0);
	}
	ak_release(e);
}

typedef struct {
	ak_view *e;
} hook_ctx;

static void hook_fn(void *ctx) { ak_on_cleanup(leave, ak_retain(((hook_ctx *)ctx)->e)); }

// hook has e leave its states when the owner it was made under is
// disposed, once.
static void hook(ak_view *e) {
	ak_ix *ix = ak_ix_of(e);
	if (ix->hooked || !e->owner || ak_owner_disposed(e->owner)) return;
	ix->hooked = true;
	hook_ctx h = {e};
	ak_with_owner(e->owner, hook_fn, &h);
}

static void set_state(ak_view *e, uint8_t bit, bool on, uint16_t handler) {
	ak_ix *ix = ak_ix_of(e);
	if (((ix->state & bit) != 0) == on) return;
	ix->state ^= bit;
	ak_signal_write(&ix->sig[state_index(bit)]);
	if (!e->dead) apply_all(e);
	if (handler) {
		ak_val arg = {.i = on};
		ak_view_call(e, handler, &arg, 1);
	}
}

void ak_ix_set_state(ak_view *e, uint8_t bit, bool on) {
	if (!e->ix || !(e->ix->listens & bit)) {
		// Nothing asked for it: a later ask starts from the focus (listen_state).
		if (e->ix && ((e->ix->state & bit) != 0) != on) e->ix->state ^= bit;
		return;
	}
	set_state(e, bit, on, 0);
}

bool ak_ix_styles_state(const ak_view *e, uint8_t bit) {
	const ak_ix *ix = e->ix;
	for (uint32_t i = 0; ix && i < ix->layers.len; i++)
		if (ix->layers.data[i].state == bit) return true;
	return false;
}

// ---- Layers, variants and states, to compiled code ----

static void check_state(uint8_t state) {
	if (!state || (state & (state - 1)) || state >= (1 << AK_STATE__COUNT)) ak_panic("ui: no state %u", state);
}

static void check_style_key(uint16_t key) {
	if (key == 0 || key >= AK_KEY__STYLE_END) ak_panic("ui: a layer sets style keys only, not %u", key);
}

static ak_layer *layer_of(ak_view *v, uint16_t layer) {
	ak_ix *ix = v->ix;
	if (!ix || layer >= ix->layers.len) ak_panic("ui: %s has no layer %u", v->el ? ak_el_names[v->el] : v->cls->name, layer);
	return &ix->layers.data[layer];
}

static uint16_t new_layer(ak_view *v, uint8_t tier, uint8_t state, bool block) {
	if (tier > AK_LAYER_VARIANT) ak_panic("ui: no layer tier %u", tier);
	if (v->frag) ak_panic("ui: a fragment takes no style");
	if ((tier == AK_LAYER_STATE) != (state != 0)) ak_panic("ui: a state's layer, and only one, has a state");
	if (state) {
		check_state(state);
		if (!v->el || !ak_el_variants[v->el]) ak_panic("ui: %s takes no variants", v->el ? ak_el_names[v->el] : v->cls->name);
	}
	ak_ix *ix = ak_ix_of(v);
	if (ix->layers.len >= UINT16_MAX) ak_panic("ui: too many layers on a view");
	ak_vec_push(&ix->layers, ((ak_layer){.tier = tier, .state = state, .on = true, .block = block}));
	if (state && !v->dead) {
		if (!v->real && (v->cls->flags & AK_ELF_FLATTEN)) ak_view_make_real(v);  // to report the state
		listen_state(v, state);
		hook(v);
		if (state == AK_STATE_FOCUS_VISIBLE) ak_focus_ring_styled(v);  // the app draws its ring
	}
	return (uint16_t)(ix->layers.len - 1);
}

uint16_t ak_ui_layer(ak_view *v, uint8_t tier, uint8_t state) { return new_layer(v, tier, state, false); }

void ak_ui_layer_set(ak_view *v, uint16_t layer, uint16_t key, ak_val value) {
	check_style_key(key);
	layer_of(v, layer);
	if (v->dead) return;
	ak_ix *ix = v->ix;
	ak_kind k = (ak_kind)ak_key_kinds[key];
	if (!has_layers(ix, key)) {
		// The style's own value so far is its base from now on.
		const ak_val *cur = ak_style_get(v, key);
		ak_val b = cur ? *cur : key_unset(key);
		ak_retain_val(b, k);
		ak_vec_push(&ix->base, ((ak_style_ent){key, b}));
	}
	ak_layer_ent *ent = find_ent(ix, layer, key);
	if (ent && ak_val_eq(ent->v, value, k)) return;
	ak_retain_val(value, k);
	if (ent) {
		ak_release_val(ent->v, k);
		ent->v = value;
	} else {
		ak_vec_push(&ix->lents, ((ak_layer_ent){layer, key, value}));
	}
	apply(v, key);
}

// A layer's binding: a key's value (key set), or whether it's on.
typedef struct {
	ak_view *v;
	ak_closure *thunk;
	uint16_t layer;
	uint16_t key;  // 0: the condition
	ak_comp_info info;
} lbinding;

static void lbinding_run(void *ctx) {
	lbinding *b = ctx;
	ak_val v = ak_call(b->thunk, NULL, 0);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	if (b->key) {
		ak_ui_layer_set(b->v, b->layer, b->key, v);
		ak_release_val(v, (ak_kind)ak_key_kinds[b->key]);
		return;
	}
	ak_layer *l = layer_of(b->v, b->layer);
	if (l->on == (v.i != 0)) return;
	l->on = v.i != 0;
	if (!b->v->dead) apply_layer(b->v, b->layer);
}

static void lbinding_drop(void *ctx) {
	lbinding *b = ctx;
	ak_release(b->v);
	ak_release(b->thunk);
	ak_free(b);
}

static void lbinding_describe(void *ctx, char *buf, size_t n) {
	lbinding *b = ctx;
	char what[64];
	snprintf(what, sizeof what, "%s binding", b->key ? ak_key_names[b->key] : "style condition");
	ak_vm_where(b->thunk, what, buf, n);
}

static const void *lbinding_site(void *ctx) { return ak_vm_where_site(((lbinding *)ctx)->thunk); }

static void layer_binding(ak_view *v, uint16_t layer, uint16_t key, ak_closure *thunk) {
	lbinding *b = ak_alloc(sizeof *b);
	b->v = ak_retain(v);
	b->thunk = ak_retain(thunk);
	b->layer = layer;
	b->key = key;
	b->info = (ak_comp_info){.drop = lbinding_drop, .label = key ? ak_key_names[key] : "style condition", .describe = lbinding_describe, .site = lbinding_site};
	ak_effect_new(lbinding_run, b, AK_EFFECT_RENDER, &b->info);
}

void ak_ui_layer_bind(ak_view *v, uint16_t layer, uint16_t key, ak_closure *thunk) {
	check_style_key(key);
	layer_of(v, layer);
	layer_binding(v, layer, key, thunk);
}

void ak_ui_layer_when(ak_view *v, uint16_t layer, ak_closure *cond) {
	ak_layer *l = layer_of(v, layer);
	if (l->on) {
		// Off until its condition says otherwise.
		l->on = false;
		if (!v->dead) apply_layer(v, layer);
	}
	layer_binding(v, layer, 0, cond);
}

// block_layer is v's block's variant of state, made the first time:
// after the variants its style's layers made, so it's over them.
static uint16_t block_layer(ak_view *v, uint8_t state) {
	check_state(state);
	ak_ix *ix = ak_ix_of(v);
	for (uint32_t i = 0; i < ix->layers.len; i++)
		if (ix->layers.data[i].block && ix->layers.data[i].state == state) return (uint16_t)i;
	return new_layer(v, AK_LAYER_STATE, state, true);
}

void ak_ui_variant(ak_view *v, uint8_t state, uint16_t key, ak_val value) {
	check_style_key(key);
	ak_ui_layer_set(v, block_layer(v, state), key, value);
}

void ak_ui_variant_bind(ak_view *v, uint8_t state, uint16_t key, ak_closure *thunk) {
	check_style_key(key);
	layer_binding(v, block_layer(v, state), key, thunk);
}

// ---- Declared variants ----

static ak_vdecl *find_vdecl(ak_view *v, int64_t id) {
	ak_ix *ix = v->ix;
	for (uint32_t i = 0; ix && i < ix->vdecls.len; i++)
		if (ix->vdecls.data[i].id == id) return &ix->vdecls.data[i];
	return NULL;
}

void ak_ui_variant_declare(ak_view *v, int64_t id, ak_closure *cond) {
	if (!cond) ak_panic("ui.variant_declare: variant %lld has no condition", (long long)id);
	if (v->frag) ak_panic("ui: a fragment takes no style, nor variants");
	ak_vdecl *d = find_vdecl(v, id);
	if (d) {
		// The same component's again (its view reused): the last wins.
		ak_closure *old = d->cond;
		d->cond = ak_retain(cond);
		ak_release(old);
		return;
	}
	ak_vec_push(&ak_ix_of(v)->vdecls, ((ak_vdecl){id, ak_retain(cond)}));
}

void ak_ui_layer_variant(ak_view *v, uint16_t layer, int64_t id) {
	ak_vdecl *d = find_vdecl(v, id);
	if (!d) ak_panic("ui: %s declares no variant %lld: its component declares it on the view it shows, before its caller's layers", v->el ? ak_el_names[v->el] : v->cls->name, (long long)id);
	if (layer_of(v, layer)->tier != AK_LAYER_VARIANT) ak_panic("ui: a declared variant's layer is of the variant tier");
	ak_ui_layer_when(v, layer, d->cond);
}

bool ak_ui_variant_read(ak_view *v, int64_t id) {
	ak_vdecl *d = find_vdecl(v, id);
	if (!d) return false;
	ak_closure *cond = ak_retain(d->cond);
	ak_val r = ak_call(cond, NULL, 0);
	ak_release(cond);
	return !ak_error_pending() && r.i != 0;
}

bool ak_ui_view_state(ak_view *v, uint8_t state) {
	if (!state || (state & (state - 1)) || state >= (1 << AK_STATE__COUNT)) ak_panic("ui: no state %u", state);
	ak_ix *ix = ak_ix_of(v);
	listen_state(v, state);
	hook(v);
	ak_signal_read(&ix->sig[state_index(state)]);
	return (ix->state & state) != 0;
}

// ---- Drags ----

// The items of the drags under way, by the view they're dragged from, as
// they were when the drag began: a drop in the app gets them.
typedef struct {
	ak_id id;
	ak_arr *items;  // retained
} drag_ent;

static AK_VEC(drag_ent) drags;

void ak_ix_unmount(void);

static void drags_forget(ak_id id) {
	for (uint32_t i = 0; i < drags.len; i++) {
		if (drags.data[i].id != id) continue;
		ak_release(drags.data[i].items);
		drags.data[i] = drags.data[--drags.len];
		return;
	}
	if (!drags.len) ak_vec_free(&drags);
}

// ak_ix_unmount forgets the drags under way: their app is gone.
void ak_ix_unmount(void) {
	for (uint32_t i = 0; i < drags.len; i++) ak_release(drags.data[i].items);
	ak_vec_free(&drags);
}

static ak_arr *drags_get(ak_id id) {
	for (uint32_t i = 0; i < drags.len; i++)
		if (drags.data[i].id == id) return drags.data[i].items;
	return NULL;
}

// A file's MIME type from its name (Go's mime.TypeByExtension, with what
// the system's tables usually add).
static const struct {
	const char *ext, *mime;
} mimes[] = {
	{"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"webp", "image/webp"},
	{"heic", "image/heic"}, {"heif", "image/heif"}, {"avif", "image/avif"}, {"svg", "image/svg+xml"}, {"bmp", "image/bmp"},
	{"tif", "image/tiff"}, {"tiff", "image/tiff"}, {"ico", "image/vnd.microsoft.icon"}, {"mp4", "video/mp4"}, {"m4v", "video/x-m4v"},
	{"mov", "video/quicktime"}, {"webm", "video/webm"}, {"mkv", "video/x-matroska"}, {"avi", "video/x-msvideo"},
	{"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"},
	{"oga", "audio/ogg"}, {"opus", "audio/ogg"}, {"flac", "audio/flac"}, {"pdf", "application/pdf"}, {"txt", "text/plain"},
	{"md", "text/markdown"}, {"csv", "text/csv"}, {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"},
	{"js", "text/javascript"}, {"mjs", "text/javascript"}, {"json", "application/json"}, {"xml", "text/xml"},
	{"wasm", "application/wasm"}, {"zip", "application/zip"}, {"gz", "application/gzip"}, {"tar", "application/x-tar"},
	{"rtf", "application/rtf"}, {"epub", "application/epub+zip"}, {"doc", "application/msword"},
	{"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"}, {"xls", "application/vnd.ms-excel"},
	{"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}, {"ppt", "application/vnd.ms-powerpoint"},
	{"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
};

static ak_span file_mime(ak_span path) {
	uint32_t dot = path.len, slash = 0;
	for (uint32_t i = 0; i < path.len; i++) {
		if (path.p[i] == '.') dot = i;
		if (path.p[i] == '/' || path.p[i] == '\\') slash = i + 1, dot = path.len;
	}
	if (dot < path.len && dot >= slash) {
		ak_span ext = {path.p + dot + 1, path.len - dot - 1};
		for (size_t i = 0; i < AK_LEN(mimes); i++) {
			size_t n = strlen(mimes[i].ext);
			if (n != ext.len) continue;
			bool same = true;
			for (size_t j = 0; j < n && same; j++) same = (char)(ext.p[j] | 0x20) == mimes[i].ext[j];
			if (same) return ak_cstr(mimes[i].mime);
		}
	}
	return AK_STR("application/octet-stream");
}

static uint8_t file_kind(ak_span m) {
	if (m.len >= 6 && memcmp(m.p, "image/", 6) == 0) return AK_SHARE_KIND_IMAGE;
	if (m.len >= 6 && memcmp(m.p, "video/", 6) == 0) return AK_SHARE_KIND_VIDEO;
	if (m.len >= 6 && memcmp(m.p, "audio/", 6) == 0) return AK_SHARE_KIND_AUDIO;
	return AK_SHARE_KIND_FILE;
}

static bool is_abs(ak_span p) {
	if (p.len && (p.p[0] == '/' || p.p[0] == '\\')) return true;
	return p.len >= 3 && p.p[1] == ':' && (p.p[2] == '\\' || p.p[2] == '/');
}

void ak_item_wire(const ak_struct *s, bool drag, ak_item *out, ak_str **abs) {
	*out = (ak_item){.kind = (uint8_t)s->f[0].i, .text = ak_span_str(s->f[1].p), .path = ak_span_str(s->f[2].p), .name = ak_span_str(s->f[3].p),
	                 .mime = ak_span_str(s->f[4].p), .size = -1};
	*abs = NULL;
	if (drag) {
		const ak_str *v = s->f[6].p;
		out->value = v && v->len > 0;
	}
	if (out->path.len) {
		if (!is_abs(out->path)) {
			// Made absolute against the working directory (Go's filepath.Abs).
			char cwd[4096];
			if (getcwd(cwd, sizeof cwd)) {
				ak_strb b = {0};
				ak_strb_cstr(&b, cwd, strlen(cwd));
				ak_strb_cstr(&b, "/", 1);
				const char *p = out->path.p;
				uint32_t n = out->path.len;
				while (n >= 2 && p[0] == '.' && p[1] == '/') p += 2, n -= 2;
				ak_strb_cstr(&b, p, n);
				*abs = ak_strb_finish(&b);
				out->path = ak_span_str(*abs);
			}
		}
		if (!out->name.len) {
			uint32_t i = out->path.len;
			while (i > 0 && out->path.p[i - 1] != '/' && out->path.p[i - 1] != '\\') i--;
			out->name = (ak_span){out->path.p + i, out->path.len - i};
		}
		if (!out->mime.len) out->mime = file_mime(out->path);
		if (out->kind < AK_SHARE_KIND_IMAGE) out->kind = file_kind(out->mime);
	}
	if (!out->mime.len) {
		if (out->kind == AK_SHARE_KIND_LINK) out->mime = AK_STR("text/uri-list");
		else if (out->text.len) out->mime = AK_STR("text/plain");
		else out->mime = AK_STR("application/octet-stream");
	}
}

// wired is an array's items on the wire, with the paths they made
// absolute.
typedef struct {
	ak_item *items;
	ak_str **abs;
	uint32_t n;
} wired;

static wired wire_items(const ak_arr *a, bool drag) {
	wired w = {0};
	w.n = a ? a->len : 0;
	if (!w.n) return w;
	w.items = ak_alloc(w.n * sizeof(ak_item));
	w.abs = ak_alloc(w.n * sizeof(ak_str *));
	for (uint32_t i = 0; i < w.n; i++) ak_item_wire(ak_arr_vals(a)[i].p, drag, &w.items[i], &w.abs[i]);
	return w;
}

static void wired_free(wired *w) {
	for (uint32_t i = 0; i < w->n; i++) ak_release(w->abs[i]);
	ak_free(w->abs);
	ak_free(w->items);
}

static ak_str *str_or_empty(ak_span s) { return ak_str_new(s.p, s.len); }

ak_arr *ak_items_value(const ak_item *items, uint32_t n, bool drag) {
	const ak_shape *sh = drag ? &ak_ui_shape_drag_item : &ak_ui_shape_shared_item;
	ak_arr *a = ak_arr_new(AK_K_REF, sh, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_struct *s = ak_struct_new(sh);
		s->f[0].i = items[i].kind;
		s->f[1].p = str_or_empty(items[i].text);
		s->f[2].p = str_or_empty(items[i].path);
		s->f[3].p = str_or_empty(items[i].name);
		s->f[4].p = str_or_empty(items[i].mime);
		s->f[5].i = items[i].size;
		if (drag) s->f[6].p = ak_str_empty();
		ak_arr_push(&a, (ak_val){.p = s});
	}
	return a;
}

static void set_drag_items(ak_view *e, const ak_arr *a) {
	ak_ix *ix = ak_ix_of(e);
	if (!e->real && (e->cls->flags & AK_ELF_FLATTEN)) ak_view_make_real(e);
	ak_buf enc = {0};
	wired w = wire_items(a, true);
	ak_append_drag_items(&enc, w.items, w.n);
	wired_free(&w);
	if (!(ix->drag_known && ix->drag_sent.len == enc.len && memcmp(ix->drag_sent.data, enc.data, enc.len) == 0)) {
		ix->drag_known = true;
		ix->drag_sent.len = 0;
		ak_buf_put(&ix->drag_sent, enc.data, enc.len);
		ak_batch_set_bytes(ak_view_batch(e), e->id, AK_PROP_VIEW_DRAG_ITEMS, enc.data, enc.len);
	}
	ak_buf_free(&enc);
	ak_view_listen(e, AK_EVENT_DRAGGING);
	hook(e);
}

static void set_drop_types(ak_view *e, const ak_arr *a) {
	if (!a || !a->len) return;
	if (!e->real && (e->cls->flags & AK_ELF_FLATTEN)) ak_view_make_real(e);
	ak_buf *b = ak_batch_set_bytes_begin(ak_view_batch(e), e->id, AK_PROP_VIEW_DROP_ACCEPT);
	ak_encode_strings(b, a);
	ak_batch_set_bytes_end(ak_view_batch(e));
	ak_view_listen(e, AK_EVENT_DRAG_OVER);
	ak_view_listen(e, AK_EVENT_DROP);
	hook(e);
}

void ak_ix_container_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	if (ak_focus_container_set(e, key, v, prev)) return;
	if (key == ak_ext_key(e, AK_KEY_VIEW_DRAG_ITEMS)) set_drag_items(e, v.p);
	else if (key == ak_ext_key(e, AK_KEY_VIEW_DROP_TYPES)) set_drop_types(e, v.p);
	else if (key == ak_ext_key(e, AK_KEY_VIEW_SWIPE_ACTIONS) || key == ak_ext_key(e, AK_KEY_VIEW_LEADING_SWIPE_ACTIONS) || key == ak_ext_key(e, AK_KEY_VIEW_SELECTED))
		ak_list_row_keys(e);  // a List's row's own (list.c)
	else if (key == ak_ext_key(e, AK_KEY_VIEW_TAKES_CLICKS)) {
		// In a click-through window, the mouse still goes to it: the host
		// needs its view to know where it is.
		if (!v.i && !prev) return;
		if (v.i && !e->real) ak_view_make_real(e);
		if (e->real) ak_batch_set_bool(ak_view_batch(e), e->id, AK_PROP_VIEW_TAKES_CLICKS, v.i != 0);
	} else if (key == ak_ext_key(e, AK_KEY_VIEW_PAN_AXIS) || key == ak_ext_key(e, AK_KEY_VIEW_PAN_MIN_DISTANCE) || key == ak_ext_key(e, AK_KEY_VIEW_PAN_WITH_SCROLL)) {
		// How its pan is recognized: its view's, sent once set (a container
		// with on pan has a view already).
		bool dist = key == ak_ext_key(e, AK_KEY_VIEW_PAN_MIN_DISTANCE);
		if (!(dist ? v.f != 0 : v.i != 0) && !prev) return;
		if (!e->real) ak_view_make_real(e);
		if (!e->real) return;
		ak_batch *b = ak_view_batch(e);
		if (dist) ak_batch_set_float(b, e->id, AK_PROP_VIEW_PAN_MIN_DISTANCE, v.f > 0 ? v.f : 0);
		else if (key == ak_ext_key(e, AK_KEY_VIEW_PAN_AXIS)) ak_batch_set_enum(b, e->id, AK_PROP_VIEW_PAN_AXIS, (int32_t)v.i);
		else ak_batch_set_bool(b, e->id, AK_PROP_VIEW_PAN_SIMULTANEOUS, v.i != 0);
	} else if (key == ak_ext_key(e, AK_KEY_VIEW_HREF) && v.p && ((ak_str *)v.p)->len) {
		// A link: pressable, the router goes there (container_event).
		if (!e->real) ak_view_make_real(e);
		ak_view_listen(e, AK_EVENT_PRESS);
	}
}

static const ak_arr *container_arr(const ak_view *e, uint16_t view_key) {
	const ak_val *v = ak_style_get(e, ak_ext_key(e, view_key));
	return v ? v->p : NULL;
}

static bool accepts(const ak_view *e, ak_span mime) {
	const ak_arr *a = container_arr(e, AK_KEY_VIEW_DROP_TYPES);
	if (!a) return false;
	ak_span pats[32];
	uint32_t n = 0;
	for (uint32_t i = 0; i < a->len && n < AK_LEN(pats); i++) pats[n++] = ak_span_str(ak_arr_vals(a)[i].p);
	return ak_match_mime(pats, n, mime);
}

// dropped delivers a Drop event: the items of a drag from the app are the
// core's own, as they were when it began; another app's come with it.
static void dropped(ak_view *e, ak_span payload) {
	ak_drop d;
	ak_item_vec items = {0};
	if (!ak_decode_drop((const uint8_t *)payload.p, payload.len, &d, &items)) {
		ak_vec_free(&items);
		return;
	}
	set_state(e, AK_STATE_DRAG_OVER, false, ak_ext_ev(e, AK_EV_VIEW_DROP_OVER));
	ak_arr *out = ak_arr_new(AK_K_REF, &ak_ui_shape_drag_item, 0);
	if (d.source) {
		const ak_arr *src = drags_get(d.source);
		if (!src) {
			// Its start unheard (or already its end): the source's items now.
			ak_view *s = ak_view_of(d.source);
			if (s && !s->dead) src = container_arr(s, AK_KEY_VIEW_DRAG_ITEMS);
		}
		wired w = wire_items(src, true);
		for (uint32_t i = 0; i < w.n; i++) {
			if (!accepts(e, w.items[i].mime)) continue;
			ak_arr *one = ak_items_value(&w.items[i], 1, true);
			ak_struct *it = ak_arr_vals(one)[0].p;
			const ak_struct *orig = ak_arr_vals(src)[i].p;
			ak_release(it->f[6].p);
			it->f[6].p = ak_retain(orig->f[6].p ? orig->f[6].p : ak_str_empty());
			ak_arr_push(&out, (ak_val){.p = ak_retain(it)});
			ak_release(one);
		}
		wired_free(&w);
	} else {
		for (uint32_t i = 0; i < items.len; i++) {
			if (!accepts(e, items.data[i].mime)) continue;
			ak_arr *one = ak_items_value(&items.data[i], 1, true);
			ak_arr_push(&out, (ak_val){.p = ak_retain(ak_arr_vals(one)[0].p)});
			ak_release(one);
		}
	}
	ak_vec_free(&items);
	uint16_t ev = ak_ext_ev(e, AK_EV_VIEW_DROP);
	if (out->len == 0 || !ak_view_handler(e, ev)) {
		ak_release(out);
		return;
	}
	ak_struct *drop = ak_struct_new(&ak_ui_shape_drop);
	drop->f[0].p = out;
	drop->f[1].f = d.x, drop->f[2].f = d.y;
	drop->f[3].i = d.source != 0, drop->f[4].i = d.copied;
	ak_val arg = {.p = drop};
	ak_view_call(e, ev, &arg, 1);
	ak_release(drop);
}

// ---- Screen readers' actions (the actions style key) ----

static void set_actions(ak_view *e, const ak_arr *a) {
	ak_ix *ix = ak_ix_of(e);
	ak_buf enc = {0};
	uint32_t n = a ? a->len : 0;
	ak_buf_u32(&enc, n);
	for (uint32_t i = 0; i < n; i++) {
		const ak_struct *s = ak_arr_vals(a)[i].p;
		ak_buf_wstr(&enc, ak_span_str(s ? s->f[0].p : NULL));
	}
	if (!(ix->actions_known && ix->actions_sent.len == enc.len && memcmp(ix->actions_sent.data, enc.data, enc.len) == 0)) {
		ix->actions_known = true;
		ix->actions_sent.len = 0;
		ak_buf_put(&ix->actions_sent, enc.data, enc.len);
		ak_batch_set_bytes(ak_view_batch(e), e->id, AK_PROP_A11Y_ACTIONS, enc.data, enc.len);
	}
	ak_buf_free(&enc);
	if (n) ak_view_listen(e, AK_EVENT_A11Y_ACTION);
}

// ---- Style keys (keys.c has keys') ----

void ak_keys_set(ak_view *e, const ak_arr *a);
bool ak_keys_event(ak_view *e, int64_t index);

bool ak_ix_style_key(uint16_t key) { return key == AK_KEY_KEYS || key == AK_KEY_ACTIONS; }

void ak_ix_style_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(prev);
	const ak_arr *a = v.p;
	if (!e->real) {
		// A flattened container with keys or actions has a view of its own
		// (which sends them as it's made).
		if ((e->cls->flags & AK_ELF_FLATTEN) && a && a->len) ak_view_make_real(e);
		return;
	}
	if (key == AK_KEY_KEYS) ak_keys_set(e, a);
	else set_actions(e, a);
}

// ---- Events ----

static ak_struct *gesture_of(const ak_gesture *g) {
	ak_struct *s = ak_struct_new(&ak_ui_shape_gesture);
	s->f[0].i = g->state;
	float fs[8] = {g->x, g->y, g->translation_x, g->translation_y, g->velocity_x, g->velocity_y, g->scale, g->rotation};
	for (int i = 0; i < 8; i++) s->f[1 + i].f = fs[i];
	return s;
}

static void call_gesture(ak_view *e, uint16_t view_event, const ak_gesture *g) {
	uint16_t ev = ak_ext_ev(e, view_event);
	if (!ev || !ak_view_handler(e, ev)) return;
	ak_struct *s = gesture_of(g);
	ak_val arg = {.p = s};
	ak_view_call(e, ev, &arg, 1);
	ak_release(s);
}

static void run_action(ak_view *e, int64_t i) {
	const ak_val *v = ak_style_get(e, AK_KEY_ACTIONS);
	const ak_arr *a = v ? v->p : NULL;
	if (!a || i < 0 || i >= a->len) return;
	const ak_struct *s = ak_arr_vals(a)[i].p;
	if (s) ak_call_closure(s->f[1].p, e->owner, NULL, 0);
}

bool ak_ix_event(ak_view *e, const ak_event *ev) {
	switch (ev->kind) {
	case AK_EVENT_PRESSED:
		set_state(e, AK_STATE_PRESSED, ev->b, ak_ext_ev(e, AK_EV_VIEW_PRESSED_CHANGE));
		return true;
	case AK_EVENT_HOVER:
		set_state(e, AK_STATE_HOVERED, ev->b, ak_ext_ev(e, AK_EV_VIEW_HOVER_CHANGE));
		return true;
	case AK_EVENT_PAN: call_gesture(e, AK_EV_VIEW_PAN, &ev->g); return true;
	case AK_EVENT_PINCH: call_gesture(e, AK_EV_VIEW_PINCH, &ev->g); return true;
	case AK_EVENT_ROTATE: call_gesture(e, AK_EV_VIEW_ROTATE, &ev->g); return true;
	case AK_EVENT_SWIPE: {
		// A direction's bit (1 right, 2 left, 4 up, 8 down): its case.
		int64_t d = 0;
		while (d < 4 && !(ev->i & (INT64_C(1) << d))) d++;
		if (d == 4) return true;
		ak_val arg = {.i = d};
		ak_view_call(e, ak_ext_ev(e, AK_EV_VIEW_SWIPE), &arg, 1);
		return true;
	}
	case AK_EVENT_ADJUST: {
		ak_val arg = {.i = ev->i};
		ak_view_call(e, ak_ext_ev(e, AK_EV_VIEW_ADJUST), &arg, 1);
		return true;
	}
	case AK_EVENT_A11Y_ACTION: run_action(e, ev->i); return true;
	case AK_EVENT_FOCUS_CHANGE: ak_focus_report(e, (int)ev->i); return true;
	case AK_EVENT_KEY_TEXT: {
		uint16_t h = ak_ext_ev(e, AK_EV_VIEW_KEY_TEXT);
		if (!h || !ak_view_handler(e, h)) return true;
		ak_str *s = ak_str_new(ev->s.p, ev->s.len);
		ak_val arg = {.p = s};
		ak_view_call(e, h, &arg, 1);
		ak_release(s);
		return true;
	}
	case AK_EVENT_PRESS_POINT:
	case AK_EVENT_DOUBLE_TAP:
	case AK_EVENT_HOVER_MOVE: {
		// A point in the view: where a press began, a double tap fell, a
		// pointer is.
		uint16_t h = ev->kind == AK_EVENT_PRESS_POINT ? AK_EV_VIEW_PRESS_START : ev->kind == AK_EVENT_DOUBLE_TAP ? AK_EV_VIEW_DOUBLE_TAP : AK_EV_VIEW_HOVER;
		ak_val args[2] = {{.f = ev->x}, {.f = ev->y}};
		ak_view_call(e, ak_ext_ev(e, h), args, 2);
		return true;
	}
	case AK_EVENT_KEY: return ak_keys_event(e, ev->i);
	case AK_EVENT_DRAG_OVER: {
		if (!container_arr(e, AK_KEY_VIEW_DROP_TYPES)) return true;
		uint16_t over = ak_ext_ev(e, AK_EV_VIEW_DROP_OVER);
		if (ev->g.state == AK_GESTURE_STATE_BEGAN || ev->g.state == AK_GESTURE_STATE_CHANGED) {
			set_state(e, AK_STATE_DRAG_OVER, true, over);
			ak_val args[2] = {{.f = ev->g.x}, {.f = ev->g.y}};
			ak_view_call(e, ak_ext_ev(e, AK_EV_VIEW_DROP_MOVE), args, 2);
		} else {
			set_state(e, AK_STATE_DRAG_OVER, false, over);
		}
		return true;
	}
	case AK_EVENT_DROP:
		if (container_arr(e, AK_KEY_VIEW_DROP_TYPES)) dropped(e, ev->s);
		return true;
	case AK_EVENT_DRAGGING: {
		const ak_arr *items = container_arr(e, AK_KEY_VIEW_DRAG_ITEMS);
		if (!items) return true;
		int64_t phase = ev->i;
		if (phase == AK_DRAG_PHASE_BEGAN) {
			// Forget the drags whose views went: they ended unheard.
			for (uint32_t i = 0; i < drags.len;) {
				ak_view *x = ak_view_of(drags.data[i].id);
				if (!x || x->dead) drags_forget(drags.data[i].id);
				else i++;
			}
			drags_forget(e->id);
			ak_vec_push(&drags, ((drag_ent){e->id, ak_retain((void *)items)}));
		} else {
			drags_forget(e->id);
		}
		set_state(e, AK_STATE_DRAGGING, phase == AK_DRAG_PHASE_BEGAN, 0);
		ak_val arg = {.i = phase};
		ak_view_call(e, ak_ext_ev(e, AK_EV_VIEW_DRAG_PHASE), &arg, 1);
		return true;
	}
	}
	return false;
}
