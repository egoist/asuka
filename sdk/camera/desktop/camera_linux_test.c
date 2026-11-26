// camera_linux_test.c: the camera's permission on Linux (platform/desktop's
// TestStdUnits, with a session bus: xvfb-run dbus-run-session):
// camera_linux.c, included, reads the permission store (its "devices"
// table's "camera") as Camera.access does, and asks the camera portal
// (AccessCamera) as requestAccess does. The store and the portal are the
// test's own, served by a child process. access answers from the store's
// last answer, asking it again for the next read: the test waits for a
// fresh one (fresh) before it reads.
#include "camera_linux.c"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

#define CHECK_IS(got, want)                                                              \
	do {                                                                                 \
		const char *g_ = (got);                                                          \
		if (!g_ || strcmp(g_, want)) {                                                   \
			printf("%s:%d: %s is %s, not %s\n", __FILE__, __LINE__, #got, g_ ? g_ : "NULL", want); \
			failed = 1;                                                                  \
		}                                                                                \
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

// ---- The test's store and portal (the child's) ----

#define TEST "dev.asuka.CameraTest"
#define TEST_PATH "/dev/asuka/CameraTest"

// What the store says (its entries for the camera):
enum {
	NO_TABLE,     // no table yet: NotFound
	APP_YES,      // the app's "yes"
	APP_NO,       // the app's "no"
	OTHER_YES,    // another app's only
	UNNAMED_NO,   // "" (apps outside a sandbox, to older portals): "no"
	FLATPAK_YES,  // a Flatpak's ("dev.asuka.flat"): "yes"
};

static struct {
	int store;
	bool present;      // IsCameraPresent
	guint32 response;  // AccessCamera's Response
	int calls;         // AccessCamera's
	GDBusConnection *conn;
} fake;

static const char *xml =
	"<node>"
	"<interface name='org.freedesktop.impl.portal.PermissionStore'><method name='Lookup'><arg type='s' direction='in'/>"
	"<arg type='s' direction='in'/><arg type='a{sas}' direction='out'/><arg type='v' direction='out'/></method></interface>"
	"<interface name='org.freedesktop.portal.Camera'><method name='AccessCamera'><arg type='a{sv}' direction='in'/>"
	"<arg type='o' direction='out'/></method><property name='IsCameraPresent' type='b' access='read'/></interface>"
	"<interface name='" TEST "'><method name='Set'><arg type='i' direction='in'/><arg type='b' direction='in'/>"
	"<arg type='u' direction='in'/></method><method name='Calls'><arg type='i' direction='out'/></method></interface>"
	"</node>";

static void lookup(GVariant *params, GDBusMethodInvocation *inv) {
	const char *table = NULL, *id = NULL;
	G(g_variant_get)(params, "(&s&s)", &table, &id);
	if (strcmp(table, "devices") || strcmp(id, "camera")) {
		G(g_dbus_method_invocation_return_dbus_error)(inv, "org.freedesktop.DBus.Error.InvalidArgs", "not the camera's");
		return;
	}
	if (fake.store == NO_TABLE) {
		G(g_dbus_method_invocation_return_dbus_error)(inv, "org.freedesktop.portal.Error.NotFound", "No entry for camera");
		return;
	}
	GVariantBuilder b;
	G(g_variant_builder_init)(&b, G_VARIANT_TYPE("a{sas}"));
	const char *yes[] = {"yes", NULL}, *no[] = {"no", NULL};
	switch (fake.store) {
	case APP_YES:
		G(g_variant_builder_add)(&b, "{s^as}", asuka_desktop_app_id(), yes);
		break;
	case APP_NO:
		G(g_variant_builder_add)(&b, "{s^as}", asuka_desktop_app_id(), no);
		break;
	case OTHER_YES:
		G(g_variant_builder_add)(&b, "{s^as}", "org.example.Other", yes);
		break;
	case UNNAMED_NO:
		G(g_variant_builder_add)(&b, "{s^as}", "", no);
		break;
	case FLATPAK_YES:
		G(g_variant_builder_add)(&b, "{s^as}", "dev.asuka.flat", yes);
		break;
	}
	G(g_dbus_method_invocation_return_value)(inv, G(g_variant_new)("(@a{sas}v)", G(g_variant_builder_end)(&b), G(g_variant_new_boolean)(false)));
}

// access_camera answers with a Request's path, then its Response.
static void access_camera(const char *sender, GVariant *params, GDBusMethodInvocation *inv) {
	fake.calls++;
	GVariant *opts = G(g_variant_get_child_value)(params, 0);
	GVariant *t = G(g_variant_lookup_value)(opts, "handle_token", G_VARIANT_TYPE("s"));
	char *who = asuka_strdup(sender + 1);
	for (char *s = who; *s; s++)
		if (*s == '.') *s = '_';
	char *req = asuka_sprintf("/org/freedesktop/portal/desktop/request/%s/%s", who, t ? G(g_variant_get_string)(t, NULL) : "none");
	free(who);
	if (t) G(g_variant_unref)(t);
	G(g_variant_unref)(opts);
	G(g_dbus_method_invocation_return_value)(inv, G(g_variant_new)("(o)", req));
	GVariantBuilder r;
	G(g_variant_builder_init)(&r, G_VARIANT_TYPE("a{sv}"));
	G(g_dbus_connection_emit_signal)(fake.conn, NULL, req, "org.freedesktop.portal.Request", "Response",
	                                 G(g_variant_new)("(u@a{sv})", fake.response, G(g_variant_builder_end)(&r)), NULL);
	free(req);
}

static void fake_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *method, GVariant *params,
                      GDBusMethodInvocation *inv, gpointer d) {
	if (!strcmp(method, "Lookup")) {
		lookup(params, inv);
	} else if (!strcmp(method, "AccessCamera")) {
		access_camera(sender, params, inv);
	} else if (!strcmp(method, "Set")) {
		gint32 store = 0;
		gboolean present = false;
		G(g_variant_get)(params, "(ibu)", &store, &present, &fake.response);
		fake.store = store;
		fake.present = present;
		G(g_dbus_method_invocation_return_value)(inv, NULL);
	} else {
		G(g_dbus_method_invocation_return_value)(inv, G(g_variant_new)("(i)", (gint32)fake.calls));
	}
}

static GVariant *fake_property(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *name, GError **e,
                               gpointer d) {
	return G(g_variant_new_boolean)(fake.present);
}

// serve stands the store and the portal in, on the session bus, and
// writes a byte to ready once their names are its own. It doesn't return.
static void serve(int ready) {
	fake.conn = asuka_linux_bus(G_BUS_TYPE_SESSION, NULL);
	if (!fake.conn) _exit(1);
	static const GDBusInterfaceVTable vt = {fake_call, fake_property, NULL, {0}};
	GDBusNodeInfo *info = G(g_dbus_node_info_new_for_xml)(xml, NULL);
	if (!info) _exit(1);
	const char *paths[] = {PERMISSIONS_PATH, ASUKA_PORTAL_PATH, TEST_PATH};
	const char *ifaces[] = {PERMISSIONS, "org.freedesktop.portal.Camera", TEST};
	for (int i = 0; i < 3; i++)
		if (!G(g_dbus_connection_register_object)(fake.conn, paths[i], G(g_dbus_node_info_lookup_interface)(info, ifaces[i]), &vt, NULL, NULL, NULL))
			_exit(1);
	// The test's name last: once it's there, the others are.
	const char *names[] = {PERMISSIONS, ASUKA_PORTAL, TEST};
	for (int i = 0; i < 3; i++) {
		GVariant *v = G(g_dbus_connection_call_sync)(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
		                                             G(g_variant_new)("(su)", names[i], 4u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
		guint32 r = 0;
		if (v) G(g_variant_get)(v, "(u)", &r), G(g_variant_unref)(v);
		if (r != 1) _exit(2);  // not the primary owner: a real one's there
	}
	char b = 1;
	if (write(ready, &b, 1) != 1) _exit(1);
	G(g_main_loop_run)(G(g_main_loop_new)(NULL, false));
	_exit(0);
}

// ---- The test ----

static void set(int store, bool present, guint32 response) {
	GVariant *v = G(g_dbus_connection_call_sync)(asuka_linux_bus(G_BUS_TYPE_SESSION, NULL), TEST, TEST_PATH, TEST, "Set",
	                                             G(g_variant_new)("(ibu)", (gint32)store, (gboolean)present, response), NULL, G_DBUS_CALL_FLAGS_NONE,
	                                             5000, NULL, NULL);
	CHECK(v != NULL);
	if (v) G(g_variant_unref)(v);
}

static int calls(void) {
	GVariant *v = G(g_dbus_connection_call_sync)(asuka_linux_bus(G_BUS_TYPE_SESSION, NULL), TEST, TEST_PATH, TEST, "Calls", NULL,
	                                             G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
	gint32 n = -1;
	if (v) G(g_variant_get)(v, "(i)", &n), G(g_variant_unref)(v);
	return n;
}

static struct {
	bool done;
	const char *answer;
} got;

static void answered(void *ctx, const char *answer) {
	got.done = true;
	got.answer = answer;
}

static const char *ask(void) {
	got.done = false;
	got.answer = NULL;
	ask_access(answered, NULL);
	WAIT(got.done);
	return got.answer;
}

// fresh is access's answer once the store has been asked after the
// test's last change (a lookup in flight may predate it: that one first).
static const char *fresh(void) {
	WAIT(!looking);
	unsigned n = lookups;
	look_up();
	WAIT(lookups > n);
	return access_now();
}

// access is the vtable's answer (the glue's: malloc's), kept to compare.
static const char *access_via_vtable(void) {
	static char kept[16];
	camera_Camera *c = camera_make_Camera(NULL);
	asuka_error err = {0};
	char *s = c->vt->access(c, &err);
	snprintf(kept, sizeof kept, "%s", s ? s : "NULL");
	free(s);
	return kept;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
		printf("no session bus: skipped (run under dbus-run-session)\n");
		return 0;
	}
	if (!asuka_linux()) {
		printf("no GLib: skipped\n");
		return 0;
	}
	unsetenv("FLATPAK_ID");
	unsetenv("ASUKA_CAMERA_SOURCE");
	if (access("/.flatpak-info", F_OK) == 0) {
		printf("in a Flatpak: skipped\n");
		return 0;
	}
	// The child serves before this process touches the bus (GDBus's
	// threads aren't forked).
	int p[2];
	if (pipe(p)) return 1;
	pid_t child = fork();
	if (child < 0) return 1;
	if (child == 0) {
		close(p[0]);
		serve(p[1]);
	}
	close(p[1]);
	char b = 0;
	bool ready = read(p[0], &b, 1) == 1;
	close(p[0]);
	if (!ready) {
		int st = 0;
		waitpid(child, &st, 0);
		printf("can't stand a store and a portal in (status %d: one's on the bus already?)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 1;
	}

	// access: what the store says for the app.
	set(NO_TABLE, false, 0);
	CHECK_IS(fresh(), "notAsked");  // no app asked yet
	set(APP_YES, false, 0);
	CHECK_IS(fresh(), "allowed");
	CHECK_IS(access_via_vtable(), "allowed");
	set(APP_NO, false, 0);
	CHECK_IS(fresh(), "denied");
	CHECK_IS(access_via_vtable(), "denied");
	set(OTHER_YES, false, 0);
	CHECK_IS(fresh(), "notAsked");
	set(UNNAMED_NO, false, 0);
	CHECK_IS(fresh(), "denied");  // outside a sandbox, "" is the app's

	// In a Flatpak: its ID's entry, not "".
	setenv("FLATPAK_ID", "dev.asuka.flat", 1);
	CHECK_IS(fresh(), "notAsked");
	set(FLATPAK_YES, false, 0);
	CHECK_IS(fresh(), "allowed");
	unsetenv("FLATPAK_ID");
	CHECK_IS(fresh(), "notAsked");  // that entry's not this app's

	// requestAccess: no camera portal, nothing asks.
	set(NO_TABLE, false, 0);
	CHECK_IS(ask(), "allowed");
	CHECK(calls() == 0);
	CHECK_IS(fresh(), "allowed");  // what this process learned
	told = NULL;

	// The portal asks: yes, then no (or the question closed).
	set(NO_TABLE, true, 0);
	CHECK_IS(ask(), "allowed");
	CHECK(calls() == 1);
	CHECK_IS(fresh(), "allowed");
	set(NO_TABLE, true, 1);
	CHECK_IS(ask(), "denied");
	CHECK(calls() == 2);
	CHECK_IS(fresh(), "denied");
	// The store's answer over this process's: the user changed it since.
	set(APP_YES, true, 1);
	CHECK_IS(fresh(), "allowed");
	told = NULL;

	// A test's camera: nothing asked.
	setenv("ASUKA_CAMERA_SOURCE", "videotestsrc is-live=true", 1);
	set(APP_NO, true, 1);
	CHECK_IS(fresh(), "allowed");
	CHECK_IS(ask(), "allowed");
	CHECK(calls() == 2);
	unsetenv("ASUKA_CAMERA_SOURCE");

	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	printf(failed ? "FAIL\n" : "ok\n");
	return failed;
}
