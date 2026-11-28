// What platform/webrtc's JavaScript makes of Asuka's values for the
// browser (RTCConfiguration, RTCDataChannelInit, getUserMedia's
// constraints) and of the browser's for Asuka (a stats report), and a
// data channel's events, kept until they're read, against a stubbed
// RTCDataChannel.
import { test } from "node:test";
import assert from "node:assert/strict";

class DeclaredError extends Error {
  constructor(index, message) {
    super(message);
    this.index = index;
  }
}
globalThis.asuka = {
  native: {
    DeclaredError, UnsupportedError: Error, NativeWriter: class {}, NativeReader: class {},
    registerModule() {}, retain() {}, object() {},
  },
};

const { rtcConfig, channelInit, videoConstraints, audioConstraints, statsOf, makeEngine } = await import("./webrtc.js");
const { IcePolicy, Facing, ChannelEventKind, ChannelState, EventKind } = await import("./webrtc.asuka.js");

test("a Config is an RTCConfiguration: TURN's credentials, the relay policy", () => {
  assert.deepEqual(rtcConfig({ iceServers: [{ urls: ["stun:s:3478"], username: "", credential: "" }, { urls: ["turn:t:3478"], username: "u", credential: "p" }], policy: IcePolicy.relay }), {
    iceServers: [{ urls: ["stun:s:3478"] }, { urls: ["turn:t:3478"], username: "u", credential: "p" }],
    iceTransportPolicy: "relay",
  });
  assert.equal(rtcConfig({ iceServers: [], policy: IcePolicy.all }).iceTransportPolicy, "all");
});

test("channel options: reliable, partly reliable, negotiated", () => {
  assert.deepEqual(channelInit({ ordered: true, maxRetransmits: null, maxPacketLifeTime: null, protocol: "", id: null }), { ordered: true });
  assert.deepEqual(channelInit({ ordered: false, maxRetransmits: 0, maxPacketLifeTime: null, protocol: "game", id: null }), { ordered: false, maxRetransmits: 0, protocol: "game" });
  assert.deepEqual(channelInit({ ordered: true, maxRetransmits: null, maxPacketLifeTime: 500, protocol: "", id: 3 }), { ordered: true, maxPacketLifeTime: 500, negotiated: true, id: 3 });
});

test("capture's constraints", () => {
  assert.deepEqual(videoConstraints({ facing: Facing.back, width: 640, height: 480, frameRate: 15 }), {
    facingMode: "environment", width: { ideal: 640 }, height: { ideal: 480 }, frameRate: { ideal: 15 },
  });
  assert.equal(videoConstraints({ facing: Facing.front }).facingMode, "user");
  assert.deepEqual(audioConstraints({ echoCancellation: true, noiseSuppression: false, autoGainControl: true }), { echoCancellation: true, noiseSuppression: false, autoGainControl: true });
});

test("stats: the selected pair's, and the media's", () => {
  const report = new Map(Object.entries({
    T: { type: "transport", selectedCandidatePairId: "P" },
    P: { type: "candidate-pair", bytesSent: 100, bytesReceived: 250, currentRoundTripTime: 0.025, availableOutgoingBitrate: 3e6, localCandidateId: "L", remoteCandidateId: "R" },
    Q: { type: "candidate-pair", bytesSent: 1, nominated: true, state: "succeeded" },
    L: { type: "local-candidate", candidateType: "srflx" },
    R: { type: "remote-candidate", candidateType: "relay" },
    O: { type: "outbound-rtp", kind: "video", packetsSent: 7 },
    A: { type: "inbound-rtp", kind: "audio", packetsReceived: 3, packetsLost: 1 },
    V: { type: "inbound-rtp", kind: "video", packetsReceived: 5, packetsLost: 2, frameWidth: 640, frameHeight: 480, framesPerSecond: 30 },
  }));
  assert.deepEqual(statsOf(report), {
    bytesSent: 100, bytesReceived: 250, packetsSent: 7, packetsReceived: 8, packetsLost: 3, roundTripTime: 25,
    availableOutgoingBitrate: 3e6, localCandidate: "srflx", remoteCandidate: "relay", frameWidth: 640, frameHeight: 480, framesPerSecond: 30,
  });
  assert.equal(statsOf(new Map()).localCandidate, "");
});

// An RTCDataChannel's stand-in, and an RTCPeerConnection's that makes it.
class Channel extends EventTarget {
  constructor(label, init) {
    super();
    Object.assign(this, { label, ordered: init.ordered ?? true, protocol: init.protocol ?? "", id: 1, readyState: "connecting", bufferedAmount: 0, sent: [] });
  }
  send(d) { this.sent.push(d); }
  close() { this.readyState = "closed"; this.dispatchEvent(new Event("close")); }
}
class PeerConnection extends EventTarget {
  constructor(c) {
    super();
    this.config = c;
    this.connectionState = "new";
  }
  createDataChannel(label, init) { return (this.channel = new Channel(label, init)); }
  close() { this.connectionState = "closed"; }
}
globalThis.RTCPeerConnection = PeerConnection;
globalThis.MediaStream = class {};

test("a channel's events wait for their reader, then come as they happen", async () => {
  const conn = makeEngine().connect({ iceServers: [], policy: IcePolicy.all });
  const ch = conn.createChannel("chat", { ordered: true, protocol: "" });
  const dc = conn.pc.channel;
  assert.equal(ch.state(), ChannelState.connecting);
  assert.throws(() => ch.sendText("early"), /isn.t open/);
  dc.readyState = "open";
  dc.dispatchEvent(new Event("open"));
  dc.dispatchEvent(Object.assign(new Event("message"), { data: "hi" }));
  const got = [];
  const done = ch.events(null, (e) => got.push(e));
  dc.dispatchEvent(Object.assign(new Event("message"), { data: new Uint8Array([1, 2]).buffer }));
  ch.sendText("hello");
  assert.deepEqual(dc.sent, ["hello"]);
  ch.close();
  await done; // a closed channel's events end
  assert.deepEqual(got.map((e) => e.kind), [ChannelEventKind.open, ChannelEventKind.message, ChannelEventKind.message, ChannelEventKind.closed]);
  assert.equal(got[1].text, "hi");
  assert.deepEqual([...got[2].data], [1, 2]);
  assert.equal(got[2].binary, true);
});

test("a connection's events end as it closes; cancelling one ends the read", async () => {
  const conn = makeEngine().connect({ iceServers: [], policy: IcePolicy.all });
  const abort = new AbortController();
  const got = [];
  const reading = conn.events(abort.signal, (e) => got.push(e));
  abort.abort();
  await reading;
  conn.close();
  const after = [];
  await conn.events(null, (e) => after.push(e)); // closed: what was kept, then the end
  assert.deepEqual(after.map((e) => e.kind), [EventKind.connection]);
  assert.equal(conn.pc.connectionState, "closed");
});
