// Interaction states (DESIGN.md, "Interaction states, drag and drop").
//
// Presses are the views' own mouse tracking (AsukaView's mouseDown,
// mouseDragged, mouseUp), not a click recognizer: Pressed and Press then
// come from the same events and agree. The recognizers the views have
// (long press, pan, ...) don't delay the mouse's events (their
// delaysPrimaryMouseButtonEvents is off), so the view sees the press as it
// happens; one that begins cancels it. Only the innermost pressable view
// under the mouse is pressed: a control (a button, a field) takes its
// clicks itself and never passes them on.
//
// A double click (NSEvent's clickCount) on a view listening for DoubleTap
// reports it as the second click goes down, as AppKit's double clicks
// act; a view listening for Press too presses only once the system's
// double-click interval has passed without a second click. On a zoomable
// Scroll's content, it zooms (Zoom.swift).
//
// A drag of a row of a List whose rows move lifts it (ListReorder.swift),
// unless a draggable view in the row takes the drag.
//
// Hover is an NSTrackingArea per view, checked again as views appear, move
// or show under a still pointer; listening for HoverMove, its area also
// sends the pointer's moves. Its areas work in the active app alone, so
// leaving the app clears every hover and coming back checks them again
// (appActive). Gestures are Gestures.swift's, drops and
// drags DragDrop.swift's.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A node's interaction states: its hover area, its drop types and drag
/// items, its pan recognizer.
@MainActor final class AsukaInteraction {
    var hoverArea: NSTrackingArea?
    var hovered = false
    /// DropAccept's MIME patterns.
    var dropAccept: [String] = []
    /// DragItems: it's draggable.
    var dragItems: [AsukaItem] = []
    /// Its NSPanGestureRecognizer (Pan, Swipe), and how it begins: along
    /// PanAxis only, past PanMinDistance, alongside other recognizers
    /// (PanSimultaneous). live: the pan under way was reported as begun.
    var panGesture: NSPanGestureRecognizer?
    var panAxis = AsukaPanAxis.both
    var panMinDistance: CGFloat = 0
    var panSimultaneous = false
    var panLive = false
    /// The last mouse move reported (HoverMove), by its time: the area's
    /// owner and the first responder may both see it.
    var movedAt: TimeInterval = -1
    /// Its other recognizers, by event (one of each).
    var gestures: Set<UInt16> = []
}

/// The primary mouse button's press, from its down to its up.
struct AsukaMouseState {
    var active = false
    /// The innermost pressable view, nil once cancelled.
    var target: AsukaNode?
    /// The mouse is on target (it's pressed).
    var inside = false
    /// The innermost draggable view.
    var source: AsukaNode?
    /// Where it went down, in window coordinates.
    var down = CGPoint.zero
    /// The row of a List whose rows move it went down on, and whether
    /// the drag lifted it.
    var row: AsukaListCell?
    var lifted = false
}

/// A press waiting to see whether a second click makes it a double click.
struct AsukaPendingPress {
    var node: AsukaNode?
    var timer: Timer?
}

/// How far the mouse moves with the button down, in points, before a
/// draggable view's drag begins (AppKit's own views begin theirs at about
/// this distance).
let asukaDragDistance: CGFloat = 3

@MainActor extension AsukaRuntime {
    /// n's interaction states, made as they're needed.
    func ix(_ n: AsukaNode) -> AsukaInteraction {
        if let x = n.interaction { return x }
        let x = AsukaInteraction()
        n.interaction = x
        return x
    }

    /// Whether a click on n is n's: it presses, long presses, reports its
    /// pressed state or opens a menu.
    func pressable(_ n: AsukaNode) -> Bool {
        n.listening(.press) || n.listening(.longPress) || n.listening(.pressed) || n.listening(.doubleTap)
            || n.menu != nil && n.menuTrigger == AsukaMenuTrigger.tap.rawValue
    }

    /// Whether n is still one of the core's views.
    func live(_ n: AsukaNode?) -> Bool {
        guard let n else { return false }
        return nodes[n.id] === n && n.view != nil
    }

    // MARK: Listening

    /// Adds what an event needs on a view: gesture recognizers, tracking
    /// areas, the press role.
    func listenGesture(_ n: AsukaNode, _ e: AsukaEventKind) {
        guard n.kind == .view, let v = n.view else { return }
        let x = ix(n)
        func add(_ g: NSGestureRecognizer) {
            // The view sees the mouse as it goes, for its press (Pressed); a
            // gesture that begins cancels the press.
            g.delaysPrimaryMouseButtonEvents = false
            v.addGestureRecognizer(g)
        }
        switch e {
        case .press:
            // Pressed and released on the view (mouseDown, mouseUp); a button
            // to VoiceOver, unless its role says otherwise (A11y.swift).
            v.setAccessibilityElement(true)
            if (n.a11y?.role ?? .none) == .none { v.setAccessibilityRole(.button) }
        case .longPress:
            guard x.gestures.insert(e.rawValue).inserted else { return }
            let g = NSPressGestureRecognizer(target: visual, action: #selector(AsukaVisual.longPressed(_:)))
            g.minimumPressDuration = 0.5
            add(g)
            // A secondary click is a Mac's long press.
            let r = NSClickGestureRecognizer(target: visual, action: #selector(AsukaVisual.longPressed(_:)))
            r.buttonMask = 2
            add(r)
        case .pan, .swipe:
            // A swipe is a quick pan (a drag with the mouse) or a trackpad's
            // flick (Gestures.swift).
            guard x.panGesture == nil else { return }
            let g = NSPanGestureRecognizer(target: visual, action: #selector(AsukaVisual.panned(_:)))
            g.delegate = visual
            add(g)
            x.panGesture = g
        case .hover, .hoverMove:
            listenHover(n)
        case .pinch:
            guard x.gestures.insert(e.rawValue).inserted else { return }
            add(NSMagnificationGestureRecognizer(target: visual, action: #selector(AsukaVisual.magnified(_:))))
        case .rotate:
            guard x.gestures.insert(e.rawValue).inserted else { return }
            add(NSRotationGestureRecognizer(target: visual, action: #selector(AsukaVisual.rotated(_:))))
        default:
            break
        }
    }

    // MARK: Presses

    /// Starts tracking a press on v (the view the click hit, or one it
    /// passed the click to); false: no view there takes it.
    func mouseDown(_ v: NSView, _ e: NSEvent) -> Bool {
        cancelPress()
        endRowDrag(cancel: true)
        visual.mouse = AsukaMouseState()
        if e.modifierFlags.contains(.control) { return false } // a secondary click: the context menu
        pressFocus(v) // the Focusable view it's on takes the focus (Focus.swift)
        if visual.listSelectClick?(v) == true { return true } // a row of a selecting list: selected or deselected
        // The innermost pressable view, draggable view, view taking double
        // clicks (or zoomable Scroll), and row whose list's rows move: a
        // draggable view in the row drags, one around the list doesn't.
        var target: AsukaNode?, source: AsukaNode?, double: AsukaNode?, zoom: AsukaNode?, row: AsukaListCell?
        var at: NSView? = v
        while let x = at {
            at = x.superview
            guard let n = node(of: x) else { continue }
            if target == nil && pressable(n) { target = n }
            if source == nil && row == nil, let d = n.interaction?.dragItems, !d.isEmpty { source = n }
            if double == nil && zoom == nil {
                if n.listening(.doubleTap) { double = n } else if zoomable(n) { zoom = n }
            }
            if row == nil, let c = n.cell, let l = c.list, l.reorders, !c.header { row = c }
        }
        // A second click (of two, of four...): a double click.
        let second = e.clickCount >= 2 && e.clickCount % 2 == 0
        if !second || visual.pendingPress.node !== double { settlePress() } // a click elsewhere: the first one's press goes
        if second, let d = double {
            let waited = visual.pendingPress.node === d
            cancelPendingPress()
            if let dv = d.view {
                let p = dv.convert(e.locationInWindow, from: nil)
                emit(d, .doubleTap) { AsukaEvents.doubleTap($0, x: Double(p.x), y: Double(p.y)) }
            }
            // Its own press doesn't follow: it's the double click's.
            if waited || target === d || target == nil { return true }
        } else if second, let z = zoom {
            doubleClickZoom(z, e)
            if target == nil { return true }
        }
        guard target != nil || source != nil || row != nil else { return false }
        visual.mouse = AsukaMouseState(active: true, target: target, inside: false, source: source, down: e.locationInWindow, row: row)
        if let t = target {
            visual.mouse.inside = true
            if t.listening(.pressPoint), let tv = t.view {
                // Where it began, from its top left.
                let p = tv.convert(e.locationInWindow, from: nil)
                emit(t, .pressPoint) { AsukaEvents.pressPoint($0, x: Double(p.x), y: Double(p.y)) }
            }
            if live(t) { emit(t, .pressed) { AsukaEvents.pressed($0, true) } }
        }
        return true
    }

    func mouseDragged(_ e: NSEvent) -> Bool {
        guard visual.mouse.active else { return false }
        let loc = e.locationInWindow
        let m = visual.mouse
        if let s = m.source, hypot(loc.x - m.down.x, loc.y - m.down.y) >= asukaDragDistance {
            cancelPress()
            visual.mouse = AsukaMouseState()
            if live(s) { beginDrag(s, e) }
            return true
        }
        if let c = m.row {
            // A row of a list whose rows move: past the drag distance, it's
            // lifted and follows the mouse (ListReorder.swift).
            if m.lifted {
                c.list?.dragRow(loc)
                return true
            }
            if hypot(loc.x - m.down.x, loc.y - m.down.y) >= asukaDragDistance, let l = c.list, l.liftRow(c, m.down) {
                cancelPress()
                visual.mouse.lifted = true
                l.dragRow(loc)
                return true
            }
        }
        if let t = m.target {
            guard live(t) else {
                visual.mouse.target = nil
                return true
            }
            let inside = mouseIn(t, loc)
            if inside != m.inside {
                visual.mouse.inside = inside
                emit(t, .pressed) { AsukaEvents.pressed($0, inside) }
            }
        }
        return true
    }

    func mouseUp(_ e: NSEvent) -> Bool {
        let m = visual.mouse
        guard m.active else { return false }
        visual.mouse = AsukaMouseState()
        if m.lifted {
            m.row?.list?.dropRow()
            return true
        }
        guard let t = m.target, m.inside, live(t) else { return true }
        if mouseIn(t, e.locationInWindow) {
            if t.menu != nil && t.menuTrigger == AsukaMenuTrigger.tap.rawValue && !t.listening(.press) {
                emit(t, .pressed) { AsukaEvents.pressed($0, false) }
                popUp(t)
                return true
            }
            if t.listening(.doubleTap) && t.listening(.press) {
                // A second click may make it a double click: its press
                // waits for the system's double-click interval.
                deferPress(t)
            } else {
                emit(t, .press) { AsukaEvents.press($0) }
            }
        }
        if live(t) { emit(t, .pressed) { AsukaEvents.pressed($0, false) } }
        return true
    }

    /// Holds n's press until a second click can't come.
    func deferPress(_ n: AsukaNode) {
        cancelPendingPress()
        let t = Timer(timeInterval: NSEvent.doubleClickInterval, repeats: false) { _ in
            MainActor.assumeIsolated { AsukaRuntime.shared.settlePress() }
        }
        RunLoop.main.add(t, forMode: .common)
        visual.pendingPress = AsukaPendingPress(node: n, timer: t)
    }

    /// Presses the view whose press waited, if one did.
    func settlePress() {
        guard let n = visual.pendingPress.node else { return }
        cancelPendingPress()
        if live(n) { emit(n, .press) { AsukaEvents.press($0) } }
    }

    /// Forgets the press that waited: a double click took it.
    func cancelPendingPress() {
        visual.pendingPress.timer?.invalidate()
        visual.pendingPress = AsukaPendingPress()
    }

    /// Ends the row drag under way: dropped where it is, or put back.
    func endRowDrag(cancel: Bool) {
        let m = visual.mouse
        guard m.lifted, let l = m.row?.list else { return }
        visual.mouse.lifted = false
        visual.mouse.row = nil
        if cancel { l.cancelRowDrag() } else { l.dropRow() }
    }

    /// Whether a point in the window is on n's view.
    func mouseIn(_ n: AsukaNode, _ loc: CGPoint) -> Bool {
        guard let v = n.view else { return false }
        let p = v.convert(loc, from: nil)
        let b = v.bounds
        return p.x >= b.minX && p.y >= b.minY && p.x < b.maxX && p.y < b.maxY
    }

    /// Ends the press under way without a Press: a gesture, a drag or the
    /// app going away took the mouse. A drag may still begin.
    func cancelPress() {
        guard let t = visual.mouse.target else { return }
        visual.mouse.target = nil
        if visual.mouse.inside && live(t) { emit(t, .pressed) { AsukaEvents.pressed($0, false) } }
        visual.mouse.inside = false
    }

    /// n left its window or was hidden: no longer pressed or hovered.
    func viewLeft(_ n: AsukaNode) {
        if visual.mouse.target === n { cancelPress() }
        setHover(n, false)
    }

    // MARK: Hover

    /// Gives n's view a tracking area: entered and exited (and moved, for
    /// HoverMove), over its visible rect, in the active app, during drags
    /// too.
    func listenHover(_ n: AsukaNode) {
        let x = ix(n)
        guard let v = n.view else { return }
        var opts: NSTrackingArea.Options = [.mouseEnteredAndExited, .activeInActiveApp, .inVisibleRect, .enabledDuringMouseDrag]
        if n.listening(.hoverMove) { opts.insert(.mouseMoved) }
        if let old = x.hoverArea {
            if old.options == opts { return }
            v.removeTrackingArea(old)
        }
        let a = NSTrackingArea(rect: .zero, options: opts, owner: v, userInfo: nil)
        v.addTrackingArea(a)
        x.hoverArea = a
        checkHover(n)
    }

    func mouseEntered(_ v: NSView) {
        if let n = node(of: v), n.interaction?.hoverArea != nil { setHover(n, true) }
    }

    func mouseExited(_ v: NSView) {
        if let n = node(of: v), n.interaction?.hoverArea != nil { setHover(n, false) }
    }

    /// The pointer moved over v: false when v doesn't report it (its
    /// superview's then).
    func mouseMoved(_ v: NSView, _ e: NSEvent) -> Bool {
        guard let n = node(of: v), let x = n.interaction, x.hoverArea != nil, n.listening(.hoverMove) else { return false }
        if x.movedAt == e.timestamp { return true } // seen
        x.movedAt = e.timestamp
        hoverMoved(n, v.convert(e.locationInWindow, from: nil))
        return true
    }

    /// Reports where the pointer is over n (from its top left), once it's
    /// hovered: a move before its entered event hovers it.
    func hoverMoved(_ n: AsukaNode, _ p: CGPoint) {
        guard let x = n.interaction else { return }
        if !x.hovered { setHover(n, true) }
        guard x.hovered else { return }
        emit(n, .hoverMove) { AsukaEvents.hoverMove($0, x: Double(p.x), y: Double(p.y)) }
    }

    /// Reports n hovered, or not, if that changed.
    func setHover(_ n: AsukaNode, _ on: Bool) {
        guard let x = n.interaction, x.hovered != on, n.listening(.hover) || n.listening(.hoverMove) || x.hovered else { return }
        if on, let v = n.view, behindModal(v) { return } // a Modal overlay takes the pointer (Overlay.swift)
        x.hovered = on
        if live(n) { emit(n, .hover) { AsukaEvents.hover($0, on) } }
    }

    /// Checks, once the core has returned, whether the pointer is over n:
    /// a view that appears, moves or shows under a still pointer gets no
    /// entered event.
    func checkHover(_ n: AsukaNode) {
        guard let x = n.interaction, !x.hovered, x.hoverArea != nil else { return }
        visual.hoverCheck.append(n)
        queueHoverCheck()
    }

    func queueHoverCheck() {
        guard !visual.hoverQueued, visual.hoverHeld == 0, !visual.hoverCheck.isEmpty else { return }
        visual.hoverQueued = true
        afterCore { self.runHoverCheck() }
    }

    /// Holds the hover checks until the views are placed (hoverHeld), then
    /// runs them.
    func holdHover() { visual.hoverHeld += 1 }

    func releaseHover() {
        visual.hoverHeld -= 1
        queueHoverCheck()
    }

    func runHoverCheck() {
        visual.hoverQueued = false
        guard visual.hoverHeld == 0 else { return } // releaseHover runs them
        let q = visual.hoverCheck
        visual.hoverCheck = []
        for n in q {
            guard let x = n.interaction, !x.hovered, live(n), n.listening(.hover) || n.listening(.hoverMove), let v = n.view, !v.isHiddenOrHasHiddenAncestor,
                  let w = v.window, let p = hoverProbe(w) else { continue }
            // A List's row's visibleRect is the list's whole shown area, not
            // clipped to the row: the bounds clip it.
            if v.visibleRect.intersection(v.bounds).contains(v.convert(p, from: nil)) { setHover(n, true) }
        }
    }

    /// Where the pointer is in w, in its coordinates, if it's over w in the
    /// active app (under the automation, its hoverprobe's: none without).
    func hoverProbe(_ w: NSWindow) -> CGPoint? {
        if let probe = visual.hoverProbe { return probe(w) }
        guard !AsukaAutomation.shared.active, NSApp.isActive, w.isVisible else { return nil }
        let p = NSEvent.mouseLocation
        guard NSWindow.windowNumber(at: p, belowWindowWithWindowNumber: 0) == w.windowNumber else { return nil }
        return w.convertPoint(fromScreen: p)
    }

    /// Follows the app's activation: leaving it, nothing is pressed or
    /// hovered; back, the views under the pointer are.
    func appActive(_ active: Bool) {
        if !active {
            cancelPress()
            endRowDrag(cancel: true)
            visual.mouse = AsukaMouseState()
        }
        for n in nodes.values where n.interaction?.hoverArea != nil {
            if active { checkHover(n) } else { setHover(n, false) }
        }
    }

    /// Forgets n as it goes: the core reports it leaving its states.
    func dropInteraction(_ n: AsukaNode) {
        if visual.mouse.target === n {
            visual.mouse.target = nil
            visual.mouse.inside = false
        }
        if visual.mouse.source === n { visual.mouse.source = nil }
        if visual.pendingPress.node === n { cancelPendingPress() }
        if visual.over.target === n { visual.over.target = nil }
        visual.hoverCheck.removeAll { $0 === n }
        n.interaction?.hovered = false
        n.interaction?.hoverArea = nil
    }

    func interactionJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let x = n.interaction, let v = n.view else { return }
        var j: [String: Any] = ["hovered": x.hovered, "dropAccept": x.dropAccept, "dragItems": x.dragItems.count,
                                "pan": x.panGesture != nil, "registeredTypes": v.registeredDraggedTypes.count,
                                "panAxis": Int(x.panAxis.rawValue), "panMinDistance": Double(x.panMinDistance), "panSimultaneous": x.panSimultaneous,
                                "recognizers": v.gestureRecognizers.map { String(describing: type(of: $0)) }]
        if let a = x.hoverArea {
            j["hoverOptions"] = a.options.rawValue
            j["trackingAreas"] = v.trackingAreas.count
        }
        d["interaction"] = j
    }
}

extension AsukaVisual {
    /// Follows the app's activation (once, as the host starts).
    @MainActor func observeApp() {
        guard !appObserved else { return }
        appObserved = true
        let c = NotificationCenter.default
        c.addObserver(self, selector: #selector(appResigned(_:)), name: NSApplication.didResignActiveNotification, object: nil)
        c.addObserver(self, selector: #selector(appBecameActive(_:)), name: NSApplication.didBecomeActiveNotification, object: nil)
    }

    @objc func appResigned(_ note: Notification) { AsukaRuntime.shared.appActive(false) }
    @objc func appBecameActive(_ note: Notification) { AsukaRuntime.shared.appActive(true) }
}
