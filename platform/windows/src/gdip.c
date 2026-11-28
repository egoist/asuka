// gdip.c: GDI+'s flat API, drawn antialiased with pixel centers at .5
// and blended in sRGB (as WinUI does).
#include "host.h"

static ULONG_PTR token;

void wh_gdiplus_start(void) {
	if (token) return;
	GdiplusStartupInput in = {1, NULL, FALSE, FALSE};
	GdiplusStartup(&token, &in, NULL);
}

gfx g_new(HDC dc) {
	GpGraphics *g = NULL;
	GdipCreateFromHDC(dc, &g);
	if (g) {
		GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
		GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
		GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
		GdipSetCompositingQuality(g, CompositingQualityAssumeLinear);
	}
	return g;
}

void g_close(gfx g) {
	if (g) GdipDeleteGraphics(g);
}

void g_fill_rect(gfx g, ARGB c, float x, float y, float w, float h) {
	GpSolidFill *b = NULL;
	GdipCreateSolidFill(c, &b);
	GdipFillRectangle(g, (GpBrush *)b, x, y, w, h);
	GdipDeleteBrush((GpBrush *)b);
}

GpPath *g_path(void) {
	GpPath *p = NULL;
	GdipCreatePath(FillModeAlternate, &p);
	return p;
}

void g_path_round_rect(GpPath *p, float x, float y, float w, float h, float r) {
	r = wh_minf(r, wh_minf(w / 2, h / 2));
	if (r <= 0) {
		GdipAddPathRectangle(p, x, y, w, h);
		return;
	}
	float d = 2 * r;
	GdipStartPathFigure(p);
	GdipAddPathArc(p, x, y, d, d, 180, 90);
	GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
	GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
	GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
	GdipClosePathFigure(p);
}

void g_path_rrect(GpPath *p, float x, float y, float w, float h, const float rx[4], const float ry[4]) {
	GdipStartPathFigure(p);
	// Clockwise from the top left: each corner's arc, or its point.
	const float cx[4] = {x, x + w, x + w, x}, cy[4] = {y, y, y + h, y + h};
	const float sx[4] = {0, -2, -2, 0}, sy[4] = {0, 0, -2, -2}, start[4] = {180, 270, 0, 90};
	for (int i = 0; i < 4; i++) {
		float a = wh_maxf(rx[i], 0), b = wh_maxf(ry[i], 0);
		if (a > 0 && b > 0) GdipAddPathArc(p, cx[i] + sx[i] * a, cy[i] + sy[i] * b, 2 * a, 2 * b, start[i], 90);
		else GdipAddPathLine(p, cx[i], cy[i], cx[i], cy[i]);
	}
	GdipClosePathFigure(p);
}

void g_path_corners(GpPath *p, float x, float y, float w, float h, const float r[4]) {
	if (r[0] <= 0 && r[1] <= 0 && r[2] <= 0 && r[3] <= 0) {
		GdipAddPathRectangle(p, x, y, w, h);
		return;
	}
	g_path_rrect(p, x, y, w, h, r, r);
}

void g_fill_corners(gfx g, ARGB c, float x, float y, float w, float h, const float r[4]) {
	GpPath *p = g_path();
	g_path_corners(p, x, y, w, h, r);
	GpSolidFill *b = NULL;
	GdipCreateSolidFill(c, &b);
	GdipFillPath(g, (GpBrush *)b, p);
	GdipDeleteBrush((GpBrush *)b);
	GdipDeletePath(p);
}

void g_fill_round_rect(gfx g, ARGB c, float x, float y, float w, float h, float r) {
	GpPath *p = g_path();
	g_path_round_rect(p, x, y, w, h, r);
	GpSolidFill *b = NULL;
	GdipCreateSolidFill(c, &b);
	GdipFillPath(g, (GpBrush *)b, p);
	GdipDeleteBrush((GpBrush *)b);
	GdipDeletePath(p);
}

// g_stroke_round_rect strokes a rounded rectangle inside its bounds.
void g_stroke_round_rect(gfx g, ARGB c, float width, float x, float y, float w, float h, float r) {
	GpPath *p = g_path();
	g_path_round_rect(p, x + width / 2, y + width / 2, w - width, h - width, wh_maxf(r - width / 2, 0));
	GpPen *pen = NULL;
	GdipCreatePen1(c, width, UnitPixel, &pen);
	GdipDrawPath(g, pen, p);
	GdipDeletePen(pen);
	GdipDeletePath(p);
}

void g_fill_ellipse(gfx g, ARGB c, float x, float y, float w, float h) {
	GpSolidFill *b = NULL;
	GdipCreateSolidFill(c, &b);
	GdipFillEllipse(g, (GpBrush *)b, x, y, w, h);
	GdipDeleteBrush((GpBrush *)b);
}

void g_draw_arc(gfx g, ARGB c, float width, int cap, float x, float y, float w, float h, float start, float sweep) {
	GpPen *pen = NULL;
	GdipCreatePen1(c, width, UnitPixel, &pen);
	if (cap) GdipSetPenLineCap197819(pen, (GpLineCap)cap, (GpLineCap)cap, DashCapFlat);
	GdipDrawArc(g, pen, x, y, w, h, start, sweep);
	GdipDeletePen(pen);
}

void g_draw_line(gfx g, ARGB c, float width, float x0, float y0, float x1, float y1) {
	GpPen *pen = NULL;
	GdipCreatePen1(c, width, UnitPixel, &pen);
	GdipDrawLine(g, pen, x0, y0, x1, y1);
	GdipDeletePen(pen);
}

void g_clip_path(gfx g, GpPath *p) { GdipSetClipPath(g, p, CombineModeReplace); }
void g_reset_clip(gfx g) { GdipResetClip(g); }

// g_bitmap is a bitmap of premultiplied BGRA pixels, its own copy.
GpBitmap *g_bitmap(int w, int h, const uint8_t *pbgra) {
	if (w <= 0 || h <= 0) return NULL;
	GpBitmap *b = NULL;
	GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppPARGB, NULL, &b);
	if (!b) return NULL;
	if (pbgra) {
		BitmapData d;
		GpRect r = {0, 0, w, h};
		if (GdipBitmapLockBits(b, &r, ImageLockModeWrite, PixelFormat32bppPARGB, &d) == Ok) {
			for (int y = 0; y < h; y++) memcpy((uint8_t *)d.Scan0 + (size_t)y * (size_t)d.Stride, pbgra + (size_t)y * (size_t)w * 4, (size_t)w * 4);
			GdipBitmapUnlockBits(b, &d);
		}
	}
	return b;
}

static GpImageAttributes *alpha_attrs;
static float alpha_at = -1;

// g_draw_image draws the part (sx, sy, sw, sh) of b into (x, y, w, h), at
// an opacity (0 to 1): a color matrix scaling the alpha, kept per opacity
// drawn last.
void g_draw_image(gfx g, GpBitmap *b, float alpha, float x, float y, float w, float h, float sx, float sy, float sw, float sh) {
	if (!b || alpha <= 0) return;
	GpImageAttributes *attrs = NULL;
	if (alpha < 1) {
		if (!alpha_attrs) GdipCreateImageAttributes(&alpha_attrs);
		if (alpha_attrs && alpha != alpha_at) {
			ColorMatrix m = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
			GdipSetImageAttributesColorMatrix(alpha_attrs, ColorAdjustTypeDefault, TRUE, &m, NULL, ColorMatrixFlagsDefault);
			alpha_at = alpha;
		}
		attrs = alpha_attrs;
	}
	GdipDrawImageRectRect(g, (GpImage *)b, x, y, w, h, sx, sy, sw, sh, UnitPixel, attrs, NULL, NULL);
}
