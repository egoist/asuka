#if canImport(UIKit)
import XCTest
import UIKit
@testable import AsukaHost
@testable import AsukaOps

// The style props beyond a view's box on the iOS host: letter spacing,
// line height, font traits, truncation and shrinking to fit on a label
// (measured as drawn), and corners and borders the layer can't draw.
@MainActor extension HeadlessTests {
    func text(_ s: String) -> UInt32 {
        let t = make(.text)
        set(t, AsukaProp.Text.text, .string(s))
        return t
    }

    func label(_ id: UInt32) -> AsukaLabel { view(id) as! AsukaLabel }

    func size(_ id: UInt32, width: Float) -> CGSize { rt.measure(id, .nan, .nan, width, .infinity) }

    func testLetterSpacingAndLineHeightAreMeasured() {
        let plain = text("Hello there"), spaced = text("Hello there")
        set(spaced, AsukaProp.letterSpacing, .float(3))
        set(spaced, AsukaProp.lineHeight, .float(40))
        let a = size(plain, width: 300), b = size(spaced, width: 300)
        // Eleven characters, 3 after each, at the default text size.
        XCTAssertEqual(b.width - a.width, 33, accuracy: 2)
        XCTAssertEqual(b.height, 40, accuracy: 1)
        XCTAssertTrue(label(spaced).styled)
        XCTAssertEqual(label(spaced).attributedText?.attribute(.kern, at: 0, effectiveRange: nil) as? CGFloat ?? 0, 3, accuracy: 0.01)
        // Back to none: the label's own text again.
        set(spaced, AsukaProp.letterSpacing, .float(0))
        set(spaced, AsukaProp.lineHeight, .float(0))
        XCTAssertFalse(label(spaced).styled)
        XCTAssertEqual(size(spaced, width: 300), a)
    }

    func testFontTraits() {
        let t = text("12:30")
        set(t, AsukaProp.fontTraits, .int(1 | 2))
        let f = label(t).font!
        XCTAssertTrue(f.fontDescriptor.symbolicTraits.contains(.traitItalic))
        let features = f.fontDescriptor.object(forKey: .featureSettings) as? [[UIFontDescriptor.FeatureKey: Int]] ?? []
        XCTAssertTrue(features.contains { $0[.type] == kNumberSpacingType && $0[.selector] == kMonospacedNumbersSelector })
    }

    func testTruncation() {
        let t = text("A long title that is cut")
        set(t, AsukaProp.lineLimit, .int(1))
        set(t, AsukaProp.truncation, .enumValue(AsukaTruncation.middle.rawValue))
        XCTAssertEqual(label(t).lineBreakMode, .byTruncatingMiddle)
        set(t, AsukaProp.truncation, .enumValue(AsukaTruncation.clip.rawValue))
        XCTAssertEqual(label(t).lineBreakMode, .byClipping)
    }

    /// A text that shrinks to fit is measured at the scale it's drawn at.
    func testMinimumScaleMeasuresAsDrawn() {
        let t = text("Shrinks to fit: a long title that does not fit")
        set(t, AsukaProp.minimumScale, .float(0.5))
        let s = size(t, width: 200)
        XCTAssertLessThanOrEqual(s.width, 200)
        let one = size(text("X"), width: 200).height
        XCTAssertLessThan(s.height, one) // one line, smaller
        frame(t, CGRect(x: 0, y: 0, width: 200, height: s.height))
        let l = label(t)
        XCTAssertLessThan(l.shrink, 1)
        XCTAssertGreaterThanOrEqual(l.shrink, 0.5)
        XCTAssertEqual(l.numberOfLines, 1)
        XCTAssertEqual(l.sizeThatFits(CGSize(width: 200, height: 1000)).height, s.height, accuracy: 1)
    }

    func testFitScale() {
        XCTAssertEqual(asukaFitScale(min: 0.5) { _ in true }, 1)
        XCTAssertEqual(asukaFitScale(min: 0.5) { _ in false }, 0.5)
        XCTAssertEqual(asukaFitScale(min: 0.25) { $0 <= 0.6 }, 0.6, accuracy: 1.0 / 128)
        XCTAssertEqual(asukaFitScale(min: 0) { _ in false }, 1) // off
    }

    func testClampRadii() {
        // As CSS's: the factor that fits the tightest side, on every corner.
        XCTAssertEqual(asukaClampRadii([999, 999, 999, 999], CGSize(width: 80, height: 60)), [30, 30, 30, 30])
        XCTAssertEqual(asukaClampRadii([40, 40, 0, 0], CGSize(width: 60, height: 100)), [30, 30, 0, 0])
        XCTAssertEqual(asukaClampRadii([10, 4, 10, 4], CGSize(width: 80, height: 60)), [10, 4, 10, 4])
    }

    func testCornersThatDifferAreAnOutline() {
        let v = make(.view)
        set(v, AsukaProp.background, .color(0x2_ff0000ff))
        set(v, AsukaProp.cornerRadii, .edges(top: 16, right: 16, bottom: 0, left: 0))
        frame(v, CGRect(x: 0, y: 0, width: 100, height: 50))
        // The same radius on the corners it rounds: the layer's own.
        XCTAssertEqual(view(v).layer.cornerRadius, 16)
        XCTAssertEqual(view(v).layer.maskedCorners, [.layerMinXMinYCorner, .layerMaxXMinYCorner])
        XCTAssertNil(rt.nodes[v]!.outline)
        // Different radii: the outline draws the background.
        set(v, AsukaProp.cornerRadii, .edges(top: 20, right: 4, bottom: 20, left: 4))
        XCTAssertEqual(view(v).layer.cornerRadius, 0)
        XCTAssertNil(view(v).backgroundColor)
        XCTAssertEqual(rt.nodes[v]!.outline?.shaped, true)
        // Back to one radius.
        set(v, AsukaProp.cornerRadii, .edges(top: 0, right: 0, bottom: 0, left: 0))
        set(v, AsukaProp.cornerRadius, .float(8))
        XCTAssertNil(rt.nodes[v]!.outline)
        XCTAssertNotNil(view(v).backgroundColor)
        XCTAssertEqual(view(v).layer.cornerRadius, 8)
    }

    func testBorderSidesAndStyle() {
        let v = make(.view)
        set(v, AsukaProp.borderColor, .color(0x2_000000ff))
        set(v, AsukaProp.borderWidths, .edges(top: 0, right: 0, bottom: 2, left: 0))
        frame(v, CGRect(x: 0, y: 0, width: 100, height: 50))
        XCTAssertEqual(view(v).layer.borderWidth, 0)
        XCTAssertNotNil(rt.nodes[v]!.outline?.border.path)
        // Dashed, the same width all round: one stroked line.
        set(v, AsukaProp.borderWidths, .edges(top: 0, right: 0, bottom: 0, left: 0))
        set(v, AsukaProp.borderWidth, .float(2))
        set(v, AsukaProp.borderStyle, .enumValue(AsukaBorderStyle.dashed.rawValue))
        let sides = rt.nodes[v]!.outline?.sides ?? []
        XCTAssertEqual(sides.count, 1)
        XCTAssertEqual(sides.first?.lineDashPattern, [6, 6])
        // Solid again: the layer's own border.
        set(v, AsukaProp.borderStyle, .enumValue(AsukaBorderStyle.solid.rawValue))
        XCTAssertNil(rt.nodes[v]!.outline)
        XCTAssertEqual(view(v).layer.borderWidth, 2)
    }
}
#endif
