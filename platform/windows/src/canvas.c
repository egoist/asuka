// canvas.c: a Canvas's display list, replayed with GDI+ as its view
// paints, in pixels at its window's scale, semantic colors in the theme's
// colors. GDI+ has no layer opacity: Alpha multiplies the colors drawn
// after it. A radial gradient is a path gradient over its circle (its focus
// the path's center point), the last color past it.
#include "host.h"
#include "parts.h"
#include "draw.h"

typedef struct {
	const uint8_t *b;
	size_t n, i;
	float k, alpha;
	bool even_odd;
} reader;

static bool has(reader *r, size_t c) { return r->i + c <= r->n; }

static float f32(reader *r) {
	if (!has(r, 4)) {
		r->i = r->n;
		return 0;
	}
	float v = wh_f32(r->b + r->i);
	r->i += 4;
	return v;
}

static uint32_t u32(reader *r) {
	if (!has(r, 4)) {
		r->i = r->n;
		return 0;
	}
	uint32_t v = wh_u32(r->b + r->i);
	r->i += 4;
	return v;
}

static ARGB color(reader *r) {
	if (!has(r, 8)) {
		r->i = r->n;
		return 0;
	}
	rgba c = {0};
	wh_resolve((ak_color)wh_u64(r->b + r->i), &c);
	r->i += 8;
	return rgba_argb(rgba_alpha(c, r->alpha));
}

static GpPath *path(reader *r) {
	size_t c = u32(r);
	if (c > r->n - r->i) c = r->n - r->i;
	GpPath *p = wh_build_path(r->b + r->i, c, r->k);
	r->i += c;
	if (p) GdipSetPathFillMode(p, r->even_odd ? FillModeAlternate : FillModeWinding);
	return p;
}

typedef struct {
	ARGB *colors;
	float *positions;
	uint32_t n;
} stops;

static stops read_stops(reader *r) {
	uint32_t c = u32(r);
	if (c > (r->n - r->i) / 12) c = (uint32_t)((r->n - r->i) / 12);  // 12 bytes each
	stops s = {wh_alloc((c ? c : 1) * sizeof(ARGB)), wh_alloc((c ? c : 1) * sizeof(float)), c};
	for (uint32_t j = 0; j < c; j++) s.colors[j] = color(r), s.positions[j] = f32(r);
	return s;
}

static void free_stops(stops *s) {
	free(s->colors);
	free(s->positions);
}

static GpBrush *solid(ARGB c) {
	GpSolidFill *b = NULL;
	GdipCreateSolidFill(c, &b);
	return (GpBrush *)b;
}

static GpBrush *linear_brush(float x0, float y0, float x1, float y1, const stops *s) {
	if (s->n < 2) return s->n == 1 ? solid(s->colors[0]) : NULL;
	GpPointF p0 = {x0, y0}, p1 = {x1, y1};
	if (p0.X == p1.X && p0.Y == p1.Y) p1.X += 0.01f;
	GpLineGradient *b = NULL;
	GdipCreateLineBrush(&p0, &p1, s->colors[0], s->colors[s->n - 1], WrapModeTile, &b);
	if (b) GdipSetLinePresetBlend(b, s->colors, s->positions, (INT)s->n);
	return (GpBrush *)b;
}

// radial_brush is a path gradient over the circle: GDI+'s positions run
// from the boundary (0) in to the center (1).
static GpBrush *radial_brush(float cx, float cy, float r, float fx, float fy, const stops *s) {
	if (s->n < 2 || r <= 0) return s->n ? solid(s->colors[s->n - 1]) : NULL;
	GpPath *circle = g_path();
	GdipAddPathEllipse(circle, cx - r, cy - r, 2 * r, 2 * r);
	GpPathGradient *b = NULL;
	GdipCreatePathGradientFromPath(circle, &b);
	GdipDeletePath(circle);
	if (!b) return NULL;
	GpPointF focus = {fx, fy};
	GdipSetPathGradientCenterPoint(b, &focus);
	uint32_t n = s->n;
	ARGB *cs = wh_alloc(n * sizeof *cs);
	float *ps = wh_alloc(n * sizeof *ps);
	for (uint32_t i = 0; i < n; i++) cs[i] = s->colors[n - 1 - i], ps[i] = 1 - s->positions[n - 1 - i];
	ps[0] = 0, ps[n - 1] = 1;
	GdipSetPathGradientPresetBlend(b, cs, ps, (INT)n);
	free(cs);
	free(ps);
	return (GpBrush *)b;
}

// radial_fill fills p with a radial gradient: the last color outside its
// circle, the gradient in it.
static void radial_fill(gfx g, GpPath *p, float cx, float cy, float r, float fx, float fy, const stops *s) {
	if (!s->n) return;
	GraphicsState st;
	GdipSaveGraphics(g, &st);
	GpPath *circle = g_path();
	GdipAddPathEllipse(circle, cx - r, cy - r, 2 * r, 2 * r);
	GdipSetClipPath(g, circle, CombineModeExclude);
	GpBrush *last = solid(s->colors[s->n - 1]);
	GdipFillPath(g, last, p);
	GdipDeleteBrush(last);
	GdipDeletePath(circle);
	GdipRestoreGraphics(g, st);
	GpBrush *br = radial_brush(cx, cy, r, fx, fy, s);
	if (br) {
		GdipFillPath(g, br, p);
		GdipDeleteBrush(br);
	}
}

// pen reads a stroke's style: its width, cap, join and dashes, and makes
// its pen of a color or a brush.
static GpPen *pen(reader *r, ARGB c, GpBrush *br) {
	float w = f32(r);
	if (!has(r, 2)) {
		r->i = r->n;
		return NULL;
	}
	int cap = r->b[r->i], join = r->b[r->i + 1];
	r->i += 2;
	uint32_t nd = u32(r);
	float phase = f32(r);
	if (nd > (r->n - r->i) / 4) nd = (uint32_t)((r->n - r->i) / 4);
	float *dash = wh_alloc((nd ? nd : 1) * sizeof *dash);
	for (uint32_t j = 0; j < nd; j++) dash[j] = f32(r);
	// In world units: its width scales with what the canvas's transform
	// scales (a scaled drawing's strokes as Core Graphics' and a browser
	// canvas's do); a pixel's wouldn't.
	GpPen *p = NULL;
	if (br) GdipCreatePen2(br, w * r->k, UnitWorld, &p);
	else GdipCreatePen1(c, w * r->k, UnitWorld, &p);
	if (p) wh_style_pen(p, cap > 2 ? 2 : cap, join > 2 ? 2 : join, dash, nd, phase, w);
	free(dash);
	return p;
}

// text draws s with its baseline at y, aligned about x (0 start, 1
// center, 2 end).
static void text(gfx g, const char *s, size_t len, float size, float k, bool bold, ARGB c, int align, float x, float y) {
	wchar_t buf[LF_FACESIZE];
	font_spec spec = {.size = size};
	const wchar_t *face = wh_face(&spec, buf, LF_FACESIZE);
	GpFontFamily *fam = NULL;
	GdipCreateFontFamilyFromName(face, NULL, &fam);
	if (!fam) return;
	INT style = bold ? FontStyleBold : FontStyleRegular;
	GpFont *font = NULL;
	GdipCreateFont(fam, size * k, style, UnitPixel, &font);
	if (!font) {
		GdipDeleteFontFamily(fam);
		return;
	}
	UINT16 ascent = 0, em = 0;
	GdipGetCellAscent(fam, style, &ascent);
	GdipGetEmHeight(fam, style, &em);
	float top = em ? y - size * k * (float)ascent / (float)em : y;
	GpStringFormat *format = NULL;
	GdipCreateStringFormat(StringFormatFlagsNoWrap, LANG_NEUTRAL, &format);
	int n = 0;
	wchar_t *w = wh_wide(s, (int)len);
	if (w) n = (int)wcslen(w);
	if (format && n) {
		GdipSetStringFormatAlign(format, (StringAlignment)align);
		GpBrush *b = solid(c);
		RectF rect = {x, top, 0, 0};  // no box: aligned around x
		GdipDrawString(g, w, n, font, &rect, format, b);
		GdipDeleteBrush(b);
	}
	free(w);
	if (format) GdipDeleteStringFormat(format);
	GdipDeleteFont(font);
	GdipDeleteFontFamily(fam);
}

enum { MAX_SAVED = 64 };

void wh_paint_canvas(node *n, gfx g) {
	if (!n->canvas || !n->canvas_len) return;
	reader r = {n->canvas, n->canvas_len, 0, wh_scale_of(n), 1, false};
	float k = r.k;
	GraphicsState saved[MAX_SAVED];
	float alphas[MAX_SAVED];
	bool rules[MAX_SAVED];
	int nsaved = 0, depth = 0;  // depth: saves past MAX_SAVED, restored as nothing
	while (r.i < r.n) {
		uint8_t op = r.b[r.i++];
		switch (op) {
		case AK_CANVAS_OP_SAVE:
			if (nsaved < MAX_SAVED) {
				GdipSaveGraphics(g, &saved[nsaved]);
				alphas[nsaved] = r.alpha, rules[nsaved] = r.even_odd;
				nsaved++;
			} else {
				depth++;
			}
			break;
		case AK_CANVAS_OP_RESTORE:
			if (depth) {
				depth--;
			} else if (nsaved > 0) {
				nsaved--;
				GdipRestoreGraphics(g, saved[nsaved]);
				r.alpha = alphas[nsaved], r.even_odd = rules[nsaved];
			}
			break;
		case AK_CANVAS_OP_TRANSFORM: {
			// In points; the world is in pixels: the translation scales.
			float a = f32(&r), b = f32(&r), c = f32(&r), d = f32(&r), tx = f32(&r), ty = f32(&r);
			GpMatrix *m = NULL;
			GdipCreateMatrix2(a, b, c, d, tx * k, ty * k, &m);
			if (m) {
				GdipMultiplyWorldTransform(g, m, MatrixOrderPrepend);
				GdipDeleteMatrix(m);
			}
			break;
		}
		case AK_CANVAS_OP_FILL_RULE:
			if (has(&r, 1)) r.even_odd = r.b[r.i++] == 1;
			break;
		case AK_CANVAS_OP_RADIAL_GRADIENT: {
			stops s = read_stops(&r);
			float cx = f32(&r) * k, cy = f32(&r) * k, rad = f32(&r) * k, fx = f32(&r) * k, fy = f32(&r) * k;
			GpPath *p = path(&r);
			if (p) {
				radial_fill(g, p, cx, cy, rad, fx, fy, &s);
				GdipDeletePath(p);
			}
			free_stops(&s);
			break;
		}
		case AK_CANVAS_OP_STROKE_GRADIENT: {
			size_t start = r.i;
			// The pen's brush comes after its style: read past it first.
			f32(&r);
			r.i += 2;
			uint32_t nd = u32(&r);
			r.i += 4 + 4 * (size_t)nd;
			if (!has(&r, 1)) return;
			bool radial = r.b[r.i++] == 1;
			stops s = read_stops(&r);
			GpBrush *br;
			if (radial) {
				float cx = f32(&r) * k, cy = f32(&r) * k, rad = f32(&r) * k, fx = f32(&r) * k, fy = f32(&r) * k;
				br = radial_brush(cx, cy, rad, fx, fy, &s);
			} else {
				float x0 = f32(&r) * k, y0 = f32(&r) * k, x1 = f32(&r) * k, y1 = f32(&r) * k;
				br = linear_brush(x0, y0, x1, y1, &s);
			}
			free_stops(&s);
			size_t end = r.i;
			r.i = start;
			GpPen *pn = br ? pen(&r, 0, br) : NULL;
			r.i = end;
			GpPath *p = path(&r);
			if (pn && p) GdipDrawPath(g, pn, p);
			if (pn) GdipDeletePen(pn);
			if (br) GdipDeleteBrush(br);
			if (p) GdipDeletePath(p);
			break;
		}
		case AK_CANVAS_OP_TRANSLATE: {
			float x = f32(&r), y = f32(&r);
			GdipTranslateWorldTransform(g, x * k, y * k, MatrixOrderPrepend);
			break;
		}
		case AK_CANVAS_OP_SCALE: {
			float x = f32(&r), y = f32(&r);
			GdipScaleWorldTransform(g, x, y, MatrixOrderPrepend);
			break;
		}
		case AK_CANVAS_OP_ROTATE:
			GdipRotateWorldTransform(g, f32(&r), MatrixOrderPrepend);
			break;
		case AK_CANVAS_OP_ALPHA:
			r.alpha *= f32(&r);
			break;
		case AK_CANVAS_OP_CLIP: {
			GpPath *p = path(&r);
			if (p) {
				GdipSetClipPath(g, p, CombineModeIntersect);
				GdipDeletePath(p);
			}
			break;
		}
		case AK_CANVAS_OP_FILL: {
			ARGB c = color(&r);
			GpPath *p = path(&r);
			if (p) {
				GpBrush *br = solid(c);
				GdipFillPath(g, br, p);
				GdipDeleteBrush(br);
				GdipDeletePath(p);
			}
			break;
		}
		case AK_CANVAS_OP_STROKE: {
			ARGB c = color(&r);
			GpPen *pn = pen(&r, c, NULL);
			GpPath *p = path(&r);
			if (pn && p) GdipDrawPath(g, pn, p);
			if (pn) GdipDeletePen(pn);
			if (p) GdipDeletePath(p);
			break;
		}
		case AK_CANVAS_OP_GRADIENT: {
			stops s = read_stops(&r);
			float x0 = f32(&r) * k, y0 = f32(&r) * k, x1 = f32(&r) * k, y1 = f32(&r) * k;
			GpPath *p = path(&r);
			if (p && s.n) {
				GpBrush *br = linear_brush(x0, y0, x1, y1, &s);
				if (br) {
					GdipFillPath(g, br, p);
					GdipDeleteBrush(br);
				}
			}
			if (p) GdipDeletePath(p);
			free_stops(&s);
			break;
		}
		case AK_CANVAS_OP_TEXT: {
			ARGB c = color(&r);
			float size = f32(&r);
			if (!has(&r, 2)) goto done;
			int weight = r.b[r.i], align = r.b[r.i + 1];
			r.i += 2;
			float x = f32(&r), y = f32(&r);
			size_t l = u32(&r);
			if (l > r.n - r.i) l = r.n - r.i;
			const char *s = (const char *)r.b + r.i;
			r.i += l;
			if (align > 2) align = 0;
			text(g, s, l, size, k, weight >= AK_FONT_WEIGHT_SEMIBOLD, c, align, x * k, y * k);
			break;
		}
		default:
			goto done;
		}
	}
done:
	while (nsaved > 0) GdipRestoreGraphics(g, saved[--nsaved]);
}
