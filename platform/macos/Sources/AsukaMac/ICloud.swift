// Kept identical in platform/ios (the iOS host) and platform/macos (the
// Mac's Swift library, which host/darwin calls through purego).
#if ASUKA_ICLOUD // an app that calls asuka/sdk/icloud (see the CLI's iosFeatures; on the Mac, its featurePermissions)
import CryptoKit
import Foundation
#if os(macOS)
import Security
#endif

/// asuka/sdk/icloud's module: internal/icloudspec's methods and JSON. The
/// account (the ubiquity identity token, hashed), the key-value store
/// (NSUbiquitousKeyValueStore, its values converted between JSON and
/// property lists), and the app's ubiquity containers: NSMetadataQuery's
/// lists, reads, writes, deletions and moves through NSFileCoordinator,
/// downloads, evictions, NSFileVersion's conflicts and public links. The
/// file system's work runs off the main thread; every answer and event
/// comes on it.
final class AsukaICloud {
    static let shared = AsukaICloud()

    /// The methods, by name (each answers through call).
    static let methods = ["account", "kvGet", "kvSet", "kvDelete", "kvAll", "kvSync", "container", "files", "watch", "unwatch",
                          "download", "evict", "read", "write", "delete", "move", "moveIn", "moveOut", "versions", "resolve", "link",
                          "next", "stop"]

    struct Failure: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    typealias Reply = (Result<Data, Error>) -> Void

    private let io = DispatchQueue(label: "dev.asuka.icloud", qos: .userInitiated, attributes: .concurrent)
    private var started = false
    private var kvStarted = false
    private var listening = false
    private var events: [Data] = []
    private var waiting: Reply?
    /// The scopes' running queries (watch), and the one-off ones (files)
    /// until they've gathered.
    private var queries: [String: NSMetadataQuery] = [:]
    private var gathering: [NSMetadataQuery: (observer: NSObjectProtocol, reply: Reply)] = [:]
    private var observers: [String: [NSObjectProtocol]] = [:]

    /// Listens for the account's and the values' changes.
    private func start() {
        guard !started else { return }
        started = true
        let nc = NotificationCenter.default
        nc.addObserver(forName: .NSUbiquityIdentityDidChange, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.send(["type": "account", "account": self.account()])
            // The lists are the new account's.
            for (scope, q) in self.queries { self.sendFiles(scope, q) }
        }
        nc.addObserver(forName: NSUbiquitousKeyValueStore.didChangeExternallyNotification, object: nil, queue: .main) { [weak self] n in
            let reasons = [NSUbiquitousKeyValueStoreServerChange: "server", NSUbiquitousKeyValueStoreInitialSyncChange: "initialSync",
                           NSUbiquitousKeyValueStoreQuotaViolationChange: "quota", NSUbiquitousKeyValueStoreAccountChange: "account"]
            let reason = (n.userInfo?[NSUbiquitousKeyValueStoreChangeReasonKey] as? Int).flatMap { reasons[$0] } ?? "server"
            let keys = n.userInfo?[NSUbiquitousKeyValueStoreChangedKeysKey] as? [String] ?? []
            self?.send(["type": "kv", "reason": reason, "keys": keys])
        }
    }

    /// The key-value store: iCloud's (a test's process, unsigned, has no
    /// entitlement for it, and gives its own: a UserDefaults suite, which
    /// keeps property lists as iCloud's does).
    var kvStore: AsukaKVStore?

    /// The store, synchronized once (Apple's advice: as the app starts
    /// using it, so iCloud's changes come).
    private var store: AsukaKVStore {
        let s = kvStore ?? NSUbiquitousKeyValueStore.default
        if !kvStarted {
            kvStarted = true
            _ = s.synchronize()
        }
        return s
    }

    // MARK: Calls

    /// Runs a method; reply answers once, on the main thread.
    func call(_ method: String, _ args: Data, _ reply: @escaping Reply) {
        start()
        if let why = missingEntitlement(method) {
            return reply(.failure(Failure("not available: " + why)))
        }
        let text = String(decoding: args, as: UTF8.self)
        func json() -> [String: Any] {
            (try? JSONSerialization.jsonObject(with: args.isEmpty ? Data("{}".utf8) : args)) as? [String: Any] ?? [:]
        }
        switch method {
        case "account":
            answer(reply, account())
        case "kvGet":
            guard let v = store.object(forKey: text) else { return reply(.success(Data())) }
            reply(Result { try JSONSerialization.data(withJSONObject: Self.jsonValue(v), options: [.fragmentsAllowed]) })
        case "kvSet":
            let a = json()
            guard let key = a["key"] as? String else { return reply(.failure(Failure("no key"))) }
            do {
                if let v = try Self.plistValue(a["value"] ?? NSNull()) {
                    store.set(v, forKey: key)
                } else {
                    store.removeObject(forKey: key)
                }
                reply(.success(Data()))
            } catch {
                reply(.failure(error))
            }
        case "kvDelete":
            store.removeObject(forKey: text)
            reply(.success(Data()))
        case "kvAll":
            answer(reply, store.values.mapValues(Self.jsonValue))
        case "kvSync":
            _ = store.synchronize()
            reply(.success(Data()))
        case "container":
            let id = text
            io.async {
                let url = FileManager.default.url(forUbiquityContainerIdentifier: id.isEmpty ? nil : id)
                DispatchQueue.main.async {
                    guard let url else {
                        return reply(.failure(Failure(FileManager.default.ubiquityIdentityToken == nil
                            ? "no account: not signed in to iCloud, or iCloud is off for the app"
                            : "not a container: \(id.isEmpty ? "the app's first" : id) isn't one of the app's (its entitlements)")))
                    }
                    reply(.success(Data(url.resolvingSymlinksInPath().path.utf8)))
                }
            }
        case "files":
            guard signedIn(reply) else { return }
            let scope = json()["scope"] as? String ?? "documents"
            let q = query(scope)
            let o = NotificationCenter.default.addObserver(forName: .NSMetadataQueryDidFinishGathering, object: q, queue: .main) { [weak self] _ in
                guard let self, let g = self.gathering.removeValue(forKey: q) else { return }
                NotificationCenter.default.removeObserver(g.observer)
                q.disableUpdates()
                let files = self.files(q)
                q.stop()
                self.answer(g.reply, files)
            }
            gathering[q] = (o, reply)
            if !q.start() {
                gathering[q] = nil
                NotificationCenter.default.removeObserver(o)
                reply(.failure(Failure("the query didn't start")))
            }
        case "watch":
            let scope = json()["scope"] as? String ?? "documents"
            listening = true // its lists wait for the next "next"
            reply(.success(Data()))
            if let q = queries[scope] {
                sendFiles(scope, q)
                return
            }
            let q = query(scope)
            queries[scope] = q
            let nc = NotificationCenter.default
            observers[scope] = [NSNotification.Name.NSMetadataQueryDidFinishGathering, .NSMetadataQueryDidUpdate].map { name in
                nc.addObserver(forName: name, object: q, queue: .main) { [weak self] _ in self?.sendFiles(scope, q) }
            }
            q.start()
        case "unwatch":
            let scope = json()["scope"] as? String ?? "documents"
            queries.removeValue(forKey: scope)?.stop()
            for o in observers.removeValue(forKey: scope) ?? [] { NotificationCenter.default.removeObserver(o) }
            reply(.success(Data()))
        case "download", "evict", "read", "delete", "versions", "resolve", "link":
            guard signedIn(reply) else { return }
            var path = text
            var keep = ""
            if method == "resolve" {
                let a = json()
                path = a["path"] as? String ?? ""
                keep = a["keep"] as? String ?? ""
            }
            guard inContainer(path, reply) else { return }
            let url = URL(fileURLWithPath: path)
            work(reply) {
                switch method {
                case "download":
                    try FileManager.default.startDownloadingUbiquitousItem(at: url)
                    return Data()
                case "evict":
                    try FileManager.default.evictUbiquitousItem(at: url)
                    return Data()
                case "read":
                    return try Self.coordinate(reading: url) { try Data(contentsOf: $0) }
                case "delete":
                    return try Self.coordinate(writing: url, .forDeleting) { u in
                        try FileManager().removeItem(at: u)
                        return Data()
                    }
                case "versions":
                    guard FileManager.default.fileExists(atPath: path) || FileManager.default.isUbiquitousItem(at: url) else {
                        throw CocoaError(.fileNoSuchFile)
                    }
                    let vs = (NSFileVersion.unresolvedConflictVersionsOfItem(at: url) ?? []).map(Self.version)
                    return try JSONSerialization.data(withJSONObject: vs)
                case "resolve":
                    return try Self.coordinate(writing: url, .forReplacing) { u in
                        if !keep.isEmpty {
                            guard let v = NSFileVersion.unresolvedConflictVersionsOfItem(at: u)?.first(where: { Self.id(of: $0) == keep }) else {
                                throw Failure("no version \(keep)")
                            }
                            try v.replaceItem(at: u, options: [])
                        }
                        try NSFileVersion.removeOtherVersionsOfItem(at: u)
                        for v in NSFileVersion.unresolvedConflictVersionsOfItem(at: u) ?? [] { v.isResolved = true }
                        return Data()
                    }
                default: // link
                    var expires: NSDate?
                    let link = try FileManager.default.url(forPublishingUbiquitousItemAt: url, expiration: &expires)
                    var out: [String: Any] = ["url": link.absoluteString]
                    if let e = expires { out["expires"] = Self.ms(e as Date) }
                    return try JSONSerialization.data(withJSONObject: out)
                }
            }
        case "write":
            guard signedIn(reply) else { return }
            guard args.count >= 4 else { return reply(.failure(Failure("bad arguments"))) }
            let n = Int(args[args.startIndex]) | Int(args[args.startIndex + 1]) << 8 | Int(args[args.startIndex + 2]) << 16 | Int(args[args.startIndex + 3]) << 24
            guard args.count >= 4 + n else { return reply(.failure(Failure("bad arguments"))) }
            let path = String(decoding: args[(args.startIndex + 4)..<(args.startIndex + 4 + n)], as: UTF8.self)
            let data = Data(args[(args.startIndex + 4 + n)...])
            guard inContainer(path, reply) else { return }
            work(reply) {
                try Self.coordinate(writing: URL(fileURLWithPath: path), .forReplacing) { u in
                    try FileManager().createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try data.write(to: u, options: .atomic)
                    return Data()
                }
            }
        case "move", "moveIn", "moveOut":
            guard signedIn(reply) else { return }
            let a = json()
            let from = a["from"] as? String ?? "", to = a["to"] as? String ?? ""
            if method != "moveIn" && !inContainer(from, reply) || method != "moveOut" && !inContainer(to, reply) { return }
            let src = URL(fileURLWithPath: from), dst = URL(fileURLWithPath: to)
            work(reply) {
                let fm = FileManager()
                try fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
                switch method {
                case "moveIn":
                    try fm.setUbiquitous(true, itemAt: src, destinationURL: dst)
                case "moveOut":
                    try fm.setUbiquitous(false, itemAt: src, destinationURL: dst)
                default:
                    let c = NSFileCoordinator(filePresenter: nil)
                    var cerr: NSError?
                    var failure: Error?
                    c.coordinate(writingItemAt: src, options: .forMoving, writingItemAt: dst, options: .forReplacing, error: &cerr) { a, b in
                        do {
                            if fm.fileExists(atPath: b.path) { try fm.removeItem(at: b) }
                            c.item(at: a, willMoveTo: b)
                            try fm.moveItem(at: a, to: b)
                            c.item(at: a, didMoveTo: b)
                        } catch {
                            failure = error
                        }
                    }
                    if let e = cerr ?? failure { throw e }
                }
                return Data()
            }
        case "next":
            listening = true
            if !events.isEmpty {
                return reply(.success(events.removeFirst()))
            }
            waiting?(.failure(Failure("stopped")))
            waiting = reply
        case "stop":
            waiting?(.failure(Failure("stopped")))
            waiting = nil
            listening = false
            events = []
            reply(.success(Data()))
        default:
            reply(.failure(Failure("no module method icloud.\(method)")))
        }
    }

    /// Why method can't run: on a Mac, an app signed without iCloud's
    /// entitlements (which only a provisioning profile grants: asuka dev
    /// signs ad hoc), whose store would keep nothing and whose containers
    /// aren't there. iOS builds always have them.
    private func missingEntitlement(_ method: String) -> String? {
        #if os(macOS)
        let kv = method.hasPrefix("kv")
        if kv && kvStore != nil || ["next", "stop", "watch", "unwatch"].contains(method) { return nil }
        func entitled(_ key: String) -> Bool {
            guard let task = SecTaskCreateFromSelf(nil) else { return false }
            return SecTaskCopyValueForEntitlement(task, key as CFString, nil) != nil
        }
        let store = entitled("com.apple.developer.ubiquity-kvstore-identifier")
        let documents = entitled("com.apple.developer.ubiquity-container-identifiers")
        let what = method == "account" ? "iCloud" : kv ? "iCloud's key-value store" : "iCloud's documents"
        if method == "account" ? !store && !documents : kv ? !store : !documents {
            return "the app isn't signed with \(what) (on a Mac, a provisioning profile's: asuka build --profile)"
        }
        #endif
        return nil
    }

    // MARK: The account

    /// Who's signed in: a hash of the identity token's archive, which is
    /// the same for the same account.
    private func account() -> [String: Any] {
        guard let token = FileManager.default.ubiquityIdentityToken else { return ["available": false] }
        let data = (try? NSKeyedArchiver.archivedData(withRootObject: token, requiringSecureCoding: false)) ?? Data()
        let id = SHA256.hash(data: data).prefix(16).map { String(format: "%02x", $0) }.joined()
        return ["available": true, "id": id]
    }

    private func signedIn(_ reply: Reply) -> Bool {
        if FileManager.default.ubiquityIdentityToken != nil { return true }
        reply(.failure(Failure("no account: not signed in to iCloud, or iCloud is off for the app")))
        return false
    }

    /// Whether path is in a ubiquity container (where iCloud keeps them).
    private func inContainer(_ path: String, _ reply: Reply) -> Bool {
        if path.hasPrefix("/") && path.contains("/Mobile Documents/") { return true }
        reply(.failure(Failure("not a container: \(path)")))
        return false
    }

    // MARK: Values

    /// A value of JSON's as a property list's (nil: none). A null in an
    /// object is left out, which decodes the same; in an array, there's no
    /// leaving it out.
    static func plistValue(_ v: Any) throws -> Any? {
        switch v {
        case is NSNull:
            return nil
        case let d as [String: Any]:
            var out: [String: Any] = [:]
            for (k, x) in d {
                if let p = try plistValue(x) { out[k] = p }
            }
            return out
        case let a as [Any]:
            return try a.map { x -> Any in
                guard let p = try plistValue(x) else { throw Failure("a null in an array isn't a property list's value") }
                return p
            }
        default:
            return v
        }
    }

    /// A property list's value as JSON's: data as base64, dates as RFC 3339.
    static func jsonValue(_ v: Any) -> Any {
        switch v {
        case let d as Data:
            return d.base64EncodedString()
        case let t as Date:
            let f = ISO8601DateFormatter()
            f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
            return f.string(from: t)
        case let d as [String: Any]:
            return d.mapValues(jsonValue)
        case let a as [Any]:
            return a.map(jsonValue)
        default:
            return v
        }
    }

    // MARK: Files

    private func query(_ scope: String) -> NSMetadataQuery {
        let q = NSMetadataQuery()
        q.searchScopes = [scope == "data" ? NSMetadataQueryUbiquitousDataScope : NSMetadataQueryUbiquitousDocumentsScope]
        q.predicate = NSPredicate(format: "%K LIKE '*'", NSMetadataItemFSNameKey)
        q.sortDescriptors = [NSSortDescriptor(key: NSMetadataItemPathKey, ascending: true)]
        return q
    }

    private func files(_ q: NSMetadataQuery) -> [[String: Any]] {
        var out: [[String: Any]] = []
        for case let item as NSMetadataItem in q.results {
            guard let url = item.value(forAttribute: NSMetadataItemURLKey) as? URL else { continue }
            var f: [String: Any] = ["path": url.resolvingSymlinksInPath().path,
                                    "name": item.value(forAttribute: NSMetadataItemDisplayNameKey) as? String ?? url.lastPathComponent]
            let attr = { (k: String) in item.value(forAttribute: k) }
            if (attr(NSMetadataItemContentTypeKey) as? String) == "public.folder" { f["dir"] = true }
            f["size"] = (attr(NSMetadataItemFSSizeKey) as? NSNumber)?.int64Value ?? 0
            if let d = attr(NSMetadataItemFSCreationDateKey) as? Date { f["created"] = Self.ms(d) }
            if let d = attr(NSMetadataItemFSContentChangeDateKey) as? Date { f["modified"] = Self.ms(d) }
            switch attr(NSMetadataUbiquitousItemDownloadingStatusKey) as? String {
            case NSMetadataUbiquitousItemDownloadingStatusCurrent: f["status"] = "current"
            case NSMetadataUbiquitousItemDownloadingStatusDownloaded: f["status"] = "downloaded"
            default: f["status"] = "notDownloaded"
            }
            let flag = { (k: String) in (attr(k) as? NSNumber)?.boolValue ?? false }
            f["downloading"] = flag(NSMetadataUbiquitousItemIsDownloadingKey)
            f["uploading"] = flag(NSMetadataUbiquitousItemIsUploadingKey)
            f["uploaded"] = flag(NSMetadataUbiquitousItemIsUploadedKey)
            f["conflicts"] = flag(NSMetadataUbiquitousItemHasUnresolvedConflictsKey)
            if let p = attr(NSMetadataUbiquitousItemPercentDownloadedKey) as? NSNumber { f["downloadPercent"] = p.doubleValue }
            if let p = attr(NSMetadataUbiquitousItemPercentUploadedKey) as? NSNumber { f["uploadPercent"] = p.doubleValue }
            if let e = attr(NSMetadataUbiquitousItemDownloadingErrorKey) as? NSError { f["downloadError"] = e.localizedDescription }
            if let e = attr(NSMetadataUbiquitousItemUploadingErrorKey) as? NSError { f["uploadError"] = e.localizedDescription }
            out.append(f)
        }
        return out
    }

    private func sendFiles(_ scope: String, _ q: NSMetadataQuery) {
        q.disableUpdates()
        let list = files(q)
        q.enableUpdates()
        send(["type": "files", "scope": scope, "files": list])
    }

    static func ms(_ d: Date) -> Int64 { Int64((d.timeIntervalSince1970 * 1000).rounded()) }

    static func id(of v: NSFileVersion) -> String {
        ((try? NSKeyedArchiver.archivedData(withRootObject: v.persistentIdentifier, requiringSecureCoding: false)) ?? Data()).base64EncodedString()
    }

    static func version(_ v: NSFileVersion) -> [String: Any] {
        var out: [String: Any] = ["id": id(of: v), "path": v.url.path]
        if let n = v.localizedName { out["name"] = n }
        if let d = v.modificationDate { out["modified"] = ms(d) }
        if let c = v.localizedNameOfSavingComputer { out["device"] = c }
        return out
    }

    static func coordinate(reading url: URL, _ f: (URL) throws -> Data) throws -> Data {
        var cerr: NSError?
        var result: Result<Data, Error> = .failure(CocoaError(.fileReadUnknown))
        NSFileCoordinator(filePresenter: nil).coordinate(readingItemAt: url, options: [], error: &cerr) { u in result = Result { try f(u) } }
        if let e = cerr { throw e }
        return try result.get()
    }

    static func coordinate(writing url: URL, _ options: NSFileCoordinator.WritingOptions, _ f: (URL) throws -> Data) throws -> Data {
        var cerr: NSError?
        var result: Result<Data, Error> = .failure(CocoaError(.fileWriteUnknown))
        NSFileCoordinator(filePresenter: nil).coordinate(writingItemAt: url, options: options, error: &cerr) { u in result = Result { try f(u) } }
        if let e = cerr { throw e }
        return try result.get()
    }

    /// Runs f off the main thread, answering with what it returns (its
    /// errors as the package reads them) on the main thread.
    private func work(_ reply: @escaping Reply, _ f: @escaping () throws -> Data) {
        io.async {
            let r = Result { try f() }.mapError(Self.mapped)
            DispatchQueue.main.async { reply(r) }
        }
    }

    /// An error, "not exist" when it says the file isn't there.
    static func mapped(_ e: Error) -> Error {
        let ns = e as NSError
        let missing = ns.domain == NSCocoaErrorDomain && [NSFileNoSuchFileError, NSFileReadNoSuchFileError].contains(ns.code)
            || ns.domain == NSPOSIXErrorDomain && ns.code == Int(ENOENT)
            || (ns.userInfo[NSUnderlyingErrorKey] as? NSError).map { $0.domain == NSPOSIXErrorDomain && $0.code == Int(ENOENT) } ?? false
        if missing {
            return Failure("not exist: " + ((ns.userInfo[NSFilePathErrorKey] as? String) ?? e.localizedDescription))
        }
        return e
    }

    // MARK: Events

    private func answer(_ reply: Reply, _ v: Any) {
        reply(Result { try JSONSerialization.data(withJSONObject: v) })
    }

    /// Gives Go an event: to the waiting "next", or kept until one comes,
    /// while Go listens.
    private func send(_ e: [String: Any]) {
        guard listening, let d = try? JSONSerialization.data(withJSONObject: e) else { return }
        if let w = waiting {
            waiting = nil
            w(.success(d))
        } else {
            events.append(d)
        }
    }
}
/// What the module keeps values in: iCloud's store, or a test's.
protocol AsukaKVStore: AnyObject {
    func object(forKey: String) -> Any?
    func set(_ value: Any?, forKey: String)
    func removeObject(forKey: String)
    func synchronize() -> Bool
    var values: [String: Any] { get }
}

extension NSUbiquitousKeyValueStore: AsukaKVStore {
    var values: [String: Any] { dictionaryRepresentation }
}

extension UserDefaults: AsukaKVStore {
    var values: [String: Any] { persistentDomain(forName: AsukaICloud.testSuite) ?? [:] }
}

extension AsukaICloud {
    /// The UserDefaults suite a test keeps values in.
    static let testSuite = "dev.asuka.icloud.test"
}
#endif
