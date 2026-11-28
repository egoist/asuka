// modules.c: the host's own modules, what platform/host's call and send
// reach (the Call command on the root, answered with the Result event):
// found by name and answered once.
#include "modules.h"

#include <shlwapi.h>
#include <stdarg.h>

static const wh_module_def *modules[] = {
	&wh_tray_module, &wh_dock_module, &wh_notify_module, &wh_hotkey_module, &wh_periodic_module, &wh_apps_module,
	&wh_update_module, &wh_recent_module, &wh_login_module, &wh_background_module, &wh_contacts_module,
	&wh_calendar_module, &wh_library_module, &wh_compose_module, &wh_photo_library_module, &wh_push_module,
	&wh_widget_module, &wh_live_activity_module, &wh_icloud_module, &wh_view_module, &wh_assets_module,
	&wh_image_module,
};


// ---- Replies ----

struct wh_reply {
	uint32_t root, call;  // the root and the Call's id
	bool answered, gone;  // gone: its task cancelled (its answer goes nowhere)
};

static void answer(wh_reply *r, bool ok, const void *p, size_t n) {
	if (!r) return;
	if (r->answered) {
		wh_log("modules: a call answered twice");
		return;
	}
	r->answered = true;
	node *root = wh_node(r->root);
	if (root) {
		ak_buf b = {0};
		ak_buf_u32(&b, r->call);
		ak_buf_u8(&b, ok ? 1 : 0);
		ak_buf_put(&b, p, n);
		ak_event e = {.id = root->id, .kind = AK_EVENT_RESULT, .s = ak_strn(b.data, b.len)};
		wh_dispatch(&e);
		ak_buf_free(&b);
	}
	free(r);
}

void wh_reply_ok(wh_reply *r, const void *p, size_t n) { answer(r, true, p, n); }
void wh_reply_nil(wh_reply *r) { answer(r, true, NULL, 0); }
void wh_reply_str(wh_reply *r, const char *s) { answer(r, true, s, s ? strlen(s) : 0); }
void wh_reply_u8(wh_reply *r, uint8_t v) { answer(r, true, &v, 1); }

void wh_reply_u32(wh_reply *r, uint32_t v) {
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	answer(r, true, b, 4);
}

void wh_reply_json(wh_reply *r, dsk_jw *w) {
	size_t n = 0;
	char *s = dsk_jw_take(w, &n);
	answer(r, true, s, n);
	free(s);
}

void wh_reply_fail(wh_reply *r, const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0) n = 0;
	if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
	answer(r, false, buf, (size_t)n);
}

bool wh_reply_gone(const wh_reply *r) { return r && r->gone; }

void wh_replies_add(wh_replies *q, wh_reply *r) {
	if (q->n == q->cap) {
		q->cap = q->cap ? q->cap * 2 : 4;
		q->v = wh_realloc(q->v, q->cap * sizeof *q->v);
	}
	q->v[q->n++] = r;
}

wh_reply *wh_replies_take(wh_replies *q) {
	if (!q->n) return NULL;
	wh_reply *r = q->v[0];
	memmove(q->v, q->v + 1, (q->n - 1) * sizeof *q->v);
	q->n--;
	return r;
}

void wh_replies_fail(wh_replies *q, const char *msg) {
	// Taken first: a failure may start another wait.
	wh_replies t = *q;
	*q = (wh_replies){0};
	for (uint32_t i = 0; i < t.n; i++) wh_reply_fail(t.v[i], "%s", msg);
	free(t.v);
}

void wh_queue_next(wh_queue *q, wh_reply *r) {
	if (!q->n) {
		wh_replies_add(&q->waiting, r);
		return;
	}
	uint8_t *p = q->items[0];
	size_t n = q->lens[0];
	memmove(q->items, q->items + 1, (q->n - 1) * sizeof *q->items);
	memmove(q->lens, q->lens + 1, (q->n - 1) * sizeof *q->lens);
	q->n--;
	wh_reply_ok(r, p, n);
	free(p);
}

void wh_queue_deliver(wh_queue *q, const void *p, size_t n) {
	// A reply whose task went takes nothing: the next one does.
	wh_reply *r;
	while ((r = wh_replies_take(&q->waiting))) {
		if (!wh_reply_gone(r)) {
			wh_reply_ok(r, p, n);
			return;
		}
		wh_reply_nil(r);
	}
	if (q->max && q->n >= q->max) return;
	if (q->n == q->cap) {
		q->cap = q->cap ? q->cap * 2 : 8;
		q->items = wh_realloc(q->items, q->cap * sizeof *q->items);
		q->lens = wh_realloc(q->lens, q->cap * sizeof *q->lens);
	}
	uint8_t *c = wh_alloc(n ? n : 1);
	if (n) memcpy(c, p, n);
	q->items[q->n] = c;
	q->lens[q->n++] = n;
}

void wh_queue_stop(wh_queue *q) { wh_replies_fail(&q->waiting, "stopped"); }

// ---- Running a method ----

static wh_method find(ak_span module, ak_span method, bool *has_module) {
	*has_module = false;
	for (size_t i = 0; i < WH_LEN(modules); i++) {
		const wh_module_def *m = modules[i];
		if (strlen(m->name) != module.len || memcmp(m->name, module.p, module.len)) continue;
		*has_module = true;
		for (const wh_method_def *d = m->methods; d->name; d++)
			if (strlen(d->name) == method.len && !memcmp(d->name, method.p, method.len)) return d->fn;
		return NULL;
	}
	return NULL;
}

// web_answered is web.c's answer to r.
static void web_answered(void *ctx, const uint8_t *b, size_t n, const char *err) {
	if (err) wh_reply_fail(ctx, "%s", err);
	else wh_reply_ok(ctx, b, n);
}

// run runs module's method for r, or fails it: a module the host hasn't
// (unsupported, as the core says it), or a method it hasn't.
static void run(wh_reply *r, ak_span module, ak_span method, const uint8_t *args, size_t len) {
	bool has;
	wh_method fn = find(module, method, &has);
	if (fn) {
		fn(r, args, len);
		return;
	}
	if (module.len == 3 && !memcmp(module.p, "web", 3) && wh_web_module(method.p, method.len, args, len, web_answered, r)) return;
	if (!has) wh_reply_fail(r, "unsupported: %.*s isn't on Windows", (int)module.len, module.p);
	else wh_reply_fail(r, "asuka: %.*s.%.*s isn't on Windows yet", (int)module.len, module.p, (int)method.len, method.p);
}

// wh_call is the Call command on the root: u32 call id, string module,
// string method, then the call's bytes (platform/host's call and send;
// a send's answer goes nowhere: the core keeps no call for it).
void wh_call(node *root, const uint8_t *args, size_t len) {
	if (len < 12) return;
	uint32_t id = wh_u32(args);
	size_t at = 4;
	uint32_t ml = wh_u32(args + at);
	if (at + 4 + ml + 4 > len) return;
	ak_span module = {(const char *)args + at + 4, ml};
	at += 4 + ml;
	uint32_t nl = wh_u32(args + at);
	if (at + 4 + nl > len) return;
	ak_span method = {(const char *)args + at + 4, nl};
	at += 4 + nl;
	wh_reply *r = wh_alloc(sizeof *r);
	r->root = root->id, r->call = id;
	run(r, module, method, args + at, len - at);
}

// ---- Work off the main thread ----

typedef struct {
	void (*work)(void *ctx);
	void (*done)(void *ctx);
	void *ctx;
} off_job;

static DWORD WINAPI off_thread(void *p) {
	off_job *j = p;
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	j->work(j->ctx);
	if (SUCCEEDED(hr)) CoUninitialize();
	wh_post(j->done, j->ctx);
	free(j);
	return 0;
}

void wh_off_main(void (*work)(void *ctx), void (*done)(void *ctx), void *ctx) {
	off_job *j = wh_alloc(sizeof *j);
	j->work = work, j->done = done, j->ctx = ctx;
	HANDLE t = CreateThread(NULL, 0, off_thread, j, 0, NULL);
	if (t) {
		CloseHandle(t);
		return;
	}
	free(j);
	work(ctx);  // no thread: here, then
	done(ctx);
}

// ---- The modules' windows ----

#define CLASS_MODULE L"AsukaModule"

typedef LRESULT (*module_fn)(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled);

static LRESULT CALLBACK module_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
	module_fn fn = (module_fn)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (fn) {
		bool handled = false;
		LRESULT r = fn(hwnd, m, wp, lp, &handled);
		if (handled) return r;
	}
	return DefWindowProcW(hwnd, m, wp, lp);
}

HWND wh_module_window(bool message_only, module_fn fn) {
	static bool registered;
	if (!registered) {
		registered = true;
		WNDCLASSEXW c = {sizeof c};
		c.lpfnWndProc = module_proc;
		c.hInstance = GetModuleHandleW(NULL);
		c.lpszClassName = CLASS_MODULE;
		RegisterClassExW(&c);
	}
	HWND w = CreateWindowExW(message_only ? 0 : WS_EX_TOOLWINDOW, CLASS_MODULE, L"", message_only ? 0 : WS_POPUP, 0, 0, 0, 0,
		message_only ? HWND_MESSAGE : NULL, NULL, GetModuleHandleW(NULL), NULL);
	if (w) SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)fn);
	return w;
}

// ---- Timers, on the host's message window ----

#define TIMER_MODULES 0x1000  // the first of the modules' timer IDs

typedef struct {
	UINT_PTR id;
	bool repeat;
	void (*fn)(void *ctx);
	void *ctx;
} mod_timer;

static mod_timer *timers;
static uint32_t ntimers, captimers;
static UINT_PTR next_timer = TIMER_MODULES;

UINT_PTR wh_set_timer(UINT ms, bool repeat, void (*fn)(void *ctx), void *ctx) {
	if (ntimers == captimers) {
		captimers = captimers ? captimers * 2 : 8;
		timers = wh_realloc(timers, captimers * sizeof *timers);
	}
	UINT_PTR id = next_timer++;
	if (next_timer > 0x7FFF0000) next_timer = TIMER_MODULES;
	timers[ntimers++] = (mod_timer){id, repeat, fn, ctx};
	SetTimer(H.msg_win, id, ms ? ms : 1, NULL);
	return id;
}

void wh_kill_timer(UINT_PTR id) {
	if (!id) return;
	KillTimer(H.msg_win, id);
	for (uint32_t i = 0; i < ntimers; i++)
		if (timers[i].id == id) {
			timers[i] = timers[--ntimers];
			return;
		}
}

void wh_module_timer(WPARAM id) {
	for (uint32_t i = 0; i < ntimers; i++) {
		if (timers[i].id != id) continue;
		mod_timer t = timers[i];
		if (!t.repeat) wh_kill_timer(id);
		t.fn(t.ctx);
		return;
	}
	KillTimer(H.msg_win, id);  // not one of ours (any more)
}

// wh_module_message: the host's message window's messages no part took.
LRESULT wh_module_message(UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	(void)m, (void)wp, (void)lp;
	*handled = false;
	return 0;
}

// ---- Helpers ----

bool wh_file_exists(const char *path) {
	wchar_t *w = wh_wide(path, -1);
	DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES;
}

char *wh_cache_path(const char *name) {
	dsk_mkdirs(H.dirs.cache);
	return dsk_join(H.dirs.cache, name);
}

char *wh_known_folder(const GUID *id) {
	PWSTR p = NULL;
	if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, NULL, &p)) || !p) {
		if (p) CoTaskMemFree(p);
		return NULL;
	}
	char *s = wh_utf8(p, -1, NULL);
	CoTaskMemFree(p);
	return s;
}

char *wh_safe_file_name(const char *s, const char *def) {
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
	size_t n = strlen(s);
	char *o = wh_alloc(n + 1);
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		o[k++] = c < 32 || strchr("<>:\"/\\|?*", c) ? '_' : (char)c;
	}
	while (k && (o[k - 1] == '.' || o[k - 1] == ' ' || o[k - 1] == '\t' || o[k - 1] == '\n' || o[k - 1] == '\r')) k--;
	o[k] = 0;
	if (!k) {
		free(o);
		return wh_strdup(def);
	}
	if (k > 80) {
		// At most 80 bytes, not in the middle of a character.
		size_t c = 80;
		while (c > 0 && ((unsigned char)o[c] & 0xC0) == 0x80) c--;
		o[c] = 0;
	}
	return o;
}

DWORD wh_shell_open(HWND owner, const char *verb, const char *file) {
	wchar_t *f = wh_wide(file, -1), *v = verb && *verb ? wh_wide(verb, -1) : NULL;
	// SEE_MASK_NOASYNC: done before it returns. Without SEE_MASK_FLAG_NO_UI,
	// Windows asks which app to use for a type with none, as Explorer does.
	SHELLEXECUTEINFOW sei = {sizeof sei};
	sei.fMask = SEE_MASK_NOASYNC;
	sei.hwnd = owner;
	sei.lpVerb = v;
	sei.lpFile = f;
	sei.nShow = SW_SHOWNORMAL;
	BOOL ok = ShellExecuteExW(&sei);
	DWORD e = ok ? 0 : GetLastError();
	free(f);
	free(v);
	if (ok) return 0;
	if ((uintptr_t)sei.hInstApp == SE_ERR_NOASSOC || e == ERROR_NO_ASSOCIATION || e == SE_ERR_NOASSOC) return ERROR_NO_ASSOCIATION;
	if (e == ERROR_CANCELLED) return 0;  // the user closed the dialog
	return e ? e : ERROR_GEN_FAILURE;
}

bool wh_scheme_has_app(const char *scheme) {
	wchar_t *s = wh_wide(scheme, -1);
	wchar_t out[MAX_PATH];
	DWORD n = WH_LEN(out);
	HRESULT hr = AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, s, L"open", out, &n);
	free(s);
	return SUCCEEDED(hr) && out[0];
}

char *wh_win_error(DWORD e) {
	wchar_t *buf = NULL;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0,
		(LPWSTR)&buf, 0, NULL);
	char *s = buf ? wh_utf8(buf, -1, NULL) : NULL;
	if (buf) LocalFree(buf);
	if (!s) {
		s = wh_alloc(32);
		snprintf(s, 32, "error %lu", e);
	}
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == '.')) s[--n] = 0;
	return s;
}

const char *wh_app_user_model_id(void) {
	static char *id;
	if (id) return id;
	if (H.app_id && *H.app_id) return id = wh_strdup(H.app_id);
	ak_buf b = {0};
	ak_buf_put(&b, "Asuka.", 6);
	size_t start = b.len;
	for (const char *p = H.app_name ? H.app_name : ""; *p; p++) {
		char c = *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.') ak_buf_u8(&b, (uint8_t)c);
	}
	if (b.len == start) ak_buf_put(&b, "App", 3);
	id = wh_strndup((const char *)b.data, b.len);
	ak_buf_free(&b);
	return id;
}

// ---- WinRT (asuka_windows.h's: platform/desktop/src/windows.c) ----

static const GUID IID_XmlDocument = {0xF7F3A506, 0x1E87, 0x42D6, {0xBC, 0xFB, 0xB8, 0xC8, 0x09, 0xFA, 0x54, 0x94}};
static const GUID IID_XmlDocumentIO = {0x6CD0E74E, 0xEE65, 0x4489, {0x9E, 0xBF, 0xCA, 0x43, 0xE8, 0x7B, 0xA6, 0x37}};

void *wh_xml_document(const char *xml) {
	void *doc;
	if (FAILED(asuka_winrt_activate("Windows.Data.Xml.Dom.XmlDocument", &doc))) return NULL;
	void *io = asuka_com_query(doc, &IID_XmlDocumentIO);
	void *out = NULL;
	if (io) {
		HSTRING s = asuka_hstring(xml);
		typedef HRESULT(STDMETHODCALLTYPE * load_fn)(void *, HSTRING);
		HRESULT hr = ASUKA_SLOT(io, 6, load_fn)(io, s);  // LoadXml
		asuka_hstring_free(s);
		asuka_com_release(io);
		if (SUCCEEDED(hr)) out = asuka_com_query(doc, &IID_XmlDocument);
	}
	asuka_com_release(doc);
	return out;
}

void wh_xml_escape(ak_buf *b, const char *s) {
	for (; s && *s; s++) {
		switch (*s) {
		case '<': ak_buf_put(b, "&lt;", 4); break;
		case '>': ak_buf_put(b, "&gt;", 4); break;
		case '&': ak_buf_put(b, "&amp;", 5); break;
		case '"': ak_buf_put(b, "&#34;", 5); break;
		case '\'': ak_buf_put(b, "&#39;", 5); break;
		default: ak_buf_u8(b, (uint8_t)*s);
		}
	}
}

// ---- The system ----

static const GUID CLSID_NetworkListManager_ = {0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
static const GUID IID_INetworkListManager_ = {0xDCB00000, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

bool wh_online(void) {
	void *nlm = NULL;
	if (FAILED(CoCreateInstance(&CLSID_NetworkListManager_, NULL, CLSCTX_ALL, &IID_INetworkListManager_, &nlm)) || !nlm) return true;
	// IDispatch (7), GetNetworks, GetNetwork, GetNetworkConnections,
	// GetNetworkConnection, get_IsConnectedToInternet, get_IsConnected.
	typedef HRESULT(STDMETHODCALLTYPE * bool_fn)(void *, VARIANT_BOOL *);
	VARIANT_BOOL on = VARIANT_TRUE;
	HRESULT hr = ASUKA_SLOT(nlm, 12, bool_fn)(nlm, &on);  // get_IsConnected
	asuka_com_release(nlm);
	return FAILED(hr) || on != VARIANT_FALSE;
}

void wh_keep_system_awake(bool on) { SetThreadExecutionState(ES_CONTINUOUS | (on ? ES_SYSTEM_REQUIRED : 0)); }
