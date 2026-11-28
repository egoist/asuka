#if canImport(UIKit) && ASUKA_WIDGET // an app that calls widget.Set (see the CLI's iosFeatures)
import UIKit
import WidgetKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// platform/widget's module, with the widget extension's data in files in
/// the App Group's container (AsukaAppGroup; the extension's
/// AsukaWidgetStore, Widget/Action.swift, says what each holds):
/// - "set" (JSON: kind, entries, config, all) keeps the kind's entries,
///   for a configuration (config: its params) or for any, and reloads its
///   widgets; with all and no entries, every configuration's go.
/// - "image" (a name, a 0 byte and the image's data; the name alone:
///   deleted) keeps an image for the widgets' layouts in the App Group's
///   container, no larger than a widget draws one (a widget's extension has
///   little memory).
/// - "nextAction" (JSON: sdk/widget's WireAction), "actionDone" (the
///   token), "stop": what the user did with a widget's buttons and
///   toggles, which the app's App Intent (AsukaWidgetAction, run in the app
///   by the system) or the extension kept in the App Group.
/// - "configurations" (the kind; JSON answer: the params of each of its
///   widgets), through the app's App Intents (AsukaWidgetBridge).
/// - "entities" (string entity, JSON): an entity param's things.
@MainActor final class AsukaWidgetData {
    static let shared = AsukaWidgetData()
    /// The actions drained from the App Group, until the app's code reads
    /// them; the readers waiting; the tokens of those it has read.
    private var items: [(token: String, kind: String, data: Data)] = []
    private var waiting: [(Result<Data, Error>) -> Void] = []
    private var given: [String: String] = [:] // token: kind
    /// The actions the app's intent waits on, by token: its kind, and the
    /// answer that lets it return.
    private var pending: [String: (kind: String, answer: () -> Void)] = [:]
    static var group: String? { Bundle.main.object(forInfoDictionaryKey: "AsukaAppGroup") as? String }

    /// The store's files (the extension's AsukaWidgetStore reads them).
    static var dir: URL? {
        guard let g = group, let root = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: g) else { return nil }
        return root.appendingPathComponent("AsukaWidgets", isDirectory: true)
    }

    static func read(_ name: String) -> Any? {
        guard let u = dir?.appendingPathComponent(name), let data = try? Data(contentsOf: u) else { return nil }
        return try? JSONSerialization.jsonObject(with: data)
    }

    static func write(_ name: String, _ value: Any?) {
        guard let d = dir else { return }
        guard let value, let data = try? JSONSerialization.data(withJSONObject: value) else {
            try? FileManager.default.removeItem(at: d.appendingPathComponent(name))
            return
        }
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        try? data.write(to: d.appendingPathComponent(name), options: .atomic)
    }

    /// Runs f holding the lock the extension shares (flock).
    static func locked<T>(_ f: () -> T) -> T {
        guard let d = dir else { return f() }
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        let fd = open(d.appendingPathComponent("lock").path, O_CREAT | O_RDWR, 0o644)
        if fd >= 0 { flock(fd, LOCK_EX) }
        defer { if fd >= 0 { flock(fd, LOCK_UN); close(fd) } }
        return f()
    }

    static func methods() -> [String: AsukaModuleMethod] {
        let s = shared
        return [
            "set": { args, reply in
                guard let o = (try? JSONSerialization.jsonObject(with: args)) as? [String: Any],
                      let kind = o["kind"] as? String, let entries = o["entries"] as? [Any] else {
                    reply(.failure(AsukaModuleError("bad args"))); return
                }
                guard dir != nil else {
                    reply(.failure(AsukaModuleError("no App Group: declare the widgets in asuka.toml"))); return
                }
                let config = o["config"] as? [String: String] ?? [:]
                locked {
                    if o["all"] as? Bool == true {
                        write("entries-\(kind).json", nil)
                        write("sets-\(kind).json", nil)
                    } else if config.isEmpty {
                        write("entries-\(kind).json", entries)
                    } else {
                        var sets = (read("sets-\(kind).json") as? [[String: Any]] ?? [])
                            .filter { ($0["config"] as? [String: String]) != config }
                        if !entries.isEmpty { sets.append(["config": config, "entries": entries]) }
                        write("sets-\(kind).json", sets)
                    }
                }
                WidgetCenter.shared.reloadTimelines(ofKind: kind)
                s.settled(kind)
                reply(.success(Data()))
            },
            "image": { args, reply in
                let zero = args.firstIndex(of: 0) ?? args.endIndex
                let name = String(decoding: args[args.startIndex..<zero], as: UTF8.self)
                guard let group = group,
                      let root = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group) else {
                    reply(.failure(AsukaModuleError("no App Group: declare the widgets in asuka.toml"))); return
                }
                let dir = root.appendingPathComponent("AsukaWidgetImages")
                let file = dir.appendingPathComponent(imageFile(name))
                if zero == args.endIndex {
                    try? FileManager.default.removeItem(at: file)
                } else {
                    let data = Data(args[args.index(after: zero)...])
                    guard let img = UIImage(data: data) else { reply(.failure(AsukaModuleError("widget: not an image"))); return }
                    do {
                        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                        try (scaled(img, data) ?? data).write(to: file, options: .atomic)
                    } catch {
                        reply(.failure(error)); return
                    }
                }
                WidgetCenter.shared.reloadAllTimelines()
                reply(.success(Data()))
            },
            "nextAction": { _, reply in
                s.drain()
                if s.items.isEmpty { s.waiting.append(reply) } else { s.give(reply) }
            },
            "actionDone": { args, reply in
                s.done(String(decoding: args, as: UTF8.self))
                reply(.success(Data()))
            },
            "stop": { args, reply in
                if String(decoding: args, as: UTF8.self) == "nextAction" {
                    let ws = s.waiting
                    s.waiting = []
                    for w in ws { w(.failure(AsukaModuleError("stopped"))) }
                }
                reply(.success(Data()))
            },
            "configurations": { args, reply in
                let kind = String(decoding: args, as: UTF8.self)
                // The app's App Intents' (the CLI's AsukaWidgetBridge): an app
                // with widgets has them.
                guard let bridge = NSClassFromString("AsukaWidgetBridge") as? NSObject.Type else {
                    reply(.success(Data("[]".utf8))); return
                }
                let done: @convention(block) (NSArray) -> Void = { cs in
                    reply(.success((try? JSONSerialization.data(withJSONObject: cs)) ?? Data("[]".utf8)))
                }
                _ = bridge.perform(NSSelectorFromString("configurations:done:"), with: kind as NSString, with: done)
            },
            "entities": { args, reply in
                let (entity, at) = args.withUnsafeBytes { b -> (String, Int) in
                    var r = AsukaReader(b)
                    let e = r.string()
                    return (e, 4 + e.utf8.count)
                }
                guard dir != nil, at <= args.count else {
                    reply(.failure(AsukaModuleError("no App Group: declare the widgets in asuka.toml"))); return
                }
                write("entities-\(entity).json", (try? JSONSerialization.jsonObject(with: Data(args.dropFirst(at)))) ?? [])
                reply(.success(Data()))
            },
        ]
    }

    /// Listens for the actions: the app's intent's (in this process, as
    /// the system runs it), and the extension's (a Darwin notification).
    /// In the app delegate's init, before the system hands the app an
    /// intent it launched it for.
    static func install() {
        NotificationCenter.default.addObserver(forName: Notification.Name("AsukaWidgetAction"), object: nil, queue: nil) { n in
            guard let reply = n.object as? NSObject, let token = n.userInfo?["token"] as? String else { return }
            reply.setValue(true, forKey: "taken")
            MainActor.assumeIsolated { shared.ran(token, reply) }
        }
        if let group = group {
            let name = "dev.asuka.widget.actions." + group
            CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), nil, { _, _, _, _, _ in
                DispatchQueue.main.async { MainActor.assumeIsolated { AsukaWidgetData.shared.drainIfStarted() } }
            }, name as CFString, nil, .deliverImmediately)
        }
    }

    /// The app's intent ran: the app's code starts (without a screen if
    /// the system launched the app for it) and gets the action; the intent
    /// returns once the app is done with it.
    private func ran(_ token: String, _ reply: NSObject) {
        let kind = (Self.read("actions.json") as? [[String: Any]] ?? [])
            .first { $0["token"] as? String == token }?["kind"] as? String ?? ""
        pending[token] = (kind, { _ = reply.perform(NSSelectorFromString("answer")) })
        AsukaRuntime.shared.startHeadless()
        drain()
        // The system's time for it (the intent gives up then too).
        DispatchQueue.main.asyncAfter(deadline: .now() + 25) { [weak self] in
            MainActor.assumeIsolated { self?.done(token) }
        }
    }

    /// The actions kept in the App Group go to the app's code.
    func drain() {
        let queue = Self.locked { () -> [[String: Any]] in
            let q = Self.read("actions.json") as? [[String: Any]] ?? []
            if !q.isEmpty { Self.write("actions.json", nil) }
            return q
        }
        for a in queue {
            guard let data = try? JSONSerialization.data(withJSONObject: a) else { continue }
            items.append((a["token"] as? String ?? "", a["kind"] as? String ?? "", data))
            if !waiting.isEmpty { give(waiting.removeFirst()) }
        }
    }

    /// The first action to a reader: the app's code has it.
    private func give(_ reply: (Result<Data, Error>) -> Void) {
        let i = items.removeFirst()
        given[i.token] = i.kind
        reply(.success(i.data))
    }

    /// The extension kept one, for a running app: it has it now.
    func drainIfStarted() {
        if AsukaRuntime.shared.started { drain() }
    }

    /// The app is done with the action: its intent returns, and a
    /// toggle's state is the app's again.
    private func done(_ token: String) {
        pending.removeValue(forKey: token)?.answer()
        given[token] = nil
        dropToggles { $0["token"] as? String == token }
    }

    /// The app set the kind's entries: the actions of it the app's code
    /// has read are done, their toggles show what it set.
    private func settled(_ kind: String) {
        let had = Set(given.filter { $0.value == kind }.keys)
        for t in had {
            given[t] = nil
            pending.removeValue(forKey: t)?.answer()
        }
        dropToggles { had.contains($0["token"] as? String ?? "") }
    }

    private func dropToggles(_ drop: ([String: Any]) -> Bool) {
        Self.locked {
            guard var ts = Self.read("toggles.json") as? [String: Any] else { return }
            let before = ts.count
            for (k, v) in ts where drop(v as? [String: Any] ?? [:]) { ts[k] = nil }
            if ts.count != before { Self.write("toggles.json", ts) }
        }
    }

    /// The file an image's name is kept in (the extension's AsukaLayout.imageFile).
    static func imageFile(_ name: String) -> String {
        String(name.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "_" }) + ".img"
    }

    /// img no larger than a large widget draws (the screen's width, in
    /// pixels), as PNG; nil: it's small enough as it is.
    static func scaled(_ img: UIImage, _ data: Data) -> Data? {
        let px = CGSize(width: img.size.width * img.scale, height: img.size.height * img.scale)
        let most = UIScreen.main.nativeBounds.width
        let k = most / max(px.width, px.height)
        if k >= 1 { return nil }
        let f = UIGraphicsImageRendererFormat()
        f.scale = 1
        let size = CGSize(width: (px.width * k).rounded(), height: (px.height * k).rounded())
        return UIGraphicsImageRenderer(size: size, format: f).pngData { _ in img.draw(in: CGRect(origin: .zero, size: size)) }
    }
}
#endif
