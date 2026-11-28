// Upload.swift: std/http's request bodies sent as they're read, on iOS,
// macOS, tvOS and watchOS (Transport.upload, begin, finished; Sender).
//
// Never the whole body in memory:
//   - one file (Upload.file): URLSession's upload from the file, which
//     reads it as it sends;
//   - bytes alone (Upload.data): URLSession's upload from them (they're
//     in memory already);
//   - parts with files among them (multipart): a streamed request, its
//     body a bound pair of streams (BodyPipe) that a writer fills, part
//     after part, each file 64 KiB at a time, as the network takes them.
//     It starts sending at once and needs no disk, where copying the parts
//     into one file first (a "spool") would read and write every file
//     twice, and hold a copy of a video as big as the video. A body asked
//     for again (a redirect that keeps it, a 401's credentials) is written
//     again from the first part.
//   - a Stream's pieces (begin, Sender): the same pipe, each piece written
//     into it as Asuka sends it; the pipe's small buffer is the
//     backpressure (write waits while it's full).
// The pipes' streams are scheduled on a thread of their own (PipeLoop),
// never the main thread; a write suspends its caller until its piece is
// in the pipe.
//
// background: a background URLSession (BackgroundUploads), which sends
// files alone: one file is cloned into a directory of ours (APFS makes the
// clone at once, sharing the file's blocks, so a caller that deletes or
// changes its file after doesn't change what's sent), anything else
// written into a file there; the file goes when its upload ends. The
// awaiting call gets its answer while the process lives. If it's gone,
// the system starts the app again (iOS, watchOS, tvOS) when the upload
// ends, the host's delegate hands this its events (the host's
// AsukaBackgroundSessions, by the session's identifier, whose last part
// is the class's name, AsukaHttpBackground), and what no call waited for
// is kept (finished.json) until finished gives it. A Mac starts no app
// for it: the session goes on after the app quits, and is reconnected as
// it next makes a transport (or asks finished).
//
// A secured transport's background upload answers its server's challenges
// with the transport's TLS settings while the process that made it lives.
// They're in its memory alone (its key isn't written anywhere), so after
// the system started the app again a challenge is refused: the upload
// fails, its FinishedUpload says why. Never the system's trust in their
// place.
import Foundation
import Asuka

extension URLSessionTransport {
    /// A request of upload's and begin's: its timeout the longest wait for
    /// the network to take more, then for the answer.
    fileprivate func uploadRequest(_ method: String, _ url: String, _ headers: [String: String], _ timeout: TimeInterval) throws -> URLRequest {
        guard let u = URL(string: url) else {
            throw URLError(.badURL)
        }
        var req = URLRequest(url: u)
        req.httpMethod = method
        req.timeoutInterval = timeout > 0 ? timeout : UploadSession.forever
        for (k, v) in headers {
            req.setValue(v, forHTTPHeaderField: k)
        }
        return req
    }

    func upload(method: String, url: String, headers: [String: String], parts: [Part], length: Int, timeout: TimeInterval, progress: @escaping (Int, Int) -> Void, background: Bool, id: String) async throws -> Response {
        var req = try uploadRequest(method, url, headers, timeout)
        if background {
            return try await BackgroundUploads.app.upload(req, parts, length, progress, id, policy)
        }
        let session = uploads.session
        let task: URLSessionUploadTask
        var source = UploadState.Source.none
        if parts.count == 1 && !parts[0].path.isEmpty {
            task = session.uploadTask(with: req, fromFile: URL(fileURLWithPath: parts[0].path))
        } else if parts.allSatisfy({ $0.path.isEmpty }) {
            let body = parts.count == 1 ? parts[0].data : parts.reduce(into: Data()) { $0.append($1.data) }
            task = session.uploadTask(with: req, from: body)
        } else {
            if length >= 0 {
                req.setValue(String(length), forHTTPHeaderField: "Content-Length")
            }
            source = .parts(parts)
            task = session.uploadTask(withStreamedRequest: req)
        }
        let s = UploadState(url: url, length: length, progress: progress, policy: policy, source: source)
        return try await uploads.run(task, s)
    }

    func begin(method: String, url: String, headers: [String: String], length: Int, timeout: TimeInterval, progress: @escaping (Int, Int) -> Void) async throws -> any Sender {
        var req = try uploadRequest(method, url, headers, timeout)
        // A length said is sent as Content-Length; else URLSession sends
        // the stream chunked.
        if length >= 0 {
            req.setValue(String(length), forHTTPHeaderField: "Content-Length")
        }
        let s = UploadState(url: url, length: length, progress: progress, policy: policy, source: .sender)
        let task = uploads.session.uploadTask(withStreamedRequest: req)
        uploads.add(task, s)
        task.resume()
        return StreamSender(s)
    }

    func finished() async throws -> [FinishedUpload] {
        try await BackgroundUploads.app.finished()
    }
}

func makeSender() throws -> any Sender {
    throw AsukaNativeError.unsupported("a sender comes from Transport.begin")
}

/// What begin gives Asuka: its request's pipe. Let go (or closed), it
/// stops the request.
final class StreamSender: Sender {
    let state: UploadState

    init(_ state: UploadState) {
        self.state = state
    }

    deinit {
        state.cancel()
    }

    func write(piece: Data) async throws -> Bool {
        try await state.write(piece)
    }

    func finish() async throws -> Response {
        try await state.finish()
    }

    func close() throws {
        state.cancel()
    }
}

/// An upload's task as it goes: its progress (a few times a second, and
/// once all is sent), its body's pipe when it's streamed, its answer and
/// who waits for it.
final class UploadState: @unchecked Sendable {
    enum Source {
        /// URLSession reads the body: a file, bytes.
        case none
        /// Parts written into a pipe, in order (a multipart's).
        case parts([Part])
        /// A Sender's pieces.
        case sender
    }

    /// Progress at most this often, but for the last.
    static let every: TimeInterval = 0.1
    /// What a file's read in, for the pipe.
    static let chunk = 64 << 10

    let url: String
    let length: Int
    let progress: (Int, Int) -> Void
    /// The TLS settings its challenges are answered with.
    let policy: TLSPolicy?
    let source: Source
    /// A background upload's body: a file of ours, removed as it ends.
    var file: URL?

    private let lock = NSLock()
    private(set) var task: URLSessionTask?
    private var pipe: BodyPipe?
    private var handed = false
    private var writer: Task<Void, Never>?
    private var reported = Date.distantPast
    private var allReported = false
    private var head: URLResponse?
    private var body = Data()
    private var waiter: CheckedContinuation<Response, Error>?
    private var result: Result<Response, Error>?
    /// Why the body couldn't be written: a part's file couldn't be read.
    private var failure: Error?
    private(set) var cancelled = false

    init(url: String, length: Int, progress: @escaping (Int, Int) -> Void, policy: TLSPolicy?, source: Source) {
        self.url = url
        self.length = length
        self.progress = progress
        self.policy = policy
        self.source = source
        if case .sender = source {
            // Its pieces may come before URLSession asks for the body.
            pipe = BodyPipe()
        }
    }

    func started(_ t: URLSessionTask) {
        lock.lock()
        task = t
        lock.unlock()
    }

    /// The body's stream URLSession asks for: a Sender's pipe once (its
    /// pieces can't be sent again), a parts' pipe each time, written from
    /// the first part.
    func newBody() -> InputStream? {
        lock.lock()
        defer { lock.unlock() }
        if cancelled || result != nil { return nil }
        switch source {
        case .none:
            return nil
        case .sender:
            if handed { return nil }
            handed = true
            return pipe?.input
        case .parts(let parts):
            writer?.cancel()
            pipe?.close()
            let p = BodyPipe()
            pipe = p
            writer = Task.detached { [weak self] in
                await UploadState.feed(parts, p, self)
            }
            return p.input
        }
    }

    /// Writes parts into p, in order, each file a chunk at a time: the pipe
    /// ended (the server answered, the task went) stops it.
    private static func feed(_ parts: [Part], _ p: BodyPipe, _ s: UploadState?) async {
        do {
            parts: for part in parts {
                if Task.isCancelled { return }
                if part.path.isEmpty {
                    if !part.data.isEmpty, !(await p.write(part.data)) { break }
                    continue
                }
                let h = try FileHandle(forReadingFrom: URL(fileURLWithPath: part.path))
                defer { try? h.close() }
                while true {
                    if Task.isCancelled { return }
                    guard let d = try h.read(upToCount: chunk), !d.isEmpty else { break }
                    if !(await p.write(d)) { break parts }
                }
            }
        } catch {
            s?.failed(error)
            return
        }
        p.finish()
    }

    func failed(_ e: Error) {
        lock.lock()
        if failure == nil { failure = e }
        let t = task
        lock.unlock()
        t?.cancel()
    }

    /// The network has taken total bytes of the body.
    func sent(_ total: Int) {
        lock.lock()
        if allReported {
            lock.unlock()
            return
        }
        let all = length >= 0 && total >= length
        let now = Date()
        if !all && now.timeIntervalSince(reported) < UploadState.every {
            lock.unlock()
            return
        }
        reported = now
        if all { allReported = true }
        lock.unlock()
        progress(total, length)
    }

    func received(_ r: URLResponse) {
        lock.lock()
        head = r
        let p = pipe
        lock.unlock()
        // The answer's here: what's still to be written won't be read
        // (a server that answers before the body's end: 413).
        p?.end()
    }

    func received(_ d: Data) {
        lock.lock()
        body.append(d)
        lock.unlock()
    }

    func completed(_ t: URLSessionTask, _ error: Error?) {
        lock.lock()
        let k = waiter
        waiter = nil
        let p = pipe
        let w = writer
        let f = failure
        let done = allReported
        allReported = true
        let r: Result<Response, Error>
        if let f {
            r = .failure(f)
        } else if let error {
            r = .failure(error)
        } else {
            r = .success(responseOf(t.response ?? head, url, body))
        }
        body = Data()
        if k == nil { result = r }
        lock.unlock()
        p?.close()
        w?.cancel()
        if case .success = r, !done {
            progress(Int(t.countOfBytesSent), length)
        }
        k?.resume(with: r)
    }

    /// Waits for the answer, start (resuming the task, ending the body)
    /// once it's waited for.
    func wait(_ start: @escaping () -> Void) async throws -> Response {
        try await withCheckedThrowingContinuation { (k: CheckedContinuation<Response, Error>) in
            lock.lock()
            if let r = result {
                lock.unlock()
                k.resume(with: r)
                return
            }
            waiter = k
            lock.unlock()
            start()
        }
    }

    /// Stops the request.
    func cancel() {
        lock.lock()
        cancelled = true
        let t = task
        let p = pipe
        lock.unlock()
        t?.cancel()
        p?.end()
    }

    // ---- A Sender's ----

    func write(_ piece: Data) async throws -> Bool {
        guard let p = pipe else { return false }
        return await withTaskCancellationHandler {
            await p.write(piece)
        } onCancel: {
            // Half a piece sent: the request can't go on.
            self.cancel()
        }
    }

    func finish() async throws -> Response {
        let p = pipe
        return try await withTaskCancellationHandler {
            try await wait { p?.finish() }
        } onCancel: {
            self.cancel()
        }
    }
}

/// The Response of what a task answered.
func responseOf(_ r: URLResponse?, _ url: String, _ body: Data) -> Response {
    let http = r as? HTTPURLResponse
    var out: [String: String] = [:]
    for (k, v) in http?.allHeaderFields ?? [:] {
        out["\(k)".lowercased()] = "\(v)"
    }
    return Response(status: http?.statusCode ?? 0, url: r?.url?.absoluteString ?? url, headers: out, body: body)
}

/// A session for uploads and its delegate, which hands each task's events
/// to its state: the system's, one per secured transport, and the
/// background one (BackgroundUploads).
class UploadSession: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    static let shared = UploadSession(policy: nil)
    /// No timeout: an upload may take as long as the network does.
    static let forever: TimeInterval = 1_000_000_000

    let policy: TLSPolicy?
    let lock = NSLock()
    private var states: [Int: UploadState] = [:]
    let failures = TrustFailures()
    private var made: URLSession?

    init(policy: TLSPolicy?) {
        self.policy = policy
        super.init()
    }

    /// The session, made the first time it's asked for.
    var session: URLSession {
        lock.lock()
        defer { lock.unlock() }
        if let s = made { return s }
        let s = makeSession()
        made = s
        return s
    }

    /// Whether the session's made.
    var connected: Bool {
        lock.lock()
        defer { lock.unlock() }
        return made != nil
    }

    func makeSession() -> URLSession {
        // The default configuration's cookies and cache, as URLSession.shared has.
        URLSession(configuration: .default, delegate: self, delegateQueue: nil)
    }

    func add(_ task: URLSessionTask, _ s: UploadState) {
        s.started(task)
        lock.lock()
        states[task.taskIdentifier] = s
        lock.unlock()
    }

    func state(_ task: URLSessionTask, remove: Bool = false) -> UploadState? {
        lock.lock()
        defer { lock.unlock() }
        return remove ? states.removeValue(forKey: task.taskIdentifier) : states[task.taskIdentifier]
    }

    /// The states' files: what uploads of this process are sending.
    func files() -> Set<String> {
        lock.lock()
        defer { lock.unlock() }
        return Set(states.values.compactMap { $0.file?.lastPathComponent })
    }

    /// Runs task, s its state, until it answers: cancelling the caller's
    /// task cancels it.
    func run(_ task: URLSessionTask, _ s: UploadState) async throws -> Response {
        add(task, s)
        return try await withTaskCancellationHandler {
            try await s.wait { task.resume() }
        } onCancel: {
            s.cancel()
        }
    }

    /// The task has ended: its state's, if it has one.
    func ended(_ task: URLSessionTask, _ s: UploadState?, _ error: Error?) {
        s?.completed(task, error)
    }

    /// Answers a challenge: with the task's TLS settings, else the system.
    func challenge(_ task: URLSessionTask, _ c: URLAuthenticationChallenge, _ done: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        guard let p = state(task)?.policy ?? policy else { return done(.performDefaultHandling, nil) }
        if let why = p.answer(c, done) { failures.record(task, why) }
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didSendBodyData bytesSent: Int64, totalBytesSent: Int64, totalBytesExpectedToSend: Int64) {
        state(task)?.sent(Int(totalBytesSent))
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, needNewBodyStream completionHandler: @escaping (InputStream?) -> Void) {
        completionHandler(state(task)?.newBody())
    }

    func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse, completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        state(dataTask)?.received(response)
        completionHandler(.allow)
    }

    func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        state(dataTask)?.received(data)
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        self.challenge(task, challenge, completionHandler)
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        ended(task, state(task, remove: true), failures.error(task, error))
    }
}

// ---- The pipe a streamed body's written into ----

/// A bound pair of streams: URLSession reads input as it sends; output is
/// written on PipeLoop's thread, a piece at a time, as its buffer has room.
/// Everything but input is the loop's.
final class BodyPipe: NSObject, StreamDelegate, @unchecked Sendable {
    /// The buffer between the two: what's written before the network takes it.
    static let size = 128 << 10

    let input: InputStream
    private let output: OutputStream
    private var piece = Data()
    private var offset = 0
    private var waiter: CheckedContinuation<Bool, Never>?
    private var finishing = false
    private var ended = false
    private var closed = false

    override init() {
        var i: InputStream?
        var o: OutputStream?
        Stream.getBoundStreams(withBufferSize: BodyPipe.size, inputStream: &i, outputStream: &o)
        input = i!
        output = o!
        super.init()
        PipeLoop.shared.run { [self] in
            output.delegate = self
            output.schedule(in: .current, forMode: .default)
            output.open()
        }
    }

    /// Writes d into the pipe: true once it's all in, false when the pipe
    /// has ended (or the body's been finished) first.
    func write(_ d: Data) async -> Bool {
        await withCheckedContinuation { (k: CheckedContinuation<Bool, Never>) in
            PipeLoop.shared.run { [self] in
                if ended || finishing || waiter != nil {
                    k.resume(returning: false)
                    return
                }
                piece = d
                offset = 0
                waiter = k
                pump()
            }
        }
    }

    /// Ends the body once what's being written is in: URLSession reads its end.
    func finish() {
        PipeLoop.shared.run { [self] in
            finishing = true
            pump()
        }
    }

    /// Takes no more: a write waiting, and those after, are false. The
    /// body isn't ended (a body cut short isn't sent as if it were whole):
    /// the task's end, or finish, closes it.
    func end() {
        PipeLoop.shared.run { [self] in
            ended = true
            release(false)
        }
    }

    /// Ends it all, the task being over.
    func close() {
        PipeLoop.shared.run { [self] in
            ended = true
            release(false)
            closeOutput()
        }
    }

    private func release(_ ok: Bool) {
        piece = Data()
        offset = 0
        let k = waiter
        waiter = nil
        k?.resume(returning: ok)
    }

    private func pump() {
        if closed { return release(false) }
        if waiter != nil && !ended {
            while offset < piece.count && output.hasSpaceAvailable {
                let n = piece.withUnsafeBytes { b in
                    output.write(b.baseAddress!.assumingMemoryBound(to: UInt8.self) + offset, maxLength: b.count - offset)
                }
                if n <= 0 { break }
                offset += n
            }
            if offset >= piece.count { release(true) }
        }
        if finishing && waiter == nil { closeOutput() }
    }

    private func closeOutput() {
        if closed { return }
        closed = true
        output.delegate = nil
        output.close()
        output.remove(from: .current, forMode: .default)
    }

    func stream(_ s: Stream, handle event: Stream.Event) {
        switch event {
        case .hasSpaceAvailable:
            pump()
        case .errorOccurred, .endEncountered:
            ended = true
            release(false)
            closeOutput()
        default:
            break
        }
    }
}

/// The thread the pipes' output streams are scheduled on: its run loop
/// runs what's handed to it, and their events.
final class PipeLoop: Thread, @unchecked Sendable {
    static let shared: PipeLoop = {
        let t = PipeLoop()
        t.name = "asuka.http.upload"
        t.start()
        t.ready.wait()
        return t
    }()

    private let ready = DispatchSemaphore(value: 0)
    private var loop: CFRunLoop!

    override func main() {
        loop = CFRunLoopGetCurrent()
        // A source, so the loop runs while there's nothing to do.
        RunLoop.current.add(Timer(timeInterval: 1_000_000, repeats: true) { _ in }, forMode: .default)
        ready.signal()
        while true {
            _ = autoreleasepool { RunLoop.current.run(mode: .default, before: .distantFuture) }
        }
    }

    /// Runs f on the loop's thread, in order.
    func run(_ f: @escaping () -> Void) {
        CFRunLoopPerformBlock(loop, CFRunLoopMode.defaultMode.rawValue, f)
        CFRunLoopWakeUp(loop)
    }
}

// ---- Background uploads ----

/// The background URLSession: one per app, its identifier
/// "<app id>.AsukaHttpBackground" (the host finds this class by its last
/// part when the system wakes the app for it), made when an upload asks
/// for it, as the app makes a transport while uploads it made are going,
/// and when the system wakes the app for it.
@objc(AsukaHttpBackground) final class BackgroundUploads: UploadSession, AsukaBackgroundSession, @unchecked Sendable {
    static let app = BackgroundUploads(policy: nil)

    /// The handler the system's waiting on, till its events are all in.
    private var events: (() -> Void)?

    /// What the app's uploads keep: their bodies (uploads/) and what
    /// ended with no call waiting (finished.json). Application Support
    /// (tvOS: Caches, the one it lets an app write), not backed up; a
    /// Mac's of the app (Application Support is shared there); under
    /// ASUKA_DATA, the app's directory there (asuka dev's, tests').
    static let directory: URL = {
        let fm = FileManager.default
        var url: URL
        if let d = ProcessInfo.processInfo.environment["ASUKA_DATA"], !d.isEmpty {
            url = URL(fileURLWithPath: d, isDirectory: true)
        } else {
            #if os(tvOS)
            url = fm.urls(for: .cachesDirectory, in: .userDomainMask).first ?? fm.temporaryDirectory
            url.appendPathComponent("asuka-app", isDirectory: true)
            #else
            url = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first ?? fm.temporaryDirectory
            #endif
            #if os(macOS)
            url.appendPathComponent(Bundle.main.bundleIdentifier ?? ProcessInfo.processInfo.processName, isDirectory: true)
            #endif
        }
        url.appendPathComponent("asuka-http", isDirectory: true)
        try? fm.createDirectory(at: url.appendingPathComponent("uploads"), withIntermediateDirectories: true)
        var v = URLResourceValues()
        v.isExcludedFromBackup = true
        try? url.setResourceValues(v)
        return url
    }()

    static var uploads: URL { directory.appendingPathComponent("uploads", isDirectory: true) }
    static let store = FinishedStore(directory.appendingPathComponent("finished.json"))

    /// The session's identifier: the app's ID, and the directory's when
    /// ASUKA_DATA names one (a program without a bundle, asuka dev's
    /// player for each app), then this class's name.
    static let identifier: String = {
        var id = Bundle.main.bundleIdentifier ?? ProcessInfo.processInfo.processName
        if let d = ProcessInfo.processInfo.environment["ASUKA_DATA"], !d.isEmpty {
            var h: UInt64 = 0xcbf2_9ce4_8422_2325
            for b in d.utf8 { h = (h ^ UInt64(b)) &* 0x100_0000_01b3 }
            id += "-" + String(h, radix: 16)
        }
        return id + ".AsukaHttpBackground"
    }()

    override func makeSession() -> URLSession {
        let c = URLSessionConfiguration.background(withIdentifier: BackgroundUploads.identifier)
        c.isDiscretionary = false
        #if !os(macOS)
        c.sessionSendsLaunchEvents = true
        #endif
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 1
        return URLSession(configuration: c, delegate: self, delegateQueue: q)
    }

    // The host's, as the system wakes the app for the session.
    static func handleEvents(_ identifier: String, _ done: @escaping () -> Void) {
        let b = app
        b.lock.lock()
        let old = b.events
        b.events = done
        b.lock.unlock()
        old?()
        _ = b.session
    }

    func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        lock.lock()
        let done = events
        events = nil
        lock.unlock()
        if let done { DispatchQueue.main.async(execute: done) }
    }

    nonisolated(unsafe) private static var checked = false

    /// Reconnects the session when uploads made before are still going
    /// (or ended unseen): once a process, as it makes its first transport.
    static func reconnectIfPending() {
        app.lock.lock()
        let first = !checked
        checked = true
        app.lock.unlock()
        if first && !pending().isEmpty { _ = app.session }
    }

    /// The bodies' files: an upload's whose end this process hasn't seen.
    static func pending() -> [URL] {
        (try? FileManager.default.contentsOfDirectory(at: uploads, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
    }

    /// What a task's taskDescription keeps (it lasts with the task, across
    /// launches): its id, its body's file, whether TLS settings made it.
    struct Description: Codable {
        var id: String
        var file: String
        var secured: Bool
    }

    static func description(_ task: URLSessionTask) -> Description? {
        guard let s = task.taskDescription else { return nil }
        return try? JSONDecoder().decode(Description.self, from: Data(s.utf8))
    }

    func upload(_ req: URLRequest, _ parts: [Part], _ length: Int, _ progress: @escaping (Int, Int) -> Void, _ id: String, _ policy: TLSPolicy?) async throws -> Response {
        let file = BackgroundUploads.uploads.appendingPathComponent(UUID().uuidString)
        do {
            try await BackgroundUploads.write(parts, file)
        } catch {
            try? FileManager.default.removeItem(at: file)
            throw error
        }
        let size = ((try? FileManager.default.attributesOfItem(atPath: file.path)[.size]) as? NSNumber)?.intValue ?? length
        let s = UploadState(url: req.url?.absoluteString ?? "", length: size, progress: progress, policy: policy, source: .none)
        s.file = file
        let task = session.uploadTask(with: req, fromFile: file)
        let d = Description(id: id, file: file.lastPathComponent, secured: policy != nil)
        task.taskDescription = String(decoding: try JSONEncoder().encode(d), as: UTF8.self)
        return try await run(task, s)
    }

    /// The body into a file of ours: one file's clone (or copy, off APFS),
    /// else the parts written in turn, each file a chunk at a time.
    static func write(_ parts: [Part], _ to: URL) async throws {
        let fm = FileManager.default
        if parts.count == 1 && !parts[0].path.isEmpty {
            try fm.copyItem(at: URL(fileURLWithPath: parts[0].path), to: to)
            return
        }
        guard fm.createFile(atPath: to.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: to.path])
        }
        let out = try FileHandle(forWritingTo: to)
        defer { try? out.close() }
        for p in parts {
            try Task.checkCancellation()
            if p.path.isEmpty {
                try out.write(contentsOf: p.data)
                continue
            }
            let h = try FileHandle(forReadingFrom: URL(fileURLWithPath: p.path))
            defer { try? h.close() }
            while let d = try h.read(upToCount: UploadState.chunk), !d.isEmpty {
                try Task.checkCancellation()
                try out.write(contentsOf: d)
            }
        }
    }

    override func challenge(_ task: URLSessionTask, _ c: URLAuthenticationChallenge, _ done: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        if let p = state(task)?.policy {
            if let why = p.answer(c, done) { failures.record(task, why) }
            return
        }
        let m = c.protectionSpace.authenticationMethod
        let tls = m == NSURLAuthenticationMethodServerTrust || m == NSURLAuthenticationMethodClientCertificate
        if tls && BackgroundUploads.description(task)?.secured == true {
            done(.cancelAuthenticationChallenge, nil)
            failures.record(task, "\(c.protectionSpace.host): the upload's TLS settings went with the app that made it (the system started it again), so its server can't be checked as they say")
            return
        }
        done(.performDefaultHandling, nil)
    }

    override func ended(_ task: URLSessionTask, _ s: UploadState?, _ error: Error?) {
        let d = BackgroundUploads.description(task)
        if let f = s?.file ?? d.map({ BackgroundUploads.uploads.appendingPathComponent($0.file) }) {
            try? FileManager.default.removeItem(at: f)
        }
        if let s {
            s.completed(task, error)
            return
        }
        // No call waits: kept for finished.
        guard let d else { return }
        var f = FinishedUpload(id: d.id)
        if let error {
            f.error = error.localizedDescription
        } else {
            let r = responseOf(task.response, task.originalRequest?.url?.absoluteString ?? "", Data())
            f.status = r.status
            f.url = r.url
            f.headers = r.headers
        }
        BackgroundUploads.store.add(f)
    }

    /// What's ended with no call waiting, each once: the session made
    /// first (when uploads were going), and its tasks' ends since the
    /// app last ran in.
    func finished() async throws -> [FinishedUpload] {
        let fm = FileManager.default
        if connected || !BackgroundUploads.pending().isEmpty {
            let s = session
            let live = await withCheckedContinuation { (k: CheckedContinuation<[URLSessionTask], Never>) in
                s.getAllTasks { k.resume(returning: $0) }
            }
            let going = Set(live.compactMap { BackgroundUploads.description($0)?.file }).union(files())
            // A file whose task isn't going: its end is on its way to the
            // delegate (which removes the file). Waited for a little.
            var unseen: [URL] = []
            for _ in 0..<40 {
                unseen = BackgroundUploads.pending().filter { !going.contains($0.lastPathComponent) }
                if unseen.isEmpty { break }
                try await Task.sleep(nanoseconds: 50_000_000)
            }
            // One no task ever had (the app ended as it made it): gone after a day.
            for u in unseen {
                let at = (try? u.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? .distantPast
                if Date().timeIntervalSince(at) > 86_400 { try? fm.removeItem(at: u) }
            }
        }
        return BackgroundUploads.store.take()
    }
}

/// The background uploads that ended with no call waiting, kept in a file
/// till they're asked for.
final class FinishedStore: @unchecked Sendable {
    let url: URL
    private let lock = NSLock()

    init(_ url: URL) {
        self.url = url
    }

    private func read() -> [[String: Any]] {
        guard let d = try? Data(contentsOf: url) else { return [] }
        return ((try? JSONSerialization.jsonObject(with: d)) as? [[String: Any]]) ?? []
    }

    func add(_ f: FinishedUpload) {
        lock.lock()
        defer { lock.unlock() }
        var all = read()
        all.append(["id": f.id, "status": f.status, "url": f.url, "headers": f.headers, "error": f.error])
        if let d = try? JSONSerialization.data(withJSONObject: all) {
            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? d.write(to: url, options: .atomic)
        }
    }

    /// What's kept, removed as it's given.
    func take() -> [FinishedUpload] {
        lock.lock()
        defer { lock.unlock() }
        let all = read()
        if !all.isEmpty { try? FileManager.default.removeItem(at: url) }
        return all.map {
            FinishedUpload(id: $0["id"] as? String ?? "", status: ($0["status"] as? NSNumber)?.intValue ?? 0, url: $0["url"] as? String ?? "",
                           headers: $0["headers"] as? [String: String] ?? [:], error: $0["error"] as? String ?? "")
        }
    }
}
