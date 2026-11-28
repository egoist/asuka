// Video: an AVPlayerView (AVKit's controls, floating) playing an AVPlayer.
// Loaded reports the video's natural size once AVFoundation knows it, Ended
// each play to its end (looping, it plays again), Failed what couldn't play.
// VideoState reports its playback as it changes (and with TimeInterval
// every so often while it plays), VideoTracks its subtitle and audio
// tracks (the asset's media selection groups). Picture in picture is
// AVPlayerView's own button, and for the app's VideoAction a player layer
// of its own behind the view, with its AVPictureInPictureController; full
// screen is the view's full-screen mode.
import AppKit
import AVFoundation
import AVKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Video node's: its player, its item and what it was asked.
@MainActor final class AsukaVideo: NSObject, AVPictureInPictureControllerDelegate, AVPlayerViewPictureInPictureDelegate {
    let view: AVPlayerView
    let player = AVPlayer()
    var item: AVPlayerItem?
    var autoPlay = false
    var loop = false
    var muted = false
    /// Loaded was reported for the item.
    var sized = false
    weak var node: AsukaNode?
    private var sizeObservation: NSKeyValueObservation?
    private var itemObservations: [NSKeyValueObservation] = []
    private var playerObservations: [NSKeyValueObservation] = []
    /// Times it played to its end (the automation's).
    var ends = 0
    /// What the next source loads with.
    var headers: [String: String] = [:]
    var startAt: Double = 0
    private var pendingStart: Double = 0
    /// The rate chosen (AVPlayer's defaultRate before macOS 13).
    var rate: Float = 1
    var wantSubtitle: String?
    var wantAudio: String?
    private var groups: (legible: AVMediaSelectionGroup?, audible: AVMediaSelectionGroup?) = (nil, nil)
    private var timeObserver: Any?
    private var last: AsukaVideoState?
    private var lastTracks: [AsukaVideoTrack]?
    var ended = false, pip = false
    private var poster: CALayer?
    private var posterSource = ""
    private var pipLayer: AVPlayerLayer?
    private var pipController: AVPictureInPictureController?
    private var selectionObserver: NSObjectProtocol?

    init(_ n: AsukaNode) {
        view = AVPlayerView(frame: .zero)
        node = n
        super.init()
        view.controlsStyle = .floating
        view.player = player
        view.wantsLayer = true
        if #available(macOS 10.15, *) { view.pictureInPictureDelegate = self }
        playerObservations.append(player.observe(\.timeControlStatus, options: [.new]) { [weak self] _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.report() } }
        })
        playerObservations.append(player.observe(\.rate, options: [.new]) { [weak self] p, _ in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let self else { return }
                    if #available(macOS 13, *) {} else if p.rate > 0 { self.rate = p.rate }
                    self.report()
                }
            }
        })
        playerObservations.append(view.observe(\.isReadyForDisplay, options: [.new]) { [weak self] v, _ in
            let ready = v.isReadyForDisplay
            DispatchQueue.main.async { MainActor.assumeIsolated { if ready { self?.poster?.isHidden = true } } }
        })
    }

    func setProp(_ rt: AsukaRuntime, _ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        switch p {
        case AsukaProp.Video.source:
            setSource(rt, n, v.string)
        case AsukaProp.Video.mode:
            let fill = AsukaImageMode(rawValue: v.enumValue) == .fill
            view.videoGravity = fill ? .resizeAspectFill : .resizeAspect
            pipLayer?.videoGravity = view.videoGravity
            poster?.contentsGravity = fill ? .resizeAspectFill : .resizeAspect
        case AsukaProp.Video.hideControls:
            view.controlsStyle = v.bool ? .none : .floating
        case AsukaProp.Video.autoPlay:
            autoPlay = v.bool
            if autoPlay && item != nil && pendingStart == 0 { play() }
        case AsukaProp.Video.loop:
            loop = v.bool
        case AsukaProp.Video.muted:
            muted = v.bool
            player.isMuted = muted
        case AsukaProp.Video.rate:
            setRate(Float(v.float > 0 ? v.float : 1))
        case AsukaProp.Video.volume:
            player.volume = Float(max(0, min(1, v.float)))
        case AsukaProp.Video.headers:
            guard case .bytes(let b) = v else { return }
            let s = asukaStrings(b)
            headers = [:]
            var i = 0
            while i + 1 < s.count {
                headers[s[i]] = s[i + 1]
                i += 2
            }
        case AsukaProp.Video.poster:
            setPoster(rt, v.string)
        case AsukaProp.Video.startAt:
            startAt = v.float
        case AsukaProp.Video.timeInterval:
            setInterval(v.float)
        case AsukaProp.Video.subtitle:
            wantSubtitle = v.string
            applyTracks()
        case AsukaProp.Video.audioTrack:
            wantAudio = v.string
            applyTracks()
        case AsukaProp.Video.pictureInPicture:
            if #available(macOS 10.15, *) { view.allowsPictureInPicturePlayback = v.bool }
        default:
            break // SubtitleFiles: AVFoundation adds no WebVTT file; AutoPictureInPicture: iOS's and Android's
        }
    }

    func setSource(_ rt: AsukaRuntime, _ n: AsukaNode, _ s: String) {
        forgetItem()
        ended = false
        groups = (nil, nil)
        lastTracks = nil
        poster?.isHidden = false
        guard !s.isEmpty else {
            player.replaceCurrentItem(with: nil)
            report()
            return
        }
        let url: URL?
        if s.contains("://") {
            url = URL(string: s)
        } else if s.hasPrefix("/") {
            url = URL(fileURLWithPath: s)
        } else {
            url = rt.assets.appendingPathComponent(s).standardizedFileURL
        }
        guard let url else { return }
        let asset = AVURLAsset(url: url, options: headers.isEmpty ? nil : ["AVURLAssetHTTPHeaderFieldsKey": headers])
        let it = AVPlayerItem(asset: asset)
        item = it
        sized = false
        pendingStart = startAt
        let c = NotificationCenter.default
        c.addObserver(self, selector: #selector(ended(_:)), name: .AVPlayerItemDidPlayToEndTime, object: it)
        c.addObserver(self, selector: #selector(failed(_:)), name: .AVPlayerItemFailedToPlayToEndTime, object: it)
        selectionObserver = c.addObserver(forName: AVPlayerItem.mediaSelectionDidChangeNotification, object: it, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reportTracks() }
        }
        // KVO's change may come on another thread.
        sizeObservation = it.observe(\.presentationSize, options: [.initial, .new]) { [weak self] _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.reportSize(); self?.report() } }
        }
        // The item failing to load at all, or ready: its status.
        statusObservation = it.observe(\.status, options: [.new]) { [weak self] item, _ in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let self, item === self.item else { return }
                    if item.status == .failed { self.reportFailure(item.error) }
                    if item.status == .readyToPlay { self.ready(item) }
                    self.report()
                }
            }
        }
        itemObservations = [it.observe(\.duration, options: [.new]) { [weak self] _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.report() } }
        }]
        player.replaceCurrentItem(with: it)
        player.isMuted = muted
        report()
        if autoPlay && pendingStart == 0 { play() }
    }

    private var statusObservation: NSKeyValueObservation?

    func forgetItem() {
        guard let it = item else { return }
        let c = NotificationCenter.default
        c.removeObserver(self, name: .AVPlayerItemDidPlayToEndTime, object: it)
        c.removeObserver(self, name: .AVPlayerItemFailedToPlayToEndTime, object: it)
        if let o = selectionObserver { c.removeObserver(o) }
        selectionObserver = nil
        sizeObservation?.invalidate()
        statusObservation?.invalidate()
        sizeObservation = nil
        statusObservation = nil
        itemObservations = []
        item = nil
    }

    /// The item is ready: it goes where it starts, then plays if it
    /// autoplays; its tracks are looked for.
    private func ready(_ it: AVPlayerItem) {
        if pendingStart > 0 {
            let t = pendingStart
            pendingStart = 0
            player.seek(to: CMTime(seconds: t, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero) { [weak self] _ in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        guard let self else { return }
                        self.report(force: true)
                        if self.autoPlay { self.play() }
                    }
                }
            }
        }
        guard let asset = it.asset as? AVURLAsset else { return }
        if #available(macOS 12, *) {
            Task { @MainActor [weak self] in
                let legible = try? await asset.loadMediaSelectionGroup(for: .legible)
                let audible = try? await asset.loadMediaSelectionGroup(for: .audible)
                guard let self, it === self.item else { return }
                self.groups = (legible, audible)
                self.applyTracks()
                self.reportTracks()
            }
        } else {
            groups = (asset.mediaSelectionGroup(forMediaCharacteristic: .legible), asset.mediaSelectionGroup(forMediaCharacteristic: .audible))
            applyTracks()
            reportTracks()
        }
    }

    func play() {
        if ended {
            ended = false
            player.seek(to: .zero)
        }
        if #available(macOS 13, *) { player.play() } else { player.rate = rate }
    }

    func pause() { player.pause() }

    func seek(_ sec: Double) {
        ended = false
        player.seek(to: CMTime(seconds: sec, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero) { [weak self] _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.report(force: true) } }
        }
    }

    func setRate(_ r: Float) {
        rate = r
        if #available(macOS 13, *) { player.defaultRate = r }
        if player.rate != 0 { player.rate = r }
        report()
    }

    func setInterval(_ s: Double) {
        if let o = timeObserver { player.removeTimeObserver(o) }
        timeObserver = nil
        guard s > 0 else { return }
        timeObserver = player.addPeriodicTimeObserver(forInterval: CMTime(seconds: s, preferredTimescale: 600), queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.player.timeControlStatus == .playing else { return }
                self.report(force: true)
            }
        }
    }

    func setPoster(_ rt: AsukaRuntime, _ src: String) {
        posterSource = src
        guard !src.isEmpty else { poster?.removeFromSuperlayer(); poster = nil; return }
        let l = poster ?? CALayer()
        if poster == nil, let overlay = view.contentOverlayView {
            overlay.wantsLayer = true
            l.frame = overlay.bounds
            l.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
            l.contentsGravity = view.videoGravity == .resizeAspectFill ? .resizeAspectFill : .resizeAspect
            l.masksToBounds = true
            overlay.layer?.addSublayer(l)
            poster = l
        }
        l.isHidden = view.isReadyForDisplay
        AsukaImageLoader.load(src, assets: rt.assets) { [weak self] r in
            MainActor.assumeIsolated {
                guard let self, self.posterSource == src, case .success(let c) = r else { return }
                self.poster?.contents = c.image.cgImage(forProposedRect: nil, context: nil, hints: nil)
            }
        }
    }

    // ---- Tracks ----

    private func applyTracks() {
        guard let it = item else { return }
        if wantSubtitle != nil || wantAudio != nil { player.appliesMediaSelectionCriteriaAutomatically = false }
        if let want = wantSubtitle, let g = groups.legible {
            if want.isEmpty {
                if g.allowsEmptySelection { it.select(nil, in: g) }
            } else if let i = Int(want), g.options.indices.contains(i) {
                it.select(g.options[i], in: g)
            }
        }
        if let want = wantAudio, let g = groups.audible, let i = Int(want), g.options.indices.contains(i) {
            it.select(g.options[i], in: g)
        }
    }

    func reportTracks() {
        guard let it = item, let n = node else { return }
        var ts: [AsukaVideoTrack] = []
        for (g, subtitle) in [(groups.legible, true), (groups.audible, false)] {
            guard let g else { continue }
            let chosen = it.currentMediaSelection.selectedMediaOption(in: g)
            for (i, o) in g.options.enumerated() {
                ts.append(AsukaVideoTrack(subtitle: subtitle, selected: o == chosen, id: String(i), label: o.displayName,
                                          language: o.extendedLanguageTag ?? o.locale?.identifier ?? ""))
            }
        }
        guard ts != lastTracks else { return }
        lastTracks = ts
        AsukaRuntime.shared.emit(n, .videoTracks) { AsukaEvents.videoTracks($0, AsukaVideoTrack.bytes(ts)) }
    }

    // ---- Reports ----

    func state() -> AsukaVideoState {
        var r = rate
        if #available(macOS 13, *) { r = player.defaultRate > 0 ? player.defaultRate : 1 }
        var s = AsukaVideoState(rate: r)
        guard let it = item else { return s }
        if it.status == .readyToPlay { s.flags.insert(.ready) }
        switch player.timeControlStatus {
        case .playing: s.flags.insert(.playing)
        case .waitingToPlayAtSpecifiedRate: s.flags.formUnion([.playing, .buffering])
        default: break
        }
        if ended { s.flags.insert(.ended) }
        if pip { s.flags.insert(.pictureInPicture) }
        if view.isInFullScreenMode || view.window?.styleMask.contains(.fullScreen) == true && view.window?.contentView === view {
            s.flags.insert(.fullScreen)
        }
        if it.status == .readyToPlay {
            let d = it.duration
            s.duration = d.isIndefinite ? .infinity : d.isNumeric ? d.seconds : .nan
        }
        let t = player.currentTime().seconds
        s.time = t.isFinite ? t : 0
        s.width = Float(it.presentationSize.width)
        s.height = Float(it.presentationSize.height)
        return s
    }

    func report(force: Bool = false) {
        guard let n = node else { return }
        let s = state()
        if !force, let l = last, l.sameBut(time: s) { return }
        last = s
        AsukaRuntime.shared.emit(n, .videoState) { AsukaEvents.videoState($0, s.bytes) }
    }

    // ---- Picture in picture, full screen ----

    func act(_ a: AsukaVideoAction?) {
        switch a {
        case .enterPictureInPicture:
            guard #available(macOS 10.15, *), view.allowsPictureInPicturePlayback, let p = pictureInPicture(), !p.isPictureInPictureActive else { return }
            p.startPictureInPicture()
        case .exitPictureInPicture:
            pipController?.stopPictureInPicture()
        case .enterFullScreen:
            guard !view.isInFullScreenMode else { return }
            view.enterFullScreenMode(view.window?.screen ?? NSScreen.main ?? NSScreen.screens[0], withOptions: [.fullScreenModeAllScreens: false])
            report()
        case .exitFullScreen:
            guard view.isInFullScreenMode else { return }
            view.exitFullScreenMode(options: nil)
            report()
        default:
            break
        }
    }

    @available(macOS 10.15, *)
    private func pictureInPicture() -> AVPictureInPictureController? {
        if let p = pipController { return p }
        guard AVPictureInPictureController.isPictureInPictureSupported(), let host = view.layer else { return nil }
        let l = AVPlayerLayer(player: player)
        l.frame = host.bounds
        l.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        l.videoGravity = view.videoGravity
        host.insertSublayer(l, at: 0) // behind the view's own
        pipLayer = l
        let p = AVPictureInPictureController(playerLayer: l)
        p?.delegate = self
        pipController = p
        return p
    }

    private func setPiP(_ on: Bool) {
        pip = on
        report()
    }

    nonisolated func pictureInPictureControllerDidStartPictureInPicture(_ c: AVPictureInPictureController) {
        MainActor.assumeIsolated { setPiP(true) }
    }

    nonisolated func pictureInPictureControllerDidStopPictureInPicture(_ c: AVPictureInPictureController) {
        MainActor.assumeIsolated { setPiP(false) }
    }

    nonisolated func playerViewDidStartPicture(inPicture v: AVPlayerView) {
        MainActor.assumeIsolated { setPiP(true) }
    }

    nonisolated func playerViewDidStopPicture(inPicture v: AVPlayerView) {
        MainActor.assumeIsolated { setPiP(false) }
    }

    nonisolated func playerView(_ v: AVPlayerView, restoreUserInterfaceForPictureInPictureStopWithCompletionHandler done: @escaping (Bool) -> Void) {
        done(true)
    }

    func close() {
        player.pause()
        pipController?.stopPictureInPicture()
        if view.isInFullScreenMode { view.exitFullScreenMode(options: nil) }
        if let o = timeObserver { player.removeTimeObserver(o) }
        timeObserver = nil
        forgetItem()
        view.player = nil
        pipLayer?.player = nil
    }

    /// Reports the video's natural size once AVFoundation knows it.
    func reportSize() {
        guard let it = item, !sized, let n = node else { return }
        let s = it.presentationSize
        guard s.width > 0, s.height > 0 else { return }
        sized = true
        AsukaRuntime.shared.emit(n, .loaded) { AsukaEvents.loaded($0, x: Double(s.width), y: Double(s.height)) }
    }

    @objc func ended(_ note: Notification) {
        guard let n = node, (note.object as AnyObject?) === item else { return }
        ends += 1
        AsukaRuntime.shared.emit(n, .ended) { AsukaEvents.ended($0) }
        if loop {
            seek(0)
            play()
        } else {
            ended = true
            report()
        }
    }

    @objc func failed(_ note: Notification) {
        guard (note.object as AnyObject?) === item else { return }
        reportFailure((note.userInfo?[AVPlayerItemFailedToPlayToEndTimeErrorKey] as? Error) ?? item?.error)
    }

    func reportFailure(_ e: Error?) {
        guard let n = node else { return }
        let msg = e.map { asukaErrorText($0) } ?? "can't play"
        AsukaRuntime.shared.emit(n, .failed) { AsukaEvents.failed($0, msg) }
    }
}

/// A list of strings as the core encodes one: u32 count, then each
/// u32-length (what's cut short is left out).
func asukaStrings(_ b: UnsafeRawBufferPointer) -> [String] {
    var r = AsukaReader(b)
    guard r.end - r.p >= 4 else { return [] }
    let n = r.u32()
    var out: [String] = []
    for _ in 0..<n {
        guard r.end - r.p >= 4 else { break }
        var peek = r
        let len = Int(peek.u32())
        guard r.end - r.p - 4 >= len else { break }
        out.append(r.string())
    }
    return out
}

@MainActor extension AsukaRuntime {
    func makeVideo(_ n: AsukaNode) -> NSView {
        let v = AsukaVideo(n)
        n.video = v
        return v.view
    }

    func setVideoProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        n.video?.setProp(self, n, p, v)
    }

    /// Play, Pause, Seek (f64 seconds) and VideoAction (u8).
    func videoCommand(_ n: AsukaNode, _ c: AsukaCommand, _ args: [UInt8]) {
        guard let v = n.video else { return }
        switch c {
        case .play: v.play()
        case .pause: v.pause()
        case .seek:
            if args.count >= 8 { v.seek(Double(bitPattern: args.u64(0))) }
        case .videoAction:
            if let a = args.first { v.act(AsukaVideoAction(rawValue: Int32(a))) }
        default: break
        }
    }

    func videoJSON(_ v: AsukaVideo, _ d: inout [String: Any]) {
        let s = v.state()
        var j: [String: Any] = ["autoPlay": v.autoPlay, "loop": v.loop, "muted": v.player.isMuted, "sized": v.sized,
                                "rate": Double(v.player.rate), "controls": v.view.controlsStyle.rawValue,
                                "gravity": v.view.videoGravity.rawValue, "ends": v.ends,
                                "time": CMTimeGetSeconds(v.player.currentTime()),
                                "flags": Int(s.flags.rawValue), "chosenRate": Double(s.rate), "volume": Double(v.player.volume)]
        if s.duration.isFinite { j["duration"] = s.duration }
        if let it = v.item {
            j["presentationSize"] = [Double(it.presentationSize.width), Double(it.presentationSize.height)]
            j["status"] = it.status.rawValue
        }
        d["video"] = j
    }
}
