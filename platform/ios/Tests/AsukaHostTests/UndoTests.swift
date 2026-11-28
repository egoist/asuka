#if canImport(UIKit)
import XCTest
import UIKit
import CAsukaStub
@testable import AsukaHost
@testable import AsukaOps

// The app's undo stack on UIKit's undo (Undo.swift), and Cursor on
// iPadOS's pointer (Cursor.swift).
@MainActor extension HeadlessTests {
    /// UndoState's args.
    func undoState(_ flags: UInt8, _ undo: String, _ redo: String) -> [UInt8] {
        var w = AsukaWriter()
        w.u8(flags)
        w.string(undo)
        w.string(redo)
        return w.bytes
    }

    func testUndoManagerForwardsToItsState() {
        var state = AsukaUndoState()
        var done: [Int64] = []
        let m = AsukaUndoManager(state: { state }, perform: { done.append($0) })
        XCTAssertFalse(m.canUndo)
        XCTAssertFalse(m.canRedo)
        m.undo()
        m.redo()
        XCTAssertEqual(done, []) // nothing to undo or redo: nothing sent
        state.canUndo = true
        state.undoName = "Rename"
        XCTAssertTrue(m.canUndo)
        XCTAssertEqual(m.undoActionName, "Rename")
        XCTAssertTrue(m.undoMenuItemTitle.contains("Rename"))
        m.undo()
        state.canRedo = true
        state.redoName = "Delete"
        XCTAssertEqual(m.redoActionName, "Delete")
        m.redo()
        XCTAssertEqual(done, [0, 1])
    }

    func testUndoStateDecodes() {
        let b = undoState(1 | 2, "Rename", "Delete")
        let s = b.withUnsafeBytes { AsukaUndoState($0) }
        XCTAssertEqual(s.canUndo, true)
        XCTAssertEqual(s.canRedo, true)
        XCTAssertEqual(s.undoName, "Rename")
        XCTAssertEqual(s.redoName, "Delete")
        let none = undoState(0, "", "").withUnsafeBytes { AsukaUndoState($0) }
        XCTAssertEqual(none, AsukaUndoState())
    }

    /// The root controller's undo manager is the core's while the root
    /// takes Undo: UndoState says what, and undoing sends the root Undo.
    func testRootControllerUndoesTheApps() {
        let r = root()
        XCTAssertFalse(vc.undoManager === vc.appUndo) // the app doesn't use it yet
        XCTAssertFalse(vc.canBecomeFirstResponder)
        rt.listen(r, event: AsukaEventKind.undo.rawValue)
        command(r, .undoState, undoState(1, "Delete", ""))
        let m = vc.undoManager
        XCTAssertTrue(m === vc.appUndo)
        XCTAssertEqual(m?.canUndo, true)
        XCTAssertEqual(m?.canRedo, false)
        XCTAssertEqual(m?.undoActionName, "Delete")
        _ = sent()
        m?.undo()
        m?.redo() // nothing to redo
        let e = sent(.undo)
        XCTAssertEqual(e.map(\.id), [r])
        XCTAssertEqual(e.map(\.int), [0])
        command(r, .undoState, undoState(2, "", "Delete"))
        m?.redo()
        XCTAssertEqual(sent(.undo).map(\.int), [1])
        // It becomes the first responder for a shake or ⌘Z to find it.
        XCTAssertTrue(vc.canBecomeFirstResponder)
        waitUntil { vc.isFirstResponder }
        rt.unlisten(r, event: AsukaEventKind.undo.rawValue)
        vc.resignFirstResponder()
    }

    /// A text field being edited undoes its own typing.
    func testATextFieldBeingEditedKeepsItsUndo() {
        let r = root()
        rt.listen(r, event: AsukaEventKind.undo.rawValue)
        let f = make(.textField)
        rt.insert(r, child: f, index: 0)
        frame(f, CGRect(x: 0, y: 100, width: 200, height: 40))
        XCTAssertTrue(view(f).becomeFirstResponder())
        XCTAssertFalse(vc.undoManager === vc.appUndo)
        XCTAssertNotNil(vc.undoManager)
        view(f).resignFirstResponder()
        XCTAssertTrue(vc.undoManager === vc.appUndo)
        // Its editing over, the root controller takes the first responder again.
        waitUntil { vc.isFirstResponder }
        rt.unlisten(r, event: AsukaEventKind.undo.rawValue)
        vc.resignFirstResponder()
    }

    func testCursorAsAPointer() {
        XCTAssertEqual(AsukaPointer(AsukaCursor.text.rawValue), .beam)
        XCTAssertEqual(AsukaPointer(AsukaCursor.hidden.rawValue), .hidden)
        XCTAssertEqual(AsukaPointer(AsukaCursor.pointer.rawValue), .effect)
        for c in [AsukaCursor.auto, .arrow, .crosshair, .resizeHorizontal, .zoomIn, .wait] {
            XCTAssertEqual(AsukaPointer(c.rawValue), .system)
        }
        let r = root()
        let v = make(.view)
        rt.insert(r, child: v, index: 0)
        frame(v, CGRect(x: 0, y: 0, width: 100, height: 100))
        set(v, AsukaProp.cursor, .enumValue(AsukaCursor.text.rawValue))
        let pointers = view(v).interactions.filter { $0 is UIPointerInteraction }
        if UIDevice.current.userInterfaceIdiom == .pad {
            XCTAssertEqual(pointers.count, 1)
            let d = rt.nodes[v]!.pointerDelegate!
            let i = pointers[0] as! UIPointerInteraction
            let region = UIPointerRegion(rect: view(v).bounds)
            XCTAssertNotNil(d.pointerInteraction(i, styleFor: region))
            set(v, AsukaProp.cursor, .enumValue(AsukaCursor.hidden.rawValue))
            XCTAssertNotNil(d.pointerInteraction(i, styleFor: region))
            set(v, AsukaProp.cursor, .enumValue(AsukaCursor.auto.rawValue))
            XCTAssertTrue(view(v).interactions.filter { $0 is UIPointerInteraction }.isEmpty)
        } else {
            XCTAssertTrue(pointers.isEmpty) // an iPhone has no pointer
        }
    }
}
#endif
