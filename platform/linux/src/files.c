// files.c: what the app is opened with, and its one instance.
// GApplication gets the whole command line (HANDLES_COMMAND_LINE): the
// instance that runs gets each launch's, its own first (the files and URLs
// it was started with: the desktop file's %U, a terminal's arguments),
// then another launch's, which GApplication hands it over D-Bus as that
// launch ends (a unique application: one per app ID). The files among the
// arguments (those that exist, from the launch's working directory, or
// file:// URIs) and the URLs of the app's scheme reach the root together,
// one IncomingURL; another launch's arguments and working directory go to
// the core as SecondInstance, after the app came to the front.
// GApplication's open (D-Bus activation, a file manager's Open With) comes
// the same way.
#include "host.h"

#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>

void lx_start(void);
static bool started;

// start starts the app on its first launch; false if it had.
static bool start(void) {
	if (started) return false;
	started = true;
	lx_start();
	return true;
}

// to_front brings the app's main window (else the one last active) to the
// front: an app without one, the one last active.
static void to_front(void) {
	if (H.main && !H.main->closing && !H.no_main)
		gtk_window_present(H.main->window);
	else if (H.active_win && !H.active_win->closing)
		gtk_window_present(H.active_win->window);
}

typedef struct {
	char *s;
} url_ctx;

static void send_url(void *ctx) {
	char *u = ctx;
	if (H.root) lx_dispatch(&(ak_event){.id = H.root->id, .kind = AK_EVENT_INCOMING_URL, .s = ak_cstr(u)});
	free(u);
}

// lx_incoming_url reports an IncomingURL string (URLs and the files the
// app is opened with, file:// URLs, a line each) to the root.
void lx_incoming_url(const char *u) {
	if (str_empty(u)) return;
	lx_after_core(send_url, strdup(u));
}

static bool exists(const char *p) {
	struct stat st;
	return stat(p, &st) == 0;
}

static void add_line(ak_buf *b, const char *s) {
	if (b->len) ak_buf_u8(b, '\n');
	ak_buf_put(b, s, strlen(s));
}

// from_args is the IncomingURL string of a launch's arguments
// (internal/fileurl's FromArgs): the URLs of the app's scheme, file://
// URIs, and the paths of files that exist (from the launch's directory),
// as file:// URLs; not options.
static void from_args(ak_buf *out, char **args, int n, const char *dir) {
	const char *scheme = H.cfg.scheme;
	size_t sn = scheme ? strlen(scheme) : 0;
	for (int i = 0; i < n; i++) {
		const char *a = args[i];
		if (!*a || *a == '-') continue;
		if (sn && strlen(a) > sn && !strncasecmp(a, scheme, sn) && a[sn] == ':') {
			ak_buf line = {0};
			for (const char *p = a; *p; p++) {
				if (*p == '\r')
					ak_buf_put(&line, "%0D", 3);
				else if (*p == '\n')
					ak_buf_put(&line, "%0A", 3);
				else
					ak_buf_u8(&line, (uint8_t)*p);
			}
			add_line(out, lx_buf_cstr(&line));
			ak_buf_free(&line);
			continue;
		}
		if (strlen(a) > 7 && !strncasecmp(a, "file://", 7)) {
			char *p = g_filename_from_uri(a, NULL, NULL);
			if (p) {
				char *u = g_filename_to_uri(p, NULL, NULL);
				if (u) add_line(out, u);
				g_free(u);
				g_free(p);
			}
			continue;
		}
		char *p = a[0] == '/' || !dir ? strdup(a) : dsk_join(dir, a);
		char *clean = g_canonicalize_filename(p, NULL);
		if (exists(clean)) {
			char *u = g_filename_to_uri(clean, NULL, NULL);
			if (u) add_line(out, u);
			g_free(u);
		}
		g_free(clean);
		free(p);
	}
}

// launch_args are a launch's arguments without the program and Asuka's
// own (the login item's).
static int launch_args(char **argv, int argc, char ***out) {
	char **args = calloc((size_t)argc + 1, sizeof *args);
	if (!args) abort();
	int n = 0;
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--asuka-opened-at-login")) args[n++] = argv[i];
	*out = args;
	return n;
}

typedef struct {
	char **args;
	int n;
	char *cwd;
} second;

static void send_second(void *ctx) {
	second *s = ctx;
	if (H.root) {
		ak_buf b = {0};
		ak_buf_u32(&b, (uint32_t)s->n);
		for (int i = 0; i < s->n; i++) ak_buf_str(&b, s->args[i], strlen(s->args[i]));
		ak_buf_str(&b, S(s->cwd), strlen(S(s->cwd)));
		lx_dispatch(&(ak_event){.id = H.root->id, .kind = AK_EVENT_SECOND_INSTANCE, .s = ak_strn(b.data, b.len)});
		ak_buf_free(&b);
	}
	for (int i = 0; i < s->n; i++) free(s->args[i]);
	free(s->args);
	free(s->cwd);
	free(s);
}

static void on_activate(GApplication *a, gpointer d) {
	(void)a;
	(void)d;
	if (!start()) to_front();
}

static int on_command_line(GApplication *a, GApplicationCommandLine *cl, gpointer d) {
	(void)a;
	(void)d;
	int argc = 0;
	char **argv = g_application_command_line_get_arguments(cl, &argc);
	const char *cwd = g_application_command_line_get_cwd(cl);
	char **args;
	int n = launch_args(argv, argc, &args);
	ak_buf url = {0};
	from_args(&url, args, n, cwd);
	if (start()) {
		lx_incoming_url(lx_buf_cstr(&url));
	} else {
		// Another launch, handed to this one: the main window comes to the
		// front, the core gets the launch's arguments, and its files and
		// links come as IncomingURL.
		to_front();
		second *s = calloc(1, sizeof *s);
		if (!s) abort();
		s->args = calloc((size_t)n + 1, sizeof *s->args);
		if (!s->args) abort();
		for (int i = 0; i < n; i++) s->args[i] = strdup(args[i]);
		s->n = n;
		s->cwd = strdup(S(cwd));
		lx_after_core(send_second, s);
		lx_incoming_url(lx_buf_cstr(&url));
	}
	ak_buf_free(&url);
	free(args);
	g_strfreev(argv);
	return 0;
}

static void on_open(GApplication *a, GFile **files, int n, const char *hint, gpointer d) {
	(void)a;
	(void)hint;
	(void)d;
	if (!start()) to_front();
	// The local ones' paths as file:// URLs, the rest's URIs (its scheme's
	// links).
	ak_buf b = {0};
	for (int i = 0; i < n; i++) {
		char *p = g_file_get_path(files[i]);
		char *u = p ? g_filename_to_uri(p, NULL, NULL) : g_file_get_uri(files[i]);
		if (u && *u) add_line(&b, u);
		g_free(u);
		g_free(p);
	}
	lx_incoming_url(lx_buf_cstr(&b));
	ak_buf_free(&b);
}

// lx_watch_launches starts the app on its first launch (activate, open,
// or its command line) and takes the next.
void lx_watch_launches(void) {
	g_signal_connect(H.app, "activate", G_CALLBACK(on_activate), NULL);
	g_signal_connect(H.app, "command-line", G_CALLBACK(on_command_line), NULL);
	g_signal_connect(H.app, "open", G_CALLBACK(on_open), NULL);
}
