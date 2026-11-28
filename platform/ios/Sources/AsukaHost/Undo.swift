#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// The app's undo stack (the core's: ui's undo) on UIKit's undo: the root
// controller's undoManager is one that asks the core's UndoState what
// there is to undo and what it's called, and sends the root Undo to undo
// or redo it. UIKit finds it up the responder chain from the first
// responder: shaking the phone (UIApplication's
// applicationSupportsShakeToEdit), the three-finger gestures and the
// edit HUD, and ⌘Z and ⇧⌘Z (an iPad's keyboard, its menu bar), with
// "Undo Rename" in the system's own words. So the root controller is the
// first responder while nothing else is (no text field being edited);
// a text field being edited keeps its typing's undo (the window's undo
// manager, which UIKit gives it up the chain).

/// UndoState's: what there is to undo and redo, and what it's called.
struct AsukaUndoState: Equatable {
    var canUndo = false
    var canRedo = false
    var undoName = ""
    var redoName = ""

    /// UndoState's args: u8 flags (1 undo, 2 redo), u32-length undo
    /// name, u32-length redo name.
    init(_ args: UnsafeRawBufferPointer) {
        guard args.count >= 1 else { return }
        var r = AsukaReader(args)
        let flags = r.u8()
        canUndo = flags & 1 != 0
        canRedo = flags & 2 != 0
        if args.count >= 5 { undoName = r.string() }
        if !r.atEnd { redoName = r.string() }
    }

    init() {}
}

/// An undo manager that is the core's undo stack: it registers nothing
/// itself, and undoing or redoing sends the root Undo.
final class AsukaUndoManager: UndoManager {
    /// The state UndoState last sent.
    var state: () -> AsukaUndoState
    /// Undo (0) or redo (1).
    var perform: (Int64) -> Void

    init(state: @escaping () -> AsukaUndoState, perform: @escaping (Int64) -> Void) {
        self.state = state
        self.perform = perform
        super.init()
        groupsByEvent = false // the core groups its own changes
    }

    override var canUndo: Bool { state().canUndo }
    override var canRedo: Bool { state().canRedo }
    override var undoActionName: String { state().undoName }
    override var redoActionName: String { state().redoName }
    override var isUndoing: Bool { false }
    override var isRedoing: Bool { false }

    override func undo() {
        guard canUndo else { return }
        perform(0)
    }

    override func redo() {
        guard canRedo else { return }
        perform(1)
    }

    override func undoNestedGroup() { undo() }
}

/// The view in v's tree that is the first responder, if one is.
@MainActor func asukaFirstResponder(in v: UIView) -> UIView? {
    if v.isFirstResponder { return v }
    for s in v.subviews {
        if let f = asukaFirstResponder(in: s) { return f }
    }
    return nil
}

extension AsukaRuntime {
    /// Whether the root takes Undo: the app uses its undo stack.
    var rootUndoes: Bool { nodes[rootID]?.listens.contains(AsukaEventKind.undo.rawValue) ?? false }

    /// The UndoState command.
    func setUndoState(_ args: UnsafeRawBufferPointer) {
        undoState = AsukaUndoState(args)
        controller?.takeUndoResponder()
    }

    /// The user undid (0) or redid (1) through UIKit's undo.
    func undo(_ how: Int64) {
        let id = rootID
        emit(id, .undo) { AsukaEvents.undo(id, how) }
    }
}

extension AsukaViewController {
    /// Whether a text field or text view is being edited: it undoes its
    /// own typing.
    var editingText: Bool { textBeingEdited?.isFirstResponder ?? false }

    /// Becomes the first responder when nothing is, so UIKit's undo (a
    /// shake, ⌘Z) finds the app's undo manager; a field being edited,
    /// or anything else that is, stays so. After this turn: the root and
    /// its Listen arrive in a batch before the view is in a window, and a
    /// field that stops being edited may hand the first responder on.
    func takeUndoResponder() {
        guard !undoResponderPending else { return }
        undoResponderPending = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.undoResponderPending = false
            guard AsukaRuntime.shared.rootUndoes, let w = self.viewIfLoaded?.window, !self.isFirstResponder, !self.editingText else { return }
            // Nothing else is: in the window, or a controller presented
            // over this one (a sheet's).
            if asukaFirstResponder(in: w) != nil { return }
            var top = self.presentedViewController
            while let p = top {
                if let v = p.viewIfLoaded, asukaFirstResponder(in: v) != nil { return }
                top = p.presentedViewController
            }
            _ = self.becomeFirstResponder()
        }
    }

    @objc func textEditingBegan(_ note: Notification) { textBeingEdited = note.object as? UIResponder }

    /// A text field or text view stopped being edited: the first responder
    /// may be no one's then, and the app's undo comes back.
    @objc func textEditingEnded(_ note: Notification) {
        if let r = note.object as? UIResponder, r === textBeingEdited { textBeingEdited = nil }
        takeUndoResponder()
    }

    func observeTextEditing() {
        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(textEditingBegan(_:)), name: UITextField.textDidBeginEditingNotification, object: nil)
        nc.addObserver(self, selector: #selector(textEditingBegan(_:)), name: UITextView.textDidBeginEditingNotification, object: nil)
        nc.addObserver(self, selector: #selector(textEditingEnded(_:)), name: UITextField.textDidEndEditingNotification, object: nil)
        nc.addObserver(self, selector: #selector(textEditingEnded(_:)), name: UITextView.textDidEndEditingNotification, object: nil)
    }
}
#endif
