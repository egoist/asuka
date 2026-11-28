// material.c: Style.Material: a translucent background that blurs what's
// behind, Fluent's acrylic in a child window (DWM's backdrops are top-level
// windows' only). The view draws what's behind it, as any view that isn't
// opaque does, then blurs it (three box blurs, a Gaussian's likeness) and
// tints it light or dark, under its own drawing. As what's behind it
// paints, it draws again. What's behind is drawn into the blur's own
// bitmap, not read from the DC it paints into, whose parts outside the
// update region hold nothing.
#include "host.h"
#include "parts.h"
#include "draw.h"

// look is a material's blur (points, each box's half width) and tint's
// opacity.
static void look(int32_t m, float *blur, float *tint) {
	switch (m) {
	case AK_MATERIAL_ULTRA_THIN: *blur = 6, *tint = 0.3f; return;
	case AK_MATERIAL_THIN: *blur = 7, *tint = 0.45f; return;
	case AK_MATERIAL_THICK: *blur = 8, *tint = 0.8f; return;
	case AK_MATERIAL_BAR: *blur = 8, *tint = 0.7f; return;
	}
	*blur = 8, *tint = 0.6f;  // Regular
}

void wh_set_material(node *n, int32_t m) {
	if ((m != AK_MATERIAL_NONE) != (n->material != AK_MATERIAL_NONE)) H.materials += m != AK_MATERIAL_NONE ? 1 : -1;
	n->material = m;
	wh_redraw(n);
}

// box_blur blurs src into dst (BGRA, w by h) with a box of half width r,
// along rows or columns; the edges' pixels extend past them.
static void box_blur(const uint8_t *src, uint8_t *dst, int w, int h, int r, bool rows) {
	if (r <= 0) {
		memcpy(dst, src, (size_t)w * (size_t)h * 4);
		return;
	}
	int n = w, lines = h;
	size_t step = 4, stride = (size_t)w * 4;
	if (!rows) n = h, lines = w, step = (size_t)w * 4, stride = 4;
	int d = 2 * r + 1;
	for (int l = 0; l < lines; l++) {
		size_t base = (size_t)l * stride;
		for (int c = 0; c < 3; c++) {
#define AT(i) ((int)src[base + (size_t)((i) < 0 ? 0 : (i) > n - 1 ? n - 1 : (i)) * step + (size_t)c])
			int sum = 0;
			for (int i = -r; i <= r; i++) sum += AT(i);
			for (int i = 0; i < n; i++) {
				dst[base + (size_t)i * step + (size_t)c] = (uint8_t)((sum + d / 2) / d);
				sum += AT(i + r + 1) - AT(i - r);
			}
#undef AT
		}
	}
}

static uint8_t *blur_tmp;
static size_t blur_cap;

void wh_paint_material(node *n, HDC dc, int w, int h) {
	RECT box;
	if (GetClipBox(dc, &box) <= NULLREGION || w <= 0 || h <= 0) return;
	float k = wh_scale_of(n), blur, tint;
	look(n->material, &blur, &tint);
	int b = (int)ceilf(blur * k);
	// What's drawn, and around it what blurs into it.
	RECT area = {wh_maxi(box.left - 3 * b, 0), wh_maxi(box.top - 3 * b, 0), wh_mini(box.right + 3 * b, w), wh_mini(box.bottom + 3 * b, h)};
	int aw = area.right - area.left, ah = area.bottom - area.top;
	if (aw <= 0 || ah <= 0) return;
	HDC mem = CreateCompatibleDC(dc);
	uint8_t *px;
	HBITMAP bmp = wh_dib(aw, ah, &px);
	if (!bmp) {
		DeleteDC(mem);
		return;
	}
	HGDIOBJ old = SelectObject(mem, bmp);
	// What's behind it, drawn again whole: dc has only what its update
	// region needed (a child's place, say, is left out), and what's left
	// out would blur in as black.
	int saved = SaveDC(mem);
	SetViewportOrgEx(mem, -area.left, -area.top, NULL);
	wh_paint_behind(n->hwnd, mem);
	RestoreDC(mem, saved);
	GdiFlush();
	size_t size = (size_t)aw * (size_t)ah * 4;
	uint8_t *behind = wh_alloc(size);
	memcpy(behind, px, size);
	if (blur_cap < size) {
		free(blur_tmp);
		blur_tmp = wh_alloc(size);
		blur_cap = size;
	}
	for (int i = 0; i < 3; i++) {
		box_blur(px, blur_tmp, aw, ah, b, true);
		box_blur(blur_tmp, px, aw, ah, b, false);
	}
	rgba c = {28, 28, 28, 255};
	if (!H.dark) c = (rgba){252, 252, 252, 255}, tint = wh_minf(tint + 0.1f, 1);
	float rf[4];
	wh_radii(n, (float)w, (float)h, rf);  // each corner its own
	double radii[4] = {rf[0], rf[1], rf[2], rf[3]};
	bool round = rf[0] > 0 || rf[1] > 0 || rf[2] > 0 || rf[3] > 0;
	double t = tint, tb = c.b * t, tg = c.g * t, tr = c.r * t;
	for (int y = 0; y < ah; y++)
		for (int x = 0; x < aw; x++) {
			size_t i = ((size_t)y * (size_t)aw + (size_t)x) * 4;
			double cov = round ? wh_clip_coverage4(area.left + x, area.top + y, w, h, radii) : 1;
			double bl = px[i] * (1 - t) + tb, gr = px[i + 1] * (1 - t) + tg, rd = px[i + 2] * (1 - t) + tr;
			px[i] = (uint8_t)(behind[i] * (1 - cov) + bl * cov + 0.5);
			px[i + 1] = (uint8_t)(behind[i + 1] * (1 - cov) + gr * cov + 0.5);
			px[i + 2] = (uint8_t)(behind[i + 2] * (1 - cov) + rd * cov + 0.5);
			px[i + 3] = 255;
		}
	free(behind);
	BitBlt(dc, area.left, area.top, aw, ah, mem, 0, 0, SRCCOPY);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
}

static bool overlaps(RECT a, RECT b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

// above reports whether window a is drawn over window b: of the windows
// they're in, the ones that share a parent, a's comes before b's.
static bool above(HWND a, HWND b) {
	HWND as[64];
	int na = 0;
	for (HWND w = a; w && na < 64; w = GetParent(w)) as[na++] = w;
	for (HWND w = b; w; w = GetParent(w)) {
		HWND p = GetParent(w);
		for (int i = 0; i + 1 < na; i++) {
			if (as[i + 1] == p && p) {
				// as[i] and w are siblings: as[i] above w if it comes first.
				for (HWND s = GetWindow(w, GW_HWNDPREV); s; s = GetWindow(s, GW_HWNDPREV))
					if (s == as[i]) return true;
				return false;
			}
		}
	}
	return false;
}

void wh_materials_behind(HWND hwnd) {
	if (!H.materials) return;
	RECT r;
	GetWindowRect(hwnd, &r);
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *m = v;
		if (!m->material || !m->hwnd || m->hwnd == hwnd || !IsWindowVisible(m->hwnd)) continue;
		RECT mr;
		GetWindowRect(m->hwnd, &mr);
		if (IsChild(m->hwnd, hwnd) || IsChild(hwnd, m->hwnd) || !overlaps(mr, r) || !above(m->hwnd, hwnd)) continue;
		RedrawWindow(m->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
	}
}
