import AppKit
import XCTest
@testable import AsukaHost

// asuka dev's error overlay (DevError, DevErrorClear), driven as the core
// drives it, the events the host sends kept (navSentEvents: NavTests's
// stand-in for asuka_event). The player shows it end to end in
// cmd/asuka's TestDevDesktopErrorOverlay.

@MainActor
final class DevErrorTests: XCTestCase {
    private var rt: AsukaRuntime { AsukaRuntime.shared }
    private var overlay: AsukaDevErrors { AsukaDevErrors.shared }

    override func setUp() async throws {
        await MainActor.run {
            _ = NSApplication.shared
            let rt = AsukaRuntime.shared
            if rt.main == nil { rt.makeMainWindow() }
            rt.started = true
            rt.main.window.orderFront(nil)
            navSentEvents = []
        }
    }

    /// ops.AppendDevError: u8 flags, then four u32-length strings.
    private func args(_ flags: UInt8, _ title: String, _ message: String, _ source: String, _ details: String) -> [UInt8] {
        var b: [UInt8] = [flags]
        for s in [title, message, source, details] {
            let u = Array(s.utf8)
            for i in 0..<4 { b.append(UInt8(truncatingIfNeeded: u.count >> (8 * i))) }
            b += u
        }
        return b
    }

    private func command(_ id: UInt32, _ c: AsukaCommand, _ b: [UInt8] = []) {
        b.withUnsafeBytes { rt.command(id, command: c.rawValue, args: $0) }
    }

    func testDecode() {
        let b = args(3, "Build failed", "app.asuka:6:32: no", "> 6 | x\n    | ^", "")
        let e = b.withUnsafeBytes { AsukaDevError.decode($0) }
        XCTAssertEqual(e, AsukaDevError(flags: 3, title: "Build failed", message: "app.asuka:6:32: no", source: "> 6 | x\n    | ^", details: ""))
        XCTAssertTrue(e?.reload ?? false)
        XCTAssertTrue(e?.build ?? false)
        // Cut short: nothing.
        XCTAssertNil(b.dropLast(5).withUnsafeBytes { AsukaDevError.decode($0) })
        XCTAssertNil([UInt8]().withUnsafeBytes { AsukaDevError.decode($0) })
    }

    // An app's error: a child window over the main window's content, its
    // texts; Reload (the default button) and Dismiss (Escape) send
    // DevErrorAction to the root and leave it to the core; another
    // DevError replaces what it shows in place; DevErrorClear removes it.
    func testOverlay() throws {
        let root: UInt32 = 900
        rt.create(root, kind: AsukaKind.view.rawValue)
        defer { rt.destroy(root) }
        let w = rt.main.window
        command(root, .devError, args(1, "Uncaught error", "boom at 1", "> 12 |   throw Error(\"boom\")", "explode (app.asuka:12)"))
        let p = try XCTUnwrap(overlay.panel)
        XCTAssertTrue(overlay.showing)
        XCTAssertTrue(p.parent === w, "not the main window's child")
        XCTAssertTrue(p.worksWhenModal)
        XCTAssertEqual(p.frame, w.convertToScreen(w.contentLayoutRect), "not over the window's content")
        XCTAssertEqual(overlay.buttons.map(\.title), ["Dismiss", "Reload"])
        XCTAssertEqual(overlay.buttons[1].keyEquivalent, "\r", "Reload isn't the default button")
        let texts = overlay.texts()
        XCTAssertEqual(texts, ["Uncaught error", "boom at 1", "> 12 |   throw Error(\"boom\")", "explode (app.asuka:12)", "Dismiss", "Reload"])
        let j = try XCTUnwrap(overlay.json())
        XCTAssertEqual(j["titleColor"] as? String, colorHex(.systemRed))
        // It's in the automation's dump, and its buttons tap.
        XCTAssertTrue(AsukaAutomation.shared.dump().contains("deverror \"Uncaught error\""))
        XCTAssertTrue(AsukaAutomation.shared.screenTexts().contains("boom at 1"))

        XCTAssertNil(AsukaAutomation.shared.tap("Reload"))
        XCTAssertEqual(navSentEvents, [AsukaEvents.devErrorAction(root, Int64(AsukaDevErrorAction.reload.rawValue))])
        XCTAssertTrue(overlay.showing, "it went without the core")
        // Escape, whatever has the focus: Dismiss.
        navSentEvents = []
        let esc = try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0, windowNumber: p.windowNumber,
                                                 context: nil, characters: "\u{1b}", charactersIgnoringModifiers: "\u{1b}", isARepeat: false, keyCode: 0x35))
        p.sendEvent(esc)
        XCTAssertEqual(navSentEvents, [AsukaEvents.devErrorAction(root, Int64(AsukaDevErrorAction.dismiss.rawValue))])

        // A build that failed replaces it, in the same window: orange, no
        // Reload (Return dismisses), no details.
        command(root, .devError, args(2, "Build failed", "app.asuka:6:32: += doesn't take an int and a string", "> 6 | n += \"x\"", ""))
        XCTAssertTrue(overlay.panel === p, "not replaced in place")
        XCTAssertEqual(overlay.buttons.map(\.title), ["Dismiss"])
        XCTAssertEqual(overlay.buttons[0].keyEquivalent, "\r")
        XCTAssertEqual(overlay.json()?["titleColor"] as? String, colorHex(.systemOrange))
        XCTAssertFalse(overlay.texts().contains("explode (app.asuka:12)"))
        navSentEvents = []
        overlay.act(.reload) // not offered: nothing
        XCTAssertTrue(navSentEvents.isEmpty)

        // It follows the window.
        let size = w.frame.size
        w.setContentSize(CGSize(width: 640, height: 480))
        XCTAssertEqual(p.frame, w.convertToScreen(w.contentLayoutRect), "didn't follow the window")
        w.setFrame(CGRect(origin: w.frame.origin, size: size), display: false)

        command(root, .devErrorClear)
        XCTAssertNil(overlay.panel)
        XCTAssertFalse(overlay.showing)
        XCTAssertFalse(p.isVisible)
        XCTAssertNil(p.parent)
        XCTAssertTrue(overlay.texts().isEmpty)
        XCTAssertFalse(AsukaAutomation.shared.dump().contains("deverror"))
        XCTAssertNotNil(AsukaAutomation.shared.tap("Dismiss"), "a button that's gone tapped")
    }

    // Over the window's sheet: the sheet's child, ordered in front of it,
    // its buttons working while the sheet holds the window; back on the
    // window when the sheet ends, and on the next sheet as it begins.
    func testOverSheet() throws {
        let root: UInt32 = 901
        rt.create(root, kind: AsukaKind.view.rawValue)
        defer { rt.destroy(root) }
        let w = rt.main.window
        func makeSheet() -> NSWindow {
            let s = NSWindow(contentRect: CGRect(x: 0, y: 0, width: 300, height: 200), styleMask: [.titled], backing: .buffered, defer: false)
            s.isReleasedWhenClosed = false
            return s
        }
        defer { if let s = w.attachedSheet { w.endSheet(s) } }
        /// Whether p is front of s, as the window server orders the app's
        /// windows (orderedWindows has no panels).
        func front(_ p: NSWindow, _ s: NSWindow) -> Bool {
            let order = (NSWindow.windowNumbers(options: []) ?? []).map(\.intValue)
            guard let pi = order.firstIndex(of: p.windowNumber), let si = order.firstIndex(of: s.windowNumber) else { return false }
            return pi < si
        }
        let sheet = makeSheet()
        w.beginSheet(sheet, completionHandler: nil)
        XCTAssertTrue(spin { sheet.isVisible })
        command(root, .devError, args(1, "Uncaught error", "boom", "", ""))
        let p = try XCTUnwrap(overlay.panel)
        XCTAssertTrue(p.parent === sheet, "not the sheet's child")
        XCTAssertTrue(spin { front(p, sheet) }, "the sheet is over it")
        XCTAssertEqual(overlay.buttons.last?.keyEquivalent, "\r", "Reload lost Return")
        navSentEvents = []
        overlay.buttons[0].performClick(nil)
        XCTAssertEqual(navSentEvents, [AsukaEvents.devErrorAction(root, Int64(AsukaDevErrorAction.dismiss.rawValue))])

        // The sheet ends: back over the window.
        w.endSheet(sheet)
        XCTAssertTrue(spin { p.parent === w && p.isVisible }, "not back on the window")
        // Another begins while it shows: over that one.
        let next = makeSheet()
        w.beginSheet(next, completionHandler: nil)
        XCTAssertTrue(spin { p.parent === next && front(p, next) }, "not over the new sheet")
        XCTAssertEqual(overlay.buttons.last?.keyEquivalent, "\r", "Reload lost Return")
        command(root, .devErrorClear)
        XCTAssertNil(overlay.panel)
        XCTAssertFalse(p.isVisible)
    }

    /// Turns the run loop until cond holds (a second at most).
    private func spin(_ cond: () -> Bool) -> Bool {
        for _ in 0..<100 {
            if cond() { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        return cond()
    }
}
