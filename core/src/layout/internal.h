// internal.h: what the layout's files share. Not for use outside
// core/src/layout.
#ifndef AK_LAYOUT_INTERNAL_H
#define AK_LAYOUT_INTERNAL_H

#include "base/base.h"
#include "layout/layout.h"

// Floats.

#define LNAN AK_NAN
#define LINF AK_INF

static inline bool l_isnan(float f) { return f != f; }
static inline bool l_isdef(float f) { return f == f; }
static inline bool l_isinf(float f) { return isinf(f); }
static inline bool l_definite(float a) { return a == a && !isinf(a); }
static inline float l_or0(float f) { return f != f ? 0 : f; }
static inline float l_orf(float f, float d) { return f != f ? d : f; }
static inline float l_maxf(float a, float b) { return a > b ? a : b; }
static inline float l_minf(float a, float b) { return a < b ? a : b; }
static inline float l_absf(float f) { return f < 0 ? -f : f; }
static inline int l_imin(int a, int b) { return a < b ? a : b; }
static inline int l_imax(int a, int b) { return a > b ? a : b; }

// maybe_min and maybe_max ignore a NaN bound.
static inline float l_maybe_min(float v, float bound) { return (bound == bound && bound < v) ? bound : v; }
static inline float l_maybe_max(float v, float bound) { return (bound == bound && bound > v) ? bound : v; }

// clampn clamps v between lo and hi, ignoring NaN bounds; min beats max,
// as in CSS.
static inline float l_clampn(float v, float lo, float hi) {
	if (v != v) return v;
	v = l_maybe_min(v, hi);
	return l_maybe_max(v, lo);
}

static inline float l_resolve(ak_llength l, float basis) {
	switch (l.u) {
	case AK_LUNIT_POINT: return l.v;
	case AK_LUNIT_PERCENT: return l.v * basis / 100;  // a NaN basis gives NaN
	}
	return LNAN;
}

static inline float l_resolve0(ak_llength l, float basis) { return l_or0(l_resolve(l, basis)); }

// Sizes and sides by axis: 0 is x, 1 is y.

static inline ak_lsize l_size(float w, float h) { return (ak_lsize){w, h}; }
static inline float l_get(ak_lsize s, int axis) { return axis == 0 ? s.w : s.h; }
static inline void l_set(ak_lsize *s, int axis, float v) {
	if (axis == 0) s->w = v;
	else s->h = v;
}

static inline float l_start(ak_ledges r, int axis) { return axis == 0 ? r.left : r.top; }
static inline float l_end(ak_ledges r, int axis) { return axis == 0 ? r.right : r.bottom; }
static inline float l_sum_axis(ak_ledges r, int axis) { return l_start(r, axis) + l_end(r, axis); }
static inline ak_lsize l_sum_both(ak_ledges r) { return (ak_lsize){r.left + r.right, r.top + r.bottom}; }

typedef struct {
	bool top, right, bottom, left;
} l_bedges;

static inline bool l_bstart(l_bedges r, int axis) { return axis == 0 ? r.left : r.top; }
static inline bool l_bend(l_bedges r, int axis) { return axis == 0 ? r.right : r.bottom; }

static inline l_bedges l_auto_margins(const ak_lstyle *s) {
	return (l_bedges){ak_llength_is_auto(s->margin.top), ak_llength_is_auto(s->margin.right),
		ak_llength_is_auto(s->margin.bottom), ak_llength_is_auto(s->margin.left)};
}

static inline bool l_flex_is_row(uint8_t d) { return d == AK_LROW || d == AK_LROW_REVERSE; }
static inline bool l_flex_is_reverse(uint8_t d) { return d == AK_LROW_REVERSE || d == AK_LCOLUMN_REVERSE; }
static inline bool l_flow_is_column(uint8_t f) { return f == AK_LFLOW_COLUMN || f == AK_LFLOW_COLUMN_DENSE; }
static inline bool l_flow_is_dense(uint8_t f) { return f == AK_LFLOW_ROW_DENSE || f == AK_LFLOW_COLUMN_DENSE; }

// Runs.

enum { MODE_SIZE, MODE_LAYOUT };        // only the size is needed; size and place the subtree
enum { SIZE_INHERENT, SIZE_CONTENT };  // apply the node's own size styles; ignore them

typedef struct {
	uint8_t mode, sizing;
	// Whether the node's top/bottom margins may collapse with its block
	// children (it sits in a block formatting context).
	bool collapse[2];
	ak_lsize known;   // border-box size the parent already decided; NaN: free
	ak_lsize parent;  // the containing block, for percentages; NaN: indefinite
	ak_lsize avail;   // definite, AK_LMAX_CONTENT or AK_LMIN_CONTENT
} l_input;

// A set of adjoining vertical margins.
typedef struct {
	float pos, neg;
} l_margins;

static inline l_margins l_margins_add(l_margins m, float v) {
	if (v >= 0) m.pos = l_maxf(m.pos, v);
	else m.neg = l_minf(m.neg, v);
	return m;
}

static inline l_margins l_margins_join(l_margins m, l_margins o) {
	return (l_margins){l_maxf(m.pos, o.pos), l_minf(m.neg, o.neg)};
}

static inline float l_margins_value(l_margins m) { return m.pos + m.neg; }

typedef struct {
	ak_lsize size;
	float baseline;  // first baseline from the top; NaN: none
	// Block layout: margins that collapse through this box's edges.
	l_margins top, bottom;
	bool collapses_through;
} l_output;

#define L_MEASURE_SLOTS 8

typedef struct {
	uint8_t mode, sizing;
	bool coll[2];
	ak_lsize known;
	ak_lsize parent;  // not a key: what revalidating the entry runs it with
	ak_lsize avail;
	l_output out;
	// The parent's cache's generation when the parent last got this
	// answer: one from before the parent's cache was last cleared is no
	// longer what any of its answers rest on.
	uint32_t pgen;
	// The order it was stored in since the cache was cleared: matches are
	// loose (a known size matches a free one that came out that size; the
	// percentage basis isn't compared), so what answers depends on what
	// was stored first, which revalidating replays.
	uint32_t seq;
} l_cache_entry;

typedef struct {
	l_cache_entry final;
	bool has_final;
	// An answer given since the last clear is gone (a slot reused, a
	// final the parent still depends on replaced): what the parent
	// computed from it can't be checked again.
	bool lossy;
	// The final answered a size query: the parent's measurements depend
	// on it.
	bool final_shared;
	// The parent's computation that asked for the final (ak_lpass's).
	uint64_t final_by;
	uint32_t gen;  // cleared this many times
	uint32_t seq;  // the next entry's
	l_cache_entry slots[L_MEASURE_SLOTS];
	uint8_t n, next;
} l_cache;

struct ak_lnode {
	ak_lstyle style;
	void *context;

	ak_lmeasure_fn measure;
	void *measure_ctx;
	ak_lbaseline_fn baseline;
	void *baseline_ctx;
	ak_lnode *parent;
	ak_lnode **children;
	uint32_t nchildren, cap_children;

	// The last layout: the border box relative to the parent's border
	// box, in points, unrounded.
	float x, y, w, h;

	bool dirty;
	// Relayout boundaries (ak_lnode_mark_content_dirty): stale, the node's
	// content changed and its cached answers are to be recomputed before
	// the next layout, its ancestors' kept unless those answers change;
	// revalidate, a stale node is at or below it.
	bool stale, revalidate;
	bool detaching;  // ak_lnode_detach's mark
	bool rtl;  // the resolved direction, the one cache was computed under
	l_cache cache;
	// The content box its host draws it at most (ak_lnode_set_content_max;
	// INFINITY: no limit), and that as a border box, as its last final
	// layout had its padding and border: where l_place narrows its frame.
	ak_lsize content_max, box_max;
	float reported[4];
	float static_y;  // block layout's static position for absolute children
};

// Scratch memory: a stack of chunks that never move, so what one call
// takes stays put while the calls it makes take more.

struct ak_lchunk {
	uint8_t *data;
	size_t cap, used;
};

typedef struct {
	uint32_t chunk;
	size_t used;
} l_mark;

l_mark l_scratch_mark(ak_lpass *p);
void l_scratch_release(ak_lpass *p, l_mark m);
// l_scratch takes n bytes, uninitialized, aligned to 16.
void *l_scratch(ak_lpass *p, size_t n);
#define L_SCRATCH(p, T, n) ((T *)l_scratch((p), sizeof(T) * (size_t)(n)))

// node.c

l_output l_compute(ak_lnode *n, l_input in, ak_lpass *p);
ak_lsize l_measure_size(ak_lnode *n, l_input in, ak_lpass *p);
void l_hide(ak_lnode *n, ak_lpass *p);
void l_place(ak_lpass *p, ak_lnode *n, float x, float y, ak_lsize s);
bool l_rtl_under(const ak_lnode *n, bool parent);

void l_padding_border(const ak_lnode *n, float parent_w, ak_ledges *pad, ak_ledges *border);
ak_ledges l_margins_of(const ak_lnode *n, float parent_w);
ak_lsize l_box_adjust(const ak_lnode *n, float parent_w);
ak_lsize l_apply_aspect(ak_lsize s, float ratio);
ak_lsize l_style_size(const ak_lnode *n, ak_lsize parent);
ak_lsize l_style_min(const ak_lnode *n, ak_lsize parent);
ak_lsize l_style_max(const ak_lnode *n, ak_lsize parent);
ak_lsize l_scroll_gutter(const ak_lnode *n);
ak_ledges l_content_inset(const ak_lnode *n, ak_ledges pad, ak_ledges bor, ak_lsize gutter);
bool l_is_scroll_container(const ak_lnode *n);
void l_place_absolute_aligned(ak_lpass *p, ak_lnode *c, float ax, float ay, float aw, float ah,
	float cx, float cy, float cw, float ch, const float frac[2]);

// flex.c

l_output l_flex(ak_lnode *n, const l_input *in, ak_lpass *p);
void l_relative_offset(const ak_lnode *ch, float *x, float *y, ak_lsize parent);
uint8_t l_resolve_self_align(uint8_t al, bool inline_axis, bool container_rtl, bool item_rtl);
void l_distribute(uint8_t c, float free, int n, float *lead, float *between);

// block.c

l_output l_block(ak_lnode *n, const l_input *in, ak_lpass *p);

// grid.c

l_output l_grid(ak_lnode *n, const l_input *in, ak_lpass *p);

#endif
