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
// ASUKA_CAMERA_SOURCE, if set, is the camera's source as gst-launch
// writes it ("videotestsrc is-live=true"), and ASUKA_CAMERA_SOUND the
// recording's sound's (autoaudiosrc's place): for tests.
#include "asuka_linux.h"
#include "camera.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	grant.src = asuka_sprintf("v4l2src device=/dev/%s ! decodebin", dev);
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

// ---- The view ----

typedef struct sheet sheet;

typedef struct {
	camera_View base;
	camera_View_events events;
	sheet *sheet;  // the take dialog's (never the current one), or NULL
	GtkWidget *picture;
	GstElement *pipe, *sink;
	GstBus *bus;
	gulong bus_handlers[3], map_handlers[2];
	guint tick, idle, rec_timer;
	bool front, video, scan;
	bool mapped, opening, destroyed, recording, stopping, mute;
	bool snap;  // the dialog's photo is taken from the next frame
	camera_kinds kinds;
	camera_seen seen;
	char *src;  // the camera's source, once open
	camera_pending *capturing, *rec_reply;
	char *rec_path;
} view;

// current is the view on screen capture and record use.
static view *current;

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

static void record_failed(view *v, const char *why) {
	v->recording = v->stopping = false;
	if (v->rec_timer) G(g_source_remove)(v->rec_timer), v->rec_timer = 0;
	if (v->rec_reply) {
		char *m = asuka_sprintf("the recording failed: %s", why);
		camera_fail(&v->rec_reply, m);
		free(m);
	}
}

// description is the pipeline: the camera, through a tee, to the
// preview's appsink (mirrored), to zbar with scan, and to the encoders
// and a file while recording.
static char *description(view *v, const char **why) {
	asuka_writer b = {0};
#define PUT(s) asuka_writer_put(&b, (s), strlen(s))
	PUT(v->src);
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
		char *s = asuka_sprintf(" t. ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=3000000000 ! videoconvert ! %s ! %s name=mux ! filesink name=file",
		                        enc.video, enc.mux);
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

typedef struct {
	camera_pending *reply;
	unsigned sheet;  // or the photo of the take dialog of this serial
	char *path;
	int w, h;
	uint8_t *rgb;
	bool ok;
} photo;

static void sheet_photo(unsigned serial, const char *path);

static void photo_saved(void *p) {
	photo *ph = p;
	if (ph->sheet) sheet_photo(ph->sheet, ph->ok ? ph->path : NULL);
	else if (ph->ok) camera_answer(&ph->reply, ph->path);
	else camera_fail(&ph->reply, "couldn't save the photo");
	free(ph->path);
	free(ph);
}

// save_photo writes the photo as a JPEG. Not on the main thread.
static void save_photo(void *p) {
	photo *ph = p;
	ph->path = camera_file(".jpg");
	if (ph->path && asuka_linux_load(ASUKA_LIB_PIXBUF) && G(gdk_pixbuf_new_from_data) && G(gdk_pixbuf_save)) {
		GdkPixbuf *pb = G(gdk_pixbuf_new_from_data)(ph->rgb, 0, false, 8, ph->w, ph->h, ph->w * 3, NULL, NULL);  // GDK_COLORSPACE_RGB
		GError *e = NULL;
		ph->ok = pb && G(gdk_pixbuf_save)(pb, ph->path, "jpeg", &e, "quality", "90", NULL);
		if (e) G(g_error_free)(e);
		if (pb) G(g_object_unref)(pb);
		if (!ph->ok) unlink(ph->path);
	}
	free(ph->rgb);
	asuka_desktop_post(photo_saved, ph);
}

// pull_frame shows the camera's latest frame, if there's a new one, and
// gives it to a waiting capture.
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
			GBytes *bytes = G(g_bytes_new)(info.data, stride * (size_t)h);
			GdkTexture *t = G(gdk_memory_texture_new)(w, h, 5, bytes, stride);  // GDK_MEMORY_R8G8B8A8
			G(g_bytes_unref)(bytes);
			G(gtk_picture_set_paintable)((GtkPicture *)v->picture, (GdkPaintable *)t);
			G(g_object_unref)(t);
			if (v->capturing || v->snap) {
				// The preview's mirrored: the photo isn't.
				photo *ph = calloc(1, sizeof *ph);
				ph->w = w, ph->h = h;
				ph->rgb = malloc((size_t)w * h * 3);
				for (int y = 0; y < h; y++) {
					const uint8_t *row = info.data + y * stride;
					uint8_t *out = ph->rgb + (size_t)y * w * 3;
					for (int x = 0; x < w; x++) memcpy(out + x * 3, row + (size_t)(w - 1 - x) * 4, 3);
				}
				camera_move(&v->capturing, &ph->reply);
				if (v->snap) ph->sheet = sheet_serial(v->sheet);
				v->snap = false;
				if (!asuka_desktop_spawn(save_photo, ph)) {
					free(ph->rgb);
					ph->ok = false;
					photo_saved(ph);
				}
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

static void set_facing(camera_View *self, camera_Facing f) { ((view *)self)->front = f == camera_Facing_front; }  // the same camera
static void set_mode(camera_View *self, camera_Mode m) {
	view *v = (view *)self;
	v->video = m == camera_Mode_video;
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
	camera_fail(&v->capturing, "the camera isn't showing");
	camera_open_cancel(v);
	for (int i = 0; i < 2; i++)
		if (v->map_handlers[i]) G(g_signal_handler_disconnect)(v->picture, v->map_handlers[i]);
	if (v->idle) G(g_source_remove)(v->idle);
	if (v->rec_timer) G(g_source_remove)(v->rec_timer);
	G(g_object_unref)(v->picture);
	camera_kinds_free(&v->kinds);
	camera_seen_free(&v->seen);
	free(v->src);
	free(v->rec_path);
	free(v);
}

static const camera_View_vtable view_vt = {
    .destroy = destroy, .set_facing = set_facing, .set_mode = set_mode, .set_scan = set_scan, .set_codes = set_codes, .measure = measure};

static view *new_view(camera_View_events events, sheet *s) {
	view *v = calloc(1, sizeof *v);
	v->base.vt = &view_vt;
	v->events = events;
	v->sheet = s;
	v->front = s != NULL;
	v->picture = G(gtk_picture_new)();
	G(g_object_ref_sink)(v->picture);
	G(gtk_picture_set_content_fit)((GtkPicture *)v->picture, 2);  // GTK_CONTENT_FIT_COVER
	G(gtk_picture_set_can_shrink)((GtkPicture *)v->picture, true);
	G(gtk_widget_add_css_class)(v->picture, "asuka-camera");
	v->map_handlers[0] = g_signal_connect(v->picture, "map", on_map, v);
	v->map_handlers[1] = g_signal_connect(v->picture, "unmap", on_unmap, v);
	v->base.widget = v->picture;
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

static void c_capture(camera_Camera *self, asuka_call *call) {
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
	camera_move(&w, &v->capturing);
}

static void stop_recording(view *v) {
	if (!v->recording || v->stopping || !v->pipe) return;
	v->stopping = true;
	if (v->rec_timer) G(g_source_remove)(v->rec_timer), v->rec_timer = 0;
	G(gst_element_send_event)(v->pipe, G(gst_event_new_eos)());
}

static gboolean rec_timeout(gpointer p) {
	view *v = p;
	v->rec_timer = 0;
	stop_recording(v);
	return false;
}

// rec_cancelled: the task that records went; the recording ends.
static void rec_cancelled(void *p) {
	if (p && p == current) stop_recording(p);
}

static void c_record(camera_Camera *self, bool mute, int64_t max, asuka_call *call) {
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
	start(v);
	if (v->recording && max > 0) v->rec_timer = G(g_timeout_add_full)(G_PRIORITY_DEFAULT, (guint)(max / 1000000), rec_timeout, v, NULL);
}

static void c_stop_recording(camera_Camera *self, asuka_error *err) {
	if (current) stop_recording(current);
}

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
                                               .access = c_access,
                                               .requestAccess = c_request_access};

camera_Camera *camera_make_Camera(asuka_error *err) {
	static camera_Camera cam = {&camera_vt};
	if (!store_known && !test_source()) look_up();  // so access has the store's answer by the time it's read
	return &cam;
}
