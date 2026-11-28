// A DateRangePicker on a Mac: two NSDatePickers (the first day and the
// last, as text fields with steppers) with an en dash between, or, inline,
// NSDatePicker's graphical calendar in its range mode (dateValue the first
// day, timeInterval to the last), the range marked by AppKit: a click
// chooses a day, a drag or a shift-click a range. Days are the device's,
// in its time zone and calendar: reported as their local midnights.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A DateRangePicker's view: its pickers, laid out in a row (the start
/// first, at the right reading right to left), or its calendar.
final class AsukaDateRangeView: NSView, AsukaCompound {
    /// The compact pickers and the dash between them; nil inline.
    private(set) var first: NSDatePicker?
    private(set) var last: NSDatePicker?
    private(set) var dash: NSTextField?
    /// The inline calendar; nil compact.
    private(set) var calendar: NSDatePicker?
    static let gap: CGFloat = 6

    /// Start, End, Min and Max (Unix ms; 0: none).
    var start: Int64 = 0, end: Int64 = 0, min: Int64 = 0, max: Int64 = 0
    /// The range last reported (or set), not to report it twice.
    var reported: (Double, Double) = (0, 0)

    override var isFlipped: Bool { true }

    /// Makes the pickers: compact, or the calendar inline.
    func build(inline: Bool, target: AnyObject, action: Selector) {
        for v in subviews { v.removeFromSuperview() }
        first = nil; last = nil; dash = nil; calendar = nil
        func picker() -> NSDatePicker {
            let d = NSDatePicker(frame: .zero)
            d.datePickerElements = .yearMonthDay
            d.isBezeled = true
            d.drawsBackground = true
            d.target = target
            d.action = action
            d.userInterfaceLayoutDirection = userInterfaceLayoutDirection
            return d
        }
        if inline {
            let c = picker()
            c.datePickerStyle = .clockAndCalendar
            c.datePickerMode = .range
            addSubview(c)
            calendar = c
        } else {
            let a = picker(), b = picker()
            a.datePickerStyle = .textFieldAndStepper
            b.datePickerStyle = .textFieldAndStepper
            // What VoiceOver calls each end (Asuka's desktop words).
            a.setAccessibilityLabel(asukaWord("startDate"))
            b.setAccessibilityLabel(asukaWord("endDate"))
            let d = NSTextField(labelWithString: "\u{2013}")
            d.textColor = .secondaryLabelColor
            d.setAccessibilityElement(false)
            addSubview(a)
            addSubview(d)
            addSubview(b)
            first = a; last = b; dash = d
        }
        apply()
        needsLayout = true
    }

    var pickers: [NSDatePicker] { [first, last, calendar].compactMap { $0 } }

    /// Shows Start, End, Min and Max: today while none is chosen.
    func apply() {
        let cal = Calendar.current
        let lo = min != 0 ? asukaDay(min) : nil
        var hi: Date?
        if max != 0, let d = cal.date(byAdding: .day, value: 1, to: asukaDay(max)) { hi = d.addingTimeInterval(-1) } // its last second
        for p in pickers {
            p.minDate = lo
            p.maxDate = hi
        }
        let s = start != 0 ? asukaDay(start) : cal.startOfDay(for: Date())
        let e = end != 0 && end >= start ? asukaDay(end) : s
        if let c = calendar {
            c.dateValue = s
            c.timeInterval = e.timeIntervalSince(s)
        } else {
            first?.dateValue = s
            last?.dateValue = e
        }
        if start != 0 { reported = (asukaMs(s), asukaMs(e)) }
    }

    /// The range shown: its first and last days' local midnights.
    func shown() -> (Date, Date) {
        let cal = Calendar.current
        if let c = calendar {
            let s = cal.startOfDay(for: c.dateValue)
            return (s, cal.startOfDay(for: c.dateValue.addingTimeInterval(Swift.max(c.timeInterval, 0))))
        }
        let s = cal.startOfDay(for: first?.dateValue ?? Date())
        let e = cal.startOfDay(for: last?.dateValue ?? Date())
        return (s, e)
    }

    /// The user changed picker p: a start past the end takes the end with
    /// it, an end before the start the start. The range, or nil if it's
    /// the one reported.
    func changed(_ p: NSDatePicker) -> (Double, Double)? {
        if calendar == nil, let a = first, let b = last {
            let cal = Calendar.current
            let s = cal.startOfDay(for: a.dateValue), e = cal.startOfDay(for: b.dateValue)
            if s > e {
                if p === a { b.dateValue = s } else { a.dateValue = e }
            }
        }
        let (s, e) = shown()
        let r = (asukaMs(s), asukaMs(e))
        if r == reported { return nil }
        reported = r
        return r
    }

    override var userInterfaceLayoutDirection: NSUserInterfaceLayoutDirection {
        didSet {
            for v in subviews { v.userInterfaceLayoutDirection = userInterfaceLayoutDirection }
            needsLayout = true
        }
    }

    /// The pickers in a row centered down its height, the first at its
    /// start; the calendar at its top start.
    override func layout() {
        super.layout()
        let rtl = userInterfaceLayoutDirection == .rightToLeft
        if let c = calendar {
            let s = c.fittingSize
            c.frame = CGRect(x: rtl ? bounds.width - s.width : 0, y: 0, width: s.width, height: s.height)
            return
        }
        guard let a = first, let b = last, let d = dash else { return }
        var x: CGFloat = 0
        for v in [a, d, b] as [NSView] {
            let s = v.fittingSize
            let fx = rtl ? bounds.width - x - s.width : x
            v.frame = CGRect(x: fx, y: ((bounds.height - s.height) / 2).rounded(), width: s.width, height: s.height)
            x += s.width + Self.gap
        }
    }

    func fit() -> CGSize {
        if let c = calendar { return c.fittingSize }
        guard let a = first, let b = last, let d = dash else { return .zero }
        let sa = a.fittingSize, sb = b.fittingSize, sd = d.fittingSize
        return CGSize(width: sa.width + sd.width + sb.width + 2 * Self.gap, height: Swift.max(sa.height, sb.height, sd.height))
    }

    func setEnabled(_ on: Bool) {
        for p in pickers { p.isEnabled = on }
        dash?.textColor = on ? .secondaryLabelColor : .disabledControlTextColor
    }

    func setFont(_ f: NSFont) {
        for p in pickers where p !== calendar { p.font = f }
        dash?.font = f
        needsLayout = true
    }

    func setColor(_ c: NSColor?) {
        for p in pickers { p.textColor = c ?? .controlTextColor }
    }
}

/// The local midnight of the day ms (Unix milliseconds) is in.
func asukaDay(_ ms: Int64) -> Date {
    Calendar.current.startOfDay(for: Date(timeIntervalSince1970: Double(ms) / 1000))
}

func asukaMs(_ d: Date) -> Double { (d.timeIntervalSince1970 * 1000).rounded() }

@MainActor extension AsukaRuntime {
    func makeDateRange(_ n: AsukaNode) -> NSView {
        let v = AsukaDateRangeView(frame: .zero)
        v.build(inline: false, target: self, action: #selector(rangeChanged(_:)))
        return v
    }

    func setDateRangeProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let r = n.view as? AsukaDateRangeView else { return }
        switch p {
        case AsukaProp.DateRangePicker.start: r.start = v.int
        case AsukaProp.DateRangePicker.end: r.end = v.int
        case AsukaProp.DateRangePicker.min: r.min = v.int
        case AsukaProp.DateRangePicker.max: r.max = v.int
        case AsukaProp.DateRangePicker.inline:
            if v.bool != (r.calendar != nil) {
                r.build(inline: v.bool, target: self, action: #selector(rangeChanged(_:)))
                applyFont(n)
                applyColor(n)
            }
            return
        default: return
        }
        r.apply()
    }

    /// A picker's change: the range, once it's another.
    @objc func rangeChanged(_ sender: Any?) {
        guard let p = sender as? NSDatePicker, let r = p.superview as? AsukaDateRangeView, let n = node(of: r) else { return }
        guard let (s, e) = r.changed(p) else { return }
        emit(n, .rangeChanged) { AsukaEvents.rangeChanged($0, x: s, y: e) }
    }

    func dateRangeJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let r = n.view as? AsukaDateRangeView else { return }
        let (s, e) = r.shown()
        var j: [String: Any] = ["inline": r.calendar != nil, "start": asukaMs(s), "end": asukaMs(e),
                                "pickers": r.pickers.map { [Double($0.frame.minX), Double($0.frame.minY), Double($0.frame.width), Double($0.frame.height)] }]
        j["labels"] = r.pickers.map { $0.accessibilityLabel() ?? "" }
        if let m = r.pickers.first?.minDate { j["minDate"] = asukaMs(m) }
        if let m = r.pickers.first?.maxDate { j["maxDate"] = (m.timeIntervalSince1970 * 1000).rounded() }
        d["dateRange"] = j
    }

    /// The automation's: the user sets a picker of a DateRangePicker
    /// (0 the first, 1 the last; the calendar: the range from the first to
    /// the day ms is in), as its action reports.
    func rangeStep(_ n: AsukaNode, _ which: Int, _ ms: Int64) -> String? {
        guard let r = n.view as? AsukaDateRangeView else { return "not a DateRangePicker" }
        if let c = r.calendar {
            let day = asukaDay(ms)
            if which == 0 {
                c.dateValue = day
                c.timeInterval = 0
            } else {
                c.timeInterval = Swift.max(day.timeIntervalSince(Calendar.current.startOfDay(for: c.dateValue)), 0)
            }
            rangeChanged(c)
            return nil
        }
        guard let p = which == 0 ? r.first : r.last else { return "no picker" }
        p.dateValue = asukaDay(ms)
        rangeChanged(p)
        return nil
    }
}
