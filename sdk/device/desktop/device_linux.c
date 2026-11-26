// platform/device on Linux: GtkFileDialog for the pickers (the portal's in a
// sandbox), modal to the window the user works in; GtkUriLauncher for the
// browser; GdkClipboard; the session kept from idling
// (gtk_application_inhibit). GTK is touched on the main thread, where its
// answers come; copying what's picked runs on a thread of its own.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // statx
#endif
#include "asuka_linux.h"
#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

static void fail(asuka_call *call, asuka_error *e) { asuka_call_fail(call, e); }

// os_version is the distribution and its version (os-release's NAME and
// VERSION_ID: the host's in a Flatpak).
static char *os_version(void) {
	static const char *paths[] = {"/run/host/os-release", "/etc/os-release", "/usr/lib/os-release"};
	for (size_t i = 0; i < 3; i++) {
		FILE *f = fopen(paths[i], "r");
		if (!f) continue;
		char line[512], name[256] = "", version[128] = "";
		while (fgets(line, sizeof line, f)) {
			char *eq = strchr(line, '=');
			if (!eq || line[0] == '#') continue;
			*eq = 0;
			char *v = eq + 1;
			v[strcspn(v, "\r\n")] = 0;
			size_t n = strlen(v);
			if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) v[n - 1] = 0, v++;
			if (strcmp(line, "NAME") == 0) snprintf(name, sizeof name, "%s", v);
			else if (strcmp(line, "VERSION_ID") == 0) snprintf(version, sizeof version, "%s", v);
		}
		fclose(f);
		if (*name) return *version ? asuka_sprintf("%s %s", name, version) : asuka_strdup(name);
	}
	return asuka_strdup("linux");
}

// born_ms is when path was made (statx's birth time, where the file
// system keeps it), else last changed if !birth; ms since 1970, 0 if
// unknown.
static int64_t born_ms(const char *path, bool birth) {
	struct statx sx;
	if (statx(AT_FDCWD, path, 0, STATX_BTIME | STATX_MTIME, &sx) != 0) return 0;
	if (birth) return (sx.stx_mask & STATX_BTIME) ? (int64_t)sx.stx_btime.tv_sec * 1000 + sx.stx_btime.tv_nsec / 1000000 : 0;
	return (int64_t)sx.stx_mtime.tv_sec * 1000 + sx.stx_mtime.tv_nsec / 1000000;
}

static void p_info(device_Platform *self, asuka_call *call) {
	// Where it came from: a Flatpak (Flathub, another remote), asuka
	// dev's, else a download or a distribution's package.
	device_InstallSource source = getenv("FLATPAK_ID") ? device_InstallSource_otherStore : device_InstallSource_direct;
	if (getenv("ASUKA_DEV")) source = device_InstallSource_development;
	// Installed as the app's data folder was made (its first run), updated
	// as its executable was written.
	int64_t updated = born_ms("/proc/self/exe", false);
	int64_t installed = born_ms(asuka_desktop_data_dir(), true);
	if (!installed || (updated && installed > updated)) installed = updated;
	device_NativeInfo i = {.model = asuka_strdup("PC"), .system = asuka_strdup("Linux"), .systemVersion = os_version(),
	                       .appVersion = asuka_strdup(asuka_desktop_app_version()), .build = asuka_strdup(asuka_desktop_app_build()),
	                       .source = source, .installedMs = installed, .updatedMs = updated};
	device_Platform_info_done(call, i);
}

static void p_browse(device_Platform *self, const char *url, asuka_call *call) {
	if (!device_web_url(url)) {
		asuka_error e = {0};
		asuka_error_failed(&e, "device", 0, "device: not a web URL: %s", url);
		fail(call, &e);
		return;
	}
	if (G(gtk_uri_launcher_new)) {
		gpointer l = G(gtk_uri_launcher_new)(url);
		G(gtk_uri_launcher_launch)(l, asuka_desktop_window(0), NULL, NULL, NULL);
		G(g_object_unref)(l);
	} else {
		G(g_app_info_launch_default_for_uri)(url, NULL, NULL);
	}
	device_Platform_browse_done(call);
}

typedef struct {
	asuka_call *call;
	gpointer dialog;
	bool photos;
	size_t limit;
	char **paths;
	size_t n;
} picking;

static void copy_run(void *p) {
	picking *k = p;
	device_list_File files;
	asuka_error e = {0};
	if (!device_copy_picked(k->paths, k->n, k->limit, &files, &e)) fail(k->call, &e);
	else if (k->photos) device_Platform_pickPhotos_done(k->call, device_photos(&files));
	else device_Platform_pickFiles_done(k->call, files);
	for (size_t i = 0; i < k->n; i++) free(k->paths[i]);
	free(k->paths);
	free(k);
}

static void add_path(picking *k, GFile *f) {
	gchar *p = f ? G(g_file_get_path)(f) : NULL;
	if (p && *p) {
		k->paths = realloc(k->paths, (k->n + 1) * sizeof *k->paths);
		k->paths[k->n++] = asuka_strdup(p);
	}
	G(g_free)(p);
}

static void picked(picking *k, GError *e) {
	G(g_object_unref)(k->dialog);
	if (e && !asuka_linux_cancelled(e)) {
		asuka_error err = {0};
		asuka_linux_fail(&err, e, "the open dialog");
		fail(k->call, &err);
		free(k->paths);
		free(k);
		return;
	}
	if (e) G(g_error_free)(e);  // cancelled: nothing picked
	if (!asuka_desktop_spawn(copy_run, k)) copy_run(k);
}

static void opened(GObject *src, GAsyncResult *res, gpointer p) {
	picking *k = p;
	GError *e = NULL;
	GFile *f = G(gtk_file_dialog_open_finish)(src, res, &e);
	add_path(k, f);
	if (f) G(g_object_unref)(f);
	picked(k, e);
}

static void opened_many(GObject *src, GAsyncResult *res, gpointer p) {
	picking *k = p;
	GError *e = NULL;
	GListModel *list = G(gtk_file_dialog_open_multiple_finish)(src, res, &e);
	if (list) {
		guint n = G(g_list_model_get_n_items)(list);
		for (guint i = 0; i < n; i++) {
			GFile *f = G(g_list_model_get_item)(list, i);
			add_path(k, f);
			if (f) G(g_object_unref)(f);
		}
		G(g_object_unref)(list);
	}
	picked(k, e);
}

// filter is a filter of the MIME types (NULL: any file).
static gpointer filter(device_list_string mimes) {
	if (mimes.len == 0) return NULL;
	for (size_t i = 0; i < mimes.len; i++)
		if (!*mimes.data[i] || strcmp(mimes.data[i], "*/*") == 0) return NULL;
	gpointer f = G(gtk_file_filter_new)();
	static const char *names[][2] = {{"image/*", "Images"}, {"video/*", "Videos"}, {"audio/*", "Audio"}, {"text/*", "Text"}};
	asuka_writer name = {0};
	for (size_t i = 0; i < mimes.len; i++) {
		if (i) asuka_writer_put(&name, ", ", 2);
		asuka_writer_put(&name, mimes.data[i], strlen(mimes.data[i]));
		G(gtk_file_filter_add_mime_type)(f, mimes.data[i]);
	}
	asuka_writer_u8(&name, 0);
	const char *n = (const char *)name.data;
	for (size_t i = 0; mimes.len == 1 && i < 4; i++)
		if (strcmp(mimes.data[0], names[i][0]) == 0) n = names[i][1];
	G(gtk_file_filter_set_name)(f, n);
	asuka_writer_free(&name);
	return f;
}

static void pick(asuka_call *call, const char *title, device_list_string mimes, bool multiple, size_t limit, bool photos) {
	if (!G(gtk_file_dialog_new)) {
		asuka_error e = {0};
		asuka_error_unsupported(&e, "no GtkFileDialog (GTK 4.10)");
		fail(call, &e);
		return;
	}
	picking *k = calloc(1, sizeof *k);
	k->call = call;
	k->photos = photos;
	k->limit = limit;
	k->dialog = G(gtk_file_dialog_new)();
	if (title) G(gtk_file_dialog_set_title)(k->dialog, title);
	G(gtk_file_dialog_set_modal)(k->dialog, true);
	gpointer f = filter(mimes);
	if (f) {
		gpointer store = G(g_list_store_new)(G(gtk_file_filter_get_type)());
		G(g_list_store_append)(store, f);
		G(gtk_file_dialog_set_filters)(k->dialog, store);
		G(gtk_file_dialog_set_default_filter)(k->dialog, f);
		G(g_object_unref)(store);
		G(g_object_unref)(f);
	}
	if (multiple) G(gtk_file_dialog_open_multiple)(k->dialog, asuka_desktop_window(0), NULL, opened_many, k);
	else G(gtk_file_dialog_open)(k->dialog, asuka_desktop_window(0), NULL, opened, k);
}

static void p_photos(device_Platform *self, int64_t max, asuka_call *call) {
	char *image = "image/*";
	pick(call, "Choose Images", (device_list_string){&image, 1}, max != 1, max > 0 ? (size_t)max : 0, true);
}

static void p_files(device_Platform *self, device_list_string types, bool multiple, asuka_call *call) { pick(call, NULL, types, multiple, 0, false); }

static GdkClipboard *clipboard(void) {
	GdkDisplay *d = G(gdk_display_get_default) ? G(gdk_display_get_default)() : NULL;
	return d ? G(gdk_display_get_clipboard)(d) : NULL;
}

static void no_clipboard(asuka_call *call) {
	asuka_error e = {0};
	asuka_error_unsupported(&e, "no display: no clipboard");
	fail(call, &e);
}

static void got_text(GObject *src, GAsyncResult *res, gpointer call) {
	GError *e = NULL;
	char *t = G(gdk_clipboard_read_text_finish)((GdkClipboard *)src, res, &e);
	if (e) G(g_error_free)(e);
	device_Platform_clipboardText_done(call, t ? asuka_strdup(t) : NULL);
	G(g_free)(t);
}

static void p_text(device_Platform *self, asuka_call *call) {
	GdkClipboard *c = clipboard();
	if (!c) return no_clipboard(call);
	G(gdk_clipboard_read_text_async)(c, NULL, got_text, call);
}

static device_ClipboardKinds kinds(void) {
	GdkClipboard *c = clipboard();
	device_ClipboardKinds k = {0};
	if (!c) return k;
	gpointer f = G(gdk_clipboard_get_formats)(c);
	k.text = G(gdk_content_formats_contain_mime_type)(f, "text/plain;charset=utf-8") || G(gdk_content_formats_contain_mime_type)(f, "text/plain") ||
	         G(gdk_content_formats_contain_gtype)(f, 16 << 2);  // G_TYPE_STRING
	k.image = G(gdk_content_formats_contain_gtype)(f, G(gdk_texture_get_type)()) || G(gdk_content_formats_contain_mime_type)(f, "image/png");
	k.url = G(gdk_content_formats_contain_mime_type)(f, "text/uri-list") || G(gdk_content_formats_contain_gtype)(f, G(gdk_file_list_get_type)()) ||
	        G(gdk_content_formats_contain_gtype)(f, G(g_file_get_type)());
	return k;
}

static void p_has(device_Platform *self, asuka_call *call) { device_Platform_clipboardHas_done(call, kinds()); }

static void got_texture(GObject *src, GAsyncResult *res, gpointer call) {
	GError *e = NULL;
	GdkTexture *t = G(gdk_clipboard_read_texture_finish)((GdkClipboard *)src, res, &e);
	if (!t) {
		if (e) G(g_error_free)(e);
		device_Platform_clipboardImage_done(call, NULL);
		return;
	}
	GBytes *png = G(gdk_texture_save_to_png_bytes)(t);
	G(g_object_unref)(t);
	gsize n = 0;
	const void *p = G(g_bytes_get_data)(png, &n);
	asuka_error err = {0};
	char *path = device_write_cache("asuka-clipboard", "png", p, n, &err);
	G(g_bytes_unref)(png);
	if (!path) fail(call, &err);
	else device_Platform_clipboardImage_done(call, path);
}

static void p_image(device_Platform *self, asuka_call *call) {
	GdkClipboard *c = clipboard();
	if (!c) return no_clipboard(call);
	if (!kinds().image) {
		device_Platform_clipboardImage_done(call, NULL);
		return;
	}
	G(gdk_clipboard_read_texture_async)(c, NULL, got_texture, call);
}

static void p_copy(device_Platform *self, const char *path, asuka_call *call) {
	struct stat st;
	asuka_error e = {0};
	if (stat(path, &st) != 0) {
		asuka_error_failed(&e, "device", 0, "device: no file at %s", path);
		return fail(call, &e);
	}
	GdkClipboard *c = clipboard();
	if (!c) return no_clipboard(call);
	GError *ge = NULL;
	GdkTexture *t = G(gdk_texture_new_from_filename)(path, &ge);
	if (!t) {
		if (ge) G(g_error_free)(ge);
		asuka_error_failed(&e, "device", 0, "device: not an image: %s", path);
		return fail(call, &e);
	}
	G(gdk_clipboard_set_texture)(c, t);
	G(g_object_unref)(t);
	device_Platform_copyImage_done(call);
}

typedef struct {
	device_fn_ClipboardKinds changed;
	asuka_call *call;
	GdkClipboard *c;
	gulong handler;
} watching;

static void on_changed(gpointer c, gpointer p) {
	watching *w = p;
	device_ClipboardKinds k = kinds();
	device_fn_ClipboardKinds_call(w->changed, &k);
}

static void stop(void *p) {
	watching *w = p;
	G(g_signal_handler_disconnect)(w->c, w->handler);
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	fail(w->call, &e);
	free(w);
}

static void p_watch(device_Platform *self, device_fn_ClipboardKinds changed, asuka_call *call) {
	GdkClipboard *c = clipboard();
	if (!c) return no_clipboard(call);
	watching *w = calloc(1, sizeof *w);
	*w = (watching){changed, call, c, 0};
	w->handler = g_signal_connect(c, "changed", on_changed, w);
	asuka_call_on_cancel(call, stop, w);
}

// inhibit is the session's idle inhibition while the screen's kept on.
static guint inhibit;

static void p_awake(device_Platform *self, bool on, asuka_error *err) {
	if (on == (inhibit != 0)) return;
	gpointer app = G(g_application_get_default)();
	if (!app) return;
	if (on) inhibit = G(gtk_application_inhibit)(app, asuka_desktop_window(0), 8, "Keeping the screen on");  // GTK_APPLICATION_INHIBIT_IDLE
	else G(gtk_application_uninhibit)(app, inhibit), inhibit = 0;
}

static const device_Platform_vtable vt = {
    .info = p_info, .browse = p_browse, .pickPhotos = p_photos, .pickFiles = p_files, .clipboardText = p_text,
    .clipboardHas = p_has, .clipboardImage = p_image, .copyImage = p_copy, .watchClipboard = p_watch, .keepAwake = p_awake,
};

device_Platform *device_make_Platform(asuka_error *err) {
	if (!asuka_linux()) {
		asuka_error_unsupported(err, "no GLib");
		return NULL;
	}
	device_Platform *p = malloc(sizeof *p);
	p->vt = &vt;
	return p;
}
