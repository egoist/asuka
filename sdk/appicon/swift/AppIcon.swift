// platform/appicon on iOS and the Mac. iOS: UIApplication's alternate
// icons, which the build lists in Info.plist (CFBundleAlternateIcons,
// actool's --alternate-app-icon); the system keeps the choice. The Mac:
// the Dock's icon while the app runs (NSApp.applicationIconImage), from
// the .icns files the build puts in Resources/AppIcons; the choice is the
// AsukaAppIcon default, which the host sets again as the app launches.
import Foundation
import Asuka
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

final class SystemIcons: Icons {
    @MainActor func names() throws -> [String] { Self.alternates() }

    @MainActor func current() throws -> String {
        #if canImport(UIKit)
        return UIApplication.shared.alternateIconName ?? ""
        #else
        let n = UserDefaults.standard.string(forKey: "AsukaAppIcon") ?? ""
        return Self.alternates().contains(n) ? n : ""
        #endif
    }

    func set(name: String) async throws { try await Self.apply(name) }

    @MainActor static func apply(_ name: String) async throws {
        if !name.isEmpty && !alternates().contains(name) { throw AppIconError.unknown }
        #if canImport(UIKit)
        guard UIApplication.shared.supportsAlternateIcons else { throw AppIconError.notAvailable }
        if (UIApplication.shared.alternateIconName ?? "") == name { return }
        // iOS changes it only for an app in front (it says so in an alert
        // over it), cancelling it otherwise: one set from the background
        // waits for the app to come back.
        while UIApplication.shared.applicationState != .active {
            _ = await NotificationCenter.default.notifications(named: UIApplication.didBecomeActiveNotification).first { _ in true }
        }
        do {
            try await UIApplication.shared.setAlternateIconName(name.isEmpty ? nil : name)
        } catch {
            print("asuka: appicon: the system didn't change the icon: \(error)")
            throw AppIconError.failed
        }
        #else
        if name.isEmpty {
            UserDefaults.standard.removeObject(forKey: "AsukaAppIcon")
            NSApp.applicationIconImage = nil // the bundle's own
            return
        }
        guard let url = Bundle.main.url(forResource: name, withExtension: "icns", subdirectory: "AppIcons"),
              let image = NSImage(contentsOf: url) else { throw AppIconError.failed }
        UserDefaults.standard.set(name, forKey: "AsukaAppIcon")
        NSApp.applicationIconImage = image
        #endif
    }

    /// The alternate icons the build has, by name.
    @MainActor static func alternates() -> [String] {
        #if canImport(UIKit)
        var names = Set<String>()
        for key in ["CFBundleIcons", "CFBundleIcons~ipad"] {
            if let icons = Bundle.main.object(forInfoDictionaryKey: key) as? [String: Any],
               let alt = icons["CFBundleAlternateIcons"] as? [String: Any] {
                names.formUnion(alt.keys)
            }
        }
        return names.sorted()
        #else
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("AppIcons"),
              let files = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return [] }
        return files.filter { $0.hasSuffix(".icns") }.map { String($0.dropLast(5)) }.sorted()
        #endif
    }
}

func makeIcons() throws -> any Icons { SystemIcons() }
