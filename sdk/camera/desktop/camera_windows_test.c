// camera_windows_test.c: the camera's permission on Windows
// (platform/windows' TestStdViews): camera_windows.c, included, reads
// Settings' privacy switches as Camera.access does. The user's switch
// for desktop apps (HKCU's NonPackaged) turned off is "denied"; turned on,
// it's what the other switches say; then it's put back as it was.
#include "camera_windows.c"

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

#define CONSENT L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam"

// says_deny reads one switch, as Settings writes it.
static bool says_deny(HKEY root, const wchar_t *path) {
	wchar_t v[16];
	DWORD n = sizeof v;
	return RegGetValueW(root, path, L"Value", RRF_RT_REG_SZ, NULL, v, &n) == ERROR_SUCCESS && !wcscmp(v, L"Deny");
}

static bool set_switch(const wchar_t *v) {
	HKEY k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, CONSENT L"\\NonPackaged", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
	LSTATUS s = RegSetValueExW(k, L"Value", 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof *v));
	RegCloseKey(k);
	return s == ERROR_SUCCESS;
}

static const char *via_vtable(void) {
	static char kept[16];
	camera_Camera *r = camera_make_Camera(NULL);
	asuka_error err = {0};
	char *s = r->vt->access(r, &err);
	snprintf(kept, sizeof kept, "%s", s ? s : "NULL");
	free(s);
	return kept;
}

// pixel reads a JPEG back (WIC): its size, and a pixel's BGR.
static bool pixel(const char *path, int x, int y, UINT *w, UINT *h, uint8_t bgr[3]) {
	uint8_t *px = NULL;
	if (!asuka_win_image_load(path, w, h, &px)) return false;
	bool ok = (UINT)x < *w && (UINT)y < *h;
	if (ok) memcpy(bgr, px + ((size_t)y * *w + x) * 4, 3);
	free(px);
	return ok;
}

// photos: a frame written as a JPEG (WIC), then rewritten as capture
// does: the middle a zoom of 2 shows, mirrored, its long side at most 100.
static void photos(void) {
	enum { W = 400, H = 200 };
	uint8_t *bgr = malloc(W * H * 3);
	// Its left half blue, its right half red.
	for (int i = 0; i < W * H; i++) {
		bool left = i % W < W / 2;
		bgr[i * 3] = left ? 255 : 0, bgr[i * 3 + 1] = 0, bgr[i * 3 + 2] = left ? 0 : 255;
	}
	char *a = camera_file(".jpg"), *b = camera_file(".jpg");
	CHECK(a && b, "no files in the caches");
	CHECK(jpeg_pixels(a, bgr, W, H, 0.9), "jpeg_pixels");
	free(bgr);
	UINT w = 0, h = 0;
	uint8_t c[3];
	CHECK(pixel(a, 10, 100, &w, &h, c) && w == W && h == H && c[0] > 200 && c[2] < 60, "written: %ux%u, left %u,%u,%u", w, h, c[0], c[1], c[2]);
	CHECK(rewrite_photo(a, b, 2, true, 100, 0.9), "rewrite_photo");
	CHECK(pixel(b, 5, 25, &w, &h, c) && w == 100 && h == 50, "rewritten: %ux%u", w, h);
	CHECK(c[2] > 200 && c[0] < 60, "mirrored: its left now red: %u,%u,%u", c[0], c[1], c[2]);
	CHECK(rewrite_photo(a, b, 1, false, 0, 0.5) && pixel(b, 390, 100, &w, &h, c) && w == W && h == H && c[2] > 200, "as it is: %ux%u", w, h);
	remove_file(a), remove_file(b);
	free(a), free(b);
	// A camera of no panel (a webcam's, a VM's): unknown.
	CHECK(panel_of(NULL) == panel_unknown && panel_of(L"\\\\?\\no#such#camera") == panel_unknown, "a panel for no camera");
	device ds[3] = {{"Integrated Webcam", NULL, panel_unknown}, {"Rear Camera", NULL, panel_unknown}, {"Camera", NULL, panel_front}};
	CHECK(device_for(ds, 3, true) == &ds[2] && device_for(ds, 3, false) == &ds[1], "facing: by its panel, then its name");
	ds[1].panel = panel_unknown, ds[1].name = "USB Camera";
	CHECK(device_for(ds, 2, false) == &ds[0], "no back camera: the first");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	photos();
	wchar_t was[16];
	DWORD n = sizeof was;
	bool had = RegGetValueW(HKEY_CURRENT_USER, CONSENT L"\\NonPackaged", L"Value", RRF_RT_REG_SZ, NULL, was, &n) == ERROR_SUCCESS;

	CHECK(set_switch(L"Deny"), "can't set the switch");
	CHECK(!strcmp(via_vtable(), "denied"), "%s", via_vtable());

	CHECK(set_switch(L"Allow"), "can't set the switch");
	bool off = says_deny(HKEY_LOCAL_MACHINE, CONSENT) || says_deny(HKEY_LOCAL_MACHINE, CONSENT L"\\NonPackaged") || says_deny(HKEY_CURRENT_USER, CONSENT);
	const char *want = off ? "denied" : "allowed";
	CHECK(!strcmp(via_vtable(), want), "%s, not %s", via_vtable(), want);
	CHECK(!strcmp(access_now(), want), "%s, not %s", access_now(), want);

	if (had) set_switch(was);
	else RegDeleteKeyValueW(HKEY_CURRENT_USER, CONSENT L"\\NonPackaged", L"Value");
	printf(failures ? "FAIL\n" : "ok\n");
	return failures ? 1 : 0;
}
