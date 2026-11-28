import AppKit
import XCTest
@testable import AsukaHost

// Interaction states the automation can't reach: under it, the steps stand
// in for the pointer and the app's activation (platform/macos/mactest).

@MainActor
final class InteractionTests: XCTestCase {
    private var rt: AsukaRuntime { AsukaRuntime.shared }

    override func setUp() async throws {
        await MainActor.run {
            _ = NSApplication.shared
            let rt = AsukaRuntime.shared
            if rt.main == nil { rt.makeMainWindow() }
            rt.started = true
        }
    }

    // A tracking area in the active app gets no exited while another app is
    // active, so the host follows the activation itself, as launch sets it
    // up: leaving the app, a hovered view isn't; back, the view under the
    // pointer is.
    func testHoverFollowsTheAppsActivation() throws {
        rt.observe() // what launch observes
        let id: UInt32 = 5001
        rt.create(id, kind: AsukaKind.view.rawValue)
        rt.listen(id, event: AsukaEventKind.hover.rawValue)
        let n = try XCTUnwrap(rt.nodes[id])
        let v = try XCTUnwrap(n.view)
        v.frame = CGRect(x: 0, y: 0, width: 50, height: 50)
        rt.main.window.contentView?.addSubview(v)
        defer { v.removeFromSuperview() }
        rt.mouseEntered(v)
        XCTAssertEqual(n.interaction?.hovered, true)

        NotificationCenter.default.post(name: NSApplication.didResignActiveNotification, object: NSApp)
        XCTAssertEqual(n.interaction?.hovered, false, "still hovered with another app active")

        let w = try XCTUnwrap(rt.main.window)
        let p = v.convert(CGPoint(x: 25, y: 25), to: nil)
        rt.visual.hoverProbe = { $0 === w ? p : nil }
        defer { rt.visual.hoverProbe = nil }
        NotificationCenter.default.post(name: NSApplication.didBecomeActiveNotification, object: NSApp)
        XCTAssertEqual(n.interaction?.hovered, true, "not hovered under the pointer once the app is back")
    }
    // HoverMove: the view's tracking area sends mouseMoved too, and the
    // pointer's move over the view (the real one: the automation stands in
    // for it with hovermove) hovers it, once per event.
    func testHoverMoveTracksTheMouse() throws {
        let id: UInt32 = 5002
        rt.create(id, kind: AsukaKind.view.rawValue)
        rt.listen(id, event: AsukaEventKind.hoverMove.rawValue)
        let n = try XCTUnwrap(rt.nodes[id])
        let v = try XCTUnwrap(n.view)
        v.frame = CGRect(x: 0, y: 0, width: 50, height: 50)
        rt.main.content.addSubview(v)
        defer { v.removeFromSuperview() }
        let area = try XCTUnwrap(n.interaction?.hoverArea)
        XCTAssertTrue(area.options.contains(.mouseMoved), "no mouseMoved in \(area.options)")
        XCTAssertEqual(v.trackingAreas.count, 1)
        let w = try XCTUnwrap(rt.main.window)
        let e = try XCTUnwrap(NSEvent.mouseEvent(with: .mouseMoved, location: v.convert(CGPoint(x: 10, y: 20), to: nil), modifierFlags: [],
                                                 timestamp: 42, windowNumber: w.windowNumber, context: nil, eventNumber: 0, clickCount: 0, pressure: 0))
        XCTAssertTrue(rt.mouseMoved(v, e))
        XCTAssertEqual(n.interaction?.hovered, true)
        XCTAssertEqual(n.interaction?.movedAt, 42)
    }

    // A pan along an axis: a drag that went the other way first isn't one.
    func testPanAxis() {
        XCTAssertTrue(asukaPanAlong(.horizontal, .zero))
        XCTAssertTrue(asukaPanAlong(.horizontal, CGPoint(x: -10, y: 4)))
        XCTAssertFalse(asukaPanAlong(.horizontal, CGPoint(x: 3, y: 9)))
        XCTAssertTrue(asukaPanAlong(.vertical, CGPoint(x: 3, y: -9)))
        XCTAssertFalse(asukaPanAlong(.vertical, CGPoint(x: 10, y: 4)))
        XCTAssertTrue(asukaPanAlong(.both, CGPoint(x: 10, y: 4)))
    }

    // A dragged row's neighbours make way: those between where it was and
    // where it would drop move by its height.
    func testRowsMakeWay() {
        let m = AsukaRowMove(AsukaRowView(), from: 2, height: 40, grab: 10, y: 80, loc: .zero)
        m.to = 5
        XCTAssertEqual((0..<8).map { m.shift($0) }, [0, 0, 0, -40, -40, -40, 0, 0])
        m.to = 0
        XCTAssertEqual((0..<8).map { m.shift($0) }, [40, 40, 0, 0, 0, 0, 0, 0])
        m.to = 2
        XCTAssertEqual((0..<8).map { m.shift($0) }, [0, 0, 0, 0, 0, 0, 0, 0])
    }
}
