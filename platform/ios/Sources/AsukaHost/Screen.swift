#if canImport(UIKit) && !os(tvOS)
import UIKit

/// What platform/screen sets of the app's screen: the orientation lock and
/// the status bar, which the root and the screens' controllers read.
@MainActor public enum AsukaScreen {
    /// The app's lock (screen.lock); nil: Info.plist's orientations.
    static var lock: UIInterfaceOrientationMask?
    static var statusBarStyle: UIStatusBarStyle = .default
    static var statusBarHidden = false

    /// Locks the app to an orientation, as screen.Orientation numbers
    /// them: any (unlocked), portrait, portraitUp, landscape,
    /// landscapeLeft, landscapeRight (platform/screen).
    public static func setLock(_ o: Int, _ runtime: AsukaRuntime? = nil) {
        let rt = runtime ?? .shared
        let iPad = UIDevice.current.userInterfaceIdiom == .pad
        let masks: [UIInterfaceOrientationMask?] = [nil, iPad ? [.portrait, .portraitUpsideDown] : .portrait, .portrait,
                                                    .landscape, .landscapeLeft, .landscapeRight]
        var mask = o >= 0 && o < masks.count ? masks[o] : nil
        // Only what Info.plist allows: UIKit throws for a lock with none of it.
        if let m = mask, let w = rt.controller?.view.window {
            let allowed = UIApplication.shared.supportedInterfaceOrientations(for: w)
            mask = m.intersection(allowed).isEmpty ? nil : m.intersection(allowed)
        }
        lock = mask
        if let c = rt.controller {
            c.setNeedsUpdateOfSupportedInterfaceOrientations()
            if let m = mask { c.view.window?.windowScene?.requestGeometryUpdate(.iOS(interfaceOrientations: m)) }
        }
    }

    /// The status bar's style (auto, light, dark) and whether it hides
    /// (platform/screen).
    public static func setBars(style: Int, hideStatus: Bool, _ runtime: AsukaRuntime? = nil) {
        let rt: AsukaRuntime? = runtime ?? .shared
        statusBarStyle = style == 1 ? .lightContent : style == 2 ? .darkContent : .default
        statusBarHidden = hideStatus
        var vc: UIViewController? = rt?.controller
        while let c = vc {
            UIView.animate(withDuration: 0.25) { c.setNeedsStatusBarAppearanceUpdate() }
            vc = c.presentedViewController
        }
    }
}
#endif

#if os(tvOS)
import UIKit

/// tvOS: a TV neither rotates nor shows a status bar.
@MainActor public enum AsukaScreen {
    public static func setLock(_ o: Int, _ runtime: AsukaRuntime? = nil) {}
    public static func setBars(style: Int, hideStatus: Bool, _ runtime: AsukaRuntime? = nil) {}
}
#endif
