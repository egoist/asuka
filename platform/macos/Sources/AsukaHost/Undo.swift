// Undo: the app's undo stack (the core's) behind the Edit menu's Undo and
// Redo, ⌘Z and ⇧⌘Z.
//
// Each of the app's windows answers windowWillReturnUndoManager with an
// undo manager of its own, which AppKit's Edit > Undo (undo:, NSWindow's)
// asks what to undo and how to say it: while a text view in the window is
// being edited it's an ordinary NSUndoManager, which the text view keeps
// its typing in (so a field undoes its own); otherwise it's the app's, as
// UndoState says (whether there's something to undo or redo, and its name:
// "Undo Delete", in the language shown), undoing and redoing with the Undo
// event to the root. The MenuBar's items of those roles are the host's own
// Edit items, so they're left out of the app's menus (MenuBar.swift).
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// The key a window's AsukaUndoManager is kept under.
@MainActor private var asukaUndoKey: UInt8 = 0

/// The app's undo stack as UndoState last said it.
@MainActor final class AsukaUndo {
    static let shared = AsukaUndo()
    var flags: UInt8 = 0
    var undoName = ""
    var redoName = ""
    /// A window's undo manager, made the first time it's asked for and
    /// kept with the window (it goes with it).
    func manager(_ w: NSWindow) -> AsukaUndoManager {
        if let m = objc_getAssociatedObject(w, &asukaUndoKey) as? AsukaUndoManager { return m }
        let m = AsukaUndoManager()
        m.window = w
        objc_setAssociatedObject(w, &asukaUndoKey, m, .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
        return m
    }

    /// The UndoState command: u8 flags (1 undo, 2 redo), the u32-length
    /// undo and redo names.
    func setState(_ b: [UInt8]) {
        var r = AsukaByteReader(b)
        flags = r.u8()
        undoName = r.string()
        redoName = r.string()
        if r.bad { undoName = ""; redoName = "" }
        NSApp.mainMenu?.update() // the Edit menu says so now, open or not
    }

    /// "Undo <name>" (redo: "Redo <name>") in the language the app shows,
    /// as Foundation says it; "Undo" without a name.
    func title(_ redo: Bool, _ name: String) -> String {
        let word = redo ? "Redo" : "Undo"
        if name.isEmpty { return AsukaShell.shared.menuWord(word) }
        let english = word + " " + name
        let locale = AsukaRuntime.shared.locale
        if let p = asukaPseudo(english, locale) { return p }
        if !locale.isEmpty, let f = Bundle(identifier: "com.apple.Foundation") {
            let sel = NSSelectorFromString("localizedStringForKey:value:table:localizations:")
            if f.responds(to: sel), let imp = f.method(for: sel) {
                typealias F = @convention(c) (AnyObject, Selector, NSString, NSString, NSString, NSArray) -> NSString?
                let fn = unsafeBitCast(imp, to: F.self)
                let key = word + " %@"
                if let s = fn(f, sel, key as NSString, key as NSString, "Undo" as NSString, [locale] as NSArray) as String?, s.contains("%@") {
                    return s.replacingOccurrences(of: "%@", with: name)
                }
            }
            return english
        }
        let m = UndoManager()
        return redo ? m.redoMenuTitle(forUndoActionName: name) : m.undoMenuTitle(forUndoActionName: name)
    }
}

/// A window's undo manager: a text view's being edited, or the app's.
final class AsukaUndoManager: UndoManager {
    weak var window: NSWindow?

    /// Undo is the app's: no text view in the window is being edited, and
    /// the app uses its undo stack (the root hears Undo).
    private var app: Bool {
        MainActor.assumeIsolated {
            if let tv = window?.firstResponder as? NSTextView, tv.isEditable { return false }
            return AsukaRuntime.shared.root?.listening(.undo) ?? false
        }
    }

    override var canUndo: Bool { app ? MainActor.assumeIsolated { AsukaUndo.shared.flags & 1 != 0 } : super.canUndo }
    override var canRedo: Bool { app ? MainActor.assumeIsolated { AsukaUndo.shared.flags & 2 != 0 } : super.canRedo }
    override var undoActionName: String { app ? MainActor.assumeIsolated { AsukaUndo.shared.undoName } : super.undoActionName }
    override var redoActionName: String { app ? MainActor.assumeIsolated { AsukaUndo.shared.redoName } : super.redoActionName }
    override var undoMenuItemTitle: String {
        app ? MainActor.assumeIsolated { AsukaUndo.shared.title(false, AsukaUndo.shared.undoName) } : super.undoMenuItemTitle
    }
    override var redoMenuItemTitle: String {
        app ? MainActor.assumeIsolated { AsukaUndo.shared.title(true, AsukaUndo.shared.redoName) } : super.redoMenuItemTitle
    }
    override func undo() {
        if app { send(0) } else { super.undo() }
    }
    override func redo() {
        if app { send(1) } else { super.redo() }
    }

    private func send(_ v: Int64) {
        MainActor.assumeIsolated {
            let rt = AsukaRuntime.shared
            guard let r = rt.root else { return }
            rt.emit(r, .undo) { AsukaEvents.undo($0, v) }
        }
    }
}

/// Edit > Undo and Redo where no window is key (the app in the background,
/// a script driving it): the window in use's, as NSWindow's undo: and
/// redo: would.
extension AsukaAppDelegate: NSMenuItemValidation {
    @objc func undo(_ sender: Any?) { AsukaRuntime.shared.keyWin.window.undoManager?.undo() }
    @objc func redo(_ sender: Any?) { AsukaRuntime.shared.keyWin.window.undoManager?.redo() }

    func validateMenuItem(_ mi: NSMenuItem) -> Bool {
        guard mi.action == #selector(undo(_:)) || mi.action == #selector(redo(_:)) else { return true }
        guard let m = AsukaRuntime.shared.keyWin.window.undoManager else { return false }
        if mi.action == #selector(undo(_:)) {
            mi.title = m.undoMenuItemTitle
            return m.canUndo
        }
        mi.title = m.redoMenuItemTitle
        return m.canRedo
    }
}

/// windowWillReturnUndoManager(_:): each window's own.
extension AsukaWin {
    @objc(windowWillReturnUndoManager:) func windowWillReturnUndoManager(_ window: NSWindow) -> UndoManager? {
        AsukaUndo.shared.manager(window)
    }
}

@MainActor extension AsukaRuntime {
    func undoState(_ b: [UInt8]) {
        AsukaUndo.shared.setState(b)
    }

    func undoJSON(_ out: inout [String: Any]) {
        let u = AsukaUndo.shared
        var j: [String: Any] = ["flags": Int(u.flags), "undoName": u.undoName, "redoName": u.redoName]
        let m = u.manager(keyWin.window)
        j["canUndo"] = m.canUndo
        j["canRedo"] = m.canRedo
        j["undoTitle"] = m.undoMenuItemTitle
        j["redoTitle"] = m.redoMenuItemTitle
        j["windowManager"] = keyWin.window.undoManager === m
        out["undo"] = j
    }
}
