#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Chip: UIKit has none, so it's a UIButton in the capsule style, as
/// the system's own filter and token buttons look: gray, its title in the
/// label's color and its icon in the tint; a selected filter chip tinted,
/// its icon Material's checkmark. A removable (input) chip has an x in a
/// circle at its end, a target of its own, while the rest of it still
/// presses. VoiceOver reads a filter chip as a toggle button, on or off,
/// and offers a removable one's remove as an action.
final class AsukaChipView: UIButton {
    var title = "" { didSet { if title != oldValue { update() } } }
    var icon = "" { didSet { if icon != oldValue { update() } } }
    var filter = false { didSet { if filter != oldValue { update() } } }
    var isOn = false { didSet { if isOn != oldValue { update() } } }
    var removable = false { didSet { if removable != oldValue { update() } } }
    /// ContentInset as the style sets it, which replaces the chip's own.
    var padding: UIEdgeInsets? { didSet { if padding != oldValue { update() } } }
    /// The font its node's text props make, if they set one.
    var font: (() -> UIFont?)?

    var pressed: (() -> Void)?
    var toggled: ((Bool) -> Void)?
    var removed: (() -> Void)?

    private(set) var removeButton: AsukaChipRemoveButton?

    /// Room the remove button takes at the end: its symbol and a gap.
    static let removeSize: CGFloat = 18
    static let removeGap: CGFloat = 6

    static func make() -> AsukaChipView {
        let c = AsukaChipView(configuration: .gray())
        c.addAction(UIAction { [weak c] _ in c?.press() }, for: .primaryActionTriggered)
        c.update()
        return c
    }

    private func press() {
        if filter {
            isOn.toggle() // shown at once; the core sends it back if the app refuses
            toggled?(isOn)
        } else {
            pressed?()
        }
    }

    /// The chip's font: its node's, else the subheadline's, in the app's text size.
    var chipFont: UIFont {
        font?() ?? .preferredFont(forTextStyle: .subheadline, compatibleWith: UITraitCollection(preferredContentSizeCategory: asukaContentSize))
    }

    /// The insets its content takes: the style's, else Material's (8
    /// before an icon, 12 before a title alone; the end's room for the
    /// remove button), leading and trailing.
    var insets: NSDirectionalEdgeInsets {
        let hasImage = !icon.isEmpty || (filter && isOn)
        if let p = padding {
            let rtl = effectiveUserInterfaceLayoutDirection == .rightToLeft
            var i = NSDirectionalEdgeInsets(top: p.top, leading: rtl ? p.right : p.left, bottom: p.bottom, trailing: rtl ? p.left : p.right)
            if removable { i.trailing += Self.removeSize + Self.removeGap }
            return i
        }
        return NSDirectionalEdgeInsets(top: 6, leading: hasImage ? 8 : 12, bottom: 6,
                                       trailing: removable ? 8 + Self.removeSize + Self.removeGap : 12)
    }

    func update() {
        let selected = filter && isOn
        var c: UIButton.Configuration = selected ? .tinted() : .gray()
        c.cornerStyle = .capsule
        c.title = title
        c.titleLineBreakMode = .byTruncatingTail
        c.titleTextAttributesTransformer = UIConfigurationTextAttributesTransformer { [weak self] a in
            var a = a
            a.font = MainActor.assumeIsolated { self?.chipFont }
            return a
        }
        let f = chipFont
        let name = selected ? "checkmark" : icon
        c.image = name.isEmpty ? nil : UIImage(systemName: name)
        c.imagePadding = 6
        c.preferredSymbolConfigurationForImage = UIImage.SymbolConfiguration(font: f, scale: .small)
        if !selected {
            // The title in the label's color, the icon in the tint (an
            // assist chip's, as Material's), both dimmed while disabled.
            c.baseForegroundColor = .label
            c.imageColorTransformer = UIConfigurationColorTransformer { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return .tintColor }
                    return self.isEnabled ? self.tintColor : .tertiaryLabel
                }
            }
        }
        c.contentInsets = insets
        configuration = c
        updateRemoveButton(f, selected)
        describe()
        invalidateIntrinsicContentSize()
    }

    private func updateRemoveButton(_ f: UIFont, _ selected: Bool) {
        guard removable else {
            removeButton?.removeFromSuperview()
            removeButton = nil
            return
        }
        let b = removeButton ?? {
            let b = AsukaChipRemoveButton()
            b.addAction(UIAction { [weak self] _ in self?.removed?() }, for: .touchUpInside) // a plain control: no primary action of its own
            b.isAccessibilityElement = false // the chip's action, for VoiceOver
            addSubview(b)
            removeButton = b
            return b
        }()
        let size = min(Self.removeSize - 2, f.pointSize)
        b.imageView.image = UIImage(systemName: "xmark.circle.fill", withConfiguration: UIImage.SymbolConfiguration(pointSize: size, weight: .regular))
        b.imageView.tintColor = selected ? nil : .secondaryLabel
        b.accessibilityLabel = "\(Self.removeWord), \(title)"
        setNeedsLayout()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard let b = removeButton else { return }
        // At the end, its own way (the chip's order turns in a right-to-left one).
        let end = (padding.map { _ in insets.trailing - Self.removeSize - Self.removeGap } ?? 8)
        let w = Self.removeSize + Self.removeGap / 2 + end
        let rtl = effectiveUserInterfaceLayoutDirection == .rightToLeft
        b.end = end
        b.frame = CGRect(x: rtl ? 0 : bounds.width - w, y: 0, width: w, height: bounds.height)
        bringSubviewToFront(b)
    }

    override func traitCollectionDidChange(_ previous: UITraitCollection?) {
        super.traitCollectionDidChange(previous)
        if previous?.layoutDirection != traitCollection.layoutDirection { update() }
    }

    /// VoiceOver: a filter chip a toggle button, on or off; a removable
    /// one with its remove action, the platform's word for it.
    private func describe() {
        var t = accessibilityTraits
        t.insert(.button)
        if #available(iOS 17.0, tvOS 17.0, *) {
            if filter { t.insert(.toggleButton) } else { t.remove(.toggleButton) }
            t.remove(.selected)
            accessibilityValue = filter ? (isOn ? "1" : "0") : nil
        } else {
            if filter && isOn { t.insert(.selected) } else { t.remove(.selected) }
        }
        accessibilityTraits = t
        var actions = (accessibilityCustomActions ?? []).filter { !($0 is AsukaChipRemoveAction) }
        if removable {
            actions.append(AsukaChipRemoveAction(name: Self.removeWord) { [weak self] _ in
                self?.removed?()
                return true
            })
        }
        accessibilityCustomActions = actions.isEmpty ? nil : actions
    }

    #if !os(tvOS)
    /// Delete or Backspace with the keyboard's focus removes a removable chip.
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if removable, presses.contains(where: { $0.key?.keyCode == .keyboardDeleteOrBackspace || $0.key?.keyCode == .keyboardDeleteForward }) {
            removed?()
            return
        }
        super.pressesBegan(presses, with: event)
    }
    #endif

    /// The platform's word for removing an item: UIKit's own, as its
    /// table views' delete buttons say it, in the user's language.
    static let removeWord: String = {
        let uikit = Bundle(for: UIView.self)
        for key in ["Remove", "Delete"] {
            let s = uikit.localizedString(forKey: key, value: "\u{1}", table: nil)
            if s != "\u{1}", !s.isEmpty { return s }
        }
        return "Remove"
    }()
}

/// A removable chip's remove button: an x in a circle at its end, its
/// target the chip's height and a finger's above and below.
final class AsukaChipRemoveButton: UIControl {
    let imageView = UIImageView()
    /// The room between the x and the chip's end.
    var end: CGFloat = 8 { didSet { setNeedsLayout() } }

    override init(frame: CGRect) {
        super.init(frame: frame)
        imageView.contentMode = .center
        imageView.isUserInteractionEnabled = false
        addSubview(imageView)
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isHighlighted: Bool { didSet { imageView.alpha = isHighlighted ? 0.5 : 1 } }

    override func layoutSubviews() {
        super.layoutSubviews()
        let s = imageView.image?.size ?? .zero
        let rtl = effectiveUserInterfaceLayoutDirection == .rightToLeft
        imageView.frame = CGRect(x: rtl ? end : bounds.width - end - s.width, y: (bounds.height - s.height) / 2, width: s.width, height: s.height)
    }

    override func point(inside p: CGPoint, with event: UIEvent?) -> Bool {
        bounds.insetBy(dx: 0, dy: min(0, (bounds.height - 44) / 2)).contains(p)
    }
}

/// The remove action a removable chip offers VoiceOver.
final class AsukaChipRemoveAction: UIAccessibilityCustomAction {}

@MainActor extension AsukaRuntime {
    func makeChip(_ id: UInt32) -> UIView {
        let c = AsukaChipView.make()
        c.pressed = { [weak self] in self?.emit(id, .press) { AsukaEvents.press(id) } }
        c.toggled = { [weak self] on in self?.emit(id, .toggled) { AsukaEvents.toggled(id, on) } }
        c.removed = { [weak self] in self?.emit(id, .remove) { AsukaEvents.remove(id) } }
        c.font = { [weak self] in
            guard let n = self?.nodes[id], n.textStyle != 0 || n.fontSize > 0 || n.fontWeight != 0 || !n.fontFamily.isEmpty || n.fontTraits != 0 else { return nil }
            return n.font
        }
        return c
    }

    func setChipProp(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) -> Bool {
        guard n.kind == .chip, let c = n.view as? AsukaChipView else { return false }
        switch prop {
        case AsukaProp.Chip.title: c.title = v.string
        case AsukaProp.Chip.icon: c.icon = v.string
        case AsukaProp.Chip.filter: c.filter = v.bool
        case AsukaProp.Chip.selected: c.isOn = v.bool
        case AsukaProp.Chip.removable: c.removable = v.bool
        default: return false
        }
        return true
    }
}
#endif
