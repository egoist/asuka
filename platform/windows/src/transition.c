// transition.c: a stack's push and pop, animated as WinUI's Frame does:
// a pushed screen comes in as its EntranceNavigationTransitionInfo (up
// from a little below, fading in), a popped one goes out the same way
// back; a screen with zoomFrom grows out of the view below that shares
// its key and shrinks back into it, as DrillInNavigationTransitionInfo
// zooms. The screens are child windows, so what moves is a picture of
// them: a layered popup over the stack's screen area, transparent to the
// mouse, shows the screen before the change, the stack changes under it,
// then the popup draws the two pictures in between, frame by frame
// (AlphaBlend scales and fades), and goes. Reduced motion (Windows' "show
// animations" off) changes at once.
#include "nav.h"

#define TR_CLASS L"AsukaTransition"
#define TR_MS 280.0     // how long it takes
#define TR_SHIFT 40.0f  // how far the entrance comes from, in points

static struct {
	HWND pop;   // the popup (NULL: none running)
	HWND top;   // the stack's top-level window
	node *stack;
	RECT area;  // the screens' area, on the screen (pixels)
	int w, h;
	HDC before, after, frame;  // memory DCs, each with a w×h DIB
	HBITMAP bbefore, bafter, bframe;
	bool push, captured;
	int ticks;
	char *zoom;  // the zoomFrom key ("": none)
	RECT src;    // the zoom's view, in the area (pixels); empty: none
	double start;
	float k;
} T;

// duration is TR_MS, or ASUKA_SLOW_ANIMATIONS times it (a debug knob, as
// the simulator's Slow Animations: to see one, or catch it in a picture).
static double duration(void) {
	const char *slow = getenv("ASUKA_SLOW_ANIMATIONS");
	double k = slow ? atof(slow) : 1;
	return TR_MS * (k >= 1 ? k : 1);
}

static double now_ms(void) {
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static HDC dib_dc(int w, int h, HBITMAP *bmp) {
	HDC screen = GetDC(NULL);
	HDC dc = CreateCompatibleDC(screen);
	ReleaseDC(NULL, screen);
	BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
	void *px = NULL;
	*bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &px, NULL, 0);
	if (*bmp) SelectObject(dc, *bmp);
	return dc;
}

static void free_dc(HDC *dc, HBITMAP *bmp) {
	if (*dc) DeleteDC(*dc);
	if (*bmp) DeleteObject(*bmp);
	*dc = NULL;
	*bmp = NULL;
}

// grab draws what the top-level window shows at T.area into dc.
static void grab(HDC dc) {
	RECT wr;
	GetWindowRect(T.top, &wr);
	int tw = wr.right - wr.left, th = wr.bottom - wr.top;
	HBITMAP bmp;
	HDC all = dib_dc(tw, th, &bmp);
	PrintWindow(T.top, all, PW_RENDERFULLCONTENT);
	BitBlt(dc, 0, 0, T.w, T.h, all, T.area.left - wr.left, T.area.top - wr.top, SRCCOPY);
	free_dc(&all, &bmp);
}

// The view on screen (under) that shares key, and its rect in the area.
static bool find_src(node *n, const char *key, RECT *out) {
	if (!n || n->dead) return false;
	if (n->match && !strcmp(n->match, key) && n->hwnd && IsWindowVisible(n->hwnd)) {
		RECT r;
		GetWindowRect(n->hwnd, &r);
		if (r.right > r.left && r.bottom > r.top) {
			OffsetRect(&r, -T.area.left, -T.area.top);
			*out = r;
			return true;
		}
	}
	for (uint32_t i = 0; i < n->nkids; i++)
		if (find_src(n->kids[i], key, out)) return true;
	return false;
}

static void finish(void) {
	if (!T.pop) return;
	KillTimer(T.pop, 1);
	DestroyWindow(T.pop);
	T.pop = NULL;
	free_dc(&T.before, &T.bbefore);
	free_dc(&T.after, &T.bafter);
	free_dc(&T.frame, &T.bframe);
	free(T.zoom);
	T.zoom = NULL;
}

static void show(HDC dc) {
	POINT at = {T.area.left, T.area.top}, src = {0, 0};
	SIZE size = {T.w, T.h};
	BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, 0};
	HDC screen = GetDC(NULL);
	UpdateLayeredWindow(T.pop, screen, &at, &size, dc, &src, 0, &bf, ULW_ALPHA);
	ReleaseDC(NULL, screen);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// blend draws dc's picture into the frame at r, faded to alpha (0 to 1).
static void blend(HDC dc, RECT r, float alpha) {
	if (alpha <= 0 || r.right <= r.left || r.bottom <= r.top) return;
	BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)lroundf(fminf(alpha, 1) * 255), 0};
	SetStretchBltMode(T.frame, HALFTONE);
	AlphaBlend(T.frame, r.left, r.top, r.right - r.left, r.bottom - r.top, dc, 0, 0, T.w, T.h, bf);
}

static void draw(float t) {
	// WinUI's curve: quick out, slow in (a cubic ease out).
	float e = 1 - powf(1 - t, 3);
	RECT full = {0, 0, T.w, T.h};
	bool zoom = !IsRectEmpty(&T.src);
	HDC under = T.push ? T.before : T.after, over = T.push ? T.after : T.before;
	BitBlt(T.frame, 0, 0, T.w, T.h, under, 0, 0, SRCCOPY);
	float in = T.push ? e : 1 - e;  // how far the moving screen is in
	if (zoom) {
		RECT r = {(int)lerpf((float)T.src.left, 0, in), (int)lerpf((float)T.src.top, 0, in), (int)lerpf((float)T.src.right, (float)T.w, in),
		          (int)lerpf((float)T.src.bottom, (float)T.h, in)};
		blend(over, r, fminf(1, in * 2));
	} else {
		int dy = (int)lroundf((1 - in) * TR_SHIFT * T.k);
		RECT r = full;
		OffsetRect(&r, 0, dy);
		blend(over, r, in);
	}
	show(T.frame);
}

static LRESULT CALLBACK tr_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	switch (m) {
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_TIMER:
		if (hwnd != T.pop) return 0;
		if (!T.captured) {
			// The stack has changed under the popup, laid out and drawn a
			// frame or two later: its picture.
			if (T.ticks++ < 1) return 0;
			grab(T.after);
			if (!T.push && T.zoom && !find_src(T.stack, T.zoom, &T.src)) SetRectEmpty(&T.src);
			T.captured = true;
			T.start = now_ms();
		}
		double t = (now_ms() - T.start) / duration();
		if (t >= 1) {
			finish();
			return 0;
		}
		draw((float)t);
		return 0;
	}
	return DefWindowProcW(hwnd, m, wp, lp);
}

// wh_transition_begin covers the stack's screen area with a picture of
// what it shows now (shown: the screen showing), before a push or a pop
// (screen: the one pushed, or the one popped, whose zoomFrom says how);
// the change happens under it, and the picture moves to what it becomes.
void wh_transition_begin(node *stack, node *shown, node *screen, bool push) {
	finish();
	if (H.env.env.reduce_motion || !stack || !stack->hwnd || !screen || !IsWindowVisible(stack->hwnd)) return;
	BOOL anim = TRUE;
	if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim) return;
	// The screens' area: the stack's, under its header (the shown screen's).
	if (!shown || !shown->hwnd || !IsWindowVisible(shown->hwnd)) return;
	GetWindowRect(shown->hwnd, &T.area);
	T.w = T.area.right - T.area.left, T.h = T.area.bottom - T.area.top;
	if (T.w <= 0 || T.h <= 0) return;
	T.top = GetAncestor(stack->hwnd, GA_ROOT);
	T.stack = stack;
	T.push = push;
	T.captured = false;
	T.ticks = 0;
	T.k = (float)wh_dpi(stack->hwnd) / 96;
	SetRectEmpty(&T.src);
	const char *key = screen->nav && screen->nav->zoom_from ? screen->nav->zoom_from : "";
	T.zoom = *key ? wh_strndup(key, strlen(key)) : NULL;
	// A push zooms out of a view on the screen shown now; a pop into one
	// on the screen under it, found once that shows.
	if (push && T.zoom && !find_src(shown, T.zoom, &T.src)) SetRectEmpty(&T.src);
	T.before = dib_dc(T.w, T.h, &T.bbefore);
	T.after = dib_dc(T.w, T.h, &T.bafter);
	T.frame = dib_dc(T.w, T.h, &T.bframe);
	grab(T.before);
	static bool registered;
	HINSTANCE inst = GetModuleHandleW(NULL);
	if (!registered) {
		WNDCLASSEXW wc = {sizeof wc};
		wc.lpfnWndProc = tr_proc;
		wc.hInstance = inst;
		wc.lpszClassName = TR_CLASS;
		RegisterClassExW(&wc);
		registered = true;
	}
	T.pop = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, TR_CLASS, L"", WS_POPUP, T.area.left,
	                        T.area.top, T.w, T.h, T.top, NULL, inst, NULL);
	if (!T.pop) {
		finish();
		return;
	}
	show(T.before);
	ShowWindow(T.pop, SW_SHOWNOACTIVATE);
	// A frame's time, then the picture of what it became: the core lays
	// the pushed screen out in the batch after the one that pushed it.
	SetTimer(T.pop, 1, 16, NULL);
}

// wh_transition_end ends a transition running in stack (or any: NULL).
void wh_transition_end(node *stack) {
	if (T.pop && (!stack || stack == T.stack)) finish();
}
