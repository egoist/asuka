// std/lottie on iOS and macOS: lottie-ios' LottieAnimationView, driven
// as the View's props say. A timer at the display's rate reports the
// progress while it plays (a change of at least 1/200 of the animation),
// which a progress set back moves nothing; reaching the end, not looping,
// reports playing false and complete. Overrides' colors are their light
// or dark one, as the view's appearance is.
import Asuka
import Foundation
import Lottie
import QuartzCore
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

@MainActor final class LottiePlayer: Player, AsukaDestroying {
    let animationView: LottieAnimationView
    let events: PlayerEvents
    var view: AsukaPlatformView { animationView }

    private var source = ""
    private var data = Data()
    private var animationID = ""
    private var loop = false
    private var playing = false
    private var speed: Double = 1
    private var segment = Segment()
    private var marker = ""
    private var pendingProgress = -1.0
    private var overrides: [Override] = []
    private var dark = false
    private var applied: [AnimationKeypath] = []
    private var loadToken = 0
    private var done = false
    private var ticker: Timer?
    private var tween: Tween?
    private var lastReported = -1.0

    struct Tween {
        var from, to: Double
        var start: CFTimeInterval
        var seconds: Double
        var curve: Int
        var damping: Double
    }

    init(_ events: PlayerEvents) {
        self.events = events
        // Reduce Motion is the Go side's to follow (std/lottie).
        animationView = LottieAnimationView(animation: nil, configuration: LottieConfiguration(reducedMotionOption: .standardMotion))
        animationView.backgroundBehavior = .pauseAndRestore
        #if canImport(UIKit)
        animationView.contentMode = .scaleAspectFit
        animationView.clipsToBounds = true
        #else
        animationView.contentMode = .scaleAspectFit
        #endif
        followAppearance()
    }

    /// Re-applies the overrides as the appearance goes light or dark.
    private func followAppearance() {
        #if canImport(UIKit)
        dark = animationView.traitCollection.userInterfaceStyle == .dark
        if #available(iOS 17.0, *) {
            animationView.registerForTraitChanges([UITraitUserInterfaceStyle.self]) { [weak self] (v: LottieAnimationView, _: UITraitCollection) in
                self?.appearance(v.traitCollection.userInterfaceStyle == .dark)
            }
        }
        #else
        dark = NSApp.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
        appearanceObserver = NSApp.observe(\.effectiveAppearance) { [weak self] app, _ in
            let d = app.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.appearance(d) } }
        }
        #endif
    }

    #if !canImport(UIKit)
    private var appearanceObserver: NSKeyValueObservation?
    #endif

    private func appearance(_ d: Bool) {
        guard d != dark else { return }
        dark = d
        if animationView.animation != nil { applyOverrides() }
    }

    func destroy() {
        loadToken += 1
        ticker?.invalidate()
        ticker = nil
        animationView.stop()
    }

    // MARK: Loading

    func setSource(_ value: String) {
        source = value
        if !value.isEmpty { load() }
    }

    func setData(_ value: Data) {
        data = value
        if !value.isEmpty, source.isEmpty { load() }
    }

    func setAnimation(_ value: String) { animationID = value }

    private func load() {
        loadToken += 1
        let token = loadToken
        let src = source
        let bytes = data
        let finish: (Result<Data, Error>) -> Void = { r in
            DispatchQueue.main.async { [weak self] in self?.read(token, r) }
        }
        if !src.isEmpty, src.hasPrefix("http://") || src.hasPrefix("https://"), let url = URL(string: src) {
            URLSession.shared.dataTask(with: url) { d, resp, err in
                if let err { finish(.failure(err)); return }
                if let h = resp as? HTTPURLResponse, !(200..<300).contains(h.statusCode) {
                    finish(.failure(LottieError.message("\(src): HTTP \(h.statusCode)")))
                    return
                }
                finish(.success(d ?? Data()))
            }.resume()
            return
        }
        DispatchQueue.global(qos: .userInitiated).async {
            if src.isEmpty { finish(.success(bytes)); return }
            guard let url = AsukaAssets.url(src), let d = try? Data(contentsOf: url) else {
                finish(.failure(LottieError.message("no animation at \(src)")))
                return
            }
            finish(.success(d))
        }
    }

    private func read(_ token: Int, _ r: Result<Data, Error>) {
        guard token == loadToken else { return }
        let d: Data
        switch r {
        case .failure(let e): events.error(message: e.localizedDescription); return
        case .success(let v): d = v
        }
        if d.starts(with: [0x50, 0x4B, 0x03, 0x04]) { // a dotLottie (a zip)
            DotLottieFile.loadedFrom(data: d, filename: "animation") { [weak self] result in
                guard let self, token == self.loadToken else { return }
                switch result {
                case .success(let file):
                    self.animationView.loadAnimation(self.animationID.isEmpty ? nil : self.animationID, from: file)
                    if self.animationView.animation == nil {
                        self.events.error(message: "the dotLottie has no animation \"\(self.animationID)\"")
                        return
                    }
                    self.didLoad()
                case .failure(let e):
                    self.events.error(message: "\(e)")
                }
            }
            return
        }
        do {
            animationView.animation = try LottieAnimation.from(data: d)
            didLoad()
        } catch {
            events.error(message: "not a Lottie animation: \(error)")
        }
    }

    private func didLoad() {
        guard let a = animationView.animation else { return }
        animationView.animationSpeed = abs(speed)
        applyOverrides()
        var markers: [Marker] = []
        for n in a.markerNames {
            if let s = a.frameTime(forMarker: n) {
                markers.append(Marker(name: n, start: Double(s), duration: Double(a.durationFrameTime(forMarker: n) ?? 0)))
            }
        }
        events.loaded(info: Info(width: Double(a.size.width), height: Double(a.size.height),
                                 start: Double(a.startFrame), end: Double(a.endFrame), frameRate: a.framerate, markers: markers))
        done = false
        animationView.currentFrame = span().from
        if pendingProgress >= 0 {
            let p = pendingProgress
            pendingProgress = -1
            setProgress(p)
        }
        if playing { play() } else { reportNow() }
        tick()
    }

    // MARK: Playing

    /// The range it plays: a marker's, the segment, or all of it.
    private func span() -> (from: Double, to: Double) {
        guard let a = animationView.animation else { return (0, 0) }
        var from = Double(a.startFrame), to = Double(a.endFrame)
        if !marker.isEmpty, let s = a.frameTime(forMarker: marker) {
            return (Double(s), Double(s) + Double(a.durationFrameTime(forMarker: marker) ?? 0))
        }
        if segment.to > segment.from {
            from = max(segment.from, from)
            to = min(segment.to, to)
        }
        return (from, to)
    }

    private func play() {
        guard animationView.animation != nil, tween == nil else { return }
        let (from, to) = span()
        let back = speed < 0
        var start = Double(animationView.realtimeAnimationFrame)
        if done || !back && start >= to - 0.001 || back && start <= from + 0.001 || start < from || start > to {
            start = back ? to : from
        }
        done = false
        let end = back ? from : to
        let whole = back ? start == to : start == from
        // Looping from the middle: to the end once, then the whole range.
        let mode: LottieLoopMode = loop && whole ? .loop : .playOnce
        animationView.play(fromFrame: start, toFrame: end, loopMode: mode) { [weak self] finished in
            guard let self, finished else { return }
            if self.loop {
                self.animationView.play(fromFrame: back ? to : from, toFrame: end, loopMode: .loop)
                return
            }
            self.done = true
            self.reportNow(force: true)
            self.playing = false
            self.tick()
            self.events.playingChange(playing: false)
            self.events.complete()
        }
        tick()
    }

    func setPlaying(_ value: Bool) {
        playing = value
        guard animationView.animation != nil else { return }
        if value {
            play()
        } else {
            animationView.pause()
            reportNow()
        }
        tick()
    }

    func setLoop(_ value: Bool) { loop = value }

    func setSpeed(_ value: Double) {
        let flipped = (value < 0) != (speed < 0)
        speed = value == 0 ? 1 : value
        animationView.animationSpeed = abs(speed)
        if flipped, playing, animationView.isAnimationPlaying { play() }
    }

    func setSegment(_ value: Segment) {
        segment = value
        rangeChanged()
    }

    func setMarker(_ value: String) {
        marker = value
        rangeChanged()
    }

    private func rangeChanged() {
        guard animationView.animation != nil else { return }
        let (from, to) = span()
        done = false
        tween = nil
        animationView.currentFrame = speed < 0 ? to : from
        if playing { play() } else { reportNow() }
    }

    /// Moves it to progress (0 to 1 of the whole animation), unless it's
    /// there already (what it reported, given back).
    func setProgress(_ value: Double) {
        guard value >= 0 else { return }
        guard let a = animationView.animation else { pendingProgress = value; return }
        let span = Double(a.endFrame - a.startFrame)
        guard span > 0 else { return }
        let at = (Double(animationView.realtimeAnimationFrame) - Double(a.startFrame)) / span
        if abs(at - value) < 0.006 { return }
        let target = Double(a.startFrame) + min(max(value, 0), 1) * span
        done = false
        tween = nil
        animationView.currentFrame = target
        lastReported = value
        if playing { play() }
    }

    // MARK: Overrides

    func setOverrides(_ value: [Override]) {
        overrides = value
        if animationView.animation != nil { applyOverrides() }
    }

    /// Sets the overrides' value providers, taking the last ones' away:
    /// a color on the fills and strokes under the keypath, an opacity on
    /// what it names, a width on the strokes under it.
    private func applyOverrides() {
        for k in applied { animationView.removeValueProvider(for: k) }
        applied = []
        func set(_ p: AnyValueProvider, _ path: String) {
            let k = AnimationKeypath(keypath: path)
            animationView.setValueProvider(p, keypath: k)
            applied.append(k)
        }
        for o in overrides where !o.keypath.isEmpty {
            switch o.kind {
            case .color:
                let c = ColorValueProvider(lottieColor(dark ? o.dark : o.light))
                set(c, "\(o.keypath).Color")
                set(c, "\(o.keypath).**.Color")
            case .opacity:
                let v = FloatValueProvider(CGFloat(o.value * 100))
                set(v, "\(o.keypath).Transform.Opacity")
                set(v, "\(o.keypath).Opacity")
            default:
                let v = FloatValueProvider(CGFloat(o.value))
                set(v, "\(o.keypath).Stroke Width")
                set(v, "\(o.keypath).**.Stroke Width")
            }
        }
    }

    // MARK: Frames

    /// Runs the timer while there's a seek to animate or a progress to
    /// report.
    private func tick() {
        let needed = animationView.animation != nil && (tween != nil || animationView.isAnimationPlaying)
        if needed, ticker == nil {
            let t = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.frame() }
            }
            RunLoop.main.add(t, forMode: .common)
            ticker = t
        } else if !needed, let t = ticker {
            t.invalidate()
            ticker = nil
        }
    }

    private func frame() {
        if let tw = tween {
            let x = (CACurrentMediaTime() - tw.start) / tw.seconds
            if x >= 1 {
                tween = nil
                animationView.currentFrame = tw.to
                if playing { play() }
            } else {
                animationView.currentFrame = tw.from + (tw.to - tw.from) * ease(tw.curve, tw.damping, x, tw.seconds)
            }
        }
        reportNow()
        tick()
    }

    /// Reports where it is, if it moved at least 1/200 of the animation
    /// (or force).
    private func reportNow(force: Bool = false) {
        guard let a = animationView.animation, a.endFrame > a.startFrame else { return }
        let f = tween != nil || !animationView.isAnimationPlaying ? Double(animationView.currentFrame) : Double(animationView.realtimeAnimationFrame)
        let p = min(1, max(0, (f - Double(a.startFrame)) / Double(a.endFrame - a.startFrame)))
        if force ? p != lastReported : abs(p - lastReported) >= 0.005 {
            lastReported = p
            events.progressChange(progress: p)
        }
    }

    func setFit(_ value: Fit) {
        switch value {
        case .cover: animationView.contentMode = .scaleAspectFill
        case .stretch: animationView.contentMode = .scaleToFill
        case .center: animationView.contentMode = .center
        default: animationView.contentMode = .scaleAspectFit
        }
    }
}

enum LottieError: Error, LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let m) = self { return m }; return nil }
}

/// A seek's curve at x (0 to 1) of its time: ui.Curve's (EaseInOut,
/// Linear, EaseIn, EaseOut, Spring), as package lottie's Go computes it.
func ease(_ curve: Int, _ damping: Double, _ x: Double, _ seconds: Double) -> Double {
    func bezier(_ x1: Double, _ y1: Double, _ x2: Double, _ y2: Double) -> Double {
        var lo = 0.0, hi = 1.0, t = x
        for _ in 0..<30 {
            let u = 1 - t
            let v = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t
            if abs(v - x) < 1e-6 { break }
            if v < x { lo = t } else { hi = t }
            t = (lo + hi) / 2
        }
        let u = 1 - t
        return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t
    }
    switch curve {
    case 1: return x
    case 2: return bezier(0.42, 0, 1, 1)
    case 3: return bezier(0, 0, 0.58, 1)
    case 4:
        let z = max(0.01, min(1, damping))
        let w = 10 / max(seconds, 0.01)
        let t = x * seconds
        if z >= 1 { return 1 - exp(-w * t) * (1 + w * t) }
        let wd = w * (1 - z * z).squareRoot()
        return 1 - exp(-z * w * t) * (cos(wd * t) + z * w / wd * sin(wd * t))
    default: return bezier(0.42, 0, 0.58, 1)
    }
}

/// A Color (the core's encoding) as lottie-ios' color: its RGBA, a
/// semantic one resolved as the platform's color is.
func lottieColor(_ v: Int) -> LottieColor {
    guard let c = AsukaColors.color(v) else { return LottieColor(r: 0, g: 0, b: 0, a: 1) }
    #if canImport(UIKit)
    var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
    c.getRed(&r, green: &g, blue: &b, alpha: &a)
    #else
    let s = c.usingColorSpace(.sRGB) ?? c
    let r = s.redComponent, g = s.greenComponent, b = s.blueComponent, a = s.alphaComponent
    #endif
    return LottieColor(r: Double(r), g: Double(g), b: Double(b), a: Double(a))
}

@MainActor func makePlayer(_ events: PlayerEvents) -> any Player { LottiePlayer(events) }
