// platform/js on iOS and the Mac: a WKWebView that's never shown, made for
// each script and let go after; callAsyncJavaScript awaits a promise.
// tvOS has no WebKit: JavaScriptCore runs the script there (JSEngine).
import Foundation
#if os(tvOS)
import JavaScriptCore
import Asuka
#else
import WebKit
#endif

#if !os(tvOS)

final class WebEngine: Engine {
    func run(script: String, timeout: TimeInterval) async throws -> String {
        let page = await MainActor.run { ScriptPage(script, timeout: timeout) }
        return try await withTaskCancellationHandler {
            try await page.result()
        } onCancel: {
            Task { @MainActor in page.finish(.failure(CancellationError())) }
        }
    }
}

/// A script's page: a blank one, where the script runs once it's loaded.
@MainActor final class ScriptPage: NSObject, WKNavigationDelegate {
    private static var running = Set<ScriptPage>()
    private let web = WKWebView(frame: .zero, configuration: WKWebViewConfiguration())
    private let script: String
    private var answer: Result<String, Error>?
    private var waiting: CheckedContinuation<String, Error>?

    init(_ script: String, timeout: TimeInterval) {
        self.script = script
        super.init()
        Self.running.insert(self)
        web.navigationDelegate = self
        web.loadHTMLString("<!doctype html><html></html>", baseURL: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + max(timeout, 0)) { [weak self] in
            self?.finish(.failure(JsError.timeout))
        }
    }

    nonisolated func result() async throws -> String {
        try await withCheckedThrowingContinuation { c in
            Task { @MainActor in
                if let a = self.answer { c.resume(with: a) } else { self.waiting = c }
            }
        }
    }

    func webView(_ w: WKWebView, didFinish navigation: WKNavigation!) {
        // Indirect eval: the global scope, and the last statement's value.
        let body = "const v = await (0, eval)(s); return JSON.stringify(v === undefined ? null : v)"
        w.callAsyncJavaScript(body, arguments: ["s": script], in: nil, in: .page) { [weak self] r in
            switch r {
            case .success(let v): self?.finish(.success((v as? String) ?? "null"))
            case .failure(let e):
                let m = (e as NSError).userInfo["WKJavaScriptExceptionMessage"] as? String ?? e.localizedDescription
                self?.finish(.failure(NSError(domain: "js", code: 0, userInfo: [NSLocalizedDescriptionKey: m])))
            }
        }
    }

    func webView(_ w: WKWebView, didFail navigation: WKNavigation!, withError error: Error) { finish(.failure(error)) }

    func finish(_ r: Result<String, Error>) {
        guard answer == nil else { return }
        answer = r
        waiting?.resume(with: r)
        waiting = nil
        web.stopLoading()
        web.navigationDelegate = nil
        Self.running.remove(self)
    }
}

func makeEngine() throws -> any Engine { WebEngine() }
#else
/// JavaScriptCore, as tvOS has it: each script in a fresh context on a
/// thread of its own, whose run loop runs its timers (setTimeout,
/// setInterval and their clears, as a page has) until the script's value,
/// or its promise's, is in. The language and its built-ins are a page's;
/// what a browser adds (the DOM, fetch) isn't there. A script past its
/// timeout is JsError.timeout; one that never yields keeps its thread
/// until it does, as JavaScriptCore can't be stopped from outside.
final class JSEngine: Engine {
    func run(script: String, timeout: TimeInterval) async throws -> String {
        let job = ScriptJob(script)
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { c in job.start(timeout: max(timeout, 0), c) }
        } onCancel: {
            job.finish(.failure(CancellationError()))
        }
    }
}

final class ScriptJob: @unchecked Sendable {
    private let script: String
    private let lock = NSLock()
    private var waiting: CheckedContinuation<String, Error>?
    private var done = false

    init(_ script: String) { self.script = script }

    func start(timeout: TimeInterval, _ c: CheckedContinuation<String, Error>) {
        lock.lock()
        if done {
            lock.unlock()
            c.resume(throwing: CancellationError())
            return
        }
        waiting = c
        lock.unlock()
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { [self] in finish(.failure(JsError.timeout)) }
        let t = Thread { [self] in runHere() }
        t.name = "asuka.js"
        t.start()
    }

    var finished: Bool {
        lock.lock()
        defer { lock.unlock() }
        return done
    }

    func finish(_ r: Result<String, Error>) {
        lock.lock()
        guard !done else { lock.unlock(); return }
        done = true
        let c = waiting
        waiting = nil
        lock.unlock()
        c?.resume(with: r)
    }

    /// On the script's thread: its context, its timers, and the run loop
    /// that fires them until it's finished.
    private func runHere() {
        guard let ctx = JSContext() else {
            finish(.failure(AsukaNativeError.unsupported("js: no JavaScript context")))
            return
        }
        let loop = RunLoop.current
        loop.add(Port(), forMode: .default) // so the loop waits, timers or not
        var timers: [Int: Timer] = [:]
        var nextTimer = 0
        let schedule: (JSValue, Double, Bool, [JSValue]) -> Int = { f, ms, repeats, args in
            nextTimer += 1
            let id = nextTimer
            let t = Timer(timeInterval: max(ms, 0) / 1000, repeats: repeats) { _ in
                if !repeats { timers[id] = nil }
                f.call(withArguments: args)
            }
            timers[id] = t
            loop.add(t, forMode: .default)
            return id
        }
        let setTimeout: @convention(block) () -> Int = {
            let a = JSContext.currentArguments() as? [JSValue] ?? []
            guard let f = a.first, f.isObject else { return 0 }
            return schedule(f, a.count > 1 ? a[1].toDouble() : 0, false, Array(a.dropFirst(2)))
        }
        let setInterval: @convention(block) () -> Int = {
            let a = JSContext.currentArguments() as? [JSValue] ?? []
            guard let f = a.first, f.isObject else { return 0 }
            return schedule(f, a.count > 1 ? a[1].toDouble() : 0, true, Array(a.dropFirst(2)))
        }
        let clear: @convention(block) (JSValue) -> Void = { v in
            let id = Int(v.toInt32())
            timers.removeValue(forKey: id)?.invalidate()
        }
        ctx.setObject(setTimeout, forKeyedSubscript: "setTimeout" as NSString)
        ctx.setObject(setInterval, forKeyedSubscript: "setInterval" as NSString)
        ctx.setObject(clear, forKeyedSubscript: "clearTimeout" as NSString)
        ctx.setObject(clear, forKeyedSubscript: "clearInterval" as NSString)
        ctx.setObject(script, forKeyedSubscript: "__asukaScript" as NSString)

        let resolved: @convention(block) (JSValue) -> Void = { [self] v in
            finish(.success(v.isString ? v.toString() : "null"))
        }
        let rejected: @convention(block) (JSValue) -> Void = { [self] e in
            finish(.failure(NSError(domain: "js", code: 0, userInfo: [NSLocalizedDescriptionKey: Self.message(e)])))
        }
        // Indirect eval: the global scope, and the last statement's value.
        let run = ctx.evaluateScript("""
            (async () => {
              const s = globalThis.__asukaScript
              delete globalThis.__asukaScript
              const v = await (0, eval)(s)
              return JSON.stringify(v === undefined ? null : v)
            })()
            """)
        if let e = ctx.exception {
            finish(.failure(NSError(domain: "js", code: 0, userInfo: [NSLocalizedDescriptionKey: Self.message(e)])))
        } else {
            run?.invokeMethod("then", withArguments: [JSValue(object: resolved, in: ctx) as Any, JSValue(object: rejected, in: ctx) as Any])
        }
        while !finished {
            _ = loop.run(mode: .default, before: Date(timeIntervalSinceNow: 0.1))
        }
        for t in timers.values { t.invalidate() }
    }

    /// What was thrown, as WebKit says it ("Error: boom" for
    /// new Error("boom")).
    static func message(_ e: JSValue) -> String { e.toString() }
}

func makeEngine() throws -> any Engine { JSEngine() }
#endif
