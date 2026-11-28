// webrtc_test.c: platform/webrtc's desktop C without the core
// (platform/desktop's TestStdUnits): ICE servers' URLs as libdatachannel
// reads them, candidates' lines, types and media lines, the states, a
// channel's options; and, where libdatachannel is installed (rtc.c
// finds it as an app does), two connections in this process that
// connect through it and carry messages both ways over a data channel,
// one declining the other's sound as webrtc.c does. The glue webrtc.c
// calls is stand-ins here, never called.
#include "rtc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

static int failed;

#define CHECK(c)                                           \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
			failed = 1;                                    \
		}                                                  \
	} while (0)

static bool is(char *got, const char *want) {
	bool ok = got && !strcmp(got, want);
	if (!ok) printf("got %s, want %s\n", got ? got : "NULL", want);
	free(got);
	return ok;
}

static void urls(void) {
	CHECK(is(wrtc_ice_url("stun:stun.l.google.com:19302", "", ""), "stun:stun.l.google.com:19302"));
	CHECK(is(wrtc_ice_url("stun:stun.example.com", "who", "pw"), "stun:stun.example.com"));  // STUN takes none
	CHECK(is(wrtc_ice_url("turn:turn.example.com:3478?transport=udp", "ana", "s3cret"), "turn:ana:s3cret@turn.example.com:3478?transport=udp"));
	CHECK(is(wrtc_ice_url("turns:turn.example.com:443", "a:b@c", "p/w?#%"), "turns:a%3Ab%40c:p%2Fw%3F%23%25@turn.example.com:443"));
	CHECK(is(wrtc_ice_url("turn://turn.example.com", "u", "p"), "turn:u:p@turn.example.com"));
	CHECK(is(wrtc_ice_url("turn:x:y@turn.example.com", "u", "p"), "turn:x:y@turn.example.com"));  // has its own
	CHECK(is(wrtc_ice_url("turn:turn.example.com", "", ""), "turn:turn.example.com"));
	CHECK(is(wrtc_ice_url(NULL, NULL, NULL), ""));
}

static const char sdp[] =
    "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 audio1 2\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio1\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:2\r\n";

static void candidates(void) {
	CHECK(is(wrtc_candidate_line("a=candidate:1 1 UDP 2122317823 192.168.1.2 54321 typ host\r\n"), "candidate:1 1 UDP 2122317823 192.168.1.2 54321 typ host"));
	CHECK(is(wrtc_candidate_line("candidate:2 1 UDP 1 198.51.100.7 9 typ srflx raddr 0.0.0.0 rport 0"), "candidate:2 1 UDP 1 198.51.100.7 9 typ srflx raddr 0.0.0.0 rport 0"));
	CHECK(is(wrtc_candidate_line(NULL), ""));
	CHECK(is(wrtc_candidate_type("a=candidate:1 1 UDP 2122317823 192.168.1.2 54321 typ host"), "host"));
	CHECK(is(wrtc_candidate_type("candidate:2 1 UDP 1 198.51.100.7 9 typ srflx raddr 0.0.0.0 rport 0"), "srflx"));
	CHECK(is(wrtc_candidate_type("192.168.1.2:54321"), ""));
	CHECK(wrtc_mline_index(sdp, "0") == 0);
	CHECK(wrtc_mline_index(sdp, "audio1") == 1);
	CHECK(wrtc_mline_index(sdp, "2") == 2);
	CHECK(wrtc_mline_index(sdp, "audio") == 0);  // not a prefix's
	CHECK(wrtc_mline_index(sdp, "") == 0 && wrtc_mline_index(NULL, "0") == 0);
}

static void states(void) {
	CHECK(wrtc_connection_state(RTC_CONNECTED) == webrtc_ConnectionState_connected);
	CHECK(wrtc_connection_state(RTC_CLOSED) == webrtc_ConnectionState_closed);
	// libdatachannel's ICE says failed before disconnected; the package after.
	CHECK(wrtc_ice_state(RTC_ICE_FAILED) == webrtc_IceState_failed);
	CHECK(wrtc_ice_state(RTC_ICE_DISCONNECTED) == webrtc_IceState_disconnected);
	CHECK(wrtc_ice_state(RTC_ICE_COMPLETED) == webrtc_IceState_completed);
	CHECK(wrtc_gathering_state(RTC_GATHERING_INPROGRESS) == webrtc_GatheringState_gathering);
	CHECK(wrtc_signaling_state(RTC_SIGNALING_HAVE_REMOTE_OFFER) == webrtc_SignalingState_haveRemoteOffer);
	CHECK(wrtc_kind("answer") == webrtc_SdpKind_answer && wrtc_kind("offer") == webrtc_SdpKind_offer);
	CHECK(!strcmp(wrtc_type(webrtc_SdpKind_pranswer), "pranswer") && !strcmp(wrtc_type(webrtc_SdpKind_offer), "offer"));
}

static void options(void) {
	webrtc_ChannelOptions o = {.ordered = true};
	rtcDataChannelInit i = wrtc_channel_init(&o);
	CHECK(!i.reliability.unordered && !i.reliability.unreliable && !i.negotiated && !strcmp(i.protocol, ""));
	int64_t zero = 0, life = 250 * 1000000LL, id = 7;
	o = (webrtc_ChannelOptions){.ordered = false, .maxRetransmits = &zero, .protocol = "chat"};
	i = wrtc_channel_init(&o);
	CHECK(i.reliability.unordered && i.reliability.unreliable && i.reliability.maxRetransmits == 0 && !strcmp(i.protocol, "chat"));
	o = (webrtc_ChannelOptions){.ordered = true, .maxPacketLifeTime = &life, .id = &id};
	i = wrtc_channel_init(&o);
	CHECK(i.reliability.unreliable && i.reliability.maxPacketLifeTime == 250);
	CHECK(i.negotiated && i.manualStream && i.stream == 7);
}

// ---- Two connections, through libdatachannel ----

typedef struct {
	const wrtc_lib *rtc;
	int pc;
	int dc;
	bool described;  // it has the other's description
	char *kept[16], *kept_mid[16];
	int nkept;
	char *description, *type;
	bool open, closed;
	char got[8][64];
	int ngot;
	bool binary;
} side;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static side a, b;

static side *of(int pc) { return pc == a.pc ? &a : &b; }

static void on_description(int pc, const char *s, const char *type, void *ptr) {
	(void)ptr;
	pthread_mutex_lock(&mu);
	side *x = of(pc);
	free(x->description);
	free(x->type);
	x->description = asuka_strdup(s);
	x->type = asuka_strdup(type);
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

// trickle adds a candidate of x's to the other side, as the signaling
// would: its line as the package gives it, back as it's added; kept
// until the other has x's description (under mu).
static void trickle(side *x, const char *line, const char *mid) {
	side *to = x == &a ? &b : &a;
	if (!to->described) {
		if (x->nkept < 16) {
			x->kept[x->nkept] = asuka_strdup(line);
			x->kept_mid[x->nkept++] = asuka_strdup(mid);
		}
		return;
	}
	CHECK(x->rtc->rtcAddRemoteCandidate(to->pc, line, mid) == 0);
}

static void on_candidate(int pc, const char *cand, const char *mid, void *ptr) {
	(void)ptr;
	char *line = wrtc_candidate_line(cand);
	pthread_mutex_lock(&mu);
	trickle(of(pc), line, mid);
	pthread_mutex_unlock(&mu);
	free(line);
}

// described says to has the other side's description: what was kept for
// it goes.
static void described(side *to) {
	side *from = to == &a ? &b : &a;
	pthread_mutex_lock(&mu);
	to->described = true;
	for (int i = 0; i < from->nkept; i++) {
		trickle(from, from->kept[i], from->kept_mid[i]);
		free(from->kept[i]);
		free(from->kept_mid[i]);
	}
	from->nkept = 0;
	pthread_mutex_unlock(&mu);
}

static void on_dc_open(int id, void *ptr) {
	(void)id;
	pthread_mutex_lock(&mu);
	((side *)ptr)->open = true;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

static void on_dc_closed(int id, void *ptr) {
	(void)id;
	pthread_mutex_lock(&mu);
	((side *)ptr)->closed = true;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

static void on_dc_message(int id, const char *m, int size, void *ptr) {
	(void)id;
	side *x = ptr;
	pthread_mutex_lock(&mu);
	if (x->ngot < 8) {
		if (size < 0) {
			snprintf(x->got[x->ngot++], 64, "%s", m);
		} else {
			x->binary = true;
			snprintf(x->got[x->ngot++], 64, "%d bytes, %d", size, size > 1 ? (unsigned char)m[1] : -1);
		}
	}
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

static void channel(side *x, int dc) {
	x->dc = dc;
	x->rtc->rtcSetUserPointer(dc, x);
	x->rtc->rtcSetOpenCallback(dc, on_dc_open);
	x->rtc->rtcSetClosedCallback(dc, on_dc_closed);
	x->rtc->rtcSetMessageCallback(dc, on_dc_message);
	if (x->rtc->rtcIsOpen(dc)) on_dc_open(dc, x);
}

static void on_channel(int pc, int dc, void *ptr) {
	(void)ptr;
	channel(of(pc), dc);
}

static int declined;
static void on_track(int pc, int tr, void *ptr) {
	(void)ptr, (void)pc;
	of(pc)->rtc->rtcDeleteTrack(tr);  // as webrtc.c does
	declined++;
}

// waits for cond (under mu), at most 10 s.
#define WAIT(cond)                                                                  \
	do {                                                                            \
		struct timespec until;                                                      \
		clock_gettime(CLOCK_REALTIME, &until);                                      \
		until.tv_sec += 10;                                                         \
		pthread_mutex_lock(&mu);                                                    \
		while (!(cond) && pthread_cond_timedwait(&cv, &mu, &until) == 0) {         \
		}                                                                           \
		bool ok_ = (cond);                                                          \
		pthread_mutex_unlock(&mu);                                                  \
		if (!ok_) printf("%s:%d: timed out waiting for %s\n", __FILE__, __LINE__, #cond); \
		if (!ok_) failed = 1;                                                       \
	} while (0)

static int make(const wrtc_lib *rtc) {
	rtcConfiguration cfg = {0};
	cfg.disableAutoNegotiation = true;
	cfg.bindAddress = "127.0.0.1";  // libjuice's: no network but this one
	int pc = wrtc_create(rtc, &cfg);
	rtc->rtcSetLocalDescriptionCallback(pc, on_description);
	rtc->rtcSetLocalCandidateCallback(pc, on_candidate);
	rtc->rtcSetDataChannelCallback(pc, on_channel);
	rtc->rtcSetTrackCallback(pc, on_track);
	return pc;
}

static void loopback(void) {
	const char *why = NULL;
	const wrtc_lib *rtc = wrtc_load(&why);
	if (!rtc) {
		printf("no libdatachannel here, two connections not tried: %s\n", why);
		return;
	}
	a.rtc = b.rtc = rtc;
	a.pc = make(rtc);
	b.pc = make(rtc);
	CHECK(a.pc > 0 && b.pc > 0);

	// The caller offers sound too, which the other declines (no media there).
	int (*add_track)(int, const char *) = NULL;
#ifndef _WIN32
	void *h = dlopen("libdatachannel.so.0.24", RTLD_NOW | RTLD_NOLOAD);
	if (!h) h = dlopen("libdatachannel.so", RTLD_NOW | RTLD_NOLOAD);
	if (h) *(void **)&add_track = dlsym(h, "rtcAddTrack");
	if (add_track)
		CHECK(add_track(a.pc, "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio\r\na=sendonly\r\na=rtpmap:111 opus/48000/2\r\n") > 0);
#endif

	rtcDataChannelInit init = {.protocol = "chat"};
	int dc = rtc->rtcCreateDataChannelEx(a.pc, "chat", &init);
	CHECK(dc > 0);
	channel(&a, dc);

	CHECK(rtc->rtcSetLocalDescription(a.pc, "offer") == 0);
	WAIT(a.description != NULL);
	pthread_mutex_lock(&mu);
	char *offer = asuka_strdup(a.description);
	CHECK(!strcmp(a.type, "offer"));
	pthread_mutex_unlock(&mu);
	CHECK(rtc->rtcSetRemoteDescription(b.pc, offer, "offer") == 0);
	described(&b);
	free(offer);
	CHECK(rtc->rtcSetLocalDescription(b.pc, "answer") == 0);
	WAIT(b.description != NULL);
	pthread_mutex_lock(&mu);
	char *answer = asuka_strdup(b.description);
	CHECK(!strcmp(b.type, "answer"));
	pthread_mutex_unlock(&mu);
	if (add_track) {
		// Declined: the answer's sound section takes nothing.
		const char *audio = strstr(answer, "m=audio");
		CHECK(declined == 1);
		CHECK(audio && (strncmp(audio, "m=audio 0 ", 10) == 0 || (strstr(audio, "a=inactive") && (!strstr(audio, "m=video") || strstr(audio, "a=inactive") < strstr(audio + 1, "m=")))));
		if (audio) printf("the answer's sound: %.*s\n", (int)strcspn(audio, "\r\n"), audio);
	}
	CHECK(rtc->rtcSetRemoteDescription(a.pc, answer, "answer") == 0);
	described(&a);
	free(answer);

	WAIT(a.open && b.open);
	char label[64] = "", protocol[64] = "";
	CHECK(rtc->rtcGetDataChannelLabel(b.dc, label, sizeof label) >= 0 && !strcmp(label, "chat"));
	CHECK(rtc->rtcGetDataChannelProtocol(b.dc, protocol, sizeof protocol) >= 0 && !strcmp(protocol, "chat"));
	CHECK(rtc->rtcSendMessage(a.dc, "hello", -1) >= 0);
	const char bin[] = {1, 2, 3};
	CHECK(rtc->rtcSendMessage(a.dc, bin, 3) >= 0);
	WAIT(b.ngot == 2);
	CHECK(b.ngot == 2 && !strcmp(b.got[0], "hello") && !strcmp(b.got[1], "3 bytes, 2"));
	CHECK(rtc->rtcSendMessage(b.dc, "re: hello", -1) >= 0);
	WAIT(a.ngot == 1);
	CHECK(a.ngot == 1 && !strcmp(a.got[0], "re: hello"));

	char local[256] = "", remote[256] = "";
	CHECK(rtc->rtcGetSelectedCandidatePair(a.pc, local, sizeof local, remote, sizeof remote) >= 0);
	CHECK(is(wrtc_candidate_type(local), "host"));

	rtc->rtcClose(a.dc);
	WAIT(b.closed);
	rtc->rtcDeleteDataChannel(a.dc);
	rtc->rtcDeleteDataChannel(b.dc);
	rtc->rtcClosePeerConnection(a.pc);
	rtc->rtcClosePeerConnection(b.pc);
	rtc->rtcDeletePeerConnection(a.pc);
	rtc->rtcDeletePeerConnection(b.pc);
	free(a.description), free(a.type), free(b.description), free(b.type);
	printf("two connections, through libdatachannel: a channel both ways\n");
}

int main(void) {
	urls();
	candidates();
	states();
	options();
	loopback();
	return failed;
}

// ---- The glue webrtc.c calls: stand-ins, never called here ----

void webrtc_ChannelEvent_free(webrtc_ChannelEvent *v) { (void)v; }
void webrtc_ChannelInfo_free(webrtc_ChannelInfo *v) { (void)v; }
void webrtc_PeerEvent_free(webrtc_PeerEvent *v) { (void)v; }
void webrtc_Description_free(webrtc_Description *v) { (void)v; }
void webrtc_fn_PeerEvent_call(webrtc_fn_PeerEvent f, const webrtc_PeerEvent *p0) { (void)f, (void)p0; }
void webrtc_fn_ChannelEvent_call(webrtc_fn_ChannelEvent f, const webrtc_ChannelEvent *p0) { (void)f, (void)p0; }
void webrtc_DataChannel_events_done(asuka_call *call) { (void)call; }
void webrtc_Connection_events_done(asuka_call *call) { (void)call; }
void webrtc_Connection_createOffer_done(asuka_call *call, webrtc_Description value) { (void)call, (void)value; }
void webrtc_Connection_createAnswer_done(asuka_call *call, webrtc_Description value) { (void)call, (void)value; }
void webrtc_Connection_setLocal_done(asuka_call *call) { (void)call; }
void webrtc_Connection_setRemote_done(asuka_call *call) { (void)call; }
void webrtc_Connection_addCandidate_done(asuka_call *call) { (void)call; }
void webrtc_Connection_stats_done(asuka_call *call, webrtc_Stats value) { (void)call, (void)value; }
