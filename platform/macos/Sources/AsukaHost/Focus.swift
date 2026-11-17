// The keyboard's focus on a Mac (RFC 0010's host contract):
//
//   - A view takes the focus (acceptsFirstResponder) when it's Focusable,
//     pressable or adjustable, or it's a control or a field, and isn't
//     disabled. Focusable views are always Tab stops; pressable and
//     adjustable views and controls are while Full Keyboard Access is on,
//     as AppKit's own controls are; fields always are.
//   - Tab and Shift+Tab move the focus in the core's order (its views'
//     tree: a stack's top screen, a tabs' selected one, a list's rows
//     shown, then the root's overlays as presented), not AppKit's
//     geometric key view loop, and within the innermost FocusTrap around
//     the focus. A multiline field's Tab is its own (it types a tab).
//   - A primary press focuses the Focusable view it's on (or around it)
//     unless it has NoFocusOnPress; a pressable view that isn't Focusable
//     isn't focused by a click, as AppKit's buttons aren't. While a press
//     is under way, AppKit's own focusing on mouse down asks the same.
//   - Each window's first responder is watched (KVO): a node that takes
//     or loses it reports FocusChange (0 lost, 1 a pointer or the app, 2
//     the keyboard: the last input was a key), unless it's a TextField,
//     which reports Focus and Blur.
//   - A Focusable view draws AppKit's focus ring, rounded as its corners,
//     while it has the focus from the keyboard, unless NoFocusRing (which
//     also sets a control's focusRingType to none).
//   - Keys no view's Keys took: Enter and Space press a focused pressable
//     view; text typed goes to it as KeyText (a listbox's typeahead).
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A node's focus props.
@MainActor final class AsukaFocusProps {
    var focusable = false
    var noFocusOnPress = false
    var noFocusRing = false
    var trap = false
    var disabled = false
    /// It has the focus from the keyboard: its ring shows.
    var visible = false
}

/// A window's first responder, as watched.
@MainActor final class AsukaFocusWatch {
    weak var window: NSWindow?
    var observation: NSKeyValueObservation?
    /// The node that has its focus.
    weak var focused: AsukaNode?
}

/// The focus as the host follows it.
@MainActor final class AsukaFocusState {
    static let shared = AsukaFocusState()
    /// The windows watched, by window.
    var watches: [ObjectIdentifier: AsukaFocusWatch] = [:]
    /// The last input was a key (focus that comes now is visible).
    var lastKey = false
    /// The pointer's monitor (focus, overlays' outside presses).
    var monitor: Any?
    /// A Focus command runs: views take the focus whatever a press says.
    var forcing = false
    /// A press a Modal overlay took as it began: its release is taken too.
    var swallowing = false
    /// The automation's mouse event while it's sent to its window (its
    /// window's sendEvent: NSApp's currentEvent isn't it).
    var testEvent: NSEvent?
}

@MainActor extension AsukaRuntime {
    var focusState: AsukaFocusState { AsukaFocusState.shared }

    /// n's focus props, made as they're set.
    func focusProps(_ n: AsukaNode) -> AsukaFocusProps {
        if let f = n.focus { return f }
        let f = AsukaFocusProps()
        n.focus = f
        return f
    }

    func setFocusProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        let f = focusProps(n)
        switch p {
        case AsukaProp.focusable:
            f.focusable = v.bool
            if !v.bool, holdsFocus(n), let w = n.view?.window { w.makeFirstResponder(nil) }
        case AsukaProp.noFocusOnPress:
            f.noFocusOnPress = v.bool
        case AsukaProp.noFocusRing:
            f.noFocusRing = v.bool
            n.view?.focusRingType = v.bool ? .none : .default
            n.textView?.focusRingType = v.bool ? .none : .default
            n.view?.noteFocusRingMaskChanged()
        case AsukaProp.focusTrap:
            f.trap = v.bool
        default:
            break
        }
    }

    // MARK: Taking the focus

    /// Whether n can have the keyboard's focus: a Focusable, pressable or
    /// adjustable view, a control, a field; not disabled.
    func takesFocus(_ n: AsukaNode) -> Bool {
        guard let v = n.view, !(n.focus?.disabled ?? false) else { return false }
        if let c = v as? NSControl, !c.isEnabled { return false }
        switch n.kind {
        case .textField, .button, .toggle, .slider, .stepper, .segmented, .picker, .wheel, .colorPicker, .datePicker:
            return true
        default:
            return n.focus?.focusable == true || n.listening(.press) || n.adjustable
        }
    }

    /// Whether a primary press focuses n: it's Focusable, without
    /// NoFocusOnPress.
    func focusesOnPress(_ n: AsukaNode) -> Bool {
        guard let f = n.focus else { return false }
        return f.focusable && !f.noFocusOnPress
    }

    /// Whether a press is under way (its mouse down being handled).
    func pointerPressing() -> Bool {
        guard let e = focusState.testEvent ?? NSApp.currentEvent else { return false }
        return e.type == .leftMouseDown || e.type == .rightMouseDown || e.type == .otherMouseDown
    }

    /// An AsukaView's acceptsFirstResponder: it takes the focus; during a
    /// press, only if the press focuses it.
    func acceptsFocus(_ v: NSView) -> Bool {
        guard let n = node(of: v), n.kind != .overlay, takesFocus(n) else { return false }
        if focusState.forcing { return true }
        if pointerPressing() { return focusesOnPress(n) }
        return true
    }

    /// Whether n is a Tab stop in w: it takes the focus, shows in w, and is
    /// Focusable or a field, or Full Keyboard Access is on.
    func isTabStop(_ n: AsukaNode, _ w: NSWindow?) -> Bool {
        guard let v = n.view, w == nil || v.window === w, takesFocus(n), !v.isHiddenOrHasHiddenAncestor else { return false }
        if n.kind == .textField || n.focus?.focusable == true { return true }
        return NSApp.isFullKeyboardAccessEnabled
    }

    /// An AsukaView's canBecomeKeyView.
    func isKeyView(_ v: NSView) -> Bool {
        guard let n = node(of: v), n.kind != .overlay else { return false }
        return isTabStop(n, nil)
    }

    /// Whether n's view (a field: its editor) is its window's first
    /// responder.
    func holdsFocus(_ n: AsukaNode) -> Bool {
        guard let v = n.view, let w = v.window else { return false }
        let fr = w.firstResponder
        if fr === v || (n.textView != nil && fr === n.textView) { return true }
        if let t = fr as? NSText, t.isFieldEditor, (t.delegate as? NSView) === v { return true }
        return false
    }

    /// The Focus command: gives n's view the focus (once it's in a window:
    /// after the batch that made it).
    func focusCommand(_ n: AsukaNode, again: Bool = true) {
        guard let v = n.textView ?? n.view else { return }
        guard let w = v.window else {
            if again { afterCore { [weak self] in if let self, self.alive(n) { self.focusCommand(n, again: false) } } }
            return
        }
        if holdsFocus(n) { return }
        focusState.forcing = true
        w.makeFirstResponder(v)
        focusState.forcing = false
    }

    /// The Blur command: takes the focus from n, if it has it.
    func blurCommand(_ n: AsukaNode) {
        guard holdsFocus(n), let w = n.view?.window else { return }
        w.makeFirstResponder(nil)
    }

    // MARK: Following it

    /// Watches w's first responder (once): a view of the host's moved into
    /// it.
    func watchFocus(_ w: NSWindow?) {
        guard let w else { return }
        startPointerMonitor()
        let k = ObjectIdentifier(w)
        if let x = focusState.watches[k], x.window === w { return }
        let x = AsukaFocusWatch()
        x.window = w
        x.observation = w.observe(\.firstResponder, options: []) { w, _ in
            MainActor.assumeIsolated { AsukaRuntime.shared.firstResponderChanged(w) }
        }
        focusState.watches[k] = x
        x.focused = focusNode(w.firstResponder)
    }

    /// The node whose view is the responder r (a field's editor: its
    /// field's), if it's one of the core's.
    func focusNode(_ r: NSResponder?) -> AsukaNode? {
        var v = r as? NSView
        if let t = r as? NSText, t.isFieldEditor, let d = t.delegate as? NSView { v = d }
        guard let x = v, let n = node(of: x), alive(n) else { return nil }
        return n
    }

    /// w's first responder changed: the node that lost the focus and the
    /// one that took it report it.
    func firstResponderChanged(_ w: NSWindow) {
        guard let x = focusState.watches[ObjectIdentifier(w)], x.window === w else { return }
        let new = focusNode(w.firstResponder)
        let old = x.focused
        if old === new { return }
        x.focused = new
        if let o = old, alive(o) {
            if let f = o.focus, f.visible {
                f.visible = false
                o.view?.noteFocusRingMaskChanged()
            }
            if o.kind != .textField { send(AsukaEvents.focusChange(o.id, 0)) }
        }
        if let n = new, n.kind != .textField {
            let how: Int64 = focusState.lastKey ? 2 : 1
            if how == 2 {
                focusProps(n).visible = true
                n.view?.noteFocusRingMaskChanged()
            }
            send(AsukaEvents.focusChange(n.id, how))
        }
    }

    /// The node that has w's focus, as last reported.
    func focusedNode(_ w: NSWindow?) -> AsukaNode? {
        guard let w, let x = focusState.watches[ObjectIdentifier(w)], x.window === w else { return nil }
        return x.focused
    }

    /// Whether an AsukaView draws its focus ring: it has the focus from the
    /// keyboard, and no NoFocusRing.
    func showsRing(_ v: NSView) -> Bool {
        guard let n = node(of: v), let f = n.focus, f.visible, !f.noFocusRing else { return false }
        return v.window?.firstResponder === v
    }

    /// The pointer's local monitor (once): a press is the last input, and
    /// overlays hear of presses outside them before their targets do.
    func startPointerMonitor() {
        guard focusState.monitor == nil else { return }
        let mask: NSEvent.EventTypeMask = [.leftMouseDown, .rightMouseDown, .otherMouseDown, .leftMouseUp, .rightMouseUp, .otherMouseUp]
        focusState.monitor = NSEvent.addLocalMonitorForEvents(matching: mask) { ev in
            let took = MainActor.assumeIsolated { AsukaRuntime.shared.pointerEvent(ev) }
            return took ? nil : ev
        }
    }

    /// A press begins or ends (the monitor's, or the automation's mouse
    /// step); true: it goes no further (a Modal overlay took it).
    func pointerEvent(_ ev: NSEvent) -> Bool {
        switch ev.type {
        case .leftMouseDown, .rightMouseDown, .otherMouseDown:
            focusState.lastKey = false
            focusState.swallowing = overlaysPointerDown(ev)
            return focusState.swallowing
        default:
            let took = focusState.swallowing
            focusState.swallowing = false
            return took
        }
    }

    /// A primary press on v focuses the Focusable view it's on or in
    /// (unless it has NoFocusOnPress); one that isn't Focusable, a
    /// pressable view's, focuses nothing.
    func pressFocus(_ v: NSView) {
        focusState.lastKey = false
        var at: NSView? = v
        while let x = at {
            if let n = node(of: x), let f = n.focus {
                if f.noFocusOnPress { return }
                if f.focusable && takesFocus(n) {
                    guard let w = x.window, w.firstResponder !== x else { return }
                    focusState.forcing = true
                    w.makeFirstResponder(x)
                    focusState.forcing = false
                    return
                }
            }
            at = x.superview
        }
    }

    // MARK: Keys

    /// What a key no view's Keys took does: Tab moves the focus; Enter and
    /// Space press a focused pressable view; text goes to it as KeyText.
    /// True: it was taken.
    func keyDefault(_ ev: NSEvent, _ w: NSWindow, _ fr: NSResponder?, _ name: String, _ mods: UInt8) -> Bool {
        if name == "tab" && mods & ~2 == 0 {
            if let t = fr as? NSTextView, !t.isFieldEditor { return false } // a multiline field types it
            return tabFocus(w, fr, backward: mods & 2 != 0)
        }
        guard let n = focusNode(fr), n.kind != .textField, n.view === fr as? NSView, n.view is AsukaView else { return false }
        if mods == 0 && (name == "enter" || name == "space") && n.listening(.press) {
            emit(n, .press) { AsukaEvents.press($0) }
            return true
        }
        if n.listening(.keyText) && mods & (1 | 8) == 0, let s = ev.characters, asukaTypeable(s) {
            send(AsukaEvents.keyText(n.id, s))
            return true
        }
        return false
    }

    /// Moves w's focus to the next Tab stop (backward: the one before),
    /// in the core's order, within the focus trap it's in; false when w
    /// has no stop (AppKit's loop goes on).
    func tabFocus(_ w: NSWindow, _ fr: NSResponder?, backward: Bool) -> Bool {
        var from = fr as? NSView
        if let t = fr as? NSText, t.isFieldEditor, let d = t.delegate as? NSView { from = d }
        let cur = nodeAt(from)
        var trap: AsukaNode?
        var x = cur
        while let y = x, trap == nil {
            if y.focus?.trap == true { trap = y }
            x = y.parent ?? (y.cell?.list?.n)
        }
        var order: [(AsukaNode, Bool)] = []
        if let t = trap {
            tabWalk(t, w, &order)
        } else {
            guard let top = win(of: w)?.root ?? nodeIn(w.contentView, 3) else { return false }
            tabWalk(top, w, &order)
            for o in overlaysOver(top) { tabWalk(o, w, &order) }
        }
        let stops = order.indices.filter { order[$0].1 }
        guard !stops.isEmpty else { return false }
        let at = cur.flatMap { c in order.firstIndex { $0.0 === c } }
        var next: Int
        if let at {
            let n = order.count
            next = at
            repeat { next = ((next + (backward ? -1 : 1)) % n + n) % n } while !order[next].1 && next != at
        } else {
            next = backward ? stops.last! : stops.first!
        }
        let target = order[next].0
        focusState.lastKey = true
        if let v = target.textView ?? target.view, w.firstResponder !== v {
            focusState.forcing = true
            w.makeFirstResponder(v)
            focusState.forcing = false
        }
        return true
    }

    /// Adds n and the nodes under it that show, in the core's order, and
    /// whether each is a Tab stop in w.
    func tabWalk(_ n: AsukaNode, _ w: NSWindow, _ out: inout [(AsukaNode, Bool)]) {
        if n.dying || n.view?.isHidden == true { return }
        out.append((n, isTabStop(n, w)))
        switch n.kind {
        case .stack:
            if let t = topScreen(n) { tabWalk(t, w, &out) }
            return
        case .tabs:
            if n.selected >= 0 && n.selected < n.children.count { tabWalk(n.children[n.selected], w, &out) }
            return
        default:
            break
        }
        for c in n.children { tabWalk(c, w, &out) }
        for (_, cells) in listRows(n) { for c in cells { tabWalk(c, w, &out) } }
    }

    // MARK: Parts

    func focusDestroyed(_ n: AsukaNode) {
        for x in focusState.watches.values where x.focused === n { x.focused = nil } // its focus goes with it, unreported
        a11yDestroyed(n)
    }

    func focusJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        a11yJSON(n, &d)
        guard let v = n.view, n.focus != nil || v.window?.firstResponder === v else { return }
        var j: [String: Any] = ["first": holdsFocus(n), "acceptsFirstResponder": v.acceptsFirstResponder,
                                "canBecomeKeyView": v.canBecomeKeyView, "focusRingType": v.focusRingType.rawValue,
                                "ring": v.focusRingMaskBounds != .zero]
        if let f = n.focus {
            j["focusable"] = f.focusable
            j["noFocusOnPress"] = f.noFocusOnPress
            j["noFocusRing"] = f.noFocusRing
            j["trap"] = f.trap
            j["visible"] = f.visible
        }
        d["focus"] = j
    }

    func focusWindowJSON(_ w: AsukaWin, _ d: inout [String: Any]) {
        if let n = focusNode(w.window.firstResponder) { d["focusedNode"] = n.id }
        d["fullKeyboardAccess"] = NSApp.isFullKeyboardAccessEnabled
        // What VoiceOver sees in its content (a modal view's way, alone):
        // the nodes of the elements, -1 for one of AppKit's own.
        d["a11yChildren"] = (w.content.accessibilityChildren() ?? []).map { a11yViewOf($0).flatMap { nodeAt($0) }.map { Int($0.id) } ?? -1 }
    }
}

/// Whether s is text a key typed (not a control character or a function
/// key's private-use character).
func asukaTypeable(_ s: String) -> Bool {
    guard !s.isEmpty else { return false }
    for u in s.unicodeScalars {
        if u.value < 0x20 || u.value == 0x7f || (0xF700...0xF8FF).contains(u.value) { return false }
    }
    return true
}
