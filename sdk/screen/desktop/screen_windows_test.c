// screen_windows_test.c: Screens.capture on Windows (platform/windows'
// TestStdViews, in the signed-in session): screen_windows.c, included,
// captures each monitor as the vtable's capture does on its worker. Each
// PNG is its monitor's mode in pixels; the primary's shows a window of
// one color the test puts on it; a display that isn't there is
// CaptureError.notAvailable.
#include "screen_windows.c"

#include <stdio.h>

static int failures;

#define CHECK(c, ...)                                      \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s: ", __FILE__, __LINE__, #c); \
			printf(__VA_ARGS__);                           \
			printf("\n");                                  \
			failures++;                                    \
		}                                                  \
	} while (0)

static void pump(void) {
	MSG msg;
	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

// png_size reads a PNG's IHDR: false if it isn't one.
static bool png_size(const char *path, UINT *w, UINT *h) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	unsigned char b[24];
	bool ok = fread(b, 1, sizeof b, f) == sizeof b && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(b + 12, "IHDR", 4) == 0;
	fclose(f);
	if (ok) {
		*w = (UINT)b[16] << 24 | (UINT)b[17] << 16 | (UINT)b[18] << 8 | b[19];
		*h = (UINT)b[20] << 24 | (UINT)b[21] << 16 | (UINT)b[22] << 8 | b[23];
	}
	return ok;
}

static LRESULT CALLBACK solid(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(hwnd, m, wp, lp); }

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // as the host does
	char dir[MAX_PATH];
	GetFullPathNameA(".", MAX_PATH, dir, NULL);

	// Each display displays() says, at its mode's size.
	screen_list_Display ds = read_displays();
	CHECK(ds.len > 0, "no displays");
	for (size_t i = 0; i < ds.len; i++) {
		asuka_error err = {0};
		int w = 0, h = 0;
		char *path = shoot(ds.data[i].id, dir, &w, &h, &err);
		CHECK(path != NULL, "%s: status %d, %s", ds.data[i].id, err.status, err.message ? err.message : "");
		if (!path) {
			asuka_error_free(&err);
			continue;
		}
		DEVMODEW dm;
		memset(&dm, 0, sizeof dm);
		dm.dmSize = sizeof dm;
		wchar_t *dev = asuka_wide(ds.data[i].id);
		BOOL mode = EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm);
		free(dev);
		UINT pw = 0, ph = 0;
		CHECK(png_size(path, &pw, &ph), "%s isn't a PNG", path);
		CHECK(pw == (UINT)w && ph == (UINT)h, "%s: %ux%u, said %dx%d", path, pw, ph, w, h);
		if (mode) CHECK(pw == dm.dmPelsWidth && ph == dm.dmPelsHeight, "%s: %ux%u, the mode %lux%lu", ds.data[i].id, pw, ph, dm.dmPelsWidth, dm.dmPelsHeight);
		CHECK(strncmp(path, dir, strlen(dir)) == 0 && strstr(path, "\\asuka-screen\\capture-"), "%s: not in the caches", path);
		printf("%s (%s, scale %.2f): %ux%u, %s\n", ds.data[i].id, ds.data[i].name, ds.data[i].scale, pw, ph, path);
		DeleteFileA(path);
		free(path);
	}
	screen_list_Display_free(&ds);

	// The primary ("") shows what's on it: a magenta window at 100, 100.
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = solid;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 255));
	wc.lpszClassName = L"screen_test_solid";
	RegisterClassW(&wc);
	HWND win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 100, 100, 200, 200, NULL, NULL, wc.hInstance, NULL);
	CHECK(win != NULL, "no window");
	UpdateWindow(win);
	HRESULT(WINAPI * dwm_flush)(void) = NULL;
	HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (dwm) *(FARPROC *)&dwm_flush = GetProcAddress(dwm, "DwmFlush");
	bool seen = false;
	for (int tries = 0; tries < 100 && !seen; tries++) {  // until DWM has composed it
		pump();
		if (dwm_flush) dwm_flush();  // dwmapi isn't linked for a package
		asuka_error err = {0};
		int w = 0, h = 0;
		char *path = shoot("", dir, &w, &h, &err);
		if (!path) {
			CHECK(path != NULL, "the primary: status %d, %s", err.status, err.message ? err.message : "");
			asuka_error_free(&err);
			break;
		}
		UINT iw = 0, ih = 0;
		uint8_t *px = NULL;
		if (asuka_win_image_load(path, &iw, &ih, &px) && iw > 200 && ih > 200) {
			const uint8_t *p = px + ((size_t)200 * iw + 200) * 4;  // BGRA, the window's middle
			seen = p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255;
		}
		free(px);
		DeleteFileA(path);
		free(path);
		if (!seen) Sleep(20);
	}
	CHECK(seen, "the primary's capture doesn't show the window");
	DestroyWindow(win);

	// No such display.
	asuka_error err = {0};
	int w, h;
	char *none = shoot("\\\\.\\DISPLAY99", dir, &w, &h, &err);
	CHECK(none == NULL && err.status == ASUKA_DECLARED && err.index == screen_CaptureError_notAvailable, "status %d index %u", err.status, err.index);
	free(none);
	asuka_error_free(&err);

	printf(failures ? "FAIL\n" : "ok\n");
	return failures ? 1 : 0;
}
