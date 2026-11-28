// platform/webrtc on the web: the browser's own WebRTC. capture is
// getUserMedia (or a canvas's stream for the test pattern), a Peer is an
// RTCPeerConnection, a channel an RTCDataChannel, and a VideoView a
// <video> of its track. The other peer's sound plays through an <audio>
// of the page's, one a track. The browser routes sound itself: there are
// no routes to choose.
import {
  Facing, TrackKind, IcePolicy, AudioRoute, EventKind, ConnectionState, IceState, SignalingState, GatheringState,
  SdpKind, ChannelEventKind, ChannelState, Fit, WebRTCError,
} from "./webrtc.asuka.js";

/** tracks are the app's tracks by their id: { track, kind, remote, facing, audio (a remote one's element) }. */
const tracks = new Map();
let nextTrack = 0;
const views = new Set();

function keep(track, remote, facing = Facing.front) {
  const id = (remote ? "r" : "l") + ++nextTrack;
  tracks.set(id, { track, kind: track.kind === "video" ? TrackKind.video : TrackKind.audio, remote, facing });
  return id;
}

function trackOf(id) {
  const t = tracks.get(id);
  if (!t) throw new Error("webrtc: no track " + id);
  return t;
}

const asTrack = (id) => {
  const t = tracks.get(id);
  return { id, kind: t.kind, remote: t.remote, facing: t.facing };
};

/** gumError is what getUserMedia's error means to the app. */
function gumError(e) {
  switch (e?.name) {
    case "NotAllowedError":
    case "SecurityError":
      return new WebRTCError("notAllowed");
    case "NotFoundError":
    case "OverconstrainedError":
    case "NotReadableError":
      return new WebRTCError("noDevice");
  }
  return e;
}

/** videoConstraints is what getUserMedia asks of the camera. */
export function videoConstraints(v) {
  return {
    facingMode: v.facing === Facing.back ? "environment" : "user",
    width: { ideal: v.width || 1280 },
    height: { ideal: v.height || 720 },
    frameRate: { ideal: v.frameRate || 30 },
  };
}

/** audioConstraints is what getUserMedia asks of the microphone. */
export function audioConstraints(a) {
  return { echoCancellation: a.echoCancellation, noiseSuppression: a.noiseSuppression, autoGainControl: a.autoGainControl };
}

/** pattern is a moving test pattern's video track: bars, a sweep and a clock, drawn on a canvas. */
function pattern(v) {
  const w = Math.min(v.width || 640, 1280), h = Math.min(v.height || 480, 720);
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  const g = c.getContext("2d");
  const bars = ["#c0c0c0", "#c0c000", "#00c0c0", "#00c000", "#c000c0", "#c00000", "#0000c0"];
  const fps = Math.min(v.frameRate || 30, 30);
  const started = performance.now();
  const draw = () => {
    const t = (performance.now() - started) / 1000;
    bars.forEach((b, i) => { g.fillStyle = b; g.fillRect((i * w) / bars.length, 0, w / bars.length + 1, h); });
    g.fillStyle = "rgba(0,0,0,0.6)";
    g.fillRect(((t * 0.25) % 1) * w - w / 16, 0, w / 8, h);
    g.fillStyle = "#fff";
    const s = Math.round(h / 8);
    const x = (Math.sin(t * 1.3) * 0.4 + 0.5) * (w - s), y = (Math.cos(t * 0.9) * 0.4 + 0.5) * (h - s);
    g.fillRect(x, y, s, s);
    g.font = `${Math.round(h / 10)}px monospace`;
    g.fillText(t.toFixed(1), 12, h - 12);
  };
  draw();
  const timer = setInterval(draw, 1000 / fps);
  const track = c.captureStream(fps).getVideoTracks()[0];
  const stop = track.stop.bind(track);
  track.stop = () => { clearInterval(timer); stop(); };
  return track;
}

const connectionStates = { new: ConnectionState.new, connecting: ConnectionState.connecting, connected: ConnectionState.connected, disconnected: ConnectionState.disconnected, failed: ConnectionState.failed, closed: ConnectionState.closed };
const iceStates = { new: IceState.new, checking: IceState.checking, connected: IceState.connected, completed: IceState.completed, disconnected: IceState.disconnected, failed: IceState.failed, closed: IceState.closed };
const signalingStates = { stable: SignalingState.stable, "have-local-offer": SignalingState.haveLocalOffer, "have-remote-offer": SignalingState.haveRemoteOffer, "have-local-pranswer": SignalingState.haveLocalPranswer, "have-remote-pranswer": SignalingState.haveRemotePranswer, closed: SignalingState.closed };
const gatheringStates = { new: GatheringState.new, gathering: GatheringState.gathering, complete: GatheringState.complete };
const sdpKinds = ["offer", "pranswer", "answer", "rollback"];
const channelStates = { connecting: ChannelState.connecting, open: ChannelState.open, closing: ChannelState.closing, closed: ChannelState.closed };

/** rtcConfig is an RTCConfiguration of the Peer's Config. */
export function rtcConfig(c) {
  return {
    iceServers: (c.iceServers || []).map((s) => {
      const o = { urls: s.urls };
      if (s.username) o.username = s.username;
      if (s.credential) o.credential = s.credential;
      return o;
    }),
    iceTransportPolicy: c.policy === IcePolicy.relay ? "relay" : "all",
  };
}

/** channelInit is an RTCDataChannelInit of ChannelOptions. */
export function channelInit(o) {
  const init = { ordered: o.ordered };
  if (o.maxRetransmits != null) init.maxRetransmits = o.maxRetransmits;
  else if (o.maxPacketLifeTime != null) init.maxPacketLifeTime = Math.round(o.maxPacketLifeTime);
  if (o.protocol) init.protocol = o.protocol;
  if (o.id != null) {
    init.negotiated = true;
    init.id = o.id;
  }
  return init;
}

/** Events keeps what happens until a reader takes it, then hands it on as it comes. */
class Events {
  constructor() {
    this.queue = [];
    this.send = null;
    this.ended = false;
    this.wake = null;
  }
  push(e) {
    if (this.ended) return;
    if (this.send) this.send(e);
    else this.queue.push(e);
  }
  end() {
    this.ended = true;
    this.wake?.();
  }
  /** read hands what's kept and what comes to send, until end or signal's abort. */
  read(signal, send) {
    this.send = send;
    for (const e of this.queue.splice(0)) send(e);
    return new Promise((resolve) => {
      const done = () => { this.send = null; resolve(); };
      if (this.ended) return done();
      this.wake = done;
      signal?.addEventListener("abort", done, { once: true });
    });
  }
}

function channelInfo(dc, handle = 0) {
  return { handle, label: dc.label, ordered: dc.ordered, protocol: dc.protocol, id: dc.id ?? -1 };
}

function makeChannel(dc) {
  dc.binaryType = "arraybuffer";
  const events = new Events();
  dc.addEventListener("open", () => events.push({ kind: ChannelEventKind.open }));
  dc.addEventListener("closing", () => events.push({ kind: ChannelEventKind.closing }));
  dc.addEventListener("close", () => { events.push({ kind: ChannelEventKind.closed }); events.end(); });
  dc.addEventListener("message", (e) => {
    if (typeof e.data === "string") events.push({ kind: ChannelEventKind.message, text: e.data });
    else events.push({ kind: ChannelEventKind.message, binary: true, data: new Uint8Array(e.data) });
  });
  return {
    dc,
    events: (signal, send) => events.read(signal, send),
    info: () => channelInfo(dc),
    state: () => channelStates[dc.readyState] ?? ChannelState.closed,
    sendText(text) {
      if (dc.readyState !== "open") throw new WebRTCError("notOpen");
      dc.send(text);
    },
    sendBytes(data) {
      if (dc.readyState !== "open") throw new WebRTCError("notOpen");
      dc.send(data);
    },
    bufferedAmount: () => dc.bufferedAmount,
    close() { dc.close(); },
  };
}

/** candidateType is the type of a stats report's candidate. */
const candidateType = (report, id) => (id && report.get(id)?.candidateType) || "";

/** statsOf reads the basic stats of an RTCStatsReport. */
export function statsOf(report) {
  const s = {
    bytesSent: 0, bytesReceived: 0, packetsSent: 0, packetsReceived: 0, packetsLost: 0, roundTripTime: 0,
    availableOutgoingBitrate: 0, localCandidate: "", remoteCandidate: "", frameWidth: 0, frameHeight: 0, framesPerSecond: 0,
  };
  let pair = null;
  for (const r of report.values()) {
    if (r.type === "transport" && r.selectedCandidatePairId) pair = report.get(r.selectedCandidatePairId) || pair;
  }
  if (!pair) {
    for (const r of report.values()) {
      if (r.type === "candidate-pair" && (r.selected || (r.nominated && r.state === "succeeded"))) pair = r;
    }
  }
  if (pair) {
    s.bytesSent = pair.bytesSent ?? 0;
    s.bytesReceived = pair.bytesReceived ?? 0;
    s.roundTripTime = (pair.currentRoundTripTime ?? 0) * 1000;
    s.availableOutgoingBitrate = pair.availableOutgoingBitrate ?? 0;
    s.localCandidate = candidateType(report, pair.localCandidateId);
    s.remoteCandidate = candidateType(report, pair.remoteCandidateId);
  }
  for (const r of report.values()) {
    if (r.type === "outbound-rtp") s.packetsSent += r.packetsSent ?? 0;
    if (r.type === "inbound-rtp") {
      s.packetsReceived += r.packetsReceived ?? 0;
      s.packetsLost += Math.max(r.packetsLost ?? 0, 0);
      if (r.kind === "video") {
        s.frameWidth = r.frameWidth ?? s.frameWidth;
        s.frameHeight = r.frameHeight ?? s.frameHeight;
        s.framesPerSecond = r.framesPerSecond ?? s.framesPerSecond;
      }
    }
  }
  return s;
}

function openConnection(config) {
  const pc = new RTCPeerConnection(rtcConfig(config));
  const events = new Events();
  const stream = new MediaStream();
  const senders = new Map(); // a local track's id: its RTCRtpSender
  const remote = new Map(); // a remote MediaStreamTrack: its id
  const channels = new Map(); // a remote channel's handle: its RTCDataChannel
  let nextHandle = 0;
  let closed = false;

  pc.addEventListener("icecandidate", (e) => {
    if (!e.candidate || !e.candidate.candidate) return; // the end: gathering's complete says it
    events.push({ kind: EventKind.candidate, candidate: { candidate: e.candidate.candidate, sdpMid: e.candidate.sdpMid ?? "", sdpMLineIndex: e.candidate.sdpMLineIndex ?? 0 } });
  });
  pc.addEventListener("connectionstatechange", () => events.push({ kind: EventKind.connection, connection: connectionStates[pc.connectionState] ?? ConnectionState.new }));
  pc.addEventListener("iceconnectionstatechange", () => events.push({ kind: EventKind.ice, ice: iceStates[pc.iceConnectionState] ?? IceState.new }));
  pc.addEventListener("signalingstatechange", () => events.push({ kind: EventKind.signaling, signaling: signalingStates[pc.signalingState] ?? SignalingState.stable }));
  pc.addEventListener("icegatheringstatechange", () => events.push({ kind: EventKind.gathering, gathering: gatheringStates[pc.iceGatheringState] ?? GatheringState.new }));
  const ended = (track) => {
    const id = remote.get(track);
    if (!id) return;
    remote.delete(track);
    const t = tracks.get(id);
    t?.audio?.remove();
    tracks.delete(id);
    events.push({ kind: EventKind.trackEnded, track: { id, kind: t?.kind ?? TrackKind.audio, remote: true } });
  };
  pc.addEventListener("track", (e) => {
    const track = e.track;
    if (remote.has(track)) return;
    const id = keep(track, true);
    remote.set(track, id);
    if (track.kind === "audio") {
      // The other peer's voice, played by the page.
      const a = document.createElement("audio");
      a.autoplay = true;
      a.srcObject = new MediaStream([track]);
      a.style.display = "none";
      document.body.appendChild(a);
      a.play?.().catch(() => {});
      tracks.get(id).audio = a;
    }
    track.addEventListener("ended", () => ended(track));
    for (const s of e.streams) s.addEventListener("removetrack", (r) => { if (r.track === track) ended(track); });
    events.push({ kind: EventKind.track, track: asTrack(id) });
  });
  pc.addEventListener("datachannel", (e) => {
    const handle = ++nextHandle;
    channels.set(handle, e.channel);
    e.channel.binaryType = "arraybuffer";
    events.push({ kind: EventKind.channel, channel: channelInfo(e.channel, handle) });
  });

  const description = (d) => ({ kind: Math.max(sdpKinds.indexOf(d.type), 0), sdp: d.sdp ?? "" });
  const init = (d) => ({ type: sdpKinds[d.kind] ?? "offer", sdp: d.sdp });

  return {
    pc,
    events: (signal, send) => events.read(signal, send),
    addTrack(id) {
      if (senders.has(id)) return;
      const t = trackOf(id);
      senders.set(id, pc.addTrack(t.track, stream));
    },
    removeTrack(id) {
      const s = senders.get(id);
      if (!s) return;
      senders.delete(id);
      pc.removeTrack(s);
    },
    replaced(oldTrack, track) {
      for (const s of pc.getSenders()) if (s.track === oldTrack) s.replaceTrack(track);
    },
    async createOffer(_signal, iceRestart) { return description(await pc.createOffer({ iceRestart })); },
    async createAnswer() { return description(await pc.createAnswer()); },
    async setLocal(_signal, d) { await pc.setLocalDescription(init(d)); },
    async setRemote(_signal, d) {
      await pc.setRemoteDescription(d.kind === SdpKind.rollback ? { type: "rollback" } : init(d));
    },
    async addCandidate(_signal, c) {
      if (!c.candidate) return pc.addIceCandidate(); // the end of the other's candidates
      await pc.addIceCandidate({ candidate: c.candidate, sdpMid: c.sdpMid || null, sdpMLineIndex: c.sdpMid ? null : c.sdpMLineIndex });
    },
    createChannel: (label, o) => makeChannel(pc.createDataChannel(label, channelInit(o))),
    remoteChannel(handle) {
      const dc = channels.get(handle);
      if (!dc) throw new Error("webrtc: no channel " + handle);
      channels.delete(handle);
      return makeChannel(dc);
    },
    async stats() { return statsOf(await pc.getStats()); },
    close() {
      if (closed) return;
      closed = true;
      connections.delete(this);
      for (const t of [...remote.keys()]) ended(t);
      pc.close();
      events.push({ kind: EventKind.connection, connection: ConnectionState.closed });
      events.end();
    },
  };
}

const connections = new Set();

export function makeEngine() {
  return {
    async capture(signal, audio, video) {
      if (!navigator.mediaDevices?.getUserMedia) throw new WebRTCError("notAvailable");
      const ask = {};
      if (audio) ask.audio = audioConstraints(audio);
      if (video && !video.pattern) ask.video = videoConstraints(video);
      let got = null;
      if (ask.audio || ask.video) {
        try {
          got = await navigator.mediaDevices.getUserMedia(ask);
        } catch (e) {
          throw gumError(e);
        }
      }
      if (signal?.aborted) {
        got?.getTracks().forEach((t) => t.stop());
        throw new DOMException("cancelled", "AbortError");
      }
      const media = {};
      const a = got?.getAudioTracks()[0];
      if (a) media.audio = asTrack(keep(a, false));
      const v = video?.pattern ? pattern(video) : got?.getVideoTracks()[0];
      if (v) media.video = asTrack(keep(v, false, video.facing));
      return media;
    },
    setEnabled(id, on) { trackOf(id).track.enabled = on; },
    stopTrack(id) {
      const t = tracks.get(id);
      if (!t || t.remote) return;
      t.track.stop();
      for (const c of connections) c.removeTrack(id);
      for (const v of views) if (v.id === id) v.show(null);
    },
    async switchCamera(_signal, id) {
      const t = trackOf(id);
      if (t.remote || t.kind !== TrackKind.video) throw new Error("webrtc: not a camera's track");
      const facing = t.facing === Facing.front ? Facing.back : Facing.front;
      const settings = t.track.getSettings?.() ?? {};
      let next;
      try {
        const s = await navigator.mediaDevices.getUserMedia({
          video: videoConstraints({ facing, width: settings.width, height: settings.height, frameRate: Math.round(settings.frameRate || 30) }),
        });
        next = s.getVideoTracks()[0];
      } catch (e) {
        throw gumError(e);
      }
      next.enabled = t.track.enabled;
      const old = t.track;
      for (const c of connections) c.replaced(old, next);
      old.stop();
      t.track = next;
      t.facing = facing;
      for (const v of views) if (v.id === id) v.show(next);
      return facing;
    },
    connect(config) {
      const c = openConnection(config);
      connections.add(c);
      return c;
    },
    audioRoutes: () => [],
    audioRoute: () => AudioRoute.speaker,
    setAudioRoute() {},
  };
}

// Connections and channels are the engine's and a connection's to make.
const unsupported = (msg) => new globalThis.asuka.native.UnsupportedError(msg);

export function makeConnection() {
  throw unsupported("webrtc: a connection is made by Engine.connect");
}

export function makeDataChannel() {
  throw unsupported("webrtc: a data channel is made by a connection");
}

/** makeVideoView is a <video> of a track: the app's or the other peer's. */
export function makeVideoView(events) {
  const el = document.createElement("div");
  el.style.cssText = "position:relative;overflow:hidden;background:#000";
  const video = document.createElement("video");
  video.muted = true; // its sound is the page's <audio>'s, or the user's own
  video.playsInline = true;
  video.autoplay = true;
  video.style.cssText = "position:absolute;inset:0;width:100%;height:100%;object-fit:cover;display:block";
  el.appendChild(video);
  let size = [0, 0];
  const resized = () => {
    const s = [video.videoWidth, video.videoHeight];
    if (!s[0] || !s[1] || (s[0] === size[0] && s[1] === size[1])) return;
    size = s;
    events.resize({ width: s[0], height: s[1] });
  };
  video.addEventListener("resize", resized);
  video.addEventListener("loadedmetadata", resized);
  const v = {
    el,
    id: "",
    show(track) {
      video.srcObject = track ? new MediaStream([track]) : null;
      if (track) video.play?.().catch(() => {});
    },
    setTrack(t) {
      if (t.id === v.id) return;
      v.id = t.id;
      v.show(tracks.get(t.id)?.track ?? null);
    },
    setMirror(on) { video.style.transform = on ? "scaleX(-1)" : ""; },
    setFit(f) { video.style.objectFit = f === Fit.contain ? "contain" : "cover"; },
    destroy() {
      views.delete(v);
      video.srcObject = null;
    },
  };
  views.add(v);
  return v;
}
