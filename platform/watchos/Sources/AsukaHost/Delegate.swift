// The watch app's delegate (WKApplicationDelegate, which App.swift's
// @WKApplicationDelegateAdaptor makes): what the system tells the app
// rather than its scene, as the iOS host's AppDelegate has it. The
// notification center's delegate is set as the app launches, so the
// response that launched it is seen; APNs' token and data messages go to
// platform/push's module; background URLSessions' events to the package
// whose session it is.

#if os(watchOS)
import Foundation
import WatchKit

public final class AsukaWatchDelegate: NSObject, WKApplicationDelegate {
    public func applicationDidFinishLaunching() {
        #if ASUKA_NOTIFY || ASUKA_PUSH
        MainActor.assumeIsolated { AsukaNotifyCenter.shared.install() }
        #endif
    }

    /// The system's background tasks: a package's background URLSession's
    /// events (std/http's uploads) go to it, the session's identifier
    /// naming it (BackgroundSessions.swift); the others are done at once,
    /// as WatchKit does them for a delegate without this.
    public func handle(_ backgroundTasks: Set<WKRefreshBackgroundTask>) {
        for task in backgroundTasks {
            if let t = task as? WKURLSessionRefreshBackgroundTask {
                AsukaBackgroundSessions.handleEvents(t.sessionIdentifier) { t.setTaskCompletedWithSnapshot(false) }
            } else if let t = task as? WKSnapshotRefreshBackgroundTask {
                t.setTaskCompleted(restoredDefaultState: true, estimatedSnapshotExpiration: .distantFuture, userInfo: nil)
            } else {
                task.setTaskCompletedWithSnapshot(false)
            }
        }
    }

    #if ASUKA_PUSH
    public func didRegisterForRemoteNotifications(withDeviceToken token: Data) {
        MainActor.assumeIsolated { AsukaPush.shared.registered(token) }
    }

    public func didFailToRegisterForRemoteNotificationsWithError(_ error: Error) {
        MainActor.assumeIsolated { AsukaPush.shared.failed(error) }
    }

    public func didReceiveRemoteNotification(_ userInfo: [AnyHashable: Any], fetchCompletionHandler done: @escaping (WKBackgroundFetchResult) -> Void) {
        // A notification with an alert comes through the center's delegate.
        MainActor.assumeIsolated {
            if (userInfo["aps"] as? [String: Any])?["alert"] == nil { AsukaPush.shared.received(userInfo) }
        }
        done(.newData)
    }
    #endif
}
#endif
