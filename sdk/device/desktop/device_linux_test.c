// device_linux_test.c: the clipboard's rich text on Linux (platform/desktop's
// TestStdUnits, with GTK under Xvfb: xvfb-run): device_linux.c, included,
// with its calls' answers caught here. copyHTML offers text/html and the
// text; clipboardHTML reads the HTML back; clipboardHas says html; text
// alone has none. Another app's HTML in UTF-16 (Firefox's, on X11) comes
// back as UTF-8. copyFiles offers a file list and GNOME's copied files;
// clipboardFiles reads the paths back. copyFormats offers an app's own
// type beside text and HTML; clipboardFormats lists them, clipboardData
// reads each back.
#define device_Platform_clipboardHTML_done t_html_done
#define device_Platform_clipboardText_done t_text_done
#define device_Platform_copyHTML_done t_copied
#define device_Platform_copyFiles_done t_copied
#define device_Platform_copyFormats_done t_copied
#define device_Platform_clipboardFiles_done t_list_done
#define device_Platform_clipboardFormats_done t_list_done
#define device_Platform_clipboardData_done t_data_done
#define asuka_call_fail t_fail
#include "device_linux.c"

#include <stdio.h>
#include <unistd.h>

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

// What the last call answered.
static struct {
	int answers;
	char *value;
	char *error;
	device_list_string list;
	asuka_bytes *data;
} got;

static void reset(void) {
	free(got.value);
	free(got.error);
	device_list_string_free(&got.list);
	if (got.data) free(got.data->data), free(got.data);
	memset(&got, 0, sizeof got);
}

void t_html_done(asuka_call *call, char *value) { got.answers++, got.value = value; }
void t_text_done(asuka_call *call, char *value) { got.answers++, got.value = value; }
void t_copied(asuka_call *call) { got.answers++; }
void t_list_done(asuka_call *call, device_list_string v) { got.answers++, got.list = v; }
void t_data_done(asuka_call *call, asuka_bytes *v) { got.answers++, got.data = v; }

// listed reports whether the last list answered has s.
static bool listed(const char *s) {
	for (size_t i = 0; i < got.list.len; i++)
		if (!strcmp(got.list.data[i], s)) return true;
	return false;
}

static bool data_is(const char *s) { return got.data && got.data->len == strlen(s) && !memcmp(got.data->data, s, got.data->len); }
void t_fail(asuka_call *call, asuka_error *e) {
	got.answers++;
	got.error = asuka_strdup(e->message ? e->message : "failed");
	asuka_error_free(e);
}

static asuka_call *const call = (asuka_call *)&got;  // never touched

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
	const char *html = "<p>Hi <b>there</b> &amp; caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac</p>";
	const char *text = "Hi there & caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac";

	// copyHTML: the HTML and the text.
	reset();
	p_copy_html(NULL, html, text, call);
	CHECK(got.answers == 1 && !got.error);
	device_ClipboardKinds k = kinds();
	CHECK(k.html && k.text && !k.image);

	// clipboardHTML: the HTML, as it was put; clipboardText: the text.
	reset();
	p_html(NULL, call);
	WAIT(got.answers == 1);
	CHECK(got.value && strcmp(got.value, html) == 0);
	if (got.value && strcmp(got.value, html) != 0) printf("  %s\n", got.value);
	reset();
	p_text(NULL, call);
	WAIT(got.answers == 1);
	CHECK(got.value && strcmp(got.value, text) == 0);

	// Another app's HTML in UTF-16, a BOM first, and a NUL last: as UTF-8.
	static const unsigned char utf16[] = {0xFF, 0xFE, '<', 0, 'b', 0, '>', 0, 0xE5, 0x65, 0x3D, 0xD8, 0x00, 0xDE, '<', 0, '/', 0, 'b', 0, '>', 0, 0, 0};
	GBytes *b = G(g_bytes_new)(utf16, sizeof utf16);
	GdkContentProvider *cp = G(gdk_content_provider_new_for_bytes)("text/html", b);
	G(g_bytes_unref)(b);
	CHECK(G(gdk_clipboard_set_content)(clipboard(), cp));
	G(g_object_unref)(cp);
	k = kinds();
	CHECK(k.html && !k.text);
	reset();
	p_html(NULL, call);
	WAIT(got.answers == 1);
	CHECK(got.value && strcmp(got.value, "<b>\xe6\x97\xa5\xf0\x9f\x98\x80</b>") == 0);  // 日 and U+1F600
	if (got.value && strcmp(got.value, "<b>\xe6\x97\xa5\xf0\x9f\x98\x80</b>") != 0) printf("  %s\n", got.value);

	// Text alone: no HTML.
	G(gdk_clipboard_set_text)(clipboard(), "plain");
	k = kinds();
	CHECK(!k.html && k.text);
	reset();
	p_html(NULL, call);
	WAIT(got.answers == 1);
	CHECK(!got.value && !got.error);
	reset();

	// copyFiles: the files, as a file manager pastes them; read back.
	char *dir = asuka_sprintf("%s/clip-test", asuka_desktop_cache_dir());
	mkdir(dir, 0755);
	char *a = asuka_sprintf("%s/caf\xc3\xa9 one.txt", dir), *b2 = asuka_sprintf("%s/b.pdf", dir);
	for (int i = 0; i < 2; i++) {
		FILE *f = fopen(i ? b2 : a, "w");
		fputs("x", f);
		fclose(f);
	}
	char *paths[] = {a, b2};
	reset();
	p_copy_files(NULL, (device_list_string){paths, 2}, call);
	CHECK(got.answers == 1 && !got.error);
	k = kinds();
	CHECK(k.files && !k.html);
	reset();
	p_files_read(NULL, call);
	WAIT(got.answers == 1);
	CHECK(got.list.len == 2 && !strcmp(got.list.data[0], a) && !strcmp(got.list.data[1], b2));
	reset();
	p_formats(NULL, call);
	CHECK(listed("text/uri-list") && listed("x-special/gnome-copied-files"));
	reset();
	p_data(NULL, "x-special/gnome-copied-files", call);
	WAIT(got.answers == 1);
	CHECK(got.data && got.data->len > 5 && !memcmp(got.data->data, "copy\nfile:///", 13));
	char *missing[] = {a, "/no/such/file"};
	reset();
	p_copy_files(NULL, (device_list_string){missing, 2}, call);
	CHECK(got.error && strstr(got.error, "no file at"));
	reset();
	// Text alone: no files.
	G(gdk_clipboard_set_text)(clipboard(), "plain");
	CHECK(!kinds().files);
	p_files_read(NULL, call);
	WAIT(got.answers == 1);
	CHECK(got.list.len == 0 && !got.error);
	reset();

	// copyFormats: an app's own type, HTML and text; listed, read back.
	const char *json = "{\"shapes\":[1]}", *bold = "<b>a drawing</b>";
	device_ClipboardFormat fs[] = {
	    {"application/x.drawing+json", {(uint8_t *)json, strlen(json)}},
	    {"text/html", {(uint8_t *)bold, strlen(bold)}},
	    {"text/plain", {(uint8_t *)"a drawing", 9}},
	};
	p_copy_formats(NULL, (device_list_ClipboardFormat){fs, 3}, call);
	CHECK(got.answers == 1 && !got.error);
	k = kinds();
	CHECK(k.text && k.html && !k.files);
	reset();
	p_formats(NULL, call);
	CHECK(listed("application/x.drawing+json") && listed("text/html") && listed("text/plain"));
	for (size_t i = 0; i < got.list.len; i++) {
		CHECK(!strchr(got.list.data[i], ';'));
		for (size_t j = i + 1; j < got.list.len; j++) CHECK(strcmp(got.list.data[i], got.list.data[j]) != 0);
	}
	reset();
	p_data(NULL, "application/x.drawing+json", call);
	WAIT(got.answers == 1);
	CHECK(data_is(json));
	reset();
	p_data(NULL, "text/html", call);
	WAIT(got.answers == 1);
	CHECK(data_is(bold));
	reset();
	p_data(NULL, "text/plain", call);
	WAIT(got.answers == 1);
	CHECK(data_is("a drawing"));
	reset();
	p_data(NULL, "application/x.other", call);
	WAIT(got.answers == 1);
	CHECK(!got.data && !got.error);
	reset();
	remove(a), remove(b2), rmdir(dir);
	free(a), free(b2), free(dir);

	printf(failed ? "FAIL\n" : "ok\n");
	return failed;
}
