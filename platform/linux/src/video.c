// video.c: Video views. A Video is a GtkMediaFile (GTK's GStreamer
// backend) shown by a GtkVideo, with GTK's playback controls, or by a
// GtkPicture without them (HideControls). Mode is the picture's content
// fit. Loaded reports the natural size once the stream knows it, Ended
// each end (looping, it plays again), Failed the stream's error,
// VideoState its playback (as it changes, and every TimeInterval while it
// plays). GTK's media stream has no rate, no tracks, no headers and no
// picture in picture: a Video plays at 1×, reports no tracks, and stays in
// its window.
#include "host.h"

#include <math.h>
#include <stdlib.h>

typedef struct {
	GtkMediaStream *stream;  // the GtkMediaFile, the host's reference
	int mode;
	bool hide, auto_play, loop, muted, sized;
	bool play;     // play once prepared
	bool started;  // went to StartAt, once prepared
	bool told;     // VideoState was sent for the stream
	bool tracked;  // VideoTracks (none) was sent for the stream
	double volume, start_at, interval;
	guint tick;  // the TimeInterval's timer, while it plays
	uint8_t flags;
	double duration;
} video;

static video *video_of(node *n) { return n->kind == AK_KIND_VIDEO ? n->ext : NULL; }

// find_picture is w if it's a GtkPicture, else the first one in it.
static GtkWidget *find_picture(GtkWidget *w) {
	if (AK_IS(w, gtk_picture_get_type)) return w;
	for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
		GtkWidget *p = find_picture(c);
		if (p) return p;
	}
	return NULL;
}

// fit sets the picture's content fit from Mode (a GtkVideo's own
// picture, inside it).
static void fit(node *n, video *v) {
	GtkContentFit f = GTK_CONTENT_FIT_CONTAIN;
	switch (v->mode) {
	case AK_IMAGE_MODE_FILL: f = GTK_CONTENT_FIT_COVER; break;
	case AK_IMAGE_MODE_STRETCH: f = GTK_CONTENT_FIT_FILL; break;
	case AK_IMAGE_MODE_CENTER: f = GTK_CONTENT_FIT_SCALE_DOWN; break;
	}
	GtkWidget *p = n->inner ? find_picture(n->inner) : NULL;
	if (p) gtk_picture_set_content_fit((GtkPicture *)p, f);
}

// attach shows the stream in the widget.
static void attach(node *n, video *v) {
	if (!n->inner) return;
	if (v->hide)
		gtk_picture_set_paintable((GtkPicture *)n->inner, (GdkPaintable *)v->stream);
	else
		gtk_video_set_media_stream((GtkVideo *)n->inner, v->stream);
}

// make_widget makes the view of the video: a GtkVideo, or a GtkPicture
// when the controls are hidden.
static void make_widget(node *n, video *v) {
	lx_set_inner(n, v->hide ? gtk_picture_new() : gtk_video_new());
	attach(n, v);
	fit(n, v);
}

// report sends VideoState: as anything but the time changed, or with
// force (a tick, a seek) the time too. The caller is inside the core.
static void report(node *n, video *v, bool force) {
	GtkMediaStream *s = v->stream;
	if (!s) return;
	uint8_t f = 0;
	bool prepared = gtk_media_stream_is_prepared(s);
	if (prepared) f |= 1;
	if (gtk_media_stream_get_playing(s)) f |= 2;
	if (gtk_media_stream_get_ended(s) && !v->loop) f |= 8;
	gint64 d = gtk_media_stream_get_duration(s);
	double dur = !prepared ? NAN : d > 0 ? (double)d / 1e6 : INFINITY;  // no duration: a live stream
	bool same = v->told && f == v->flags && (dur == v->duration || (isnan(dur) && isnan(v->duration)));
	if (same && !force) return;
	v->told = true;
	v->flags = f;
	v->duration = dur;
	int w = gdk_paintable_get_intrinsic_width((GdkPaintable *)s), h = gdk_paintable_get_intrinsic_height((GdkPaintable *)s);
	ak_buf b = {0};
	ak_buf_u8(&b, f);
	ak_buf_f64(&b, (double)gtk_media_stream_get_timestamp(s) / 1e6);
	ak_buf_f64(&b, dur);
	ak_buf_f32(&b, 1);
	ak_buf_f32(&b, (float)(w > 0 ? w : 0));
	ak_buf_f32(&b, (float)(h > 0 ? h : 0));
	lx_emit(n, (ak_event){.kind = AK_EVENT_VIDEO_STATE, .s = {(const char *)b.data, b.len}});
	ak_buf_free(&b);
	if (prepared && !v->tracked) {
		v->tracked = true;
		uint8_t none[4] = {0};
		lx_emit(n, (ak_event){.kind = AK_EVENT_VIDEO_TRACKS, .s = {(const char *)none, 4}});
	}
}

static void reported(node *n, video *v, bool force) {
	lx_enter_begin();
	report(n, v, force);
	lx_enter_end();
}

static gboolean on_tick(gpointer data) {
	node *n = data;
	video *v = video_of(n);
	if (!v || !v->stream || !gtk_media_stream_get_playing(v->stream)) {
		if (v) v->tick = 0;
		return 0;  // G_SOURCE_REMOVE
	}
	reported(n, v, true);
	return 1;  // G_SOURCE_CONTINUE
}

// ticking has TimeInterval's timer run while it plays.
static void ticking(node *n, video *v) {
	bool want = v->interval > 0 && v->stream && gtk_media_stream_get_playing(v->stream);
	if (v->tick && !want) {
		g_source_remove(v->tick);
		v->tick = 0;
	} else if (!v->tick && want) {
		v->tick = g_timeout_add((guint)(v->interval * 1000), on_tick, n);
	}
}

// size_known reports the natural size, once, when the stream knows it.
static void size_known(node *n, video *v) {
	if (v->sized || !v->stream) return;
	int w = gdk_paintable_get_intrinsic_width((GdkPaintable *)v->stream), h = gdk_paintable_get_intrinsic_height((GdkPaintable *)v->stream);
	if (w <= 0 || h <= 0) return;
	v->sized = true;
	lx_emit(n, (ak_event){.kind = AK_EVENT_LOADED, .x = w, .y = h});
}

static void on_prepared(GObject *s, GParamSpec *ps, node *n) {
	(void)ps;
	video *v = video_of(n);
	if (!v || (GtkMediaStream *)s != v->stream) return;
	bool prepared = gtk_media_stream_is_prepared(v->stream);
	if (prepared && !v->started) {
		v->started = true;
		if (v->start_at > 0) gtk_media_stream_seek(v->stream, (gint64)(v->start_at * 1e6));
	}
	lx_enter_begin();
	size_known(n, v);
	report(n, v, false);
	lx_enter_end();
	if (v->play && prepared && !gtk_media_stream_get_playing(v->stream)) gtk_media_stream_play(v->stream);
}

static void on_size(GdkPaintable *p, node *n) {
	video *v = video_of(n);
	if (!v || (GtkMediaStream *)p != v->stream) return;
	lx_enter_begin();
	size_known(n, v);
	report(n, v, false);
	lx_enter_end();
}

static void on_ended(GObject *s, GParamSpec *ps, node *n) {
	(void)ps;
	video *v = video_of(n);
	GtkMediaStream *st = (GtkMediaStream *)s;
	if (!v || st != v->stream || !gtk_media_stream_get_ended(st)) return;
	lx_enter_begin();
	lx_emit(n, (ak_event){.kind = AK_EVENT_ENDED});
	report(n, v, true);
	lx_enter_end();
	if (v->loop && v->stream == st) {
		gtk_media_stream_seek(st, 0);
		gtk_media_stream_play(st);
	}
}

// on_changed: playing, the duration, a seek done.
static void on_changed(GObject *s, GParamSpec *ps, node *n) {
	(void)ps;
	video *v = video_of(n);
	if (!v || (GtkMediaStream *)s != v->stream) return;
	ticking(n, v);
	reported(n, v, true);
}

static void on_error(GObject *s, GParamSpec *ps, node *n) {
	(void)ps;
	video *v = video_of(n);
	const GError *e = gtk_media_stream_get_error((GtkMediaStream *)s);
	if (!v || !e) return;
	lx_enter_begin();
	lx_emit(n, (ak_event){.kind = AK_EVENT_FAILED, .s = ak_cstr(e->message ? e->message : "")});
	lx_enter_end();
}

// set_stream lets the stream shown go, and shows s (NULL: none).
static void set_stream(node *n, video *v, GtkMediaStream *s) {
	if (v->stream) {
		GtkMediaStream *old = v->stream;
		g_signal_handlers_disconnect_matched(old, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
		gtk_media_stream_pause(old);
		v->stream = NULL;
		attach(n, v);
		g_object_unref(old);
	}
	if (v->tick) g_source_remove(v->tick);
	v->tick = 0;
	v->sized = v->play = v->started = v->told = v->tracked = false;
	if (!s) return;
	v->stream = g_object_ref(s);
	g_signal_connect(s, "notify::prepared", G_CALLBACK(on_prepared), n);
	g_signal_connect(s, "invalidate-size", G_CALLBACK(on_size), n);
	g_signal_connect(s, "notify::ended", G_CALLBACK(on_ended), n);
	g_signal_connect(s, "notify::error", G_CALLBACK(on_error), n);
	g_signal_connect(s, "notify::playing", G_CALLBACK(on_changed), n);
	g_signal_connect(s, "notify::duration", G_CALLBACK(on_changed), n);
	g_signal_connect(s, "notify::seeking", G_CALLBACK(on_changed), n);
	gtk_media_stream_set_muted(s, v->muted);
	gtk_media_stream_set_volume(s, v->volume);
	attach(n, v);
}

// start plays the video, once it's ready.
static void start(video *v) {
	v->play = true;
	if (!v->stream) return;
	if (gtk_media_stream_get_ended(v->stream)) gtk_media_stream_seek(v->stream, 0);
	gtk_media_stream_play(v->stream);
}

// set_source plays s: an http(s) URL, a file, or one of the app's assets.
static void set_source(node *n, video *v, ak_span src) {
	if (!src.len) {
		set_stream(n, v, NULL);
		return;
	}
	char *s = str_dup(src);
	GFile *file;
	if (strstr(s, "://")) {
		file = g_file_new_for_uri(s);
	} else if (s[0] == '/') {
		file = g_file_new_for_path(s);
	} else {
		char *p = dsk_join(S(H.assets), s);
		file = g_file_new_for_path(p);
		free(p);
	}
	free(s);
	GtkMediaStream *stream = (GtkMediaStream *)gtk_media_file_new_for_file(file);
	g_object_unref(file);
	set_stream(n, v, stream);
	g_object_unref(stream);  // set_stream took its own
	if (v->auto_play) start(v);
}

static void free_video(node *n) {
	video *v = n->ext;
	set_stream(n, v, NULL);
	free(v);
	n->ext = NULL;
}

static bool video_create(node *n) {
	if (n->kind != AK_KIND_VIDEO) return false;
	video *v = calloc(1, sizeof *v);
	if (!v) abort();
	v->volume = 1;
	v->duration = NAN;
	n->ext = v;
	n->ext_free = free_video;
	make_widget(n, v);
	return true;
}

static bool video_prop(node *n, ak_prop p, const ak_value *val) {
	video *v = video_of(n);
	if (!v) return false;
	switch (p) {
	case AK_PROP_VIDEO_SOURCE: set_source(n, v, val->s); break;
	case AK_PROP_VIDEO_MODE:
		v->mode = (int)val->i;
		fit(n, v);
		break;
	case AK_PROP_VIDEO_HIDE_CONTROLS:
		if ((val->i != 0) != v->hide) {
			v->hide = val->i != 0;
			make_widget(n, v);
		}
		break;
	case AK_PROP_VIDEO_AUTO_PLAY:
		v->auto_play = val->i != 0;
		if (v->auto_play && v->stream) start(v);
		break;
	case AK_PROP_VIDEO_LOOP: v->loop = val->i != 0; break;
	case AK_PROP_VIDEO_MUTED:
		v->muted = val->i != 0;
		if (v->stream) gtk_media_stream_set_muted(v->stream, v->muted);
		break;
	case AK_PROP_VIDEO_VOLUME:
		v->volume = val->f < 0 ? 0 : val->f > 1 ? 1 : val->f;
		if (v->stream) gtk_media_stream_set_volume(v->stream, v->volume);
		break;
	case AK_PROP_VIDEO_START_AT: v->start_at = val->f; break;
	case AK_PROP_VIDEO_TIME_INTERVAL:
		v->interval = val->f;
		if (v->tick) g_source_remove(v->tick);
		v->tick = 0;
		ticking(n, v);
		break;
	// GTK's media stream has no rate, tracks, headers, poster or picture
	// in picture: the docs say so.
	case AK_PROP_VIDEO_RATE:
	case AK_PROP_VIDEO_HEADERS:
	case AK_PROP_VIDEO_POSTER:
	case AK_PROP_VIDEO_SUBTITLE:
	case AK_PROP_VIDEO_AUDIO_TRACK:
	case AK_PROP_VIDEO_SUBTITLE_FILES:
	case AK_PROP_VIDEO_PICTURE_IN_PICTURE:
	case AK_PROP_VIDEO_AUTO_PICTURE_IN_PICTURE:
		break;
	default:
		return false;
	}
	return true;
}

static bool video_command(node *n, ak_cmd c, ak_span args) {
	video *v = video_of(n);
	if (!v) return false;
	switch (c) {
	case AK_CMD_PLAY: start(v); return true;
	case AK_CMD_PAUSE:
		v->play = false;
		if (v->stream) gtk_media_stream_pause(v->stream);
		return true;
	case AK_CMD_SEEK: {
		ak_reader r = {(const uint8_t *)args.p, args.len, 0, false};
		double sec = ak_read_f64(&r);
		if (v->stream && !r.err) gtk_media_stream_seek(v->stream, (gint64)(sec * 1e6));
		return true;
	}
	case AK_CMD_VIDEO_ACTION: return true;  // no picture in picture; GtkVideo's own full screen is the user's
	default:
		return false;
	}
}

const area lx_area_video = {
	.name = "video",
	.create = video_create,
	.prop = video_prop,
	.command = video_command,
};
