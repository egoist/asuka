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

GType lx_view_type;

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

void lx_register_classes(void) {
	if (lx_view_type) return;
	GType parent = gtk_widget_get_type();
	GTypeQuery q;
	g_type_query(parent, &q);
	node_offset = (q.instance_size + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *);
	lx_view_type = g_type_register_static_simple(parent, "AsukaView", q.class_size, class_init,
		(guint)(node_offset + sizeof(void *)), NULL, (GTypeFlags)0);
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
	graphene_point_t start = {n->gradient_line[0] * bounds.size.width, n->gradient_line[1] * bounds.size.height};
	graphene_point_t end = {n->gradient_line[2] * bounds.size.width, n->gradient_line[3] * bounds.size.height};
	if (start.x == end.x && start.y == end.y) end.y += 1;
	if (n->corner_radius > 0) gtk_snapshot_push_rounded_clip(snap, rr);
	gtk_snapshot_append_linear_gradient(snap, &bounds, &start, &end, stops, count);
	if (n->corner_radius > 0) gtk_snapshot_pop(snap);
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
	GskRoundedRect rr = rounded(bounds, n->corner_radius);
	GdkRGBA c;
	if (n->shadow.color && lx_resolve(n->shadow.color, &c) && c.alpha > 0)
		gtk_snapshot_append_outset_shadow(snap, &rr, &c, n->shadow.dx, n->shadow.dy, 0, n->shadow.blur);
	bool round = n->corner_radius > 0;
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
	snapshot_content(n, snap, &bounds);
	lx_snapshot_children(w, snap);
	if (clip) gtk_snapshot_pop(snap);
	if (n->border_width > 0 && lx_resolve(n->border_color, &c) && c.alpha > 0) {
		float bw[4] = {n->border_width, n->border_width, n->border_width, n->border_width};
		GdkRGBA cs[4] = {c, c, c, c};
		gtk_snapshot_append_border(snap, &rr, bw, cs);
	}
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
