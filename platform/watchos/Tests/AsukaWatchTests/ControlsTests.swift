// The watch's Chip, DateRangePicker and Disclosure: their props into the
// node store, measured as SwiftUI draws them; and Image's Headers and Cache.
import SwiftUI
import XCTest
@testable import AsukaHost

extension Batch {
    mutating func bool(_ id: UInt32, _ prop: UInt16, _ v: Bool) {
        var w = AsukaWriter(); w.u8(5); w.u32(id); w.u16(prop); w.u8(1); w.u8(v ? 1 : 0); ops.append(w.bytes)
    }
}

@MainActor final class ControlsTests: XCTestCase {
    func apply(_ b: Batch) {
        b.bytes.withUnsafeBytes { AsukaRuntime.shared.apply($0) }
    }

    func testChipIsMeasuredWithWhatItShows() {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(601, .chip)
        b.text(601, AsukaProp.Chip.title, "Pool")
        b.create(602, .chip)
        b.text(602, AsukaProp.Chip.title, "Pool")
        b.bool(602, AsukaProp.Chip.filter, true)
        b.bool(602, AsukaProp.Chip.selected, true)
        b.bool(602, AsukaProp.Chip.removable, true)
        apply(b)
        let n = rt.nodes[602]!
        XCTAssertTrue(n.filter && n.on && n.removable)
        let plain = rt.measure(601, .nan, .nan, 300, .infinity)
        let more = rt.measure(602, .nan, .nan, 300, .infinity)
        XCTAssertGreaterThan(plain.width, 0)
        XCTAssertGreaterThan(plain.height, 0)
        XCTAssertGreaterThan(more.width, plain.width, "the checkmark and the remove button")
    }

    func testDateRangeAndDisclosure() {
        let rt = AsukaRuntime.shared
        var b = Batch()
        b.create(611, .dateRangePicker)
        b.int(611, AsukaProp.DateRangePicker.start, 1_780_000_000_000)
        b.int(611, AsukaProp.DateRangePicker.end, 1_780_300_000_000)
        b.create(612, .disclosure)
        b.text(612, AsukaProp.Disclosure.title, "More")
        b.bool(612, AsukaProp.Disclosure.expanded, true)
        apply(b)
        XCTAssertEqual(rt.nodes[611]!.rangeStart, 1_780_000_000_000)
        XCTAssertEqual(rt.nodes[611]!.rangeEnd, 1_780_300_000_000)
        XCTAssertEqual(rt.nodes[611]!.max, 0, "no latest day")
        XCTAssertTrue(rt.nodes[612]!.on)
        let d = rt.measure(612, .nan, .nan, 180, .infinity)
        XCTAssertEqual(d.width, 180, "as wide as it's laid out")
        XCTAssertGreaterThan(d.height, 0)
        XCTAssertGreaterThan(rt.measure(611, .nan, .nan, 180, .infinity).height, 0)
    }

    func testImageHeadersAndCache() {
        let rt = AsukaRuntime.shared
        var w = AsukaWriter()
        w.u32(2)
        w.string("Authorization")
        w.string("Bearer t")
        var b = Batch()
        b.create(621, .image)
        b.bytes(621, AsukaProp.Image.headers, w.bytes)
        b.enumValue(621, AsukaProp.Image.cache, AsukaImageCache.memory.rawValue)
        apply(b)
        XCTAssertEqual(rt.nodes[621]!.headers, ["Authorization": "Bearer t"])
        XCTAssertEqual(rt.nodes[621]!.imageCache, AsukaImageCache.memory.rawValue)
    }
}
