// notify.c: platform/notify on Windows: toasts (WinRT's
// ToastNotificationManager). The app is registered with the shell as an
// unpackaged app is (a Start menu shortcut with its AppUserModelID, the
// AppUserModelId key) with a COM activator of its own, so that a toast's
// click and its buttons reach the app: the running one, or one COM starts
// for them (-ToastActivated, then -Embedding). A toast carries its
// notification (its ID, URL, data, category, thread, when it showed) in its
// arguments, JSON, which come back with a click, and which the notification
// center's history gives back for Showing.
//
// A category's actions are the toast's buttons (five at most), a reply a
// text box with its send button beside it; an action that opens the app
// activates it in the foreground, another in the background: the app gets
// the Response without coming to the front, and an app COM started for it
// quits once the package is done (actionDone) or the time's up, unless it
// came to the front meanwhile. The badge is the taskbar button's overlay
// (dock.c). What Windows lacks: Android's channels (ignored), threads (a
// thread's toasts aren't grouped; removeThread finds them), a say in how
// a scheduled toast shows while the app is in front, and a custom sound
// (an unpackaged app's toast plays Windows' own).
//
// A scheduled notification is a ScheduledToastNotification, which
// Windows shows whether or not the app runs. Windows has no repeating
// toast: a repeating one is scheduled ahead, its next SCHEDULE_AHEAD
// times (within a year), and scheduled ahead again each time the app
// starts. What's scheduled is kept (asuka-notify-scheduled.json: each
// spec by its ID, with its first time, "asuka.due", and its image's
// local copy) for "pending" and the top-up.
#include "modules.h"

#include <bcrypt.h>
#include <propvarutil.h>
#include <shobjidl.h>

#define ACTION_SECONDS 25    // what a background action has, as on iOS
#define TOAST_WAIT_MS 10000  // how long an app COM started waits for its click
#define TOAST_GROUP "asuka"
#define SCHEDULE_AHEAD 64                          // a repeating one's times scheduled at once
#define SCHEDULE_HORIZON (400LL * 24 * 3600 * 1000)  // and no further ahead than this

typedef HRESULT(STDMETHODCALLTYPE *get_fn)(void *, void **);
typedef HRESULT(STDMETHODCALLTYPE *put_fn)(void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *put_bool_fn)(void *, unsigned char);
typedef HRESULT(STDMETHODCALLTYPE *get_u32_fn)(void *, UINT32 *);
typedef HRESULT(STDMETHODCALLTYPE *get_at_fn)(void *, UINT32, void **);

static const GUID IID_ToastManagerStatics = {0x50AC103F, 0xD235, 0x4598, {0xBB, 0xEF, 0x98, 0xFE, 0x4D, 0x1A, 0x3A, 0xD4}};
static const GUID IID_ToastManagerStatics2 = {0x7AB93C52, 0x0E48, 0x4750, {0xBA, 0x9D, 0x1A, 0x41, 0x13, 0x98, 0x18, 0x47}};
static const GUID IID_ToastFactory = {0x04124B20, 0x82C6, 0x4229, {0xB1, 0x09, 0xFD, 0x9E, 0xD4, 0x66, 0x2B, 0x53}};
static const GUID IID_ScheduledToastFactory = {0xE7BED191, 0x0BB9, 0x4189, {0x83, 0x94, 0x31, 0x76, 0x1B, 0x47, 0x6F, 0xD7}};
static const GUID IID_ToastNotification2 = {0x9DFB9FD1, 0x143A, 0x490E, {0x90, 0xBF, 0xB9, 0xFB, 0xA7, 0x13, 0x2D, 0xE7}};
static const GUID IID_ScheduledToast2 = {0xA66EA09C, 0x31B4, 0x43B0, {0xB5, 0xDD, 0x7A, 0x40, 0xE8, 0x53, 0x63, 0xB1}};
static const GUID IID_ToastHistory2 = {0x3BC3D253, 0x2F31, 0x4092, {0x91, 0x29, 0x8A, 0xD5, 0xAB, 0xF0, 0x67, 0xDA}};
static const GUID IID_XmlNodeSerialize = {0x5CC5B382, 0xE6DD, 0x4991, {0xAB, 0xEF, 0x06, 0xD8, 0xD2, 0xE7, 0xBD, 0x0C}};
static const GUID IID_ActivationCallback = {0x53E31837, 0x6600, 0x4A81, {0x93, 0x95, 0x75, 0xCF, 0xFE, 0x74, 0x6F, 0x94}};
static const GUID IID_IAgile_ = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
static const PROPERTYKEY PKEY_AUMID = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
static const PROPERTYKEY PKEY_ToastActivator = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 26};

typedef struct pending {
	char *token;
	struct pending *next;
	// A presentation the package decides (how), or a background action
	// (its timer).
	char *spec, *image;  // the notification to show: its spec's JSON
	UINT_PTR timer;
	bool action;
} pending;

static struct {
	void *notifier, *statics;
	bool activator;  // the class object is registered
	DWORD cookie;
	bool presenting;
	wh_queue presents, actions;
	pending *pendings;
	char *categories;
	bool categories_read;
	int tokens;
	bool hidden;  // started for a toast, not shown yet
	UINT_PTR wait;
} nt;

// ---- The app's identity ----

// toast_clsid is the CLSID of the app's activator: a name-based UUID of
// its AppUserModelID, the same for every build of the app.
static GUID toast_clsid(void) {
	static GUID g;
	static bool made;
	if (made) return g;
	made = true;
	const char *aumid = wh_app_user_model_id();
	char *name = wh_alloc(strlen(aumid) + 32);
	sprintf(name, "asuka.toast-activator:%s", aumid);
	uint8_t sum[20] = {0};
	BCryptHash(BCRYPT_SHA1_ALG_HANDLE, NULL, 0, (PUCHAR)name, (ULONG)strlen(name), sum, sizeof sum);
	free(name);
	sum[6] = (sum[6] & 0x0f) | 0x50;
	sum[8] = (sum[8] & 0x3f) | 0x80;
	g.Data1 = (uint32_t)sum[0] << 24 | (uint32_t)sum[1] << 16 | (uint32_t)sum[2] << 8 | sum[3];
	g.Data2 = (uint16_t)(sum[4] << 8 | sum[5]);
	g.Data3 = (uint16_t)(sum[6] << 8 | sum[7]);
	memcpy(g.Data4, sum + 8, 8);
	return g;
}

static void guid_string(GUID g, wchar_t *out, size_t cap) {
	swprintf(out, cap, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
		g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static bool reg_set(const wchar_t *path, const wchar_t *name, const wchar_t *v) {
	HKEY k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
	LSTATUS s = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return s == ERROR_SUCCESS;
}

static void activator_key(wchar_t *out, size_t cap) {
	wchar_t g[64];
	guid_string(toast_clsid(), g, WH_LEN(g));
	swprintf(out, cap, L"Software\\Classes\\CLSID\\%ls", g);
}

static bool activator_registered(void) {
	wchar_t key[160];
	activator_key(key, WH_LEN(key));
	wcscat(key, L"\\LocalServer32");
	wchar_t *v = wh_reg_string(HKEY_CURRENT_USER, key, NULL);
	bool on = v && *v;
	free(v);
	return on;
}

// start_shortcut makes the user's Start menu shortcut to the app, with
// its AppUserModelID and the toasts' activator, unless it's there with
// them.
static bool start_shortcut(const wchar_t *aumid, const wchar_t *exe, char **err) {
	wchar_t *appdata = _wgetenv(L"APPDATA");
	if (!appdata) {
		*err = wh_strdup("no APPDATA");
		return false;
	}
	wchar_t path[MAX_PATH * 2];
	swprintf(path, WH_LEN(path), L"%ls\\Microsoft\\Windows\\Start Menu\\Programs\\%ls.lnk", appdata, H.app_namew ? H.app_namew : L"Asuka");
	IShellLinkW *link = NULL;
	if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
		*err = wh_strdup("no shell links");
		return false;
	}
	IPersistFile *pf = NULL;
	IPropertyStore *ps = NULL;
	IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
	IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **)&ps);
	GUID clsid = toast_clsid();
	bool ok = false;
	if (!pf) {
		*err = wh_strdup("no shell links");
		goto out;
	}
	if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && SUCCEEDED(IPersistFile_Load(pf, path, STGM_READ))) {
		wchar_t have[MAX_PATH * 4];
		bool same = SUCCEEDED(IShellLinkW_GetPath(link, have, WH_LEN(have), NULL, 0)) && !_wcsicmp(have, exe);
		if (same && ps) {
			PROPVARIANT v;
			PropVariantInit(&v);
			if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_AUMID, &v))) same = v.vt == VT_LPWSTR && v.pwszVal && !wcscmp(v.pwszVal, aumid);
			PropVariantClear(&v);
			if (same && SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_ToastActivator, &v)))
				same = v.vt == VT_CLSID && v.puuid && IsEqualGUID(v.puuid, &clsid);
			PropVariantClear(&v);
		}
		if (same) {
			ok = true;
			goto out;
		}
	}
	IShellLinkW_SetPath(link, exe);
	if (ps) {
		PROPVARIANT v;
		PropVariantInit(&v);
		v.vt = VT_LPWSTR, v.pwszVal = (LPWSTR)aumid;
		IPropertyStore_SetValue(ps, &PKEY_AUMID, &v);
		v.vt = VT_CLSID, v.puuid = &clsid;
		IPropertyStore_SetValue(ps, &PKEY_ToastActivator, &v);
		IPropertyStore_Commit(ps);
	}
	HRESULT hr = IPersistFile_Save(pf, path, TRUE);
	if (FAILED(hr)) {
		*err = wh_alloc(64);
		sprintf(*err, "the Start menu's shortcut: %#lx", (unsigned long)hr);
		goto out;
	}
	ok = true;
out:
	if (ps) IPropertyStore_Release(ps);
	if (pf) IPersistFile_Release(pf);
	IShellLinkW_Release(link);
	return ok;
}

// toast_app_id is the app ID toasts are shown for: the package's
// (PackageFamilyName!App, GetCurrentApplicationUserModelId) when the app
// is packaged, whose manifest declares its activator; else its own
// (wh_app_user_model_id), which its Start menu shortcut carries.
static const char *toast_app_id(void) {
	static char *id;
	if (id) return id;
	if (wh_packaged()) {
		typedef LONG(WINAPI * id_fn)(UINT32 *, PWSTR);
		id_fn f = (id_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentApplicationUserModelId");
		wchar_t w[130];  // APPLICATION_USER_MODEL_ID_MAX_LENGTH
		UINT32 n = WH_LEN(w);
		if (f && f(&n, w) == ERROR_SUCCESS) return id = wh_utf8(w, -1, NULL);
	}
	return id = wh_strdup(wh_app_user_model_id());
}

// ---- The activator ----

// NOTIFICATION_USER_INPUT_DATA: what the user typed in a toast's input.
typedef struct {
	LPCWSTR Key, Value;
} toast_input;

static void activated(const wchar_t *args, const toast_input *data, ULONG count);

static HRESULT STDMETHODCALLTYPE obj_qi(void *self, REFIID riid, void **out, const GUID *iid) {
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, iid) || IsEqualGUID(riid, &IID_IAgile_)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

// Both objects live as long as the process: their counts are fixed.
static ULONG STDMETHODCALLTYPE obj_add_ref(void *self) {
	(void)self;
	return 2;
}
static ULONG STDMETHODCALLTYPE obj_release(void *self) {
	(void)self;
	return 1;
}

static HRESULT STDMETHODCALLTYPE cb_qi(void *self, REFIID riid, void **out) { return obj_qi(self, riid, out, &IID_ActivationCallback); }

static HRESULT STDMETHODCALLTYPE cb_activate(void *self, LPCWSTR aumid, LPCWSTR args, const toast_input *data, ULONG count) {
	(void)self, (void)aumid;
	activated(args, data, count);
	return S_OK;
}

static void *callback_vtbl[] = {(void *)cb_qi, (void *)obj_add_ref, (void *)obj_release, (void *)cb_activate};
static struct {
	void **vtbl;
} callback_obj = {callback_vtbl};

static HRESULT STDMETHODCALLTYPE cf_qi(void *self, REFIID riid, void **out) { return obj_qi(self, riid, out, &IID_IClassFactory); }

static HRESULT STDMETHODCALLTYPE cf_create(void *self, IUnknown *outer, REFIID riid, void **out) {
	(void)self;
	*out = NULL;
	if (outer) return CLASS_E_NOAGGREGATION;
	return obj_qi(&callback_obj, riid, out, &IID_ActivationCallback);
}

static HRESULT STDMETHODCALLTYPE cf_lock(void *self, BOOL lock) {
	(void)self, (void)lock;
	return S_OK;
}

static void *factory_vtbl[] = {(void *)cf_qi, (void *)obj_add_ref, (void *)obj_release, (void *)cf_create, (void *)cf_lock};
static struct {
	void **vtbl;
} factory_obj = {factory_vtbl};

// start_activator registers the activator's class object, so that clicks
// on the app's toasts come to this process (its main thread: an STA
// whose message loop runs Activate).
static void start_activator(void) {
	if (nt.activator) return;
	GUID clsid = toast_clsid();
	HRESULT hr = CoRegisterClassObject(&clsid, (IUnknown *)&factory_obj, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &nt.cookie);
	if (FAILED(hr)) {
		wh_log("platform/notify: the toasts' activator: %#lx", (unsigned long)hr);
		return;
	}
	nt.activator = true;
}

// register_activator registers the app's activator class: its server,
// the app, which COM starts with -ToastActivated when it isn't running;
// and names it the activator of the app's toasts.
static bool register_activator(const wchar_t *aumid, const wchar_t *exe) {
	wchar_t key[160], server[180];
	activator_key(key, WH_LEN(key));
	swprintf(server, WH_LEN(server), L"%ls\\LocalServer32", key);
	wchar_t cmd[MAX_PATH * 4 + 32];
	swprintf(cmd, WH_LEN(cmd), L"\"%ls\" -ToastActivated", exe);
	wchar_t *have = wh_reg_string(HKEY_CURRENT_USER, server, NULL);
	bool same = have && !wcscmp(have, cmd);
	free(have);
	if (!same && (!reg_set(key, NULL, L"Asuka toast activator") || !reg_set(server, NULL, cmd))) return false;
	wchar_t idkey[600], g[64];
	swprintf(idkey, WH_LEN(idkey), L"Software\\Classes\\AppUserModelId\\%ls", aumid);
	guid_string(toast_clsid(), g, WH_LEN(g));
	return reg_set(idkey, L"CustomActivator", g);
}

// wh_take_toast_launch reports whether COM started the process for a
// toast's activation, taking the arguments it added (-ToastActivated,
// -Embedding) out.
void wh_take_toast_launch(int *argc, wchar_t **argv) {
	for (int i = 1; i < *argc;) {
		if (!_wcsicmp(argv[i], L"-ToastActivated") || !_wcsicmp(argv[i], L"-Embedding") || !_wcsicmp(argv[i], L"/Embedding")) {
			H.launched_for_toast = true;
			memmove(argv + i, argv + i + 1, (size_t)(*argc - i - 1) * sizeof *argv);
			(*argc)--;
		} else i++;
	}
}

// wh_start_toasts readies the app for its toasts as it starts: its
// activator, if it has shown toasts before (or COM started it for one).
static void topup(void);

void wh_start_toasts(void) {
	if (H.launched_for_toast || activator_registered()) {
		start_activator();
		// A repeating notification's next times, scheduled ahead again
		// (once a launch).
		static bool topped;
		if (!topped) {
			topped = true;
			topup();
		}
	}
}

// ---- The notifier ----

static void write_icon_png(HICON icon, int size, const wchar_t *path) {
	HDC dc = CreateCompatibleDC(NULL);
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = size, bi.bmiHeader.biHeight = -size, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32;
	void *bits = NULL;
	HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bmp) {
		DeleteDC(dc);
		return;
	}
	HGDIOBJ old = SelectObject(dc, bmp);
	if (DrawIconEx(dc, 0, 0, icon, size, size, 0, NULL, DI_NORMAL)) {
		GdiFlush();
		uint8_t *px = bits;
		// Straight alpha, as the encoder takes it.
		for (int i = 0; i < size * size; i++) {
			unsigned a = px[i * 4 + 3];
			if (a && a < 255)
				for (int c = 0; c < 3; c++) px[i * 4 + c] = (uint8_t)(px[i * 4 + c] * 255 / a > 255 ? 255 : px[i * 4 + c] * 255 / a);
		}
		wh_encode_image_file(path, false, px, size, size);
	}
	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
}

// toasts is the app's notifier, registered with the shell the first time
// (a Start menu shortcut, its name, its icon as a PNG in the caches, the
// activator): NULL with *err.
static void *toasts(char **err) {
	if (nt.notifier) return nt.notifier;
	const char *id = toast_app_id();
	wchar_t *aumid = wh_wide(id, -1);
	wchar_t exe[MAX_PATH * 4];
	GetModuleFileNameW(NULL, exe, WH_LEN(exe));
	wchar_t *icon = NULL;
	// A packaged app is registered already: its package's manifest names
	// it, its icon and its activator.
	bool packaged = wh_packaged();
	if (wh_app_icon() && !packaged) {
		char *p = wh_cache_path("asuka-notify-icon.png");
		icon = wh_wide(p, -1);
		free(p);
		write_icon_png(wh_app_icon(), 64, icon);
	}
	char *e = NULL;
	if (!packaged && !start_shortcut(aumid, exe, &e)) {
		*err = wh_alloc(strlen(e) + 32);
		sprintf(*err, "platform/notify: %s", e);
		free(e), free(aumid), free(icon);
		return NULL;
	}
	if (!packaged) {
		wchar_t idkey[600];
		swprintf(idkey, WH_LEN(idkey), L"Software\\Classes\\AppUserModelId\\%ls", aumid);
		reg_set(idkey, L"DisplayName", H.app_namew ? H.app_namew : L"Asuka");
		if (icon && GetFileAttributesW(icon) != INVALID_FILE_ATTRIBUTES) reg_set(idkey, L"IconUri", icon);
		SetCurrentProcessExplicitAppUserModelID(aumid);
	}
	free(icon);
	if (!packaged && !register_activator(aumid, exe)) {
		*err = wh_strdup("platform/notify: couldn't register the toasts' activator");
		free(aumid);
		return NULL;
	}
	start_activator();
	void *st;
	asuka_winrt_factory("Windows.UI.Notifications.ToastNotificationManager", &IID_ToastManagerStatics, &st);
	if (!st) {
		*err = wh_strdup("platform/notify: no ToastNotificationManager");
		free(aumid);
		return NULL;
	}
	HSTRING h = asuka_hstring_w(aumid);
	free(aumid);
	void *n = NULL;
	typedef HRESULT(STDMETHODCALLTYPE * make_fn)(void *, HSTRING, void **);
	HRESULT hr = ASUKA_SLOT(st, 7, make_fn)(st, h, &n);  // CreateToastNotifierWithId
	asuka_hstring_free(h);
	if (FAILED(hr) || !n) {
		asuka_com_release(st);
		*err = wh_alloc(48);
		sprintf(*err, "platform/notify: toasts: %#lx", (unsigned long)hr);
		return NULL;
	}
	nt.notifier = n, nt.statics = st;
	return n;
}

// enabled reports whether the user lets the app show toasts; a newly
// registered app's setting isn't found until the shell has seen its
// shortcut: its toasts are on, as every app's are at first.
static bool enabled(void *n) {
	int s = 0;
	typedef HRESULT(STDMETHODCALLTYPE * setting_fn)(void *, int *);
	HRESULT hr = ASUKA_SLOT(n, 8, setting_fn)(n, &s);  // get_Setting
	return (uint32_t)hr == 0x80070490 || (SUCCEEDED(hr) && s == 0);  // ERROR_NOT_FOUND
}

// show shows a toast of xml now, tagged id; at, if not 0, schedules it
// for then (Unix milliseconds). quiet puts it in the notification center
// without a banner.
static HRESULT show(void *n, const char *xml, const char *id, int64_t at, bool quiet) {
	void *doc = wh_xml_document(xml);
	if (!doc) return E_INVALIDARG;
	HSTRING tag = asuka_hstring(id), group = asuka_hstring(TOAST_GROUP);
	HRESULT hr;
	if (at) {
		void *f;
		asuka_winrt_factory("Windows.UI.Notifications.ScheduledToastNotification", &IID_ScheduledToastFactory, &f);
		void *st = NULL;
		typedef HRESULT(STDMETHODCALLTYPE * make_fn)(void *, void *, int64_t, void **);
		// A DateTime: 100 ns since 1601.
		hr = f ? ASUKA_SLOT(f, 6, make_fn)(f, doc, (at + 11644473600000LL) * 10000, &st) : E_FAIL;
		asuka_com_release(f);
		if (SUCCEEDED(hr) && st) {
			ASUKA_SLOT(st, 10, put_fn)(st, tag);  // put_Id
			void *s2 = asuka_com_query(st, &IID_ScheduledToast2);
			if (s2) {
				ASUKA_SLOT(s2, 6, put_fn)(s2, tag);    // put_Tag
				ASUKA_SLOT(s2, 8, put_fn)(s2, group);  // put_Group
				if (quiet) ASUKA_SLOT(s2, 10, put_bool_fn)(s2, 1);  // put_SuppressPopup
				asuka_com_release(s2);
			}
			hr = ASUKA_SLOT(n, 9, put_fn)(n, st);  // AddToSchedule
		}
		asuka_com_release(st);
	} else {
		void *f;
		asuka_winrt_factory("Windows.UI.Notifications.ToastNotification", &IID_ToastFactory, &f);
		void *t = NULL;
		typedef HRESULT(STDMETHODCALLTYPE * make_fn)(void *, void *, void **);
		hr = f ? ASUKA_SLOT(f, 6, make_fn)(f, doc, &t) : E_FAIL;
		asuka_com_release(f);
		if (SUCCEEDED(hr) && t) {
			void *t2 = asuka_com_query(t, &IID_ToastNotification2);
			if (t2) {
				ASUKA_SLOT(t2, 6, put_fn)(t2, tag);
				ASUKA_SLOT(t2, 8, put_fn)(t2, group);
				if (quiet) ASUKA_SLOT(t2, 10, put_bool_fn)(t2, 1);
				asuka_com_release(t2);
			}
			hr = ASUKA_SLOT(n, 6, put_fn)(n, t);  // Show
		}
		asuka_com_release(t);
	}
	asuka_hstring_free(tag);
	asuka_hstring_free(group);
	asuka_com_release(doc);
	return hr;
}

// cancel takes a scheduled toast off the schedule.
static void cancel(void *n, const char *id) {
	void *list = NULL;
	if (FAILED(ASUKA_SLOT(n, 11, get_fn)(n, &list)) || !list) return;  // GetScheduledToastNotifications
	UINT32 count = 0;
	ASUKA_SLOT(list, 7, get_u32_fn)(list, &count);
	for (UINT32 i = 0; i < count; i++) {
		void *st = NULL;
		if (FAILED(ASUKA_SLOT(list, 6, get_at_fn)(list, i, &st)) || !st) continue;
		HSTRING h = NULL;
		ASUKA_SLOT(st, 11, get_fn)(st, (void **)&h);  // get_Id
		char *s = asuka_hstring_utf8(h);
		if (!strcmp(s, id)) ASUKA_SLOT(n, 10, put_fn)(n, st);  // RemoveFromSchedule
		free(s);
		asuka_hstring_free(h);
		asuka_com_release(st);
	}
	asuka_com_release(list);
}

static void *history(void) {
	void *s2 = asuka_com_query(nt.statics, &IID_ToastManagerStatics2);
	if (!s2) return NULL;
	void *h = NULL;
	ASUKA_SLOT(s2, 6, get_fn)(s2, &h);  // get_History
	asuka_com_release(s2);
	return h;
}

// remove takes the toast tagged id out of the notification center.
static void remove_toast(const char *id) {
	void *h = history();
	if (!h) return;
	HSTRING tag = asuka_hstring(id), group = asuka_hstring(TOAST_GROUP), app = asuka_hstring(toast_app_id());
	typedef HRESULT(STDMETHODCALLTYPE * remove_fn)(void *, HSTRING, HSTRING, HSTRING);
	ASUKA_SLOT(h, 8, remove_fn)(h, tag, group, app);  // RemoveGroupedTagWithId
	asuka_hstring_free(tag), asuka_hstring_free(group), asuka_hstring_free(app);
	asuka_com_release(h);
}

// ---- The notifications' JSON ----

static void jw_shown(dsk_jw *w, const dsk_json *spec, const char *id, int64_t at, size_t body_runes) {
	const char *title = dsk_json_str(spec, "title", "");
	const dsk_json *sender = dsk_json_get(spec, "sender");
	if (!*title && sender) title = dsk_json_str(sender, "name", "");
	const char *body = dsk_json_str(spec, "body", "");
	dsk_jw_obj(w);
	dsk_jw_key(w, "id"), dsk_jw_str(w, id);
	dsk_jw_key(w, "title"), dsk_jw_str(w, title);
	dsk_jw_key(w, "subtitle"), dsk_jw_str(w, dsk_json_str(spec, "subtitle", ""));
	size_t bl = strlen(body);
	if (body_runes) {
		// Cut short: the toast's XML is 5 KB at most, and the notification
		// is in it once for each button.
		size_t runes = 0, i = 0;
		for (; i < bl; i++) {
			if (((unsigned char)body[i] & 0xC0) != 0x80) {
				if (runes == body_runes) break;
				runes++;
			}
		}
		bl = i;
	}
	dsk_jw_key(w, "body"), dsk_jw_strn(w, body, bl);
	dsk_jw_key(w, "url"), dsk_jw_str(w, dsk_json_str(spec, "url", ""));
	dsk_jw_key(w, "data");
	const dsk_json *data = dsk_json_get(spec, "data");
	if (data && data->type == DSK_JOBJ) dsk_jw_value(w, data);
	else dsk_jw_obj(w), dsk_jw_end(w);
	dsk_jw_key(w, "category"), dsk_jw_str(w, dsk_json_str(spec, "category", ""));
	dsk_jw_key(w, "thread"), dsk_jw_str(w, dsk_json_str(spec, "thread", ""));
	dsk_jw_key(w, "time"), dsk_jw_int(w, at);
	dsk_jw_key(w, "remote"), dsk_jw_bool(w, false);
	dsk_jw_end(w);
}

// toast_args is a toast's or a button's arguments: the notification and
// the action (free it).
static char *toast_args(const dsk_json *spec, const char *id, int64_t at, const char *action) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "asuka"), dsk_jw_int(&w, 1);
	if (action && *action) dsk_jw_key(&w, "a"), dsk_jw_str(&w, action);
	dsk_jw_key(&w, "n");
	jw_shown(&w, spec, id, at, 240);
	dsk_jw_end(&w);
	return dsk_jw_take(&w, NULL);
}

static char *categories_path(void) { return dsk_join(H.dirs.data, "asuka-notify-categories.json"); }

// category is a category's JSON (an object of the categories' array:
// its own parse, which the caller frees with *all).
static const dsk_json *category(const char *id, dsk_json **all) {
	*all = NULL;
	if (!nt.categories_read) {
		nt.categories_read = true;
		if (!nt.categories) {
			char *p = categories_path();
			nt.categories = dsk_read_file(p, NULL);
			free(p);
		}
	}
	if (!nt.categories || !id || !*id) return NULL;
	*all = dsk_json_parse(nt.categories, strlen(nt.categories));
	for (size_t i = 0; *all && (*all)->type == DSK_JARR && i < (*all)->n; i++)
		if (!strcmp(dsk_json_str(&(*all)->kids[i], "id", ""), id)) return &(*all)->kids[i];
	return NULL;
}

static bool opens_app(const char *cat, const char *action) {
	dsk_json *all;
	const dsk_json *c = category(cat, &all);
	const dsk_json *acts = c ? dsk_json_get(c, "actions") : NULL;
	bool open = false;
	for (size_t i = 0; acts && acts->type == DSK_JARR && i < acts->n; i++)
		if (!strcmp(dsk_json_str(&acts->kids[i], "id", ""), action)) {
			open = dsk_json_bool(&acts->kids[i], "open", false);
			break;
		}
	dsk_json_free(all);
	return open;
}

static void file_url(ak_buf *b, const char *path) {
	ak_buf_put(b, "file:///", 8);
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		unsigned char c = *p == '\\' ? '/' : *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/:!$&'()*+,;=@", c)) ak_buf_u8(b, c);
		else ak_buf_printf(b, "%%%02X", c);
	}
}

// toast_xml is a notification's toast: its title (a message's sender),
// subtitle and body, an image, silent or not, its category's actions, and
// its arguments (free it).
static char *toast_xml(const dsk_json *spec, const char *id, int64_t at, const char *image) {
	ak_buf b = {0};
	char *launch = toast_args(spec, id, at, NULL);
	ak_buf_printf(&b, "<toast launch=\"");
	wh_xml_escape(&b, launch);
	ak_buf_u8(&b, '"');
	free(launch);
	dsk_json *all;
	const dsk_json *c = category(dsk_json_str(spec, "category", ""), &all);
	const dsk_json *acts = c ? dsk_json_get(c, "actions") : NULL;
	size_t nacts = acts && acts->type == DSK_JARR ? acts->n : 0;
	for (size_t i = 0; i < nacts; i++)
		if (dsk_json_bool(&acts->kids[i], "destructive", false)) {
			ak_buf_printf(&b, " useButtonStyle=\"true\"");
			break;
		}
	// Time-sensitive and critical ones: Windows 11's important
	// notifications, through Do Not Disturb when the user lets them.
	const char *level = dsk_json_str(spec, "interruption", "");
	if (!strcmp(level, "timeSensitive") || !strcmp(level, "critical")) ak_buf_printf(&b, " scenario=\"urgent\"");
	ak_buf_printf(&b, "><visual><binding template=\"ToastGeneric\">");
	const char *title = dsk_json_str(spec, "title", "");
	const dsk_json *sender = dsk_json_get(spec, "sender");
	if (!*title && sender) title = dsk_json_str(sender, "name", "");
	const char *texts[3] = {title, dsk_json_str(spec, "subtitle", ""), dsk_json_str(spec, "body", "")};
	for (int i = 0; i < 3; i++)
		if (*texts[i]) {
			ak_buf_put(&b, "<text>", 6);
			wh_xml_escape(&b, texts[i]);
			ak_buf_put(&b, "</text>", 7);
		}
	if (image && *image) {
		ak_buf u = {0};
		file_url(&u, image);
		ak_buf_u8(&u, 0);
		ak_buf_put(&b, "<image src=\"", 12);
		wh_xml_escape(&b, (const char *)u.data);
		ak_buf_put(&b, "\"/>", 3);
		ak_buf_free(&u);
	}
	const dsk_json *progress = dsk_json_get(spec, "progress");
	if (progress && progress->type == DSK_JOBJ) {
		if (dsk_json_bool(progress, "indeterminate", false)) ak_buf_printf(&b, "<progress value=\"indeterminate\" status=\"\"/>");
		else ak_buf_printf(&b, "<progress value=\"%.3f\" status=\"\"/>", dsk_json_num(progress, "value", 0));
	}
	ak_buf_printf(&b, "</binding></visual>");
	if (nacts) {
		ak_buf_printf(&b, "<actions>");
		int inputs = 0;
		for (size_t i = 0; i < nacts; i++) {
			const dsk_json *a = &acts->kids[i];
			const dsk_json *reply = dsk_json_get(a, "reply");
			if (reply && reply->type == DSK_JOBJ && inputs < 5) {
				ak_buf_printf(&b, "<input id=\"r:");
				wh_xml_escape(&b, dsk_json_str(a, "id", ""));
				ak_buf_printf(&b, "\" type=\"text\" placeHolderContent=\"");
				wh_xml_escape(&b, dsk_json_str(reply, "placeholder", ""));
				ak_buf_printf(&b, "\"/>");
				inputs++;
			}
		}
		for (size_t i = 0; i < nacts && i < 5; i++) {
			const dsk_json *a = &acts->kids[i];
			const dsk_json *reply = dsk_json_get(a, "reply");
			bool has_reply = reply && reply->type == DSK_JOBJ;
			const char *t = dsk_json_str(a, "title", "");
			if (has_reply && *dsk_json_str(reply, "button", "")) t = dsk_json_str(reply, "button", "");
			char *args = toast_args(spec, id, at, dsk_json_str(a, "id", ""));
			ak_buf_printf(&b, "<action content=\"");
			wh_xml_escape(&b, t);
			ak_buf_printf(&b, "\" arguments=\"");
			wh_xml_escape(&b, args);
			ak_buf_printf(&b, "\" activationType=\"%s\"", dsk_json_bool(a, "open", false) ? "foreground" : "background");
			free(args);
			if (has_reply) {
				ak_buf_printf(&b, " hint-inputId=\"r:");
				wh_xml_escape(&b, dsk_json_str(a, "id", ""));
				ak_buf_u8(&b, '"');
			}
			if (dsk_json_bool(a, "destructive", false)) ak_buf_printf(&b, " hint-buttonStyle=\"Critical\"");
			ak_buf_printf(&b, "/>");
		}
		ak_buf_printf(&b, "</actions>");
	}
	dsk_json_free(all);
	if (dsk_json_bool(spec, "silent", false) || !strcmp(level, "passive")) ak_buf_printf(&b, "<audio silent=\"true\"/>");
	ak_buf_printf(&b, "</toast>");
	char *out = wh_strndup((const char *)b.data, b.len);
	ak_buf_free(&b);
	return out;
}

static char *new_token(void) {
	SYSTEMTIME t;
	GetLocalTime(&t);
	char *s = wh_alloc(64);
	snprintf(s, 64, "%02d%02d%02d.%03d%06d-%d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (int)(wh_now_ns() % 1000000), ++nt.tokens);
	return s;
}

static int64_t now_ms(void) {
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	return (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000) - 11644473600000LL;
}

// ---- Toasts clicked ----

static void to_front(void) {
	if (nt.wait) {
		wh_kill_timer(nt.wait);
		nt.wait = 0;
	}
	if (nt.hidden) {
		nt.hidden = false;
		wh_show_main();
		return;
	}
	if (IsIconic(H.window)) ShowWindow(H.window, SW_RESTORE);
	wh_to_front();
	SetForegroundWindow(H.window);
}

static pending *take_pending(const char *token, bool action) {
	for (pending **p = &nt.pendings; *p; p = &(*p)->next)
		if ((*p)->action == action && !strcmp((*p)->token, token)) {
			pending *x = *p;
			*p = x->next;
			return x;
		}
	return NULL;
}

static void free_pending(pending *p) {
	free(p->token), free(p->spec), free(p->image);
	free(p);
}

static bool any_actions(void) {
	for (pending *p = nt.pendings; p; p = p->next)
		if (p->action) return true;
	return false;
}

// action_done is a background action done: an app started for it quits
// then.
static void action_done(const char *token) {
	pending *p = take_pending(token, true);
	if (!p) return;
	wh_kill_timer(p->timer);
	free_pending(p);
	if (nt.hidden && !any_actions()) PostQuitMessage(0);
}

static void action_timeout(void *ctx) {
	pending *p = ctx;
	char *token = wh_strdup(p->token);
	p->timer = 0;
	action_done(token);
	free(token);
}

typedef struct {
	char *args;
	char **keys, **values;
	ULONG n;
} activation;

static void activation_run(void *ctx) {
	activation *a = ctx;
	dsk_json *args = dsk_json_parse(a->args, strlen(a->args));
	if (!args || dsk_json_num(args, "asuka", 0) != 1) {
		to_front();  // not one of ours: the app's opened
		goto out;
	}
	const char *action = dsk_json_str(args, "a", "");
	const dsk_json *note = dsk_json_get(args, "n");
	char input[300];
	snprintf(input, sizeof input, "r:%s", action);
	const char *reply = "";
	for (ULONG i = 0; i < a->n; i++)
		if (!strcmp(a->keys[i], input)) reply = a->values[i];
	char *token = new_token();
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "token"), dsk_jw_str(&w, token);
	dsk_jw_key(&w, "action"), dsk_jw_str(&w, action);
	dsk_jw_key(&w, "reply"), dsk_jw_str(&w, reply);
	dsk_jw_key(&w, "seconds"), dsk_jw_int(&w, ACTION_SECONDS);
	dsk_jw_key(&w, "notification");
	if (note && note->type == DSK_JOBJ) {
		// As it was, but its data an object (never null).
		dsk_jw_obj(&w);
		for (size_t i = 0; i < note->n; i++) {
			dsk_jw_key(&w, note->keys[i]);
			if (!strcmp(note->keys[i], "data") && note->kids[i].type != DSK_JOBJ) dsk_jw_obj(&w), dsk_jw_end(&w);
			else dsk_jw_value(&w, &note->kids[i]);
		}
		if (!dsk_json_get(note, "data")) dsk_jw_key(&w, "data"), dsk_jw_obj(&w), dsk_jw_end(&w);
		dsk_jw_end(&w);
	} else {
		dsk_jw_obj(&w), dsk_jw_key(&w, "data"), dsk_jw_obj(&w), dsk_jw_end(&w), dsk_jw_end(&w);
	}
	dsk_jw_end(&w);
	size_t len = 0;
	char *resp = dsk_jw_take(&w, &len);
	wh_queue_deliver(&nt.actions, resp, len);
	free(resp);
	const char *cat = note ? dsk_json_str(note, "category", "") : "";
	if (!*action || opens_app(cat, action)) {
		to_front();
		const char *url = note ? dsk_json_str(note, "url", "") : "";
		if (!*action && *url) wh_incoming_url(url, strlen(url));
		free(token);
		goto out;
	}
	// A background action: the package says when it's done; an app
	// started for it quits then.
	pending *p = wh_alloc(sizeof *p);
	p->token = token, p->action = true;
	p->next = nt.pendings, nt.pendings = p;
	p->timer = wh_set_timer(ACTION_SECONDS * 1000, false, action_timeout, p);
out:
	dsk_json_free(args);
	for (ULONG i = 0; i < a->n; i++) free(a->keys[i]), free(a->values[i]);
	free(a->keys), free(a->values), free(a->args);
	free(a);
}

// activated is a click on one of the app's toasts, or on one of its
// buttons: Activate is a COM call, so the work waits until it's returned.
static void activated(const wchar_t *args, const toast_input *data, ULONG count) {
	activation *a = wh_alloc(sizeof *a);
	a->args = wh_utf8(args ? args : L"", -1, NULL);
	if (data && count && count < 64) {
		a->keys = wh_alloc(count * sizeof *a->keys);
		a->values = wh_alloc(count * sizeof *a->values);
		for (ULONG i = 0; i < count; i++) {
			a->keys[i] = wh_utf8(data[i].Key ? data[i].Key : L"", -1, NULL);
			a->values[i] = wh_utf8(data[i].Value ? data[i].Value : L"", -1, NULL);
		}
		a->n = count;
	}
	wh_post(activation_run, a);
}

static void wait_over(void *ctx) {
	(void)ctx;
	nt.wait = 0;
	if (!any_actions()) to_front();
}

// wh_wait_for_toast holds the window back while the app COM started waits
// for the click: one in the background leaves it hidden; one in the
// foreground, or none in time, shows it.
void wh_wait_for_toast(void) {
	nt.hidden = true;
	nt.wait = wh_set_timer(TOAST_WAIT_MS, false, wait_over, NULL);
}

// ---- Showing ----

typedef struct {
	wh_reply *r;
	char *spec_json, *id, *source, *image, *err;
} schedule_job;

static void show_now(wh_reply *r, const dsk_json *spec, const char *id, const char *image, uint8_t how) {
	if (how == 2) {  // none
		wh_reply_nil(r);
		return;
	}
	int badge = (int)dsk_json_num(spec, "badge", 0);
	if (badge > 0) wh_set_badge_count(badge);
	char *xml = toast_xml(spec, id, now_ms(), image);
	HRESULT hr = show(nt.notifier, xml, id, 0, how == 1);
	free(xml);
	if (FAILED(hr)) wh_reply_fail(r, "toasts: %#lx", (unsigned long)hr);
	else wh_reply_nil(r);
}

typedef struct {
	char *token;
	wh_reply *r;
	char *spec, *id, *image;
	UINT_PTR timer;
} present_job;

static present_job **presents;
static uint32_t npresents;

static void present_answer(present_job *p, uint8_t how) {
	for (uint32_t i = 0; i < npresents; i++)
		if (presents[i] == p) {
			presents[i] = presents[--npresents];
			break;
		}
	wh_kill_timer(p->timer);
	dsk_json *spec = dsk_json_parse(p->spec, strlen(p->spec));
	show_now(p->r, spec, p->id, p->image, how);
	dsk_json_free(spec);
	free(p->token), free(p->spec), free(p->id), free(p->image);
	free(p);
}

static void present_timeout(void *ctx) {
	present_job *p = ctx;
	p->timer = 0;
	present_answer(p, 0);
}

static bool app_active(void) { return wh_key_window(GetForegroundWindow()) != NULL; }

// show_toast shows a notification now: if the app is in front and the
// package presents (notify.onPresent), it decides how first (a second at
// most).
static void show_toast(wh_reply *r, const char *spec_json, const char *id, const char *image) {
	dsk_json *spec = dsk_json_parse(spec_json, strlen(spec_json));
	if (!nt.presenting || !app_active()) {
		show_now(r, spec, id, image, 0);
		dsk_json_free(spec);
		return;
	}
	present_job *p = wh_alloc(sizeof *p);
	p->token = new_token(), p->r = r, p->spec = wh_strdup(spec_json), p->id = wh_strdup(id), p->image = image ? wh_strdup(image) : NULL;
	presents = wh_realloc(presents, (npresents + 1) * sizeof *presents);
	presents[npresents++] = p;
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "token"), dsk_jw_str(&w, p->token);
	dsk_jw_key(&w, "notification");
	jw_shown(&w, spec, id, now_ms(), 0);
	dsk_jw_end(&w);
	size_t n = 0;
	char *s = dsk_jw_take(&w, &n);
	wh_queue_deliver(&nt.presents, s, n);
	free(s);
	dsk_json_free(spec);
	p->timer = wh_set_timer(1000, false, present_timeout, p);
}

static void schedule_ahead(void *n, const dsk_json *spec, const char *id, const char *image, int64_t due, int64_t now);
static void keep_scheduled(const char *id, const dsk_json *spec, const char *image, int64_t due);

static void add_toast(wh_reply *r, const char *spec_json, const char *id, const char *image) {
	dsk_json *spec = dsk_json_parse(spec_json, strlen(spec_json));
	double secs = dsk_json_num(spec, "seconds", 0);
	const dsk_json *trigger = dsk_json_get(spec, "trigger");
	if (trigger && trigger->type == DSK_JOBJ) {
		int64_t now = now_ms();
		int64_t due = dsk_trigger_next(trigger, now);
		if (!due) due = now + 1000;  // a time that's past: in a second
		keep_scheduled(id, spec, image, due);
		schedule_ahead(nt.notifier, spec, id, image, due, now);
		dsk_json_free(spec);
		wh_reply_nil(r);
		return;
	}
	if (secs <= 0) {
		dsk_json_free(spec);
		show_toast(r, spec_json, id, image);
		return;
	}
	int64_t at = now_ms() + (int64_t)((secs < 1 ? 1 : secs) * 1000);
	char *xml = toast_xml(spec, id, at, image);
	HRESULT hr = show(nt.notifier, xml, id, at, false);
	free(xml);
	dsk_json_free(spec);
	if (FAILED(hr)) wh_reply_fail(r, "toasts: %#lx", (unsigned long)hr);
	else wh_reply_nil(r);
}

static void image_work(void *ctx) {
	schedule_job *j = ctx;
	j->image = wh_local_file(j->source, &j->err);
}

static void image_done(void *ctx) {
	schedule_job *j = ctx;
	add_toast(j->r, j->spec_json, j->id, j->image);  // without its image if it failed
	free(j->spec_json), free(j->id), free(j->source), free(j->image), free(j->err);
	free(j);
}

// ---- Scheduled ----

static void *with(wh_reply *r);

static char *scheduled_path(void) { return dsk_join(H.dirs.data, "asuka-notify-scheduled.json"); }

// scheduled_read is what's kept: an object of specs by ID (an empty one
// if none).
static dsk_json *scheduled_read(void) {
	char *p = scheduled_path();
	size_t len = 0;
	char *t = dsk_read_file(p, &len);
	free(p);
	dsk_json *j = t ? dsk_json_parse(t, len) : NULL;
	free(t);
	if (!j || j->type != DSK_JOBJ) {
		dsk_json_free(j);
		j = dsk_json_parse("{}", 2);
	}
	return j;
}

// scheduled_write keeps all's specs but drop's (NULL: none), and add
// (JSON, NULL: none) as id's.
static void scheduled_write(const dsk_json *all, const char *drop, const char *id, const char *add) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	for (size_t i = 0; all && i < all->n; i++) {
		if ((drop && !strcmp(all->keys[i], drop)) || (id && !strcmp(all->keys[i], id))) continue;
		dsk_jw_key(&w, all->keys[i]);
		dsk_jw_value(&w, &all->kids[i]);
	}
	if (id && add) {
		dsk_jw_key(&w, id);
		dsk_jw_json(&w, add, strlen(add));
	}
	dsk_jw_end(&w);
	size_t len = 0;
	char *t = dsk_jw_take(&w, &len);
	char *p = scheduled_path();
	if (t) dsk_write_file(p, t, len);
	free(p), free(t);
}

// keep_scheduled keeps spec as id's, its first time due and its image's
// local copy beside it.
static void keep_scheduled(const char *id, const dsk_json *spec, const char *image, int64_t due) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	for (size_t i = 0; i < spec->n; i++) {
		if (!strcmp(spec->keys[i], "asuka.due") || !strcmp(spec->keys[i], "asuka.image") || !strcmp(spec->keys[i], "id")) continue;
		dsk_jw_key(&w, spec->keys[i]);
		dsk_jw_value(&w, &spec->kids[i]);
	}
	dsk_jw_key(&w, "id"), dsk_jw_str(&w, id);
	dsk_jw_key(&w, "asuka.due"), dsk_jw_int(&w, due);
	if (image) dsk_jw_key(&w, "asuka.image"), dsk_jw_str(&w, image);
	dsk_jw_end(&w);
	char *t = dsk_jw_take(&w, NULL);
	dsk_json *all = scheduled_read();
	scheduled_write(all, NULL, id, t);
	dsk_json_free(all);
	free(t);
}

static void forget_scheduled(const char *id) {
	dsk_json *all = scheduled_read();
	if (dsk_json_get(all, id)) scheduled_write(all, id, NULL, NULL);
	dsk_json_free(all);
}

// schedule_ahead schedules id's toasts from due on (after now): once, or
// a repeating one's next times, in place of those scheduled before.
static void schedule_ahead(void *n, const dsk_json *spec, const char *id, const char *image, int64_t due, int64_t now) {
	cancel(n, id);
	const dsk_json *t = dsk_json_get(spec, "trigger");
	int64_t at = dsk_trigger_after(t, due, now);
	for (int i = 0; at && i < SCHEDULE_AHEAD && at - now < SCHEDULE_HORIZON; i++) {
		char *xml = toast_xml(spec, id, at, image);
		show(n, xml, id, at, false);
		free(xml);
		if (!dsk_trigger_repeats(t)) break;
		int64_t next = dsk_trigger_next(t, at);
		if (next <= at) break;
		at = next;
	}
}

// topup schedules the repeating ones ahead again, and forgets the
// one-time ones that showed: as the app starts.
static void topup(void) {
	dsk_json *all = scheduled_read();
	if (!all->n) {
		dsk_json_free(all);
		return;
	}
	char *err = NULL;
	void *n = toasts(&err);
	free(err);
	int64_t now = now_ms();
	for (size_t i = 0; n && i < all->n; i++) {
		const dsk_json *spec = &all->kids[i];
		const dsk_json *t = dsk_json_get(spec, "trigger");
		int64_t due = (int64_t)dsk_json_num(spec, "asuka.due", 0);
		if (!dsk_trigger_after(t, due, now)) {
			forget_scheduled(all->keys[i]);
			continue;
		}
		if (!dsk_trigger_repeats(t)) continue;  // scheduled already
		const char *image = dsk_json_str(spec, "asuka.image", "");
		schedule_ahead(n, spec, all->keys[i], *image ? image : NULL, due, now);
	}
	dsk_json_free(all);
}

// pending: what's kept and still due, the soonest first (sdk/notify's
// WireScheduled).
static void m_pending(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	dsk_json *all = scheduled_read();
	int64_t now = now_ms();
	size_t count = 0;
	struct item {
		size_t i;
		int64_t next;
	} *items = wh_alloc((all->n ? all->n : 1) * sizeof *items);
	for (size_t i = 0; i < all->n; i++) {
		const dsk_json *spec = &all->kids[i];
		int64_t next = dsk_trigger_after(dsk_json_get(spec, "trigger"), (int64_t)dsk_json_num(spec, "asuka.due", 0), now);
		if (next) items[count++] = (struct item){i, next};
	}
	for (size_t i = 1; i < count; i++)
		for (size_t j = i; j > 0 && items[j].next < items[j - 1].next; j--) {
			struct item x = items[j];
			items[j] = items[j - 1], items[j - 1] = x;
		}
	dsk_jw w = {0};
	dsk_jw_arr(&w);
	for (size_t k = 0; k < count; k++) {
		const dsk_json *spec = &all->kids[items[k].i];
		dsk_jw_obj(&w);
		const char *keys[] = {"id", "title", "subtitle", "body", "url", "category", "thread"};
		for (size_t q = 0; q < WH_LEN(keys); q++) dsk_jw_key(&w, keys[q]), dsk_jw_str(&w, dsk_json_str(spec, keys[q], ""));
		dsk_jw_key(&w, "data");
		const dsk_json *data = dsk_json_get(spec, "data");
		if (data && data->type == DSK_JOBJ) dsk_jw_value(&w, data);
		else dsk_jw_obj(&w), dsk_jw_end(&w);
		const dsk_json *trig = dsk_json_get(spec, "trigger");
		dsk_jw_key(&w, "trigger");
		if (trig) dsk_jw_value(&w, trig);
		else dsk_jw_obj(&w), dsk_jw_key(&w, "kind"), dsk_jw_str(&w, "after"), dsk_jw_end(&w);
		dsk_jw_key(&w, "next"), dsk_jw_int(&w, items[k].next);
		dsk_jw_end(&w);
	}
	dsk_jw_aend(&w);
	free(items);
	dsk_json_free(all);
	wh_reply_json(r, &w);
}

// cancelAll: every scheduled toast of the app's, and what's kept.
static void m_cancel_all(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	void *t = with(r);
	if (!t) return;
	void *list = NULL;
	if (SUCCEEDED(ASUKA_SLOT(t, 11, get_fn)(t, &list)) && list) {  // GetScheduledToastNotifications
		UINT32 count = 0;
		ASUKA_SLOT(list, 7, get_u32_fn)(list, &count);
		for (UINT32 i = 0; i < count; i++) {
			void *st = NULL;
			if (FAILED(ASUKA_SLOT(list, 6, get_at_fn)(list, i, &st)) || !st) continue;
			ASUKA_SLOT(t, 10, put_fn)(t, st);  // RemoveFromSchedule
			asuka_com_release(st);
		}
		asuka_com_release(list);
	}
	scheduled_write(NULL, NULL, NULL, NULL);
	wh_reply_nil(r);
}

// ---- The module ----

// with is the notifier, made the first time; NULL with r failed.
static void *with(wh_reply *r) {
	char *err = NULL;
	void *n = toasts(&err);
	if (!n) wh_reply_fail(r, "%s", err ? err : "platform/notify: no toasts");
	free(err);
	return n;
}

static void m_request(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	void *t = with(r);
	if (t) wh_reply_u8(r, enabled(t));
}

static void m_schedule(wh_reply *r, const uint8_t *a, size_t n) {
	void *t = with(r);
	if (!t) return;
	dsk_json *spec = dsk_json_parse((const char *)a, n);
	if (!spec || spec->type != DSK_JOBJ) {
		dsk_json_free(spec);
		wh_reply_fail(r, "notify.schedule: bad arguments");
		return;
	}
	char *id = wh_strdup(dsk_json_str(spec, "id", ""));
	if (!*id) {
		free(id);
		SYSTEMTIME st;
		GetLocalTime(&st);
		id = wh_alloc(48);
		snprintf(id, 48, "%04d%02d%02d%02d%02d%02d.%03d%06d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			(int)(wh_now_ns() % 1000000));
	}
	cancel(t, id);  // one of the same ID due is replaced
	const char *image = dsk_json_str(spec, "image", "");
	char *json = wh_strndup((const char *)a, n);
	if (!*image) {
		add_toast(r, json, id, NULL);
		free(json), free(id);
		dsk_json_free(spec);
		return;
	}
	schedule_job *j = wh_alloc(sizeof *j);
	j->r = r, j->spec_json = json, j->id = id, j->source = wh_strdup(image);
	dsk_json_free(spec);
	wh_off_main(image_work, image_done, j);
}

static void m_cancel(wh_reply *r, const uint8_t *a, size_t n) {
	void *t = with(r);
	if (!t) return;
	char *id = wh_strndup((const char *)a, n);
	cancel(t, id);
	remove_toast(id);
	forget_scheduled(id);
	free(id);
	wh_reply_nil(r);
}

static void m_categories(wh_reply *r, const uint8_t *a, size_t n) {
	free(nt.categories);
	nt.categories = wh_strndup((const char *)a, n);
	nt.categories_read = true;
	char *p = categories_path();
	dsk_write_file(p, a, n);
	free(p);
	wh_reply_nil(r);
}

// badge: u32.
static void m_badge(wh_reply *r, const uint8_t *a, size_t n) {
	int count = n >= 4 ? (int)wh_u32(a) : 0;
	if (wh_set_badge_count(count)) wh_reply_nil(r);
	else wh_reply_fail(r, "the taskbar's overlay");
}

// launch_attr is a toast's XML's launch attribute, its entities undone
// (free it).
static char *launch_attr(const char *xml) {
	const char *p = strstr(xml, "launch=\"");
	if (!p) return wh_strdup("");
	p += 8;
	const char *end = strchr(p, '"');
	if (!end) return wh_strdup("");
	char *o = wh_alloc((size_t)(end - p) + 1);
	size_t k = 0;
	for (const char *c = p; c < end; c++) {
		if (*c != '&') {
			o[k++] = *c;
			continue;
		}
		const char *semi = memchr(c, ';', (size_t)(end - c));
		if (!semi) {
			o[k++] = *c;
			continue;
		}
		size_t l = (size_t)(semi - c - 1);
		const char *e = c + 1;
		if (l == 2 && !strncmp(e, "lt", 2)) o[k++] = '<';
		else if (l == 2 && !strncmp(e, "gt", 2)) o[k++] = '>';
		else if (l == 3 && !strncmp(e, "amp", 3)) o[k++] = '&';
		else if (l == 4 && !strncmp(e, "quot", 4)) o[k++] = '"';
		else if (l == 4 && !strncmp(e, "apos", 4)) o[k++] = '\'';
		else if (l > 1 && e[0] == '#') {
			// No longer than the reference: one of 4 bytes is 9 characters
			// at least (&#x10000;), one of 3, 7 (&#x800;).
			unsigned long v = e[1] == 'x' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
			if (v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) v = 0xFFFD;
			if (v < 0x80) o[k++] = (char)v;
			else if (v < 0x800) o[k++] = (char)(0xC0 | v >> 6), o[k++] = (char)(0x80 | (v & 0x3F));
			else if (v < 0x10000) o[k++] = (char)(0xE0 | v >> 12), o[k++] = (char)(0x80 | ((v >> 6) & 0x3F)), o[k++] = (char)(0x80 | (v & 0x3F));
			else
				o[k++] = (char)(0xF0 | v >> 18), o[k++] = (char)(0x80 | ((v >> 12) & 0x3F)), o[k++] = (char)(0x80 | ((v >> 6) & 0x3F)),
				o[k++] = (char)(0x80 | (v & 0x3F));
		} else {
			memcpy(o + k, c, l + 2);
			k += l + 2;
		}
		c = semi;
	}
	o[k] = 0;
	return o;
}

typedef struct {
	char *json;  // the notification's
	int64_t time;
	char *tag;
} shown;

static int newest_first(const void *a, const void *b) {
	const shown *x = a, *y = b;
	return x->time < y->time ? 1 : x->time > y->time ? -1 : 0;
}

// showing is the app's toasts in the notification center, newest first:
// false if this Windows keeps no history.
static bool showing(shown **out, uint32_t *count) {
	*out = NULL, *count = 0;
	void *h = history();
	void *h2 = h ? asuka_com_query(h, &IID_ToastHistory2) : NULL;
	asuka_com_release(h);
	if (!h2) return false;
	HSTRING app = asuka_hstring(toast_app_id());
	void *list = NULL;
	typedef HRESULT(STDMETHODCALLTYPE * hist_fn)(void *, HSTRING, void **);
	HRESULT hr = ASUKA_SLOT(h2, 7, hist_fn)(h2, app, &list);  // GetHistoryWithId
	asuka_hstring_free(app);
	asuka_com_release(h2);
	if (FAILED(hr) || !list) return false;
	UINT32 n = 0;
	ASUKA_SLOT(list, 7, get_u32_fn)(list, &n);
	*out = wh_alloc((n ? n : 1) * sizeof **out);
	for (UINT32 i = 0; i < n; i++) {
		void *tn = NULL;
		if (FAILED(ASUKA_SLOT(list, 6, get_at_fn)(list, i, &tn)) || !tn) continue;
		shown s = {0};
		void *t2 = asuka_com_query(tn, &IID_ToastNotification2);
		if (t2) {
			HSTRING tag = NULL;
			ASUKA_SLOT(t2, 7, get_fn)(t2, (void **)&tag);  // get_Tag
			s.tag = asuka_hstring_utf8(tag);
			asuka_hstring_free(tag);
			asuka_com_release(t2);
		} else s.tag = wh_strdup("");
		char *launch = NULL;
		void *doc = NULL;
		if (SUCCEEDED(ASUKA_SLOT(tn, 6, get_fn)(tn, &doc)) && doc) {  // get_Content
			void *ser = asuka_com_query(doc, &IID_XmlNodeSerialize);
			if (ser) {
				HSTRING x = NULL;
				ASUKA_SLOT(ser, 6, get_fn)(ser, (void **)&x);  // GetXml
				char *xml = asuka_hstring_utf8(x);
				launch = launch_attr(xml);
				free(xml);
				asuka_hstring_free(x);
				asuka_com_release(ser);
			}
			asuka_com_release(doc);
		}
		asuka_com_release(tn);
		dsk_json *args = launch ? dsk_json_parse(launch, strlen(launch)) : NULL;
		const dsk_json *note = args && dsk_json_num(args, "asuka", 0) == 1 ? dsk_json_get(args, "n") : NULL;
		dsk_jw w = {0};
		if (note && note->type == DSK_JOBJ) {
			dsk_jw_obj(&w);
			for (size_t k = 0; k < note->n; k++) {
				dsk_jw_key(&w, note->keys[k]);
				if (!strcmp(note->keys[k], "id") && !*dsk_json_str(note, "id", "")) dsk_jw_str(&w, s.tag);
				else if (!strcmp(note->keys[k], "data") && note->kids[k].type != DSK_JOBJ) dsk_jw_obj(&w), dsk_jw_end(&w);
				else dsk_jw_value(&w, &note->kids[k]);
			}
			if (!dsk_json_get(note, "data")) dsk_jw_key(&w, "data"), dsk_jw_obj(&w), dsk_jw_end(&w);
			dsk_jw_end(&w);
			s.time = (int64_t)dsk_json_num(note, "time", 0);
		} else {
			dsk_jw_obj(&w);
			dsk_jw_key(&w, "id"), dsk_jw_str(&w, s.tag);
			const char *keys[] = {"title", "subtitle", "body", "url", "category", "thread"};
			for (size_t k = 0; k < WH_LEN(keys); k++) dsk_jw_key(&w, keys[k]), dsk_jw_str(&w, "");
			dsk_jw_key(&w, "data"), dsk_jw_obj(&w), dsk_jw_end(&w);
			dsk_jw_key(&w, "time"), dsk_jw_int(&w, 0);
			dsk_jw_key(&w, "remote"), dsk_jw_bool(&w, false);
			dsk_jw_end(&w);
		}
		s.json = dsk_jw_take(&w, NULL);
		dsk_json_free(args);
		free(launch);
		(*out)[(*count)++] = s;
	}
	asuka_com_release(list);
	// Stable, newest first: the history is oldest first.
	for (uint32_t i = 0; i < *count / 2; i++) {
		shown t = (*out)[i];
		(*out)[i] = (*out)[*count - 1 - i];
		(*out)[*count - 1 - i] = t;
	}
	qsort(*out, *count, sizeof **out, newest_first);
	return true;
}

static void free_shown(shown *v, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) free(v[i].json), free(v[i].tag);
	free(v);
}

static void m_showing(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	if (!with(r)) return;
	shown *v;
	uint32_t count;
	if (!showing(&v, &count)) {
		wh_reply_fail(r, "toasts: no history");
		return;
	}
	dsk_jw w = {0};
	dsk_jw_arr(&w);
	for (uint32_t i = 0; i < count; i++) {
		if (w.comma) dsk_jw_raw(&w, ",", 1);
		dsk_jw_raw(&w, v[i].json, strlen(v[i].json));
		w.comma = true;
	}
	dsk_jw_aend(&w);
	free_shown(v, count);
	wh_reply_json(r, &w);
}

static void m_remove(wh_reply *r, const uint8_t *a, size_t n) {
	if (!with(r)) return;
	dsk_json *ids = dsk_json_parse((const char *)a, n);
	for (size_t i = 0; ids && ids->type == DSK_JARR && i < ids->n; i++)
		if (ids->kids[i].type == DSK_JSTR) remove_toast(ids->kids[i].str);
	dsk_json_free(ids);
	wh_reply_nil(r);
}

static void m_remove_thread(wh_reply *r, const uint8_t *a, size_t n) {
	if (!with(r)) return;
	shown *v;
	uint32_t count;
	if (!showing(&v, &count)) {
		wh_reply_fail(r, "toasts: no history");
		return;
	}
	char *thread = wh_strndup((const char *)a, n);
	for (uint32_t i = 0; i < count; i++) {
		dsk_json *j = dsk_json_parse(v[i].json, strlen(v[i].json));
		if (j && !strcmp(dsk_json_str(j, "thread", ""), thread)) remove_toast(dsk_json_str(j, "id", ""));
		dsk_json_free(j);
	}
	free(thread);
	free_shown(v, count);
	wh_reply_nil(r);
}

static void m_remove_all(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	if (!with(r)) return;
	void *h = history();
	if (h) {
		HSTRING app = asuka_hstring(toast_app_id());
		typedef HRESULT(STDMETHODCALLTYPE * clear_fn)(void *, HSTRING);
		ASUKA_SLOT(h, 12, clear_fn)(h, app);  // ClearWithId
		asuka_hstring_free(app);
		asuka_com_release(h);
	}
	wh_reply_nil(r);
}

static void m_present(wh_reply *r, const uint8_t *a, size_t n) {
	nt.presenting = n > 0 && a[0] == 1;
	if (!nt.presenting)
		while (npresents) present_answer(presents[npresents - 1], 0);
	wh_reply_nil(r);
}

static void m_next_present(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_queue_next(&nt.presents, r);
}

// presented: u8 how (0 banner, 1 list, 2 none), then the token.
static void m_presented(wh_reply *r, const uint8_t *a, size_t n) {
	if (n > 0) {
		for (uint32_t i = 0; i < npresents; i++)
			if (strlen(presents[i]->token) == n - 1 && !memcmp(presents[i]->token, a + 1, n - 1)) {
				present_answer(presents[i], a[0]);
				break;
			}
	}
	wh_reply_nil(r);
}

static void m_next_action(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_start_toasts();
	wh_queue_next(&nt.actions, r);
}

static void m_action_done(wh_reply *r, const uint8_t *a, size_t n) {
	char *token = wh_strndup((const char *)a, n);
	action_done(token);
	free(token);
	wh_reply_nil(r);
}

static void m_stop(wh_reply *r, const uint8_t *a, size_t n) {
	if (n == 11 && !memcmp(a, "nextPresent", 11)) wh_queue_stop(&nt.presents);
	if (n == 10 && !memcmp(a, "nextAction", 10)) wh_queue_stop(&nt.actions);
	wh_reply_nil(r);
}

static void ok_nil(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_nil(r);
}

static const wh_method_def notify_methods[] = {
	{"request", m_request}, {"schedule", m_schedule}, {"cancel", m_cancel}, {"cancelAll", m_cancel_all}, {"pending", m_pending},
	{"categories", m_categories}, {"badge", m_badge},
	{"showing", m_showing}, {"remove", m_remove}, {"removeThread", m_remove_thread}, {"removeAll", m_remove_all},
	{"present", m_present}, {"nextPresent", m_next_present}, {"presented", m_presented}, {"nextAction", m_next_action},
	{"actionDone", m_action_done}, {"stop", m_stop},
	// Android's: Windows has no channels.
	{"channels", ok_nil}, {"channelGroups", ok_nil}, {NULL, NULL},
};

const wh_module_def wh_notify_module = {"notify", notify_methods};
