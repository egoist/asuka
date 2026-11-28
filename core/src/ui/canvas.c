// canvas.c: Canvas and what it draws with (its Graphics), Path, Shape
// and Shader; std/svg's Document's builtins (svg.read, svg.size,
// svg.draw: ui/svg.c reads and draws it).
//
// A Canvas's draw closure gets a Graphics, a runtime object valid while
// it runs: each call records a drawing command, in points from the
// canvas's top left (y down), and the host replays them natively (Core
// Graphics, Android's Canvas). A Path is a builder: its methods add to it
// and return it, to chain (Path().move(0, 0).line(w, h)). Graphics.path()
// gives one of the canvas's own, reused from draw to draw, so drawing
// again allocates nothing once it has drawn as much before.
//
// A Canvas and a Shape know their size once they're laid out (as a
// layout handler does: after the frames are applied), and draw again
// when it changes and when what they read changes; only a drawing that
// differs from the last is sent.
#include "ui/ui_ext.h"
#include "ui/svg.h"

// ---- Path ----

typedef struct ak_path_obj {
	ak_opaque h;
	ak_buf b;
} ak_path_obj;

static void path_drop(ak_opaque *o) { ak_buf_free(&((ak_path_obj *)o)->b); }

static ak_path_obj *path_new(void) { return ak_opaque_new(AK_TAG_OPAQUE, sizeof(ak_path_obj), path_drop); }

static bool is_path(const void *p) { return p && ((const ak_obj *)p)->tag == AK_TAG_OPAQUE && ((const ak_opaque *)p)->drop == path_drop; }

static ak_path_obj *path_arg(void *p, const char *what) {
	if (!is_path(p)) ak_panic("%s: not a Path", what);
	return p;
}

static void path_op(ak_path_obj *p, uint8_t op, const float *args, int n) {
	ak_buf_u8(&p->b, op);
	for (int i = 0; i < n; i++) ak_buf_f32(&p->b, args[i]);
}

// ---- Graphics ----

typedef struct ak_graphics {
	ak_opaque hdr;
	ak_buf b;
	float w, h;
	AK_VEC(ak_path_obj *) paths;  // reused from draw to draw
	uint32_t np;
	bool live;  // while a draw runs
} ak_graphics;

static void graphics_drop(ak_opaque *o) {
	ak_graphics *g = (ak_graphics *)o;
	ak_buf_free(&g->b);
	for (uint32_t i = 0; i < g->paths.len; i++) ak_release(g->paths.data[i]);
	ak_vec_free(&g->paths);
}

static ak_graphics *gfx(void *p, const char *what) {
	if (!p || ((ak_obj *)p)->tag != AK_TAG_OPAQUE || ((ak_opaque *)p)->drop != graphics_drop) ak_panic("%s: not a Graphics", what);
	ak_graphics *g = p;
	if (!g->live) ak_panic("%s: a Graphics is only valid while its canvas draws", what);
	return g;
}

static void g_op(ak_graphics *g, uint8_t op) { ak_buf_u8(&g->b, op); }
static void g_f32(ak_graphics *g, double f) { ak_buf_f32(&g->b, (float)f); }
static void g_color(ak_graphics *g, int64_t c) { ak_buf_u64(&g->b, (uint64_t)c); }

static void g_path(ak_graphics *g, ak_path_obj *p) {
	ak_buf_u32(&g->b, p ? (uint32_t)p->b.len : 0);
	if (p) ak_buf_put(&g->b, p->b.data, p->b.len);
}

// stops are a gradient's colors at their locations (evenly spaced past
// the locations given).
static void g_stops(ak_graphics *g, const ak_arr *colors, const ak_arr *locs) {
	uint32_t n = colors ? colors->len : 0;
	ak_buf_u32(&g->b, n);
	for (uint32_t i = 0; i < n; i++) {
		double loc = 0;
		if (locs && i < locs->len) loc = ak_arr_vals(locs)[i].f;
		else if (n > 1) loc = (double)i / (double)(n - 1);
		g_color(g, ak_arr_vals(colors)[i].i);
		g_f32(g, loc);
	}
}

// StrokeStyle: width, cap, join, dash, dashPhase.
static void g_stroke_style(ak_graphics *g, const ak_struct *s) {
	double w = s ? s->f[0].f : 1;
	if (w <= 0) w = 1;
	g_f32(g, w);
	ak_buf_u8(&g->b, s ? (uint8_t)s->f[1].i : 0);
	ak_buf_u8(&g->b, s ? (uint8_t)s->f[2].i : 0);
	const ak_arr *dash = s ? s->f[3].p : NULL;
	ak_buf_u32(&g->b, dash ? dash->len : 0);
	g_f32(g, s ? s->f[4].f : 0);
	for (uint32_t i = 0; dash && i < dash->len; i++) g_f32(g, ak_arr_vals(dash)[i].f);
}

// RadialGradient: colors, locations, centerX, centerY, radius, focusX?,
// focusY?.
static void g_radial(ak_graphics *g, const ak_struct *r) {
	double cx = r->f[2].f, cy = r->f[3].f;
	const ak_box *fx = r->f[5].p, *fy = r->f[6].p;
	g_f32(g, cx), g_f32(g, cy), g_f32(g, r->f[4].f);
	g_f32(g, fx ? fx->v.f : cx), g_f32(g, fy ? fy->v.f : cy);
}

// ---- Graphics, to compiled code (canvas.* builtins) ----

static ak_val none(void) { return (ak_val){0}; }
static ak_val fv(double f) { return (ak_val){.f = f}; }

static ak_val b_width(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return fv(gfx(a[0].p, "canvas.width")->w);
}

static ak_val b_height(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	return fv(gfx(a[0].p, "canvas.height")->h);
}

// canvas.path(g): one of the canvas's paths, empty.
static ak_val b_gpath(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.path");
	if (g->np == g->paths.len) ak_vec_push(&g->paths, path_new());
	ak_path_obj *p = g->paths.data[g->np++];
	p->b.len = 0;
	return (ak_val){.p = ak_retain(p)};
}

static ak_val b_path_new(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = path_new()};
}

static ak_val path_ret(ak_path_obj *p) { return (ak_val){.p = ak_retain(p)}; }

#define PATH_OP(fname, op, nargs)                              \
	static ak_val fname(ak_val *a, uint32_t n) {               \
		AK_UNUSED(n);                                          \
		ak_path_obj *p = path_arg(a[0].p, "path");              \
		float f[6];                                            \
		for (int i = 0; i < (nargs); i++) f[i] = (float)a[1 + i].f; \
		path_op(p, op, f, nargs);                              \
		return path_ret(p);                                    \
	}

PATH_OP(b_move, AK_PATH_OP_MOVE, 2)
PATH_OP(b_line, AK_PATH_OP_LINE, 2)
PATH_OP(b_quad, AK_PATH_OP_QUAD, 4)
PATH_OP(b_cubic, AK_PATH_OP_CUBIC, 6)
PATH_OP(b_rect, AK_PATH_OP_RECT, 4)
PATH_OP(b_rounded, AK_PATH_OP_ROUNDED_RECT, 5)
PATH_OP(b_ellipse, AK_PATH_OP_ELLIPSE, 4)

static ak_val b_close(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_path_obj *p = path_arg(a[0].p, "path.close");
	path_op(p, AK_PATH_OP_CLOSE, NULL, 0);
	return path_ret(p);
}

// path.arc(p, cx, cy, r, start, end, clockwise): radians, clockwise from
// the positive x axis (the screen's way).
static ak_val b_arc(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_path_obj *p = path_arg(a[0].p, "path.arc");
	float f[6] = {(float)a[1].f, (float)a[2].f, (float)a[3].f, (float)a[4].f, (float)a[5].f, a[6].i ? 1.0f : 0.0f};
	path_op(p, AK_PATH_OP_ARC, f, 6);
	return path_ret(p);
}

static ak_val b_save(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	g_op(gfx(a[0].p, "canvas.save"), AK_CANVAS_OP_SAVE);
	return none();
}

static ak_val b_restore(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	g_op(gfx(a[0].p, "canvas.restore"), AK_CANVAS_OP_RESTORE);
	return none();
}

static ak_val b_translate(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.translate");
	g_op(g, AK_CANVAS_OP_TRANSLATE), g_f32(g, a[1].f), g_f32(g, a[2].f);
	return none();
}

static ak_val b_scale(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.scale");
	g_op(g, AK_CANVAS_OP_SCALE), g_f32(g, a[1].f), g_f32(g, a[2].f);
	return none();
}

static ak_val b_rotate(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.rotate");
	g_op(g, AK_CANVAS_OP_ROTATE), g_f32(g, a[1].f);
	return none();
}

static ak_val b_opacity(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.opacity");
	g_op(g, AK_CANVAS_OP_ALPHA), g_f32(g, a[1].f);
	return none();
}

static ak_val b_clip(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.clip");
	g_op(g, AK_CANVAS_OP_CLIP), g_path(g, path_arg(a[1].p, "canvas.clip"));
	return none();
}

static ak_val b_fill(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.fill");
	ak_path_obj *p = path_arg(a[1].p, "canvas.fill");
	g_op(g, AK_CANVAS_OP_FILL), g_color(g, a[2].i), g_path(g, p);
	return none();
}

// canvas.stroke(g, path, color, style: StrokeStyle?)
static ak_val b_stroke(ak_val *a, uint32_t n) {
	ak_graphics *g = gfx(a[0].p, "canvas.stroke");
	ak_path_obj *p = path_arg(a[1].p, "canvas.stroke");
	g_op(g, AK_CANVAS_OP_STROKE), g_color(g, a[2].i);
	g_stroke_style(g, n > 3 ? a[3].p : NULL);
	g_path(g, p);
	return none();
}

// canvas.fill_gradient(g, path, LinearGradient): its start and end both 0
// are the canvas's top to its bottom.
static ak_val b_fill_gradient(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.fill_gradient");
	ak_path_obj *p = path_arg(a[1].p, "canvas.fill_gradient");
	const ak_struct *gr = a[2].p;
	g_op(g, AK_CANVAS_OP_GRADIENT);
	g_stops(g, gr ? gr->f[0].p : NULL, gr ? gr->f[1].p : NULL);
	double sx = gr ? gr->f[2].f : 0, sy = gr ? gr->f[3].f : 0, ex = gr ? gr->f[4].f : 0, ey = gr ? gr->f[5].f : 0;
	if (sx == 0 && sy == 0 && ex == 0 && ey == 0) ey = g->h;
	g_f32(g, sx), g_f32(g, sy), g_f32(g, ex), g_f32(g, ey);
	g_path(g, p);
	return none();
}

static ak_val b_fill_radial(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.fill_radial");
	ak_path_obj *p = path_arg(a[1].p, "canvas.fill_radial");
	const ak_struct *gr = a[2].p;
	if (!gr) return none();
	g_op(g, AK_CANVAS_OP_RADIAL_GRADIENT);
	g_stops(g, gr->f[0].p, gr->f[1].p);
	g_radial(g, gr);
	g_path(g, p);
	return none();
}

// canvas.stroke_gradient(g, path, StrokeStyle?, LinearGradient)
static ak_val b_stroke_gradient(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.stroke_gradient");
	ak_path_obj *p = path_arg(a[1].p, "canvas.stroke_gradient");
	const ak_struct *gr = a[3].p;
	g_op(g, AK_CANVAS_OP_STROKE_GRADIENT);
	g_stroke_style(g, a[2].p);
	ak_buf_u8(&g->b, 0);
	g_stops(g, gr ? gr->f[0].p : NULL, gr ? gr->f[1].p : NULL);
	for (int i = 2; i <= 5; i++) g_f32(g, gr ? gr->f[i].f : 0);
	g_path(g, p);
	return none();
}

static ak_val b_stroke_radial(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.stroke_radial");
	ak_path_obj *p = path_arg(a[1].p, "canvas.stroke_radial");
	const ak_struct *gr = a[3].p;
	if (!gr) return none();
	g_op(g, AK_CANVAS_OP_STROKE_GRADIENT);
	g_stroke_style(g, a[2].p);
	ak_buf_u8(&g->b, 1);
	g_stops(g, gr->f[0].p, gr->f[1].p);
	g_radial(g, gr);
	g_path(g, p);
	return none();
}

static ak_val b_transform(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.transform");
	g_op(g, AK_CANVAS_OP_TRANSFORM);
	for (int i = 1; i <= 6; i++) g_f32(g, a[i].f);
	return none();
}

static ak_val b_even_odd(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "canvas.even_odd");
	g_op(g, AK_CANVAS_OP_FILL_RULE);
	ak_buf_u8(&g->b, a[1].i ? 1 : 0);
	return none();
}

// canvas.text(g, s, x, y, CanvasText?): on one line, its baseline at y,
// at its own size (not the user's text size).
static ak_val b_text(ak_val *a, uint32_t n) {
	ak_graphics *g = gfx(a[0].p, "canvas.text");
	const ak_str *s = a[1].p;
	const ak_struct *t = n > 4 ? a[4].p : NULL;
	double size = t ? t->f[0].f : 0;
	int64_t color = t ? t->f[2].i : 0;
	if (size <= 0) size = 17;
	if (color == 0) color = (int64_t)ak_semantic_color(AK_SEMANTIC_COLOR_LABEL);
	g_op(g, AK_CANVAS_OP_TEXT);
	g_color(g, color);
	g_f32(g, size);
	ak_buf_u8(&g->b, t ? (uint8_t)t->f[1].i : 0);
	ak_buf_u8(&g->b, t ? (uint8_t)t->f[3].i : 0);
	g_f32(g, a[2].f), g_f32(g, a[3].f);
	ak_buf_str(&g->b, s ? s->data : "", s ? s->len : 0);
	return none();
}

// canvas.color_uniform(name, color): a color as a uniform vec4, its red,
// green, blue and alpha from 0 to 1 (a semantic color reads as black).
extern const ak_shape ak_ui_shape_uniform;

static ak_val b_color_uniform(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	uint8_t r = 0, g = 0, b = 0, al = 255;
	if (!ak_color_channels((ak_color)a[1].i, &r, &g, &b, &al)) al = ak_color_alpha((ak_color)a[1].i);
	ak_struct *u = ak_struct_new(&ak_ui_shape_uniform);
	u->f[0].p = ak_retain(a[0].p ? a[0].p : ak_str_empty());
	ak_arr *v = ak_arr_new(AK_K_FLOAT, NULL, 4);
	uint8_t c[4] = {r, g, b, al};
	for (int i = 0; i < 4; i++) ak_arr_push(&v, fv((double)c[i] / 255));
	u->f[1].p = v;
	return (ak_val){.p = u};
}

// ---- std/svg's Document (ui/svg.c), to compiled code ----

// svg.read(markup) -> bytes: the document, or a throw saying why not.
// Any thread (spawn's): it keeps no state.
static ak_val b_svg_read(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *s = a[0].p;
	char err[160];
	ak_buf b = {0};
	if (!ak_svg_parse(s ? s->data : "", s ? s->len : 0, &b, err)) {
		ak_buf_free(&b);
		ak_throw_msg("svg: %s", err);
		return none();
	}
	ak_bytes *r = ak_bytes_new(b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = r};
}

// svg.size(data) -> [float]: its width and height ([0, 0]: not a
// document).
static ak_val b_svg_size(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	const ak_bytes *d = a[0].p;
	float w = 0, h = 0;
	if (d) ak_svg_size(d->data, d->len, &w, &h);
	ak_arr *v = ak_arr_new(AK_K_FLOAT, NULL, 2);
	ak_arr_push(&v, fv(w));
	ak_arr_push(&v, fv(h));
	return (ak_val){.p = v};
}

// svg.draw(g, data, fit, color): the document fitted into the canvas (an
// ImageMode's way), its currentColor color.
static ak_val b_svg_draw(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_graphics *g = gfx(a[0].p, "svg.draw");
	const ak_bytes *d = a[1].p;
	ak_color c = (ak_color)a[3].i;
	if (!c) c = ak_semantic_color(AK_SEMANTIC_COLOR_LABEL);
	if (d) ak_svg_draw(d->data, d->len, &g->b, g->w, g->h, (int)a[2].i, c, 0);
	return none();
}

void ak_canvas_builtins_init(void);

void ak_canvas_builtins_init(void) {
	static const struct {
		const char *name;
		ak_builtin_fn fn;
	} t[] = {
		{"canvas.width", b_width}, {"canvas.height", b_height}, {"canvas.path", b_gpath}, {"canvas.save", b_save},
		{"canvas.restore", b_restore}, {"canvas.translate", b_translate}, {"canvas.scale", b_scale}, {"canvas.rotate", b_rotate},
		{"canvas.opacity", b_opacity}, {"canvas.clip", b_clip}, {"canvas.fill", b_fill}, {"canvas.stroke", b_stroke},
		{"canvas.fill_gradient", b_fill_gradient}, {"canvas.fill_radial", b_fill_radial}, {"canvas.stroke_gradient", b_stroke_gradient},
		{"canvas.stroke_radial", b_stroke_radial}, {"canvas.transform", b_transform}, {"canvas.even_odd", b_even_odd},
		{"canvas.text", b_text}, {"canvas.color_uniform", b_color_uniform}, {"path.new", b_path_new}, {"path.move", b_move},
		{"path.line", b_line}, {"path.quad", b_quad}, {"path.cubic", b_cubic}, {"path.arc", b_arc}, {"path.close", b_close},
		{"path.rect", b_rect}, {"path.rounded_rect", b_rounded}, {"path.ellipse", b_ellipse},
		{"svg.read", b_svg_read}, {"svg.size", b_svg_size}, {"svg.draw", b_svg_draw},
	};
	for (size_t i = 0; i < AK_LEN(t); i++) ak_builtin_register(t[i].name, t[i].fn);
}

// ---- Sized views (Canvas, Shape) ----

// A Canvas's or a Shape's size, as last laid out, a signal its drawing
// reads; and what it last sent.
typedef struct {
	ak_node size_node;
	float w, h;
	ak_buf sent;
	bool known;
	ak_graphics *g;  // a Canvas's
	bool effect;     // its drawing effect is made
} sized_state;

static void sized_init(ak_view *e) {
	sized_state *s = ak_alloc(sizeof *s);
	ak_signal_init(&s->size_node);
	e->impl = s;
}

static void sized_drop(ak_view *e) {
	sized_state *s = e->impl;
	ak_signal_drop(&s->size_node);
	ak_buf_free(&s->sent);
	ak_release(s->g);
	ak_free(s);
}

static void sized(ak_view *e, float w, float h) {
	sized_state *s = e->impl;
	if (s->w == w && s->h == h) return;
	s->w = w, s->h = h;
	ak_signal_write(&s->size_node);
}

// send_if_changed sends b as prop p unless it's what was last sent.
static void send_if_changed(ak_view *e, sized_state *s, ak_prop p, const uint8_t *b, size_t n) {
	if (s->known && s->sent.len == n && (n == 0 || memcmp(s->sent.data, b, n) == 0)) return;
	s->known = true;
	s->sent.len = 0;
	ak_buf_put(&s->sent, b, n);
	ak_batch_set_bytes(ak_view_batch(e), e->id, p, s->sent.data, s->sent.len);
}

static void draw_drop(void *ctx) {
	ak_release(ctx);
}

static const ak_comp_info canvas_info = {.drop = draw_drop, .label = "Canvas.draw"};
static const ak_comp_info shape_info = {.drop = draw_drop, .label = "Shape.path"};

// ---- Canvas ----

static void canvas_run(void *ctx) {
	ak_view *e = ctx;
	sized_state *s = e->impl;
	ak_signal_read(&s->size_node);
	if (e->dead || s->w <= 0 || s->h <= 0) return;
	const ak_val *dv = ak_style_get(e, AK_KEY_CANVAS_DRAW);
	ak_closure *draw = dv ? dv->p : NULL;
	if (!draw) return;
	if (!s->g) s->g = ak_opaque_new(AK_TAG_OPAQUE, sizeof(ak_graphics), graphics_drop);
	ak_graphics *g = s->g;
	g->b.len = 0, g->np = 0, g->w = s->w, g->h = s->h;
	g->live = true;
	ak_retain(draw);
	ak_val arg = {.p = g};
	ak_val r = ak_call(draw, &arg, 1);
	g->live = false;
	ak_release(draw);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	ak_release(r.p);
	send_if_changed(e, s, AK_PROP_CANVAS_COMMANDS, g->b.data, g->b.len);
}

// The drawing effect is owned by what owns the view.
static void canvas_effect(void *ctx) { ak_effect_new(canvas_run, ak_retain(ctx), AK_EFFECT_RENDER, &canvas_info); }
static void shape_effect(void *ctx);

static void own_effect(ak_view *e, void (*make)(void *ctx)) {
	sized_state *s = e->impl;
	if (s->effect) return;
	s->effect = true;
	if (e->owner && !ak_owner_disposed(e->owner)) ak_with_owner(e->owner, make, e);
	else make(e);
}

static void canvas_finalize(ak_view *e) { own_effect(e, canvas_effect); }

const ak_el_class ak_class_canvas = {.name = "Canvas", .kind = AK_KIND_CANVAS, .flags = AK_ELF_SIZED, .init = sized_init,
                                     .finalize = canvas_finalize, .drop = sized_drop, .sized = sized};

// ---- Shape ----

static void shape_run(void *ctx) {
	ak_view *e = ctx;
	sized_state *s = e->impl;
	ak_signal_read(&s->size_node);
	if (e->dead || s->w <= 0 || s->h <= 0) return;
	const ak_val *pv = ak_style_get(e, AK_KEY_SHAPE_PATH);
	ak_closure *fn = pv ? pv->p : NULL;
	if (!fn) return;
	ak_retain(fn);
	ak_val args[2] = {{.f = s->w}, {.f = s->h}};
	ak_val r = ak_call(fn, args, 2);
	ak_release(fn);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	if (is_path(r.p)) {
		ak_path_obj *p = r.p;
		send_if_changed(e, s, AK_PROP_SHAPE_PATH, p->b.data, p->b.len);
	}
	ak_release(r.p);
}

static void send_width(ak_view *e) {
	double w = ak_style_get(e, AK_KEY_SHAPE_LINE_WIDTH) ? ak_style_get(e, AK_KEY_SHAPE_LINE_WIDTH)->f : 1;
	ak_batch_set_float(ak_view_batch(e), e->id, AK_PROP_SHAPE_LINE_WIDTH, w > 0 ? w : 1);
}

static void send_dash(ak_view *e) {
	const ak_val *d = ak_style_get(e, AK_KEY_SHAPE_DASH), *ph = ak_style_get(e, AK_KEY_SHAPE_DASH_PHASE);
	const ak_arr *a = d ? d->p : NULL;
	ak_buf *b = ak_batch_set_bytes_begin(ak_view_batch(e), e->id, AK_PROP_SHAPE_DASH);
	if (a && a->len) {
		ak_buf_f32(b, ph ? (float)ph->f : 0);
		for (uint32_t i = 0; i < a->len; i++) ak_buf_f32(b, (float)ak_arr_vals(a)[i].f);
	}
	ak_batch_set_bytes_end(ak_view_batch(e));
}

static void shape_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	ak_batch *b = ak_view_batch(e);
	switch (key) {
	case AK_KEY_SHAPE_FILL:
		if (prev || v.i) ak_batch_set_color(b, e->id, AK_PROP_SHAPE_FILL, (ak_color)v.i);
		break;
	case AK_KEY_SHAPE_STROKE:
		if (prev || v.i) ak_batch_set_color(b, e->id, AK_PROP_SHAPE_STROKE, (ak_color)v.i);
		if (!prev && v.i && e->finalized) send_width(e);
		break;
	case AK_KEY_SHAPE_LINE_WIDTH:
		if (e->finalized && ak_style_int(e, AK_KEY_SHAPE_STROKE, 0)) send_width(e);
		break;
	case AK_KEY_SHAPE_LINE_CAP:
		if (prev || v.i) ak_batch_set_enum(b, e->id, AK_PROP_SHAPE_LINE_CAP, (int32_t)v.i);
		break;
	case AK_KEY_SHAPE_LINE_JOIN:
		if (prev || v.i) ak_batch_set_enum(b, e->id, AK_PROP_SHAPE_LINE_JOIN, (int32_t)v.i);
		break;
	case AK_KEY_SHAPE_TRIM_START: ak_batch_set_float(b, e->id, AK_PROP_SHAPE_TRIM_START, v.f); break;
	case AK_KEY_SHAPE_TRIM_END: ak_batch_set_float(b, e->id, AK_PROP_SHAPE_TRIM_END, v.f); break;
	case AK_KEY_SHAPE_DASH:
	case AK_KEY_SHAPE_DASH_PHASE:
		if (e->finalized) send_dash(e);
		break;
	}
}

static void shape_finalize(ak_view *e) {
	// A stroke's width goes with it; the dash once both its keys are set.
	if (ak_style_int(e, AK_KEY_SHAPE_STROKE, 0)) send_width(e);
	const ak_val *d = ak_style_get(e, AK_KEY_SHAPE_DASH);
	if (d && d->p && ((ak_arr *)d->p)->len) send_dash(e);
	if (ak_style_get(e, AK_KEY_SHAPE_PATH)) own_effect(e, shape_effect);
}

static void shape_effect(void *ctx) { ak_effect_new(shape_run, ak_retain(ctx), AK_EFFECT_RENDER, &shape_info); }

const ak_el_class ak_class_shape = {.name = "Shape", .kind = AK_KIND_SHAPE, .flags = AK_ELF_SIZED, .init = sized_init, .set = shape_set,
                                    .finalize = shape_finalize, .drop = sized_drop, .sized = sized};

// ---- Shader ----

// A compiled shader's uniform table (internal/shadercode's Encode): three
// u32-length strings (Kage, Metal, AGSL), u32 lanes, size, scale,
// time+1, u32 Metal size and per lane a u32 offset, then u32 uniform
// count, per uniform a string name, u32 lane, u32 lanes, u8 kind (0
// float, 1 int, 2 uint, 3 bool).
typedef struct {
	ak_span name;
	uint32_t lane, lanes;
	uint8_t kind;
} shader_uniform;

typedef struct {
	ak_bytes *code;  // retained: the uniforms' names point into it
	uint32_t lanes;
	AK_VEC(shader_uniform) uniforms;
	ak_buf enc;   // the lanes, encoded: reused from change to change
	ak_buf sent;
	bool known;
} shader_state;

static void shader_init(ak_view *e) { e->impl = ak_alloc(sizeof(shader_state)); }

static void shader_drop(ak_view *e) {
	shader_state *s = e->impl;
	ak_release(s->code);
	ak_vec_free(&s->uniforms);
	ak_buf_free(&s->enc);
	ak_buf_free(&s->sent);
	ak_free(s);
}

static void shader_parse(shader_state *s, ak_bytes *code) {
	ak_release(s->code);
	s->code = ak_retain(code);
	s->uniforms.len = 0;
	s->lanes = 0;
	s->known = false;
	if (!code) return;
	ak_reader r = {code->data, code->len, 0, false};
	for (int i = 0; i < 3; i++) ak_read_str(&r);
	uint32_t lanes = ak_read_u32(&r);
	for (int i = 0; i < 3; i++) ak_read_u32(&r);
	ak_read_u32(&r);  // the Metal buffer's size
	if (lanes > 1 << 16) return;
	for (uint32_t i = 0; i < lanes; i++) ak_read_u32(&r);
	uint32_t n = ak_read_u32(&r);
	for (uint32_t i = 0; i < n && !r.err; i++) {
		shader_uniform u = {.name = ak_read_str(&r)};
		u.lane = ak_read_u32(&r);
		u.lanes = ak_read_u32(&r);
		u.kind = ak_read_u8(&r);
		// Its lanes within the shader's (a sum would wrap in 32 bits).
		if (!r.err && u.lane <= lanes && u.lanes <= lanes - u.lane) ak_vec_push(&s->uniforms, u);
	}
	if (!r.err) s->lanes = lanes;
}

static void shader_uniforms(ak_view *e, const ak_arr *us) {
	shader_state *s = e->impl;
	if (!s->code) return;
	ak_buf *b = &s->enc;
	b->len = 0;
	for (uint32_t i = 0; i < s->lanes; i++) ak_buf_u32(b, 0);
	for (uint32_t i = 0; us && i < us->len; i++) {
		const ak_struct *u = ak_arr_vals(us)[i].p;
		if (!u) continue;
		ak_span name = ak_span_str(u->f[0].p);
		const ak_arr *vals = u->f[1].p;
		for (uint32_t k = 0; k < s->uniforms.len; k++) {
			shader_uniform *su = &s->uniforms.data[k];
			if (!ak_span_eq(su->name, name)) continue;
			for (uint32_t j = 0; vals && j < vals->len && j < su->lanes; j++) {
				float v = (float)ak_arr_vals(vals)[j].f;
				uint32_t w;
				switch (su->kind) {
				case 1: w = (uint32_t)(int32_t)v; break;
				case 2: w = v > 0 ? (uint32_t)v : 0; break;
				case 3: w = v != 0; break;
				default: memcpy(&w, &v, 4); break;
				}
				uint8_t *p = b->data + 4 * (su->lane + j);
				for (int q = 0; q < 4; q++) p[q] = (uint8_t)(w >> (8 * q));
			}
		}
	}
	if (s->known && s->sent.len == b->len && memcmp(s->sent.data, b->data, b->len) == 0) return;
	s->known = true;
	s->sent.len = 0;
	ak_buf_put(&s->sent, b->data, b->len);
	ak_batch_set_bytes(ak_view_batch(e), e->id, AK_PROP_SHADER_UNIFORMS, s->sent.data, s->sent.len);
}

static void shader_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	switch (key) {
	case AK_KEY_SHADER_PROGRAM: {
		ak_bytes *code = v.p;
		shader_parse(e->impl, code);
		ak_batch_set_bytes(ak_view_batch(e), e->id, AK_PROP_SHADER_CODE, code ? code->data : NULL, code ? code->len : 0);
		const ak_val *u = ak_style_get(e, AK_KEY_SHADER_UNIFORMS);
		if (u) shader_uniforms(e, u->p);
		break;
	}
	case AK_KEY_SHADER_UNIFORMS: shader_uniforms(e, v.p); break;
	case AK_KEY_SHADER_ANIMATED:
		if (prev || v.i) ak_batch_set_bool(ak_view_batch(e), e->id, AK_PROP_SHADER_ANIMATED, v.i != 0);
		break;
	}
}

const ak_el_class ak_class_shader = {.name = "Shader", .kind = AK_KIND_SHADER, .flags = AK_ELF_CHILDREN, .init = shader_init,
                                     .set = shader_set, .drop = shader_drop};

static const ak_shape *const no_shapes[4];
static const uint8_t uniform_kinds[] = {AK_K_REF, AK_K_REF};
static const char *const uniform_names[] = {"name", "value"};
const ak_shape ak_ui_shape_uniform = {"Uniform", 2, uniform_kinds, no_shapes, uniform_names, 0, NULL, false, NULL};
