// Texts' letter spacing, line height, font variants and shrinking to
// fit, measured as SwiftUI draws them; boxes' corners, borders and
// gradients from the wire.
import CoreText
import SwiftUI
import XCTest
@testable import AsukaHost

extension Batch {
    mutating func int(_ id: UInt32, _ prop: UInt16, _ v: Int64) {
        var w = AsukaWriter(); w.u8(5); w.u32(id); w.u16(prop); w.u8(2); w.u64(UInt64(bitPattern: v)); ops.append(w.bytes)
    }
    mutating func enumValue(_ id: UInt32, _ prop: UInt16, _ v: Int32) {
        var w = AsukaWriter(); w.u8(5); w.u32(id); w.u16(prop); w.u8(7); w.u32(UInt32(bitPattern: v)); ops.append(w.bytes)
    }
    mutating func edges(_ id: UInt32, _ prop: UInt16, _ e: [Float]) {
        var w = AsukaWriter(); w.u8(5); w.u32(id); w.u16(prop); w.u8(6)
        for f in e { w.f32(f) }
        ops.append(w.bytes)
    }
    mutating func bytes(_ id: UInt32, _ prop: UInt16, _ b: [UInt8]) {
        var w = AsukaWriter(); w.u8(5); w.u32(id); w.u16(prop); w.u8(8); w.u32(UInt32(b.count)); w.bytes.append(contentsOf: b); ops.append(w.bytes)
    }
}

@MainActor final class TypesetTests: XCTestCase {
    func apply(_ b: Batch) {
        b.bytes.withUnsafeBytes { AsukaRuntime.shared.apply($0) }
    }

    /// A text node with text, set up by more.
    func text(_ id: UInt32, _ s: String, _ more: (inout Batch) -> Void = { _ in }) -> AsukaNode {
        var b = Batch()
        b.create(id, .text)
        b.text(id, AsukaProp.Text.text, s)
        more(&b)
        apply(b)
        return AsukaRuntime.shared.nodes[id]!
    }

    /// SwiftUI's size for the view the host draws, at a width (nil: its
    /// own).
    func drawn(_ n: AsukaNode, width: CGFloat?) -> CGSize { AsukaMeasure.control(n, width: width) }

    func testLetterSpacingIsMeasuredAsDrawn() {
        let rt = AsukaRuntime.shared
        _ = text(301, "Tracking")
        let spaced = text(302, "Tracking") { $0.float(302, AsukaProp.letterSpacing, 3) }
        let plain = rt.measure(301, .nan, .nan, .infinity, .infinity)
        let wide = rt.measure(302, .nan, .nan, .infinity, .infinity)
        // 3 points after each of its 8 characters.
        XCTAssertEqual(wide.width - plain.width, 24, accuracy: 1)
        XCTAssertEqual(wide.height, plain.height)
        XCTAssertEqual(drawn(spaced, width: nil).width, wide.width, accuracy: 1, "SwiftUI's kerning is CoreText's kern")
    }

    func testLineHeightIsEachLinesHeight() {
        let rt = AsukaRuntime.shared
        let n = text(311, "one two three four five six") { $0.float(311, AsukaProp.lineHeight, 40) }
        let one = rt.measure(311, .nan, .nan, .infinity, .infinity)
        XCTAssertEqual(one.height, 40, accuracy: 1, "a line, 40 points")
        let narrow = rt.measure(311, .nan, .nan, 60, .infinity)
        let count = (narrow.height / 40).rounded()
        XCTAssertGreaterThan(count, 1)
        XCTAssertEqual(narrow.height, count * 40, accuracy: 1)
        XCTAssertEqual(drawn(n, width: narrow.width).height, narrow.height, accuracy: 1.5, "SwiftUI's line spacing and padding")
        // With a line limit, that many lines.
        var b = Batch()
        b.int(311, AsukaProp.lineLimit, 2)
        apply(b)
        XCTAssertEqual(rt.measure(311, .nan, .nan, 60, .infinity).height, 80, accuracy: 1)
    }

    func testMonospacedDigitsAreTheFonts() {
        let rt = AsukaRuntime.shared
        func width(_ id: UInt32, _ s: String, _ traits: Int64) -> CGFloat {
            _ = text(id, s) { $0.int(id, AsukaProp.fontTraits, traits) }
            return rt.measure(id, .nan, .nan, .infinity, .infinity).width
        }
        XCTAssertEqual(width(321, "1111", 2), width(322, "0000", 2), "tabular digits are all as wide")
        XCTAssertNotEqual(width(323, "1111", 0), width(324, "0000", 0), "the system font's own digits aren't")
        let italic = AsukaFonts.font(style: 0, size: 17, weight: 0, traits: 1)
        XCTAssertTrue(CTFontGetSymbolicTraits(italic).contains(.traitItalic))
    }

    func testMinimumScaleShrinksToItsWidth() {
        let rt = AsukaRuntime.shared
        let n = text(331, "A title too long for its line") { $0.float(331, AsukaProp.minimumScale, 0.5) }
        let natural = rt.measure(331, .nan, .nan, .infinity, .infinity)
        let fit = rt.measure(331, .nan, .nan, Float(natural.width * 0.7), .infinity)
        XCTAssertLessThan(fit.height, natural.height, "one line, smaller")
        XCTAssertLessThanOrEqual(fit.width, natural.width * 0.7 + 0.5)
        // Drawn at the frame it was measured to, it finds the same scale.
        let at = AsukaTypeset.scale(n, width: natural.width * 0.7)
        XCTAssertEqual(AsukaTypeset.scale(n, width: fit.width), at)
        XCTAssertLessThan(at, 1)
        XCTAssertGreaterThanOrEqual(at, 0.5)
        // It never goes under the minimum.
        XCTAssertEqual(AsukaTypeset.scale(n, width: 10), 0.5)
        XCTAssertEqual(AsukaTypeset.scale(n, width: natural.width + 1), 1)
    }

    func testBoxesFromTheWire() {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(341, .view)
        b.edges(341, AsukaProp.cornerRadii, [16, 16, 0, 0])
        b.edges(341, AsukaProp.borderWidths, [0, 0, 2, 0])
        b.enumValue(341, AsukaProp.borderStyle, AsukaBorderStyle.dashed.rawValue)
        var g = AsukaWriter()
        g.u32(2)
        g.u64(0x2_ff00_00ff); g.f32(0)
        g.u64(0x2_0000_ffff); g.f32(1)
        for f: Float in [0, 0, 0, 1] { g.f32(f) }
        g.u8(UInt8(AsukaGradientKind.conic.rawValue))
        for f: Float in [0.25, 0.75, 0.6, 90] { g.f32(f) }
        b.bytes(341, AsukaProp.gradient, g.bytes)
        apply(b)
        let n = rt.nodes[341]!
        XCTAssertEqual(n.radii, [16, 16, 0, 0])
        XCTAssertEqual(n.borderWidths, [0, 0, 2, 0])
        XCTAssertEqual(n.borderStyle, AsukaBorderStyle.dashed.rawValue)
        XCTAssertEqual(n.gradient?.kind, .conic)
        XCTAssertEqual(n.gradient?.center, UnitPoint(x: 0.25, y: 0.75))
        XCTAssertEqual(n.gradient?.angle, 90)
        XCTAssertEqual(n.gradient?.stops.count, 2)
        // As the same radius again: none.
        b = Batch()
        b.edges(341, AsukaProp.cornerRadii, [0, 0, 0, 0])
        b.float(341, AsukaProp.cornerRadius, 8)
        apply(b)
        XCTAssertEqual(n.radii, [8, 8, 8, 8])
        // Radii too big for their side are scaled down together, as CSS's.
        XCTAssertEqual(AsukaBoxShape.fit([60, 60, 0, 0], CGSize(width: 100, height: 50)), [50, 50, 0, 0])
        XCTAssertEqual(AsukaBoxShape.fit([999, 999, 999, 999], CGSize(width: 100, height: 40)), [20, 20, 20, 20])
    }
}
