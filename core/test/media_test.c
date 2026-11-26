// Image's placeholders and animated images, Video, WebView, native views
// (media.c), the hash decoders (imagehash.c), against the reference
// implementations' golden data (testdata/imagehash).
#include "uih.h"

#include <stdlib.h>

bool ak_blurhash(const char *hash, size_t n, int w, int h, double punch, uint8_t *out);
bool ak_thumbhash(const uint8_t *hash, size_t n, int *w, int *h, uint8_t **rgba);
bool ak_thumbhash_parse(const char *s, size_t n, uint8_t **out, size_t *len);

static char log_buf[512];

static void logs(const char *s) {
	size_t n = strlen(log_buf);
	snprintf(log_buf + n, sizeof log_buf - n, "%s%s", n ? "|" : "", s);
}

static ak_val log_str(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	char s[128];
	snprintf(s, sizeof s, "%s:%s", (const char *)c->fn, ((ak_str *)a[0].p)->data);
	logs(s);
	return noval();
}

static ak_val log_run(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	logs((const char *)c->fn);
	return noval();
}

// ---- The decoders, against the reference implementations' output ----

static char *read_file(const char *rel) {
	const char *root = getenv("ASUKA_ROOT");
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", root ? root : "..", rel);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)n + 1);
	size_t got = fread(b, 1, (size_t)n, f);
	b[got] = 0;
	fclose(f);
	return b;
}

// field finds "key": in s from *at and reads its string (into out) or
// number.
static bool field(const char *s, const char **at, const char *key, char *out, size_t cap, long *num) {
	char pat[32];
	snprintf(pat, sizeof pat, "\"%s\":", key);
	const char *p = strstr(*at, pat);
	if (!p) return false;
	p += strlen(pat);
	while (*p == ' ') p++;
	if (*p == '"') {
		const char *e = strchr(p + 1, '"');
		size_t n = (size_t)(e - p - 1);
		if (n >= cap) n = cap - 1;
		memcpy(out, p + 1, n);
		out[n] = 0;
		*at = e + 1;
	} else {
		*num = strtol(p, (char **)at, 10);
	}
	AK_UNUSED(s);
	return true;
}

static void golden(const char *file, bool blur) {
	char *json = read_file(file);
	REQUIRE(json);
	const char *at = json;
	char hash[512], *rgba64 = malloc(1 << 20);
	int cases = 0;
	long w = 0, h = 0;
	while (field(json, &at, "hash", hash, sizeof hash, NULL)) {
		REQUIRE(field(json, &at, "w", NULL, 0, &w) && field(json, &at, "h", NULL, 0, &h) && field(json, &at, "rgba", rgba64, 1 << 20, NULL));
		uint8_t *want;
		size_t wn;
		REQUIRE(ak_thumbhash_parse(rgba64, strlen(rgba64), &want, &wn));  // base64
		uint8_t *got = NULL;
		int gw = (int)w, gh = (int)h;
		if (blur) {
			got = ak_alloc((size_t)(w * h * 4));
			if (!ak_blurhash(hash, strlen(hash), (int)w, (int)h, 1, got)) T_FAIL("%s: not decoded", hash);
		} else {
			uint8_t *raw;
			size_t rn;
			if (!ak_thumbhash_parse(hash, strlen(hash), &raw, &rn) || !ak_thumbhash(raw, rn, &gw, &gh, &got)) T_FAIL("%s: not decoded", hash);
			else ak_free(raw);
		}
		if (got && (gw != w || gh != h || wn != (size_t)(w * h * 4) || memcmp(got, want, wn) != 0)) {
			size_t i = 0;
			while (i < wn && got[i] == want[i]) i++;
			T_FAIL("%s %s: %dx%d, byte %zu is %d, want %d", file, hash, gw, gh, i, i < wn ? got[i] : -1, i < wn ? want[i] : -1);
		}
		ak_free(got);
		ak_free(want);
		cases++;
	}
	CHECK(cases > (blur ? 50 : 5));
	free(rgba64);
	free(json);
}

TEST(blurhash_golden) { golden("core/test/testdata/imagehash/blurhash.json", true); }
TEST(thumbhash_golden) { golden("core/test/testdata/imagehash/thumbhash.json", false); }

TEST(bad_hashes) {
	uint8_t px[32 * 32 * 4];
	const char *bad[] = {"", "LEHV6", "LEHV6nWB2yk8pyo0adR*.7kCMdn", "LEHV6nWB2yk8pyo0adR*.7kCMdn\x01", "\"EHV6nWB2yk8pyo0adR*.7kCMdnj"};
	for (size_t i = 0; i < AK_LEN(bad); i++) CHECK(!ak_blurhash(bad[i], strlen(bad[i]), 32, 32, 1, px));
	uint8_t *raw;
	size_t n;
	CHECK(!ak_thumbhash_parse("abc", 3, &raw, &n));
	CHECK(!ak_thumbhash_parse("!!!!!!!!", 8, &raw, &n));
}

// ---- Image placeholders (TestImageHashPlaceholders) ----

static void *blur;

static ak_val read_blur(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(blur, true);
}

static ak_val hash_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *i1 = ak_ui_new(AK_EL_IMAGE);
	set_str(i1, AK_KEY_IMAGE_SOURCE, "a.png");
	bind(i1, AK_KEY_IMAGE_BLUR_HASH, read_blur, NULL);
	set_f(i1, AK_KEY_IMAGE_TRANSITION, 0.4);
	add(col, i1);
	ak_view *i2 = ak_ui_new(AK_EL_IMAGE);
	set_str(i2, AK_KEY_IMAGE_SOURCE, "b.png");
	set_str(i2, AK_KEY_IMAGE_BLUR_HASH, "LEHV6nWB2yk8pyo0adR*.7kCMdnj");  // before the aspect ratio: taken at finalize
	set_i(i2, AK_KEY_IMAGE_MODE, AK_IMAGE_MODE_FILL);
	set_f(i2, AK_KEY_ASPECT_RATIO, 2);
	add(col, i2);
	ak_view *i3 = ak_ui_new(AK_EL_IMAGE);
	set_str(i3, AK_KEY_IMAGE_SOURCE, "c.png");
	set_str(i3, AK_KEY_IMAGE_THUMB_HASH, "1QcSHQRnh493V4dIh4eXh1h4kJUI");
	set_i(i3, AK_KEY_IMAGE_PLACEHOLDER_MODE, 2);  // fill
	set_f(i3, AK_KEY_IMAGE_TRANSITION, -1);
	add(col, i3);
	return (ak_val){.p = col};
}

static void ph_size(const ak_fake_view *v, int *w, int *h, size_t *n) {
	ak_span b = bytes_of(v, AK_PROP_IMAGE_PLACEHOLDER_IMAGE);
	*w = *h = 0, *n = 0;
	if (b.len < 8) return;
	const uint8_t *p = (const uint8_t *)b.p;
	*w = p[0] | p[1] << 8, *h = p[4] | p[5] << 8;
	*n = b.len - 8;
}

// A BlurHash or ThumbHash is decoded into the placeholder's picture: a
// BlurHash at the style's aspect ratio (square, stretched, without one), a
// ThumbHash at its own; a new hash sends its picture, a malformed none.
TEST(image_hash_placeholders) {
	blur = cell_ref(ak_str_c("LEHV6nWB2yk8pyo0adR*.7kCMdnj"));
	ak_uitest *a = mount(hash_app, NULL);
	ak_fake_view *i1 = nth(a, AK_KIND_IMAGE, 0), *i2 = nth(a, AK_KIND_IMAGE, 1), *i3 = nth(a, AK_KIND_IMAGE, 2);
	int w, h;
	size_t n;
	ph_size(i1, &w, &h, &n);
	CHECK(w == 32 && h == 32 && n == 32 * 32 * 4 && ak_fake_int(i1, AK_PROP_IMAGE_PLACEHOLDER_MODE) == AK_IMAGE_MODE_STRETCH);
	CHECK_EQ_F(ak_fake_float(i1, AK_PROP_IMAGE_TRANSITION), 0.4);
	ph_size(i2, &w, &h, &n);
	CHECK(w == 32 && h == 16 && ak_fake_int(i2, AK_PROP_IMAGE_PLACEHOLDER_MODE) == AK_IMAGE_MODE_FILL);
	ph_size(i3, &w, &h, &n);
	CHECK(w > 0 && h > 0 && ak_fake_int(i3, AK_PROP_IMAGE_PLACEHOLDER_MODE) == AK_IMAGE_MODE_FILL);
	CHECK_EQ_F(ak_fake_float(i3, AK_PROP_IMAGE_TRANSITION), -1);
	ak_span before = bytes_of(i1, AK_PROP_IMAGE_PLACEHOLDER_IMAGE);
	uint8_t first[16];
	memcpy(first, before.p + 8, 16);
	write_p(blur, ak_str_c("L6PZfSi_.AyE_3t7t7R**0o#DgR4"));
	ak_ui_flush();
	ak_span after = bytes_of(i1, AK_PROP_IMAGE_PLACEHOLDER_IMAGE);
	CHECK(after.len == 8 + 32 * 32 * 4 && memcmp(first, after.p + 8, 16) != 0);
	write_p(blur, ak_str_c("nonsense"));
	ak_ui_flush();
	CHECK_EQ_INT(bytes_of(i1, AK_PROP_IMAGE_PLACEHOLDER_IMAGE).len, 0);
	done(a);
	ak_release(blur);
}

// ---- Animated images (TestAnimatedImagesFollowReduceMotion) ----

static void *playing;

static ak_val read_playing(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_box_new(AK_K_BOOL, ak_vm_cell_get(playing, true))};
}

static ak_val animated_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *i1 = ak_ui_new(AK_EL_IMAGE);
	set_str(i1, AK_KEY_IMAGE_SOURCE, "a.gif");
	add(col, i1);
	ak_view *i2 = ak_ui_new(AK_EL_IMAGE);
	set_str(i2, AK_KEY_IMAGE_SOURCE, "b.gif");
	bind(i2, AK_KEY_IMAGE_ANIMATED, read_playing, NULL);
	add(col, i2);
	return (ak_val){.p = col};
}

// An animated image plays while animated says so; without it, it follows
// Reduce Motion once it says it has frames.
TEST(animated_images_follow_reduce_motion) {
	playing = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
	ak_uitest *a = mount(animated_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *autov = nth(a, AK_KIND_IMAGE, 0), *ctl = nth(a, AK_KIND_IMAGE, 1);
	CHECK(ak_fake_listens(autov, AK_EVENT_ANIMATED) && !ak_fake_listens(ctl, AK_EVENT_ANIMATED));
	CHECK(!has_prop(autov, AK_PROP_IMAGE_PAUSED));
	CHECK(has_prop(ctl, AK_PROP_IMAGE_PAUSED) && !ak_fake_bool(ctl, AK_PROP_IMAGE_PAUSED));
	write_i(playing, 0);
	ak_ui_flush();
	CHECK(ak_fake_bool(ctl, AK_PROP_IMAGE_PAUSED));
	ak_environment env = f->env;
	env.reduce_motion = true;
	ak_fake_set_environment(f, &env);
	CHECK(!has_prop(autov, AK_PROP_IMAGE_PAUSED));  // not before it said it animates
	dispatch(a, (ak_event){.id = autov->id, .kind = AK_EVENT_LOADED, .x = 10, .y = 10});
	dispatch(a, (ak_event){.id = autov->id, .kind = AK_EVENT_ANIMATED});
	CHECK(ak_fake_bool(autov, AK_PROP_IMAGE_PAUSED));
	env.reduce_motion = false;
	ak_fake_set_environment(f, &env);
	CHECK(!ak_fake_bool(autov, AK_PROP_IMAGE_PAUSED));
	write_i(playing, 1);
	ak_ui_flush();
	CHECK(!ak_fake_bool(ctl, AK_PROP_IMAGE_PAUSED));
	done(a);
	ak_release(playing);
}

// ---- Video (TestVideo) ----

static ak_view *video;

static ak_val video_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_i(col, AK_KEY_ALIGN, 1);  // start
	ak_view *v = ak_ui_new(AK_EL_VIDEO);
	set_str(v, AK_KEY_VIDEO_SOURCE, "https://example.com/a.mp4");
	set_i(v, AK_KEY_VIDEO_LOOP, 1);
	on(v, AK_EV_VIDEO_END, log_run, "ended");
	video = ak_ui_ref(v);
	add(col, v);
	return (ak_val){.p = col};
}

// A Video takes its natural size, as an Image does, and is driven by
// commands.
TEST(video) {
	log_buf[0] = 0;
	ak_uitest *a = mount(video_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *v = nth(a, AK_KIND_VIDEO, 0);
	REQUIRE(v);
	CHECK(ak_fake_bool(v, AK_PROP_VIDEO_LOOP) && !has_prop(v, AK_PROP_VIDEO_AUTO_PLAY));
	CHECK(ak_span_eqc(ak_fake_string(v, AK_PROP_VIDEO_SOURCE), "https://example.com/a.mp4"));
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_LOADED, .x = 1920, .y = 1080});
	float d = v->frame[3] - 390.0f * 1080 / 1920;
	CHECK(v->frame[2] == 390 && d < 0.5f && d > -0.5f);
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_ENDED});
	ak_fake_reset(f);
	ak_ui_video_play(video);
	ak_ui_video_seek(video, 1.5);
	ak_ui_flush();
	ak_fake_index_vec cmds = {0};
	ak_fake_commands(f, AK_CMD_PLAY, &cmds);
	CHECK_EQ_INT(cmds.len, 1);
	cmds.len = 0;
	ak_fake_commands(f, AK_CMD_SEEK, &cmds);
	REQUIRE(cmds.len == 1);
	ak_op op = ak_fake_op_at(f, cmds.data[0]);
	double s;
	memcpy(&s, op.value.s.p, 8);
	CHECK(s == 1.5);
	ak_vec_free(&cmds);
	CHECK_EQ_STR(log_buf, "ended");
	ak_release(video);
	done(a);
}

// ---- WebView (TestWebView, TestWebViewExtras) ----

static ak_view *web;

static ak_val log_state(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	const ak_struct *s = a[0].p;
	char b[160];
	snprintf(b, sizeof b, "state:%s,%s,%d,%g,%d,%d", ((ak_str *)s->f[0].p)->data, ((ak_str *)s->f[1].p)->data, (int)s->f[2].i, s->f[3].f,
	         (int)s->f[4].i, (int)s->f[5].i);
	logs(b);
	return noval();
}

static ak_val log_download(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	const ak_struct *d = a[0].p;
	char b[160];
	snprintf(b, sizeof b, "download:%s,%s,%s,%lld", ((ak_str *)d->f[0].p)->data, ((ak_str *)d->f[1].p)->data, ((ak_str *)d->f[2].p)->data,
	         (long long)d->f[3].i);
	logs(b);
	return noval();
}

static ak_val web_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_f(col, AK_KEY_GROW, 1);
	ak_view *v = ak_ui_new(AK_EL_WEB_VIEW);
	set_str(v, AK_KEY_WEB_VIEW_URL, "https://example.com");  // before its options: sent after them
	set_str(v, AK_KEY_WEB_VIEW_PRELOAD, "window.x = 1");
	set_str(v, AK_KEY_WEB_VIEW_USER_AGENT, "UA/1");
	ak_arr *block = ak_arr_new(AK_K_REF, NULL, 2);
	ak_arr_push(&block, (ak_val){.p = ak_str_c("ads.example")});
	ak_arr_push(&block, (ak_val){.p = ak_str_c("/track/")});
	set(v, AK_KEY_WEB_VIEW_BLOCK, (ak_val){.p = block});
	on(v, AK_EV_WEB_VIEW_LOADED, log_str, "load");
	on(v, AK_EV_WEB_VIEW_MESSAGE, log_str, "message");
	on(v, AK_EV_WEB_VIEW_STATE, log_state, NULL);
	on(v, AK_EV_WEB_VIEW_DOWNLOAD, log_download, NULL);
	on(v, AK_EV_WEB_VIEW_NEW_WINDOW, log_str, "new");
	on(v, AK_EV_WEB_VIEW_REQUEST, log_str, "request");
	web = ak_ui_ref(v);
	add(col, v);
	return (ak_val){.p = col};
}

static bool web_module(void *ctx, ak_span module, ak_span method, ak_span args, ak_buf *out) {
	AK_UNUSED(ctx);
	if (!ak_span_eqc(module, "web")) return false;
	if (ak_span_eqc(method, "eval")) {
		CHECK(args.len > 4 && ak_span_eq((ak_span){args.p + 4, args.len - 4}, AK_STR("document.title")));
		ak_buf_put(out, "\"A page\"", 8);
		return true;
	}
	ak_buf_put(out, "sid=1; theme=dark", 17);
	return true;
}

static ak_str *settled_str(ak_uitest *a, ak_future *f) {
	ak_uitest_settle(a);
	if (f->state != AK_FUT_DONE) return NULL;
	return f->result.p;
}

TEST(web_view) {
	log_buf[0] = 0;
	ak_uitest *a = mount(web_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *v = nth(a, AK_KIND_WEB_VIEW, 0);
	REQUIRE(v);
	CHECK(ak_span_eqc(ak_fake_string(v, AK_PROP_WEB_VIEW_URL), "https://example.com") && v->frame[3] == 844);
	CHECK(ak_span_eqc(ak_fake_string(v, AK_PROP_WEB_VIEW_PRELOAD), "window.x = 1") && ak_span_eqc(ak_fake_string(v, AK_PROP_WEB_VIEW_USER_AGENT), "UA/1"));
	CHECK(ak_fake_bool(v, AK_PROP_WEB_VIEW_REQUESTS));
	ak_str_vec block = {0};
	ak_fake_strings(v, AK_PROP_WEB_VIEW_BLOCK, &block);
	CHECK(block.len == 2 && ak_span_eqc(block.data[1], "/track/"));
	ak_vec_free(&block);
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_NAVIGATED, .s = AK_STR("https://example.com/")});
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_MESSAGE, .s = AK_STR("hi")});
	ak_buf sb = {0};
	ak_buf_wstr(&sb, AK_STR("https://example.com/a"));
	ak_buf_wstr(&sb, AK_STR("A page"));
	ak_buf_u8(&sb, 1 | 2);
	ak_buf_f32(&sb, 0.5f);
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_WEB_STATE, .s = ak_strn(sb.data, sb.len)});
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_WEB_STATE, .s = ak_strn(sb.data, 5)});  // short: dropped
	ak_buf_free(&sb);
	ak_buf db = {0};
	ak_buf_wstr(&db, AK_STR("https://example.com/f.zip"));
	ak_buf_wstr(&db, AK_STR("application/zip"));
	ak_buf_wstr(&db, AK_STR("f.zip"));
	ak_buf_u64(&db, 1234);
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_DOWNLOAD, .s = ak_strn(db.data, db.len)});
	ak_buf_free(&db);
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_NEW_WINDOW, .s = AK_STR("https://example.com/new")});
	ak_fake_dispatch(f, &(ak_event){.id = v->id, .kind = AK_EVENT_REQUEST, .s = AK_STR("https://cdn.example/v.m3u8")});
	CHECK_EQ_STR(log_buf, "load:https://example.com/|message:hi|state:https://example.com/a,A page,1,0.5,1,0|"
	                      "download:https://example.com/f.zip,application/zip,f.zip,1234|new:https://example.com/new|"
	                      "request:https://cdn.example/v.m3u8");
	// The page's options go before its URL.
	int url_at = -1, last_option = -1;
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.code != AK_OP_SET_PROP || op.id != v->id) continue;
		if (op.prop == AK_PROP_WEB_VIEW_URL) url_at = (int)i;
		else if (op.prop == AK_PROP_WEB_VIEW_PRELOAD || op.prop == AK_PROP_WEB_VIEW_BLOCK) last_option = (int)i;
	}
	CHECK(url_at > last_option && last_option > 0);
	// Commands, and module calls answered.
	ak_fake_reset(f);
	ak_str *js = ak_str_c("1+1");
	ak_ui_web_eval(web, js);
	ak_release(js);
	ak_ui_web_navigate(web, 0);
	ak_ui_web_navigate(web, 2);
	f->call_fn = web_module;
	ak_str *title_js = ak_str_c("document.title"), *url = ak_str_c("https://example.com");
	ak_future *title = ak_ui_web_eval_result(web, title_js), *cookies = ak_ui_web_cookies(url);
	ak_release(title_js), ak_release(url);
	ak_ui_flush();
	ak_fake_index_vec cmds = {0};
	ak_fake_commands(f, AK_CMD_EVAL, &cmds);
	CHECK(cmds.len == 1 && ak_span_eqc(ak_fake_op_at(f, cmds.data[0]).value.s, "1+1"));
	cmds.len = 0;
	ak_fake_commands(f, AK_CMD_WEB_NAVIGATE, &cmds);
	CHECK(cmds.len == 2 && ak_fake_op_at(f, cmds.data[0]).value.s.p[0] == 0 && ak_fake_op_at(f, cmds.data[1]).value.s.p[0] == 2);
	ak_vec_free(&cmds);
	// load(url): the URL it was given, sent again (its page has moved on
	// since, which its prop, unchanged, wouldn't say).
	ak_fake_reset(f);
	ak_str *again = ak_str_c("https://example.com");
	ak_ui_web_load(web, again);
	ak_release(again);
	ak_ui_flush();
	ak_fake_view *after = nth(a, AK_KIND_WEB_VIEW, 0);
	int sent = 0;
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.prop == AK_PROP_WEB_VIEW_URL && op.id == after->id) sent++;
	}
	CHECK_EQ_INT(sent, 1);
	ak_str *t = settled_str(a, title), *c = settled_str(a, cookies);
	CHECK(t && strcmp(t->data, "\"A page\"") == 0);
	CHECK(c && strcmp(c->data, "sid=1; theme=dark") == 0);
	ak_release(title), ak_release(cookies);
	ak_release(web);
	done(a);
}

// ---- Native views (TestNative, TestNativeListensBeforeProps) ----

static void *selected;
static int64_t changed[4];
static int nchanged;

static ak_val read_selected(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(selected, true);
}

static ak_val on_changed(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	if (nchanged < 4) changed[nchanged] = a[0].i;
	nchanged++;
	write_i(selected, a[0].i);
	return noval();
}

static ak_val native_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_str *type = ak_str_c("SegmentedNative#Segmented"), *items = ak_str_c("items"), *sel = ak_str_c("selected"), *ch = ak_str_c("changed");
	ak_view *v = ak_ui_native(type);
	ak_arr *list = ak_arr_new(AK_K_REF, NULL, 2);
	ak_arr_push(&list, (ak_val){.p = ak_str_c("Day")});
	ak_arr_push(&list, (ak_val){.p = ak_str_c("Week")});
	ak_ui_native_set(v, items, (ak_val){.p = list}, "[s", NULL);  // set before the handler: sent after its listen
	ak_release(list);
	ak_closure *h = fn(on_changed, NULL), *t = fn(read_selected, NULL);
	ak_ui_native_on(v, ch, h, "i", NULL);
	ak_ui_native_bind(v, sel, t, "i", NULL);
	ak_release(h), ak_release(t);
	ak_release(type), ak_release(items), ak_release(sel), ak_release(ch);
	return (ak_val){.p = v};
}

// A native component's view: its type, then its listening, then its
// props, encoded as the native codec has them; a prop is sent again only
// as it changes; its events are decoded for their handlers.
TEST(native_view) {
	selected = cell_int(1);
	nchanged = 0;
	ak_uitest *a = mount(native_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *v = nth(a, AK_KIND_NATIVE, 0);
	REQUIRE(v);
	CHECK(ak_span_eqc(ak_fake_string(v, AK_PROP_NATIVE_TYPE), "SegmentedNative#Segmented") && ak_fake_listens(v, AK_EVENT_NATIVE));
	const ak_fake_native *sel = ak_fake_native_value(v, "selected"), *items = ak_fake_native_value(v, "items");
	REQUIRE(sel && items);
	CHECK(sel->type == 5 && sel->s.len == 8 && sel->s.p[0] == 1);
	// [s: u32 count, then u32-length strings.
	CHECK(items->type == 5 && items->s.len == 4 + 4 + 3 + 4 + 4);
	ak_fake_reset(f);
	write_i(selected, 0);
	ak_ui_flush();
	CHECK_EQ_INT(sets(a), 1);
	write_i(selected, 0);
	ak_ui_flush();
	uint8_t v1[8] = {1};
	ak_fake_native vals[] = {{.name = AK_STR("v"), .type = 5, .s = ak_strn(v1, 8)}};
	ak_fake_emit_native(f, v, "changed", vals, 1);
	CHECK(nchanged == 1 && changed[0] == 1 && peek_i(selected) == 1);
	ak_fake_emit_native(f, v, "other", vals, 1);  // no handler: nothing
	CHECK_EQ_INT(nchanged, 1);
	done(a);
	ak_release(selected);
}

TEST(native_listens_before_props) {
	selected = cell_int(1);
	ak_uitest *a = mount(native_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *v = nth(a, AK_KIND_NATIVE, 0);
	REQUIRE(v);
	ak_buf order = {0};
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.id != v->id) continue;
		if (op.code == AK_OP_LISTEN && op.event == AK_EVENT_NATIVE) ak_buf_put(&order, "listen ", 7);
		else if (op.code == AK_OP_SET_PROP && op.prop == AK_PROP_NATIVE_TYPE) ak_buf_put(&order, "type ", 5);
		else if (op.code == AK_OP_SET_PROP && op.prop == AK_PROP_NATIVE_PROP) ak_buf_put(&order, "prop ", 5);
	}
	CHECK_EQ_STR(cstr(&order), "type listen prop prop ");
	ak_buf_free(&order);
	done(a);
	ak_release(selected);
}

T_MAIN()
