// modutil.c: what the modules share: the files a call names (platform/
// desktop's asuka_desktop_local_file), images through WIC, icons drawn
// from pixels and glyphs.
#include "modules.h"

#include <wincodec.h>

// ---- The files a call names ----

char *wh_local_file(const char *source, char **err) {
	*err = NULL;
	asuka_error e = {0};
	char *p = asuka_desktop_local_file(source, "", &e);
	if (!p) *err = wh_strdup(e.message ? e.message : "no such file");
	asuka_error_free(&e);
	return p;
}

// ---- Images (WIC) ----

static IWICImagingFactory *wic(void) {
	IWICImagingFactory *f = NULL;
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
	return f;
}

// orientation is a frame's EXIF orientation (1 to 8; 1 if none).
static int orientation(IWICBitmapFrameDecode *frame) {
	IWICMetadataQueryReader *q = NULL;
	if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &q)) || !q) return 1;
	const wchar_t *names[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation"};
	int o = 1;
	for (size_t i = 0; i < WH_LEN(names); i++) {
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(q, names[i], &v))) {
			if (v.vt == VT_UI2) o = v.uiVal;
			PropVariantClear(&v);
			if (o >= 1 && o <= 8) break;
		}
	}
	IWICMetadataQueryReader_Release(q);
	return o >= 1 && o <= 8 ? o : 1;
}

static IWICBitmapFrameDecode *open_frame(IWICImagingFactory *f, const wchar_t *path, int *o) {
	IWICBitmapDecoder *d = NULL;
	if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) || !d) return NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICBitmapDecoder_GetFrame(d, 0, &frame);
	IWICBitmapDecoder_Release(d);
	if (frame && o) *o = orientation(frame);
	return frame;
}

bool wh_image_file_size(const wchar_t *path, int *w, int *h) {
	IWICImagingFactory *f = wic();
	if (!f) return false;
	int o = 1;
	IWICBitmapFrameDecode *frame = open_frame(f, path, &o);
	bool ok = false;
	if (frame) {
		UINT fw = 0, fh = 0;
		IWICBitmapFrameDecode_GetSize(frame, &fw, &fh);
		if (o >= 5) *w = (int)fh, *h = (int)fw;
		else *w = (int)fw, *h = (int)fh;
		ok = true;
		IWICBitmapFrameDecode_Release(frame);
	}
	IWICImagingFactory_Release(f);
	return ok;
}

bool wh_decode_image_file(const wchar_t *path, int max, uint8_t **px, int *w, int *h) {
	*px = NULL;
	IWICImagingFactory *f = wic();
	if (!f) return false;
	int o = 1;
	IWICBitmapFrameDecode *frame = open_frame(f, path, &o);
	bool ok = false;
	if (frame) {
		IWICBitmapSource *src = (IWICBitmapSource *)frame;
		IWICBitmapSource_AddRef(src);
		UINT fw = 0, fh = 0;
		IWICBitmapSource_GetSize(src, &fw, &fh);
		if (max > 0 && (fw > (UINT)max || fh > (UINT)max)) {
			double s = (double)max / (double)(fw > fh ? fw : fh);
			UINT nw = (UINT)(fw * s + 0.5), nh = (UINT)(fh * s + 0.5);
			IWICBitmapScaler *sc = NULL;
			if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) &&
				SUCCEEDED(IWICBitmapScaler_Initialize(sc, src, nw ? nw : 1, nh ? nh : 1, WICBitmapInterpolationModeFant))) {
				IWICBitmapSource_Release(src);
				src = (IWICBitmapSource *)sc;
			} else if (sc) IWICBitmapScaler_Release(sc);
		}
		if (o != 1) {
			static const WICBitmapTransformOptions t[9] = {0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
				WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
				WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal, WICBitmapTransformRotate90,
				WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal, WICBitmapTransformRotate270};
			IWICBitmapFlipRotator *r = NULL;
			if (SUCCEEDED(IWICImagingFactory_CreateBitmapFlipRotator(f, &r)) && SUCCEEDED(IWICBitmapFlipRotator_Initialize(r, src, t[o]))) {
				IWICBitmapSource_Release(src);
				src = (IWICBitmapSource *)r;
			} else if (r) IWICBitmapFlipRotator_Release(r);
		}
		IWICFormatConverter *cv = NULL;
		if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
			SUCCEEDED(IWICFormatConverter_Initialize(cv, src, &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
			UINT cw = 0, ch = 0;
			IWICFormatConverter_GetSize(cv, &cw, &ch);
			uint8_t *buf = malloc((size_t)cw * ch * 4 + 1);
			if (buf && SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, cw * 4, cw * ch * 4, buf))) {
				*px = buf, *w = (int)cw, *h = (int)ch;
				ok = true;
			} else free(buf);
		}
		if (cv) IWICFormatConverter_Release(cv);
		IWICBitmapSource_Release(src);
		IWICBitmapFrameDecode_Release(frame);
	}
	IWICImagingFactory_Release(f);
	return ok;
}

bool wh_encode_image_file(const wchar_t *path, bool jpeg, const uint8_t *px, int w, int h) {
	IWICImagingFactory *f = wic();
	if (!f) return false;
	bool ok = false;
	IWICStream *st = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *frame = NULL;
	IPropertyBag2 *props = NULL;
	if (FAILED(IWICImagingFactory_CreateStream(f, &st)) || FAILED(IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE))) goto out;
	if (FAILED(IWICImagingFactory_CreateEncoder(f, jpeg ? &GUID_ContainerFormatJpeg : &GUID_ContainerFormatPng, NULL, &enc))) goto out;
	if (FAILED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache))) goto out;
	if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props))) goto out;
	if (jpeg && props) {
		PROPBAG2 opt = {0};
		opt.pstrName = (LPOLESTR)L"ImageQuality";
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_R4, v.fltVal = 0.8f;
		IPropertyBag2_Write(props, 1, &opt, &v);
	}
	if (FAILED(IWICBitmapFrameEncode_Initialize(frame, props))) goto out;
	IWICBitmapFrameEncode_SetSize(frame, (UINT)w, (UINT)h);
	WICPixelFormatGUID fmt = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
	IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt);
	if (jpeg) {
		// On white: a JPEG has no alpha.
		uint8_t *rgb = malloc((size_t)w * h * 3 + 1);
		if (!rgb) goto out;
		for (int i = 0; i < w * h; i++) {
			unsigned a = px[i * 4 + 3];
			for (int c = 0; c < 3; c++) rgb[i * 3 + c] = (uint8_t)((px[i * 4 + c] * a + 255 * (255 - a)) / 255);
		}
		ok = SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, (UINT)h, (UINT)w * 3, (UINT)(w * h * 3), rgb));
		free(rgb);
	} else {
		ok = SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, (UINT)h, (UINT)w * 4, (UINT)(w * h * 4), (BYTE *)px));
	}
	ok = ok && SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc));
out:
	if (props) IPropertyBag2_Release(props);
	if (frame) IWICBitmapFrameEncode_Release(frame);
	if (enc) IWICBitmapEncoder_Release(enc);
	if (st) IWICStream_Release(st);
	IWICImagingFactory_Release(f);
	if (!ok) DeleteFileW(path);
	return ok;
}

// ---- Icons ----

// dib is a top-down 32-bit DIB section of w×h, its bits in *bits.
static HBITMAP dib(int w, int h, void **bits) {
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

HICON wh_icon_from_pixels(const uint8_t *px, int w, int h) {
	void *bits = NULL;
	HBITMAP bmp = dib(w, h, &bits);
	if (!bmp) return NULL;
	memcpy(bits, px, (size_t)w * h * 4);
	HBITMAP mask = CreateBitmap(w, h, 1, 1, NULL);
	ICONINFO ii = {TRUE, 0, 0, mask, bmp};
	HICON icon = CreateIconIndirect(&ii);
	DeleteObject(mask);
	DeleteObject(bmp);
	return icon;
}

uint8_t *wh_glyph_coverage(uint16_t glyph, const wchar_t *face, int size) {
	uint8_t *out = wh_alloc((size_t)size * size);
	HDC dc = CreateCompatibleDC(NULL);
	void *bits = NULL;
	HBITMAP bmp = dc ? dib(size, size, &bits) : NULL;
	if (!bmp) {
		if (dc) DeleteDC(dc);
		return out;
	}
	HGDIOBJ old = SelectObject(dc, bmp);
	LOGFONTW lf = {0};
	lf.lfHeight = -size, lf.lfWeight = FW_NORMAL, lf.lfQuality = ANTIALIASED_QUALITY;  // gray, not ClearType's colors
	wcsncpy(lf.lfFaceName, face, 31);
	HFONT font = CreateFontIndirectW(&lf);
	HGDIOBJ of = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	RECT r = {0, 0, size, size};
	wchar_t g[2] = {glyph, 0};
	DrawTextW(dc, g, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	GdiFlush();
	SelectObject(dc, of);
	DeleteObject(font);
	const uint8_t *p = bits;
	for (int i = 0; i < size * size; i++) out[i] = p[i * 4 + 1];
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
	return out;
}

