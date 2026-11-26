// platform/analytics on iOS and the Mac: Google Analytics for Firebase,
// configured from the GoogleService-Info.plist the CLI puts in the app's
// bundle (asuka.toml's ios.googleServices). iOS configures it as the app
// launches (AnalyticsLaunch, the manifest's ios.launch), the Mac as the
// package makes its Collector, as the app starts. Without the file there
// is no Collector: the package sends nothing.
//
// What Asuka checked crosses as it is: the event's parameters (their
// kinds) become the NSNumbers and Strings Firebase takes, its items the
// items parameter's dictionaries.
import Asuka
import FirebaseAnalytics
import FirebaseCore
import Foundation
#if canImport(UIKit)
import UIKit
#endif

enum AnalyticsSetup {
    /// Configures Firebase from the bundle's GoogleService-Info.plist,
    /// once (another package may have): false when the app has none.
    static func configure() -> Bool {
        if FirebaseApp.app() != nil { return true }
        guard Bundle.main.path(forResource: "GoogleService-Info", ofType: "plist") != nil else { return false }
        FirebaseApp.configure()
        return FirebaseApp.app() != nil
    }
}

final class FirebaseCollector: Collector {
    func log(name: String, params: [WireValue], items: [WireItem]) throws {
        var p = Self.dictionary(params)
        if !items.isEmpty {
            p[AnalyticsParameterItems] = items.map { Self.dictionary($0.params) }
        }
        Analytics.logEvent(name, parameters: p.isEmpty ? nil : p)
    }

    func setUserId(id: String?) throws { Analytics.setUserID(id) }

    func setUserProperty(name: String, value: String?) throws { Analytics.setUserProperty(value, forName: name) }

    func setDefaults(params: [WireValue]) throws {
        // Firebase adds to its defaults: these replace them.
        Analytics.setDefaultEventParameters(nil)
        if !params.isEmpty { Analytics.setDefaultEventParameters(Self.dictionary(params)) }
    }

    func setEnabled(on: Bool) throws { Analytics.setAnalyticsCollectionEnabled(on) }

    func setConsent(consent: Consent) throws {
        var s: [ConsentType: ConsentStatus] = [:]
        let pairs: [(ConsentType, Bool?)] = [
            (.analyticsStorage, consent.analyticsStorage), (.adStorage, consent.adStorage),
            (.adUserData, consent.adUserData), (.adPersonalization, consent.adPersonalization),
        ]
        for (type, granted) in pairs {
            if let g = granted { s[type] = g ? .granted : .denied }
        }
        if !s.isEmpty { Analytics.setConsent(s) }
    }

    func appInstanceId() async throws -> String? { Analytics.appInstanceID() }

    func reset() throws { Analytics.resetAnalyticsData() }

    static func dictionary(_ params: [WireValue]) -> [String: Any] {
        var d: [String: Any] = [:]
        for v in params {
            switch v.kind {
            case .integer: d[v.name] = NSNumber(value: Int64(v.integer))
            case .number: d[v.name] = NSNumber(value: v.number)
            default: d[v.name] = v.text
            }
        }
        return d
    }
}

func makeCollector() throws -> any Collector {
    guard AnalyticsSetup.configure() else {
        throw AsukaNativeError.unsupported("analytics: no GoogleService-Info.plist in the app (asuka.toml's ios.googleServices)")
    }
    return FirebaseCollector()
}

#if os(iOS)
/// Firebase from the app's launch (the manifest's ios.launch), as its
/// setup asks: what it measures of the app's start (first_open,
/// session_start) and its delegate's calls are counted from there.
@objc(AsukaAnalyticsLaunch) final class AnalyticsLaunch: NSObject, AsukaLaunching {
    @MainActor static func launch(_ options: [UIApplication.LaunchOptionsKey: Any]) -> Bool {
        _ = AnalyticsSetup.configure()
        return false
    }
}
#endif
