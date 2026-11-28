// rtc.h: libdatachannel's C API (its rtc/rtc.h, MPL-2.0), as
// platform/webrtc's Windows and Linux C reaches it: the library is opened
// as the app runs (libdatachannel.so.0.x, datachannel.dll), never linked,
// so building needs neither its headers nor the library, and an app
// without it runs, its Engine saying what to install. Only what the
// package calls is declared. Releases 0.18 to 0.24 lay rtcConfiguration
// out one way; the next (its main branch since May 2026) adds
// certificate fields in its middle and a flag at its end, and
// rtcSetLocalDescriptionEx with them, by which wrtc_load tells them apart
// (wrtc_create fills the one the library reads). rtcReliability and
// rtcDataChannelInit are the same in all. Its callbacks come on its own
// threads.
//
// The helpers below are what the package works out without the library:
// ICE servers' URLs, a candidate's type and media line, the states.
#ifndef WEBRTC_RTC_H
#define WEBRTC_RTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "webrtc.asuka.h"

typedef enum { RTC_NEW = 0, RTC_CONNECTING = 1, RTC_CONNECTED = 2, RTC_DISCONNECTED = 3, RTC_FAILED = 4, RTC_CLOSED = 5 } rtcState;
typedef enum {
	RTC_ICE_NEW = 0,
	RTC_ICE_CHECKING = 1,
	RTC_ICE_CONNECTED = 2,
	RTC_ICE_COMPLETED = 3,
	RTC_ICE_FAILED = 4,
	RTC_ICE_DISCONNECTED = 5,
	RTC_ICE_CLOSED = 6
} rtcIceState;
typedef enum { RTC_GATHERING_NEW = 0, RTC_GATHERING_INPROGRESS = 1, RTC_GATHERING_COMPLETE = 2 } rtcGatheringState;
typedef enum {
	RTC_SIGNALING_STABLE = 0,
	RTC_SIGNALING_HAVE_LOCAL_OFFER = 1,
	RTC_SIGNALING_HAVE_REMOTE_OFFER = 2,
	RTC_SIGNALING_HAVE_LOCAL_PRANSWER = 3,
	RTC_SIGNALING_HAVE_REMOTE_PRANSWER = 4,
} rtcSignalingState;
typedef enum { RTC_LOG_NONE = 0, RTC_LOG_FATAL = 1, RTC_LOG_ERROR = 2, RTC_LOG_WARNING = 3 } rtcLogLevel;
typedef enum { RTC_TRANSPORT_POLICY_ALL = 0, RTC_TRANSPORT_POLICY_RELAY = 1 } rtcTransportPolicy;

#define RTC_ERR_SUCCESS 0
#define RTC_ERR_INVALID -1
#define RTC_ERR_FAILURE -2
#define RTC_ERR_NOT_AVAIL -3
#define RTC_ERR_TOO_SMALL -4

// rtcConfiguration as 0.18 to 0.24 have it.
typedef struct {
	const char **iceServers;
	int iceServersCount;
	const char *proxyServer;
	const char *bindAddress;  // libjuice's; NULL: any
	int certificateType;
	rtcTransportPolicy iceTransportPolicy;
	bool enableIceTcp;
	bool enableIceUdpMux;
	bool disableAutoNegotiation;
	bool forceMediaTransport;
	uint16_t portRangeBegin;
	uint16_t portRangeEnd;
	int mtu;
	int maxMessageSize;
} rtcConfiguration;

// rtcConfiguration as the releases after 0.24 have it.
typedef struct {
	const char **iceServers;
	int iceServersCount;
	const char *proxyServer;
	const char *bindAddress;
	int certificateType;
	const char *certificatePemFile;
	const char *keyPemFile;
	const char *keyPemPass;
	rtcTransportPolicy iceTransportPolicy;
	bool enableIceTcp;
	bool enableIceUdpMux;
	bool disableAutoNegotiation;
	bool forceMediaTransport;
	uint16_t portRangeBegin;
	uint16_t portRangeEnd;
	int mtu;
	int maxMessageSize;
	bool disableFingerprintVerification;
} rtcConfigurationNext;

typedef struct {
	bool unordered;
	bool unreliable;
	unsigned int maxPacketLifeTime;  // milliseconds, if unreliable
	unsigned int maxRetransmits;     // if unreliable (and no lifetime)
} rtcReliability;

typedef struct {
	rtcReliability reliability;
	const char *protocol;
	bool negotiated;
	bool manualStream;
	uint16_t stream;
} rtcDataChannelInit;

typedef void (*rtcLogCallbackFunc)(rtcLogLevel level, const char *message);
typedef void (*rtcDescriptionCallbackFunc)(int pc, const char *sdp, const char *type, void *ptr);
typedef void (*rtcCandidateCallbackFunc)(int pc, const char *cand, const char *mid, void *ptr);
typedef void (*rtcStateChangeCallbackFunc)(int pc, rtcState state, void *ptr);
typedef void (*rtcIceStateChangeCallbackFunc)(int pc, rtcIceState state, void *ptr);
typedef void (*rtcGatheringStateCallbackFunc)(int pc, rtcGatheringState state, void *ptr);
typedef void (*rtcSignalingStateCallbackFunc)(int pc, rtcSignalingState state, void *ptr);
typedef void (*rtcDataChannelCallbackFunc)(int pc, int dc, void *ptr);
typedef void (*rtcTrackCallbackFunc)(int pc, int tr, void *ptr);
typedef void (*rtcOpenCallbackFunc)(int id, void *ptr);
typedef void (*rtcClosedCallbackFunc)(int id, void *ptr);
typedef void (*rtcErrorCallbackFunc)(int id, const char *error, void *ptr);
typedef void (*rtcMessageCallbackFunc)(int id, const char *message, int size, void *ptr);

// RTC_FUNCS is every function the package calls: X(result, name, params).
#define RTC_FUNCS(X)                                                                                   \
	X(void, rtcInitLogger, (rtcLogLevel level, rtcLogCallbackFunc cb))                                 \
	X(void, rtcSetUserPointer, (int id, void *ptr))                                                    \
	X(int, rtcCreatePeerConnection, (const void *config))                                              \
	X(int, rtcClosePeerConnection, (int pc))                                                           \
	X(int, rtcDeletePeerConnection, (int pc))                                                          \
	X(int, rtcSetLocalDescriptionCallback, (int pc, rtcDescriptionCallbackFunc cb))                    \
	X(int, rtcSetLocalCandidateCallback, (int pc, rtcCandidateCallbackFunc cb))                        \
	X(int, rtcSetStateChangeCallback, (int pc, rtcStateChangeCallbackFunc cb))                         \
	X(int, rtcSetIceStateChangeCallback, (int pc, rtcIceStateChangeCallbackFunc cb))                   \
	X(int, rtcSetGatheringStateChangeCallback, (int pc, rtcGatheringStateCallbackFunc cb))             \
	X(int, rtcSetSignalingStateChangeCallback, (int pc, rtcSignalingStateCallbackFunc cb))             \
	X(int, rtcSetDataChannelCallback, (int pc, rtcDataChannelCallbackFunc cb))                         \
	X(int, rtcSetTrackCallback, (int pc, rtcTrackCallbackFunc cb))                                     \
	X(int, rtcSetLocalDescription, (int pc, const char *type))                                         \
	X(int, rtcSetRemoteDescription, (int pc, const char *sdp, const char *type))                       \
	X(int, rtcAddRemoteCandidate, (int pc, const char *cand, const char *mid))                         \
	X(int, rtcGetLocalDescription, (int pc, char *buffer, int size))                                   \
	X(int, rtcGetSelectedCandidatePair, (int pc, char *local, int localSize, char *remote, int remoteSize)) \
	X(int, rtcSetOpenCallback, (int id, rtcOpenCallbackFunc cb))                                       \
	X(int, rtcSetClosedCallback, (int id, rtcClosedCallbackFunc cb))                                   \
	X(int, rtcSetErrorCallback, (int id, rtcErrorCallbackFunc cb))                                     \
	X(int, rtcSetMessageCallback, (int id, rtcMessageCallbackFunc cb))                                 \
	X(int, rtcSendMessage, (int id, const char *data, int size))                                       \
	X(int, rtcClose, (int id))                                                                         \
	X(bool, rtcIsOpen, (int id))                                                                       \
	X(int, rtcGetBufferedAmount, (int id))                                                             \
	X(int, rtcCreateDataChannelEx, (int pc, const char *label, const rtcDataChannelInit *init))        \
	X(int, rtcDeleteDataChannel, (int dc))                                                             \
	X(int, rtcDeleteTrack, (int tr))                                                                   \
	X(int, rtcGetDataChannelStream, (int dc))                                                          \
	X(int, rtcGetDataChannelLabel, (int dc, char *buffer, int size))                                   \
	X(int, rtcGetDataChannelProtocol, (int dc, char *buffer, int size))                                \
	X(int, rtcGetDataChannelReliability, (int dc, rtcReliability *reliability))

typedef struct {
#define RTC_FIELD(ret, name, params) ret(*name) params;
	RTC_FUNCS(RTC_FIELD)
#undef RTC_FIELD
	bool next;  // it reads rtcConfigurationNext
} wrtc_lib;

// wrtc_create makes a peer connection of c (as 0.18 to 0.24 lay it out),
// in the layout the library reads: its id, or an RTC_ERR_.
int wrtc_create(const wrtc_lib *rtc, const rtcConfiguration *c);

// wrtc_load opens libdatachannel, once (any thread): its functions, or
// NULL, with why (static text) saying what to install.
const wrtc_lib *wrtc_load(const char **why);

// wrtc_ice_url is an ICE server's URL as libdatachannel reads it: a TURN
// server's user name and credential in it (turn:user:pass@host:port),
// percent-encoded. malloc's.
char *wrtc_ice_url(const char *url, const char *username, const char *credential);

// wrtc_candidate_type is a candidate's type ("host", "srflx", "prflx",
// "relay"; "" for none), from its "typ" (a candidate line, "a=" or not).
// malloc's.
char *wrtc_candidate_type(const char *candidate);

// wrtc_mline_index is the index of the media section whose a=mid is mid
// in sdp (0 when it's not there).
int64_t wrtc_mline_index(const char *sdp, const char *mid);

// wrtc_candidate_line is a candidate as WebRTC's API has it, without the
// "a=" an SDP attribute has. malloc's.
char *wrtc_candidate_line(const char *cand);

// The states, as the package says them.
webrtc_ConnectionState wrtc_connection_state(rtcState s);
webrtc_IceState wrtc_ice_state(rtcIceState s);
webrtc_GatheringState wrtc_gathering_state(rtcGatheringState s);
webrtc_SignalingState wrtc_signaling_state(rtcSignalingState s);

// wrtc_kind is a description's type as the package says it
// ("offer"...); wrtc_type the other way.
webrtc_SdpKind wrtc_kind(const char *type);
const char *wrtc_type(webrtc_SdpKind kind);

// wrtc_reliability is a channel's options as libdatachannel takes them
// (maxPacketLifeTime is nanoseconds: Asuka's Duration).
rtcDataChannelInit wrtc_channel_init(const webrtc_ChannelOptions *o);

#endif
