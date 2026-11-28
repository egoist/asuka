// Gestures: long presses (NSPressGestureRecognizer, and a secondary
// click), pans, pinches (magnification) and rotations from AppKit's
// recognizers; and swipes (OnSwipe) on a Mac, as UISwipeGestureRecognizer's
// on iOS, a quick flick one way:
//
//   - With the mouse, a pan that ends fast (the view's pan recognizer).
//   - On a trackpad, a two-finger flick (its scroll events, from the
//     fingers touching to their lifting): across always, up and down only
//     where no scroll view around the view scrolls that way. The scroll
//     goes on to the scroll views, which still scroll.
//   - A three-finger swipe (swipe(with:)), where the system's settings
//     leave it to apps.
//
// A pan is the mouse's drag (NSPanGestureRecognizer): it's reported as
// begun once the mouse has gone PanMinDistance (AppKit's own distance
// without it), along PanAxis; a drag that goes the other way first isn't a
// pan, and the press it began goes on. The scroll views around scroll by
// the wheel and the trackpad's scrolls, which a pan never takes: they
// always follow those; PanSimultaneous lets the pan's recognizer go along
// with the other recognizers around it too.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// The swipe directions, as ui.SwipeDirection's bits.
let asukaSwipeRight: Int64 = 1 << 0
let asukaSwipeLeft: Int64 = 1 << 1
let asukaSwipeUp: Int64 = 1 << 2
let asukaSwipeDown: Int64 = 1 << 3

/// A gesture's swipe, from how far (points) and how fast (points a second)
/// it went when it ended: 0 for none.
func asukaSwipeDirection(_ dx: CGFloat, _ dy: CGFloat, _ vx: CGFloat, _ vy: CGFloat) -> Int64 {
    let far: CGFloat = 30, fast: CGFloat = 300
    if abs(dx) > abs(dy) && dx > far && vx > fast { return asukaSwipeRight }
    if abs(dx) > abs(dy) && dx < -far && vx < -fast { return asukaSwipeLeft }
    if abs(dy) >= abs(dx) && dy < -far && vy < -fast { return asukaSwipeUp }
    if abs(dy) >= abs(dx) && dy > far && vy > fast { return asukaSwipeDown }
    return 0
}

/// A trackpad's two-finger scroll over a view that takes swipes: how far
/// the fingers went, and how fast lately.
struct AsukaWheelSwipe {
    weak var view: NSView?
    /// Points, the way the fingers moved.
    var dx: CGFloat = 0, dy: CGFloat = 0
    /// Points a second.
    var vx: CGFloat = 0, vy: CGFloat = 0
    /// The last event's time (seconds).
    var last: TimeInterval = 0
}

extension AsukaVisual: NSGestureRecognizerDelegate {
    @objc func longPressed(_ g: NSGestureRecognizer) { AsukaRuntime.shared.gesture(g, .longPress) }
    @objc func panned(_ g: NSGestureRecognizer) { AsukaRuntime.shared.gesture(g, .pan) }
    @objc func magnified(_ g: NSGestureRecognizer) { AsukaRuntime.shared.gesture(g, .pinch) }
    @objc func rotated(_ g: NSGestureRecognizer) { AsukaRuntime.shared.gesture(g, .rotate) }

    /// A pan doesn't begin while the mouse went down on a draggable view
    /// (moving it drags the view) or lifted a list's row, nor along the
    /// axis its PanAxis doesn't take.
    func gestureRecognizerShouldBegin(_ g: NSGestureRecognizer) -> Bool {
        MainActor.assumeIsolated {
            guard mouse.source == nil && drag == nil && !mouse.lifted else { return false }
            guard let p = g as? NSPanGestureRecognizer, let v = p.view, let x = AsukaRuntime.shared.node(of: v)?.interaction else { return true }
            return asukaPanAlong(x.panAxis, p.translation(in: v))
        }
    }

    /// PanSimultaneous: the pan goes along with the recognizers around it.
    func gestureRecognizer(_ g: NSGestureRecognizer, shouldRecognizeSimultaneouslyWith other: NSGestureRecognizer) -> Bool {
        MainActor.assumeIsolated {
            let rt = AsukaRuntime.shared
            for r in [g, other] where r is NSPanGestureRecognizer {
                if let v = r.view, rt.node(of: v)?.interaction?.panSimultaneous == true { return true }
            }
            return false
        }
    }
}

/// Whether a drag that went t so far goes along axis (no move yet: it may).
func asukaPanAlong(_ axis: AsukaPanAxis, _ t: CGPoint) -> Bool {
    if t.x == 0 && t.y == 0 { return true }
    switch axis {
    case .horizontal: return abs(t.x) >= abs(t.y)
    case .vertical: return abs(t.y) >= abs(t.x)
    default: return true
    }
}

@MainActor extension AsukaRuntime {
    /// Reports a recognizer's gesture.
    func gesture(_ g: NSGestureRecognizer, _ e: AsukaEventKind) {
        guard let view = g.view, let n = node(of: view) else { return }
        let state = g.state
        if e != .pan && (state == .began || state == .ended && e == .longPress) {
            cancelPress() // the gesture takes the mouse (a pan once it's live)
        }
        if e == .longPress {
            if state == .began || state == .ended && g is NSClickGestureRecognizer {
                if n.menu != nil && n.menuTrigger == AsukaMenuTrigger.longPress.rawValue { return } // its context menu shows
                if visual.drag != nil { return } // the hold became a drag
                emit(n, .longPress) { AsukaEvents.longPress($0) }
            }
            return
        }
        var gs = AsukaGesture()
        switch state {
        case .began: gs.state = AsukaGestureState.began.rawValue
        case .changed: gs.state = AsukaGestureState.changed.rawValue
        case .ended: gs.state = AsukaGestureState.ended.rawValue
        default: gs.state = AsukaGestureState.cancelled.rawValue
        }
        let loc = g.location(in: view)
        gs.x = Float(loc.x)
        gs.y = Float(loc.y)
        gs.scale = 1
        switch e {
        case .pan:
            guard let p = g as? NSPanGestureRecognizer, let x = n.interaction else { return }
            let t = p.translation(in: view), vel = p.velocity(in: view)
            if state == .began { x.panLive = false }
            if !x.panLive {
                // Not a pan till it has gone its distance along its axis.
                guard state == .began || state == .changed else { return }
                if hypot(t.x, t.y) < x.panMinDistance { return }
                if !asukaPanAlong(x.panAxis, t) {
                    // The other way: not a pan; the press goes on.
                    p.isEnabled = false
                    p.isEnabled = true
                    return
                }
                x.panLive = true
                cancelPress() // the pan takes the mouse
                visual.mouse.row = nil // and the row it's in doesn't lift
                gs.state = AsukaGestureState.began.rawValue
            }
            if gs.state == AsukaGestureState.ended.rawValue || gs.state == AsukaGestureState.cancelled.rawValue { x.panLive = false }
            gs.translationX = Float(t.x)
            gs.translationY = Float(t.y)
            gs.velocityX = Float(vel.x)
            gs.velocityY = Float(vel.y)
            let ev = gs
            emit(n, .pan) { AsukaEvents.pan($0, ev) }
            if gs.state == AsukaGestureState.ended.rawValue {
                let dir = asukaSwipeDirection(t.x, t.y, vel.x, vel.y)
                if dir != 0 { emit(n, .swipe) { AsukaEvents.swipe($0, dir) } }
            }
            return
        case .pinch:
            gs.scale = Float(1 + ((g as? NSMagnificationGestureRecognizer)?.magnification ?? 0))
            let ev = gs
            emit(n, .pinch) { AsukaEvents.pinch($0, ev) }
        case .rotate:
            // AppKit's is counterclockwise; Asuka's clockwise.
            gs.rotation = -Float((g as? NSRotationGestureRecognizer)?.rotation ?? 0)
            let ev = gs
            emit(n, .rotate) { AsukaEvents.rotate($0, ev) }
        default:
            break
        }
    }

    /// A three-finger swipe on a view: false when it takes no swipes (its
    /// superview's then).
    func swipeEvent(_ v: NSView, _ e: NSEvent) -> Bool {
        guard let n = node(of: v), n.listening(.swipe) else { return false }
        // AppKit's deltas: x 1 a swipe to the left, y 1 a swipe up.
        var dir: Int64 = 0
        if e.deltaX > 0 { dir = asukaSwipeLeft } else if e.deltaX < 0 { dir = asukaSwipeRight }
        else if e.deltaY > 0 { dir = asukaSwipeUp } else if e.deltaY < 0 { dir = asukaSwipeDown }
        if dir != 0 { emit(n, .swipe) { AsukaEvents.swipe($0, dir) } }
        return true
    }

    /// Looks at a scroll event over v for a swipe. It never takes the
    /// event: the scroll views around still scroll (false).
    func wheel(_ v: NSView, _ e: NSEvent) -> Bool {
        guard let n = node(of: v), n.listening(.swipe), e.hasPreciseScrollingDeltas else { return false }
        let t = e.timestamp
        // The deltas move the content; the fingers move the other way
        // unless scrolling is natural (the content follows them). AppKit's
        // y grows up the screen in a scroll's deltas: a finger moving down
        // the trackpad scrolls the content down (natural), a positive y.
        var dx = e.scrollingDeltaX, dy = e.scrollingDeltaY
        if !e.isDirectionInvertedFromDevice {
            dx = -dx
            dy = -dy
        }
        let phase = e.phase
        if phase.contains(.began) {
            visual.wheelSwipe = AsukaWheelSwipe(view: v, last: t)
            return false
        }
        guard visual.wheelSwipe.view === v else { return false }
        if phase.contains(.changed) {
            var w = visual.wheelSwipe
            let dt = t - w.last
            if dt > 0 {
                // The last ~50 ms's velocity.
                let a = CGFloat(min(dt / 0.05, 1))
                w.vx += (dx / CGFloat(dt) - w.vx) * a
                w.vy += (dy / CGFloat(dt) - w.vy) * a
            }
            w.dx += dx
            w.dy += dy
            w.last = t
            visual.wheelSwipe = w
        } else if phase.contains(.ended) || phase.contains(.cancelled) {
            let ended = phase.contains(.ended)
            let sw = visual.wheelSwipe
            visual.wheelSwipe = AsukaWheelSwipe()
            guard ended, t - sw.last <= 0.1 else { return false } // the fingers stopped before they lifted
            let dir = asukaSwipeDirection(sw.dx, sw.dy, sw.vx, sw.vy)
            if dir & (asukaSwipeUp | asukaSwipeDown) != 0 && scrollsAround(v, across: false) { return false } // the scroll view's
            if dir & (asukaSwipeLeft | asukaSwipeRight) != 0 && scrollsAround(v, across: true) { return false }
            if dir != 0 { emit(n, .swipe) { AsukaEvents.swipe($0, dir) } }
        }
        return false
    }

    /// Whether a scroll view around v scrolls that way.
    func scrollsAround(_ v: NSView, across: Bool) -> Bool {
        var sv = v.enclosingScrollView
        while let s = sv {
            if let doc = s.documentView {
                let d = doc.frame, c = s.contentView.bounds
                if across && d.width > c.width + 0.5 || !across && d.height > c.height + 0.5 { return true }
            }
            sv = s.superview?.enclosingScrollView
        }
        return false
    }
}
