// Background URLSessions' events: the system wakes the app (or starts it
// again, in the background) when a package's background session has
// events, and hands the app's delegate the session's identifier and a
// handler to call once they're all in (iOS's
// application(_:handleEventsForBackgroundURLSession:completionHandler:),
// watchOS's WKURLSessionRefreshBackgroundTask). The host doesn't know
// whose session it is: its identifier's last dot-separated name is the
// class (by its Objective-C name) of the package that made it, which
// reconnects the session and calls the handler from its delegate's
// urlSessionDidFinishEvents(forBackgroundURLSession:). The Mac starts no
// app for a session: the next launch reconnects it. The same file in
// every Apple host.
import Foundation

/// A package's background URLSessions, named by their identifiers' last
/// part (`<bundle id>.AsukaHttpBackground`: `@objc(AsukaHttpBackground)`).
@objc public protocol AsukaBackgroundSession: NSObjectProtocol {
    /// Runs on the main thread: reconnects the session of identifier (the
    /// same configuration, its delegate), and calls done on the main
    /// thread once its delegate has had every event.
    static func handleEvents(_ identifier: String, _ done: @escaping () -> Void)
}

public enum AsukaBackgroundSessions {
    /// Hands identifier's events to the package whose session it is; done
    /// at once when no package claims it.
    public static func handleEvents(_ identifier: String, _ done: @escaping () -> Void) {
        let name = identifier.split(separator: ".").last.map(String.init) ?? ""
        guard let c = NSClassFromString(name) as? AsukaBackgroundSession.Type else {
            NSLog("asuka: no package's background session %@", identifier)
            done()
            return
        }
        c.handleEvents(identifier, done)
    }
}
