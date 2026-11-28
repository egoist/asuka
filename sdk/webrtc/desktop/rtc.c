// rtc.c: libdatachannel opened as the app runs, and what platform/webrtc's
// desktop C works out without it (rtc.h).
#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

static wrtc_lib lib;
static const wrtc_lib *loaded;
static const char *missing;

// What to install, where it's missing.
#if defined(_WIN32)
static const char *const names[] = {"datachannel.dll", "libdatachannel.dll"};
static const char why_missing[] =
    "webrtc: data channels need libdatachannel (0.18 or later): datachannel.dll beside the app or on PATH "
    "(vcpkg's libdatachannel, MSYS2's mingw-w64-libdatachannel)";
#elif defined(__APPLE__)
// A Mac app runs the package's Swift; this is for the C's tests.
static const char *const names[] = {"libdatachannel.dylib", "/opt/homebrew/lib/libdatachannel.dylib", "/usr/local/lib/libdatachannel.dylib"};
static const char why_missing[] = "webrtc: data channels need libdatachannel (0.18 or later)";
#else
// Its soname is its minor version's (libdatachannel.so.0.24): the newest
// first, then whatever "libdatachannel.so" is (a development install).
static const char *const names[] = {
    "libdatachannel.so.0.30", "libdatachannel.so.0.29", "libdatachannel.so.0.28", "libdatachannel.so.0.27",
    "libdatachannel.so.0.26", "libdatachannel.so.0.25", "libdatachannel.so.0.24", "libdatachannel.so.0.23",
    "libdatachannel.so.0.22", "libdatachannel.so.0.21", "libdatachannel.so.0.20", "libdatachannel.so.0.19",
    "libdatachannel.so.0.18", "libdatachannel.so",
};
static const char why_missing[] =
    "webrtc: data channels need libdatachannel (0.18 or later), which isn't installed "
    "(Fedora's and Arch's libdatachannel package; a Flatpak of the app has it)";
#endif

// logged is libdatachannel's errors, on standard error (any thread).
static void logged(rtcLogLevel level, const char *message) {
	(void)level;
	fprintf(stderr, "webrtc: libdatachannel: %s\n", message);
}

static void load_once(void) {
	void *h = NULL;
	for (size_t i = 0; i < sizeof names / sizeof *names && !h; i++) {
#ifdef _WIN32
		h = (void *)LoadLibraryA(names[i]);
#else
		h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
#endif
	}
	if (!h) {
		missing = why_missing;
		return;
	}
	bool ok = true;
#ifdef _WIN32
#define RTC_RESOLVE(ret, name, params)                                             \
	*(FARPROC *)&lib.name = GetProcAddress((HMODULE)h, #name);                     \
	if (!lib.name) ok = false;
#else
#define RTC_RESOLVE(ret, name, params)                                             \
	*(void **)&lib.name = dlsym(h, #name);                                         \
	if (!lib.name) ok = false;
#endif
	RTC_FUNCS(RTC_RESOLVE)
#undef RTC_RESOLVE
	if (!ok) {
		missing = "webrtc: the libdatachannel installed is older than 0.18, which data channels need";
		return;
	}
#ifdef _WIN32
	lib.next = GetProcAddress((HMODULE)h, "rtcSetLocalDescriptionEx") != NULL;
#else
	lib.next = dlsym(h, "rtcSetLocalDescriptionEx") != NULL;
#endif
	lib.rtcInitLogger(RTC_LOG_ERROR, logged);
	loaded = &lib;
}

#ifdef _WIN32
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK load_win(PINIT_ONCE o, PVOID p, PVOID *c) {
	(void)o, (void)p, (void)c;
	load_once();
	return TRUE;
}
#else
static pthread_once_t once = PTHREAD_ONCE_INIT;
#endif

const wrtc_lib *wrtc_load(const char **why) {
#ifdef _WIN32
	InitOnceExecuteOnce(&once, load_win, NULL, NULL);
#else
	pthread_once(&once, load_once);
#endif
	if (!loaded && why) *why = missing;
	return loaded;
}

int wrtc_create(const wrtc_lib *rtc, const rtcConfiguration *c) {
	if (!rtc->next) return rtc->rtcCreatePeerConnection(c);
	rtcConfigurationNext n = {
	    .iceServers = c->iceServers,
	    .iceServersCount = c->iceServersCount,
	    .proxyServer = c->proxyServer,
	    .bindAddress = c->bindAddress,
	    .certificateType = c->certificateType,
	    .iceTransportPolicy = c->iceTransportPolicy,
	    .enableIceTcp = c->enableIceTcp,
	    .enableIceUdpMux = c->enableIceUdpMux,
	    .disableAutoNegotiation = c->disableAutoNegotiation,
	    .forceMediaTransport = c->forceMediaTransport,
	    .portRangeBegin = c->portRangeBegin,
	    .portRangeEnd = c->portRangeEnd,
	    .mtu = c->mtu,
	    .maxMessageSize = c->maxMessageSize,
	};
	return rtc->rtcCreatePeerConnection(&n);
}

// ---- What needs no library ----

// percent writes s into out (at *n, growing it), its reserved characters
// %-encoded, as a URL's user information has them.
static void percent(char *out, size_t *n, const char *s) {
	static const char hex[] = "0123456789ABCDEF";
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p);
		if (plain) {
			out[(*n)++] = (char)*p;
		} else {
			out[(*n)++] = '%';
			out[(*n)++] = hex[*p >> 4];
			out[(*n)++] = hex[*p & 15];
		}
	}
}

char *wrtc_ice_url(const char *url, const char *username, const char *credential) {
	if (!url) url = "";
	if (!username) username = "";
	if (!credential) credential = "";
	const char *colon = strchr(url, ':');
	bool turn = colon && (strncmp(url, "turn:", 5) == 0 || strncmp(url, "turns:", 6) == 0 || strncmp(url, "TURN:", 5) == 0 ||
	                      strncmp(url, "TURNS:", 6) == 0);
	// A URL that has its user already (turn:u:p@host), or no credentials
	// to put in it, is as it is.
	if (!turn || (!*username && !*credential) || strchr(url, '@')) return asuka_strdup(url);
	size_t cap = strlen(url) + 3 * (strlen(username) + strlen(credential)) + 4, n = 0;
	char *out = malloc(cap);
	if (!out) return NULL;
	size_t scheme = (size_t)(colon - url) + 1;
	memcpy(out, url, scheme);
	n = scheme;
	const char *rest = colon + 1;
	while (*rest == '/') rest++;  // turn://host
	percent(out, &n, username);
	out[n++] = ':';
	percent(out, &n, credential);
	out[n++] = '@';
	size_t r = strlen(rest);
	memcpy(out + n, rest, r);
	n += r;
	out[n] = 0;
	return out;
}

char *wrtc_candidate_type(const char *candidate) {
	const char *p = candidate ? strstr(candidate, " typ ") : NULL;
	if (!p) return asuka_strdup("");
	p += 5;
	size_t n = 0;
	while (p[n] && p[n] != ' ' && p[n] != '\r' && p[n] != '\n') n++;
	return asuka_strndup(p, n);
}

int64_t wrtc_mline_index(const char *sdp, const char *mid) {
	if (!sdp || !mid || !*mid) return 0;
	int64_t index = -1;
	size_t mlen = strlen(mid);
	for (const char *l = sdp; l && *l;) {
		if (strncmp(l, "m=", 2) == 0) index++;
		if (index >= 0 && strncmp(l, "a=mid:", 6) == 0 && strncmp(l + 6, mid, mlen) == 0 &&
		    (l[6 + mlen] == '\r' || l[6 + mlen] == '\n' || l[6 + mlen] == 0))
			return index;
		l = strchr(l, '\n');
		if (l) l++;
	}
	return 0;
}

char *wrtc_candidate_line(const char *cand) {
	if (!cand) return asuka_strdup("");
	if (strncmp(cand, "a=", 2) == 0) cand += 2;
	size_t n = strlen(cand);
	while (n && (cand[n - 1] == '\r' || cand[n - 1] == '\n')) n--;
	return asuka_strndup(cand, n);
}

webrtc_ConnectionState wrtc_connection_state(rtcState s) {
	switch (s) {
	case RTC_CONNECTING: return webrtc_ConnectionState_connecting;
	case RTC_CONNECTED: return webrtc_ConnectionState_connected;
	case RTC_DISCONNECTED: return webrtc_ConnectionState_disconnected;
	case RTC_FAILED: return webrtc_ConnectionState_failed;
	case RTC_CLOSED: return webrtc_ConnectionState_closed;
	default: return webrtc_ConnectionState_new;
	}
}

webrtc_IceState wrtc_ice_state(rtcIceState s) {
	switch (s) {
	case RTC_ICE_CHECKING: return webrtc_IceState_checking;
	case RTC_ICE_CONNECTED: return webrtc_IceState_connected;
	case RTC_ICE_COMPLETED: return webrtc_IceState_completed;
	case RTC_ICE_FAILED: return webrtc_IceState_failed;
	case RTC_ICE_DISCONNECTED: return webrtc_IceState_disconnected;
	case RTC_ICE_CLOSED: return webrtc_IceState_closed;
	default: return webrtc_IceState_new;
	}
}

webrtc_GatheringState wrtc_gathering_state(rtcGatheringState s) {
	switch (s) {
	case RTC_GATHERING_INPROGRESS: return webrtc_GatheringState_gathering;
	case RTC_GATHERING_COMPLETE: return webrtc_GatheringState_complete;
	default: return webrtc_GatheringState_new;
	}
}

webrtc_SignalingState wrtc_signaling_state(rtcSignalingState s) {
	switch (s) {
	case RTC_SIGNALING_HAVE_LOCAL_OFFER: return webrtc_SignalingState_haveLocalOffer;
	case RTC_SIGNALING_HAVE_REMOTE_OFFER: return webrtc_SignalingState_haveRemoteOffer;
	case RTC_SIGNALING_HAVE_LOCAL_PRANSWER: return webrtc_SignalingState_haveLocalPranswer;
	case RTC_SIGNALING_HAVE_REMOTE_PRANSWER: return webrtc_SignalingState_haveRemotePranswer;
	default: return webrtc_SignalingState_stable;
	}
}

webrtc_SdpKind wrtc_kind(const char *type) {
	if (!type) return webrtc_SdpKind_offer;
	if (strcmp(type, "answer") == 0) return webrtc_SdpKind_answer;
	if (strcmp(type, "pranswer") == 0) return webrtc_SdpKind_pranswer;
	if (strcmp(type, "rollback") == 0) return webrtc_SdpKind_rollback;
	return webrtc_SdpKind_offer;
}

const char *wrtc_type(webrtc_SdpKind kind) {
	switch (kind) {
	case webrtc_SdpKind_answer: return "answer";
	case webrtc_SdpKind_pranswer: return "pranswer";
	case webrtc_SdpKind_rollback: return "rollback";
	default: return "offer";
	}
}

rtcDataChannelInit wrtc_channel_init(const webrtc_ChannelOptions *o) {
	rtcDataChannelInit init = {0};
	init.reliability.unordered = !o->ordered;
	if (o->maxPacketLifeTime) {
		int64_t ms = *o->maxPacketLifeTime / 1000000;
		init.reliability.unreliable = true;
		init.reliability.maxPacketLifeTime = ms < 0 ? 0 : ms > 65535 ? 65535 : (unsigned)ms;
	} else if (o->maxRetransmits) {
		int64_t n = *o->maxRetransmits;
		init.reliability.unreliable = true;
		init.reliability.maxRetransmits = n < 0 ? 0 : n > 65535 ? 65535 : (unsigned)n;
	}
	init.protocol = o->protocol ? o->protocol : "";
	if (o->id && *o->id >= 0 && *o->id <= 65534) {
		init.negotiated = true;
		init.manualStream = true;
		init.stream = (uint16_t)*o->id;
	}
	return init;
}
