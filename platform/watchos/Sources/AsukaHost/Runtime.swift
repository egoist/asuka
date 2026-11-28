// The watch's host (docs/watch.md): the C core drives SwiftUI. watchOS has
// no UIKit views (and WatchKit's interface objects are deprecated), so this
// host, alone among Apple's, is SwiftUI's; the contract is the others'
// (core/include/asuka.h, DESIGN.md's "One flush, two crossings"): the core
// sends a batch of props, measures the leaves through `measure`, then sends
// a batch of frames; the host decodes each batch into its node store
// (Node.swift) and publishes it once, and draws every view at the frame the
// core gave it (Views.swift). Events go back to the core on the main
// thread.

import Foundation
import Observation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif
#if canImport(CAsuka)
@_implementationOnly import CAsuka
#endif
#if os(watchOS)
import WatchKit
#endif

/// What the scene shows besides the root: sheets over it, an alert over
/// those, asuka dev's error. Observed by the root view.
@MainActor @Observable public final class AsukaScene {
    public internal(set) var root: AsukaNode?
    /// The sheets presented, bottom first.
    public internal(set) var sheets: [AsukaNode] = []
    public internal(set) var alert: AsukaNode?
    var devError: AsukaDevError?
    /// The app's accent (the Accent command's): the controls' tint.
    var accent: Color?
}

/// asuka dev's error overlay's content (ops.AppendDevError).
struct AsukaDevError: Equatable {
    var reload: Bool
    var title: String
    var message: String
    var source: String
    var details: String

    init?(_ b: UnsafeRawBufferPointer) {
        var r = AsukaReader(b)
        guard b.count >= 17 else { return nil }
        let flags = r.u8()
        reload = flags & 1 != 0
        title = r.string()
        message = r.string()
        source = r.p + 4 <= r.end ? r.string() : ""
        details = r.p + 4 <= r.end ? r.string() : ""
    }
}

@MainActor public final class AsukaRuntime: AsukaOpHandler {
    public static let shared = AsukaRuntime()
    public let scene = AsukaScene()

    var nodes: [UInt32: AsukaNode] = [:]
    var rootID: UInt32 = 0
    var started = false

    /// Calls into the core in progress: events the host raises meanwhile
    /// (a view appearing as a batch is published) wait for them to return.
    var depth = 0
    var pendingEvents: [[UInt8]] = []

    /// The nodes the batch being applied changed, to publish at its end;
    /// those it changed while an animation was on (AnimateBegin), with it.
    var dirty: [AsukaNode] = []
    var dirtySet = Set<ObjectIdentifier>()
    var animatedDirty = Set<ObjectIdentifier>()
    var animation: Animation?
    var sceneDirty = false

    var nextCellID: UInt32 = 0
    /// The environment last reported for the root.
    var rootEnv: AsukaEnvironment?
    /// The root's size (the whole screen) and its safe area (the time,
    /// the rounded corners), as SwiftUI last gave them.
    var rootSize = CGSize.zero
    var rootSafe = EdgeInsets()
    /// Pixels per point.
    var scale: CGFloat = 2
    /// The modules ui.Call reaches (Modules.swift).
    let modules = AsukaModules()
    /// URLs the app was opened with before its root was (a notification's).
    var pendingURLs: [URL] = []

    // MARK: Starting

    /// Starts the core and the app, once: under asuka dev (ASUKA_DEV in
    /// the environment, which the CLI launches the player with), the
    /// bytecode it sends; otherwise the app compiled into this build.
    public func start() {
        guard !started else { return }
        started = true
        #if os(watchOS)
        scale = WKInterfaceDevice.current().screenScale
        #endif
        if let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first {
            dir.path.withCString { asuka_set_app_dir($0) }
        }
        #if os(watchOS)
        // Before the app's init, in the app's directory: its crashes are kept.
        AsukaCrash.start()
        #endif
        #if ASUKA_NOTIFY || ASUKA_PUSH
        AsukaNotifyCenter.shared.install() // platform/push's too
        #endif
        #if DEBUG
        AsukaAutomation.start()
        #endif
        AsukaNative.done = { call, b in b.withUnsafeBufferPointer { asuka_native_done(call, $0.baseAddress, $0.count) } }
        AsukaNative.invoke = { id, b in
            var out: UnsafeMutablePointer<UInt8>? = nil
            let n = b.withUnsafeBufferPointer { asuka_native_invoke(id, $0.baseAddress, $0.count, &out) }
            guard let out else { return nil }
            defer { asuka_free(out) }
            return Array(UnsafeBufferPointer(start: out, count: n))
        }
        var cb = asuka_host_callbacks(
            apply: { buf, len in
                MainActor.assumeIsolated {
                    AsukaRuntime.shared.apply(UnsafeRawBufferPointer(start: buf, count: len))
                }
            },
            measure: { id, w, h, aw, ah, ow, oh in
                MainActor.assumeIsolated {
                    let rt = AsukaRuntime.shared
                    let s = rt.measure(id, w, h, aw, ah)
                    ow?.pointee = Float(s.width)
                    oh?.pointee = Float(s.height)
                    // Drawn no wider than this, whatever its frame: the
                    // core makes its frame what's drawn.
                    if let widest = rt.widest(id) { asuka_measure_max(Float(widest), .infinity) }
                }
            },
            post: {
                DispatchQueue.main.async {
                    MainActor.assumeIsolated { AsukaRuntime.shared.enter { asuka_run_posted() } }
                }
            },
            native_new: { cls, len, iface, out, cap, big in
                let name = String(decoding: UnsafeBufferPointer(start: cls, count: len), as: UTF8.self)
                return AsukaNative.write(AsukaNative.new(name, iface), out, cap, big)
            },
            native_call: { h, method, p, n, out, cap, big in
                AsukaNative.write(AsukaNative.call(h, method, Array(UnsafeBufferPointer(start: p, count: n))), out, cap, big)
            },
            native_start: { h, method, p, n, call in
                AsukaNative.start(h, method, Array(UnsafeBufferPointer(start: p, count: n)), call)
            },
            native_cancel: { AsukaNative.cancel($0) },
            native_release: { AsukaNative.release($0) },
            native_free: { free($0) },
            measure_v: nil
        )
        enter {
            asuka_start(&cb)
            runApp()
        }
    }

    /// Starts the core without a screen, if it hasn't started: the system
    /// launched the app for a notification's action. The root goes on
    /// screen if the scene comes.
    public func startHeadless() { start() }

    /// The app was opened with url (a notification's): the core's
    /// ui.OnOpenURL handlers get it, once its root is there.
    public func openURL(_ url: URL) {
        guard started, rootID != 0 else { pendingURLs.append(url); return }
        send(AsukaEvents.incomingURL(rootID, url.absoluteString))
    }

    private func runApp() {
        var err: UnsafeMutablePointer<CChar>?
        var rc: Int32
        let env = ProcessInfo.processInfo.environment
        let player = asuka_has_app() == 0
        // A player keeps where asuka dev was, so opened again from the
        // watch's apps it finds it.
        let dev = env["ASUKA_DEV"] ?? (player ? UserDefaults.standard.string(forKey: "AsukaDev") : nil)
        if let dev, let colon = dev.lastIndex(of: ":"), let port = Int32(dev[dev.index(after: colon)...]) {
            if env["ASUKA_DEV"] != nil, player { UserDefaults.standard.set(dev, forKey: "AsukaDev") }
            let hosts = String(dev[..<colon])
            #if targetEnvironment(simulator)
            let kind = "simulator"
            #else
            let kind = "device"
            #endif
            #if os(watchOS)
            let os = WKInterfaceDevice.current().systemVersion
            #else
            let os = ProcessInfo.processInfo.operatingSystemVersionString
            #endif
            // @platform is .iOS on the watch, as on an Apple TV: the app's
            // iOS code runs, and environment().watch tells the watch.
            let hello = "{\"platform\":\"ios\",\"device\":\"\(kind)\",\"os\":\"\(os)\",\"watch\":true}"
            rc = asuka_dev_connect(hosts, port, hello, &err)
        } else {
            rc = asuka_run(&err)
        }
        if rc != 0 {
            let msg = err.map { String(cString: $0) } ?? "asuka: the core failed"
            asuka_free_string(err)
            print(msg)
        }
    }

    // MARK: Calling into the core

    /// Every call into the core goes through here, so that events the
    /// host raises while the core is working wait until it returns.
    func enter(_ f: () -> Void) {
        depth += 1
        f()
        depth -= 1
        while depth == 0 && !pendingEvents.isEmpty {
            send(pendingEvents.removeFirst())
        }
    }

    func send(_ bytes: [UInt8]) {
        guard started else { return }
        if depth > 0 {
            pendingEvents.append(bytes)
            return
        }
        enter { bytes.withUnsafeBufferPointer { asuka_event($0.baseAddress, $0.count) } }
    }

    func emit(_ id: UInt32, _ event: AsukaEventKind, _ make: () -> [UInt8]) {
        guard let n = nodes[id], n.listens(event) else { return }
        send(make())
    }

    public func lifecycle(_ state: AsukaLifecycle) {
        guard started, rootID != 0 else { return }
        send(AsukaEvents.lifecycle(rootID, Int64(state.rawValue)))
    }

    // MARK: The environment

    /// The environment of a view of size and safe area, on the watch.
    func environment(size: CGSize, safe: EdgeInsets) -> AsukaEnvironment {
        var e = AsukaEnvironment()
        e.width = Float(size.width)
        e.height = Float(size.height)
        e.safeTop = Float(safe.top)
        e.safeBottom = Float(safe.bottom)
        e.safeLeft = Float(safe.leading)
        e.safeRight = Float(safe.trailing)
        e.scale = Float(scale)
        e.fontScale = AsukaFonts.scale
        e.dark = true // watchOS is dark
        e.watch = true
        e.platform = 1 // iOS's code, as on an Apple TV
        e.widthClass = 1
        e.heightClass = 1
        #if os(watchOS)
        e.reduceMotion = WKAccessibilityIsReduceMotionEnabled()
        e.screenReader = WKAccessibilityIsVoiceOverRunning()
        #endif
        e.locale = Locale.current.identifier
        e.languages = Locale.preferredLanguages.joined(separator: ",")
        e.firstWeekday = UInt8(Calendar.current.firstWeekday)
        let hours = DateFormatter.dateFormat(fromTemplate: "j", options: 0, locale: .current) ?? ""
        e.hourCycle = hours.contains("H") || hours.contains("k") ? 24 : 12
        return e
    }

    /// Reports the root's size: the whole screen, its safe area inset.
    /// Each is the latest SwiftUI gave (nil: unchanged): the size from a
    /// view over the whole screen, the safe area from one inside it.
    func reportRoot(size: CGSize? = nil, safe: EdgeInsets? = nil) {
        if let size { rootSize = size }
        if let safe { rootSafe = safe }
        guard rootID != 0, rootSize.width > 0 else { return }
        let e = environment(size: rootSize, safe: rootSafe)
        if e == rootEnv { return }
        rootEnv = e
        send(AsukaEvents.environment(rootID, e))
    }

    /// Reports a screen's or a sheet's own environment: the core lays it
    /// out as a root of its own at that size.
    func report(_ n: AsukaNode, size: CGSize, safe: EdgeInsets) {
        guard nodes[n.id] === n, size.width > 0 else { return }
        let e = environment(size: size, safe: safe)
        if e == n.lastEnv { return }
        n.lastEnv = e
        send(AsukaEvents.environment(n.id, e))
    }

    /// The text size changed: fonts are made again and the environment
    /// reported with the new scale, which has the core measure again.
    func textSizeChanged() {
        AsukaFonts.reset()
        if var e = rootEnv {
            e.fontScale = AsukaFonts.scale
            rootEnv = e
            send(AsukaEvents.environment(rootID, e))
        }
        for n in nodes.values {
            guard var e = n.lastEnv else { continue }
            e.fontScale = AsukaFonts.scale
            n.lastEnv = e
            send(AsukaEvents.environment(n.id, e))
        }
    }

    // MARK: Applying ops

    func apply(_ buf: UnsafeRawBufferPointer) {
        asukaDecode(buf, self)
        publish()
    }

    /// Marks n changed in this batch: its view runs its body again once
    /// the batch is applied.
    func touch(_ n: AsukaNode) {
        let k = ObjectIdentifier(n)
        if animation != nil { animatedDirty.insert(k) }
        guard dirtySet.insert(k).inserted else { return }
        dirty.append(n)
    }

    /// The batch's changes, published at once: each node changed bumps its
    /// revision once (with the batch's animation, if it had one).
    func publish() {
        guard !dirty.isEmpty else { return }
        let changed = dirty
        let animated = animatedDirty
        dirty = []
        dirtySet = []
        animatedDirty = []
        var plain: [AsukaNode] = [], anim: [AsukaNode] = []
        for n in changed {
            if animated.contains(ObjectIdentifier(n)) { anim.append(n) } else { plain.append(n) }
        }
        for n in plain { n.rev &+= 1 }
        if !anim.isEmpty {
            withAnimation(lastAnimation ?? .default) {
                for n in anim { n.rev &+= 1 }
            }
        }
    }

    var lastAnimation: Animation?

    public func create(_ id: UInt32, kind raw: UInt16) {
        let kind = AsukaKind(rawValue: raw)
        let n = AsukaNode(id: id, kind: kind)
        if kind == .list { n.list = AsukaWatchList(node: n) }
        if kind == .slider { n.max = 1 }
        if kind == .stepper { n.max = 100 }
        if kind == .progress || kind == .progressRing { n.max = 1 }
        nodes[id] = n
    }

    public func destroy(_ id: UInt32) {
        guard let n = nodes.removeValue(forKey: id) else { return }
        if let p = n.parent {
            p.remove(n)
            touch(p)
        }
        n.list?.destroyed()
        if let l = n.cellOf { l.cellDestroyed(n) }
        if scene.sheets.contains(where: { $0 === n }) {
            scene.sheets.removeAll { $0 === n }
            sceneDirty = true
        }
        if scene.alert === n { scene.alert = nil }
        if scene.root === n { scene.root = nil }
        for c in n.children where c.parent === n { c.parent = nil }
    }

    public func insert(_ parent: UInt32, child: UInt32, index: Int) {
        guard let p = nodes[parent], let c = nodes[child] else { return }
        p.insert(c, at: index)
        touch(p)
        touch(c)
    }

    public func remove(_ parent: UInt32, child: UInt32) {
        guard let p = nodes[parent], let c = nodes[child] else { return }
        p.remove(c)
        touch(p)
    }

    public func setFrame(_ id: UInt32, x: Float, y: Float, width: Float, height: Float) {
        guard let n = nodes[id] else { return }
        let f = CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(width), height: CGFloat(height))
        if f == n.frame && n.placed { return }
        n.frame = f
        n.placed = true
        touch(n)
        // A Scroll's content: its frame is how far the scroll view
        // scrolls, which the Scroll's view sizes (AsukaScrollView).
        if let p = n.parent, p.kind == .scroll { touch(p) }
        // A list's row: its height is the row's.
        if let l = n.cellOf { l.rowResized(n) }
    }

    public func listen(_ id: UInt32, event: UInt16) {
        guard let n = nodes[id] else { return }
        n.listens.insert(event)
        touch(n)
    }

    public func unlisten(_ id: UInt32, event: UInt16) {
        guard let n = nodes[id] else { return }
        n.listens.remove(event)
        touch(n)
    }

    public func setRoot(_ id: UInt32) {
        guard let n = nodes[id] else { return }
        rootID = id
        rootEnv = nil
        scene.root = n
        let urls = pendingURLs
        pendingURLs = []
        for u in urls { openURL(u) }
    }

    public func setProp(_ id: UInt32, prop: UInt16, value v: AsukaValue) {
        guard let n = nodes[id] else { return }
        setProp(n, prop, v)
        touch(n)
    }

    public func command(_ id: UInt32, command: UInt16, args: UnsafeRawBufferPointer) {
        guard let n = nodes[id], let c = AsukaCommand(rawValue: command) else { return }
        switch c {
        case .present:
            present(n)
        case .listUpdate:
            var r = AsukaReader(args)
            guard args.count >= 4 else { return }
            let count = Int(r.u32())
            var ids = [Int64]()
            ids.reserveCapacity(count)
            for _ in 0..<count where r.p + 8 <= r.end { ids.append(Int64(bitPattern: r.u64())) }
            n.list?.update(ids)
            touch(n)
        case .scrollTo:
            guard args.count >= 5 else { return }
            var r = AsukaReader(args)
            let i = Int(r.u32())
            n.list?.scroll(to: i, animated: r.u8() != 0)
            touch(n)
        case .scrollToOffset:
            guard args.count >= 9 else { return }
            var r = AsukaReader(args)
            // A Scroll's or a List's view goes there (AsukaScrollOffsets).
            n.scrollTarget = CGPoint(x: CGFloat(r.f32()), y: CGFloat(r.f32()))
            n.scrollAnimated = r.u8() != 0
            n.scrollRequests &+= 1
            touch(n)
        case .haptic:
            if let k = args.first { haptic(Int32(k)) }
        case .animateBegin:
            beginAnimation(args)
        case .animateEnd:
            animation = nil
        case .openURL:
            #if os(watchOS)
            if let url = URL(string: String(decoding: args, as: UTF8.self)) { WKApplication.shared().openSystemURL(url) }
            #endif
        case .accent:
            guard args.count >= 8 else { return }
            var r = AsukaReader(args)
            scene.accent = asukaColor(r.u64())
        case .call:
            call(id, args)
        case .devError:
            scene.devError = AsukaDevError(args)
        case .devErrorClear:
            scene.devError = nil
        default:
            // Focus, keyboards, menus, windows, sharing, the web's and the
            // desktops' commands: the watch has none of them (docs/watch.md).
            break
        }
    }

    private func beginAnimation(_ args: UnsafeRawBufferPointer) {
        guard args.count >= 5 else { return }
        var r = AsukaReader(args)
        let d = Double(r.f32())
        let curve = AsukaAnimationCurve(rawValue: Int32(r.u8()))
        var a: Animation
        switch curve {
        case .linear: a = .linear(duration: d)
        case .easeIn: a = .easeIn(duration: d)
        case .easeOut: a = .easeOut(duration: d)
        case .spring: a = .spring(duration: d)
        default: a = .easeInOut(duration: d)
        }
        if args.count >= 22 {
            _ = r.u32() // its repeats: not the watch's
            _ = r.u8()
            let delay = Double(r.f32())
            if delay > 0 { a = a.delay(delay) }
        }
        animation = a
        lastAnimation = a
    }

    func haptic(_ kind: Int32) {
        #if os(watchOS)
        let t: WKHapticType
        switch AsukaHaptic(rawValue: kind) {
        case .selection, .light: t = .click
        case .medium, .heavy: t = .directionUp
        case .success: t = .success
        case .warning: t = .retry
        case .error: t = .failure
        default: return
        }
        WKInterfaceDevice.current().play(t)
        #endif
    }

    // MARK: Presenting

    func present(_ n: AsukaNode) {
        switch n.kind {
        case .alert:
            scene.alert = n
        case .sheet, .popover:
            // A popover is a sheet on the watch, as on an iPhone's compact width.
            if !scene.sheets.contains(where: { $0 === n }) { scene.sheets.append(n) }
        default:
            break // a Snackbar, a Window, an Overlay: the watch has none
        }
    }

    /// The user dismissed a sheet (swiped it away, its close button).
    func userDismissed(_ n: AsukaNode) {
        guard scene.sheets.contains(where: { $0 === n }) else { return }
        scene.sheets.removeAll { $0 === n }
        n.dying = true
        emit(n.id, .dismissed) { AsukaEvents.dismissed(n.id) }
    }

    /// The user answered an alert: one of its buttons, or none.
    func alertAnswered(_ n: AsukaNode, _ index: Int64?) {
        if scene.alert === n { scene.alert = nil }
        if let i = index {
            emit(n.id, .action) { AsukaEvents.action(n.id, i) }
        } else {
            emit(n.id, .dismissed) { AsukaEvents.dismissed(n.id) }
        }
    }

    /// The user popped screens off a stack (its back button, a swipe).
    func userPopped(_ stack: AsukaNode, to count: Int) {
        let shown = stack.shownChildren
        guard count < shown.count else { return }
        for c in shown[count...].reversed() {
            c.dying = true
            emit(stack.id, .popped) { AsukaEvents.popped(stack.id, Int64(c.id)) }
        }
    }

    // MARK: List cells

    func makeCell(_ list: AsukaWatchList) -> AsukaNode {
        nextCellID += 1
        let n = AsukaNode(id: 0x8000_0000 | nextCellID, kind: nil)
        n.cellOf = list
        nodes[n.id] = n
        return n
    }
}
