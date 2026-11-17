// Keys on a Mac (the Keys prop).
//
// A local event monitor for key down, installed once, sees each key going
// down (and each repeat) as the application takes it from its queue, before
// NSApplication's sendEvent: hands it to the menu bar's key equivalents
// (performKeyEquivalent:) and the key window's first responder (a field
// editor, cancelOperation:, insertNewline:). So it serves every window the
// same way, whatever its class: the main window, a Window's, a sheet's, a
// popover's, a flyout's or a panel's (a nonactivating panel takes the
// keyboard without the app being active, and its keys still come through
// the app's queue). An NSWindow sendEvent: override would come after the
// menu bar's key equivalents, and only in the windows the host subclasses,
// not a popover's.
//
// The key starts at the first responder's node (a field editor's: its
// field's; a view the host doesn't know: its nearest superview that it
// does), else, when the window itself or nothing of the host's has the
// focus, at the window's root node (the root, a Window's, a sheet's or a
// popover's content). It goes out through the nodes' parents, as the core
// has them, and on from a node with none where it shows (keyOut): a list's
// cell to its list, a sheet's node to the window it's declared in and a
// popover's to its anchor's window (a sheet's node, for a popover in a
// sheet), ending at that window's root (the root, or a Window node: another
// window's keys are its own). The first node whose Keys have it takes it:
// the event goes no further and the node gets Key. While an input method
// composes text (the first responder has marked text), its keys are its
// own.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// Watches the keyboard: views' Keys, the app's shortcuts.
@MainActor final class AsukaKeyWatcher {
    static let shared = AsukaKeyWatcher()
    private var monitor: Any?

    /// Installs the key monitor (once), and the text views' selection
    /// observer, which the fields' keys move.
    func watch() {
        AsukaTextDelegate.shared.start()
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { ev in
            let took = MainActor.assumeIsolated { AsukaRuntime.shared.keyDown(ev) }
            return took ? nil : ev
        }
    }
}

/// A key event's name, as the Keys prop has it (a named key, or the
/// character the key types without modifiers), and its modifiers (1 ⌘, 2
/// shift, 4 option, 8 control).
func asukaKeyOf(_ ev: NSEvent) -> (String, UInt8) {
    let f = ev.modifierFlags
    var mods: UInt8 = 0
    if f.contains(.command) { mods |= 1 }
    if f.contains(.shift) { mods |= 2 }
    if f.contains(.option) { mods |= 4 }
    if f.contains(.control) { mods |= 8 }
    if let name = asukaKeyCodeNames[ev.keyCode] { return (name, mods) }
    // The character without modifiers: Shift is a modifier (⇧1 is 1 with
    // shift, not !), as option and control are.
    let c: String?
    if #available(macOS 10.15, *) { c = ev.characters(byApplyingModifiers: []) } else { c = ev.charactersIgnoringModifiers }
    guard let c, c.unicodeScalars.count == 1 else { return ("", 0) } // a dead key, or none
    return (AsukaKeys.keyName(c), mods)
}

/// The named keys, by their virtual key codes.
let asukaKeyCodeNames: [UInt16: String] = [
    0x24: "enter", 0x4c: "enter", // Return, the keypad's Enter
    0x35: "escape", 0x30: "tab", 0x31: "space",
    0x75: "delete", 0x33: "backspace", // ⌦, ⌫
    0x7e: "up", 0x7d: "down", 0x7b: "left", 0x7c: "right",
    0x73: "home", 0x77: "end", 0x74: "pageup", 0x79: "pagedown",
    0x7a: "f1", 0x78: "f2", 0x63: "f3", 0x76: "f4", 0x60: "f5", 0x61: "f6",
    0x62: "f7", 0x64: "f8", 0x65: "f9", 0x6d: "f10", 0x67: "f11", 0x6f: "f12",
]

@MainActor extension AsukaRuntime {
    /// n's Keys prop changed (n.keys, decoded): the monitor reads them as
    /// keys go down.
    func keysChanged(_ n: AsukaNode) {}

    /// Routes a key going down; true: a node took it.
    func keyDown(_ ev: NSEvent) -> Bool {
        guard let w = ev.window ?? NSApp.keyWindow else { return false }
        let fr = w.firstResponder
        if let t = fr as? NSTextInputClient, t.hasMarkedText() { return false } // an input method's
        guard var n = keyStart(w, fr) else { return false }
        let (name, mods) = asukaKeyOf(ev)
        guard !name.isEmpty else { return false }
        focusState.lastKey = true // focus that comes now is visible (Focus.swift)
        // A cycle can't be (the core's tree has none), but a node is never
        // asked twice.
        var seen = Set<ObjectIdentifier>()
        while seen.insert(ObjectIdentifier(n)).inserted {
            if !n.keys.isEmpty {
                let i = AsukaKeys.match(n.keys, name, mods)
                if i >= 0 {
                    emit(n, .key) { AsukaEvents.key($0, Int64(i)) }
                    return true
                }
            }
            guard let next = keyOut(n) else { break }
            n = next
        }
        // Tab, Enter and Space on a focused view, its typeahead (Focus.swift).
        return keyDefault(ev, w, fr, name, mods)
    }

    /// Where a key goes on from n, not taking it: n's parent; for a node the
    /// core leaves without one, where it shows: a list's cell its list, a
    /// sheet's node the window it's declared in (its Window prop: the root,
    /// a Window node), a popover's node the root of what its anchor is in (a
    /// window's, or a sheet's node). The root and a Window node are ends
    /// (nil).
    func keyOut(_ n: AsukaNode) -> AsukaNode? {
        if let p = n.parent { return p }
        if let c = n.cell {
            if let l = c.list?.n, alive(l) { return l }
            return nil
        }
        switch n.kind {
        case .sheet:
            return onNode(n)
        case .popover:
            var a = n.popover.flatMap { nodes[$0.anchor] }
            while let x = a, x.parent != nil || x.cell != nil {
                a = x.parent ?? x.cell?.list?.n
            }
            if let a, a !== n { return a }
            return main?.root
        default:
            return nil
        }
    }

    /// The node a key in window w starts at, fr its first responder: fr's,
    /// or w's root (nil: w isn't the app's).
    func keyStart(_ w: NSWindow, _ fr: NSResponder?) -> AsukaNode? {
        var v: NSView? = fr as? NSView
        if let t = fr as? NSText, t.isFieldEditor, let d = t.delegate as? NSView { v = d } // a field being edited
        while let x = v {
            if let n = node(of: x), alive(n) { return n }
            v = x.superview
        }
        // The window, or nothing of the host's, has the focus.
        if let x = win(of: w) { return x.root }
        // A sheet's or a popover's window: its content's node.
        return nodeIn(w.contentView, 3)
    }

    /// The node of v or of the first view under v, depth levels down at
    /// most, the host knows.
    func nodeIn(_ v: NSView?, _ depth: Int) -> AsukaNode? {
        guard let v else { return nil }
        if let n = node(of: v), alive(n) { return n }
        guard depth > 0 else { return nil }
        for c in v.subviews {
            if let n = nodeIn(c, depth - 1) { return n }
        }
        return nil
    }
}
