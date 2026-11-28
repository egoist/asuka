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
    /// A Window's scene's controller: the Window node it shows (0: the
    /// main window's, the root's).
    var windowNode: UInt32 = 0
    /// Follows the keyboard's top (UIKeyboardLayoutGuide), which moves
    /// with the finger as a list's interactive dismissal drags it down,
    /// when no notification says so.
    private let keyboardTracker = UIView()
    /// The field that took the focus last (fieldFocus).
    private weak var keyboardField: UIResponder?
    /// Whether the layout guide waits: set as focus moves from one field to
    /// another with an accessory on either (fieldFocus), cleared by the
    /// keyboard's next notification (keyboardWillChange).
    private var keyboardHeld = false
    /// The app's undo stack as UIKit's undo (Undo.swift).
    lazy var appUndo = AsukaUndoManager(state: { AsukaRuntime.shared.undoState }, perform: { AsukaRuntime.shared.undo($0) })
    var undoResponderPending = false
    /// The text field or text view being edited, which has its own undo.
    weak var textBeingEdited: UIResponder?

    public override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        let nc = NotificationCenter.default
        #if !os(tvOS) // a TV's keyboard covers the screen: nothing moves for it
        nc.addObserver(self, selector: #selector(keyboardWillChange(_:)), name: UIResponder.keyboardWillChangeFrameNotification, object: nil)
        nc.addObserver(self, selector: #selector(keyboardWillChange(_:)), name: UIResponder.keyboardWillHideNotification, object: nil)
        #endif
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
        #if !os(tvOS)
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
        #endif
        let rt = AsukaRuntime.shared
        rt.scale = traitCollection.displayScale > 0 ? traitCollection.displayScale : UIScreen.main.scale
        sendEnvironment()
        if windowNode == 0 { rt.start(self) } // a Window's shows its node (Windows.swift)
        observeTextEditing()
    }

    /// The core's undo stack (Undo.swift), while the app uses it and no
    /// text field is being edited: that one undoes its typing with the
    /// window's undo manager.
    public override var undoManager: UndoManager? {
        guard AsukaRuntime.shared.rootUndoes else { return super.undoManager }
        if editingText { return super.undoManager }
        return appUndo
    }

    /// While the app uses its undo stack, the first responder when
    /// nothing else is, so a shake or ⌘Z reaches its undo manager.
    public override var canBecomeFirstResponder: Bool { AsukaRuntime.shared.rootUndoes }

    #if !os(tvOS) // a TV neither rotates nor has a status bar
    // platform/screen's lock and status bar.
    public override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        AsukaScreen.lock ?? super.supportedInterfaceOrientations
    }
    public override var preferredStatusBarStyle: UIStatusBarStyle { AsukaScreen.statusBarStyle }
    public override var prefersStatusBarHidden: Bool { AsukaScreen.statusBarHidden }
    #endif

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
        #if os(tvOS)
        if AsukaRuntime.shared.remotePressBegan(presses) { return }
        // Menu with nothing to go back to: the root's ui.OnBack, else
        // tvOS's own (the Home screen).
        if presses.first?.type == .menu, AsukaRuntime.shared.rootBack(presses) { return }
        #endif
        super.pressesBegan(presses, with: event)
    }

    #if os(tvOS)
    public override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if AsukaRuntime.shared.remotePressTaken(presses) { return }
        super.pressesEnded(presses, with: event)
    }

    // The remote's moves, which a view's Keys take first (TV.swift).
    public override func shouldUpdateFocus(in context: UIFocusUpdateContext) -> Bool {
        if AsukaRuntime.shared.remoteFocusMove(context) { return false }
        return super.shouldUpdateFocus(in: context)
    }
    #endif

    func mount(_ v: UIView) {
        if root !== v { root?.removeFromSuperview() }
        root = v
        if windowNode != 0 {
            // A Window's content fills its scene, as the core lays it out.
            v.frame = view.bounds
            v.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        }
        view.addSubview(v)
        AsukaRuntime.shared.raiseOverlays(in: view) // the overlays over it
    }

    public override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        AsukaRuntime.shared.adoptControllers(in: self)
        if let c = AsukaRuntime.shared.accent { view.window?.tintColor = c }
        if AsukaRuntime.shared.colorScheme != .unspecified { view.window?.overrideUserInterfaceStyle = AsukaRuntime.shared.colorScheme }
        if let w = view.window { AsukaRuntime.shared.setWindowDirection(w) }
        if let w = view.window { AsukaRuntime.shared.applyContentSize(w) }
    }

    public override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        // Outside presses for the overlays, and that the last input was a touch.
        if let w = view.window { AsukaRuntime.shared.observeTouches(in: w) }
        takeUndoResponder()
    }

    public override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let moved = trackKeyboard()
        updateKeyboard()
        sendEnvironment()
        if moved { AsukaRuntime.shared.reportScreens() }
        if windowNode != 0 { AsukaRuntime.shared.reportWindowState(windowNode) }
    }

    /// The keyboard's top as the layout guide has it: during an
    /// interactive dismissal (a list's keyboardDismissMode), the keyboard
    /// follows the finger with no notification, and what sits on it (a
    /// chat's composer) follows the guide, frame by frame. Notified
    /// changes agree with it once their animation is set up. While the
    /// keyboard is held (fieldFocus), the guide isn't followed.
    private func trackKeyboard() -> Bool {
        #if os(tvOS)
        return false
        #else
        guard view.window != nil, !keyboardHeld else { return false }
        var h = keyboardTracker.frame.height
        if #unavailable(iOS 17.0) {
            // The guide sits on the safe area while the keyboard's away.
            if h <= view.safeAreaInsets.bottom + 0.5 { h = 0 }
        }
        h = max(0, min(h, view.bounds.height))
        guard abs(h - keyboard) > 0.5 else { return false }
        setKeyboardEnd(height: h)
        return true
        #endif
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
            let app = asukaContentSize
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

    #if !os(tvOS)
    @objc func keyboardWillChange(_ note: Notification) {
        let info = note.userInfo ?? [:]
        if note.name == UIResponder.keyboardWillHideNotification {
            keyboardEnd = nil
        } else {
            keyboardEnd = (info[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue)?.cgRectValue
        }
        keyboardHeld = false  // the keyboard's own word on a move held for (fieldFocus)
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
    #endif

    /// A field takes the focus: become is its becomeFirstResponder.
    ///
    /// When the focus comes from another field with the keyboard up and
    /// either has an accessory (inputAccessoryView), UIKit swaps their
    /// bars, and the keyboard's layout guide has the keyboard shrink by the
    /// old bar and grow by the new one, a few layout passes in a row:
    /// followed, what sits on the keyboard would drop and come back up. So
    /// the keyboard is held: the guide isn't followed until the keyboard's
    /// next notification, which is followed as ever, with its animation. On
    /// the on-screen keyboard that notification follows the swap's layout
    /// passes, and a real change (another keyboard, another bar's height)
    /// moves the views with the keyboard.
    ///
    /// With a hardware keyboard, where only the bars show (the keyboard is
    /// as tall as the old bar), the swap sends no notification, and the
    /// guide stays at 0 after it. There the keyboard is the new bar, known
    /// now, and the hold lasts until the keyboard's next notification (the
    /// field losing the focus, say): till then an interactive dismissal
    /// isn't followed.
    func fieldFocus(_ field: UIResponder, _ become: () -> Bool) -> Bool {
        let from = keyboardField
        let oldBar = from?.inputAccessoryView?.frame.height ?? 0
        let swap = keyboard > 0 && from !== field && from?.isFirstResponder == true
            && (from?.inputAccessoryView != nil || field.inputAccessoryView != nil)
        if swap { keyboardHeld = true }
        let took = become()
        if !took {
            if swap {
                keyboardHeld = false
                view.setNeedsLayout()
            }
            return false
        }
        keyboardField = field
        if swap && abs(keyboard - oldBar) <= 0.5 {
            // The bars swap at once, and so do the views.
            setKeyboardEnd(height: field.inputAccessoryView?.frame.height ?? 0)
            updateKeyboard()
            sendEnvironment()
            AsukaRuntime.shared.reportScreens()
        }
        return true
    }

    /// The keyboard's frame, as tall as h, from the view's bottom.
    private func setKeyboardEnd(height h: CGFloat) {
        guard h >= 0.5, let window = view.window else {
            keyboardEnd = nil
            return
        }
        let r = CGRect(x: 0, y: view.bounds.maxY - h, width: view.bounds.width, height: h)
        keyboardEnd = view.convert(r, to: window.screen.coordinateSpace)
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
        e.tv = traitCollection.userInterfaceIdiom == .tv
        let rt = AsukaRuntime.shared
        e.multipleWindows = rt.windowsSupported
        e.setLanguages()
        guard e != lastEnv, e.width > 0 else { return }
        lastEnv = e
        rt.scale = CGFloat(e.scale)
        rt.send(AsukaEvents.environment(windowNode != 0 ? windowNode : rt.rootID, e))
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
