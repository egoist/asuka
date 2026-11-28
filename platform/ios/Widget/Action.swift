// platform/widget's interactive widgets: the App Intent a widget's button,
// toggle or action part runs (iOS 17), and what the widgets and the app
// share of them in the App Group's container (AsukaAppGroup). The CLI
// compiles this into the widget extension, and into the app's App Intents
// (with ASUKA_WIDGET_APP), where the system runs it: it launches the app
// in the background, if it isn't running, and the host hands the action
// to the app's code (platform/widget's actions()), waiting until it's done
// before the widget is drawn again. Where it runs in the extension
// instead (the app can't be launched), it's kept in the App Group for the
// app's next run, and a toggle shows its new state until then.
import Foundation
#if canImport(AppIntents)
import AppIntents
#endif

/// What the widgets and the app share, as files in the App Group's
/// container (AsukaWidgets/), which a process reads as another has just
/// written them (the App Group's defaults reach another process a moment
/// later, after a widget has been drawn from them):
/// - "entries-<kind>.json": the entries set without a configuration (the
///   host's "set");
/// - "sets-<kind>.json": those set for one, [{config, entries}];
/// - "actions.json": the actions the app hasn't had, [{token, kind, id,
///   value, isOn, config, atMs}];
/// - "toggles.json": a toggle's state the user set, until the app has had
///   it: {its key: {on, token, kind}};
/// - "entities-<entity>.json": an entity param's things.
/// What's read, changed and written again is under "lock" (flock).
enum AsukaWidgetStore {
    static var group: String? { Bundle.main.object(forInfoDictionaryKey: "AsukaAppGroup") as? String }

    static var dir: URL? {
        guard let g = group, let root = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: g) else { return nil }
        return root.appendingPathComponent("AsukaWidgets", isDirectory: true)
    }

    static func read(_ name: String) -> Any? {
        guard let u = dir?.appendingPathComponent(name), let data = try? Data(contentsOf: u) else { return nil }
        return try? JSONSerialization.jsonObject(with: data)
    }

    static func write(_ name: String, _ value: Any) {
        guard let d = dir, let data = try? JSONSerialization.data(withJSONObject: value) else { return }
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        try? data.write(to: d.appendingPathComponent(name), options: .atomic)
    }

    /// Runs f holding the store's lock, which the app and its extension share.
    static func locked<T>(_ f: () -> T) -> T {
        guard let d = dir else { return f() }
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        let fd = open(d.appendingPathComponent("lock").path, O_CREAT | O_RDWR, 0o644)
        if fd >= 0 { flock(fd, LOCK_EX) }
        defer { if fd >= 0 { flock(fd, LOCK_UN); close(fd) } }
        return f()
    }

    /// The Darwin notification the extension posts for a running app (the
    /// host drains the actions as it hears it).
    static var notification: String { "dev.asuka.widget.actions." + (group ?? "") }

    /// A configuration as one string: its params sorted by name.
    static func configKey(_ c: [String: String]) -> String {
        c.keys.sorted().map { $0 + "\u{1}" + (c[$0] ?? "") }.joined(separator: "\u{2}")
    }

    /// A toggle's key: its widget's kind and configuration, its action and value.
    static func toggleKey(kind: String, config: [String: String], action: String, value: String) -> String {
        [kind, configKey(config), action, value].joined(separator: "\u{3}")
    }

    /// The state the user set a toggle to, until the app has had it.
    static func toggle(_ key: String) -> Bool? {
        ((read("toggles.json") as? [String: Any])?[key] as? [String: Any])?["on"] as? Bool
    }

    /// Keeps an action for the app, and a toggle's state: the token the
    /// app answers it with.
    @discardableResult
    static func record(kind: String, action: String, value: String, isOn: Bool, toggle: Bool, config: String) -> String {
        let token = UUID().uuidString
        let cfg = ((try? JSONSerialization.jsonObject(with: Data(config.utf8))) as? [String: String]) ?? [:]
        locked {
            var queue = read("actions.json") as? [Any] ?? []
            queue.append(["token": token, "kind": kind, "id": action, "value": value, "isOn": isOn, "config": cfg,
                          "atMs": (Date().timeIntervalSince1970 * 1000).rounded()] as [String: Any])
            write("actions.json", queue)
            if toggle {
                var ts = read("toggles.json") as? [String: Any] ?? [:]
                ts[toggleKey(kind: kind, config: cfg, action: action, value: value)] = ["on": isOn, "token": token, "kind": kind]
                write("toggles.json", ts)
            }
        }
        return token
    }

    /// The entries set for the configuration that matches the most of c's
    /// params (all of the set's must), else those set without one.
    static func entries(kind: String, config c: [String: String]) -> Data? {
        var best: Any? = read("entries-\(kind).json")
        var most = 0
        for s in read("sets-\(kind).json") as? [[String: Any]] ?? [] {
            guard let sc = s["config"] as? [String: String], let es = s["entries"],
                  sc.count > most, sc.allSatisfy({ c[$0.key] == $0.value }) else { continue }
            best = es
            most = sc.count
        }
        return best.flatMap { try? JSONSerialization.data(withJSONObject: $0) }
    }

    /// What a toggle shows now: the state the user set it to, until the
    /// app has had it, else the app's (the current entry's toggle of the
    /// action and value).
    static func isOn(kind: String, action: String, value: String, config: String) -> Bool {
        let cfg = ((try? JSONSerialization.jsonObject(with: Data(config.utf8))) as? [String: String]) ?? [:]
        if let on = toggle(toggleKey(kind: kind, config: cfg, action: action, value: value)) { return on }
        guard let data = entries(kind: kind, config: cfg),
              let es = (try? JSONSerialization.jsonObject(with: data)) as? [[String: Any]] else { return false }
        let now = Date().timeIntervalSince1970 * 1000
        guard let e = es.last(where: { ($0["atMs"] as? Double ?? 0) <= now }) ?? es.first else { return false }
        func find(_ n: Any?) -> Bool? {
            guard let n = n as? [String: Any] else { return nil }
            if n["t"] as? String == "toggle", n["action"] as? String == action, n["arg"] as? String ?? "" == value {
                return n["on"] as? Bool ?? false
            }
            for c in n["c"] as? [Any] ?? [] { if let on = find(c) { return on } }
            return nil
        }
        if let on = find(e["layout"]) { return on }
        for (_, n) in e["sizes"] as? [String: Any] ?? [:] { if let on = find(n) { return on } }
        return false
    }

    /// An entity param's things: id, title, subtitle.
    static func entities(_ entity: String) -> [[String: String]] {
        (read("entities-\(entity).json") as? [[String: Any]] ?? []).compactMap { e in
            guard let id = e["id"] as? String else { return nil }
            return ["id": id, "title": e["title"] as? String ?? id, "subtitle": e["subtitle"] as? String ?? ""]
        }
    }
}

#if canImport(AppIntents) && os(iOS)
/// Runs a widget's action: keeps it for the app, and in the app's process
/// hands it to the app's code, returning once the app is done with it
/// (the widget is drawn again then).
@available(iOS 17.0, *)
func asukaRunWidgetAction(kind: String, action: String, value: String, isOn: Bool, toggle: Bool, config: String) async {
    let token = AsukaWidgetStore.record(kind: kind, action: action, value: value, isOn: isOn, toggle: toggle, config: config)
    #if ASUKA_WIDGET_APP
    // The host (AsukaWidgetData) answers.
    await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
        let reply = AsukaWidgetReply { c.resume() }
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: Notification.Name("AsukaWidgetAction"), object: reply, userInfo: ["token": token])
            if !reply.taken { reply.answer() } // no host module: the app's next run has it
        }
        // The system's time for an intent is about 30 seconds.
        DispatchQueue.main.asyncAfter(deadline: .now() + 25) { reply.answer() }
    }
    #else
    // The extension's: a running app hears of it now, another as it runs.
    CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(), CFNotificationName(AsukaWidgetStore.notification as CFString), nil, nil, true)
    #endif
}

/// A widget's button or action part: what the user tapped, in the widget
/// of a kind and configuration (JSON).
@available(iOS 17.0, *)
struct AsukaWidgetAction: AppIntent {
    static let title: LocalizedStringResource = "Widget Action"
    static let isDiscoverable = false

    @Parameter(title: "Kind") var kind: String
    @Parameter(title: "Action") var action: String
    @Parameter(title: "Value") var value: String
    @Parameter(title: "Configuration") var config: String

    init() {}

    init(kind: String, action: String, value: String, config: String) {
        self.kind = kind
        self.action = action
        self.value = value
        self.config = config
    }

    // In the app's process, launched in the background if it isn't
    // running: iOS 27 names it; before, an intent the app may continue in
    // the foreground (the app's ForegroundContinuableIntent) runs there.
    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    func perform() async throws -> some IntentResult {
        await asukaRunWidgetAction(kind: kind, action: action, value: value, isOn: false, toggle: false, config: config)
        return .result()
    }
}

/// A widget's toggle. The system draws it flipped at once; the intent is
/// the same in either state (so the system knows the toggle again in the
/// widget drawn next), and flips what it shows (AsukaWidgetStore.flip).
@available(iOS 17.0, *)
struct AsukaWidgetToggle: AppIntent {
    static let title: LocalizedStringResource = "Widget Toggle"
    static let isDiscoverable = false

    @Parameter(title: "Kind") var kind: String
    @Parameter(title: "Action") var action: String
    @Parameter(title: "Value") var arg: String
    @Parameter(title: "Configuration") var config: String

    init() {}

    init(kind: String, action: String, arg: String, config: String) {
        self.kind = kind
        self.action = action
        self.arg = arg
        self.config = config
    }

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    func perform() async throws -> some IntentResult {
        let on = !AsukaWidgetStore.isOn(kind: kind, action: action, value: arg, config: config)
        await asukaRunWidgetAction(kind: kind, action: action, value: arg, isOn: on, toggle: true, config: config)
        return .result()
    }
}

#if ASUKA_WIDGET_APP
@available(iOS 17.0, *)
@available(iOSApplicationExtension, unavailable)
extension AsukaWidgetAction: ForegroundContinuableIntent {}

@available(iOS 17.0, *)
@available(iOSApplicationExtension, unavailable)
extension AsukaWidgetToggle: ForegroundContinuableIntent {}

/// The host's answer to an action: taken as it's posted, answered once.
final class AsukaWidgetReply: NSObject, @unchecked Sendable {
    private let lock = NSLock()
    private var done: (() -> Void)?
    @objc var taken = false

    init(_ done: @escaping () -> Void) { self.done = done }

    @objc func answer() {
        lock.lock()
        let d = done
        done = nil
        lock.unlock()
        d?()
    }
}
#endif
#endif
