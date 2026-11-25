// platform/screen on iOS and the Mac. iOS: the screens (the device's own
// first, those its scenes show on after it, in a row),
// the host's orientation lock and status bar (AsukaScreen, which its
// controllers read), a cover over the window while the screen's recorded
// or mirrored, UIApplication's screenshot notification, and the screen's
// brightness, the user's put back as the app goes. The Mac:
// NSScreen's screens (the first has the menu bar), its windows left out
// of captures (NSWindow.sharingType); the orientation and the bars are a
// phone's.
import Foundation
import Asuka
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

final class SystemScreens: Screens {
    func displays() throws -> [Display] {
        if Thread.isMainThread { return MainActor.assumeIsolated { Self.read() } }
        return DispatchQueue.main.sync { MainActor.assumeIsolated { Self.read() } }
    }

    func watchDisplays(changed: @escaping ([Display]) -> Void) async throws {
        let w = await MainActor.run { DisplaysWatch(changed) }
        defer { Task { @MainActor in w.stop() } }
        await Self.untilCancelled()
    }

    @MainActor func lock(orientation: Orientation) throws {
        #if canImport(UIKit)
        AsukaScreen.setLock(orientation.rawValue)
        #endif
    }

    @MainActor func setBars(style: BarStyle, hideStatus: Bool, hideNavigation: Bool) throws {
        #if canImport(UIKit)
        AsukaScreen.setBars(style: style.rawValue, hideStatus: hideStatus)
        #endif
    }

    @MainActor func preventCapture(on: Bool) throws { CaptureGuard.shared.set(on) }

    @MainActor func setBrightness(level: Double) throws {
        #if canImport(UIKit)
        BrightnessKeeper.shared.set(level < 0 ? nil : CGFloat(level))
        #endif
    }

    @MainActor func brightness() throws -> Double {
        #if canImport(UIKit)
        return Double(BrightnessKeeper.shared.level ?? BrightnessKeeper.screen.brightness)
        #else
        return -1 // a Mac's displays are the user's to set
        #endif
    }

    func watchScreenshots(taken: @escaping (Int) -> Void) async throws {
        #if canImport(UIKit)
        let w = await MainActor.run { ScreenshotWatch(taken) }
        defer { Task { @MainActor in w.stop() } }
        #endif
        await Self.untilCancelled()
    }

    static func untilCancelled() async {
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    /// The displays now, the primary first.
    @MainActor static func read() -> [Display] {
        #if canImport(UIKit)
        // The device's own screen, then those other scenes show on (a
        // display the iPhone mirrors or extends to: AirPlay, a cable).
        let main = UIScreen.main
        var screens = [main]
        for case let w as UIWindowScene in UIApplication.shared.connectedScenes where !screens.contains(where: { $0 === w.screen }) {
            screens.append(w.screen)
        }
        var x = 0.0
        return screens.enumerated().map { i, s in
            let b = s.bounds
            let r = Rect(x: x, y: 0, width: b.width, height: b.height)
            x += b.width
            return Display(id: s === main ? "main" : "external-\(i)", name: s === main ? "Built-in Display" : "External Display",
                           bounds: r, workArea: r, scale: s.scale, refreshRate: Double(s.maximumFramesPerSecond), primary: s === main)
        }
        #else
        // AppKit's global coordinates are points from the primary
        // display's bottom left corner, y up; Asuka's from its top left,
        // y down.
        let screens = NSScreen.screens
        let primaryH = screens.first?.frame.height ?? 0
        func flip(_ r: NSRect) -> Rect { Rect(x: r.minX, y: primaryH - r.minY - r.height, width: r.width, height: r.height) }
        return screens.enumerated().map { i, s in
            var d = Display(bounds: flip(s.frame), workArea: flip(s.visibleFrame), scale: s.backingScaleFactor, primary: i == 0)
            if let n = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber { d.id = String(n.uint32Value) }
            if #available(macOS 10.15, *) { d.name = s.localizedName }
            if #available(macOS 12, *) { d.refreshRate = Double(s.maximumFramesPerSecond) }
            return d
        }
        #endif
    }

    static func same(_ a: [Display], _ b: [Display]) -> Bool {
        guard a.count == b.count else { return false }
        for (x, y) in zip(a, b) {
            if x.id != y.id || x.name != y.name || x.scale != y.scale || x.refreshRate != y.refreshRate || x.primary != y.primary { return false }
            for (r, s) in [(x.bounds, y.bounds), (x.workArea, y.workArea)] {
                if r.x != s.x || r.y != s.y || r.width != s.width || r.height != s.height { return false }
            }
        }
        return true
    }
}

/// Follows the screens: reported at once, then as they change.
@MainActor final class DisplaysWatch: NSObject {
    private let changed: ([Display]) -> Void
    private var last: [Display]?

    init(_ changed: @escaping ([Display]) -> Void) {
        self.changed = changed
        super.init()
        let nc = NotificationCenter.default
        #if canImport(UIKit)
        for n in [UIScene.willConnectNotification, UIScene.didDisconnectNotification, UIScreen.modeDidChangeNotification, UIDevice.orientationDidChangeNotification] {
            nc.addObserver(self, selector: #selector(report), name: n, object: nil)
        }
        #else
        nc.addObserver(self, selector: #selector(report), name: NSApplication.didChangeScreenParametersNotification, object: nil)
        #endif
        report()
    }

    @objc func report() {
        let ds = SystemScreens.read()
        if let l = last, SystemScreens.same(l, ds) { return }
        last = ds
        changed(ds)
    }

    func stop() { NotificationCenter.default.removeObserver(self) }
}

/// Keeps the app out of captures while asked: iOS covers the window while
/// the screen's recorded, mirrored or on AirPlay; the Mac leaves its
/// windows out (sharingType none), those that show later too.
@MainActor final class CaptureGuard: NSObject {
    static let shared = CaptureGuard()
    private var on = false
    private var observing = false
    #if canImport(UIKit)
    private var cover: UIView?
    #endif

    func set(_ on: Bool) {
        self.on = on
        if !observing {
            observing = true
            let nc = NotificationCenter.default
            #if canImport(UIKit)
            nc.addObserver(self, selector: #selector(apply), name: UIScreen.capturedDidChangeNotification, object: nil)
            #else
            nc.addObserver(self, selector: #selector(apply), name: NSWindow.didBecomeKeyNotification, object: nil)
            nc.addObserver(self, selector: #selector(apply), name: NSWindow.didBecomeMainNotification, object: nil)
            #endif
        }
        apply()
    }

    @objc func apply() {
        #if canImport(UIKit)
        guard let window = AsukaNative.window else { return }
        if on && window.windowScene?.screen.isCaptured == true {
            let v = cover ?? {
                let v = UIView(frame: window.bounds)
                v.autoresizingMask = [.flexibleWidth, .flexibleHeight]
                v.backgroundColor = .systemBackground
                return v
            }()
            cover = v
            window.addSubview(v)
            window.bringSubviewToFront(v)
        } else {
            cover?.removeFromSuperview()
        }
        #else
        for w in NSApp.windows { w.sharingType = on ? .none : .readOnly }
        #endif
    }
}

#if canImport(UIKit)
/// The app's brightness: UIScreen's is the device's, kept after the app
/// leaves, so the user's is put back as the app goes into the background
/// and the app's set again as it comes back, as Wallet does with a pass.
@MainActor final class BrightnessKeeper: NSObject {
    static let shared = BrightnessKeeper()
    private(set) var level: CGFloat? // the app's; nil: the system's
    private var users: CGFloat? // the user's, while the app's shows
    private var observing = false

    static var screen: UIScreen { AsukaNative.window?.windowScene?.screen ?? UIScreen.main }

    func set(_ l: CGFloat?) {
        if !observing {
            observing = true
            let nc = NotificationCenter.default
            nc.addObserver(self, selector: #selector(leave), name: UIApplication.willResignActiveNotification, object: nil)
            nc.addObserver(self, selector: #selector(back), name: UIApplication.didBecomeActiveNotification, object: nil)
        }
        if l == nil { leave() }
        level = l.map { min(max($0, 0), 1) }
        if UIApplication.shared.applicationState == .active { back() }
    }

    /// The user's brightness again.
    @objc func leave() {
        if let u = users { Self.screen.brightness = u }
        users = nil
    }

    /// The app's brightness, the user's kept to put back.
    @objc func back() {
        guard let l = level else { return }
        if users == nil { users = Self.screen.brightness }
        Self.screen.brightness = l
    }
}

/// Counts the screenshots the user takes while watched.
@MainActor final class ScreenshotWatch: NSObject {
    private let taken: (Int) -> Void
    private var count = 0

    init(_ taken: @escaping (Int) -> Void) {
        self.taken = taken
        super.init()
        NotificationCenter.default.addObserver(self, selector: #selector(took), name: UIApplication.userDidTakeScreenshotNotification, object: nil)
    }

    @objc func took() {
        count += 1
        taken(count)
    }

    func stop() { NotificationCenter.default.removeObserver(self) }
}
#endif

func makeScreens() throws -> any Screens { SystemScreens() }
