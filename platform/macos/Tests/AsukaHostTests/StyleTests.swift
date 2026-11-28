import AppKit
import XCTest
@testable import AsukaHost
#if canImport(AsukaOps)
import AsukaOps
#endif

// The box's and the text's styling's pure logic (Box.swift,
// TextStyle.swift): corner radii scaled down as CSS's, a box's path, a
// font's variants, the scale a shrinking text fits at.
@MainActor final class StyleTests: XCTestCase {
    private func node(_ kind: AsukaKind, _ w: CGFloat, _ h: CGFloat) -> AsukaNode {
        let n = AsukaNode(id: 1, kind: kind, view: nil)
        n.frame = CGRect(x: 0, y: 0, width: w, height: h)
        return n
    }

    /// Radii whose sum is more than their side are all scaled by one
    /// factor; a capsule's 999 is half the shorter side.
    func testRadii() {
        let n = node(.view, 80, 60)
        n.cornerRadius = 999
        XCTAssertEqual(n.radii.map { Double($0) }, [30, 30, 30, 30].map { Double($0) })
        n.cornerRadii = [60, 60, 0, 0] // the top's 120 on 80: two thirds
        let r = n.radii
        XCTAssertEqual(Double(r[0]), 40, accuracy: 1e-9)
        XCTAssertEqual(Double(r[1]), 40, accuracy: 1e-9)
        XCTAssertEqual(Double(r[2]), 0)
        XCTAssertTrue(n.unevenCorners && n.needsBox)
        n.cornerRadii = [0, 0, 0, 0]
        XCTAssertFalse(n.needsBox)
        n.borderWidth = 2
        n.borderStyle = AsukaBorderStyle.dashed.rawValue
        XCTAssertTrue(n.needsBox)
        n.borderWidth = 0
        XCTAssertFalse(n.needsBox) // a style without a width draws nothing
        n.borderWidths = [0, 0, 1, 0]
        XCTAssertEqual(n.borderSides, [0, 0, 1, 0])
        XCTAssertTrue(n.needsBox)
    }

    /// The path's corners are where its radii put them.
    func testBoxPath() {
        let p = asukaBoxPath(CGRect(x: 0, y: 0, width: 100, height: 50), [(20, 20), (0, 0), (10, 10), (0, 0)])
        XCTAssertEqual(p.boundingBoxOfPath, CGRect(x: 0, y: 0, width: 100, height: 50))
        XCTAssertFalse(p.contains(CGPoint(x: 1, y: 1)))     // rounded off at the top left
        XCTAssertTrue(p.contains(CGPoint(x: 99, y: 1)))     // square at the top right
        XCTAssertFalse(p.contains(CGPoint(x: 99.5, y: 49.5))) // rounded at the bottom right
        XCTAssertTrue(p.contains(CGPoint(x: 0.5, y: 49.5))) // square at the bottom left
    }

    /// FontTraits: italic, monospaced digits and small capitals.
    func testTraited() {
        let rt = AsukaRuntime.shared
        let f = NSFont.systemFont(ofSize: 13)
        XCTAssertTrue(rt.traited(f, 1).fontDescriptor.symbolicTraits.contains(.italic) || rt.traited(f, 1).fontDescriptor.object(forKey: .matrix) != nil)
        let digits = rt.traited(f, 2)
        // Monospaced digits: every one as wide.
        let w1 = ("1111" as NSString).size(withAttributes: [.font: digits]).width
        let w8 = ("8888" as NSString).size(withAttributes: [.font: digits]).width
        XCTAssertEqual(Double(w1), Double(w8), accuracy: 0.01)
        let caps = rt.traited(f, 4)
        let features = caps.fontDescriptor.object(forKey: .featureSettings) as? [[NSFontDescriptor.FeatureKey: Any]]
        XCTAssertEqual(features?.count, 1)
        XCTAssertTrue(rt.traited(f, 0) === f)
    }

    /// A shrinking text's scale: 1 when it fits, the largest that fits
    /// otherwise, never under its minimum.
    func testFitScale() {
        let rt = AsukaRuntime.shared
        let n = node(.text, 0, 0)
        n.text = "A long title that doesn't fit"
        n.fontSize = 20
        let natural = NSAttributedString(string: n.text, attributes: rt.textAttributes(n, scale: 1)).size().width
        XCTAssertEqual(Double(rt.fitScale(n, natural + 10)), 1) // no MinimumScale
        n.minimumScale = 0.25
        XCTAssertEqual(Double(rt.fitScale(n, natural + 10)), 1)
        let half = rt.fitScale(n, natural / 2 + 4)
        XCTAssertLessThan(Double(half), 0.52)
        XCTAssertGreaterThan(Double(half), 0.4) // a smaller size of SF is wider, for its size
        let shrunk = NSAttributedString(string: n.text, attributes: rt.textAttributes(n, scale: half)).size().width
        XCTAssertLessThanOrEqual(Double(shrunk), Double(natural / 2) + 0.25)
        n.minimumScale = 0.8
        XCTAssertEqual(Double(rt.fitScale(n, natural / 2 + 4)), 0.8, accuracy: 1e-6)
        XCTAssertEqual(rt.effectiveLines(n), 1)
        XCTAssertEqual(rt.lineBreak(n), .byTruncatingTail)
        n.truncation = AsukaTruncation.middle.rawValue
        XCTAssertEqual(rt.lineBreak(n), .byTruncatingMiddle)
    }
}
