// std/http on iOS and macOS: URLSession, the system's own client
// (proxies, trusted certificates, the shared cache and cookies). A
// transport secured with TLS settings (TLS.swift) has sessions of its own,
// so its connections aren't another's: their delegates answer the
// server's trust, and a client certificate when one's asked for. Bodies
// sent as they're read (upload, begin, background uploads): Upload.swift.
import Foundation
import Network
import Asuka

final class URLSessionTransport: Transport {
    /// The TLS settings, none for the system's.
    let policy: TLSPolicy?
    /// send's session when secured: its delegate the policy's.
    private let trusted: TrustedSession?
    /// open's session.
    private let streams: Streams
    /// upload's and begin's session (Upload.swift).
    let uploads: UploadSession

    init(policy: TLSPolicy? = nil) {
        self.policy = policy
        trusted = policy.map { TrustedSession($0) }
        streams = policy.map { Streams(policy: $0) } ?? Streams.shared
        uploads = policy.map { UploadSession(policy: $0) } ?? UploadSession.shared
    }

    private static let lock = NSLock()
    nonisolated(unsafe) private static var secured: [ObjectIdentifier: URLSessionTransport] = [:]

    func secured(tls: TLS) async throws -> any Transport {
        URLSessionTransport.of(try TLSPolicy.of(ca: tls.ca, trustSystem: tls.trustSystem, certificate: tls.certificate, key: tls.key, insecure: tls.insecureSkipVerify))
    }

    /// The transport of p: one per policy.
    private static func of(_ p: TLSPolicy) -> URLSessionTransport {
        lock.lock()
        defer { lock.unlock() }
        if let t = secured[ObjectIdentifier(p)] { return t }
        let t = URLSessionTransport(policy: p)
        secured[ObjectIdentifier(p)] = t
        return t
    }

    func send(method: String, url: String, headers: [String: String], body: Data, timeout: TimeInterval) async throws -> Response {
        guard let u = URL(string: url) else {
            throw URLError(.badURL)
        }
        var req = URLRequest(url: u)
        req.httpMethod = method
        if timeout > 0 {
            req.timeoutInterval = timeout
        }
        for (k, v) in headers {
            req.setValue(v, forHTTPHeaderField: k)
        }
        if !body.isEmpty {
            req.httpBody = body
        }
        let (data, resp) = if let trusted { try await trusted.data(for: req) } else { try await URLSession.shared.data(for: req) }
        let http = resp as? HTTPURLResponse
        var out: [String: String] = [:]
        for (k, v) in http?.allHeaderFields ?? [:] {
            out["\(k)".lowercased()] = "\(v)"
        }
        return Response(status: http?.statusCode ?? 0, url: resp.url?.absoluteString ?? url, headers: out, body: data)
    }
}

extension URLSessionTransport {
    func download(method: String, url: String, headers: [String: String], body: Data, timeout: TimeInterval, path: String, progress: @escaping (Int, Int) -> Void) async throws -> Response {
        guard let u = URL(string: url) else {
            throw URLError(.badURL)
        }
        var req = URLRequest(url: u)
        req.httpMethod = method
        if timeout > 0 {
            req.timeoutInterval = timeout  // the longest wait for more
        }
        for (k, v) in headers {
            req.setValue(v, forHTTPHeaderField: k)
        }
        if !body.isEmpty {
            req.httpBody = body
        }
        let d = Download(path: path, progress: progress, policy: policy)
        // A session of its own, for its delegate: the shared cookies and
        // cache (the default configuration's), as URLSession.shared has.
        let session = URLSession(configuration: .default, delegate: d, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }
        let task = session.downloadTask(with: req)
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (k: CheckedContinuation<Response, Error>) in
                d.lock.lock()
                d.continuation = k
                d.lock.unlock()
                task.resume()
            }
        } onCancel: {
            task.cancel()
        }
    }
}

/// A download's delegate: progress as it comes (a few times a second),
/// the file moved to its path once it's all in, the answer; the server's
/// trust as the policy says.
private final class Download: NSObject, URLSessionDownloadDelegate {
    let path: String
    let progress: (Int, Int) -> Void
    let policy: TLSPolicy?
    let failures = TrustFailures()
    let lock = NSLock()
    var continuation: CheckedContinuation<Response, Error>?
    var answer: Result<Response, Error>?
    var reported = Date.distantPast

    init(path: String, progress: @escaping (Int, Int) -> Void, policy: TLSPolicy?) {
        self.path = path
        self.progress = progress
        self.policy = policy
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        guard let policy else { return completionHandler(.performDefaultHandling, nil) }
        if let why = policy.answer(challenge, completionHandler) { failures.record(task, why) }
    }

    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytes: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        let now = Date()
        if now.timeIntervalSince(reported) < 0.1 { return }
        reported = now
        progress(Int(totalBytesWritten), totalBytesExpectedToWrite == NSURLSessionTransferSizeUnknown ? -1 : Int(totalBytesExpectedToWrite))
    }

    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The file goes before this returns: move it now.
        let to = URL(fileURLWithPath: path)
        do {
            let fm = FileManager.default
            try fm.createDirectory(at: to.deletingLastPathComponent(), withIntermediateDirectories: true)
            if fm.fileExists(atPath: to.path) {
                _ = try fm.replaceItemAt(to, withItemAt: location)
            } else {
                try fm.moveItem(at: location, to: to)
            }
            let size = (try? fm.attributesOfItem(atPath: to.path)[.size] as? NSNumber)?.intValue ?? 0
            progress(size, size)
            let http = downloadTask.response as? HTTPURLResponse
            var out: [String: String] = [:]
            for (k, v) in http?.allHeaderFields ?? [:] {
                out["\(k)".lowercased()] = "\(v)"
            }
            answer = .success(Response(status: http?.statusCode ?? 0, url: downloadTask.response?.url?.absoluteString ?? to.absoluteString, headers: out, body: Data()))
        } catch {
            answer = .failure(error)
        }
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        lock.lock()
        let k = continuation
        continuation = nil
        lock.unlock()
        if let e = failures.error(task, error) {
            k?.resume(throwing: e)
        } else {
            k?.resume(with: answer ?? .failure(URLError(.cannotWriteToFile)))
        }
    }
}

func makeTransport() throws -> any Transport {
    // Background uploads that were going as the app last ran: their
    // session again, for what they've done since (Upload.swift).
    BackgroundUploads.reconnectIfPending()
    return URLSessionTransport()
}

func makeBody() throws -> any Body {
    throw AsukaNativeError.unsupported("a body comes from Transport.open")
}

// ---- Bodies as they come: a data task whose pieces are read in turn ----

extension URLSessionTransport {
    func `open`(method: String, url: String, headers: [String: String], body: Data, timeout: TimeInterval) async throws -> any Body {
        guard let u = URL(string: url) else {
            throw URLError(.badURL)
        }
        var req = URLRequest(url: u)
        req.httpMethod = method
        // URLSession's request timeout is its longest wait for more: for
        // the head, then for each piece (a task suspended for its reader,
        // below, isn't waiting).
        req.timeoutInterval = timeout > 0 ? timeout : Streams.forever
        for (k, v) in headers {
            req.setValue(v, forHTTPHeaderField: k)
        }
        if !body.isEmpty {
            req.httpBody = body
        }
        // URLSession holds a text/plain answer's head and body (or one
        // without a Content-Type) until 512 bytes are in, to sniff what
        // it is: a stream of text would come late, in one piece. The
        // request's property WebKit sets turns that off (a protocol
        // property, through the public URLProtocol).
        let m = (req as NSURLRequest).mutableCopy() as! NSMutableURLRequest
        URLProtocol.setProperty(false, forKey: "_kCFURLConnectionPropertyShouldSniff", in: m)
        let task = streams.session.dataTask(with: m as URLRequest)
        let s = StreamState(task: task, url: url)
        streams.add(task, s)
        return try await withTaskCancellationHandler {
            _ = try await withCheckedThrowingContinuation { (k: CheckedContinuation<Response, Error>) in
                s.waitHead(k)
                if Task.isCancelled { task.cancel() }  // before the handler was in
            }
            return StreamBody(s)
        } onCancel: {
            task.cancel()
        }
    }
}

/// The session the streamed requests share (its connections reused from
/// one to the next), with the default configuration's cookies and cache,
/// and its delegate, which hands each task's events to its state: the
/// system's, and one per secured transport.
private final class Streams: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    static let shared = Streams(policy: nil)
    /// No timeout: a stream may run as long as it's read.
    static let forever: TimeInterval = 1_000_000_000

    private let lock = NSLock()
    private var states: [Int: StreamState] = [:]
    private(set) var session: URLSession!
    private let policy: TLSPolicy?
    private let failures = TrustFailures()

    init(policy: TLSPolicy?) {
        self.policy = policy
        super.init()
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForResource = Streams.forever
        session = URLSession(configuration: config, delegate: self, delegateQueue: nil)
    }

    func add(_ task: URLSessionTask, _ s: StreamState) {
        lock.lock()
        states[task.taskIdentifier] = s
        lock.unlock()
    }

    private func state(_ task: URLSessionTask, remove: Bool = false) -> StreamState? {
        lock.lock()
        defer { lock.unlock() }
        return remove ? states.removeValue(forKey: task.taskIdentifier) : states[task.taskIdentifier]
    }

    func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse, completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        state(dataTask)?.received(response)
        completionHandler(.allow)
    }

    func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        state(dataTask)?.received(data)
    }

    func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, willCacheResponse proposedResponse: CachedURLResponse, completionHandler: @escaping (CachedURLResponse?) -> Void) {
        completionHandler(nil)  // read once, as it came
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        guard let policy else { return completionHandler(.performDefaultHandling, nil) }
        if let why = policy.answer(challenge, completionHandler) { failures.record(task, why) }
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        state(task, remove: true)?.completed(task, failures.error(task, error))
    }
}

/// A secured transport's session for send: a task's data at its end, its
/// challenges the policy's.
private final class TrustedSession: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    let policy: TLSPolicy
    private let failures = TrustFailures()
    private(set) var session: URLSession!

    init(_ policy: TLSPolicy) {
        self.policy = policy
        super.init()
        session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        if let why = policy.answer(challenge, completionHandler) { failures.record(task, why) }
    }

    /// URLSession.data(for:), on this session: cancelling the task
    /// cancels the request.
    func data(for req: URLRequest) async throws -> (Data, URLResponse) {
        let box = TaskBox()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (k: CheckedContinuation<(Data, URLResponse), Error>) in
                let failures = self.failures
                let task = session.dataTask(with: req) { data, resp, error in
                    if let e = box.ended(error, failures) {
                        k.resume(throwing: e)
                    } else if let resp {
                        k.resume(returning: (data ?? Data(), resp))
                    } else {
                        k.resume(throwing: URLError(.badServerResponse))
                    }
                }
                box.set(task)
                task.resume()
            }
        } onCancel: {
            box.cancel()
        }
    }
}

/// A task a cancel may come for before it's made.
private final class TaskBox: @unchecked Sendable {
    private let lock = NSLock()
    private var task: URLSessionTask?
    private var cancelled = false

    func set(_ t: URLSessionTask) {
        lock.lock()
        task = t
        let c = cancelled
        lock.unlock()
        if c { t.cancel() }
    }

    func cancel() {
        lock.lock()
        cancelled = true
        let t = task
        lock.unlock()
        t?.cancel()
    }

    /// The task's error as it ended (failures' say).
    func ended(_ error: Error?, _ failures: TrustFailures) -> Error? {
        lock.lock()
        let t = task
        lock.unlock()
        guard let t else { return error }
        return failures.error(t, error)
    }
}

/// A streamed request's state, under its lock: the head (and who waits
/// for it), the pieces in that no one has read, the reader waiting, and
/// how it ended.
private final class StreamState: @unchecked Sendable {
    /// Past this much unread, the task is suspended until its reader
    /// catches up.
    static let most = 1 << 20

    let task: URLSessionDataTask
    let url: String
    private let lock = NSLock()
    private var headWaiter: CheckedContinuation<Response, Error>?
    private(set) var head: Response?
    private var pieces: [Data] = []
    private var buffered = 0
    private var suspended = false
    private var reader: (id: UInt64, k: CheckedContinuation<Data, Error>)?
    private var readers: UInt64 = 0
    private var ended = false
    private var failure: Error?
    private var closed = false

    init(task: URLSessionDataTask, url: String) {
        self.task = task
        self.url = url
    }

    func waitHead(_ k: CheckedContinuation<Response, Error>) {
        lock.lock()
        headWaiter = k
        lock.unlock()
        task.resume()
    }

    func received(_ response: URLResponse) {
        let http = response as? HTTPURLResponse
        var out: [String: String] = [:]
        for (k, v) in http?.allHeaderFields ?? [:] {
            out["\(k)".lowercased()] = "\(v)"
        }
        let r = Response(status: http?.statusCode ?? 0, url: response.url?.absoluteString ?? url, headers: out, body: Data())
        lock.lock()
        head = r
        let k = headWaiter
        headWaiter = nil
        lock.unlock()
        k?.resume(returning: r)
    }

    func received(_ data: Data) {
        guard !data.isEmpty else { return }
        lock.lock()
        if let r = reader {
            reader = nil
            lock.unlock()
            r.k.resume(returning: data)
            return
        }
        pieces.append(data)
        buffered += data.count
        if buffered > StreamState.most && !suspended && !closed {
            suspended = true
            task.suspend()
        }
        lock.unlock()
    }

    func completed(_ task: URLSessionTask, _ error: Error?) {
        lock.lock()
        ended = true
        failure = error
        let h = headWaiter
        headWaiter = nil
        let r = reader
        reader = nil
        let gone = closed
        lock.unlock()
        if let h {
            if let error {
                h.resume(throwing: error)
            } else {
                // No head before its end (not HTTP): what the task has.
                h.resume(returning: Response(status: (task.response as? HTTPURLResponse)?.statusCode ?? 0, url: task.response?.url?.absoluteString ?? url, headers: [:], body: Data()))
            }
        }
        if let r {
            if let error, !gone {
                r.k.resume(throwing: error)
            } else {
                r.k.resume(returning: Data())
            }
        }
    }

        private func nextReader() -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        readers += 1
        return readers
    }

    func read() async throws -> Data {
        let id = nextReader()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (k: CheckedContinuation<Data, Error>) in
                lock.lock()
                if Task.isCancelled {
                    lock.unlock()
                    k.resume(throwing: CancellationError())
                    return
                }
                if !pieces.isEmpty {
                    // What's in, in one piece.
                    let d = pieces.count == 1 ? pieces[0] : pieces.reduce(into: Data(capacity: buffered)) { $0.append($1) }
                    pieces = []
                    buffered = 0
                    let wake = suspended && !closed
                    suspended = false
                    lock.unlock()
                    if wake { task.resume() }
                    k.resume(returning: d)
                    return
                }
                if closed || (ended && failure == nil) {
                    lock.unlock()
                    k.resume(returning: Data())
                    return
                }
                if let e = failure {
                    lock.unlock()
                    k.resume(throwing: e)
                    return
                }
                if reader != nil {
                    lock.unlock()
                    k.resume(throwing: URLError(.unknown, userInfo: [NSLocalizedDescriptionKey: "the body is already being read"]))
                    return
                }
                reader = (id, k)
                lock.unlock()
            }
        } onCancel: {
            // The read stops; the request goes on, for the next read.
            lock.lock()
            let r = reader?.id == id ? reader : nil
            if r != nil { reader = nil }
            lock.unlock()
            r?.k.resume(throwing: CancellationError())
        }
    }

    func close() {
        lock.lock()
        if closed {
            lock.unlock()
            return
        }
        closed = true
        pieces = []
        buffered = 0
        let r = reader
        reader = nil
        lock.unlock()
        task.cancel()
        r?.k.resume(returning: Data())
    }
}

/// What open gives Asuka: its state's reader. Let go, it stops the
/// request (the session holds the state until its task ends).
private final class StreamBody: Body {
    let state: StreamState

    init(_ state: StreamState) {
        self.state = state
    }

    deinit {
        state.close()
    }

    func head() throws -> Response {
        state.head ?? Response(url: state.url)
    }

    func read() async throws -> Data {
        try await state.read()
    }

    func close() throws {
        state.close()
    }
}

// ---- LocalServer: Network.framework's listener on the loopback ----

// Its state is the queue's: everything that reads or writes it runs there.
final class LoopbackServers: LocalServer, @unchecked Sendable {
    private let queue = DispatchQueue(label: "asuka.http.loopback")
    private var servers: [Int: Server] = [:]

    private final class Server: @unchecked Sendable {
        let listener: NWListener
        let path: String
        let page: String
        var started = false  // listen has answered
        var arrived: [String] = []
        var waiting: [(id: UUID, k: CheckedContinuation<String, Error>)] = []
        init(listener: NWListener, path: String, page: String) {
            self.listener = listener
            self.path = path
            self.page = page
        }
    }

    func listen(path: String, page: String) async throws -> Int {
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: .any)
        let listener = try NWListener(using: params)
        let server = Server(listener: listener, path: path, page: page)
        listener.newConnectionHandler = { [weak self] c in self?.serve(c, server) }
        return try await withCheckedThrowingContinuation { (k: CheckedContinuation<Int, Error>) in
            listener.stateUpdateHandler = { [weak self] state in
                guard !server.started else { return }
                switch state {
                case .ready:
                    server.started = true
                    let port = Int(listener.port?.rawValue ?? 0)
                    self?.servers[port] = server
                    k.resume(returning: port)
                case .failed(let e):
                    server.started = true
                    k.resume(throwing: e)
                case .cancelled:
                    server.started = true
                    k.resume(throwing: CancellationError())
                default:
                    break
                }
            }
            listener.start(queue: queue)
        }
    }

    // serve reads a request's head and answers it: the page for the
    // server's path (its URL to a waiting next), else a 404.
    private func serve(_ c: NWConnection, _ server: Server) {
        c.start(queue: queue)
        var head = Data()
        func read() {
            c.receive(minimumIncompleteLength: 1, maximumLength: 8192) { [weak self] data, _, done, error in
                if let d = data { head.append(d) }
                let text = String(decoding: head, as: UTF8.self)
                if text.contains("\r\n\r\n") || done || error != nil || head.count >= 8192 {
                    self?.answer(c, server, text)
                } else {
                    read()
                }
            }
        }
        read()
    }

    private func answer(_ c: NWConnection, _ server: Server, _ head: String) {
        let parts = head.split(separator: "\r\n", maxSplits: 1).first?.split(separator: " ") ?? []
        let target = parts.count >= 2 ? String(parts[1]) : ""
        let ours = target == server.path || target.hasPrefix(server.path + "?")
        let body = ours ? Data(server.page.utf8) : Data()
        let status = ours ? "200 OK" : "404 Not Found"
        var out = Data("HTTP/1.1 \(status)\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n".utf8)
        out.append(body)
        c.send(content: out, completion: .contentProcessed { _ in c.cancel() })
        guard ours else { return }
        let url = "http://127.0.0.1:\(server.listener.port?.rawValue ?? 0)\(target)"
        if !server.waiting.isEmpty {
            server.waiting.removeFirst().k.resume(returning: url)
        } else {
            server.arrived.append(url)
        }
    }

    func next(port: Int) async throws -> String {
        let id = UUID()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (k: CheckedContinuation<String, Error>) in
                queue.async {
                    guard let s = self.servers[port] else {
                        k.resume(throwing: URLError(.cannotConnectToHost))
                        return
                    }
                    if !s.arrived.isEmpty {
                        k.resume(returning: s.arrived.removeFirst())
                    } else {
                        s.waiting.append((id, k))
                    }
                }
            }
        } onCancel: {
            queue.async {
                guard let s = self.servers[port], let i = s.waiting.firstIndex(where: { $0.id == id }) else { return }
                s.waiting.remove(at: i).k.resume(throwing: CancellationError())
            }
        }
    }

    func close(port: Int) throws {
        queue.async {
            guard let s = self.servers.removeValue(forKey: port) else { return }
            s.listener.cancel()
            for w in s.waiting { w.k.resume(throwing: CancellationError()) }
            s.waiting = []
        }
    }
}

private let loopbackServers = LoopbackServers()

func makeLocalServer() throws -> any LocalServer { loopbackServers }
