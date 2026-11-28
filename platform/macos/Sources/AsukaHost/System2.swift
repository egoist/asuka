// More of the system's modules on a Mac, those the app's std packages
// call: platform/background (an activity that keeps App Nap off),
// platform/widget and platform/liveactivity (none on a Mac). Others (sensors,
// splash, screen, tracking, integrity, ageRange, nfc) aren't here: their
// std packages have their own Swift, or none calls them.
import AppKit

/// Answers at once, with nothing.
private let answerNil: AsukaModuleMethod = { _, reply in reply(.success(Data())) }

private func notAvailable(_ why: String = "") -> AsukaModuleMethod {
    { _, reply in reply(.failure(AsukaModuleError(why.isEmpty ? "not available" : "not available: " + why))) }
}

@MainActor extension AsukaRuntime {
    func registerSystem2Modules() {
        registerModule("background", AsukaBackgroundTasks.shared.methods())
        // platform/widget's: a Mac app from asuka build has no widget
        // extension (WidgetKit's widgets are an extension the iOS build
        // makes), so there are none to give entries to.
        registerModule("widget", ["set": notAvailable("widgets are iOS's and Android's"), "entities": notAvailable("widgets are iOS's and Android's")])
        // platform/liveactivity's: Live Activities are an iPhone's (a Mac
        // shows an iPhone's in its menu bar, through iPhone Mirroring, but
        // no app of its own starts one): none, and nothing to observe.
        registerModule("liveActivity", [
            "enabled": { _, reply in reply(.success(Data([0]))) },
            "start": notAvailable("live activities are iOS's and Android's"),
            "update": answerNil, "end": answerNil,
            "running": { _, reply in reply(.success(Data("[]".utf8))) },
            "next": notAvailable("live activities are iOS's and Android's"),
            "stop": answerNil,
        ])
    }
}

/// platform/background's tasks: a Mac app keeps running out of sight, so a
/// task holds an activity (NSProcessInfo's) that keeps App Nap from slowing
/// it and the system from ending it to free memory, until the last one
/// ends. There's no notification to show (iOS shows none either): "update"
/// does nothing, and "next" (actions tapped) waits until the last task
/// ends, then fails "ended".
@MainActor final class AsukaBackgroundTasks {
    static let shared = AsukaBackgroundTasks()
    private var ids: Set<UInt32> = []
    private(set) var activity: NSObjectProtocol?
    private var next: [AsukaReply] = []

    private static func id(_ args: Data) -> UInt32 {
        ((try? JSONSerialization.jsonObject(with: args)) as? [String: Any])?["id"].flatMap { ($0 as? NSNumber)?.uint32Value } ?? 0
    }

    func methods() -> [String: AsukaModuleMethod] {
        [
            "start": { [unowned self] args, reply in
                self.ids.insert(Self.id(args))
                if self.activity == nil {
                    var opts: ProcessInfo.ActivityOptions = [.userInitiatedAllowingIdleSystemSleep, .suddenTerminationDisabled, .automaticTerminationDisabled]
                    opts.remove(.idleSystemSleepDisabled)
                    self.activity = ProcessInfo.processInfo.beginActivity(options: opts, reason: "platform/background: a task")
                }
                reply(.success(Data()))
            },
            "update": answerNil,
            "end": { [unowned self] args, reply in
                self.end(Self.id(args))
                reply(.success(Data()))
            },
            "next": { [unowned self] _, reply in
                if self.ids.isEmpty { return reply(.failure(AsukaModuleError("ended"))) }
                self.next.append(reply)
            },
        ]
    }

    private func end(_ id: UInt32) {
        guard ids.remove(id) != nil, ids.isEmpty else { return }
        if let a = activity {
            ProcessInfo.processInfo.endActivity(a)
            activity = nil
        }
        let ws = next
        next = []
        for w in ws { w(.failure(AsukaModuleError("ended"))) }
    }
}
