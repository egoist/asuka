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
// no status or navigation bar: lock and setBars do nothing. Main thread.
#include "asuka_linux.h"
#include "screen.asuka.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

static const screen_Screens_vtable vt = {.displays = s_displays, .watchDisplays = s_watch, .lock = s_lock, .setBars = s_bars, .preventCapture = s_capture,
	.setBrightness = s_set_brightness, .brightness = s_brightness, .watchScreenshots = s_shots};

screen_Screens *screen_make_Screens(asuka_error *err) {
	screen_Screens *s = malloc(sizeof *s);
	s->vt = &vt;
	return s;
}
