// platform/document on iOS and macOS: the files, read and written through
// NSFileCoordinator (iCloud's and other apps' writes are coordinated),
// off the main thread.
import Foundation

final class CoordinatedFiles: Files {
    func read(path: String) async throws -> Data {
        try await Self.coordinated { c, url in
            var out = Data()
            var failure: Error?
            var err: NSError?
            c.coordinate(readingItemAt: url, options: [], error: &err) { u in
                do { out = try Data(contentsOf: u) } catch { failure = error }
            }
            if let e = err ?? failure { throw e }
            return out
        }(path)
    }

    func write(path: String, data: Data) async throws {
        _ = try await Self.coordinated { c, url -> Bool in
            var failure: Error?
            var err: NSError?
            c.coordinate(writingItemAt: url, options: .forReplacing, error: &err) { u in
                do { try data.write(to: u, options: .atomic) } catch { failure = error }
            }
            if let e = err ?? failure { throw e }
            return true
        }(path)
    }

    func modified(path: String) async throws -> Int {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: path),
              let date = attrs[.modificationDate] as? Date else { return 0 }
        return Int(date.timeIntervalSince1970 * 1000)
    }

    /// A Mac's Locked (the Finder's): the file's immutable flag, which
    /// nothing changes or deletes while it's set.
    func locked(path: String) throws -> Bool {
        let attrs = try? FileManager.default.attributesOfItem(atPath: path)
        return (attrs?[.immutable] as? Bool) ?? false
    }

    func setLocked(path: String, on: Bool) throws {
        try FileManager.default.setAttributes([.immutable: on], ofItemAtPath: path)
    }

    /// Runs f with a coordinator on a thread of its own.
    private static func coordinated<R>(_ f: @escaping (NSFileCoordinator, URL) throws -> R) -> (String) async throws -> R {
        { path in
            try await withCheckedThrowingContinuation { k in
                DispatchQueue.global(qos: .userInitiated).async {
                    k.resume(with: Result { try f(NSFileCoordinator(filePresenter: nil), URL(fileURLWithPath: path)) })
                }
            }
        }
    }
}

func makeFiles() throws -> any Files { CoordinatedFiles() }
