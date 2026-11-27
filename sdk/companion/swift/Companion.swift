// platform/companion on iOS and watchOS: WatchConnectivity's session,
// the same on both sides. What crosses is a dictionary with the text
// under "asuka" (a message, a queued one, the context) or the file's
// metadata under it.
import Foundation
#if canImport(WatchConnectivity)
import WatchConnectivity

final class CompanionSession: NSObject, Session, WCSessionDelegate, @unchecked Sendable {
    static let shared = CompanionSession()

    /// Everything below, touched from WatchConnectivity's queue and the
    /// callers' threads.
    private let lock = NSLock()
    private var started = false
    private var filesDir = ""
    private var statusWatchers: [UUID: (Status) -> Void] = [:]
    private var messageWatchers: [UUID: (Message) -> Void] = [:]
    private var contextWatchers: [UUID: (String) -> Void] = [:]
    private var fileWatchers: [UUID: (File) -> Void] = [:]
    /// What came while none listened, given to the first that does.
    private var pendingMessages: [Message] = []
    private var pendingFiles: [File] = []
    /// The answers awaited, by message id.
    private var replies: [Int: ([String: Any]) -> Void] = [:]
    private var nextID = 0

    func start(filesDir: String) throws {
        lock.lock()
        self.filesDir = filesDir
        let first = !started
        started = true
        lock.unlock()
        guard first, WCSession.isSupported() else { return }
        let s = WCSession.default
        s.delegate = self
        s.activate()
    }

    func status() throws -> Status { Self.status() }

    static func status() -> Status {
        guard WCSession.isSupported() else { return Status() }
        let s = WCSession.default
        // The pairing and the other app are known once it's activated.
        guard s.activationState == .activated else { return Status(supported: true) }
        #if os(iOS)
        return Status(supported: true, paired: s.isPaired, installed: s.isPaired && s.isWatchAppInstalled, reachable: s.isReachable)
        #else
        return Status(supported: true, paired: true, installed: s.isCompanionAppInstalled, reachable: s.isReachable)
        #endif
    }

    // MARK: Watching

    /// Runs until the task is cancelled, with add's watcher in place.
    private func until(_ add: () -> UUID, _ remove: @escaping (UUID) -> Void) async {
        let id = add()
        defer { remove(id) }
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    func watch(changed: @escaping (Status) -> Void) async throws {
        changed(Self.status())
        await until({
            lock.lock(); defer { lock.unlock() }
            let id = UUID()
            statusWatchers[id] = changed
            return id
        }, { [weak self] id in
            guard let self else { return }
            self.lock.lock(); self.statusWatchers[id] = nil; self.lock.unlock()
        })
    }

    func messages(received: @escaping (Message) -> Void) async throws {
        await until({
            lock.lock()
            let id = UUID()
            messageWatchers[id] = received
            let waiting = pendingMessages
            pendingMessages = []
            lock.unlock()
            for m in waiting { received(m) }
            return id
        }, { [weak self] id in
            guard let self else { return }
            self.lock.lock(); self.messageWatchers[id] = nil; self.lock.unlock()
        })
    }

    func contexts(changed: @escaping (String) -> Void) async throws {
        await until({
            lock.lock(); defer { lock.unlock() }
            let id = UUID()
            contextWatchers[id] = changed
            return id
        }, { [weak self] id in
            guard let self else { return }
            self.lock.lock(); self.contextWatchers[id] = nil; self.lock.unlock()
        })
    }

    func files(received: @escaping (File) -> Void) async throws {
        await until({
            lock.lock()
            let id = UUID()
            fileWatchers[id] = received
            let waiting = pendingFiles
            pendingFiles = []
            lock.unlock()
            for f in waiting { received(f) }
            return id
        }, { [weak self] id in
            guard let self else { return }
            self.lock.lock(); self.fileWatchers[id] = nil; self.lock.unlock()
        })
    }

    // MARK: Sending

    private func session() throws -> WCSession {
        guard WCSession.isSupported() else { throw CompanionError.unavailable }
        let s = WCSession.default
        guard s.activationState == .activated else { throw CompanionError.unavailable }
        return s
    }

    /// WatchConnectivity's error as the package's.
    static func error(_ e: Error) -> Error {
        guard let w = e as? WCError else { return e }
        switch w.code {
        case .notReachable, .messageReplyTimedOut, .messageReplyFailed: return CompanionError.notReachable
        case .watchAppNotInstalled, .companionAppNotInstalled: return CompanionError.notInstalled
        case .sessionNotSupported, .sessionNotActivated, .deviceNotPaired, .sessionInactive: return CompanionError.unavailable
        default: return e
        }
    }

    func send(text: String) async throws -> String {
        let s = try session()
        guard s.isReachable else { throw CompanionError.notReachable }
        return try await withCheckedThrowingContinuation { c in
            s.sendMessage(["asuka": text], replyHandler: { r in
                c.resume(returning: r["asuka"] as? String ?? "")
            }, errorHandler: { e in
                c.resume(throwing: Self.error(e))
            })
        }
    }

    func reply(id: Int, text: String) throws {
        lock.lock()
        let r = replies.removeValue(forKey: id)
        lock.unlock()
        r?(["asuka": text])
    }

    func queue(text: String) throws {
        _ = try session().transferUserInfo(["asuka": text])
    }

    func setContext(text: String) throws {
        do {
            try session().updateApplicationContext(["asuka": text])
        } catch {
            throw Self.error(error)
        }
    }

    func context() throws -> String {
        guard WCSession.isSupported() else { return "" }
        return WCSession.default.receivedApplicationContext["asuka"] as? String ?? ""
    }

    func sendFile(path: String, metadata: String) throws {
        let url = URL(fileURLWithPath: path)
        guard FileManager.default.fileExists(atPath: path) else {
            throw NSError(domain: NSCocoaErrorDomain, code: NSFileNoSuchFileError, userInfo: [NSLocalizedDescriptionKey: "companion: no file \(path)"])
        }
        _ = try session().transferFile(url, metadata: ["asuka": metadata])
    }

    // MARK: Receiving

    private func statusChanged() {
        let st = Self.status()
        lock.lock()
        let ws = Array(statusWatchers.values)
        lock.unlock()
        for w in ws { w(st) }
    }

    /// Gives m to those that listen, or keeps it for the first that does;
    /// a message whose sender waits is answered "" when none listens.
    private func deliver(_ m: Message, reply: (([String: Any]) -> Void)?) {
        lock.lock()
        nextID += 1
        var m = m
        m.id = nextID
        let ws = Array(messageWatchers.values)
        if ws.isEmpty {
            m.wantsReply = false
            pendingMessages.append(m)
        } else if let reply {
            replies[m.id] = reply
        }
        lock.unlock()
        if ws.isEmpty {
            reply?(["asuka": ""])
        }
        for w in ws { w(m) }
    }

    func session(_ session: WCSession, activationDidCompleteWith activationState: WCSessionActivationState, error: Error?) {
        statusChanged()
        // The context that came while the app didn't run.
        let c = session.receivedApplicationContext["asuka"] as? String
        if let c {
            lock.lock()
            let ws = Array(contextWatchers.values)
            lock.unlock()
            for w in ws { w(c) }
        }
    }

    #if os(iOS)
    func sessionDidBecomeInactive(_ session: WCSession) { statusChanged() }

    func sessionDidDeactivate(_ session: WCSession) {
        // The user switched watches: the session goes on with the new one.
        WCSession.default.activate()
        statusChanged()
    }

    func sessionWatchStateDidChange(_ session: WCSession) { statusChanged() }
    #else
    func sessionCompanionAppInstalledDidChange(_ session: WCSession) { statusChanged() }
    #endif

    func sessionReachabilityDidChange(_ session: WCSession) { statusChanged() }

    func session(_ session: WCSession, didReceiveMessage message: [String: Any]) {
        deliver(Message(text: message["asuka"] as? String ?? ""), reply: nil)
    }

    func session(_ session: WCSession, didReceiveMessage message: [String: Any], replyHandler: @escaping ([String: Any]) -> Void) {
        deliver(Message(text: message["asuka"] as? String ?? "", wantsReply: true), reply: replyHandler)
    }

    func session(_ session: WCSession, didReceiveUserInfo userInfo: [String: Any] = [:]) {
        deliver(Message(text: userInfo["asuka"] as? String ?? "", queued: true), reply: nil)
    }

    func session(_ session: WCSession, didReceiveApplicationContext applicationContext: [String: Any]) {
        let c = applicationContext["asuka"] as? String ?? ""
        lock.lock()
        let ws = Array(contextWatchers.values)
        lock.unlock()
        for w in ws { w(c) }
    }

    func session(_ session: WCSession, didReceive file: WCSessionFile) {
        // The file goes once this returns: moved into the app's folder now.
        lock.lock()
        let dir = filesDir
        lock.unlock()
        let fm = FileManager.default
        let folder = URL(fileURLWithPath: dir, isDirectory: true)
        try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
        var dest = folder.appendingPathComponent(file.fileURL.lastPathComponent)
        if fm.fileExists(atPath: dest.path) {
            dest = folder.appendingPathComponent(UUID().uuidString + "-" + file.fileURL.lastPathComponent)
        }
        guard (try? fm.moveItem(at: file.fileURL, to: dest)) != nil else { return }
        let f = File(path: dest.path, metadata: file.metadata?["asuka"] as? String ?? "")
        lock.lock()
        let ws = Array(fileWatchers.values)
        if ws.isEmpty { pendingFiles.append(f) }
        lock.unlock()
        for w in ws { w(f) }
    }
}

func makeSession() throws -> any Session { CompanionSession.shared }
#else
func makeSession() throws -> any Session {
    throw AsukaNativeError.unsupported("platform/companion: WatchConnectivity is an iPhone's and an Apple Watch's")
}
#endif
