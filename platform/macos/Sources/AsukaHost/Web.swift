// WebView: a WKWebView. The page posts to the app with
// window.asuka.postMessage(text), as on iOS and Android. As on iOS, the
// requests Block names are blocked (a content rule list, compiled before
// the first load so the page's first requests are blocked too), a response
// the page can't show is a Download, and its state (WebState) is reported
// once a turn as it changes. A WKWebView can't see a page's requests
// (Requests).
import AppKit
import WebKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A WebView's view.
final class AsukaWebView: WKWebView {
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.reassertShadow(n) }
    }
}

/// A WebView node's: its view, its content controller and its loads.
@MainActor final class AsukaWeb {
    let view: AsukaWebView
    let ucc: WKUserContentController
    var url = ""
    /// Block's rule lists compiled, to keep only the last.
    var block = 0
    /// The rule list compiling (its block), 0 for none.
    var pending = 0
    /// A load waiting for Block's rule list to compile.
    var held: (() -> Void)?
    /// A WebState report waits for the end of the turn.
    var queued = false
    /// The page's state as it changes.
    var observations: [NSKeyValueObservation] = []
    /// Rule lists applied (the automation's).
    var ruleLists = 0

    init(_ view: AsukaWebView, _ ucc: WKUserContentController) {
        self.view = view
        self.ucc = ucc
    }

    /// Whether Block's rule list is being compiled.
    var compiling: Bool { pending != 0 && pending == block }

    /// Runs a load now, or once Block's rule list has compiled.
    func hold(_ load: @escaping () -> Void) {
        if compiling {
            held = load
            return
        }
        load()
    }

    /// Runs the load held while the rule list compiled.
    func released() {
        pending = 0
        if let f = held {
            held = nil
            f()
        }
    }

    func addScript(_ js: String) {
        ucc.addUserScript(WKUserScript(source: js, injectionTime: .atDocumentStart, forMainFrameOnly: false))
    }
}

/// The page's bridge: window.asuka.postMessage.
let asukaBridgeJS = "window.asuka = { postMessage: function (m) { window.webkit.messageHandlers.asuka.postMessage(String(m)); } };"

/// The WebViews' navigation, UI and script message delegate (the content
/// controllers keep their handler: one for the app).
@MainActor final class AsukaWebDelegate: NSObject, WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler {
    static let shared = AsukaWebDelegate()
    var rt: AsukaRuntime { AsukaRuntime.shared }

    nonisolated func userContentController(_ c: WKUserContentController, didReceive message: WKScriptMessage) {
        MainActor.assumeIsolated {
            guard let v = message.webView, let n = rt.node(of: v) else { return }
            let m = message.body as? String ?? "\(message.body)"
            rt.emit(n, .message) { AsukaEvents.message($0, m) }
        }
    }

    func webView(_ v: WKWebView, didFinish navigation: WKNavigation!) {
        AsukaWebCrash.reloaded.remove(ObjectIdentifier(v))
        if let n = rt.node(of: v) {
            let u = v.url?.absoluteString ?? ""
            rt.emit(n, .navigated) { AsukaEvents.navigated($0, u) }
        }
        rt.webState(v)
    }

    func webView(_ v: WKWebView, didStartProvisionalNavigation navigation: WKNavigation!) { rt.webState(v) }
    func webView(_ v: WKWebView, didFail navigation: WKNavigation!, withError error: Error) { rt.webFailed(v, error) }
    func webView(_ v: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) { rt.webFailed(v, error) }
    func webViewWebContentProcessDidTerminate(_ v: WKWebView) { rt.webTerminated(v) }

    /// The main frame's response that the view can't show, or an
    /// attachment, is cancelled and reported as a Download; the rest load.
    func webView(_ v: WKWebView, decidePolicyFor response: WKNavigationResponse, decisionHandler: @escaping @MainActor (WKNavigationResponsePolicy) -> Void) {
        let r = response.response
        let disposition = (r as? HTTPURLResponse)?.value(forHTTPHeaderField: "Content-Disposition") ?? ""
        if !response.isForMainFrame || response.canShowMIMEType && !disposition.lowercased().hasPrefix("attachment") {
            decisionHandler(.allow)
            return
        }
        decisionHandler(.cancel)
        guard let n = rt.node(of: v) else { return }
        var b = AsukaByteWriter()
        b.string(r.url?.absoluteString ?? "")
        b.string(r.mimeType ?? "")
        b.string(r.suggestedFilename ?? "")
        b.i64(r.expectedContentLength)
        let bytes = b.bytes
        rt.emit(n, .download) { AsukaEvents.download($0, bytes) }
    }

    /// A page for a new window: the app's NewWindow, or this view.
    func webView(_ v: WKWebView, createWebViewWith c: WKWebViewConfiguration, for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        let u = action.request.url?.absoluteString ?? ""
        if let n = rt.node(of: v), n.listening(.newWindow) {
            rt.emit(n, .newWindow) { AsukaEvents.newWindow($0, u) }
        } else {
            v.load(action.request)
        }
        return nil
    }
}

@MainActor extension AsukaRuntime {
    func makeWeb(_ n: AsukaNode) -> NSView {
        let conf = WKWebViewConfiguration()
        let ucc = conf.userContentController
        ucc.add(AsukaWebDelegate.shared, name: "asuka")
        let v = AsukaWebView(frame: .zero, configuration: conf)
        v.navigationDelegate = AsukaWebDelegate.shared
        v.uiDelegate = AsukaWebDelegate.shared
        if #available(macOS 13.3, *) { v.isInspectable = true }
        let w = AsukaWeb(v, ucc)
        w.addScript(asukaBridgeJS)
        n.web = w
        // The page's state the view reports (WebState), observed as it
        // changes.
        let changed: (WKWebView) -> Void = { v in
            DispatchQueue.main.async { MainActor.assumeIsolated { AsukaRuntime.shared.queueWebState(v) } }
        }
        w.observations = [
            v.observe(\.url) { v, _ in changed(v) },
            v.observe(\.title) { v, _ in changed(v) },
            v.observe(\.isLoading) { v, _ in changed(v) },
            v.observe(\.estimatedProgress) { v, _ in changed(v) },
            v.observe(\.canGoBack) { v, _ in changed(v) },
            v.observe(\.canGoForward) { v, _ in changed(v) },
        ]
        return v
    }

    func setWebProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let w = n.web else { return }
        switch p {
        case AsukaProp.WebView.url:
            let u = v.string
            w.url = u
            w.hold { [weak self] in self?.loadWeb(w, u) }
        case AsukaProp.WebView.html:
            let html = v.string
            w.hold { w.view.loadHTMLString(html, baseURL: nil) }
        case AsukaProp.WebView.preload:
            w.ucc.removeAllUserScripts()
            w.addScript(asukaBridgeJS)
            if !v.string.isEmpty { w.addScript(v.string) }
        case AsukaProp.WebView.userAgent:
            w.view.customUserAgent = v.string.isEmpty ? nil : v.string
        case AsukaProp.WebView.block:
            setWebBlock(n, w, v.data)
        case AsukaProp.WebView.requests:
            break // a WKWebView can't see a page's requests
        default:
            break
        }
    }

    func loadWeb(_ w: AsukaWeb, _ s: String) {
        guard !s.isEmpty else { return }
        for p in ["http://", "https://", "file://", "about:", "data:"] where s.hasPrefix(p) {
            if let u = URL(string: s) { w.view.load(URLRequest(url: u)) }
            return
        }
        // An asset: the page, with read access to the assets.
        let file = (s.hasPrefix("/") ? URL(fileURLWithPath: s) : assets.appendingPathComponent(s)).standardizedFileURL
        var dir = file.deletingLastPathComponent()
        var isDir: ObjCBool = false
        let a = assets.standardizedFileURL
        if FileManager.default.fileExists(atPath: a.path, isDirectory: &isDir), isDir.boolValue { dir = a }
        w.view.loadFileURL(file, allowingReadAccessTo: dir)
    }

    /// Eval (the JavaScript) and WebNavigate (u8 0 back, 1 forward, 2
    /// reload, 3 stop).
    func webCommand(_ n: AsukaNode, _ c: AsukaCommand, _ args: [UInt8]) {
        guard let w = n.web else { return }
        switch c {
        case .eval:
            w.view.evaluateJavaScript(String(decoding: args, as: UTF8.self), completionHandler: nil)
        case .webNavigate:
            guard let to = args.first else { return }
            switch to {
            case 0: w.view.goBack()
            case 1: w.view.goForward()
            case 2: w.view.reload()
            case 3: w.view.stopLoading()
            default: break
            }
        default:
            break
        }
    }

    /// Compiles Block's patterns (u32 count, then u32-length strings) into
    /// a content rule list: a host blocks it and its subdomains, a part of
    /// a URL any URL with it.
    func setWebBlock(_ n: AsukaNode, _ w: AsukaWeb, _ b: [UInt8]) {
        w.ucc.removeAllContentRuleLists()
        w.block += 1
        let gen = w.block
        var rules: [[String: Any]] = []
        for p in asukaStrings(b) {
            let esc = asukaQuoteMeta(p)
            let filter = p.contains("/") ? esc : "^[a-z]+://([^/:]+\\.)?" + esc + "[:/]"
            rules.append(["trigger": ["url-filter": filter], "action": ["type": "block"]])
        }
        guard !rules.isEmpty, let data = try? JSONSerialization.data(withJSONObject: rules),
              let js = String(data: data, encoding: .utf8) else {
            w.released()
            return
        }
        w.pending = gen
        WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "asuka-block-\(n.id)", encodedContentRuleList: js) { list, err in
            MainActor.assumeIsolated {
                guard w.block == gen else { return } // another list, or the view went
                if let list {
                    w.ucc.add(list)
                    w.ruleLists += 1
                } else if let err {
                    FileHandle.standardError.write(Data("asuka: a WebView's Block doesn't compile: \(err.localizedDescription)\n".utf8))
                }
                w.released()
            }
        }
    }

    /// Reports the page's state once for the changes of a turn of the
    /// main loop.
    func queueWebState(_ v: WKWebView) {
        guard let n = node(of: v), let w = n.web, !w.queued, n.listening(.webState) else { return }
        w.queued = true
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                let rt = AsukaRuntime.shared
                guard n.web === w, rt.node(of: v) === n else { return }
                w.queued = false
                rt.webState(v)
            }
        }
    }

    /// WebState: string URL, string title, u8 flags (1 loading, 2 can go
    /// back, 4 forward), f32 progress.
    func webState(_ v: WKWebView) {
        guard let n = node(of: v) else { return }
        var b = AsukaByteWriter()
        b.string(v.url?.absoluteString ?? "")
        b.string(v.title ?? "")
        var flags: UInt8 = 0
        if v.isLoading { flags |= 1 }
        if v.canGoBack { flags |= 2 }
        if v.canGoForward { flags |= 4 }
        b.u8(flags)
        b.f32(Float(flags & 1 != 0 ? v.estimatedProgress : 1))
        let bytes = b.bytes
        emit(n, .webState) { AsukaEvents.webState($0, bytes) }
    }

    /// The page's web content process ended (memory, a crash): reported,
    /// and the page loaded again once, so the view isn't left blank; again
    /// before a page loads, only reported.
    func webTerminated(_ v: WKWebView) {
        if let n = node(of: v) {
            emit(n, .failed) { AsukaEvents.failed($0, "the page's web process ended") }
        }
        if !AsukaWebCrash.reloaded.contains(ObjectIdentifier(v)) {
            AsukaWebCrash.reloaded.insert(ObjectIdentifier(v))
            v.reload()
        }
        webState(v)
    }

    func webFailed(_ v: WKWebView, _ err: Error) {
        if let n = node(of: v) {
            let msg = asukaErrorText(err)
            emit(n, .failed) { AsukaEvents.failed($0, msg) }
        }
        webState(v)
    }

    func closeWeb(_ n: AsukaNode, _ w: AsukaWeb) {
        AsukaWebCrash.reloaded.remove(ObjectIdentifier(w.view))
        for o in w.observations { o.invalidate() }
        w.observations = []
        w.block += 1 // a rule list still compiling is dropped
        w.held = nil
        w.ucc.removeScriptMessageHandler(forName: "asuka")
        w.view.navigationDelegate = nil
        w.view.uiDelegate = nil
        w.view.stopLoading()
    }

    func webJSON(_ n: AsukaNode, _ w: AsukaWeb, _ d: inout [String: Any]) {
        d["web"] = ["url": w.view.url?.absoluteString ?? "", "title": w.view.title ?? "", "loading": w.view.isLoading,
                    "compiling": w.compiling, "ruleLists": w.ruleLists, "userAgent": w.view.customUserAgent ?? "",
                    "canGoBack": w.view.canGoBack]
    }
}

/// s with what a regular expression means escaped (Go's regexp.QuoteMeta).
func asukaQuoteMeta(_ s: String) -> String {
    var out = ""
    for c in s {
        if "\\.+*?()|[]{}^$".contains(c) { out.append("\\") }
        out.append(c)
    }
    return out
}

/// The web views reloaded after their page's process ended, until a page
/// loads in them again.
@MainActor enum AsukaWebCrash {
    static var reloaded = Set<ObjectIdentifier>()
}
