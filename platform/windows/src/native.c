// native.c: what the app's native packages' C meets in the host (RFC 0008,
// "Native packages on the desktop"): the host's half of asuka_desktop.h
// (posting, a view's window, colors, the app's names and directories),
// Native views (a package's widget, which the registry makes, in the view
// of the host's that holds it), and the core's native_* callbacks: the
// registry's (platform/desktop/src/native.c).
#include "nav.h"

// ---- The host's half of asuka_desktop.h ----

static DWORD main_thread;

void asuka_desktop_post(void (*fn)(void *ctx), void *ctx) { wh_post(fn, ctx); }  // the core's queue too: one FIFO

bool asuka_desktop_on_main(void) { return GetCurrentThreadId() == main_thread; }

void *asuka_desktop_window(uint32_t view) {
	if (view == 0) return H.window;
	node *n = wh_node(view);
	while (n && !n->hwnd) n = n->parent;  // a view without a window is placed in its ancestor's
	if (!n) return NULL;
	HWND top = GetAncestor(n->hwnd, GA_ROOT);
	return top && top != H.park ? top : NULL;  // parked: in no window yet
}

void *asuka_desktop_view(uint32_t view) {
	node *n = view ? wh_node(view) : NULL;
	return n ? n->hwnd : NULL;
}

void *asuka_desktop_webview(uint32_t view) { return view ? wh_web_view(wh_node(view)) : NULL; }

uint32_t asuka_desktop_color(uint64_t color) {
	rgba c = H.theme.label;  // the platform's default: the text's
	if (color) wh_resolve((ak_color)color, &c);
	return (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | c.a;
}

bool asuka_desktop_dark(void) { return H.dark; }

const char *asuka_desktop_app_name(void) { return H.app_name ? H.app_name : ""; }
const char *asuka_desktop_app_id(void) { return H.app_id ? H.app_id : ""; }
const char *asuka_desktop_app_version(void) { return H.config.version ? H.config.version : ""; }
const char *asuka_desktop_app_build(void) { return H.config.build ? H.config.build : ""; }

// made reports a directory, made the first time it's asked for.
static const char *made(const char *dir, bool *done) {
	if (!dir) return "";
	if (!*done) {
		dsk_mkdirs(dir);
		*done = true;
	}
	return dir;
}

const char *asuka_desktop_data_dir(void) {
	static bool done;
	return made(H.dirs.data, &done);
}

const char *asuka_desktop_config_dir(void) {
	static bool done;
	return made(H.dirs.config, &done);
}

const char *asuka_desktop_cache_dir(void) {
	static bool done;
	return made(H.dirs.cache, &done);
}

const char *asuka_desktop_logs_dir(void) {
	static bool done;
	return made(H.dirs.logs, &done);
}

// ---- The app's icon ----
//
// Its own is the executable's first icon group; its alternates
// (platform/appicon) the groups from 201 on, in asuka-app.json's
// alternateIcons order. The one the app shows is kept in its config
// directory's asuka-appicon file ("" or none: its own), read once.

static char chosen_icon[128];
static bool chosen_read;

// alternate_index is name's place among the alternates, -1 if it isn't one.
static int alternate_index(const char *name) {
	const char *p = H.config.alternate_icons;
	size_t n = strlen(name);
	if (!p || !n) return -1;
	for (int i = 0;; i++) {
		const char *e = strchr(p, ',');
		size_t k = e ? (size_t)(e - p) : strlen(p);
		if (k == n && !strncmp(p, name, n)) return i;
		if (!e) return -1;
		p = e + 1;
	}
}

static char *chosen_path(void) { return dsk_join(asuka_desktop_config_dir(), "asuka-appicon"); }

static void read_chosen(void) {
	if (chosen_read) return;
	chosen_read = true;
	char *path = chosen_path();
	size_t n;
	char *t = dsk_read_file(path, &n);
	free(path);
	if (!t) return;
	t[strcspn(t, "\r\n")] = 0;
	if (alternate_index(t) >= 0) snprintf(chosen_icon, sizeof chosen_icon, "%s", t);
	free(t);
}

// wh_app_icon is the icon the app shows: its own, or the alternate it chose.
HICON wh_app_icon(void) {
	static HICON own;
	static bool tried;
	if (!tried) {
		tried = true;
		own = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
	}
	read_chosen();
	int i = alternate_index(chosen_icon);
	if (i >= 0) {
		HICON h = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(201 + i), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
		if (h) return h;
	}
	return own;
}

const char *asuka_desktop_app_icons(void) { return H.config.alternate_icons ? H.config.alternate_icons : ""; }

const char *asuka_desktop_app_icon(void) {
	read_chosen();
	return chosen_icon;
}

// set_window_icon gives one of the process's windows the icon, and its
// class too (the windows made from now on), lp the icon.
static BOOL CALLBACK set_window_icon(HWND w, LPARAM lp) {
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId()) return TRUE;
	wchar_t cls[64];
	if (!GetClassNameW(w, cls, (int)WH_LEN(cls)) || wcscmp(cls, WH_CLASS_WINDOW) != 0) return TRUE;
	SetClassLongPtrW(w, GCLP_HICON, lp);
	SendMessageW(w, WM_SETICON, ICON_BIG, lp);
	SendMessageW(w, WM_SETICON, ICON_SMALL, lp);
	return TRUE;
}

bool asuka_desktop_set_app_icon(const char *name) {
	read_chosen();
	if (*name && alternate_index(name) < 0) return false;
	char *path = chosen_path();
	bool ok = dsk_write_file(path, name, strlen(name));
	free(path);
	if (!ok) return false;
	snprintf(chosen_icon, sizeof chosen_icon, "%s", name);
	// The windows' title bars and taskbar buttons, now.
	EnumWindows(set_window_icon, (LPARAM)wh_app_icon());
	return true;
}

const char *asuka_desktop_assets_dir(void) {
	static char *abs;
	if (!abs) {
		const char *a = H.assets ? H.assets : "assets";
		wchar_t *w = wh_wide(a, (int)strlen(a)), full[MAX_PATH * 4];
		DWORD n = GetFullPathNameW(w, (DWORD)WH_LEN(full), full, NULL);
		abs = n > 0 && n < WH_LEN(full) ? wh_utf8(full, (int)n, NULL) : wh_strdup(H.assets ? H.assets : "assets");
		free(w);
	}
	return abs;
}

// ---- Native views ----

// A Native view shows a package's widget: the registry makes it (a child
// window) in the view's window, which holds it and keeps it filling it
// (it's the node's control). Its props go to the registry as they come;
// its events come back through asuka_native_emit (the registry's); its
// package measures it, or the host does, as it would a control of its
// own: a button's ideal size, else the size the package made it.
struct wh_native {
	char *kind;  // "<path>#<Name>"
	HWND widget;
};

void wh_set_native_prop(node *n, uint16_t p, const ak_value *v) {
	switch (p) {
	case AK_PROP_NATIVE_TYPE: {
		if (n->native) return;
		wh_native *nv = wh_alloc(sizeof *nv);
		nv->kind = wh_strndup(v->s.p, v->s.len);
		n->native = nv;
		nv->widget = asuka_native_view(nv->kind, n->id, n->hwnd);
		if (!nv->widget) {
			wh_log("no native view \"%s\" is registered (a native package's desktop C)", nv->kind);
			return;
		}
		if (GetParent(nv->widget) != n->hwnd) SetParent(nv->widget, n->hwnd);
		n->ctl = nv->widget;
		EnableWindow(nv->widget, !n->disabled);
		wh_layout_field(n);
		break;
	}
	case AK_PROP_NATIVE_PROP:
		if (n->native && n->native->widget) asuka_native_view_prop(n->native->widget, (const uint8_t *)v->s.p, v->s.len);
		break;
	}
}

// unbounded is NaN for a size the layout doesn't know (NaN, infinite or
// huge), as asuka_native_view_measure takes it.
static float unbounded(float f) { return f != f || f > 1e30f || f < -1e30f ? NAN : f; }

void wh_measure_native(node *n, float w, float h, float *ow, float *oh) {
	*ow = *oh = 0;
	if (!n->native || !n->native->widget) return;
	HWND widget = n->native->widget;
	if (asuka_native_view_measure(widget, unbounded(w), unbounded(h), ow, oh)) return;
	wchar_t cls[32] = {0};
	GetClassNameW(widget, cls, (int)WH_LEN(cls));
	SIZE ideal = {0, 0};
	if (!_wcsicmp(cls, L"Button") && SendMessageW(widget, BCM_GETIDEALSIZE, 0, (LPARAM)&ideal) && ideal.cx > 0) {
		*ow = wh_pt(n, ideal.cx), *oh = wh_pt(n, ideal.cy);
		return;
	}
	RECT r;
	GetWindowRect(widget, &r);
	*ow = wh_pt(n, r.right - r.left), *oh = wh_pt(n, r.bottom - r.top);
}

// wh_native_message gives the package's widget what it tells its parent
// (the view): reflected back to it as OCM_ messages (OCM__BASE plus the
// message, ATL's and WinForms' convention), so that a widget that's a
// control hears its own notifications. A nonzero answer is taken.
LRESULT wh_native_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = false;
	if (!n->native || !n->native->widget) return 0;
	HWND w = n->native->widget, from = NULL;
	switch (m) {
	case WM_COMMAND: case WM_HSCROLL: case WM_VSCROLL:
	case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX:
		from = (HWND)lp;
		break;
	case WM_NOTIFY:
		from = ((NMHDR *)lp)->hwndFrom;
		break;
	case WM_DRAWITEM:
		from = ((DRAWITEMSTRUCT *)lp)->hwndItem;
		break;
	default:
		return 0;
	}
	if (from != w) return 0;
	LRESULT r = SendMessageW(w, (WM_USER + 0x1c00) + m, wp, lp);  // OCM__BASE + m
	*handled = r != 0;
	return r;
}

void wh_destroy_native(node *n) {
	wh_native *nv = n->native;
	if (!nv) return;
	if (nv->widget) asuka_native_view_destroy(nv->kind, nv->widget);  // the widget goes with the view's window
	free(nv->kind);
	free(nv);
	n->native = NULL;
}

// ---- The core's native_* callbacks: the registry's ----

void wh_native_host_callbacks(asuka_host_callbacks *cb) {
	main_thread = GetCurrentThreadId();
	asuka_native_callbacks(cb);
}
