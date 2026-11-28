// platform/device on Windows: the open dialog (modal to the window the user
// works in), ShellExecute for the browser, the clipboard (its changes
// heard with AddClipboardFormatListener), SetThreadExecutionState.
// Win32 is touched on the main thread; copying what's picked runs on a
// thread of its own. pickMedia's dialog offers images and/or videos; a
// photo scaled (maxSize, quality) is written with WIC, upright, and a
// video the MPEG-4 reader can't read is measured with Media Foundation's
// source reader.
#include "asuka_windows.h"
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>
#include "device.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void fail(asuka_call *call, asuka_error *e) { asuka_call_fail(call, e); }

static DWORD build(void) {
	RTL_OSVERSIONINFOW v = {sizeof v};
	LONG(WINAPI * rtl)(RTL_OSVERSIONINFOW *) = NULL;
	*(FARPROC *)&rtl = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	return rtl && rtl(&v) == 0 ? v.dwBuildNumber : 0;
}

// file_ms is a file's creation (or last write) time, in ms since 1970;
// 0 when it can't be read.
static int64_t file_ms(const wchar_t *path, bool created) {
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return 0;
	FILETIME ft = created ? a.ftCreationTime : a.ftLastWriteTime;
	uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;  // 100 ns since 1601
	return t ? (int64_t)(t / 10000) - INT64_C(11644473600000) : 0;
}

static void p_info(device_Platform *self, asuka_call *call) {
	// Where it came from: a package's (the Microsoft Store's, an MSIX
	// installer's), asuka dev's, else a download or an installer.
	device_InstallSource source = device_InstallSource_direct;
	typedef LONG(WINAPI * full_name_fn)(UINT32 *, PWSTR);
	full_name_fn full = (full_name_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFullName");
	UINT32 n = 0;
	if (full && full(&n, NULL) != 15700L /* APPMODEL_ERROR_NO_PACKAGE */) source = device_InstallSource_otherStore;
	if (getenv("ASUKA_DEV")) source = device_InstallSource_development;
	// Installed as the app's data folder was made (its first run), updated
	// as its executable was written.
	wchar_t exe[MAX_PATH];
	int64_t updated = GetModuleFileNameW(NULL, exe, MAX_PATH) ? file_ms(exe, false) : 0;
	wchar_t *data = asuka_wide(asuka_desktop_data_dir());
	int64_t installed = file_ms(data, true);
	free(data);
	if (!installed || (updated && installed > updated)) installed = updated;
	device_NativeInfo i = {.model = asuka_strdup("PC"), .system = asuka_strdup("Windows"),
	                       .systemVersion = asuka_sprintf("10.0.%lu", (unsigned long)build()),
	                       .appVersion = asuka_strdup(asuka_desktop_app_version()), .build = asuka_strdup(asuka_desktop_app_build()),
	                       .source = source, .installedMs = installed, .updatedMs = updated};
	device_Platform_info_done(call, i);
}

static void p_browse(device_Platform *self, const char *url, asuka_call *call) {
	if (!device_web_url(url)) {
		asuka_error e = {0};
		asuka_error_failed(&e, "device", 0, "device: not a web URL: %s", url);
		return fail(call, &e);
	}
	wchar_t *w = asuka_wide(url);
	ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
	free(w);
	device_Platform_browse_done(call);
}

// filters are the open dialog's types for MIME types: the extensions of
// each ("image/*" its family's), then any file; NULL: any file.
static wchar_t *filters(device_list_string mimes) {
	static const char *families[][2] = {
	    {"image/*", "*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.heic;*.heif;*.tif;*.tiff;*.ico;*.svg"},
	    {"video/*", "*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv;*.mkv"},
	    {"audio/*", "*.mp3;*.m4a;*.aac;*.wav;*.flac;*.ogg;*.opus;*.wma"},
	    {"text/*", "*.txt;*.csv;*.md;*.html;*.htm;*.xml;*.json;*.log"},
	    {"application/pdf", "*.pdf"},  {"image/png", "*.png"},   {"image/jpeg", "*.jpg;*.jpeg"}, {"image/gif", "*.gif"},
	    {"application/json", "*.json"}, {"text/plain", "*.txt"}, {"text/csv", "*.csv"},          {"application/zip", "*.zip"},
	};
	asuka_writer name = {0}, pats = {0};
	for (size_t i = 0; i < mimes.len; i++) {
		const char *m = mimes.data[i];
		if (!*m || strcmp(m, "*/*") == 0 || strcmp(m, "application/*") == 0) {
			asuka_writer_free(&name);
			asuka_writer_free(&pats);
			return NULL;
		}
		if (name.len) asuka_writer_put(&name, ", ", 2);
		asuka_writer_put(&name, m, strlen(m));
		for (size_t k = 0; k < sizeof families / sizeof *families; k++) {
			if (strcmp(m, families[k][0]) != 0) continue;
			if (pats.len) asuka_writer_put(&pats, ";", 1);
			asuka_writer_put(&pats, families[k][1], strlen(families[k][1]));
		}
	}
	if (!pats.len) {
		asuka_writer_free(&name);
		return NULL;
	}
	asuka_writer_u8(&name, 0);
	asuka_writer_u8(&pats, 0);
	wchar_t *n = asuka_wide((const char *)name.data), *p = asuka_wide((const char *)pats.data);
	size_t nn = wcslen(n), pn = wcslen(p);
	// name\0patterns\0*.*\0*.*\0\0
	wchar_t *out = calloc(nn + pn + 12, sizeof(wchar_t));
	wmemcpy(out, n, nn);
	wmemcpy(out + nn + 1, p, pn);
	wmemcpy(out + nn + pn + 2, L"*.*", 3);
	wmemcpy(out + nn + pn + 6, L"*.*", 3);
	free(n);
	free(p);
	asuka_writer_free(&name);
	asuka_writer_free(&pats);
	return out;
}

typedef struct {
	asuka_call *call;
	bool media;  // pickMedia's: scaled to max_size, of quality
	int64_t max_size;
	double quality;
	size_t limit;
	char **paths;
	size_t n;
} picking;

static const device_media_ops media_ops;

static void copy_run(void *p) {
	picking *k = p;
	asuka_error e = {0};
	if (k->media) {
		device_list_NativeMedia media;
		if (!device_copy_media(k->paths, k->n, k->limit, k->max_size, k->quality, &media_ops, &media, &e)) fail(k->call, &e);
		else device_Platform_pickMedia_done(k->call, media);
	} else {
		device_list_File files;
		if (!device_copy_picked(k->paths, k->n, k->limit, &files, &e)) fail(k->call, &e);
		else device_Platform_pickFiles_done(k->call, files);
	}
	for (size_t i = 0; i < k->n; i++) free(k->paths[i]);
	free(k->paths);
	free(k);
}

static void add(picking *k, char *p) {
	k->paths = realloc(k->paths, (k->n + 1) * sizeof *k->paths);
	k->paths[k->n++] = p;
}

static void pick(asuka_call *call, const char *title, device_list_string mimes, bool multiple, const picking *how) {
	enum { cap = 64 * 1024 };
	wchar_t *buf = calloc(cap, sizeof(wchar_t));
	wchar_t *filter = filters(mimes);
	wchar_t *wt = title ? asuka_wide(title) : NULL;
	OPENFILENAMEW o;
	memset(&o, 0, sizeof o);
	o.lStructSize = sizeof o;
	o.hwndOwner = (HWND)asuka_desktop_window(0);
	o.lpstrFilter = filter;
	o.nFilterIndex = 1;
	o.lpstrFile = buf;
	o.nMaxFile = cap;
	o.lpstrTitle = wt;
	o.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (multiple ? OFN_ALLOWMULTISELECT : 0);
	BOOL ok = GetOpenFileNameW(&o);
	free(filter);
	free(wt);
	picking *k = calloc(1, sizeof *k);
	*k = *how;
	k->call = call;
	if (!ok) {
		free(buf);
		if (CommDlgExtendedError()) {
			free(k);
			asuka_error e = {0};
			asuka_error_failed(&e, "device", 0, "the open dialog failed");
			return fail(call, &e);
		}
		copy_run(k);  // cancelled: none
		return;
	}
	// One file: its path. Several: the folder, then each name, NUL
	// separated, ending in two NULs.
	wchar_t *first = buf;
	wchar_t *next = first + wcslen(first) + 1;
	if (!*next) {
		add(k, asuka_narrow(first, -1));
	} else {
		char *dir = asuka_narrow(first, -1);
		size_t dn = strlen(dir);
		while (dn > 0 && dir[dn - 1] == '\\') dir[--dn] = 0;
		for (wchar_t *n = next; *n; n += wcslen(n) + 1) {
			char *name = asuka_narrow(n, -1);
			add(k, asuka_sprintf("%s\\%s", dir, name));
			free(name);
		}
		free(dir);
	}
	free(buf);
	if (!asuka_desktop_spawn(copy_run, k)) copy_run(k);
}

// ---- Picked photos and videos: WIC for images, Media Foundation's
// source reader for a video the MPEG-4 reader can't read (WebM, MKV,
// AVI, WMV, where Windows has their codecs) ----

static IWICImagingFactory *wic(void) {
	CoInitializeEx(NULL, COINIT_MULTITHREADED);  // a copying thread's
	IWICImagingFactory *f = NULL;
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
	return f;
}

// orientation is a frame's EXIF orientation (1 to 8; 1 none), as WIC
// reads a JPEG's, a TIFF's or a HEIF's.
static USHORT orientation(IWICBitmapFrameDecode *frame) {
	IWICMetadataQueryReader *q = NULL;
	USHORT o = 1;
	if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &q))) return 1;
	static const wchar_t *paths[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation"};
	for (size_t i = 0; i < 3 && o == 1; i++) {
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(q, paths[i], &v)) && v.vt == VT_UI2 && v.uiVal >= 1 && v.uiVal <= 8) o = v.uiVal;
		PropVariantClear(&v);
	}
	IWICMetadataQueryReader_Release(q);
	return o;
}

// open_image decodes an image file's first frame.
static bool open_image(IWICImagingFactory *f, const char *path, IWICBitmapDecoder **dec, IWICBitmapFrameDecode **frame) {
	wchar_t *w = asuka_wide(path);
	*dec = NULL;
	*frame = NULL;
	HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(f, w, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec);
	free(w);
	if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(*dec, 0, frame);
	if (FAILED(hr)) {
		if (*dec) IWICBitmapDecoder_Release(*dec), *dec = NULL;
		*frame = NULL;
		return false;
	}
	return true;
}

static bool win_image_size(const char *path, int64_t *w, int64_t *h) {
	IWICImagingFactory *f = wic();
	if (!f) return false;
	IWICBitmapDecoder *dec;
	IWICBitmapFrameDecode *frame;
	bool ok = open_image(f, path, &dec, &frame);
	if (ok) {
		UINT fw = 0, fh = 0;
		ok = SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &fw, &fh)) && fw && fh;
		bool turned = orientation(frame) >= 5;  // 90° or 270°
		*w = turned ? fh : fw;
		*h = turned ? fw : fh;
		IWICBitmapFrameDecode_Release(frame);
		IWICBitmapDecoder_Release(dec);
	}
	IWICImagingFactory_Release(f);
	return ok;
}

static bool win_jpeg(const char *from, const char *to, int64_t max, double quality, int64_t *ow, int64_t *oh) {
	IWICImagingFactory *f = wic();
	if (!f) return false;
	IWICBitmapDecoder *dec;
	IWICBitmapFrameDecode *frame;
	if (!open_image(f, from, &dec, &frame)) {
		IWICImagingFactory_Release(f);
		return false;
	}
	IWICBitmapSource *src = (IWICBitmapSource *)frame;
	IWICBitmapFlipRotator *rot = NULL;
	IWICBitmapScaler *scaler = NULL;
	IWICFormatConverter *pre = NULL, *bgr = NULL;
	IWICStream *st = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *out = NULL;
	IPropertyBag2 *bag = NULL;
	static const WICBitmapTransformOptions transforms[9] = {
	    WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal, WICBitmapTransformRotate180,
	    WICBitmapTransformFlipVertical, (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
	    WICBitmapTransformRotate90, (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal), WICBitmapTransformRotate270};
	USHORT o = orientation(frame);
	HRESULT hr = S_OK;
	if (o > 1 && SUCCEEDED(hr = IWICImagingFactory_CreateBitmapFlipRotator(f, &rot)) && SUCCEEDED(hr = IWICBitmapFlipRotator_Initialize(rot, src, transforms[o])))
		src = (IWICBitmapSource *)rot;
	UINT w = 0, h = 0;
	if (SUCCEEDED(hr)) hr = IWICBitmapSource_GetSize(src, &w, &h);
	int64_t sw = w, sh = h;
	if (SUCCEEDED(hr)) device_fit(w, h, max, &sw, &sh);
	if (SUCCEEDED(hr) && (sw != (int64_t)w || sh != (int64_t)h)) {
		hr = IWICImagingFactory_CreateBitmapScaler(f, &scaler);
		if (SUCCEEDED(hr)) hr = IWICBitmapScaler_Initialize(scaler, src, (UINT)sw, (UINT)sh, WICBitmapInterpolationModeFant);
		if (SUCCEEDED(hr)) src = (IWICBitmapSource *)scaler;
	}
	// Over black where it's transparent (premultiplied), then BGR: what
	// the JPEG encoder takes.
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &pre);
	if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(pre, src, &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &bgr);
	if (SUCCEEDED(hr))
		hr = IWICFormatConverter_Initialize(bgr, (IWICBitmapSource *)pre, &GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
	wchar_t *wto = asuka_wide(to);
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &st);
	if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, wto, GENERIC_WRITE);
	free(wto);
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &enc);
	if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
	if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &out, &bag);
	if (SUCCEEDED(hr) && bag) {
		PROPBAG2 opt;
		memset(&opt, 0, sizeof opt);
		opt.pstrName = L"ImageQuality";
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_R4;
		v.fltVal = (float)(quality > 0 && quality <= 1 ? quality : 0.9);
		IPropertyBag2_Write(bag, 1, &opt, &v);
	}
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(out, bag);
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(out, (UINT)sw, (UINT)sh);
	WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(out, &fmt);
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(out, (IWICBitmapSource *)bgr, NULL);
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(out);
	if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
	if (bag) IPropertyBag2_Release(bag);
	if (out) IWICBitmapFrameEncode_Release(out);
	if (enc) IWICBitmapEncoder_Release(enc);
	if (st) IWICStream_Release(st);
	if (bgr) IWICFormatConverter_Release(bgr);
	if (pre) IWICFormatConverter_Release(pre);
	if (scaler) IWICBitmapScaler_Release(scaler);
	if (rot) IWICBitmapFlipRotator_Release(rot);
	IWICBitmapFrameDecode_Release(frame);
	IWICBitmapDecoder_Release(dec);
	IWICImagingFactory_Release(f);
	*ow = sw;
	*oh = sh;
	return SUCCEEDED(hr);
}

// Media Foundation's, by slot (its GUIDs are mfuuid's, which isn't
// linked).
static const GUID pd_duration = {0x6c990d33, 0xbb8e, 0x477a, {0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a}};
static const GUID mt_frame_size = {0x1652C33D, 0xD6B2, 0x4012, {0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D}};
static const GUID mt_rotation = {0xc380465d, 0x2271, 0x428c, {0x9b, 0x83, 0xec, 0xea, 0x3b, 0x4a, 0x85, 0xc1}};

static void win_video(const char *path, int64_t *w, int64_t *h, int64_t *ms) {
	static HRESULT(WINAPI * startup)(ULONG, DWORD);
	static HRESULT(WINAPI * reader_from_url)(const WCHAR *, void *, void **);
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	BOOL pending = FALSE;
	if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
		HMODULE plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		HMODULE rw = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (plat) *(FARPROC *)&startup = GetProcAddress(plat, "MFStartup");
		if (rw) *(FARPROC *)&reader_from_url = GetProcAddress(rw, "MFCreateSourceReaderFromURL");
		if (startup && FAILED(startup(0x00020070, 0))) reader_from_url = NULL;  // MF_VERSION, MFSTARTUP_FULL
		InitOnceComplete(&once, 0, NULL);
	}
	if (!reader_from_url) return;  // an N edition without its Media Feature Pack
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	wchar_t *wp = asuka_wide(path);
	void *r = NULL;
	HRESULT hr = reader_from_url(wp, NULL, &r);
	free(wp);
	if (FAILED(hr) || !r) return;
	PROPVARIANT v;
	PropVariantInit(&v);
	// GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION): 100 ns.
	if (SUCCEEDED(ASUKA_SLOT(r, 12, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, const GUID *, PROPVARIANT *))(r, 0xFFFFFFFF, &pd_duration, &v)) &&
	    v.vt == VT_UI8)
		*ms = (int64_t)(v.uhVal.QuadPart / 10000);
	PropVariantClear(&v);
	void *t = NULL;
	// GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0).
	if (SUCCEEDED(ASUKA_SLOT(r, 5, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, DWORD, void **))(r, 0xFFFFFFFC, 0, &t)) && t) {
		UINT64 size = 0;
		UINT32 rot = 0;
		ASUKA_SLOT(t, 8, HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, UINT64 *))(t, &mt_frame_size, &size);  // GetUINT64
		ASUKA_SLOT(t, 7, HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, UINT32 *))(t, &mt_rotation, &rot);    // GetUINT32
		int64_t fw = (int64_t)(size >> 32), fh = (int64_t)(UINT32)size;
		bool turned = rot == 90 || rot == 270;
		*w = turned ? fh : fw;
		*h = turned ? fw : fh;
		asuka_com_release(t);
	}
	asuka_com_release(r);
}

static const device_media_ops media_ops = {win_image_size, win_jpeg, win_video};

static void p_media(device_Platform *self, device_MediaKind kind, int64_t max, bool ordered, int64_t maxSize, double quality, asuka_call *call) {
	(void)ordered;  // the open dialog doesn't number what's picked: its order is the dialog's
	char *image = "image/*", *video = "video/*";
	char *both[] = {image, video};
	device_list_string mimes = kind == device_MediaKind_videos ? (device_list_string){&video, 1}
	                           : kind == device_MediaKind_all  ? (device_list_string){both, 2}
	                                                           : (device_list_string){&image, 1};
	picking how = {.media = true, .max_size = maxSize, .quality = quality, .limit = max > 0 ? (size_t)max : 0};
	pick(call, NULL, mimes, max != 1, &how);
}

static void p_files(device_Platform *self, device_list_string types, bool multiple, asuka_call *call) {
	picking how = {0};
	pick(call, NULL, types, multiple, &how);
}

static UINT cf_png(void) {
	static UINT cf;
	if (!cf) cf = RegisterClipboardFormatW(L"PNG");
	return cf;
}

static UINT cf_url(void) {
	static UINT cf;
	if (!cf) cf = RegisterClipboardFormatW(L"UniformResourceLocatorW");
	return cf;
}

static UINT cf_html(void) {
	static UINT cf;
	if (!cf) cf = RegisterClipboardFormatW(L"HTML Format");
	return cf;
}

// open_clipboard opens it, waiting a little for another app that has it
// open (the clipboard history reads what's just copied).
static bool open_clipboard(void) {
	for (int i = 0; i < 20; i++) {
		if (OpenClipboard((HWND)asuka_desktop_window(0))) return true;
		Sleep(10);
	}
	return false;
}

static void p_text(device_Platform *self, asuka_call *call) {
	char *s = NULL;
	if (IsClipboardFormatAvailable(CF_UNICODETEXT) && open_clipboard()) {
		HANDLE h = GetClipboardData(CF_UNICODETEXT);
		const wchar_t *w = h ? GlobalLock(h) : NULL;
		if (w) {
			// Another app's text: up to its NUL, if it has one, but never
			// past its memory.
			size_t cap = GlobalSize(h) / sizeof(wchar_t), n = 0;
			while (n < cap && w[n]) n++;
			s = n ? asuka_narrow(w, n > INT_MAX ? INT_MAX : (int)n) : asuka_strdup("");
			GlobalUnlock(h);
		}
		CloseClipboard();
	}
	device_Platform_clipboardText_done(call, s);
}

static device_ClipboardKinds kinds(void) {
	device_ClipboardKinds k = {0};
	k.text = IsClipboardFormatAvailable(CF_UNICODETEXT);
	k.image = IsClipboardFormatAvailable(cf_png()) || IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5);
	k.url = IsClipboardFormatAvailable(cf_url()) || IsClipboardFormatAvailable(CF_HDROP);
	k.html = IsClipboardFormatAvailable(cf_html());
	return k;
}

static void p_has(device_Platform *self, asuka_call *call) { device_Platform_clipboardHas_done(call, kinds()); }

// dib_bgra reads a packed DIB (BITMAPINFOHEADER or V5, then its masks or
// palette, then its pixels): 24 and 32 bits, as most images on the
// clipboard are, into top-down premultiplied BGRA.
static uint8_t *dib_bgra(const uint8_t *b, size_t len, UINT *w, UINT *h) {
	if (len < 40) return NULL;
	const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)b;
	LONG width = bi->biWidth, height = bi->biHeight;
	int bpp = bi->biBitCount;
	if (width <= 0 || height == 0 || height < -LONG_MAX || bi->biSize < 40 || bi->biSize > len || (bpp != 24 && bpp != 32)) return NULL;
	bool top_down = height < 0;
	if (top_down) height = -height;
	size_t off = bi->biSize + bi->biClrUsed * 4;
	if (bi->biCompression == BI_BITFIELDS && bi->biSize == 40) off += 12;
	size_t stride = ((size_t)width * bpp / 8 + 3) & ~(size_t)3;
	if (off + stride * (size_t)height > len) return NULL;
	// Alpha counts only if some pixel has it (many apps leave it 0).
	bool alpha = false;
	for (LONG y = 0; bpp == 32 && y < height && !alpha; y++)
		for (LONG x = 0; x < width; x++)
			if (b[off + (size_t)y * stride + (size_t)x * 4 + 3]) {
				alpha = true;
				break;
			}
	uint8_t *out = malloc((size_t)width * height * 4);
	for (LONG y = 0; y < height; y++) {
		const uint8_t *row = b + off + (size_t)(top_down ? y : height - 1 - y) * stride;
		uint8_t *o = out + (size_t)y * width * 4;
		for (LONG x = 0; x < width; x++) {
			const uint8_t *px = row + (size_t)x * (bpp / 8);
			uint8_t a = bpp == 32 && alpha ? px[3] : 255;
			// Premultiplied, as WIC's PBGRA (a DIB with alpha is too).
			o[x * 4] = px[0], o[x * 4 + 1] = px[1], o[x * 4 + 2] = px[2], o[x * 4 + 3] = a;
		}
	}
	*w = (UINT)width;
	*h = (UINT)height;
	return out;
}

static void p_image(device_Platform *self, asuka_call *call) {
	asuka_bytes png = {0};
	if (open_clipboard()) {
		if (IsClipboardFormatAvailable(cf_png())) {
			HANDLE h = GetClipboardData(cf_png());
			const void *p = h ? GlobalLock(h) : NULL;
			size_t n = h ? GlobalSize(h) : 0;
			if (p && n > 8) png = asuka_bytes_copy(p, n);
			if (p) GlobalUnlock(h);
		}
		static const UINT dibs[] = {CF_DIBV5, CF_DIB};
		for (int i = 0; !png.data && i < 2; i++) {
			if (!IsClipboardFormatAvailable(dibs[i])) continue;
			HANDLE h = GetClipboardData(dibs[i]);
			const uint8_t *p = h ? GlobalLock(h) : NULL;
			UINT w = 0, ht = 0;
			uint8_t *px = p ? dib_bgra(p, GlobalSize(h), &w, &ht) : NULL;
			if (p) GlobalUnlock(h);
			if (px) asuka_win_png(w, ht, px, &png), free(px);
		}
		CloseClipboard();
	}
	if (!png.data) {
		device_Platform_clipboardImage_done(call, NULL);
		return;
	}
	asuka_error e = {0};
	char *path = device_write_cache("asuka-clipboard", "png", png.data, png.len, &e);
	free(png.data);
	if (!path) fail(call, &e);
	else device_Platform_clipboardImage_done(call, path);
}

static bool put(UINT cf, const void *p, size_t n) {
	HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
	if (!g) return false;
	memcpy(GlobalLock(g), p, n);
	GlobalUnlock(g);
	if (SetClipboardData(cf, g)) return true;
	GlobalFree(g);
	return false;
}

static void p_copy(device_Platform *self, const char *path, asuka_call *call) {
	UINT w = 0, h = 0;
	uint8_t *px = NULL;
	asuka_error e = {0};
	if (!asuka_win_image_load(path, &w, &h, &px)) {
		asuka_error_failed(&e, "device", 0, "device: not an image: %s", path);
		return fail(call, &e);
	}
	asuka_bytes png = {0};
	asuka_win_png(w, h, px, &png);
	// A bitmap (CF_DIB, which every app reads, and Windows makes the
	// others of), bottom-up, and the PNG (for those that keep
	// transparency).
	size_t n = 40 + (size_t)w * h * 4;
	uint8_t *dib = calloc(1, n);
	BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)dib;
	bi->biSize = 40;
	bi->biWidth = (LONG)w;
	bi->biHeight = (LONG)h;
	bi->biPlanes = 1;
	bi->biBitCount = 32;
	bi->biSizeImage = (DWORD)(n - 40);
	for (UINT y = 0; y < h; y++) memcpy(dib + 40 + (size_t)(h - 1 - y) * w * 4, px + (size_t)y * w * 4, (size_t)w * 4);
	free(px);
	if (!open_clipboard()) {
		asuka_error_failed(&e, "device", 0, "the clipboard is in use");
		fail(call, &e);
	} else {
		EmptyClipboard();
		bool ok = put(CF_DIB, dib, n);
		if (png.data) put(cf_png(), png.data, png.len);
		CloseClipboard();
		if (ok) {
			device_Platform_copyImage_done(call);
		} else {
			asuka_error_failed(&e, "device", 0, "the clipboard took no image");
			fail(call, &e);
		}
	}
	free(dib);
	free(png.data);
}

// offset is a CF_HTML header's field (name:digits), or -1: absent, or
// past the data's n bytes.
static long long offset(const char *head, const char *name, size_t n) {
	const char *f = strstr(head, name);
	if (!f) return -1;
	f += strlen(name);
	long long v = 0;
	if (*f < '0' || *f > '9') return -1;
	for (; *f >= '0' && *f <= '9'; f++) {
		v = v * 10 + (*f - '0');
		if (v > (long long)n) return -1;
	}
	return v;
}

// fragment is CF_HTML's HTML: the fragment its StartFragment and
// EndFragment byte offsets mark, else what's between its comments, else
// all from StartHTML (each app writes the header a little differently).
static char *fragment(const char *d, size_t n) {
	// The header: up to the first tag.
	size_t hn = 0;
	while (hn < n && d[hn] != '<') hn++;
	char *head = malloc(hn + 1);
	memcpy(head, d, hn);
	head[hn] = 0;
	long long sf = offset(head, "StartFragment:", n), ef = offset(head, "EndFragment:", n), sh = offset(head, "StartHTML:", n),
	          eh = offset(head, "EndHTML:", n);
	free(head);
	size_t from = 0, to = 0;
	if (sf >= 0 && ef >= sf) {
		from = (size_t)sf, to = (size_t)ef;
	} else {
		static const char starts[] = "<!--StartFragment-->", ends[] = "<!--EndFragment-->";
		char *z = malloc(n + 1);
		memcpy(z, d, n);
		z[n] = 0;
		const char *a = strstr(z, starts), *b = a ? strstr(a, ends) : NULL;
		if (b) from = (size_t)(a - z) + sizeof starts - 1, to = (size_t)(b - z);
		else from = sh >= 0 ? (size_t)sh : hn, to = eh >= (long long)from ? (size_t)eh : n;
		free(z);
	}
	char *s = malloc(to - from + 1);
	memcpy(s, d + from, to - from);
	s[to - from] = 0;
	return s;
}

static void p_html(device_Platform *self, asuka_call *call) {
	char *s = NULL;
	if (IsClipboardFormatAvailable(cf_html()) && open_clipboard()) {
		HANDLE h = GetClipboardData(cf_html());
		const char *p = h ? GlobalLock(h) : NULL;
		if (p) {
			// Up to its NUL, if it has one, but never past its memory.
			size_t cap = GlobalSize(h), n = 0;
			while (n < cap && p[n]) n++;
			s = fragment(p, n);
			GlobalUnlock(h);
		}
		CloseClipboard();
	}
	device_Platform_clipboardHTML_done(call, s);
}

// cf_html_of is html as CF_HTML: the header and its byte offsets (fixed
// width, so its length is known before they are), then html as a
// fragment in a document.
static char *cf_html_of(const char *html, size_t *len) {
	static const char fmt[] = "Version:0.9\r\nStartHTML:%010lu\r\nEndHTML:%010lu\r\nStartFragment:%010lu\r\nEndFragment:%010lu\r\n";
	static const char pre[] = "<html><body>\r\n<!--StartFragment-->", post[] = "<!--EndFragment-->\r\n</body>\r\n</html>";
	size_t head = (size_t)snprintf(NULL, 0, fmt, 0ul, 0ul, 0ul, 0ul), hn = strlen(html);
	size_t sf = head + sizeof pre - 1, ef = sf + hn, end = ef + sizeof post - 1;
	char *s = malloc(end + 1);
	snprintf(s, head + 1, fmt, (unsigned long)head, (unsigned long)end, (unsigned long)sf, (unsigned long)ef);
	memcpy(s + head, pre, sizeof pre - 1);
	memcpy(s + sf, html, hn);
	memcpy(s + ef, post, sizeof post);
	*len = end + 1;
	return s;
}

static void p_copy_html(device_Platform *self, const char *html, const char *text, asuka_call *call) {
	asuka_error e = {0};
	if (strlen(html) > 0xFFFFFFF0u) {
		asuka_error_failed(&e, "device", 0, "device: the HTML is too long for the clipboard");
		return fail(call, &e);
	}
	size_t n = 0;
	char *cf = cf_html_of(html, &n);
	wchar_t *w = asuka_wide(text);
	if (!open_clipboard()) {
		asuka_error_failed(&e, "device", 0, "the clipboard is in use");
		fail(call, &e);
	} else {
		EmptyClipboard();
		bool ok = put(cf_html(), cf, n);
		ok = put(CF_UNICODETEXT, w, (wcslen(w) + 1) * sizeof(wchar_t)) && ok;
		CloseClipboard();
		if (ok) {
			device_Platform_copyHTML_done(call);
		} else {
			asuka_error_failed(&e, "device", 0, "the clipboard took no HTML");
			fail(call, &e);
		}
	}
	free(cf);
	free(w);
}

typedef struct {
	device_fn_ClipboardKinds changed;
	asuka_call *call;
	HWND hwnd;
} watching;

static bool on_message(void *ctx, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, LRESULT *r) {
	watching *w = ctx;
	if (m == WM_CLIPBOARDUPDATE && w) {
		device_ClipboardKinds k = kinds();
		device_fn_ClipboardKinds_call(w->changed, &k);
		*r = 0;
		return true;
	}
	return false;
}

static void stop(void *p) {
	watching *w = p;
	RemoveClipboardFormatListener(w->hwnd);
	DestroyWindow(w->hwnd);
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	fail(w->call, &e);
	free(w);
}

static void p_watch(device_Platform *self, device_fn_ClipboardKinds changed, asuka_call *call) {
	watching *w = calloc(1, sizeof *w);
	w->changed = changed;
	w->call = call;
	w->hwnd = asuka_win_window(0, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, on_message, w);
	if (!w->hwnd || !AddClipboardFormatListener(w->hwnd)) {
		if (w->hwnd) DestroyWindow(w->hwnd);
		free(w);
		asuka_error e = {0};
		asuka_win_fail(&e, HRESULT_FROM_WIN32(GetLastError()), "the clipboard's listener");
		return fail(call, &e);
	}
	asuka_call_on_cancel(call, stop, w);
}

static void p_awake(device_Platform *self, bool on, asuka_error *err) {
	SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

// A desktop app has no page of its own in Settings: Privacy & security,
// where what apps may use (the camera, the microphone, location) is.
static bool p_settings(device_Platform *self, asuka_error *err) {
	return (INT_PTR)ShellExecuteW(NULL, L"open", L"ms-settings:privacy", NULL, NULL, SW_SHOWNORMAL) > 32;
}

static const device_Platform_vtable vt = {
    .info = p_info, .browse = p_browse, .pickMedia = p_media, .pickFiles = p_files, .clipboardText = p_text,
    .clipboardHas = p_has, .clipboardImage = p_image, .copyImage = p_copy, .clipboardHTML = p_html, .copyHTML = p_copy_html,
    .watchClipboard = p_watch, .keepAwake = p_awake, .openSettings = p_settings,
};

device_Platform *device_make_Platform(asuka_error *err) {
	device_Platform *p = malloc(sizeof *p);
	p->vt = &vt;
	return p;
}
