#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// The days a range of a calendar's taps has chosen: the first tap starts
/// one, the second (on that day or later) ends it, a third starts again;
/// a second tap before the first moves the start there, as Material's
/// range calendar does. Days are local midnights in the calendar.
struct AsukaRangeSelection {
    var calendar: Calendar
    /// The range's first day, and its last once it's ended.
    private(set) var start: Date?
    private(set) var end: Date?

    init(calendar: Calendar = .current) { self.calendar = calendar }

    /// A tap on the day of date: whether it ended a range.
    mutating func tap(_ date: Date) -> Bool {
        let day = calendar.startOfDay(for: date)
        if let s = start, end == nil, day >= s {
            end = day
            return true
        }
        start = day
        end = nil
        return false
    }

    /// The range as the app sets it (nil: none chosen; an end alone is none).
    mutating func set(start s: Date?, end e: Date?) {
        start = s.map { calendar.startOfDay(for: $0) }
        end = start == nil ? nil : e.map { max(calendar.startOfDay(for: $0), start!) }
    }

    /// The days it marks: the start alone, or each from the start to the end.
    var days: [Date] {
        guard let s = start else { return [] }
        guard let e = end else { return [s] }
        var out: [Date] = []
        var d = s
        while d <= e, out.count < 3660 { // ten years of days at most
            out.append(d)
            guard let next = calendar.date(byAdding: .day, value: 1, to: d) else { break }
            d = calendar.startOfDay(for: next)
        }
        return out
    }
}

extension UIDatePicker {
    /// A compact picker's own size: its fitting size, rounded up to a
    /// whole point and a point to spare. A sub-point short of its text
    /// ("Nov 30, 2026"), UIKit shows the short format ("11/30/26").
    var asukaCompactSize: CGSize {
        let s = systemLayoutSizeFitting(UIView.layoutFittingCompressedSize)
        return CGSize(width: ceil(s.width) + 1, height: s.height)
    }
}

/// Unix milliseconds as a Date (0: none).
func asukaDate(_ ms: Int64) -> Date? { ms == 0 ? nil : Date(timeIntervalSince1970: Double(ms) / 1000) }

/// A Date's local midnight in Unix milliseconds, as RangeChanged reports.
func asukaMidnight(_ d: Date, _ cal: Calendar = .current) -> Double {
    (cal.startOfDay(for: d).timeIntervalSince1970 * 1000).rounded()
}

/// A DateRangePicker: two compact UIDatePickers, the start's and the
/// end's, an en dash between (the end's earliest the start's day), or an
/// inline UICalendarView whose taps choose a range (iOS 16; tvOS, which
/// has no calendar view, the two pickers). What's shown is today while
/// none is chosen, reported only once the user changes it. VoiceOver
/// reads the group as the range, then each picker.
final class AsukaDateRangeView: UIControl {
    let startPicker = UIDatePicker()
    let endPicker = UIDatePicker()
    let dash = UILabel()
    var calendar = Calendar.current
    #if !os(tvOS)
    private(set) var calendarView: UICalendarView?
    private var multi: UICalendarSelectionMultiDate?
    private var shownDays: [DateComponents] = []
    #endif
    var selection = AsukaRangeSelection()
    /// As the app last set them.
    var start: Date? { didSet { show() } }
    var end: Date? { didSet { show() } }
    var minDate: Date? { didSet { bounds_() } }
    var maxDate: Date? { didSet { bounds_() } }
    var inline = false { didSet { if inline != oldValue { makeInline() } } }
    /// Reports a range the user chose: its first and last days.
    var changed: ((Date, Date) -> Void)?

    override init(frame: CGRect) {
        super.init(frame: frame)
        autoresizesSubviews = false
        for p in [startPicker, endPicker] {
            p.datePickerMode = .date
            p.preferredDatePickerStyle = .compact
            p.autoresizingMask = []
            addSubview(p)
        }
        dash.text = "\u{2013}"
        dash.font = .preferredFont(forTextStyle: .body)
        dash.adjustsFontForContentSizeCategory = true
        dash.textColor = .secondaryLabel
        dash.isAccessibilityElement = false
        addSubview(dash)
        startPicker.addAction(UIAction { [weak self] _ in self?.picked(start: true) }, for: .valueChanged)
        endPicker.addAction(UIAction { [weak self] _ in self?.picked(start: false) }, for: .valueChanged)
        isAccessibilityElement = false
        accessibilityContainerType = .semanticGroup
        show()
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isEnabled: Bool {
        didSet {
            startPicker.isEnabled = isEnabled
            endPicker.isEnabled = isEnabled
            #if !os(tvOS)
            calendarView?.alpha = isEnabled ? 1 : 0.5
            calendarView?.isUserInteractionEnabled = isEnabled
            #endif
        }
    }

    override var tintColor: UIColor! {
        get { super.tintColor }
        set {
            super.tintColor = newValue
            startPicker.tintColor = newValue
            endPicker.tintColor = newValue
            #if !os(tvOS)
            calendarView?.tintColor = newValue
            #endif
        }
    }

    /// A picker changed: the other follows (a start past the end moves the
    /// end to it, an end before the start moves the start), and the pair
    /// is reported.
    private func picked(start isStart: Bool) {
        let s = calendar.startOfDay(for: startPicker.date)
        var e = calendar.startOfDay(for: endPicker.date)
        if isStart, e < s { e = s; endPicker.date = s }
        if !isStart, e < s { startPicker.date = e }
        let first = calendar.startOfDay(for: startPicker.date)
        e = max(e, first)
        endPicker.minimumDate = max(first, minDate ?? first)
        describe(first, e)
        setNeedsLayout() // the dates' text has its own width
        changed?(first, e)
    }

    /// Shows the range the app set (today while there's none).
    private func show() {
        let today = calendar.startOfDay(for: Date())
        let s = start ?? today
        let e = max(end ?? s, s)
        startPicker.date = s
        endPicker.minimumDate = max(s, minDate ?? s)
        endPicker.date = e
        selection.set(start: start, end: end)
        #if !os(tvOS)
        if calendarView != nil { markDays() }
        #endif
        describe(start, end)
        setNeedsLayout()
    }

    private func bounds_() {
        startPicker.minimumDate = minDate
        startPicker.maximumDate = maxDate
        endPicker.maximumDate = maxDate
        endPicker.minimumDate = max(calendar.startOfDay(for: startPicker.date), minDate ?? .distantPast)
        #if !os(tvOS)
        calendarView?.availableDateRange = available
        #endif
    }

    /// The group's name: the range, as the user's locale writes one.
    private func describe(_ s: Date?, _ e: Date?) {
        guard let s else { accessibilityLabel = nil; return }
        let f = DateIntervalFormatter()
        f.dateStyle = .medium
        f.timeStyle = .none
        accessibilityLabel = f.string(from: s, to: e ?? s)
    }

    // MARK: Inline

    #if !os(tvOS)
    private var available: DateInterval {
        let lo = minDate.map { calendar.startOfDay(for: $0) } ?? .distantPast
        var hi = maxDate.map { calendar.date(byAdding: DateComponents(day: 1, second: -1), to: calendar.startOfDay(for: $0)) ?? $0 } ?? .distantFuture
        if hi < lo { hi = lo }
        return DateInterval(start: lo, end: hi)
    }

    private func makeInline() {
        guard inline else {
            calendarView?.removeFromSuperview()
            calendarView = nil
            multi = nil
            for v in [startPicker, endPicker, dash] { v.isHidden = false }
            return
        }
        for v in [startPicker, endPicker, dash] { v.isHidden = true }
        let c = UICalendarView()
        c.calendar = calendar
        c.locale = .current
        c.fontDesign = .default
        c.availableDateRange = available
        let m = UICalendarSelectionMultiDate(delegate: self)
        c.selectionBehavior = m
        c.autoresizingMask = []
        addSubview(c)
        calendarView = c
        multi = m
        markDays()
        if let s = selection.start { c.visibleDateComponents = calendar.dateComponents([.calendar, .era, .year, .month], from: s) }
        setNeedsLayout()
    }

    /// Marks the range's days on the calendar.
    private func markDays() {
        let comps = selection.days.map { calendar.dateComponents([.calendar, .era, .year, .month, .day], from: $0) }
        guard comps != shownDays else { return }
        shownDays = comps
        multi?.setSelectedDates(comps, animated: window != nil)
    }

    /// A day tapped (selected or, already in the range, deselected).
    fileprivate func tapped(_ day: DateComponents) {
        guard let d = calendar.date(from: day) else { return }
        let ended = selection.tap(d)
        shownDays = []
        markDays()
        describe(selection.start, selection.end)
        if ended, let s = selection.start, let e = selection.end { changed?(s, e) }
    }
    #else
    private func makeInline() {} // tvOS: no calendar view, the two pickers
    #endif

    // MARK: Measuring and layout

    /// Its own size: the two compact pickers and the dash, or the
    /// calendar at the width it's given.
    override func sizeThatFits(_ size: CGSize) -> CGSize {
        #if !os(tvOS)
        if let c = calendarView {
            let w = size.width.isFinite && size.width < 10_000 ? size.width : 360
            let s = c.systemLayoutSizeFitting(CGSize(width: w, height: UIView.layoutFittingCompressedSize.height),
                                              withHorizontalFittingPriority: .required, verticalFittingPriority: .fittingSizeLevel)
            return CGSize(width: w, height: ceil(s.height))
        }
        #endif
        let a = widestPicker()
        let d = dash.sizeThatFits(CGSize(width: CGFloat.greatestFiniteMagnitude, height: .greatestFiniteMagnitude))
        return CGSize(width: 2 * a.width + d.width + 2 * Self.gap, height: max(a.height, d.height))
    }

    static let gap: CGFloat = 6

    /// The widest a compact picker shows a date of this year (each month's
    /// 28th): what's measured, so the core, which measures it once, has
    /// room for whichever dates are chosen. Kept for the text size and
    /// language it was found in.
    private var widest: (key: String, size: CGSize)?
    private func widestPicker() -> CGSize {
        let key = "\(Locale.current.identifier) \(asukaContentSize.rawValue) \(traitCollection.preferredContentSizeCategory.rawValue)"
        if let w = widest, w.key == key { return w.size }
        let shown = startPicker.date, lo = startPicker.minimumDate, hi = startPicker.maximumDate
        startPicker.minimumDate = nil
        startPicker.maximumDate = nil
        var size = startPicker.asukaCompactSize
        let year = calendar.component(.year, from: Date())
        for m in 1...12 {
            guard let d = calendar.date(from: DateComponents(year: year, month: m, day: 28)) else { continue }
            startPicker.date = d
            let s = startPicker.asukaCompactSize
            size = CGSize(width: max(size.width, s.width), height: max(size.height, s.height))
        }
        startPicker.minimumDate = lo
        startPicker.maximumDate = hi
        startPicker.date = shown
        widest = (key, size)
        return size
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        // Compact pickers compute NaN frames from a zero size: laid out
        // only once there's room.
        guard bounds.width > 0, bounds.height > 0 else { return }
        #if !os(tvOS)
        if let c = calendarView {
            c.frame = bounds
            return
        }
        #endif
        // From the start: start, dash, end, the other way in a right-to-left one.
        let a = startPicker.asukaCompactSize
        let b = endPicker.asukaCompactSize
        let d = dash.sizeThatFits(CGSize(width: CGFloat.greatestFiniteMagnitude, height: .greatestFiniteMagnitude))
        let rtl = effectiveUserInterfaceLayoutDirection == .rightToLeft
        var x: CGFloat = rtl ? bounds.width : 0
        for (v, s) in [(startPicker as UIView, a), (dash, d), (endPicker, b)] {
            let w = s.width
            let at = rtl ? x - w : x
            v.frame = CGRect(x: at, y: (bounds.height - s.height) / 2, width: w, height: s.height)
            x = rtl ? at - Self.gap : at + w + Self.gap
        }
    }
}

#if !os(tvOS)
extension AsukaDateRangeView: UICalendarSelectionMultiDateDelegate {
    func multiDateSelection(_ selection: UICalendarSelectionMultiDate, didSelectDate dateComponents: DateComponents) {
        tapped(dateComponents)
    }

    func multiDateSelection(_ selection: UICalendarSelectionMultiDate, didDeselectDate dateComponents: DateComponents) {
        tapped(dateComponents)
    }
}
#endif

@MainActor extension AsukaRuntime {
    func makeDateRange(_ id: UInt32) -> UIView {
        let v = AsukaDateRangeView()
        v.changed = { [weak self, weak v] s, e in
            let cal = v?.calendar ?? .current
            let x = asukaMidnight(s, cal), y = asukaMidnight(e, cal)
            self?.emit(id, .rangeChanged) { AsukaEvents.rangeChanged(id, x: x, y: y) }
        }
        return v
    }

    func setDateRangeProp(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) -> Bool {
        guard n.kind == .dateRangePicker, let r = n.view as? AsukaDateRangeView else { return false }
        switch prop {
        case AsukaProp.DateRangePicker.start: r.start = asukaDate(v.int)
        case AsukaProp.DateRangePicker.end: r.end = asukaDate(v.int)
        case AsukaProp.DateRangePicker.min: r.minDate = asukaDate(v.int)
        case AsukaProp.DateRangePicker.max: r.maxDate = asukaDate(v.int)
        case AsukaProp.DateRangePicker.inline: r.inline = v.bool
        default: return false
        }
        return true
    }
}
#endif
