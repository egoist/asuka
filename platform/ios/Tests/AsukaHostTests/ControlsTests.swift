#if canImport(UIKit)
import XCTest
import UIKit
@testable import AsukaHost
@testable import AsukaOps

// The controls UIKit has no view of its own for: Chip (a capsule
// UIButton, filter and input chips), DateRangePicker (two compact pickers,
// or a calendar whose taps choose a range), Disclosure's header, and a
// TextField's Suggestions (a list under the field, a combo box's menu);
// and Image's caches: where a load may keep what it loaded, and the image
// module's arguments.
@MainActor extension HeadlessTests {
    func listen(_ id: UInt32, _ events: AsukaEventKind...) {
        for e in events { rt.listen(id, event: e.rawValue) }
    }

    func strings(_ list: [String]) -> [UInt8] {
        var w = AsukaWriter()
        w.u32(UInt32(list.count))
        for s in list { w.string(s) }
        return w.bytes
    }

    // MARK: Chip

    func chip(_ title: String, in r: UInt32) -> UInt32 {
        let c = make(.chip)
        set(c, AsukaProp.Chip.title, .string(title))
        rt.insert(r, child: c, index: 0)
        listen(c, .press, .toggled, .remove)
        return c
    }

    /// A chip is measured with what it shows: an icon, a selected filter
    /// chip's checkmark, a removable one's remove button.
    func testChipMeasuresWhatItShows() {
        let r = root()
        let c = chip("Breakfast", in: r)
        let plain = rt.measure(c, .nan, .nan, 390, .infinity)
        XCTAssertGreaterThanOrEqual(plain.height, 28)
        XCTAssertLessThan(plain.height, 44)
        set(c, AsukaProp.Chip.filter, .bool(true))
        set(c, AsukaProp.Chip.selected, .bool(true))
        let checked = rt.measure(c, .nan, .nan, 390, .infinity)
        XCTAssertGreaterThan(checked.width, plain.width + 10, "the checkmark")
        let input = chip("Ana", in: r)
        let a = rt.measure(input, .nan, .nan, 390, .infinity)
        set(input, AsukaProp.Chip.removable, .bool(true))
        let b = rt.measure(input, .nan, .nan, 390, .infinity)
        XCTAssertEqual(b.width - a.width, AsukaChipView.removeSize + AsukaChipView.removeGap + 8 - 12, accuracy: 1)
    }

    /// An action chip reports Press; a filter chip flips at once and
    /// reports Toggled, not Press, and VoiceOver reads it as a toggle.
    func testChipPressAndToggle() {
        let r = root()
        let c = chip("Add me", in: r)
        let v = view(c) as! AsukaChipView
        _ = sent()
        v.sendActions(for: .primaryActionTriggered)
        XCTAssertEqual(sent().map(\.kind), [AsukaEventKind.press.rawValue])
        let f = chip("Pool", in: r)
        set(f, AsukaProp.Chip.filter, .bool(true))
        let fv = view(f) as! AsukaChipView
        fv.sendActions(for: .primaryActionTriggered)
        let e = sent()
        XCTAssertEqual(e.map(\.kind), [AsukaEventKind.toggled.rawValue])
        XCTAssertEqual(e.first?.payload, [1])
        XCTAssertTrue(fv.isOn)
        XCTAssertEqual(fv.configuration?.image, UIImage(systemName: "checkmark"))
        if #available(iOS 17.0, *) {
            XCTAssertTrue(fv.accessibilityTraits.contains(.toggleButton))
            XCTAssertEqual(fv.accessibilityValue, "1")
        }
        // The app refusing: the core sends it back.
        set(f, AsukaProp.Chip.selected, .bool(false))
        XCTAssertFalse(fv.isOn)
        XCTAssertNil(fv.configuration?.image)
    }

    /// A removable chip's remove button, and VoiceOver's action, report
    /// Remove; its body still presses.
    func testChipRemove() {
        let r = root()
        let c = chip("Bo", in: r)
        set(c, AsukaProp.Chip.icon, .string("person"))
        set(c, AsukaProp.Chip.removable, .bool(true))
        let s = rt.measure(c, .nan, .nan, 390, .infinity)
        frame(c, CGRect(x: 20, y: 100, width: s.width, height: s.height))
        let v = view(c) as! AsukaChipView
        v.layoutIfNeeded()
        let x = try! XCTUnwrap(v.removeButton)
        XCTAssertEqual(x.frame.maxX, v.bounds.maxX, accuracy: 0.5, "at the end")
        XCTAssertTrue(v.hitTest(CGPoint(x: v.bounds.maxX - 10, y: v.bounds.midY), with: nil) === x)
        XCTAssertTrue(v.hitTest(CGPoint(x: 10, y: v.bounds.midY), with: nil) === v)
        _ = sent()
        x.sendActions(for: .touchUpInside)
        XCTAssertEqual(sent().map(\.kind), [AsukaEventKind.remove.rawValue])
        let action = try! XCTUnwrap(v.accessibilityCustomActions?.first { $0 is AsukaChipRemoveAction })
        XCTAssertFalse(action.name.isEmpty)
        _ = action.actionHandler?(action)
        XCTAssertEqual(sent().map(\.kind), [AsukaEventKind.remove.rawValue])
        v.sendActions(for: .primaryActionTriggered)
        XCTAssertEqual(sent().map(\.kind), [AsukaEventKind.press.rawValue])
    }

    // MARK: DateRangePicker

    func day(_ y: Int, _ m: Int, _ d: Int, hour: Int = 0) -> Date {
        Calendar.current.date(from: DateComponents(year: y, month: m, day: d, hour: hour))!
    }

    /// A calendar's taps: the first starts a range, the second (that day
    /// or later) ends it, a third starts again; a second before the first
    /// moves the start.
    func testRangeSelectionTaps() {
        var s = AsukaRangeSelection()
        XCTAssertFalse(s.tap(day(2026, 3, 10, hour: 15)))
        XCTAssertEqual(s.start, day(2026, 3, 10))
        XCTAssertNil(s.end)
        XCTAssertEqual(s.days, [day(2026, 3, 10)])
        XCTAssertFalse(s.tap(day(2026, 3, 8)), "before the start: the start moves")
        XCTAssertEqual(s.start, day(2026, 3, 8))
        XCTAssertTrue(s.tap(day(2026, 3, 12, hour: 9)))
        XCTAssertEqual(s.end, day(2026, 3, 12))
        XCTAssertEqual(s.days, (8...12).map { day(2026, 3, $0) })
        XCTAssertFalse(s.tap(day(2026, 4, 1)), "a third starts again")
        XCTAssertEqual(s.days, [day(2026, 4, 1)])
        XCTAssertTrue(s.tap(day(2026, 4, 1)), "a day's range: the same day twice")
        XCTAssertEqual(s.days, [day(2026, 4, 1)])
        // Across a month's end.
        s.set(start: day(2026, 1, 30), end: day(2026, 2, 2, hour: 12))
        XCTAssertEqual(s.days.count, 4)
        s.set(start: nil, end: day(2026, 2, 2))
        XCTAssertEqual(s.days, [])
    }

    /// The compact pickers report local midnights; a start past the end
    /// moves the end to it; the end can't go before the start.
    func testDateRangeCompact() {
        let r = root()
        let id = make(.dateRangePicker)
        rt.insert(r, child: id, index: 0)
        listen(id, .rangeChanged)
        let v = view(id) as! AsukaDateRangeView
        set(id, AsukaProp.DateRangePicker.start, .int(Int64(day(2026, 5, 3, hour: 10).timeIntervalSince1970 * 1000)))
        set(id, AsukaProp.DateRangePicker.end, .int(Int64(day(2026, 5, 6, hour: 10).timeIntervalSince1970 * 1000)))
        XCTAssertEqual(Calendar.current.startOfDay(for: v.endPicker.minimumDate!), day(2026, 5, 3))
        XCTAssertNotNil(v.accessibilityLabel)
        _ = sent()
        v.startPicker.date = day(2026, 5, 9, hour: 8)
        v.startPicker.sendActions(for: .valueChanged)
        let e = sent(.rangeChanged)
        XCTAssertEqual(e.count, 1)
        var rd = Reader(e[0].payload)
        XCTAssertEqual(rd.f64(), day(2026, 5, 9).timeIntervalSince1970 * 1000)
        XCTAssertEqual(rd.f64(), day(2026, 5, 9).timeIntervalSince1970 * 1000, "the end followed the start")
        let s = rt.measure(id, .nan, .nan, 390, .infinity)
        XCTAssertGreaterThan(s.width, 150)
        XCTAssertGreaterThan(s.height, 20)
        // Laid out start, dash, end; the other way right to left.
        frame(id, CGRect(x: 0, y: 0, width: 390, height: s.height))
        v.layoutIfNeeded()
        XCTAssertLessThan(v.startPicker.frame.maxX, v.dash.frame.minX)
        XCTAssertLessThan(v.dash.frame.maxX, v.endPicker.frame.minX)
        v.semanticContentAttribute = .forceRightToLeft
        v.setNeedsLayout()
        v.layoutIfNeeded()
        XCTAssertGreaterThan(v.startPicker.frame.minX, v.endPicker.frame.maxX)
        XCTAssertEqual(v.startPicker.frame.maxX, 390, accuracy: 0.5)
    }

    /// Inline: a calendar, the range's days marked on it, measured at its width.
    func testDateRangeInline() {
        let r = root()
        let id = make(.dateRangePicker)
        set(id, AsukaProp.DateRangePicker.inline, .bool(true))
        rt.insert(r, child: id, index: 0)
        let v = view(id) as! AsukaDateRangeView
        set(id, AsukaProp.DateRangePicker.start, .int(Int64(day(2026, 7, 1).timeIntervalSince1970 * 1000)))
        set(id, AsukaProp.DateRangePicker.end, .int(Int64(day(2026, 7, 5).timeIntervalSince1970 * 1000)))
        let cal = try! XCTUnwrap(v.calendarView)
        let multi = try! XCTUnwrap(cal.selectionBehavior as? UICalendarSelectionMultiDate)
        XCTAssertEqual(multi.selectedDates.count, 5)
        let s = rt.measure(id, .nan, .nan, 350, .infinity)
        XCTAssertEqual(s.width, 350)
        XCTAssertGreaterThan(s.height, 300)
    }

    // MARK: Disclosure

    func testDisclosure() {
        let r = root()
        let id = make(.disclosure)
        set(id, AsukaProp.Disclosure.title, .string("More options"))
        rt.insert(r, child: id, index: 0)
        listen(id, .toggled)
        let s = rt.measure(id, .nan, .nan, 350, .infinity)
        XCTAssertEqual(s.width, 350, "as wide as it's laid out")
        XCTAssertGreaterThanOrEqual(s.height, 44)
        frame(id, CGRect(x: 0, y: 0, width: 350, height: s.height))
        let d = view(id) as! AsukaDisclosureView
        d.layoutIfNeeded()
        XCTAssertEqual(d.titleLabel.frame.minX, 0)
        XCTAssertEqual(d.chevron.frame.maxX, 350, accuracy: 0.5)
        _ = sent()
        d.sendActions(for: .touchUpInside)
        let e = sent()
        XCTAssertEqual(e.map(\.kind), [AsukaEventKind.toggled.rawValue])
        XCTAssertEqual(e.first?.payload, [1])
        XCTAssertTrue(d.expanded)
        XCTAssertEqual(atan2(d.chevron.transform.b, d.chevron.transform.a), .pi / 2, accuracy: 0.01)
        if #available(iOS 18.0, *) { XCTAssertEqual(d.accessibilityExpandedStatus, .expanded) }
        set(id, AsukaProp.Disclosure.expanded, .bool(false))
        XCTAssertEqual(d.chevron.transform, .identity)
        XCTAssertTrue(d.accessibilityTraits.contains(.button))
    }

    // MARK: Suggestions

    func field(in r: UInt32, _ list: [String], dropdown: Bool = false) -> (UInt32, AsukaTextField) {
        let id = make(.textField)
        bytes(strings(list)) { set(id, AsukaProp.TextField.suggestions, .bytes($0)) }
        if dropdown { set(id, AsukaProp.TextField.dropdown, .bool(true)) }
        rt.insert(r, child: id, index: 0)
        frame(id, CGRect(x: 20, y: 120, width: 300, height: 36))
        listen(id, .textChanged, .suggestion)
        return (id, view(id) as! AsukaTextField)
    }

    /// The list shows once the user types (not on focus alone), as the
    /// core filters it; a choice puts its text in the field, reported as
    /// the choice and not typing, and closes it until they type again.
    func testSuggestionsShowAfterTyping() {
        let r = root()
        let (id, t) = field(in: r, ["Paris", "Prague"])
        let s = try! XCTUnwrap(t.suggestions)
        XCTAssertTrue(t.becomeFirstResponder())
        XCTAssertFalse(s.isShowing, "not on focus alone")
        t.text = "P"
        t.sendActions(for: .editingChanged)
        XCTAssertTrue(s.isShowing)
        XCTAssertTrue(s.popup.superview === window)
        XCTAssertEqual(s.table.numberOfRows(inSection: 0), 2)
        let f = s.popup.frame, ff = t.convert(t.bounds, to: window)
        XCTAssertGreaterThan(f.minY, ff.maxY, "under the field")
        XCTAssertEqual(f.width, ff.width)
        XCTAssertEqual(f.height, s.table.contentSize.height, accuracy: 0.5, "as tall as its rows")
        XCTAssertGreaterThanOrEqual(f.height, 88, "a list's rows, 44 points at least")
        for c in s.table.visibleCells {
            c.layoutIfNeeded()
            func label(_ v: UIView) -> UILabel? { (v as? UILabel) ?? v.subviews.lazy.compactMap(label).first }
            let l = try! XCTUnwrap(label(c))
            XCTAssertGreaterThanOrEqual(l.bounds.height, l.font.lineHeight - 0.5, "its text whole")
        }
        // The core's new list, in place; empty, it hides.
        bytes(strings(["Paris"])) { set(id, AsukaProp.TextField.suggestions, .bytes($0)) }
        XCTAssertEqual(s.table.numberOfRows(inSection: 0), 1)
        bytes(strings([])) { set(id, AsukaProp.TextField.suggestions, .bytes($0)) }
        XCTAssertFalse(s.isShowing)
        bytes(strings(["Paris", "Prague"])) { set(id, AsukaProp.TextField.suggestions, .bytes($0)) }
        XCTAssertTrue(s.isShowing)
        _ = sent()
        s.tableView(s.table, didSelectRowAt: IndexPath(row: 1, section: 0))
        XCTAssertEqual(t.text, "Prague")
        XCTAssertFalse(s.isShowing)
        let e = sent().filter { $0.id == id }
        XCTAssertEqual(e.map(\.kind), [AsukaEventKind.suggestion.rawValue])
        XCTAssertEqual(e.first?.int, 1)
        // Not again until they type.
        bytes(strings(["Prague"])) { set(id, AsukaProp.TextField.suggestions, .bytes($0)) }
        XCTAssertFalse(s.isShowing)
        // The arrow keys, then Return, choose; Escape closes.
        t.sendActions(for: .editingChanged)
        XCTAssertTrue(s.isShowing)
        s.move(1)
        XCTAssertEqual(s.highlighted, 0)
        _ = sent()
        XCTAssertFalse(rt.textFieldShouldReturn(t))
        XCTAssertEqual(sent().filter { $0.id == id }.map(\.kind), [AsukaEventKind.suggestion.rawValue])
        t.sendActions(for: .editingChanged)
        s.dismiss()
        XCTAssertFalse(s.isShowing)
        // Leaving the field hides it.
        t.sendActions(for: .editingChanged)
        XCTAssertTrue(s.isShowing)
        t.resignFirstResponder()
        XCTAssertFalse(s.isShowing)
    }

    /// Short of room under the field (the keyboard), the list goes above it.
    func testSuggestionsFlipAbove() {
        let field = CGRect(x: 20, y: 500, width: 300, height: 36)
        let under = AsukaSuggestions.frame(field: field, want: 132, below: 300, above: 400, gap: 6)
        XCTAssertEqual(under, CGRect(x: 20, y: 542, width: 300, height: 132))
        let above = AsukaSuggestions.frame(field: field, want: 132, below: 60, above: 400, gap: 6)
        XCTAssertEqual(above, CGRect(x: 20, y: 500 - 6 - 132, width: 300, height: 132))
        let squeezed = AsukaSuggestions.frame(field: field, want: 242, below: 120, above: 90, gap: 6)
        XCTAssertEqual(squeezed.minY, 542, "more room under it: under, shorter")
        XCTAssertEqual(squeezed.height, 120)
    }

    /// Dropdown: a chevron at the field's end whose menu lists the
    /// suggestions as they are.
    func testSuggestionsDropdown() {
        let r = root()
        let (_, t) = field(in: r, ["Kyoto", "Osaka"], dropdown: true)
        let b = try! XCTUnwrap(t.rightView as? AsukaDropdownButton)
        XCTAssertEqual(t.rightViewMode, .always)
        XCTAssertTrue(b.showsMenuAsPrimaryAction)
        XCTAssertTrue(b.isEnabled)
        t.suggestions?.set([])
        XCTAssertFalse(b.isEnabled, "nothing to list")
    }

    // MARK: Image caches

    /// Default keeps an image in memory (and on disk when fetched);
    /// Memory in memory alone; None nowhere.
    func testImageCachePolicy() throws {
        XCTAssertTrue(AsukaImageLoader.policy(AsukaImageCache.default.rawValue) == (true, true))
        XCTAssertTrue(AsukaImageLoader.policy(AsukaImageCache.memory.rawValue) == (true, false))
        XCTAssertTrue(AsukaImageLoader.policy(AsukaImageCache.none.rawValue) == (false, false))
        let png = UIGraphicsImageRenderer(size: CGSize(width: 4, height: 4)).pngData { ctx in
            UIColor.red.setFill()
            ctx.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
        }
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("asuka-image-test-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let loader = AsukaImageLoader(disk: AsukaDiskCache("asuka-image-test", limit: 1 << 20))
        for (policy, kept) in [(AsukaImageCache.none, false), (.memory, true), (.default, true)] {
            let file = dir.appendingPathComponent("\(policy.rawValue).png")
            try png.write(to: file)
            var got: UIImage?
            loader.load(file.path, cache: policy.rawValue) { got = try? $0.get() }
            waitUntil { got != nil }
            XCTAssertEqual(loader.cached(file.path) != nil, kept, "\(policy)")
        }
        loader.clear(memory: true, disk: false)
        XCTAssertNil(loader.cached(dir.appendingPathComponent("0.png").path))
        // A response that says no-store stays off the disk.
        let url = URL(string: "https://example.com/a.png")!
        let noStore = HTTPURLResponse(url: url, statusCode: 200, httpVersion: nil, headerFields: ["Cache-Control": "no-store"])!
        let ok = HTTPURLResponse(url: url, statusCode: 200, httpVersion: nil, headerFields: ["Cache-Control": "max-age=60"])!
        XCTAssertFalse(AsukaDiskCache.storable(url, noStore))
        XCTAssertTrue(AsukaDiskCache.storable(url, ok))
        loader.disk.write("k", Data([1, 2, 3]))
        XCTAssertEqual(loader.disk.read("k"), Data([1, 2, 3]))
        loader.disk.removeAll()
        XCTAssertNil(loader.disk.read("k"))
    }

    /// An Image's Headers and Cache are kept for the loads that follow.
    func testImageHeadersAndCacheProps() {
        let id = make(.image)
        bytes(strings(["Authorization", "Bearer x", "X-A", "1"])) { set(id, AsukaProp.Image.headers, .bytes($0)) }
        set(id, AsukaProp.Image.cache, .enumValue(AsukaImageCache.memory.rawValue))
        let iv = view(id) as! AsukaImageView
        XCTAssertEqual(iv.headers, ["Authorization": "Bearer x", "X-A": "1"])
        XCTAssertEqual(iv.cachePolicy, AsukaImageCache.memory.rawValue)
    }

    /// The image module's prefetch: its URLs, then its headers; what runs
    /// short is left out.
    func testImageModulePrefetchArgs() {
        var w = AsukaWriter()
        w.bytes.append(contentsOf: strings(["https://a/1.jpg", "b.png"]))
        w.bytes.append(contentsOf: strings(["Authorization", "Bearer t"]))
        let (urls, headers) = AsukaImageModule.decodePrefetch(Data(w.bytes))
        XCTAssertEqual(urls, ["https://a/1.jpg", "b.png"])
        XCTAssertEqual(headers, ["Authorization": "Bearer t"])
        let short = AsukaImageModule.decodePrefetch(Data(w.bytes.prefix(10)))
        XCTAssertEqual(short.urls, [])
        XCTAssertEqual(short.headers, [:])
        let none = AsukaImageModule.decodePrefetch(Data())
        XCTAssertEqual(none.urls, [])
        // Without headers.
        let alone = AsukaImageModule.decodePrefetch(Data(strings(["x.png"])))
        XCTAssertEqual(alone.urls, ["x.png"])
        XCTAssertEqual(alone.headers, [:])
        // Both methods answer.
        let m = AsukaImageModule.methods(AsukaImageLoader(disk: AsukaDiskCache("asuka-image-test", limit: 1 << 20)))
        var answers = 0
        m["clearCache"]?(Data([1])) { if case .success = $0 { answers += 1 } }
        m["prefetch"]?(Data(strings(["/nonexistent/x.png"]) + strings([]))) { if case .success = $0 { answers += 1 } }
        XCTAssertEqual(answers, 2)
    }
}
#endif
