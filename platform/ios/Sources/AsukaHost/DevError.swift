#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// asuka dev's error overlay (the DevError command, to the root;
// ops/deverror.go): an error nothing in the app caught, a module that
// didn't load, a build that failed. It's in UIKit's own views, not the
// app's (the app's tree may be what's broken), in a window of its own
// above the app's: over its screens and everything they present (sheets,
// alerts, popovers, which UIKit puts in the app's window), whatever the
// app presents after it. The title, the message, the lines of the source
// where it went wrong (monospaced, scrolling sideways) and the stack, all
// selectable, scrolling when long; Dismiss and, when the core offers it,
// Reload. They send DevErrorAction to the root; the core removes the
// overlay (DevErrorClear) or sends what shows next (DevError again).

/// DevErrorFlags (ops/deverror.go).
let asukaDevErrorReload: UInt8 = 1
let asukaDevErrorBuild: UInt8 = 2

/// A DevError command's arguments.
struct AsukaDevError: Equatable {
    var flags: UInt8 = 0
    var title = ""
    var message = ""
    var source = ""
    var details = ""

    /// Reads what ops.AppendDevError writes: nil if it's cut short.
    init?(_ args: UnsafeRawBufferPointer) {
        var i = 0
        func string() -> String? {
            guard i + 4 <= args.count else { return nil }
            let n = Int(args[i]) | Int(args[i + 1]) << 8 | Int(args[i + 2]) << 16 | Int(args[i + 3]) << 24
            i += 4
            guard n <= args.count - i else { return nil }
            defer { i += n }
            return String(decoding: UnsafeRawBufferPointer(rebasing: args[i..<i + n]), as: UTF8.self)
        }
        guard args.count >= 1 else { return nil }
        flags = args[0]
        i = 1
        guard let t = string(), let m = string(), let s = string(), let d = string() else { return nil }
        (title, message, source, details) = (t, m, s, d)
    }
}

/// The overlay's window: above the app's, alerts' included.
final class AsukaDevErrorWindow: UIWindow {}

/// The overlay: its window and what it shows.
@MainActor final class AsukaDevErrorOverlay {
    /// The root the DevError came to, which its actions go to.
    var root: UInt32
    let window: AsukaDevErrorWindow
    let controller: AsukaDevErrorController
    /// The window that was key before it, made key again when it goes.
    weak var appWindow: UIWindow?

    init(root: UInt32, scene: UIWindowScene?, appWindow: UIWindow?) {
        self.root = root
        self.appWindow = appWindow
        if let scene {
            window = AsukaDevErrorWindow(windowScene: scene)
        } else {
            window = AsukaDevErrorWindow(frame: appWindow?.frame ?? UIScreen.main.bounds)
        }
        controller = AsukaDevErrorController()
        window.windowLevel = .alert + 1
        window.rootViewController = controller
    }
}

extension AsukaRuntime {
    /// DevError: shows e over everything, or shows it instead of what the
    /// overlay shows.
    func showDevError(_ root: UInt32, _ args: UnsafeRawBufferPointer) {
        guard let e = AsukaDevError(args) else { return }
        let o: AsukaDevErrorOverlay
        if let d = devError {
            o = d
            o.root = root
        } else {
            let app = controller?.view.window ?? nodes[root]?.view.window
            let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            let scene = app?.windowScene
                ?? scenes.first { $0.activationState == .foregroundActive }
                ?? scenes.first
            o = AsukaDevErrorOverlay(root: root, scene: scene, appWindow: app)
            o.window.overrideUserInterfaceStyle = colorScheme
            o.window.tintColor = accent
            o.window.semanticContentAttribute = rtl ? .forceRightToLeft : .forceLeftToRight
            o.controller.act = { [weak self, weak o] action in
                guard let self, let o else { return }
                self.send(AsukaEvents.devErrorAction(o.root, Int64(action.rawValue)))
            }
            devError = o
        }
        o.controller.show(e)
        if o.window.isHidden {
            // The keyboard would cover its buttons; VoiceOver reads only it.
            o.appWindow?.endEditing(true)
            o.appWindow?.accessibilityElementsHidden = true
            o.window.makeKeyAndVisible()
            UIAccessibility.post(notification: .screenChanged, argument: o.controller.titleView)
        }
    }

    /// DevErrorClear: the overlay goes; the app's window is key again.
    func clearDevError() {
        guard let o = devError else { return }
        devError = nil
        let wasKey = o.window.isKeyWindow
        o.window.isHidden = true
        o.appWindow?.accessibilityElementsHidden = false
        if wasKey { o.appWindow?.makeKey() }
        UIAccessibility.post(notification: .screenChanged, argument: nil)
    }
}

/// The overlay's content: a scrolling column of the error's parts over a
/// bar with its buttons.
final class AsukaDevErrorController: UIViewController {
    var act: ((AsukaDevErrorAction) -> Void)?
    private(set) var shown: AsukaDevError?

    let titleView = AsukaDevErrorController.text()
    let messageView = AsukaDevErrorController.text()
    let sourceBox = AsukaDevErrorCodeBox()
    let detailsView = AsukaDevErrorController.text()
    let dismissButton = UIButton(type: .system)
    let reloadButton = UIButton(type: .system)
    private let scroll = UIScrollView()
    private let column = UIStackView()

    /// A selectable text that isn't edited and doesn't scroll (the column does).
    static func text() -> UITextView {
        let t = UITextView()
        t.isEditable = false
        t.isSelectable = true
        t.isScrollEnabled = false
        t.backgroundColor = .clear
        t.textContainerInset = .zero
        t.textContainer.lineFragmentPadding = 0
        t.adjustsFontForContentSizeCategory = true
        t.dataDetectorTypes = []
        return t
    }

    /// The monospaced font of the source and the details, sized with Dynamic Type.
    static var codeFont: UIFont {
        UIFontMetrics(forTextStyle: .footnote).scaledFont(for: .monospacedSystemFont(ofSize: 12, weight: .regular))
    }

    override func loadView() {
        let v = UIView()
        v.backgroundColor = .systemBackground
        v.accessibilityViewIsModal = true
        view = v

        titleView.font = UIFontMetrics(forTextStyle: .footnote).scaledFont(for: .systemFont(ofSize: 13, weight: .semibold))
        messageView.font = UIFontMetrics(forTextStyle: .title3).scaledFont(for: .systemFont(ofSize: 20, weight: .semibold))
        messageView.textColor = .label
        messageView.accessibilityTraits.insert(.header)
        detailsView.font = Self.codeFont
        detailsView.textColor = .secondaryLabel
        detailsView.semanticContentAttribute = .forceLeftToRight
        detailsView.textAlignment = .left
        detailsView.accessibilityLabel = "Details"
        sourceBox.textView.accessibilityLabel = "Source"

        column.axis = .vertical
        column.alignment = .fill
        column.spacing = 16
        column.addArrangedSubview(titleView)
        column.setCustomSpacing(8, after: titleView)
        column.addArrangedSubview(messageView)
        column.addArrangedSubview(sourceBox)
        column.addArrangedSubview(detailsView)
        column.translatesAutoresizingMaskIntoConstraints = false

        scroll.alwaysBounceVertical = true
        scroll.keyboardDismissMode = .interactive
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.addSubview(column)

        let bar = UIView()
        bar.backgroundColor = .systemBackground
        bar.translatesAutoresizingMaskIntoConstraints = false
        let line = UIView()
        line.backgroundColor = .separator
        line.translatesAutoresizingMaskIntoConstraints = false
        let buttons = UIStackView(arrangedSubviews: [dismissButton, reloadButton])
        buttons.axis = .horizontal
        buttons.distribution = .fillEqually
        buttons.spacing = 12
        buttons.translatesAutoresizingMaskIntoConstraints = false
        bar.addSubview(line)
        bar.addSubview(buttons)

        style(dismissButton, "Dismiss", primary: false)
        style(reloadButton, "Reload", primary: true)
        dismissButton.addAction(UIAction { [weak self] _ in self?.act?(.dismiss) }, for: .primaryActionTriggered)
        reloadButton.addAction(UIAction { [weak self] _ in self?.act?(.reload) }, for: .primaryActionTriggered)

        v.addSubview(scroll)
        v.addSubview(bar)
        let safe = v.safeAreaLayoutGuide
        let hair = 1 / max(1, UIScreen.main.scale)
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: v.topAnchor),
            scroll.leadingAnchor.constraint(equalTo: safe.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: safe.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bar.topAnchor),

            column.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor, constant: 24),
            column.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor, constant: -20),
            column.leadingAnchor.constraint(equalTo: scroll.frameLayoutGuide.leadingAnchor, constant: 20),
            column.trailingAnchor.constraint(equalTo: scroll.frameLayoutGuide.trailingAnchor, constant: -20),

            bar.leadingAnchor.constraint(equalTo: v.leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: v.trailingAnchor),
            bar.bottomAnchor.constraint(equalTo: v.bottomAnchor),
            line.topAnchor.constraint(equalTo: bar.topAnchor),
            line.leadingAnchor.constraint(equalTo: bar.leadingAnchor),
            line.trailingAnchor.constraint(equalTo: bar.trailingAnchor),
            line.heightAnchor.constraint(equalToConstant: hair),
            buttons.topAnchor.constraint(equalTo: bar.topAnchor, constant: 12),
            buttons.leadingAnchor.constraint(equalTo: safe.leadingAnchor, constant: 20),
            buttons.trailingAnchor.constraint(equalTo: safe.trailingAnchor, constant: -20),
            buttons.bottomAnchor.constraint(equalTo: safe.bottomAnchor, constant: -12),
            buttons.heightAnchor.constraint(greaterThanOrEqualToConstant: 44),
        ])
    }

    private func style(_ b: UIButton, _ title: String, primary: Bool) {
        var c: UIButton.Configuration
        if #available(iOS 26.0, *) {
            c = primary ? .prominentGlass() : .glass()
        } else {
            c = primary ? .filled() : .gray()
            c.cornerStyle = .large
        }
        c.title = title
        c.buttonSize = .large
        c.titleTextAttributesTransformer = UIConfigurationTextAttributesTransformer { a in
            var a = a
            a.font = UIFont.preferredFont(forTextStyle: .body).withWeight(.semibold)
            return a
        }
        if !primary { c.baseForegroundColor = .label }
        b.configuration = c
        b.titleLabel?.adjustsFontForContentSizeCategory = true
    }

    /// Shows e, in place of what it showed (from the top again when it's
    /// another error).
    func show(_ e: AsukaDevError) {
        loadViewIfNeeded()
        let isNew = shown.map { $0.title != e.title || $0.message != e.message } ?? true
        shown = e
        let build = e.flags & asukaDevErrorBuild != 0
        titleView.text = e.title.uppercased()
        titleView.accessibilityLabel = e.title
        titleView.textColor = build ? .systemOrange : .systemRed
        messageView.text = e.message
        sourceBox.text = e.source
        sourceBox.isHidden = e.source.isEmpty
        detailsView.text = e.details
        detailsView.isHidden = e.details.isEmpty
        reloadButton.isHidden = e.flags & asukaDevErrorReload == 0
        if isNew { scroll.setContentOffset(CGPoint(x: 0, y: -scroll.adjustedContentInset.top), animated: false) }
    }

    /// What it shows, a part a line (the source and the details as one
    /// each), the buttons' titles last: for the automation's dump.
    var texts: [String] {
        guard let e = shown else { return [] }
        var out = [e.title, e.message, e.source, e.details].filter { !$0.isEmpty }
        out.append("Dismiss")
        if !reloadButton.isHidden { out.append("Reload") }
        return out
    }

    /// Presses the button titled title: whether there's one.
    func press(_ title: String) -> Bool {
        for b in [dismissButton, reloadButton] where !b.isHidden && b.configuration?.title == title {
            b.sendActions(for: .primaryActionTriggered)
            return true
        }
        return false
    }
}

/// The source's box: its lines as they are, never wrapped, in a rounded
/// box that scrolls sideways when one is wider than it.
final class AsukaDevErrorCodeBox: UIScrollView {
    let textView = AsukaDevErrorController.text()
    private let pad = UIEdgeInsets(top: 12, left: 14, bottom: 12, right: 14)
    private var natural: CGSize?

    var text: String {
        get { textView.text }
        set {
            textView.text = newValue
            remeasure()
        }
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .secondarySystemBackground
        layer.cornerRadius = 10
        layer.cornerCurve = .continuous
        showsVerticalScrollIndicator = false
        alwaysBounceHorizontal = false
        textView.font = AsukaDevErrorController.codeFont
        textView.textColor = .label
        textView.textContainer.lineBreakMode = .byClipping
        // Code reads left to right, in a right-to-left app too.
        semanticContentAttribute = .forceLeftToRight
        textView.semanticContentAttribute = .forceLeftToRight
        textView.textAlignment = .left
        addSubview(textView)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    /// The text's size unwrapped, in its font now.
    private var textSize: CGSize {
        if let natural { return natural }
        let font = textView.font ?? AsukaDevErrorController.codeFont
        let r = (textView.text as NSString).boundingRect(
            with: CGSize(width: CGFloat.greatestFiniteMagnitude, height: .greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading], attributes: [.font: font], context: nil)
        // A little over, so rounding never wraps a line.
        let s = CGSize(width: ceil(r.width) + 2, height: ceil(r.height))
        natural = s
        return s
    }

    private func remeasure() {
        natural = nil
        invalidateIntrinsicContentSize()
        setNeedsLayout()
    }

    override var intrinsicContentSize: CGSize {
        CGSize(width: UIView.noIntrinsicMetric, height: textSize.height + pad.top + pad.bottom)
    }

    override func traitCollectionDidChange(_ previous: UITraitCollection?) {
        super.traitCollectionDidChange(previous)
        if previous?.preferredContentSizeCategory != traitCollection.preferredContentSizeCategory { remeasure() }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let s = textSize
        let width = max(s.width, bounds.width - pad.left - pad.right)
        textView.frame = CGRect(x: pad.left, y: pad.top, width: width, height: s.height)
        let content = CGSize(width: width + pad.left + pad.right, height: bounds.height)
        if contentSize != content { contentSize = content }
    }
}

private extension UIFont {
    func withWeight(_ w: UIFont.Weight) -> UIFont {
        let d = fontDescriptor.addingAttributes([.traits: [UIFontDescriptor.TraitKey.weight: w]])
        return UIFont(descriptor: d, size: 0)
    }
}
#endif
