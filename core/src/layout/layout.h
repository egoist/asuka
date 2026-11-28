// layout.h: CSS block, flexbox and grid layout, in points (iOS points,
// Android dp). A port of the Go package layout (DESIGN.md's "layout"):
// the same algorithms, caches and fixtures. Main thread only.
//
// A tree of ak_lnode is laid out by ak_lnode_layout with an ak_lpass,
// whose scratch memory is reused: steady-state layout allocates nothing.
// Each node caches its final layout and eight measurements keyed by
// their inputs; ak_lnode_mark_dirty clears a node's caches and its
// ancestors'. A layout reports the nodes whose frame changed in the
// pass's changed list.
#ifndef AK_LAYOUT_H
#define AK_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

// Lengths.

typedef enum {
	AK_LUNIT_AUTO,  // the zero length
	AK_LUNIT_POINT,
	AK_LUNIT_PERCENT,
} ak_lunit;

// ak_llength is auto, points or a percentage of the containing block.
// Where auto isn't allowed (padding, border, gap) it means 0.
typedef struct {
	float v;
	uint8_t u;  // ak_lunit
} ak_llength;

#define AK_LAUTO ((ak_llength){0, AK_LUNIT_AUTO})
static inline ak_llength ak_lpt(float v) { return (ak_llength){v, AK_LUNIT_POINT}; }
// A percentage of the containing block's size along the same axis (its
// width for margin and padding).
static inline ak_llength ak_lpct(float v) { return (ak_llength){v, AK_LUNIT_PERCENT}; }
static inline bool ak_llength_is_auto(ak_llength l) { return l.u == AK_LUNIT_AUTO; }

// A length per side.
typedef struct {
	ak_llength top, right, bottom, left;
} ak_lrect;

static inline ak_lrect ak_lrect_all(ak_llength v) { return (ak_lrect){v, v, v, v}; }

// A resolved value per side.
typedef struct {
	float top, right, bottom, left;
} ak_ledges;

// A width and a height. In inputs, NaN means unknown.
typedef struct {
	float w, h;
} ak_lsize;

// A node's frame: its border box relative to its parent's, in points.
typedef struct {
	float x, y, w, h;
} ak_lframe;

// Available space on an axis: a definite amount, or one of these.
#define AK_LMAX_CONTENT ((float)INFINITY)
#define AK_LMIN_CONTENT (-(float)INFINITY)

// Enumerations, with CSS's meaning.

typedef enum { AK_LDISPLAY_FLEX, AK_LDISPLAY_BLOCK, AK_LDISPLAY_GRID, AK_LDISPLAY_NONE } ak_ldisplay;

typedef enum {
	AK_LRELATIVE,  // in flow; insets offset it visually
	AK_LABSOLUTE,  // out of flow, placed by insets in its parent's padding box
} ak_lposition;

typedef enum {
	AK_LBORDER_BOX,  // padding and border included (the default here)
	AK_LCONTENT_BOX,
} ak_lbox_sizing;

// Overflow affects layout through automatic minimum sizes and scrollbar
// space: a hidden, clip or scroll node's minimum size is 0 rather than
// its content's.
typedef enum { AK_LOVERFLOW_VISIBLE, AK_LOVERFLOW_CLIP, AK_LOVERFLOW_HIDDEN, AK_LOVERFLOW_SCROLL } ak_loverflow;

// The way a box's inline axis runs. Inherited: AK_LDIR_INHERIT takes the
// parent's, and a layout root without an ancestor that sets one is left
// to right.
typedef enum { AK_LDIR_INHERIT, AK_LDIR_LTR, AK_LDIR_RTL } ak_ldirection;

typedef enum { AK_LROW, AK_LCOLUMN, AK_LROW_REVERSE, AK_LCOLUMN_REVERSE } ak_lflex_direction;

typedef enum { AK_LNOWRAP, AK_LWRAP, AK_LWRAP_REVERSE } ak_lflex_wrap;

// Positions an item within its line or grid area (align-items,
// align-self, justify-items, justify-self). AUTO means "normal" for
// containers and "the container's value" for self-alignment. START and
// END are the container's start and end; SELF_START and SELF_END the
// item's own, which differ on the inline axis when the item's direction
// isn't the container's.
typedef enum {
	AK_LALIGN_AUTO,
	AK_LALIGN_START,
	AK_LALIGN_END,
	AK_LALIGN_FLEX_START,
	AK_LALIGN_FLEX_END,
	AK_LALIGN_CENTER,
	AK_LALIGN_BASELINE,
	AK_LALIGN_STRETCH,
	AK_LALIGN_SELF_START,
	AK_LALIGN_SELF_END,
} ak_lalign;

// Distributes space between lines, tracks or items (align-content,
// justify-content). NORMAL behaves as stretch for align-content and as
// start for justify-content.
typedef enum {
	AK_LCONTENT_NORMAL,
	AK_LCONTENT_START,
	AK_LCONTENT_END,
	AK_LCONTENT_FLEX_START,
	AK_LCONTENT_FLEX_END,
	AK_LCONTENT_CENTER,
	AK_LCONTENT_STRETCH,
	AK_LCONTENT_SPACE_BETWEEN,
	AK_LCONTENT_SPACE_AROUND,
	AK_LCONTENT_SPACE_EVENLY,
} ak_lcontent;

typedef enum { AK_LFLOW_ROW, AK_LFLOW_COLUMN, AK_LFLOW_ROW_DENSE, AK_LFLOW_COLUMN_DENSE } ak_lgrid_auto_flow;

// Grid tracks.

typedef enum {
	AK_LBREADTH_AUTO,
	AK_LBREADTH_POINT,
	AK_LBREADTH_PERCENT,
	AK_LBREADTH_FR,
	AK_LBREADTH_MIN_CONTENT,
	AK_LBREADTH_MAX_CONTENT,
	AK_LBREADTH_FIT_CONTENT,  // fit-content(v points)
	AK_LBREADTH_FIT_CONTENT_PERCENT,
} ak_lbreadth_kind;

// One side of a grid track's sizing function.
typedef struct {
	uint8_t kind;  // ak_lbreadth_kind
	float v;       // points, percent or fr
} ak_lbreadth;

// A grid track's sizing function: minmax(min, max).
typedef struct {
	ak_lbreadth min, max;
} ak_ltrack_size;

static inline ak_lbreadth ak_lbpx(float v) { return (ak_lbreadth){AK_LBREADTH_POINT, v}; }
static inline ak_lbreadth ak_lbpct(float v) { return (ak_lbreadth){AK_LBREADTH_PERCENT, v}; }
static inline ak_lbreadth ak_lbfr(float v) { return (ak_lbreadth){AK_LBREADTH_FR, v}; }
static inline ak_lbreadth ak_lbauto(void) { return (ak_lbreadth){AK_LBREADTH_AUTO, 0}; }
static inline ak_lbreadth ak_lbmin_content(void) { return (ak_lbreadth){AK_LBREADTH_MIN_CONTENT, 0}; }
static inline ak_lbreadth ak_lbmax_content(void) { return (ak_lbreadth){AK_LBREADTH_MAX_CONTENT, 0}; }

static inline ak_ltrack_size ak_lpx(float v) { return (ak_ltrack_size){ak_lbpx(v), ak_lbpx(v)}; }
static inline ak_ltrack_size ak_lpercent(float v) { return (ak_ltrack_size){ak_lbpct(v), ak_lbpct(v)}; }
static inline ak_ltrack_size ak_lfr(float v) { return (ak_ltrack_size){ak_lbauto(), ak_lbfr(v)}; }
static inline ak_ltrack_size ak_lauto_track(void) { return (ak_ltrack_size){ak_lbauto(), ak_lbauto()}; }
static inline ak_ltrack_size ak_lmin_content(void) { return (ak_ltrack_size){ak_lbmin_content(), ak_lbmin_content()}; }
static inline ak_ltrack_size ak_lmax_content(void) { return (ak_ltrack_size){ak_lbmax_content(), ak_lbmax_content()}; }
static inline ak_ltrack_size ak_lfit_content(float v) { return (ak_ltrack_size){ak_lbauto(), {AK_LBREADTH_FIT_CONTENT, v}}; }
static inline ak_ltrack_size ak_lfit_content_pct(float v) { return (ak_ltrack_size){ak_lbauto(), {AK_LBREADTH_FIT_CONTENT_PERCENT, v}}; }
static inline ak_ltrack_size ak_lminmax(ak_lbreadth min, ak_lbreadth max) { return (ak_ltrack_size){min, max}; }

// Repeat counts for ak_lgrid_track.
#define AK_LREPEAT_AUTO_FILL (-1)
#define AK_LREPEAT_AUTO_FIT (-2)

// An entry of grid-template-rows/columns: one track, or repeat(count,
// sizes...) when count != 0 (count may be AK_LREPEAT_AUTO_FILL or
// AK_LREPEAT_AUTO_FIT). sizes belongs to the caller (see ak_lstyle).
typedef struct {
	int32_t count;
	const ak_ltrack_size *sizes;
	uint32_t nsizes;
} ak_lgrid_track;

// A single track; s belongs to the caller.
static inline ak_lgrid_track ak_ltrack(const ak_ltrack_size *s) { return (ak_lgrid_track){0, s, 1}; }
// repeat(count, sizes[0..n)); sizes belongs to the caller.
static inline ak_lgrid_track ak_lrepeat(int32_t count, const ak_ltrack_size *sizes, uint32_t n) { return (ak_lgrid_track){count, sizes, n}; }

typedef enum { AK_LLINE_AUTO, AK_LLINE_INDEX, AK_LLINE_SPAN } ak_lgrid_line_kind;

// One end of a grid item's placement.
typedef struct {
	uint8_t kind;  // ak_lgrid_line_kind
	int16_t n;     // the line (1-based, negative from the end) or the span
} ak_lgrid_line;

// Grid line n (1 is the first; -1 the last explicit one).
static inline ak_lgrid_line ak_lline(int n) { return (ak_lgrid_line){AK_LLINE_INDEX, (int16_t)n}; }
// Spans n tracks.
static inline ak_lgrid_line ak_lspan(int n) { return (ak_lgrid_line){AK_LLINE_SPAN, (int16_t)n}; }

// An item's grid-row or grid-column.
typedef struct {
	ak_lgrid_line start, end;
} ak_lplacement;

// ak_lstyle is everything layout reads from a node, with CSS's meaning.
// Start from ak_lstyle_default: the zero style differs from CSS's
// initial values in flex_shrink and margins.
//
// Ownership: the grid arrays (grid_template_*, each track's sizes and
// grid_auto_*) belong to the caller, who keeps them alive and unchanged
// while the style is a node's (until the node gets another style or is
// freed); layout never copies or frees them, so setting a style never
// allocates.
typedef struct {
	uint8_t display;     // ak_ldisplay
	uint8_t position;    // ak_lposition
	uint8_t box_sizing;  // ak_lbox_sizing
	uint8_t overflow_x;  // ak_loverflow
	uint8_t overflow_y;
	// The space a scroll container reserves for its scrollbars (0 for
	// overlay scrollbars, as on mobile).
	float scrollbar_width;

	ak_llength width, height;
	ak_llength min_width, min_height;  // auto: the automatic minimum size
	ak_llength max_width, max_height;  // auto: none
	float aspect_ratio;                // width / height, when > 0

	ak_lrect margin;  // auto allowed
	ak_lrect padding;
	ak_lrect border;
	ak_lrect inset;  // auto: not set

	ak_llength column_gap, row_gap;

	uint8_t align_items;  // ak_lalign
	uint8_t align_self;
	uint8_t justify_items;
	uint8_t justify_self;
	uint8_t align_content;  // ak_lcontent
	uint8_t justify_content;

	uint8_t direction;       // ak_ldirection
	uint8_t flex_direction;  // ak_lflex_direction
	uint8_t flex_wrap;       // ak_lflex_wrap
	float flex_grow;
	float flex_shrink;
	ak_llength flex_basis;

	const ak_lgrid_track *grid_template_columns;
	uint32_t ngrid_template_columns;
	const ak_lgrid_track *grid_template_rows;
	uint32_t ngrid_template_rows;
	const ak_ltrack_size *grid_auto_columns;
	uint32_t ngrid_auto_columns;
	const ak_ltrack_size *grid_auto_rows;
	uint32_t ngrid_auto_rows;
	uint8_t grid_auto_flow;  // ak_lgrid_auto_flow
	ak_lplacement grid_column;
	ak_lplacement grid_row;
} ak_lstyle;

// CSS's initial values (flex-shrink: 1, margins 0) with border-box
// sizing and display: flex.
ak_lstyle ak_lstyle_default(void);

// Measured leaves.

// What a measured leaf (text, a native control) is asked: its content
// box's known size on each axis, NaN when free, and the space available
// on each axis: a definite amount, AK_LMAX_CONTENT (lay out without
// limit) or AK_LMIN_CONTENT (as narrow as it can be, e.g. wrapping at
// every word).
typedef struct {
	float width, height;
	float avail_w, avail_h;
} ak_lconstraints;

// Returns the size of a leaf's content box.
typedef ak_lsize (*ak_lmeasure_fn)(void *ctx, ak_lconstraints c);
// Returns a leaf's first baseline, from the top of its border box, given
// its final size.
typedef float (*ak_lbaseline_fn)(void *ctx, ak_lsize s);

// Nodes.

typedef struct ak_lnode ak_lnode;

// ak_lpass carries one layout run's state and scratch memory. Zero it to
// start (`ak_lpass p = {0};`), reuse it so layout allocates nothing in
// the steady state, and free it with ak_lpass_free.
typedef struct ak_lpass {
	// The nodes whose frame changed. Layout appends; the caller resets
	// nchanged (or calls ak_lpass_reset).
	ak_lnode **changed;
	uint32_t nchanged, cap_changed;
	// Measure callbacks made.
	uint64_t measures;
	// Private: scratch memory, chunks that never move while in use.
	struct ak_lchunk *chunks_;
	uint32_t nchunks_, cur_;
	// The computation in progress and its mode (0: none), which the
	// caches note of the answers they keep.
	uint64_t comp_;
	uint8_t mode_;
} ak_lpass;

// The nodes computed (not answered by their caches) by every layout in
// the process: the work a bench counts (main thread only).
extern uint64_t ak_lnode_computed;

void ak_lpass_reset(ak_lpass *p);  // nchanged and measures to 0; keeps the scratch memory
void ak_lpass_free(ak_lpass *p);

// ak_lnode_new makes a node with ak_lstyle_default, dirty.
ak_lnode *ak_lnode_new(void);
// ak_lnode_free removes n from its parent, orphans its children and
// frees it.
void ak_lnode_free(ak_lnode *n);
// ak_lnode_free_tree frees n and everything below it.
void ak_lnode_free_tree(ak_lnode *n);

// The node's style, to read or write in place; after writing, call
// ak_lnode_mark_dirty (or use ak_lnode_set_style).
ak_lstyle *ak_lnode_style(ak_lnode *n);
// ak_lnode_set_style replaces the style and marks the node dirty, with
// everything below it if its direction changed.
void ak_lnode_set_style(ak_lnode *n, const ak_lstyle *s);

// The tree's owner's (ui keeps its element here).
void *ak_lnode_context(const ak_lnode *n);
void ak_lnode_set_context(ak_lnode *n, void *ctx);

// ak_lnode_set_measure makes n a leaf measured by fn (NULL: an empty
// box), called with ctx.
void ak_lnode_set_measure(ak_lnode *n, ak_lmeasure_fn fn, void *ctx);
// ak_lnode_set_baseline sets how a measured leaf reports its baseline.
void ak_lnode_set_baseline(ak_lnode *n, ak_lbaseline_fn fn, void *ctx);
// ak_lnode_set_content_max says how large a measured leaf's content is
// ever drawn, per axis (INFINITY: no limit): what its host reported as it
// measured it (a watch's button is never wider than the screen less its
// margins). Called from its measure function, it holds until its content
// or style changes (ak_lnode_mark_content_dirty, ak_lnode_mark_dirty).
// It doesn't size the leaf: layout gives it the box CSS does (a stretch,
// a flex-grow, a width of its own), and its frame is that box narrowed
// to the limit (and its padding and border), centered in it, as the
// platform draws a control it can't make larger, so the frame is what's
// drawn. Its siblings are where they were.
void ak_lnode_set_content_max(ak_lnode *n, float w, float h);

// ak_lnode_mark_dirty drops the cached layouts of n and its ancestors:
// call it when n's style changed (or anything else about it).
void ak_lnode_mark_dirty(ak_lnode *n);
// ak_lnode_mark_content_dirty says n's measured content or its children
// changed, not its style. Its ancestors keep their layouts unless what n
// answers them changes: the next layout first asks n again everything
// its cache says it was asked, and goes up a level only where an answer
// differs (a relayout boundary, as Flutter's), so a label that changes
// in a long list re-lays out its row, not the list. The children
// functions below and ak_lnode_set_measure call it.
void ak_lnode_mark_content_dirty(ak_lnode *n);
// ak_lnode_mark_subtree_dirty clears the caches of n, its ancestors and
// everything below it: what the node's direction changes, which no
// cached input says.
void ak_lnode_mark_subtree_dirty(ak_lnode *n);
// Whether n must be laid out again (it or a node below it changed).
bool ak_lnode_dirty(const ak_lnode *n);

ak_lnode *ak_lnode_parent(const ak_lnode *n);
// n's children (*count of them); valid until the children change.
ak_lnode *const *ak_lnode_children(const ak_lnode *n, uint32_t *count);
uint32_t ak_lnode_child_count(const ak_lnode *n);
ak_lnode *ak_lnode_child(const ak_lnode *n, uint32_t i);
// ak_lnode_set_children replaces n's children with a copy of
// children[0..count), taking each from its previous parent.
void ak_lnode_set_children(ak_lnode *n, ak_lnode *const *children, uint32_t count);
// ak_lnode_append_child adds c as n's last child.
void ak_lnode_append_child(ak_lnode *n, ak_lnode *c);
// ak_lnode_insert_child puts c at index (clamped) among n's children,
// counted without c.
void ak_lnode_insert_child(ak_lnode *n, ak_lnode *c, uint32_t index);
// ak_lnode_remove_child removes c from n's children.
void ak_lnode_remove_child(ak_lnode *n, ak_lnode *c);
// ak_lnode_detach removes each of nodes[0..count) from its parent, each
// parent's children compacted once: what removing many children one by
// one (each a search and a move) costs n² for.
void ak_lnode_detach(ak_lnode *const *nodes, uint32_t count);

// ak_lnode_force_report makes the next layout report n's frame as
// changed.
void ak_lnode_force_report(ak_lnode *n);

// The last layout: the border box relative to the parent's border box,
// in points, unrounded (unless ak_lround rewrote it).
ak_lframe ak_lnode_frame(const ak_lnode *n);

// ak_lnode_content_insets is the room n's padding and border take inside
// its box, per side (percentages of its parent's content width, as its
// last layout had it): where its content sits.
ak_ledges ak_lnode_content_insets(const ak_lnode *n);

// ak_lnode_layout lays out the tree under n within the available space
// (a definite size, AK_LMAX_CONTENT or AK_LMIN_CONTENT per axis). n's
// own size comes from its style resolved against avail; its x and y are
// its margins. Returns n's size.
ak_lsize ak_lnode_layout(ak_lnode *n, ak_lsize avail, ak_lpass *p);
// ak_lnode_measure is the size ak_lnode_layout would give n within
// avail, placing nothing.
ak_lsize ak_lnode_measure(ak_lnode *n, ak_lsize avail, ak_lpass *p);

// ak_lround snaps a laid-out tree to a pixel grid of scale pixels per
// point, the way browsers do: edges are rounded in absolute coordinates
// so neighbors never gap or overlap. It rewrites the frames.
void ak_lround(ak_lnode *n, float scale);

#endif
