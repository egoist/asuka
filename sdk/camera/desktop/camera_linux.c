// platform/camera on Linux, with GStreamer. The camera is the camera
// portal's (org.freedesktop.portal.Camera: it asks the user once, and
// gives a PipeWire remote with the cameras, which pipewiresrc reads),
// or, where there's no portal or it has no camera (a session without
// PipeWire's camera support), the first Video4Linux camera (v4l2src).
// The View is a GtkPicture showing the camera's frames (an appsink's,
// mirrored as a camera facing the user is), filling its box; capture
// writes its latest frame as a JPEG (GdkPixbuf's); in video mode record
// records an MPEG-4 file (H.264 and AAC; WebM with VP8 and Opus where
// there's no H.264 encoder), with the microphone unless muted; with
// scan, GStreamer's zbar element reads codes. take shows a dialog of the
// package's over the window in use, as macOS does: a live preview with
// Take Photo and Cancel (Return and Escape). A computer's cameras face
// the user: front and back both take the first one, as on a Mac. Photos
// and videos go to the app's caches (asuka-camera).
//
// The zoom is a V4L2 camera's own (zoom_absolute), else digital: the
// middle of the frames cropped, for the preview, photos, frames and,
// with videocrop, recordings (scaled back to the camera's size). A pinch
// (GtkGestureZoom: a touchpad's, a touchscreen's) or Ctrl and the wheel
// zooms. The exposure's bias and the torch are a V4L2 camera's controls
// where its driver has them; nothing has a flash, or focuses at a point.
// In video mode the camera's asked for videoQuality's size, else the
// nearest it has. frameRate's frames are the preview's (mirrored only with mirror), a ring of JPEGs
// written off the main thread; maxBytes ends a recording as its file
// nears it.
//
// ASUKA_CAMERA_SOURCE, if set, is the camera's source as gst-launch
// writes it ("videotestsrc is-live=true"), and ASUKA_CAMERA_SOUND the
// recording's sound's (autoaudiosrc's place): for tests.
#include "asuka_linux.h"
#include "camera.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct _GstMessage GstMessage;

enum { E_OK, E_NO_CAMERA, E_DENIED, E_OTHER };

static const char *error_text(int code, const char *msg) {
	switch (code) {
	case E_NO_CAMERA:
		return "no camera";
	case E_DENIED:
		return "not allowed";
	}
	return msg ? msg : "the camera failed";
}

// ---- Finding the camera ----

typedef void (*opened_fn)(void *ctx, const char *src, int code, const char *msg);

typedef struct opener {
	opened_fn fn;
	void *ctx;
	struct opener *next;
} opener;

static struct {
	int fd;  // the portal's PipeWire remote, once granted (-1: none)
	bool asked;
	opener *waiting;
	char *src;  // a source found without the portal (V4L2's)
} grant = {-1, false, NULL, NULL};

// told is the permission as this process last learned it ("allowed",
// "denied"; NULL: not yet): the portal's answer, or no portal to ask.
static const char *told;

static bool gst_has(const char *element) {
	gpointer f = G(gst_element_factory_find)(element);
	if (f) G(gst_object_unref)(f);
	return f != NULL;
}

static void finish_open(const char *src, int code, const char *msg) {
	opener *w = grant.waiting;
	grant.waiting = NULL;
	while (w) {
		opener *next = w->next;
		w->fn(w->ctx, src, code, msg);
		free(w);
		w = next;
	}
}

// camera_devices are the Video4Linux cameras' nodes (a camera's capture
// node: its metadata node is its second, index 1), in order.
static char *first_v4l2(void) {
	DIR *d = opendir("/sys/class/video4linux");
	if (!d) return NULL;
	char *best = NULL;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "video", 5)) continue;
		char *idx = asuka_sprintf("/sys/class/video4linux/%s/index", e->d_name);
		FILE *f = fopen(idx, "r");
		free(idx);
		int i = 0;
		if (f) {
			if (fscanf(f, "%d", &i) != 1) i = 0;
			fclose(f);
		}
		if (i != 0) continue;
		size_t n = strlen(e->d_name), bn = best ? strlen(best) : 0;
		if (!best || n < bn || (n == bn && strcmp(e->d_name, best) < 0)) {
			free(best);
			best = asuka_strdup(e->d_name);
		}
	}
	closedir(d);
	return best;
}

static void v4l2(void) {
	char *dev = first_v4l2();
	if (!dev || !gst_has("v4l2src")) {
		free(dev);
		finish_open(NULL, E_NO_CAMERA, NULL);
		return;
	}
	told = "allowed";  // no portal asked: the device's node is the user's
	free(grant.src);
	grant.src = asuka_sprintf("v4l2src device=/dev/%s", dev);  // decoded as the pipeline is made
	free(dev);
	finish_open(grant.src, E_OK, NULL);
}

// Portals' requests: a Request object's Response signal answers.

typedef void (*responded_fn)(void *ctx, guint32 code, GVariant *results);

typedef struct {
	GDBusConnection *conn;
	char *path;
	guint sub;
	bool answered;
	int refs;  // the call's, and the answer's until it comes: either may be last
	responded_fn fn;
	void *ctx;
} request;

static void request_unref(request *r) {
	if (--r->refs) return;
	free(r->path);
	free(r);
}

static void request_response(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal, GVariant *params,
                             gpointer p) {
	request *r = p;
	if (r->answered) return;
	r->answered = true;
	G(g_dbus_connection_signal_unsubscribe)(r->conn, r->sub);
	guint32 code = 2;
	GVariant *results = NULL;
	if (G(g_variant_n_children)(params) >= 2) {
		GVariant *c0 = G(g_variant_get_child_value)(params, 0);
		code = G(g_variant_get_uint32)(c0);
		G(g_variant_unref)(c0);
		results = G(g_variant_get_child_value)(params, 1);
	}
	r->fn(r->ctx, code, results);
	if (results) G(g_variant_unref)(results);
	request_unref(r);  // the answer's: the call's may not have returned
}

static void request_listen(request *r, const char *path) {
	free(r->path);
	r->path = asuka_strdup(path);
	r->sub = G(g_dbus_connection_signal_subscribe)(r->conn, ASUKA_PORTAL, "org.freedesktop.portal.Request", "Response", r->path, NULL,
	                                               G_DBUS_SIGNAL_FLAGS_NONE, request_response, r, NULL);
}

static void request_replied(GObject *src, GAsyncResult *res, gpointer p) {
	request *r = p;
	GError *e = NULL;
	GVariant *v = G(g_dbus_connection_call_finish)(r->conn, res, &e);
	if (r->answered) {  // answered before the call returned
		if (v) G(g_variant_unref)(v);
		if (e) G(g_error_free)(e);
		request_unref(r);
		return;
	}
	if (!v) {
		r->answered = true;
		G(g_dbus_connection_signal_unsubscribe)(r->conn, r->sub);
		if (e) G(g_error_free)(e);
		r->fn(r->ctx, 2, NULL);
		request_unref(r);  // the answer's
		request_unref(r);
		return;
	}
	// An older portal's request path isn't the token's.
	const char *path = NULL;
	G(g_variant_get)(v, "(&o)", &path);
	if (path && strcmp(path, r->path)) {
		G(g_dbus_connection_signal_unsubscribe)(r->conn, r->sub);
		request_listen(r, path);
	}
	G(g_variant_unref)(v);
	request_unref(r);
}

// portal_request calls a portal's method whose answer is a Request's
// Response; params is "(a{sv})" with the handle_token given.
static void portal_request(GDBusConnection *conn, const char *iface, const char *method, responded_fn fn, void *ctx) {
	static int tokens;
	char *token = asuka_sprintf("asukacamera%d", ++tokens);
	const char *unique = G(g_dbus_connection_get_unique_name)(conn);
	char *sender = asuka_strdup(unique && *unique == ':' ? unique + 1 : unique ? unique : "");
	for (char *s = sender; *s; s++)
		if (*s == '.') *s = '_';
	request *r = calloc(1, sizeof *r);
	r->conn = conn;
	r->refs = 2;
	r->fn = fn;
	r->ctx = ctx;
	char *path = asuka_sprintf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
	request_listen(r, path);
	free(path);
	free(sender);
	GVariantBuilder b;
	G(g_variant_builder_init)(&b, G_VARIANT_TYPE("a{sv}"));
	G(g_variant_builder_add)(&b, "{sv}", "handle_token", G(g_variant_new_string)(token));
	free(token);
	G(g_dbus_connection_call)(conn, ASUKA_PORTAL, ASUKA_PORTAL_PATH, iface, method, G(g_variant_new)("(a{sv})", &b), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
	                          NULL, request_replied, r);
}

static void remote_opened(GObject *src, GAsyncResult *res, gpointer p) {
	GDBusConnection *conn = p;
	GError *e = NULL;
	GUnixFDList *list = NULL;
	GVariant *v = G(g_dbus_connection_call_with_unix_fd_list_finish)(conn, &list, res, &e);
	if (e) G(g_error_free)(e);
	gint32 idx = -1;
	if (v) {
		G(g_variant_get)(v, "(h)", &idx);
		G(g_variant_unref)(v);
	}
	gint n = 0;
	gint *fds = list ? G(g_unix_fd_list_steal_fds)(list, &n) : NULL;
	if (list) G(g_object_unref)(list);
	for (gint i = 0; i < n; i++)
		if (i != idx) close(fds[i]);
	if (idx < 0 || idx >= n) {
		G(g_free)(fds);
		finish_open(NULL, E_NO_CAMERA, NULL);
		return;
	}
	grant.asked = true;
	grant.fd = fds[idx];
	G(g_free)(fds);
	char *s = asuka_sprintf("pipewiresrc fd=%d", grant.fd);
	finish_open(s, E_OK, NULL);
	free(s);
}

static void access_answered(void *ctx, guint32 code, GVariant *results) {
	GDBusConnection *conn = ctx;
	told = code == 0 ? "allowed" : "denied";
	if (code != 0) {
		finish_open(NULL, E_DENIED, NULL);
		return;
	}
	GVariantBuilder b;
	G(g_variant_builder_init)(&b, G_VARIANT_TYPE("a{sv}"));
	G(g_dbus_connection_call_with_unix_fd_list)(conn, ASUKA_PORTAL, ASUKA_PORTAL_PATH, "org.freedesktop.portal.Camera", "OpenPipeWireRemote",
	                                            G(g_variant_new)("(a{sv})", &b), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, remote_opened, conn);
}

// ask_presence asks the camera portal whether it has a camera (its
// IsCameraPresent), answered to done with the bus as its source.
static void ask_presence(GDBusConnection *conn, GAsyncReadyCallback done, gpointer p) {
	G(g_dbus_connection_call)(conn, ASUKA_PORTAL, ASUKA_PORTAL_PATH, "org.freedesktop.DBus.Properties", "Get",
	                          G(g_variant_new)("(ss)", "org.freedesktop.portal.Camera", "IsCameraPresent"), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
	                          done, p);
}

// present reads ask_presence's answer: false with no portal, or no
// camera it shows.
static bool present(GDBusConnection *conn, GAsyncResult *res) {
	GError *e = NULL;
	GVariant *v = G(g_dbus_connection_call_finish)(conn, res, &e);
	bool yes = false;
	if (v) {
		GVariant *inner = NULL;
		G(g_variant_get)(v, "(v)", &inner);
		if (inner && G(g_variant_is_of_type)(inner, G_VARIANT_TYPE("b"))) yes = G(g_variant_get_boolean)(inner);
		if (inner) G(g_variant_unref)(inner);
		G(g_variant_unref)(v);
	}
	if (e) G(g_error_free)(e);
	return yes;
}

static void presence(GObject *src, GAsyncResult *res, gpointer p) {
	GDBusConnection *conn = p;
	if (!present(conn, res) || !gst_has("pipewiresrc")) {
		// No portal, or no camera it shows (PipeWire without its camera
		// support): V4L2's, outside a sandbox.
		v4l2();
		return;
	}
	portal_request(conn, "org.freedesktop.portal.Camera", "AccessCamera", access_answered, conn);
}

// camera_open finds the camera, asking the portal the first time, and
// gives its source's description to fn(ctx).
static void camera_open(opened_fn fn, void *ctx) {
	if (!asuka_linux() || !asuka_linux_gst() || !G(gst_app_sink_try_pull_sample)) {
		fn(ctx, NULL, E_OTHER, "the camera needs GStreamer (its base plugins)");
		return;
	}
	const char *test = getenv("ASUKA_CAMERA_SOURCE");
	if (test && *test) {
		fn(ctx, test, E_OK, NULL);
		return;
	}
	if (grant.asked && grant.fd >= 0) {
		char *s = asuka_sprintf("pipewiresrc fd=%d", grant.fd);
		fn(ctx, s, E_OK, NULL);
		free(s);
		return;
	}
	opener *w = malloc(sizeof *w);
	*w = (opener){fn, ctx, NULL};
	opener **q = &grant.waiting;
	while (*q) q = &(*q)->next;
	*q = w;
	if (grant.waiting != w) return;  // already asking
	GDBusConnection *conn = asuka_linux_bus(G_BUS_TYPE_SESSION, NULL);
	if (!conn) {
		v4l2();
		return;
	}
	ask_presence(conn, presence, conn);
}

// camera_open_cancel forgets ctx's waiting.
static void camera_open_cancel(void *ctx) {
	for (opener **q = &grant.waiting; *q;)
		if ((*q)->ctx == ctx) {
			opener *w = *q;
			*q = w->next;
			free(w);
		} else {
			q = &(*q)->next;
		}
}

// encoders picks the recording's: H.264 and AAC in an MPEG-4 file, else
// VP8 and Opus (or Vorbis) in WebM. NULL ext with *why set when there's
// none.
typedef struct {
	const char *ext, *video, *audio, *mux;
} encoding;

static bool encoders(bool mute, encoding *out, const char **why) {
	static const char *h264[][2] = {
	    {"x264enc", "x264enc tune=zerolatency speed-preset=veryfast key-int-max=60"},
	    {"openh264enc", "openh264enc"},
	    {"vah264enc", "vah264enc"},
	    {"vah264lpenc", "vah264lpenc"},
	    {"v4l2h264enc", "v4l2h264enc"},
	};
	static const char *aac[][2] = {
	    {"fdkaacenc", "fdkaacenc ! aacparse"}, {"avenc_aac", "avenc_aac ! aacparse"}, {"voaacenc", "voaacenc ! aacparse"}, {"faac", "faac ! aacparse"}};
	static char video[160];
	*out = (encoding){0};
	for (size_t i = 0; i < sizeof h264 / sizeof *h264; i++) {
		if (!gst_has(h264[i][0]) || !gst_has("h264parse") || !gst_has("mp4mux")) continue;
		snprintf(video, sizeof video, "%s ! h264parse", h264[i][1]);
		out->video = video;
		out->mux = "mp4mux";
		out->ext = ".mp4";
		if (!mute) {
			for (size_t j = 0; j < sizeof aac / sizeof *aac && !out->audio; j++)
				if (gst_has(aac[j][0])) out->audio = aac[j][1];
			if (!out->audio && gst_has("opusenc")) out->audio = "opusenc";
		}
		return true;
	}
	if (!gst_has("vp8enc") || !gst_has("webmmux")) {
		*why = "no video encoder (install GStreamer's ugly or bad plugins: x264 or OpenH264)";
		return false;
	}
	out->video = "vp8enc deadline=1 cpu-used=8";
	out->mux = "webmmux";
	out->ext = ".webm";
	if (!mute) out->audio = gst_has("opusenc") ? "opusenc" : gst_has("vorbisenc") ? "vorbisenc" : NULL;
	return true;
}

// ---- A Video4Linux camera's controls ----
//
// Where the camera is v4l2src's, its node is opened beside it for the
// controls a driver has: zoom_absolute (a UVC camera's own zoom, used as
// the zoom when its values read as factors: camera_zoom_optical), the
// auto exposure's bias (EV, in thousandths, an integer or a menu of
// them), and a flash LED's torch mode. PipeWire's camera has none an app
// reaches: its zoom is digital.

typedef struct {
	int fd;  // -1: none
	bool zoom, bias, torch;
	camera_zoom_range zr;
	double max_zoom;
	// The bias: an integer's range, or a menu's values (index i at
	// bias_min + i).
	long bias_min, bias_max;
	int64_t *menu;
	int menu_n;
	double ev_min, ev_max;
} controls;

static bool query_ctrl(int fd, uint32_t id, struct v4l2_queryctrl *q) {
	memset(q, 0, sizeof *q);
	q->id = id;
	return ioctl(fd, VIDIOC_QUERYCTRL, q) == 0 && !(q->flags & V4L2_CTRL_FLAG_DISABLED);
}

static void set_ctrl(int fd, uint32_t id, int32_t value) {
	struct v4l2_control c = {id, value};
	ioctl(fd, VIDIOC_S_CTRL, &c);
}

static void controls_close(controls *c) {
	if (c->fd >= 0) close(c->fd);
	free(c->menu);
	*c = (controls){.fd = -1};
}

// controls_open reads a v4l2src source's node's controls ("v4l2src
// device=/dev/video0").
static void controls_open(controls *c, const char *src) {
	controls_close(c);
	const char *dev = src && !strncmp(src, "v4l2src device=", 15) ? src + 15 : NULL;
	if (!dev) return;
	char *path = asuka_sprintf("%.*s", (int)strcspn(dev, " !"), dev);
	c->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	free(path);
	if (c->fd < 0) return;
	struct v4l2_queryctrl q;
	if (query_ctrl(c->fd, V4L2_CID_ZOOM_ABSOLUTE, &q)) {
		c->zr = (camera_zoom_range){q.minimum, q.maximum};
		c->zoom = camera_zoom_optical(c->zr, &c->max_zoom);
	}
	if (query_ctrl(c->fd, V4L2_CID_AUTO_EXPOSURE_BIAS, &q) && q.maximum > q.minimum) {
		c->bias_min = q.minimum, c->bias_max = q.maximum;
		if (q.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
			c->menu_n = (int)(q.maximum - q.minimum + 1);
			c->menu = calloc((size_t)c->menu_n, sizeof *c->menu);
			for (int i = 0; i < c->menu_n; i++) {
				struct v4l2_querymenu m = {.id = q.id, .index = (uint32_t)(q.minimum + i)};
				if (ioctl(c->fd, VIDIOC_QUERYMENU, &m) == 0) c->menu[i] = m.value;
			}
			c->ev_min = c->ev_max = (double)c->menu[0] / 1000;
			for (int i = 0; i < c->menu_n; i++) {
				double ev = (double)c->menu[i] / 1000;
				if (ev < c->ev_min) c->ev_min = ev;
				if (ev > c->ev_max) c->ev_max = ev;
			}
		} else {
			c->ev_min = (double)q.minimum / 1000, c->ev_max = (double)q.maximum / 1000;
		}
		c->bias = c->ev_max > c->ev_min;
	}
	if (query_ctrl(c->fd, V4L2_CID_FLASH_LED_MODE, &q) && q.maximum >= V4L2_FLASH_LED_MODE_TORCH) c->torch = true;
}

static void controls_bias(controls *c, double ev) {
	if (!c->bias) return;
	if (!c->menu) {
		set_ctrl(c->fd, V4L2_CID_AUTO_EXPOSURE_BIAS, (int32_t)camera_clamp(ev * 1000 + (ev < 0 ? -0.5 : 0.5), (double)c->bias_min, (double)c->bias_max));
		return;
	}
	int best = 0;
	for (int i = 1; i < c->menu_n; i++)
		if (fabs((double)c->menu[i] / 1000 - ev) < fabs((double)c->menu[best] / 1000 - ev)) best = i;
	set_ctrl(c->fd, V4L2_CID_AUTO_EXPOSURE_BIAS, (int32_t)(c->bias_min + best));
}

// ---- The view ----

typedef struct sheet sheet;

typedef struct view view;
struct view {
	camera_View base;
	camera_View_events events;
	unsigned serial;
	sheet *sheet;  // the take dialog's (never the current one), or NULL
	GtkWidget *picture;
	GtkGesture *pinch;
	GtkEventController *wheel;
	GstElement *pipe, *sink, *crop;
	GstBus *bus;
	gulong bus_handlers[3], map_handlers[2];
	guint tick, idle, rec_timer, bytes_timer;
	bool front, video, scan;
	bool mapped, opening, destroyed, recording, stopping, mute;
	bool snap;  // the dialog's photo is taken from the next frame
	bool ready_due;
	// The props.
	double zoom, exposure, pinch_from;
	bool pinch_on, torch, mirror;
	camera_VideoQuality quality;
	camera_frames frames;
	camera_kinds kinds;
	camera_seen seen;
	controls ctl;
	int fw, fh;   // the camera's frames, as the last came
	char *src;    // the camera's source, once open
	camera_pending *capturing, *rec_reply;
	int64_t cap_max;  // what the waiting capture asked for
	double cap_quality;
	char *rec_path;
	int64_t rec_max_bytes, rec_last_size;
	bool rec_mirror;
	view *next;
};

static view *views;  // those not destroyed
// current is the view on screen capture and record use.
static view *current;

static view *view_of(unsigned serial) {
	for (view *v = views; v; v = v->next)
		if (v->serial == serial) return v;
	return NULL;
}

static void sheet_failed(sheet *s, const char *msg);
static unsigned sheet_serial(sheet *s);
static void start(view *v);
static void stop_pipe(view *v);

static void fail(view *v, const char *msg) {
	if (v->destroyed) return;
	if (v->sheet) sheet_failed(v->sheet, msg);
	else camera_View_error(v->events, msg);
}

// fail_all reports the camera's failing: the view's error, and a
// waiting photo's.
static void fail_all(view *v, const char *msg) {
	camera_fail(&v->capturing, msg);
	fail(v, msg);
}

static void rec_timers_stop(view *v) {
	if (v->rec_timer) G(g_source_remove)(v->rec_timer), v->rec_timer = 0;
	if (v->bytes_timer) G(g_source_remove)(v->bytes_timer), v->bytes_timer = 0;
}

static void record_failed(view *v, const char *why) {
	v->recording = v->stopping = false;
	rec_timers_stop(v);
	if (v->rec_reply) {
		char *m = asuka_sprintf("the recording failed: %s", why);
		camera_fail(&v->rec_reply, m);
		free(m);
	}
}

// can_crop_video reports whether a recording can be zoomed digitally
// (GStreamer's videocrop and videoscale, of the good and base plugins).
static bool can_crop_video(void) {
	static int has = -1;
	if (has < 0) has = gst_has("videocrop") && gst_has("videoscale");
	return has;
}

// max_zoom is the most the view zooms: its camera's own zoom's, else
// digitally (a recording's too, where it can be cropped).
static double max_zoom(const view *v) {
	if (v->ctl.zoom) return v->ctl.max_zoom;
	if (v->video && !can_crop_video()) return 1;
	return CAMERA_DIGITAL_ZOOM;
}

static double zoom_now(const view *v) { return camera_clamp(v->zoom, 1, max_zoom(v)); }

// digital is the zoom the view crops its frames by (1: its camera's own
// zoom does it).
static double digital(const view *v) { return v->ctl.zoom ? 1 : zoom_now(v); }

// crop_props are videocrop's sides for a w×h frame at the digital zoom.
static void crop_props(const view *v, int *l, int *r, int *t, int *b) {
	camera_rect c = camera_zoom_crop(v->fw, v->fh, digital(v));
	*l = c.x, *t = c.y, *r = v->fw - c.x - c.w, *b = v->fh - c.y - c.h;
}

// apply_zoom zooms the camera (its own zoom), or a recording's crop.
static void apply_zoom(view *v) {
	if (v->ctl.zoom) {
		set_ctrl(v->ctl.fd, V4L2_CID_ZOOM_ABSOLUTE, (int32_t)camera_zoom_value(v->ctl.zr, zoom_now(v)));
	} else if (v->crop) {
		int l, r, t, b;
		crop_props(v, &l, &r, &t, &b);
		G(g_object_set)(v->crop, "left", l, "right", r, "top", t, "bottom", b, NULL);
	}
}

static void apply_controls(view *v) {
	if (v->ctl.fd < 0) return;
	if (v->ctl.zoom) apply_zoom(v);
	controls_bias(&v->ctl, v->exposure);
	if (v->ctl.torch) set_ctrl(v->ctl.fd, V4L2_CID_FLASH_LED_MODE, v->torch ? V4L2_FLASH_LED_MODE_TORCH : V4L2_FLASH_LED_MODE_NONE);
}

// The caps a video mode's camera is asked for: videoQuality's size, else
// the nearest, else what it has.
static void put_quality(asuka_writer *b, camera_VideoQuality q) {
	camera_size sizes[4];
	camera_quality_sizes(q, sizes);
	asuka_writer_put(b, " ! capsfilter caps=\"", 20);
	for (int i = 0; i < 4; i++) {
		char *s = asuka_sprintf("video/x-raw,width=%d,height=%d;image/jpeg,width=%d,height=%d;", sizes[i].w, sizes[i].h, sizes[i].w, sizes[i].h);
		asuka_writer_put(b, s, strlen(s));
		free(s);
	}
	const char *any = "video/x-raw;image/jpeg\"";
	asuka_writer_put(b, any, strlen(any));
}

// description is the pipeline: the camera, through a tee, to the
// preview's appsink (mirrored), to zbar with scan, and to the encoders
// and a file while recording (cropped to the zoom, mirrored if mirror).
static char *description(view *v, const char **why) {
	asuka_writer b = {0};
#define PUT(s) asuka_writer_put(&b, (s), strlen(s))
	PUT(v->src);
	bool v4l2 = !strncmp(v->src, "v4l2src", 7);
	if (v->video && (v4l2 || !strncmp(v->src, "pipewiresrc", 11))) put_quality(&b, v->quality);
	if (v4l2) PUT(" ! decodebin");
	PUT(" ! queue leaky=downstream max-size-buffers=2 ! videoconvert ! tee name=t");
	PUT(" t. ! queue leaky=downstream max-size-buffers=1 ! videoflip method=horizontal-flip ! videoconvert ! video/x-raw,format=RGBA ! appsink "
	    "name=frames max-buffers=1 drop=true sync=false");
	if (v->scan && gst_has("zbar")) PUT(" t. ! queue leaky=downstream max-size-buffers=1 ! videoconvert ! zbar cache=false ! fakesink sync=false async=false");
	if (v->recording) {
		encoding enc;
		if (!encoders(v->mute, &enc, why)) {
			asuka_writer_free(&b);
			return NULL;
		}
		char *dot = strrchr(v->rec_path, '.');
		if (!dot || strcmp(dot, enc.ext)) {
			size_t keep = dot ? (size_t)(dot - v->rec_path) : strlen(v->rec_path);
			char *p = asuka_sprintf("%.*s%s", (int)keep, v->rec_path, enc.ext);
			rename(v->rec_path, p);
			free(v->rec_path);
			v->rec_path = p;
		}
		PUT(" t. ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=3000000000 ! videoconvert");
		if (v->rec_mirror) PUT(" ! videoflip method=horizontal-flip");
		if (!v->ctl.zoom && can_crop_video() && v->fw > 0 && v->fh > 0) {
			// Cropped to the zoom, scaled back to the camera's size: the
			// zoom changes as it records.
			int l, r, t, bt;
			crop_props(v, &l, &r, &t, &bt);
			char *s = asuka_sprintf(" ! videocrop name=crop left=%d right=%d top=%d bottom=%d ! videoscale ! video/x-raw,width=%d,height=%d,pixel-aspect-ratio=1/1"
			                        " ! videoconvert",
			                        l, r, t, bt, v->fw, v->fh);
			PUT(s);
			free(s);
		}
		char *s = asuka_sprintf(" ! %s ! %s name=mux ! filesink name=file", enc.video, enc.mux);
		PUT(s);
		free(s);
		if (enc.audio) {
			const char *sound = getenv("ASUKA_CAMERA_SOUND");
			s = asuka_sprintf(" %s ! queue ! audioconvert ! audioresample ! %s ! mux.", sound && *sound ? sound : "autoaudiosrc", enc.audio);
			PUT(s);
			free(s);
		}
	}
#undef PUT
	asuka_writer_put(&b, "", 1);
	return (char *)b.data;
}

// ---- Photos and frames, written off the main thread ----

typedef struct {
	camera_pending *reply;
	unsigned sheet;  // or the photo of the take dialog of this serial
	unsigned view;   // or a frame of the view of this serial
	char *path;
	int w, h;          // the crop's
	uint8_t *rgba;     // the crop of the frame, rows of w*4
	bool mirror;       // flipped as it's written
	int max, quality;  // its long side's most (0: as it is); the JPEG's quality (1 to 100)
	int ow, oh;        // as written
	int64_t ms;        // a frame's time
	bool ok;
} photo;

static void sheet_photo(unsigned serial, const char *path);

static void photo_saved(void *p) {
	photo *ph = p;
	if (ph->view) {
		view *v = view_of(ph->view);
		if (v) {
			v->frames.writing = false;
			if (ph->ok && !v->destroyed) {
				camera_Frame f = {ph->path, ph->ow, ph->oh, ph->ms * 1000000};
				camera_View_frame(v->events, &f);
			}
		} else if (ph->path) {
			unlink(ph->path);  // its view went
		}
	} else if (ph->sheet) {
		sheet_photo(ph->sheet, ph->ok ? ph->path : NULL);
	} else if (ph->ok) {
		camera_answer(&ph->reply, ph->path);
	} else {
		camera_fail(&ph->reply, "couldn't save the photo");
	}
	free(ph->path);
	free(ph);
}

// save_photo writes the photo (or the frame) as a JPEG. Not on the main
// thread.
static void save_photo(void *p) {
	photo *ph = p;
	if (!ph->path) ph->path = camera_file(".jpg");
	camera_fit(ph->w, ph->h, ph->max, &ph->ow, &ph->oh);
	if (ph->path && asuka_linux_load(ASUKA_LIB_PIXBUF) && G(gdk_pixbuf_new_from_data) && G(gdk_pixbuf_save)) {
		uint8_t *rgb = malloc((size_t)ph->ow * ph->oh * 3);
		camera_resample(ph->rgba, (size_t)ph->w * 4, (camera_rect){0, 0, ph->w, ph->h}, ph->mirror, rgb, ph->ow, ph->oh);
		GdkPixbuf *pb = G(gdk_pixbuf_new_from_data)(rgb, GDK_COLORSPACE_RGB, false, 8, ph->ow, ph->oh, ph->ow * 3, NULL, NULL);
		GError *e = NULL;
		char q[8];
		snprintf(q, sizeof q, "%d", ph->quality);
		ph->ok = pb && G(gdk_pixbuf_save)(pb, ph->path, "jpeg", &e, "quality", q, NULL);
		if (e) G(g_error_free)(e);
		if (pb) G(g_object_unref)(pb);
		free(rgb);
		if (!ph->ok) unlink(ph->path);
	}
	free(ph->rgba);
	ph->rgba = NULL;
	asuka_desktop_post(photo_saved, ph);
}

// crop_copy copies the part r of a frame of rows of stride bytes.
static uint8_t *crop_copy(const uint8_t *data, size_t stride, camera_rect r) {
	uint8_t *out = malloc((size_t)r.w * r.h * 4);
	for (int y = 0; y < r.h; y++) memcpy(out + (size_t)y * r.w * 4, data + (size_t)(r.y + y) * stride + (size_t)r.x * 4, (size_t)r.w * 4);
	return out;
}

static void spawn_photo(photo *ph) {
	if (!asuka_desktop_spawn(save_photo, ph)) {
		free(ph->rgba);
		ph->rgba = NULL;
		ph->ok = false;
		photo_saved(ph);
	}
}

// ready says what the camera can do, once it shows.
static void ready(view *v) {
	v->ready_due = false;
	if (v->sheet || v->destroyed) return;
	camera_Info i = {.minZoom = 1, .maxZoom = max_zoom(v), .torch = v->ctl.torch};
	if (v->ctl.bias) i.minExposure = v->ctl.ev_min, i.maxExposure = v->ctl.ev_max;
	camera_View_ready(v->events, &i);
}

// pull_frame shows the camera's latest frame, if there's a new one,
// cropped to the zoom; gives it to a waiting capture, and to frame when
// one's due.
static void pull_frame(view *v) {
	if (!v->sink) return;
	gpointer sample = G(gst_app_sink_try_pull_sample)((GstAppSink *)v->sink, 0);
	if (!sample) return;
	gpointer buf = G(gst_sample_get_buffer)(sample);
	GstCaps *caps = G(gst_sample_get_caps)(sample);
	gpointer st = caps ? G(gst_caps_get_structure)(caps, 0) : NULL;
	gint w = 0, h = 0;
	GstMapInfo info;
	if (buf && st && G(gst_structure_get_int)(st, "width", &w) && G(gst_structure_get_int)(st, "height", &h) && w > 0 && h > 0 &&
	    G(gst_buffer_map)(buf, &info, GST_MAP_READ)) {
		// RGBA rows are 4-byte aligned: GStreamer's stride is the width's.
		size_t stride = (size_t)w * 4;
		if (info.size >= stride * (size_t)h) {
			v->fw = w, v->fh = h;
			camera_rect r = camera_zoom_crop(w, h, digital(v));
			GBytes *bytes = G(g_bytes_new)(info.data + (size_t)r.y * stride + (size_t)r.x * 4, stride * (size_t)(r.h - 1) + (size_t)r.w * 4);
			GdkTexture *t = G(gdk_memory_texture_new)(r.w, r.h, 5, bytes, stride);  // GDK_MEMORY_R8G8B8A8
			G(g_bytes_unref)(bytes);
			G(gtk_picture_set_paintable)((GtkPicture *)v->picture, (GdkPaintable *)t);
			G(g_object_unref)(t);
			if (v->ready_due) ready(v);
			if (v->capturing || v->snap) {
				// The preview's mirrored: the photo is as others see the
				// user, unless mirror.
				photo *ph = calloc(1, sizeof *ph);
				ph->w = r.w, ph->h = r.h;
				ph->rgba = crop_copy(info.data, stride, r);
				ph->mirror = !(v->mirror && !v->sheet);
				ph->max = v->snap ? 0 : (int)v->cap_max;
				double q = v->snap || !(v->cap_quality > 0) ? 0.9 : v->cap_quality;
				ph->quality = (int)camera_clamp(q * 100 + 0.5, 1, 100);
				camera_move(&v->capturing, &ph->reply);
				if (v->snap) ph->sheet = sheet_serial(v->sheet);
				v->snap = false;
				spawn_photo(ph);
			}
			char *path = NULL;
			int64_t ms = 0;
			if (!v->sheet && camera_frames_due(&v->frames, camera_now_ms(), &path, &ms)) {
				photo *ph = calloc(1, sizeof *ph);
				ph->view = v->serial;
				ph->path = path;
				ph->ms = ms;
				ph->w = r.w, ph->h = r.h;
				ph->rgba = crop_copy(info.data, stride, r);
				ph->mirror = !v->mirror;  // the preview's mirrored: as others see the user, unless mirror
				ph->max = v->frames.size > 0 ? v->frames.size : 480;
				ph->quality = 80;
				spawn_photo(ph);
			}
		}
		G(gst_buffer_unmap)(buf, &info);
	}
	G(gst_mini_object_unref)(sample);
}

static gboolean ticked(GtkWidget *w, gpointer clock, gpointer p) {
	view *v = p;
	if (!v->pipe || v->destroyed) {
		v->tick = 0;
		return false;
	}
	pull_frame(v);
	return true;
}

// zbar_kind is platform/scan's name of a ZBar symbology (NULL: none of its).
static const char *zbar_kind(const char *t) {
	static const char *names[][2] = {
	    {"QR-Code", "qr"},     {"EAN-13", "ean13"},     {"EAN-8", "ean8"},     {"UPC-A", "upca"},     {"UPC-E", "upce"},        {"CODE-128", "code128"},
	    {"CODE-39", "code39"}, {"CODE-93", "code93"},   {"Codabar", "codabar"}, {"CODABAR", "codabar"}, {"I2/5", "itf"}, {"PDF417", "pdf417"},
	};
	for (size_t i = 0; t && i < sizeof names / sizeof *names; i++)
		if (!strcmp(t, names[i][0])) return names[i][1];
	return NULL;
}

// on_element reports a code zbar read, newly in view.
static void on_element(GstBus *bus, GstMessage *msg, gpointer p) {
	view *v = p;
	const GstStructure *st = G(gst_message_get_structure)(msg);
	const char *name = st ? G(gst_structure_get_name)(st) : NULL;
	if (!name || strcmp(name, "barcode")) return;
	const char *kind = zbar_kind(G(gst_structure_get_string)(st, "type"));
	const char *value = G(gst_structure_get_string)(st, "symbol");
	if (!kind || !value || !*value) return;
	char *named = NULL;
	// A UPC-A for an app that asked for EAN-13s but not UPC-As is an
	// EAN-13 with a leading 0, as Apple's readers give it.
	if (!strcmp(kind, "upca") && !camera_kinds_wants(&v->kinds, "upca") && camera_kinds_wants(&v->kinds, "ean13") && strlen(value) == 12) {
		kind = "ean13";
		value = named = asuka_sprintf("0%s", value);
	}
	if (camera_kinds_wants(&v->kinds, kind) && camera_seen_new(&v->seen, kind, value, camera_now_ms()) && !v->sheet) {
		camera_Code c = {(char *)kind, (char *)value};
		camera_View_code(v->events, &c);
	}
	free(named);
}

// ended is the pipeline's end: a recording's file is whole; the preview
// goes on.
static void ended(view *v) {
	bool was = v->recording;
	v->recording = v->stopping = false;
	rec_timers_stop(v);
	stop_pipe(v);
	if (was) camera_answer(&v->rec_reply, v->rec_path);
	start(v);
}

static void on_eos(GstBus *bus, GstMessage *msg, gpointer p) { ended(p); }

static void on_error(GstBus *bus, GstMessage *msg, gpointer p) {
	view *v = p;
	GError *e = NULL;
	gchar *debug = NULL;
	G(gst_message_parse_error)(msg, &e, &debug);
	if (debug) G(g_free)(debug);
	char *m = asuka_strdup(e && e->message ? e->message : "GStreamer: an error");
	if (e) G(g_error_free)(e);
	stop_pipe(v);
	if (v->recording) {
		record_failed(v, m);
		start(v);
	} else {
		fail_all(v, m);
	}
	free(m);
}

// start (re)makes the pipeline and plays it.
static void start(view *v) {
	stop_pipe(v);
	if (v->destroyed || !v->mapped || !v->src) return;
	const char *why = NULL;
	char *desc = description(v, &why);
	if (!desc) {
		record_failed(v, why);
		return;
	}
	GError *e = NULL;
	GstElement *pipe = G(gst_parse_launch)(desc, &e);
	free(desc);
	if (!pipe) {
		char *m = asuka_sprintf("the camera can't show: %s", e && e->message ? e->message : "no pipeline");
		record_failed(v, e && e->message ? e->message : "no pipeline");
		if (e) G(g_error_free)(e);
		fail(v, m);  // last: a dialog's view goes with its dialog
		free(m);
		return;
	}
	if (e) G(g_error_free)(e);
	G(g_object_ref_sink)(pipe);
	v->pipe = pipe;
	v->sink = G(gst_bin_get_by_name)(pipe, "frames");
	v->crop = G(gst_bin_get_by_name)(pipe, "crop");
	GstElement *file = G(gst_bin_get_by_name)(pipe, "file");
	if (file) {
		G(g_object_set)(file, "location", v->rec_path, NULL);
		G(gst_object_unref)(file);
	}
	v->bus = G(gst_element_get_bus)(pipe);
	G(gst_bus_add_signal_watch)(v->bus);
	v->bus_handlers[0] = g_signal_connect(v->bus, "message::error", on_error, v);
	v->bus_handlers[1] = g_signal_connect(v->bus, "message::eos", on_eos, v);
	v->bus_handlers[2] = g_signal_connect(v->bus, "message::element", on_element, v);
	if (G(gst_element_set_state)(pipe, GST_STATE_PLAYING) == 0) {  // GST_STATE_CHANGE_FAILURE
		stop_pipe(v);
		fail_all(v, "no camera");
		return;
	}
	apply_controls(v);
	if (!v->tick) v->tick = G(gtk_widget_add_tick_callback)(v->picture, (GtkTickCallback)ticked, v, NULL);
}

// stop_pipe stops and lets go of the pipeline.
static void stop_pipe(view *v) {
	if (!v->pipe) return;
	for (int i = 0; i < 3; i++)
		if (v->bus_handlers[i]) G(g_signal_handler_disconnect)(v->bus, v->bus_handlers[i]), v->bus_handlers[i] = 0;
	G(gst_bus_remove_signal_watch)(v->bus);
	G(gst_object_unref)(v->bus);
	v->bus = NULL;
	if (v->sink) G(gst_object_unref)(v->sink), v->sink = NULL;
	if (v->crop) G(gst_object_unref)(v->crop), v->crop = NULL;
	G(gst_element_set_state)(v->pipe, GST_STATE_NULL);
	G(gst_object_unref)(v->pipe);
	v->pipe = NULL;
	if (v->tick) G(gtk_widget_remove_tick_callback)(v->picture, v->tick), v->tick = 0;
}

static void opened(void *p, const char *src, int code, const char *msg) {
	view *v = p;
	v->opening = false;
	if (code != E_OK) {
		fail(v, error_text(code, msg));
		return;
	}
	free(v->src);
	v->src = asuka_strdup(src);
	controls_open(&v->ctl, v->src);
	v->ready_due = true;
	if (v->mapped) start(v);
}

// open finds the camera (once), then starts the pipeline.
static void open_camera(view *v) {
	if (v->destroyed || v->opening) return;
	if (v->src) {
		start(v);
		return;
	}
	v->opening = true;
	camera_open(opened, v);
}

static void attached(view *v, bool on) {
	v->mapped = on;
	if (!v->sheet) {
		if (on) current = v;
		else if (current == v) current = NULL;
	}
	if (on) open_camera(v);
	else stop_pipe(v);
}

static void on_map(GtkWidget *w, gpointer p) { attached(p, true); }
static void on_unmap(GtkWidget *w, gpointer p) { attached(p, false); }

static gboolean configure(gpointer p) {
	view *v = p;
	v->idle = 0;
	if (v->pipe && !v->recording) start(v);
	return false;
}

// changed restarts the pipeline once for the props set together.
static void changed(view *v) {
	if (v->idle || !v->pipe) return;
	v->idle = G(g_idle_add_full)(G_PRIORITY_DEFAULT, configure, v, NULL);
}

// zoom_to zooms to z (within the camera's range); the user's pinch says
// so (zoomChange).
static void zoom_to(view *v, double z, bool user) {
	z = camera_clamp(z, 1, max_zoom(v));
	if (z == zoom_now(v) && z == v->zoom) return;
	v->zoom = z;
	apply_zoom(v);
	if (user && !v->sheet) camera_View_zoomChange(v->events, z);
}

static void pinch_begin(GtkGesture *g, gpointer seq, gpointer p) {
	view *v = p;
	v->pinch_from = zoom_now(v);
}

static void pinch_scale(GtkGestureZoom *g, double scale, gpointer p) {
	view *v = p;
	if (v->pinch_on && scale > 0) zoom_to(v, v->pinch_from * scale, true);
}

// Ctrl and the wheel zoom too: 10% a notch.
static gboolean wheel(GtkEventControllerScroll *c, double dx, double dy, gpointer p) {
	view *v = p;
	if (!v->pinch_on || !(G(gtk_event_controller_get_current_event_state)((GtkEventController *)c) & GDK_CONTROL_MASK)) return false;
	zoom_to(v, zoom_now(v) * pow(1.1, -dy), true);
	return true;
}

static void set_facing(camera_View *self, camera_Facing f) {
	view *v = (view *)self;
	v->front = f == camera_Facing_front;  // the same camera: it says so again
	v->ready_due = v->src != NULL;
}
static void set_mode(camera_View *self, camera_Mode m) {
	view *v = (view *)self;
	bool video = m == camera_Mode_video;
	if (video == v->video) return;
	v->video = video;
	v->ready_due = v->src != NULL;  // its zoom's most may change
	changed(v);
}
static void set_scan(camera_View *self, bool on) {
	view *v = (view *)self;
	v->scan = on;
	changed(v);
}
static void set_codes(camera_View *self, camera_list_string codes) {
	view *v = (view *)self;
	camera_kinds_set(&v->kinds, codes);
	changed(v);
}
static void set_zoom(camera_View *self, double z) { zoom_to((view *)self, z, false); }
static void set_pinch(camera_View *self, bool on) { ((view *)self)->pinch_on = on; }
static void set_torch(camera_View *self, bool on) {
	view *v = (view *)self;
	v->torch = on;
	apply_controls(v);
}
static void set_flash(camera_View *self, camera_Flash f) {}         // a computer's camera has none
static void set_tap(camera_View *self, bool on) {}                  // none focuses at a point
static void set_photo_quality(camera_View *self, camera_PhotoQuality q) {}  // a frame of the preview's: nothing to favor
static void set_exposure(camera_View *self, double ev) {
	view *v = (view *)self;
	v->exposure = ev;
	if (v->ctl.fd >= 0) controls_bias(&v->ctl, ev);
}
static void set_video_quality(camera_View *self, camera_VideoQuality q) {
	view *v = (view *)self;
	if (q == v->quality) return;
	v->quality = q;
	if (v->video) changed(v);
}
static void set_mirror(camera_View *self, bool on) { ((view *)self)->mirror = on; }  // the next photo's and recording's
static void set_frame_rate(camera_View *self, double r) {
	view *v = (view *)self;
	v->frames.rate = camera_clamp(r, 0, CAMERA_MAX_FRAME_RATE);
}
static void set_frame_size(camera_View *self, int64_t s) { ((view *)self)->frames.size = s > 0 ? (int)(s < 8192 ? s : 8192) : 480; }

// measure: none of its own; its style gives it a size.
static bool measure(camera_View *self, float w, float h, float *ow, float *oh) {
	*ow = w == w ? w : 0;
	*oh = h == h ? h : 0;
	return true;
}

// destroy: the pipeline stops, and what waits on it fails.
static void destroy(camera_View *self) {
	view *v = (view *)self;
	if (current == v) current = NULL;
	if (v->recording) record_failed(v, "the camera isn't showing");  // what's recorded so far is lost with the pipeline
	stop_pipe(v);
	v->destroyed = true;
	for (view **q = &views; *q; q = &(*q)->next)
		if (*q == v) {
			*q = v->next;
			break;
		}
	camera_fail(&v->capturing, "the camera isn't showing");
	camera_open_cancel(v);
	for (int i = 0; i < 2; i++)
		if (v->map_handlers[i]) G(g_signal_handler_disconnect)(v->picture, v->map_handlers[i]);
	if (v->pinch) G(gtk_widget_remove_controller)(v->picture, (GtkEventController *)v->pinch);
	if (v->wheel) G(gtk_widget_remove_controller)(v->picture, v->wheel);
	if (v->idle) G(g_source_remove)(v->idle);
	rec_timers_stop(v);
	controls_close(&v->ctl);
	if (!v->sheet) camera_frames_clean(v->serial);
	G(g_object_unref)(v->picture);
	camera_kinds_free(&v->kinds);
	camera_seen_free(&v->seen);
	free(v->src);
	free(v->rec_path);
	free(v);
}

static const camera_View_vtable view_vt = {
    .destroy = destroy,
    .set_facing = set_facing,
    .set_mode = set_mode,
    .set_scan = set_scan,
    .set_codes = set_codes,
    .set_zoom = set_zoom,
    .set_pinchToZoom = set_pinch,
    .set_torch = set_torch,
    .set_flash = set_flash,
    .set_exposure = set_exposure,
    .set_tapToFocus = set_tap,
    .set_videoQuality = set_video_quality,
    .set_photoQuality = set_photo_quality,
    .set_mirror = set_mirror,
    .set_frameRate = set_frame_rate,
    .set_frameSize = set_frame_size,
    .measure = measure,
};

static view *new_view(camera_View_events events, sheet *s) {
	static unsigned serials;
	view *v = calloc(1, sizeof *v);
	v->base.vt = &view_vt;
	v->events = events;
	v->serial = ++serials;
	v->sheet = s;
	v->front = s != NULL;
	v->zoom = 1;
	v->pinch_on = true;
	v->quality = camera_VideoQuality_fullHD;
	v->ctl.fd = -1;
	v->frames = (camera_frames){.view = v->serial, .size = 480, .started = camera_now_ms()};
	v->picture = G(gtk_picture_new)();
	G(g_object_ref_sink)(v->picture);
	G(gtk_picture_set_content_fit)((GtkPicture *)v->picture, 2);  // GTK_CONTENT_FIT_COVER
	G(gtk_picture_set_can_shrink)((GtkPicture *)v->picture, true);
	G(gtk_widget_add_css_class)(v->picture, "asuka-camera");
	v->map_handlers[0] = g_signal_connect(v->picture, "map", on_map, v);
	v->map_handlers[1] = g_signal_connect(v->picture, "unmap", on_unmap, v);
	if (!s && G(gtk_gesture_zoom_new) && G(gtk_event_controller_scroll_new)) {
		// A pinch (a touchscreen's, a touchpad's), or Ctrl and the wheel.
		v->pinch = G(gtk_gesture_zoom_new)();
		g_signal_connect(v->pinch, "begin", pinch_begin, v);
		g_signal_connect(v->pinch, "scale-changed", pinch_scale, v);
		G(gtk_widget_add_controller)(v->picture, (GtkEventController *)v->pinch);
		v->wheel = G(gtk_event_controller_scroll_new)(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
		g_signal_connect(v->wheel, "scroll", wheel, v);
		G(gtk_widget_add_controller)(v->picture, v->wheel);
	}
	v->base.widget = v->picture;
	v->next = views;
	views = v;
	return v;
}

camera_View *camera_make_View(camera_View_events events) {
	if (!asuka_linux() || !G(gtk_picture_new)) return NULL;
	return &new_view(events, NULL)->base;
}

// ---- The take dialog ----

struct sheet {
	unsigned serial;
	camera_pending *reply;
	GtkWidget *dialog, *button, *cancel;
	view *view;
	gulong closed;
};

static sheet *taking;              // the dialog shown
static camera_pending *take_wait;  // a take waiting on the camera

// sheet_finish closes the dialog and answers: the photo, an error, or
// "" (cancelled; err and path NULL).
static void sheet_finish(sheet *s, const char *path, const char *err) {
	if (!s || taking != s) return;
	taking = NULL;
	G(g_signal_handler_disconnect)(s->dialog, s->closed);
	destroy(&s->view->base);
	G(adw_dialog_force_close)((AdwDialog *)s->dialog);
	G(g_object_unref)(s->dialog);
	if (err) camera_fail(&s->reply, err);
	else camera_answer(&s->reply, path ? path : "");
	free(s);
}

static void sheet_failed(sheet *s, const char *msg) { sheet_finish(s, NULL, msg); }

static unsigned sheet_serial(sheet *s) { return s ? s->serial : 0; }

// sheet_photo is the photo the dialog of serial took (NULL: none saved).
static void sheet_photo(unsigned serial, const char *path) {
	if (!taking || taking->serial != serial) {
		if (path) unlink(path);  // cancelled since
		return;
	}
	sheet_finish(taking, path, path ? NULL : "couldn't save the photo");
}

static void sheet_take(GtkWidget *b, gpointer p) {
	sheet *s = p;
	if (taking != s) return;
	view *v = s->view;
	if (!v->pipe) return;  // not showing yet
	G(gtk_widget_set_sensitive)(s->button, false);
	v->snap = true;
}

static void sheet_cancel(GtkWidget *b, gpointer p) { sheet_finish(p, NULL, NULL); }
static void sheet_closed(GtkWidget *d, gpointer p) { sheet_finish(p, NULL, NULL); }

// words are the dialog's, in the user's language (LANGUAGE's first,
// LC_ALL, LC_MESSAGES, LANG).
static const camera_word *words(void) {
	static char bufs[4][64];
	const char *ls[4];
	size_t n = 0;
	const char *keys[] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
	for (int i = 0; i < 4; i++) {
		const char *v = getenv(keys[i]);
		if (!v || !*v) continue;
		snprintf(bufs[n], sizeof bufs[n], "%.*s", (int)strcspn(v, ":"), v);  // LANGUAGE: "de:en"
		if (!*bufs[n] || !strcmp(bufs[n], "C") || !strcmp(bufs[n], "POSIX") || !strncmp(bufs[n], "C.", 2)) continue;
		ls[n] = bufs[n];
		n++;
	}
	return camera_words_for(ls, n);
}

static void show_sheet(const char *src) {
	static unsigned serials;
	sheet *s = calloc(1, sizeof *s);
	s->serial = ++serials;
	camera_move(&take_wait, &s->reply);
	const camera_word *words_ = words();
	s->dialog = (GtkWidget *)G(adw_dialog_new)();
	G(g_object_ref_sink)(s->dialog);
	G(adw_dialog_set_title)((AdwDialog *)s->dialog, words_->take);
	G(adw_dialog_set_content_width)((AdwDialog *)s->dialog, 640);
	G(adw_dialog_set_content_height)((AdwDialog *)s->dialog, 532);
	GtkWidget *tv = G(adw_toolbar_view_new)();
	GtkWidget *bar = G(adw_header_bar_new)();
	G(adw_header_bar_set_show_start_title_buttons)((AdwHeaderBar *)bar, false);
	G(adw_header_bar_set_show_end_title_buttons)((AdwHeaderBar *)bar, false);
	s->cancel = G(gtk_button_new)();
	G(gtk_button_set_label)((GtkButton *)s->cancel, words_->cancel);
	G(adw_header_bar_pack_start)((AdwHeaderBar *)bar, s->cancel);
	s->button = G(gtk_button_new)();
	G(gtk_button_set_label)((GtkButton *)s->button, words_->take);
	G(gtk_widget_add_css_class)(s->button, "suggested-action");
	G(adw_header_bar_pack_end)((AdwHeaderBar *)bar, s->button);
	G(adw_toolbar_view_add_top_bar)((AdwToolbarView *)tv, bar);
	s->view = new_view((camera_View_events){0}, s);
	s->view->src = asuka_strdup(src);
	G(adw_toolbar_view_set_content)((AdwToolbarView *)tv, s->view->picture);
	G(adw_dialog_set_child)((AdwDialog *)s->dialog, tv);
	if (G(adw_dialog_set_default_widget)) G(adw_dialog_set_default_widget)((AdwDialog *)s->dialog, s->button);  // Return
	g_signal_connect(s->button, "clicked", sheet_take, s);
	g_signal_connect(s->cancel, "clicked", sheet_cancel, s);
	s->closed = g_signal_connect(s->dialog, "closed", sheet_closed, s);  // Escape, too
	taking = s;
	G(adw_dialog_present)((AdwDialog *)s->dialog, asuka_desktop_window(0));
}

static void take_opened(void *ctx, const char *src, int code, const char *msg) {
	if (!take_wait) return;  // cancelled
	if (code == E_NO_CAMERA) camera_fail_declared(&take_wait, camera_CameraError_noCamera);
	else if (code == E_DENIED) camera_fail_declared(&take_wait, camera_CameraError_notAllowed);
	else if (code != E_OK) camera_fail(&take_wait, error_text(code, msg));
	else show_sheet(src);
}

static void take_cancelled(void *ctx) {
	camera_open_cancel(&take_wait);
	if (taking) sheet_finish(taking, NULL, NULL);
}

// ---- The camera's calls ----

static void c_take(camera_Camera *self, asuka_call *call) {
	if (taking) sheet_finish(taking, NULL, NULL);  // one at a time
	if (take_wait) camera_answer(&take_wait, "");
	if (!asuka_linux() || !G(adw_dialog_new)) {
		asuka_error e = {0};
		asuka_error_unsupported(&e, "the camera's dialog needs libadwaita 1.5");
		asuka_call_fail(call, &e);
		return;
	}
	camera_wait(call, camera_Camera_take_done, &take_wait, take_cancelled, NULL);
	camera_open(take_opened, &take_wait);
}

static void c_capture(camera_Camera *self, int64_t max_size, double quality, asuka_call *call) {
	camera_pending *w = NULL;
	camera_wait(call, camera_Camera_capture_done, &w, NULL, NULL);
	view *v = current;
	if (!v) {
		camera_fail_declared(&w, camera_CameraError_noView);
		return;
	}
	if (!v->pipe) {
		camera_fail(&w, "the camera isn't showing");
		return;
	}
	camera_fail(&v->capturing, "another photo was taken");
	v->cap_max = max_size > 0 ? max_size : 0;
	v->cap_quality = quality;
	camera_move(&w, &v->capturing);
}

static void stop_recording(view *v) {
	if (!v->recording || v->stopping || !v->pipe) return;
	v->stopping = true;
	rec_timers_stop(v);
	G(gst_element_send_event)(v->pipe, G(gst_event_new_eos)());
}

static gboolean rec_timeout(gpointer p) {
	view *v = p;
	v->rec_timer = 0;
	stop_recording(v);
	return false;
}

// rec_bytes reads the recording's file as it grows: it ends before it
// would pass maxBytes (at the rate it's written).
#define BYTES_EVERY 250

static gboolean rec_bytes(gpointer p) {
	view *v = p;
	struct stat st;
	if (!v->recording || v->stopping || !v->rec_path || stat(v->rec_path, &st) != 0) return true;
	int64_t size = (int64_t)st.st_size;
	double rate = (double)(size - v->rec_last_size) * 8 * 1000 / BYTES_EVERY;
	v->rec_last_size = size;
	if (camera_record_full(size, v->rec_max_bytes, rate > 1e6 ? rate : 1e6, BYTES_EVERY)) {
		v->bytes_timer = 0;
		stop_recording(v);
		return false;
	}
	return true;
}

// rec_cancelled: the task that records went; the recording ends.
static void rec_cancelled(void *p) {
	if (p && p == current) stop_recording(p);
}

static void c_record(camera_Camera *self, bool mute, int64_t max, int64_t max_bytes, asuka_call *call) {
	camera_pending *w = NULL;
	view *v = current;
	camera_wait(call, camera_Camera_record_done, &w, rec_cancelled, v);
	const char *why = NULL;
	encoding enc;
	if (!v) camera_fail_declared(&w, camera_CameraError_noView);
	else if (!v->pipe) camera_fail(&w, "the camera isn't showing");
	else if (!v->video) camera_fail(&w, "the camera view isn't in video mode");
	else if (v->recording) camera_fail(&w, "already recording");
	else if (!encoders(mute, &enc, &why)) camera_fail(&w, why);
	if (!w) return;
	free(v->rec_path);
	v->rec_path = camera_file(".mp4");
	if (!v->rec_path) {
		camera_fail(&w, "the recording failed: no file in the caches");
		return;
	}
	camera_move(&w, &v->rec_reply);
	v->recording = true;
	v->stopping = false;
	v->mute = mute;
	v->rec_mirror = v->mirror;
	v->rec_max_bytes = max_bytes > 0 ? max_bytes : 0;
	v->rec_last_size = 0;
	start(v);
	if (v->recording && max > 0) v->rec_timer = G(g_timeout_add_full)(G_PRIORITY_DEFAULT, (guint)(max / 1000000), rec_timeout, v, NULL);
	if (v->recording && v->rec_max_bytes) v->bytes_timer = G(g_timeout_add_full)(G_PRIORITY_DEFAULT, BYTES_EVERY, rec_bytes, v, NULL);
}

static void c_stop_recording(camera_Camera *self, asuka_error *err) {
	if (current) stop_recording(current);
}

// focus: a computer's camera focuses as it does (its own autofocus, if
// it has one); none Video4Linux or PipeWire reaches focuses at a point.
static void c_focus(camera_Camera *self, double x, double y, asuka_error *err) {}

// ---- The permission ----
//
// The camera portal asks the user once, and its permission store keeps
// the answer: the "devices" table's "camera", "yes" or "no" for an app's
// ID (a Flatpak's; "" for apps outside a sandbox, in older portals).
// Without a portal there's no one to ask: the camera's node is open to
// the user, allowed.

#define PERMISSIONS "org.freedesktop.impl.portal.PermissionStore"
#define PERMISSIONS_PATH "/org/freedesktop/impl/portal/PermissionStore"

static bool sandboxed(void) {
	const char *f = getenv("FLATPAK_ID");
	struct stat st;
	return (f && *f) || stat("/.flatpak-info", &st) == 0;
}

static const char *app_id(void) {
	const char *f = getenv("FLATPAK_ID");
	return f && *f ? f : asuka_desktop_app_id();
}

static bool test_source(void) {
	const char *test = getenv("ASUKA_CAMERA_SOURCE");
	return test && *test;
}

enum { STORE_NONE, STORE_UNSET, STORE_YES, STORE_NO };

// store is what the permission store said for the app, last time it was
// asked: STORE_NONE when it can't be read (no store, or a sandbox's bus
// that doesn't reach it), STORE_UNSET when it has no answer (no table
// yet: no app was asked). The store is asked without waiting (the main
// thread never blocks on the bus): as the camera's made, and again each
// time access reads what it said, for the next read. lookups counts the
// answers.
static int store = STORE_NONE;
static bool store_known, looking;
static unsigned lookups;

static void looked_up(GObject *src, GAsyncResult *res, gpointer p) {
	GError *e = NULL;
	GVariant *r = G(g_dbus_connection_call_finish)((GDBusConnection *)src, res, &e);
	int say = STORE_UNSET;
	if (!r) {
		bool unset = e && e->message && strstr(e->message, "org.freedesktop.portal.Error.NotFound");
		if (e) G(g_error_free)(e);
		say = unset ? STORE_UNSET : STORE_NONE;
	} else {
		GVariant *perms = G(g_variant_get_child_value)(r, 0);
		const char *ids[] = {app_id(), sandboxed() ? NULL : ""};
		for (size_t i = 0; i < 2 && say == STORE_UNSET; i++) {
			if (!ids[i]) continue;
			GVariant *v = G(g_variant_lookup_value)(perms, ids[i], G_VARIANT_TYPE("as"));
			if (!v) continue;
			if (G(g_variant_n_children)(v) > 0) {
				GVariant *c = G(g_variant_get_child_value)(v, 0);
				const char *word = G(g_variant_get_string)(c, NULL);
				if (word && !strcmp(word, "yes")) say = STORE_YES;
				else if (word && !strcmp(word, "no")) say = STORE_NO;
				G(g_variant_unref)(c);
			}
			G(g_variant_unref)(v);
		}
		G(g_variant_unref)(perms);
		G(g_variant_unref)(r);
	}
	store = say;
	store_known = true;
	looking = false;
	lookups++;
}

// look_up asks the store again, unless it's being asked.
static void look_up(void) {
	if (looking) return;
	GDBusConnection *conn = asuka_linux() ? asuka_linux_bus(G_BUS_TYPE_SESSION, NULL) : NULL;
	if (!conn) {
		store = STORE_NONE;
		store_known = true;
		lookups++;
		return;
	}
	looking = true;
	G(g_dbus_connection_call)(conn, PERMISSIONS, PERMISSIONS_PATH, PERMISSIONS, "Lookup", G(g_variant_new)("(ss)", "devices", "camera"),
	                          G_VARIANT_TYPE("(a{sas}v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, looked_up, NULL);
}

static const char *access_now(void) {
	if (test_source()) return "allowed";  // nothing's asked for it
	look_up();
	// Before the store's first answer: what this process learned, else
	// not asked.
	if (!store_known) return told ? told : "notAsked";
	if (store == STORE_YES) return "allowed";
	if (store == STORE_NO) return "denied";
	if (told) return told;
	if (store == STORE_NONE && !sandboxed()) return "allowed";  // no portal's store: no portal to ask
	return "notAsked";
}

static char *c_access(camera_Camera *self, asuka_error *err) { return asuka_strdup(access_now()); }

// ask_access asks the camera portal (AccessCamera: the user's asked
// once, the store's answer after) and gives fn(ctx) the answer: 0 is
// allowed; else the user said no (or closed the question). Without a
// camera portal nothing asks: allowed.
typedef void (*access_fn)(void *ctx, const char *answer);

typedef struct {
	access_fn fn;
	void *ctx;
	GDBusConnection *conn;
} asking;

static void asked(asking *a, const char *answer) {
	told = answer;
	a->fn(a->ctx, answer);
	free(a);
}

static void portal_answered(void *ctx, guint32 code, GVariant *results) { asked(ctx, code == 0 ? "allowed" : "denied"); }

static void request_presence(GObject *src, GAsyncResult *res, gpointer p) {
	asking *a = p;
	if (!present(a->conn, res)) {
		asked(a, "allowed");
		return;
	}
	portal_request(a->conn, "org.freedesktop.portal.Camera", "AccessCamera", portal_answered, a);
}

static void ask_access(access_fn fn, void *ctx) {
	asking *a = calloc(1, sizeof *a);
	*a = (asking){fn, ctx, test_source() || !asuka_linux() ? NULL : asuka_linux_bus(G_BUS_TYPE_SESSION, NULL)};
	if (!a->conn) {
		asked(a, "allowed");
		return;
	}
	ask_presence(a->conn, request_presence, a);
}

// request_done answers requestAccess, waiting in ctx (a slot of its own,
// emptied if its task went first).
static void request_done(void *ctx, const char *answer) {
	camera_pending **w = ctx;
	camera_answer(w, answer);
	free(w);
}

static void c_request_access(camera_Camera *self, asuka_call *call) {
	camera_pending **w = calloc(1, sizeof *w);
	camera_wait(call, camera_Camera_requestAccess_done, w, NULL, NULL);
	ask_access(request_done, w);
}

static void c_release(camera_Camera *self) {}  // the one camera lives with the app

static const camera_Camera_vtable camera_vt = {.release = c_release,
                                               .take = c_take,
                                               .capture = c_capture,
                                               .record = c_record,
                                               .stopRecording = c_stop_recording,
                                               .focus = c_focus,
                                               .access = c_access,
                                               .requestAccess = c_request_access};

camera_Camera *camera_make_Camera(asuka_error *err) {
	static camera_Camera cam = {&camera_vt};
	if (!store_known && !test_source()) look_up();  // so access has the store's answer by the time it's read
	return &cam;
}
