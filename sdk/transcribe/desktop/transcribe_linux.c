// platform/transcribe on Linux: CMU PocketSphinx, the recognizer the
// distributions package (Debian's and Ubuntu's pocketsphinx-en-us,
// Fedora's pocketsphinx-models), on the device: its US English model,
// opened with dlopen as the host opens GTK (5's, or 5prealpha's with
// sphinxbase). GStreamer decodes a recording (decodebin: what it plays)
// or reads the default microphone (autoaudiosrc), down to 16 kHz mono,
// into an appsink a thread of the call's own reads; PocketSphinx's own
// voice activity cuts what it hears into phrases. Where it isn't
// installed, or for another language, check says so and file and listen
// fail with notAvailable, rather than sending the sound to a server.
#include "asuka_linux.h"
#include "transcribe.asuka.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { RATE = 16000 };  // what the models listen at
#define FRAMES_PER_SECOND 100.0

static char *dup(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	memcpy(p, s, n);
	return p;
}

// ---- Languages and the model ----

static char *device_language(void) {
	const char *lang = getenv("LANG");
	char *s = dup(lang && *lang ? lang : "");
	char *dot = strpbrk(s, ".@");
	if (dot) *dot = 0;
	for (char *p = s; *p; p++)
		if (*p == '_') *p = '-';
	return s;
}

static int tolower_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static char *language_of(const char *language) { return language && *language ? dup(language) : device_language(); }

// english reports whether tag is English, the language of the model
// distributions ship (en-us; C and POSIX locales are taken as English).
static bool english(const char *tag) {
	return tag[0] == 0 || strcmp(tag, "C") == 0 || strcmp(tag, "POSIX") == 0 || (tolower_ascii(tag[0]) == 'e' && tolower_ascii(tag[1]) == 'n' && (tag[2] == 0 || tag[2] == '-'));
}

static bool exists(const char *p) {
	struct stat st;
	return stat(p, &st) == 0;
}

// The model's files: its acoustic model, language model and dictionary,
// where the distributions put them.
typedef struct {
	char hmm[512], lm[512], dict[512];
} model;

static bool find_model(model *m) {
	static const char *const dirs[] = {"/usr/share/pocketsphinx/model/en-us", "/usr/local/share/pocketsphinx/model/en-us",
	                                   "/app/share/pocketsphinx/model/en-us", NULL};
	for (int i = 0; dirs[i]; i++) {
		snprintf(m->hmm, sizeof m->hmm, "%s/en-us", dirs[i]);
		snprintf(m->lm, sizeof m->lm, "%s/en-us.lm.bin", dirs[i]);
		snprintf(m->dict, sizeof m->dict, "%s/cmudict-en-us.dict", dirs[i]);
		if (exists(m->hmm) && exists(m->lm) && exists(m->dict)) return true;
	}
	return false;
}

// recognizer reports whether PocketSphinx and its English model are here.
static bool recognizer(model *m) {
	if (!asuka_linux_load(ASUKA_LIB_POCKETSPHINX) || !G(ps_init) || !G(ps_process_raw)) return false;
	if (!G(ps_config_init) && !(asuka_linux_load(ASUKA_LIB_SPHINXBASE) && G(cmd_ln_init))) return false;
	return find_model(m);
}

static ps_decoder_t *decoder(const model *m) {
	if (G(ps_config_init)) {
		ps_config_t *c = G(ps_config_init)(NULL);
		if (!c) return NULL;
		if (G(ps_default_search_args)) G(ps_default_search_args)(c);
		G(ps_config_set_str)(c, "hmm", m->hmm);
		G(ps_config_set_str)(c, "lm", m->lm);
		G(ps_config_set_str)(c, "dict", m->dict);
		G(ps_config_set_str)(c, "logfn", "/dev/null");
		ps_decoder_t *ps = G(ps_init)(c);
		G(ps_config_free)(c);
		return ps;
	}
	ps_config_t *c = G(cmd_ln_init)(NULL, G(ps_args)(), 1, "-hmm", m->hmm, "-lm", m->lm, "-dict", m->dict, "-logfn", "/dev/null", (char *)NULL);
	if (!c) return NULL;
	ps_decoder_t *ps = G(ps_init)(c);
	G(cmd_ln_free_r)(c);
	return ps;
}

static void check(transcribe_Recognizer *self, const char *language, asuka_call *call) {
	transcribe_Availability a = {.language = language_of(language)};
	model m;
	a.onDevice = english(a.language) && recognizer(&m);
	transcribe_Recognizer_check_done(call, a);
}

// ---- A recognition ----

typedef struct {
	asuka_call *call;
	char *path;  // the recording (NULL: the microphone)
	char *language;
	transcribe_fn_Heard heard;
	atomic_bool stop, finish;
} job;

static job *listening;  // main thread

static void job_free(void *ctx) {
	job *j = ctx;
	if (listening == j) listening = NULL;
	free(j->path);
	free(j->language);
	free(j);
}

static void cancel_job(void *ctx) { atomic_store(&((job *)ctx)->stop, true); }

// What's been said: the phrases' text, and their words' timing.
typedef struct {
	char *text;
	size_t len;
	transcribe_Segment *segs;
	size_t nsegs, cap;
} said;

static void append(said *s, const char *t) {
	if (!t || !*t) return;
	size_t n = strlen(t);
	s->text = realloc(s->text, s->len + n + 2);
	if (s->len) s->text[s->len++] = ' ';
	memcpy(s->text + s->len, t, n + 1);
	s->len += n;
}

// word is a segment's word as said: none for silence and noise (<s>,
// <sil>, [NOISE]), a pronunciation's number dropped ("read(2)").
static char *word(const char *w) {
	if (!w || !*w || w[0] == '<' || w[0] == '[') return NULL;
	char *s = dup(w);
	char *paren = strchr(s, '(');
	if (paren && paren != s) *paren = 0;
	return s;
}

// phrase adds the utterance just ended to s: its words, timed from
// start (seconds into the sound).
static void phrase(said *s, ps_decoder_t *ps, double start) {
	gint32 score = 0;
	ps_seg_t *seg = G(ps_seg_iter)(ps, &score);
	for (; seg; seg = G(ps_seg_next)(seg)) {
		char *w = word(G(ps_seg_word)(seg));
		if (!w) continue;
		int sf = 0, ef = 0;
		G(ps_seg_frames)(seg, &sf, &ef);
		append(s, w);
		if (s->nsegs == s->cap) {
			s->cap = s->cap ? s->cap * 2 : 16;
			s->segs = realloc(s->segs, s->cap * sizeof *s->segs);
		}
		s->segs[s->nsegs++] = (transcribe_Segment){.text = w, .start = start + sf / FRAMES_PER_SECOND,
		                                           .duration = (ef - sf + 1) / FRAMES_PER_SECOND};
	}
}

static void hear(job *j, const said *s, const char *partial, bool final) {
	said now = {.text = s->text ? dup(s->text) : NULL, .len = s->len};
	append(&now, partial);
	transcribe_Heard h = {.text = now.text ? now.text : "", .final = final};
	transcribe_fn_Heard_call(j->heard, &h);
	free(now.text);
}

static bool gst_ready(void) {
	static int ok = -1;
	if (ok < 0) ok = asuka_linux_load(ASUKA_LIB_GST) && asuka_linux_load(ASUKA_LIB_GSTAPP) && G(gst_init_check)(NULL, NULL, NULL);
	return ok == 1;
}

// bus_error is the pipeline's error, if it has one (malloc'd).
static char *bus_error(GstElement *p) {
	GstBus *bus = G(gst_element_get_bus)(p);
	GstMessage *m = bus ? G(gst_bus_pop_filtered)(bus, GST_MESSAGE_ERROR) : NULL;
	char *out = NULL;
	if (m) {
		GError *e = NULL;
		gchar *debug = NULL;
		G(gst_message_parse_error)(m, &e, &debug);
		out = dup(e && e->message ? e->message : "GStreamer failed");
		if (e) G(g_error_free)(e);
		G(g_free)(debug);
		G(gst_mini_object_unref)(m);
	}
	if (bus) G(gst_object_unref)(bus);
	return out;
}

// recognize reads the sound (the file's, or the microphone's until finish
// or the task goes) into PocketSphinx, a phrase each time it stops
// hearing speech. It answers the call.
static void recognize(job *j, ps_decoder_t *ps) {
	asuka_error err = {0};
	said s = {0};
	const char *desc = j->path ? "filesrc name=src ! decodebin ! audioconvert ! audioresample ! "
	                             "audio/x-raw,format=S16LE,rate=16000,channels=1 ! appsink name=sink sync=false"
	                           : "autoaudiosrc ! audioconvert ! audioresample ! "
	                             "audio/x-raw,format=S16LE,rate=16000,channels=1 ! appsink name=sink sync=false";
	GError *pe = NULL;
	GstElement *p = G(gst_parse_launch)(desc, &pe);
	if (!p) {
		asuka_error_failed(&err, "transcribe", 0, "transcribe: GStreamer: %s", pe && pe->message ? pe->message : "no pipeline");
		if (pe) G(g_error_free)(pe);
		goto answer;
	}
	if (j->path) {
		GstElement *src = G(gst_bin_get_by_name)(p, "src");
		G(g_object_set)(src, "location", j->path, NULL);
		G(gst_object_unref)(src);
	}
	GstAppSink *sink = (GstAppSink *)G(gst_bin_get_by_name)(p, "sink");
	if (G(gst_element_set_state)(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		char *why = bus_error(p);
		if (j->path) asuka_error_failed(&err, "transcribe", 0, "transcribe: can't read the recording: %s", why ? why : "");
		else asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);  // no microphone
		free(why);
		goto done;
	}
	uint64_t samples = 0;     // read so far
	double utt_start = 0;     // where the utterance began (seconds)
	bool speaking = false, eos_sent = false;
	G(ps_start_utt)(ps);
	for (;;) {
		if (atomic_load(&j->stop)) {
			asuka_error_cancelled(&err);
			break;
		}
		if (!j->path && atomic_load(&j->finish) && !eos_sent) {
			// Finishing: the microphone's last sound, then the end.
			G(gst_element_send_event)(p, G(gst_event_new_eos)());
			eos_sent = true;
		}
		GstSample *smp = G(gst_app_sink_try_pull_sample)(sink, GST_SECOND / 10);
		if (!smp) {
			if (G(gst_app_sink_is_eos)(sink)) break;
			char *why = bus_error(p);
			if (why) {
				if (j->path) asuka_error_failed(&err, "transcribe", 0, "transcribe: can't read the recording: %s", why);
				else asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);
				free(why);
				break;
			}
			continue;
		}
		GstBuffer *buf = G(gst_sample_get_buffer)(smp);
		GstMapInfo info;
		if (buf && G(gst_buffer_map)(buf, &info, GST_MAP_READ)) {
			size_t n = info.size / 2;
			G(ps_process_raw)(ps, (const gint16 *)info.data, n, 0, 0);
			samples += n;
			G(gst_buffer_unmap)(buf, &info);
		}
		G(gst_mini_object_unref)(smp);
		bool in = G(ps_get_in_speech)(ps) != 0;
		if (in && !speaking) {
			speaking = true;
		} else if (!in && speaking) {
			// A phrase said: its words, then the next.
			speaking = false;
			G(ps_end_utt)(ps);
			phrase(&s, ps, utt_start);
			utt_start = (double)samples / RATE;
			G(ps_start_utt)(ps);
			if (!j->path) hear(j, &s, NULL, false);
		} else if (in && !j->path) {
			gint32 score = 0;
			hear(j, &s, G(ps_get_hyp)(ps, &score), false);
		}
	}
	G(ps_end_utt)(ps);
	if (err.status == ASUKA_OK) phrase(&s, ps, utt_start);
done:
	G(gst_element_set_state)(p, GST_STATE_NULL);
	if (sink) G(gst_object_unref)(sink);
	G(gst_object_unref)(p);
answer:
	if (err.status != ASUKA_OK) {
		for (size_t i = 0; i < s.nsegs; i++) free(s.segs[i].text);
		free(s.segs);
		free(s.text);
		asuka_call_fail(j->call, &err);
	} else if (j->path) {
		transcribe_Result r = {.text = s.text ? s.text : dup(""), .segments = {s.segs, s.nsegs}, .language = dup("en-US")};
		transcribe_Recognizer_file_done(j->call, r);
	} else {
		hear(j, &s, NULL, true);
		for (size_t i = 0; i < s.nsegs; i++) free(s.segs[i].text);
		free(s.segs);
		free(s.text);
		transcribe_Recognizer_listen_done(j->call);
	}
}

static void work(void *ctx) {
	job *j = ctx;
	asuka_error err = {0};
	model m;
	if (!english(j->language) || !recognizer(&m)) {
		asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);
		asuka_call_fail(j->call, &err);
	} else if (!gst_ready()) {
		asuka_error_unsupported(&err, "transcribe: reading sound needs GStreamer");
		asuka_call_fail(j->call, &err);
	} else {
		ps_decoder_t *ps = decoder(&m);
		if (!ps) {
			asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);
			asuka_call_fail(j->call, &err);
		} else {
			recognize(j, ps);
			G(ps_free)(ps);
		}
	}
	asuka_desktop_post(job_free, j);
}

static job *start(const char *path, const char *language, transcribe_fn_Heard heard, asuka_call *call) {
	job *j = calloc(1, sizeof *j);
	j->call = call;
	j->path = path ? dup(path) : NULL;
	j->language = language_of(language);
	j->heard = heard;
	asuka_call_on_cancel(call, cancel_job, j);
	if (!asuka_desktop_spawn(work, j)) {
		asuka_call_off_cancel(call);
		asuka_error e = {0};
		asuka_error_failed(&e, "transcribe", 0, "transcribe: no thread");
		asuka_call_fail(call, &e);
		job_free(j);
		return NULL;
	}
	return j;
}

static void file(transcribe_Recognizer *self, const char *path, const char *language, asuka_call *call) {
	start(path ? path : "", language, (transcribe_fn_Heard){0}, call);
}

static void listen(transcribe_Recognizer *self, const char *language, transcribe_fn_Heard heard, asuka_call *call) {
	if (listening) {
		asuka_error e = {0};
		asuka_error_failed(&e, "transcribe", 1, "transcribe: already listening");
		asuka_call_fail(call, &e);
		return;
	}
	listening = start(NULL, language, heard, call);  // until its job's freed
}

static void finish(transcribe_Recognizer *self, asuka_error *err) {
	if (listening) atomic_store(&listening->finish, true);
}

static const transcribe_Recognizer_vtable vt = {.check = check, .file = file, .listen = listen, .finish = finish};

transcribe_Recognizer *transcribe_make_Recognizer(asuka_error *err) {
	transcribe_Recognizer *r = malloc(sizeof *r);
	r->vt = &vt;
	return r;
}
