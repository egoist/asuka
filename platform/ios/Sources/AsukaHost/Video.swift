#if canImport(UIKit) && ASUKA_VIDEO // an app that has a Video (see the CLI's iosFeatures)
import UIKit
import AVKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Video: an AVPlayerViewController's view (its controls, full screen,
/// picture in picture, AirPlay, Now Playing), the controller a child of
/// the nearest one while the view is in a window. It reports its playback
/// (VideoState) as it changes, and with TimeInterval every so often while
/// it plays; its subtitle and audio tracks (VideoTracks, the asset's
/// media selection groups) once it's ready and as the choice changes.
@MainActor final class AsukaVideoView: UIView, AVPlayerViewControllerDelegate, AVPictureInPictureControllerDelegate {
    let asukaID: UInt32
    weak var runtime: AsukaRuntime?
    let vc = AVPlayerViewController()
    let player = AVPlayer()
    /// It plays what's loaded, and what comes: the props go in the order
    /// the app wrote them, so Source may be applied before AutoPlay.
    var autoPlay = false {
        didSet { if autoPlay && !oldValue { startWhenSettled() } }
    }
    var loop = false
    /// What the next source loads with: its requests' headers, where it starts.
    var headers: [String: String] = [:]
    var startAt: Double = 0
    /// The app's choice of tracks, by VideoTracks' ids ("": no subtitles;
    /// nil: the platform's), kept until the tracks are known.
    var wantSubtitle: String?
    var wantAudio: String?
    private var starting = false
    private var observations: [NSKeyValueObservation] = []
    private var reported = CGSize.zero
    private var endObserver: NSObjectProtocol?
    private var selectionObserver: NSObjectProtocol?
    private var timeObserver: Any?
    private var interval: Double = 0
    private var last: AsukaVideoState?
    private var lastTracks: [AsukaVideoTrack]?
    private var ended = false, pip = false, full = false
    /// Where a source starts (StartAt), until it's ready and has gone there.
    private var pendingStart: Double = 0
    private var groups: (legible: AVMediaSelectionGroup?, audible: AVMediaSelectionGroup?) = (nil, nil)
    private var poster: UIImageView?
    private var posterSource = ""
    private var fill = false
    /// Picture in picture the app asks for: a layer of the player's own
    /// behind the controller's view (AVPlayerViewController has no call
    /// that starts it), its controller.
    private var pipLayer: AVPlayerLayer?
    private var pipController: AVPictureInPictureController?
    /// The player full screen, as enterFullScreen presents it.
    private weak var fullScreen: AVPlayerViewController?

    init(_ id: UInt32, _ rt: AsukaRuntime) {
        asukaID = id
        runtime = rt
        super.init(frame: .zero)
        vc.player = player
        vc.delegate = self
        vc.view.frame = bounds
        vc.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        vc.view.backgroundColor = .clear
        vc.allowsPictureInPicturePlayback = false // until PictureInPicture
        addSubview(vc.view)
        clipsToBounds = true
        observations.append(player.observe(\.timeControlStatus, options: [.new]) { [weak self] _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.report() } }
        })
        observations.append(player.observe(\.rate, options: [.new]) { [weak self] _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.report() } }
        })
        observations.append(vc.observe(\.isReadyForDisplay, options: [.new]) { [weak self] vc, _ in
            let ready = vc.isReadyForDisplay
            DispatchQueue.main.async { MainActor.assumeIsolated { if ready { self?.poster?.isHidden = true } } }
        })
    }

    required init?(coder: NSCoder) { fatalError() }

    override func layoutSubviews() {
        super.layoutSubviews()
        pipLayer?.frame = bounds
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            var r: UIResponder? = superview
            while let x = r, !(x is UIViewController) { r = x.next }
            if let parent = r as? UIViewController, vc.parent !== parent {
                parent.addChild(vc)
                vc.didMove(toParent: parent)
            }
        } else if vc.parent != nil {
            vc.willMove(toParent: nil)
            vc.removeFromParent()
            if !pip && fullScreen == nil { player.pause() } // gone from the screen (popped, dismissed)
        }
    }

    /// Plays source: an http(s) URL, a file, or one of the app's assets.
    func load(source: String) {
        observations.removeSubrange(3...)
        for o in [endObserver, selectionObserver].compactMap({ $0 }) { NotificationCenter.default.removeObserver(o) }
        endObserver = nil
        selectionObserver = nil
        reported = .zero
        ended = false
        groups = (nil, nil)
        lastTracks = nil
        pendingStart = 0
        poster?.isHidden = false
        guard !source.isEmpty else {
            player.replaceCurrentItem(with: nil)
            report()
            return
        }
        let url: URL? = source.hasPrefix("http://") || source.hasPrefix("https://") || source.hasPrefix("file://") ? URL(string: source)
            : source.hasPrefix("/") ? URL(fileURLWithPath: source)
            : asukaAssets?.appendingPathComponent(source)
        guard let url else { return }
        // Headers for an authenticated stream (AVURLAsset's documented-in-
        // practice AVURLAssetHTTPHeaderFieldsKey; iOS 16's has no other).
        let asset = AVURLAsset(url: url, options: headers.isEmpty ? nil : ["AVURLAssetHTTPHeaderFieldsKey": headers])
        let item = AVPlayerItem(asset: asset)
        pendingStart = startAt
        let id = asukaID
        observations.append(item.observe(\.presentationSize, options: [.new]) { [weak self] item, _ in
            let s = item.presentationSize
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let self, item === self.player.currentItem else { return }
                    if s.width > 0, s != self.reported {
                        self.reported = s
                        self.runtime?.emit(id, .loaded) { AsukaEvents.loaded(id, x: Double(s.width), y: Double(s.height)) }
                    }
                    self.report()
                }
            }
        })
        observations.append(item.observe(\.status, options: [.new]) { [weak self] item, _ in
            let status = item.status
            let m = item.error?.localizedDescription ?? "can't play the video"
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let self, item === self.player.currentItem else { return }
                    if status == .failed {
                        self.runtime?.emit(id, .failed) { AsukaEvents.failed(id, m) }
                    } else if status == .readyToPlay {
                        self.ready(item)
                    }
                    self.report()
                }
            }
        })
        observations.append(item.observe(\.duration, options: [.new]) { [weak self] _, _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.report() } }
        })
        endObserver = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.runtime?.emit(id, .ended) { AsukaEvents.ended(id) }
                if self.loop {
                    self.player.seek(to: .zero)
                    self.player.play()
                } else {
                    self.ended = true
                    self.report()
                }
            }
        }
        selectionObserver = NotificationCenter.default.addObserver(forName: AVPlayerItem.mediaSelectionDidChangeNotification, object: item, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reportTracks() }
        }
        player.replaceCurrentItem(with: item)
        report()
        if autoPlay { startWhenSettled() }
    }

    /// The item is ready: it goes where it starts, and its tracks are
    /// looked for.
    private func ready(_ item: AVPlayerItem) {
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
        guard let asset = item.asset as? AVURLAsset else { return }
        Task { @MainActor [weak self] in
            let legible = try? await asset.loadMediaSelectionGroup(for: .legible)
            let audible = try? await asset.loadMediaSelectionGroup(for: .audible)
            guard let self, item === self.player.currentItem else { return }
            self.groups = (legible, audible)
            self.applyTracks()
            self.reportTracks()
        }
    }

    /// Plays once the props that came with autoPlay are applied: Muted may
    /// come after it, and a muted video takes no audio session.
    private func startWhenSettled() {
        guard player.currentItem != nil, !starting else { return }
        starting = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.starting = false
            if self.autoPlay && self.pendingStart == 0 { self.play() }
        }
    }

    func seek(_ seconds: Double) {
        ended = false
        player.seek(to: CMTime(seconds: seconds, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero) { [weak self] _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.report(force: true) } }
        }
    }

    func play() {
        if !player.isMuted || vc.allowsPictureInPicturePlayback {
            // Sound as a video player's: heard with the silent switch on,
            // and picture in picture's (which needs it).
            try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .moviePlayback)
        }
        if ended {
            ended = false
            player.seek(to: .zero)
        }
        player.play()
    }

    func pause() { player.pause() }

    // ---- The props past the source ----

    func setRate(_ r: Double) {
        let rate = Float(r > 0 ? r : 1)
        player.defaultRate = rate
        if player.rate != 0 { player.rate = rate }
        report()
    }

    func setVolume(_ v: Double) { player.volume = Float(max(0, min(1, v))) }

    func setHeaders(_ b: UnsafeRawBufferPointer) {
        let s = AsukaInteraction.decodeStrings(b)
        var h: [String: String] = [:]
        var i = 0
        while i + 1 < s.count {
            h[s[i]] = s[i + 1]
            i += 2
        }
        headers = h
    }

    func setPoster(_ source: String) {
        posterSource = source
        guard !source.isEmpty else { poster?.removeFromSuperview(); poster = nil; return }
        let iv = poster ?? UIImageView()
        if poster == nil, let overlay = vc.contentOverlayView {
            iv.frame = overlay.bounds
            iv.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            iv.clipsToBounds = true
            overlay.addSubview(iv)
            poster = iv
        }
        iv.contentMode = fill ? .scaleAspectFill : .scaleAspectFit
        iv.isHidden = vc.isReadyForDisplay
        AsukaImageLoader.shared.load(source) { [weak self] r in
            guard let self, self.posterSource == source, case .success(let img) = r else { return }
            iv.image = img
        }
    }

    func setMode(_ fill: Bool) {
        self.fill = fill
        vc.videoGravity = fill ? .resizeAspectFill : .resizeAspect
        pipLayer?.videoGravity = vc.videoGravity
        poster?.contentMode = fill ? .scaleAspectFill : .scaleAspectFit
    }

    /// TimeInterval: while it plays, the time every interval seconds.
    func setInterval(_ s: Double) {
        if let o = timeObserver { player.removeTimeObserver(o) }
        timeObserver = nil
        interval = s
        guard s > 0 else { return }
        timeObserver = player.addPeriodicTimeObserver(forInterval: CMTime(seconds: s, preferredTimescale: 600), queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.player.timeControlStatus == .playing else { return }
                self.report(force: true)
            }
        }
    }

    func setPictureInPicture(_ on: Bool) {
        vc.allowsPictureInPicturePlayback = on
        if on { _ = pictureInPicture() }
    }

    /// The controller of the picture in picture the app asks for, made as
    /// it's first allowed: with the playback audio session it needs.
    private func pictureInPicture() -> AVPictureInPictureController? {
        if let p = pipController { return p }
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .moviePlayback)
        guard AVPictureInPictureController.isPictureInPictureSupported() else { return nil }
        let l = AVPlayerLayer(player: player)
        l.frame = bounds
        l.videoGravity = vc.videoGravity
        layer.insertSublayer(l, at: 0) // behind the controller's view
        pipLayer = l
        let p = AVPictureInPictureController(playerLayer: l)
        p?.delegate = self
        pipController = p
        return p
    }

    func setAutoPictureInPicture(_ on: Bool) {
        #if !os(tvOS)
        vc.canStartPictureInPictureAutomaticallyFromInline = on
        #endif
    }

    /// VideoAction: picture in picture, full screen, in or out.
    func act(_ a: AsukaVideoAction?) {
        switch a {
        case .enterPictureInPicture:
            guard vc.allowsPictureInPicturePlayback, let p = pictureInPicture(), !p.isPictureInPictureActive else { return }
            try? AVAudioSession.sharedInstance().setActive(true)
            p.startPictureInPicture()
        case .exitPictureInPicture:
            pipController?.stopPictureInPicture()
        case .enterFullScreen:
            guard fullScreen == nil, let from = vc.parent ?? window?.rootViewController else { return }
            var top = from
            while let p = top.presentedViewController { top = p }
            let f = AVPlayerViewController()
            f.player = player
            f.delegate = self
            f.allowsPictureInPicturePlayback = vc.allowsPictureInPicturePlayback
            f.modalPresentationStyle = .fullScreen
            fullScreen = f
            full = true
            report()
            top.present(f, animated: true)
        case .exitFullScreen:
            guard let f = fullScreen else { return }
            f.dismiss(animated: true) { [weak self] in self?.leftFullScreen() }
        default:
            break
        }
    }

    private func leftFullScreen() {
        fullScreen = nil
        full = false
        report()
    }

    // ---- Tracks ----

    func setSubtitle(_ id: String) {
        wantSubtitle = id
        applyTracks()
    }

    func setAudioTrack(_ id: String) {
        wantAudio = id
        applyTracks()
    }

    /// Selects the app's choice of tracks, once they're known.
    private func applyTracks() {
        guard let item = player.currentItem else { return }
        if wantSubtitle != nil || wantAudio != nil { player.appliesMediaSelectionCriteriaAutomatically = false }
        if let want = wantSubtitle, let g = groups.legible {
            if want.isEmpty {
                if g.allowsEmptySelection { item.select(nil, in: g) }
            } else if let i = Int(want), g.options.indices.contains(i) {
                item.select(g.options[i], in: g)
            }
        }
        if let want = wantAudio, let g = groups.audible, let i = Int(want), g.options.indices.contains(i) {
            item.select(g.options[i], in: g)
        }
    }

    /// VideoTracks: each option of the legible and audible groups, by its
    /// index in its group.
    func reportTracks() {
        guard let item = player.currentItem, let rt = runtime else { return }
        var ts: [AsukaVideoTrack] = []
        for (g, subtitle) in [(groups.legible, true), (groups.audible, false)] {
            guard let g else { continue }
            let chosen = item.currentMediaSelection.selectedMediaOption(in: g)
            for (i, o) in g.options.enumerated() {
                ts.append(AsukaVideoTrack(subtitle: subtitle, selected: o == chosen, id: String(i), label: o.displayName,
                                          language: o.extendedLanguageTag ?? o.locale?.identifier ?? ""))
            }
        }
        guard ts != lastTracks else { return }
        lastTracks = ts
        let id = asukaID
        rt.emit(id, .videoTracks) { AsukaEvents.videoTracks(id, AsukaVideoTrack.bytes(ts)) }
    }

    // ---- Reports ----

    private func state() -> AsukaVideoState {
        var s = AsukaVideoState(rate: player.defaultRate > 0 ? player.defaultRate : 1)
        guard let item = player.currentItem else { return s }
        if item.status == .readyToPlay { s.flags.insert(.ready) }
        switch player.timeControlStatus {
        case .playing: s.flags.insert(.playing)
        case .waitingToPlayAtSpecifiedRate: s.flags.formUnion([.playing, .buffering])
        default: break
        }
        if ended { s.flags.insert(.ended) }
        if pip { s.flags.insert(.pictureInPicture) }
        if full { s.flags.insert(.fullScreen) }
        if item.status == .readyToPlay {
            let d = item.duration
            s.duration = d.isIndefinite ? .infinity : d.isNumeric ? d.seconds : .nan
        }
        let t = player.currentTime().seconds
        s.time = t.isFinite ? t : 0
        s.width = Float(item.presentationSize.width)
        s.height = Float(item.presentationSize.height)
        return s
    }

    /// Sends VideoState if any of it but the time changed (force: the
    /// time too, a tick's or a seek's).
    func report(force: Bool = false) {
        guard let rt = runtime else { return }
        let s = state()
        if !force, let l = last, l.sameBut(time: s) { return }
        last = s
        let id = asukaID
        rt.emit(id, .videoState) { AsukaEvents.videoState(id, s.bytes) }
    }

    // ---- Picture in picture and full screen ----

    private func setPiP(_ on: Bool) {
        pip = on
        report()
    }

    nonisolated func playerViewControllerDidStartPictureInPicture(_ c: AVPlayerViewController) {
        MainActor.assumeIsolated { setPiP(true) }
    }

    nonisolated func playerViewControllerDidStopPictureInPicture(_ c: AVPlayerViewController) {
        MainActor.assumeIsolated { setPiP(false) }
    }

    /// It plays inline: picture in picture doesn't dismiss anything.
    nonisolated func playerViewControllerShouldAutomaticallyDismissAtPictureInPictureStart(_ c: AVPlayerViewController) -> Bool { false }

    nonisolated func playerViewController(_ c: AVPlayerViewController,
                                          restoreUserInterfaceForPictureInPictureStopWithCompletionHandler done: @escaping (Bool) -> Void) {
        done(true)
    }

    nonisolated func pictureInPictureControllerDidStartPictureInPicture(_ c: AVPictureInPictureController) {
        MainActor.assumeIsolated { setPiP(true) }
    }

    nonisolated func pictureInPictureControllerDidStopPictureInPicture(_ c: AVPictureInPictureController) {
        MainActor.assumeIsolated { setPiP(false) }
    }

    nonisolated func pictureInPictureController(_ c: AVPictureInPictureController,
                                                restoreUserInterfaceForPictureInPictureStopWithCompletionHandler done: @escaping (Bool) -> Void) {
        done(true)
    }

    #if !os(tvOS)
    nonisolated func playerViewController(_ c: AVPlayerViewController, willBeginFullScreenPresentationWithAnimationCoordinator co: UIViewControllerTransitionCoordinator) {
        MainActor.assumeIsolated {
            full = true
            report()
        }
    }

    nonisolated func playerViewController(_ c: AVPlayerViewController, willEndFullScreenPresentationWithAnimationCoordinator co: UIViewControllerTransitionCoordinator) {
        MainActor.assumeIsolated {
            co.animate(alongsideTransition: nil) { [weak self] ctx in
                MainActor.assumeIsolated {
                    guard let self, !ctx.isCancelled else { return }
                    if c === self.fullScreen { self.fullScreen = nil }
                    self.full = false
                    self.report()
                }
            }
        }
    }
    #endif
}
#endif
