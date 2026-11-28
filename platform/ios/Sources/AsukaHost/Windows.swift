#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// A Window's window on an iPad: a window scene of its own, which the
// system shows beside the app's (Split View, Slide Over, Stage Manager,
// iPadOS 26's windows), as a Mac's host makes an NSWindow. The app's
// Info.plist supports several scenes when its code opens a Window (asuka
// sets UIApplicationSupportsMultipleScenes); without them, as on an
// iPhone, a Window shows nothing.
//
// A Window's scene is asked for with an NSUserActivity naming its node.
// Its session keeps the Window's state key (session.userInfo), so that
// the system bringing it back as the app launches again (its state
// restored) has the core open that Window again (RestoreWindow); a
// session no Window takes is closed. The user closing the window (its
// close button, a swipe in the app switcher) discards its session:
// Dismissed. The main window stays one: the system's New Window brings
// the app's own forward.

/// The activity a Window's scene is asked for with.
let asukaWindowActivity = "dev.asuka.window"
/// session.userInfo's key: the scene is a Window's, with this state key.
let asukaWindowKey = "AsukaWindow"

/// A Window node's window scene.
@MainActor final class AsukaSceneWin {
    let nodeID: UInt32
    /// Its Config's state key and least size; its Title.
    var stateKey = ""
    var minSize = CGSize.zero
    var title = ""
    var session: UISceneSession?
    var window: UIWindow?
    var controller: AsukaViewController?
    /// A scene was asked for, and hasn't connected yet.
    var asked = false
    /// Its WindowState as last reported, while the core listens.
    var lastState: [UInt8] = []

    init(_ id: UInt32) { nodeID = id }

    var scene: UIWindowScene? { window?.windowScene }
}

/// A scene the system connected for a Window that isn't open (brought
/// back as the app launches): it waits for the Window to open.
@MainActor final class AsukaWaitingScene {
    let scene: UIWindowScene
    let key: String
    var asked = false
    init(_ scene: UIWindowScene, _ key: String) {
        self.scene = scene
        self.key = key
    }
}

@MainActor extension AsukaRuntime {
    /// Whether a Window gets a window of its own: an iPad's, the app
    /// supporting several scenes.
    var windowsSupported: Bool {
        #if os(tvOS)
        return false
        #else
        return UIApplication.shared.supportsMultipleScenes
        #endif
    }

    /// The Window node's Config: its state key and least size (ops.WindowConfig).
    func setWindowConfig(_ n: AsukaNode, _ b: [UInt8]) {
        let w = sceneWin(n.id)
        func f32(_ i: Int) -> Float {
            guard i + 4 <= b.count else { return 0 }
            return Float(bitPattern: UInt32(b[i]) | UInt32(b[i + 1]) << 8 | UInt32(b[i + 2]) << 16 | UInt32(b[i + 3]) << 24)
        }
        w.minSize = CGSize(width: CGFloat(f32(8)), height: CGFloat(f32(12)))
        // f32 ×8, u32 flags, u8 title bar, u64 background, u32 parent, then the key.
        let k = 32 + 4 + 1 + 8 + 4
        if b.count >= k + 4 {
            let n = Int(b[k]) | Int(b[k + 1]) << 8 | Int(b[k + 2]) << 16 | Int(b[k + 3]) << 24
            if k + 4 + n <= b.count { w.stateKey = String(decoding: b[(k + 4)..<(k + 4 + n)], as: UTF8.self) }
        }
        if let s = w.scene { applyRestrictions(w, s) }
    }

    func setWindowTitle(_ n: AsukaNode, _ title: String) {
        let w = sceneWin(n.id)
        w.title = title
        w.scene?.title = title
    }

    /// The main window's title (WindowTitle): its scene's, which the app
    /// switcher and the window menus show.
    func setMainTitle(_ title: String) {
        controller?.view.window?.windowScene?.title = title
    }

    private func sceneWin(_ id: UInt32) -> AsukaSceneWin {
        if let w = sceneWins[id] { return w }
        let w = AsukaSceneWin(id)
        sceneWins[id] = w
        return w
    }

    private func applyRestrictions(_ w: AsukaSceneWin, _ s: UIWindowScene) {
        s.title = w.title
        #if !os(tvOS)
        // Its least size (minWidth, minHeight): Stage Manager's and
        // iPadOS 26's windows don't go smaller.
        if w.minSize.width > 0 || w.minSize.height > 0, let r = s.sizeRestrictions {
            r.minimumSize = CGSize(width: max(w.minSize.width, r.minimumSize.width), height: max(w.minSize.height, r.minimumSize.height))
        }
        #endif
    }

    /// Present: the Window opens. A scene the system brought back for its
    /// key takes it; otherwise a scene is asked for. Without several
    /// scenes (an iPhone) it shows nothing.
    func presentWindow(_ n: AsukaNode) {
        guard windowsSupported else { return }
        let w = sceneWin(n.id)
        guard w.window == nil, !w.asked else { return }
        if !w.stateKey.isEmpty, let i = waitingScenes.firstIndex(where: { $0.key == w.stateKey }) {
            let s = waitingScenes.remove(at: i)
            attachWindow(w, s.scene)
            return
        }
        w.asked = true
        #if !os(tvOS)
        // Its session from before (the app launched again with the Window
        // open), not connected yet: it comes back, rather than a new one.
        if !w.stateKey.isEmpty, let s = UIApplication.shared.openSessions.first(where: {
            $0.role == .windowApplication && ($0.userInfo?[asukaWindowKey] as? String) == w.stateKey
        }), !sceneWins.values.contains(where: { $0.session === s }) {
            if let ws = s.scene as? UIWindowScene, ws.windows.isEmpty {
                attachWindow(w, ws)
            } else if #available(iOS 17.0, *) {
                UIApplication.shared.activateSceneSession(for: UISceneSessionActivationRequest(session: s), errorHandler: nil)
            } else {
                UIApplication.shared.requestSceneSessionActivation(s, userActivity: nil, options: nil, errorHandler: nil)
            }
            return
        }
        #endif
        let a = NSUserActivity(activityType: asukaWindowActivity)
        a.userInfo = ["window": Int(n.id), "key": w.stateKey]
        let id = n.id
        let failed: (Error) -> Void = { err in
            MainActor.assumeIsolated {
                let rt = AsukaRuntime.shared
                print("asuka: couldn't open a window: \(err.localizedDescription)")
                guard let w = rt.sceneWins[id], w.window == nil else { return }
                w.asked = false
                rt.emit(id, .dismissed) { AsukaEvents.dismissed(id) }
            }
        }
        #if !os(tvOS)
        let opts = UIScene.ActivationRequestOptions()
        opts.requestingScene = controller?.view.window?.windowScene
        if #available(iOS 17.0, *) {
            let r = UISceneSessionActivationRequest(role: .windowApplication, userActivity: a, options: opts)
            UIApplication.shared.activateSceneSession(for: r, errorHandler: failed)
        } else {
            UIApplication.shared.requestSceneSessionActivation(nil, userActivity: a, options: opts, errorHandler: failed)
        }
        #endif
    }

    /// The core destroyed a Window's node: its scene closes.
    func destroyWindow(_ n: AsukaNode) {
        guard let w = sceneWins.removeValue(forKey: n.id) else { return }
        n.view.removeFromSuperview()
        if let s = w.session {
            s.userInfo = nil // never brought back
            #if !os(tvOS)
            UIApplication.shared.requestSceneSessionDestruction(s, options: nil, errorHandler: nil)
            #endif
        }
        w.window?.isHidden = true
        w.window = nil
        w.controller = nil
    }

    /// WindowAction on a Window's node (or the root, the main window's):
    /// on an iPad, Show and Focus bring its scene forward; the others are
    /// a desktop's.
    func windowAction(_ n: AsukaNode, _ args: UnsafeRawBufferPointer) {
        #if !os(tvOS)
        guard let a = args.first, a == UInt8(AsukaWindowAction.show.rawValue) || a == UInt8(AsukaWindowAction.focus.rawValue) else { return }
        let session = n.id == rootID ? controller?.view.window?.windowScene?.session : sceneWins[n.id]?.session
        guard let s = session, windowsSupported else { return }
        if #available(iOS 17.0, *) {
            UIApplication.shared.activateSceneSession(for: UISceneSessionActivationRequest(session: s), errorHandler: nil)
        } else {
            UIApplication.shared.requestSceneSessionActivation(s, userActivity: nil, options: nil, errorHandler: nil)
        }
        #endif
    }

    /// The controller a presentation over n (a Sheet's, an Alert's, a
    /// Snackbar's) goes on: its window's (its Window prop), the main
    /// window's otherwise, and the one presented over it.
    func topController(over n: AsukaNode) -> UIViewController? {
        var vc: UIViewController? = windowController(n.onWindow) ?? controller
        while let p = vc?.presentedViewController, !p.isBeingDismissed { vc = p }
        return vc
    }

    /// A Window node's scene's controller (nil: the main window's, or
    /// none).
    func windowController(_ id: UInt32) -> AsukaViewController? {
        id != 0 && id != rootID ? sceneWins[id]?.controller : nil
    }

    /// The host's controller of the window view is in: a Window's, else
    /// the main window's.
    func viewController(of view: UIView?) -> AsukaViewController? {
        (view?.window?.rootViewController as? AsukaViewController) ?? controller
    }

    // MARK: Scenes

    /// A window scene connects: a Window's (asked for with its activity,
    /// or brought back with its session), which it shows; false for the
    /// main window's.
    public func connectScene(_ scene: UIWindowScene, session: UISceneSession, options: UIScene.ConnectionOptions) -> Bool {
        // A Window's scene the system let go and connects again.
        if let w = sceneWins.values.first(where: { $0.session === session }), w.window == nil, nodes[w.nodeID] != nil {
            attachWindow(w, scene)
            return true
        }
        let act = options.userActivities.first { $0.activityType == asukaWindowActivity } ?? session.stateRestorationActivity
        if let a = act, a.activityType == asukaWindowActivity, let id = (a.userInfo?["window"] as? Int).map(UInt32.init),
           let w = sceneWins[id], w.window == nil, nodes[id] != nil {
            attachWindow(w, scene)
            return true
        }
        let key = (session.userInfo?[asukaWindowKey] as? String) ?? (act?.activityType == asukaWindowActivity ? act?.userInfo?["key"] as? String : nil)
        guard let key else { return false }
        // A Window's scene from before: its Window opens again, if the
        // app has it closed, and takes it; else it closes.
        if let w = sceneWins.values.first(where: { $0.window == nil && !key.isEmpty && $0.stateKey == key }), nodes[w.nodeID] != nil {
            attachWindow(w, scene)
            return true
        }
        waitingScenes.append(AsukaWaitingScene(scene, key))
        startHeadless()
        DispatchQueue.main.async { AsukaRuntime.shared.restoreWaiting() }
        return true
    }

    /// Asks the core to open the Windows the waiting scenes had, once the
    /// app's root is there (asuka dev's player gets the app later); those
    /// no Window takes close.
    func restoreWaiting() {
        guard rootID != 0 else { return } // setRoot asks again
        for s in waitingScenes where !s.asked {
            s.asked = true
            if !s.key.isEmpty { send(AsukaEvents.restoreWindow(rootID, s.key)) }
            if let i = waitingScenes.firstIndex(where: { $0 === s }) {
                waitingScenes.remove(at: i)
                s.scene.session.userInfo = nil
                #if !os(tvOS)
                UIApplication.shared.requestSceneSessionDestruction(s.scene.session, options: nil, errorHandler: nil)
                #endif
            }
        }
    }

    /// Shows the Window's content in scene.
    private func attachWindow(_ w: AsukaSceneWin, _ scene: UIWindowScene) {
        guard let n = nodes[w.nodeID] else { return }
        w.asked = false
        let vc = AsukaViewController()
        vc.windowNode = w.nodeID
        let win = UIWindow(windowScene: scene)
        win.rootViewController = vc
        if let c = accent { win.tintColor = c }
        win.overrideUserInterfaceStyle = colorScheme
        applyContentSize(win)
        w.window = win
        w.controller = vc
        w.session = scene.session
        scene.session.userInfo = [asukaWindowKey: w.stateKey]
        applyRestrictions(w, scene)
        vc.loadViewIfNeeded()
        vc.mount(n.view)
        win.makeKeyAndVisible()
    }

    /// A Window's scene went: the system let it go to save memory (its
    /// session stays, and it may connect again), or the user closed it.
    public func disconnectScene(_ scene: UIScene) {
        waitingScenes.removeAll { $0.scene === scene }
        guard let w = sceneWins.values.first(where: { $0.window?.windowScene === scene }) else { return }
        nodes[w.nodeID]?.view.removeFromSuperview()
        w.window = nil
        w.controller = nil
    }

    /// The user closed windows (their sessions discarded): a Window's
    /// tells the core, which closes it (Dismissed).
    public func discardSessions(_ sessions: Set<UISceneSession>) {
        for s in sessions {
            guard let w = sceneWins.values.first(where: { $0.session === s }) else { continue }
            let id = w.nodeID
            w.session = nil
            w.window = nil
            w.controller = nil
            nodes[id]?.view.removeFromSuperview()
            emit(id, .dismissed) { AsukaEvents.dismissed(id) }
        }
    }

    /// Whether scene is a Window's.
    public func isWindowScene(_ scene: UIScene) -> Bool {
        sceneWins.values.contains { $0.window?.windowScene === scene }
    }

    /// A Window's scene came forward or went back, or was resized: its
    /// WindowState, while the core listens.
    func reportWindowState(_ id: UInt32) {
        guard let w = sceneWins[id], let n = nodes[id], n.listens.contains(AsukaEventKind.windowState.rawValue) else { return }
        var b = AsukaWriter()
        let size = w.controller?.viewIfLoaded?.bounds.size ?? .zero
        for f in [Float(0), 0, Float(size.width), Float(size.height)] { b.f32(f) }
        var flags: UInt8 = 0
        if let s = w.scene, s.activationState == .foregroundActive || s.activationState == .foregroundInactive { flags |= 1 }
        if w.window?.isKeyWindow == true { flags |= 2 }
        b.u8(flags)
        guard b.bytes != w.lastState else { return }
        w.lastState = b.bytes
        send(AsukaEvents.windowState(id, b.bytes))
    }

    /// A scene's activation changed: the Windows' state.
    public func scenesChanged() {
        for id in sceneWins.keys { reportWindowState(id) }
    }
}
#endif
