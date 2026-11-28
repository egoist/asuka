#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Disclosure's header, as SwiftUI's DisclosureGroup draws one in a
/// list: its title in the label's color at the start, a chevron in the
/// tint at the end pointing forward, which turns down as it expands, in a
/// row a list's height (44 points at least). The content under it is the
/// core's. VoiceOver reads a button, expanded or collapsed.
final class AsukaDisclosureView: UIControl {
    let titleLabel = UILabel()
    let chevron = UIImageView()
    var expanded = false { didSet { if expanded != oldValue { turn(animated: window != nil) } } }
    /// ContentInset: the title and chevron are inside it.
    var contentInset_: UIEdgeInsets = .zero { didSet { setNeedsLayout() } }
    var toggled: ((Bool) -> Void)?
    /// The chevron's color (Color): the tint's unless set.
    var tint: UIColor? { didSet { if isEnabled { chevron.tintColor = tint } } }

    static let minHeight: CGFloat = 44
    static let gap: CGFloat = 8

    override init(frame: CGRect) {
        super.init(frame: frame)
        titleLabel.font = .preferredFont(forTextStyle: .body)
        titleLabel.adjustsFontForContentSizeCategory = true
        titleLabel.textColor = .label
        titleLabel.numberOfLines = 0
        addSubview(titleLabel)
        chevron.contentMode = .center
        chevron.preferredSymbolConfiguration = UIImage.SymbolConfiguration(textStyle: .body, scale: .small)
            .applying(UIImage.SymbolConfiguration(weight: .semibold))
        chevron.image = UIImage(systemName: "chevron.forward")
        addSubview(chevron)
        isAccessibilityElement = true
        accessibilityTraits = .button
        addAction(UIAction { [weak self] _ in self?.press() }, for: .touchUpInside)
        describe()
    }
    required init?(coder: NSCoder) { fatalError() }

    var title: String {
        get { titleLabel.text ?? "" }
        set {
            titleLabel.text = newValue
            describe()
            setNeedsLayout()
        }
    }

    private func press() {
        expanded.toggle() // shown at once; the core shows the content
        toggled?(expanded)
    }

    override var isHighlighted: Bool {
        didSet { backgroundColor = isHighlighted ? .systemFill.withAlphaComponent(0.3) : nil }
    }

    override var isEnabled: Bool {
        didSet {
            titleLabel.textColor = isEnabled ? .label : .tertiaryLabel
            chevron.tintColor = isEnabled ? tint : .tertiaryLabel
            if isEnabled { accessibilityTraits.remove(.notEnabled) } else { accessibilityTraits.insert(.notEnabled) }
        }
    }

    /// The chevron forward collapsed, down expanded: forward turns
    /// clockwise to down, and in a right-to-left view (where it points
    /// left) the other way.
    private func turn(animated: Bool) {
        let rtl = effectiveUserInterfaceLayoutDirection == .rightToLeft
        let t: CGAffineTransform = expanded ? CGAffineTransform(rotationAngle: (rtl ? -1 : 1) * .pi / 2) : .identity
        if animated {
            UIView.animate(withDuration: 0.25, delay: 0, options: [.beginFromCurrentState, .allowUserInteraction]) { self.chevron.transform = t }
        } else {
            chevron.transform = t
        }
        describe()
    }

    private func describe() {
        accessibilityLabel = title
        if #available(iOS 18.0, tvOS 18.0, *) {
            accessibilityExpandedStatus = expanded ? .expanded : .collapsed
        }
    }

    override func traitCollectionDidChange(_ previous: UITraitCollection?) {
        super.traitCollectionDidChange(previous)
        if previous?.layoutDirection != traitCollection.layoutDirection { directionChanged() }
    }

    /// The chevron turns the other way, and the title and it swap ends.
    func directionChanged() {
        turn(animated: false)
        setNeedsLayout()
    }

    /// Space or Return with the keyboard's focus flips it (tvOS: Select).
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        #if os(tvOS)
        let flips = presses.contains { $0.type == .select }
        #else
        let flips = presses.contains { [.keyboardSpacebar, .keyboardReturnOrEnter, .keypadEnter].contains($0.key?.keyCode) }
        #endif
        if isEnabled, flips {
            press()
            return
        }
        super.pressesBegan(presses, with: event)
    }

    override var canBecomeFocused: Bool { isEnabled }

    private var chevronSize: CGSize { chevron.image?.size ?? .zero }

    /// Its natural size: the title and the chevron, a row's height at
    /// least; its content's alone (the core adds its padding).
    override func sizeThatFits(_ size: CGSize) -> CGSize {
        let c = chevronSize
        let room = size.width.isFinite && size.width < 10_000 ? max(size.width - c.width - Self.gap, 0) : .greatestFiniteMagnitude
        let t = titleLabel.sizeThatFits(CGSize(width: room, height: .greatestFiniteMagnitude))
        let w = (room < .greatestFiniteMagnitude ? min(t.width, room) : t.width) + Self.gap + c.width
        let i = contentInset_
        let minHeight = max(Self.minHeight - i.top - i.bottom, 0)
        return CGSize(width: ceil(w), height: max(minHeight, ceil(max(t.height, c.height)) + 22 - min(i.top + i.bottom, 22)))
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let b = bounds.inset(by: contentInset_)
        let c = chevronSize
        let rtl = effectiveUserInterfaceLayoutDirection == .rightToLeft
        let room = max(b.width - c.width - Self.gap, 0)
        let t = titleLabel.sizeThatFits(CGSize(width: room, height: .greatestFiniteMagnitude))
        let tw = min(t.width, room)
        titleLabel.frame = CGRect(x: rtl ? b.maxX - tw : b.minX, y: b.midY - t.height / 2, width: tw, height: t.height)
        // Through bounds and center: it may be turned.
        chevron.bounds = CGRect(origin: .zero, size: c)
        chevron.center = CGPoint(x: rtl ? b.minX + c.width / 2 : b.maxX - c.width / 2, y: b.midY)
        titleLabel.textAlignment = rtl ? .right : .left
    }
}

@MainActor extension AsukaRuntime {
    func makeDisclosure(_ id: UInt32) -> UIView {
        let d = AsukaDisclosureView()
        d.toggled = { [weak self] on in self?.emit(id, .toggled) { AsukaEvents.toggled(id, on) } }
        return d
    }

    func setDisclosureProp(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) -> Bool {
        guard n.kind == .disclosure, let d = n.view as? AsukaDisclosureView else { return false }
        switch prop {
        case AsukaProp.Disclosure.title: d.title = v.string
        case AsukaProp.Disclosure.expanded: d.expanded = v.bool
        default: return false
        }
        return true
    }
}
#endif
