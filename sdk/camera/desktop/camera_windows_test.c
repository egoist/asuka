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

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
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
