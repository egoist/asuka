// main.c: the app's executable starts here. It reads the app's settings
// (asuka-app.json beside the executable), opens GTK, libadwaita and the
// GLib stack, makes the AdwApplication (its ID asuka.toml's; unique, so
// a second launch activates the first, but in tests and automation runs)
// and runs GLib's main loop on the main thread. The core starts with the
// application: the app compiled into this executable (asuka_run), or,
// under asuka dev (ASUKA_DEV=host:port), the bytecode the CLI sends
// (asuka_dev_connect): this executable without an app is the player.
#include "host.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

asuka_host_callbacks lx_callbacks(void);
int lx_notify_headless(const char *id);
void lx_update_start(void);
void lx_install_notify_actions(void);
void lx_notify_action_at_start(void);
void lx_notify_restore(void);
void lx_watch_desktop(void);

static const char *login_arg = "--asuka-opened-at-login";

// app_name is asuka.toml's name, or the executable's.
static char *app_name(const char *argv0) {
	if (!str_empty(H.cfg.name)) return strdup(H.cfg.name);
	const char *b = strrchr(argv0, '/');
	return strdup(b ? b + 1 : argv0);
}

// app_id is asuka.toml's ID, else one made of the app's name
// (GApplication wants one with a dot).
static char *app_id(const char *name) {
	if (!str_empty(H.cfg.id) && g_application_id_is_valid(H.cfg.id)) return strdup(H.cfg.id);
	char clean[256];
	size_t n = 0;
	for (const char *p = name; *p && n < sizeof clean - 1; p++) {
		char c = *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			clean[n++] = c;
		else if (c == '-' || c == '_')
			clean[n++] = '_';
	}
	clean[n] = 0;
	char id[300];
	snprintf(id, sizeof id, "dev.asuka.app.%s", clean);
	if (n == 0 || !g_application_id_is_valid(id)) return strdup("dev.asuka.App");
	return strdup(id);
}

// app_flags: the application handles its command line and open, and is
// unique unless it has no ID, runs for automation, a test or asuka dev,
// or asked to run as often as it's started.
static GApplicationFlags app_flags(void) {
	GApplicationFlags f = G_APPLICATION_HANDLES_OPEN | G_APPLICATION_HANDLES_COMMAND_LINE;
	if (str_empty(H.cfg.id) || H.cfg.multiple_instances) return f | G_APPLICATION_NON_UNIQUE;
	const char *keys[] = {"ASUKA_SNAPSHOT", "ASUKA_SCRIPT", "ASUKA_STATE", "ASUKA_COMMANDS", "ASUKA_TEST", "ASUKA_DEV"};
	for (size_t i = 0; i < AK_LEN(keys); i++) {
		const char *v = getenv(keys[i]);
		if (v && *v) return f | G_APPLICATION_NON_UNIQUE;
	}
	return f;
}

// lx_mount starts the core with the host's callbacks and runs the app:
// the bytecode asuka dev sends when ASUKA_DEV is set, else the app
// compiled in.
void lx_mount(void);
void lx_mount(void) {
	static asuka_host_callbacks cb;
	cb = lx_callbacks();
	H.depth++;
	lx_route_start();
	asuka_start(&cb);
	H.started = true;
	char *err = NULL;
	const char *dev = getenv("ASUKA_DEV");
	int rc;
	if (dev && *dev) {
		H.dev = true;
		const char *colon = strrchr(dev, ':');
		if (!colon) {
			lx_log("ASUKA_DEV=%s: want host:port", dev);
			exit(1);
		}
		char hostname[256];
		size_t hn = (size_t)(colon - dev) < sizeof hostname - 1 ? (size_t)(colon - dev) : sizeof hostname - 1;
		memcpy(hostname, dev, hn);
		hostname[hn] = 0;
		rc = asuka_dev_connect(hostname, atoi(colon + 1), "{\"platform\":\"linux\",\"device\":\"desktop\"}", &err);
	} else if (asuka_has_app()) {
		rc = asuka_run(&err);
	} else {
		lx_log("this player has no app: run it under asuka dev (ASUKA_DEV=host:port)");
		exit(1);
	}
	H.depth--;
	lx_enter_begin();
	lx_enter_end();
	if (rc != 0) {
		lx_log("%s", err ? err : "the core failed (see its log)");
		if (err) asuka_free_string(err);
		exit(1);
	}
}

void lx_setup(void);
void lx_setup(void) {
	lx_load_theme();
	lx_init_locale();
	H.main = lx_new_main_window();
	lx_read_palette(H.main->content);
	lx_win_env(H.main, &H.env);
	lx_watch_settings();
	lx_watch_desktop();
}

// lx_start makes the main window and mounts the root in it, on the
// application's first launch.
void lx_start(void);
void lx_start(void) {
	lx_setup();
	lx_mount();
	// An app that keeps running with no window is held: GApplication
	// would quit as its last window goes. One in the background shows no
	// window at launch (but in automation runs, which look at it) until
	// it shows one.
	if (H.cfg.keep_running || H.cfg.background) g_application_hold((GApplication *)H.app);
	if (H.cfg.background && !lx_automating()) {
		if (!H.main->configured) {
			ak_window_config c = {0};
			lx_configure(H.main, &c);
		}
		H.main->hidden = true;
	}
	lx_show_main();
	lx_notify_action_at_start();
	lx_notify_restore();
	lx_automation();
}

int main(int argc, char **argv) {
	dsk_config_load(&H.cfg);
	// The app's data, where GNOME apps keep theirs (Flatpak's own): before
	// anything reads it.
	lx_dirs_init(argv[0]);
	lx_crash_start();
	// Opened at login: the argument the autostart entry adds, taken out.
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], login_arg) == 0) {
			lx_opened_at_login = true;
			memmove(argv + i, argv + i + 1, (size_t)(argc - i) * sizeof *argv);
			argc--;
			break;
		}
	char err[512];
	if (!lx_gtk_load(err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	// The user's locale for GTK's own words and input methods, as gtk_init
	// would set it, but numbers in C's: the core and packages read and
	// write them with strtod and printf ("2.5", JSON, SQL), which a
	// locale with a decimal comma would make "2,5". Qt does the same.
	setlocale(LC_ALL, "");
	setlocale(LC_NUMERIC, "C");
	gtk_disable_setlocale();
	lx_register_classes();
	H.app_name = app_name(argv[0]);
	H.app_id = app_id(H.app_name);
	H.assets = lx_assets_dir(argv[0]);
	lx_update_start();
	H.automating = lx_automating();
	// A notification's systemd timer runs the app to show it: no window,
	// no GTK (notify_timer.c).
	const char *nid = getenv("ASUKA_NOTIFY");
	if (nid && *nid) return lx_notify_headless(nid);
	H.app = (GtkApplication *)adw_application_new(H.app_id, app_flags());
	lx_install_notify_actions();
	lx_watch_app();
	lx_watch_signals();
	lx_watch_memory();
	g_set_application_name(H.app_name);
	lx_watch_launches();
	lx_modules_init();
	// GApplication gets the whole command line: with HANDLES_COMMAND_LINE
	// and no options of its own, it parses none of the app's, and hands
	// the lot to the instance that runs.
	int rc = g_application_run((GApplication *)H.app, argc, argv);
	lx_relaunch_if_asked(argv);
	return rc;
}
