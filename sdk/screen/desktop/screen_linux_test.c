// screen_linux_test.c: Screens.capture on Linux (platform/desktop's
// TestStdUnits, with GTK under Xvfb and a session bus: xvfb-run
// dbus-run-session): screen_linux.c, included, captures through the
// screenshot portal as the vtable's capture does: a portal of the test's
// own (on a connection of its own) without screenshots, which is
// CaptureError.notAvailable; then with them, answering with a file (moved
// into the caches), the user's no (1: notAllowed), and a failure (2),
// which the permission store tells apart (its "no": notAllowed; none:
// notAvailable). The desktop's picture of more than one display is cut
// to the one asked for.
#include "screen_linux.c"

#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

static bool timed_out;

static gboolean time_out(gpointer p) {
	timed_out = true;
	return 0;
}

// WAIT runs GLib's loop until cond holds (failing after 30 s of nothing:
// a guard, not what's tested).
#define WAIT(cond)                                                                         \
	do {                                                                                   \
		timed_out = false;                                                                 \
		guint t_ = G(g_timeout_add_full)(G_PRIORITY_DEFAULT, 30000, time_out, NULL, NULL); \
		while (!(cond) && !timed_out) G(g_main_context_iteration)(NULL, 1);                \
		if (!timed_out) G(g_source_remove)(t_);                                            \
		if (!(cond)) {                                                                     \
			printf("%s:%d: timed out waiting for %s\n", __FILE__, __LINE__, #cond);        \
			failed = 1;                                                                    \
		}                                                                                  \
	} while (0)

// What a capture answered.
static struct {
	bool done;
	char *path;
	asuka_error err;
} got;

static void reset(void) {
	free(got.path);
	asuka_error_free(&got.err);
	memset(&got, 0, sizeof got);
}

static gboolean once(gpointer p) { return 0; }

// answered may run on the worker that moved the file: the unit tests'
// host runs a post where it's posted (a host, on its main thread). It
// wakes the loop WAIT runs.
static void answered(void *ctx, char *path, asuka_error *err) {
	got.path = path;
	if (err) got.err = *err;
	got.done = true;
	G(g_idle_add_full)(G_PRIORITY_DEFAULT, once, NULL, NULL);
}

static bool declared(uint32_t index) { return got.done && !got.path && got.err.status == ASUKA_DECLARED && got.err.index == index; }

// solid writes a w x h PNG at path: its left half red, its right blue.
static bool solid(const char *path, int w, int h) {
	guchar *px = malloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			guchar *p = px + ((size_t)y * w + x) * 4;  // RGBA
			p[0] = x < w / 2 ? 255 : 0, p[1] = 0, p[2] = x < w / 2 ? 0 : 255, p[3] = 255;
		}
	GBytes *b = G(g_bytes_new_take)(px, (gsize)w * h * 4);
	GdkTexture *t = G(gdk_memory_texture_new)(w, h, GDK_MEMORY_R8G8B8A8, b, (gsize)w * 4);
	G(g_bytes_unref)(b);
	bool ok = G(gdk_texture_save_to_png)(t, path);
	G(g_object_unref)(t);
	return ok;
}

// look reads a PNG's size and its middle pixel (cairo's ARGB32).
static bool look(const char *path, int *w, int *h, uint32_t *middle) {
	GdkTexture *t = G(gdk_texture_new_from_filename)(path, NULL);
	if (!t) return false;
	*w = G(gdk_texture_get_width)(t), *h = G(gdk_texture_get_height)(t);
	uint32_t *px = malloc((size_t)*w * *h * 4);
	G(gdk_texture_download)(t, (guchar *)px, (size_t)*w * 4);
	*middle = px[(size_t)(*h / 2) * *w + *w / 2];
	free(px);
	G(g_object_unref)(t);
	return true;
}

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

// ---- The test's portal ----

static struct {
	GDBusConnection *conn;
	int code;           // what Response says
	int w, h;           // the picture's size, with code 0
	bool interactive;   // what the last call asked
	bool store;         // the permission store's there, saying no
	char file[PATH_MAX + 32];
	int calls;
	bool named[2];
	GDBusNodeInfo *portal;
} fake;

static const char *portal_xml =
	"<node><interface name='org.freedesktop.portal.Screenshot'><method name='Screenshot'><arg type='s' direction='in'/>"
	"<arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method></interface></node>";
static const char *store_xml =
	"<node><interface name='org.freedesktop.impl.portal.PermissionStore'><method name='Lookup'><arg type='s' direction='in'/>"
	"<arg type='s' direction='in'/><arg type='a{sas}' direction='out'/><arg type='v' direction='out'/></method></interface></node>";

static void portal_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer d) {
	if (strcmp(method, "Lookup") == 0) {
		if (!fake.store) {
			G(g_dbus_method_invocation_return_dbus_error)(inv, "org.freedesktop.portal.Error.NotFound", "no such table");
			return;
		}
		GVariantBuilder b;
		G(g_variant_builder_init)(&b, G_VARIANT_TYPE("a{sas}"));
		const char *no[] = {"no", NULL};
		G(g_variant_builder_add)(&b, "{s^as}", asuka_desktop_app_id(), no);
		G(g_dbus_method_invocation_return_value)(inv, G(g_variant_new)("(@a{sas}v)", G(g_variant_builder_end)(&b), G(g_variant_new_boolean)(false)));
		return;
	}
	fake.calls++;
	GVariant *opts = G(g_variant_get_child_value)(params, 1);
	const char *token = "none";
	GVariant *t = G(g_variant_lookup_value)(opts, "handle_token", G_VARIANT_TYPE("s"));
	if (t) token = G(g_variant_get_string)(t, NULL);
	gboolean interactive = true;
	G(g_variant_lookup)(opts, "interactive", "b", &interactive);
	fake.interactive = interactive;
	char *who = asuka_strdup(sender + 1);
	for (char *s = who; *s; s++)
		if (*s == '.') *s = '_';
	char *req = asuka_sprintf("/org/freedesktop/portal/desktop/request/%s/%s", who, token);
	free(who);
	if (t) G(g_variant_unref)(t);
	G(g_variant_unref)(opts);
	G(g_dbus_method_invocation_return_value)(inv, G(g_variant_new)("(o)", req));
	GVariantBuilder r;
	G(g_variant_builder_init)(&r, G_VARIANT_TYPE("a{sv}"));
	if (fake.code == 0) {
		CHECK(solid(fake.file, fake.w, fake.h));
		char *uri = asuka_sprintf("file://%s", fake.file);
		G(g_variant_builder_add)(&r, "{sv}", "uri", G(g_variant_new_string)(uri));
		free(uri);
	}
	G(g_dbus_connection_emit_signal)(fake.conn, NULL, req, "org.freedesktop.portal.Request", "Response",
	                                 G(g_variant_new)("(u@a{sv})", (guint32)fake.code, G(g_variant_builder_end)(&r)), NULL);
	free(req);
}

static void named(GObject *src, GAsyncResult *res, gpointer p) {
	GVariant *v = G(g_dbus_connection_call_finish)((GDBusConnection *)src, res, NULL);
	guint32 r = 0;
	if (v) {
		G(g_variant_get)(v, "(u)", &r);
		G(g_variant_unref)(v);
	}
	CHECK(r == 1);  // the primary owner
	*(bool *)p = true;
}

static bool start_portal(void) {
	gchar *addr = G(g_dbus_address_get_for_bus_sync)(G_BUS_TYPE_SESSION, NULL, NULL);
	if (!addr) return false;
	fake.conn = G(g_dbus_connection_new_for_address_sync)(
		addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
	G(g_free)(addr);
	if (!fake.conn) return false;
	static const GDBusInterfaceVTable vt = {portal_call, NULL, NULL, {0}};
	GDBusNodeInfo *p = G(g_dbus_node_info_new_for_xml)(portal_xml, NULL), *s = G(g_dbus_node_info_new_for_xml)(store_xml, NULL);
	fake.portal = p;
	CHECK(G(g_dbus_connection_register_object)(fake.conn, PERMISSIONS_PATH, G(g_dbus_node_info_lookup_interface)(s, PERMISSIONS), &vt, NULL, NULL, NULL));
	const char *names[] = {ASUKA_PORTAL, PERMISSIONS};
	for (int i = 0; i < 2; i++)
		G(g_dbus_connection_call)(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
		                          G(g_variant_new)("(su)", names[i], 4u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, named, &fake.named[i]);
	WAIT(fake.named[0] && fake.named[1]);
	return fake.named[0] && fake.named[1];
}

// add_screenshots has the portal take screenshots.
static void add_screenshots(void) {
	static const GDBusInterfaceVTable vt = {portal_call, NULL, NULL, {0}};
	CHECK(G(g_dbus_connection_register_object)(fake.conn, ASUKA_PORTAL_PATH, G(g_dbus_node_info_lookup_interface)(fake.portal, SCREENSHOT), &vt, NULL,
	                                           NULL, NULL));
}

int main(void) {
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("no display: skipped (run under xvfb-run)\n");
		return 0;
	}
	if (!asuka_linux() || !G(gtk_init)) {
		printf("no GTK: skipped\n");
		return 0;
	}
	G(gtk_init)();
	char dir[PATH_MAX];
	if (!realpath(asuka_desktop_cache_dir(), dir)) return 1;
	screen_list_Display ds = read_displays();
	CHECK(ds.len > 0);
	if (ds.len == 0) return 1;
	screen_Rect primary = ds.data[0].bounds;
	double scale = ds.data[0].scale > 0 ? ds.data[0].scale : 1;
	screen_list_Display_free(&ds);

	// A display that isn't there, at once.
	reset();
	CHECK(shoot("NOPE-9", dir, answered, NULL) == NULL);
	CHECK(declared(screen_CaptureError_notAvailable));

	// The desktop's picture cut to one display: two side by side, 150 x 50
	// points each, at twice their points; the right one's blue.
	char src[PATH_MAX + 16], dst[PATH_MAX + 16];
	snprintf(src, sizeof src, "%s/desktop.png", dir);
	snprintf(dst, sizeof dst, "%s/right.png", dir);
	CHECK(solid(src, 600, 100));
	bool whole = false;
	asuka_error err = {0};
	CHECK(cut(src, dst, (screen_Rect){0, 0, 300, 50}, (screen_Rect){150, 0, 150, 50}, &whole, &err) && !whole);
	int w = 0, h = 0;
	uint32_t mid = 0;
	CHECK(look(dst, &w, &h, &mid) && w == 300 && h == 100 && mid == 0xff0000ff);
	// A picture that isn't the desktop's (one display's own): as it is.
	CHECK(!cut(src, dst, (screen_Rect){0, 0, 300, 300}, (screen_Rect){150, 0, 150, 300}, &whole, &err) && whole && err.status == ASUKA_OK);
	unlink(src);
	unlink(dst);

	if (!asuka_linux_bus(G_BUS_TYPE_SESSION, NULL)) {
		printf("no session bus: the portal's captures not checked (run under dbus-run-session)\n");
		return failed;
	}
	// The test's portal, first: the bus may start the system's (its
	// .service there) for the first call to it.
	if (!start_portal()) {
		printf("can't stand a portal in (one's on the bus already?)\n");
		return 1;
	}

	// A portal that takes no screenshots (GNOME's gone, an old portal).
	reset();
	shot *s = shoot("", dir, answered, NULL);
	CHECK(s != NULL);
	WAIT(got.done);
	CHECK(declared(screen_CaptureError_notAvailable));
	if (!declared(screen_CaptureError_notAvailable)) printf("  status %d: %s\n", got.err.status, got.err.message ? got.err.message : "");
	add_screenshots();
	snprintf(fake.file, sizeof fake.file, "%s/Screenshot.png", dir);

	// A file: moved into the caches, the display's (one: not cut).
	reset();
	fake.code = 0, fake.w = (int)lround(primary.width * scale), fake.h = (int)lround(primary.height * scale);
	shoot("", dir, answered, NULL);
	WAIT(got.done);
	CHECK(got.path && strstr(got.path, "/asuka-screen/capture-") && strncmp(got.path, dir, strlen(dir)) == 0);
	CHECK(fake.calls == 1 && !fake.interactive);
	if (got.path) {
		CHECK(look(got.path, &w, &h, &mid) && w == fake.w && h == fake.h);
		CHECK(!exists(fake.file));  // moved
		unlink(got.path);
	} else {
		printf("  %s\n", got.err.message ? got.err.message : "");
	}

	// The user said no.
	reset();
	fake.code = 1;
	shoot("", dir, answered, NULL);
	WAIT(got.done);
	CHECK(declared(screen_CaptureError_notAllowed));

	// The portal failed: the store says why (no permission in it: not
	// available).
	reset();
	fake.code = 2;
	shoot("", dir, answered, NULL);
	WAIT(got.done);
	CHECK(declared(screen_CaptureError_notAvailable));
	reset();
	fake.store = true;
	shoot("", dir, answered, NULL);
	WAIT(got.done);
	CHECK(declared(screen_CaptureError_notAllowed));

	// The caller leaving before the answer: no answer, nothing left.
	reset();
	fake.code = 0;
	s = shoot("", dir, answered, NULL);
	shot_leave(s);
	for (int i = 0; i < 1000 && G(g_main_context_iteration)(NULL, 0); i++) {
	}
	WAIT(fake.calls == 5);
	for (int i = 0; i < 1000 && G(g_main_context_iteration)(NULL, 0); i++) {
	}
	CHECK(!got.done);
	unlink(fake.file);
	reset();

	printf(failed ? "FAIL\n" : "ok\n");
	return failed;
}
