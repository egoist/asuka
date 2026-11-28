// platform/webrtc on Windows and Linux: peer connections and their data
// channels, through libdatachannel (rtc.h: opened as the app runs). Video
// and sound aren't here: capture and the camera fail as unsupported, a
// track can't be added, and media the other peer offers is declined (its
// track removed as it comes, so the answer leaves it inactive). macOS
// runs the package's Swift.
//
// libdatachannel calls back on its own threads: what it says is copied
// and posted to the main thread, where everything else runs, and kept
// until the app reads it (events). A connection and a channel are
// counted: the app's reference, libdatachannel's (until it's deleted)
// and each post's, so nothing goes while something still points at it.
//
// libdatachannel makes its own descriptions: createOffer and
// createAnswer set this side's description (rtcSetLocalDescription) and
// give it; setLocalDescription then takes that one, as it was given (a
// changed one fails as unsupported). Candidates found before it's set
// wait for it, so they go to the other peer after the description.
// There's no ICE restart.
#include "rtc.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char no_media[] = "webrtc: video and sound need Google's WebRTC, which this build has only on iOS, macOS, Android and the web";

static char *dup(const char *s) { return asuka_strdup(s ? s : ""); }

// ---------------------------------------------------------------------
// Data channels

typedef struct cev {
	struct cev *next;
	webrtc_ChannelEvent e;
} cev;

typedef struct chan {
	webrtc_DataChannel base;
	const wrtc_lib *rtc;
	int dc;  // 0 once deleted
	atomic_int refs;
	webrtc_ChannelState state;
	webrtc_ChannelInfo info;
	// What happened, until events reads it.
	cev *head, **tail;
	asuka_call *call;
	webrtc_fn_ChannelEvent send;
} chan;

static void chan_unref(chan *c) {
	if (atomic_fetch_sub(&c->refs, 1) != 1) return;
	for (cev *e = c->head, *n; e; e = n) {
		n = e->next;
		webrtc_ChannelEvent_free(&e->e);
		free(e);
	}
	webrtc_ChannelInfo_free(&c->info);
	free(c);
}

// chan_tell gives e to the app, or keeps it until it reads (main thread).
static void chan_tell(chan *c, webrtc_ChannelEvent e) {
	if (c->call) {
		webrtc_fn_ChannelEvent_call(c->send, &e);
		webrtc_ChannelEvent_free(&e);
		return;
	}
	cev *k = malloc(sizeof *k);
	if (!k) {
		webrtc_ChannelEvent_free(&e);
		return;
	}
	k->next = NULL;
	k->e = e;
	*c->tail = k;
	c->tail = &k->next;
}

// chan_end answers events: the channel closed.
static void chan_end(chan *c) {
	asuka_call *call = c->call;
	c->call = NULL;
	if (call) {
		asuka_call_off_cancel(call);
		webrtc_DataChannel_events_done(call);
	}
}

// chan_drop deletes the channel in libdatachannel: no more of its
// callbacks (main thread).
static void chan_drop(chan *c) {
	if (!c->dc) return;
	int dc = c->dc;
	c->dc = 0;
	c->rtc->rtcDeleteDataChannel(dc);
	chan_unref(c);  // libdatachannel's
}

typedef struct {
	chan *c;
	webrtc_ChannelEventKind kind;
	bool binary;
	char *text;
	asuka_bytes data;
} chan_post;

static void chan_posted(void *p) {
	chan_post *m = p;
	chan *c = m->c;
	switch (m->kind) {
	case webrtc_ChannelEventKind_open:
		if (c->state == webrtc_ChannelState_connecting) {
			c->state = webrtc_ChannelState_open;
			chan_tell(c, (webrtc_ChannelEvent){.kind = webrtc_ChannelEventKind_open, .text = dup("")});
		}
		break;
	case webrtc_ChannelEventKind_message:
		if (c->state != webrtc_ChannelState_closed) {
			chan_tell(c, (webrtc_ChannelEvent){.kind = webrtc_ChannelEventKind_message, .binary = m->binary, .text = m->text ? m->text : dup(""), .data = m->data});
			m->text = NULL;
			m->data = (asuka_bytes){0};
		}
		break;
	default:
		if (c->state != webrtc_ChannelState_closed) {
			c->state = webrtc_ChannelState_closed;
			chan_tell(c, (webrtc_ChannelEvent){.kind = webrtc_ChannelEventKind_closed, .text = dup("")});
			chan_end(c);
		}
		break;
	}
	free(m->text);
	free(m->data.data);
	chan_unref(c);
	free(m);
}

// chan_say posts what a libdatachannel thread said (any thread).
static void chan_say(chan *c, webrtc_ChannelEventKind kind, bool binary, const char *data, int size) {
	chan_post *m = calloc(1, sizeof *m);
	if (!m) return;
	m->c = c;
	m->kind = kind;
	m->binary = binary;
	if (kind == webrtc_ChannelEventKind_message) {
		if (binary)
			m->data = asuka_bytes_copy(data, (size_t)size);
		else
			m->text = dup(data);
	}
	atomic_fetch_add(&c->refs, 1);
	asuka_desktop_post(chan_posted, m);
}

static void on_open(int id, void *ptr) {
	(void)id;
	chan_say(ptr, webrtc_ChannelEventKind_open, false, NULL, 0);
}

static void on_closed(int id, void *ptr) {
	(void)id;
	chan_say(ptr, webrtc_ChannelEventKind_closed, false, NULL, 0);
}

static void on_error(int id, const char *error, void *ptr) {
	(void)id, (void)error, (void)ptr;  // a close follows
}

static void on_message(int id, const char *message, int size, void *ptr) {
	(void)id;
	// A negative size is a string's, NUL-terminated.
	chan_say(ptr, webrtc_ChannelEventKind_message, size >= 0, message, size);
}

static void chan_info_read(chan *c) {
	char buf[1024];
	webrtc_ChannelInfo_free(&c->info);
	c->info.label = dup(c->rtc->rtcGetDataChannelLabel(c->dc, buf, sizeof buf) >= 0 ? buf : "");
	c->info.protocol = dup(c->rtc->rtcGetDataChannelProtocol(c->dc, buf, sizeof buf) >= 0 ? buf : "");
	rtcReliability r = {0};
	c->info.ordered = c->rtc->rtcGetDataChannelReliability(c->dc, &r) < 0 || !r.unordered;
	int stream = c->rtc->rtcGetDataChannelStream(c->dc);
	c->info.id = stream >= 0 ? stream : -1;
}

static const webrtc_DataChannel_vtable chan_vt;

// chan_new makes a channel of libdatachannel's dc, its callbacks set:
// from here on, what it says is posted (any thread).
static chan *chan_new(const wrtc_lib *rtc, int dc) {
	chan *c = calloc(1, sizeof *c);
	if (!c) return NULL;
	c->base.vt = &chan_vt;
	c->rtc = rtc;
	c->dc = dc;
	c->tail = &c->head;
	atomic_init(&c->refs, 2);  // the app's, libdatachannel's
	rtc->rtcSetUserPointer(dc, c);
	rtc->rtcSetOpenCallback(dc, on_open);
	rtc->rtcSetClosedCallback(dc, on_closed);
	rtc->rtcSetErrorCallback(dc, on_error);
	rtc->rtcSetMessageCallback(dc, on_message);
	// One the other peer opened is open already.
	if (rtc->rtcIsOpen(dc)) chan_say(c, webrtc_ChannelEventKind_open, false, NULL, 0);
	chan_info_read(c);
	return c;
}

static void chan_cancelled(void *p) {
	chan *c = p;
	asuka_call *call = c->call;
	c->call = NULL;
	if (!call) return;
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(call, &e);
}

static void chan_events(webrtc_DataChannel *self, webrtc_fn_ChannelEvent send, asuka_call *call) {
	chan *c = (chan *)self;
	chan_cancelled(c);  // an events of before
	c->send = send;
	c->call = call;
	asuka_call_on_cancel(call, chan_cancelled, c);
	cev *e = c->head;
	c->head = NULL;
	c->tail = &c->head;
	while (e && c->call == call) {
		cev *n = e->next;
		webrtc_fn_ChannelEvent_call(send, &e->e);
		webrtc_ChannelEvent_free(&e->e);
		free(e);
		e = n;
	}
	if (c->state == webrtc_ChannelState_closed) chan_end(c);
}

static webrtc_ChannelInfo chan_info(webrtc_DataChannel *self, asuka_error *err) {
	(void)err;
	chan *c = (chan *)self;
	if (c->dc) chan_info_read(c);
	webrtc_ChannelInfo i = c->info;
	i.label = dup(i.label);
	i.protocol = dup(i.protocol);
	return i;
}

static webrtc_ChannelState chan_state(webrtc_DataChannel *self, asuka_error *err) {
	(void)err;
	return ((chan *)self)->state;
}

static void chan_send(chan *c, const char *data, int size, asuka_error *err) {
	if (!c->dc || c->state != webrtc_ChannelState_open) {
		asuka_error_declared(err, webrtc_WebRTCError_notOpen);
		return;
	}
	int r = c->rtc->rtcSendMessage(c->dc, data, size);
	if (r < 0) asuka_error_failed(err, "libdatachannel", r, "webrtc: the message wasn't sent (%d)", r);
}

static void chan_send_text(webrtc_DataChannel *self, const char *text, asuka_error *err) {
	chan_send((chan *)self, text ? text : "", -1, err);
}

static void chan_send_bytes(webrtc_DataChannel *self, asuka_bytes data, asuka_error *err) {
	if (data.len > (size_t)INT32_MAX) {
		asuka_error_failed(err, "libdatachannel", RTC_ERR_INVALID, "webrtc: a message too large");
		return;
	}
	static const char empty[1];
	chan_send((chan *)self, data.len ? (const char *)data.data : empty, (int)data.len, err);
}

static int64_t chan_buffered(webrtc_DataChannel *self, asuka_error *err) {
	(void)err;
	chan *c = (chan *)self;
	int n = c->dc ? c->rtc->rtcGetBufferedAmount(c->dc) : 0;
	return n > 0 ? n : 0;
}

static void chan_close(webrtc_DataChannel *self, asuka_error *err) {
	(void)err;
	chan *c = (chan *)self;
	if (c->dc) c->rtc->rtcClose(c->dc);
	chan_drop(c);
	if (c->state != webrtc_ChannelState_closed) {
		c->state = webrtc_ChannelState_closed;
		chan_tell(c, (webrtc_ChannelEvent){.kind = webrtc_ChannelEventKind_closed, .text = dup("")});
	}
	chan_end(c);
}

static void chan_release(webrtc_DataChannel *self) {
	chan *c = (chan *)self;
	chan_close(self, NULL);
	chan_cancelled(c);
	chan_unref(c);  // the app's
}

static const webrtc_DataChannel_vtable chan_vt = {
    .release = chan_release,
    .events_ = chan_events,
    .info = chan_info,
    .state = chan_state,
    .sendText = chan_send_text,
    .sendBytes = chan_send_bytes,
    .bufferedAmount = chan_buffered,
    .close = chan_close,
};

webrtc_DataChannel *webrtc_make_DataChannel(asuka_error *err) {
	asuka_error_unsupported(err, "webrtc: a data channel comes from a Peer");
	return NULL;
}

// ---------------------------------------------------------------------
// Connections

typedef struct pev {
	struct pev *next;
	webrtc_PeerEvent e;
} pev;

typedef struct conn {
	webrtc_Connection base;
	const wrtc_lib *rtc;
	int pc;  // 0 once closed
	atomic_int refs;
	// What happened, until events reads it.
	pev *head, **tail;
	asuka_call *call;
	webrtc_fn_PeerEvent send;
	// createOffer or createAnswer, waiting for its description.
	asuka_call *describing;
	bool offering;  // describing's an offer's
	// The description libdatachannel made last, and whether the app set it.
	char *local;
	bool local_set;
	// Candidates found before the app set the description.
	pev *held, **held_tail;
	// Channels the other peer opened, until the app takes them: by handle.
	chan **remote;
	size_t nremote;
} conn;

static void events_free(pev *e) {
	while (e) {
		pev *n = e->next;
		webrtc_PeerEvent_free(&e->e);
		free(e);
		e = n;
	}
}

static void conn_unref(conn *c) {
	if (atomic_fetch_sub(&c->refs, 1) != 1) return;
	events_free(c->head);
	events_free(c->held);
	for (size_t i = 0; i < c->nremote; i++)
		if (c->remote[i]) chan_release(&c->remote[i]->base);
	free(c->remote);
	free(c->local);
	free(c);
}

// An event with every string it has: the generated code writes them all.
static webrtc_PeerEvent event(webrtc_EventKind kind) {
	webrtc_PeerEvent e = {.kind = kind};
	e.candidate.candidate = dup("");
	e.candidate.sdpMid = dup("");
	e.track.id = dup("");
	e.channel.label = dup("");
	e.channel.protocol = dup("");
	e.channel.id = -1;
	return e;
}

static void queue(pev ***tail, webrtc_PeerEvent e) {
	pev *k = malloc(sizeof *k);
	if (!k) {
		webrtc_PeerEvent_free(&e);
		return;
	}
	k->next = NULL;
	k->e = e;
	**tail = k;
	*tail = &k->next;
}

// tell gives e to the app, or keeps it until it reads (main thread).
static void tell(conn *c, webrtc_PeerEvent e) {
	if (c->call) {
		webrtc_fn_PeerEvent_call(c->send, &e);
		webrtc_PeerEvent_free(&e);
		return;
	}
	queue(&c->tail, e);
}

// conn_end answers events: the connection closed.
static void conn_end(conn *c) {
	asuka_call *call = c->call;
	c->call = NULL;
	if (call) {
		asuka_call_off_cancel(call);
		webrtc_Connection_events_done(call);
	}
}

static void fail(asuka_call *call, int r, const char *what) {
	asuka_error e = {0};
	if (r == RTC_ERR_INVALID)
		asuka_error_failed(&e, "libdatachannel", r, "webrtc: %s: not valid here", what);
	else
		asuka_error_failed(&e, "libdatachannel", r, "webrtc: %s failed (%d)", what, r);
	asuka_call_fail(call, &e);
}

static void fail_closed(asuka_call *call) {
	asuka_error e = {0};
	asuka_error_declared(&e, webrtc_WebRTCError_closed);
	asuka_call_fail(call, &e);
}

typedef enum { P_DESCRIPTION, P_CANDIDATE, P_STATE, P_ICE, P_GATHERING, P_SIGNALING, P_CHANNEL } post_kind;

typedef struct {
	conn *c;
	post_kind kind;
	int value;
	char *a, *b;  // a description's SDP and type; a candidate and its mid
	chan *ch;
} conn_post;

static void conn_posted(void *p) {
	conn_post *m = p;
	conn *c = m->c;
	if (c->pc) switch (m->kind) {
		case P_DESCRIPTION: {
			free(c->local);
			c->local = m->a;
			m->a = NULL;
			c->local_set = false;
			asuka_call *call = c->describing;
			c->describing = NULL;
			webrtc_Description d = {.kind = wrtc_kind(m->b), .sdp = dup(c->local)};
			if (!call)
				webrtc_Description_free(&d);
			else if (c->offering)
				webrtc_Connection_createOffer_done(call, d);
			else
				webrtc_Connection_createAnswer_done(call, d);
			break;
		}
		case P_CANDIDATE: {
			webrtc_PeerEvent e = event(webrtc_EventKind_candidate);
			free(e.candidate.candidate);
			free(e.candidate.sdpMid);
			e.candidate.candidate = wrtc_candidate_line(m->a);
			e.candidate.sdpMid = dup(m->b);
			e.candidate.sdpMLineIndex = wrtc_mline_index(c->local, m->b);
			if (c->local_set)
				tell(c, e);
			else
				queue(&c->held_tail, e);
			break;
		}
		case P_STATE: {
			webrtc_PeerEvent e = event(webrtc_EventKind_connection);
			e.connection = wrtc_connection_state((rtcState)m->value);
			tell(c, e);
			break;
		}
		case P_ICE: {
			webrtc_PeerEvent e = event(webrtc_EventKind_ice);
			e.ice = wrtc_ice_state((rtcIceState)m->value);
			tell(c, e);
			break;
		}
		case P_GATHERING: {
			webrtc_PeerEvent e = event(webrtc_EventKind_gathering);
			e.gathering = wrtc_gathering_state((rtcGatheringState)m->value);
			tell(c, e);
			break;
		}
		case P_SIGNALING: {
			webrtc_PeerEvent e = event(webrtc_EventKind_signaling);
			e.signaling = wrtc_signaling_state((rtcSignalingState)m->value);
			tell(c, e);
			break;
		}
		case P_CHANNEL: {
			chan **grown = realloc(c->remote, (c->nremote + 1) * sizeof *grown);
			if (!grown) break;
			c->remote = grown;
			c->remote[c->nremote++] = m->ch;
			webrtc_PeerEvent e = event(webrtc_EventKind_channel);
			webrtc_ChannelInfo_free(&e.channel);
			e.channel = m->ch->info;
			e.channel.label = dup(e.channel.label);
			e.channel.protocol = dup(e.channel.protocol);
			e.channel.handle = (int64_t)c->nremote;
			m->ch = NULL;  // the table's now
			tell(c, e);
			break;
		}
		}
	if (m->ch) chan_release(&m->ch->base);  // came as the connection closed
	free(m->a);
	free(m->b);
	conn_unref(c);
	free(m);
}

static void say(void *ptr, post_kind kind, int value, const char *a, const char *b, chan *ch) {
	conn *c = ptr;
	conn_post *m = calloc(1, sizeof *m);
	if (!m) {
		if (ch) chan_release(&ch->base);
		return;
	}
	m->c = c;
	m->kind = kind;
	m->value = value;
	m->a = a ? dup(a) : NULL;
	m->b = b ? dup(b) : NULL;
	m->ch = ch;
	atomic_fetch_add(&c->refs, 1);
	asuka_desktop_post(conn_posted, m);
}

static void on_description(int pc, const char *sdp, const char *type, void *ptr) {
	(void)pc;
	say(ptr, P_DESCRIPTION, 0, sdp, type, NULL);
}

static void on_candidate(int pc, const char *cand, const char *mid, void *ptr) {
	(void)pc;
	say(ptr, P_CANDIDATE, 0, cand, mid, NULL);
}

static void on_state(int pc, rtcState s, void *ptr) {
	(void)pc;
	say(ptr, P_STATE, (int)s, NULL, NULL, NULL);
}

static void on_ice(int pc, rtcIceState s, void *ptr) {
	(void)pc;
	say(ptr, P_ICE, (int)s, NULL, NULL, NULL);
}

static void on_gathering(int pc, rtcGatheringState s, void *ptr) {
	(void)pc;
	say(ptr, P_GATHERING, (int)s, NULL, NULL, NULL);
}

static void on_signaling(int pc, rtcSignalingState s, void *ptr) {
	(void)pc;
	say(ptr, P_SIGNALING, (int)s, NULL, NULL, NULL);
}

static void on_channel(int pc, int dc, void *ptr) {
	(void)pc;
	conn *c = ptr;
	chan *ch = chan_new(c->rtc, dc);
	if (ch) say(ptr, P_CHANNEL, 0, NULL, NULL, ch);
}

// on_track declines the other peer's media: no video or sound here.
static void on_track(int pc, int tr, void *ptr) {
	(void)pc;
	((conn *)ptr)->rtc->rtcDeleteTrack(tr);
}

static void conn_cancelled(void *p) {
	conn *c = p;
	asuka_call *call = c->call;
	c->call = NULL;
	if (!call) return;
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(call, &e);
}

static void conn_events(webrtc_Connection *self, webrtc_fn_PeerEvent send, asuka_call *call) {
	conn *c = (conn *)self;
	conn_cancelled(c);
	c->send = send;
	c->call = call;
	asuka_call_on_cancel(call, conn_cancelled, c);
	pev *e = c->head;
	c->head = NULL;
	c->tail = &c->head;
	while (e && c->call == call) {
		pev *n = e->next;
		webrtc_fn_PeerEvent_call(send, &e->e);
		webrtc_PeerEvent_free(&e->e);
		free(e);
		e = n;
	}
	events_free(e);
	if (!c->pc) conn_end(c);
}

static void conn_add_track(webrtc_Connection *self, const char *track, asuka_error *err) {
	(void)self, (void)track;
	asuka_error_unsupported(err, "%s", no_media);
}

static void describe(conn *c, const char *type, asuka_call *call) {
	if (!c->pc) {
		fail_closed(call);
		return;
	}
	if (c->describing) {
		asuka_error e = {0};
		asuka_error_failed(&e, "libdatachannel", RTC_ERR_FAILURE, "webrtc: a description is being made already");
		asuka_call_fail(call, &e);
		return;
	}
	c->describing = call;
	c->offering = strcmp(type, "offer") == 0;
	int r = c->rtc->rtcSetLocalDescription(c->pc, type);
	if (r < 0) {
		c->describing = NULL;
		fail(call, r, strcmp(type, "offer") == 0 ? "createOffer" : "createAnswer");
	}
	// The description comes in on_description, posted.
}

static void conn_offer(webrtc_Connection *self, bool iceRestart, asuka_call *call) {
	if (iceRestart) {
		asuka_error e = {0};
		asuka_error_unsupported(&e, "webrtc: libdatachannel doesn't restart ICE");
		asuka_call_fail(call, &e);
		return;
	}
	describe((conn *)self, "offer", call);
}

static void conn_answer(webrtc_Connection *self, asuka_call *call) { describe((conn *)self, "answer", call); }

// set_local releases the candidates found before the app set the
// description: after its answer, so they go out after it.
static void release_held(void *p) {
	conn *c = p;
	pev *e = c->held;
	c->held = NULL;
	c->held_tail = &c->held;
	while (e) {
		pev *n = e->next;
		if (c->pc)
			tell(c, e->e);
		else
			webrtc_PeerEvent_free(&e->e);
		free(e);
		e = n;
	}
	conn_unref(c);
}

static void conn_set_local(webrtc_Connection *self, const webrtc_Description *d, asuka_call *call) {
	conn *c = (conn *)self;
	if (!c->pc) {
		fail_closed(call);
		return;
	}
	if (d->kind == webrtc_SdpKind_rollback || !c->local || strcmp(d->sdp ? d->sdp : "", c->local) != 0) {
		asuka_error e = {0};
		asuka_error_unsupported(&e, "webrtc: here a description is set as createOffer or createAnswer made it, unchanged");
		asuka_call_fail(call, &e);
		return;
	}
	c->local_set = true;
	webrtc_Connection_setLocal_done(call);
	atomic_fetch_add(&c->refs, 1);
	asuka_desktop_post(release_held, c);
}

static void conn_set_remote(webrtc_Connection *self, const webrtc_Description *d, asuka_call *call) {
	conn *c = (conn *)self;
	if (!c->pc) {
		fail_closed(call);
		return;
	}
	int r = c->rtc->rtcSetRemoteDescription(c->pc, d->kind == webrtc_SdpKind_rollback ? NULL : d->sdp, wrtc_type(d->kind));
	if (r < 0)
		fail(call, r, "setRemoteDescription");
	else
		webrtc_Connection_setRemote_done(call);
}

static void conn_add_candidate(webrtc_Connection *self, const webrtc_Candidate *cand, asuka_call *call) {
	conn *c = (conn *)self;
	if (!c->pc) {
		fail_closed(call);
		return;
	}
	// "" is the end of the other peer's candidates: nothing to add.
	if (!cand->candidate || !*cand->candidate) {
		webrtc_Connection_addCandidate_done(call);
		return;
	}
	int r = c->rtc->rtcAddRemoteCandidate(c->pc, cand->candidate, cand->sdpMid && *cand->sdpMid ? cand->sdpMid : NULL);
	if (r < 0)
		fail(call, r, "addCandidate");
	else
		webrtc_Connection_addCandidate_done(call);
}

static webrtc_DataChannel *conn_create_channel(webrtc_Connection *self, const char *label, const webrtc_ChannelOptions *o, asuka_error *err) {
	conn *c = (conn *)self;
	if (!c->pc) {
		asuka_error_declared(err, webrtc_WebRTCError_closed);
		return NULL;
	}
	rtcDataChannelInit init = wrtc_channel_init(o);
	int dc = c->rtc->rtcCreateDataChannelEx(c->pc, label ? label : "", &init);
	if (dc < 0) {
		asuka_error_failed(err, "libdatachannel", dc, "webrtc: the data channel wasn't made (%d)", dc);
		return NULL;
	}
	chan *ch = chan_new(c->rtc, dc);
	if (!ch) {
		c->rtc->rtcDeleteDataChannel(dc);
		asuka_error_failed(err, "libdatachannel", RTC_ERR_FAILURE, "webrtc: out of memory");
		return NULL;
	}
	return &ch->base;
}

static webrtc_DataChannel *conn_remote_channel(webrtc_Connection *self, int64_t handle, asuka_error *err) {
	conn *c = (conn *)self;
	if (handle < 1 || (size_t)handle > c->nremote || !c->remote[handle - 1]) {
		asuka_error_failed(err, "libdatachannel", RTC_ERR_NOT_AVAIL, "webrtc: no such channel");
		return NULL;
	}
	chan *ch = c->remote[handle - 1];
	c->remote[handle - 1] = NULL;  // the app's now
	return &ch->base;
}

static void conn_stats(webrtc_Connection *self, asuka_call *call) {
	conn *c = (conn *)self;
	webrtc_Stats s = {0};
	char local[512] = "", remote[512] = "";
	if (c->pc && c->rtc->rtcGetSelectedCandidatePair(c->pc, local, sizeof local, remote, sizeof remote) >= 0) {
		s.localCandidate = wrtc_candidate_type(local);
		s.remoteCandidate = wrtc_candidate_type(remote);
	} else {
		s.localCandidate = dup("");
		s.remoteCandidate = dup("");
	}
	webrtc_Connection_stats_done(call, s);
}

static void conn_close(webrtc_Connection *self, asuka_error *err) {
	(void)err;
	conn *c = (conn *)self;
	if (!c->pc) return;
	int pc = c->pc;
	c->pc = 0;
	asuka_call *call = c->describing;
	c->describing = NULL;
	if (call) fail_closed(call);
	// Its channels close with it (their closed comes, posted).
	c->rtc->rtcClosePeerConnection(pc);
	c->rtc->rtcDeletePeerConnection(pc);
	webrtc_PeerEvent e = event(webrtc_EventKind_connection);
	e.connection = webrtc_ConnectionState_closed;
	tell(c, e);
	conn_end(c);
	conn_unref(c);  // libdatachannel's
}

static void conn_release(webrtc_Connection *self) {
	conn *c = (conn *)self;
	conn_close(self, NULL);
	conn_cancelled(c);
	conn_unref(c);  // the app's
}

static const webrtc_Connection_vtable conn_vt = {
    .release = conn_release,
    .events_ = conn_events,
    .addTrack = conn_add_track,
    .removeTrack = conn_add_track,
    .createOffer = conn_offer,
    .createAnswer = conn_answer,
    .setLocal = conn_set_local,
    .setRemote = conn_set_remote,
    .addCandidate = conn_add_candidate,
    .createChannel = conn_create_channel,
    .remoteChannel = conn_remote_channel,
    .stats = conn_stats,
    .close = conn_close,
};

webrtc_Connection *webrtc_make_Connection(asuka_error *err) {
	asuka_error_unsupported(err, "webrtc: a connection comes from Engine.connect");
	return NULL;
}

// ---------------------------------------------------------------------
// The engine

typedef struct {
	webrtc_Engine base;
	const wrtc_lib *rtc;
} engine;

static void capture(webrtc_Engine *self, const webrtc_Audio *audio, const webrtc_Video *video, asuka_call *call) {
	(void)self, (void)audio, (void)video;
	asuka_error e = {0};
	asuka_error_unsupported(&e, "%s", no_media);
	asuka_call_fail(call, &e);
}

static void no_track(webrtc_Engine *self, const char *track, bool enabled, asuka_error *err) {
	(void)self, (void)track, (void)enabled, (void)err;  // there are none
}

static void stop_track(webrtc_Engine *self, const char *track, asuka_error *err) { (void)self, (void)track, (void)err; }

static void switch_camera(webrtc_Engine *self, const char *track, asuka_call *call) {
	(void)self, (void)track;
	asuka_error e = {0};
	asuka_error_unsupported(&e, "%s", no_media);
	asuka_call_fail(call, &e);
}

static webrtc_Connection *connect(webrtc_Engine *self, const webrtc_Config *config, asuka_error *err) {
	const wrtc_lib *rtc = ((engine *)self)->rtc;
	size_t n = 0;
	for (size_t i = 0; i < config->iceServers.len; i++) n += config->iceServers.data[i].urls.len;
	char **urls = calloc(n ? n : 1, sizeof *urls);
	if (!urls) {
		asuka_error_failed(err, "libdatachannel", RTC_ERR_FAILURE, "webrtc: out of memory");
		return NULL;
	}
	n = 0;
	for (size_t i = 0; i < config->iceServers.len; i++) {
		const webrtc_IceServer *s = &config->iceServers.data[i];
		for (size_t j = 0; j < s->urls.len; j++) {
			char *u = wrtc_ice_url(s->urls.data[j], s->username, s->credential);
			if (u) urls[n++] = u;
		}
	}
	rtcConfiguration cfg = {0};
	cfg.iceServers = (const char **)urls;
	cfg.iceServersCount = (int)n;
	cfg.iceTransportPolicy = config->policy == webrtc_IcePolicy_relay ? RTC_TRANSPORT_POLICY_RELAY : RTC_TRANSPORT_POLICY_ALL;
	cfg.disableAutoNegotiation = true;  // createOffer and createAnswer say when
	conn *c = calloc(1, sizeof *c);
	int pc = c ? wrtc_create(rtc, &cfg) : RTC_ERR_FAILURE;
	for (size_t i = 0; i < n; i++) free(urls[i]);
	free(urls);
	if (pc < 0) {
		free(c);
		asuka_error_failed(err, "libdatachannel", pc, "webrtc: the connection wasn't made (%d): an ICE server's URL it can't read?", pc);
		return NULL;
	}
	c->base.vt = &conn_vt;
	c->rtc = rtc;
	c->pc = pc;
	c->tail = &c->head;
	c->held_tail = &c->held;
	atomic_init(&c->refs, 2);  // the app's, libdatachannel's
	rtc->rtcSetUserPointer(pc, c);
	rtc->rtcSetLocalDescriptionCallback(pc, on_description);
	rtc->rtcSetLocalCandidateCallback(pc, on_candidate);
	rtc->rtcSetStateChangeCallback(pc, on_state);
	rtc->rtcSetIceStateChangeCallback(pc, on_ice);
	rtc->rtcSetGatheringStateChangeCallback(pc, on_gathering);
	rtc->rtcSetSignalingStateChangeCallback(pc, on_signaling);
	rtc->rtcSetDataChannelCallback(pc, on_channel);
	rtc->rtcSetTrackCallback(pc, on_track);
	return &c->base;
}

static webrtc_list_AudioRoute audio_routes(webrtc_Engine *self, asuka_error *err) {
	(void)self, (void)err;
	return (webrtc_list_AudioRoute){0};  // the system routes the sound
}

static webrtc_AudioRoute audio_route(webrtc_Engine *self, asuka_error *err) {
	(void)self, (void)err;
	return webrtc_AudioRoute_speaker;
}

static void set_audio_route(webrtc_Engine *self, webrtc_AudioRoute route, asuka_error *err) { (void)self, (void)route, (void)err; }

static const webrtc_Engine_vtable engine_vt = {
    .capture = capture,
    .setEnabled = no_track,
    .stopTrack = stop_track,
    .switchCamera = switch_camera,
    .connect = connect,
    .audioRoutes = audio_routes,
    .audioRoute = audio_route,
    .setAudioRoute = set_audio_route,
};

webrtc_Engine *webrtc_make_Engine(asuka_error *err) {
	const char *why = NULL;
	const wrtc_lib *rtc = wrtc_load(&why);
	if (!rtc) {
		asuka_error_unsupported(err, "%s", why);
		return NULL;
	}
	engine *e = calloc(1, sizeof *e);
	if (!e) {
		asuka_error_failed(err, "libdatachannel", RTC_ERR_FAILURE, "webrtc: out of memory");
		return NULL;
	}
	e->base.vt = &engine_vt;
	e->rtc = rtc;
	return &e->base;  // freed as Asuka lets it go
}
