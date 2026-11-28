import AppKit
import XCTest
@testable import AsukaHost
#if canImport(AsukaOps)
import AsukaOps
#endif

// A Sheet's Detents, as the core sends them: u32 count, then per detent
// u8 DetentKind and f32 its value.
final class SheetTests: XCTestCase {
    private func detents(_ ds: [(Int32, Float)]) -> [UInt8] {
        var b: [UInt8] = []
        withUnsafeBytes(of: UInt32(ds.count).littleEndian) { b += $0 }
        for (k, v) in ds {
            b.append(UInt8(k))
            withUnsafeBytes(of: v.bitPattern.littleEndian) { b += $0 }
        }
        return b
    }

    func testDecode() {
        let b = detents([(AsukaDetentKind.height.rawValue, 120), (AsukaDetentKind.medium.rawValue, 0), (AsukaDetentKind.fraction.rawValue, 0.25)])
        XCTAssertEqual(AsukaDetent.decode(b), [
            AsukaDetent(kind: AsukaDetentKind.height.rawValue, value: 120),
            AsukaDetent(kind: AsukaDetentKind.medium.rawValue, value: 0),
            AsukaDetent(kind: AsukaDetentKind.fraction.rawValue, value: 0.25),
        ])
        XCTAssertEqual(AsukaDetent.decode(detents([])), [])
        // Short: none, not a read past its end.
        XCTAssertEqual(AsukaDetent.decode(Array(b.dropLast())), [])
        XCTAssertEqual(AsukaDetent.decode([]), [])
    }
}
