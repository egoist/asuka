// clip.c: rounded clips. A child window ignores its parent's drawing, so a
// view that clips with a corner radius is shaped by a window region
// (SetWindowRgn), which clips its children's pixels too: the pixels its
// rounded rectangle covers whole. A region has no partial pixels, so the
// corners' edge pixels, outside it, are the parent's, and the parent draws
// them antialiased: the clipping view's tree drawn there and mixed with
// what's behind it by how much of each pixel the rounded rectangle covers.
// When something in the clipping view paints over its corners, the parent
// draws them again.
//
// A node's clip_rgn is its rounded clip's region (its client
// coordinates), clip_r its radius in pixels; H.round_clips counts them.
#include "host.h"
#include "parts.h"
#include "draw.h"

static node *clip_off;  // the view whose corners are being drawn whole

// clip_radii are n's rounded clip's radii in pixels (top left, top
// right, bottom right, bottom left), the largest returned: 0 for none.
static double clip_radii(node *n, int w, int h, double rs[4]) {
	rs[0] = rs[1] = rs[2] = rs[3] = 0;
	if (!n->clip || !wh_round(n) || w <= 0 || h <= 0) return 0;
	float r[4];
	wh_radii(n, (float)w, (float)h, r);
	double most = 0;
	for (int i = 0; i < 4; i++) rs[i] = r[i], most = fmax(most, r[i]);
	return most;
}

// inset is where a row's pixels' centers enter a corner's circle of
// radius rc, ry rows from the corner's side.
static int inset(double rc, int ry, int w) {
	if (rc <= 0) return 0;
	double dy = rc - (ry + 0.5);
	if (dy <= 0) return 0;
	double s = (rc - 0.5) * (rc - 0.5) - dy * dy;
	double x = s < 0 ? ceil(rc - 0.5) : ceil(rc - 0.5 - sqrt(s));
	x = x < 0 ? 0 : x > w / 2.0 ? w / 2.0 : x;
	return (int)x;
}

// clip_rows is the region of the pixels a w by h rectangle of corner
// radii rs covers whole (coverage 1), runs of rows of the same width
// merged.
static HRGN clip_rows(int w, int h, const double rs[4]) {
	int zt = (int)ceil(fmax(rs[0], rs[1]) - 0.5), zb = (int)ceil(fmax(rs[2], rs[3]) - 0.5);  // rows above the corners' centers
	if (zt > h / 2) zt = h / 2;
	if (zb > h / 2) zb = h / 2;
	int z = zt;
	size_t cap = (size_t)zt + (size_t)zb + 1;
	size_t hs = sizeof(RGNDATAHEADER);
	RGNDATA *d = wh_alloc(hs + cap * sizeof(RECT));
	RECT *rects = (RECT *)d->Buffer;
	DWORD n = 0;
	// The top corners' rows, the middle, the bottom corners' rows: a row's
	// inset is where its pixels' centers enter its corners' circles.
	for (int pass = 0; pass < 3; pass++) {
		int y0 = pass == 0 ? 0 : pass == 1 ? z : h - zb;
		int y1 = pass == 0 ? z : pass == 1 ? h - zb : h;
		for (int y = y0; y < y1; y++) {
			int x0 = 0, x1 = w, yb = y + 1;
			if (pass == 1) {
				yb = y1;
			} else if (pass == 0) {
				x0 = inset(rs[0], y, w), x1 = w - inset(rs[1], y, w);
			} else {
				x0 = inset(rs[3], h - 1 - y, w), x1 = w - inset(rs[2], h - 1 - y, w);
			}
			if (x1 > x0 && yb > y) {
				RECT *p = n ? &rects[n - 1] : NULL;
				if (p && p->left == x0 && p->right == x1 && p->bottom == y) {
					p->bottom = yb;
				} else {
					rects[n++] = (RECT){x0, y, x1, yb};
				}
			}
			if (pass == 1) break;
		}
	}
	d->rdh.dwSize = (DWORD)hs;
	d->rdh.iType = RDH_RECTANGLES;
	d->rdh.nCount = n;
	d->rdh.nRgnSize = n * (DWORD)sizeof(RECT);
	d->rdh.rcBound = (RECT){0, 0, w, h};
	HRGN rgn = ExtCreateRegion(NULL, (DWORD)(hs + n * sizeof(RECT)), d);
	free(d);
	return rgn;
}

// wh_clip_coverage is how much of the pixel at (x, y) (pixels from the
// view's top left) a w by h rounded rectangle of radius rc covers.
double wh_clip_coverage(int x, int y, int w, int h, double rc) {
	const double r[4] = {rc, rc, rc, rc};
	return wh_clip_coverage4(x, y, w, h, r);
}

double wh_clip_coverage4(int x, int y, int w, int h, const double r[4]) {
	double px = x + 0.5, py = y + 0.5, dx, dy, rc;
	// The corner whose square it's in (scaled as CSS's, they don't meet).
	if (px < r[0] && py < r[0]) rc = r[0], dx = rc - px, dy = rc - py;
	else if (px > w - r[1] && py < r[1]) rc = r[1], dx = px - (w - rc), dy = rc - py;
	else if (px > w - r[2] && py > h - r[2]) rc = r[2], dx = px - (w - rc), dy = py - (h - rc);
	else if (px < r[3] && py > h - r[3]) rc = r[3], dx = rc - px, dy = py - (h - rc);
	else return 1;
	double c = rc - hypot(dx, dy) + 0.5;
	return c < 0 ? 0 : c > 1 ? 1 : c;
}

static HRGN copy_rgn(HRGN r) {
	HRGN c = CreateRectRgn(0, 0, 0, 0);
	CombineRgn(c, r, NULL, RGN_COPY);
	return c;
}

// wh_restore_shape gives n's window its own shape again: its rounded
// clip's, or none.
void wh_restore_shape(node *n) { SetWindowRgn(n->hwnd, n->clip_rgn ? copy_rgn(n->clip_rgn) : NULL, TRUE); }

void wh_shape_clip(node *n) {
	if (!n->hwnd) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	double rs[4];
	double rc = clip_radii(n, r.right, r.bottom, rs);
	if (rc <= 0) {
		if (n->clip_rgn) {
			DeleteObject(n->clip_rgn);
			n->clip_rgn = NULL;
			H.round_clips--;
			if (!wh_picture_shaped(n)) SetWindowRgn(n->hwnd, NULL, TRUE);
		}
		return;
	}
	HRGN rgn = clip_rows(r.right, r.bottom, rs);
	if (!rgn) return;
	if (!n->clip_rgn) {
		H.round_clips++;
	} else if (n->clip_r == rc && memcmp(n->clip_rs, rs, sizeof rs) == 0 && EqualRgn(n->clip_rgn, rgn)) {
		DeleteObject(rgn);
		return;
	} else {
		DeleteObject(n->clip_rgn);
	}
	n->clip_rgn = rgn, n->clip_r = rc;
	memcpy(n->clip_rs, rs, sizeof rs);
	if (wh_picture_shaped(n)) return;  // shaped to nothing while it's rotated (picture.c)
	SetWindowRgn(n->hwnd, copy_rgn(rgn), TRUE);
	HWND p = GetParent(n->hwnd);
	if (p) {
		RECT at = wh_rect_in(n->hwnd, p);
		RedrawWindow(p, &at, NULL, RDW_INVALIDATE);  // its corners' edges
	}
}

static bool overlaps(RECT a, RECT b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

// corners are the squares of c's window's corners, in its parent's
// client coordinates.
static void corners(const node *c, RECT at, RECT q[4]) {
	int z = (int)ceil(c->clip_r);
	q[0] = (RECT){at.left, at.top, at.left + z, at.top + z};
	q[1] = (RECT){at.right - z, at.top, at.right, at.top + z};
	q[2] = (RECT){at.left, at.bottom - z, at.left + z, at.bottom};
	q[3] = (RECT){at.right - z, at.bottom - z, at.right, at.bottom};
}

// paint_corners draws c's tree into its corners' squares, over what dc
// has there, mixed by how much its rounded rectangle covers each pixel.
static void paint_corners(node *c, HWND parent, HDC dc) {
	RECT box;
	if (GetClipBox(dc, &box) <= NULLREGION) return;
	RECT at = wh_rect_in(c->hwnd, parent);
	int w = at.right - at.left, h = at.bottom - at.top;
	int z = (int)ceil(c->clip_r);
	if (z <= 0) return;
	HDC mem = NULL;
	HBITMAP bmp = NULL;
	HGDIOBJ old = NULL;
	uint8_t *px = NULL, *behind = NULL;
	RECT q[4];
	corners(c, at, q);
	for (int i = 0; i < 4; i++) {
		if (!overlaps(q[i], box)) continue;
		if (!mem) {
			mem = CreateCompatibleDC(dc);
			bmp = wh_dib(z, z, &px);
			if (!bmp) {
				DeleteDC(mem);
				return;
			}
			old = SelectObject(mem, bmp);
			behind = wh_alloc((size_t)z * (size_t)z * 4);
		}
		BitBlt(mem, 0, 0, z, z, dc, q[i].left, q[i].top, SRCCOPY);
		GdiFlush();
		memcpy(behind, px, (size_t)z * (size_t)z * 4);
		int saved = SaveDC(mem);
		OffsetViewportOrgEx(mem, -q[i].left, -q[i].top, NULL);
		node *off = clip_off;
		clip_off = c;  // drawn whole: its region would leave out the edge
		wh_paint_tree(c->hwnd, parent, mem);
		clip_off = off;
		RestoreDC(mem, saved);
		GdiFlush();
		for (int y = 0; y < z; y++)
			for (int x = 0; x < z; x++) {
				size_t k = ((size_t)y * (size_t)z + (size_t)x) * 4;
				double cov = wh_clip_coverage4(q[i].left - at.left + x, q[i].top - at.top + y, w, h, c->clip_rs);
				if (cov >= 1) continue;
				for (int j = 0; j < 3; j++) px[k + j] = (uint8_t)(behind[k + j] * (1 - cov) + px[k + j] * cov + 0.5);
				px[k + 3] = 255;
			}
		BitBlt(dc, q[i].left, q[i].top, z, z, mem, 0, 0, SRCCOPY);
	}
	if (mem) {
		SelectObject(mem, old);
		DeleteObject(bmp);
		DeleteDC(mem);
		free(behind);
	}
}

void wh_paint_clip_corners(node *n, HWND hwnd, HDC dc) {
	if (!H.round_clips) return;
	for (uint32_t i = 0; i < n->nkids; i++) {
		node *c = n->kids[i];
		if (c->clip_rgn && c->hwnd && c != clip_off && !c->pict && IsWindowVisible(c->hwnd) && GetParent(c->hwnd) == hwnd)
			paint_corners(c, hwnd, dc);
	}
}

void wh_clip_tree(node *n, HDC dc) {
	if (!n->clip_rgn || n == clip_off) return;
	POINT o;
	GetViewportOrgEx(dc, &o);
	HRGN r = copy_rgn(n->clip_rgn);
	OffsetRgn(r, o.x, o.y);
	ExtSelectClipRgn(dc, r, RGN_AND);
	DeleteObject(r);
}

static bool window_kind(const node *c) {
	return c->kind == KIND_SHEET_WINDOW || c->kind == KIND_WINDOW_FRAME || c->kind == KIND_POPOVER_WINDOW || c->kind == KIND_ALERT_WINDOW;
}

void wh_corners_painted(HWND hwnd) {
	if (!H.round_clips) return;
	RECT r = {0};
	bool have = false;
	for (HWND w = hwnd; w; w = GetParent(w)) {
		node *c = wh_node_of(w);
		if (!c || c->hwnd != w || window_kind(c) || w == H.window) return;
		if (!c->clip_rgn) continue;
		HWND p = GetParent(w);
		if (!p) return;
		if (!have) GetWindowRect(hwnd, &r), have = true;
		RECT at = wh_rect_in(w, p);
		POINT o = {0, 0};
		MapWindowPoints(NULL, p, &o, 1);
		RECT painted = {r.left + o.x, r.top + o.y, r.right + o.x, r.bottom + o.y};
		RECT q[4];
		corners(c, at, q);
		for (int i = 0; i < 4; i++)
			if (overlaps(q[i], painted)) RedrawWindow(p, &q[i], NULL, RDW_INVALIDATE);
	}
}
