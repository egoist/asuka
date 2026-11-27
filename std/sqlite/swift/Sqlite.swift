// std/sqlite on iOS and macOS: where a database's relative name is.
// SQLite itself is std/sqlite's C (its [c]), linked into the app, which
// the core calls.
import Foundation

final class AppPlace: Place {
    func dataDir() throws -> String {
        // asuka dev's runs keep their files apart from the installed app's.
        if let d = ProcessInfo.processInfo.environment["ASUKA_DATA"], !d.isEmpty { return d }
        let fm = FileManager.default
        #if os(tvOS)
        // tvOS lets an app write in Caches alone: the app's directory
        // there (the host's appDir).
        guard var url = fm.urls(for: .cachesDirectory, in: .userDomainMask).first else { return "" }
        url.appendPathComponent("asuka-app", isDirectory: true)
        #else
        guard var url = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else { return "" }
        #endif
        #if os(macOS)
        // A Mac's Application Support is shared by the user's apps.
        url.appendPathComponent(Bundle.main.bundleIdentifier ?? ProcessInfo.processInfo.processName)
        #endif
        try fm.createDirectory(at: url, withIntermediateDirectories: true)
        return url.path
    }
}

func makePlace() throws -> any Place { AppPlace() }
