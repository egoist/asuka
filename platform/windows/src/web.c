// web.c: WebView and Video. A WebView is WebView2 (Edge's runtime) in a
// view of the host's: a windowed controller in the view's window. The page
// posts to the app with window.asuka.postMessage(text), as on iOS and
// Android; the app's assets are served from https://asuka.assets/. The
// environment is made once, for the app, the first time a web view shows;
// until it and the view's controller exist, what the core asks for waits.
//
// A Video is a WebView showing the video in a <video> element, with Edge's
// media controls: Win32 has no video view with controls. Its page reports
// its playback and tracks as the web host's does; full screen and picture
// in picture are the page's (full screen fills the view, not the screen).
//
// WebView2 is reached through its COM interfaces' vtables, by slot (from
// WebView2.h), in the runtime's own client library, which EdgeUpdate's
// registry keys locate as WebView2Loader.dll does: no loader ships.
#include "nav.h"

#include "asuka_windows.h"

#define ASSETS_HOST L"asuka.assets"
#define BRIDGE_JS L"window.asuka = { postMessage: function (m) { window.chrome.webview.postMessage(String(m)); } };"

// The vtable slots used, from WebView2.h.
enum {
	ENV_CREATE_CONTROLLER = 3,
	ENV_CREATE_WEB_RESOURCE_RESPONSE = 4,

	CTL_PUT_IS_VISIBLE = 4,
	CTL_PUT_BOUNDS = 6,
	CTL_CLOSE = 24,
	CTL_GET_CORE_WEBVIEW2 = 25,
	CTL2_PUT_DEFAULT_BACKGROUND_COLOR = 27,

	WV_GET_SETTINGS = 3,
	WV_GET_SOURCE = 4,
	WV_NAVIGATE = 5,
	WV_NAVIGATE_TO_STRING = 6,
	WV_ADD_NAVIGATION_STARTING = 7,
	WV_ADD_SOURCE_CHANGED = 11,
	WV_ADD_HISTORY_CHANGED = 13,
	WV_ADD_NAVIGATION_COMPLETED = 15,
	WV_ADD_PROCESS_FAILED = 25,
	WV_ADD_SCRIPT_ON_DOCUMENT_CREATED = 27,
	WV_REMOVE_SCRIPT_ON_DOCUMENT_CREATED = 28,
	WV_EXECUTE_SCRIPT = 29,
	WV_RELOAD = 31,
	WV_ADD_WEB_MESSAGE_RECEIVED = 34,
	WV_CALL_DEV_TOOLS_PROTOCOL_METHOD = 36,
	WV_GET_CAN_GO_BACK = 38,
	WV_GET_CAN_GO_FORWARD = 39,
	WV_GO_BACK = 40,
	WV_GO_FORWARD = 41,
	WV_STOP = 43,
	WV_ADD_NEW_WINDOW_REQUESTED = 44,
	WV_ADD_DOCUMENT_TITLE_CHANGED = 46,
	WV_GET_DOCUMENT_TITLE = 48,
	WV_ADD_WEB_RESOURCE_REQUESTED = 55,
	WV_ADD_WEB_RESOURCE_REQUESTED_FILTER = 57,
	WV3_SET_VIRTUAL_HOST_MAPPING = 71,
	WV4_ADD_DOWNLOAD_STARTING = 75,
	WV13_GET_PROFILE = 105,
	PROFILE_PUT_PREFERRED_COLOR_SCHEME = 9,

	SET_PUT_ARE_DEV_TOOLS_ENABLED = 12,
	SET2_PUT_USER_AGENT = 22,

	NAV_COMPLETED_GET_IS_SUCCESS = 3,
	PROCESS_FAILED_GET_KIND = 3,
	NAV_COMPLETED_GET_STATUS = 4,
	MSG_TRY_GET_STRING = 5,
	NEW_WIN_GET_URI = 3,
	NEW_WIN_PUT_HANDLED = 6,
	RES_REQ_GET_REQUEST = 3,
	RES_REQ_PUT_RESPONSE = 5,
	REQ_GET_URI = 3,
	DL_START_GET_OPERATION = 3,
	DL_START_PUT_CANCEL = 5,
	DL_OP_GET_URI = 9,
	DL_OP_GET_CONTENT_DISPOSITION = 10,
	DL_OP_GET_MIME_TYPE = 11,
	DL_OP_GET_TOTAL_BYTES = 12,
};

static const GUID IID_CONTROLLER2 = {0xc979903e, 0xd4ca, 0x4228, {0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab}};
static const GUID IID_SETTINGS2 = {0xee9a0f68, 0xf46c, 0x4e32, {0xac, 0x23, 0xef, 0x8c, 0xac, 0x22, 0x4d, 0x2a}};
static const GUID IID_WEBVIEW2_3 = {0xa0d6df20, 0x3b92, 0x416d, {0xaa, 0x0c, 0x43, 0x7a, 0x9c, 0x72, 0x78, 0x57}};
static const GUID IID_WEBVIEW2_4 = {0x20d02d59, 0x6df2, 0x42dc, {0xbd, 0x06, 0xf9, 0x8a, 0x69, 0x4b, 0x13, 0x02}};
static const GUID IID_WEBVIEW2_13 = {0xf75f09a8, 0x667e, 0x4983, {0x88, 0xd6, 0xc8, 0x77, 0x3f, 0x31, 0x5e, 0x84}};

// ---- Calls through a vtable, by slot ----

typedef void *com;  // an interface pointer

static inline void *slot(com o, int i) { return (*(void ***)o)[i]; }

typedef HRESULT(STDMETHODCALLTYPE *f_v)(com);
typedef HRESULT(STDMETHODCALLTYPE *f_p)(com, void *);
typedef HRESULT(STDMETHODCALLTYPE *f_pp)(com, void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *f_ppp)(com, void *, void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *f_i)(com, int);
typedef HRESULT(STDMETHODCALLTYPE *f_pi)(com, const void *, int);
typedef HRESULT(STDMETHODCALLTYPE *f_ppi)(com, const void *, const void *, int);
typedef HRESULT(STDMETHODCALLTYPE *f_rect)(com, RECT);
typedef struct {
	BYTE a, r, g, b;
} wv_color;
typedef HRESULT(STDMETHODCALLTYPE *f_color)(com, wv_color);
typedef HRESULT(STDMETHODCALLTYPE *f_response)(com, void *, int, const wchar_t *, const wchar_t *, com *);

#define CALL_V(o, i) ((f_v)slot(o, i))(o)
#define CALL_P(o, i, a) ((f_p)slot(o, i))(o, (void *)(a))
#define CALL_PP(o, i, a, b) ((f_pp)slot(o, i))(o, (void *)(a), (void *)(b))
#define CALL_PPP(o, i, a, b, c) ((f_ppp)slot(o, i))(o, (void *)(a), (void *)(b), (void *)(c))
#define CALL_I(o, i, a) ((f_i)slot(o, i))(o, a)


// take is a string WebView2 gave (CoTaskMem's) as UTF-8, freed (free the
// result).
static char *take(wchar_t *w, size_t *len) {
	int n = 0;
	char *s = wh_utf8(w ? w : L"", -1, &n);
	if (w) CoTaskMemFree(w);
	if (len) *len = (size_t)n;
	return s;
}

// ---- Handlers: the callbacks WebView2 calls, COM objects of our own ----

typedef struct wh_web wh_web;

// A handler is a completed handler's or an event's (asuka_win_handler's):
// Invoke(this, a, b), a an HRESULT or the sender, b the result or the
// event's arguments. It holds a reference to its view's state while
// WebView2 holds it.
typedef void (*view_fn)(wh_web *w, uintptr_t a, void *b);

static void web_unref(wh_web *w);
static void *new_handler(wh_web *w, view_fn fn);

// hr is a completed handler's HRESULT (32 bits, its register's rest not
// ours).
static inline HRESULT hr_of(uintptr_t a) { return (HRESULT)(int32_t)(uint32_t)a; }

// ---- The environment ----

typedef struct {
	void (*fn)(void *ctx, void *env);
	void *ctx;
} env_wait;

static com shared_env;  // the process's, once it's made (not ours to release)

static void env_made(void *ctx, void *env, HRESULT hr) {
	env_wait *w = ctx;
	if (env) shared_env = env;
	if (!env) wh_log("no WebView2 environment (HRESULT 0x%08lx): is the Microsoft Edge WebView2 Runtime installed? (https://go.microsoft.com/fwlink/p/?LinkId=2124703)", (unsigned long)hr);
	w->fn(w->ctx, env);
	free(w);
}

// wh_web_environment calls fn with the app's WebView2 environment (NULL if
// there's none: no runtime): the process's (asuka_win_webview2), which web
// views, hidden pages and packages' C share. Main thread.
void wh_web_environment(void (*fn)(void *ctx, void *env), void *ctx) {
	env_wait *w = wh_alloc(sizeof *w);
	w->fn = fn, w->ctx = ctx;
	asuka_win_webview2(env_made, w);
}

// ---- A view's WebView2 ----

enum {
	OP_URL,
	OP_HTML,
	OP_PRELOAD,
	OP_USER_AGENT,
	OP_FILTER,
	OP_EVAL,
	OP_NAVIGATE,
	OP_VIDEO,
	OP_DEV_TOOLS,
};

// An op: what waits for the controller, in order.
typedef struct {
	int op;
	wchar_t *s;
	uint8_t b;
	// OP_DEV_TOOLS: a method, its params (JSON), what its answer becomes
	// (THEN_*) and who it goes to.
	char *method, *params;
	int then;
	wh_answer reply;
	void *ctx;
} web_op;

typedef struct {
	char *source, *poster;
	char *subtitle, *audio;  // the app's choices (NULL: the platform's)
	char **files;            // SubtitleFiles: URL, language, label in turn
	uint32_t nfiles;
	int32_t mode;  // AK_IMAGE_MODE_*
	double rate, volume, start_at, interval;
	bool hide_controls, auto_play, loop, muted, pip;
} video;

struct wh_web {
	LONG refs;  // the node's, and each handler's and wait's
	node *n;    // NULL once closed
	com controller, view, settings;
	web_op *ops;
	uint32_t nops, capops;
	wchar_t **scripts;  // the preload's script IDs
	uint32_t nscripts;
	wchar_t *url, *html, *preload;
	char **block;
	uint32_t nblock;
	bool requests, filtered, loading, closed;
	bool crash_reloaded;  // reloaded after its page's process ended, before a page loaded since
	video *video;
};

static void web_free_ops(wh_web *w) {
	for (uint32_t i = 0; i < w->nops; i++) {
		web_op *o = &w->ops[i];
		if (o->reply) o->reply(o->ctx, NULL, 0, "the WebView went");
		free(o->s), free(o->method), free(o->params);
	}
	free(w->ops);
	w->ops = NULL, w->nops = w->capops = 0;
}

static void web_unref(wh_web *w) {
	if (InterlockedDecrement(&w->refs) > 0) return;
	web_free_ops(w);
	for (uint32_t i = 0; i < w->nscripts; i++) free(w->scripts[i]);
	free(w->scripts);
	free(w->url);
	free(w->html);
	free(w->preload);
	wh_free_cstrings(w->block, w->nblock);
	if (w->video) {
		free(w->video->source), free(w->video->poster), free(w->video->subtitle), free(w->video->audio);
		wh_free_cstrings(w->video->files, w->video->nfiles);
	}
	free(w->video);
	free(w);
}

typedef struct {
	view_fn fn;
	wh_web *w;
} view_handler;

static void view_invoke(void *ctx, void *a, void *b) {
	view_handler *v = ctx;
	v->fn(v->w, (uintptr_t)a, b);
}

static void view_free(void *ctx) {
	view_handler *v = ctx;
	web_unref(v->w);
	free(v);
}

static void *new_handler(wh_web *w, view_fn fn) {
	view_handler *v = wh_alloc(sizeof *v);
	v->fn = fn;
	v->w = w;
	InterlockedIncrement(&w->refs);
	return asuka_win_handler(view_invoke, v, view_free);
}

static void run_op(wh_web *w, const web_op *o);

// do_op runs an op now if the view's WebView2 is ready, else once it is.
static void queue_op(wh_web *w, web_op o) {
	if (w->view) {
		run_op(w, &o);
		free(o.s), free(o.method), free(o.params);
		return;
	}
	if (w->closed) {
		if (o.reply) o.reply(o.ctx, NULL, 0, "the WebView went");
		free(o.s), free(o.method), free(o.params);
		return;
	}
	if (w->nops == w->capops) {
		w->capops = w->capops ? w->capops * 2 : 8;
		w->ops = wh_realloc(w->ops, w->capops * sizeof *w->ops);
	}
	w->ops[w->nops++] = o;
}

static void do_op(wh_web *w, int op, const wchar_t *s, uint8_t b) { queue_op(w, (web_op){.op = op, .s = s ? _wcsdup(s) : NULL, .b = b}); }

static char *source(wh_web *w, size_t *len) {
	wchar_t *p = NULL;
	CALL_P(w->view, WV_GET_SOURCE, &p);
	return take(p, len);
}

static void put_str(ak_buf *b, const char *s, size_t n) { ak_buf_str(b, s, n); }

// web_state reports the page's address, title and history (WebState).
static void web_state(wh_web *w) {
	node *n = w->n;
	if (!n || w->video || !w->view || !node_listening(n, AK_EVENT_WEB_STATE)) return;
	ak_buf b = {0};
	size_t len;
	char *s = source(w, &len);
	put_str(&b, s, len);
	free(s);
	wchar_t *t = NULL;
	CALL_P(w->view, WV_GET_DOCUMENT_TITLE, &t);
	s = take(t, &len);
	put_str(&b, s, len);
	free(s);
	BOOL back = FALSE, fwd = FALSE;
	CALL_P(w->view, WV_GET_CAN_GO_BACK, &back);
	CALL_P(w->view, WV_GET_CAN_GO_FORWARD, &fwd);
	uint8_t flags = 0;
	float progress = 1;
	if (w->loading) {
		flags |= 1;
		progress = 0.1f;  // WebView2 doesn't tell how far a load is
	}
	if (back) flags |= 2;
	if (fwd) flags |= 4;
	ak_buf_u8(&b, flags);
	ak_buf_f32(&b, progress);
	wh_emit_bytes(n, AK_EVENT_WEB_STATE, b.data, b.len);
	ak_buf_free(&b);
}

static void on_navigation_starting(wh_web *w, uintptr_t a, void *b) {
	(void)a, (void)b;
	if (w->closed) return;
	w->loading = true;
	web_state(w);
}

static void on_navigation_completed(wh_web *w, uintptr_t a, void *args) {
	(void)a;
	if (w->closed) return;
	w->loading = false;
	BOOL ok = FALSE;
	CALL_P(args, NAV_COMPLETED_GET_IS_SUCCESS, &ok);
	if (ok) {
		w->crash_reloaded = false;
		size_t len;
		char *s = source(w, &len);
		wh_emit_str(w->n, AK_EVENT_NAVIGATED, s, len);
		free(s);
	} else if (!w->video) {
		int status = 0;
		CALL_P(args, NAV_COMPLETED_GET_STATUS, &status);
		char msg[96];
		int len = snprintf(msg, sizeof msg, "the page couldn't load (WebView2 error %d)", status);
		wh_emit_str(w->n, AK_EVENT_FAILED, msg, (size_t)len);
	}
	web_state(w);
}

// on_process_failed: the page's renderer ended or stopped answering (out
// of memory, a crash): reported, and the page loaded again once, so the
// view isn't left blank; again before a page loads, only reported. The
// browser's own process ending is reported alone.
static void on_process_failed(wh_web *w, uintptr_t a, void *args) {
	(void)a;
	if (w->closed) return;
	int kind = 0;
	CALL_P(args, PROCESS_FAILED_GET_KIND, &kind);
	// RENDER_PROCESS_EXITED, RENDER_PROCESS_UNRESPONSIVE (the main frame's).
	bool page = kind == 1 || kind == 2;
	static const char msg[] = "the page's web process ended";
	wh_emit_str(w->n, AK_EVENT_FAILED, msg, sizeof msg - 1);
	if (page && !w->crash_reloaded) {
		w->crash_reloaded = true;
		CALL_V(w->view, WV_RELOAD);
	}
	web_state(w);
}

static void on_state_changed(wh_web *w, uintptr_t a, void *b) {
	(void)a, (void)b;
	if (!w->closed) web_state(w);
}

static void video_message(wh_web *w, const char *msg, size_t len);

static void on_message(wh_web *w, uintptr_t a, void *args) {
	(void)a;
	if (w->closed) return;
	wchar_t *p = NULL;
	if (FAILED(CALL_P(args, MSG_TRY_GET_STRING, &p))) return;
	size_t len;
	char *s = take(p, &len);
	if (w->video) video_message(w, s, len);
	else wh_emit_str(w->n, AK_EVENT_MESSAGE, s, len);
	free(s);
}

static void on_new_window(wh_web *w, uintptr_t a, void *args) {
	(void)a;
	if (w->closed) return;
	wchar_t *p = NULL;
	CALL_P(args, NEW_WIN_GET_URI, &p);
	CALL_I(args, NEW_WIN_PUT_HANDLED, TRUE);
	if (node_listening(w->n, AK_EVENT_NEW_WINDOW)) {
		size_t len;
		char *s = take(p, &len);
		wh_emit_str(w->n, AK_EVENT_NEW_WINDOW, s, len);
		free(s);
		return;
	}
	if (p) {
		CALL_P(w->view, WV_NAVIGATE, p);
		CoTaskMemFree(p);
	}
}

// blocked matches a URL against Block's patterns: a host (its subdomains
// too), or with a /, a part of the URL.
static bool blocked(const char *u, char **patterns, uint32_t n) {
	const char *host = u, *p = strstr(u, "://");
	if (p) host = p + 3;
	size_t hl = strcspn(host, "/?#");
	const char *at = NULL;
	for (size_t i = 0; i < hl; i++)
		if (host[i] == '@') at = host + i;
	if (at) {
		hl -= (size_t)(at + 1 - host);
		host = at + 1;
	}
	for (size_t i = hl; i > 0; i--)
		if (host[i - 1] == ':') {
			hl = i - 1;
			break;
		}
	for (uint32_t i = 0; i < n; i++) {
		const char *pat = patterns[i];
		size_t pl = strlen(pat);
		if (strchr(pat, '/')) {
			if (strstr(u, pat)) return true;
		} else if ((hl == pl && !strncmp(host, pat, pl)) || (hl > pl + 1 && host[hl - pl - 1] == '.' && !strncmp(host + hl - pl, pat, pl))) {
			return true;
		}
	}
	return false;
}

static void on_request(wh_web *w, uintptr_t a, void *args) {
	(void)a;
	if (w->closed) return;
	com req = NULL;
	CALL_P(args, RES_REQ_GET_REQUEST, &req);
	if (!req) return;
	wchar_t *p = NULL;
	CALL_P(req, REQ_GET_URI, &p);
	asuka_com_release(req);
	size_t len;
	char *u = take(p, &len);
	if (w->requests) wh_emit_str(w->n, AK_EVENT_REQUEST, u, len);
	if (blocked(u, w->block, w->nblock) && shared_env) {
		com resp = NULL;
		((f_response)slot(shared_env, ENV_CREATE_WEB_RESOURCE_RESPONSE))(shared_env, NULL, 403, L"Blocked", L"", &resp);
		if (resp) {
			CALL_P(args, RES_REQ_PUT_RESPONSE, resp);
			asuka_com_release(resp);
		}
	}
	free(u);
}

// file_name is a download's name: its Content-Disposition's, or the URL's
// last part.
static void file_name(const char *disp, const char *u, const char **name, size_t *len) {
	const char *f = strstr(disp, "filename=");
	if (f) {
		f += strlen("filename=");
		while (*f == '"' || *f == ';' || *f == ' ') f++;
		size_t n = strcspn(f, ";");
		while (n > 0 && (f[n - 1] == '"' || f[n - 1] == ' ' || f[n - 1] == ';')) n--;
		if (n > 0) {
			*name = f, *len = n;
			return;
		}
	}
	size_t end = strcspn(u, "?#");
	size_t start = end;
	while (start > 0 && u[start - 1] != '/') start--;
	*name = u + start, *len = end - start;
}

// on_download reports a file the page can't show, with Download, instead
// of saving it, when the core listens.
static void on_download(wh_web *w, uintptr_t a, void *args) {
	(void)a;
	if (w->closed || !node_listening(w->n, AK_EVENT_DOWNLOAD)) return;
	com op = NULL;
	CALL_P(args, DL_START_GET_OPERATION, &op);
	CALL_I(args, DL_START_PUT_CANCEL, TRUE);
	if (!op) return;
	wchar_t *p = NULL;
	size_t ul, ml, dl, nl;
	CALL_P(op, DL_OP_GET_URI, &p);
	char *u = take(p, &ul);
	p = NULL;
	CALL_P(op, DL_OP_GET_MIME_TYPE, &p);
	char *mime = take(p, &ml);
	p = NULL;
	CALL_P(op, DL_OP_GET_CONTENT_DISPOSITION, &p);
	char *disp = take(p, &dl);
	INT64 size = -1;
	CALL_P(op, DL_OP_GET_TOTAL_BYTES, &size);
	asuka_com_release(op);
	const char *name;
	file_name(disp, u, &name, &nl);
	ak_buf b = {0};
	put_str(&b, u, ul);
	put_str(&b, mime, ml);
	put_str(&b, name, nl);
	ak_buf_u64(&b, (uint64_t)size);
	wh_emit_bytes(w->n, AK_EVENT_DOWNLOAD, b.data, b.len);
	ak_buf_free(&b);
	free(u), free(mime), free(disp);
}

static void on_script_added(wh_web *w, uintptr_t a, void *id) {
	if (FAILED(hr_of(a)) || !id) return;
	w->scripts = wh_realloc(w->scripts, (w->nscripts + 1) * sizeof *w->scripts);
	w->scripts[w->nscripts++] = _wcsdup(id);
}

static void add_script(wh_web *w, const wchar_t *js) {
	void *h = new_handler(w, on_script_added);
	CALL_PP(w->view, WV_ADD_SCRIPT_ON_DOCUMENT_CREATED, js, h);
	asuka_com_release(h);
}

// add_event adds an event's handler (its token kept by nobody: the
// controller's Close removes them all).
static void add_event(wh_web *w, com o, int index, void (*fn)(wh_web *w, uintptr_t a, void *b)) {
	int64_t tok;  // EventRegistrationToken
	void *h = new_handler(w, fn);
	CALL_PP(o, index, h, &tok);
	asuka_com_release(h);
}

static void color_scheme(wh_web *w, bool dark) {
	if (!w->view) return;
	com wv13 = asuka_com_query(w->view, &IID_WEBVIEW2_13);
	if (!wv13) return;
	com profile = NULL;
	CALL_P(wv13, WV13_GET_PROFILE, &profile);
	asuka_com_release(wv13);
	if (!profile) return;
	CALL_I(profile, PROFILE_PUT_PREFERRED_COLOR_SCHEME, dark ? 2 : 1);  // DARK, LIGHT
	asuka_com_release(profile);
}

static void web_resized(wh_web *w) {
	if (!w->controller || !w->n || !w->n->hwnd) return;
	RECT r;
	GetClientRect(w->n->hwnd, &r);
	((f_rect)slot(w->controller, CTL_PUT_BOUNDS))(w->controller, r);
}

static wchar_t *assets_dir(void) {
	const char *a = asuka_desktop_assets_dir();
	return wh_wide(a, (int)strlen(a));
}

static void load_video(wh_web *w);

// set_up readies a view's controller, once made: its events, the bridge,
// its colors and size; then what waited for it.
static void set_up(wh_web *w, com controller) {
	w->controller = controller;
	CALL_P(controller, CTL_GET_CORE_WEBVIEW2, &w->view);
	if (!w->view) return;
	CALL_P(w->view, WV_GET_SETTINGS, &w->settings);
	if (w->settings) CALL_I(w->settings, SET_PUT_ARE_DEV_TOOLS_ENABLED, TRUE);
	com c2 = asuka_com_query(controller, &IID_CONTROLLER2);
	if (c2) {
		// Transparent until the page draws: the view's background shows.
		((f_color)slot(c2, CTL2_PUT_DEFAULT_BACKGROUND_COLOR))(c2, (wv_color){0, 0, 0, 0});
		asuka_com_release(c2);
	}
	com wv3 = asuka_com_query(w->view, &IID_WEBVIEW2_3);
	if (wv3) {
		wchar_t *dir = assets_dir();
		((f_ppi)slot(wv3, WV3_SET_VIRTUAL_HOST_MAPPING))(wv3, ASSETS_HOST, dir, 1);  // ALLOW
		free(dir);
		asuka_com_release(wv3);
	}
	add_event(w, w->view, WV_ADD_NAVIGATION_STARTING, on_navigation_starting);
	add_event(w, w->view, WV_ADD_NAVIGATION_COMPLETED, on_navigation_completed);
	add_event(w, w->view, WV_ADD_PROCESS_FAILED, on_process_failed);
	add_event(w, w->view, WV_ADD_SOURCE_CHANGED, on_state_changed);
	add_event(w, w->view, WV_ADD_HISTORY_CHANGED, on_state_changed);
	add_event(w, w->view, WV_ADD_DOCUMENT_TITLE_CHANGED, on_state_changed);
	add_event(w, w->view, WV_ADD_WEB_MESSAGE_RECEIVED, on_message);
	add_event(w, w->view, WV_ADD_NEW_WINDOW_REQUESTED, on_new_window);
	add_event(w, w->view, WV_ADD_WEB_RESOURCE_REQUESTED, on_request);
	com wv4 = asuka_com_query(w->view, &IID_WEBVIEW2_4);
	if (wv4) {
		add_event(w, wv4, WV4_ADD_DOWNLOAD_STARTING, on_download);
		asuka_com_release(wv4);
	}
	add_script(w, BRIDGE_JS);
	color_scheme(w, H.dark);
	web_resized(w);
	CALL_I(controller, CTL_PUT_IS_VISIBLE, TRUE);
	web_op *ops = w->ops;
	uint32_t n = w->nops;
	w->ops = NULL, w->nops = w->capops = 0;
	for (uint32_t i = 0; i < n; i++) {
		run_op(w, &ops[i]);
		free(ops[i].s), free(ops[i].method), free(ops[i].params);
	}
	free(ops);
}

static void controller_made(wh_web *w, uintptr_t a, void *controller) {
	if (FAILED(hr_of(a)) || !controller) {
		if (!w->closed) wh_log("making a WebView2 failed (HRESULT 0x%08lx)", (unsigned long)hr_of(a));
		return;
	}
	if (w->closed) return;  // the view went meanwhile: the controller goes with its handler
	((IUnknown *)controller)->lpVtbl->AddRef((IUnknown *)controller);
	set_up(w, controller);
}

static void with_environment(void *ctx, void *env) {
	wh_web *w = ctx;
	if (env && !w->closed && w->n && w->n->hwnd) {
		void *h = new_handler(w, controller_made);
		CALL_PP(env, ENV_CREATE_CONTROLLER, w->n->hwnd, h);
		asuka_com_release(h);
	}
	web_unref(w);  // the wait's
}

void wh_make_web(node *n) {
	n->hwnd = wh_make_view(0);
	wh_web *w = wh_alloc(sizeof *w);
	w->refs = 2;  // the node's, and the wait's for the environment
	w->n = n;
	n->web = w;
	if (n->kind == AK_KIND_VIDEO) {
		w->video = wh_alloc(sizeof *w->video);
		w->video->rate = w->video->volume = 1;
	}
	wh_web_environment(with_environment, w);
}

// load navigates to s: a URL, or an asset (served from the assets'
// virtual host, so it reaches the others).
static void load(wh_web *w, const wchar_t *s) {
	if (!s || !*s) return;
	if (wcschr(s, L':')) {
		CALL_P(w->view, WV_NAVIGATE, s);
		return;
	}
	while (*s == L'/' || *s == L'\\') s++;
	size_t n = wcslen(s) + 32;
	wchar_t *u = wh_alloc(n * sizeof *u);
	_snwprintf(u, n, L"https://%ls/%ls", ASSETS_HOST, s);
	for (wchar_t *p = u; *p; p++)
		if (*p == L'\\') *p = L'/';
	CALL_P(w->view, WV_NAVIGATE, u);
	free(u);
}

// filter has WebView2 tell the host of every request (WebResourceRequested)
// once something wants to see them: Block, Requests.
static void filter(wh_web *w) {
	if (w->filtered || (w->nblock == 0 && !w->requests)) return;
	w->filtered = true;
	((f_pi)slot(w->view, WV_ADD_WEB_RESOURCE_REQUESTED_FILTER))(w->view, L"*", 0);  // ALL
}

// ---- The DevTools protocol: evaluating, cookies ----

enum { THEN_RAW, THEN_EVALUATED, THEN_COOKIES };

typedef struct {
	char *method;
	int then;
	wh_answer reply;
	void *ctx;
	HRESULT hr;
	char *result;
	size_t len;
} dev_call;

// evaluated is Runtime.evaluate's answer: the value's JSON (null for
// undefined), or what was thrown (its message, not its stack).
static void evaluated(dev_call *c) {
	dsk_json *r = dsk_json_parse(c->result, c->len);
	const dsk_json *ex = dsk_json_get(r, "exceptionDetails");
	if (!r) {
		c->reply(c->ctx, NULL, 0, "WebView2: Runtime.evaluate's answer isn't JSON");
	} else if (ex) {
		const char *msg = dsk_json_str(ex, "text", "");
		const char *desc = dsk_json_str(dsk_json_get(ex, "exception"), "description", "");
		if (*desc) msg = desc;
		size_t n = strcspn(msg, "\n");
		char *m = wh_strndup(msg, n > 0 ? n : strlen(msg));
		c->reply(c->ctx, NULL, 0, m);
		free(m);
	} else {
		const dsk_json *v = dsk_json_get(dsk_json_get(r, "result"), "value");
		if (!v) {
			c->reply(c->ctx, (const uint8_t *)"null", 4, NULL);  // undefined
		} else {
			dsk_jw jw = {0};
			dsk_jw_value(&jw, v);
			size_t n;
			char *t = dsk_jw_take(&jw, &n);
			c->reply(c->ctx, (const uint8_t *)t, n, NULL);
			free(t);
		}
	}
	dsk_json_free(r);
}

// cookies is Network.getCookies's answer as a Cookie header.
static void cookies(dev_call *c) {
	dsk_json *r = dsk_json_parse(c->result, c->len);
	const dsk_json *cs = dsk_json_get(r, "cookies");
	ak_buf b = {0};
	for (size_t i = 0; cs && cs->type == DSK_JARR && i < cs->n; i++) {
		if (b.len) ak_buf_put(&b, "; ", 2);
		ak_buf_printf(&b, "%s=%s", dsk_json_str(&cs->kids[i], "name", ""), dsk_json_str(&cs->kids[i], "value", ""));
	}
	c->reply(c->ctx, b.data, b.len, NULL);
	ak_buf_free(&b);
	dsk_json_free(r);
}

// dev_answer answers a DevTools call, on a later turn than WebView2's
// callback: what the answer does (closing the view) mustn't happen inside
// it.
static void dev_answer(void *ctx) {
	dev_call *c = ctx;
	if (FAILED(c->hr)) {
		char msg[160];
		snprintf(msg, sizeof msg, "WebView2: %s failed (0x%lx)", c->method, (unsigned long)c->hr);
		c->reply(c->ctx, NULL, 0, msg);
	} else if (c->then == THEN_EVALUATED) {
		evaluated(c);
	} else if (c->then == THEN_COOKIES) {
		cookies(c);
	} else {
		c->reply(c->ctx, (const uint8_t *)c->result, c->len, NULL);
	}
	free(c->method);
	free(c->result);
	free(c);
}

static void dev_done(void *ctx, void *a, void *json) {
	dev_call *c = ctx;
	c->hr = hr_of((uintptr_t)a);
	if (SUCCEEDED(c->hr)) {
		int n = 0;
		c->result = wh_utf8(json, -1, &n);
		c->len = (size_t)n;
	}
	wh_post(dev_answer, c);
}

// dev_tools calls a DevTools protocol method in view's page (an
// ICoreWebView2's CallDevToolsProtocolMethod) and answers on a later turn.
static void dev_tools(com view, const char *method, const char *params, int then, wh_answer reply, void *ctx) {
	dev_call *c = wh_alloc(sizeof *c);
	c->method = wh_strdup(method);
	c->then = then;
	c->reply = reply;
	c->ctx = ctx;
	void *h = asuka_win_handler(dev_done, c, NULL);
	wchar_t *m = wh_wide(method, (int)strlen(method)), *p = wh_wide(params, (int)strlen(params));
	HRESULT hr = CALL_PPP(view, WV_CALL_DEV_TOOLS_PROTOCOL_METHOD, m, p, h);
	free(m), free(p);
	asuka_com_release(h);
	if (FAILED(hr)) {
		c->hr = hr;
		dev_answer(c);  // the handler wasn't kept: it won't be called
	}
}

// evaluate_params is Runtime.evaluate's params for script: its value
// awaited and returned by value.
static char *evaluate_params(const char *script, size_t len) {
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "expression");
	dsk_jw_strn(&w, script, len);
	dsk_jw_key(&w, "awaitPromise");
	dsk_jw_bool(&w, true);
	dsk_jw_key(&w, "returnByValue");
	dsk_jw_bool(&w, true);
	dsk_jw_end(&w);
	return dsk_jw_take(&w, NULL);
}

void wh_web_evaluate(void *view, const char *script, size_t len, wh_answer reply, void *ctx) {
	char *p = evaluate_params(script, len);
	dev_tools(view, "Runtime.evaluate", p, THEN_EVALUATED, reply, ctx);
	free(p);
}

// wh_web_module is the web module: eval (u32 a WebView's view, then the
// script: its value as JSON) and cookies (a URL: the WebViews' cookies for
// it, as a Cookie header, from the environment's store, which every
// WebView shares).
bool wh_web_module(const char *method, size_t mlen, const uint8_t *args, size_t len, wh_answer reply, void *ctx) {
	if (mlen == 4 && !memcmp(method, "eval", 4)) {
		if (len < 4) {
			reply(ctx, NULL, 0, "bad arguments");
			return true;
		}
		node *n = wh_node(wh_u32(args));
		if (!n || !n->web || n->web->video) {
			reply(ctx, NULL, 0, "not a WebView");
			return true;
		}
		web_op o = {.op = OP_DEV_TOOLS, .method = wh_strdup("Runtime.evaluate"), .params = evaluate_params((const char *)args + 4, len - 4),
			.then = THEN_EVALUATED, .reply = reply, .ctx = ctx};
		queue_op(n->web, o);
		return true;
	}
	if (mlen == 7 && !memcmp(method, "cookies", 7)) {
		wh_web *w = NULL;
		uint32_t i = 0;
		void *v;
		while (!w && wh_map_next(&H.nodes, &i, NULL, &v))
			if (((node *)v)->web && !((node *)v)->web->closed) w = ((node *)v)->web;
		if (!w) {
			reply(ctx, NULL, 0, NULL);  // no WebView has been: no cookies
			return true;
		}
		dsk_jw jw = {0};
		dsk_jw_obj(&jw);
		dsk_jw_key(&jw, "urls");
		dsk_jw_arr(&jw);
		dsk_jw_strn(&jw, (const char *)args, len);
		dsk_jw_aend(&jw);
		dsk_jw_end(&jw);
		web_op o = {.op = OP_DEV_TOOLS, .method = wh_strdup("Network.getCookies"), .params = dsk_jw_take(&jw, NULL), .then = THEN_COOKIES,
			.reply = reply, .ctx = ctx};
		queue_op(w, o);
		return true;
	}
	return false;
}

static void run_op(wh_web *w, const web_op *o) {
	switch (o->op) {
	case OP_URL:
		load(w, o->s);
		break;
	case OP_HTML:
		CALL_P(w->view, WV_NAVIGATE_TO_STRING, o->s ? o->s : L"");
		break;
	case OP_PRELOAD:
		for (uint32_t i = 0; i < w->nscripts; i++) {
			CALL_P(w->view, WV_REMOVE_SCRIPT_ON_DOCUMENT_CREATED, w->scripts[i]);
			free(w->scripts[i]);
		}
		w->nscripts = 0;
		add_script(w, BRIDGE_JS);
		if (o->s && *o->s) add_script(w, o->s);
		break;
	case OP_USER_AGENT: {
		com s2 = asuka_com_query(w->settings, &IID_SETTINGS2);
		if (s2) {
			CALL_P(s2, SET2_PUT_USER_AGENT, o->s ? o->s : L"");
			asuka_com_release(s2);
		}
		break;
	}
	case OP_FILTER:
		filter(w);
		break;
	case OP_EVAL:
		CALL_PP(w->view, WV_EXECUTE_SCRIPT, o->s ? o->s : L"", NULL);
		break;
	case OP_NAVIGATE:
		switch (o->b) {
		case 0: CALL_V(w->view, WV_GO_BACK); break;
		case 1: CALL_V(w->view, WV_GO_FORWARD); break;
		case 2: CALL_V(w->view, WV_RELOAD); break;
		case 3: CALL_V(w->view, WV_STOP); break;
		}
		break;
	case OP_VIDEO:
		load_video(w);
		break;
	case OP_DEV_TOOLS:
		dev_tools(w->view, o->method, o->params, o->then, o->reply, o->ctx);
		break;
	}
}

static void eval(wh_web *w, const wchar_t *js) { do_op(w, OP_EVAL, js, 0); }

static void eval8(wh_web *w, const char *js) {
	wchar_t *s = wh_wide(js, (int)strlen(js));
	eval(w, s);
	free(s);
}

// ---- Video ----

// The page's script: it keeps the <video> as the props say (set(c), c the
// props as JSON), and posts what it does: loaded:<width>,<height>, ended,
// failed:<message>, state:<flags>,<time>,<duration>,<rate>,<width>,
// <height> (VideoState's, as the web host's video.js works it out: as it
// changes, and every interval while it plays) and tracks:<JSON> (its text
// tracks, its SubtitleFiles' <track>s among them, and its audio tracks).
#define VIDEO_JS \
	"var v=document.querySelector('video'),A={iv:0,tick:0,last:'',tk:'',start:0,c:{}};" \
	"function P(m){chrome.webview.postMessage(m)}" \
	"function S(force){var f=0,r=v.readyState>=1&&!!v.currentSrc;if(r)f|=1;if(!v.paused&&!v.ended){f|=2;if(A.wait||v.readyState<3)f|=4}" \
	"if(v.ended&&!v.loop)f|=8;if(document.pictureInPictureElement===v)f|=16;if(document.fullscreenElement===v)f|=32;" \
	"var d=r?v.duration:NaN,k=f+'|'+d+'|'+v.playbackRate+'|'+v.videoWidth+'x'+v.videoHeight;if(!force&&k===A.last)return;A.last=k;" \
	"P('state:'+f+','+(v.currentTime||0)+','+d+','+(v.playbackRate||1)+','+v.videoWidth+','+v.videoHeight)}" \
	"function I(t,i){return t.id||String(i)}" \
	"function T(){var ts=[],i,t,a=v.audioTracks;for(i=0;i<v.textTracks.length;i++){t=v.textTracks[i];if(t.kind!=='subtitles'&&t.kind!=='captions')continue;" \
	"ts.push({s:1,c:t.mode==='showing'?1:0,id:I(t,i),l:t.label,g:t.language})}" \
	"for(i=0;a&&i<a.length;i++)ts.push({s:0,c:a[i].enabled?1:0,id:a[i].id||String(i),l:a[i].label,g:a[i].language});" \
	"var k=JSON.stringify(ts);if(k===A.tk)return;A.tk=k;P('tracks:'+k)}" \
	"function Ap(){var c=A.c,i,t,a=v.audioTracks;if(c.sub!=null)for(i=0;i<v.textTracks.length;i++){t=v.textTracks[i];" \
	"if(t.kind!=='subtitles'&&t.kind!=='captions')continue;var m=I(t,i)===c.sub?'showing':'disabled';if(t.mode!==m)t.mode=m}" \
	"if(c.aud!=null&&a){var f=0;for(i=0;i<a.length;i++)if((a[i].id||String(i))===c.aud)f=1;if(f)for(i=0;i<a.length;i++)a[i].enabled=(a[i].id||String(i))===c.aud}}" \
	"function K(){clearInterval(A.tick);A.tick=0;if(A.iv>0&&!v.paused&&!v.ended)A.tick=setInterval(function(){S(1)},A.iv*1000)}" \
	"function U(s){return /^[a-z]+:/i.test(s)?s:'https://asuka.assets/'+s.replace(/^[\\/\\\\]+/,'').replace(/\\\\/g,'/')}" \
	"function set(c){var o=A.c;A.c=c;v.style.objectFit=c.fit;v.controls=c.controls;v.loop=c.loop;v.muted=c.muted;v.autoplay=c.autoplay;" \
	"v.defaultPlaybackRate=v.playbackRate=c.rate;v.volume=c.volume;v.poster=c.poster?U(c.poster):'';v.disablePictureInPicture=!c.pip;A.iv=c.iv;K();" \
	"if(JSON.stringify(o.files)!==JSON.stringify(c.files)){var x=v.querySelectorAll('track');for(var i=0;i<x.length;i++)x[i].remove();" \
	"for(i=0;i+2<c.files.length;i+=3){var t=document.createElement('track');t.kind='subtitles';t.id=c.files[i];t.src=U(c.files[i]);" \
	"if(c.files[i+1])t.srclang=c.files[i+1];t.label=c.files[i+2]||c.files[i+1]||'';v.appendChild(t)}}Ap();S(0)}" \
	"function act(a){if(a===0&&!v.disablePictureInPicture&&document.pictureInPictureEnabled)v.requestPictureInPicture().catch(function(){});" \
	"if(a===1&&document.pictureInPictureElement===v)document.exitPictureInPicture().catch(function(){});" \
	"if(a===2&&v.requestFullscreen)v.requestFullscreen().catch(function(){});if(a===3&&document.fullscreenElement===v)document.exitFullscreen().catch(function(){})}" \
	"window.asukaVideo={set:set,act:act};" \
	"v.addEventListener('loadedmetadata',function(){P('loaded:'+v.videoWidth+','+v.videoHeight);if(A.c.start>0&&!A.started){A.started=1;v.currentTime=A.c.start}Ap();T();S(0)});" \
	"v.addEventListener('ended',function(){P('ended');S(1)});" \
	"v.addEventListener('error',function(){P('failed:'+(v.error&&v.error.message||'the video could not play'))});" \
	"v.addEventListener('waiting',function(){A.wait=1;S(0)});v.addEventListener('playing',function(){A.wait=0;K();S(1)});" \
	"['play','pause','canplay','durationchange','ratechange','enterpictureinpicture','leavepictureinpicture','resize','fullscreenchange'].forEach(function(e){" \
	"v.addEventListener(e,function(){K();S(e==='pause')})});v.addEventListener('seeked',function(){S(1)});" \
	"[v.textTracks,v.audioTracks].forEach(function(l){if(!l)return;l.addEventListener('addtrack',function(){Ap();T()});" \
	"l.addEventListener('removetrack',T);l.addEventListener('change',T)});"

// video_config is the props as the page's set() takes them: JSON, with
// < escaped (it goes in a <script> too).
static char *video_config(video *v, size_t *len) {
	const char *fit = "contain";
	switch (v->mode) {
	case AK_IMAGE_MODE_FILL: fit = "cover"; break;
	case AK_IMAGE_MODE_STRETCH: fit = "fill"; break;
	case AK_IMAGE_MODE_CENTER: fit = "none"; break;
	}
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "fit"), dsk_jw_str(&w, fit);
	dsk_jw_key(&w, "controls"), dsk_jw_bool(&w, !v->hide_controls);
	dsk_jw_key(&w, "loop"), dsk_jw_bool(&w, v->loop);
	dsk_jw_key(&w, "muted"), dsk_jw_bool(&w, v->muted);
	dsk_jw_key(&w, "autoplay"), dsk_jw_bool(&w, v->auto_play);
	dsk_jw_key(&w, "rate"), dsk_jw_num(&w, v->rate > 0 ? v->rate : 1);
	dsk_jw_key(&w, "volume"), dsk_jw_num(&w, v->volume);
	dsk_jw_key(&w, "poster"), dsk_jw_str(&w, v->poster ? v->poster : "");
	dsk_jw_key(&w, "start"), dsk_jw_num(&w, v->start_at);
	dsk_jw_key(&w, "iv"), dsk_jw_num(&w, v->interval);
	dsk_jw_key(&w, "pip"), dsk_jw_bool(&w, v->pip);
	// null through dsk_jw_json, which counts it as a value: dsk_jw_raw
	// would leave out the comma before the next key.
	dsk_jw_key(&w, "sub");
	if (v->subtitle) dsk_jw_str(&w, v->subtitle);
	else dsk_jw_json(&w, "null", 4);
	dsk_jw_key(&w, "aud");
	if (v->audio) dsk_jw_str(&w, v->audio);
	else dsk_jw_json(&w, "null", 4);
	dsk_jw_key(&w, "files");
	dsk_jw_arr(&w);
	for (uint32_t i = 0; i < v->nfiles; i++) dsk_jw_str(&w, v->files[i]);
	dsk_jw_aend(&w);
	dsk_jw_end(&w);
	size_t n;
	char *s = dsk_jw_take(&w, &n);
	if (!s) {
		*len = 0;
		return wh_strndup("{}", 2);
	}
	ak_buf b = {0};
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '<') ak_buf_put(&b, "\\u003c", 6);
		else ak_buf_u8(&b, (uint8_t)s[i]);
	}
	free(s);
	ak_buf_u8(&b, 0);
	char *out = wh_strndup((const char *)b.data, b.len - 1);
	*len = b.len - 1;
	ak_buf_free(&b);
	return out;
}

// video_attrs has the page's <video> take the props.
static void video_attrs(wh_web *w) {
	size_t n;
	char *c = video_config(w->video, &n);
	ak_buf js = {0};
	ak_buf_printf(&js, "window.asukaVideo&&asukaVideo.set(%s)", c);
	ak_buf_u8(&js, 0);
	eval8(w, (const char *)js.data);
	ak_buf_free(&js);
	free(c);
}

// html_escape appends s with HTML's specials escaped (as Go's
// html.EscapeString).
static void html_escape(ak_buf *b, const char *s) {
	for (; *s; s++) {
		switch (*s) {
		case '<': ak_buf_put(b, "&lt;", 4); break;
		case '>': ak_buf_put(b, "&gt;", 4); break;
		case '&': ak_buf_put(b, "&amp;", 5); break;
		case '\'': ak_buf_put(b, "&#39;", 5); break;
		case '"': ak_buf_put(b, "&#34;", 5); break;
		default: ak_buf_u8(b, (uint8_t)*s);
		}
	}
}

// load_video shows the page of the video: a <video> filling it, with
// the script that reports it (VIDEO_JS), set from the props.
static void load_video(wh_web *w) {
	video *v = w->video;
	if (!v || !v->source || !*v->source || !w->view) return;
	ak_buf src = {0};
	if (!strchr(v->source, ':')) {
		const char *s = v->source;
		while (*s == '/' || *s == '\\') s++;
		ak_buf_printf(&src, "https://asuka.assets/");
		for (; *s; s++) ak_buf_u8(&src, *s == '\\' ? '/' : (uint8_t)*s);
	} else {
		ak_buf_put(&src, v->source, strlen(v->source));
	}
	ak_buf_u8(&src, 0);
	size_t cn;
	char *c = video_config(v, &cn);
	ak_buf page = {0};
	ak_buf_printf(&page,
		"<!doctype html><html><head><meta charset=\"utf-8\"><style>html,body{margin:0;height:100%%;background:#000;overflow:hidden}"
		"video{width:100%%;height:100%%;display:block}</style></head><body><video playsinline preload=\"metadata\"%s%s%s%s src=\"",
		v->hide_controls ? "" : " controls", v->loop ? " loop" : "", v->muted ? " muted" : "", v->auto_play ? " autoplay" : "");
	html_escape(&page, (const char *)src.data);
	ak_buf_printf(&page, "%s", "\"></video><script>" VIDEO_JS "set(");
	ak_buf_put(&page, c, cn);
	ak_buf_printf(&page, "%s", ");</script></body></html>");
	free(c);
	wchar_t *html = wh_wide((const char *)page.data, (int)page.len);
	CALL_P(w->view, WV_NAVIGATE_TO_STRING, html);
	free(html);
	ak_buf_free(&page);
	ak_buf_free(&src);
}

// video_tracks turns the page's tracks (JSON) into VideoTracks.
static void video_tracks(node *n, const char *json, size_t len) {
	dsk_json *a = dsk_json_parse(json, len);
	if (!a || a->type != DSK_JARR) {
		dsk_json_free(a);
		return;
	}
	ak_buf b = {0};
	ak_buf_u32(&b, (uint32_t)a->n);
	for (size_t i = 0; i < a->n; i++) {
		const dsk_json *t = &a->kids[i];
		ak_buf_u8(&b, dsk_json_num(t, "s", 0) != 0 ? 0 : 1);
		ak_buf_u8(&b, dsk_json_num(t, "c", 0) != 0);
		const char *ss[3] = {dsk_json_str(t, "id", ""), dsk_json_str(t, "l", ""), dsk_json_str(t, "g", "")};
		for (int k = 0; k < 3; k++) ak_buf_str(&b, ss[k], strlen(ss[k]));
	}
	wh_emit_bytes(n, AK_EVENT_VIDEO_TRACKS, b.data, b.len);
	ak_buf_free(&b);
	dsk_json_free(a);
}

// video_state turns the page's state:<flags>,<time>,<duration>,<rate>,
// <width>,<height> into VideoState (strtod reads JavaScript's NaN and
// Infinity).
static void video_state(node *n, const char *s) {
	double x[6] = {0, 0, NAN, 1, 0, 0};
	char *end;
	for (int i = 0; i < 6 && *s; i++) {
		x[i] = strtod(s, &end);
		if (end == s) break;
		s = *end == ',' ? end + 1 : end;
	}
	ak_buf b = {0};
	ak_buf_u8(&b, (uint8_t)x[0]);
	ak_buf_f64(&b, x[1]);
	ak_buf_f64(&b, x[2]);
	ak_buf_f32(&b, (float)x[3]);
	ak_buf_f32(&b, (float)x[4]);
	ak_buf_f32(&b, (float)x[5]);
	wh_emit_bytes(n, AK_EVENT_VIDEO_STATE, b.data, b.len);
	ak_buf_free(&b);
}

static void video_message(wh_web *w, const char *msg, size_t len) {
	node *n = w->n;
	if (len > 7 && !strncmp(msg, "loaded:", 7)) {
		ak_event e = {.kind = AK_EVENT_LOADED};
		char *end;
		e.x = strtod(msg + 7, &end);
		if (*end == ',') e.y = strtod(end + 1, NULL);
		wh_emit(n, &e);
	} else if (len == 5 && !strncmp(msg, "ended", 5)) {
		wh_emit_kind(n, AK_EVENT_ENDED);
	} else if (len >= 7 && !strncmp(msg, "failed:", 7)) {
		wh_emit_str(n, AK_EVENT_FAILED, msg + 7, len - 7);
	} else if (len > 6 && !strncmp(msg, "state:", 6)) {
		video_state(n, msg + 6);
	} else if (len > 7 && !strncmp(msg, "tracks:", 7)) {
		video_tracks(n, msg + 7, len - 7);
	}
}

static void set_str(char **p, ak_span s) {
	free(*p);
	*p = wh_strndup(s.p, s.len);
}

static void set_video_prop(wh_web *w, uint16_t p, const ak_value *v) {
	video *vd = w->video;
	switch (p) {
	case AK_PROP_VIDEO_SOURCE:
		set_str(&vd->source, v->s);
		do_op(w, OP_VIDEO, NULL, 0);
		return;
	case AK_PROP_VIDEO_MODE: vd->mode = (int32_t)v->i; break;
	case AK_PROP_VIDEO_HIDE_CONTROLS: vd->hide_controls = v->i != 0; break;
	case AK_PROP_VIDEO_AUTO_PLAY: vd->auto_play = v->i != 0; break;
	case AK_PROP_VIDEO_LOOP: vd->loop = v->i != 0; break;
	case AK_PROP_VIDEO_MUTED: vd->muted = v->i != 0; break;
	case AK_PROP_VIDEO_RATE: vd->rate = v->f; break;
	case AK_PROP_VIDEO_VOLUME: vd->volume = v->f < 0 ? 0 : v->f > 1 ? 1 : v->f; break;
	case AK_PROP_VIDEO_POSTER: set_str(&vd->poster, v->s); break;
	case AK_PROP_VIDEO_START_AT: vd->start_at = v->f; return;  // the next source's
	case AK_PROP_VIDEO_TIME_INTERVAL: vd->interval = v->f; break;
	case AK_PROP_VIDEO_SUBTITLE: set_str(&vd->subtitle, v->s); break;
	case AK_PROP_VIDEO_AUDIO_TRACK: set_str(&vd->audio, v->s); break;
	case AK_PROP_VIDEO_SUBTITLE_FILES:
		wh_free_cstrings(vd->files, vd->nfiles);
		vd->nfiles = wh_decode_cstrings(&vd->files, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_VIDEO_PICTURE_IN_PICTURE: vd->pip = v->i != 0; break;
	default: return;  // Headers: a <video> sends none of the app's; AutoPictureInPicture: phones'
	}
	video_attrs(w);
}

// ---- The host's calls ----

void wh_web_set_prop(node *n, uint16_t p, const ak_value *v) {
	wh_web *w = n->web;
	if (!w) return;
	if (w->video) {
		set_video_prop(w, p, v);
		return;
	}
	switch (p) {
	case AK_PROP_WEB_VIEW_URL:
		free(w->url);
		w->url = wh_wspan(v->s);
		do_op(w, OP_URL, w->url, 0);
		break;
	case AK_PROP_WEB_VIEW_HTML:
		free(w->html);
		w->html = wh_wspan(v->s);
		do_op(w, OP_HTML, w->html, 0);
		break;
	case AK_PROP_WEB_VIEW_PRELOAD:
		free(w->preload);
		w->preload = wh_wspan(v->s);
		do_op(w, OP_PRELOAD, w->preload, 0);
		break;
	case AK_PROP_WEB_VIEW_USER_AGENT: {
		wchar_t *ua = wh_wspan(v->s);
		do_op(w, OP_USER_AGENT, ua, 0);
		free(ua);
		break;
	}
	case AK_PROP_WEB_VIEW_BLOCK:
		wh_free_cstrings(w->block, w->nblock);
		w->nblock = wh_decode_cstrings(&w->block, (const uint8_t *)v->s.p, v->s.len);
		do_op(w, OP_FILTER, NULL, 0);
		break;
	case AK_PROP_WEB_VIEW_REQUESTS:
		w->requests = v->i != 0;
		do_op(w, OP_FILTER, NULL, 0);
		break;
	}
}

void wh_web_command(node *n, uint16_t cmd, const uint8_t *args, size_t len) {
	wh_web *w = n->web;
	if (!w) return;
	switch (cmd) {
	case AK_CMD_PLAY:
		if (w->video) eval(w, L"document.querySelector('video').play()");
		break;
	case AK_CMD_PAUSE:
		if (w->video) eval(w, L"document.querySelector('video').pause()");
		break;
	case AK_CMD_VIDEO_ACTION:
		if (w->video && len >= 1) {
			char js[64];
			snprintf(js, sizeof js, "window.asukaVideo&&asukaVideo.act(%d)", args[0]);
			eval8(w, js);
		}
		break;
	case AK_CMD_SEEK:
		if (w->video && len >= 8) {
			uint64_t bits = wh_u64(args);
			double t;
			memcpy(&t, &bits, sizeof t);
			char js[96];
			snprintf(js, sizeof js, "document.querySelector('video').currentTime=%.17g", t);
			eval8(w, js);
		}
		break;
	case AK_CMD_EVAL: {
		wchar_t *js = wh_wide((const char *)args, (int)len);
		eval(w, js);
		free(js);
		break;
	}
	case AK_CMD_WEB_NAVIGATE:
		if (len > 0) do_op(w, OP_NAVIGATE, NULL, args[0]);
		break;
	}
}

void *wh_web_view(node *n) { return n && n->web && !n->web->closed && !n->web->video ? n->web->view : NULL; }

void wh_web_resized(node *n) {
	if (n->web) web_resized(n->web);
}

void wh_web_color_scheme(node *n, bool dark) {
	if (n->web) color_scheme(n->web, dark);
}

// A controller and its window that go on a later turn of the message
// loop.
typedef struct {
	wh_web *w;
	HWND hwnd;
} closing;

static void close_controller(wh_web *w) {
	if (!w->controller) return;
	CALL_V(w->controller, CTL_CLOSE);
	asuka_com_release(w->settings);
	asuka_com_release(w->view);
	asuka_com_release(w->controller);
	w->controller = w->view = w->settings = NULL;
}

static void close_later(void *ctx) {
	closing *c = ctx;
	close_controller(c->w);
	if (c->hwnd) DestroyWindow(c->hwnd);
	web_unref(c->w);
	free(c);
}

// wh_web_close lets the view's controller go, and then hwnd (its window,
// NULL for none), on a later turn of the message loop: never inside one
// of WebView2's callbacks, which an event the page raised can be (a
// controller closed in its own callback is freed under it).
void wh_web_close(node *n, HWND hwnd) {
	wh_web *w = n->web;
	if (!w) {
		if (hwnd) DestroyWindow(hwnd);
		return;
	}
	n->web = NULL;
	w->closed = true;
	w->n = NULL;
	web_free_ops(w);
	if (hwnd) {
		ShowWindow(hwnd, SW_HIDE);
		SetParent(hwnd, H.park);  // out of its window, until it goes
	}
	closing *c = wh_alloc(sizeof *c);
	c->w = w;  // the node's reference, let go once closed
	c->hwnd = hwnd;
	wh_post(close_later, c);
}

// wh_web_close_all lets every controller go at once: as the process exits
// (automation), not from WebView2's callbacks, so its browser goes quietly.
void wh_web_close_all(void) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (!n->web) continue;
		n->web->closed = true;
		close_controller(n->web);
	}
}
