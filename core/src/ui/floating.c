// floating.c: Overlay, what std/base's popups are made of: its children
// in a layer over a layout root (the main window's, a Window's, a
// Sheet's or a Popover's), placed next to an anchor.
//
// The element is a fragment: it makes no view where it's written. Open,
// its layer is a view of no parent (an Overlay, a host-sized provider
// whose up is the element) that the host puts over everything in the
// root it names (Root), with two views of the core's in it: the
// backdrop, filling it, and the content, which ak_place puts next to the
// anchor once the host has said where the anchor is (AnchorFrame; until
// then the layer is hidden). The content's owner is a root under the
// element's, so it's disposed when it closes, and with the element.
//
// What closes it (dismissOn): a press outside it (the host's OutsidePress
// for a press on no view of the layer, not on the anchor nor in a child
// overlay; a press on the backdrop), Escape (the root's keys), Back
// (the back stack, as a sheet's), the focus going somewhere outside it
// and its anchor, its anchor scrolled or hidden out of view. Each reports
// change(false) and dismiss(reason): the element closes when open goes
// false, so a caller that keeps it true keeps it open.
//
// The layer stays, empty and passing presses through, after it closes,
// until the element goes: content leaving in animate { } plays its
// transition in it. Opening again presents it over the root's other
// overlays.
#include "ui/ui_ext.h"

static const ak_val *key_of(const ak_view *e, uint16_t key) { return ak_style_get(e, key); }

static float key_f(const ak_view *e, uint16_t key, float def) {
	const ak_val *v = key_of(e, key);
	return v ? (float)v->f : def;
}

static bool key_b(const ak_view *e, uint16_t key, bool def) {
	const ak_val *v = key_of(e, key);
	return v ? v->i != 0 : def;
}

static int64_t key_i(const ak_view *e, uint16_t key, int64_t def) {
	const ak_val *v = key_of(e, key);
	return v ? v->i : def;
}

typedef struct ov ov;

struct ov {
	ak_view *e;          // the element (not retained: it holds this)
	ak_closure *build;   // its children's builder
	ak_view *layer;      // retained, once made
	ak_view *content;    // retained: the core's views in the layer
	ak_view *backdrop;
	ak_owner *owner;     // the content's and backdrop's, while open
	ak_back *back;
	struct ak_keyreg *keys;
	ak_view *root;       // not retained: the layout root it's over (checked by id)
	ak_id root_id;
	ak_id anchor_id;     // as sent
	ak_place_rect anchor;  // as the host reported it
	bool anchor_known, anchor_visible;
	bool open;
	// As placed: the content's place, its size limits, the side.
	float x, y, max_w, max_h, min_w, fix_w, fix_h;
	ak_place_result placed;
	float placed_w, placed_h;  // its size, as placed
	bool place_due;            // place to report (ak_overlays_report)
	bool place_sent;
	ov *parent;  // the overlay whose content it's written in
};

static void ov_dismiss(ov *o, int reason);

// ---- The views the core makes in the layer ----

static void content_adjust(ak_view *e, ak_lstyle *ls) {
	ov *o = e->impl;
	ls->position = AK_LABSOLUTE;
	if (!o) return;
	ls->inset.left = ak_lpt(o->x);
	ls->inset.top = ak_lpt(o->y);
	ls->inset.right = ls->inset.bottom = (ak_llength){0, AK_LUNIT_AUTO};
	if (o->max_w > 0) ls->max_width = ak_lpt(o->max_w);
	if (o->max_h > 0) ls->max_height = ak_lpt(o->max_h);
	if (o->min_w > 0) ls->min_width = ak_lpt(o->min_w);
	if (o->fix_w > 0) ls->width = ak_lpt(o->fix_w);
	if (o->fix_h > 0) ls->height = ak_lpt(o->fix_h);
}

static void backdrop_adjust(ak_view *e, ak_lstyle *ls) {
	AK_UNUSED(e);
	ls->position = AK_LABSOLUTE;
	ls->inset = ak_lrect_all(ak_lpt(0));
}

static void backdrop_event(ak_view *e, const ak_event *ev) {
	ov *o = e->impl;
	if (o && ev->kind == AK_EVENT_PRESS) ov_dismiss(o, AK_DISMISS_REASON_OUTSIDE_PRESS);
}

static const ak_el_class cls_content = {.name = "OverlayContent", .kind = AK_KIND_VIEW, .flags = AK_ELF_FLATTEN | AK_ELF_CHILDREN, .adjust = content_adjust};
static const ak_el_class cls_backdrop = {.name = "OverlayBackdrop", .kind = AK_KIND_VIEW, .flags = AK_ELF_CHILDREN, .adjust = backdrop_adjust, .event = backdrop_event};

// ---- The layer ----

static void layer_event(ak_view *l, const ak_event *ev);

static void layer_adjust(ak_view *e, ak_lstyle *ls) {
	AK_UNUSED(e);
	ls->flex_direction = AK_LCOLUMN;
	ls->width = ak_lpct(100), ls->height = ak_lpct(100);
}

static const ak_el_class cls_layer = {.name = "Overlay", .kind = AK_KIND_OVERLAY, .flags = AK_ELF_CHILDREN, .event = layer_event, .adjust = layer_adjust};

// layout_root is the layout root v shows in: the root's view, a Window's,
// a Sheet's or Popover's (a provider); a List's cell is in its list's.
static ov *ov_of_layer(const ak_view *l);

static ak_view *layout_root(ak_view *v) {
	for (int guard = 0; v && guard < 1024; guard++) {
		while (v->parent) v = v->parent;
		if (v->cls == &cls_layer) {
			// In another overlay (a submenu's anchor in a menu): over the
			// root that one is over.
			ov *p = ov_of_layer(v);
			return p && p->root && !p->root->dead ? p->root : v;
		}
		if (v->prov || v == v->r->el || !v->up) return v;
		v = v->up;
	}
	return v;
}

// anchor_view is the view the anchor Ref holds (one that makes a view of
// its own, or its first child that does), NULL for none.
static ak_view *anchor_view(const ov *o) {
	const ak_val *a = key_of(o->e, AK_KEY_OVERLAY_ANCHOR);
	if (!a || !a->p) return NULL;
	ak_view *t = ak_vm_cell_get(a->p, false).p;
	if (!t || ((const ak_obj *)t)->tag != AK_TAG_VIEW || t->dead) return NULL;
	while (t && !t->real && t->kids.len) t = t->kids.data[0];
	return t && t->real && !t->dead ? t : NULL;
}

static bool rtl_of(const ak_view *v) { return v && v->dir == AK_LAYOUT_DIRECTION_RIGHT_TO_LEFT; }

static bool dismisses_on(const ov *o, int reason) {
	const ak_val *d = key_of(o->e, AK_KEY_OVERLAY_DISMISS_ON);
	if (!d) return true;  // the default: all of them
	const ak_arr *a = d->p;
	for (uint32_t i = 0; a && i < a->len; i++)
		if (a->data[i].i == reason) return true;
	return false;
}

// layer_at is x's ith overlay layer, NULL when it went.
static ak_view *layer_at(const ak_rext *x, uint32_t i) {
	ak_view *l = ak_view_of(x->overlays.data[i]);
	return l && !l->dead ? l : NULL;
}

static ov *ov_of_layer(const ak_view *l) {
	ak_view *e = l ? l->up : NULL;
	return e && !e->dead && e->impl && ((ov *)e->impl)->layer == l ? e->impl : NULL;
}

// ---- Placing ----

static ak_place_rect bounds_of(const ak_view *layer) {
	const ak_prov *p = layer->prov;
	const ak_environment *env = &p->env;
	float bottom = env->safe_bottom > env->keyboard ? env->safe_bottom : env->keyboard;
	ak_place_rect b = {env->safe_left, env->safe_top, env->width - env->safe_left - env->safe_right, env->height - env->safe_top - bottom};
	if (b.w < 0) b.w = 0;
	if (b.h < 0) b.h = 0;
	return b;
}

// place puts the content where ak_place says for its size as last laid
// out; true when that moved it or changed its limits (lay it out again).
static bool place(ov *o) {
	ak_view *c = o->content;
	if (!c || c->dead || !c->box) return false;
	bool has_anchor = o->anchor_id != 0;
	if (has_anchor && !o->anchor_known) return false;
	ak_lframe f = ak_lnode_frame(c->box);
	int64_t align = key_i(o->e, AK_KEY_OVERLAY_ALIGN, AK_PLACE_ALIGN_CENTER);
	if (align == AK_PLACE_ALIGN_AUTO) align = AK_PLACE_ALIGN_CENTER;
	ak_place_opts opts = {
	    .side = (int)key_i(o->e, AK_KEY_OVERLAY_SIDE, AK_SIDE_BOTTOM),
	    .align = (int)align,
	    .offset = key_f(o->e, AK_KEY_OVERLAY_OFFSET, 0),
	    .align_offset = key_f(o->e, AK_KEY_OVERLAY_ALIGN_OFFSET, 0),
	    .gap = key_f(o->e, AK_KEY_OVERLAY_EDGE_GAP, 8),
	    .flip = key_b(o->e, AK_KEY_OVERLAY_FLIP, true),
	    .rtl = rtl_of(o->layer),
	    .has_anchor = has_anchor,
	};
	ak_place_rect b = bounds_of(o->layer);
	ak_place_result r = ak_place(o->anchor, f.w, f.h, b, &opts);
	// Its limits: the room on its side; as wide as the anchor (at least, or
	// exactly with align stretch) on its top or bottom, as tall on a side.
	float max_w = r.avail_w, max_h = r.avail_h, min_w = 0, fix_w = 0, fix_h = 0;
	bool across = r.side == AK_SIDE_BOTTOM || r.side == AK_SIDE_TOP;
	if (has_anchor && key_b(o->e, AK_KEY_OVERLAY_MATCH_WIDTH, false)) min_w = o->anchor.w < max_w ? o->anchor.w : max_w;
	if (has_anchor && align == AK_PLACE_ALIGN_STRETCH) {
		if (across) fix_w = o->anchor.w;
		else if (r.side != AK_SIDE_CENTER) fix_h = o->anchor.h;
	}
	bool limits = max_w != o->max_w || max_h != o->max_h || min_w != o->min_w || fix_w != o->fix_w || fix_h != o->fix_h;
	bool moved = r.x != o->x || r.y != o->y;
	o->x = r.x, o->y = r.y, o->max_w = max_w, o->max_h = max_h, o->min_w = min_w, o->fix_w = fix_w, o->fix_h = fix_h;
	if (!o->place_sent || r.side != o->placed.side || r.arrow != o->placed.arrow || r.avail_w != o->placed.avail_w ||
	    r.avail_h != o->placed.avail_h || f.w != o->placed_w || f.h != o->placed_h)
		o->place_due = true;  // reported after the flush, as a layout handler is
	o->placed = r;
	o->placed_w = f.w, o->placed_h = f.h;
	if (limits || moved) ak_style_relayout(c);
	return limits || moved;
}

static const uint8_t placed_kinds[] = {AK_K_INT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const char *const placed_names[] = {"side", "arrow", "width", "height", "availableWidth", "availableHeight"};
static const ak_shape *const placed_shapes[6];
const ak_shape ak_ui_shape_placed = {"Placed", 6, placed_kinds, placed_shapes, placed_names, 0, NULL, false, NULL};

static ak_struct *placed_value(const ov *o) {
	ak_struct *s = ak_struct_new(&ak_ui_shape_placed);
	s->f[0].i = o->placed.side;
	s->f[1].f = o->placed.arrow;
	s->f[2].f = o->placed_w;
	s->f[3].f = o->placed_h;
	s->f[4].f = o->placed.avail_w;
	s->f[5].f = o->placed.avail_h;
	return s;
}

bool ak_overlays_report_due(ak_ui_root *r) {
	ak_rext *x = r->ext;
	for (uint32_t i = 0; x && i < x->overlays.len; i++) {
		ov *o = ov_of_layer(layer_at(x, i));
		if (o && o->open && o->place_due) return true;
	}
	return false;
}

bool ak_overlays_report(ak_ui_root *r) {
	ak_rext *x = r->ext;
	bool called = false;
	for (uint32_t i = 0; x && i < x->overlays.len; i++) {
		ov *o = ov_of_layer(layer_at(x, i));
		if (!o || !o->open || !o->place_due) continue;
		o->place_due = false;
		o->place_sent = true;
		if (!ak_view_handler(o->e, AK_EV_OVERLAY_PLACE)) continue;
		ak_struct *s = placed_value(o);
		ak_val arg = {.p = s};
		ak_view *e = ak_retain(o->e);
		ak_view_call(e, AK_EV_OVERLAY_PLACE, &arg, 1);  // may close others: x->overlays is read again
		ak_release(e);
		ak_release(s);
		called = true;
	}
	return called;
}

void ak_overlays_layout(ak_ui_root *r, ak_lpass *p) {
	ak_rext *x = r->ext;
	if (!x) return;
	uint32_t live = 0;
	for (uint32_t i = 0; i < x->overlays.len; i++) {
		ak_view *l = layer_at(x, i);
		if (!l) continue;
		x->overlays.data[live++] = l->id;
		ov *o = ov_of_layer(l);
		if (!o || !o->open || !l->box) continue;
		ak_lsize size = {l->prov->env.width, l->prov->env.height};
		if (size.w <= 0 || size.h <= 0) continue;
		// Laid out (ak_root_layout_provs) with its last place: placed for
		// its size, and again for the size its new limits give it.
		for (int pass = 0; pass < 3 && place(o); pass++) ak_lnode_layout(l->box, size, p);
		if (o->anchor_id && o->anchor_known && l->real) {
			const ak_val *h = ak_style_get(l, AK_KEY_HIDDEN);
			if (h && h->i) ak_style_set(l, AK_KEY_HIDDEN, (ak_val){.i = 0});
		}
	}
	x->overlays.len = live;
}

// ---- Opening and closing ----

static ak_val escape_fn(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_view *e = c->caps[0].p;
	if (!e->dead && e->impl) ov_dismiss(e->impl, AK_DISMISS_REASON_ESCAPE);
	return (ak_val){0};
}

static const uint8_t ref_cap[] = {AK_K_REF};
static const uint8_t short_kinds[] = {AK_K_REF, AK_K_BOOL, AK_K_BOOL, AK_K_BOOL, AK_K_BOOL, AK_K_REF};
static const char *const short_names[] = {"key", "primary", "shift", "alt", "control", "run"};
static const ak_shape *const short_shapes[6];
static const ak_shape shape_shortcut = {"Shortcut", 6, short_kinds, short_shapes, short_names, 0, NULL, false, NULL};

// escape_keys is [Shortcut(key: "escape", run: dismiss)] for e.
static ak_arr *escape_keys(ak_view *e) {
	ak_struct *s = ak_struct_new(&shape_shortcut);
	s->f[0].p = ak_str_new("escape", 6);
	ak_closure *c = ak_closure_new(escape_fn, NULL, 1, ref_cap);
	c->caps[0].p = ak_retain(e);
	s->f[5].p = c;
	ak_arr *a = ak_arr_new(AK_K_REF, &shape_shortcut, 1);
	ak_arr_push(&a, (ak_val){.p = s});
	return a;
}

static void back_fn(void *ctx) {
	ak_view *e = ctx;
	if (!e->dead && e->impl) ov_dismiss(e->impl, AK_DISMISS_REASON_BACK);
}

static void make_layer(ov *o) {
	ak_view *e = o->e;
	ak_ui_root *r = e->r;
	ak_view *l = ak_view_new(r, &cls_layer, 0, true);
	ak_prov *pv = ak_prov_new(r);
	if (o->root && o->root->prov) ak_prov_follow(pv, o->root->prov);
	ak_prov_attach(l, pv);
	ak_release(pv);
	l->up = e;
	l->dir = o->root ? o->root->dir : e->dir;
	l->finalized = true;
	o->layer = l;
	ak_batch_set_int(r->batch, l->id, AK_PROP_OVERLAY_ROOT, o->root_id);
	ak_view_listen(l, AK_EVENT_OUTSIDE_PRESS);
	ak_view_listen(l, AK_EVENT_ANCHOR_FRAME);
	ak_vec_push(&ak_rext_of(r)->overlays, l->id);
}

// parent_of is the open overlay whose content e is written in.
static ov *parent_of(ak_view *e) {
	for (ak_view *v = e->parent; v; v = v->parent ? v->parent : (v->up && v->cls == &cls_layer ? v->up : NULL)) {
		if (v->cls == &cls_layer) return ov_of_layer(v);
	}
	return NULL;
}

static void send_anchor(ov *o) {
	ak_view *a = anchor_view(o);
	ak_id id = a ? a->id : 0;
	if (id == o->anchor_id) return;
	o->anchor_id = id;
	o->anchor_known = false;
	ak_batch_set_int(o->e->r->batch, o->layer->id, AK_PROP_OVERLAY_ANCHOR, id);
	if (id) ak_style_set(o->layer, AK_KEY_HIDDEN, (ak_val){.i = 1});  // until the host says where it is
}

static void ov_open(ov *o) {
	ak_view *e = o->e;
	if (o->open || !o->build || e->dead || !e->finalized) return;
	ak_ui_root *r = e->r;
	ak_view *a = anchor_view(o);
	ak_view *root = layout_root(a ? a : e);
	if (o->layer && (o->root_id != (root ? root->id : 0))) {
		// Its root changed (an anchor in a sheet now): a new layer.
		ak_view_destroy(o->layer);
		ak_release(o->layer);
		o->layer = NULL;
	}
	o->root = root;
	o->root_id = root ? root->id : 0;
	if (!o->layer) make_layer(o);
	ak_view *l = o->layer;
	o->open = true;
	o->parent = parent_of(e);
	o->anchor_id = (ak_id)-1;  // sent below
	o->anchor_known = false;
	o->place_sent = false;
	o->x = o->y = o->max_w = o->max_h = o->min_w = o->fix_w = o->fix_h = 0;
	send_anchor(o);
	ak_batch_set_bool(r->batch, l->id, AK_PROP_OVERLAY_MODAL, key_b(e, AK_KEY_OVERLAY_MODAL, false));
	o->owner = ak_root_under(e->owner);
	if (dismisses_on(o, AK_DISMISS_REASON_BACK)) {
		// The host's going back (Android's Back) comes to the top overlay
		// that takes it. Its content's OnBack goes to what's around it.
		o->back = ak_back_new(back_fn, e);
		ak_back_attach(o->back, l);
		ak_view_listen(l, AK_EVENT_BACK);
		ak_batch_set_bool(r->batch, l->id, AK_PROP_INTERCEPT_BACK, true);
	}
	ak_owner_set_context(o->owner, ak_env_key, l->prov);
	// The backdrop, under the content.
	// The backdrop, under the content: none when its builder shows nothing
	// (a component passing on a backdrop it wasn't given), which leaves a
	// non-modal overlay passing presses through.
	const ak_val *bd = key_of(e, AK_KEY_OVERLAY_BACKDROP);
	ak_view *bv = bd && bd->p && ((ak_obj *)bd->p)->tag == AK_TAG_CLOSURE ? ak_build_under(r, o->owner, bd->p, NULL, 0) : NULL;
	if (bv && bv->frag && bv->kids.len == 0) {
		ak_view_destroy(bv);
		ak_release(bv);
		bv = NULL;
	}
	if (bv) {
		bv = ak_only_kid(r, bv);  // it fills the backdrop (a dimming View with no size of its own)
		ak_view *b = ak_view_new(r, &cls_backdrop, 0, true);
		b->impl = o;
		b->finalized = true;
		ak_style_relayout(b);  // backdrop_adjust: it fills the layer
		ak_view_listen(b, AK_EVENT_PRESS);
		ak_view_append(b, bv);
		ak_release(bv);
		ak_view_append(l, b);
		o->backdrop = b;
	}
	ak_view *c = ak_view_new(r, &cls_content, 0, false);
	c->impl = o;
	c->finalized = true;
	ak_style_relayout(c);
	ak_view *v = ak_build_under(r, o->owner, o->build, NULL, 0);
	if (v) {
		ak_view_append(c, v);
		ak_release(v);
	}
	ak_view_append(l, c);
	o->content = c;
	if (dismisses_on(o, AK_DISMISS_REASON_ESCAPE)) {
		ak_arr *keys = escape_keys(e);
		ak_keys_register(ak_keys_window(), keys, &o->keys);
		ak_release(keys);
	}
	ak_batch_command(r->batch, l->id, AK_CMD_PRESENT, NULL, 0);
}

static void ov_close(ov *o) {
	if (!o->open) return;
	o->open = false;
	if (o->keys) {
		ak_keys_forget(o->keys);
		o->keys = NULL;
	}
	ak_view *l = o->layer;
	if (o->back && l && !l->dead) ak_batch_set_bool(l->r->batch, l->id, AK_PROP_INTERCEPT_BACK, false);
	ak_back_release(o->back);
	o->back = NULL;
	// Its views go (in animate { }, playing their transitions in the layer).
	if (o->owner) {
		ak_owner *own = o->owner;
		o->owner = NULL;
		ak_owner_dispose(own);
	}
	ak_view *c = o->content, *b = o->backdrop;
	o->content = o->backdrop = NULL;
	if (c) {
		c->impl = NULL;
		if (l && !l->dead) ak_view_remove(l, c, true);
		ak_release(c);
	}
	if (b) {
		b->impl = NULL;
		if (l && !l->dead) ak_view_remove(l, b, true);
		ak_release(b);
	}
	if (l && !l->dead) ak_batch_set_bool(l->r->batch, l->id, AK_PROP_OVERLAY_MODAL, false);  // passes presses through, empty
}

// ov_dismiss is the user's closing it for reason: open false, reported.
static void ov_dismiss(ov *o, int reason) {
	if (!o->open || !dismisses_on(o, reason)) return;
	ak_view *e = o->e;
	ak_retain(e);
	ak_val f = {.i = 0};
	ak_view_call(e, AK_EV_OVERLAY_CHANGE, &f, 1);
	ak_val why = {.i = reason};
	ak_view_call(e, AK_EV_OVERLAY_DISMISS, &why, 1);
	ak_release(e);
}

// ---- Events ----

static bool in_rect(ak_place_rect r, double x, double y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// in_child reports whether (x, y) of o's root is on an open overlay
// opened from o's content (a submenu), or from one of those.
static bool in_child(const ov *o, double x, double y) {
	ak_rext *xr = o->e->r->ext;
	for (uint32_t i = 0; xr && i < xr->overlays.len; i++) {
		ov *c = ov_of_layer(layer_at(xr, i));
		if (!c || !c->open || c == o || !c->content || !c->content->box || c->root_id != o->root_id) continue;
		bool under = false;
		for (ov *p = c->parent; p && !under; p = p->parent) under = p == o;
		if (!under) continue;
		ak_lframe f = ak_lnode_frame(c->content->box);
		if (in_rect((ak_place_rect){f.x, f.y, f.w, f.h}, x, y)) return true;
	}
	return false;
}

static void layer_event(ak_view *l, const ak_event *ev) {
	ov *o = ov_of_layer(l);
	if (!o) return;
	switch (ev->kind) {
	case AK_EVENT_ANCHOR_FRAME: {
		if (ev->s.len < 17) break;
		const uint8_t *p = (const uint8_t *)ev->s.p;
		float f[4];
		memcpy(f, p, sizeof f);
		ak_place_rect a = {f[0], f[1], f[2], f[3]};
		bool visible = p[16] != 0;
		bool changed = !o->anchor_known || memcmp(&a, &o->anchor, sizeof a) != 0;
		o->anchor = a;
		o->anchor_known = true;
		o->anchor_visible = visible;
		if (!visible && o->open) {
			ov_dismiss(o, AK_DISMISS_REASON_ANCHOR_HIDDEN);
			break;
		}
		if (changed && o->content) ak_style_relayout(o->content);  // placed again at the flush
		break;
	}
	case AK_EVENT_OUTSIDE_PRESS:
		if (o->anchor_id && o->anchor_known && in_rect(o->anchor, ev->x, ev->y)) break;  // the trigger's own
		if (in_child(o, ev->x, ev->y)) break;
		ov_dismiss(o, AK_DISMISS_REASON_OUTSIDE_PRESS);
		break;
	case AK_EVENT_BACK:
		if (o->back) ak_back_run(o->back);
		break;
	}
}

static bool within(const ak_view *v, const ak_view *a) {
	for (; v; v = v->parent)
		if (v == a) return true;
	return false;
}

// holds reports whether f is in o's views, its anchor, or a child
// overlay's views.
static bool holds(const ov *o, const ak_view *f) {
	if ((o->content && within(f, o->content)) || (o->backdrop && within(f, o->backdrop))) return true;
	ak_view *a = anchor_view(o);
	if (a && within(f, a)) return true;
	ak_rext *xr = o->e->r->ext;
	for (uint32_t i = 0; xr && i < xr->overlays.len; i++) {
		ov *c = ov_of_layer(layer_at(xr, i));
		if (!c || !c->open || c == o) continue;
		bool under = false;
		for (ov *p = c->parent; p && !under; p = p->parent) under = p == o;
		if (under && c->content && within(f, c->content)) return true;
	}
	return false;
}

void ak_overlay_focus_moved(ak_ui_root *r, ak_view *f) {
	ak_rext *x = r->ext;
	if (!f || !x) return;  // the focus gone nowhere: a press outside says it
	for (uint32_t i = x->overlays.len; i-- > 0;) {
		if (i >= x->overlays.len) continue;  // a dismissal closed others
		ov *o = ov_of_layer(layer_at(x, i));
		if (o && o->open && !holds(o, f)) ov_dismiss(o, AK_DISMISS_REASON_FOCUS_OUT);
	}
}

// ---- The element ----

static void ov_cleanup(void *ctx) {
	ak_view *e = ctx;
	ov *o = e->impl;
	if (o) {
		ov_close(o);
		if (o->layer) {
			ak_view_destroy(o->layer);
			ak_release(o->layer);
			o->layer = NULL;
		}
	}
	ak_release(e);
}

static void ov_init(ak_view *e) {
	ov *o = ak_alloc(sizeof(ov));
	o->e = e;
	e->impl = o;
	ak_on_cleanup(ov_cleanup, ak_retain(e));
}

static void ov_drop(ak_view *e) {
	ov *o = e->impl;
	if (!o) return;
	if (o->owner) ak_owner_dispose(o->owner);
	if (o->keys) ak_keys_forget(o->keys);
	ak_back_release(o->back);
	if (o->content) {
		o->content->impl = NULL;
		ak_release(o->content);
	}
	if (o->backdrop) {
		o->backdrop->impl = NULL;
		ak_release(o->backdrop);
	}
	if (o->layer) {
		ak_view_destroy(o->layer);
		ak_release(o->layer);
	}
	ak_release(o->build);
	ak_free(o);
	e->impl = NULL;
}

static void ov_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(prev);
	ov *o = e->impl;
	switch (key) {
	case AK_KEY_OVERLAY_OPEN:
		if (v.i) ov_open(o);
		else ov_close(o);
		break;
	case AK_KEY_OVERLAY_ANCHOR:
		if (o->open) send_anchor(o);
		break;
	case AK_KEY_OVERLAY_MODAL:
		if (o->open && o->layer) ak_batch_set_bool(e->r->batch, o->layer->id, AK_PROP_OVERLAY_MODAL, v.i != 0);
		break;
	case AK_KEY_OVERLAY_SIDE:
	case AK_KEY_OVERLAY_ALIGN:
	case AK_KEY_OVERLAY_OFFSET:
	case AK_KEY_OVERLAY_ALIGN_OFFSET:
	case AK_KEY_OVERLAY_FLIP:
	case AK_KEY_OVERLAY_EDGE_GAP:
	case AK_KEY_OVERLAY_MATCH_WIDTH:
		if (o->open && o->content) ak_style_relayout(o->content);  // placed again
		break;
	}
}

static void ov_finalize(ak_view *e) {
	if (key_b(e, AK_KEY_OVERLAY_OPEN, false)) ov_open(e->impl);
}

static void ov_lazy(ak_view *e, ak_closure *build) {
	ov *o = e->impl;
	ak_retain(build);
	ak_release(o->build);
	o->build = build;
	if (key_b(e, AK_KEY_OVERLAY_OPEN, false)) ov_open(o);
}

// ak_overlays_sync sends an open overlay's anchor again when its Ref
// holds another view (or one now that it didn't): at the flush.
void ak_overlays_sync(ak_ui_root *r) {
	ak_rext *x = r->ext;
	for (uint32_t i = 0; x && i < x->overlays.len; i++) {
		ov *o = ov_of_layer(layer_at(x, i));
		if (!o || !o->open) continue;
		if (o->anchor_id && !anchor_view(o)) {
			ov_dismiss(o, AK_DISMISS_REASON_ANCHOR_HIDDEN);  // its anchor went
			continue;
		}
		send_anchor(o);
	}
}

const ak_el_class ak_class_overlay = {.name = "Overlay", .kind = AK_KIND_VIEW, .flags = AK_ELF_FRAGMENT | AK_ELF_FLATTEN | AK_ELF_CHILDREN,
                                      .init = ov_init, .set = ov_set, .finalize = ov_finalize, .lazy = ov_lazy, .drop = ov_drop};

void ak_overlays_layout_hook(ak_ui_root *r, ak_lpass *p) { ak_overlays_layout(r, p); }
