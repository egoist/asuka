#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// Hosts the core's root view and reports its environment: size, safe
/// area, keyboard, scale, appearance and text size.
public final class AsukaViewController: UIViewController {
    var keyboard: CGFloat = 0
    var keyboardEnd: CGRect?
    var lastEnv: AsukaEnvironment?
    weak var root: UIView?
    /// Follows the keyboard's top (UIKeyboardLayoutGuide), which moves
    /// with the finger as a list's interactive dismissal drags it down,
    /// when no notification says so.
    private let keyboardTracker = UIView()

    public override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(keyboardWillChange(_:)), name: UIResponder.keyboardWillChangeFrameNotification, object: nil)
        nc.addObserver(self, selector: #selector(keyboardWillChange(_:)), name: UIResponder.keyboardWillHideNotification, object: nil)
        nc.addObserver(self, selector: #selector(contentSizeChanged), name: UIContentSizeCategory.didChangeNotification, object: nil)
        nc.addObserver(self, selector: #selector(contentSizeChanged), name: UIAccessibility.reduceMotionStatusDidChangeNotification, object: nil)
        nc.addObserver(self, selector: #selector(contentSizeChanged), name: UIAccessibility.voiceOverStatusDidChangeNotification, object: nil)
        nc.addObserver(self, selector: #selector(localeChanged), name: NSLocale.currentLocaleDidChangeNotification, object: nil)
        if #available(iOS 17.0, *) {
            registerForTraitChanges([UITraitUserInterfaceStyle.self, UITraitDisplayScale.self]) { (vc: AsukaViewController, _: UITraitCollection) in
                vc.sendEnvironment()
                AsukaRuntime.shared.resolveLayerColors()
            }
            // Split View, Slide Over, Stage Manager: the size classes
            // usually change with the size, but not always.
            registerForTraitChanges([UITraitHorizontalSizeClass.self, UITraitVerticalSizeClass.self]) { (vc: AsukaViewController, _: UITraitCollection) in
                vc.sendEnvironment()
                AsukaRuntime.shared.reportScreens()
            }
            // The text size, as the views in the window have it: on iOS
            // 27 the traits take it after its notification. Each text
            // takes its new font, and the core measures them.
            registerForTraitChanges([UITraitPreferredContentSizeCategory.self]) { (vc: AsukaViewController, _: UITraitCollection) in
                vc.followTextSize()
            }
        }
        keyboardTracker.isHidden = true
        keyboardTracker.isUserInteractionEnabled = false
        keyboardTracker.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(keyboardTracker)
        if #available(iOS 17.0, *) { view.keyboardLayoutGuide.usesBottomSafeArea = false }
        NSLayoutConstraint.activate([
            keyboardTracker.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            keyboardTracker.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            keyboardTracker.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            keyboardTracker.topAnchor.constraint(equalTo: view.keyboardLayoutGuide.topAnchor),
        ])
        let rt = AsukaRuntime.shared
        rt.scale = traitCollection.displayScale > 0 ? traitCollection.displayScale : UIScreen.main.scale
        sendEnvironment()
        rt.start(self)
    }

    // platform/screen's lock and status bar.
    public override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        AsukaScreen.lock ?? super.supportedInterfaceOrientations
    }
    public override var preferredStatusBarStyle: UIStatusBarStyle { AsukaScreen.statusBarStyle }
    public override var prefersStatusBarHidden: Bool { AsukaScreen.statusBarHidden }

    // A hardware keyboard's keys: the Keys of the views the keyboard's
    // focus is in (KeyCommands.swift). UIKit asks the responder chain, from the
    // first responder, which comes here (a presented controller's goes
    // through the one presenting it).
    public override var keyCommands: [UIKeyCommand]? { AsukaRuntime.shared.keyCommands() }

    @objc func asukaKey(_ c: UIKeyCommand) { AsukaRuntime.shared.keyCommand(c) }

    /// Tab and Shift-Tab in a FocusTrap (Focus.swift).
    @objc func asukaTab(_ c: UIKeyCommand) {
        AsukaRuntime.shared.tabInTrap(backward: (c.propertyList as? Int) == 1)
    }

    // The keyboard's focus (Focus.swift): what UIKit's focus system moves,
    // and keys the focused view takes that no Keys did.
    public override func didUpdateFocus(in context: UIFocusUpdateContext, with coordinator: UIFocusAnimationCoordinator) {
        super.didUpdateFocus(in: context, with: coordinator)
        AsukaRuntime.shared.focusSystemMoved(context)
    }

    public override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if AsukaRuntime.shared.focusedKey(presses) { return }
        super.pressesBegan(presses, with: event)
    }

    func mount(_ v: UIView) {
        root?.removeFromSuperview()
        root = v
        view.addSubview(v)
        AsukaRuntime.shared.raiseOverlays(in: view) // the overlays over it
    }

    public override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        AsukaRuntime.shared.adoptControllers(in: self)
        if let c = AsukaRuntime.shared.accent { view.window?.tintColor = c }
        if AsukaRuntime.shared.colorScheme != .unspecified { view.window?.overrideUserInterfaceStyle = AsukaRuntime.shared.colorScheme }
        if let w = view.window { AsukaRuntime.shared.setWindowDirection(w) }
    }

    public override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        // Outside presses for the overlays, and that the last input was a touch.
        if let w = view.window { AsukaRuntime.shared.observeTouches(in: w) }
    }

    public override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let moved = trackKeyboard()
        updateKeyboard()
        sendEnvironment()
        if moved { AsukaRuntime.shared.reportScreens() }
    }

    /// The keyboard's top as the layout guide has it: during an
    /// interactive dismissal (a list's keyboardDismissMode), the keyboard
    /// follows the finger with no notification, and what sits on it (a
    /// chat's composer) follows the guide, frame by frame. Notified
    /// changes agree with it once their animation is set up.
    private func trackKeyboard() -> Bool {
        guard let window = view.window else { return false }
        var h = keyboardTracker.frame.height
        if #unavailable(iOS 17.0) {
            // The guide sits on the safe area while the keyboard's away.
            if h <= view.safeAreaInsets.bottom + 0.5 { h = 0 }
        }
        h = max(0, min(h, view.bounds.height))
        guard abs(h - keyboard) > 0.5 else { return false }
        if h < 0.5 {
            keyboardEnd = nil
        } else {
            let r = CGRect(x: 0, y: view.bounds.maxY - h, width: view.bounds.width, height: h)
            keyboardEnd = view.convert(r, to: window.screen.coordinateSpace)
        }
        return true
    }

    public override func viewSafeAreaInsetsDidChange() {
        super.viewSafeAreaInsetsDidChange()
        sendEnvironment()
    }

    /// Text size or Reduce Motion changed: the root and the screens report
    /// it, a turn later, once each text has its new font (followTextSize)
    /// for the core to measure with. The views in the window take the new
    /// text size from their traits, and a label that adjusts to the text
    /// size puts back the font of the size its traits have: while they
    /// lag the app's (iOS 27 changes them after the notification), the
    /// core would measure the old fonts, and see no change as UIKit grows
    /// them. Then it waits for the traits (their change follows the text
    /// size), or a second at most.
    @objc func contentSizeChanged() {
        DispatchQueue.main.async {
            let app = UIApplication.shared.preferredContentSizeCategory
            if self.traitCollection.preferredContentSizeCategory == app {
                self.followTextSize()
                return
            }
            self.textSizePending = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                if self.textSizePending { self.followTextSize() }
            }
        }
    }

    /// Whether a text size change waits for the window's traits to have it.
    var textSizePending = false

    /// Each text takes its new font, then the core measures them.
    func followTextSize() {
        textSizePending = false
        for n in AsukaRuntime.shared.nodes.values { n.followTextSize() }
        sendEnvironment()
        AsukaRuntime.shared.reportScreens()
    }

    /// The formats or the languages changed in Settings.
    @objc func localeChanged() {
        DispatchQueue.main.async { AsukaRuntime.shared.localeChanged() }
    }

    func updateKeyboard() {
        guard let end = keyboardEnd, let window = view.window else {
            keyboard = 0
            return
        }
        let frame = view.convert(end, from: window.screen.coordinateSpace)
        keyboard = max(0, view.bounds.maxY - frame.minY)
    }

    @objc func keyboardWillChange(_ note: Notification) {
        let info = note.userInfo ?? [:]
        if note.name == UIResponder.keyboardWillHideNotification {
            keyboardEnd = nil
        } else {
            keyboardEnd = (info[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue)?.cgRectValue
        }
        updateKeyboard()
        let duration = (info[UIResponder.keyboardAnimationDurationUserInfoKey] as? Double) ?? 0.25
        let curve = (info[UIResponder.keyboardAnimationCurveUserInfoKey] as? UInt) ?? 7
        // The core lays out and applies frames inside this block, so the
        // views move with the keyboard.
        UIView.animate(withDuration: duration, delay: 0, options: UIView.AnimationOptions(rawValue: curve << 16)) {
            self.sendEnvironment()
            AsukaRuntime.shared.reportScreens()
        } completion: { _ in
            AsukaRuntime.shared.revealFocused()
        }
    }

    func sendEnvironment() {
        var e = AsukaEnvironment()
        let b = view.bounds
        e.width = Float(b.width)
        e.height = Float(b.height)
        let s = view.safeAreaInsets
        e.safeTop = Float(s.top)
        e.safeRight = Float(s.right)
        e.safeBottom = Float(s.bottom)
        e.safeLeft = Float(s.left)
        e.keyboard = Float(keyboard)
        let scale = traitCollection.displayScale
        e.scale = Float(scale > 0 ? scale : UIScreen.main.scale)
        // The text size the window's traits have, which its views' fonts
        // follow: the core measures again as they change (their change
        // makes followTextSize send it), never before the fonts have it.
        e.fontScale = asukaFontScale(of: view)
        e.dark = traitCollection.userInterfaceStyle == .dark
        e.reduceMotion = UIAccessibility.isReduceMotionEnabled
        e.screenReader = UIAccessibility.isVoiceOverRunning
        e.widthClass = traitCollection.horizontalSizeClass.asuka
        e.heightClass = traitCollection.verticalSizeClass.asuka
        e.setLanguages()
        guard e != lastEnv, e.width > 0 else { return }
        lastEnv = e
        let rt = AsukaRuntime.shared
        rt.scale = CGFloat(e.scale)
        rt.send(AsukaEvents.environment(rt.rootID, e))
    }
}

extension UIUserInterfaceSizeClass {
    /// As ops.SizeClass: compact, or regular as expanded.
    var asuka: UInt8 {
        switch self {
        case .compact: return 1
        case .regular: return 3
        default: return 0
        }
    }
}
#endif
