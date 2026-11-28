// view.c: AsukaView, the widget of every node: a GtkWidget subclass the
// host registers as it starts, its vfuncs set at GTK 4's stable class
// offsets, its instance one pointer bigger than a GtkWidget's (its node).
//
// size_allocate places its children at the frames the core gave them
// (whole sizes, the fractional origin in the allocation's transform, with
// Style.Transform about the center); snapshot draws the node's shadow,
// background or gradient, its own content (an image, a symbol, a shape,
// a canvas), its children (clipped to its rounded shape when it clips)
// and its border over them, as a layer does; measure asks for nothing
// (the core lays out), but a scroll's content asks for its frame, which
// the viewport scrolls; focus keeps the focus in a focus trap (focus.c);
// contains takes no presses for an Overlay that isn't modal (its children
// do: overlay.c).
#include "host.h"

#include <math.h>
#include <stdlib.h>

GType lx_view_type, lx_window_type;

// GTK 4's class layout on 64-bit Linux (ABI-stable for the 4.x series).
#if UINTPTR_MAX == 0xffffffffffffffffu
enum {
	OFF_OBJECT_DISPOSE = 40,
	OFF_WIDGET_SIZE_ALLOCATE = 200,
	OFF_WIDGET_MEASURE = 232,
	OFF_WIDGET_FOCUS = 256,
	OFF_WIDGET_SNAPSHOT = 320,
	OFF_WIDGET_CONTAINS = 328,
};
#else
enum {
	OFF_OBJECT_DISPOSE = 20,
	OFF_WIDGET_SIZE_ALLOCATE = 100,
	OFF_WIDGET_MEASURE = 116,
	OFF_WIDGET_FOCUS = 128,
	OFF_WIDGET_SNAPSHOT = 160,
	OFF_WIDGET_CONTAINS = 164,
};
#endif

static size_t node_offset;  // where an AsukaView keeps its node
static void (*parent_dispose)(GObject *o);
static gboolean (*parent_focus)(GtkWidget *w, GtkDirectionType d);
static gboolean (*parent_contains)(GtkWidget *w, double x, double y);

#define SET_VFUNC(class, off, fn) (*(void **)((char *)(class) + (off)) = (void *)(fn))
#define GET_VFUNC(class, off) (*(void **)((char *)(class) + (off)))

node *lx_view_node(GtkWidget *w) {
	if (!w || !lx_is_view(w)) return NULL;
	return *(node **)((char *)w + node_offset);
}

void lx_view_set_node(GtkWidget *w, node *n) { *(node **)((char *)w + node_offset) = n; }

bool lx_is_view(gpointer w) { return w && g_type_check_instance_is_a((GTypeInstance *)w, lx_view_type); }

void lx_snapshot_view(GtkWidget *w, GtkSnapshot *snap);
static void snapshot_highlight(node *n, GtkSnapshot *snap, graphene_rect_t bounds);
static void snapshot_perf(GtkWidget *window, GtkSnapshot *snap);
void lx_allocate_view(GtkWidget *w, int width, int height);

static void view_snapshot(GtkWidget *w, GtkSnapshot *snap) { lx_snapshot_view(w, snap); }
static void view_allocate(GtkWidget *w, int width, int height, int baseline) {
	(void)baseline;
	lx_allocate_view(w, width, height);
}

// measure: a view asks for nothing (the core lays out), but a scroll's
// content, its frame (its minimum too: a viewport scrolls its child's
// minimum size, GtkScrollable's default policy).
static void view_measure(GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat, int *min_b, int *nat_b) {
	(void)for_size;
	node *n = lx_view_node(w);
	int v = 0;
	if (n && n->parent && n->parent->scroll) {
		float s = o == GTK_ORIENTATION_HORIZONTAL ? n->frame[2] : n->frame[3];
		v = (int)ceilf(s);
	}
	*min = *nat = v;
	if (min_b) *min_b = -1;
	if (nat_b) *nat_b = -1;
}

// A container widget unparents its children as it's disposed (GTK 4's
// rule), then disposes as a widget.
static void view_dispose(GObject *o) {
	GtkWidget *c;
	while ((c = gtk_widget_get_first_child((GtkWidget *)o))) gtk_widget_unparent(c);
	parent_dispose(o);
}

// focus: GTK's, but a focus trap keeps it in (lx_trap_focus).
static gboolean view_focus(GtkWidget *w, GtkDirectionType d) {
	if (parent_focus(w, d)) return 1;
	return lx_trap_focus(w, d);
}

// contains: an Overlay that isn't modal takes no presses itself (GTK
// picks its children first, then what's under it).
static gboolean view_contains(GtkWidget *w, double x, double y) {
	node *n = lx_view_node(w);
	if (n && n->kind == AK_KIND_OVERLAY && !lx_overlay_modal(n)) return 0;
	return parent_contains(w, x, y);
}

static void class_init(gpointer class, gpointer data) {
	(void)data;
	gpointer parent = g_type_class_peek_parent(class);
	parent_dispose = GET_VFUNC(parent, OFF_OBJECT_DISPOSE);
	parent_focus = GET_VFUNC(parent, OFF_WIDGET_FOCUS);
	parent_contains = GET_VFUNC(parent, OFF_WIDGET_CONTAINS);
	SET_VFUNC(class, OFF_WIDGET_FOCUS, view_focus);
	SET_VFUNC(class, OFF_WIDGET_CONTAINS, view_contains);
	SET_VFUNC(class, OFF_OBJECT_DISPOSE, view_dispose);
	SET_VFUNC(class, OFF_WIDGET_SNAPSHOT, view_snapshot);
	SET_VFUNC(class, OFF_WIDGET_SIZE_ALLOCATE, view_allocate);
	SET_VFUNC(class, OFF_WIDGET_MEASURE, view_measure);
	gtk_widget_class_set_css_name(class, "asuka-view");
}

// AsukaWindow, the main window: an AdwApplicationWindow that draws asuka
// dev's performance overlay after everything in it, the dialogs it hosts
// (sheets, alerts) and their dimming too.
static void (*parent_window_snapshot)(GtkWidget *w, GtkSnapshot *snap);

static void window_snapshot(GtkWidget *w, GtkSnapshot *snap) {
	parent_window_snapshot(w, snap);
	snapshot_perf(w, snap);
}

static void window_class_init(gpointer class, gpointer data) {
	(void)data;
	parent_window_snapshot = GET_VFUNC(g_type_class_peek_parent(class), OFF_WIDGET_SNAPSHOT);
	SET_VFUNC(class, OFF_WIDGET_SNAPSHOT, window_snapshot);
}

void lx_register_classes(void) {
	if (lx_view_type) return;
	GType parent = gtk_widget_get_type();
	GTypeQuery q;
	g_type_query(parent, &q);
	node_offset = (q.instance_size + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *);
	lx_view_type = g_type_register_static_simple(parent, "AsukaView", q.class_size, class_init,
		(guint)(node_offset + sizeof(void *)), NULL, (GTypeFlags)0);
	parent = adw_application_window_get_type();
	g_type_query(parent, &q);
	lx_window_type = g_type_register_static_simple(parent, "AsukaWindow", q.class_size, window_class_init, q.instance_size, NULL,
		(GTypeFlags)0);
}

// lx_view_new makes an AsukaView the host holds a reference to.
GtkWidget *lx_view_new(void) {
	GtkWidget *w = g_object_new(lx_view_type, NULL);
	g_object_ref_sink(w);
	return w;
}

bool lx_is_layout_root(const node *n) {
	switch (n->kind) {
	case AK_KIND_SCREEN:
	case AK_KIND_SHEET:
	case AK_KIND_WINDOW:
	case AK_KIND_POPOVER:
	case AK_KIND_OVERLAY:
		return true;
	}
	return false;
}

// lx_allocate gives w a size and its place (fractional points through the
// transform, as GTK's sizes are whole), with t about its center.
void lx_allocate(GtkWidget *w, int width, int height, float x, float y, const transform_t *t) {
	graphene_point_t p = {x, y};
	GskTransform *tr = gsk_transform_translate(NULL, &p);
	if (t) {
		float cx = (float)width / 2, cy = (float)height / 2;
		graphene_point_t c = {cx + t->tx, cy + t->ty};
		tr = gsk_transform_translate(tr, &c);
		if (t->rotation != 0) tr = gsk_transform_rotate(tr, t->rotation);
		if (t->sx != 1 || t->sy != 1) tr = gsk_transform_scale(tr, t->sx, t->sy);
		graphene_point_t back = {-cx, -cy};
		tr = gsk_transform_translate(tr, &back);
	}
	gtk_widget_allocate(w, width > 0 ? width : 0, height > 0 ? height : 0, -1, tr);  // takes the transform
}

void lx_place_inline(node *p, node *c, float f[4]);

// allocate_child places a node's view at its frame, with its transform.
static void allocate_child(node *c) {
	float f[4];
	memcpy(f, c->frame, sizeof f);
	const float *a = lx_frame_now(c);
	if (a) memcpy(f, a, sizeof f);
	if (c->parent && c->parent->kind == AK_KIND_RICH_TEXT) lx_place_inline(c->parent, c, f);  // an inline component: where the text puts it
	lx_allocate(c->widget, (int)ceilf(f[2]), (int)ceilf(f[3]), f[0], f[1], c->transform.set ? &c->transform : NULL);
}

// lx_allocate_view places a view's children: the core's nodes at their
// frames, its control filling it less the inset.
void lx_allocate_view(GtkWidget *w, int width, int height) {
	win *wn = lx_win_by_content(w);
	if (wn) {
		lx_win_allocated(wn, width, height);
		return;
	}
	node *n = lx_view_node(w);
	if (!n) return;
	if (lx_is_layout_root(n)) lx_root_allocated(n, width, height);
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
		// A node's view; or one going out (a transition, a match: no
		// parent any more), at the frame it goes with.
		node *cn = lx_view_node(c);
		node *sheet = cn ? NULL : lx_sheet_panel_of(c);
		if (sheet) {
			// An undimmed sheet's panel over this root, or its scrim (nav.c).
			lx_allocate_sheet_panel(sheet, c, width, height);
			continue;
		}
		if (cn && cn->kind == AK_KIND_OVERLAY && !cn->parent) {
			// An overlay over this root: its whole area (overlay.c).
			lx_allocate(c, width, height, 0, 0, NULL);
			continue;
		}
		if (cn && cn->parent == n && lx_is_layout_root(cn) && !cn->placed) {
			// A root the host sizes, in another view (a screen's own Screen in
			// the stack's): it fills it.
			lx_allocate(c, width, height, 0, 0, NULL);
			continue;
		}
		if (cn && (cn->parent == n || (!cn->parent && cn->placed))) {
			allocate_child(cn);
			continue;
		}
		if (c == n->inner) {
			float l = n->inset[3], t = n->inset[0];
			int iw = width - (int)lroundf(n->inset[1] + l);
			int ih = height - (int)lroundf(n->inset[0] + n->inset[2]);
			lx_allocate(c, iw > 0 ? iw : 0, ih > 0 ? ih : 0, l, t, NULL);
			continue;
		}
		if (AK_IS(c, gtk_popover_get_type)) {
			gtk_popover_present((GtkPopover *)c);  // a native: sized and placed by itself
			continue;
		}
		lx_allocate(c, width, height, 0, 0, NULL);
	}
}

// lx_size is the size a node's view draws at: its frame's, once it has
// one (an allocation is whole points), else its allocation.
void lx_size(node *n, GtkWidget *w, float *width, float *height) {
	if (n && n->placed && !lx_is_layout_root(n)) {
		*width = n->frame[2];
		*height = n->frame[3];
		return;
	}
	*width = (float)gtk_widget_get_width(w);
	*height = (float)gtk_widget_get_height(w);
}

static GskRoundedRect rounded(graphene_rect_t r, float radius) {
	float m = fminf(r.size.width / 2, r.size.height / 2);
	if (radius > m) radius = m;
	if (radius < 0) radius = 0;
	GskRoundedRect rr = {.bounds = r};
	for (int i = 0; i < 4; i++) rr.corner[i] = (graphene_size_t){radius, radius};
	return rr;
}

// lx_corners are a node's corner radii: top left, top right, bottom
// right, bottom left (CornerRadii, else CornerRadius on each).
void lx_corners(node *n, float out[4]) {
	bool per = false;
	for (int i = 0; i < 4; i++) per |= n->corner_radii[i] != 0;
	for (int i = 0; i < 4; i++) out[i] = fmaxf(per ? n->corner_radii[i] : n->corner_radius, 0);
}

bool lx_rounded_node(node *n) {
	float c[4];
	lx_corners(n, c);
	return c[0] > 0 || c[1] > 0 || c[2] > 0 || c[3] > 0;
}

// lx_node_rounded is a node's shape in r: its corners, scaled down
// together where two on a side are more than it, as CSS's are.
GskRoundedRect lx_node_rounded(node *n, graphene_rect_t r) {
	float c[4];
	lx_corners(n, c);
	float w = r.size.width, h = r.size.height, f = 1;
	float sums[4] = {c[0] + c[1], c[1] + c[2], c[2] + c[3], c[3] + c[0]}, sides[4] = {w, h, w, h};
	for (int i = 0; i < 4; i++)
		if (sums[i] > sides[i] && sums[i] > 0) f = fminf(f, sides[i] / sums[i]);
	GskRoundedRect rr = {.bounds = r};
	for (int i = 0; i < 4; i++) rr.corner[i] = (graphene_size_t){c[i] * f, c[i] * f};
	return rr;
}

// border_widths are a node's border's sides: top, right, bottom, left.
static bool border_widths(node *n, float bw[4]) {
	bool per = false;
	for (int i = 0; i < 4; i++) per |= n->border_widths[i] != 0;
	bool any = false;
	for (int i = 0; i < 4; i++) {
		bw[i] = fmaxf(per ? n->border_widths[i] : n->border_width, 0);
		any |= bw[i] > 0;
	}
	return any;
}

// stroke_dashed strokes path w wide in c, as a dashed border's dashes
// (3 widths long, gaps as long) or a dotted one's round dots (as wide as
// it, a width apart).
static void stroke_dashed(GtkSnapshot *snap, GskPath *path, float w, bool dotted, const GdkRGBA *c) {
	GskStroke *s = gsk_stroke_new(w);
	if (dotted) {
		float dash[2] = {0, 2 * w};
		gsk_stroke_set_line_cap(s, GSK_LINE_CAP_ROUND);
		gsk_stroke_set_dash(s, dash, 2);
	} else {
		float dash[2] = {3 * w, 3 * w};
		gsk_stroke_set_dash(s, dash, 2);
	}
	gtk_snapshot_append_stroke(snap, path, s, c);
	gsk_stroke_free(s);
	gsk_path_unref(path);
}

// draw_border draws a node's border inside rr: solid as GSK's border (its
// sides' widths, the inner corners the outer less them), dashed or
// dotted along the middle of its sides, around its corners when every
// side is as wide.
static void draw_border(node *n, GtkSnapshot *snap, const GskRoundedRect *rr, const float bw[4], GdkRGBA c) {
	if (n->border_style == AK_BORDER_STYLE_SOLID || !gsk_path_builder_add_rounded_rect || !gsk_path_builder_move_to) {
		GdkRGBA cs[4] = {c, c, c, c};
		gtk_snapshot_append_border(snap, rr, bw, cs);
		return;
	}
	bool dotted = n->border_style == AK_BORDER_STYLE_DOTTED;
	const graphene_rect_t *b = &rr->bounds;
	if (bw[0] == bw[1] && bw[1] == bw[2] && bw[2] == bw[3]) {
		float w = bw[0], h = w / 2;
		GskRoundedRect in = {.bounds = {{b->origin.x + h, b->origin.y + h}, {fmaxf(b->size.width - w, 0), fmaxf(b->size.height - w, 0)}}};
		for (int i = 0; i < 4; i++) in.corner[i] = (graphene_size_t){fmaxf(rr->corner[i].width - h, 0), fmaxf(rr->corner[i].height - h, 0)};
		GskPathBuilder *pb = gsk_path_builder_new();
		gsk_path_builder_add_rounded_rect(pb, &in);
		stroke_dashed(snap, gsk_path_builder_free_to_path(pb), w, dotted, &c);
		return;
	}
	float x0 = b->origin.x, y0 = b->origin.y, x1 = x0 + b->size.width, y1 = y0 + b->size.height;
	float seg[4][4] = {
		{x0, y0 + bw[0] / 2, x1, y0 + bw[0] / 2},
		{x1 - bw[1] / 2, y0, x1 - bw[1] / 2, y1},
		{x1, y1 - bw[2] / 2, x0, y1 - bw[2] / 2},
		{x0 + bw[3] / 2, y1, x0 + bw[3] / 2, y0},
	};
	for (int i = 0; i < 4; i++) {
		if (bw[i] <= 0) continue;
		GskPathBuilder *pb = gsk_path_builder_new();
		gsk_path_builder_move_to(pb, seg[i][0], seg[i][1]);
		gsk_path_builder_line_to(pb, seg[i][2], seg[i][3]);
		stroke_dashed(snap, gsk_path_builder_free_to_path(pb), bw[i], dotted, &c);
	}
}

// background is the color behind a node's content: its Background, or a
// material's translucent surface (GTK can't blur what's behind a view).
static bool background(node *n, GdkRGBA *c) {
	if (n->background) return lx_resolve(n->background, c);
	if (n->material) {
		static const float alphas[] = {0, 0.6f, 0.7f, 0.8f, 0.9f, 0.85f};
		*c = H.theme.card;
		c->alpha = alphas[n->material < 5 ? n->material : 5];
		return true;
	}
	return false;
}

static void draw_gradient(node *n, GtkSnapshot *snap, graphene_rect_t bounds, GskRoundedRect *rr) {
	uint32_t count = n->gradient.len;
	GskColorStop stack[16];
	GskColorStop *stops = count <= 16 ? stack : malloc(count * sizeof *stops);
	float last = 0;
	for (uint32_t i = 0; i < count; i++) {
		GdkRGBA c = {0};
		lx_resolve(n->gradient.data[i].color, &c);
		float off = fminf(fmaxf(n->gradient.data[i].location, last), 1);  // GSK wants them in order
		stops[i] = (GskColorStop){off, c};
		last = off;
	}
	bool round = lx_rounded_node(n);
	if (round) gtk_snapshot_push_rounded_clip(snap, rr);
	graphene_point_t center = {n->gradient_center[0] * bounds.size.width, n->gradient_center[1] * bounds.size.height};
	if (n->gradient_kind == AK_GRADIENT_KIND_RADIAL) {
		// A circle, its radius a fraction of the shorter side.
		float r = fmaxf(n->gradient_radius * fminf(bounds.size.width, bounds.size.height), 0.01f);
		gtk_snapshot_append_radial_gradient(snap, &bounds, &center, r, r, 0, 1, stops, count);
	} else if (n->gradient_kind == AK_GRADIENT_KIND_CONIC && gtk_snapshot_append_conic_gradient) {
		// From angle, clockwise from the top, as GSK's rotation is.
		gtk_snapshot_append_conic_gradient(snap, &bounds, &center, n->gradient_angle, stops, count);
	} else {
		graphene_point_t start = {n->gradient_line[0] * bounds.size.width, n->gradient_line[1] * bounds.size.height};
		graphene_point_t end = {n->gradient_line[2] * bounds.size.width, n->gradient_line[3] * bounds.size.height};
		if (start.x == end.x && start.y == end.y) end.y += 1;
		gtk_snapshot_append_linear_gradient(snap, &bounds, &start, &end, stops, count);
	}
	if (round) gtk_snapshot_pop(snap);
	if (stops != stack) free(stops);
}

void lx_snapshot_children(GtkWidget *w, GtkSnapshot *snap) {
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) gtk_widget_snapshot_child(w, c, snap);
}

static void snapshot_content(node *n, GtkSnapshot *snap, const graphene_rect_t *b) {
	switch (n->kind) {
	case AK_KIND_IMAGE:
		lx_draw_image(n, snap, b);
		return;
	case AK_KIND_SYMBOL:
		lx_draw_symbol(n, snap, b);
		return;
	}
	lx_draw_more(n, snap, b);
}

// lx_snapshot_view draws a view: its shadow, background (or gradient),
// its children (clipped to its shape when it clips) and its border over
// them, as a layer draws its border.
void lx_snapshot_view(GtkWidget *w, GtkSnapshot *snap) {
	node *n = lx_view_node(w);
	if (!n) {
		if (lx_win_by_content(w)) {
			GdkRGBA bg = H.theme.bg;
			graphene_rect_t r = {{0, 0}, {(float)gtk_widget_get_width(w), (float)gtk_widget_get_height(w)}};
			gtk_snapshot_append_color(snap, &bg, &r);
		}
		lx_snapshot_children(w, snap);
		return;
	}
	float width, height;
	lx_size(n, w, &width, &height);
	graphene_rect_t bounds = {{0, 0}, {width, height}};
	GskRoundedRect rr = lx_node_rounded(n, bounds);
	GdkRGBA c;
	if (n->shadow.color && lx_resolve(n->shadow.color, &c) && c.alpha > 0)
		gtk_snapshot_append_outset_shadow(snap, &rr, &c, n->shadow.dx, n->shadow.dy, 0, n->shadow.blur);
	bool round = lx_rounded_node(n);
	if (n->gradient.len > 0) {
		draw_gradient(n, snap, bounds, &rr);
	} else if (background(n, &c) && c.alpha > 0) {
		if (round) gtk_snapshot_push_rounded_clip(snap, &rr);
		gtk_snapshot_append_color(snap, &c, &bounds);
		if (round) gtk_snapshot_pop(snap);
	}
	// A video's corners round it, as its layer's do on macOS.
	bool clip = n->clip || n->kind == AK_KIND_SCROLL || (n->kind == AK_KIND_VIDEO && round);
	if (clip) {
		if (round)
			gtk_snapshot_push_rounded_clip(snap, &rr);
		else
			gtk_snapshot_push_clip(snap, &bounds);
	}
	// A text's shadow, under its glyphs: what its label draws, shadowed.
	GdkRGBA tc;
	bool shadowed = n->text_shadow.color && gtk_snapshot_push_shadow && lx_resolve(n->text_shadow.color, &tc) && tc.alpha > 0;
	if (shadowed) {
		GskShadow sh = {tc, n->text_shadow.dx, n->text_shadow.dy, n->text_shadow.blur};
		gtk_snapshot_push_shadow(snap, &sh, 1);
	}
	snapshot_content(n, snap, &bounds);
	lx_snapshot_children(w, snap);
	if (shadowed) gtk_snapshot_pop(snap);
	if (clip) gtk_snapshot_pop(snap);
	float bw[4];
	if (border_widths(n, bw) && lx_resolve(n->border_color, &c) && c.alpha > 0) draw_border(n, snap, &rr, bw, c);
	snapshot_highlight(n, snap, bounds);
}

// ---- asuka dev's inspector's highlight (DevHighlight, DevHighlightClear) ----
//
// A translucent box over the view the inspector selected, drawn by the
// view itself after its children (over its control too), so it takes no
// clicks; held by the view's ID, so a view that goes takes it along.

static struct {
	ak_id id;
	GtkWidget *widget;  // referenced: queued to draw again as it goes
	float rect[4];      // in the view, points; a width below 0: all of it
} HL;

void lx_dev_highlight_clear(void) {
	if (HL.widget) {
		gtk_widget_queue_draw(HL.widget);
		g_object_unref(HL.widget);
	}
	HL.widget = NULL;
	HL.id = 0;
}

void lx_dev_highlight(node *n, ak_span args) {
	lx_dev_highlight_clear();
	if (!n || !n->widget || args.len < 16) return;
	ak_reader r = {(const uint8_t *)args.p, args.len, 0, false};
	for (int i = 0; i < 4; i++) HL.rect[i] = ak_read_f32(&r);
	HL.id = n->id;
	HL.widget = g_object_ref(n->widget);
	gtk_widget_queue_draw(n->widget);
}

static void snapshot_highlight(node *n, GtkSnapshot *snap, graphene_rect_t bounds) {
	if (!HL.id || n->id != HL.id) return;
	graphene_rect_t b = bounds;
	if (HL.rect[2] >= 0 && HL.rect[3] >= 0) b = (graphene_rect_t){{HL.rect[0], HL.rect[1]}, {HL.rect[2], HL.rect[3]}};
	GdkRGBA fill = {10 / 255.0f, 132 / 255.0f, 1, 0.18f}, line = {10 / 255.0f, 132 / 255.0f, 1, 1};
	gtk_snapshot_append_color(snap, &fill, &b);
	GskRoundedRect rr = rounded(b, 0);
	float bw[4] = {1, 1, 1, 1};
	GdkRGBA cs[4] = {line, line, line, line};
	gtk_snapshot_append_border(snap, &rr, bw, cs);
}

// ---- asuka dev's performance overlay (DevPerf) ----
//
// The core's numbers (frames a second, flushes, the last flush's phases,
// memory) in a small box over the top end corner of the root's view,
// drawn by its window (AsukaWindow) after everything in it, the dialogs
// the app presented since too, so it takes no clicks and isn't in the
// accessibility tree: monospaced, light on a dark translucent rounded
// box. The core sends its text twice a second while it's on, and none to
// remove it.

static struct {
	GtkWidget *widget;  // the root's, referenced
	char *text;
} PF;

// redraw_perf has the window the root's view is in draw again.
static void redraw_perf(void) {
	GtkRoot *r = PF.widget ? gtk_widget_get_root(PF.widget) : NULL;
	if (r) gtk_widget_queue_draw((GtkWidget *)r);
}

void lx_dev_perf(node *n, ak_span args) {
	redraw_perf();
	if (PF.widget) g_object_unref(PF.widget);
	PF.widget = NULL;
	free(PF.text);
	PF.text = NULL;
	if (!n || !n->widget || !args.len) return;
	PF.text = str_dup(args);
	PF.widget = g_object_ref(n->widget);
	redraw_perf();
}

void lx_dev_perf_dump(ak_buf *b) {
	if (PF.text) ak_buf_printf(b, "DevPerf %s\n", PF.text);
}

static void snapshot_perf(GtkWidget *window, GtkSnapshot *snap) {
	graphene_rect_t root;
	if (!PF.text || (GtkWidget *)gtk_widget_get_root(PF.widget) != window || !gtk_widget_compute_bounds(PF.widget, window, &root)) return;
	PangoLayout *l = gtk_widget_create_pango_layout(window, PF.text);
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
	pango_attr_list_insert(attrs, pango_attr_size_new_absolute(10 * PANGO_SCALE));
	pango_attr_list_insert(attrs, pango_attr_weight_new((PangoWeight)500));  // medium
	pango_layout_set_attributes(l, attrs);
	pango_attr_list_unref(attrs);
	int tw = 0, th = 0;
	pango_layout_get_pixel_size(l, &tw, &th);
	float pw = (float)tw + 12, ph = (float)th + 8;
	float x = root.origin.x + (H.rtl ? 8 : root.size.width - pw - 8), y = root.origin.y + 6;
	graphene_rect_t box = {{x, y}, {pw, ph}};
	GskRoundedRect rr = rounded(box, 6);
	GdkRGBA bg = {0, 0, 0, 0.72f}, fg = {1, 1, 1, 1};
	gtk_snapshot_push_rounded_clip(snap, &rr);
	gtk_snapshot_append_color(snap, &bg, &box);
	gtk_snapshot_pop(snap);
	gtk_snapshot_save(snap);
	gtk_snapshot_translate(snap, &(graphene_point_t){x + 6, y + 4});
	gtk_snapshot_append_layout(snap, l, &fg);
	gtk_snapshot_restore(snap);
	g_object_unref(l);
}
