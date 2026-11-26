#if os(tvOS)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// tvOS: the iOS host, built with the Apple TV SDK. tvOS is UIKit without
// touch: the focus engine moves the focus among focusable views as the
// Siri Remote swipes and clicks, and Select presses what has it. Its
// UIKit lacks some of iOS's controls and services; this file stands in
// for them under their iOS names, so the rest of the host is one source:
//
//   - UISwitch is a checkmark button (the system button's focus effect),
//     as tvOS's own settings show an on/off choice;
//   - UISlider, UIStepper and UIDatePicker are adjustable controls: focus
//     one, then click or swipe left and right to step its value (a
//     slider's by a twentieth of its range, a stepper's by its step, a
//     date's by a day, a time's by a minute);
//   - UIColorWell shows its color, which only the app changes;
//   - UIPasteboard is empty: tvOS has no clipboard;
//   - the system colors iOS has and tvOS doesn't (systemBackground and
//     its kin) are tvOS's own light and dark backgrounds;
//   - UIView.focusEffect (iOS's focus halo) is the lift every focusable
//     view of the app's shows: it grows and casts a shadow while it has
//     the focus, as tvOS's cards and buttons do.

// MARK: - Colors

extension UIColor {
    private static func tv(_ light: UIColor, _ dark: UIColor) -> UIColor {
        UIColor { $0.userInterfaceStyle == .dark ? dark : light }
    }
    @objc(asuka_systemBackground) class var systemBackground: UIColor { tv(UIColor(white: 0.92, alpha: 1), .black) }
    @objc(asuka_secondarySystemBackground) class var secondarySystemBackground: UIColor { tv(.white, UIColor(white: 0.11, alpha: 1)) }
    @objc(asuka_tertiarySystemBackground) class var tertiarySystemBackground: UIColor { tv(UIColor(white: 0.96, alpha: 1), UIColor(white: 0.17, alpha: 1)) }
    @objc(asuka_systemGroupedBackground) class var systemGroupedBackground: UIColor { systemBackground }
    @objc(asuka_secondarySystemGroupedBackground) class var secondarySystemGroupedBackground: UIColor { secondarySystemBackground }
    @objc(asuka_tertiarySystemGroupedBackground) class var tertiarySystemGroupedBackground: UIColor { tertiarySystemBackground }
    // iOS's grays, light and dark.
    private static func rgb(_ v: UInt32) -> UIColor {
        UIColor(red: CGFloat(v >> 16 & 255) / 255, green: CGFloat(v >> 8 & 255) / 255, blue: CGFloat(v & 255) / 255, alpha: 1)
    }
    @objc(asuka_systemGray2) class var systemGray2: UIColor { tv(rgb(0xAEAEB2), rgb(0x636366)) }
    @objc(asuka_systemGray3) class var systemGray3: UIColor { tv(rgb(0xC7C7CC), rgb(0x48484A)) }
    @objc(asuka_systemGray4) class var systemGray4: UIColor { tv(rgb(0xD1D1D6), rgb(0x3A3A3C)) }
    @objc(asuka_systemGray5) class var systemGray5: UIColor { tv(rgb(0xE5E5EA), rgb(0x2C2C2E)) }
    @objc(asuka_systemGray6) class var systemGray6: UIColor { tv(rgb(0xF2F2F7), rgb(0x1C1C1E)) }
    // The fills iOS puts behind controls: a gray, more or less of it.
    @objc(asuka_systemFill) class var systemFill: UIColor { tv(UIColor(white: 0.47, alpha: 0.2), UIColor(white: 0.47, alpha: 0.36)) }
    @objc(asuka_secondarySystemFill) class var secondarySystemFill: UIColor { tv(UIColor(white: 0.47, alpha: 0.16), UIColor(white: 0.47, alpha: 0.32)) }
    @objc(asuka_tertiarySystemFill) class var tertiarySystemFill: UIColor { tv(UIColor(white: 0.46, alpha: 0.12), UIColor(white: 0.46, alpha: 0.24)) }
    @objc(asuka_quaternarySystemFill) class var quaternarySystemFill: UIColor { tv(UIColor(white: 0.45, alpha: 0.08), UIColor(white: 0.46, alpha: 0.18)) }
}

// MARK: - The focus effect

/// UIFocusEffect stands for iOS's: tvOS's focusable views draw their own.
class UIFocusEffect: NSObject {}

extension UIView {
    /// The lift (asukaLift) is on unless set to nil (NoFocusRing).
    @objc(asuka_focusEffect) var focusEffect: UIFocusEffect? {
        get { asukaNoLift ? nil : UIFocusEffect() }
        set { asukaNoLift = newValue == nil }
    }

    private static var noLiftKey = 0
    private static var liftedKey = 0

    var asukaNoLift: Bool {
        get { objc_getAssociatedObject(self, &UIView.noLiftKey) as? Bool ?? false }
        set { objc_setAssociatedObject(self, &UIView.noLiftKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
    }

    /// The shadow the view had before the lift (its own, which the lift
    /// puts back).
    private var asukaLifted: [Any]? {
        get { objc_getAssociatedObject(self, &UIView.liftedKey) as? [Any] }
        set { objc_setAssociatedObject(self, &UIView.liftedKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
    }

    /// tvOS's focus effect for a view of the app's: it grows and casts a
    /// shadow as the focus comes, and settles as it goes, in step with the
    /// focus engine's animation.
    func asukaLift(_ focused: Bool, _ coordinator: UIFocusAnimationCoordinator?) {
        if asukaNoLift && asukaLifted == nil { return }
        let grow: CGFloat = bounds.width > 400 || bounds.height > 300 ? 1.04 : 1.1
        let apply = { [self] in
            if focused, asukaLifted == nil, !asukaNoLift {
                asukaLifted = [layer.shadowOpacity, layer.shadowRadius, layer.shadowOffset, layer.shadowColor as Any, grow]
                transform = transform.scaledBy(x: grow, y: grow)
                layer.shadowColor = UIColor.black.cgColor
                layer.shadowOpacity = 0.35
                layer.shadowRadius = 16
                layer.shadowOffset = CGSize(width: 0, height: 12)
            } else if !focused, let was = asukaLifted {
                asukaLifted = nil
                let g = was[4] as? CGFloat ?? 1
                transform = transform.scaledBy(x: 1 / g, y: 1 / g)
                layer.shadowOpacity = was[0] as? Float ?? 0
                layer.shadowRadius = was[1] as? CGFloat ?? 3
                layer.shadowOffset = was[2] as? CGSize ?? CGSize(width: 0, height: -3)
                layer.shadowColor = (was[3] as! CGColor?)
            }
        }
        if let coordinator {
            if focused { coordinator.addCoordinatedFocusingAnimations({ _ in apply() }) }
            else { coordinator.addCoordinatedUnfocusingAnimations({ _ in apply() }) }
        } else {
            apply()
        }
    }
}

// MARK: - Toggle: a checkmark button

/// A Toggle: tvOS has no switch, so it's a button that shows a checkmark
/// while on (a circle while off), which Select flips.
class UISwitch: UIButton {
    var isOn = false { didSet { update() } }
    var onTintColor: UIColor? { didSet { update() } }

    convenience init() {
        self.init(configuration: .plain())
        configuration?.preferredSymbolConfigurationForImage = UIImage.SymbolConfiguration(textStyle: .title2)
        addAction(UIAction { [weak self] _ in
            guard let self else { return }
            self.isOn.toggle()
            self.sendActions(for: .valueChanged)
        }, for: .primaryActionTriggered)
        accessibilityTraits = [.button, .toggleButton]
        update()
    }

    func setOn(_ on: Bool, animated: Bool) { isOn = on }

    private func update() {
        configuration?.image = UIImage(systemName: isOn ? "checkmark.circle.fill" : "circle")
        configuration?.baseForegroundColor = isOn ? onTintColor : .tertiaryLabel
        accessibilityValue = isOn ? "1" : "0"
    }
}

// MARK: - Adjustable controls

/// A control the remote adjusts: focused, a click or swipe left steps its
/// value down, right up (the focus stays: it moves on up and down).
class AsukaTVAdjustable: UIControl {
    override init(frame: CGRect) {
        super.init(frame: frame)
        for (dir, type) in [(UISwipeGestureRecognizer.Direction.left, UIPress.PressType.leftArrow), (.right, .rightArrow)] {
            let s = UISwipeGestureRecognizer(target: self, action: #selector(swiped(_:)))
            s.direction = dir
            addGestureRecognizer(s)
            let t = UITapGestureRecognizer(target: self, action: #selector(clicked(_:)))
            t.allowedPressTypes = [NSNumber(value: type.rawValue)]
            addGestureRecognizer(t)
        }
        isAccessibilityElement = true
        accessibilityTraits = .adjustable
    }
    required init?(coder: NSCoder) { fatalError() }

    override var canBecomeFocused: Bool { isEnabled && !isHidden }

    /// Steps the value by n steps (-1 down, 1 up): true if it changed.
    func step(_ n: Int) -> Bool { false }

    @objc private func swiped(_ g: UISwipeGestureRecognizer) { adjust(g.direction == .left ? -1 : 1) }
    @objc private func clicked(_ g: UITapGestureRecognizer) {
        let left = g.allowedPressTypes.contains(NSNumber(value: UIPress.PressType.leftArrow.rawValue))
        adjust(left ? -1 : 1)
    }

    private func adjust(_ n: Int) {
        guard isFocused else { return }
        let rtl = effectiveUserInterfaceLayoutDirection == .rightToLeft
        if step(rtl ? -n : n) { sendActions(for: .valueChanged) }
    }

    override func accessibilityIncrement() { if step(1) { sendActions(for: .valueChanged) } }
    override func accessibilityDecrement() { if step(-1) { sendActions(for: .valueChanged) } }

    /// The focus stays while left and right adjust.
    override func shouldUpdateFocus(in context: UIFocusUpdateContext) -> Bool {
        if context.previouslyFocusedItem === self, !context.focusHeading.intersection([.left, .right]).isEmpty { return false }
        return super.shouldUpdateFocus(in: context)
    }

    override func didUpdateFocus(in context: UIFocusUpdateContext, with coordinator: UIFocusAnimationCoordinator) {
        super.didUpdateFocus(in: context, with: coordinator)
        if context.nextFocusedItem === self { asukaLift(true, coordinator) }
        else if context.previouslyFocusedItem === self { asukaLift(false, coordinator) }
        coordinator.addCoordinatedAnimations({ self.focusChanged() })
    }

    /// Draws the focused state (a platter behind it while focused).
    func focusChanged() {}
}

/// A Slider: a track the remote adjusts.
class UISlider: AsukaTVAdjustable {
    private let track = UIProgressView(progressViewStyle: .default)
    var value: Float = 0 { didSet { update() } }
    var minimumValue: Float = 0 { didSet { update() } }
    var maximumValue: Float = 1 { didSet { update() } }
    var minimumTrackTintColor: UIColor? { didSet { track.progressTintColor = minimumTrackTintColor } }

    override init(frame: CGRect) {
        super.init(frame: frame)
        track.isUserInteractionEnabled = false
        track.trackTintColor = .tertiarySystemFill
        addSubview(track)
    }
    required init?(coder: NSCoder) { fatalError() }

    override func tintColorDidChange() {
        super.tintColorDidChange()
        if minimumTrackTintColor == nil { track.progressTintColor = tintColor }
    }

    override func sizeThatFits(_ size: CGSize) -> CGSize { CGSize(width: size.width, height: 44) }

    override func focusChanged() {
        // Focused, its track thickens, as the focus engine's own sliders'.
        track.transform = isFocused ? CGAffineTransform(scaleX: 1, y: 2) : .identity
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let h = track.sizeThatFits(bounds.size).height
        track.frame = CGRect(x: 0, y: (bounds.height - h) / 2, width: bounds.width, height: h)
    }

    override func step(_ n: Int) -> Bool {
        let was = value
        value = min(max(value + Float(n) * (maximumValue - minimumValue) / 20, minimumValue), maximumValue)
        return value != was
    }

    private func update() {
        let span = maximumValue - minimumValue
        track.progress = span > 0 ? (value - minimumValue) / span : 0
        accessibilityValue = "\(Int((track.progress * 100).rounded()))%"
    }
}

/// A Stepper: a minus and a plus the remote adjusts between.
class UIStepper: AsukaTVAdjustable {
    var value: Double = 0 { didSet { update() } }
    var minimumValue: Double = 0 { didSet { update() } }
    var maximumValue: Double = 100 { didSet { update() } }
    var stepValue: Double = 1
    private let minus = UIImageView(image: UIImage(systemName: "minus"))
    private let plus = UIImageView(image: UIImage(systemName: "plus"))
    private let platter = UIView()

    override init(frame: CGRect) {
        super.init(frame: frame)
        platter.isUserInteractionEnabled = false
        platter.layer.cornerRadius = 12
        platter.backgroundColor = .tertiarySystemFill
        addSubview(platter)
        for v in [minus, plus] {
            v.contentMode = .center
            v.preferredSymbolConfiguration = UIImage.SymbolConfiguration(textStyle: .headline)
            v.tintColor = .label
            addSubview(v)
        }
        update()
    }
    required init?(coder: NSCoder) { fatalError() }

    override func sizeThatFits(_ size: CGSize) -> CGSize { CGSize(width: 160, height: 66) }

    override func layoutSubviews() {
        super.layoutSubviews()
        platter.frame = bounds
        let half = bounds.width / 2
        minus.frame = CGRect(x: 0, y: 0, width: half, height: bounds.height)
        plus.frame = CGRect(x: half, y: 0, width: half, height: bounds.height)
    }

    override func focusChanged() {
        platter.backgroundColor = isFocused ? .white : .tertiarySystemFill
        let c: UIColor = isFocused ? .black : .label
        minus.tintColor = c
        plus.tintColor = c
    }

    override func step(_ n: Int) -> Bool {
        let was = value
        value = min(max(value + Double(n) * stepValue, minimumValue), maximumValue)
        return value != was
    }

    private func update() {
        minus.alpha = value > minimumValue ? 1 : 0.4
        plus.alpha = value < maximumValue ? 1 : 0.4
        accessibilityValue = value.rounded() == value ? String(Int(value)) : String(value)
    }
}

/// A DatePicker: the date (or time) the remote adjusts, a day (a minute)
/// at a time.
class UIDatePicker: AsukaTVAdjustable {
    enum Mode { case date, time, dateAndTime }
    var date = Date() { didSet { update() } }
    var datePickerMode = Mode.date { didSet { update() } }
    var preferredDatePickerStyle = UIDatePickerStyle.compact
    var minimumDate: Date? { didSet { update() } }
    var maximumDate: Date? { didSet { update() } }
    private let label = UILabel()
    private let platter = UIView()

    override init(frame: CGRect) {
        super.init(frame: frame)
        platter.isUserInteractionEnabled = false
        platter.layer.cornerRadius = 12
        platter.backgroundColor = .tertiarySystemFill
        addSubview(platter)
        label.font = .preferredFont(forTextStyle: .headline)
        label.textAlignment = .center
        addSubview(label)
        update()
    }
    required init?(coder: NSCoder) { fatalError() }

    override func sizeThatFits(_ size: CGSize) -> CGSize {
        let s = label.sizeThatFits(size)
        return CGSize(width: s.width + 64, height: max(66, s.height + 24))
    }

    override func systemLayoutSizeFitting(_ targetSize: CGSize) -> CGSize { sizeThatFits(targetSize) }

    override func layoutSubviews() {
        super.layoutSubviews()
        platter.frame = bounds
        label.frame = bounds.insetBy(dx: 16, dy: 0)
    }

    override func focusChanged() {
        platter.backgroundColor = isFocused ? .white : .tertiarySystemFill
        label.textColor = isFocused ? .black : .label
    }

    override func step(_ n: Int) -> Bool {
        let was = date
        let unit: Calendar.Component = datePickerMode == .time ? .minute : .day
        var d = Calendar.current.date(byAdding: unit, value: n, to: date) ?? date
        if let lo = minimumDate, d < lo { d = lo }
        if let hi = maximumDate, d > hi { d = hi }
        date = d
        return date != was
    }

    private func update() {
        let f = DateFormatter()
        f.locale = .current
        switch datePickerMode {
        case .date: f.dateStyle = .medium; f.timeStyle = .none
        case .time: f.dateStyle = .none; f.timeStyle = .short
        case .dateAndTime: f.dateStyle = .medium; f.timeStyle = .short
        }
        label.text = f.string(from: date)
        accessibilityValue = label.text
        invalidateIntrinsicContentSize()
    }
}

enum UIDatePickerStyle { case automatic, wheels, compact, inline }

/// A ColorPicker: its color, as a swatch; tvOS has no color picker, so
/// only the app changes it.
class UIColorWell: UIView {
    var selectedColor: UIColor? { didSet { backgroundColor = selectedColor } }
    var supportsAlpha = false
    var title: String? { didSet { accessibilityLabel = title } }

    convenience init() {
        self.init(frame: .zero)
        layer.cornerRadius = 22
        layer.borderWidth = 2
        layer.borderColor = UIColor.separator.cgColor
        isAccessibilityElement = true
    }

    func addAction(_ action: UIAction, for events: UIControl.Event) {}
    override func sizeThatFits(_ size: CGSize) -> CGSize { CGSize(width: 44, height: 44) }
}

// MARK: - Text views

extension UITextView {
    /// tvOS's text views show text; its fields edit it.
    @objc(asuka_isEditable) var isEditable: Bool { get { false } set {} }
    var dataDetectorTypes: UIDataDetectorTypes { get { [] } set {} }
}

/// tvOS's text views detect nothing.
struct UIDataDetectorTypes: OptionSet {
    let rawValue: UInt
}

// MARK: - Scrolling

extension UIScrollView {
    /// tvOS pages by the focus: a paging Scroll scrolls as the focus
    /// moves through its pages.
    @objc(asuka_isPagingEnabled) var isPagingEnabled: Bool { get { false } set {} }
}

// MARK: - No drag and drop, no pointer

/// tvOS has no drag and drop: a View's DropAccept and DragItems do
/// nothing, and these are never made.
final class UIDropInteraction {}
final class UIDragInteraction {}

extension NSItemProvider {
    @objc(asuka_suggestedName) var suggestedName: String? { get { nil } set {} }
}

// MARK: - Wheel: a menu button

/// A Wheel: tvOS has no picker wheel, so it's a button showing the chosen
/// item, whose menu chooses another (as a Picker's).
final class AsukaWheelView: UIButton {
    var items: [String] = [] { didSet { refresh() } }
    var picked: ((Int) -> Void)?
    private var chosen = -1

    convenience init() {
        var c = UIButton.Configuration.bordered()
        c.indicator = .popup
        c.titleLineBreakMode = .byTruncatingTail
        self.init(configuration: c)
        showsMenuAsPrimaryAction = true
        refresh()
    }

    func selectedRow(inComponent c: Int) -> Int { chosen }

    func selectRow(_ row: Int, inComponent c: Int, animated: Bool) {
        chosen = row
        refresh()
    }

    private func refresh() {
        let actions = items.enumerated().map { i, title in
            UIAction(title: title, state: i == chosen ? .on : .off) { [weak self] _ in
                guard let self, self.chosen != i else { return }
                self.chosen = i
                self.refresh()
                self.picked?(i)
            }
        }
        configuration?.title = chosen >= 0 && chosen < items.count ? items[chosen] : (items.first ?? "")
        menu = UIMenu(options: .singleSelection, children: actions)
        invalidateIntrinsicContentSize()
    }
}

// MARK: - No clipboard

/// tvOS has no clipboard: what's copied goes nowhere, and nothing's there
/// to paste.
final class UIPasteboard {
    static let general = UIPasteboard()
    var string: String?
    var types: [String] { [] }
    var itemProviders: [NSItemProvider] { [] }
    var hasStrings: Bool { false }
}

// MARK: - The remote

@MainActor extension AsukaRuntime {
    /// The Keys name a remote's press goes by: its arrows (and swipes),
    /// Play/Pause as playpause. Select is none: it presses what has the
    /// focus, as Android TV's D-pad center does.
    static func remoteKey(_ t: UIPress.PressType) -> String? {
        switch t {
        case .upArrow: return "up"
        case .downArrow: return "down"
        case .leftArrow: return "left"
        case .rightArrow: return "right"
        case .playPause: return "playpause"
        default: return nil
        }
    }

    /// The view whose Keys take name, from the focused view out (as a
    /// keyboard's keys go: KeyCommands.swift), and the key's index.
    func remoteKeyTaker(_ name: String) -> (id: UInt32, index: Int)? {
        guard !keyed.isEmpty else { return nil }
        var start: AsukaNode? = focusedNode
        if start == nil, let top = topController, let n = node(of: top), n.kind == .sheet || n.kind == .popover { start = n }
        let way = AsukaKeys.way(from: start, root: nodes[rootID], parent: { $0.parent },
                                list: { [self] n in keysOut(n) }, kind: { $0.kind })
        for m in way {
            if let i = m.remoteKeys.firstIndex(of: name) { return (m.id, i) }
        }
        return nil
    }

    /// A remote's press went down: a view's Keys take it (Key) ahead of
    /// what the focused view does with it. true: taken, and its end too.
    func remotePressBegan(_ presses: Set<UIPress>) -> Bool {
        guard let p = presses.first, let name = Self.remoteKey(p.type), let k = remoteKeyTaker(name) else { return false }
        handledPresses.insert(ObjectIdentifier(p))
        remoteMove = (name, CACurrentMediaTime())
        let id = k.id, i = Int64(k.index)
        emit(id, .key) { AsukaEvents.key(id, i) }
        return true
    }

    /// Whether a press's end belongs to a press a view's Keys took.
    func remotePressTaken(_ presses: Set<UIPress>) -> Bool {
        guard let p = presses.first else { return false }
        return handledPresses.remove(ObjectIdentifier(p)) != nil
    }

    /// The focus engine moves the focus (a swipe, or an arrow's click): a
    /// view whose Keys take the direction takes it instead, and the focus
    /// stays. true: taken.
    func remoteFocusMove(_ context: UIFocusUpdateContext) -> Bool {
        let h = context.focusHeading
        let name: String
        if h.contains(.up) { name = "up" } else if h.contains(.down) { name = "down" }
        else if h.contains(.left) { name = "left" } else if h.contains(.right) { name = "right" }
        else { return false }
        guard let k = remoteKeyTaker(name) else { return false }
        // The click that began it already went to the Keys.
        if let m = remoteMove, m.name == name, CACurrentMediaTime() - m.at < 0.3 { return true }
        remoteMove = (name, CACurrentMediaTime())
        let id = k.id, i = Int64(k.index)
        emit(id, .key) { AsukaEvents.key(id, i) }
        return true
    }

    /// Menu at the root, with nothing to go back to: the root's ui.OnBack
    /// takes it (Back), else tvOS does (to the Home screen).
    func rootBack(_ presses: Set<UIPress>) -> Bool {
        guard rootInterceptsBack, let p = presses.first, let root = nodes[rootID],
              root.listens.contains(AsukaEventKind.back.rawValue) else { return false }
        let id = rootID
        backExitAsked = false
        emit(id, .back) { AsukaEvents.back(id) }
        // The app's handlers let it go on (BackExit, as they ran): tvOS's.
        if backExitAsked {
            backExitAsked = false
            return false
        }
        handledPresses.insert(ObjectIdentifier(p))
        return true
    }
}

extension AsukaScreenController {
    /// Menu on a screen or a sheet whose ui.OnBack takes going back: Back
    /// to it, and nothing pops or dismisses. true: taken.
    func remoteMenu(_ presses: Set<UIPress>) -> Bool {
        guard interceptsBack, let p = presses.first, p.type == .menu else { return false }
        let rt = AsukaRuntime.shared
        guard let n = rt.nodes[nodeID], !n.dying, n.listens.contains(AsukaEventKind.back.rawValue) else { return false }
        let id = nodeID
        rt.backExitAsked = false
        rt.emit(id, .back) { AsukaEvents.back(id) }
        // The app's handlers let it go on from the root (BackExit): tvOS's
        // own, to the Home screen.
        if rt.backExitAsked {
            rt.backExitAsked = false
            return false
        }
        rt.handledPresses.insert(ObjectIdentifier(p))
        return true
    }
}

#endif
