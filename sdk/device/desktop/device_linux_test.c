// device_linux_test.c: the clipboard's rich text on Linux (platform/desktop's
// TestStdUnits, with GTK under Xvfb: xvfb-run): device_linux.c, included,
// with its calls' answers caught here. copyHTML offers text/html and the
// text; clipboardHTML reads the HTML back; clipboardHas says html; text
// alone has none. Another app's HTML in UTF-16 (Firefox's, on X11) comes
// back as UTF-8.
#define device_Platform_clipboardHTML_done t_html_done
#define device_Platform_clipboardText_done t_text_done
#define device_Platform_copyHTML_done t_copied
#define asuka_call_fail t_fail
#include "device_linux.c"

#include <stdio.h>

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
} got;

static void reset(void) {
	free(got.value);
	free(got.error);
	memset(&got, 0, sizeof got);
}

void t_html_done(asuka_call *call, char *value) { got.answers++, got.value = value; }
void t_text_done(asuka_call *call, char *value) { got.answers++, got.value = value; }
void t_copied(asuka_call *call) { got.answers++; }
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

	printf(failed ? "FAIL\n" : "ok\n");
	return failed;
}
