import AppKit
import XCTest
@testable import AsukaHost

// The controls of their own files (Chip.swift, DateRange.swift), as views
// alone; mactest's controls_test.go drives them in the player.

@MainActor final class ControlsTests: XCTestCase {
    private func day(_ y: Int, _ m: Int, _ d: Int, hour: Int = 0) -> Int64 {
        let c = DateComponents(calendar: .current, year: y, month: m, day: d, hour: hour)
        return Int64((c.date!.timeIntervalSince1970 * 1000).rounded())
    }

    private let sel = Selector(("noAction:")) // the pickers' action, never sent here

    /// Two pickers: the range is their days' midnights; a start past the
    /// end takes the end with it, an end before the start the start; the
    /// same range isn't reported twice.
    func testDateRangeCompact() {
        let r = AsukaDateRangeView(frame: .zero)
        r.build(inline: false, target: self, action: sel)
        XCTAssertEqual(r.pickers.count, 2)
        // None chosen: today, unreported.
        let today = Calendar.current.startOfDay(for: Date())
        XCTAssertEqual(r.shown().0, today)
        r.start = day(2030, 3, 4, hour: 15)
        r.end = day(2030, 3, 9, hour: 1)
        r.apply()
        XCTAssertEqual(asukaMs(r.shown().0), Double(day(2030, 3, 4)))
        XCTAssertEqual(asukaMs(r.shown().1), Double(day(2030, 3, 9)))
        XCTAssertNil(r.changed(r.first!), "the range set isn't the user's")
        r.first!.dateValue = Date(timeIntervalSince1970: Double(day(2030, 3, 12)) / 1000)
        let moved = r.changed(r.first!)
        XCTAssertEqual(moved?.0, Double(day(2030, 3, 12)))
        XCTAssertEqual(moved?.1, Double(day(2030, 3, 12)))
        r.last!.dateValue = Date(timeIntervalSince1970: Double(day(2030, 3, 1)) / 1000)
        let back = r.changed(r.last!)
        XCTAssertEqual(back?.0, Double(day(2030, 3, 1)))
        XCTAssertEqual(back?.1, Double(day(2030, 3, 1)))
        // Min and Max bound every picker: Max to its day's last second.
        r.min = day(2030, 1, 1, hour: 9)
        r.max = day(2030, 12, 31, hour: 9)
        r.apply()
        XCTAssertEqual(r.first!.minDate.map(asukaMs), Double(day(2030, 1, 1)))
        XCTAssertEqual(r.last!.maxDate.map(asukaMs), Double(day(2031, 1, 1)) - 1000)
        XCTAssertEqual(r.pickers.map { $0.accessibilityLabel() ?? "" }, [asukaWord("startDate"), asukaWord("endDate")])
        // In a row, the start at the right reading right to left.
        r.frame = CGRect(x: 0, y: 0, width: 400, height: 40)
        r.layout()
        XCTAssertLessThan(r.first!.frame.minX, r.last!.frame.minX)
        r.userInterfaceLayoutDirection = .rightToLeft
        r.layout()
        XCTAssertGreaterThan(r.first!.frame.minX, r.last!.frame.minX)
    }

    /// Inline: NSDatePicker's range mode, dateValue the first day and
    /// timeInterval to the last.
    func testDateRangeInline() {
        let r = AsukaDateRangeView(frame: .zero)
        r.build(inline: true, target: self, action: sel)
        let c = r.calendar!
        XCTAssertEqual(c.datePickerMode, .range)
        XCTAssertEqual(c.datePickerStyle, .clockAndCalendar)
        r.start = day(2030, 5, 2)
        r.end = day(2030, 5, 6, hour: 23)
        r.apply()
        XCTAssertEqual(asukaMs(c.dateValue), Double(day(2030, 5, 2)))
        XCTAssertEqual(c.timeInterval, 4 * 86400, accuracy: 3600) // a day of summer time
        XCTAssertEqual(asukaMs(r.shown().1), Double(day(2030, 5, 6)))
        XCTAssertGreaterThan(r.fit().height, 100)
    }

    /// A removable chip measures its remove button's room, which takes the
    /// end: the right, or the left reading right to left.
    func testChipRemover() {
        let b = AsukaChipButton(frame: .zero)
        b.cell = AsukaChipCell(textCell: "")
        b.bezelStyle = .recessed
        b.title = "Ana"
        let plain = b.cell!.cellSize.width
        b.setRemovable(true, name: "Remove Ana")
        XCTAssertEqual(b.cell!.cellSize.width, plain + AsukaChipCell.removerRoom)
        XCTAssertEqual(b.remover?.accessibilityLabel(), "Remove Ana")
        b.frame = CGRect(x: 0, y: 0, width: b.cell!.cellSize.width, height: 24)
        b.layout()
        XCTAssertGreaterThan(b.remover!.frame.minX, b.frame.width / 2)
        b.userInterfaceLayoutDirection = .rightToLeft
        b.layout()
        XCTAssertLessThan(b.remover!.frame.maxX, b.frame.width / 2)
        b.isEnabled = false
        XCTAssertFalse(b.remover!.isEnabled)
        b.setRemovable(false, name: "")
        XCTAssertNil(b.remover)
        XCTAssertEqual(b.cell!.cellSize.width, plain)
    }

    /// The words the controls say: English where a language has none.
    func testControlWords() {
        AsukaRuntime.shared.locale = "en"
        defer { AsukaRuntime.shared.locale = "" }
        XCTAssertEqual(asukaWord("remove"), "Remove")
        XCTAssertEqual(asukaWord("startDate"), "Start date")
        XCTAssertEqual(asukaWord("endDate"), "End date")
    }
}
