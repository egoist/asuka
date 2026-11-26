// platform/device on Windows: the open dialog (modal to the window the user
// works in), ShellExecute for the browser, the clipboard (its changes
// heard with AddClipboardFormatListener), SetThreadExecutionState.
// Win32 is touched on the main thread; copying what's picked runs on a
// thread of its own.
#include "asuka_windows.h"
#include <commdlg.h>
#include <shellapi.h>
#include "device.h"

#include <limits.h>
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
	bool photos;
	size_t limit;
	char **paths;
	size_t n;
} picking;

static void copy_run(void *p) {
	picking *k = p;
	device_list_File files;
	asuka_error e = {0};
	if (!device_copy_picked(k->paths, k->n, k->limit, &files, &e)) fail(k->call, &e);
	else if (k->photos) device_Platform_pickPhotos_done(k->call, device_photos(&files));
	else device_Platform_pickFiles_done(k->call, files);
	for (size_t i = 0; i < k->n; i++) free(k->paths[i]);
	free(k->paths);
	free(k);
}

static void add(picking *k, char *p) {
	k->paths = realloc(k->paths, (k->n + 1) * sizeof *k->paths);
	k->paths[k->n++] = p;
}

static void pick(asuka_call *call, const char *title, device_list_string mimes, bool multiple, size_t limit, bool photos) {
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
	k->call = call;
	k->photos = photos;
	k->limit = limit;
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

static void p_photos(device_Platform *self, int64_t max, asuka_call *call) {
	char *image = "image/*";
	pick(call, NULL, (device_list_string){&image, 1}, max != 1, max > 0 ? (size_t)max : 0, true);
}

static void p_files(device_Platform *self, device_list_string types, bool multiple, asuka_call *call) { pick(call, NULL, types, multiple, 0, false); }

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
    .info = p_info, .browse = p_browse, .pickPhotos = p_photos, .pickFiles = p_files, .clipboardText = p_text,
    .clipboardHas = p_has, .clipboardImage = p_image, .copyImage = p_copy, .watchClipboard = p_watch, .keepAwake = p_awake,
    .openSettings = p_settings,
};

device_Platform *device_make_Platform(asuka_error *err) {
	device_Platform *p = malloc(sizeof *p);
	p->vt = &vt;
	return p;
}
