// The watch app's entry point: one scene, the core's root (AsukaRootView),
// and the app's going to the background and back, which the core hears as
// Lifecycle events; the app's delegate (AsukaWatchDelegate: notifications
// and pushes).

import SwiftUI
import WatchKit
#if canImport(AsukaHost)
import AsukaHost
#endif

@main
struct AsukaWatchApp: App {
    @WKApplicationDelegateAdaptor(AsukaWatchDelegate.self) private var delegate
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            AsukaRootView()
        }
        .onChange(of: phase) { _, p in
            let rt = AsukaRuntime.shared
            switch p {
            case .active: rt.lifecycle(.foreground)
            case .background: rt.lifecycle(.background)
            default: break
            }
        }
    }
}
