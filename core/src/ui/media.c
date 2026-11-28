// media.c: Image (its placeholders, hash placeholders and animated
// images), Video, WebView and native views (what a native package's
// `native component` shows).
#include "ui/ui_ext.h"
#include "native/native.h"

static void send_str(ak_view *e, ak_prop p, ak_val v) {
	const ak_str *s = v.p;
	ak_batch_set_string(ak_view_batch(e), e->id, p, s ? s->data : "", s ? s->len : 0);
}

static const ak_val *key_val(const ak_view *e, uint16_t key) { return ak_style_get(e, key); }

static void call_str(ak_view *e, uint16_t ev, ak_span s) {
	if (!ak_view_handler(e, ev)) return;
	ak_str *m = ak_str_span(s);
	ak_val arg = {.p = m};
	ak_view_call(e, ev, &arg, 1);
	ak_release(m);
}

// image_size is an image's (or a video's) size within c, from its
// natural size; the style's aspect ratio overrides its own, as CSS's
// does.
static ak_lsize image_size(ak_lsize n, float ratio, ak_lconstraints c) {
	bool kw = !ak_isnan(c.width), kh = !ak_isnan(c.height);
	if (ratio > 0 && n.w > 0 && n.h > 0) n.h = n.w / ratio;
	if (kw && kh) return (ak_lsize){c.width, c.height};
	if (ratio > 0 && kw) return (ak_lsize){c.width, c.width / ratio};
	if (ratio > 0 && kh) return (ak_lsize){c.height * ratio, c.height};
	if (n.w <= 0 || n.h <= 0) return (ak_lsize){kw ? c.width : 0, kh ? c.height : 0};
	if (kw) return (ak_lsize){c.width, c.width * n.h / n.w};
	if (kh) return (ak_lsize){c.height * n.w / n.h, c.height};
	ak_lsize s = n;
	float aw = c.avail_w;
	if (aw > 0 && aw < AK_INF && s.w > aw) s = (ak_lsize){aw, aw * n.h / n.w};
	return s;
}

// ---- Image ----

typedef struct {
	ak_lsize natural;  // as loaded, one point a pixel
	bool watching;     // follows Reduce Motion (it said it animates)
	bool paused;       // as Reduce Motion paused it
} image_state;

static ak_lsize natural_measure(void *ctx, ak_lconstraints c) {
	ak_view *e = ctx;
	return image_size(*(ak_lsize *)e->impl, ak_style_float(e, AK_KEY_ASPECT_RATIO, 0), c);
}

static void image_init(ak_view *e) {
	e->impl = ak_alloc(sizeof(image_state));
	ak_lnode_set_measure(e->box, natural_measure, e);
	ak_view_listen(e, AK_EVENT_LOADED);
}

static void free_impl(ak_view *e) { ak_free(e->impl); }

// Hash placeholders are decoded once per hash and kept, as the
// PlaceholderImage prop's bytes, in two generations of up to 256 each: a
// list scrolling back and forth over its rows decodes each hash once.
#define HASH_CACHE 256

typedef struct {
	ak_str *key;
	ak_bytes *v;  // NULL: a malformed hash
} hash_ent;

static AK_VEC(hash_ent) hash_cur, hash_old;

static void hash_gen_free(void *vec) {
	AK_VEC(hash_ent) *g = vec;
	for (uint32_t i = 0; i < g->len; i++) {
		ak_release(g->data[i].key);
		ak_release(g->data[i].v);
	}
	ak_vec_free(g);
}

void ak_media_cache_free(void);

void ak_media_cache_free(void) {
	hash_gen_free(&hash_cur);
	hash_gen_free(&hash_old);
}

static void hash_put(ak_str *key, ak_bytes *v) {
	if (hash_cur.len >= HASH_CACHE) {
		hash_gen_free(&hash_old);
		hash_old = hash_cur;
		hash_cur.data = NULL;
		hash_cur.len = hash_cur.cap = 0;
	}
	ak_vec_push(&hash_cur, ((hash_ent){ak_retain(key), ak_retain(v)}));
}

static bool hash_get(ak_str *key, ak_bytes **v) {
	for (uint32_t i = 0; i < hash_cur.len; i++)
		if (ak_str_eq(hash_cur.data[i].key, key)) return *v = hash_cur.data[i].v, true;
	for (uint32_t i = 0; i < hash_old.len; i++) {
		if (!ak_str_eq(hash_old.data[i].key, key)) continue;
		*v = hash_old.data[i].v;
		hash_put(key, *v);
		return true;
	}
	return false;
}

bool ak_blurhash(const char *hash, size_t n, int w, int h, double punch, uint8_t *out);
bool ak_thumbhash(const uint8_t *hash, size_t n, int *w, int *h, uint8_t **rgba);
bool ak_thumbhash_parse(const char *s, size_t n, uint8_t **out, size_t *len);

// placeholder is the PlaceholderImage prop of w×h RGBA pixels.
static ak_bytes *placeholder(int w, int h, const uint8_t *rgba) {
	ak_buf b = {0};
	ak_buf_u32(&b, (uint32_t)w);
	ak_buf_u32(&b, (uint32_t)h);
	ak_buf_put(&b, rgba, (size_t)w * h * 4);
	ak_bytes *out = ak_bytes_new(b.data, b.len);
	ak_buf_free(&b);
	return out;
}

// blurhash_image is a BlurHash's picture at ratio (width over height; 0:
// square), 32 pixels on its longer side: a blur needs no more. NULL for
// a malformed hash.
static ak_bytes *blurhash_image(const ak_str *hash, float ratio) {
	if (!hash || !hash->len) return NULL;
	int w = 32, h = 32;
	if (ratio > 1) h = (int)fmax(1, round(32.0 / ratio));
	else if (ratio > 0) w = (int)fmax(1, round(32.0 * ratio));
	char head[2] = {(char)('0' + w), (char)('0' + h)};
	ak_strb kb = {0};
	ak_strb_cstr(&kb, head, 2);
	ak_strb_str(&kb, hash);
	ak_str *key = ak_strb_finish(&kb);
	ak_bytes *v;
	if (hash_get(key, &v)) {
		ak_release(key);
		return ak_retain(v);
	}
	uint8_t *px = ak_alloc((size_t)w * h * 4);
	v = ak_blurhash(hash->data, hash->len, w, h, 1, px) ? placeholder(w, h, px) : NULL;
	ak_free(px);
	hash_put(key, v);
	ak_release(key);
	return v;
}

// thumbhash_image is a ThumbHash's picture (base64); NULL for a
// malformed hash.
static ak_bytes *thumbhash_image(const ak_str *hash) {
	if (!hash || !hash->len) return NULL;
	ak_strb kb = {0};
	ak_strb_cstr(&kb, "t", 1);
	ak_strb_str(&kb, hash);
	ak_str *key = ak_strb_finish(&kb);
	ak_bytes *v;
	if (hash_get(key, &v)) {
		ak_release(key);
		return ak_retain(v);
	}
	v = NULL;
	uint8_t *raw;
	size_t n;
	if (ak_thumbhash_parse(hash->data, hash->len, &raw, &n)) {
		int w, h;
		uint8_t *px;
		if (ak_thumbhash(raw, n, &w, &h, &px)) {
			v = placeholder(w, h, px);
			ak_free(px);
		}
		ak_free(raw);
	}
	hash_put(key, v);
	ak_release(key);
	return v;
}

static void send_placeholder(ak_view *e, ak_bytes *b) {
	ak_batch_set_bytes(ak_view_batch(e), e->id, AK_PROP_IMAGE_PLACEHOLDER_IMAGE, b ? b->data : NULL, b ? b->len : 0);
	ak_release(b);
}

// send_blurhash sends a BlurHash's picture, decoded at the style's aspect
// ratio (taken once the keys are set): decoded at the image's shape, it
// fills the box as the image will; without one, stretched to it.
static void send_blurhash(ak_view *e, const ak_str *hash) {
	float ratio = ak_style_float(e, AK_KEY_ASPECT_RATIO, 0);
	bool stretch = ak_style_int(e, AK_KEY_IMAGE_PLACEHOLDER_MODE, 0) == 0;
	ak_batch *b = ak_view_batch(e);
	if (ratio > 0 && stretch) ak_batch_set_enum(b, e->id, AK_PROP_IMAGE_PLACEHOLDER_MODE, (int32_t)ak_style_int(e, AK_KEY_IMAGE_MODE, 0));
	else if (stretch) ak_batch_set_enum(b, e->id, AK_PROP_IMAGE_PLACEHOLDER_MODE, AK_IMAGE_MODE_STRETCH);
	send_placeholder(e, blurhash_image(hash, ratio));
}

static void image_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	ak_batch *b = ak_view_batch(e);
	switch (key) {
	case AK_KEY_IMAGE_SOURCE: send_str(e, AK_PROP_IMAGE_SOURCE, v); break;
	case AK_KEY_IMAGE_MODE:
		if (prev || v.i) ak_batch_set_enum(b, e->id, AK_PROP_IMAGE_MODE, (int32_t)v.i);
		break;
	case AK_KEY_IMAGE_PLACEHOLDER: send_str(e, AK_PROP_IMAGE_PLACEHOLDER, v); break;
	case AK_KEY_IMAGE_TRANSITION:
		if (prev || v.f != 0) ak_batch_set_float(b, e->id, AK_PROP_IMAGE_TRANSITION, v.f);
		break;
	case AK_KEY_IMAGE_PLACEHOLDER_MODE:
		// asImage: as the image fills its box; the others are its modes.
		if (v.i) ak_batch_set_enum(b, e->id, AK_PROP_IMAGE_PLACEHOLDER_MODE, (int32_t)v.i - 1);
		else if (prev) ak_batch_set_enum(b, e->id, AK_PROP_IMAGE_PLACEHOLDER_MODE, (int32_t)ak_style_int(e, AK_KEY_IMAGE_MODE, 0));
		break;
	case AK_KEY_IMAGE_THUMB_HASH:
		if (prev || (v.p && ((ak_str *)v.p)->len)) send_placeholder(e, thumbhash_image(v.p));
		break;
	case AK_KEY_IMAGE_BLUR_HASH:
		if (e->finalized) send_blurhash(e, v.p);  // else once the style is set
		break;
	case AK_KEY_IMAGE_ANIMATED: {
		const ak_box *box = v.p;
		if (box) ak_batch_set_bool(b, e->id, AK_PROP_IMAGE_PAUSED, !box->v.i);
		break;
	}
	}
}

static void image_finalize(ak_view *e) {
	const ak_val *bh = key_val(e, AK_KEY_IMAGE_BLUR_HASH);
	if (bh && bh->p && ((ak_str *)bh->p)->len) send_blurhash(e, bh->p);
	// Without animated, an image that turns out to have frames follows
	// Reduce Motion: it says so with Animated.
	const ak_val *an = key_val(e, AK_KEY_IMAGE_ANIMATED);
	if (!an || !an->p) ak_view_listen(e, AK_EVENT_ANIMATED);
	// A labelled image is one to screen readers.
	const ak_val *label = key_val(e, AK_KEY_LABEL);
	if (label && label->p && ((ak_str *)label->p)->len && !key_val(e, AK_KEY_ROLE)) ak_batch_set_enum(ak_view_batch(e), e->id, AK_PROP_A11Y_ROLE, AK_ROLE_IMAGE);
}

// Following Reduce Motion: an effect owned by what owns the image.
static void motion_run(void *ctx) {
	ak_view *e = ctx;
	if (e->dead) return;
	ak_signal_read(&e->r->env_node);
	image_state *s = e->impl;
	bool v = e->r->env.reduce_motion;
	const ak_val *an = key_val(e, AK_KEY_IMAGE_ANIMATED);
	if (an && an->p) return;  // the app's
	if (v != s->paused) {
		s->paused = v;
		ak_batch_set_bool(ak_view_batch(e), e->id, AK_PROP_IMAGE_PAUSED, v);
	}
}

static void motion_drop(void *ctx) { ak_release(ctx); }

static const ak_comp_info motion_info = {.drop = motion_drop, .label = "Image.animated"};

static void motion_start(void *ctx) { ak_effect_new(motion_run, ak_retain(ctx), AK_EFFECT_RENDER, &motion_info); }

static void image_event(ak_view *e, const ak_event *ev) {
	image_state *s = e->impl;
	if (ev->kind == AK_EVENT_LOADED) {
		ak_lsize n = {(float)ev->x, (float)ev->y};
		if (n.w != s->natural.w || n.h != s->natural.h) {
			s->natural = n;
			ak_view_remeasure(e);
		}
		ak_val args[2] = {{.f = ev->x}, {.f = ev->y}};
		ak_view_call(e, AK_EV_IMAGE_LOADED, args, 2);
	} else if (ev->kind == AK_EVENT_FAILED) {
		call_str(e, AK_EV_IMAGE_ERROR, ev->s);
	} else if (ev->kind == AK_EVENT_ANIMATED) {
		if (s->watching) return;
		s->watching = true;
		if (e->owner && !ak_owner_disposed(e->owner)) ak_with_owner(e->owner, motion_start, e);
	}
}

const ak_el_class ak_class_image = {.name = "Image", .kind = AK_KIND_IMAGE, .init = image_init, .set = image_set,
                                    .finalize = image_finalize, .event = image_event, .drop = free_impl};

// ---- Video ----
//
// The host reports a Video's playback with VideoState (its flags, the
// time, the duration, the rate chosen, the natural size) and its tracks
// with VideoTracks; the core works out what the app hears from them: the
// status (VideoStatus), loaded once per source, the time every
// timeInterval while it plays (the host sends it about that often, with
// TimeInterval, only once something listens to it or reads it), a choice
// in the platform's controls (a speed, a subtitle, an audio track) only
// when it's the user's, not the app's echoed back.

// VideoStatus's cases (types.asuka).
enum { VS_IDLE, VS_LOADING, VS_READY, VS_PLAYING, VS_BUFFERING, VS_PAUSED, VS_ENDED, VS_FAILED };

// VideoState's flags (ops/video.go).
enum { VF_READY = 1, VF_PLAYING = 2, VF_BUFFERING = 4, VF_ENDED = 8, VF_PIP = 16, VF_FULL = 32 };

typedef struct {
	ak_lsize natural;  // first: natural_measure reads it
	ak_node time_sig, dur_sig, status_sig;
	double time, duration;  // duration NaN: not known
	double tick;            // the time timeUpdate was last called with (NaN: none since the source)
	float rate;             // the host's, as last reported
	uint8_t flags;          // the host's VideoState flags, as last reported
	uint8_t status, told;  // the status, and as the handler was last told it
	bool source;    // a source is set
	bool loaded;    // loaded was called for the source
	bool played;    // it played since the source loaded
	bool failed;    // Failed since the source
	bool ticking;   // TimeInterval was sent
	bool rate_known, sub_known, audio_known, tracks_known;
	ak_str *sub, *audio;  // the host's choices, as last reported
	ak_buf tracks;        // the tracks last reported (their kinds, ids, labels and languages)
} video_state;

static video_state *video_of(ak_view *v) { return v && v->kind == AK_KIND_VIDEO && !v->dead && v->impl ? v->impl : NULL; }

static void video_init(ak_view *e) {
	video_state *s = ak_alloc(sizeof(video_state));
	memset(s, 0, sizeof *s);
	ak_signal_init(&s->time_sig);
	ak_signal_init(&s->dur_sig);
	ak_signal_init(&s->status_sig);
	s->duration = s->tick = NAN;
	s->rate = 1;
	e->impl = s;
	ak_lnode_set_measure(e->box, natural_measure, e);
	ak_view_listen(e, AK_EVENT_LOADED);
	ak_view_listen(e, AK_EVENT_VIDEO_STATE);
	ak_view_listen(e, AK_EVENT_VIDEO_TRACKS);
}

static void video_drop(ak_view *e) {
	video_state *s = e->impl;
	if (!s) return;
	ak_signal_drop(&s->time_sig);
	ak_signal_drop(&s->dur_sig);
	ak_signal_drop(&s->status_sig);
	ak_release(s->sub);
	ak_release(s->audio);
	ak_buf_free(&s->tracks);
	ak_free(s);
	e->impl = NULL;
}

static uint8_t video_derive(const video_state *s) {
	if (!s->source) return VS_IDLE;
	if (s->failed) return VS_FAILED;
	if (!(s->flags & VF_READY)) return VS_LOADING;
	if (s->flags & VF_ENDED) return VS_ENDED;
	if (s->flags & VF_PLAYING) return s->flags & VF_BUFFERING ? VS_BUFFERING : VS_PLAYING;
	return s->played ? VS_PAUSED : VS_READY;
}

// video_status works the status out again: a change is written, and
// with tell (an event's, not a prop's: no handler runs as props apply)
// told.
static void video_status(ak_view *e, video_state *s, bool tell) {
	uint8_t st = video_derive(s);
	if (st != s->status) {
		s->status = st;
		ak_signal_write(&s->status_sig);
	}
	if (!tell || st == s->told) return;
	s->told = st;
	ak_val arg = {.i = st};
	ak_view_call(e, AK_EV_VIDEO_STATUS, &arg, 1);
}

static double video_interval(const ak_view *e) {
	const ak_val *v = key_val(e, AK_KEY_VIDEO_TIME_INTERVAL);
	return v ? v->f : 0.25;
}

// video_tick has the host report the time while it plays, once
// something hears it (timeUpdate) or reads it (currentTime).
static void video_tick(ak_view *e, video_state *s) {
	if (s->ticking) return;
	s->ticking = true;
	double t = video_interval(e);
	ak_batch_set_float(ak_view_batch(e), e->id, AK_PROP_VIDEO_TIME_INTERVAL, t > 0 ? t : 0.25);
}

// A new source: nothing of the last one's playback holds (what the
// handlers hear of it, at the host's first report).
static void video_reset(ak_view *e, video_state *s, bool source) {
	s->source = source;
	s->loaded = s->played = s->failed = false;
	s->tracks_known = s->sub_known = s->audio_known = false;
	s->flags = 0;
	s->tick = NAN;
	if (s->time != 0) {
		s->time = 0;
		ak_signal_write(&s->time_sig);
	}
	if (!ak_isnan((float)s->duration)) {
		s->duration = NAN;
		ak_signal_write(&s->dur_sig);
	}
	video_status(e, s, false);
}

static void video_source(ak_view *e, ak_val v) {
	const ak_str *src = v.p;
	send_str(e, AK_PROP_VIDEO_SOURCE, v);
	video_reset(e, e->impl, src && src->len);
}

// send_float sends a float prop that's not its default, or is again.
static void send_float(ak_view *e, ak_prop p, double v, double def, const ak_val *prev) {
	if (prev || v != def) ak_batch_set_float(ak_view_batch(e), e->id, p, v);
}

// video_choice sends a track's choice, unless it's what the host shows.
static void video_choice(ak_view *e, ak_prop p, ak_val v, bool known, const ak_str *shown) {
	const ak_str *s = v.p;
	if (known && ak_span_eq(ak_span_str(s), ak_span_str(shown))) return;
	send_str(e, p, v);
}

static void video_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	ak_batch *b = ak_view_batch(e);
	video_state *s = e->impl;
	ak_prop flag = 0;
	switch (key) {
	// The source goes once what applies to it has: its headers, where it
	// starts, its subtitle files.
	case AK_KEY_VIDEO_SOURCE:
		if (e->finalized) video_source(e, v);
		return;
	case AK_KEY_VIDEO_MODE:
		if (prev || v.i) ak_batch_set_enum(b, e->id, AK_PROP_VIDEO_MODE, (int32_t)v.i);
		return;
	case AK_KEY_VIDEO_VOLUME: send_float(e, AK_PROP_VIDEO_VOLUME, v.f, 1, prev); return;
	case AK_KEY_VIDEO_RATE:
		if (s->rate_known && v.f == s->rate) return;  // the host's already (the user's choice, written back)
		send_float(e, AK_PROP_VIDEO_RATE, v.f, 1, prev);
		return;
	case AK_KEY_VIDEO_START_AT: send_float(e, AK_PROP_VIDEO_START_AT, v.f, 0, prev); return;
	case AK_KEY_VIDEO_POSTER:
		if (prev || (v.p && ((ak_str *)v.p)->len)) send_str(e, AK_PROP_VIDEO_POSTER, v);
		return;
	case AK_KEY_VIDEO_TIME_INTERVAL:
		if (s->ticking) ak_batch_set_float(b, e->id, AK_PROP_VIDEO_TIME_INTERVAL, v.f > 0 ? v.f : 0.25);
		return;
	case AK_KEY_VIDEO_HEADERS: {
		const ak_map *m = v.p;
		if (!prev && (!m || !m->live)) return;
		ak_buf *w = ak_batch_set_bytes_begin(b, e->id, AK_PROP_VIDEO_HEADERS);
		ak_arr *keys = m ? ak_map_keys(m) : NULL;
		ak_buf_u32(w, keys ? keys->len * 2 : 0);
		for (uint32_t i = 0; keys && i < keys->len; i++) {
			ak_val k = ak_arr_vals(keys)[i], val = {0};
			ak_map_get(m, k, &val);
			ak_span ks = ak_span_str(k.p), vs = ak_span_str(val.p);
			ak_buf_str(w, ks.p, ks.len);
			ak_buf_str(w, vs.p, vs.len);
		}
		ak_release(keys);
		ak_batch_set_bytes_end(b);
		return;
	}
	case AK_KEY_VIDEO_SUBTITLE_FILES: {
		const ak_arr *a = v.p;
		if (!prev && (!a || !a->len)) return;
		ak_buf *w = ak_batch_set_bytes_begin(b, e->id, AK_PROP_VIDEO_SUBTITLE_FILES);
		ak_buf_u32(w, a ? a->len * 3 : 0);  // the strings: three a file
		for (uint32_t i = 0; a && i < a->len; i++) {
			const ak_struct *f = ak_arr_vals(a)[i].p;
			for (int j = 0; j < 3; j++) {
				ak_span x = ak_span_str(f ? f->f[j].p : NULL);
				ak_buf_str(w, x.p, x.len);
			}
		}
		ak_batch_set_bytes_end(b);
		return;
	}
	case AK_KEY_VIDEO_SUBTITLE: video_choice(e, AK_PROP_VIDEO_SUBTITLE, v, s->sub_known, s->sub); return;
	case AK_KEY_VIDEO_AUDIO_TRACK: video_choice(e, AK_PROP_VIDEO_AUDIO_TRACK, v, s->audio_known, s->audio); return;
	case AK_KEY_VIDEO_HIDE_CONTROLS: flag = AK_PROP_VIDEO_HIDE_CONTROLS; break;
	case AK_KEY_VIDEO_AUTO_PLAY: flag = AK_PROP_VIDEO_AUTO_PLAY; break;
	case AK_KEY_VIDEO_LOOP: flag = AK_PROP_VIDEO_LOOP; break;
	case AK_KEY_VIDEO_MUTED: flag = AK_PROP_VIDEO_MUTED; break;
	case AK_KEY_VIDEO_PICTURE_IN_PICTURE: flag = AK_PROP_VIDEO_PICTURE_IN_PICTURE; break;
	case AK_KEY_VIDEO_AUTO_PICTURE_IN_PICTURE: flag = AK_PROP_VIDEO_AUTO_PICTURE_IN_PICTURE; break;
	}
	if (flag && (prev || v.i)) ak_batch_set_bool(b, e->id, flag, v.i != 0);
}

static void video_finalize(ak_view *e) {
	video_state *s = e->impl;
	if (ak_view_handler(e, AK_EV_VIDEO_TIME_UPDATE)) video_tick(e, s);
	const ak_val *src = key_val(e, AK_KEY_VIDEO_SOURCE);
	if (src) video_source(e, *src);
}

// A little-endian reader of an event's bytes.
static ak_str *read_str(ak_reader *r) { return ak_str_span(ak_read_str(r)); }

static bool same_str(ak_span a, const ak_str *b) { return ak_span_eq(a, ak_span_str(b)); }

// video_chosen takes the host's choice of a track (which): told when it's
// the user's, not what the app chose (key) coming back.
static void video_chosen(ak_view *e, ak_span id, ak_str **shown, bool *known, uint16_t key, uint16_t ev) {
	bool changed = !*known || !same_str(id, *shown);
	bool was_known = *known;
	if (changed) {
		ak_release(*shown);
		*shown = ak_str_span(id);
		*known = true;
	}
	if (!changed || !was_known) return;
	const ak_val *app = key_val(e, key);
	if (app && same_str(id, app->p)) return;
	if (!ak_view_handler(e, ev)) return;
	ak_val arg = {.p = ak_retain(*shown)};
	ak_view_call(e, ev, &arg, 1);
	ak_release(arg.p);
}

static void video_tracks(ak_view *e, video_state *s, ak_span payload) {
	ak_reader r = {(const uint8_t *)payload.p, payload.len, 0, false};
	uint32_t n = ak_read_u32(&r);
	ak_span sub = {"", 0}, audio = {"", 0};
	bool has_audio = false;
	// What the tracks are, apart from which is chosen: told on a change.
	ak_buf key = {0};
	for (uint32_t i = 0; i < n && !r.err; i++) {
		uint8_t kind = ak_read_u8(&r), sel = ak_read_u8(&r);
		ak_span id = ak_read_str(&r), label = ak_read_str(&r), lang = ak_read_str(&r);
		if (r.err) break;
		ak_buf_u8(&key, kind);
		ak_buf_str(&key, id.p, id.len);
		ak_buf_str(&key, label.p, label.len);
		ak_buf_str(&key, lang.p, lang.len);
		if (sel && kind == 0) sub = id;
		if (sel && kind == 1) audio = id, has_audio = true;
	}
	if (r.err) {
		ak_buf_free(&key);
		return;
	}
	bool changed = !s->tracks_known || key.len != s->tracks.len || (key.len && memcmp(key.data, s->tracks.data, key.len) != 0);
	s->tracks_known = true;
	if (changed) {
		ak_buf_free(&s->tracks);
		s->tracks = key;
	} else {
		ak_buf_free(&key);
	}
	if (changed && ak_view_handler(e, AK_EV_VIDEO_TRACKS)) {
		ak_arr *subs = ak_arr_new(AK_K_REF, &ak_ui_shape_video_track, 0), *auds = ak_arr_new(AK_K_REF, &ak_ui_shape_video_track, 0);
		r = (ak_reader){(const uint8_t *)payload.p, payload.len, 0, false};
		n = ak_read_u32(&r);
		for (uint32_t i = 0; i < n; i++) {
			uint8_t kind = ak_read_u8(&r);
			ak_read_u8(&r);
			ak_struct *t = ak_struct_new(&ak_ui_shape_video_track);
			t->f[0].p = read_str(&r);
			t->f[1].p = read_str(&r);
			t->f[2].p = read_str(&r);
			ak_arr_push(kind == 1 ? &auds : &subs, (ak_val){.p = t});
		}
		ak_val args[2] = {{.p = subs}, {.p = auds}};
		ak_view_call(e, AK_EV_VIDEO_TRACKS, args, 2);
		ak_release(subs);
		ak_release(auds);
	}
	video_chosen(e, sub, &s->sub, &s->sub_known, AK_KEY_VIDEO_SUBTITLE, AK_EV_VIDEO_SUBTITLE_CHANGE);
	if (has_audio) video_chosen(e, audio, &s->audio, &s->audio_known, AK_KEY_VIDEO_AUDIO_TRACK, AK_EV_VIDEO_AUDIO_TRACK_CHANGE);
}

static void call_bool(ak_view *e, uint16_t ev, bool on) {
	ak_val arg = {.i = on};
	ak_view_call(e, ev, &arg, 1);
}

static void video_state_event(ak_view *e, video_state *s, ak_span payload) {
	ak_reader r = {(const uint8_t *)payload.p, payload.len, 0, false};
	uint8_t flags = ak_read_u8(&r);
	double t = ak_read_f64(&r), dur = ak_read_f64(&r);
	float rate = ak_read_f32(&r), w = ak_read_f32(&r), h = ak_read_f32(&r);
	if (r.err || !s->source) return;
	uint8_t was = s->flags;
	s->flags = flags;
	if ((flags & VF_READY) && (flags & VF_PLAYING)) s->played = true;
	if (t != s->time) {
		s->time = t;
		ak_signal_write(&s->time_sig);
	}
	if (dur != s->duration && !(dur != dur && s->duration != s->duration)) {
		s->duration = dur;
		ak_signal_write(&s->dur_sig);
	}
	if (!s->loaded && (flags & VF_READY)) {
		s->loaded = true;
		ak_val args[3] = {{.f = dur == dur ? dur : 0}, {.f = w}, {.f = h}};
		ak_view_call(e, AK_EV_VIDEO_LOADED, args, 3);
	}
	video_status(e, s, true);
	if (!s->rate_known || rate != s->rate) {
		bool user = s->rate_known;
		s->rate = rate;
		s->rate_known = true;
		const ak_val *app = key_val(e, AK_KEY_VIDEO_RATE);
		double want = app ? app->f : 1;
		if (user && (float)want != rate) {
			ak_val arg = {.f = rate};
			ak_view_call(e, AK_EV_VIDEO_RATE_CHANGE, &arg, 1);
		}
	}
	if ((flags ^ was) & VF_PIP) call_bool(e, AK_EV_VIDEO_PICTURE_IN_PICTURE_CHANGE, flags & VF_PIP);
	if ((flags ^ was) & VF_FULL) call_bool(e, AK_EV_VIDEO_FULL_SCREEN_CHANGE, flags & VF_FULL);
	// The time, at most every timeInterval (half of it, in the video's
	// time, as a host's timer drifts) while it plays; any change while it
	// doesn't (a seek), and as it starts.
	if (!(flags & VF_READY) || t == s->tick || !ak_view_handler(e, AK_EV_VIDEO_TIME_UPDATE)) return;
	bool playing = (flags & VF_PLAYING) && !(flags & VF_BUFFERING);
	if (playing && (was & VF_PLAYING) && s->tick == s->tick) {
		double d = t - s->tick, step = 0.5 * video_interval(e) * (rate > 0 ? rate : 1);
		if (d >= 0 && d < step) return;
	}
	s->tick = t;
	ak_val arg = {.f = t};
	ak_view_call(e, AK_EV_VIDEO_TIME_UPDATE, &arg, 1);
}

static void video_event(ak_view *e, const ak_event *ev) {
	video_state *s = e->impl;
	switch (ev->kind) {
	case AK_EVENT_LOADED: {
		ak_lsize n = {(float)ev->x, (float)ev->y};
		if (n.w != s->natural.w || n.h != s->natural.h) {
			s->natural = n;
			ak_view_remeasure(e);
		}
		break;
	}
	case AK_EVENT_ENDED: ak_view_call(e, AK_EV_VIDEO_END, NULL, 0); break;
	case AK_EVENT_FAILED:
		if (s->source && !s->failed) {
			s->failed = true;
			video_status(e, s, true);
		}
		call_str(e, AK_EV_VIDEO_ERROR, ev->s);
		break;
	case AK_EVENT_VIDEO_STATE: video_state_event(e, s, ev->s); break;
	case AK_EVENT_VIDEO_TRACKS: video_tracks(e, s, ev->s); break;
	}
}

const ak_el_class ak_class_video = {.name = "Video", .kind = AK_KIND_VIDEO, .init = video_init, .set = video_set, .finalize = video_finalize,
                                    .event = video_event, .drop = video_drop};

static void video_command(ak_view *v, ak_cmd c, const void *args, size_t n) {
	if (video_of(v)) ak_batch_command(ak_view_batch(v), v->id, c, args, n);
}

void ak_ui_video_play(ak_view *v) { video_command(v, AK_CMD_PLAY, NULL, 0); }
void ak_ui_video_pause(ak_view *v) { video_command(v, AK_CMD_PAUSE, NULL, 0); }

// ak_ui_video_seek moves a Video to seconds from its start.
void ak_ui_video_seek(ak_view *v, double seconds) {
	uint8_t b[8];
	uint64_t bits;
	memcpy(&bits, &seconds, 8);
	for (int i = 0; i < 8; i++) b[i] = (uint8_t)(bits >> (8 * i));
	video_command(v, AK_CMD_SEEK, b, 8);
}

// ak_ui_video_action sends a VideoAction: picture in picture, full
// screen, in or out.
void ak_ui_video_action(ak_view *v, int64_t action) {
	uint8_t b = (uint8_t)action;
	if (action >= 0 && action < AK_VIDEO_ACTION_COUNT) video_command(v, AK_CMD_VIDEO_ACTION, &b, 1);
}

// ak_ui_video_time is a Video's time (which 0) or duration (1), in
// seconds, tracked; 0 for none (or a duration not known yet). The first
// read of the time has the host report it as it plays.
double ak_ui_video_time(ak_view *v, int64_t which) {
	video_state *s = video_of(v);
	if (!s) return 0;
	if (which == 1) {
		ak_signal_read(&s->dur_sig);
		return s->duration == s->duration ? s->duration : 0;
	}
	ak_signal_read(&s->time_sig);
	video_tick(v, s);
	return s->time;
}

// ak_ui_video_status is a Video's VideoStatus, tracked (idle for none).
int64_t ak_ui_video_status(ak_view *v) {
	video_state *s = video_of(v);
	if (!s) return VS_IDLE;
	ak_signal_read(&s->status_sig);
	return s->status;
}

// ---- WebView ----

// A WebView grows into its room, as a List does, with no height and no
// grow of its own.
static void web_adjust(ak_view *e, ak_lstyle *ls) {
	AK_UNUSED(e);
	if (ak_llength_is_auto(ls->height) && ls->flex_grow == 0) ls->flex_grow = 1;
}

static void web_init(ak_view *e) { ak_style_relayout(e); }

static void web_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	switch (key) {
	// The page goes once its options are set: they apply to it.
	case AK_KEY_WEB_VIEW_URL:
		if (e->finalized) send_str(e, AK_PROP_WEB_VIEW_URL, v);
		break;
	case AK_KEY_WEB_VIEW_HTML:
		if (e->finalized) send_str(e, AK_PROP_WEB_VIEW_HTML, v);
		break;
	case AK_KEY_WEB_VIEW_PRELOAD:
		if (prev || (v.p && ((ak_str *)v.p)->len)) send_str(e, AK_PROP_WEB_VIEW_PRELOAD, v);
		break;
	case AK_KEY_WEB_VIEW_USER_AGENT:
		if (prev || (v.p && ((ak_str *)v.p)->len)) send_str(e, AK_PROP_WEB_VIEW_USER_AGENT, v);
		break;
	case AK_KEY_WEB_VIEW_BLOCK: {
		const ak_arr *a = v.p;
		if (!prev && (!a || !a->len)) break;
		ak_buf *b = ak_batch_set_bytes_begin(ak_view_batch(e), e->id, AK_PROP_WEB_VIEW_BLOCK);
		ak_encode_strings(b, a);
		ak_batch_set_bytes_end(ak_view_batch(e));
		break;
	}
	}
}

static void web_finalize(ak_view *e) {
	const ak_val *html = key_val(e, AK_KEY_WEB_VIEW_HTML), *url = key_val(e, AK_KEY_WEB_VIEW_URL);
	if (html) send_str(e, AK_PROP_WEB_VIEW_HTML, *html);
	if (url) send_str(e, AK_PROP_WEB_VIEW_URL, *url);
}

static void web_event(ak_view *e, const ak_event *ev) {
	switch (ev->kind) {
	case AK_EVENT_NAVIGATED: call_str(e, AK_EV_WEB_VIEW_LOADED, ev->s); break;
	case AK_EVENT_FAILED: call_str(e, AK_EV_WEB_VIEW_ERROR, ev->s); break;
	case AK_EVENT_MESSAGE: call_str(e, AK_EV_WEB_VIEW_MESSAGE, ev->s); break;
	case AK_EVENT_REQUEST: call_str(e, AK_EV_WEB_VIEW_REQUEST, ev->s); break;
	case AK_EVENT_NEW_WINDOW: call_str(e, AK_EV_WEB_VIEW_NEW_WINDOW, ev->s); break;
	case AK_EVENT_WEB_STATE: {
		// string URL, string title, u8 flags (1 loading, 2 back, 4
		// forward), f32 progress.
		ak_reader r = {(const uint8_t *)ev->s.p, ev->s.len, 0, false};
		ak_struct *s = ak_struct_new(&ak_ui_shape_web_state);
		s->f[0].p = read_str(&r);
		s->f[1].p = read_str(&r);
		uint8_t f = ak_read_u8(&r);
		s->f[2].i = (f & 1) != 0;
		s->f[3].f = ak_read_f32(&r);
		s->f[4].i = (f & 2) != 0;
		s->f[5].i = (f & 4) != 0;
		if (!r.err) {
			ak_val arg = {.p = s};
			ak_view_call(e, AK_EV_WEB_VIEW_STATE, &arg, 1);
		}
		ak_release(s);
		break;
	}
	case AK_EVENT_DOWNLOAD: {
		// string URL, MIME, filename, u64 size.
		ak_reader r = {(const uint8_t *)ev->s.p, ev->s.len, 0, false};
		ak_struct *d = ak_struct_new(&ak_ui_shape_web_download);
		d->f[0].p = read_str(&r);
		d->f[1].p = read_str(&r);
		d->f[2].p = read_str(&r);
		d->f[3].i = (int64_t)ak_read_u64(&r);
		if (!r.err) {
			ak_val arg = {.p = d};
			ak_view_call(e, AK_EV_WEB_VIEW_DOWNLOAD, &arg, 1);
		}
		ak_release(d);
		break;
	}
	}
}

const ak_el_class ak_class_web = {.name = "WebView", .kind = AK_KIND_WEB_VIEW, .init = web_init, .set = web_set, .finalize = web_finalize,
                                  .event = web_event, .adjust = web_adjust};

static bool is_web(const ak_view *v) { return v && v->kind == AK_KIND_WEB_VIEW && !v->dead; }

// ak_ui_web_eval runs script in a WebView's page (its result isn't
// returned: have it post one, or use ak_ui_web_eval_result).
void ak_ui_web_eval(ak_view *v, ak_str *script) {
	if (is_web(v)) ak_batch_command(ak_view_batch(v), v->id, AK_CMD_EVAL, script ? script->data : "", script ? script->len : 0);
}

// ak_ui_web_navigate moves its page: 0 back, 1 forward, 2 reload, 3 stop.
void ak_ui_web_navigate(ak_view *v, int64_t to) {
	uint8_t b = (uint8_t)to;
	if (is_web(v) && to >= 0 && to <= 3) ak_batch_command(ak_view_batch(v), v->id, AK_CMD_WEB_NAVIGATE, &b, 1);
}

// ak_ui_web_load loads url in the view now: its URL prop sent again, as
// it may be the one the view was given, which its page has left since
// (a link followed), and which a binding doesn't send twice.
void ak_ui_web_load(ak_view *v, ak_str *url) {
	if (is_web(v) && url) send_str(v, AK_PROP_WEB_VIEW_URL, (ak_val){.p = url});
}

// ak_ui_web_eval_result runs script as eval does, and is a future of its
// value as JSON (the last statement's, or what its promise resolves to),
// rejected with what it threw.
ak_future *ak_ui_web_eval_result(ak_view *v, ak_str *script) {
	if (!is_web(v)) {
		ak_future *f = ak_future_new(AK_K_REF);
		ak_str *m = ak_str_c("evalResult: not a WebView");
		ak_struct *err = ak_struct_new(&ak_shape_error);
		err->f[0].p = m;
		ak_future_reject(f, err);
		return f;
	}
	ak_buf b = {0};
	ak_buf_u32(&b, v->id);
	if (script) ak_buf_put(&b, script->data, script->len);
	ak_future *f = ak_ext_call_future(AK_STR("web"), AK_STR("eval"), b.data, b.len);
	ak_buf_free(&b);
	return f;
}

// ak_ui_web_cookies is a future of the cookies the WebViews' store has
// for url, as a Cookie header.
ak_future *ak_ui_web_cookies(ak_str *url) {
	return ak_ext_call_future(AK_STR("web"), AK_STR("cookies"), url ? url->data : "", url ? url->len : 0);
}

// ---- Native views ----

// Values on the wire (native/native.go's): a typed view's (a native
// component's) props and events are bytes, its values encoded in the
// native codec.
enum { NATIVE_BYTES = 5 };

typedef struct {
	ak_str *name;
	ak_buf sent;  // the value as last encoded
	bool pending;  // not sent yet: before finalize, listens go first
} native_prop;

typedef struct {
	ak_str *name;
	ak_closure *handler;
	const char *desc;
	const ak_json_types *types;
} native_handler;

typedef struct {
	AK_VEC(native_prop) props;
	AK_VEC(native_handler) handlers;
} native_state;

static void native_init(ak_view *e) {
	e->impl = ak_alloc(sizeof(native_state));
	ak_view_measure(e);
}

static void native_drop(ak_view *e) {
	native_state *s = e->impl;
	for (uint32_t i = 0; i < s->props.len; i++) {
		ak_release(s->props.data[i].name);
		ak_buf_free(&s->props.data[i].sent);
	}
	ak_vec_free(&s->props);
	for (uint32_t i = 0; i < s->handlers.len; i++) {
		ak_release(s->handlers.data[i].name);
		ak_release(s->handlers.data[i].handler);
	}
	ak_vec_free(&s->handlers);
	ak_free(s);
}

static void native_send(ak_view *e, native_prop *p) {
	ak_buf *b = ak_batch_set_bytes_begin(ak_view_batch(e), e->id, AK_PROP_NATIVE_PROP);
	ak_buf_str(b, p->name->data, p->name->len);
	ak_buf_u8(b, NATIVE_BYTES);
	ak_buf_u32(b, (uint32_t)p->sent.len);
	ak_buf_put(b, p->sent.data, p->sent.len);
	ak_batch_set_bytes_end(ak_view_batch(e));
	p->pending = false;
	ak_view_remeasure(e);
}

static void native_finalize(ak_view *e) {
	native_state *s = e->impl;
	for (uint32_t i = 0; i < s->props.len; i++)
		if (s->props.data[i].pending) native_send(e, &s->props.data[i]);
}

static native_handler *find_handler(native_state *s, ak_span name) {
	for (uint32_t i = 0; i < s->handlers.len; i++)
		if (ak_span_eq(ak_span_str(s->handlers.data[i].name), name)) return &s->handlers.data[i];
	return NULL;
}

// A Native event: string name, u32 count, per value a string key, u8
// type, the value; a typed view's has one, "v", its parameters encoded.
// Each handler of the name runs, in the order they were given (a bind's
// write, then the app's on).
static void native_event(ak_view *e, const ak_event *ev) {
	if (ev->kind != AK_EVENT_NATIVE) return;
	native_state *s = e->impl;
	ak_reader r = {(const uint8_t *)ev->s.p, ev->s.len, 0, false};
	ak_span name = ak_read_str(&r);
	if (!find_handler(s, name) || r.err) return;
	uint32_t n = ak_read_u32(&r);
	ak_span v = {0};
	for (uint32_t i = 0; i < n && !r.err; i++) {
		ak_span k = ak_read_str(&r);
		uint8_t t = ak_read_u8(&r);
		switch (t) {
		case 1: ak_read_u8(&r); break;
		case 2:
		case 3: ak_read_u64(&r); break;
		case 4:
		case NATIVE_BYTES: {
			ak_span x = ak_read_str(&r);
			if (t == NATIVE_BYTES && ak_span_eqc(k, "v")) v = x;
			break;
		}
		default: return;
		}
	}
	if (r.err) return;
	// The handlers, held: one may dispose the view (and its state).
	uint32_t nh = 0;
	native_handler *hs = ak_alloc(s->handlers.len * sizeof *hs);
	for (uint32_t i = 0; i < s->handlers.len; i++)
		if (ak_span_eq(ak_span_str(s->handlers.data[i].name), name)) {
			hs[nh] = s->handlers.data[i];
			ak_retain(hs[nh++].handler);
		}
	ak_retain(e);
	for (uint32_t k = 0; k < nh; k++) {
		// The parameters, each decoded by its descriptor in turn.
		const char *d = hs[k].desc;
		ak_val args[16];
		ak_kind kinds[16];
		uint32_t nargs = 0;
		ak_reader vr = {(const uint8_t *)v.p, v.len, 0, false};
		while (d && *d && nargs < AK_LEN(args)) {
			kinds[nargs] = (*d == 'i' || *d == 'e') ? AK_K_INT : *d == 'f' ? AK_K_FLOAT : *d == 'b' ? AK_K_BOOL : AK_K_REF;
			args[nargs++] = ak_native_decode(&vr, d, hs[k].types);
			if (ak_error_pending()) break;
			d = ak_native_desc_skip(d);
		}
		if (ak_error_pending()) {
			ak_release(ak_error_take());  // a bad event: dropped, as Go's DecodeEvent
		} else {
			ak_call_closure(hs[k].handler, e->owner, args, nargs);
		}
		for (uint32_t i = 0; i < nargs; i++) ak_release_val(args[i], kinds[i]);
	}
	ak_release(e);
	for (uint32_t k = 0; k < nh; k++) ak_release(hs[k].handler);
	ak_free(hs);
}

const ak_el_class ak_class_native = {.name = "Native", .kind = AK_KIND_NATIVE, .flags = AK_ELF_MEASURED, .init = native_init,
                                     .finalize = native_finalize, .event = native_event, .drop = native_drop};

// ak_ui_native makes a native view of type: a native package's view, as
// its platform registers it ("<module class>#<View>").
ak_view *ak_ui_native(ak_str *type) {
	ak_ui_root *r = ak_ui_current();
	ak_view *e = ak_view_new(r, &ak_class_native, 0, true);
	ak_batch_set_string(r->batch, e->id, AK_PROP_NATIVE_TYPE, type ? type->data : "", type ? type->len : 0);
	ak_vec_push(&r->init_q, (ak_view *)ak_retain(e));
	return e;
}

static void check_native(const ak_view *v, const char *what) {
	if (!v || v->cls != &ak_class_native) ak_panic("ui.%s: not a native view", what);
}

// ak_ui_native_set sets a prop, encoded by its descriptor (rt/json.h's,
// with native.h's); sent only when its encoding changed.
void ak_ui_native_set(ak_view *v, ak_str *name, ak_val value, const char *desc, const ak_json_types *t) {
	check_native(v, "native_set");
	if (v->dead) return;
	native_state *s = v->impl;
	native_prop *p = NULL;
	for (uint32_t i = 0; i < s->props.len; i++)
		if (ak_str_eq(s->props.data[i].name, name)) p = &s->props.data[i];
	ak_buf enc = {0};
	ak_native_encode(&enc, value, desc, t);
	if (p && p->sent.len == enc.len && memcmp(p->sent.data, enc.data, enc.len) == 0) {
		ak_buf_free(&enc);
		return;
	}
	if (!p) {
		ak_vec_push(&s->props, ((native_prop){.name = ak_retain(name)}));
		p = &s->props.data[s->props.len - 1];
	}
	ak_buf_free(&p->sent);
	p->sent = enc;
	p->pending = true;
	if (v->finalized) native_send(v, p);
}

typedef struct {
	ak_view *v;
	ak_str *name;
	ak_closure *thunk;
	const char *desc;
	const ak_json_types *types;
	ak_comp_info info;
} native_binding;

static void nb_run(void *ctx) {
	native_binding *b = ctx;
	ak_val v = ak_call(b->thunk, NULL, 0);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		if (ak_is_cancel(err)) ak_release(err);
		else ak_raise(err);
		return;
	}
	ak_ui_native_set(b->v, b->name, v, b->desc, b->types);
	char c = *b->desc;
	if (!(c == 'i' || c == 'e' || c == 'f' || c == 'b')) ak_release(v.p);
}

static void nb_drop(void *ctx) {
	native_binding *b = ctx;
	ak_release(b->v);
	ak_release(b->name);
	ak_release(b->thunk);
	ak_free(b);
}

// ak_ui_native_bind makes a prop follow thunk.
void ak_ui_native_bind(ak_view *v, ak_str *name, ak_closure *thunk, const char *desc, const ak_json_types *t) {
	check_native(v, "native_bind");
	native_binding *b = ak_alloc(sizeof *b);
	b->v = ak_retain(v);
	b->name = ak_retain(name);
	b->thunk = ak_retain(thunk);
	b->desc = desc;
	b->types = t;
	b->info = (ak_comp_info){.drop = nb_drop, .label = "native prop"};
	ak_effect_new(nb_run, b, AK_EFFECT_RENDER, &b->info);
}

// ak_ui_native_on handles the event name: its parameters' descriptors,
// in order, are desc. The view listens from the first handler, before
// its props go.
void ak_ui_native_on(ak_view *v, ak_str *name, ak_closure *handler, const char *desc, const ak_json_types *t) {
	check_native(v, "native_on");
	if (!handler) return;
	native_state *s = v->impl;
	ak_retain(handler);
	ak_vec_push(&s->handlers, ((native_handler){ak_retain(name), handler, desc, t}));
	ak_view_listen(v, AK_EVENT_NATIVE);
}

// ---- Shapes ----

static const ak_shape *const no_shapes[8];
static const uint8_t web_state_kinds[] = {AK_K_REF, AK_K_REF, AK_K_BOOL, AK_K_FLOAT, AK_K_BOOL, AK_K_BOOL};
static const char *const web_state_names[] = {"url", "title", "loading", "progress", "canGoBack", "canGoForward"};
const ak_shape ak_ui_shape_web_state = {"WebState", 6, web_state_kinds, no_shapes, web_state_names, 0, NULL, false, NULL};
static const uint8_t download_kinds[] = {AK_K_REF, AK_K_REF, AK_K_REF, AK_K_INT};
static const char *const download_names[] = {"url", "mime", "filename", "size"};
const ak_shape ak_ui_shape_web_download = {"WebDownload", 4, download_kinds, no_shapes, download_names, 0, NULL, false, NULL};
static const uint8_t video_track_kinds[] = {AK_K_REF, AK_K_REF, AK_K_REF};
static const char *const video_track_names[] = {"id", "label", "language"};
const ak_shape ak_ui_shape_video_track = {"VideoTrack", 3, video_track_kinds, no_shapes, video_track_names, 0, NULL, false, NULL};

bool ak_ext_reduce_motion(ak_ui_root *r) { return r && r->env.reduce_motion; }

// ---- The extensions' classes ----

const ak_el_class *ak_ext_class_of(uint16_t el) {
	switch (el) {
	case AK_EL_RICH_TEXT: return &ak_class_rich;
	case AK_EL_SELECTABLE: return &ak_class_selectable;
	case AK_EL_CANVAS: return &ak_class_canvas;
	case AK_EL_SHAPE: return &ak_class_shape;
	case AK_EL_SHADER: return &ak_class_shader;
	case AK_EL_VIDEO: return &ak_class_video;
	case AK_EL_WEB_VIEW: return &ak_class_web;
	}
	return NULL;
}
