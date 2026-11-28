// cursor.c: Cursor, the pointer's shape over a view and what's in it
// that sets none (CSS's cursor, inherited). A view's window answers
// WM_SETCURSOR, which Windows asks the window under the pointer's
// ancestors first: each finds the deepest of the host's windows there
// (the views the mouse goes through, a Text, included) and the nearest
// view up from it with a cursor of its own. A text field keeps its
// I-beam, a table its header's resizing, a web view its own, unless they
// set one. Where Windows has no cursor of the shape (grab, grabbing,
// copy, alias, a context menu, a column's or row's resizing, zooming),
// the host draws one, as browsers on Windows do, at the system's cursor
// size.
#include "host.h"
#include "parts.h"

static HCURSOR drawn[AK_CURSOR_COUNT];
static int drawn_size;

void wh_set_cursor_prop(node *n, int32_t c) {
	if (c < 0 || c >= AK_CURSOR_COUNT) c = 0;
	if (!n->cursor != !c) H.cursors += c ? 1 : -1;
	n->cursor = c;
}

// ---- Cursors drawn ----

typedef struct {
	GpBitmap *bmp;
	gfx g;
	uint8_t *px;
	HBITMAP dib;
	int size;
	float u;  // a 32-pixel cursor's pixel
} canvas;

static bool canvas_open(canvas *c, int size) {
	memset(c, 0, sizeof *c);
	BITMAPV5HEADER bi = {sizeof bi};
	bi.bV5Width = size, bi.bV5Height = -size, bi.bV5Planes = 1, bi.bV5BitCount = 32, bi.bV5Compression = BI_BITFIELDS;
	bi.bV5RedMask = 0x00FF0000, bi.bV5GreenMask = 0x0000FF00, bi.bV5BlueMask = 0x000000FF, bi.bV5AlphaMask = 0xFF000000;
	HDC screen = GetDC(NULL);
	c->dib = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&c->px, NULL, 0);
	ReleaseDC(NULL, screen);
	if (!c->dib) return false;
	memset(c->px, 0, (size_t)size * (size_t)size * 4);
	// Straight alpha, as a cursor's color bitmap has it.
	GdipCreateBitmapFromScan0(size, size, size * 4, PixelFormat32bppARGB, c->px, &c->bmp);
	if (!c->bmp) {
		DeleteObject(c->dib);
		return false;
	}
	GdipGetImageGraphicsContext((GpImage *)c->bmp, &c->g);
	GdipSetSmoothingMode(c->g, SmoothingModeAntiAlias);
	GdipSetPixelOffsetMode(c->g, PixelOffsetModeHalf);
	c->size = size;
	c->u = (float)size / 32;
	return true;
}

static HCURSOR canvas_close(canvas *c, float hx, float hy) {
	GdipDeleteGraphics(c->g);
	GdipDisposeImage((GpImage *)c->bmp);
	HBITMAP mask = CreateBitmap(c->size, c->size, 1, 1, NULL);
	ICONINFO ii = {FALSE, (DWORD)lroundf(hx * c->u), (DWORD)lroundf(hy * c->u), mask, c->dib};
	HCURSOR h = (HCURSOR)CreateIconIndirect(&ii);
	DeleteObject(mask);
	DeleteObject(c->dib);
	return h;
}

// shape fills and outlines a path of points (in a 32-pixel cursor's
// units): white with a black edge, as Windows' own cursors are.
static void shape(canvas *c, GpPath *p, bool dark_fill) {
	GpMatrix *m = NULL;
	GdipCreateMatrix2(c->u, 0, 0, c->u, 0, 0, &m);
	GdipTransformPath(p, m);
	GdipDeleteMatrix(m);
	GpSolidFill *b = NULL;
	GdipCreateSolidFill(dark_fill ? 0xFF000000 : 0xFFFFFFFF, &b);
	GdipFillPath(c->g, (GpBrush *)b, p);
	GdipDeleteBrush((GpBrush *)b);
	GpPen *pen = NULL;
	GdipCreatePen1(dark_fill ? 0xFFFFFFFF : 0xFF000000, 1.25f * c->u, UnitPixel, &pen);
	GdipSetPenLineJoin(pen, LineJoinRound);
	GdipDrawPath(c->g, pen, p);
	GdipDeletePen(pen);
}

static void poly(canvas *c, const float *xy, int n, bool dark) {
	GpPath *p = g_path();
	GpPointF pts[32];
	for (int i = 0; i < n && i < 32; i++) pts[i] = (GpPointF){xy[i * 2], xy[i * 2 + 1]};
	GdipAddPathPolygon(p, pts, n);
	shape(c, p, dark);
	GdipDeletePath(p);
}

static void rrect(canvas *c, float x, float y, float w, float h, float r, bool dark) {
	GpPath *p = g_path();
	g_path_round_rect(p, x, y, w, h, r);
	shape(c, p, dark);
	GdipDeletePath(p);
}

// The arrow, with its tip at 1, 1.
static void arrow(canvas *c) {
	static const float a[] = {1, 1, 1, 18, 5, 14.5f, 8, 21, 11, 19.6f, 8.2f, 13.4f, 13, 13.4f};
	poly(c, a, 7, false);
}

static void line(canvas *c, float x0, float y0, float x1, float y1, float w, ARGB col) {
	g_draw_line(c->g, col, w * c->u, x0 * c->u, y0 * c->u, x1 * c->u, y1 * c->u);
}

// A badge at the arrow's lower right: a copy's plus, an alias's arrow, a
// context menu's menu.
static void badge(canvas *c, int which) {
	if (which == 2) {
		rrect(c, 15, 14, 13, 15, 1.5f, false);
		for (int i = 0; i < 3; i++) line(c, 17.5f, 17.5f + (float)i * 4, 25.5f, 17.5f + (float)i * 4, 1.5f, 0xFF000000);
		return;
	}
	rrect(c, 16, 17, 12, 12, 1.5f, false);
	if (which == 0) {
		line(c, 22, 19.5f, 22, 26.5f, 1.6f, 0xFF000000);
		line(c, 18.5f, 23, 25.5f, 23, 1.6f, 0xFF000000);
	} else {
		line(c, 19, 26, 25, 20, 1.6f, 0xFF000000);
		line(c, 21, 20, 25, 20, 1.6f, 0xFF000000);
		line(c, 25, 20, 25, 24, 1.6f, 0xFF000000);
	}
}

// A hand: open (grab) or closed (grabbing).
static void hand(canvas *c, bool closed) {
	if (closed) {
		rrect(c, 8, 12, 17, 14, 5, false);
		for (int i = 0; i < 4; i++) rrect(c, 8.5f + (float)i * 4, 9.5f, 4, 6, 2, false);
		rrect(c, 5.5f, 14, 5, 7, 2.5f, false);
		return;
	}
	for (int i = 0; i < 4; i++) rrect(c, 9 + (float)i * 4, i == 0 || i == 3 ? 7 : 4.5f, 4, 14, 2, false);
	rrect(c, 3.5f, 13, 5, 10, 2.5f, false);
	rrect(c, 8, 14, 17, 13, 5, false);
	for (int i = 1; i < 4; i++) line(c, 9 + (float)i * 4, 15, 9 + (float)i * 4, 19, 1, 0xFF000000);
}

// A magnifier with a plus or a minus.
static void magnifier(canvas *c, bool in) {
	line(c, 17, 17, 26, 26, 4.5f, 0xFF000000);
	line(c, 17.5f, 17.5f, 25.5f, 25.5f, 2.2f, 0xFFFFFFFF);
	GpPath *p = g_path();
	GdipAddPathEllipse(p, 2, 2, 17, 17);
	shape(c, p, false);
	GdipDeletePath(p);
	line(c, 6.5f, 10.5f, 14.5f, 10.5f, 1.8f, 0xFF000000);
	if (in) line(c, 10.5f, 6.5f, 10.5f, 14.5f, 1.8f, 0xFF000000);
}

// A column's (or row's) resizing: two bars, an arrow out from each.
static void split(canvas *c, bool vertical) {
	float a[] = {1, 16, 7, 10, 7, 14, 12, 14, 12, 18, 7, 18, 7, 22};
	float b[14];
	for (int i = 0; i < 7; i++) b[i * 2] = 32 - a[i * 2], b[i * 2 + 1] = a[i * 2 + 1];
	float bars[2][4] = {{13, 5, 2.5f, 22}, {16.5f, 5, 2.5f, 22}};
	if (vertical) {
		for (int i = 0; i < 7; i++) {
			float x = a[i * 2];
			a[i * 2] = a[i * 2 + 1], a[i * 2 + 1] = x;
			x = b[i * 2];
			b[i * 2] = b[i * 2 + 1], b[i * 2 + 1] = x;
		}
		for (int i = 0; i < 2; i++) {
			float x = bars[i][0], w = bars[i][2];
			bars[i][0] = bars[i][1], bars[i][1] = x, bars[i][2] = bars[i][3], bars[i][3] = w;
		}
	}
	poly(c, a, 7, false);
	poly(c, b, 7, false);
	for (int i = 0; i < 2; i++) rrect(c, bars[i][0], bars[i][1], bars[i][2], bars[i][3], 0.5f, false);
}

static HCURSOR draw(int32_t which, int size) {
	canvas c;
	if (!canvas_open(&c, size)) return NULL;
	float hx = 1, hy = 1;
	switch (which) {
	case AK_CURSOR_COPY: arrow(&c), badge(&c, 0); break;
	case AK_CURSOR_ALIAS: arrow(&c), badge(&c, 1); break;
	case AK_CURSOR_CONTEXT_MENU: arrow(&c), badge(&c, 2); break;
	case AK_CURSOR_GRAB: hand(&c, false), hx = 16, hy = 14; break;
	case AK_CURSOR_GRABBING: hand(&c, true), hx = 16, hy = 16; break;
	case AK_CURSOR_ZOOM_IN: magnifier(&c, true), hx = hy = 10.5f; break;
	case AK_CURSOR_ZOOM_OUT: magnifier(&c, false), hx = hy = 10.5f; break;
	case AK_CURSOR_RESIZE_COLUMN: split(&c, false), hx = hy = 16; break;
	case AK_CURSOR_RESIZE_ROW: split(&c, true), hx = hy = 16; break;
	}
	return canvas_close(&c, hx, hy);
}

// load is the cursor of a Cursor value: the system's, or one drawn (at
// the system's cursor size for the main window's DPI); NULL: hidden.
static HCURSOR load(int32_t c) {
	const wchar_t *id = NULL;
	switch (c) {
	case AK_CURSOR_ARROW: id = IDC_ARROW; break;
	case AK_CURSOR_POINTER: id = IDC_HAND; break;
	case AK_CURSOR_TEXT: id = IDC_IBEAM; break;
	case AK_CURSOR_CROSSHAIR: id = IDC_CROSS; break;
	case AK_CURSOR_MOVE: id = IDC_SIZEALL; break;
	case AK_CURSOR_NOT_ALLOWED: id = IDC_NO; break;
	case AK_CURSOR_WAIT: id = IDC_WAIT; break;
	case AK_CURSOR_PROGRESS: id = IDC_APPSTARTING; break;
	case AK_CURSOR_HELP: id = IDC_HELP; break;
	case AK_CURSOR_RESIZE_HORIZONTAL: id = IDC_SIZEWE; break;
	case AK_CURSOR_RESIZE_VERTICAL: id = IDC_SIZENS; break;
	case AK_CURSOR_RESIZE_DIAGONAL: id = IDC_SIZENWSE; break;
	case AK_CURSOR_RESIZE_ANTI_DIAGONAL: id = IDC_SIZENESW; break;
	case AK_CURSOR_HIDDEN: return NULL;
	}
	if (id) return LoadCursorW(NULL, id);
	int size = wh_system_metric(SM_CXCURSOR, H.dpi);
	if (size <= 0) size = 32;
	if (size != drawn_size) {
		for (int i = 0; i < AK_CURSOR_COUNT; i++)
			if (drawn[i]) DestroyCursor(drawn[i]), drawn[i] = NULL;
		drawn_size = size;
	}
	if (!drawn[c]) drawn[c] = draw(c, size);
	return drawn[c] ? drawn[c] : LoadCursorW(NULL, IDC_ARROW);
}

// keeps_own reports whether n, with no cursor of its own, keeps the one
// its window gives it: a field's I-beam, a table's header's resizing.
static bool keeps_own(const node *n) {
	switch (n->kind) {
	case AK_KIND_TEXT_FIELD: case AK_KIND_TABLE: case AK_KIND_OUTLINE: case AK_KIND_WEB_VIEW: case AK_KIND_VIDEO: case AK_KIND_NATIVE:
		return true;
	case AK_KIND_RICH_TEXT:
		return true;  // its links' hand, its selection's I-beam
	}
	return false;
}

static node *up(node *n) {
	if (n->kind == KIND_TABLE_CELL) return wh_table_of(n);
	if (n->cell) return wh_cell_list(n);
	return n->parent;
}

// wh_cursor_message answers WM_SETCURSOR for one of the host's views
// (hwnd): true when a view's Cursor is shown.
bool wh_cursor_message(HWND hwnd, WPARAM wp, LPARAM lp) {
	(void)hwnd;
	if (H.cursors <= 0 || LOWORD(lp) != HTCLIENT) return false;
	HWND under = (HWND)wp;
	POINT p;
	GetCursorPos(&p);
	// The deepest window there, through the views that let the mouse go
	// through them (a Text in a row).
	for (int i = 0; i < 64; i++) {
		POINT c = p;
		ScreenToClient(under, &c);
		HWND child = ChildWindowFromPointEx(under, c, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
		if (!child || child == under) break;
		under = child;
	}
	node *n = NULL;
	for (HWND w = under; w && !n; w = GetParent(w)) n = wh_node_of(w);
	for (; n; n = up(n)) {
		if (n->cursor) {
			SetCursor(load(n->cursor));
			return true;
		}
		if (keeps_own(n)) return false;
	}
	return false;
}

// wh_cursor_name names the cursor shown (for the automation's dump).
const char *wh_cursor_name(HCURSOR c) {
	static const struct {
		const wchar_t *id;
		const char *name;
	} stock[] = {
		{IDC_ARROW, "arrow"}, {IDC_HAND, "hand"}, {IDC_IBEAM, "ibeam"}, {IDC_CROSS, "cross"}, {IDC_SIZEALL, "sizeall"},
		{IDC_NO, "no"}, {IDC_WAIT, "wait"}, {IDC_APPSTARTING, "appstarting"}, {IDC_HELP, "help"}, {IDC_SIZEWE, "sizewe"},
		{IDC_SIZENS, "sizens"}, {IDC_SIZENWSE, "sizenwse"}, {IDC_SIZENESW, "sizenesw"},
	};
	if (!c) return "none";
	for (size_t i = 0; i < WH_LEN(stock); i++)
		if (LoadCursorW(NULL, stock[i].id) == c) return stock[i].name;
	static const char *const names[AK_CURSOR_COUNT] = {
		[AK_CURSOR_COPY] = "copy", [AK_CURSOR_ALIAS] = "alias", [AK_CURSOR_CONTEXT_MENU] = "context-menu", [AK_CURSOR_GRAB] = "grab",
		[AK_CURSOR_GRABBING] = "grabbing", [AK_CURSOR_ZOOM_IN] = "zoom-in", [AK_CURSOR_ZOOM_OUT] = "zoom-out",
		[AK_CURSOR_RESIZE_COLUMN] = "col-resize", [AK_CURSOR_RESIZE_ROW] = "row-resize",
	};
	for (int i = 0; i < AK_CURSOR_COUNT; i++)
		if (drawn[i] == c && names[i]) return names[i];
	return "other";
}
