// platform/webrtc on iOS and macOS: Google's WebRTC (WebRTC.framework,
// stasel/WebRTC's build). One RTCPeerConnectionFactory for the app; the
// tracks it makes and the ones peers receive are kept by ID (local "l1",
// remote "r1": unique in the app, two peers in it too), which Connection
// and VideoView look up. Capture is RTCCameraVideoCapturer's (the format
// nearest what's asked), or a test pattern of this file's on the
// simulator (which has no camera) and where Video.pattern asks. WebRTC
// calls its delegates on its own threads: everything hops to the main
// thread, in order, where Asuka's calls run. On iOS a call's audio
// session is RTCAudioSession's, play and record in voice chat mode;
// its sound plays on the speaker when video was captured, else in the
// earpiece.
import Asuka
import AVFoundation
import Foundation
import WebRTC
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

// MARK: - The factory and the tracks

@MainActor enum RTC {
    static let factory: RTCPeerConnectionFactory = {
        RTCInitializeSSL()
        return RTCPeerConnectionFactory(encoderFactory: RTCDefaultVideoEncoderFactory(), decoderFactory: RTCDefaultVideoDecoderFactory())
    }()

    /// A track the app has: its own (with what captures it) or a peer's.
    final class Entry {
        let track: RTCMediaStreamTrack
        let kind: TrackKind
        var facing: Facing
        var camera: RTCCameraVideoCapturer?
        var pattern: PatternCapturer?
        var video: Video?
        init(_ track: RTCMediaStreamTrack, _ kind: TrackKind, _ facing: Facing = .front) {
            self.track = track
            self.kind = kind
            self.facing = facing
        }

        func stop() {
            camera?.stopCapture()
            camera = nil
            pattern?.stop()
            pattern = nil
            track.isEnabled = false
        }
    }

    static var tracks: [String: Entry] = [:]
    static var next = 0

    static func id(_ prefix: String) -> String {
        next += 1
        return "\(prefix)\(next)"
    }
}

/// The cameras, by facing (a Mac's first, for either).
private func camera(_ facing: Facing) -> AVCaptureDevice? {
    let all = RTCCameraVideoCapturer.captureDevices()
    #if os(iOS)
    let want: AVCaptureDevice.Position = facing == .back ? .back : .front
    return all.first { $0.position == want } ?? all.first
    #else
    return all.first
    #endif
}

/// The format of device nearest w×h, and the frame rate it takes nearest fps.
private func format(_ device: AVCaptureDevice, _ w: Int, _ h: Int, _ fps: Int) -> (AVCaptureDevice.Format, Int)? {
    let wanted = max(w, 1) * max(h, 1)
    var best: (AVCaptureDevice.Format, Int)?
    var bestScore = Int.max
    for f in RTCCameraVideoCapturer.supportedFormats(for: device) {
        let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
        let area = Int(d.width) * Int(d.height)
        let top = f.videoSupportedFrameRateRanges.map { $0.maxFrameRate }.max() ?? 30
        let rate = min(Double(max(fps, 1)), top)
        var score = abs(area - wanted)
        if rate < Double(fps) { score += wanted / 4 } // a format that can't keep up loses
        if score < bestScore {
            bestScore = score
            best = (f, Int(rate))
        }
    }
    return best
}

private func allowed(_ media: AVMediaType) async -> Bool {
    switch AVCaptureDevice.authorizationStatus(for: media) {
    case .authorized: return true
    case .notDetermined: return await AVCaptureDevice.requestAccess(for: media)
    default: return false
    }
}

// MARK: - A test pattern

/// Frames of a moving test pattern (color bars, a bar that sweeps across,
/// a square that bounces), NV12, pushed to a video source at fps from a
/// queue of its own: what a device without a camera sends.
final class PatternCapturer: RTCVideoCapturer, @unchecked Sendable {
    private let queue = DispatchQueue(label: "asuka.webrtc.pattern")
    private var timer: DispatchSourceTimer?
    private var pool: CVPixelBufferPool?
    private let width: Int
    private let height: Int
    private let fps: Int
    private var n = 0
    var tint: UInt8 = 0 // the other "camera" shows other colors

    init(source: RTCVideoSource, width: Int, height: Int, fps: Int) {
        self.width = (min(max(width, 160), 1280) / 16) * 16
        self.height = (min(max(height, 120), 720) / 16) * 16
        self.fps = min(max(fps, 1), 30)
        super.init(delegate: source)
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            kCVPixelBufferWidthKey as String: self.width,
            kCVPixelBufferHeightKey as String: self.height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
    }

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / fps))
        t.setEventHandler { [weak self] in self?.frame() }
        timer = t
        t.resume()
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func frame() {
        guard let pool else { return }
        var pb: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb) == kCVReturnSuccess, let buf = pb else { return }
        CVPixelBufferLockBaseAddress(buf, [])
        let w = width, h = height
        let yp = CVPixelBufferGetBaseAddressOfPlane(buf, 0)!.assumingMemoryBound(to: UInt8.self)
        let ys = CVPixelBufferGetBytesPerRowOfPlane(buf, 0)
        let uvp = CVPixelBufferGetBaseAddressOfPlane(buf, 1)!.assumingMemoryBound(to: UInt8.self)
        let uvs = CVPixelBufferGetBytesPerRowOfPlane(buf, 1)
        // Seven bars (Y, U, V), a sweeping bright bar, a bouncing square.
        let bars: [(UInt8, UInt8, UInt8)] = [(235, 128, 128), (210, 16, 146), (170, 166, 16), (145, 54, 34), (106, 202, 222), (81, 90, 240), (41, 240, 110)]
        let sweep = (n * 6) % w
        let side = h / 4
        let period = max(h - side, 1)
        let bounce = (n * 4) % (2 * period)
        let sy = bounce < period ? bounce : 2 * period - bounce
        let sx = (n * 3) % max(w - side, 1)
        for y in 0..<h {
            let row = yp + y * ys
            for x in 0..<w {
                var v = bars[min(x * bars.count / w, bars.count - 1)].0
                if abs(x - sweep) < 6 { v = 255 }
                if x >= sx && x < sx + side && y >= sy && y < sy + side { v = 16 }
                row[x] = v
            }
        }
        for y in 0..<(h / 2) {
            let row = uvp + y * uvs
            for x in 0..<(w / 2) {
                let b = bars[min(2 * x * bars.count / w, bars.count - 1)]
                row[2 * x] = b.1 &+ tint
                row[2 * x + 1] = b.2 &- tint
            }
        }
        CVPixelBufferUnlockBaseAddress(buf, [])
        n += 1
        let ns = Int64(DispatchTime.now().uptimeNanoseconds)
        let f = RTCVideoFrame(buffer: RTCCVPixelBuffer(pixelBuffer: buf), rotation: ._0, timeStampNs: ns)
        delegate?.capturer(self, didCapture: f)
    }
}

// MARK: - The engine

@MainActor final class SwiftEngine: Engine {
    private var speakerByDefault = false

    func capture(audio: Audio?, video: Video?) async throws -> Media {
        var media = Media()
        if audio != nil {
            guard await allowed(.audio) else { throw WebRTCError.notAllowed }
        }
        var useCamera = false
        if let v = video {
            #if targetEnvironment(simulator)
            useCamera = false
            #else
            useCamera = !v.pattern
            #endif
            if useCamera {
                guard await allowed(.video) else { throw WebRTCError.notAllowed }
                if camera(v.facing) == nil { throw WebRTCError.noDevice }
            }
        }
        #if os(iOS)
        configureSession(video: video != nil)
        #endif
        if let a = audio {
            let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: [
                "googEchoCancellation": a.echoCancellation ? "true" : "false",
                "googNoiseSuppression": a.noiseSuppression ? "true" : "false",
                "googAutoGainControl": a.autoGainControl ? "true" : "false",
                "googHighpassFilter": "true",
            ])
            let source = RTC.factory.audioSource(with: constraints)
            let id = RTC.id("l")
            let track = RTC.factory.audioTrack(with: source, trackId: id)
            RTC.tracks[id] = RTC.Entry(track, .audio)
            media.audio = Track(id: id, kind: .audio)
        }
        if let v = video {
            let source = RTC.factory.videoSource()
            let id = RTC.id("l")
            let track = RTC.factory.videoTrack(with: source, trackId: id)
            let e = RTC.Entry(track, .video, v.facing)
            e.video = v
            if useCamera, let device = camera(v.facing) {
                let cap = RTCCameraVideoCapturer(delegate: source)
                e.camera = cap
                try await start(cap, device, v)
            } else {
                let p = PatternCapturer(source: source, width: v.width, height: v.height, fps: v.frameRate)
                p.tint = v.facing == .back ? 40 : 0
                e.pattern = p
                p.start()
            }
            RTC.tracks[id] = e
            media.video = Track(id: id, kind: .video, facing: v.facing)
        }
        return media
    }

    private func start(_ cap: RTCCameraVideoCapturer, _ device: AVCaptureDevice, _ v: Video) async throws {
        guard let (f, fps) = format(device, v.width > 0 ? v.width : 1280, v.height > 0 ? v.height : 720, v.frameRate > 0 ? v.frameRate : 30) else {
            throw WebRTCError.noDevice
        }
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            cap.startCapture(with: device, format: f, fps: fps) { err in
                if let err { c.resume(throwing: err) } else { c.resume() }
            }
        }
    }

    func setEnabled(track: String, enabled: Bool) throws {
        RTC.tracks[track]?.track.isEnabled = enabled
    }

    func stopTrack(track: String) throws {
        if let e = RTC.tracks.removeValue(forKey: track) {
            e.stop()
        }
    }

    func switchCamera(track: String) async throws -> Facing {
        guard let e = RTC.tracks[track], e.kind == .video else { throw WebRTCError.noDevice }
        let to: Facing = e.facing == .front ? .back : .front
        if let cap = e.camera {
            guard let device = camera(to) else { throw WebRTCError.noDevice }
            await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in cap.stopCapture { c.resume() } }
            try await start(cap, device, e.video ?? Video(facing: to, width: 1280, height: 720, frameRate: 30))
        } else if let p = e.pattern {
            p.tint = to == .back ? 40 : 0
        }
        e.facing = to
        return to
    }

    func connect(config: Config) throws -> any Connection { try SwiftConnection(config) }

    // MARK: Audio routes

    #if os(iOS)
    private func configureSession(video: Bool) {
        let s = RTCAudioSession.sharedInstance()
        s.lockForConfiguration()
        defer { s.unlockForConfiguration() }
        try? s.setCategory(.playAndRecord, mode: .voiceChat, options: [.allowBluetoothHFP, .allowBluetoothA2DP])
        try? s.setActive(true)
        speakerByDefault = video
        if video, Self.outputRoute() == .earpiece {
            try? s.overrideOutputAudioPort(.speaker)
        }
        let c = RTCAudioSessionConfiguration.webRTC()
        c.category = AVAudioSession.Category.playAndRecord.rawValue
        c.mode = AVAudioSession.Mode.voiceChat.rawValue
        c.categoryOptions = [.allowBluetoothHFP, .allowBluetoothA2DP]
        RTCAudioSessionConfiguration.setWebRTC(c)
    }

    private static func routeOf(_ port: AVAudioSession.Port) -> AudioRoute? {
        switch port {
        case .builtInReceiver: return .earpiece
        case .builtInSpeaker: return .speaker
        case .headphones, .headsetMic, .usbAudio, .lineOut: return .headset
        case .bluetoothHFP, .bluetoothA2DP, .bluetoothLE: return .bluetooth
        default: return nil
        }
    }

    private static func outputRoute() -> AudioRoute {
        let outs = AVAudioSession.sharedInstance().currentRoute.outputs
        return outs.compactMap { routeOf($0.portType) }.first ?? .speaker
    }

    func audioRoutes() throws -> [AudioRoute] {
        let s = AVAudioSession.sharedInstance()
        var out: [AudioRoute] = []
        if UIDevice.current.userInterfaceIdiom == .phone { out.append(.earpiece) }
        out.append(.speaker)
        let ports = (s.availableInputs ?? []).map(\.portType) + s.currentRoute.outputs.map(\.portType)
        if ports.contains(where: { [.headphones, .headsetMic, .usbAudio].contains($0) }) { out.append(.headset) }
        if ports.contains(where: { [.bluetoothHFP, .bluetoothA2DP, .bluetoothLE].contains($0) }) { out.append(.bluetooth) }
        return out
    }

    func audioRoute() throws -> AudioRoute { Self.outputRoute() }

    func setAudioRoute(route: AudioRoute) throws {
        let s = RTCAudioSession.sharedInstance()
        s.lockForConfiguration()
        defer { s.unlockForConfiguration() }
        let inputs = AVAudioSession.sharedInstance().availableInputs ?? []
        switch route {
        case .speaker:
            try s.overrideOutputAudioPort(.speaker)
        case .earpiece:
            try s.overrideOutputAudioPort(.none)
            if let mic = inputs.first(where: { $0.portType == .builtInMic }) { try s.setPreferredInput(mic) }
        case .headset:
            try s.overrideOutputAudioPort(.none)
            if let mic = inputs.first(where: { $0.portType == .headsetMic || $0.portType == .usbAudio }) { try s.setPreferredInput(mic) }
        case .bluetooth:
            try s.overrideOutputAudioPort(.none)
            if let bt = inputs.first(where: { $0.portType == .bluetoothHFP || $0.portType == .bluetoothLE }) { try s.setPreferredInput(bt) }
        default:
            break
        }
    }
    #else
    // A Mac routes sound as its user set it.
    func audioRoutes() throws -> [AudioRoute] { [] }
    func audioRoute() throws -> AudioRoute { .speaker }
    func setAudioRoute(route: AudioRoute) throws {}
    #endif
}

@MainActor func makeEngine() throws -> any Engine { SwiftEngine() }
@MainActor func makeConnection() throws -> any Connection { throw AsukaNativeError.unsupported("webrtc: a connection is made by Engine.connect") }
@MainActor func makeDataChannel() throws -> any DataChannel { throw AsukaNativeError.unsupported("webrtc: a data channel is made by a connection") }

// MARK: - Connections

/// Calls body on the main thread, after what was posted before it.
private func onMain(_ body: @escaping @MainActor () -> Void) {
    DispatchQueue.main.async { MainActor.assumeIsolated { body() } }
}

private func connectionState(_ s: RTCPeerConnectionState) -> ConnectionState {
    switch s {
    case .new: return .new
    case .connecting: return .connecting
    case .connected: return .connected
    case .disconnected: return .disconnected
    case .failed: return .failed
    case .closed: return .closed
    @unknown default: return .new
    }
}

private func iceState(_ s: RTCIceConnectionState) -> IceState {
    switch s {
    case .new: return .new
    case .checking: return .checking
    case .connected: return .connected
    case .completed: return .completed
    case .failed: return .failed
    case .disconnected: return .disconnected
    case .closed: return .closed
    default: return .new
    }
}

private func signalingState(_ s: RTCSignalingState) -> SignalingState {
    switch s {
    case .stable: return .stable
    case .haveLocalOffer: return .haveLocalOffer
    case .haveLocalPrAnswer: return .haveLocalPranswer
    case .haveRemoteOffer: return .haveRemoteOffer
    case .haveRemotePrAnswer: return .haveRemotePranswer
    case .closed: return .closed
    @unknown default: return .stable
    }
}

private func gatheringState(_ s: RTCIceGatheringState) -> GatheringState {
    switch s {
    case .new: return .new
    case .gathering: return .gathering
    case .complete: return .complete
    @unknown default: return .new
    }
}

private func sdpType(_ k: SdpKind) -> RTCSdpType {
    switch k {
    case .offer: return .offer
    case .pranswer: return .prAnswer
    case .rollback: return .rollback
    default: return .answer
    }
}

private func sdpKind(_ t: RTCSdpType) -> SdpKind {
    switch t {
    case .offer: return .offer
    case .prAnswer: return .pranswer
    case .rollback: return .rollback
    default: return .answer
    }
}

/// What WebRTC tells a connection, on its own threads: to the main thread.
private final class PeerDelegate: NSObject, RTCPeerConnectionDelegate {
    weak var owner: SwiftConnection?

    func peerConnection(_ pc: RTCPeerConnection, didChange stateChanged: RTCSignalingState) {
        onMain { [weak self] in self?.owner?.emit(PeerEvent(kind: .signaling, signaling: signalingState(stateChanged))) }
    }
    func peerConnection(_ pc: RTCPeerConnection, didAdd stream: RTCMediaStream) {}
    func peerConnection(_ pc: RTCPeerConnection, didRemove stream: RTCMediaStream) {}
    func peerConnectionShouldNegotiate(_ pc: RTCPeerConnection) {}
    func peerConnection(_ pc: RTCPeerConnection, didChange newState: RTCIceConnectionState) {}
    func peerConnection(_ pc: RTCPeerConnection, didChangeStandardizedIceConnectionState newState: RTCIceConnectionState) {
        onMain { [weak self] in self?.owner?.emit(PeerEvent(kind: .ice, ice: iceState(newState))) }
    }
    func peerConnection(_ pc: RTCPeerConnection, didChange newState: RTCPeerConnectionState) {
        onMain { [weak self] in self?.owner?.emit(PeerEvent(kind: .connection, connection: connectionState(newState))) }
    }
    func peerConnection(_ pc: RTCPeerConnection, didChange newState: RTCIceGatheringState) {
        onMain { [weak self] in self?.owner?.emit(PeerEvent(kind: .gathering, gathering: gatheringState(newState))) }
    }
    func peerConnection(_ pc: RTCPeerConnection, didGenerate candidate: RTCIceCandidate) {
        let c = Candidate(candidate: candidate.sdp, sdpMid: candidate.sdpMid ?? "", sdpMLineIndex: Int(candidate.sdpMLineIndex))
        onMain { [weak self] in self?.owner?.emit(PeerEvent(kind: .candidate, candidate: c)) }
    }
    func peerConnection(_ pc: RTCPeerConnection, didRemove candidates: [RTCIceCandidate]) {}
    func peerConnection(_ pc: RTCPeerConnection, didOpen dataChannel: RTCDataChannel) {
        // The channel's events go to the main thread after this does, so
        // its wrapper is there for them.
        let proxy = ChannelDelegate()
        onMain { [weak self] in self?.owner?.opened(dataChannel, proxy) }
        dataChannel.delegate = proxy
    }
    func peerConnection(_ pc: RTCPeerConnection, didAdd rtpReceiver: RTCRtpReceiver, streams mediaStreams: [RTCMediaStream]) {
        onMain { [weak self] in self?.owner?.received(rtpReceiver) }
    }
    func peerConnection(_ pc: RTCPeerConnection, didRemove rtpReceiver: RTCRtpReceiver) {
        onMain { [weak self] in self?.owner?.lost(rtpReceiver) }
    }
}

@MainActor final class SwiftConnection: Connection {
    private var pc: RTCPeerConnection?
    private let delegate = PeerDelegate()
    private var pending: [PeerEvent] = []
    private var sink: ((PeerEvent) -> Void)?
    private var waiter: CheckedContinuation<Void, Never>?
    private var senders: [String: RTCRtpSender] = [:]
    /// The other peer's tracks, by their receiver's ID: their IDs here.
    private var remotes: [String: String] = [:]
    private var channels: [Int: SwiftDataChannel] = [:]
    private var nextChannel = 0

    init(_ config: Config) throws {
        let c = RTCConfiguration()
        c.sdpSemantics = .unifiedPlan
        c.iceServers = config.iceServers.map { s in
            RTCIceServer(urlStrings: s.urls, username: s.username.isEmpty ? nil : s.username, credential: s.credential.isEmpty ? nil : s.credential)
        }
        c.iceTransportPolicy = config.policy == .relay ? .relay : .all
        c.continualGatheringPolicy = .gatherContinually
        c.bundlePolicy = .maxBundle
        c.rtcpMuxPolicy = .require
        let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: ["DtlsSrtpKeyAgreement": "true"])
        guard let pc = RTC.factory.peerConnection(with: c, constraints: constraints, delegate: delegate) else {
            throw AsukaNativeError.unsupported("webrtc: couldn't make a connection")
        }
        self.pc = pc
        delegate.owner = self
    }

    func emit(_ e: PeerEvent) {
        if let s = sink { s(e) } else { pending.append(e) }
    }

    func events(send: @escaping (PeerEvent) -> Void) async throws {
        guard pc != nil else { return }
        sink = send
        let p = pending
        pending = []
        for e in p { send(e) }
        await withTaskCancellationHandler {
            await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
                if pc == nil { c.resume() } else { waiter = c }
            }
        } onCancel: {
            onMain { [weak self] in self?.endEvents() }
        }
        sink = nil
    }

    private func endEvents() {
        let w = waiter
        waiter = nil
        w?.resume()
    }

    private func live() throws -> RTCPeerConnection {
        guard let pc else { throw WebRTCError.closed }
        return pc
    }

    func received(_ r: RTCRtpReceiver) {
        guard pc != nil, let track = r.track, remotes[r.receiverId] == nil else { return }
        let kind: TrackKind = track.kind == kRTCMediaStreamTrackKindVideo ? .video : .audio
        let id = RTC.id("r")
        remotes[r.receiverId] = id
        RTC.tracks[id] = RTC.Entry(track, kind)
        emit(PeerEvent(kind: .track, track: Track(id: id, kind: kind, remote: true)))
    }

    func lost(_ r: RTCRtpReceiver) {
        guard let id = remotes.removeValue(forKey: r.receiverId), let e = RTC.tracks.removeValue(forKey: id) else { return }
        emit(PeerEvent(kind: .trackEnded, track: Track(id: id, kind: e.kind, remote: true)))
    }

    func opened(_ dc: RTCDataChannel, _ proxy: ChannelDelegate) {
        guard pc != nil else {
            dc.close()
            return
        }
        nextChannel += 1
        let ch = SwiftDataChannel(dc, proxy)
        channels[nextChannel] = ch
        var info = (try? ch.info()) ?? ChannelInfo()
        info.handle = nextChannel
        emit(PeerEvent(kind: .channel, channel: info))
    }

    func addTrack(track: String) throws {
        let pc = try live()
        guard let e = RTC.tracks[track] else { throw WebRTCError.closed }
        guard senders[track] == nil else { return }
        guard let s = pc.add(e.track, streamIds: ["asuka"]) else {
            throw AsukaNativeError.unsupported("webrtc: couldn't add the track")
        }
        senders[track] = s
    }

    func removeTrack(track: String) throws {
        let pc = try live()
        if let s = senders.removeValue(forKey: track) {
            pc.removeTrack(s)
        }
    }

    private func constraints(_ iceRestart: Bool) -> RTCMediaConstraints {
        var m = ["OfferToReceiveAudio": "true", "OfferToReceiveVideo": "true"]
        if iceRestart { m["IceRestart"] = "true" }
        return RTCMediaConstraints(mandatoryConstraints: m, optionalConstraints: nil)
    }

    nonisolated private static func described(_ d: RTCSessionDescription?, _ err: Error?, _ c: CheckedContinuation<Description, Error>) {
        if let d {
            c.resume(returning: Description(kind: sdpKind(d.type), sdp: d.sdp))
        } else {
            c.resume(throwing: err ?? AsukaNativeError.unsupported("webrtc: no description"))
        }
    }

    func createOffer(iceRestart: Bool) async throws -> Description {
        let pc = try live()
        let cs = constraints(iceRestart)
        return try await withCheckedThrowingContinuation { c in
            pc.offer(for: cs) { d, err in Self.described(d, err, c) }
        }
    }

    func createAnswer() async throws -> Description {
        let pc = try live()
        let cs = constraints(false)
        return try await withCheckedThrowingContinuation { c in
            pc.answer(for: cs) { d, err in Self.described(d, err, c) }
        }
    }

    nonisolated private static func done(_ err: Error?, _ c: CheckedContinuation<Void, Error>) {
        if let err { c.resume(throwing: err) } else { c.resume() }
    }

    func setLocal(description: Description) async throws {
        let pc = try live()
        let d = RTCSessionDescription(type: sdpType(description.kind), sdp: description.sdp)
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            pc.setLocalDescription(d) { err in Self.done(err, c) }
        }
    }

    func setRemote(description: Description) async throws {
        let pc = try live()
        let d = RTCSessionDescription(type: sdpType(description.kind), sdp: description.sdp)
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            pc.setRemoteDescription(d) { err in Self.done(err, c) }
        }
    }

    func addCandidate(candidate: Candidate) async throws {
        let pc = try live()
        // The end of the other peer's candidates: nothing to add.
        if candidate.candidate.isEmpty { return }
        let c = RTCIceCandidate(sdp: candidate.candidate, sdpMLineIndex: Int32(candidate.sdpMLineIndex), sdpMid: candidate.sdpMid.isEmpty ? nil : candidate.sdpMid)
        try await withCheckedThrowingContinuation { (k: CheckedContinuation<Void, Error>) in
            pc.add(c) { err in Self.done(err, k) }
        }
    }

    func createChannel(label: String, options: ChannelOptions) throws -> any DataChannel {
        let pc = try live()
        let c = RTCDataChannelConfiguration()
        c.isOrdered = options.ordered
        if let r = options.maxRetransmits { c.maxRetransmits = Int32(r) }
        if let t = options.maxPacketLifeTime { c.maxPacketLifeTime = Int32((t * 1000).rounded()) }
        c.protocol = options.protocol
        if let id = options.id {
            c.isNegotiated = true
            c.channelId = Int32(id)
        }
        guard let dc = pc.dataChannel(forLabel: label, configuration: c) else {
            throw AsukaNativeError.unsupported("webrtc: couldn't open the data channel")
        }
        let proxy = ChannelDelegate()
        dc.delegate = proxy
        return SwiftDataChannel(dc, proxy)
    }

    func remoteChannel(handle: Int) throws -> any DataChannel {
        guard let ch = channels.removeValue(forKey: handle) else { throw WebRTCError.closed }
        return ch
    }

    func stats() async throws -> Stats {
        let pc = try live()
        let report = await withCheckedContinuation { (c: CheckedContinuation<RTCStatisticsReport, Never>) in
            pc.statistics { c.resume(returning: $0) }
        }
        return statsOf(report)
    }

    func close() throws {
        guard let p = pc else { return }
        pc = nil
        p.close()
        for (_, id) in remotes { RTC.tracks.removeValue(forKey: id) }
        remotes = [:]
        senders = [:]
        for (_, ch) in channels { try? ch.close() }
        channels = [:]
        delegate.owner = nil
        endEvents()
    }
}

private func num(_ v: NSObject?) -> Double { (v as? NSNumber)?.doubleValue ?? 0 }

/// Stats from a report: the selected candidate pair's (the transport's,
/// else the nominated succeeded one), and the media's.
private func statsOf(_ report: RTCStatisticsReport) -> Stats {
    var s = Stats()
    let all = report.statistics.values
    var pairId = all.first { $0.type == "transport" }?.values["selectedCandidatePairId"] as? String
    if pairId == nil {
        pairId = all.first { $0.type == "candidate-pair" && ($0.values["state"] as? String) == "succeeded" && (($0.values["nominated"] as? NSNumber)?.boolValue ?? false) }?.id
    }
    if let id = pairId, let pair = report.statistics[id] {
        let v = pair.values
        s.bytesSent = Int(num(v["bytesSent"]))
        s.bytesReceived = Int(num(v["bytesReceived"]))
        s.roundTripTime = num(v["currentRoundTripTime"])
        s.availableOutgoingBitrate = num(v["availableOutgoingBitrate"])
        if let l = v["localCandidateId"] as? String { s.localCandidate = report.statistics[l]?.values["candidateType"] as? String ?? "" }
        if let r = v["remoteCandidateId"] as? String { s.remoteCandidate = report.statistics[r]?.values["candidateType"] as? String ?? "" }
    }
    for st in all {
        let v = st.values
        switch st.type {
        case "inbound-rtp":
            s.packetsReceived += Int(num(v["packetsReceived"]))
            s.packetsLost += Int(num(v["packetsLost"]))
            if (v["kind"] as? String) == "video" {
                s.frameWidth = max(s.frameWidth, Int(num(v["frameWidth"])))
                s.frameHeight = max(s.frameHeight, Int(num(v["frameHeight"])))
                s.framesPerSecond = max(s.framesPerSecond, num(v["framesPerSecond"]))
            }
        case "outbound-rtp":
            s.packetsSent += Int(num(v["packetsSent"]))
        default:
            break
        }
    }
    return s
}

// MARK: - Data channels

/// What WebRTC tells a data channel, on its own threads: to the main
/// thread, to its wrapper (which is there by then: see didOpen).
final class ChannelDelegate: NSObject, RTCDataChannelDelegate, @unchecked Sendable {
    weak var owner: SwiftDataChannel?

    func dataChannelDidChangeState(_ dc: RTCDataChannel) {
        let s = dc.readyState
        onMain { [weak self] in
            switch s {
            case .open: self?.owner?.emit(ChannelEvent(kind: .open))
            case .closing: self?.owner?.emit(ChannelEvent(kind: .closing))
            case .closed: self?.owner?.emit(ChannelEvent(kind: .closed))
            default: break
            }
        }
    }

    func dataChannel(_ dc: RTCDataChannel, didReceiveMessageWith buffer: RTCDataBuffer) {
        let e = buffer.isBinary
            ? ChannelEvent(kind: .message, binary: true, data: buffer.data)
            : ChannelEvent(kind: .message, text: String(decoding: buffer.data, as: UTF8.self))
        onMain { [weak self] in self?.owner?.emit(e) }
    }
}

@MainActor final class SwiftDataChannel: DataChannel {
    private let dc: RTCDataChannel
    private let proxy: ChannelDelegate
    private var pending: [ChannelEvent] = []
    private var sink: ((ChannelEvent) -> Void)?
    private var waiter: CheckedContinuation<Void, Never>?
    private var ended = false

    init(_ dc: RTCDataChannel, _ proxy: ChannelDelegate) {
        self.dc = dc
        self.proxy = proxy
        proxy.owner = self
    }

    func emit(_ e: ChannelEvent) {
        if ended { return }
        if let s = sink { s(e) } else { pending.append(e) }
        if e.kind == .closed {
            ended = true
            finish()
        }
    }

    private func finish() {
        let w = waiter
        waiter = nil
        w?.resume()
    }

    func events(send: @escaping (ChannelEvent) -> Void) async throws {
        sink = send
        let p = pending
        pending = []
        for e in p { send(e) }
        if ended { return }
        await withTaskCancellationHandler {
            await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
                if ended { c.resume() } else { waiter = c }
            }
        } onCancel: {
            onMain { [weak self] in self?.finish() }
        }
        sink = nil
    }

    func info() throws -> ChannelInfo {
        ChannelInfo(handle: 0, label: dc.label, ordered: dc.isOrdered, protocol: dc.protocol, id: Int(dc.channelId))
    }

    func state() throws -> ChannelState {
        switch dc.readyState {
        case .open: return .open
        case .closing: return .closing
        case .closed: return .closed
        default: return .connecting
        }
    }

    func sendText(text: String) throws {
        guard dc.readyState == .open else { throw WebRTCError.notOpen }
        if !dc.sendData(RTCDataBuffer(data: Data(text.utf8), isBinary: false)) { throw WebRTCError.notOpen }
    }

    func sendBytes(data: Data) throws {
        guard dc.readyState == .open else { throw WebRTCError.notOpen }
        if !dc.sendData(RTCDataBuffer(data: data, isBinary: true)) { throw WebRTCError.notOpen }
    }

    func bufferedAmount() throws -> Int { Int(dc.bufferedAmount) }

    func close() throws {
        dc.close()
        if !ended {
            ended = true
            finish()
        }
    }
}

// MARK: - VideoView

/// Renders frames into a view it holds weakly: a track keeps its
/// renderers, and must not keep the view.
private final class WeakRenderer: NSObject, RTCVideoRenderer {
    weak var target: RTCVideoRenderer?
    init(_ t: RTCVideoRenderer) { target = t }
    func setSize(_ size: CGSize) { target?.setSize(size) }
    func renderFrame(_ frame: RTCVideoFrame?) { target?.renderFrame(frame) }
}

#if canImport(UIKit)
typealias RTCRenderView = RTCMTLVideoView
#else
typealias RTCRenderView = RTCMTLNSVideoView
#endif

/// The box a VideoView shows: the video inside it, fitted (cover crops
/// to the box, contain letterboxes), mirrored when asked.
@MainActor final class VideoBox: AsukaPlatformView, RTCVideoViewDelegate {
    let video = RTCRenderView(frame: .zero)
    var size = CGSize.zero
    var fit: Fit = .cover
    var mirror = false
    var resized: ((CGSize) -> Void)?

    override init(frame: CGRect) {
        super.init(frame: frame)
        video.delegate = self
        #if canImport(UIKit)
        clipsToBounds = true
        backgroundColor = .black
        video.videoContentMode = .scaleAspectFill
        #else
        wantsLayer = true
        layer?.masksToBounds = true
        layer?.backgroundColor = NSColor.black.cgColor
        video.wantsLayer = true
        #endif
        addSubview(video)
    }

    required init?(coder: NSCoder) { fatalError("not from a coder") }

    nonisolated func videoView(_ videoView: RTCVideoRenderer, didChangeVideoSize size: CGSize) {
        onMain { [weak self] in
            guard let self else { return }
            self.size = size
            self.place()
            self.resized?(size)
        }
    }

    /// The video's frame in the box: on iOS the view fits it itself; on a
    /// Mac it's sized to cover or contain the box.
    func place() {
        let b = bounds
        #if canImport(UIKit)
        video.videoContentMode = fit == .contain ? .scaleAspectFit : .scaleAspectFill
        video.transform = .identity
        video.frame = b
        video.transform = mirror ? CGAffineTransform(scaleX: -1, y: 1) : .identity
        #else
        var f = b
        if size.width > 0, size.height > 0, b.width > 0, b.height > 0 {
            let sx = b.width / size.width, sy = b.height / size.height
            let s = fit == .contain ? min(sx, sy) : max(sx, sy)
            let w = size.width * s, h = size.height * s
            f = CGRect(x: (b.width - w) / 2, y: (b.height - h) / 2, width: w, height: h)
        }
        video.frame = f
        if let l = video.layer {
            l.anchorPoint = .zero
            l.transform = mirror ? CATransform3DTranslate(CATransform3DMakeScale(-1, 1, 1), -f.width, 0, 0) : CATransform3DIdentity
        }
        #endif
    }

    #if canImport(UIKit)
    override func layoutSubviews() {
        super.layoutSubviews()
        place()
    }
    #else
    override var isFlipped: Bool { true }
    override func layout() {
        super.layout()
        place()
    }
    #endif
}

@MainActor final class SwiftVideoView: VideoView, AsukaDestroying {
    private let box = VideoBox(frame: .zero)
    private let renderer: WeakRenderer
    private var attached: RTCVideoTrack?
    private let events: VideoViewEvents

    var view: AsukaPlatformView { box }

    init(_ events: VideoViewEvents) {
        self.events = events
        renderer = WeakRenderer(box.video)
        box.resized = { [weak self] s in
            self?.events.resize(size: VideoSize(width: Int(s.width), height: Int(s.height)))
        }
    }

    func setTrack(_ value: Track) {
        let t = RTC.tracks[value.id]?.track as? RTCVideoTrack
        if t === attached { return }
        attached?.remove(renderer)
        attached = t
        t?.add(renderer)
        if t == nil { box.video.renderFrame(nil) }
    }

    func setMirror(_ value: Bool) {
        box.mirror = value
        box.place()
    }

    func setFit(_ value: Fit) {
        box.fit = value
        box.place()
    }

    func destroy() {
        attached?.remove(renderer)
        attached = nil
    }

    deinit {
        // The track's renderer goes with the view (iOS destroys none).
        attached?.remove(renderer)
    }
}

@MainActor func makeVideoView(_ events: VideoViewEvents) -> any VideoView { SwiftVideoView(events) }
