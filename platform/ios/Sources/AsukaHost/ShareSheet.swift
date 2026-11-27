#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif
#if canImport(CAsuka)
@_implementationOnly import CAsuka
#endif

/// The share extension of an app with a share sheet of its own
/// (platform/sharesheet): its principal class, which runs the app in the
/// extension (ASUKA_SHARE_SHEET: platform/host's process is "share",
/// sharesheet.active() true, and the app block shows the sheet). iOS
/// shows it as a sheet over the app that shares. What's shared is
/// collected (AsukaShareCollector: files into the App Group) and sent to
/// the root as the Shared event (onShare); the root's ShareClose
/// (sharesheet.done, cancel, openApp) ends it.
@objc(AsukaShareSheetController)
public final class AsukaShareSheetController: UIViewController {
    private var collector: AsukaShareCollector?
    private var items: [AsukaShareCollector.Item] = []
    private var collected = false

    public override func viewDidLoad() {
        super.viewDidLoad()
        "ASUKA_SHARE_SHEET".withCString { k in "1".withCString { v in asuka_setenv(k, v) } }
        let rt = AsukaRuntime.shared
        rt.sheet = self
        let vc = AsukaViewController()
        addChild(vc)
        vc.view.frame = view.bounds
        vc.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(vc.view)
        vc.didMove(toParent: self)
        // The sheet goes on a swipe down only as the screen says (Close).
        isModalInPresentation = true
        let c = AsukaShareCollector(extensionContext)
        collector = c
        c.collect { [weak self] items in
            guard let self else { return }
            self.items = items
            self.collected = true
            self.deliver()
        }
    }

    /// Sends what's shared to the root, once there's both.
    func deliver() {
        let rt = AsukaRuntime.shared
        guard collected, rt.started, rt.rootID != 0, let c = collector else { return }
        var w = AsukaWriter()
        w.u32(UInt32(items.count))
        for it in items {
            w.u8(UInt8(clamping: it.kind))
            w.string(it.text)
            w.string(c.absolute(it))
            w.string(it.name)
            w.string(it.mime)
            w.u64(UInt64(bitPattern: it.size))
        }
        collected = false // once
        rt.send(AsukaEvents.shared(rt.rootID, w.bytes))
    }

    /// The root's ShareClose: 0 done, 1 cancelled, 2 open the app with
    /// what's shared (its files stay in the App Group for it).
    func close(_ how: UInt8) {
        guard let ctx = extensionContext, let c = collector else { return }
        collector = nil
        if how == 2, let url = c.shareURL(items) {
            AsukaShareCollector.openApp(url, from: self) { _ in ctx.completeRequest(returningItems: nil) }
            return
        }
        switch how {
        case 1:
            c.remove(items)
            ctx.cancelRequest(withError: NSError(domain: NSCocoaErrorDomain, code: NSUserCancelledError))
        default:
            c.remove(items)
            ctx.completeRequest(returningItems: nil)
        }
    }
}

/// What an app with a share sheet or a push rewrite shares with its
/// extensions, which run the app too (Info.plist's AsukaShareSheet,
/// AsukaNotificationService): its files, in the App Group's container,
/// which all see.
enum AsukaShared {
    /// The App Group, when the app runs in an extension too; nil without.
    static var group: String? {
        let info = Bundle.main.infoDictionary ?? [:]
        guard info["AsukaShareSheet"] as? Bool == true || info["AsukaNotificationService"] as? Bool == true else { return nil }
        return info["AsukaAppGroup"] as? String
    }

    /// The directory the app keeps its files in (std/fs's appDir):
    /// Application Support, the App Group's when the app has extensions
    /// that run it. The first time the app runs with them, what it kept
    /// in its own moves there. On Apple TV, Caches: tvOS lets an app
    /// write nowhere else (Application Support and Documents can't be
    /// made or written on a device, though the simulator allows them),
    /// and may clear it while the app isn't running.
    static func appDir() -> URL? {
        let fm = FileManager.default
        #if os(tvOS) // no extensions, so no App Group
        return fm.urls(for: .cachesDirectory, in: .userDomainMask).first?.appendingPathComponent("asuka-app", isDirectory: true)
        #else
        let own = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
        guard let g = group, let c = fm.containerURL(forSecurityApplicationGroupIdentifier: g) else { return own }
        let shared = c.appendingPathComponent("Library/Application Support", isDirectory: true)
        if !Bundle.main.bundlePath.hasSuffix(".appex"), let own, !fm.fileExists(atPath: shared.path), fm.fileExists(atPath: own.path) {
            try? fm.createDirectory(at: shared.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.moveItem(at: own, to: shared)
        }
        return shared
        #endif
    }
}
#endif
