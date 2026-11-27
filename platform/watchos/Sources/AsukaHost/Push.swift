#if os(watchOS) && ASUKA_PUSH // an app that calls platform/push (see the CLI's iosFeatures)
import Foundation
import UserNotifications
import WatchKit

/// platform/push's module, as the iOS host's (platform/ios Push.swift), on
/// the watch's APNs registration (WKApplication's): "register" asks for
/// notifications, registers with APNs and answers JSON (token, service,
/// allowed); "nextReceived" and "nextOpened" answer with the next message
/// (push.Message's JSON), kept until asked; "stop" (the method's name)
/// fails the waiting ones with "stopped". The notification center's
/// delegate (AsukaNotifyCenter, Notify.swift) hands it the pushes that
/// come and the ones tapped; the app's delegate (AsukaWatchDelegate) the
/// token and the data messages.
@MainActor final class AsukaPush {
    static let shared = AsukaPush()
    private var registering: [(Result<Data, Error>) -> Void] = []
    private var allowed = false
    private var queues: [String: AsukaWaitQueue] = ["nextReceived": AsukaWaitQueue(), "nextOpened": AsukaWaitQueue()]

    func methods() -> [String: AsukaModuleMethod] {
        [
            "register": { [unowned self] _, reply in
                UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { granted, _ in
                    DispatchQueue.main.async {
                        self.allowed = granted
                        self.registering.append(reply)
                        WKApplication.shared().registerForRemoteNotifications()
                    }
                }
            },
            "nextReceived": { [unowned self] _, reply in self.queues["nextReceived"]?.next(reply) },
            "nextOpened": { [unowned self] _, reply in self.queues["nextOpened"]?.next(reply) },
            "stop": { [unowned self] args, reply in
                self.queues[String(decoding: args, as: UTF8.self)]?.stop()
                reply(.success(Data()))
            },
        ]
    }

    func deliver(_ kind: String, _ m: Data) {
        queues[kind]?.deliver(m)
    }

    // From the app's delegate.

    func registered(_ token: Data) {
        let hex = token.map { String(format: "%02x", $0) }.joined()
        let d = (try? JSONSerialization.data(withJSONObject: ["token": hex, "service": "apns", "allowed": allowed] as [String: Any])) ?? Data()
        let rs = registering
        registering = []
        for r in rs { r(.success(d)) }
    }

    func failed(_ error: Error) {
        let rs = registering
        registering = []
        for r in rs { r(.failure(error)) }
    }

    /// A data message (content-available), in the foreground or woken for it.
    func received(_ userInfo: [AnyHashable: Any]) {
        deliver("nextReceived", AsukaNotifyContent.pushMessage(userInfo, id: ""))
    }
}
#endif
