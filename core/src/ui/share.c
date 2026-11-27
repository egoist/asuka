// share.c: what the app asks of the system and hears from it outside its
// views: the share sheet, haptics, the clipboard, opening a URL, a rich
// text's commands, a screen reader's announcements and focus; native modules' calls (Call, and the
// Result that answers it); the URLs and files the app is opened with,
// and what other apps share with it.
#include "ui/ui_ext.h"

#include <stdlib.h>

static ak_ui_root *root_now(void) { return ak_ui.cur ? ak_ui.cur : ak_ui.root; }

static void root_command(ak_cmd c, const void *args, size_t n) {
	ak_ui_root *r = root_now();
	if (r && r->el) ak_batch_command(r->batch, r->el->id, c, args, n);
}

static void str_command(ak_cmd c, const ak_str *s) { root_command(c, s ? s->data : "", s ? s->len : 0); }

// ak_ui_share shows the system's share sheet for text (a URL is shared
// as a link).
void ak_ui_share(ak_str *text) { str_command(AK_CMD_SHARE, text); }
// ak_ui_haptic plays haptic feedback (an AK_HAPTIC_*).
void ak_ui_haptic(int64_t kind) {
	uint8_t k = (uint8_t)kind;
	if (kind >= 0 && kind < AK_HAPTIC_COUNT) root_command(AK_CMD_HAPTIC, &k, 1);
}
// ak_ui_color_scheme shows the app light or dark whatever the system's
// setting, or follows it again (AK_COLOR_SCHEME_*).
// A preview's (asuka dev's) shows over it until the app comes back.
void ak_ui_color_scheme(int64_t scheme) {
	if (scheme >= 0 && scheme < AK_COLOR_SCHEME_COUNT) ak_preview_color_scheme((int)scheme);
}
// ak_ui_copy puts text on the clipboard.
void ak_ui_copy(ak_str *text) { str_command(AK_CMD_COPY, text); }
// ak_ui_open_url asks the system to open url: a web address in the
// browser, another scheme in the app that handles it.
void ak_ui_open_url(ak_str *url) { str_command(AK_CMD_OPEN_URL, url); }
// ak_ui_announce has a screen reader, if one is on, read text out.
void ak_ui_announce(ak_str *text) { str_command(AK_CMD_ANNOUNCE, text); }
// ak_ui_share_close closes the share sheet the app's root is (an iOS
// share extension's, Android's AsukaShareActivity): 0 done, 1 cancelled,
// 2 open the app with what was shared.
void ak_ui_share_close(int64_t how) {
	uint8_t h = (uint8_t)how;
	if (how >= 0 && how <= 2) root_command(AK_CMD_SHARE_CLOSE, &h, 1);
}

static bool env_on(const char *name) {
	const char *v = getenv(name);
	return v && strcmp(v, "1") == 0;
}

const char *ak_ui_process(void) {
	if (env_on("ASUKA_SHARE_SHEET")) return "share";
	if (env_on("ASUKA_NOTIFICATION_SERVICE")) return "notification";
	return "app";
}

static void first_native(ak_view *d, void *ctx) {
	ak_view **t = ctx;
	if (!*t) *t = d;
}

// ak_ui_a11y_focus moves a screen reader's focus to v's view (its first,
// for a view of none of its own).
void ak_ui_a11y_focus(ak_view *v) {
	if (!v) return;
	ak_view *t = NULL;
	ak_view_each_native(v, first_native, &t);
	if (t && !t->dead) ak_batch_command(ak_view_batch(t), t->id, AK_CMD_A11Y_FOCUS, NULL, 0);
}

// ---- Native modules' calls ----

typedef struct {
	uint32_t id;
	ak_ui_root *r;
	ak_closure *done;  // fn(bytes?, string), or NULL
	ak_future *fut;    // or a future (retained)
	bool text;         // the future's value is a string
} pending_call;

static AK_VEC(pending_call) calls;
static uint32_t next_call;

static ak_struct *error_of(ak_span msg) {
	ak_struct *e = ak_struct_new(&ak_shape_error);
	e->f[0].p = ak_str_span(msg);
	return e;
}

static void answer(pending_call *c, bool ok, ak_span b) {
	if (c->fut) {
		if (ok) ak_future_resolve(c->fut, (ak_val){.p = c->text ? (void *)ak_str_span(b) : (void *)ak_bytes_new(b.p, b.len)});
		else ak_future_reject(c->fut, error_of(b));
		ak_release(c->fut);
	}
	if (c->done) {
		ak_val args[2] = {{.p = ok ? (void *)ak_bytes_new(b.p, b.len) : NULL}, {.p = ok ? ak_str_empty() : ak_str_span(b)}};
		ak_call_closure(c->done, NULL, args, 2);
		ak_release(args[0].p);
		ak_release(args[1].p);
		ak_release(c->done);
	}
}

static pending_call *call_send(ak_span module, ak_span method, const void *args, size_t n) {
	ak_ui_root *r = root_now();
	if (!r || !r->el || r->disposed) return NULL;
	pending_call c = {.id = ++next_call, .r = r};
	ak_buf *b = ak_batch_command_begin(r->batch, r->el->id, AK_CMD_CALL);
	ak_buf_u32(b, c.id);
	ak_buf_wstr(b, module);
	ak_buf_wstr(b, method);
	ak_buf_put(b, args, n);
	ak_batch_command_end(r->batch);
	ak_vec_push(&calls, c);
	return &calls.data[calls.len - 1];
}

void ak_ext_module_call(ak_span module, ak_span method, const void *args, size_t n, ak_closure *done) {
	pending_call *c = call_send(module, method, args, n);
	if (!c) {
		if (done) {
			pending_call x = {.done = ak_retain(done)};
			answer(&x, false, AK_STR("unsupported: no host to call (no app is shown)"));
		}
		return;
	}
	if (done) c->done = ak_retain(done);
	else calls.len--;  // no answer wanted
}

static ak_future *call_future(ak_span module, ak_span method, const void *args, size_t n, bool text) {
	ak_future *f = ak_future_new(AK_K_REF);
	pending_call *c = call_send(module, method, args, n);
	if (!c) {
		ak_future_reject(f, error_of(AK_STR("unsupported: no host to call (no app is shown)")));
		return f;
	}
	c->fut = ak_retain(f);
	c->text = text;
	return f;
}

ak_future *ak_ext_call_future(ak_span module, ak_span method, const void *args, size_t n) { return call_future(module, method, args, n, true); }

// ak_ui_call calls a native module's method with args; a future of its
// result's bytes, rejected with its error.
ak_future *ak_ui_call(ak_str *module, ak_str *method, ak_bytes *args) {
	return call_future(ak_span_str(module), ak_span_str(method), args ? args->data : NULL, args ? args->len : 0, false);
}

// result delivers a Result event to its call: u32 id, u8 ok, then the
// result or the error's message.
static void result(ak_span b) {
	if (b.len < 5) return;
	const uint8_t *p = (const uint8_t *)b.p;
	uint32_t id = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	for (uint32_t i = 0; i < calls.len; i++) {
		if (calls.data[i].id != id) continue;
		pending_call c = calls.data[i];
		calls.data[i] = calls.data[--calls.len];
		answer(&c, p[4] != 0, (ak_span){b.p + 5, b.len - 5});
		return;
	}
}

// ak_ui_view_id is the id the host knows v's view by (0: none), for a
// module call that points at a view.
uint32_t ak_ui_view_id(ak_view *v) { return v && v->real && !v->dead ? v->id : 0; }

// ---- What the app is opened with, and what's shared with it ----

typedef struct handler {
	ak_closure *f;
	ak_owner *owner;  // not retained: the handler goes with it
	ak_ui_root *r;
	bool gone;
} handler;

typedef AK_VEC(handler *) handler_vec;

static handler_vec url_handlers, file_handlers, share_handlers, life_handlers, fore_handlers, back_handlers;
static ak_str *pending_url;
static AK_VEC(ak_arr *) pending_files;
// Shares that came before any handler: the share that launched the app.
static AK_VEC(ak_arr *) pending_shares;

// ak_ui_route_url is the router's: it goes to a URL one of its routes
// takes (true), before the app's on_open_url handlers hear of it.
bool (*ak_ui_route_url)(ak_span url);

static void handler_gone(void *ctx) {
	handler *h = ctx;
	h->gone = true;
	handler_vec *vs[] = {&url_handlers, &file_handlers, &share_handlers, &life_handlers, &fore_handlers, &back_handlers};
	for (size_t k = 0; k < AK_LEN(vs); k++) {
		for (uint32_t i = 0; i < vs[k]->len; i++) {
			if (vs[k]->data[i] != h) continue;
			memmove(vs[k]->data + i, vs[k]->data + i + 1, (vs[k]->len - i - 1) * sizeof(handler *));
			vs[k]->len--;
		}
		if (!vs[k]->len) ak_vec_free(vs[k]);
	}
	ak_release(h->f);
	ak_free(h);
}

static handler *add_handler(handler_vec *v, ak_closure *f) {
	handler *h = ak_alloc(sizeof *h);
	h->f = ak_retain(f);
	h->owner = ak_current_owner();
	h->r = root_now();
	ak_vec_push(v, h);
	ak_on_cleanup(handler_gone, h);
	return h;
}

static void call_handlers(handler_vec *v, ak_val arg) {
	// A copy: a handler may add or remove others.
	uint32_t n = v->len;
	handler **hs = ak_alloc((n ? n : 1) * sizeof(handler *));
	if (n) memcpy(hs, v->data, n * sizeof(handler *));
	for (uint32_t i = 0; i < n; i++) {
		bool live = false;
		for (uint32_t j = 0; j < v->len; j++) live |= v->data[j] == hs[i];
		if (live) ak_call_closure(hs[i]->f, hs[i]->owner, &arg, 1);
	}
	ak_free(hs);
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
	return -1;
}

// file_path is a file:// URL's path (internal/fileurl's ToPath), or NULL.
static ak_str *file_path(ak_span s) {
	if (s.len < 7) return NULL;
	for (int i = 0; i < 7; i++)
		if ((s.p[i] | (i < 4 ? 0x20 : 0)) != "file://"[i]) return NULL;
	ak_span rest = {s.p + 7, s.len - 7};
	uint32_t slash = 0;
	while (slash < rest.len && rest.p[slash] != '/') slash++;
	ak_span host = {rest.p, slash};
	if (host.len && !(host.len == 9 && strncmp(host.p, "localhost", 9) == 0)) return NULL;
	ak_span path = {rest.p + slash, rest.len - slash};
	if (!path.len) return NULL;
	ak_strb b = {0};
	for (uint32_t i = 0; i < path.len; i++) {
		char c = path.p[i];
		if (c == '?' || c == '#') break;
		if (c == '%' && i + 2 < path.len && hexval(path.p[i + 1]) >= 0 && hexval(path.p[i + 2]) >= 0) {
			c = (char)(hexval(path.p[i + 1]) << 4 | hexval(path.p[i + 2]));
			i += 2;
		}
		ak_strb_cstr(&b, &c, 1);
	}
	ak_str *p = ak_strb_finish(&b);
	if (p->len >= 3 && p->data[0] == '/' && p->data[2] == ':') {  // /C:/x on Windows
		ak_str *q = ak_str_new(p->data + 1, p->len - 1);
		ak_release(p);
		p = q;
	}
	return p;
}

static void open_url(ak_str *url) {
	if (ak_ui_route_url && ak_ui_route_url(ak_span_str(url))) return;
	if (!url_handlers.len) {
		ak_release(pending_url);
		pending_url = ak_retain(url);
		return;
	}
	call_handlers(&url_handlers, (ak_val){.p = url});
}

static void open_files(ak_arr *paths) {
	if (!file_handlers.len) {
		ak_vec_push(&pending_files, (ak_arr *)ak_retain(paths));
		return;
	}
	call_handlers(&file_handlers, (ak_val){.p = paths});
}

// incoming takes an IncomingURL event's string: one URL, or files the app
// is opened with, as file:// URLs one per line.
static void incoming(ak_span s) {
	ak_arr *paths = ak_arr_new(AK_K_REF, NULL, 0);
	ak_arr *urls = ak_arr_new(AK_K_REF, NULL, 0);
	paths->h.flags = urls->h.flags = 1;
	uint32_t at = 0;
	while (at <= s.len) {
		uint32_t end = at;
		while (end < s.len && s.p[end] != '\n') end++;
		ak_span line = {s.p + at, end - at};
		if (line.len && line.p[line.len - 1] == '\r') line.len--;
		if (line.len) {
			ak_str *p = file_path(line);
			if (p) ak_arr_push(&paths, (ak_val){.p = p});
			else ak_arr_push(&urls, (ak_val){.p = ak_str_span(line)});
		}
		at = end + 1;
	}
	if (paths->len) open_files(paths);
	for (uint32_t i = 0; i < urls->len; i++) open_url(ak_arr_vals(urls)[i].p);
	ak_release(paths);
	ak_release(urls);
}

// ak_ui_on_open_url calls f (fn(string)) with each URL the app is opened
// with (its scheme, a universal or app link) that no router takes,
// while the current owner lives. One that came before any handler (the
// launch's) goes to the first.
void ak_ui_on_open_url(ak_closure *f) {
	add_handler(&url_handlers, f);
	if (pending_url) {
		ak_str *u = pending_url;
		pending_url = NULL;
		ak_call_closure(f, ak_current_owner(), &(ak_val){.p = u}, 1);
		ak_release(u);
	}
}

// ak_ui_on_lifecycle calls f (fn(Lifecycle)) as the app comes to the
// foreground, goes to the background, or the system is low on memory,
// while the current owner lives; on_foreground and on_background call
// theirs (fn()) for the first two.
void ak_ui_on_lifecycle(ak_closure *f) { add_handler(&life_handlers, f); }
void ak_ui_on_foreground(ak_closure *f) { add_handler(&fore_handlers, f); }
void ak_ui_on_background(ak_closure *f) { add_handler(&back_handlers, f); }

static void call_handlers0(handler_vec *v) {
	uint32_t n = v->len;
	handler **hs = ak_alloc((n ? n : 1) * sizeof(handler *));
	if (n) memcpy(hs, v->data, n * sizeof(handler *));
	for (uint32_t i = 0; i < n; i++) {
		bool live = false;
		for (uint32_t j = 0; j < v->len; j++) live |= v->data[j] == hs[i];
		if (live) ak_call_closure(hs[i]->f, hs[i]->owner, NULL, 0);
	}
	ak_free(hs);
}

// ak_ui_lifecycle tells the lifecycle handlers the app's state changed
// (AK_LIFECYCLE_*).
void ak_ui_lifecycle(int64_t state) {
	call_handlers(&life_handlers, (ak_val){.i = state});
	if (state == AK_LIFECYCLE_FOREGROUND) call_handlers0(&fore_handlers);
	else if (state == AK_LIFECYCLE_BACKGROUND) call_handlers0(&back_handlers);
}

// ak_ui_on_open_files calls f (fn([string])) with the files the app is
// opened with, those opened together in one call, as absolute paths.
void ak_ui_on_open_files(ak_closure *f) {
	add_handler(&file_handlers, f);
	uint32_t n = pending_files.len;
	ak_arr **ps = pending_files.data;
	pending_files.data = NULL, pending_files.len = pending_files.cap = 0;
	for (uint32_t i = 0; i < n; i++) {
		ak_call_closure(f, ak_current_owner(), &(ak_val){.p = ps[i]}, 1);
		ak_release(ps[i]);
	}
	ak_free(ps);
}

// ak_ui_on_share calls f (fn([SharedItem])) with what another app shares
// with this one from its share sheet; what came before any handler goes
// to the first.
void ak_ui_on_share(ak_closure *f) {
	add_handler(&share_handlers, f);
	uint32_t n = pending_shares.len;
	ak_arr **ps = pending_shares.data;
	pending_shares.data = NULL, pending_shares.len = pending_shares.cap = 0;
	for (uint32_t i = 0; i < n; i++) {
		ak_call_closure(f, ak_current_owner(), &(ak_val){.p = ps[i]}, 1);
		ak_release(ps[i]);
	}
	ak_free(ps);
}

static void shared(ak_span b) {
	ak_item_vec items = {0};
	ak_decode_items((const uint8_t *)b.p, b.len, &items);
	if (!items.len) {
		ak_vec_free(&items);
		return;
	}
	ak_arr *a = ak_items_value(items.data, items.len, false);
	ak_vec_free(&items);
	if (!share_handlers.len) ak_vec_push(&pending_shares, (ak_arr *)ak_retain(a));
	else call_handlers(&share_handlers, (ak_val){.p = a});
	ak_release(a);
}

// ak_ui_share_files shows the share sheet for the files at paths ([string]):
// mime (empty: the first file's), uti (iOS), title, from (the view it
// points at, on an iPad or a Mac; NULL: none). A future of nothing,
// rejected if it couldn't show.
ak_future *ak_ui_share_files(ak_arr *paths, ak_str *mime, ak_str *uti, ak_str *title, ak_view *from) {
	if (!paths || !paths->len) {
		ak_future *f = ak_future_new(AK_K_REF);
		ak_future_reject(f, error_of(AK_STR("shareFiles with no files")));
		return f;
	}
	ak_buf b = {0};
	ak_buf_u32(&b, ak_ui_view_id(from));
	ak_span m = ak_span_str(mime);
	ak_str *first_abs = NULL;
	if (!m.len) {
		// The first file's type, by its name.
		ak_struct *it = ak_struct_new(&ak_ui_shape_shared_item);
		it->f[2].p = ak_retain(ak_arr_vals(paths)[0].p);
		ak_item w;
		ak_item_wire(it, false, &w, &first_abs);
		m = w.mime;
		if (m.len == 24 && memcmp(m.p, "application/octet-stream", 24) == 0) m = AK_STR("");
		ak_buf_wstr(&b, m);
		ak_release(it);
	} else {
		ak_buf_wstr(&b, m);
	}
	ak_release(first_abs);
	ak_buf_wstr(&b, ak_span_str(uti));
	ak_buf_wstr(&b, ak_span_str(title));
	ak_buf_u32(&b, paths->len);
	for (uint32_t i = 0; i < paths->len; i++) {
		ak_struct *it = ak_struct_new(&ak_ui_shape_shared_item);
		it->f[2].p = ak_retain(ak_arr_vals(paths)[i].p);
		ak_item w;
		ak_str *abs;
		ak_item_wire(it, false, &w, &abs);
		ak_buf_wstr(&b, w.path);
		ak_release(abs);
		ak_release(it);
	}
	ak_future *f = call_future(AK_STR("share"), AK_STR("files"), b.data, b.len, false);
	ak_buf_free(&b);
	return f;
}

// ak_ui_capture draws v as it shows now, with what's in it, into an image
// file in the app's caches, the host's own drawing (the "view" module's
// "capture": u32 the native view, f32 x, y, width, height of v in it (a
// width of -1: all of it), u8 format (imageedit's Format: 0 JPEG, 1 PNG),
// f32 quality (JPEG's, 0 to 1), f32 scale (pixels per point; 0: the
// screen's)): a future of the file's path.
ak_future *ak_ui_capture(ak_view *v, int64_t format, double quality, double scale) {
	ak_view *a;
	float rect[4];
	if (!v || !ak_view_native_rect(v, &a, rect)) {
		ak_future *f = ak_future_new(AK_K_REF);
		ak_future_reject(f, error_of(v ? AK_STR("capture: the view isn't showing") : AK_STR("capture: the Ref holds no view")));
		return f;
	}
	ak_buf b = {0};
	ak_buf_u32(&b, a->id);
	for (int i = 0; i < 4; i++) ak_buf_f32(&b, rect[i]);
	ak_buf_u8(&b, (uint8_t)format);
	ak_buf_f32(&b, (float)quality);
	ak_buf_f32(&b, (float)scale);
	ak_future *f = call_future(AK_STR("view"), AK_STR("capture"), b.data, b.len, true);
	ak_buf_free(&b);
	return f;
}

// ak_ui_read_asset reads one of the app's assets (name: relative to its
// assets/), as its host finds them (the "assets" module's "read": the
// bundle's, Android's AssetManager, the site's, the folder beside the
// executable): a future of its bytes.
ak_future *ak_ui_read_asset(ak_str *name) {
	ak_span n = ak_span_str(name);
	if (!n.len) {
		ak_future *f = ak_future_new(AK_K_REF);
		ak_future_reject(f, error_of(AK_STR("readAsset: no name")));
		return f;
	}
	return call_future(AK_STR("assets"), AK_STR("read"), n.p, n.len, false);
}

// ---- Events that aren't a view's ----

void ak_anim_frame(ak_id id, double seconds);
void ak_app_second_instance(ak_span s);

static bool begin_root_event(void) {
	ak_batch_begin();
	return true;
}

static void end_root_event(void) {
	ak_sched_run();
	ak_batch_end();
	ak_ui_flush();
}

bool ak_ext_dispatch(const ak_event *ev) {
	switch (ev->kind) {
	case AK_EVENT_FRAME:
		begin_root_event();
		ak_perf_display_frame(ak_ui.root, ev->f);
		ak_anim_frame(ev->id, ev->f);
		end_root_event();
		return true;
	case AK_EVENT_RESULT:
		begin_root_event();
		result(ev->s);
		end_root_event();
		return true;
	case AK_EVENT_INCOMING_URL:
		begin_root_event();
		incoming(ev->s);
		end_root_event();
		return true;
	case AK_EVENT_SHARED:
		begin_root_event();
		shared(ev->s);
		end_root_event();
		return true;
	case AK_EVENT_SECOND_INSTANCE:
		begin_root_event();
		ak_app_second_instance(ev->s);
		end_root_event();
		return true;
	}
	return false;
}

// ---- Mounting ----

void ak_locale_mounted(ak_ui_root *r);
void ak_locale_environment(ak_ui_root *r);
void ak_media_cache_free(void);

void ak_accent_mounted(ak_ui_root *r);

void ak_ext_mounted(ak_ui_root *r) {
	ak_accent_mounted(r);  // Accent comes before Locale
	ak_locale_mounted(r);
	ak_preview_mounted(r);
}

void ak_ext_environment(ak_ui_root *r) { ak_locale_environment(r); }

// ak_ext_unmount ends the calls the root's host will never answer, and
// lets the process's caches go: what awaits one is cancelled (the host
// it waited for went, as a task's owner goes: it unwinds, and nothing is
// reported); a callback hears that the root went.
void ak_ext_unmount(ak_ui_root *r) {
	for (uint32_t i = 0; i < calls.len;) {
		if (calls.data[i].r != r) {
			i++;
			continue;
		}
		pending_call c = calls.data[i];
		calls.data[i] = calls.data[--calls.len];
		if (c.fut) {
			ak_future_cancel(c.fut);
			ak_release(c.fut);
			c.fut = NULL;
		}
		answer(&c, false, AK_STR("the root went before the call was answered"));
	}
	if (!calls.len) ak_vec_free(&calls);
	ak_ix_unmount();
}

// ak_ext_shutdown lets go of what the process kept for apps to come: the
// hash placeholders' cache, what the app was opened with and no handler
// heard.
void ak_ext_shutdown(void) {
	ak_media_cache_free();
	ak_release(pending_url);
	pending_url = NULL;
	for (uint32_t i = 0; i < pending_files.len; i++) ak_release(pending_files.data[i]);
	ak_vec_free(&pending_files);
	for (uint32_t i = 0; i < pending_shares.len; i++) ak_release(pending_shares.data[i]);
	ak_vec_free(&pending_shares);
}
