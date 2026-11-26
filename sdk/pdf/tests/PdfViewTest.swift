// platform/pdf's view on macOS and the iOS simulator: the reader, given
// its events as closures, shows three.pdf (three pages, "1", "2", "3"),
// goes to the page set without saying it back, says the user's, and says
// why a file it can't show isn't shown. Its first argument is a file in
// the tests directory (where three.pdf is).
import Foundation
import PDFKit
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

@main struct PdfViewTest {
    @MainActor static func main() {
        let dir = URL(fileURLWithPath: CommandLine.arguments[1]).deletingLastPathComponent()
        let three = dir.appendingPathComponent("three.pdf").path

        // Shown: three pages, the first, and nothing said of it.
        var r = Reader()
        r.reader.setSource(three)
        r.wait("loaded") { !r.loaded.isEmpty || !r.errors.isEmpty }
        check(r.loaded == [3], "loaded(3), got \(r.loaded) (errors \(r.errors))")
        check(r.index == 0, "the first page shown, got \(r.index)")
        // The page set: shown, not said back.
        r.reader.setPage(2)
        r.settle()
        check(r.index == 2, "setPage(2) shows the third page, got \(r.index)")
        check(r.pages.isEmpty, "the page set isn't the user's: \(r.pages)")
        // The user's page (the view moved, not by the prop): said, once.
        r.reader.pdf.go(to: r.reader.pdf.document!.page(at: 0)!)
        r.wait("pageChange") { !r.pages.isEmpty }
        check(r.pages == [0], "the user's page said: \(r.pages)")
        // Scrolled (the view's scroll view moved, as the user's finger or
        // wheel does) to the second page: said.
        r.scroll(to: 1)
        r.wait("pageChange") { r.pages.count > 1 }
        check(r.pages == [0, 1] && r.index == 1, "the page scrolled to said: \(r.pages) \(r.index)")
        // Given back by the binding: already there, nothing said.
        r.reader.setPage(1)
        r.settle()
        check(r.index == 1 && r.pages == [0, 1], "the page given back stays: \(r.index) \(r.pages)")
        // Past the last page: the last, and said, so the binding has it.
        r.reader.setPage(9)
        check(r.index == 2 && r.pages == [0, 1, 2], "setPage(9) shows and says the last: \(r.index) \(r.pages)")

        // The last page set in a view taller than what's left of the
        // document: as far as it scrolls, and the page in the middle (the
        // one before it) isn't said back.
        r = Reader(height: 1000)
        r.reader.setSource(three)
        r.wait("loaded") { !r.loaded.isEmpty || !r.errors.isEmpty }
        r.reader.setPage(2)
        r.settle()
        check(r.loaded == [3] && r.pages.isEmpty, "setPage(2) at the end isn't said back: \(r.pages)")

        // A page set before the document's open: shown once it is.
        r = Reader()
        r.reader.setPage(1)
        r.reader.setSource(three)
        r.wait("loaded") { !r.loaded.isEmpty || !r.errors.isEmpty }
        r.settle()
        check(r.loaded == [3] && r.index == 1, "setPage(1) before loading shows the second page: \(r.loaded) \(r.index)")
        check(r.pages.isEmpty, "the page set before isn't the user's: \(r.pages)")

        // What it can't show: why.
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("pdf-view-test-\(getpid())", isDirectory: true)
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tmp) }
        let text = tmp.appendingPathComponent("not.pdf")
        try! Data("not a PDF".utf8).write(to: text)
        let locked = tmp.appendingPathComponent("locked.pdf")
        let doc = PDFDocument(url: URL(fileURLWithPath: three))!
        check(doc.write(to: locked, withOptions: [.userPasswordOption: "secret", .ownerPasswordOption: "owner"]), "an encrypted copy written")
        for (file, want) in [(text.path, "isn't a PDF"), (locked.path, "encrypted"), (tmp.appendingPathComponent("none.pdf").path, "no file at")] {
            r = Reader()
            r.reader.setSource(file)
            r.wait("error") { !r.errors.isEmpty || !r.loaded.isEmpty }
            check(r.loaded.isEmpty && r.errors.count == 1 && r.errors[0].hasPrefix("platform/pdf: ") && r.errors[0].contains(want),
                  "\((file as NSString).lastPathComponent): an error saying \"\(want)\", got \(r.errors) (loaded \(r.loaded))")
        }
        print("ok")
        exit(0)
    }

    static func check(_ ok: Bool, _ what: String) {
        if !ok {
            print("FAIL: " + what)
            exit(1)
        }
    }
}

/// A reader sized as a page, and what it said.
@MainActor final class Reader {
    var loaded: [Int] = []
    var pages: [Int] = []
    var errors: [String] = []
    var reader: PdfReader!

    init(height: CGFloat = 400) {
        reader = PdfReader(loaded: { [unowned self] in loaded.append($0) },
                           pageChange: { [unowned self] in pages.append($0) },
                           error: { [unowned self] in errors.append($0) })
        reader.pdf.frame = CGRect(x: 0, y: 0, width: 300, height: height)
        layout()
    }

    /// The page shown: PDFKit's current page on iOS, the page at the
    /// view's middle on the Mac (as the reader has them).
    var index: Int {
        let pdf = reader.pdf
        #if canImport(UIKit)
        let p = pdf.currentPage
        #else
        let p = pdf.page(for: CGPoint(x: pdf.bounds.midX, y: pdf.bounds.midY), nearest: true)
        #endif
        guard let d = pdf.document, let p else { return -1 }
        return d.index(for: p)
    }

    /// Moves the PDF view's scroll view so page i's top is just above the
    /// view's.
    func scroll(to i: Int) {
        let pdf = reader.pdf
        let r = pdf.convert(pdf.document!.page(at: i)!.bounds(for: pdf.displayBox), from: pdf.document!.page(at: i)!)
        #if canImport(UIKit)
        func find(_ v: UIView) -> UIScrollView? { (v as? UIScrollView) ?? v.subviews.lazy.compactMap(find).first }
        let s = find(pdf)!
        let top = s.convert(r, from: pdf)
        s.setContentOffset(CGPoint(x: s.contentOffset.x, y: top.minY + 20), animated: false)
        #else
        let doc = pdf.documentView!
        let page = doc.convert(r, from: pdf)
        doc.scrollToVisible(NSRect(x: page.minX, y: doc.isFlipped ? page.minY + 20 : page.maxY - 20 - pdf.bounds.height, width: 1, height: pdf.bounds.height))
        #endif
        layout()
    }

    /// Lays the view out and runs what's on the main loop already: a
    /// notification posted for later comes in.
    func settle() {
        layout()
        for _ in 0..<5 { RunLoop.main.run(until: Date()) }
    }

    func layout() {
        #if canImport(UIKit)
        reader.pdf.setNeedsLayout()
        reader.pdf.layoutIfNeeded()
        #else
        reader.pdf.needsLayout = true
        reader.pdf.layoutSubtreeIfNeeded()
        #endif
    }

    /// Runs the main loop until done holds (the loading's on a worker, its
    /// end on the main thread); a minute without it fails.
    func wait(_ what: String, _ done: () -> Bool) {
        let end = Date().addingTimeInterval(60)
        while !done() {
            if Date() > end { PdfViewTest.check(false, "no \(what)") }
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
    }
}
