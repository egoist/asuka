// capture.c: a view drawn into an image file (imageedit.capture): the
// "view" module's "capture". The view's top-level window as it shows
// (PrintWindow, its WebView2 and video surfaces too), cropped to the view,
// encoded by WIC as PNG or JPEG into the app's caches. args: u32 the
// native view, f32 x, y, width, height of what's drawn in it (points; a
// width below 0: all of it), u8 format (0 JPEG, 1 PNG), f32 quality, f32
// scale (Windows draws at the screen's); answers the file's path.
#include "modules.h"

#include <wincodec.h>

static float f32_at(const uint8_t *a) {
	uint32_t u = wh_u32(a);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}

// write_image encodes w by h BGRA pixels (top row first) as a PNG or a
// JPEG at quality (WIC converts them to the encoder's format).
static bool write_image(const char *path, int w, int h, uint8_t *bgra, int stride, bool png, float quality) {
	IWICImagingFactory *f = NULL;
	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) return false;
	bool ok = false;
	IWICBitmap *bmp = NULL;
	IWICStream *s = NULL;
	IWICBitmapEncoder *e = NULL;
	IWICBitmapFrameEncode *fr = NULL;
	IPropertyBag2 *props = NULL;
	wchar_t *wp = wh_wide(path, (int)strlen(path));
	if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(f, (UINT)w, (UINT)h, &GUID_WICPixelFormat32bppBGRA, (UINT)stride, (UINT)(stride * h), bgra, &bmp)) &&
		SUCCEEDED(IWICImagingFactory_CreateStream(f, &s)) && SUCCEEDED(IWICStream_InitializeFromFilename(s, wp, GENERIC_WRITE)) &&
		SUCCEEDED(IWICImagingFactory_CreateEncoder(f, png ? &GUID_ContainerFormatPng : &GUID_ContainerFormatJpeg, NULL, &e)) &&
		SUCCEEDED(IWICBitmapEncoder_Initialize(e, (IStream *)s, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(e, &fr, &props))) {
		if (!png && props) {
			PROPBAG2 opt = {0};
			opt.pstrName = L"ImageQuality";
			VARIANT v;
			VariantInit(&v);
			v.vt = VT_R4;
			v.fltVal = quality;
			IPropertyBag2_Write(props, 1, &opt, &v);
		}
		if (SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, props)) && SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, (UINT)w, (UINT)h)) &&
			SUCCEEDED(IWICBitmapFrameEncode_WriteSource(fr, (IWICBitmapSource *)bmp, NULL)) && SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) &&
			SUCCEEDED(IWICBitmapEncoder_Commit(e)))
			ok = true;
	}
	free(wp);
	if (props) IPropertyBag2_Release(props);
	if (fr) IWICBitmapFrameEncode_Release(fr);
	if (e) IWICBitmapEncoder_Release(e);
	if (s) IWICStream_Release(s);
	if (bmp) IWICBitmap_Release(bmp);
	IWICImagingFactory_Release(f);
	return ok;
}

static void m_capture(wh_reply *r, const uint8_t *a, size_t n) {
	if (n < 29) {
		wh_reply_fail(r, "bad arguments");
		return;
	}
	node *v = wh_node(wh_u32(a));
	float rect[4] = {f32_at(a + 4), f32_at(a + 8), f32_at(a + 12), f32_at(a + 16)};
	bool png = a[20] == 1;
	float quality = f32_at(a + 21);
	// A view drawn in its parent's window: its rect in the nearest that has one.
	while (v && !v->hwnd) {
		if (rect[2] < 0 || rect[3] < 0) rect[0] = rect[1] = 0, rect[2] = v->frame[2], rect[3] = v->frame[3];
		rect[0] += v->frame[0];
		rect[1] += v->frame[1];
		v = v->parent;
	}
	if (!v || !IsWindowVisible(v->hwnd)) {
		wh_reply_fail(r, "capture: the view isn't showing");
		return;
	}
	float k = (float)wh_dpi(v->hwnd) / 96;
	RECT cr;
	GetClientRect(v->hwnd, &cr);
	POINT at = {0, 0};
	int w = cr.right, h = cr.bottom;
	if (rect[2] >= 0 && rect[3] >= 0) {
		at.x = (int)lroundf(rect[0] * k), at.y = (int)lroundf(rect[1] * k);
		w = (int)lroundf(rect[2] * k), h = (int)lroundf(rect[3] * k);
	}
	if (w < 1 || h < 1) {
		wh_reply_fail(r, "capture: the view has no size");
		return;
	}
	// The top-level window, all of it, then the view's part of it.
	HWND top = GetAncestor(v->hwnd, GA_ROOT);
	RECT wr;
	GetWindowRect(top, &wr);
	ClientToScreen(v->hwnd, &at);
	int tw = wr.right - wr.left, th = wr.bottom - wr.top, x0 = at.x - wr.left, y0 = at.y - wr.top;
	if (x0 < 0) w += x0, x0 = 0;
	if (y0 < 0) h += y0, y0 = 0;
	if (x0 + w > tw) w = tw - x0;
	if (y0 + h > th) h = th - y0;
	if (w < 1 || h < 1 || tw < 1 || th < 1) {
		wh_reply_fail(r, "capture: the view is out of its window");
		return;
	}
	HDC screen = GetDC(NULL);
	HDC dc = CreateCompatibleDC(screen);
	ReleaseDC(NULL, screen);
	BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), tw, -th, 1, 32, BI_RGB}};
	uint8_t *px = NULL;
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
	if (!bmp || !px) {
		DeleteDC(dc);
		wh_reply_fail(r, "capture: the image couldn't be made");
		return;
	}
	HGDIOBJ old = SelectObject(dc, bmp);
	PrintWindow(top, dc, PW_RENDERFULLCONTENT);
	GdiFlush();
	for (size_t i = 0; i < (size_t)tw * (size_t)th; i++) px[i * 4 + 3] = 255;  // GDI leaves alpha 0
	char dir[MAX_PATH * 3], path[MAX_PATH * 3];
	snprintf(dir, sizeof dir, "%s\\asuka-capture", asuka_desktop_cache_dir());
	dsk_mkdirs(dir);
	snprintf(path, sizeof path, "%s\\%08lx%08lx.%s", dir, (unsigned long)GetTickCount64(), (unsigned long)GetCurrentProcessId() ^ (unsigned long)(uintptr_t)bmp, png ? "png" : "jpg");
	bool ok = write_image(path, w, h, px + ((size_t)y0 * (size_t)tw + (size_t)x0) * 4, tw * 4, png, quality);
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
	if (ok) wh_reply_str(r, path);
	else wh_reply_fail(r, "capture: the image couldn't be written");
}

static const wh_method_def view_methods[] = {{"capture", m_capture}, {NULL, NULL}};

const wh_module_def wh_view_module = {"view", view_methods};
