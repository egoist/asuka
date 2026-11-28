// canvas.c: Canvas views.
//
// A Canvas's display list, replayed by its view with GtkSnapshot. The
// commands are read once, as they're set, into GSK objects (paths,
// strokes, transforms, Pango layouts); a frame appends them, resolving
// the colors in the theme then (semantic colors follow dark mode), with
// no allocation. Clip is a fill clip (gtk_snapshot_push_fill) popped at
// the Restore that ends it; Alpha multiplies the colors drawn after it,
// as Core Graphics' context alpha does. FillRule is read as the commands
// are: each fill and clip keeps its rule. A Transform is a GskTransform
// of its matrix; a radial gradient starts at its center (GSK has no focal
// point); a gradient stroke is the gradient in a stroke clip.
#include "host.h"

#include <math.h>
#include <stdlib.h>

GskPath *lx_build_path(const uint8_t *p, size_t len);
GskStroke *lx_new_stroke(float width, int cap, int join, const float *dash, size_t n, float phase);

typedef struct {
	uint8_t op;
	GskFillRule rule;  // Clip, Fill, the gradients
	bool radial;       // RadialGradient, StrokeGradient: a radial one (from: its center)
	float radius;
	GskPath *path;      // Clip, Fill, Stroke, the gradients
	GskStroke *stroke;  // Stroke's, StrokeGradient's
	GskTransform *tr;   // Translate, Scale, Rotate, Transform
	ak_color color;
	float alpha;  // Alpha
	// A gradient: its stops (in order), the line, the path's bounds; cs
	// the frame's resolved stops.
	grad_stop *stops;
	GskColorStop *cs;
	uint32_t nstops;
	graphene_point_t from, to;
	graphene_rect_t bounds;
	// Text: its layout and its top left.
	PangoLayout *layout;
	graphene_point_t at;
} canvas_cmd;

typedef struct {
	float alpha;
	int pushes;  // fill clips pushed before the Save
} canvas_save;

typedef struct {
	AK_VEC(canvas_cmd) cmds;
	AK_VEC(canvas_save) saves;  // a frame's, kept for the next
} canvas_view;

static canvas_view *canvas_of(node *n) { return n->kind == AK_KIND_CANVAS ? n->ext : NULL; }

// clear lets go of the GSK objects and layouts the commands hold.
static void clear(canvas_view *c) {
	for (uint32_t i = 0; i < c->cmds.len; i++) {
		canvas_cmd *x = &c->cmds.data[i];
		if (x->path) gsk_path_unref(x->path);
		if (x->stroke) gsk_stroke_free(x->stroke);
		if (x->tr) gsk_transform_unref(x->tr);
		if (x->layout) g_object_unref(x->layout);
		free(x->stops);
		free(x->cs);
	}
	c->cmds.len = 0;
}

static void free_canvas(node *n) {
	canvas_view *c = n->ext;
	clear(c);
	ak_vec_free(&c->cmds);
	ak_vec_free(&c->saves);
	free(c);
	n->ext = NULL;
}

static bool canvas_create(node *n) {
	if (n->kind != AK_KIND_CANVAS) return false;
	canvas_view *c = calloc(1, sizeof *c);
	if (!c) abort();
	n->ext = c;
	n->ext_free = free_canvas;
	return true;
}

static float rf(ak_reader *r) {
	float v = ak_read_f32(r);
	return v != v ? 0 : v;
}

static bool has(ak_reader *r, size_t n) { return !r->err && r->off + n <= r->len; }

static GskPath *read_path(ak_reader *r) {
	uint32_t n = ak_read_u32(r);
	if (r->err) return NULL;
	if (n > r->len - r->off) n = (uint32_t)(r->len - r->off);
	GskPath *p = lx_build_path(r->p + r->off, n);
	r->off += n;
	return p;
}

static void read_stops(ak_reader *r, canvas_cmd *x) {
	uint32_t n = ak_read_u32(r);
	if (n > (r->len - r->off) / 12) n = (uint32_t)((r->len - r->off) / 12);
	x->stops = calloc(n ? n : 1, sizeof *x->stops);
	x->cs = calloc(n < 2 ? 2 : n, sizeof *x->cs);
	if (!x->stops || !x->cs) abort();
	for (uint32_t i = 0; i < n; i++) {
		x->stops[i].color = ak_read_u64(r);
		x->stops[i].location = rf(r);
	}
	x->nstops = n;
}

static bool read_stroke(ak_reader *r, canvas_cmd *x) {
	float w = rf(r);
	if (!has(r, 2)) return false;
	uint8_t cap = ak_read_u8(r), join = ak_read_u8(r);
	uint32_t nd = ak_read_u32(r);
	float phase = rf(r);
	float dash[64];
	size_t k = 0;
	for (uint32_t i = 0; i < nd && has(r, 4); i++) {
		float d = rf(r);
		if (k < AK_LEN(dash)) dash[k++] = d;
	}
	x->stroke = lx_new_stroke(w, cap > 2 ? 2 : cap, join > 2 ? 2 : join, dash, k, phase);
	if (w > x->radius) x->radius = w;  // the stroke's reach past its path, for its gradient's bounds
	return true;
}

static void path_bounds(canvas_cmd *x, float pad) {
	if (x->path && !gsk_path_get_bounds(x->path, &x->bounds)) {
		gsk_path_unref(x->path);
		x->path = NULL;
	}
	x->bounds.origin.x -= pad;
	x->bounds.origin.y -= pad;
	x->bounds.size.width += 2 * pad;
	x->bounds.size.height += 2 * pad;
}

// canvas_text lays a text out for a canvas: its layout, and its top left
// for its baseline at x, y and its alignment there.
static PangoLayout *canvas_text(node *n, ak_span s, float size, int weight, int align, float x, float y, graphene_point_t *at) {
	if (!s.len || !n->widget) return NULL;
	char *text = str_dup(s);
	PangoLayout *l = gtk_widget_create_pango_layout(n->widget, text);
	free(text);
	node f = {.font_size = size > 0 ? size : 17, .font_weight = weight};
	float px;
	int wt;
	lx_font(&f, &px, &wt);
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_size_new_absolute((int)(f.font_size * 1024 + 0.5f)));
	pango_attr_list_insert(attrs, pango_attr_weight_new((PangoWeight)(weight > 0 ? wt : 400)));
	pango_layout_set_attributes(l, attrs);
	pango_attr_list_unref(attrs);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	at->x = x;
	at->y = y - (float)pango_layout_get_baseline(l) / 1024;
	if (align == AK_TEXT_ALIGN_CENTER) at->x -= (float)w / 2;
	if (align == AK_TEXT_ALIGN_END) at->x -= (float)w;
	return l;
}

// compile reads the display list into commands.
static void compile(node *n, canvas_view *c, const uint8_t *b, size_t len) {
	ak_reader r = {b, len, 0, false};
	GskFillRule rule = GSK_FILL_RULE_WINDING;
	AK_VEC(GskFillRule) rules = {0};
	while (has(&r, 1)) {
		canvas_cmd x = {.op = ak_read_u8(&r), .rule = rule};
		switch (x.op) {
		case AK_CANVAS_OP_SAVE:
			ak_vec_push(&rules, rule);
			break;
		case AK_CANVAS_OP_RESTORE:
			if (rules.len) rule = rules.data[--rules.len];
			break;
		case AK_CANVAS_OP_FILL_RULE:
			if (has(&r, 1)) rule = ak_read_u8(&r) == 1 ? GSK_FILL_RULE_EVEN_ODD : GSK_FILL_RULE_WINDING;
			continue;
		case AK_CANVAS_OP_TRANSFORM: {
			float a = rf(&r), bb = rf(&r), cc = rf(&r), d = rf(&r), tx = rf(&r), ty = rf(&r);
			graphene_matrix_t m;
			graphene_matrix_init_from_2d(&m, a, bb, cc, d, tx, ty);
			x.tr = gsk_transform_matrix(NULL, &m);
			break;
		}
		case AK_CANVAS_OP_RADIAL_GRADIENT:
			read_stops(&r, &x);
			x.radial = true;
			x.from.x = rf(&r);
			x.from.y = rf(&r);
			x.radius = rf(&r);
			rf(&r);  // its focus: GSK has none
			rf(&r);
			x.path = read_path(&r);
			path_bounds(&x, 0);
			break;
		case AK_CANVAS_OP_STROKE_GRADIENT: {
			if (!read_stroke(&r, &x) || !has(&r, 1)) goto out;
			float pad = x.radius;
			x.radius = 0;
			x.radial = ak_read_u8(&r) == 1;
			read_stops(&r, &x);
			x.from.x = rf(&r);
			x.from.y = rf(&r);
			if (x.radial) {
				x.radius = rf(&r);
				rf(&r);
				rf(&r);
			} else {
				x.to.x = rf(&r);
				x.to.y = rf(&r);
				if (x.from.x == x.to.x && x.from.y == x.to.y) x.to.y += 1;
			}
			x.path = read_path(&r);
			path_bounds(&x, pad);
			break;
		}
		case AK_CANVAS_OP_TRANSLATE: {
			graphene_point_t p = {rf(&r), 0};
			p.y = rf(&r);
			x.tr = gsk_transform_translate(NULL, &p);
			break;
		}
		case AK_CANVAS_OP_SCALE: {
			float sx = rf(&r), sy = rf(&r);
			x.tr = gsk_transform_scale(NULL, sx, sy);
			break;
		}
		case AK_CANVAS_OP_ROTATE:
			x.tr = gsk_transform_rotate(NULL, rf(&r));  // y down: clockwise on screen
			break;
		case AK_CANVAS_OP_ALPHA:
			x.alpha = fminf(fmaxf(rf(&r), 0), 1);
			break;
		case AK_CANVAS_OP_CLIP:
			x.path = read_path(&r);
			if (!x.path) x.path = gsk_path_parse("M 0 0 Z");  // clips everything away
			break;
		case AK_CANVAS_OP_FILL:
			x.color = ak_read_u64(&r);
			x.path = read_path(&r);
			break;
		case AK_CANVAS_OP_STROKE:
			x.color = ak_read_u64(&r);
			if (!read_stroke(&r, &x)) goto out;
			x.radius = 0;
			x.path = read_path(&r);
			break;
		case AK_CANVAS_OP_GRADIENT:
			read_stops(&r, &x);
			x.from.x = rf(&r);
			x.from.y = rf(&r);
			x.to.x = rf(&r);
			x.to.y = rf(&r);
			if (x.from.x == x.to.x && x.from.y == x.to.y) x.to.y += 1;
			x.path = read_path(&r);
			path_bounds(&x, 0);
			break;
		case AK_CANVAS_OP_TEXT: {
			x.color = ak_read_u64(&r);
			float size = rf(&r);
			if (!has(&r, 2)) goto out;
			int weight = ak_read_u8(&r), align = ak_read_u8(&r);
			float tx = rf(&r), ty = rf(&r);
			uint32_t l = ak_read_u32(&r);
			if (r.err) goto out;
			if (l > r.len - r.off) l = (uint32_t)(r.len - r.off);
			x.layout = canvas_text(n, ak_strn((const char *)r.p + r.off, l), size, weight, align, tx, ty, &x.at);
			r.off += l;
			break;
		}
		default:
			goto out;
		}
		if (r.err) {
			if (x.path) gsk_path_unref(x.path);
			if (x.stroke) gsk_stroke_free(x.stroke);
			if (x.tr) gsk_transform_unref(x.tr);
			if (x.layout) g_object_unref(x.layout);
			free(x.stops);
			free(x.cs);
			goto out;
		}
		ak_vec_push(&c->cmds, x);
	}
out:
	ak_vec_free(&rules);
}

static bool canvas_prop(node *n, ak_prop p, const ak_value *v) {
	canvas_view *c = canvas_of(n);
	if (!c || p != AK_PROP_CANVAS_COMMANDS) return false;
	if (!c->cmds.len && n->widget) gtk_widget_set_can_target(n->widget, 0);  // clicks go to what's under it
	clear(c);
	compile(n, c, (const uint8_t *)v->s.p, v->s.len);
	lx_queue_draw(n);
	return true;
}

static bool resolve(ak_color col, float alpha, GdkRGBA *out) {
	bool ok = lx_resolve(col, out);
	out->alpha *= alpha;
	return ok && col && out->alpha > 0;
}

// gradient fills a path (or a Stroke's outline) with a linear or radial
// gradient, padded before its start and after its end.
static void gradient(canvas_cmd *x, GtkSnapshot *snap, float alpha) {
	if (!x->path || !x->nstops) return;
	float last = 0;
	for (uint32_t i = 0; i < x->nstops; i++) {
		GdkRGBA c;
		lx_resolve(x->stops[i].color, &c);
		c.alpha *= alpha;
		float off = fminf(fmaxf(x->stops[i].location, last), 1);  // GSK wants them in order
		x->cs[i] = (GskColorStop){off, c};
		last = off;
	}
	gsize n = x->nstops;
	if (n == 1) {
		x->cs[1] = (GskColorStop){1, x->cs[0].color};
		x->cs[0].offset = 0;
		n = 2;
	}
	if (x->stroke)
		gtk_snapshot_push_stroke(snap, x->path, x->stroke);
	else
		gtk_snapshot_push_fill(snap, x->path, x->rule);
	if (x->radial)
		gtk_snapshot_append_radial_gradient(snap, &x->bounds, &x->from, x->radius, x->radius, 0, 1, x->cs, n);
	else
		gtk_snapshot_append_linear_gradient(snap, &x->bounds, &x->from, &x->to, x->cs, n);
	gtk_snapshot_pop(snap);
}

static void display_draw(canvas_view *c, GtkSnapshot *snap);

static bool canvas_draw(node *n, GtkSnapshot *snap, const graphene_rect_t *b) {
	(void)b;
	canvas_view *c = canvas_of(n);
	if (!c) return false;
	display_draw(c, snap);
	return true;
}

// ---- Display lists another view draws (an SVG Image's: the core's
// commands for it) ----

lx_display *lx_display_new(void) {
	canvas_view *c = calloc(1, sizeof *c);
	if (!c) abort();
	return (lx_display *)c;
}

void lx_display_set(lx_display *d, node *n, const uint8_t *b, size_t len) {
	canvas_view *c = (canvas_view *)d;
	clear(c);
	compile(n, c, b, len);
}

void lx_display_draw(lx_display *d, GtkSnapshot *snap) { display_draw((canvas_view *)d, snap); }

void lx_display_free(lx_display *d) {
	canvas_view *c = (canvas_view *)d;
	if (!c) return;
	clear(c);
	ak_vec_free(&c->cmds);
	ak_vec_free(&c->saves);
	free(c);
}

static void display_draw(canvas_view *c, GtkSnapshot *snap) {
	if (!c->cmds.len) return;
	gtk_snapshot_save(snap);
	float alpha = 1;
	int pushes = 0;
	c->saves.len = 0;
	GdkRGBA col;
	for (uint32_t i = 0; i < c->cmds.len; i++) {
		canvas_cmd *x = &c->cmds.data[i];
		switch (x->op) {
		case AK_CANVAS_OP_SAVE:
			ak_vec_push(&c->saves, ((canvas_save){alpha, pushes}));
			pushes = 0;
			gtk_snapshot_save(snap);
			break;
		case AK_CANVAS_OP_RESTORE: {
			if (!c->saves.len) break;
			for (; pushes > 0; pushes--) gtk_snapshot_pop(snap);
			gtk_snapshot_restore(snap);
			canvas_save s = c->saves.data[--c->saves.len];
			alpha = s.alpha;
			pushes = s.pushes;
			break;
		}
		case AK_CANVAS_OP_TRANSLATE:
		case AK_CANVAS_OP_SCALE:
		case AK_CANVAS_OP_ROTATE:
		case AK_CANVAS_OP_TRANSFORM:
			if (x->tr) gtk_snapshot_transform(snap, x->tr);
			break;
		case AK_CANVAS_OP_ALPHA:
			alpha *= x->alpha;
			break;
		case AK_CANVAS_OP_CLIP:
			gtk_snapshot_push_fill(snap, x->path, x->rule);
			pushes++;
			break;
		case AK_CANVAS_OP_FILL:
			if (x->path && resolve(x->color, alpha, &col)) gtk_snapshot_append_fill(snap, x->path, x->rule, &col);
			break;
		case AK_CANVAS_OP_STROKE:
			if (x->path && resolve(x->color, alpha, &col)) gtk_snapshot_append_stroke(snap, x->path, x->stroke, &col);
			break;
		case AK_CANVAS_OP_GRADIENT:
		case AK_CANVAS_OP_RADIAL_GRADIENT:
		case AK_CANVAS_OP_STROKE_GRADIENT:
			gradient(x, snap, alpha);
			break;
		case AK_CANVAS_OP_TEXT:
			if (x->layout && resolve(x->color, alpha, &col)) {
				gtk_snapshot_save(snap);
				gtk_snapshot_translate(snap, &x->at);
				gtk_snapshot_append_layout(snap, x->layout, &col);
				gtk_snapshot_restore(snap);
			}
			break;
		}
	}
	for (;;) {
		for (; pushes > 0; pushes--) gtk_snapshot_pop(snap);
		if (!c->saves.len) break;
		gtk_snapshot_restore(snap);
		pushes = c->saves.data[--c->saves.len].pushes;
	}
	gtk_snapshot_restore(snap);
}

const area lx_area_canvas = {
	.name = "canvas",
	.create = canvas_create,
	.prop = canvas_prop,
	.draw = canvas_draw,
};
