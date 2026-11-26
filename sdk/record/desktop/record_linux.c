// platform/record on Linux: GStreamer, a pipeline from the default source
// (PipeWire's, PulseAudio's) through an AAC encoder into an MPEG-4 file,
// its level element's messages the meter, its bus heard on the main
// thread (GLib's loop, the host's). A desktop app records out of sight
// anyway: background and title change nothing. macOS runs the package's
// Swift; video is platform/camera's.
//
// ASUKA_RECORD_SOURCE, a GStreamer source's description, stands in for
// the microphone (tests: "audiotestsrc is-live=true").
#include "asuka_linux.h"
#include "record.asuka.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	GstElement *pipe;
	GstBus *bus;
	gulong handlers[3];
	char *path;
	asuka_call *call;  // record's, until it answers
	uint8_t level;
	bool paused, stopping;
	int64_t kept;   // ns recorded before the last start
	int64_t since;  // recording since (monotonic ns; 0: paused)
	guint timer;
} recording;

static recording *rec;

static int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int64_t elapsed(recording *r) { return r->since ? r->kept + now_ns() - r->since : r->kept; }

// mkdirs makes dir and its parents.
static bool mkdirs(const char *dir) {
	char *p = asuka_strdup(dir);
	for (char *q = p + 1; *q; q++) {
		if (*q != '/') continue;
		*q = 0;
		mkdir(p, 0755);
		*q = '/';
	}
	bool ok = mkdir(p, 0755) == 0 || errno == EEXIST;
	free(p);
	return ok;
}

// new_file is a new file in the app's caches' asuka-audio.
static char *new_file(asuka_error *err) {
	char *dir = asuka_sprintf("%s/asuka-audio", asuka_desktop_cache_dir());
	char *path = asuka_sprintf("%s/XXXXXX.m4a", dir);
	int fd = mkdirs(dir) ? mkstemps(path, 4) : -1;
	free(dir);
	if (fd < 0) {
		asuka_error_failed(err, "errno", errno, "record: %s", strerror(errno));
		free(path);
		return NULL;
	}
	close(fd);
	return path;
}

// level_of is a level message's loudest channel's RMS as iOS's meter
// reads: 0 below -60 dB, else its amplitude, 0 to 255. Its RMS values
// (dB, a channel each) are an array: "rms=(GValueArray)< -20.1, -20.3 >"
// ("(double){ ... }" in some versions).
static uint8_t level_of(const char *text) {
	const char *p = strstr(text, "rms=(");
	if (!p || !(p = strchr(p, ')'))) return 0;
	p++;
	while (*p == ' ') p++;
	if (*p != '<' && *p != '{') return 0;
	p++;
	double db = -INFINITY;
	while (*p && *p != '>' && *p != '}') {
		char *end;
		double v = strtod(p, &end);
		if (end == p) {
			p++;
			continue;
		}
		if (v > db) db = v;
		p = end;
	}
	if (db <= -60 || isnan(db)) return 0;
	double a = pow(10, db / 20);
	return (uint8_t)((a < 0 ? 0 : a > 1 ? 1 : a) * 255);
}

static void release(recording *r) {
	for (int i = 0; i < 3; i++)
		if (r->handlers[i]) G(g_signal_handler_disconnect)(r->bus, r->handlers[i]), r->handlers[i] = 0;
	if (r->bus) {
		G(gst_bus_remove_signal_watch)(r->bus);
		G(gst_object_unref)(r->bus);
		r->bus = NULL;
	}
	if (r->pipe) {
		G(gst_element_set_state)(r->pipe, GST_STATE_NULL);
		G(gst_object_unref)(r->pipe);
		r->pipe = NULL;
	}
}

// finish answers record: the file, or why it failed (msg).
static void finish(recording *r, const char *failed) {
	if (rec == r) rec = NULL;
	if (r->timer) G(g_source_remove)(r->timer), r->timer = 0;
	release(r);
	asuka_call *c = r->call;
	r->call = NULL;
	if (c) {
		if (failed) {
			asuka_error e = {0};
			asuka_error_failed(&e, "gstreamer", 0, "the recording failed: %s", failed);
			asuka_call_fail(c, &e);
			unlink(r->path);
		} else {
			record_Recorder_record_done(c, asuka_strdup(r->path));
		}
	}
	free(r->path);
	free(r);
}

// stop ends the recording: the encoder and the muxer finish the file
// (the end of the stream), then record answers with it.
static void stop(recording *r) {
	if (r->stopping) return;
	r->stopping = true;
	if (r->timer) G(g_source_remove)(r->timer), r->timer = 0;
	if (r->since) r->kept += now_ns() - r->since, r->since = 0;
	// A paused pipeline doesn't carry the end through: playing, it does
	// at once.
	if (r->paused) G(gst_element_set_state)(r->pipe, GST_STATE_PLAYING);
	G(gst_element_send_event)(r->pipe, G(gst_event_new_eos)());
}

static void on_element(GstBus *bus, gpointer msg, gpointer d) {
	recording *r = d;
	const GstStructure *s = G(gst_message_get_structure)(msg);
	if (!s || strcmp(G(gst_structure_get_name)(s), "level") != 0) return;
	gchar *text = G(gst_structure_to_string)(s);
	r->level = level_of(text);
	G(g_free)(text);
}

static void on_eos(GstBus *bus, gpointer msg, gpointer d) { finish(d, NULL); }

static void on_error(GstBus *bus, gpointer msg, gpointer d) {
	GError *e = NULL;
	gchar *debug = NULL;
	G(gst_message_parse_error)(msg, &e, &debug);
	char *why = asuka_strdup(e && e->message ? e->message : "GStreamer's error");
	if (e) G(g_error_free)(e);
	G(g_free)(debug);
	finish(d, why);
	free(why);
}

static gboolean on_timer(gpointer d) {
	recording *r = d;
	r->timer = 0;
	stop(r);
	return false;
}

// The task that waits went: the recording ends (its file kept, as stop
// keeps it), and record answers cancelled.
static void on_cancel(void *d) {
	recording *r = d;
	asuka_call *c = r->call;
	r->call = NULL;
	if (c) {
		asuka_error e = {0};
		asuka_error_cancelled(&e);
		asuka_call_fail(c, &e);
	}
	stop(r);
}

// AAC encoders GStreamer may have, best first.
static const char *encoders[] = {"fdkaacenc", "avenc_aac", "voaacenc", "faac"};

static void r_record(record_Recorder *self, record_Quality quality, int64_t max_duration, const char *path, bool background,
                     const char *title, asuka_call *call) {
	asuka_error e = {0};
	if (rec) {
		asuka_error_declared(&e, record_RecordError_busy);
		asuka_call_fail(call, &e);
		return;
	}
	if (!asuka_linux_gst()) {
		asuka_error_unsupported(&e, "record: GStreamer isn't installed");
		asuka_call_fail(call, &e);
		return;
	}
	const char *enc = NULL;
	for (size_t i = 0; i < sizeof encoders / sizeof *encoders && !enc; i++) {
		gpointer f = G(gst_element_factory_find)(encoders[i]);
		if (f) enc = encoders[i], G(gst_object_unref)(f);
	}
	if (!enc) {
		asuka_error_unsupported(&e, "record: no AAC encoder (install GStreamer's libav or bad plugins)");
		asuka_call_fail(call, &e);
		return;
	}
	char *file;
	if (path && *path) {
		file = asuka_strdup(path);
		char *slash = strrchr(file, '/');
		if (slash && slash != file) {
			*slash = 0;
			mkdirs(file);
			*slash = '/';
		}
	} else if (!(file = new_file(&e))) {
		asuka_call_fail(call, &e);
		return;
	}
	bool music = quality == record_Quality_music;
	const char *src = getenv("ASUKA_RECORD_SOURCE");
	char *desc = asuka_sprintf("%s ! queue ! audioconvert ! audioresample ! %s ! level name=meter interval=100000000 post-messages=true ! "
	                           "%s name=enc ! aacparse ! mp4mux ! filesink name=sink",
	                           src && *src ? src : "autoaudiosrc", music ? "audio/x-raw,rate=44100,channels=2" : "audio/x-raw,rate=22050,channels=1",
	                           enc);
	GError *gerr = NULL;
	GstElement *pipe = G(gst_parse_launch)(desc, &gerr);
	free(desc);
	if (!pipe) {
		asuka_error_failed(&e, "gstreamer", 0, "record: %s", gerr && gerr->message ? gerr->message : "no pipeline");
		if (gerr) G(g_error_free)(gerr);
		free(file);
		asuka_call_fail(call, &e);
		return;
	}
	if (gerr) G(g_error_free)(gerr);  // a warning: an element it could make without
	G(g_object_ref_sink)(pipe);
	GstElement *el = G(gst_bin_get_by_name)(pipe, "enc");
	if (el) {
		G(g_object_set)(el, "bitrate", (gint)(music ? 128000 : 64000), NULL);
		G(gst_object_unref)(el);
	}
	el = G(gst_bin_get_by_name)(pipe, "sink");
	if (el) {
		G(g_object_set)(el, "location", file, NULL);
		G(gst_object_unref)(el);
	}
	recording *r = calloc(1, sizeof *r);
	r->pipe = pipe;
	r->path = file;
	r->call = call;
	r->since = now_ns();
	r->bus = G(gst_element_get_bus)(pipe);
	G(gst_bus_add_signal_watch)(r->bus);
	r->handlers[0] = g_signal_connect(r->bus, "message::element", on_element, r);
	r->handlers[1] = g_signal_connect(r->bus, "message::eos", on_eos, r);
	r->handlers[2] = g_signal_connect(r->bus, "message::error", on_error, r);
	if (G(gst_element_set_state)(pipe, GST_STATE_PLAYING) == 0) {  // GST_STATE_CHANGE_FAILURE
		r->call = NULL;
		release(r);
		unlink(r->path);
		free(r->path);
		free(r);
		asuka_error_failed(&e, "gstreamer", 0, "record: the microphone can't record");
		asuka_call_fail(call, &e);
		return;
	}
	rec = r;
	if (max_duration > 0) {
		int64_t ms = max_duration / 1000000;
		r->timer = G(g_timeout_add_full)(0, (guint)(ms > 0 ? ms : 1), on_timer, r, NULL);
	}
	asuka_call_on_cancel(call, on_cancel, r);
}

static void r_stop(record_Recorder *self, asuka_error *err) {
	if (rec) stop(rec);
}

static void r_pause(record_Recorder *self, asuka_error *err) {
	recording *r = rec;
	if (!r || r->paused || r->stopping) return;
	r->paused = true;
	r->kept += now_ns() - r->since;
	r->since = 0;
	r->level = 0;
	G(gst_element_set_state)(r->pipe, GST_STATE_PAUSED);
}

static void r_resume(record_Recorder *self, asuka_error *err) {
	recording *r = rec;
	if (!r || !r->paused || r->stopping) return;
	r->paused = false;
	r->since = now_ns();
	G(gst_element_set_state)(r->pipe, GST_STATE_PLAYING);
}

static record_Status r_status(record_Recorder *self, asuka_error *err) {
	recording *r = rec;
	if (!r) return (record_Status){0};
	return (record_Status){
	    .level = r->paused ? 0 : r->level / 255.0,
	    .recording = !r->paused && !r->stopping,
	    .elapsed = elapsed(r),
	};
}

// Linux has no microphone permission to ask for: no portal guards it,
// the user's session reaches the sound server, and a Flatpak has its
// socket from its manifest (--socket=pulseaudio). Allowed, both ways.
static char *r_access(record_Recorder *self, asuka_error *err) { return asuka_strdup("allowed"); }

static void r_request_access(record_Recorder *self, asuka_call *call) { record_Recorder_requestAccess_done(call, asuka_strdup("allowed")); }

static void r_release(record_Recorder *self) {}  // the one recorder lives with the app

static const record_Recorder_vtable vt = {.release = r_release,
                                          .record = r_record,
                                          .stop = r_stop,
                                          .pause = r_pause,
                                          .resume = r_resume,
                                          .status = r_status,
                                          .access = r_access,
                                          .requestAccess = r_request_access};

record_Recorder *record_make_Recorder(asuka_error *err) {
	static record_Recorder recorder = {&vt};
	return &recorder;
}
