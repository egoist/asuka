#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// The keyboard's focus (RFC 0010): what the core follows as focused and
// focusVisible, reported as FocusChange for every view but a TextField
// (which reports Focus and Blur).
//
// On iOS and iPadOS the keyboard's focus is UIKit's focus system, there
// while a hardware keyboard drives the app (Tab, the arrows, Full
// Keyboard Access). A tap never moves it. A focusable, pressable or
// adjustable View can become focused (AsukaContainerView), as UIKit's
// controls and fields can; what UIKit focuses is reported, 2 for a move
// the keyboard made. The core's Focus moves UIKit's focus there when it
// has one; without one (a phone with no keyboard) the focus is the
// core's alone, reported as if UIKit had moved it, so the core's states
// and keys follow it all the same. Focus right after a key is visible
// (2), else 1: the web's :focus-visible.
//
// FocusTrap: Tab and Shift-Tab cycle among the focusable views inside
// the trap the focus is in (key commands ahead of UIKit's own Tab, which
// moves between focus groups), and the focus system's arrows never leave
// it (shouldUpdateFocus). Typed text no Keys took goes to the focused
// view as KeyText; Return and Space press a focused view that presses.

@MainActor extension AsukaRuntime {
    /// Whether the last input was a key (as opposed to a touch, a pointer's
    /// press or a pen): what makes the core's Focus visible.
    func noteKey() { keyboardModality = true }
    func notePointer() { keyboardModality = false }

    /// The node with the keyboard's focus as last reported, other than a
    /// text field.
    var focusedNode: AsukaNode? {
        guard focusedID != 0, let n = nodes[focusedID] else { return nil }
        return n
    }

    /// Reports the focus moved to id (0: nowhere), how it came (1 a press
    /// or the program, 2 the keyboard): FocusChange 0 to the view that had
    /// it, how to the one that has it. A text field reports its own Focus
    /// and Blur; the focus going to one only forgets the view that had it,
    /// which the core takes it from as the field reports Focus.
    func focusMoved(to id: UInt32, how: Int64) {
        let to = id == 0 ? nil : nodes[id]
        if let to, to.kind == .textField {
            focusedID = 0
            return
        }
        if focusedID == id { return }
        let old = focusedID
        focusedID = id
        if old != 0, nodes[old] != nil { send(AsukaEvents.focusChange(old, 0)) }
        if id != 0, to != nil { send(AsukaEvents.focusChange(id, how)) }
    }

    /// The focus system moved its focus (a container's, a screen's or the
    /// root's didUpdateFocus; each update is seen once).
    func focusSystemMoved(_ context: UIFocusUpdateContext) {
        let key = ObjectIdentifier(context)
        if key == lastFocusUpdate { return }
        lastFocusUpdate = key
        let next = context.nextFocusedView.flatMap { focusNode(of: $0) }
        let how: Int64 = programmaticFocus ?? 2
        if let next {
            focusMoved(to: next.id, how: how)
        } else if let prev = context.previouslyFocusedView.flatMap({ focusNode(of: $0) }), prev.id == focusedID {
            focusMoved(to: 0, how: 0)
        }
    }

    /// The node whose view v is (a focus system's focused item: the view
    /// itself, or a control's own subview).
    func focusNode(of v: UIView) -> AsukaNode? {
        var x: UIView? = v
        while let cur = x {
            if cur.tag > 0, let n = nodes[UInt32(truncatingIfNeeded: cur.tag)], n.view === cur { return n }
            if cur is AsukaContainerView { return nil } // another of the app's views, not one the core made
            x = cur.superview
        }
        return nil
    }

    /// The Focus command: the keyboard's focus to n.
    func focus(_ n: AsukaNode) {
        if n.kind == .textField {
            n.view.becomeFirstResponder()
            return
        }
        let how: Int64 = keyboardModality ? 2 : 1
        // A field had the keyboard: the focus leaves it, as on a desktop.
        if let f = nodes.values.first(where: { $0.kind == .textField && $0.view.isFirstResponder }) {
            f.view.resignFirstResponder()
        }
        if n.view.window != nil, let fs = UIFocusSystem.focusSystem(for: n.view) {
            programmaticFocus = how
            fs.requestFocusUpdate(to: n.view)
            fs.updateFocusIfNeeded()
            programmaticFocus = nil
            if focusedID == n.id { return }
        }
        // No focus system (no keyboard), or it wouldn't: the core's alone.
        focusMoved(to: n.id, how: how)
    }

    /// The Blur command: the keyboard's focus leaves n.
    func blur(_ n: AsukaNode) {
        if n.kind == .textField {
            n.view.resignFirstResponder()
            return
        }
        if let c = n.view as? AsukaContainerView, n.view.window != nil, let fs = UIFocusSystem.focusSystem(for: c),
           fs.focusedItem === c {
            c.refusesFocus = true
            programmaticFocus = 1
            c.setNeedsFocusUpdate()
            c.updateFocusIfNeeded()
            programmaticFocus = nil
            c.refusesFocus = false
        }
        if focusedID == n.id { focusMoved(to: 0, how: 0) }
    }

    /// Whether n takes the keyboard's focus, as the core says (focus.c's
    /// takes_focus): a control or a field, enabled; a focusable, pressable
    /// or adjustable View.
    func takesFocus(_ n: AsukaNode) -> Bool {
        guard !n.view.isHidden else { return false }
        switch n.kind {
        case .textField, .button, .toggle, .slider, .stepper, .segmented, .picker, .wheel, .colorPicker, .datePicker, .chip, .dateRangePicker, .disclosure:
            return (n.view as? UIControl)?.isEnabled ?? true
        default:
            guard let c = n.view as? AsukaContainerView else { return false }
            return c.takesFocus
        }
    }

    /// The views inside n that take the focus, in the tree's order (what
    /// shows: none under a hidden view).
    func tabOrder(_ n: AsukaNode, into out: inout [AsukaNode]) {
        if n.view.isHidden { return }
        if takesFocus(n) { out.append(n) }
        for c in n.children { tabOrder(c, into: &out) }
    }

    /// The innermost FocusTrap around the node with the focus.
    func trap(around n: AsukaNode?) -> AsukaNode? {
        var p = n
        while let x = p {
            if (x.view as? AsukaContainerView)?.focusTrap == true { return x }
            p = x.parent
        }
        return nil
    }

    /// The node with the focus: a field being edited, UIKit's focused
    /// view, or the core's focus.
    var currentFocus: AsukaNode? {
        if let f = nodes.values.first(where: { $0.kind == .textField && $0.view.isFirstResponder }) { return f }
        return focusedNode
    }

    /// Tab or Shift-Tab inside a FocusTrap: on to the next (previous)
    /// focusable view in it, round from the last to the first.
    func tabInTrap(backward: Bool) {
        noteKey()
        let cur = currentFocus
        guard let t = trap(around: cur) else { return }
        var order: [AsukaNode] = []
        tabOrder(t, into: &order)
        guard !order.isEmpty else { return }
        let at = cur.flatMap { c in order.firstIndex { $0 === c } }
        let n = order.count
        let next = at.map { ((($0 + (backward ? -1 : 1)) % n) + n) % n } ?? (backward ? n - 1 : 0)
        focus(order[next])
    }

    /// The Tab and Shift-Tab key commands of a FocusTrap, while the focus
    /// is in one.
    func trapCommands() -> [UIKeyCommand] {
        #if os(tvOS)
        // A remote has no Tab: the trap's focus guide keeps the focus in.
        return []
        #else
        guard trap(around: currentFocus) != nil else { return [] }
        return [false, true].map { back in
            let c = UIKeyCommand(title: "", action: #selector(AsukaViewController.asukaTab(_:)), input: "\t",
                                 modifierFlags: back ? .shift : [], propertyList: back ? 1 : 0)
            c.wantsPriorityOverSystemBehavior = true
            return c
        }
        #endif
    }

    /// A key went down with the focus on a view of the app's, which none
    /// of the Keys took: Return or Space press a view that presses (as a
    /// button's own press), other text is the view's KeyText (a listbox's
    /// typeahead). true: it was taken.
    func focusedKey(_ presses: Set<UIPress>) -> Bool {
        guard let press = presses.first, let key = press.key else { return false }
        let p = ObjectIdentifier(press)
        if handledPresses.contains(p) { return true }
        guard focusedKey(key.keyCode, key.characters, key.modifierFlags) else { return false }
        // Taken: a responder further on doesn't take it again.
        handledPresses.insert(p)
        DispatchQueue.main.async { [weak self] in self?.handledPresses.remove(p) }
        return true
    }

    /// A key, its text and its modifiers, with the focus on a view.
    func focusedKey(_ code: UIKeyboardHIDUsage, _ text: String, _ modifiers: UIKeyModifierFlags) -> Bool {
        noteKey()
        guard let n = focusedNode, n.kind != .textField else { return false }
        let mods = modifiers.subtracting([.shift, .alphaShift, .numericPad])
        guard mods.isEmpty else { return false }
        let id = n.id
        let isPress = code == .keyboardReturnOrEnter || code == .keypadEnter || code == .keyboardSpacebar
        if isPress, !modifiers.contains(.shift), n.listens.contains(AsukaEventKind.press.rawValue) {
            send(AsukaEvents.press(id))
            return true
        }
        guard text.unicodeScalars.count == 1, let s = text.unicodeScalars.first,
              s.value >= 0x20, s.value != 0x7f, !(0xF700...0xF8FF).contains(s.value),
              n.listens.contains(AsukaEventKind.keyText.rawValue) else { return false }
        send(AsukaEvents.keyText(id, text))
        return true
    }

    /// NoFocusRing: no focus effect of UIKit's (the app draws its own);
    /// else the view's own again (the system's).
    func setNoFocusRing(_ n: AsukaNode, _ on: Bool) {
        if on, n.ownFocusEffect == nil { n.ownFocusEffect = .some(n.view.focusEffect) }
        n.noFocusRing = on
        n.view.focusEffect = on ? nil : (n.ownFocusEffect ?? n.view.focusEffect)
    }

    /// FocusTrap: the view is a focus group of its own, which the focus
    /// system's arrows never leave, and Tab cycles inside it.
    func setFocusTrap(_ n: AsukaNode, _ on: Bool) {
        guard let c = n.view as? AsukaContainerView else { return }
        c.focusTrap = on
        #if !os(tvOS)
        c.focusGroupIdentifier = on ? "dev.asuka.trap.\(n.id)" : nil
        #endif
    }
}

#endif
