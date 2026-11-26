// platform/screen on Linux: the default GdkDisplay's monitors
// (gdk_display_get_monitors, which changes as one's plugged in or out),
// each one's geometry in application pixels (points, in the desktop's
// layout), its scale, refresh rate, connector (the ID: DP-1, eDP-1) and
// description. GTK 4 has no primary monitor of its own: on X11 it's the
// X server's and the work area the window manager's (_NET_WORKAREA); on
// Wayland the compositor keeps both, so the work area is the bounds, and
// the primary monitor is GNOME's (Mutter's DisplayConfig, its logical
// monitor marked primary), else the one at the layout's origin, else the
// first. GTK can't keep a window out of captures: preventCapture does
// nothing. A desktop's display doesn't turn with the window, and it has
// no status or navigation bar: lock and setBars do nothing. A capture is
// the screenshot portal's (org.freedesktop.portal.Screenshot, not
// interactive: the portal asks the user the first time), a PNG of the
// whole desktop on GNOME, cut to the display asked for when there are
// more, and moved into the app's caches. Main thread, but the capture's
// file work.
#include "asuka_linux.h"
#include "screen.asuka.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MUTTER "org.gnome.Mutter.DisplayConfig"
#define MUTTER_PATH "/org/gnome/Mutter/DisplayConfig"

typedef struct watcher {
	screen_fn_list_Display changed;
	asuka_call *call;
	screen_list_Display last;
	bool first;
	struct watcher *next;
} watcher;

static struct {
	bool started;
	GListModel *list;
	GdkMonitor **monitors;  // those whose changes are followed (a reference each)
	size_t n;
	char *primary;  // Mutter's primary monitor's connector, NULL if not known
	bool mutter;    // Mutter's state is being asked
	watcher *watchers;
} dw;

static screen_list_Display read_displays(void);

static bool same(const screen_list_Display *a, const screen_list_Display *b) {
	if (a->len != b->len) return false;
	for (size_t i = 0; i < a->len; i++) {
		const screen_Display *x = &a->data[i], *y = &b->data[i];
		if (strcmp(x->id, y->id) || strcmp(x->name, y->name) || memcmp(&x->bounds, &y->bounds, sizeof x->bounds) ||
		    memcmp(&x->workArea, &y->workArea, sizeof x->workArea) || x->scale != y->scale || x->refreshRate != y->refreshRate ||
		    x->primary != y->primary)
			return false;
	}
	return true;
}

static void report(watcher *w) {
	screen_list_Display ds = read_displays();
	if (!w->first && same(&ds, &w->last)) {
		screen_list_Display_free(&ds);
		return;
	}
	w->first = false;
	screen_list_Display_free(&w->last);
	w->last = ds;
	screen_fn_list_Display_call(w->changed, ds);
}

static void changed(void) {
	for (watcher *w = dw.watchers; w; w = w->next) report(w);
}

static void on_monitor(gpointer m, gpointer spec, gpointer d) { changed(); }

// follow_monitors follows the changes of the monitors there are, and
// lets go of those gone.
static void follow_monitors(void) {
	guint n = G(g_list_model_get_n_items)(dw.list);
	GdkMonitor **now = calloc(n ? n : 1, sizeof *now);
	for (guint i = 0; i < n; i++) {
		GdkMonitor *m = G(g_list_model_get_item)(dw.list, i);  // a reference: kept
		now[i] = m;
		bool known = false;
		for (size_t k = 0; k < dw.n; k++) known = known || dw.monitors[k] == m;
		if (!known && m) {
			static const char *sigs[] = {"notify::geometry", "notify::scale", "notify::scale-factor", "notify::refresh-rate", "notify::description"};
			for (size_t s = 0; s < sizeof sigs / sizeof *sigs; s++) g_signal_connect(m, sigs[s], on_monitor, NULL);
		}
	}
	for (size_t k = 0; k < dw.n; k++)
		if (dw.monitors[k]) G(g_object_unref)(dw.monitors[k]);  // its handlers go with the last reference
	free(dw.monitors);
	dw.monitors = now;
	dw.n = n;
}

static void on_items(gpointer list, guint pos, guint removed, guint added, gpointer d) {
	follow_monitors();
	changed();
}

// mutter_primary is GetCurrentState's primary logical monitor's first
// monitor's connector (NULL if none says): (ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv}).
static char *mutter_primary(GVariant *state) {
	if (G(g_variant_n_children)(state) < 3) return NULL;
	GVariant *logical = G(g_variant_get_child_value)(state, 2);
	char *out = NULL;
	for (gsize i = 0; !out && i < G(g_variant_n_children)(logical); i++) {
		GVariant *lm = G(g_variant_get_child_value)(logical, i);
		if (G(g_variant_n_children)(lm) >= 6) {
			GVariant *p = G(g_variant_get_child_value)(lm, 4);
			bool primary = G(g_variant_get_boolean)(p);
			G(g_variant_unref)(p);
			if (primary) {
				GVariant *mons = G(g_variant_get_child_value)(lm, 5);
				if (G(g_variant_n_children)(mons) > 0) {
					GVariant *spec = G(g_variant_get_child_value)(mons, 0);
					GVariant *c = G(g_variant_get_child_value)(spec, 0);
					const char *s = G(g_variant_get_string)(c, NULL);
					if (s && *s) out = asuka_strdup(s);
					G(g_variant_unref)(c);
					G(g_variant_unref)(spec);
				}
				G(g_variant_unref)(mons);
			}
		}
		G(g_variant_unref)(lm);
	}
	G(g_variant_unref)(logical);
	return out;
}

static void got_state(GObject *src, GAsyncResult *res, gpointer d) {
	dw.mutter = false;
	free(dw.primary);
	dw.primary = NULL;
	GError *e = NULL;
	GVariant *r = G(g_dbus_connection_call_finish)((GDBusConnection *)src, res, &e);
	if (r) {
		dw.primary = mutter_primary(r);
		G(g_variant_unref)(r);
	} else if (e) {
		G(g_error_free)(e);
	}
	changed();
}

// ask_mutter reads GNOME's primary monitor, then reports the displays.
static void ask_mutter(void) {
	GDBusConnection *c = asuka_linux_bus(G_BUS_TYPE_SESSION, NULL);
	if (!c || dw.mutter) return;
	dw.mutter = true;
	G(g_dbus_connection_call)(c, MUTTER, MUTTER_PATH, MUTTER, "GetCurrentState", NULL, NULL, 0, 2000, NULL, got_state, NULL);
}

static void on_mutter(GDBusConnection *c, const gchar *s, const gchar *path, const gchar *i, const gchar *n, GVariant *v, gpointer d) { ask_mutter(); }

// start follows the monitors, once.
static bool start(void) {
	if (dw.started) return true;
	if (!asuka_linux() || !G(gdk_display_get_default)) return false;
	GdkDisplay *disp = G(gdk_display_get_default)();
	if (!disp || !G(gdk_display_get_monitors)) return false;
	dw.started = true;
	dw.list = G(gdk_display_get_monitors)(disp);
	g_signal_connect(dw.list, "items-changed", on_items, NULL);
	if (!asuka_linux_is_a(disp, "GdkX11Display")) {
		// GNOME on Wayland says which monitor is primary.
		GDBusConnection *c = asuka_linux_bus(G_BUS_TYPE_SESSION, NULL);
		if (c) {
			G(g_dbus_connection_signal_subscribe)(c, MUTTER, MUTTER, "MonitorsChanged", MUTTER_PATH, NULL, 0, on_mutter, NULL, NULL);
			ask_mutter();
		}
	}
	follow_monitors();
	return true;
}

static screen_Rect rect_of(GdkRectangle r) { return (screen_Rect){r.x, r.y, r.width, r.height}; }

// display describes a monitor (not yet whether it's primary).
static screen_Display display(GdkMonitor *m, bool x11) {
	GdkRectangle g = {0};
	G(gdk_monitor_get_geometry)(m, &g);
	screen_Display o = {.bounds = rect_of(g), .workArea = rect_of(g)};
	const char *conn = G(gdk_monitor_get_connector)(m);
	o.id = asuka_strdup(conn ? conn : "");
	const char *desc = G(gdk_monitor_get_description) ? G(gdk_monitor_get_description)(m) : NULL;
	if (desc) {
		o.name = asuka_strdup(desc);
	} else {
		const char *maker = G(gdk_monitor_get_manufacturer)(m), *model = G(gdk_monitor_get_model)(m);
		o.name = asuka_sprintf("%s%s%s", maker ? maker : "", maker && model && *maker && *model ? " " : "", model ? model : "");
	}
	if (G(gdk_monitor_get_scale)) o.scale = G(gdk_monitor_get_scale)(m);
	if (o.scale <= 0) o.scale = G(gdk_monitor_get_scale_factor)(m);
	int hz = G(gdk_monitor_get_refresh_rate)(m);
	if (hz > 0) o.refreshRate = round(hz / 10.0) / 100;  // milli-Hertz, to 0.01
	if (x11 && G(gdk_x11_monitor_get_workarea)) {
		GdkRectangle w = {0};
		G(gdk_x11_monitor_get_workarea)(m, &w);
		if (w.width > 0 && w.height > 0) o.workArea = rect_of(w);
	}
	return o;
}

// read_displays is the monitors, the primary first.
static screen_list_Display read_displays(void) {
	screen_list_Display out = {0};
	if (!start()) {
		out.data = calloc(1, sizeof *out.data);
		return out;
	}
	GdkDisplay *disp = G(gdk_display_get_default)();
	bool x11 = asuka_linux_is_a(disp, "GdkX11Display");
	GdkMonitor *x11_primary = x11 && G(gdk_x11_display_get_primary_monitor) ? G(gdk_x11_display_get_primary_monitor)(disp) : NULL;
	guint n = G(g_list_model_get_n_items)(dw.list);
	out.data = calloc(n ? n : 1, sizeof *out.data);
	ptrdiff_t primary = -1;
	for (guint i = 0; i < n; i++) {
		GdkMonitor *m = G(g_list_model_get_item)(dw.list, i);
		if (!m) continue;
		out.data[out.len] = display(m, x11);
		if (primary < 0 && ((x11 && m == x11_primary) || (!x11 && dw.primary && strcmp(out.data[out.len].id, dw.primary) == 0))) primary = (ptrdiff_t)out.len;
		out.len++;
		G(g_object_unref)(m);
	}
	for (size_t i = 0; primary < 0 && i < out.len; i++)
		if (out.data[i].bounds.x == 0 && out.data[i].bounds.y == 0) primary = (ptrdiff_t)i;
	if (primary < 0 && out.len > 0) primary = 0;
	if (primary > 0) {
		screen_Display p = out.data[primary];
		memmove(out.data + 1, out.data, (size_t)primary * sizeof *out.data);
		out.data[0] = p;
	}
	if (out.len > 0) out.data[0].primary = true;
	return out;
}

static screen_list_Display s_displays(screen_Screens *self, asuka_error *err) { return read_displays(); }

static void stop(void *p) {
	watcher *w = p;
	for (watcher **q = &dw.watchers; *q; q = &(*q)->next)
		if (*q == w) {
			*q = w->next;
			break;
		}
	screen_list_Display_free(&w->last);
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(w->call, &e);
	free(w);
}

static void s_watch(screen_Screens *self, screen_fn_list_Display f, asuka_call *call) {
	watcher *w = calloc(1, sizeof *w);
	w->changed = f;
	w->call = call;
	w->first = true;
	start();
	w->next = dw.watchers;
	dw.watchers = w;
	report(w);
	asuka_call_on_cancel(call, stop, w);
}

static void s_lock(screen_Screens *self, screen_Orientation o, asuka_error *err) {}
static void s_bars(screen_Screens *self, screen_BarStyle s, bool a, bool b, asuka_error *err) {}
static void s_capture(screen_Screens *self, bool on, asuka_error *err) {}

static void stop_shots(void *call) {
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(call, &e);
}

// A desktop doesn't tell an app of its screenshots: it waits for the end.
static void s_shots(screen_Screens *self, screen_fn_int taken, asuka_call *call) { asuka_call_on_cancel(call, stop_shots, call); }

// The display's brightness is the user's to set on a desktop: the app
// neither sets nor reads it.
static void s_set_brightness(screen_Screens *self, double level, asuka_error *err) { (void)self, (void)level, (void)err; }
static double s_brightness(screen_Screens *self, asuka_error *err) { (void)self, (void)err; return -1; }

// ---- Capture ----

#define SCREENSHOT "org.freedesktop.portal.Screenshot"
#define PERMISSIONS "org.freedesktop.impl.portal.PermissionStore"
#define PERMISSIONS_PATH "/org/freedesktop/impl/portal/PermissionStore"

// shot_done gets the capture's path (malloc's, the callee's), or NULL
// and why (err, the callee's). Main thread.
typedef void (*shot_done)(void *ctx, char *path, asuka_error *err);

// A capture: the portal's request (its method's reply and its Response
// signal), then the worker that moves or cuts the portal's file. It goes
// once it has ended (answered, or its caller left) and the reply has come.
typedef struct {
	GDBusConnection *bus;
	char *request;  // the Request object the Response comes from
	guint sub;
	bool replied, ended, working;
	bool gone;  // the caller left (the task cancelled): done isn't called
	bool crop;  // more than one display: the file is the desktop's
	screen_Rect all, want;  // the desktop's bounds, the display's (points)
	char *dir;
	char *src, *out;  // the worker's: the portal's file, the one made
	asuka_error err;
	shot_done done;
	void *ctx;
} shot;

static void shot_free(shot *s) {
	if (!s->replied || !s->ended || s->working) return;
	free(s->request);
	free(s->dir);
	free(s->src);
	free(s->out);
	asuka_error_free(&s->err);
	free(s);
}

static void shot_unsubscribe(shot *s) {
	if (s->sub) G(g_dbus_connection_signal_unsubscribe)(s->bus, s->sub);
	s->sub = 0;
}

// shot_end answers the capture (once): path (taken) or err.
static void shot_end(shot *s, char *path, asuka_error *err) {
	if (s->ended) {
		free(path);
		if (err) asuka_error_free(err);
		return;
	}
	s->ended = true;
	shot_unsubscribe(s);
	if (s->gone) {
		if (path) unlink(path);
		free(path);
		if (err) asuka_error_free(err);
		return;
	}
	s->done(s->ctx, path, err);
}

static void shot_fail(shot *s, uint32_t declared) {
	asuka_error e = {0};
	asuka_error_declared(&e, declared);
	shot_end(s, NULL, &e);
}

// shot_leave is the caller leaving (its task cancelled): done isn't
// called; the portal's request is closed, and what it makes is let go.
static void shot_leave(shot *s) {
	s->gone = true;
	if (s->ended) return;
	if (s->bus && s->request && !s->working)
		G(g_dbus_connection_call)(s->bus, ASUKA_PORTAL, s->request, "org.freedesktop.portal.Request", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
		                          NULL, NULL, NULL);
	if (!s->working) {
		s->ended = true;
		shot_unsubscribe(s);
	}
	shot_free(s);
}

static char *shot_path(const char *dir) {
	static atomic_uint count;
	char *sub = asuka_sprintf("%s/asuka-screen", dir);
	mkdir(sub, 0755);  // there already, or made
	char *path = asuka_sprintf("%s/capture-%llx-%lx-%x.png", sub, (unsigned long long)G(g_get_real_time)(), (unsigned long)getpid(),
	                           atomic_fetch_add(&count, 1) + 1);
	free(sub);
	return path;
}

// move_file moves src to dst: renamed, or copied (another file system:
// the document portal's) and src removed. false with errno.
static bool move_file(const char *src, const char *dst) {
	if (rename(src, dst) == 0) return true;
	if (errno != EXDEV) return false;
	FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
	bool ok = in && out;
	char buf[1 << 16];
	size_t n;
	while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
	if (in && ferror(in)) ok = false;
	if (out && fclose(out) != 0) ok = false;
	if (in) fclose(in);
	if (ok) unlink(src);
	else unlink(dst);
	return ok;
}

// cut writes the part of the desktop's picture (src, all's bounds at its
// own scale) that's want's, as a PNG at dst: false if the picture isn't
// the desktop's (another size's), or can't be read or written, with err
// set then for the last two.
static bool cut(const char *src, const char *dst, screen_Rect all, screen_Rect want, bool *whole, asuka_error *err) {
	*whole = false;
	if (!G(gdk_texture_new_from_filename) || !G(gdk_texture_download) || !G(gdk_memory_texture_new) || !G(gdk_texture_save_to_png)) {
		*whole = true;
		return false;
	}
	GError *e = NULL;
	GdkTexture *t = G(gdk_texture_new_from_filename)(src, &e);
	if (!t) {
		asuka_linux_fail(err, e, "screen: can't read the portal's screenshot");
		return false;
	}
	int W = G(gdk_texture_get_width)(t), H = G(gdk_texture_get_height)(t);
	double kx = W / all.width, ky = H / all.height;
	// The compositor's own picture of one display, or another layout: as it is.
	if (all.width <= 0 || all.height <= 0 || fabs(kx - ky) > 0.02 * kx) {
		G(g_object_unref)(t);
		*whole = true;
		return false;
	}
	int x0 = (int)lround((want.x - all.x) * kx), y0 = (int)lround((want.y - all.y) * ky);
	int x1 = (int)lround((want.x + want.width - all.x) * kx), y1 = (int)lround((want.y + want.height - all.y) * ky);
	x0 = x0 < 0 ? 0 : x0 > W ? W : x0;
	x1 = x1 < x0 ? x0 : x1 > W ? W : x1;
	y0 = y0 < 0 ? 0 : y0 > H ? H : y0;
	y1 = y1 < y0 ? y0 : y1 > H ? H : y1;
	int w = x1 - x0, h = y1 - y0;
	if (w == W && h == H) {
		G(g_object_unref)(t);
		*whole = true;
		return false;
	}
	if (w <= 0 || h <= 0) {
		G(g_object_unref)(t);
		asuka_error_declared(err, screen_CaptureError_notAvailable);
		return false;
	}
	size_t stride = (size_t)W * 4;
	guchar *all_px = malloc(stride * H);
	G(gdk_texture_download)(t, all_px, stride);  // cairo's ARGB32: premultiplied, native-endian
	G(g_object_unref)(t);
	guchar *px = malloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++) memcpy(px + (size_t)y * w * 4, all_px + (size_t)(y0 + y) * stride + (size_t)x0 * 4, (size_t)w * 4);
	free(all_px);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	GdkMemoryFormat f = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
	GdkMemoryFormat f = GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif
	GBytes *b = G(g_bytes_new_take)(px, (gsize)w * h * 4);
	GdkTexture *part = G(gdk_memory_texture_new)(w, h, f, b, (gsize)w * 4);
	G(g_bytes_unref)(b);
	bool ok = part && G(gdk_texture_save_to_png)(part, dst);
	if (part) G(g_object_unref)(part);
	if (!ok) {
		unlink(dst);
		asuka_error_failed(err, "screen", 0, "screen: can't write %s", dst);
	}
	return ok;
}

// place makes the portal's file the capture: cut to the display, or
// moved as it is, into the caches (a thread of its own).
static void place(void *p) {
	shot *s = p;
	char *dst = shot_path(s->dir);
	bool whole = !s->crop;
	if (s->crop && cut(s->src, dst, s->all, s->want, &whole, &s->err)) {
		unlink(s->src);  // the portal wrote it for this capture only
		s->out = dst;
	} else if (whole) {
		asuka_error_free(&s->err);
		if (move_file(s->src, dst)) s->out = dst;
		else {
			asuka_error_failed(&s->err, "posix", errno, "screen: can't move the screenshot to %s: %s", dst, strerror(errno));
			free(dst);
		}
	} else {
		unlink(s->src);
		free(dst);
	}
}

static void placed(void *p) {
	shot *s = p;
	s->working = false;
	char *out = s->out;
	s->out = NULL;
	asuka_error e = s->err;
	memset(&s->err, 0, sizeof s->err);
	if (out) shot_end(s, out, NULL);
	else shot_end(s, NULL, &e);
	shot_free(s);
}

static void place_off_main(void *p) {
	place(p);
	asuka_desktop_post(placed, p);
}

// permission_says_no reads the store's screenshot permission (Lookup's
// (a{sas}v)) for the app's ID, else for apps without one: "no" (the user
// said no) and "ask" (asked this time, and the portal failed: said no)
// are refusals.
static bool permission_says_no(GVariant *r) {
	if (!r || G(g_variant_n_children)(r) < 1) return false;
	GVariant *perms = G(g_variant_get_child_value)(r, 0);
	const char *ids[] = {asuka_desktop_app_id(), ""};
	bool no = false, found = false;
	for (size_t i = 0; i < 2 && !found; i++) {
		if (!ids[i]) continue;
		GVariant *v = G(g_variant_lookup_value)(perms, ids[i], G_VARIANT_TYPE("as"));
		if (!v) continue;
		found = true;
		if (G(g_variant_n_children)(v) > 0) {
			GVariant *c = G(g_variant_get_child_value)(v, 0);
			const char *say = G(g_variant_get_string)(c, NULL);
			no = say && (strcmp(say, "no") == 0 || strcmp(say, "ask") == 0);
			G(g_variant_unref)(c);
		}
		G(g_variant_unref)(v);
	}
	G(g_variant_unref)(perms);
	return no;
}

static void looked_up(GObject *src, GAsyncResult *res, gpointer p) {
	shot *s = p;
	GError *e = NULL;
	GVariant *r = G(g_dbus_connection_call_finish)((GDBusConnection *)src, res, &e);
	bool no = permission_says_no(r);
	if (r) G(g_variant_unref)(r);
	if (e) G(g_error_free)(e);
	s->working = false;
	shot_fail(s, no ? screen_CaptureError_notAllowed : screen_CaptureError_notAvailable);
	shot_free(s);
}

static void on_shot(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal, GVariant *params, gpointer p) {
	shot *s = p;
	if (s->ended || s->working) return;
	shot_unsubscribe(s);
	guint32 code = 2;
	GVariant *results = NULL;
	if (G(g_variant_is_of_type)(params, G_VARIANT_TYPE("(ua{sv})"))) G(g_variant_get)(params, "(u@a{sv})", &code, &results);
	char *file = NULL;
	if (code == 0 && results) {
		GVariant *u = G(g_variant_lookup_value)(results, "uri", G_VARIANT_TYPE("s"));
		if (u) {
			gchar *f = G(g_filename_from_uri)(G(g_variant_get_string)(u, NULL), NULL, NULL);
			if (f) file = asuka_strdup(f);
			G(g_free)(f);
			G(g_variant_unref)(u);
		}
	}
	if (results) G(g_variant_unref)(results);
	if (code == 1) {
		shot_fail(s, screen_CaptureError_notAllowed);
	} else if (code == 0 && file) {
		s->src = file;
		file = NULL;
		s->working = true;
		if (!asuka_desktop_spawn(place_off_main, s)) {
			place(s);
			placed(s);
			return;
		}
	} else if (code == 0) {
		asuka_error e = {0};
		asuka_error_failed(&e, "screen", 0, "screen: the screenshot portal gave no file");
		shot_end(s, NULL, &e);
	} else {
		// The portal says no the same way it fails: the permission store
		// tells them apart, where the app may read it.
		s->working = true;
		G(g_dbus_connection_call)(s->bus, PERMISSIONS, PERMISSIONS_PATH, PERMISSIONS, "Lookup", G(g_variant_new)("(ss)", "screenshot", "screenshot"),
		                          G_VARIANT_TYPE("(a{sas}v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, looked_up, s);
	}
	free(file);
	shot_free(s);
}

static void listen_shot(shot *s, const char *path) {
	shot_unsubscribe(s);
	free(s->request);
	s->request = asuka_strdup(path);
	s->sub = G(g_dbus_connection_signal_subscribe)(s->bus, ASUKA_PORTAL, "org.freedesktop.portal.Request", "Response", s->request, NULL,
	                                               G_DBUS_SIGNAL_FLAGS_NONE, on_shot, s, NULL);
}

// no_portal reports whether e says there's no portal, or no screenshot
// portal.
static bool no_portal(const GError *e) {
	static const char *says[] = {"ServiceUnknown", "UnknownMethod", "UnknownInterface", "UnknownObject", "No such interface", "NameHasNoOwner",
	                             "not provided by any .service"};
	if (!e || !e->message) return false;
	for (size_t i = 0; i < sizeof says / sizeof *says; i++)
		if (strstr(e->message, says[i])) return true;
	return false;
}

static void shot_replied(GObject *src, GAsyncResult *res, gpointer p) {
	shot *s = p;
	s->replied = true;
	GError *e = NULL;
	GVariant *v = G(g_dbus_connection_call_finish)((GDBusConnection *)src, res, &e);
	if (!v) {
		if (no_portal(e)) {
			G(g_error_free)(e);
			shot_fail(s, screen_CaptureError_notAvailable);
		} else {
			asuka_error err = {0};
			asuka_linux_fail(&err, e, "screen: the screenshot portal");
			shot_end(s, NULL, &err);
		}
		shot_free(s);
		return;
	}
	// An older portal's request path isn't the token's.
	const gchar *path = NULL;
	if (!s->ended && !s->working && G(g_variant_is_of_type)(v, G_VARIANT_TYPE("(o)"))) {
		G(g_variant_get)(v, "(&o)", &path);
		if (path && strcmp(path, s->request)) listen_shot(s, path);
	}
	G(g_variant_unref)(v);
	shot_free(s);
}

// shoot captures the display (its connector; "": the primary) as a PNG
// in dir: done gets its path or why not (a display that isn't there, no
// portal: CaptureError.notAvailable; the user said no: notAllowed),
// maybe before this returns. The shot is the caller's to leave until
// done is called.
static shot *shoot(const char *display, const char *dir, shot_done done, void *ctx) {
	static unsigned tokens;
	shot *s = calloc(1, sizeof *s);
	s->done = done;
	s->ctx = ctx;
	s->dir = asuka_strdup(dir);
	s->replied = true;  // until the portal's called
	screen_list_Display ds = read_displays();
	const screen_Display *d = NULL;
	for (size_t i = 0; i < ds.len && !d; i++)
		if (display && *display ? strcmp(ds.data[i].id, display) == 0 : ds.data[i].primary) d = &ds.data[i];
	if (d) {
		s->want = d->bounds;
		s->crop = ds.len > 1;
		double x0 = d->bounds.x, y0 = d->bounds.y, x1 = x0 + d->bounds.width, y1 = y0 + d->bounds.height;
		for (size_t i = 0; i < ds.len; i++) {
			screen_Rect b = ds.data[i].bounds;
			x0 = fmin(x0, b.x), y0 = fmin(y0, b.y), x1 = fmax(x1, b.x + b.width), y1 = fmax(y1, b.y + b.height);
		}
		s->all = (screen_Rect){x0, y0, x1 - x0, y1 - y0};
	}
	screen_list_Display_free(&ds);
	if (!d || !(s->bus = asuka_linux_bus(G_BUS_TYPE_SESSION, NULL))) {
		shot_fail(s, screen_CaptureError_notAvailable);
		shot_free(s);
		return NULL;
	}
	char token[32];
	snprintf(token, sizeof token, "asuka_screen%u", ++tokens);
	char *sender = asuka_strdup(G(g_dbus_connection_get_unique_name)(s->bus) + 1);
	for (char *c = sender; *c; c++)
		if (*c == '.') *c = '_';
	char *path = asuka_sprintf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
	free(sender);
	listen_shot(s, path);
	free(path);
	GVariantBuilder o;
	G(g_variant_builder_init)(&o, G_VARIANT_TYPE("a{sv}"));
	G(g_variant_builder_add)(&o, "{sv}", "handle_token", G(g_variant_new_string)(token));
	G(g_variant_builder_add)(&o, "{sv}", "interactive", G(g_variant_new_boolean)(false));
	s->replied = false;
	G(g_dbus_connection_call)(s->bus, ASUKA_PORTAL, ASUKA_PORTAL_PATH, SCREENSHOT, "Screenshot", G(g_variant_new)("(s@a{sv})", "", G(g_variant_builder_end)(&o)),
	                          G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, shot_replied, s);
	return s;
}

static void shot_answer(void *ctx, char *path, asuka_error *err) {
	asuka_call *call = ctx;
	asuka_call_off_cancel(call);
	if (path) screen_Screens_capture_done(call, path);  // takes path
	else asuka_call_fail(call, err);
}

static void shot_cancel(void *p) {
	shot *s = p;
	asuka_call *call = s->ctx;
	shot_leave(s);
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(call, &e);
}

static void s_shoot(screen_Screens *self, const char *display, asuka_call *call) {
	shot *s = shoot(display, asuka_desktop_cache_dir(), shot_answer, call);
	if (s && !s->ended) asuka_call_on_cancel(call, shot_cancel, s);
}

static const screen_Screens_vtable vt = {.displays = s_displays, .watchDisplays = s_watch, .lock = s_lock, .setBars = s_bars, .preventCapture = s_capture,
	.setBrightness = s_set_brightness, .brightness = s_brightness, .watchScreenshots = s_shots, .capture = s_shoot};

screen_Screens *screen_make_Screens(asuka_error *err) {
	screen_Screens *s = malloc(sizeof *s);
	s->vt = &vt;
	return s;
}
