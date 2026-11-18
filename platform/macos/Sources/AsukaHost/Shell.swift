// The app as a whole, on a Mac: its activation policy (an accessory in
// the background), quitting (the Quit command, the user's, a signal's,
// the session's end: each asks the app once through asuka_quit_request),
// reopening (the Dock icon clicked with no window: the main window comes
// back), its lifecycle as AppKit reports it, the links and files it's
// opened with, and the shell's modules. The menu bar is MenuBar.swift's,
// the tray and the Dock Tray.swift's and Dock.swift's, the files opened
// and the recent documents Files.swift's.
//
// One instance: a Mac app runs once through Launch Services, which hands
// a second launch's files to the running app as Apple Events
// (application(_:open:)), not arguments: the Mac host never sends
// SecondInstance (DESIGN.md).
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif
#if canImport(CAsuka)
@_implementationOnly import CAsuka
#endif

@MainActor final class AsukaShell: NSObject {
    static let shared = AsukaShell()
    var rt: AsukaRuntime { AsukaRuntime.shared }

    /// Regular (a Dock icon and a menu bar), or an accessory in the
    /// background (no Dock icon, not in ⌘Tab).
    var activationPolicy: NSApplication.ActivationPolicy { inBackground ? .accessory : .regular }

    /// The app runs in the background: asuka.toml's background, or its
    /// bundle says so (LSUIElement).
    var inBackground: Bool {
        if rt.config.background { return true }
        if let v = Bundle.main.object(forInfoDictionaryKey: "LSUIElement") {
            if let b = v as? Bool { return b }
            if let s = v as? String { return s == "1" || s.lowercased() == "yes" || s.lowercased() == "true" }
            if let n = v as? NSNumber { return n.boolValue }
        }
        return false
    }

    /// The app keeps running with no window (asuka.toml's keepRunning).
    var keepRunning: Bool { rt.config.keepRunning }

    // MARK: Menus (MenuBar.swift)

    /// The app's MenuBar menus, and their items by number.
    var appMenus: [AsukaMenuBarMenu] = []
    var appItems: [AsukaMenuItem] = []

    // MARK: Quitting

    /// A quit the app allowed only finishes.
    var quitAllowed = false
    /// The session ends (NSWorkspace.willPowerOffNotification): the quit
    /// can't be refused.
    var sessionEnding = false
    /// The Quit command asked to relaunch.
    var relaunchAsked = false
    var signalSources: [DispatchSourceSignal] = []

    /// Under asuka dev the player isn't restarted: asuka dev sends it each new module and
    /// the core keeps the state itself. Nothing to wait for.
    func waitForPreviousRun() {}

    /// Nothing to restore: asuka dev keeps the app's state itself
    /// (asuka_dev_reload).
    func restoreState() {}

    /// Before the core starts: what must be in place before the app's
    /// launch ends: quitting's handlers, the launch's Apple Events.
    func beforeCore() {
        AsukaNotify.install() // platform/notify: the center's delegate before launch ends
        startQuit()
        startFiles()
        AsukaUpdater.cleanAtLaunch() // what an update left beside the bundle
    }

    /// The Location command: a window's Router address, kept.
    func location(_ b: [UInt8]) { AsukaRoutes.shared.location(b) }
    func flushRoutes() { AsukaRoutes.shared.flush() }

    /// The Quit command: the app quits as the user's Quit does.
    func quit() { quit([]) }

    /// The Quit command with its args: u8 flags, 1 relaunch.
    func quit(_ args: [UInt8]) {
        relaunchAsked = args.first.map { $0 & 1 != 0 } ?? false
        terminateLater()
    }

    /// The application's terminate:, from the run loop: not from within a
    /// call into the core, which the quit's last events would wait for.
    func terminateLater() {
        DispatchQueue.main.async {
            MainActor.assumeIsolated { NSApp.terminate(nil) }
        }
    }

    /// Quits however it starts (⌘Q, the Dock's Quit, the Quit command, the
    /// last window closing, a signal, the session ending): AppKit asks the
    /// delegate first, which asks the app's on quit handlers once; a quit
    /// they allowed only finishes. The session's end can't be canceled.
    func shouldTerminate() -> NSApplication.TerminateReply {
        if quitAllowed { return .terminateNow }
        let ending = sessionEnding
        sessionEnding = false
        var canceled = false
        if rt.started {
            rt.enter { canceled = asuka_quit_request(ending ? 1 : 0) != 0 }
        }
        if canceled && !ending {
            relaunchAsked = false // the relaunch goes with the quit
            return .terminateCancel
        }
        quitAllowed = true
        return .terminateNow
    }

    /// SIGINT and SIGTERM (Ctrl+C in the terminal, asuka dev stopping the
    /// app) quit as ⌘Q does; a second one ends the app at once (its main
    /// thread may be stuck). The session's end is noted: it can't be
    /// refused.
    func startQuit() {
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(willPowerOff(_:)),
            name: NSWorkspace.willPowerOffNotification, object: nil)
        for sig in [SIGINT, SIGTERM] {
            signal(sig, SIG_IGN)
            let s = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            s.setEventHandler { MainActor.assumeIsolated { NSApp.terminate(nil) } }
            s.resume()
            signalSources.append(s)
        }
        // A second signal while the main thread is stuck: a thread of its
        // own ends the app.
        asukaWatchSecondSignal()
    }

    @objc func willPowerOff(_ note: Notification) { sessionEnding = true }

    /// The app quits: what's kept is written, a staged update put in place
    /// (and the app opened again, if the quit asked).
    func willTerminate() {
        rt.lifecycle(.background)
        flushRoutes()
        let relaunch = relaunchAsked && !sessionEnding
        if !applyStagedUpdate(relaunch: relaunch) && relaunch { relaunchApp() }
    }

    /// Starts the app again once this process has ended: a shell of its own
    /// waits for this pid to exit, then opens the bundle through Launch
    /// Services (so the Dock keeps its tile), or runs the executable again
    /// with the same arguments when it isn't a bundle.
    func relaunchApp() {
        let pid = ProcessInfo.processInfo.processIdentifier
        let bundle = Bundle.main.bundleURL
        var script = "while kill -0 \(pid) 2>/dev/null; do sleep 0.1; done; "
        let args = CommandLine.arguments.dropFirst().map(asukaShellQuote).joined(separator: " ")
        if bundle.pathExtension == "app" {
            script += "/usr/bin/open -n " + asukaShellQuote(bundle.path) + (args.isEmpty ? "" : " --args " + args)
        } else {
            script += "cd " + asukaShellQuote(FileManager.default.currentDirectoryPath) + " && exec " +
                asukaShellQuote(Bundle.main.executablePath ?? CommandLine.arguments[0]) + " " + args
        }
        asukaDetached("/bin/sh", ["-c", script])
    }

    /// The Dock icon clicked (or the app opened again) with no window
    /// showing, as when one that keeps running has closed its main window:
    /// the main window comes back.
    /// An app without a main window has none to bring back.
    func reopen(_ visible: Bool) -> Bool {
        guard !visible, let m = rt.main, !m.cfg.flags.contains(.none), !m.window.isVisible else { return true }
        let w = m.window
        if w.isMiniaturized { w.deminiaturize(nil) }
        w.makeKeyAndOrderFront(nil)
        if inBackground { NSApp.activate(ignoringOtherApps: true) }
        return true
    }

    // MARK: The automation

    /// What the tray icons show and their menus hold, and the Dock menu's,
    /// for the dump.
    var trayDump: String { AsukaTray.shared.dump() }
    /// Writes each tray icon's button beside a snapshot (path.tray.png).
    func traySnapshots(_ path: String) { AsukaTray.shared.snapshots(path) }
    /// What the shell adds to the automation's JSON: the tray, the Dock
    /// menu.
    func dumpJSON(_ d: inout [String: Any]) {
        // The menu bar as validated (its items enabled as they'd show), with
        // each item's action and tag.
        d["menuBar"] = menuJSON(NSApp.mainMenu)
        AsukaTray.shared.json(&d)
        d["app"] = ["active": NSApp.isActive, "policy": NSApp.activationPolicy().rawValue,
                    "terminatesAfterLastWindow": AsukaAppDelegate.shared.applicationShouldTerminateAfterLastWindowClosed(NSApp),
                    "background": inBackground, "keepRunning": keepRunning]
    }

    /// A menu for the automation's JSON (Automation's menuJSON), validated
    /// first, with each item's action and tag.
    func menuJSON(_ m: NSMenu?) -> [[String: Any]] {
        guard let m else { return [] }
        m.update()
        let base = AsukaAutomation.shared.menuJSON(m)
        return zip(m.items, base).map { it, b in
            var d = b
            if let a = it.action { d["action"] = NSStringFromSelector(a) }
            if it.tag != 0 { d["tag"] = it.tag }
            if let s = it.submenu {
                if let del = s.delegate { del.menuNeedsUpdate?(s) }
                d["items"] = menuJSON(s)
            }
            return d
        }
    }
}

/// A word for /bin/sh, quoted.
func asukaShellQuote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

/// Runs a program in a session of its own, not waiting for it: it outlives
/// the app.
func asukaDetached(_ path: String, _ args: [String]) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/nohup")
    p.arguments = [path] + args
    p.standardInput = FileHandle.nullDevice
    p.standardOutput = FileHandle.nullDevice
    p.standardError = FileHandle.nullDevice
    try? p.run()
}

/// Ends the process at a second SIGINT or SIGTERM even while the main
/// thread is stuck: a thread of its own waits for them (the main queue's
/// sources can't run then).
private func asukaWatchSecondSignal() {
    Thread.detachNewThread {
        var seen = 0
        let kq = kqueue()
        guard kq >= 0 else { return }
        var evs = [kevent(ident: UInt(SIGINT), filter: Int16(EVFILT_SIGNAL), flags: UInt16(EV_ADD), fflags: 0, data: 0, udata: nil),
                   kevent(ident: UInt(SIGTERM), filter: Int16(EVFILT_SIGNAL), flags: UInt16(EV_ADD), fflags: 0, data: 0, udata: nil)]
        _ = kevent(kq, &evs, 2, nil, 0, nil)
        var out = kevent()
        while kevent(kq, nil, 0, &out, 1, nil) >= 0 {
            seen += Int(out.data)
            if seen >= 2 { Darwin.exit(128 + Int32(out.ident)) }
        }
    }
}

// MARK: The application's delegate

extension AsukaAppDelegate {
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        AsukaShell.shared.shouldTerminate()
    }

    func applicationWillTerminate(_ notification: Notification) {
        AsukaShell.shared.willTerminate()
    }

    /// An app that keeps running (keepRunning, in the background) doesn't
    /// quit with its last window: closing the main one hides it.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        // AppKit asks as soon as the last visible window is ordered out,
        // hidden too: the app's own hiding of its windows isn't the user
        // closing the last one (Windows.swift's windowsHiddenByApp).
        let s = AsukaShell.shared
        return !s.keepRunning && !s.inBackground && !AsukaRuntime.shared.windowsHiddenByApp
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        AsukaShell.shared.reopen(flag)
    }

    func applicationDidHide(_ notification: Notification) {
        AsukaRuntime.shared.lifecycle(.background)
        AsukaRuntime.shared.windowsChanged()
    }

    func applicationDidUnhide(_ notification: Notification) {
        AsukaRuntime.shared.lifecycle(.foreground)
        AsukaRuntime.shared.windowsChanged()
    }

    func applicationDidResignActive(_ notification: Notification) {
        AsukaRuntime.shared.windowsChanged()
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        AsukaRuntime.shared.windowsChanged()
    }

    func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool { true }

    /// Links of its scheme and files, together (Files.swift).
    func application(_ application: NSApplication, open urls: [URL]) {
        AsukaShell.shared.openURLs(urls)
    }

    /// The Dock menu's items, above the Dock's own (Tray.swift).
    func applicationDockMenu(_ sender: NSApplication) -> NSMenu? {
        AsukaTray.shared.dockMenu()
    }
}

@MainActor extension AsukaRuntime {
    /// The shell's modules: the app's (tray, dock, hotkey, apps, recent,
    /// login), the system's (share, compose, periodic, background, widget,
    /// liveActivity: System.swift and the files beside it) and the
    /// updater's (Updater.swift).
    func registerShellModules() {
        registerAppModules()
        registerSystemModules()
        registerUpdateModule()
    }
}
