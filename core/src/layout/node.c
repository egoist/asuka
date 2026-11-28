// node.c: the tree, the caches, the box model, leaves and absolutely
// positioned children.
#include "layout/internal.h"

ak_lstyle ak_lstyle_default(void) {
	ak_lstyle s;
	memset(&s, 0, sizeof s);
	s.flex_shrink = 1;
	s.margin = ak_lrect_all(ak_lpt(0));
	return s;
}

// Scratch memory.

l_mark l_scratch_mark(ak_lpass *p) {
	if (p->nchunks_ == 0) return (l_mark){0, 0};
	return (l_mark){p->cur_, p->chunks_[p->cur_].used};
}

void l_scratch_release(ak_lpass *p, l_mark m) {
	if (p->nchunks_ == 0) return;
	p->cur_ = m.chunk;
	p->chunks_[m.chunk].used = m.used;
}

void *l_scratch(ak_lpass *p, size_t n) {
	n = (n + 15) & ~(size_t)15;
	if (n == 0) n = 16;
	if (p->nchunks_ > 0) {
		struct ak_lchunk *c = &p->chunks_[p->cur_];
		if (c->used + n <= c->cap) {
			void *r = c->data + c->used;
			c->used += n;
			return r;
		}
	}
	// The next chunk, made (or remade) big enough: nothing in it is in use.
	uint32_t next = p->nchunks_ == 0 ? 0 : p->cur_ + 1;
	size_t prev = p->nchunks_ == 0 ? 0 : p->chunks_[p->cur_].cap;
	if (next == p->nchunks_) {
		p->chunks_ = ak_realloc(p->chunks_, sizeof(struct ak_lchunk) * (next + 1));
		p->chunks_[next] = (struct ak_lchunk){0};
		p->nchunks_ = next + 1;
	}
	struct ak_lchunk *c = &p->chunks_[next];
	if (c->cap < n) {
		size_t cap = prev * 2;
		if (cap < 16384) cap = 16384;
		while (cap < n) cap *= 2;
		ak_free(c->data);
		c->data = ak_alloc(cap);
		c->cap = cap;
	}
	p->cur_ = next;
	c->used = n;
	return c->data;
}

void ak_lpass_reset(ak_lpass *p) {
	p->nchanged = 0;
	p->measures = 0;
}

void ak_lpass_free(ak_lpass *p) {
	for (uint32_t i = 0; i < p->nchunks_; i++) ak_free(p->chunks_[i].data);
	ak_free(p->chunks_);
	ak_free(p->changed);
	memset(p, 0, sizeof *p);
}

uint64_t ak_lnode_computed;

// The cache.

static bool same_f(float a, float b) { return a == b || (a != a && b != b); }

static bool same_avail(float a, float b) {
	if (isinf(a) || isinf(b)) return a == b;
	float d = a - b;
	return d < 0.0001f && d > -0.0001f;
}

static bool entry_matches(const l_cache_entry *e, const l_input *in) {
	if (e->sizing != in->sizing || e->coll[0] != in->collapse[0] || e->coll[1] != in->collapse[1]) return false;
	if (!(same_f(in->known.w, e->known.w) || in->known.w == e->out.size.w) ||
		!(same_f(in->known.h, e->known.h) || in->known.h == e->out.size.h))
		return false;
	return (l_isdef(in->known.w) || same_avail(e->avail.w, in->avail.w)) &&
		(l_isdef(in->known.h) || same_avail(e->avail.h, in->avail.h));
}

static bool same_output(const l_output *a, const l_output *b) {
	return same_f(a->size.w, b->size.w) && same_f(a->size.h, b->size.h) && same_f(a->baseline, b->baseline) &&
		same_f(a->top.pos, b->top.pos) && same_f(a->top.neg, b->top.neg) && same_f(a->bottom.pos, b->bottom.pos) &&
		same_f(a->bottom.neg, b->bottom.neg) && a->collapses_through == b->collapses_through;
}

// cache_get answers in from the cache, noting the generation of the
// parent's cache that got the answer (pgen).
static bool cache_get(l_cache *c, const l_input *in, l_output *out, uint32_t pgen) {
	if (c->has_final && entry_matches(&c->final, in)) {
		if (in->mode == MODE_SIZE) c->final_shared = true;
		c->final.pgen = pgen;
		*out = c->final.out;
		return true;
	}
	if (in->mode == MODE_LAYOUT) return false;
	for (uint8_t i = 0; i < c->n; i++) {
		if (entry_matches(&c->slots[i], in)) {
			c->slots[i].pgen = pgen;
			*out = c->slots[i].out;
			return true;
		}
	}
	return false;
}

// cache_put keeps what n answered, asked by the computation caller (0:
// none, a layout's root) in caller_mode (MODE_SIZE + 1, MODE_LAYOUT + 1;
// 0: none). A final the parent may still depend on, replaced, is an
// answer lost: one that answered its measuring, or that its measuring
// asked for (a baseline), or that the same computation of it asked for
// before.
static void cache_put(l_cache *c, const l_input *in, l_output out, uint64_t caller, uint8_t caller_mode, uint32_t pgen) {
	l_cache_entry e = {in->mode, in->sizing, {in->collapse[0], in->collapse[1]}, in->known, in->parent, in->avail, out, pgen, c->seq++};
	if (in->mode == MODE_LAYOUT) {
		if (c->has_final && (c->final_shared || caller_mode == MODE_SIZE + 1 || (caller && c->final_by == caller))) c->lossy = true;
		c->final = e;
		c->has_final = true;
		c->final_shared = false;
		c->final_by = caller;
		return;
	}
	if (c->n == L_MEASURE_SLOTS) c->lossy = true;
	c->slots[c->next] = e;
	c->next = (uint8_t)((c->next + 1) % L_MEASURE_SLOTS);
	if (c->n < L_MEASURE_SLOTS) c->n++;
}

static void cache_clear(l_cache *c) {
	c->gen++;
	c->has_final = false;
	c->lossy = false;
	c->final_shared = false;
	c->seq = 0;
	c->n = 0;
	c->next = 0;
}

// Nodes.

ak_lnode *ak_lnode_new(void) {
	ak_lnode *n = ak_alloc(sizeof *n);
	n->style = ak_lstyle_default();
	n->dirty = true;
	n->content_max = n->box_max = (ak_lsize){LINF, LINF};
	for (int i = 0; i < 4; i++) n->reported[i] = LNAN;
	return n;
}

static void remove_child(ak_lnode *n, ak_lnode *c);
static l_output compute_dir(ak_lnode *n, l_input in, bool rtl, ak_lpass *p);
static l_output leaf(ak_lnode *n, const l_input *in, ak_lpass *p);

void ak_lnode_free(ak_lnode *n) {
	if (!n) return;
	if (n->parent) ak_lnode_remove_child(n->parent, n);
	for (uint32_t i = 0; i < n->nchildren; i++) {
		if (n->children[i]->parent == n) n->children[i]->parent = NULL;
	}
	ak_free(n->children);
	ak_free(n);
}

void ak_lnode_free_tree(ak_lnode *n) {
	if (!n) return;
	if (n->parent) ak_lnode_remove_child(n->parent, n);
	while (n->nchildren > 0) ak_lnode_free_tree(n->children[n->nchildren - 1]);
	ak_free(n->children);
	ak_free(n);
}

ak_lstyle *ak_lnode_style(ak_lnode *n) { return &n->style; }

void ak_lnode_set_style(ak_lnode *n, const ak_lstyle *s) {
	bool flipped = s->direction != n->style.direction;
	n->style = *s;
	if (flipped) ak_lnode_mark_subtree_dirty(n);
	else ak_lnode_mark_dirty(n);
}

void *ak_lnode_context(const ak_lnode *n) { return n->context; }
void ak_lnode_set_context(ak_lnode *n, void *ctx) { n->context = ctx; }

void ak_lnode_set_measure(ak_lnode *n, ak_lmeasure_fn fn, void *ctx) {
	n->measure = fn;
	n->measure_ctx = ctx;
	ak_lnode_mark_content_dirty(n);
}

void ak_lnode_set_content_max(ak_lnode *n, float w, float h) {
	n->content_max = (ak_lsize){l_isnan(w) ? LINF : w, l_isnan(h) ? LINF : h};
}

void ak_lnode_set_baseline(ak_lnode *n, ak_lbaseline_fn fn, void *ctx) {
	n->baseline = fn;
	n->baseline_ctx = ctx;
}

void ak_lnode_mark_dirty(ak_lnode *n) {
	// What its host reported of its content is reported again as it's
	// measured again.
	n->content_max = (ak_lsize){LINF, LINF};
	for (ak_lnode *p = n; p; p = p->parent) {
		if (p->dirty && p != n) return;
		p->dirty = true;
		cache_clear(&p->cache);
	}
}

static void mark_below(ak_lnode *n) {
	n->dirty = true;
	cache_clear(&n->cache);
	for (uint32_t i = 0; i < n->nchildren; i++) mark_below(n->children[i]);
}

void ak_lnode_mark_subtree_dirty(ak_lnode *n) {
	ak_lnode_mark_dirty(n);
	for (uint32_t i = 0; i < n->nchildren; i++) mark_below(n->children[i]);
}

void ak_lnode_mark_content_dirty(ak_lnode *n) {
	n->content_max = (ak_lsize){LINF, LINF};
	if (n->dirty || n->stale) return;
	if (!n->parent) {
		ak_lnode_mark_dirty(n);
		return;
	}
	n->stale = true;
	for (ak_lnode *a = n; a && !a->revalidate; a = a->parent) a->revalidate = true;
}

bool ak_lnode_dirty(const ak_lnode *n) { return n->dirty || n->revalidate; }
ak_lnode *ak_lnode_parent(const ak_lnode *n) { return n->parent; }

ak_lnode *const *ak_lnode_children(const ak_lnode *n, uint32_t *count) {
	if (count) *count = n->nchildren;
	return n->children;
}

uint32_t ak_lnode_child_count(const ak_lnode *n) { return n->nchildren; }

ak_lnode *ak_lnode_child(const ak_lnode *n, uint32_t i) { return i < n->nchildren ? n->children[i] : NULL; }

static void reserve_children(ak_lnode *n, uint32_t count) {
	if (count <= n->cap_children) return;
	uint32_t cap = n->cap_children ? n->cap_children * 2 : 4;
	while (cap < count) cap *= 2;
	n->children = ak_realloc(n->children, sizeof(ak_lnode *) * cap);
	n->cap_children = cap;
}

void ak_lnode_set_children(ak_lnode *n, ak_lnode *const *children, uint32_t count) {
	for (uint32_t i = 0; i < n->nchildren; i++) {
		if (n->children[i]->parent == n) n->children[i]->parent = NULL;
	}
	if (count > n->cap_children) {
		// A new array: children may point into the old one.
		uint32_t cap = n->cap_children ? n->cap_children : 4;
		while (cap < count) cap *= 2;
		ak_lnode **a = ak_alloc(sizeof(ak_lnode *) * cap);
		memcpy(a, children, sizeof(ak_lnode *) * count);
		ak_free(n->children);
		n->children = a;
		n->cap_children = cap;
	} else if (count > 0) {
		memmove(n->children, children, sizeof(ak_lnode *) * count);
	}
	n->nchildren = count;
	for (uint32_t i = 0; i < count; i++) {
		ak_lnode *c = n->children[i];
		if (c->parent && c->parent != n) remove_child(c->parent, c);
		c->parent = n;
		ak_lnode_force_report(c);
	}
	ak_lnode_mark_content_dirty(n);
}

void ak_lnode_append_child(ak_lnode *n, ak_lnode *c) {
	if (c->parent) remove_child(c->parent, c);
	c->parent = n;
	ak_lnode_force_report(c);
	reserve_children(n, n->nchildren + 1);
	n->children[n->nchildren++] = c;
	ak_lnode_mark_content_dirty(n);
}

void ak_lnode_insert_child(ak_lnode *n, ak_lnode *c, uint32_t index) {
	if (c->parent) remove_child(c->parent, c);
	c->parent = n;
	ak_lnode_force_report(c);
	reserve_children(n, n->nchildren + 1);
	if (index > n->nchildren) index = n->nchildren;
	memmove(n->children + index + 1, n->children + index, sizeof(ak_lnode *) * (n->nchildren - index));
	n->children[index] = c;
	n->nchildren++;
	ak_lnode_mark_content_dirty(n);
}

void ak_lnode_remove_child(ak_lnode *n, ak_lnode *c) {
	if (c->parent == n) {
		remove_child(n, c);
		c->parent = NULL;
	}
}

void ak_lnode_detach(ak_lnode *const *nodes, uint32_t count) {
	for (uint32_t i = 0; i < count; i++)
		if (nodes[i]->parent) nodes[i]->detaching = true;
	for (uint32_t i = 0; i < count; i++) {
		ak_lnode *p = nodes[i]->parent;
		if (!p || !nodes[i]->detaching) continue;  // its parent's done
		uint32_t k = 0;
		for (uint32_t j = 0; j < p->nchildren; j++) {
			ak_lnode *c = p->children[j];
			if (c->detaching && c->parent == p) {
				c->detaching = false;
				c->parent = NULL;
			} else {
				p->children[k++] = c;
			}
		}
		p->nchildren = k;
		ak_lnode_mark_content_dirty(p);
	}
}

static void remove_child(ak_lnode *n, ak_lnode *c) {
	for (uint32_t i = 0; i < n->nchildren; i++) {
		if (n->children[i] == c) {
			memmove(n->children + i, n->children + i + 1, sizeof(ak_lnode *) * (n->nchildren - i - 1));
			n->nchildren--;
			ak_lnode_mark_content_dirty(n);
			return;
		}
	}
}

void ak_lnode_force_report(ak_lnode *n) { n->reported[0] = LNAN; }

ak_lframe ak_lnode_frame(const ak_lnode *n) { return (ak_lframe){n->x, n->y, n->w, n->h}; }

// Relayout boundaries.

// revalidate asks stale n again what its cache says it was asked, in the
// order its answers were stored (the final layout among them, which
// places its children), and says whether every answer is the same: then
// its parent's layout stands. Replayed in order, each question that
// found an answer stored before finds the same one, as when it was first
// asked, so the cache ends as a layout from clean caches would leave it.
// Answers the parent got before its cache was last cleared aren't asked
// again (none of its answers rest on them: a host's measurement saved).
// It can't tell when an answer was dropped from the cache (lossy) or n
// wasn't laid out since: those count as changed.
static bool revalidate(ak_lnode *n, ak_lpass *p) {
	l_cache *c = &n->cache;
	uint32_t pgen = n->parent->cache.gen;
	bool ok = !c->lossy && c->has_final && c->final.pgen == pgen;
	// The answers to ask again, copied out (the cache is cleared as
	// they're asked): the parent's, by seq, at most nine.
	l_cache_entry es[L_MEASURE_SLOTS + 1];
	uint32_t ne = 0;
	if (ok) {
		for (uint8_t k = 0; k < c->n; k++)
			if (c->slots[k].pgen == pgen) es[ne++] = c->slots[k];
		es[ne++] = c->final;
	}
	cache_clear(c);
	n->dirty = true;
	if (!ok) return false;
	// Its frame narrows to what its host draws: a limit that changed
	// moves it, though its answers are the same.
	ak_lsize box_max = n->box_max;
	for (uint32_t i = 1; i < ne; i++)
		for (uint32_t j = i; j > 0 && es[j - 1].seq > es[j].seq; j--) {
			l_cache_entry t = es[j];
			es[j] = es[j - 1];
			es[j - 1] = t;
		}
	for (uint32_t i = 0; i < ne; i++) {
		const l_cache_entry *e = &es[i];
		l_input in = {.mode = e->mode, .sizing = e->sizing, .collapse = {e->coll[0], e->coll[1]}, .known = e->known, .parent = e->parent, .avail = e->avail};
		l_output out = l_compute(n, in, p);
		if (!same_output(&out, &e->out)) return false;
	}
	return same_f(box_max.w, n->box_max.w) && same_f(box_max.h, n->box_max.h);
}

// resolve settles the stale nodes at and below n, deepest first, before
// top (n's tree's layout root) is laid out: each is revalidated, and
// where an answer changed its parent is stale in turn. A stale top is
// laid out afresh (its ancestors' caches dropped, as ak_lnode_mark_dirty
// does).
static void resolve(ak_lnode *n, ak_lnode *top, ak_lpass *p) {
	n->revalidate = false;
	for (uint32_t i = 0; i < n->nchildren; i++) {
		ak_lnode *c = n->children[i];
		if (c->revalidate) resolve(c, top, p);
	}
	if (!n->stale) return;
	n->stale = false;
	if (n->dirty) return;  // dropped already, and its ancestors
	if (n == top || !n->parent) {
		ak_lnode_mark_dirty(n);
		return;
	}
	if (!revalidate(n, p)) {
		ak_lnode *parent = n->parent;
		if (!parent->dirty) parent->stale = true;
	}
}

// Layout.

static ak_lsize layout_root(ak_lnode *n, ak_lsize avail, ak_lpass *p, int mode);

ak_lsize ak_lnode_layout(ak_lnode *n, ak_lsize avail, ak_lpass *p) { return layout_root(n, avail, p, MODE_LAYOUT); }

ak_lsize ak_lnode_measure(ak_lnode *n, ak_lsize avail, ak_lpass *p) { return layout_root(n, avail, p, MODE_SIZE); }

static ak_lsize layout_root(ak_lnode *n, ak_lsize avail, ak_lpass *p, int mode) {
	if (n->revalidate) resolve(n, n, p);
	ak_lsize parent = {LNAN, LNAN};
	if (l_definite(avail.w)) parent.w = avail.w;
	if (l_definite(avail.h)) parent.h = avail.h;
	ak_lsize known = l_style_size(n, parent);
	ak_lsize min = l_style_min(n, parent), max = l_style_max(n, parent);
	known.w = l_clampn(known.w, min.w, max.w);
	known.h = l_clampn(known.h, min.h, max.h);
	ak_ledges pad, bor;
	l_padding_border(n, parent.w, &pad, &bor);
	if (l_isdef(known.w)) known.w = l_maxf(known.w, pad.left + pad.right + bor.left + bor.right);
	if (l_isdef(known.h)) known.h = l_maxf(known.h, pad.top + pad.bottom + bor.top + bor.bottom);
	// The root's direction comes from its style and its ancestors': they
	// aren't being laid out. Below it each node reads its parent's,
	// resolved as the parent is computed.
	bool rtl = false;
	for (const ak_lnode *a = n; a; a = a->parent) {
		if (a->style.direction != AK_LDIR_INHERIT) {
			rtl = a->style.direction == AK_LDIR_RTL;
			break;
		}
	}
	l_input in = {.mode = (uint8_t)mode, .known = known, .parent = parent, .avail = avail};
	l_output out = compute_dir(n, in, rtl, p);
	if (mode != MODE_LAYOUT) return out.size;
	ak_ledges m = l_margins_of(n, parent.w);
	l_place(p, n, m.left, m.top, out.size);
	return out.size;
}

void l_place(ak_lpass *p, ak_lnode *n, float x, float y, ak_lsize s) {
	// A leaf its host draws no larger than box_max: its frame is what's
	// drawn, centered in the box it was given.
	if (n->measure && n->nchildren == 0) {
		if (s.w > n->box_max.w) {
			x += (s.w - n->box_max.w) / 2;
			s.w = n->box_max.w;
		}
		if (s.h > n->box_max.h) {
			y += (s.h - n->box_max.h) / 2;
			s.h = n->box_max.h;
		}
	}
	n->x = x;
	n->y = y;
	n->w = s.w;
	n->h = s.h;
	float f[4] = {x, y, s.w, s.h};
	if (f[0] != n->reported[0] || f[1] != n->reported[1] || f[2] != n->reported[2] || f[3] != n->reported[3]) {
		memcpy(n->reported, f, sizeof f);
		if (p->nchanged == p->cap_changed) {
			p->cap_changed = p->cap_changed ? p->cap_changed * 2 : 64;
			p->changed = ak_realloc(p->changed, sizeof(ak_lnode *) * p->cap_changed);
		}
		p->changed[p->nchanged++] = n;
	}
}

// l_rtl_under is n's direction inside a box whose direction is parent.
bool l_rtl_under(const ak_lnode *n, bool parent) {
	uint8_t d = n->style.direction;
	if (d != AK_LDIR_INHERIT) return d == AK_LDIR_RTL;
	return parent;
}

// l_compute runs n under in. Only n's parent calls it, in the middle of
// its own computation, so the parent's direction is current.
l_output l_compute(ak_lnode *n, l_input in, ak_lpass *p) {
	bool parent = n->parent ? n->parent->rtl : false;
	return compute_dir(n, in, l_rtl_under(n, parent), p);
}

static l_output compute_dir(ak_lnode *n, l_input in, bool rtl, ak_lpass *p) {
	// The cache's keys don't say the direction: answers made under the
	// other one are dropped here, however it changed (a new parent, an
	// ancestor's style), and the children come through here in turn when
	// n recomputes.
	if (rtl != n->rtl) {
		n->rtl = rtl;
		cache_clear(&n->cache);
	}
	if (n->stale) {
		// Asked before a layout from its root resolved it (a layout of a
		// subtree beside it): its answers are dropped, as before.
		n->stale = false;
		ak_lnode_mark_dirty(n);
	}
	l_output out;
	uint32_t pgen = n->parent ? n->parent->cache.gen : 0;
	if (cache_get(&n->cache, &in, &out, pgen)) return out;
	ak_lnode_computed++;
	static uint64_t seq;  // computations, numbered (main thread only)
	uint64_t caller = p->comp_;
	uint8_t caller_mode = p->mode_;
	p->comp_ = ++seq;
	p->mode_ = (uint8_t)(in.mode + 1);
	if (n->style.display == AK_LDISPLAY_NONE) {
		out = (l_output){.baseline = LNAN};
		if (in.mode == MODE_LAYOUT) l_hide(n, p);
	} else if (n->nchildren == 0) {
		out = leaf(n, &in, p);
	} else if (n->style.display == AK_LDISPLAY_BLOCK) {
		out = l_block(n, &in, p);
	} else if (n->style.display == AK_LDISPLAY_GRID) {
		out = l_grid(n, &in, p);
	} else {
		out = l_flex(n, &in, p);
	}
	p->comp_ = caller;
	p->mode_ = caller_mode;
	cache_put(&n->cache, &in, out, caller, caller_mode, n->parent ? n->parent->cache.gen : 0);
	if (in.mode == MODE_LAYOUT) n->dirty = false;
	return out;
}

// l_hide gives a display: none subtree zero frames.
void l_hide(ak_lnode *n, ak_lpass *p) {
	for (uint32_t i = 0; i < n->nchildren; i++) {
		ak_lnode *c = n->children[i];
		l_place(p, c, 0, 0, (ak_lsize){0, 0});
		l_hide(c, p);
		c->dirty = false;
	}
}

// l_measure_size returns the border-box size of n under in.
ak_lsize l_measure_size(ak_lnode *n, l_input in, ak_lpass *p) {
	in.mode = MODE_SIZE;
	return l_compute(n, in, p).size;
}

// The resolved box model.

static ak_ledges resolve_rect(const ak_lrect *r, float basis) {
	return (ak_ledges){l_resolve0(r->top, basis), l_resolve0(r->right, basis), l_resolve0(r->bottom, basis), l_resolve0(r->left, basis)};
}

static bool zero_length(ak_llength l) { return l.v == 0 && l.u == AK_LUNIT_AUTO; }

static bool zero_rect(const ak_lrect *r) {
	return zero_length(r->top) && zero_length(r->right) && zero_length(r->bottom) && zero_length(r->left);
}

static bool pct_rect(const ak_lrect *r) {
	return r->top.u == AK_LUNIT_PERCENT || r->right.u == AK_LUNIT_PERCENT || r->bottom.u == AK_LUNIT_PERCENT || r->left.u == AK_LUNIT_PERCENT;
}

ak_ledges ak_lnode_content_insets(const ak_lnode *n) {
	if (zero_rect(&n->style.padding) && zero_rect(&n->style.border)) return (ak_ledges){0, 0, 0, 0};
	float cw = 0;
	// The parent's content width is a percentage's basis; points don't
	// read it (and finding it climbs the ancestors).
	if (n->parent && (pct_rect(&n->style.padding) || pct_rect(&n->style.border))) {
		const ak_lnode *p = n->parent;
		ak_ledges pi = ak_lnode_content_insets(p);
		cw = p->w - pi.left - pi.right;
		if (!(cw != cw) && cw < 0) cw = 0;  // Go's max(v, 0): NaN stays NaN
	}
	ak_ledges pad, bor;
	l_padding_border(n, cw, &pad, &bor);
	return (ak_ledges){pad.top + bor.top, pad.right + bor.right, pad.bottom + bor.bottom, pad.left + bor.left};
}

void l_padding_border(const ak_lnode *n, float parent_w, ak_ledges *pad, ak_ledges *border) {
	*pad = resolve_rect(&n->style.padding, parent_w);
	*border = resolve_rect(&n->style.border, parent_w);
}

ak_ledges l_margins_of(const ak_lnode *n, float parent_w) { return resolve_rect(&n->style.margin, parent_w); }

ak_lsize l_box_adjust(const ak_lnode *n, float parent_w) {
	if (n->style.box_sizing != AK_LCONTENT_BOX) return (ak_lsize){0, 0};
	ak_ledges pad, bor;
	l_padding_border(n, parent_w, &pad, &bor);
	return (ak_lsize){pad.left + pad.right + bor.left + bor.right, pad.top + pad.bottom + bor.top + bor.bottom};
}

ak_lsize l_apply_aspect(ak_lsize s, float ratio) {
	if (ratio > 0) {
		if (l_isdef(s.w) && l_isnan(s.h)) s.h = s.w / ratio;
		else if (l_isdef(s.h) && l_isnan(s.w)) s.w = s.h * ratio;
	}
	return s;
}

// l_style_size is the node's width/height as border-box sizes (NaN:
// auto).
ak_lsize l_style_size(const ak_lnode *n, ak_lsize parent) {
	const ak_lstyle *st = &n->style;
	ak_lsize s = l_apply_aspect((ak_lsize){l_resolve(st->width, parent.w), l_resolve(st->height, parent.h)}, st->aspect_ratio);
	ak_lsize adj = l_box_adjust(n, parent.w);
	return (ak_lsize){s.w + adj.w, s.h + adj.h};
}

ak_lsize l_style_min(const ak_lnode *n, ak_lsize parent) {
	const ak_lstyle *st = &n->style;
	ak_lsize s = l_apply_aspect((ak_lsize){l_resolve(st->min_width, parent.w), l_resolve(st->min_height, parent.h)}, st->aspect_ratio);
	ak_lsize adj = l_box_adjust(n, parent.w);
	return (ak_lsize){s.w + adj.w, s.h + adj.h};
}

ak_lsize l_style_max(const ak_lnode *n, ak_lsize parent) {
	const ak_lstyle *st = &n->style;
	ak_lsize adj = l_box_adjust(n, parent.w);
	return (ak_lsize){l_resolve(st->max_width, parent.w) + adj.w, l_resolve(st->max_height, parent.h) + adj.h};
}

// l_scroll_gutter is the space a scroll container reserves for
// scrollbars: a vertical scroller takes width.
ak_lsize l_scroll_gutter(const ak_lnode *n) {
	ak_lsize g = {0, 0};
	if (n->style.overflow_y == AK_LOVERFLOW_SCROLL) g.w = n->style.scrollbar_width;
	if (n->style.overflow_x == AK_LOVERFLOW_SCROLL) g.h = n->style.scrollbar_width;
	return g;
}

// l_content_inset is what separates the border box from the content box:
// padding, border, and the gutter of the scrollbars, whose vertical one
// is on the left in a right-to-left box, as in Chrome.
ak_ledges l_content_inset(const ak_lnode *n, ak_ledges pad, ak_ledges bor, ak_lsize gutter) {
	ak_ledges r = {pad.top + bor.top, pad.right + bor.right + gutter.w, pad.bottom + bor.bottom + gutter.h, pad.left + bor.left};
	if (n->rtl) {
		r.right = pad.right + bor.right;
		r.left = pad.left + bor.left + gutter.w;
	}
	return r;
}

bool l_is_scroll_container(const ak_lnode *n) {
	return n->style.overflow_x != AK_LOVERFLOW_VISIBLE || n->style.overflow_y != AK_LOVERFLOW_VISIBLE;
}

static l_output leaf(ak_lnode *n, const l_input *in, ak_lpass *p) {
	const ak_lstyle *st = &n->style;
	ak_ledges pad, bor;
	l_padding_border(n, in->parent.w, &pad, &bor);
	ak_lsize pb = {pad.left + pad.right + bor.left + bor.right, pad.top + pad.bottom + bor.top + bor.bottom};
	ak_lsize gutter = l_scroll_gutter(n);
	ak_lsize inset = {pb.w + gutter.w, pb.h + gutter.h};

	ak_lsize size = in->known, min = {LNAN, LNAN}, max = {LNAN, LNAN};
	float ratio = 0;
	if (in->sizing == SIZE_INHERENT) {
		ak_lsize ss = l_style_size(n, in->parent);
		size = (ak_lsize){l_orf(size.w, ss.w), l_orf(size.h, ss.h)};
		min = l_style_min(n, in->parent);
		max = l_style_max(n, in->parent);
		ratio = st->aspect_ratio;
	}
	bool blocks_collapse = st->display != AK_LDISPLAY_BLOCK || l_is_scroll_container(n) || st->position == AK_LABSOLUTE ||
		pad.top > 0 || pad.bottom > 0 || bor.top > 0 || bor.bottom > 0 ||
		(l_isdef(size.h) && size.h > 0) || (l_isdef(min.h) && min.h > 0);

	if (in->mode == MODE_SIZE && blocks_collapse && l_isdef(size.w) && l_isdef(size.h)) {
		ak_lsize s = {l_clampn(size.w, min.w, max.w), l_clampn(size.h, min.h, max.h)};
		return (l_output){.size = {l_maxf(s.w, pb.w), l_maxf(s.h, pb.h)}, .baseline = LNAN};
	}

	ak_lsize avail = in->avail;
	for (int a = 0; a < 2; a++) {
		float v = l_get(avail, a);
		float k = l_get(in->known, a);
		if (l_isdef(k)) v = k;
		float s = l_get(size, a);
		if (l_isdef(s)) v = s;
		if (l_definite(v)) v = l_clampn(v, l_get(min, a), l_get(max, a)) - l_get(inset, a);
		l_set(&avail, a, v);
	}
	ak_lsize measured = {0, 0};
	if (n->measure) {
		ak_lsize known = {in->known.w - inset.w, in->known.h - inset.h};
		if (l_isdef(known.w) && l_isdef(known.h)) {
			measured = known;
		} else {
			p->measures++;
			measured = n->measure(n->measure_ctx, (ak_lconstraints){known.w, known.h, avail.w, avail.h});
			if (l_isdef(known.w)) measured.w = known.w;
			if (l_isdef(known.h)) measured.h = known.h;
		}
	}
	ak_lsize s = {l_orf(in->known.w, size.w), l_orf(in->known.h, size.h)};
	if (l_isnan(s.w)) s.w = measured.w + inset.w;
	if (l_isnan(s.h)) s.h = measured.h + inset.h;
	s.w = l_clampn(s.w, min.w, max.w);
	s.h = l_clampn(s.h, min.h, max.h);
	if (ratio > 0) s.h = l_maxf(s.h, s.w / ratio);
	s.w = l_maxf(s.w, pb.w);
	s.h = l_maxf(s.h, pb.h);
	if (in->mode == MODE_LAYOUT) {
		// Where its parent narrows its frame (l_place): the content its
		// host draws at most, with this layout's padding and border.
		n->box_max = (ak_lsize){n->content_max.w + inset.w, n->content_max.h + inset.h};
	}
	l_output out = {.size = s, .baseline = LNAN};
	if (n->baseline) out.baseline = n->baseline(n->baseline_ctx, s);
	out.collapses_through = !blocks_collapse && s.h == 0 && measured.h == 0;
	return out;
}

// l_place_absolute_aligned lays out an absolutely positioned child against
// its containing block (ax, ay, aw, ah: the padding box or a grid area,
// relative to the parent's border box). An axis whose insets are both
// auto takes its static position: frac (0 start, 0.5 center, 1 end) of
// the way along the content box (cx, cy, cw, ch), from its left and top.
// Over-constrained, the containing block's inline start wins: right, in
// a right-to-left parent.
void l_place_absolute_aligned(ak_lpass *p, ak_lnode *c, float ax, float ay, float aw, float ah,
	float cx, float cy, float cw, float ch, const float frac[2]) {
	const ak_lstyle *st = &c->style;
	bool rtl = c->parent->rtl;
	ak_lsize area = {aw, ah};
	ak_ledges m = l_margins_of(c, aw);
	ak_ledges ins = {l_resolve(st->inset.top, ah), l_resolve(st->inset.right, aw), l_resolve(st->inset.bottom, ah), l_resolve(st->inset.left, aw)};
	ak_lsize size = l_style_size(c, area);
	ak_lsize min = l_style_min(c, area), max = l_style_max(c, area);
	bool auto_m[2][2] = {{ak_llength_is_auto(st->margin.left), ak_llength_is_auto(st->margin.right)},
		{ak_llength_is_auto(st->margin.top), ak_llength_is_auto(st->margin.bottom)}};
	for (int a = 0; a < 2; a++) {
		if (l_isnan(l_get(size, a)) && l_isdef(l_start(ins, a)) && l_isdef(l_end(ins, a))) {
			l_set(&size, a, l_maxf(0, l_get(area, a) - l_start(ins, a) - l_end(ins, a) - l_sum_axis(m, a)));
		}
	}
	size = l_apply_aspect(size, st->aspect_ratio);
	size.w = l_clampn(size.w, min.w, max.w);
	size.h = l_clampn(size.h, min.h, max.h);
	ak_lsize avail;
	for (int a = 0; a < 2; a++) {
		l_set(&avail, a, l_maxf(0, l_get(area, a) - l_or0(l_start(ins, a)) - l_or0(l_end(ins, a)) - l_sum_axis(m, a)));
	}
	ak_lsize final = size;
	if (l_isnan(final.w) || l_isnan(final.h)) {
		ak_lsize measured = l_measure_size(c, (l_input){.known = size, .parent = area, .avail = avail}, p);
		final = (ak_lsize){l_orf(size.w, measured.w), l_orf(size.h, measured.h)};
		final.w = l_clampn(final.w, min.w, max.w);
		final.h = l_clampn(final.h, min.h, max.h);
	}
	ak_ledges cpad, cbor;
	l_padding_border(c, aw, &cpad, &cbor);
	final.w = l_maxf(final.w, cpad.left + cpad.right + cbor.left + cbor.right);
	final.h = l_maxf(final.h, cpad.top + cpad.bottom + cbor.top + cbor.bottom);
	l_compute(c, (l_input){.mode = MODE_LAYOUT, .known = final, .parent = area, .avail = avail}, p);

	float pos[2] = {0, 0};
	float origin[2] = {ax, ay};
	float content[2][2] = {{cx, cw}, {cy, ch}};
	for (int a = 0; a < 2; a++) {
		float s = l_start(ins, a), e = l_end(ins, a);
		float ms = l_start(m, a), me = l_end(m, a);
		float sz = l_get(final, a);
		if (l_isdef(s) && l_isdef(e)) {
			float free = l_get(area, a) - s - e - sz - ms - me;
			if (auto_m[a][0] && auto_m[a][1]) {
				float half = free / 2;
				if (half < 0 && a == 0) {
					half = 0;
					if (rtl) half = free;
				}
				ms += half;
			} else if (auto_m[a][0]) {
				ms += free;
			} else if (a == 0 && rtl && !auto_m[a][1]) {
				ms += free;  // left is the one ignored
			}
		}
		if (l_isdef(s)) pos[a] = origin[a] + s + ms;
		else if (l_isdef(e)) pos[a] = origin[a] + l_get(area, a) - e - me - sz;
		else pos[a] = content[a][0] + frac[a] * (content[a][1] - sz - ms - me) + ms;
	}
	l_place(p, c, pos[0], pos[1], final);
}

// Rounding.

static double int64_round(double v) {
	// Out of int64's range (and NaN) the conversion is undefined in C:
	// such a value is already whole, or meaningless.
	if (!(fabs(v) < 9.2e18)) return v;
	if (v < 0) return -(double)(int64_t)(-v + 0.5);
	return (double)(int64_t)(v + 0.5);
}

static float round_to(float v, float scale) { return (float)int64_round((double)(v * scale)) / scale; }

static void round_node(ak_lnode *n, float ax, float ay, float scale) {
	ax += n->x;
	ay += n->y;
	float x = round_to(n->x, scale), y = round_to(n->y, scale);
	float w = round_to(ax + n->w, scale) - round_to(ax, scale);
	float h = round_to(ay + n->h, scale) - round_to(ay, scale);
	for (uint32_t i = 0; i < n->nchildren; i++) round_node(n->children[i], ax, ay, scale);
	n->x = x;
	n->y = y;
	n->w = w;
	n->h = h;
}

void ak_lround(ak_lnode *n, float scale) { round_node(n, 0, 0, scale); }
