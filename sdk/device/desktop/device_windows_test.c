// device_windows_test.c: the clipboard's rich text on Windows
// (platform/windows' TestStdViews, in the signed-in session):
// device_windows.c, included, with its calls' answers caught here.
// copyHTML puts CF_HTML (its header's byte offsets right, UTF-8) and
// CF_UNICODETEXT; clipboardHTML reads the fragment back; clipboardHas
// says html; text alone has none. Another app's CF_HTML, as Chrome and
// Word write it, gives its fragment.
#define device_Platform_clipboardHTML_done t_html_done
#define device_Platform_clipboardText_done t_text_done
#define device_Platform_copyHTML_done t_copied
#define asuka_call_fail t_fail
#include "device_windows.c"

#include <stdio.h>

static int failures;

#define CHECK(c, ...)                                      \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s: ", __FILE__, __LINE__, #c); \
			printf(__VA_ARGS__);                           \
			printf("\n");                                  \
			failures++;                                    \
		}                                                  \
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

// raw is the clipboard's data of a format, NUL ended (NULL: none).
static char *raw(UINT cf, size_t *n) {
	char *s = NULL;
	if (!open_clipboard()) return NULL;
	HANDLE h = GetClipboardData(cf);
	const char *p = h ? GlobalLock(h) : NULL;
	if (p) {
		*n = GlobalSize(h);
		s = malloc(*n + 1);
		memcpy(s, p, *n);
		s[*n] = 0;
		GlobalUnlock(h);
	}
	CloseClipboard();
	return s;
}

static long long field(const char *s, const char *name) {
	const char *f = strstr(s, name);
	return f ? strtoll(f + strlen(name), NULL, 10) : -1;
}

// media: pickMedia's images with WIC: a PNG's size; written as a JPEG,
// scaled to its long side's most (never up), of its quality; what isn't
// an image refused (copied as it is, then).
static void media(void) {
	enum { W = 400, H = 300 };
	uint8_t *px = malloc(W * H * 4);
	for (int i = 0; i < W * H; i++) {
		px[i * 4] = (uint8_t)(i % W), px[i * 4 + 1] = (uint8_t)(i / W), px[i * 4 + 2] = 128, px[i * 4 + 3] = 255;
	}
	asuka_bytes png = {0};
	CHECK(asuka_win_png(W, H, px, &png), "no PNG");
	free(px);
	char *dir = asuka_sprintf("%s\\pick-test", asuka_desktop_cache_dir());
	CreateDirectoryA(dir, NULL);
	char *in = asuka_sprintf("%s\\in.png", dir), *out = asuka_sprintf("%s\\out.jpg", dir), *big = asuka_sprintf("%s\\big.jpg", dir),
	     *low = asuka_sprintf("%s\\low.jpg", dir), *text = asuka_sprintf("%s\\note.png", dir);
	FILE *f = fopen(in, "wb");
	fwrite(png.data, 1, png.len, f);
	fclose(f);
	free(png.data);
	f = fopen(text, "wb");
	fputs("not an image", f);
	fclose(f);
	int64_t w = 0, h = 0;
	CHECK(win_image_size(in, &w, &h) && w == W && h == H, "size %lldx%lld", (long long)w, (long long)h);
	CHECK(win_jpeg(in, out, 100, 0.9, &w, &h) && w == 100 && h == 75, "scaled %lldx%lld", (long long)w, (long long)h);
	CHECK(win_image_size(out, &w, &h) && w == 100 && h == 75, "written %lldx%lld", (long long)w, (long long)h);
	CHECK(win_jpeg(in, big, 4000, 0.9, &w, &h) && w == W && h == H, "never up: %lldx%lld", (long long)w, (long long)h);
	CHECK(win_jpeg(in, low, 4000, 0.1, &w, &h), "quality 0.1");
	WIN32_FILE_ATTRIBUTE_DATA a, b;
	CHECK(GetFileAttributesExA(big, GetFileExInfoStandard, &a) && GetFileAttributesExA(low, GetFileExInfoStandard, &b) && b.nFileSizeLow < a.nFileSizeLow,
	      "quality 0.1 isn't smaller");
	CHECK(!win_image_size(text, &w, &h) && !win_jpeg(text, out, 100, 0.9, &w, &h), "a text file read as an image");
	free(in), free(out), free(big), free(low), free(text), free(dir);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const char *html = "<p>Hi <b>there</b> &amp; caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac</p>";
	const char *text = "Hi there & caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac";

	// copyHTML: both formats.
	reset();
	p_copy_html(NULL, html, text, call);
	CHECK(got.answers == 1 && !got.error, "copyHTML: %s", got.error ? got.error : "no answer");
	device_ClipboardKinds k = kinds();
	CHECK(k.html && k.text && !k.image, "kinds: html %d text %d image %d", k.html, k.text, k.image);

	// CF_HTML as other apps read it: each offset where it says.
	size_t n = 0;
	char *cf = raw(cf_html(), &n);
	CHECK(cf != NULL, "no CF_HTML");
	if (cf) {
		long long sh = field(cf, "StartHTML:"), eh = field(cf, "EndHTML:"), sf = field(cf, "StartFragment:"), ef = field(cf, "EndFragment:");
		size_t len = strlen(cf), hn = strlen(html);
		CHECK(strncmp(cf, "Version:0.9\r\n", 13) == 0, "%.40s", cf);
		CHECK(sh > 0 && (size_t)eh == len && strncmp(cf + sh, "<html>", 6) == 0, "StartHTML %lld EndHTML %lld of %zu", sh, eh, len);
		CHECK(sf > sh && ef - sf == (long long)hn && strncmp(cf + sf, html, hn) == 0, "StartFragment %lld EndFragment %lld", sf, ef);
		CHECK(sf >= 20 && strncmp(cf + sf - 20, "<!--StartFragment-->", 20) == 0 && strncmp(cf + ef, "<!--EndFragment-->", 18) == 0, "the comments");
		CHECK(strstr(cf + eh - 7, "</html>") != NULL, "ends %s", cf + (eh > 7 ? eh - 7 : 0));
		free(cf);
	}

	// clipboardHTML: the fragment, as it was put; clipboardText: the text.
	reset();
	p_html(NULL, call);
	CHECK(got.answers == 1 && got.value && strcmp(got.value, html) == 0, "clipboardHTML: %s", got.value ? got.value : "(none)");
	reset();
	p_text(NULL, call);
	CHECK(got.answers == 1 && got.value && strcmp(got.value, text) == 0, "clipboardText: %s", got.value ? got.value : "(none)");

	// Another app's: Chrome's header (with SourceURL), its offsets the
	// fragment's.
	const char *chrome_html = "<html>\r\n<body>\r\n<!--StartFragment--><a href=\"https://example.com/\">link</a><!--EndFragment-->\r\n</body>\r\n</html>";
	char chrome[512];
	size_t head = 0;
	for (int pass = 0; pass < 2; pass++) {
		const char *start = strstr(chrome_html, "<!--StartFragment-->") + 20, *end = strstr(chrome_html, "<!--EndFragment-->");
		head = (size_t)snprintf(chrome, sizeof chrome,
		                        "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\nSourceURL:https://example.com/\r\n",
		                        head, head + strlen(chrome_html), head + (size_t)(start - chrome_html), head + (size_t)(end - chrome_html));
	}
	snprintf(chrome + head, sizeof chrome - head, "%s", chrome_html);
	char *f = fragment(chrome, strlen(chrome));
	CHECK(strcmp(f, "<a href=\"https://example.com/\">link</a>") == 0, "Chrome's: %s", f);
	free(f);
	// No fragment offsets: what's between the comments.
	const char *bare = "Version:1.0\r\nStartHTML:-1\r\nEndHTML:-1\r\n<html><body><!--StartFragment--><i>x</i><!--EndFragment--></body></html>";
	f = fragment(bare, strlen(bare));
	CHECK(strcmp(f, "<i>x</i>") == 0, "no offsets: %s", f);
	free(f);
	// Offsets past the data: not trusted.
	const char *bad = "Version:0.9\r\nStartFragment:0000009999\r\nEndFragment:0000099999\r\n<b>y</b>";
	f = fragment(bad, strlen(bad));
	CHECK(strcmp(f, "<b>y</b>") == 0, "bad offsets: %s", f);
	free(f);

	// Text alone: no HTML.
	CHECK(open_clipboard(), "the clipboard is in use");
	EmptyClipboard();
	CHECK(put(CF_UNICODETEXT, L"plain", sizeof L"plain"), "no text put");
	CloseClipboard();
	k = kinds();
	CHECK(!k.html && k.text, "text alone: html %d text %d", k.html, k.text);
	reset();
	p_html(NULL, call);
	CHECK(got.answers == 1 && !got.value && !got.error, "text alone: %s", got.value ? got.value : got.error ? got.error : "");
	reset();

	media();

	printf(failures ? "FAIL\n" : "ok\n");
	return failures ? 1 : 0;
}
