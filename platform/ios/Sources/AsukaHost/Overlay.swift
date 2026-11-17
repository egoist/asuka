#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// Overlay (RFC 0010): a layer over a layout root, which std/base's popups
// (menus, listboxes, tooltips, dialogs) are made in. Its view goes over
// everything in its root's view: the root controller's for the main
// root, a presented sheet's or popover's own for one of those. It fills
// that view, so its children's frames are in the root's coordinates, and
// it reports its Environment as a sheet does.
//
// Presses pass through the layer to what's under it, unless it's Modal:
// then the layer takes those that land on none of its children, and
// what's behind gets nothing. A press that begins in the root on none of
// its views is reported as OutsidePress, before it reaches its target,
// by a window-level gesture recognizer that never takes touches.
//
// AnchorFrame: where its anchor is in the root, and whether any of it
// shows (in the window, not hidden, not clipped away by a view around
// it), after each batch, as a scroll view around the anchor scrolls
// (KVO on its contentOffset), as the root resizes; never one unchanged.

/// An Overlay's view.
final class AsukaOverlayView: UIView {
    weak var overlay: AsukaOverlay?

    /// Presses pass through it, unless it's modal; its children take theirs.
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        let hit = super.hitTest(point, with: event)
        if hit === self { return overlay?.modal == true ? self : nil }
        return hit
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        overlay?.resized()
    }

    override func safeAreaInsetsDidChange() {
        super.safeAreaInsetsDidChange()
        overlay?.reportEnvironment()
    }

    override func traitCollectionDidChange(_ previous: UITraitCollection?) {
        super.traitCollectionDidChange(previous)
        overlay?.reportEnvironment()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard let w = window else { return }
        AsukaRuntime.shared.observeTouches(in: w)
        overlay?.reportEnvironment()
        AsukaRuntime.shared.scheduleAnchors()
    }
}

/// An Overlay's state.
@MainActor final class AsukaOverlay {
    let id: UInt32
    let view: AsukaOverlayView
    var rootID: UInt32 = 0
    var anchorID: UInt32 = 0
    var modal = false {
        didSet { updateModal() }
    }
    /// Views in it with A11yModal: VoiceOver stays in the layer while
    /// one shows.
    var modalViews = Set<UInt32>()
    /// When it was last presented: the last on top.
    var presented = 0
    var lastEnv: AsukaEnvironment?
    /// The AnchorFrame last sent.
    var sentAnchor: [UInt8]?
    /// The scroll views around the anchor, watched.
    var watched: [ObjectIdentifier] = []
    var observers: [NSKeyValueObservation] = []

    init(id: UInt32, view: AsukaOverlayView) {
        self.id = id
        self.view = view
    }

    func updateModal() { view.accessibilityViewIsModal = modal || !modalViews.isEmpty }

    /// Its size changed (its root's did): its environment, and where its
    /// anchor is in it.
    func resized() {
        reportEnvironment()
        AsukaRuntime.shared.scheduleAnchors()
    }

    /// Its Environment, as a sheet's: its size, its root's safe area, the
    /// keyboard over it; when that changed.
    func reportEnvironment() {
        let rt = AsukaRuntime.shared
        guard view.bounds.width > 0, view.superview != nil, rt.nodes[id] != nil else { return }
        let e = rt.environment(of: view)
        if e == lastEnv { return }
        lastEnv = e
        rt.send(AsukaEvents.environment(id, e))
    }

    func forget() {
        for o in observers { o.invalidate() }
        observers = []
        watched = []
    }
}

/// Sees every touch that begins in a window, and takes none: outside
/// presses for the Overlays, and that the last input was a touch.
final class AsukaTouchObserver: UIGestureRecognizer, UIGestureRecognizerDelegate {
    private var down = 0

    init() {
        super.init(target: nil, action: nil)
        cancelsTouchesInView = false
        delaysTouchesBegan = false
        delaysTouchesEnded = false
        delegate = self
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        down += touches.count
        MainActor.assumeIsolated {
            let rt = AsukaRuntime.shared
            rt.notePointer()
            for t in touches { rt.outsidePress(t) }
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) { lift(touches) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) { lift(touches) }

    private func lift(_ touches: Set<UITouch>) {
        down -= touches.count
        if down <= 0 {
            down = 0
            state = .failed
        }
    }

    override func reset() {
        super.reset()
        down = 0
    }

    override func canPrevent(_ g: UIGestureRecognizer) -> Bool { false }
    override func canBePrevented(by g: UIGestureRecognizer) -> Bool { false }
    func gestureRecognizer(_ g: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }
}

@MainActor extension AsukaRuntime {
    func makeOverlay(_ n: AsukaNode) {
        guard let v = n.view as? AsukaOverlayView else { return }
        let o = AsukaOverlay(id: n.id, view: v)
        v.overlay = o
        v.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        n.overlay = o
        overlays.append(n)
    }

    func destroyOverlay(_ n: AsukaNode) {
        n.overlay?.forget()
        overlays.removeAll { $0 === n }
    }

    func setOverlayProp(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) {
        guard let o = n.overlay else { return }
        switch prop {
        case AsukaProp.Overlay.root:
            o.rootID = UInt32(truncatingIfNeeded: v.int)
            attach(n)
        case AsukaProp.Overlay.anchor:
            o.anchorID = UInt32(truncatingIfNeeded: v.int)
            o.sentAnchor = nil // reported again
            o.forget()
            scheduleAnchors()
        case AsukaProp.Overlay.modal:
            o.modal = v.bool
        default:
            break
        }
    }

    /// The view an Overlay over root goes in: a sheet's or a popover's
    /// own, else the root controller's (the node whose container it is,
    /// if one is).
    func overlayHost(_ root: UInt32) -> (UIView, AsukaNode?)? {
        if let r = nodes[root], r.vc is AsukaSheetController || r.vc is AsukaPopoverController {
            return (r.container, r)
        }
        if let c = controller { return (c.view, nil) }
        if let r = nodes[rootID], let s = r.view.superview { return (s, nil) } // a share sheet's
        return nil
    }

    /// Puts an Overlay's view over its root's, filling it.
    func attach(_ n: AsukaNode) {
        guard let o = n.overlay, let (host, hostNode) = overlayHost(o.rootID) else { return }
        hostNode?.hostsOverlays = true
        if o.view.superview !== host {
            o.view.removeFromSuperview()
            host.addSubview(o.view)
        }
        o.view.frame = host.bounds
        raiseOverlays(in: host)
        o.reportEnvironment()
    }

    /// Present: over its root's other overlays.
    func presentOverlay(_ n: AsukaNode) {
        guard let o = n.overlay else { return }
        if o.view.superview == nil { attach(n) }
        presentCount += 1
        o.presented = presentCount
        if let host = o.view.superview { raiseOverlays(in: host) }
        scheduleAnchors()
    }

    /// Keeps the overlays in host over its other views, the last
    /// presented on top.
    func raiseOverlays(in host: UIView) {
        let mine = overlays.compactMap { $0.overlay }.filter { $0.view.superview === host }.sorted { $0.presented < $1.presented }
        for o in mine { host.bringSubviewToFront(o.view) }
    }

    /// Reports each overlay's environment again (the keyboard moved).
    func reportOverlays() {
        for n in overlays { n.overlay?.reportEnvironment() }
    }

    // MARK: Outside presses

    /// Watches a window's touches (once per window).
    func observeTouches(in w: UIWindow) {
        if w.gestureRecognizers?.contains(where: { $0 is AsukaTouchObserver }) == true { return }
        w.addGestureRecognizer(AsukaTouchObserver())
    }

    /// A touch began: each overlay whose root it's in, and which it's on
    /// no view of, is told (OutsidePress, in the root's coordinates).
    func outsidePress(_ t: UITouch) {
        guard let w = t.window else { return }
        outsidePress(in: w, at: t.location(in: w), on: t.view)
    }

    /// A press began at point (in the window's coordinates) on view (the
    /// one hit-testing found).
    func outsidePress(in window: UIWindow, at point: CGPoint, on view: UIView?) {
        for n in overlays {
            guard let o = n.overlay, n.listens.contains(AsukaEventKind.outsidePress.rawValue), o.view.window === window else { continue }
            let p = o.view.convert(point, from: window)
            guard o.view.bounds.contains(p) else { continue }
            if let on = view, on !== o.view, on.isDescendant(of: o.view) { continue }
            send(AsukaEvents.outsidePress(n.id, x: Double(p.x), y: Double(p.y)))
        }
    }

    // MARK: Anchors

    /// Reports the anchors soon: once for every change this turn.
    func scheduleAnchors() {
        guard !anchorsDue else { return }
        anchorsDue = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.anchorsDue = false
            self.reportAnchors()
        }
    }

    /// Reports where each overlay's anchor is, if that changed.
    func reportAnchors() {
        for n in overlays {
            guard let o = n.overlay, o.anchorID != 0, n.listens.contains(AsukaEventKind.anchorFrame.rawValue) else { continue }
            guard let b = anchorFrame(o) else { continue }
            if b == o.sentAnchor { continue }
            o.sentAnchor = b
            send(AsukaEvents.anchorFrame(n.id, b))
        }
    }

    /// An overlay's AnchorFrame: f32 x, y, w, h of its anchor in its
    /// root, then 1 if any of it shows; nil while its root isn't in a
    /// window (it's yet to show: it's reported once it is) or its anchor
    /// is gone.
    func anchorFrame(_ o: AsukaOverlay) -> [UInt8]? {
        guard let window = o.view.window, let a = nodes[o.anchorID]?.view else { return nil }
        watchScrolls(o, a)
        var w = AsukaWriter()
        guard a.window === window else {
            for _ in 0..<4 { w.f32(0) }
            w.u8(0)
            return w.bytes
        }
        let r = a.convert(a.bounds, to: o.view)
        w.f32(Float(r.minX))
        w.f32(Float(r.minY))
        w.f32(Float(r.width))
        w.f32(Float(r.height))
        w.u8(shows(a) ? 1 : 0)
        return w.bytes
    }

    /// Whether any of v shows: neither it nor a view around it hidden, and
    /// some of it inside every view around it that clips (a scroll view's
    /// visible part) and the window.
    func shows(_ v: UIView) -> Bool {
        if v.isHidden { return false }
        var p = v.superview
        while let x = p {
            if x.isHidden { return false }
            if x.clipsToBounds || x is UIWindow {
                let r = v.convert(v.bounds, to: x)
                let b = x.bounds
                if r.maxX < b.minX || r.minX > b.maxX || r.maxY < b.minY || r.minY > b.maxY { return false }
            }
            if x is UIWindow { break }
            p = x.superview
        }
        return true
    }

    /// Watches the scroll views around an overlay's anchor, which move it
    /// as they scroll.
    func watchScrolls(_ o: AsukaOverlay, _ a: UIView) {
        var scrolls: [UIScrollView] = []
        var p = a.superview
        while let x = p {
            if let s = x as? UIScrollView { scrolls.append(s) }
            p = x.superview
        }
        let ids = scrolls.map { ObjectIdentifier($0) }
        if ids == o.watched { return }
        o.forget()
        o.watched = ids
        o.observers = scrolls.map { s in
            s.observe(\.contentOffset, options: []) { _, _ in
                MainActor.assumeIsolated { AsukaRuntime.shared.scheduleAnchors() }
            }
        }
    }
}
#endif
