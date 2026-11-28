// The watch host's node store, run on this Mac (swift test): a batch is
// decoded into the nodes and published once, each node changed bumping its
// revision once; texts are measured as they're drawn; a list's cells are
// recycled.
import SwiftUI
import XCTest
@testable import AsukaHost

/// Encodes ops as the core does (ops/ops.go): u32 count, then each op.
struct Batch {
    var ops: [[UInt8]] = []

    private func head(_ code: UInt8, _ id: UInt32) -> AsukaWriter {
        var w = AsukaWriter()
        w.u8(code)
        w.u32(id)
        return w
    }

    mutating func create(_ id: UInt32, _ kind: AsukaKind) { var w = head(1, id); w.u16(kind.rawValue); ops.append(w.bytes) }
    mutating func insert(_ parent: UInt32, _ child: UInt32, _ i: UInt32) { var w = head(3, parent); w.u32(child); w.u32(i); ops.append(w.bytes) }
    mutating func text(_ id: UInt32, _ prop: UInt16, _ s: String) { var w = head(5, id); w.u16(prop); w.u8(4); w.string(s); ops.append(w.bytes) }
    mutating func float(_ id: UInt32, _ prop: UInt16, _ v: Double) { var w = head(5, id); w.u16(prop); w.u8(3); w.f64(v); ops.append(w.bytes) }
    mutating func frame(_ id: UInt32, _ x: Float, _ y: Float, _ width: Float, _ height: Float) {
        var w = head(6, id)
        for f in [x, y, width, height] { w.f32(f) }
        ops.append(w.bytes)
    }
    mutating func root(_ id: UInt32) { ops.append(head(10, id).bytes) }
    mutating func listUpdate(_ id: UInt32, _ items: [Int64]) {
        var args = AsukaWriter()
        args.u32(UInt32(items.count))
        for i in items { args.u64(UInt64(bitPattern: i)) }
        command(id, .listUpdate, args.bytes)
    }
    mutating func scrollTo(_ id: UInt32, x: Float, y: Float, animated: Bool) {
        var args = AsukaWriter()
        args.f32(x)
        args.f32(y)
        args.u8(animated ? 1 : 0)
        command(id, .scrollToOffset, args.bytes)
    }
    mutating func command(_ id: UInt32, _ c: AsukaCommand, _ args: [UInt8]) {
        var w = head(9, id)
        w.u16(c.rawValue)
        w.u32(UInt32(args.count))
        w.bytes.append(contentsOf: args)
        ops.append(w.bytes)
    }

    var bytes: [UInt8] {
        var w = AsukaWriter()
        w.u32(UInt32(ops.count))
        for o in ops { w.bytes.append(contentsOf: o) }
        return w.bytes
    }
}

@MainActor final class StoreTests: XCTestCase {
    func apply(_ b: Batch) {
        b.bytes.withUnsafeBytes { AsukaRuntime.shared.apply($0) }
    }

    func testABatchPublishesOnce() {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(101, .view)
        b.create(102, .text)
        b.insert(101, 102, 0)
        b.text(102, AsukaProp.Text.text, "Hi")
        b.float(102, AsukaProp.opacity, 0.5)
        b.frame(102, 8, 16, 40, 20)
        b.root(101)
        apply(b)
        let text = rt.nodes[102]!
        XCTAssertEqual(text.rev, 1, "four changes, one publish")
        XCTAssertEqual(text.text, "Hi")
        XCTAssertEqual(text.opacity, 0.5)
        XCTAssertEqual(text.frame, CGRect(x: 8, y: 16, width: 40, height: 20))
        XCTAssertTrue(text.parent === rt.nodes[101])
        XCTAssertTrue(rt.scene.root === rt.nodes[101])

        var b2 = Batch()
        b2.text(102, AsukaProp.Text.text, "Hello")
        b2.frame(102, 8, 16, 60, 20)
        apply(b2)
        XCTAssertEqual(text.rev, 2)
        XCTAssertEqual(rt.nodes[101]!.rev, 1, "an untouched parent isn't published")

        // An unchanged frame changes nothing.
        var b3 = Batch()
        b3.frame(102, 8, 16, 60, 20)
        apply(b3)
        XCTAssertEqual(text.rev, 2)
    }

    func testTextMeasuresAsDrawn() {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(201, .text)
        b.text(201, AsukaProp.Text.text, "Hello wonderful world")
        apply(b)
        let max = rt.measure(201, .nan, .nan, .infinity, .infinity)
        let min = rt.measure(201, .nan, .nan, -.infinity, .infinity)
        XCTAssertGreaterThan(max.width, 0)
        XCTAssertLessThan(min.width, max.width, "min-content: the widest word")
        XCTAssertGreaterThan(min.height, max.height, "the words wrap at min-content")
        let at = rt.measure(201, .nan, .nan, Float(min.width), .infinity)
        XCTAssertLessThanOrEqual(at.width, min.width)
        // Known sizes are the core's.
        XCTAssertEqual(rt.measure(201, 33, 44, .infinity, .infinity), CGSize(width: 33, height: 44))
        // Sizes round up to the pixel.
        let k = rt.scale
        XCTAssertEqual((max.width * k).rounded(.up), max.width * k)
    }

    func testListRecyclesCells() {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(301, .list)
        b.listUpdate(301, [10, 11, 12])
        apply(b)
        let list = rt.nodes[301]!.list!
        XCTAssertEqual(list.ids, [10, 11, 12])
        let first = list.appear(10, 0)!
        XCTAssertNotEqual(first.id & 0x8000_0000, 0, "a cell's ID has the high bit")
        XCTAssertTrue(list.appear(10, 0) === first, "a row on screen keeps its cell")
        list.disappear(10)
        XCTAssertTrue(list.appear(11, 1) === first, "a row off screen gives its cell to the next")
        // An item that goes lets its cell go.
        var b2 = Batch()
        b2.listUpdate(301, [10, 12])
        apply(b2)
        XCTAssertTrue(list.appear(12, 1) === first)
    }

    /// The root is reported at the whole screen with the safe area around
    /// what SwiftUI gives a view inside it (the time, the rounded
    /// corners), each as it comes: the view over the whole screen ignores
    /// the safe area, and has none of its own.
    func testTheRootReportsTheSafeArea() throws {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(501, .view)
        b.root(501)
        apply(b)
        rt.rootSize = .zero
        rt.reportRoot(safe: EdgeInsets(top: 53, leading: 2, bottom: 36, trailing: 2))
        XCTAssertNil(rt.rootEnv, "nothing is reported before the root's size")
        rt.reportRoot(size: CGSize(width: 208, height: 248))
        var e = try XCTUnwrap(rt.rootEnv)
        XCTAssertEqual([e.width, e.height], [208, 248])
        XCTAssertEqual([e.safeTop, e.safeLeft, e.safeBottom, e.safeRight], [53, 2, 36, 2])
        rt.reportRoot(safe: EdgeInsets(top: 40, leading: 0, bottom: 30, trailing: 0))
        e = try XCTUnwrap(rt.rootEnv)
        XCTAssertEqual([e.width, e.height, e.safeTop, e.safeBottom], [208, 248, 40, 30], "a new safe area keeps the size")
    }

    /// A screen laid out before SwiftUI places it (centered on the
    /// screen's corner) isn't at its size, and isn't reported.
    func testAScreenNotPlacedIsNotReported() {
        XCTAssertFalse(asukaPlaced(CGRect(x: -104, y: -124, width: 208, height: 248)))
        XCTAssertTrue(asukaPlaced(CGRect(x: 2, y: 62, width: 204, height: 150)))
        XCTAssertTrue(asukaPlaced(CGRect(x: 210, y: 62, width: 204, height: 150)), "sliding in as it's pushed")
    }

    /// ScrollToOffset reaches a Scroll's or a List's view as a request it
    /// scrolls on, each one, the same offset again too (scrolling to the
    /// end as the content grows).
    func testScrollToOffsetAsksTheScroll() {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(601, .scroll)
        b.scrollTo(601, x: 0, y: 300, animated: true)
        apply(b)
        let s = rt.nodes[601]!
        XCTAssertEqual(s.scrollTarget, CGPoint(x: 0, y: 300))
        XCTAssertTrue(s.scrollAnimated)
        let asked = s.scrollRequests
        var b2 = Batch()
        b2.scrollTo(601, x: 0, y: 300, animated: false)
        apply(b2)
        XCTAssertEqual(s.scrollRequests, asked + 1, "the same offset again scrolls again")
        XCTAssertFalse(s.scrollAnimated)
        var b3 = Batch()
        b3.create(602, .list)
        b3.listUpdate(602, [1, 2])
        b3.scrollTo(602, x: 0, y: 0, animated: false)
        apply(b3)
        let l = rt.nodes[602]!
        XCTAssertEqual(l.scrollRequests, 1)
        XCTAssertEqual(l.scrollTarget, .zero)
    }
}

/// Where a node's view sits in its frame: a Button narrower than its
/// frame (watchOS draws none wider than the screen less its margins, so a
/// button stretched across a wider column is) is centered in it, as
/// SwiftUI's frame centers it; a text keeps its top leading corner.
@MainActor final class PlacementTests: XCTestCase {
    /// The columns of a node's view, drawn in its frame, that have ink:
    /// the first and last.
    func inked(_ n: AsukaNode) -> (Int, Int)? {
        let r = ImageRenderer(content: AsukaNodeView(node: n).frame(width: n.frame.maxX, height: n.frame.maxY, alignment: .topLeading))
        r.scale = 1
        guard let img = r.cgImage else { return nil }
        let w = img.width, h = img.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                            space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        var first = -1, last = -1
        for x in 0..<w {
            for y in 0..<h where px[(y * w + x) * 4 + 3] > 0 {
                if first < 0 { first = x }
                last = x
                break
            }
        }
        return first < 0 ? nil : (first, last)
    }

    func testAButtonIsCenteredInAWiderFrame() throws {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(401, .button)
        b.text(401, AsukaProp.Button.title, "Go")
        b.frame(401, 0, 0, 300, 60)
        b.create(402, .text)
        b.text(402, AsukaProp.Text.text, "Go")
        b.frame(402, 0, 0, 300, 60)
        apply(b)
        XCTAssertEqual(asukaPlacement(.button), .center)
        XCTAssertEqual(asukaPlacement(.text), .topLeading)
        let (l, r) = try XCTUnwrap(inked(rt.nodes[401]!), "the button draws")
        XCTAssertGreaterThan(l, 20, "the button is narrower than its frame")
        XCTAssertLessThanOrEqual(abs(l - (299 - r)), 2, "and centered in it: \(l)...\(r) in 300")
        let (tl, _) = try XCTUnwrap(inked(rt.nodes[402]!), "the text draws")
        XCTAssertLessThan(tl, 4, "a text sits at its frame's leading edge")
    }

    /// A Button says the widest it's drawn (what the measure callback
    /// tells the core, asuka_measure_max): what SwiftUI draws it at,
    /// proposed more than any screen, measured once per look; it's never
    /// measured wider. A text, and a plain button in a background of the
    /// app's (which fills its frame), have none.
    func testAButtonSaysTheWidestItsDrawn() throws {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(411, .button)
        b.text(411, AsukaProp.Button.title, "Go")
        b.create(412, .button)
        b.text(412, AsukaProp.Button.title, "Go")
        b.create(413, .text)
        b.text(413, AsukaProp.Text.text, "Go")
        b.create(414, .button)
        b.text(414, AsukaProp.Button.title, "Go")
        apply(b)
        rt.nodes[414]!.background = 0xFF0000FF
        AsukaMeasure.widestButtons.removeAll()
        let widest = try XCTUnwrap(rt.widest(411))
        XCTAssertGreaterThan(widest, 0)
        XCTAssertEqual(rt.widest(412), widest)
        XCTAssertEqual(AsukaMeasure.widestButtons.count, 1, "one look, measured once")
        XCTAssertNil(rt.widest(413))
        XCTAssertNil(rt.widest(414))
        XCTAssertLessThanOrEqual(rt.measure(411, .nan, .nan, 10_000, .infinity).width, widest)
        // At a proposal wider than that it's drawn at it: what the core
        // narrows a stretched button's frame to.
        XCTAssertEqual(AsukaMeasure.control(rt.nodes[411]!, width: widest + 100).width, widest)
    }

    func apply(_ b: Batch) {
        b.bytes.withUnsafeBytes { AsukaRuntime.shared.apply($0) }
    }
}
