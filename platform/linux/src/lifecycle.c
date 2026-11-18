// lifecycle.c: the app's lifecycle, quitting, its windows' and routers'
// state, and its languages.
//
// As on a Mac, the app is in the background while none of its windows
// shows (each hidden, minimized or suspended: GDK's state for a window the
// compositor doesn't show), and as it quits: its last window closing, the
// session ending (GtkApplication's query-end), or a signal (SIGTERM,
// SIGINT, SIGHUP), which quits it as closing it does. Each sends
// Lifecycle, and the core's handlers run then: std/store writes
// what's pending. GLib's memory monitor's warnings are MemoryWarning.
//
// Quitting: the Quit command (u8 flags, 1 relaunch), the app menu's Quit,
// closing the app's last window, a signal and the session ending each ask
// the core first, once (asuka_quit_request): a quit it allowed only
// finishes. It may cancel it, except as the session ends.
//
// Windows come back as the user left them: their size and whether they
// were maximized, in asuka-window.json in the app's data directory, by
// the key the app gives each (the main window's "main" unless it names
// another; a Window's only with a StateKey). Not their place: the
// compositor places windows. Saved as a window closes and as the app
// quits; tests and automation runs keep none.
//
// A window's Router address comes back as the window does: the last
// Location command each window was sent is kept by the window's key in
// asuka-routes.json (the main window's under "main"): the main window's
// is given to the core before the app runs (asuka_set_start), where its
// Router starts; a Window's, by its StateKey, as its Router starts
// following the window's address (the Location event). None is kept under
// asuka dev or automation unless ASUKA_ROUTES=1 (dsk_routes_path). Written
// a moment after it changes, and as the app goes to the background or
// quits (an automation run's end too).
//
// Languages: the environment's Languages are the app's own language
// (SaveLocale's, kept in its data directory) and then the user's
// (LANGUAGE, LANG). The Locale command says which the app shows and its
// direction: GTK lays its own widgets out right to left with the default
// direction, and each view takes its node's.
#include "host.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void lx_each_node(void (*f)(node *n, void *ctx), void *ctx);
void lx_label_tabs_all(void);
void lx_split_badges_all(void);

// ---- Lifecycle ----

static uint32_t toplevel_state(win *w) {
	GdkSurface *s = gtk_native_get_surface((GtkNative *)w->window);
	return s ? (uint32_t)gdk_toplevel_get_state((GdkToplevel *)s) : 0;
}

void lx_routes_flush(void);

// lifecycle sends s to the core: Foreground and Background as the app's
// state changes (not again when it's already in it), MemoryWarning each
// time.
static void lifecycle(int s) {
	switch (s) {
	case AK_LIFECYCLE_BACKGROUND:
		lx_routes_flush();
		if (H.in_background) return;
		H.in_background = true;
		break;
	case AK_LIFECYCLE_FOREGROUND:
		if (!H.in_background || H.quitting) return;
		H.in_background = false;
		break;
	}
	if (!H.root) return;
	lx_dispatch(&(ak_event){.id = H.root->id, .kind = AK_EVENT_LIFECYCLE, .i = s});
}

// shown is whether a window is seen: visible, and neither minimized nor
// suspended.
static bool shown(win *w) {
	if (w->closing || !gtk_widget_get_visible((GtkWidget *)w->window)) return false;
	return !(toplevel_state(w) & (GDK_TOPLEVEL_STATE_MINIMIZED | GDK_TOPLEVEL_STATE_SUSPENDED));
}

// lx_lifecycle_check sends Background when no window of the app's is seen,
// Foreground when one is again.
void lx_lifecycle_check(void) {
	if (!H.root) return;  // not mounted: it starts in the foreground
	for (uint32_t i = 0; i < H.wins.len; i++)
		if (shown(H.wins.data[i])) {
			lifecycle(AK_LIFECYCLE_FOREGROUND);
			return;
		}
	lifecycle(AK_LIFECYCLE_BACKGROUND);
}

static void on_vis(GObject *o, GParamSpec *ps, gpointer d) {
	(void)o;
	(void)ps;
	(void)d;
	lx_lifecycle_check();
}

static void on_realized(GtkWidget *window, gpointer d) {
	GdkSurface *s = gtk_native_get_surface((GtkNative *)window);
	if (s) g_signal_connect(s, "notify::state", G_CALLBACK(on_vis), d);
}

// Destroyed (the user closed the main window, or the core a Window): it's
// no longer the app's.
static void on_destroy(GtkWidget *window, gpointer d) {
	(void)window;
	win *w = d;
	w->closing = true;
	lx_free_win(w);
	lx_lifecycle_check();
}

// lx_watch_window follows a window's showing: made visible or hidden, its
// surface's state, and its end.
void lx_watch_window(win *w) {
	g_signal_connect(w->window, "notify::visible", G_CALLBACK(on_vis), w);
	g_signal_connect(w->window, "realize", G_CALLBACK(on_realized), w);
	g_signal_connect(w->window, "destroy", G_CALLBACK(on_destroy), w);
}

// ---- Quitting ----

// lx_ask_quit asks the core's on quit handlers whether the app may quit
// (ending: the session is, and they can't refuse), and reports whether it
// goes on. Once allowed, it's allowed.
bool lx_ask_quit(bool ending) {
	if (H.quit_allowed) return true;
	int cancel = 0;
	if (H.root) {
		lx_enter_begin();
		cancel = asuka_quit_request(ending ? 1 : 0);
		lx_enter_end();
	}
	if (cancel && !ending) return false;
	H.quit_allowed = true;
	return true;
}

// quit ends the app as closing its last window does: Background (the
// store written), then the application's main loop.
static void quit(void) {
	lifecycle(AK_LIFECYCLE_BACKGROUND);
	lx_save_windows();
	H.quitting = true;
	g_application_quit((GApplication *)H.app);
}

// lx_request_quit quits the app as the user's quit does, unless an on
// quit handler cancels it.
void lx_request_quit(void) {
	if (lx_ask_quit(false)) quit();
}

static void quit_later(void *ctx) {
	(void)ctx;
	if (lx_ask_quit(false)) {
		quit();
		return;
	}
	lx_relaunch_set(false);  // canceled: no relaunch either
}

// lx_quit_command is the Quit command (quit(), relaunch()): the user's
// Quit, once the core has returned; with the relaunch flag, the app starts
// again once this process has ended (main.c).
void lx_quit_command(ak_span args) {
	bool relaunch = args.len && (args.p[0] & 1);
	if (relaunch) lx_relaunch_set(true);
	lx_after_core(quit_later, NULL);
}

static void quit_without_windows(void *ctx) {
	(void)ctx;
	if (!H.no_main || H.quitting || H.cfg.keep_running || H.cfg.background) return;
	for (uint32_t i = 0; i < H.wins.len; i++) {
		win *w = H.wins.data[i];
		if (w != H.main && !w->closing && gtk_widget_get_visible((GtkWidget *)w->window)) return;
	}
	lx_request_quit();
}

// lx_closed_window hears the user close a window: an app without a main
// window ends with its last (its main one, hidden, would hold it), as a
// quit does, unless it keeps running.
void lx_closed_window(void) {
	if (H.no_main) lx_after_core(quit_without_windows, NULL);
}

// lx_closing_last reports whether closing w would close the app's last
// window: the app quits then, so the user's close asks first.
bool lx_closing_last(win *w) {
	for (uint32_t i = 0; i < H.wins.len; i++)
		if (H.wins.data[i] != w && !H.wins.data[i]->closing) return false;
	return true;
}

static void on_query_end(GtkApplication *a, gpointer d) {
	(void)a;
	(void)d;
	lx_ask_quit(true);  // on quit, which can't cancel it now
	lifecycle(AK_LIFECYCLE_BACKGROUND);
}

static void on_shutdown(GApplication *a, gpointer d) {
	(void)a;
	(void)d;
	lx_save_windows();
	lifecycle(AK_LIFECYCLE_BACKGROUND);
	H.quitting = true;
}

// lx_watch_app follows the application's end: the session ending
// (query-end) and its shutdown however it quits. The session manager is
// joined as it registers.
void lx_watch_app(void) {
	GObjectClass *c = *(GObjectClass **)H.app;
	if (g_object_class_find_property(c, "register-session")) {
		g_object_set(H.app, "register-session", 1, NULL);
		g_signal_connect(H.app, "query-end", G_CALLBACK(on_query_end), NULL);
	}
	g_signal_connect(H.app, "shutdown", G_CALLBACK(on_shutdown), NULL);
}

static int signals_seen;

static gboolean on_signal(gpointer data) {
	(void)data;
	if (++signals_seen > 1) _exit(128 + SIGTERM);  // a second ends it at once
	lx_enter_begin();
	lx_request_quit();
	lx_enter_end();
	return 1;
}

// lx_watch_signals quits the app on SIGTERM, SIGINT and SIGHUP as closing
// it does (on quit's handlers asked first), so its last values are
// written; a second signal ends it at once (the main thread may be
// stuck: the second one's default action).
void lx_watch_signals(void) {
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_unix_signal_add(SIGHUP, on_signal, NULL);
}

static void on_low_memory(GMemoryMonitor *m, int level, gpointer d) {
	(void)m;
	(void)level;
	(void)d;
	lifecycle(AK_LIFECYCLE_MEMORY_WARNING);
}

// lx_watch_memory sends MemoryWarning as GLib's memory monitor warns.
void lx_watch_memory(void) {
	if (!g_memory_monitor_dup_default) return;
	GMemoryMonitor *m = g_memory_monitor_dup_default();  // kept for the process's life
	if (m) g_signal_connect(m, "low-memory-warning", G_CALLBACK(on_low_memory), NULL);
}

// ---- Where the windows were ----

static dsk_winstate *states;

// no_state: tests and automation runs keep no windows' state.
static bool no_state(void) {
	static const char *const keys[] = {"ASUKA_SNAPSHOT", "ASUKA_SCRIPT", "ASUKA_TEST"};
	for (size_t i = 0; i < AK_LEN(keys); i++) {
		const char *v = getenv(keys[i]);
		if (v && *v) return true;
	}
	return false;
}

static dsk_winstate *state_store(void) {
	if (!states) {
		char *p = NULL;
		if (!no_state()) p = dsk_join(lx_data_dir(), "asuka-window.json");
		states = dsk_winstate_open(p);
		free(p);
	}
	return states;
}

// lx_restore_window sizes w as the user left it under its key, and
// maximizes it if it was.
void lx_restore_window(win *w) {
	if (str_empty(w->state_key)) return;
	const dsk_json *s = dsk_winstate_load(state_store(), w->state_key);
	if (!s) return;
	int width = (int)dsk_json_num(s, "width", 0), height = (int)dsk_json_num(s, "height", 0);
	if (width < 100 || height < 100) return;
	gtk_window_set_default_size(w->window, width, height);
	if (dsk_json_bool(s, "maximized", false)) gtk_window_maximize(w->window);
	w->restored = true;
}

// lx_save_window keeps w's size (its unmaximized one: GTK's default size,
// which follows the user's resizing) and whether it's maximized, under its
// key.
void lx_save_window(win *w) {
	if (str_empty(w->state_key) || !w->window) return;
	int width = 0, height = 0;
	gtk_window_get_default_size(w->window, &width, &height);
	if (width <= 0 || height <= 0) return;
	char json[96];
	snprintf(json, sizeof json, "{\"width\":%d,\"height\":%d,\"maximized\":%s}", width, height, gtk_window_is_maximized(w->window) ? "true" : "false");
	dsk_winstate_save(state_store(), w->state_key, json);
}

void lx_save_windows(void) {
	for (uint32_t i = 0; i < H.wins.len; i++) lx_save_window(H.wins.data[i]);
}

// ---- Where the routers were ----

static dsk_routes *routes;
static guint routes_timer;

static dsk_routes *route_store(void) {
	if (!routes) {
		char *p = dsk_routes_path();
		routes = dsk_routes_open(p && *p ? p : NULL);
		free(p);
	}
	return routes;
}

// lx_routes_flush writes what changed now (the app quits).
void lx_routes_flush(void) {
	if (routes_timer) {
		g_source_remove(routes_timer);
		routes_timer = 0;
	}
	if (routes) dsk_routes_flush(routes);
}

static gboolean routes_due(gpointer d) {
	(void)d;
	routes_timer = 0;
	dsk_routes_flush(route_store());
	return 0;
}

// route_key is the key the router address of n's window is kept by: the
// main window's "main" (the core sends its Location without a key), a
// Window's its StateKey (NULL: none).
static const char *route_key(node *n) {
	win *w = n ? lx_win_for(n) : NULL;
	if (!w && n) w = lx_win_of(n);
	if (!w || w == H.main) return "main";
	return str_empty(w->state_key) ? NULL : w->state_key;
}

// lx_route_location keeps a Location command's address, by the key it
// names, else by the window it's sent to (n).
void lx_route_location(node *n, ak_span args) {
	ak_location l;
	if (!ak_decode_location((const uint8_t *)args.p, args.len, &l)) return;
	char kb[256];
	const char *key = kb;
	if (l.window.len)
		snprintf(kb, sizeof kb, "%.*s", (int)l.window.len, l.window.p);
	else if (!(key = route_key(n)))
		return;
	if (dsk_routes_set(route_store(), key, l.href.p, l.href.len, l.state.p, l.state.len) && !routes_timer)
		routes_timer = g_timeout_add(300, routes_due, NULL);
}

// lx_route_start tells the core where the main window was left, before
// the app runs, so its Router starts there (asuka_set_start): the first
// screen shown is the one the user left, with the screens under it.
void lx_route_start(void) {
	const char *href, *state;
	size_t state_len;
	if (dsk_routes_location(route_store(), "main", &href, &state, &state_len) && *href) asuka_set_start(href);
}

// lx_listen_location: a Window's Router follows its address: it's given
// the one the window last had (by its StateKey), if it had one. The main
// window's started there (lx_route_start).
void lx_listen_location(node *n) {
	win *w = lx_win_for(n);
	if (!w) w = lx_win_of(n);
	if (!w || w == H.main || str_empty(w->state_key)) return;
	const char *href, *state;
	size_t state_len;
	if (!dsk_routes_location(route_store(), w->state_key, &href, &state, &state_len) || !*href) return;
	ak_buf b = {0};
	ak_append_location_event(&b, ak_cstr(href), ak_strn(state, state_len));
	lx_dispatch(&(ak_event){.id = n->id, .kind = AK_EVENT_LOCATION, .s = ak_strn(b.data, b.len)});
	ak_buf_free(&b);
}

// ---- Languages ----

static char *locale_path, *saved_locale;

// user_languages are the languages the user reads, most preferred first,
// as BCP 47 tags: LANGUAGE's list, else LC_ALL, LC_MESSAGES or LANG.
static void user_languages(ak_buf *out) {
	const char *lang = getenv("LANGUAGE");
	char *copy = strdup(lang ? lang : "");
	for (char *v = strtok(copy, ":"); v; v = strtok(NULL, ":")) {
		char t[64];
		snprintf(t, sizeof t, "%s", v);
		char *cut = strpbrk(t, ".@");
		if (cut) *cut = 0;
		if (!*t || !strcmp(t, "C") || !strcmp(t, "POSIX")) continue;
		for (char *p = t; *p; p++)
			if (*p == '_') *p = '-';
		if (out->len) ak_buf_u8(out, ',');
		ak_buf_put(out, t, strlen(t));
	}
	free(copy);
	if (!out->len) {
		static const char *const keys[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
		for (size_t i = 0; i < AK_LEN(keys); i++) {
			const char *v = getenv(keys[i]);
			if (!v || !*v) continue;
			char t[64];
			snprintf(t, sizeof t, "%s", v);
			char *cut = strpbrk(t, ".@");
			if (cut) *cut = 0;
			if (*t && strcmp(t, "C") && strcmp(t, "POSIX")) {
				for (char *p = t; *p; p++)
					if (*p == '_') *p = '-';
				ak_buf_put(out, t, strlen(t));
			}
			break;
		}
	}
	if (!out->len) ak_buf_put(out, "en-US", 5);
}

// join_languages is the environment's Languages: the app's own language
// first, then the user's without it.
static void join_languages(void) {
	ak_buf user = {0}, out = {0};
	user_languages(&user);
	const char *list = lx_buf_cstr(&user);
	if (!str_empty(saved_locale)) ak_buf_put(&out, saved_locale, strlen(saved_locale));
	char *copy = strdup(list);
	for (char *t = strtok(copy, ","); t; t = strtok(NULL, ","))
		if (str_empty(saved_locale) || strcasecmp(t, saved_locale)) {
			if (out.len) ak_buf_u8(&out, ',');
			ak_buf_put(&out, t, strlen(t));
		}
	free(copy);
	str_setc(&H.languages, lx_buf_cstr(&out));
	ak_buf_free(&user);
	ak_buf_free(&out);
}

void lx_init_locale(void) {
	locale_path = dsk_join(lx_data_dir(), "asuka-locale");
	size_t n;
	char *s = dsk_read_file(locale_path, &n);
	if (s) {
		while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\r')) s[--n] = 0;
		saved_locale = s;
	}
	join_languages();
}

const char *lx_word_locale(void);
const char *lx_word_locale(void) { return S(H.locale); }

void lx_set_locale(ak_span args) {
	if (args.len < 1) return;
	bool rtl = args.p[0] & 1;
	ak_span tag = {args.p + 1, args.len - 1};
	if (!ak_span_eqc(tag, S(H.locale))) {
		str_set(&H.locale, tag);
		// A badge is said in the language: "3 items".
		lx_label_tabs_all();
		lx_split_badges_all();
	}
	if (rtl == H.rtl) return;
	H.rtl = rtl;
	gtk_widget_set_default_direction(rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
	lx_each_node((void (*)(node *, void *))lx_apply_direction_inherit, NULL);
	lx_queue_chrome();
}

void lx_save_locale(ak_span tag) {
	if (ak_span_eqc(tag, S(saved_locale))) return;
	str_set(&saved_locale, tag);
	if (!tag.len)
		remove(locale_path);
	else
		dsk_write_file(locale_path, tag.p, tag.len);
	join_languages();
	lx_env_changed();
}

// lx_apply_direction sets the way a node reads, from its LayoutDirection
// or the app's, on its control (frames stay physical: the core places the
// children).
void lx_apply_direction(node *n) {
	bool rtl = H.rtl;
	if (n->dir == AK_LAYOUT_DIRECTION_LEFT_TO_RIGHT) rtl = false;
	if (n->dir == AK_LAYOUT_DIRECTION_RIGHT_TO_LEFT) rtl = true;
	n->rtl = rtl;
	if (n->inner) gtk_widget_set_direction(n->inner, rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
	if (n->kind == AK_KIND_TEXT || n->kind == AK_KIND_TEXT_FIELD || n->kind == AK_KIND_RICH_TEXT) lx_apply_text(n);
}

void lx_apply_direction_inherit(node *n, void *ctx) {
	(void)ctx;
	if (n->dir == AK_LAYOUT_DIRECTION_INHERIT) lx_apply_direction(n);
}

// lx_posix_locale is the locale of the user's formats: LC_ALL, LC_TIME or
// LANG, as en_US.
const char *lx_posix_locale(void) {
	static char buf[64];
	static const char *const keys[] = {"LC_ALL", "LC_TIME", "LANG"};
	for (size_t i = 0; i < AK_LEN(keys); i++) {
		const char *v = getenv(keys[i]);
		if (v && *v && strcmp(v, "C") && strcmp(v, "POSIX")) {
			snprintf(buf, sizeof buf, "%s", v);
			char *cut = strpbrk(buf, ".@");
			if (cut) *cut = 0;
			return buf;
		}
	}
	return "en_US";
}

// lx_time_zone is the system's IANA time zone: TZ, /etc/localtime's link,
// or /etc/timezone.
const char *lx_time_zone(void) {
	static char buf[128];
	const char *tz = getenv("TZ");
	if (tz && *tz == ':') tz++;
	if (tz && *tz && *tz != '/') {
		snprintf(buf, sizeof buf, "%s", tz);
		return buf;
	}
	char *real = realpath("/etc/localtime", NULL);
	if (real) {
		const char *z = strstr(real, "zoneinfo/");
		if (z) {
			snprintf(buf, sizeof buf, "%s", z + 9);
			free(real);
			return buf;
		}
		free(real);
	}
	size_t n;
	char *s = dsk_read_file("/etc/timezone", &n);
	if (s) {
		while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
		snprintf(buf, sizeof buf, "%s", s);
		free(s);
		if (*buf) return buf;
	}
	return "UTC";
}
