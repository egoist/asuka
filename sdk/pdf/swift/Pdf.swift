// platform/pdf on iOS and macOS: PDFKit's PDFView, its pages one under the
// other and fitted to the view's width, as Files and Preview show them;
// pinching zooms. The source is resolved as platform/preview's files are
// (a path, an asset's path, an http(s) URL downloaded to the caches), and
// the document is opened off the main thread. The page set scrolls there;
// the page the user scrolls to is reported only when it isn't the one
// the app set or was last told, so a set page never comes back as the
// user's.
import Asuka
import Foundation
import PDFKit
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

@MainActor final class PdfReader: NSObject, View {
    let pdf = PlatformPDFView()
    var view: AsukaPlatformView { pdf }
    private let loaded: (Int) -> Void
    private let pageChange: (Int) -> Void
    private let failed: (String) -> Void
    private var source = ""
    private var load: Task<Void, Never>?
    // The page the app set or was last told: the user's moves are the
    // pages shown that aren't it.
    private var shown = 0
    // A page the app set that isn't shown yet: there's no document, or
    // the view has no size to scroll in.
    private var wanted: Int?
    // Moving the view for the app (a document shown, a page set): not the
    // user's moves.
    private var settling = false
    #if os(macOS)
    // The clip view whose scrolling is watched, and the view's size when
    // it last scrolled: a scroll that comes with a new size is the
    // layout's, not the user's.
    private weak var clip: NSClipView?
    private var size = CGSize.zero
    #endif

    init(loaded: @escaping (Int) -> Void, pageChange: @escaping (Int) -> Void, error: @escaping (String) -> Void) {
        self.loaded = loaded
        self.pageChange = pageChange
        self.failed = error
        super.init()
        pdf.autoScales = true
        pdf.displayMode = .singlePageContinuous
        pdf.displayDirection = .vertical
        pdf.displaysPageBreaks = true
        if #available(iOS 12, macOS 10.14, *) { pdf.pageShadowsEnabled = true }
        pdf.laidOut = { [weak self] in self?.goToWanted() }
        #if canImport(UIKit)
        NotificationCenter.default.addObserver(self, selector: #selector(moved), name: .PDFViewPageChanged, object: pdf)
        #endif
    }

    convenience init(_ events: ViewEvents) {
        self.init(loaded: { events.loaded(pages: $0) }, pageChange: { events.pageChange(page: $0) }, error: { events.error(message: $0) })
    }

    func setSource(_ value: String) {
        guard value != source else { return }
        source = value
        load?.cancel()
        load = nil
        pdf.document = nil
        guard !value.isEmpty else { return }
        load = Task { [weak self] in
            let result: Result<PDFDocument, Error>
            do {
                let url = try await AsukaNative.localFile(value, type: "application/pdf")
                result = .success(try await Self.open(url, named: value))
            } catch {
                result = .failure(error)
            }
            guard let self, !Task.isCancelled, self.source == value else { return }
            self.load = nil
            switch result {
            case .success(let doc): self.show(doc)
            case .failure(let e as PdfError): self.failed(e.message)
            case .failure(let e): self.failed("platform/pdf: " + e.localizedDescription)
            }
        }
    }

    func setPage(_ value: Int) {
        shown = max(value, 0)
        guard pdf.document != nil else {
            wanted = shown
            return
        }
        // The page the user scrolled to, given back: already there (going
        // to it again would move them to its top).
        if pageShown() == shown { return }
        wanted = shown
        if let last = goToWanted() { said(last) }
    }

    /// Opens the file off the main thread: a big document's parsed there.
    nonisolated private static func open(_ url: URL, named source: String) async throws -> PDFDocument {
        let opened = await Task.detached { Opened(PDFDocument(url: url)) }.value
        guard let doc = opened.doc, doc.pageCount > 0 else { throw PdfError("platform/pdf: \(source) isn't a PDF") }
        guard !doc.isLocked else { throw PdfError("platform/pdf: \(source) is encrypted: it needs a password to open") }
        return doc
    }

    private func show(_ doc: PDFDocument) {
        settling = true
        pdf.document = doc
        #if os(macOS)
        watchScrolling()
        #endif
        if wanted == nil && shown > 0 { wanted = shown }
        let last = goToWanted()
        settling = false
        loaded(doc.pageCount)
        if let last { said(last) }
    }

    /// Scrolls to the page the app set, once there's a document and a size
    /// to scroll in. A page past the last goes to the last: it's returned,
    /// for the app to be told.
    @discardableResult private func goToWanted() -> Int? {
        guard let want = wanted, let doc = pdf.document, doc.pageCount > 0, pdf.bounds.width > 0, pdf.bounds.height > 0 else { return nil }
        wanted = nil
        let i = min(want, doc.pageCount - 1)
        guard let page = doc.page(at: i) else { return nil }
        let was = settling
        settling = true
        pdf.go(to: page)
        #if os(macOS)
        size = pdf.bounds.size
        #endif
        settling = was
        return i != want ? i : nil
    }

    /// The view moved: if the user moved it to another page, they're told.
    @objc private func moved(_ n: Notification) {
        guard !settling, wanted == nil, let i = pageShown() else { return }
        said(i)
    }

    private func said(_ i: Int) {
        guard i != shown else { return }
        shown = i
        pageChange(i)
    }

    /// The page shown. On iOS, PDFKit's current page, which follows the
    /// scrolling. On the Mac, PDFKit's doesn't follow a scroll until it
    /// draws, so it's the page at the view's middle (the one filling most
    /// of it).
    private func pageShown() -> Int? {
        guard let doc = pdf.document else { return nil }
        #if canImport(UIKit)
        guard let p = pdf.currentPage else { return nil }
        #else
        guard let p = pdf.page(for: CGPoint(x: pdf.bounds.midX, y: pdf.bounds.midY), nearest: true) ?? pdf.currentPage else { return nil }
        #endif
        return doc.index(for: p)
    }

    #if os(macOS)
    /// Watches the scroll view's clip view: its bounds move as the user
    /// scrolls, pages with the keyboard or zooms, and as the layout follows
    /// a new size. (PDFKit's page changes aren't watched: they come after
    /// a page set, when the page at the middle may be another, the set
    /// one being at the document's end.)
    private func watchScrolling() {
        guard let c = pdf.documentView?.enclosingScrollView?.contentView, c !== clip else { return }
        if let old = clip { NotificationCenter.default.removeObserver(self, name: NSView.boundsDidChangeNotification, object: old) }
        clip = c
        size = pdf.bounds.size
        c.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(self, selector: #selector(scrolled), name: NSView.boundsDidChangeNotification, object: c)
    }

    @objc private func scrolled(_ n: Notification) {
        guard pdf.bounds.size == size else {
            size = pdf.bounds.size
            return
        }
        moved(n)
    }
    #endif
}

#if os(macOS)
extension PdfReader: AsukaDestroying {
    func destroy() {
        load?.cancel()
        load = nil
        NotificationCenter.default.removeObserver(self)
        pdf.document = nil
    }
}
#endif

/// A failure with the message the app is given.
struct PdfError: Error {
    let message: String
    init(_ message: String) { self.message = message }
}

/// A document opened on a worker, handed to the main thread (PDFDocument
/// is safe to make on one and show on another).
private final class Opened: @unchecked Sendable {
    let doc: PDFDocument?
    init(_ doc: PDFDocument?) { self.doc = doc }
}

/// The PDF view: it has no size of its own (its style gives it one, or
/// grow), and says when it's laid out, which scrolls to a page set before
/// it had a size.
final class PlatformPDFView: PDFView {
    var laidOut: (@MainActor () -> Void)?
    #if canImport(UIKit)
    override func sizeThatFits(_ size: CGSize) -> CGSize { .zero }
    override func layoutSubviews() {
        super.layoutSubviews()
        laidOut?()
    }
    #else
    override var fittingSize: NSSize { .zero }
    override func layout() {
        super.layout()
        laidOut?()
    }
    #endif
}

@MainActor func makeView(_ events: ViewEvents) -> any View { PdfReader(events) }
