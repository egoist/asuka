// device_windows_test.c: the clipboard's rich text on Windows
// (platform/windows' TestStdViews, in the signed-in session):
// device_windows.c, included, with its calls' answers caught here.
// copyHTML puts CF_HTML (its header's byte offsets right, UTF-8) and
// CF_UNICODETEXT; clipboardHTML reads the fragment back; clipboardHas
// says html; text alone has none. Another app's CF_HTML, as Chrome and
// Word write it, gives its fragment. copyFiles puts CF_HDROP (wide, full
// paths) and a copy's drop effect; clipboardFiles reads them back.
// copyFormats puts an app's own format (registered by its type) beside
// text and HTML; clipboardFormats lists them as MIME types, clipboardData
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

// listed reports whether the last list answered has s.
static bool listed(const char *s) {
	for (size_t i = 0; i < got.list.len; i++)
		if (!strcmp(got.list.data[i], s)) return true;
	return false;
}

static bool ends(const char *s, const char *end) { return strlen(s) >= strlen(end) && !strcmp(s + strlen(s) - strlen(end), end); }

static bool data_is(const char *s) { return got.data && got.data->len == strlen(s) && !memcmp(got.data->data, s, got.data->len); }

// files: copyFiles' CF_HDROP, read back; formats: an app's own beside
// text and HTML, listed and read back.
static void files_and_formats(void) {
	char *dir = asuka_sprintf("%s\\clip-test", asuka_desktop_cache_dir());
	CreateDirectoryA(dir, NULL);
	char *a = asuka_sprintf("%s\\caf\xc3\xa9.txt", dir), *b = asuka_sprintf("%s\\b.pdf", dir);
	for (int i = 0; i < 2; i++) {
		wchar_t *w = asuka_wide(i ? b : a);
		FILE *f = _wfopen(w, L"wb");
		fputs("x", f);
		fclose(f);
		free(w);
	}
	char *paths[] = {a, b};
	reset();
	p_copy_files(NULL, (device_list_string){paths, 2}, call);
	CHECK(got.answers == 1 && !got.error, "copyFiles: %s", got.error ? got.error : "no answer");
	device_ClipboardKinds k = kinds();
	CHECK(k.files && !k.text, "kinds: files %d text %d", k.files, k.text);
	size_t n = 0;
	char *drop = raw(CF_HDROP, &n);
	CHECK(drop && ((DROPFILES *)drop)->fWide && ((DROPFILES *)drop)->pFiles == sizeof(DROPFILES), "DROPFILES");
	free(drop);
	reset();
	p_files_read(NULL, call);
	// Full paths (the cache's may be the working directory's), in order.
	CHECK(got.answers == 1 && got.list.len == 2 && got.list.data[0][1] == ':' && ends(got.list.data[0], "\\clip-test\\caf\xc3\xa9.txt") &&
	          ends(got.list.data[1], "\\clip-test\\b.pdf"),
	      "clipboardFiles: %zu %s", got.list.len, got.list.len ? got.list.data[0] : "");
	reset();
	p_formats(NULL, call);
	CHECK(listed("CF_HDROP") && listed("Preferred DropEffect"), "formats of files");
	char *missing[] = {b, "C:\\no\\such.txt"};
	reset();
	p_copy_files(NULL, (device_list_string){missing, 2}, call);
	CHECK(got.error && strstr(got.error, "no file at"), "a missing file: %s", got.error ? got.error : "no error");
	reset();

	const char *json = "{\"shapes\":[1]}", *html = "<b>a drawing</b>";
	device_ClipboardFormat fs[] = {
	    {"application/x.drawing+json", {(uint8_t *)json, strlen(json)}},
	    {"text/html", {(uint8_t *)html, strlen(html)}},
	    {"text/plain", {(uint8_t *)"a drawing", 9}},
	};
	p_copy_formats(NULL, (device_list_ClipboardFormat){fs, 3}, call);
	CHECK(got.answers == 1 && !got.error, "copyFormats: %s", got.error ? got.error : "no answer");
	k = kinds();
	CHECK(k.text && k.html && !k.files, "kinds: text %d html %d files %d", k.text, k.html, k.files);
	reset();
	p_formats(NULL, call);
	CHECK(listed("application/x.drawing+json") && listed("text/html") && listed("text/plain") && !listed("CF_HDROP"), "formats: %zu, %s", got.list.len,
	      got.list.len ? got.list.data[0] : "");
	for (size_t i = 0; i < got.list.len; i++)
		for (size_t j = i + 1; j < got.list.len; j++) CHECK(strcmp(got.list.data[i], got.list.data[j]), "twice: %s", got.list.data[i]);
	reset();
	p_data(NULL, "application/x.drawing+json", call);
	CHECK(data_is(json), "the app's own: %zu bytes", got.data ? got.data->len : 0);
	reset();
	p_data(NULL, "text/html", call);
	CHECK(data_is(html), "html");
	reset();
	p_data(NULL, "text/plain", call);
	CHECK(data_is("a drawing"), "text");
	reset();
	p_data(NULL, "application/x.other", call);
	CHECK(got.answers == 1 && !got.data && !got.error, "another type: none");
	reset();
	// A bitmap's, as a BMP file, and back.
	uint8_t bmp[14 + 40 + 4] = {'B', 'M'};
	BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)(bmp + 14);
	bi->biSize = 40, bi->biWidth = 1, bi->biHeight = 1, bi->biPlanes = 1, bi->biBitCount = 32;
	bmp[14 + 40] = 0x11, bmp[14 + 40 + 3] = 0xFF;
	device_ClipboardFormat img[] = {{"image/bmp", {bmp, sizeof bmp}}};
	p_copy_formats(NULL, (device_list_ClipboardFormat){img, 1}, call);
	CHECK(kinds().image, "a BMP: an image");
	reset();
	p_data(NULL, "image/bmp", call);
	CHECK(got.data && got.data->len >= sizeof bmp && got.data->data[0] == 'B' && got.data->data[1] == 'M' && *(DWORD *)(got.data->data + 10) == 54 &&
	          got.data->data[54] == 0x11,
	      "the BMP back: %zu bytes", got.data ? got.data->len : 0);
	reset();
	free(a), free(b), free(dir);
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
	files_and_formats();

	printf(failures ? "FAIL\n" : "ok\n");
	return failures ? 1 : 0;
}
