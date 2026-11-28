import UIKit

// The app around the Asuka host: a window scene showing the core's root,
// and on an iPad one for each Window the app opens (Windows.swift).

// AsukaAppDelegate: the name an app with a share sheet's main starts it by
// (its code is a framework, Frameworks/AsukaApp.framework).
@main
@objc(AsukaAppDelegate)
final class AppDelegate: UIResponder, UIApplicationDelegate {
    override init() {
        super.init()
        // asuka.json's shortcuts: their App Intents hand the host their URL.
        NotificationCenter.default.addObserver(forName: Notification.Name("AsukaOpenURL"), object: nil, queue: .main) { n in
            guard let s = n.userInfo?["url"] as? String, let u = URL(string: s) else { return }
            MainActor.assumeIsolated { AsukaRuntime.shared.openURL(u) }
        }
        #if ASUKA_WIDGET
        // platform/widget: its buttons' and toggles' App Intent, which the
        // system may launch the app (in the background) to run.
        AsukaWidgetData.install()
        #endif
    }

    func application(_ application: UIApplication, didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        #if ASUKA_PERIODIC
        // platform/periodic: its tasks' handlers, which the system wants before launch ends.
        AsukaPeriodic.shared.registerAtLaunch()
        #endif
        #if ASUKA_NOTIFY || ASUKA_PUSH || ASUKA_NOTIFICATION_SERVICE
        // platform/notify, platform/push: the center's delegate before launch
        // ends, to see the response that launched the app (a background
        // action launches it without a scene).
        AsukaNotifyCenter.shared.install()
        #endif
        // The app's packages' launch code (their manifests' ios.launch):
        // what they must make before launch ends, and the app's code at
        // once, without a screen, if the system launched the app for one.
        AsukaLaunch.run(options ?? [:])
        return true
    }

    func application(_ application: UIApplication, configurationForConnecting session: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        // Another role's scene (platform/car's CarPlay) is a package's: its
        // configuration is Info.plist's, with the package's delegate.
        if session.role != .windowApplication { return session.configuration }
        let c = UISceneConfiguration(name: nil, sessionRole: session.role)
        c.delegateClass = SceneDelegate.self
        return c
    }

    #if ASUKA_PUSH
    // platform/push: APNs' token, and data messages.
    func application(_ application: UIApplication, didRegisterForRemoteNotificationsWithDeviceToken token: Data) {
        AsukaPush.shared.registered(token)
    }

    func application(_ application: UIApplication, didFailToRegisterForRemoteNotificationsWithError error: Error) {
        AsukaPush.shared.failed(error)
    }

    func application(_ application: UIApplication, didReceiveRemoteNotification userInfo: [AnyHashable: Any],
                     fetchCompletionHandler done: @escaping (UIBackgroundFetchResult) -> Void) {
        #if os(tvOS)
        // A TV shows no alerts: every push that reaches the app is data.
        AsukaPush.shared.received(userInfo)
        #else
        // A notification with an alert comes through the center's delegate.
        if (userInfo["aps"] as? [String: Any])?["alert"] == nil { AsukaPush.shared.received(userInfo) }
        #endif
        done(.newData)
    }
    #endif

    // A package's background URLSession (std/http's uploads) has events:
    // the system woke the app, or started it again, for them.
    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        AsukaBackgroundSessions.handleEvents(identifier, completionHandler)
    }

    func applicationDidReceiveMemoryWarning(_ application: UIApplication) {
        AsukaRuntime.shared.lifecycle(.memoryWarning)
    }

    /// The user closed windows: a Window's closes (an iPad's).
    func application(_ application: UIApplication, didDiscardSceneSessions sessions: Set<UISceneSession>) {
        AsukaRuntime.shared.discardSessions(sessions)
        if let m = SceneDelegate.mainSession, sessions.contains(m) { SceneDelegate.mainSession = nil }
    }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    /// The main window's scene's session: an iPad's app shows its root in
    /// one, its Windows each in their own.
    static var mainSession: UISceneSession?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        // A Window's (an iPad's): its content, not the root (Windows.swift).
        if AsukaRuntime.shared.connectScene(ws, session: session, options: options) { return }
        // The system's New Window while the main window is open: the app
        // has one, which comes forward.
        if let m = SceneDelegate.mainSession, m !== session, m.scene != nil {
            if #available(iOS 17.0, *) {
                UIApplication.shared.activateSceneSession(for: UISceneSessionActivationRequest(session: m), errorHandler: nil)
            } else {
                UIApplication.shared.requestSceneSessionActivation(m, userActivity: nil, options: nil, errorHandler: nil)
            }
            UIApplication.shared.requestSceneSessionDestruction(session, options: nil, errorHandler: nil)
            return
        }
        SceneDelegate.mainSession = session
        let w = UIWindow(windowScene: ws)
        w.rootViewController = AsukaViewController()
        w.makeKeyAndVisible()
        window = w
        // Opened with a URL, or a universal link.
        AsukaRuntime.shared.openURLs(options.urlContexts.map { $0.url })
        for a in options.userActivities where a.activityType == NSUserActivityTypeBrowsingWeb {
            if let u = a.webpageURL { AsukaRuntime.shared.openURL(u) }
        }
    }

    func scene(_ scene: UIScene, openURLContexts contexts: Set<UIOpenURLContext>) {
        AsukaRuntime.shared.openURLs(contexts.map { $0.url })
    }

    func scene(_ scene: UIScene, continue activity: NSUserActivity) {
        if activity.activityType == NSUserActivityTypeBrowsingWeb, let u = activity.webpageURL {
            AsukaRuntime.shared.openURL(u)
        }
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        AsukaRuntime.shared.disconnectScene(scene)
    }

    func sceneDidBecomeActive(_ scene: UIScene) { AsukaRuntime.shared.scenesChanged() }
    func sceneWillResignActive(_ scene: UIScene) { AsukaRuntime.shared.scenesChanged() }

    /// Whether a scene other than scene is in front: the app is, whichever
    /// of its windows comes or goes (an iPad's).
    private func othersInFront(_ scene: UIScene) -> Bool {
        UIApplication.shared.connectedScenes.contains {
            $0 !== scene && $0 is UIWindowScene && ($0.activationState == .foregroundActive || $0.activationState == .foregroundInactive)
        }
    }

    func sceneWillEnterForeground(_ scene: UIScene) {
        AsukaRuntime.shared.scenesChanged()
        if othersInFront(scene) { return } // the app is in front already
        AsukaRuntime.shared.lifecycle(.foreground)
        #if ASUKA_WIDGET
        AsukaWidgetData.shared.drainIfStarted() // what the widgets' extension kept meanwhile
        #endif
    }
    func sceneDidEnterBackground(_ scene: UIScene) {
        AsukaRuntime.shared.scenesChanged()
        if othersInFront(scene) { return } // another of its windows keeps the app in front
        AsukaRuntime.shared.lifecycle(.background)
    }
}
