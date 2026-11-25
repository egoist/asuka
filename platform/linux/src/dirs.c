// dirs.c: the app's directories and assets, relaunching, and the host's
// half of asuka_desktop.h (what a native package's C may use of it).
#include "host.h"
#include "asuka_desktop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static dsk_dirs dirs;
static char *assets;

// xdg_data_home is $XDG_DATA_HOME if it's absolute, else ~/.local/share.
static char *xdg_data_home(void) {
	char *d = dsk_getenv("XDG_DATA_HOME");
	if (d && d[0] == '/') return d;
	free(d);
	char *home = dsk_getenv("HOME");
	char *p = dsk_join(home ? home : "/tmp", ".local/share");
	free(home);
	return p;
}

void lx_dirs_init(const char *argv0) {
	char *data = dsk_getenv("ASUKA_DATA");
	if (!data) {
		// The XDG data directory's folder for the app (Flatpak's own), by
		// its ID: before the app's packages read it.
		const char *name = !str_empty(H.cfg.id) ? H.cfg.id : dsk_base(argv0);
		char *home = xdg_data_home();
		data = dsk_join(home, name);
		free(home);
		setenv("ASUKA_DATA", data, 1);
	}
	dsk_dirs_find(&dirs, DSK_LINUX, data);
	free(data);
	// The app's own directory (std/fs's appDir), before the crash record
	// and the core start.
	asuka_set_app_dir(lx_data_dir());
	// Its other directories (std/fs's configDir, cacheDir, logDir): the
	// host's own, so the two agree.
	asuka_set_dir(ASUKA_DIR_CONFIG, dirs.config);
	asuka_set_dir(ASUKA_DIR_CACHE, dirs.cache);
	asuka_set_dir(ASUKA_DIR_LOGS, dirs.logs);
}

// lx_os_version is the distribution and its version: the host's, in a
// Flatpak (os-release).
const char *lx_os_version(void) {
	static char v[256];
	if (v[0]) return v;
	static const char *const paths[] = {"/run/host/os-release", "/etc/os-release", "/usr/lib/os-release"};
	for (size_t i = 0; i < AK_LEN(paths) && !v[0]; i++) {
		size_t n;
		char *text = dsk_read_file(paths[i], &n);
		if (!text) continue;
		char name[128] = "", ver[64] = "";
		for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
			char *eq = strchr(line, '=');
			if (!eq || line[0] == '#') continue;
			*eq = 0;
			char *val = eq + 1;
			size_t vl = strlen(val);
			if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl - 1] == val[0]) {
				val[vl - 1] = 0;
				val++;
			}
			if (!strcmp(line, "NAME")) snprintf(name, sizeof name, "%s", val);
			if (!strcmp(line, "VERSION_ID")) snprintf(ver, sizeof ver, "%s", val);
		}
		free(text);
		if (name[0]) snprintf(v, sizeof v, "%s%s%s", name, ver[0] ? " " : "", ver);
	}
	if (!v[0]) snprintf(v, sizeof v, "linux");
	return v;
}

static const char *made(char *d) {
	if (d) dsk_mkdirs(d);
	return S(d);
}

const char *lx_data_dir(void) { return made(dirs.data); }
const char *lx_config_dir(void) { return made(dirs.config); }
const char *lx_cache_dir(void) { return made(dirs.cache); }
const char *lx_logs_dir(void) { return made(dirs.logs); }

static bool is_dir(const char *p) {
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

char *lx_assets_dir(const char *argv0) {
	(void)argv0;
	char *d = dsk_getenv("ASUKA_ASSETS");
	if (d) return assets = d;
	char *exe = dsk_exe_path();
	if (exe) {
		char *dir = dsk_dir(exe);
		char *a = dsk_join(dir, "assets");
		if (is_dir(a)) {
			free(dir);
			free(exe);
			return assets = a;
		}
		free(a);
		// An install prefix: bin/<exe>, share/<exe>/assets.
		char share[4096];
		snprintf(share, sizeof share, "%s/../share/%s/assets", dir, dsk_base(exe));
		free(dir);
		free(exe);
		if (is_dir(share)) return assets = strdup(share);
	}
	return assets = strdup("assets");
}

// ---- Relaunching ----

static bool relaunch;
bool lx_opened_at_login;
const char *(*lx_relaunch_exe)(void);

void lx_relaunch_set(bool on) { relaunch = on; }
bool lx_relaunch_asked(void) { return relaunch; }

// lx_relaunch_if_asked starts the app again once this process's main loop
// has ended (the Quit command's relaunch flag), with the same arguments,
// in a session of its own so it outlives this one's end: the executable
// at the path it started from (lx_relaunch_exe: an update replaces the
// file), with ASUKA_RELAUNCH_WAIT.
void lx_relaunch_if_asked(char **argv) {
	if (!relaunch) return;
	const char *exe = lx_relaunch_exe ? lx_relaunch_exe() : NULL;
	char *self = NULL;
	if (!exe || !*exe) exe = self = dsk_exe_path();
	if (!exe) exe = argv[0];
	// The new process waits for this one to end before it takes the
	// single instance (update.c).
	char me[32];
	snprintf(me, sizeof me, "%d", (int)getpid());
	setenv("ASUKA_RELAUNCH_WAIT", me, 1);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() != 0) _exit(0);
		execv(exe, argv);
		_exit(127);
	}
	if (pid > 0) {
		int status;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
	}
	free(self);
}

// ---- asuka_desktop.h: the host's half ----

typedef struct {
	void (*fn)(void *ctx);
	void *ctx;
} desk_post;

static void run_desk_post(void *p) {
	desk_post *d = p;
	d->fn(d->ctx);
	free(d);
}

void asuka_desktop_post(void (*fn)(void *ctx), void *ctx) {
	desk_post *d = malloc(sizeof *d);
	if (!d) abort();
	d->fn = fn;
	d->ctx = ctx;
	lx_post(run_desk_post, d);
}

bool asuka_desktop_on_main(void) { return g_main_context_is_owner(g_main_context_default()) != 0; }

void *asuka_desktop_window(uint32_t view) {
	if (view == 0) return H.main ? H.main->window : NULL;
	node *n = node_get(view);
	if (!n) return NULL;
	win *w = lx_win_of(n);
	return w ? w->window : NULL;
}

void *asuka_desktop_view(uint32_t view) {
	node *n = view ? node_get(view) : NULL;
	return n ? n->widget : NULL;
}

void *asuka_desktop_webview(uint32_t view) { return view ? lx_web_view(node_get(view)) : NULL; }

uint32_t asuka_desktop_color(uint64_t color) {
	GdkRGBA c;
	if (!lx_resolve(color, &c)) c = H.theme.fg;
	return (uint32_t)u8c(c.red) << 24 | (uint32_t)u8c(c.green) << 16 | (uint32_t)u8c(c.blue) << 8 | u8c(c.alpha);
}

bool asuka_desktop_dark(void) { return H.dark; }
const char *asuka_desktop_app_name(void) { return S(H.app_name); }
const char *asuka_desktop_app_id(void) { return S(H.app_id); }
const char *asuka_desktop_app_version(void) { return S(H.cfg.version); }
const char *asuka_desktop_app_build(void) { return S(H.cfg.build); }
const char *asuka_desktop_data_dir(void) { return lx_data_dir(); }
const char *asuka_desktop_config_dir(void) { return lx_config_dir(); }
const char *asuka_desktop_cache_dir(void) { return lx_cache_dir(); }
const char *asuka_desktop_logs_dir(void) { return lx_logs_dir(); }
// GNOME and the other launchers take the app's icon from its desktop
// file: it doesn't change while the app runs.
const char *asuka_desktop_app_icons(void) { return NULL; }
const char *asuka_desktop_app_icon(void) { return ""; }
bool asuka_desktop_set_app_icon(const char *name) {
	(void)name;
	return false;
}
const char *asuka_desktop_assets_dir(void) { return S(assets); }
