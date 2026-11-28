// platform/device's clipboard beyond text, on a Mac and the iOS simulator
// (platform/macos' TestStdSwiftViews; no view: the pasteboard), on a
// pasteboard of its own, not the user's. How types are named and mapped:
// MIME types as UTIs and back, which are files'. On a Mac copyFiles writes
// file URLs, which clipboardFiles reads back and clipboardHas calls files;
// copyFormats puts an app's own MIME type (as its dynamic UTI) beside HTML
// and text, which clipboardFormats lists as MIME types (none of the Mac's
// legacy aliases) and clipboardData reads back. On iOS copyFormats'
// formats are read back likewise, and copyFiles' file (its content, named)
// comes back from clipboardFiles as a copy.
import Foundation
import UniformTypeIdentifiers
#if os(macOS)
import AppKit
#else
import UIKit
#endif

var failures = 0

func check(_ ok: Bool, _ what: @autoclosure () -> String, line: Int = #line) {
    if !ok {
        print("ClipboardViewTest.swift:\(line): \(what())")
        failures += 1
    }
}

@main struct ClipboardViewTest {
    static func main() async {
        // How types cross: MIME types as UTIs (a dynamic one, made of the
        // MIME type, for one the system doesn't declare), and back.
        check(SystemDevice.boardType("text/plain") == UTType.utf8PlainText.identifier, "text/plain's UTI")
        check(SystemDevice.boardType("com.example.drawing") == "com.example.drawing", "a UTI as it is")
        let own = SystemDevice.boardType("application/x.drawing+json")
        check(own.hasPrefix("dyn.") && SystemDevice.formatName(own) == "application/x.drawing+json", "an app's own type: \(own)")
        check(SystemDevice.formatName(UTType.utf8PlainText.identifier) == "text/plain", "text's name")
        check(SystemDevice.formatName("NSStringPboardType") == nil && SystemDevice.formatName("Apple URL pasteboard type") == nil, "legacy aliases")
        check(SystemDevice.fileish(UTType.pdf.identifier) && SystemDevice.fileish(UTType.png.identifier) && SystemDevice.fileish(UTType.fileURL.identifier),
              "files' types")
        check(!SystemDevice.fileish(UTType.utf8PlainText.identifier) && !SystemDevice.fileish(UTType.html.identifier) && !SystemDevice.fileish(own) &&
              !SystemDevice.fileish(UTType.url.identifier), "not files' types")
        #if os(macOS)
        await mac()
        #elseif os(iOS)
        await ios()
        #endif
        print(failures == 0 ? "ok" : "FAIL")
        exit(failures == 0 ? 0 : 1)
    }

    #if os(macOS)
    static func mac() async {
        let pb = NSPasteboard(name: NSPasteboard.Name("dev.asuka.device-test.\(getpid())"))
        await MainActor.run { SystemDevice.board = pb }
        defer { pb.releaseGlobally() }
        let d = SystemDevice()
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("asuka-clip-\(getpid())", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let a = dir.appendingPathComponent("café one.txt"), b = dir.appendingPathComponent("b.pdf")
        try? Data("x".utf8).write(to: a)
        try? Data("%PDF".utf8).write(to: b)

        // Files: file URLs, read back in order.
        do {
            try await d.copyFiles(paths: [a.path, b.path])
            let k = try await d.clipboardHas()
            check(k.files && !k.html, "kinds after copyFiles: \(k)")
            let got = try await d.clipboardFiles()
            let want = [a, b].map { $0.resolvingSymlinksInPath().path }
            check(got.map { URL(fileURLWithPath: $0).resolvingSymlinksInPath().path } == want, "clipboardFiles: \(got)")
            let formats = try await d.clipboardFormats()
            check(formats.contains("public.file-url") && !formats.contains { $0.contains(" ") || $0.hasSuffix("PboardType") }, "formats of files: \(formats)")
        } catch {
            check(false, "files: \(error)")
        }
        do {
            try await d.copyFiles(paths: [a.path, dir.appendingPathComponent("no such").path])
            check(false, "a missing file copied")
        } catch {
            check("\(error)".contains("no file at"), "a missing file: \(error)")
        }

        // An app's own format, HTML and text.
        let json = Data("{\"shapes\":[1]}".utf8)
        do {
            try await d.copyFormats(formats: [
                ClipboardFormat(type: "application/x.drawing+json", data: json),
                ClipboardFormat(type: "text/html", data: Data("<b>a drawing</b>".utf8)),
                ClipboardFormat(type: "text/plain", data: Data("a drawing".utf8)),
            ])
            let k = try await d.clipboardHas()
            check(k.text && k.html && !k.files, "kinds after copyFormats: \(k)")
            let formats = try await d.clipboardFormats()
            check(formats.contains("application/x.drawing+json") && formats.contains("text/html") && formats.contains("text/plain"), "formats: \(formats)")
            check(Set(formats).count == formats.count && !formats.contains { $0.hasPrefix("dyn.") || $0.contains(" ") }, "formats named once: \(formats)")
            check(try await d.clipboardData(type: "application/x.drawing+json") == json, "the app's own read back")
            check(try await d.clipboardData(type: "text/plain") == Data("a drawing".utf8), "text read back")
            check(try await d.clipboardData(type: "text/html") == Data("<b>a drawing</b>".utf8), "HTML read back")
            check(try await d.clipboardData(type: "application/x.other") == nil, "another type: none")
            check(try await d.clipboardText() == "a drawing", "clipboardText")
            check(try await d.clipboardFiles().isEmpty, "no files")
        } catch {
            check(false, "formats: \(error)")
        }
    }
    #endif

    #if os(iOS)
    static func ios() async {
        let pb = await MainActor.run { () -> UIPasteboard in
            let pb = UIPasteboard.withUniqueName()
            SystemDevice.board = pb
            return pb
        }
        defer { UIPasteboard.remove(withName: pb.name) }
        let d = SystemDevice()
        let json = Data("{\"shapes\":[1]}".utf8)
        do {
            try await d.copyFormats(formats: [
                ClipboardFormat(type: "application/x.drawing+json", data: json),
                ClipboardFormat(type: "text/plain", data: Data("a drawing".utf8)),
            ])
            let formats = try await d.clipboardFormats()
            check(formats.contains("application/x.drawing+json") && formats.contains("text/plain"), "formats: \(formats)")
            check(try await d.clipboardData(type: "application/x.drawing+json") == json, "the app's own read back")
            check(try await d.clipboardData(type: "text/plain") == Data("a drawing".utf8), "text read back")
            let k = try await d.clipboardHas()
            check(k.text && !k.files, "kinds after copyFormats: \(k)")
        } catch {
            check(false, "formats: \(error)")
        }
        // Files: each one's content, named; read back as copies.
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("asuka-clip-\(getpid())", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let a = dir.appendingPathComponent("report.pdf")
        try? Data("%PDF-1.4".utf8).write(to: a)
        do {
            try await d.copyFiles(paths: [a.path])
            let k = try await d.clipboardHas()
            check(k.files, "kinds after copyFiles: \(k)")
            let got = try await d.clipboardFiles()
            check(got.count == 1 && got.first.map { $0.hasSuffix("/report.pdf") } == true, "clipboardFiles: \(got)")
            if let p = got.first { check(FileManager.default.contents(atPath: p) == Data("%PDF-1.4".utf8), "the copy's content") }
        } catch {
            check(false, "files: \(error)")
        }
    }
    #endif
}
