// paint.c: every view of the host's own class draws itself into a
// buffered DC (no flicker): first what's behind it where it's not opaque
// (its parent, and the siblings under it, drawn again into the DC), then
// its background, border and content.
#include "host.h"
#include "parts.h"

void wh_paint_window(node *n, HWND hwnd) {
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hwnd, &ps);
	RECT r;
	GetClientRect(hwnd, &r);
	if (r.right > 0 && r.bottom > 0) {
		HDC mem = NULL;
		HPAINTBUFFER pb = BeginBufferedPaint(dc, &r, BPBF_COMPATIBLEBITMAP, NULL, &mem);
		if (pb && mem) {
			wh_paint(n, hwnd, mem, r);
			EndBufferedPaint(pb, TRUE);
		} else {
			wh_paint(n, hwnd, dc, r);
		}
	}
	EndPaint(hwnd, &ps);
	wh_corners_painted(hwnd);
	wh_pictures_stale(hwnd);
	wh_materials_behind(hwnd);
}

// wh_own_alpha is how much n's own drawing fades: its opacity, unless a
// picture fades it with what's in it.
float wh_own_alpha(const node *n) { return n->pict ? 1 : n->opacity; }

bool wh_round(const node *n) {
	return n->corner_radius > 0 || n->corner_radii[0] > 0 || n->corner_radii[1] > 0 || n->corner_radii[2] > 0 || n->corner_radii[3] > 0;
}

bool wh_uniform_radii(const float r[4]) { return r[0] == r[1] && r[1] == r[2] && r[2] == r[3]; }

void wh_radii(const node *n, float w, float h, float r[4]) {
	float k = wh_scale_of(n);
	bool per = n->corner_radii[0] > 0 || n->corner_radii[1] > 0 || n->corner_radii[2] > 0 || n->corner_radii[3] > 0;
	for (int i = 0; i < 4; i++) r[i] = wh_maxf((per ? n->corner_radii[i] : n->corner_radius) * k, 0);
	// CSS's: when two radii on a side are more than it, every radius is
	// scaled down by the same factor, the least the sides need (one
	// radius of 999: half the shorter side, a capsule).
	float f = 1;
	const float side[4] = {w, h, w, h};
	for (int i = 0; i < 4; i++) {
		float sum = r[i] + r[(i + 1) % 4];
		if (sum > side[i] && sum > 0) f = wh_minf(f, wh_maxf(side[i], 0) / sum);
	}
	if (f < 1)
		for (int i = 0; i < 4; i++) r[i] *= f;
}

bool wh_has_border(const node *n) {
	return n->border_width > 0 || n->border_widths[0] > 0 || n->border_widths[1] > 0 || n->border_widths[2] > 0 || n->border_widths[3] > 0;
}

bool wh_opaque(const node *n) {
	if (n->material) return false;  // it blurs what's behind it
	switch (n->kind) {
	case AK_KIND_SCREEN: case AK_KIND_SHEET: case AK_KIND_WINDOW: case KIND_HEADER: case KIND_TAB_BAR:
	case KIND_SIDEBAR: case KIND_COMMAND_BAR: case KIND_ALERT_WINDOW:
		return !wh_round(n);
	}
	rgba c;
	return wh_resolve(n->background, &c) && c.a == 255 && !wh_round(n) && wh_own_alpha(n) >= 1;
}

void wh_paint(node *n, HWND hwnd, HDC dc, RECT r) {
	if (!wh_opaque(n)) wh_paint_behind(hwnd, dc);
	wh_paint_own(n, hwnd, dc, r);
}

// ---- Shadows ----

// shadow_steps are the rounded rectangles that draw a shadow of alpha a
// blurred by a Gaussian of σ sigma (pixels): at a distance d past the
// shape's edge, a·½erfc(d/(σ√2)), from 3σ outside to 3σ inside, the
// outermost first; overlapping, their alphas compound to the Gaussian's
// in each step's middle.
int wh_shadow_steps(double sigma, double a, shadow_step out[MAX_SHADOW_STEPS]) {
	int k = 0;
	a = a < 0.999 ? a : 0.999;
	if (a <= 0) return 0;
	if (sigma <= 0.25) {
		out[0] = (shadow_step){0, a};  // no blur to speak of
		return 1;
	}
	int n = (int)fmin(fmax(ceil(6 * sigma), 2), MAX_SHADOW_STEPS);
	double step = 6 * sigma / n, prev = 0;
	for (int i = 0; i < n; i++) {
		double spread = 3 * sigma - i * step;
		double want = a * 0.5 * erfc((spread - step / 2) / (sigma * M_SQRT2));
		if (i == n - 1) want = a;  // the innermost: as dark as the shadow gets
		if (want <= prev) continue;
		out[k++] = (shadow_step){spread, 1 - (1 - want) / (1 - prev)};
		prev = want;
	}
	return k;
}

typedef struct {
	ak_color color;
	float blur, dx, dy;
} shadow_spec;

static bool read_shadow(const node *n, shadow_spec *s) {
	const uint8_t *b = n->shadow;
	if (n->shadow_len < 20) {
		if (!n->has_text_shadow) return false;
		b = n->text_shadow;  // a text's, under its glyphs
	}
	s->color = wh_u64(b), s->blur = wh_f32(b + 8), s->dx = wh_f32(b + 12), s->dy = wh_f32(b + 16);
	return s->color != 0;
}

RECT wh_rect_in(HWND hwnd, HWND parent) {
	RECT r;
	GetWindowRect(hwnd, &r);
	MapWindowPoints(HWND_DESKTOP, parent, (POINT *)&r, 2);
	return r;
}

// paint_shadows draws the shadows of n's children whose windows are in
// hwnd, under them.
static void paint_shadows(node *n, HWND hwnd, gfx g) {
	shadow_step steps[MAX_SHADOW_STEPS];
	for (uint32_t i = 0; i < n->nkids; i++) {
		node *c = n->kids[i];
		shadow_spec s;
		rgba col;
		if (!c->shadow_len || !c->hwnd || c->hidden || GetParent(c->hwnd) != hwnd || !IsWindowVisible(c->hwnd)) continue;
		if (!read_shadow(c, &s) || !wh_resolve(s.color, &col) || col.a == 0) continue;
		float k = wh_scale_of(n);
		RECT r = wh_rect_in(c->hwnd, hwnd);
		float x = (float)r.left + s.dx * k, y = (float)r.top + s.dy * k;
		float w = (float)(r.right - r.left), ht = (float)(r.bottom - r.top);
		float radii[4];
		wh_radii(c, w, ht, radii);
		// Radius is a blur radius, as CSS and SwiftUI mean it: 2σ.
		double a = (double)col.a / 255 * c->opacity;
		int ns = wh_shadow_steps((double)(s.blur * k) / 2, a, steps);
		for (int j = 0; j < ns; j++) {
			float spread = wh_maxf((float)steps[j].spread, -wh_minf(w, ht) / 2);
			rgba lc = col;
			lc.a = (uint8_t)lround(steps[j].alpha * 255);
			if (!lc.a) continue;
			float rs[4];
			for (int q = 0; q < 4; q++) rs[q] = radii[q] > 0 ? wh_maxf(radii[q] + spread, 0) : 0;
			g_fill_corners(g, rgba_argb(lc), x - spread, y - spread, w + 2 * spread, ht + 2 * spread, rs);
		}
	}
	// Texts' shadows, under their glyphs: in the parent, so that they
	// reach past the text's window.
	for (uint32_t i = 0; i < n->nkids; i++) {
		node *c = n->kids[i];
		if (!c->has_text_shadow || c->kind != AK_KIND_TEXT || !c->hwnd || c->hidden || GetParent(c->hwnd) != hwnd || !IsWindowVisible(c->hwnd)) continue;
		RECT r = wh_rect_in(c->hwnd, hwnd);
		wh_paint_text_shadow(c, g, (float)r.left, (float)r.top);
	}
}

// wh_invalidate_shadow repaints the parent where n's shadow is.
void wh_invalidate_shadow(node *n) {
	shadow_spec s;
	if (!read_shadow(n, &s) || !n->hwnd || !GetParent(n->hwnd)) return;
	float k = wh_scale_of(n);
	RECT r = wh_rect_in(n->hwnd, GetParent(n->hwnd));
	// 3σ, Radius being 2σ.
	int pad = (int)ceilf((1.5f * s.blur + wh_maxf(fabsf(s.dx), fabsf(s.dy))) * k);
	RECT a = {r.left - pad, r.top - pad, r.right + pad, r.bottom + pad};
	InvalidateRect(GetParent(n->hwnd), &a, FALSE);
}

// ---- A view's own drawing ----

// stop_color is the gradient's color at t (0 to 1), its stops (count of
// them, at b + 4) mixed.
static rgba stop_color(const uint8_t *b, uint32_t count, float t) {
	rgba prev = {0}, c = {0};
	float pt = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t o = 4 + i * 12;
		wh_resolve(wh_u64(b + o), &c);
		float at = wh_clampf(wh_f32(b + o + 8), 0, 1);
		if (i == 0 && t <= at) return c;
		if (t <= at) {
			float f = at > pt ? (t - pt) / (at - pt) : 1;
			rgba m = {(uint8_t)lroundf(prev.r + (c.r - prev.r) * f), (uint8_t)lroundf(prev.g + (c.g - prev.g) * f),
			          (uint8_t)lroundf(prev.b + (c.b - prev.b) * f), (uint8_t)lroundf(prev.a + (c.a - prev.a) * f)};
			return m;
		}
		prev = c, pt = at;
	}
	return c;
}

// conic_picture draws a conic gradient into a w by h bitmap: a wedge a
// degree wide for each degree, from angle (clockwise from the top) round
// (cx, cy). GDI+ has no conic brush; the bitmap fills the view's shape
// as a texture, so its edge is antialiased as any fill's.
static GpBitmap *conic_picture(const uint8_t *b, uint32_t count, int w, int h, float cx, float cy, float angle) {
	GpBitmap *bmp = NULL;
	GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppPARGB, NULL, &bmp);
	if (!bmp) return NULL;
	GpGraphics *g = NULL;
	GdipGetImageGraphicsContext((GpImage *)bmp, &g);
	if (!g) return bmp;
	GdipSetSmoothingMode(g, SmoothingModeNone);
	float reach = 0;  // its farthest corner
	const float xs[4] = {0, (float)w, (float)w, 0}, ys[4] = {0, 0, (float)h, (float)h};
	for (int i = 0; i < 4; i++) reach = wh_maxf(reach, hypotf(xs[i] - cx, ys[i] - cy));
	reach += 2;
	GpSolidFill *br = NULL;
	GdipCreateSolidFill(0, &br);
	for (int d = 0; d < 360; d++) {
		rgba c = stop_color(b, count, ((float)d + 0.5f) / 360);
		GdipSetSolidFillColor(br, rgba_argb(c));
		// GDI+'s angles are clockwise from the right; each wedge a little
		// wider, over the next one's edge.
		GdipFillPie(g, (GpBrush *)br, cx - reach, cy - reach, 2 * reach, 2 * reach, angle - 90 + (float)d, 1.6f);
	}
	GdipDeleteBrush((GpBrush *)br);
	GdipDeleteGraphics(g);
	return bmp;
}

static void paint_gradient(gfx g, node *n, float w, float ht, const float radii[4]) {
	const uint8_t *b = n->gradient;
	if (n->gradient_len < 4) return;
	uint32_t count = wh_u32(b);
	if (count == 0 || n->gradient_len < 4 + count * 12 + 16) return;
	ARGB colors[66];
	REAL pos[66];
	uint32_t m = 0;
	if (count > 64) count = 64;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t o = 4 + i * 12;
		rgba c = {0};
		wh_resolve(wh_u64(b + o), &c);
		colors[m + 1] = rgba_argb(c);
		pos[m + 1] = wh_clampf(wh_f32(b + o + 8), 0, 1);
		m++;
	}
	// GDI+ wants the stops to start at 0 and end at 1.
	ARGB *cs = colors + 1;
	REAL *ps = pos + 1;
	if (ps[0] > 0) {
		cs--, ps--;
		cs[0] = cs[1], ps[0] = 0;
		m++;
	}
	if (ps[m - 1] < 1) {
		cs[m] = cs[m - 1], ps[m] = 1;
		m++;
	}
	uint32_t o = 4 + count * 12;
	// After the line: its kind, center, radius and angle (none: linear).
	uint8_t kind = AK_GRADIENT_KIND_LINEAR;
	float cx = 0.5f * w, cy = 0.5f * ht, rad = 0.5f, angle = 0;
	if (n->gradient_len >= o + 16 + 17) {
		const uint8_t *e = b + o + 16;
		kind = e[0];
		cx = wh_f32(e + 1) * w, cy = wh_f32(e + 5) * ht, rad = wh_f32(e + 9), angle = wh_f32(e + 13);
	}
	GpPath *p = g_path();
	g_path_corners(p, 0, 0, w, ht, radii);
	if (kind == AK_GRADIENT_KIND_RADIAL) {
		// A circle of the radius (a fraction of the shorter side): GDI+'s
		// path gradient on it, its blend from the edge (0) to the center
		// (1); past it, the last stop's color, as CSS's.
		float r = wh_maxf(rad * wh_minf(w, ht), 0.5f);
		GpSolidFill *last = NULL;
		GdipCreateSolidFill(cs[m - 1], &last);
		GdipFillPath(g, (GpBrush *)last, p);
		GdipDeleteBrush((GpBrush *)last);
		GpPath *circle = g_path();
		GdipAddPathEllipse(circle, cx - r, cy - r, 2 * r, 2 * r);
		GpPathGradient *pg = NULL;
		GdipCreatePathGradientFromPath(circle, &pg);
		if (pg) {
			ARGB rc[66];
			REAL rp[66];
			for (uint32_t i = 0; i < m; i++) rc[i] = cs[m - 1 - i], rp[i] = 1 - ps[m - 1 - i];
			GpPointF c = {cx, cy};
			GdipSetPathGradientCenterPoint(pg, &c);
			GdipSetPathGradientPresetBlend(pg, rc, rp, (INT)m);
			g_clip_path(g, p);
			GdipFillPath(g, (GpBrush *)pg, circle);
			g_reset_clip(g);
			GdipDeleteBrush((GpBrush *)pg);
		}
		GdipDeletePath(circle);
	} else if (kind == AK_GRADIENT_KIND_CONIC) {
		int iw = (int)ceilf(w), ih = (int)ceilf(ht);
		if (!n->conic || n->conic_w != iw || n->conic_h != ih) {
			if (n->conic) GdipDisposeImage((GpImage *)n->conic);
			n->conic = iw > 0 && ih > 0 ? conic_picture(b, count, iw, ih, cx, cy, angle) : NULL;
			n->conic_w = iw, n->conic_h = ih;
		}
		GpTexture *t = NULL;
		if (n->conic) GdipCreateTexture((GpImage *)n->conic, WrapModeClamp, &t);
		if (t) {
			GdipFillPath(g, (GpBrush *)t, p);
			GdipDeleteBrush((GpBrush *)t);
		}
	} else {
		GpPointF p0 = {wh_f32(b + o) * w, wh_f32(b + o + 4) * ht}, p1 = {wh_f32(b + o + 8) * w, wh_f32(b + o + 12) * ht};
		if (p0.X == p1.X && p0.Y == p1.Y) p1.X += 0.01f;
		GpLineGradient *br = NULL;
		GdipCreateLineBrush(&p0, &p1, cs[0], cs[m - 1], WrapModeTile, &br);
		if (br) {
			GdipSetLinePresetBlend(br, cs, ps, (INT)m);
			GdipFillPath(g, (GpBrush *)br, p);
			GdipDeleteBrush((GpBrush *)br);
		}
	}
	GdipDeletePath(p);
}

static void dash_pen(GpPen *pen, int32_t style) {
	if (style == AK_BORDER_STYLE_DASHED) {
		REAL dash[2] = {3, 3};  // in widths: three on, three off
		GdipSetPenDashArray(pen, dash, 2);
	} else if (style == AK_BORDER_STYLE_DOTTED) {
		// Round dots as wide as the line, a width apart: dashes of almost
		// nothing, their round caps the dots.
		REAL dash[2] = {0.001f, 1.999f};
		GdipSetPenDashCap197819(pen, DashCapRound);
		GdipSetPenDashArray(pen, dash, 2);
	}
}

// paint_border draws n's border inside its w by ht box of corner radii
// radii: one width all round, stroked along the middle of its width
// (dashed or dotted with its style); a width a side, each side its own:
// solid, the ring between the box and its inside, whose corners are the
// outer ones less the sides' widths (CSS's); dashed or dotted, each
// side's line.
static void paint_border(gfx g, node *n, float w, float ht, const float radii[4]) {
	float k = wh_scale_of(n);
	bool per = n->border_widths[0] > 0 || n->border_widths[1] > 0 || n->border_widths[2] > 0 || n->border_widths[3] > 0;
	float bw[4];
	for (int i = 0; i < 4; i++) bw[i] = wh_maxf((per ? n->border_widths[i] : n->border_width) * k, 0);
	if (bw[0] <= 0 && bw[1] <= 0 && bw[2] <= 0 && bw[3] <= 0) return;
	rgba bc;
	if (!wh_resolve(n->border_color, &bc)) bc = H.theme.label;
	ARGB c = rgba_argb(bc);
	int32_t style = n->border_style;
	GpPen *pen = NULL;
	if (bw[0] == bw[1] && bw[1] == bw[2] && bw[2] == bw[3]) {
		float width = bw[0], ri[4];
		for (int i = 0; i < 4; i++) ri[i] = radii[i] > 0 ? wh_maxf(radii[i] - width / 2, 0) : 0;
		GpPath *p = g_path();
		g_path_corners(p, width / 2, width / 2, w - width, ht - width, ri);
		GdipCreatePen1(c, width, UnitPixel, &pen);
		dash_pen(pen, style);
		GdipDrawPath(g, pen, p);
		GdipDeletePen(pen);
		GdipDeletePath(p);
		return;
	}
	if (style != AK_BORDER_STYLE_SOLID) {
		const float x0[4] = {0, w, w, 0}, y0[4] = {0, 0, ht, ht}, x1[4] = {w, w, 0, 0}, y1[4] = {0, ht, ht, 0};
		const float nx[4] = {0, -1, 0, 1}, ny[4] = {1, 0, -1, 0};  // inward
		for (int i = 0; i < 4; i++) {
			if (bw[i] <= 0) continue;
			float off = bw[i] / 2;
			GdipCreatePen1(c, bw[i], UnitPixel, &pen);
			dash_pen(pen, style);
			GdipDrawLine(g, pen, x0[i] + nx[i] * off, y0[i] + ny[i] * off, x1[i] + nx[i] * off, y1[i] + ny[i] * off);
			GdipDeletePen(pen);
		}
		return;
	}
	// Each corner's inner ellipse: its radius less the widths of the sides
	// it joins (top left: left across, top down).
	float rx[4], ry[4];
	const int hs[4] = {3, 1, 1, 3}, vs[4] = {0, 0, 2, 2};
	for (int i = 0; i < 4; i++) rx[i] = wh_maxf(radii[i] - bw[hs[i]], 0), ry[i] = wh_maxf(radii[i] - bw[vs[i]], 0);
	GpPath *p = g_path();
	g_path_corners(p, 0, 0, w, ht, radii);
	float iw = w - bw[1] - bw[3], ih = ht - bw[0] - bw[2];
	if (iw > 0 && ih > 0) g_path_rrect(p, bw[3], bw[0], iw, ih, rx, ry);
	GpSolidFill *br = NULL;
	GdipCreateSolidFill(c, &br);
	GdipFillPath(g, (GpBrush *)br, p);  // alternate fill: the ring between them
	GdipDeleteBrush((GpBrush *)br);
	GdipDeletePath(p);
}

// The progress bar: Fluent's hairline track and a 3-point fill in the
// accent color (Color sets it); indeterminate, a segment that runs along
// it.
static void paint_progress(node *n, gfx g, float w, float ht) {
	float k = wh_scale_of(n);
	rgba fill;
	if (!wh_resolve(n->color, &fill)) fill = H.theme.accent;
	float th = 3 * k, y = (ht - th) / 2;
	g_draw_line(g, rgba_argb(H.theme.track), wh_maxf(k, 1), 0, ht / 2, w, ht / 2);
	if (n->indeterminate) {
		// A segment 40% of the track, moving across and back in.
		float t = (float)fmod(n->phase, 1), seg = w * 0.4f, x = -seg + t * (w + seg);
		if (wh_is_rtl(n)) x = w - x - seg;  // it runs from the right
		float x0 = wh_maxf(x, 0), x1 = wh_minf(x + seg, w);
		if (x1 > x0) g_fill_round_rect(g, rgba_argb(fill), x0, y, x1 - x0, th, th / 2);
		return;
	}
	float v = (float)wh_clampd(n->value, 0, 1);
	if (v > 0) {
		float fw = wh_maxf(w * v, th), x = wh_is_rtl(n) ? w - fw : 0;
		g_fill_round_rect(g, rgba_argb(fill), x, y, fw, th, th / 2);
	}
}

// The activity ring: Fluent's progress ring, an arc that turns and grows.
static void paint_activity(node *n, gfx g, float w, float ht) {
	float k = wh_scale_of(n);
	rgba c;
	if (!wh_resolve(n->color, &c)) c = H.theme.accent;
	// Inside its padding (a refresh's spinner on its disc).
	float ox = n->inset[3] * k, oy = n->inset[0] * k;
	w = wh_maxf(w - (n->inset[1] + n->inset[3]) * k, 1), ht = wh_maxf(ht - (n->inset[0] + n->inset[2]) * k, 1);
	float d = wh_minf(w, ht), stroke = wh_maxf(d / 8, 2 * k);
	double t = n->phase;
	float start = (float)fmod(t * 360 * 1.5, 360);
	float sweep = (float)(60 + 200 * (0.5 - 0.5 * cos(t * M_PI)));
	float x = ox + (w - d) / 2 + stroke / 2, y = oy + (ht - d) / 2 + stroke / 2;
	g_draw_arc(g, rgba_argb(c), stroke, LineCapRound, x, y, d - stroke, d - stroke, start, sweep);
}

// The progress ring (WinUI's): determinate, an arc in the accent color
// from the top, clockwise, as far as its value, with no track;
// indeterminate, the activity's arc. Its stroke is an eighth of its size.
static void paint_progress_ring(node *n, gfx g, float w, float ht) {
	if (n->indeterminate) {
		paint_activity(n, g, w, ht);
		return;
	}
	float v = (float)wh_clampd(n->shown, 0, 1);
	if (v <= 0) return;
	float k = wh_scale_of(n);
	rgba c;
	if (!wh_resolve(n->color, &c)) c = H.theme.accent;
	float d = wh_minf(w, ht), stroke = wh_maxf(d / 8, 2 * k);
	float x = (w - d) / 2 + stroke / 2, y = (ht - d) / 2 + stroke / 2;
	g_draw_arc(g, rgba_argb(c), stroke, LineCapRound, x, y, d - stroke, d - stroke, -90, 360 * v);
}

UINT wh_text_direction(bool rtl) { return rtl ? DT_RTLREADING : 0; }
UINT wh_start_side(bool rtl) { return rtl ? DT_RIGHT | DT_RTLREADING : DT_LEFT; }

// wh_aligned_right reports whether an alignment puts text on the right in
// a direction: End on the side the direction ends on, Center on neither,
// Start (and Justify) on the side it starts on.
bool wh_aligned_right(int32_t a, bool rtl) {
	if (a == AK_TEXT_ALIGN_END) return !rtl;
	if (a == AK_TEXT_ALIGN_CENTER) return false;
	return rtl;
}

UINT wh_text_format(const node *n) {
	bool rtl = wh_is_rtl(n);
	UINT f = DT_NOPREFIX | DT_EDITCONTROL | wh_text_direction(wh_reads_rtl(n));
	if (n->text_align == AK_TEXT_ALIGN_CENTER) f |= DT_CENTER;
	else if (wh_aligned_right(n->text_align, rtl)) f |= DT_RIGHT;
	if (n->line_limit == 1) return f | DT_SINGLELINE | DT_END_ELLIPSIS;
	f |= DT_WORDBREAK;
	if (n->line_limit > 1) f |= DT_END_ELLIPSIS;
	return f;
}

// wh_inset_rect is r, the node's client rect, less its padding
// (ContentInset). Its inner edges are rounded to pixels as its frame's
// edges are (wh_place_at), not each side's width alone: at 150%, 5pt is
// 7.5px, 8 a side, 16 in all, where the frame (the text's width and 10pt,
// rounded as one) has 15, and the text, a pixel short of what it
// measured, would wrap. Transformed, or not placed yet: each side alone.
RECT wh_inset_rect(const node *n, RECT r) {
	bool xf = n->xf.tx != 0 || n->xf.ty != 0 || n->xf.sx != 1 || n->xf.sy != 1 || n->xf.rot != 0;
	if (n->placed && !xf && n->frame[2] > 0 && n->frame[3] > 0) {
		float x = n->frame[0], y = n->frame[1];
		if (n->parent && n->parent->kind == AK_KIND_SCROLL) x -= n->parent->scroll_x, y -= n->parent->scroll_y;
		int x0 = wh_px(n, x), y0 = wh_px(n, y), x1 = wh_px(n, x + n->frame[2]), y1 = wh_px(n, y + n->frame[3]);
		r.top += wh_px(n, y + n->inset[0]) - y0;
		r.right -= x1 - wh_px(n, x + n->frame[2] - n->inset[1]);
		r.bottom -= y1 - wh_px(n, y + n->frame[3] - n->inset[2]);
		r.left += wh_px(n, x + n->inset[3]) - x0;
	} else {
		r.top += wh_px(n, n->inset[0]);
		r.right -= wh_px(n, n->inset[1]);
		r.bottom -= wh_px(n, n->inset[2]);
		r.left += wh_px(n, n->inset[3]);
	}
	if (r.right < r.left) r.right = r.left;
	if (r.bottom < r.top) r.bottom = r.top;
	return r;
}

// wh_draw_text draws a Text in its client rect r, in color: DrawText's
// lines, or its own (textlay.c) when its style asks for what DrawText
// can't do.
void wh_draw_text(node *n, HDC dc, RECT r, COLORREF color) {
	if (!n->text.n) return;
	if (wh_styled_text(n)) {
		wh_styled_paint(n, dc, r, color);
		return;
	}
	r = wh_inset_rect(n, r);
	HGDIOBJ old = SelectObject(dc, wh_node_font(n));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, color);
	if (n->line_limit > 1) {
		TEXTMETRICW tm;
		GetTextMetricsW(dc, &tm);
		r.bottom = wh_mini(r.bottom, r.top + tm.tmHeight * n->line_limit);
	}
	DrawTextW(dc, n->text.p, n->text.n, &r, wh_text_format(n));
	SelectObject(dc, old);
}

// paint_symbol draws a symbol's glyph of Segoe Fluent Icons, centered.
static void paint_symbol(node *n, HDC dc, RECT r) {
	wchar_t glyph = wh_directed_glyph(n->symbol, wh_is_rtl(n));
	if (!glyph) return;
	font_spec s = wh_node_spec(n);
	HGDIOBJ old = SelectObject(dc, wh_icon_font(n, s.size));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, rgba_ref(wh_text_color(n, H.theme.label)));
	DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old);
}

void wh_focus_ring(const node *n, gfx g, float w, float h, float radius) {
	float k = wh_scale_of(n);
	g_stroke_round_rect(g, rgba_argb(H.theme.label), 2 * k, 0, 0, w, h, wh_maxf(radius, 4 * k));
}

bool wh_keyboard_cues(HWND hwnd) { return (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0; }

static bool window_kind(const node *n) {
	return n->kind == AK_KIND_SCREEN || n->kind == AK_KIND_SHEET || n->kind == AK_KIND_WINDOW || n->kind == KIND_HEADER ||
		n->kind == KIND_TAB_BAR || n->kind == KIND_SIDEBAR || n->kind == KIND_COMMAND_BAR;
}

void wh_paint_own(node *n, HWND hwnd, HDC dc, RECT r) {
	float w = (float)(r.right - r.left), ht = (float)(r.bottom - r.top);
	if (n->material) wh_paint_material(n, dc, r.right - r.left, r.bottom - r.top);
	// GDI+ draws first, GDI's text after its Graphics is closed: GDI draws
	// at the DC's origin while one is open, not at the origin paint_behind
	// moved it to.
	gfx g = g_new(dc);
	float radii[4];
	wh_radii(n, w, ht, radii);
	float radius = wh_maxf(wh_maxf(radii[0], radii[1]), wh_maxf(radii[2], radii[3]));  // what takes one: a focus ring, a clip's test
	rgba bg;
	bool has_bg = wh_resolve(n->background, &bg);
	if (!has_bg && window_kind(n)) bg = wh_bg_in(hwnd), has_bg = true;  // its window's
	float a = wh_own_alpha(n);
	if (a < 1) bg = rgba_alpha(bg, a);
	if (n->gradient) {
		paint_gradient(g, n, w, ht, radii);
	} else if (has_bg && bg.a > 0) {
		// GDI+, not GDI: GDI leaves the bitmap's alpha 0, and GDI+ blends
		// what it draws next against that alpha.
		if (radius > 0) g_fill_corners(g, rgba_argb(bg), 0, 0, w, ht, radii);
		else g_fill_rect(g, rgba_argb(bg), 0, 0, w, ht);
	}
	paint_shadows(n, hwnd, g);  // its children's, under them
	if (n->list) wh_paint_list(n, g);  // a lifted row's shadow
	if (wh_pressable(n) && !n->disabled && (n->hot || n->pressed) && !wh_draws_states(n)) {
		rgba c = H.theme.subtle_hot;
		if (n->pressed) c = H.dark ? H.theme.subtle_pressed : (rgba){0, 0, 0, 0x0C};
		if (radius > 0) g_fill_corners(g, rgba_argb(c), 0, 0, w, ht, radii);
		else g_fill_rect(g, rgba_argb(c), 0, 0, w, ht);
	}
	if (wh_has_border(n)) paint_border(g, n, w, ht, radii);
	switch (n->kind) {
	case AK_KIND_IMAGE: wh_paint_image(n, g, w, ht, radius); break;
	case AK_KIND_SHAPE: wh_paint_shape(n, g); break;
	case AK_KIND_SEGMENTED: wh_paint_segmented(n, NULL, g, r); break;
	case AK_KIND_PROGRESS: paint_progress(n, g, w, ht); break;
	case AK_KIND_PAGE_CONTROL: wh_paint_pips(n, g, w, ht); break;
	case AK_KIND_CANVAS: wh_paint_canvas(n, g); break;
	case AK_KIND_SHADER: wh_paint_shader(n, g, w, ht, radius); break;
	case AK_KIND_ACTIVITY:
		if (n->animating) paint_activity(n, g, w, ht);
		break;
	case AK_KIND_PROGRESS_RING: paint_progress_ring(n, g, w, ht); break;
	case AK_KIND_TEXT_FIELD: wh_paint_field(n, g, w, ht); break;
	case KIND_HEADER: wh_paint_header(n, NULL, g, r); break;
	case KIND_COMMAND_BAR: wh_paint_command_bar(n, g, r); break;
	case KIND_TAB_BAR: wh_paint_tab_bar(n, NULL, g, r); break;
	case KIND_SIDEBAR: wh_paint_pane(n, NULL, g, r); break;
	case KIND_ALERT_WINDOW: wh_paint_alert_dialog(n, g, r); break;
	case KIND_ROW: wh_paint_row(n, g, w, ht); break;
	case AK_KIND_SNACKBAR: wh_paint_snack(n, g, w, ht); break;
	}
	g_close(g);
	switch (n->kind) {
	case KIND_PICTURE: wh_paint_picture(n, dc, false); break;  // drawn behind another view
	case AK_KIND_TEXT: wh_draw_text(n, dc, r, rgba_ref(wh_text_color(n, H.theme.label))); break;
	case AK_KIND_SYMBOL: paint_symbol(n, dc, r); break;
	case AK_KIND_SEGMENTED: wh_paint_segmented(n, dc, NULL, r); break;
	case KIND_HEADER: wh_paint_header(n, dc, NULL, r); break;
	case KIND_TAB_BAR: wh_paint_tab_bar(n, dc, NULL, r); break;
	case KIND_SIDEBAR: wh_paint_pane(n, dc, NULL, r); break;
	case AK_KIND_SNACKBAR: wh_paint_snack_text(n, dc); break;
	case AK_KIND_TEXT_FIELD:
		if (n->search_of) wh_paint_search_icon(n, dc, r);
		break;
	}
	bool takes = wh_pressable(n) || n->focusable || n->adjustable || n->na11y_actions || n->kind == AK_KIND_SEGMENTED;
	if (n->focused && takes && !n->no_focus_ring && wh_keyboard_cues(hwnd)) {
		gfx g2 = g_new(dc);
		wh_focus_ring(n, g2, w, ht, radius);
		g_close(g2);
	}
	wh_paint_clip_corners(n, hwnd, dc);
}

// wh_redraw repaints n and its children (they draw n behind them).
void wh_redraw(node *n) {
	if (!n->hwnd) return;
	RedrawWindow(n->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
	wh_pictures_stale(n->hwnd);
}

static bool overlaps(RECT a, RECT b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

// wh_paint_behind draws what's behind a window into its DC: its parent,
// with the DC's origin moved to the parent's (which draws what's behind
// itself in turn, where it isn't opaque), and the siblings under it that
// it overlaps. uxtheme's DrawThemeParentBackground does this through
// WM_PRINTCLIENT but ignores calls nested in another, which a tree of
// transparent views makes.
void wh_paint_behind(HWND hwnd, HDC dc) {
	HWND parent = GetParent(hwnd);
	if (!parent) return;
	RECT at = wh_rect_in(hwnd, parent);
	int saved = SaveDC(dc);
	OffsetViewportOrgEx(dc, -at.left, -at.top, NULL);
	RECT pr;
	GetClientRect(parent, &pr);
	node *pn = wh_node_of(parent);
	if (pn && pn->hwnd == parent && pn->kind != KIND_SHEET_WINDOW && pn->kind != KIND_WINDOW_FRAME) {
		wh_paint(pn, parent, dc, pr);
	} else if (pn && pn->ctl == parent) {
		DrawThemeParentBackground(hwnd, dc, NULL);
	} else {
		gfx g = g_new(dc);
		g_fill_rect(g, rgba_argb(wh_bg_of(parent)), 0, 0, (float)pr.right, (float)pr.bottom);
		g_close(g);
	}
	HWND below[256];
	int nb = 0;
	for (HWND s = GetWindow(hwnd, GW_HWNDNEXT); s && nb < 256; s = GetWindow(s, GW_HWNDNEXT))
		if (IsWindowVisible(s) && overlaps(wh_rect_in(s, parent), at)) below[nb++] = s;
	for (int i = nb - 1; i >= 0; i--) wh_paint_tree(below[i], parent, dc);
	RestoreDC(dc, saved);
}

// wh_paint_tree draws a window and its children into dc, whose origin is
// its parent's: the host's views as they paint, controls through
// WM_PRINTCLIENT.
void wh_paint_tree(HWND hwnd, HWND parent, HDC dc) {
	if (hwnd == H.skip_tree) return;  // a rotated view, under its picture
	node *n = wh_node_of(hwnd);
	if (n && n->hwnd == hwnd && wh_tree_in_picture(n)) return;  // its picture draws it
	RECT at = wh_rect_in(hwnd, parent);
	int saved = SaveDC(dc);
	OffsetViewportOrgEx(dc, at.left, at.top, NULL);
	RECT r;
	GetClientRect(hwnd, &r);
	IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
	if (n && n->hwnd == hwnd) wh_clip_tree(n, dc);
	RECT box;
	if (GetClipBox(dc, &box) == NULLREGION) {
		RestoreDC(dc, saved);  // none of it shows
		return;
	}
	if (n && n->hwnd == hwnd) wh_paint_own(n, hwnd, dc, r);
	else SendMessageW(hwnd, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
	HWND kids[512];
	int nk = 0;
	for (HWND c = GetWindow(hwnd, GW_CHILD); c && nk < 512; c = GetWindow(c, GW_HWNDNEXT))
		if (IsWindowVisible(c)) kids[nk++] = c;
	for (int i = nk - 1; i >= 0; i--) wh_paint_tree(kids[i], hwnd, dc);
	RestoreDC(dc, saved);
}

// wh_color_behind is the color behind the middle of n's window: what a
// view that can't be transparent (a rich edit) fills itself with.
rgba wh_color_behind(node *n) {
	if (!n->hwnd || GetParent(n->hwnd) == H.park) return wh_backdrop(n);
	// The middle of the part that shows: what scroll views and parents clip
	// off isn't painted.
	RECT vis;
	GetWindowRect(n->hwnd, &vis);
	HWND root = GetAncestor(n->hwnd, GA_ROOT);
	for (HWND p = GetParent(n->hwnd); p && p != root; p = GetParent(p)) {
		RECT c;
		GetClientRect(p, &c);
		POINT o = {0, 0};
		ClientToScreen(p, &o);
		vis.left = wh_maxi(vis.left, o.x), vis.top = wh_maxi(vis.top, o.y);
		vis.right = wh_mini(vis.right, o.x + c.right), vis.bottom = wh_mini(vis.bottom, o.y + c.bottom);
	}
	if (vis.right <= vis.left || vis.bottom <= vis.top) return wh_backdrop(n);
	POINT at = {(vis.left + vis.right) / 2, (vis.top + vis.bottom) / 2};
	ScreenToClient(n->hwnd, &at);
	HDC dc = CreateCompatibleDC(NULL);
	BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), 1, -1, 1, 32, BI_RGB}};
	uint8_t *px = NULL;
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
	HGDIOBJ old = SelectObject(dc, bmp);
	OffsetViewportOrgEx(dc, -at.x, -at.y, NULL);
	wh_paint_behind(n->hwnd, dc);
	GdiFlush();
	rgba c = {px[2], px[1], px[0], 255};
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
	return c;
}
