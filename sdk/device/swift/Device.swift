// platform/device on iOS and the Mac: UIDevice and the bundle, the in-app
// browser (SFSafariViewController; the Mac's browser), PHPicker and the
// document picker (NSOpenPanel on the Mac), the pasteboard. What
// pickMedia copies is read with ImageIO (an image's size upright, its
// scaling) and AVFoundation (a video's size as it plays, its length);
// videos are copied as they are, never transcoded. Apple TV has
// no browser, pickers or clipboard: those say unsupported, or nothing's
// there. Nor has Apple Watch (WatchKit's device for the rest), whose
// screen sleeps as the wrist comes down: keepAwake does nothing there.
import Foundation
import UniformTypeIdentifiers
import Asuka
#if !os(tvOS) && !os(watchOS)
import AVFoundation
import ImageIO
#endif
#if os(tvOS)
import UIKit
#elseif os(watchOS)
import WatchKit
#elseif canImport(UIKit)
import UIKit
import SafariServices
import PhotosUI
#else
import AppKit
#endif

final class SystemDevice: Platform {
    func info() async throws -> NativeInfo {
        let b = Bundle.main.infoDictionary ?? [:]
        let app = b["CFBundleShortVersionString"] as? String ?? ""
        let build = b["CFBundleVersion"] as? String ?? ""
        let (source, installed, updated) = Self.install()
        #if os(watchOS)
        return await MainActor.run {
            let d = WKInterfaceDevice.current()
            return NativeInfo(model: d.model, system: "watchOS", systemVersion: d.systemVersion, appVersion: app, build: build,
                              source: source, installedMs: installed, updatedMs: updated)
        }
        #elseif canImport(UIKit)
        return await MainActor.run {
            NativeInfo(model: UIDevice.current.model, system: Self.systemName, systemVersion: UIDevice.current.systemVersion, appVersion: app, build: build,
                       source: source, installedMs: installed, updatedMs: updated)
        }
        #else
        let v = ProcessInfo.processInfo.operatingSystemVersion
        let version = v.patchVersion > 0 ? "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)" : "\(v.majorVersion).\(v.minorVersion)"
        return NativeInfo(model: "Mac", system: "macOS", systemVersion: version, appVersion: app, build: build,
                          source: source, installedMs: installed, updatedMs: updated)
        #endif
    }

    #if canImport(UIKit) && !os(watchOS)
    /// The system's name: "iOS" (iPadOS's too), "tvOS".
    @MainActor static var systemName: String {
        #if os(tvOS)
        return "tvOS"
        #else
        return UIDevice.current.systemName
        #endif
    }
    #endif

    /// Where the app came from, and when it was installed and updated (ms
    /// since 1970; 0: unknown). The store's receipt says a store's copy:
    /// sandboxReceipt is TestFlight's. A simulator's, a provisioning
    /// profile's (a developer's, an ad hoc copy) and asuka dev's player
    /// are a developer's; a Mac app without a receipt was downloaded. The
    /// app's data container was made as it was installed; its bundle, as
    /// this version was.
    static func install() -> (InstallSource, Int, Int) {
        let fm = FileManager.default
        func ms(_ url: URL?, _ key: FileAttributeKey) -> Int {
            guard let p = url?.path, let d = (try? fm.attributesOfItem(atPath: p))?[key] as? Date else { return 0 }
            return Int(d.timeIntervalSince1970 * 1000)
        }
        let receipt = Bundle.main.appStoreReceiptURL
        let hasReceipt = receipt.map { fm.fileExists(atPath: $0.path) } ?? false
        var source = InstallSource.direct
        #if targetEnvironment(simulator)
        source = .development
        #else
        if ProcessInfo.processInfo.environment["ASUKA_DEV"] != nil {
            source = .development
        } else if receipt?.lastPathComponent == "sandboxReceipt" {
            // TestFlight's, or a developer's build run from Xcode's.
            source = Bundle.main.path(forResource: "embedded", ofType: "mobileprovision") == nil ? .testFlight : .development
        } else if Bundle.main.path(forResource: "embedded", ofType: "mobileprovision") != nil {
            source = .development
        } else if hasReceipt {
            source = .appStore
        } else {
            #if canImport(UIKit)
            source = .appStore // a store's copy whose receipt is yet to come
            #endif
        }
        #endif
        #if canImport(UIKit)
        let home = fm.urls(for: .documentDirectory, in: .userDomainMask).first?.deletingLastPathComponent()
        #else
        let home = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first.map { $0.appendingPathComponent(Bundle.main.bundleIdentifier ?? "") }
        #endif
        let updated = ms(Bundle.main.bundleURL, .modificationDate)
        var installed = ms(home, .creationDate)
        if installed == 0 || (updated > 0 && installed > updated) { installed = updated }
        return (source, installed, updated)
    }

    func browse(url: String) async throws {
        guard let u = URL(string: url), u.scheme == "https" || u.scheme == "http" else {
            throw Self.error("device: not a web URL: \(url)")
        }
        #if os(tvOS)
        throw AsukaNativeError.unsupported("device: tvOS has no web browser")
        #elseif os(watchOS)
        throw AsukaNativeError.unsupported("device: watchOS has no web browser")
        #elseif canImport(UIKit)
        try await MainActor.run {
            guard let top = AsukaNative.presenter else { throw Self.error("device: no screen to show it on") }
            top.present(SFSafariViewController(url: u), animated: true)
        }
        #else
        let ok = await MainActor.run { NSWorkspace.shared.open(u) }
        if !ok { throw Self.error("device: couldn't open \(url)") }
        #endif
    }

    func pickMedia(kind: MediaKind, max: Int, ordered: Bool, maxSize: Int, quality: Double) async throws -> [NativeMedia] {
        #if os(tvOS)
        throw AsukaNativeError.unsupported("device: tvOS has no photo picker")
        #elseif os(watchOS)
        throw AsukaNativeError.unsupported("device: watchOS has no photo picker")
        #elseif canImport(UIKit)
        let results: [PHPickerResult] = try await MainActor.run {
            guard let top = AsukaNative.presenter else { throw Self.error("device: no screen to show it on") }
            return top
        }.pickMedia(kind, max, ordered)
        return await Self.copy(results, maxSize: maxSize, quality: quality)
        #else
        var types: [UTType] = []
        if kind != .videos { types.append(.image) }
        if kind != .images { types += Self.videoTypes }
        let urls: [URL] = await MainActor.run { () -> NSOpenPanel in
            let p = NSOpenPanel()
            p.canChooseFiles = true
            p.canChooseDirectories = false
            p.allowsMultipleSelection = max != 1
            p.allowedContentTypes = types
            return p
        }.choose()
        var out: [NativeMedia] = []
        for u in max > 0 ? Array(urls.prefix(max)) : urls {
            let video = !(UTType(filenameExtension: u.pathExtension)?.conforms(to: .image) ?? false)
            if let m = await Self.media(u, name: u.lastPathComponent, video: video, maxSize: maxSize, quality: quality) { out.append(m) }
        }
        return out
        #endif
    }

    func pickFiles(types: [String], multiple: Bool) async throws -> [File] {
        #if os(tvOS)
        throw AsukaNativeError.unsupported("device: tvOS has no document picker")
        #elseif os(watchOS)
        throw AsukaNativeError.unsupported("device: watchOS has no document picker")
        #else
        var uts = types.compactMap(Self.utType)
        if uts.isEmpty || uts.contains(.item) { uts = [.item] }
        #if canImport(UIKit)
        let urls: [URL] = try await MainActor.run {
            guard let top = AsukaNative.presenter else { throw Self.error("device: no screen to show it on") }
            return top
        }.pickDocuments(uts, multiple)
        return Self.keep(urls, move: true)
        #else
        return try await Self.open(types: uts, multiple: multiple, limit: 0)
        #endif
        #endif
    }

    func clipboardText() async throws -> String? {
        #if os(tvOS) || os(watchOS)
        return nil // tvOS and watchOS have no clipboard
        #else
        return await MainActor.run {
            #if canImport(UIKit)
            SystemDevice.board.string
            #else
            SystemDevice.board.string(forType: .string)
            #endif
        }
        #endif
    }

    func clipboardHas() async throws -> ClipboardKinds { await MainActor.run { Self.kinds() } }

    @MainActor static func kinds() -> ClipboardKinds {
        #if os(tvOS) || os(watchOS)
        return ClipboardKinds(text: false, image: false, url: false, html: false)
        #elseif canImport(UIKit)
        let pb = SystemDevice.board
        // contains(pasteboardTypes:) reads the types, not the data: no prompt.
        return ClipboardKinds(text: pb.hasStrings, image: pb.hasImages, url: pb.hasURLs,
                              html: pb.contains(pasteboardTypes: richTypes), files: pb.types.contains { fileish($0) })
        #else
        let pb = SystemDevice.board
        let types = pb.types ?? []
        let image = types.contains { UTType($0.rawValue)?.conforms(to: .image) ?? false }
        let url = types.contains(.URL) || types.contains(.fileURL)
        let html = types.contains { richTypes.contains($0.rawValue) }
        return ClipboardKinds(text: types.contains(.string), image: image, url: url, html: html, files: types.contains(.fileURL))
        #endif
    }

    #if !os(tvOS) && !os(watchOS)
    /// The pasteboard the clipboard is: the general one (a test's own).
    #if canImport(UIKit)
    @MainActor static var board = UIPasteboard.general
    #else
    @MainActor static var board = NSPasteboard.general
    #endif
    #endif

    /// Whether a pasteboard type is a file's (iOS, which has no files on
    /// its pasteboard as such: what the Files app copies is each file's
    /// content, named): a file URL, or content that's neither text nor a
    /// link nor rich text's (an image, a PDF, a movie, an archive), of a
    /// type the system declares, not the system's own (UIKit's attributed
    /// strings, Notes' rich text).
    static func fileish(_ id: String) -> Bool {
        guard let t = UTType(id), !t.isDynamic else { return false }
        if t.conforms(to: .fileURL) { return true }
        guard t.conforms(to: .data), !t.conforms(to: .text), !t.conforms(to: .url) else { return false }
        if [UTType.rtfd, .flatRTFD, .webArchive].contains(where: { t.conforms(to: $0) }) { return false }
        return !id.hasPrefix("com.apple.")
    }

    /// A clipboard format's pasteboard type: a MIME type's UTI (a dynamic
    /// one, made of the MIME type, for one the system doesn't declare:
    /// another copy of the app makes the same), text/plain as UTF-8's; a
    /// UTI as it is.
    static func boardType(_ type: String) -> String {
        guard type.contains("/") else { return type }
        if type == "text/plain" { return UTType.utf8PlainText.identifier }
        return UTType(mimeType: type)?.identifier ?? type
    }

    /// A pasteboard type as clipboardFormats names it: its MIME type (less
    /// its parameters) where it has one, else the UTI; none for the Mac's
    /// legacy aliases of the types beside them ("NSStringPboardType",
    /// "Apple URL pasteboard type", a dynamic type without a MIME type).
    static func formatName(_ id: String) -> String? {
        if let m = UTType(id)?.preferredMIMEType {
            return String(m.split(separator: ";")[0]).trimmingCharacters(in: .whitespaces)
        }
        if id.contains(" ") || id.hasSuffix("PboardType") || id.hasPrefix("dyn.") { return nil }
        return id
    }

    func clipboardFormats() async throws -> [String] {
        #if os(tvOS) || os(watchOS)
        return []
        #else
        return await MainActor.run {
            #if canImport(UIKit)
            let types = Self.board.types
            #else
            let types = (Self.board.types ?? []).map { $0.rawValue }
            #endif
            var seen = Set<String>()
            return types.compactMap { Self.formatName($0) }.filter { seen.insert($0).inserted }
        }
        #endif
    }

    func clipboardData(type: String) async throws -> Data? {
        #if os(tvOS) || os(watchOS)
        return nil
        #else
        let t = Self.boardType(type)
        return await MainActor.run {
            #if canImport(UIKit)
            let pb = Self.board
            guard pb.contains(pasteboardTypes: [t]) else { return nil }
            if t == UTType.utf8PlainText.identifier, let s = pb.string { return Data(s.utf8) }
            if let d = pb.data(forPasteboardType: t) { return d }
            return (pb.value(forPasteboardType: t) as? String).map { Data($0.utf8) }
            #else
            let pb = Self.board
            if t == UTType.utf8PlainText.identifier { return pb.string(forType: .string).map { Data($0.utf8) } }
            return pb.data(forType: NSPasteboard.PasteboardType(t))
            #endif
        }
        #endif
    }

    func copyFormats(formats: [ClipboardFormat]) async throws {
        #if os(tvOS) || os(watchOS)
        throw AsukaNativeError.unsupported("device: this system has no clipboard")
        #else
        await MainActor.run {
            let types = formats.map { Self.boardType($0.type) }
            #if canImport(UIKit)
            // One item, each format a representation of it; text as a
            // string, which every app's paste reads.
            var item: [String: Any] = [:]
            for (f, t) in zip(formats, types) {
                item[t] = t == UTType.utf8PlainText.identifier ? String(decoding: f.data, as: UTF8.self) : f.data
            }
            Self.board.setItems([item])
            #else
            let pb = Self.board
            pb.clearContents()
            pb.declareTypes(types.map { NSPasteboard.PasteboardType($0) }, owner: nil)
            for (f, t) in zip(formats, types) { pb.setData(f.data, forType: NSPasteboard.PasteboardType(t)) }
            #endif
        }
        #endif
    }

    func clipboardFiles() async throws -> [String] {
        #if os(tvOS) || os(watchOS)
        return []
        #elseif canImport(UIKit)
        // Each item that's a file (a file URL the app may read, or a
        // file's content, named), copied into the caches.
        let providers: [NSItemProvider] = await MainActor.run {
            Self.board.types.contains { Self.fileish($0) } ? Self.board.itemProviders : []
        }
        var out: [String] = []
        for p in providers {
            if let path = await Self.keepFile(p) { out.append(path) }
        }
        return out
        #else
        return await MainActor.run {
            let urls = Self.board.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
            return urls.map { $0.path }
        }
        #endif
    }

    func copyFiles(paths: [String]) async throws {
        #if os(tvOS) || os(watchOS)
        throw AsukaNativeError.unsupported("device: this system has no clipboard")
        #else
        let urls = paths.map { URL(fileURLWithPath: $0) }
        for u in urls where !FileManager.default.fileExists(atPath: u.path) { throw Self.error("device: no file at \(u.path)") }
        try await MainActor.run {
            #if canImport(UIKit)
            // Each file's content, of its type, with its name: what the
            // Files app copies, and pastes.
            let providers = try urls.map { u -> NSItemProvider in
                guard let p = NSItemProvider(contentsOf: u) else { throw Self.error("device: can't read \(u.path)") }
                p.suggestedName = u.lastPathComponent
                return p
            }
            Self.board.setItemProviders(providers, localOnly: false, expirationDate: nil)
            #else
            let pb = Self.board
            pb.clearContents()
            if !pb.writeObjects(urls as [NSURL]) { throw Self.error("device: the clipboard took no files") }
            #endif
        }
        #endif
    }

    #if canImport(UIKit) && !os(tvOS) && !os(watchOS)
    /// An item provider's file, copied into the caches' asuka-clipboard, in
    /// a folder of its own (its name stays its own): a file URL's file, or
    /// its first file type's content, named as it says (else "Pasted").
    static func keepFile(_ p: NSItemProvider) async -> String? {
        let dir: URL
        do { dir = try cacheDir("asuka-clipboard").appendingPathComponent(UUID().uuidString, isDirectory: true) } catch { return nil }
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        if p.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
            let url: URL? = await withCheckedContinuation { c in
                _ = p.loadObject(ofClass: URL.self) { u, _ in c.resume(returning: u) }
            }
            if let u = url, u.isFileURL {
                let scoped = u.startAccessingSecurityScopedResource()
                defer { if scoped { u.stopAccessingSecurityScopedResource() } }
                let to = dir.appendingPathComponent(u.lastPathComponent)
                if (try? FileManager.default.copyItem(at: u, to: to)) != nil { return to.path }
            }
        }
        guard let id = p.registeredTypeIdentifiers.first(where: { fileish($0) && $0 != UTType.fileURL.identifier }) else { return nil }
        let ext = UTType(id)?.preferredFilenameExtension ?? ""
        var name = (p.suggestedName ?? "").replacingOccurrences(of: "/", with: "_")
        if name.isEmpty { name = "Pasted" }
        if !ext.isEmpty && (name as NSString).pathExtension.isEmpty { name += "." + ext }
        let to = dir.appendingPathComponent(name)
        return await withCheckedContinuation { c in
            _ = p.loadFileRepresentation(forTypeIdentifier: id) { u, _ in
                // The file is the provider's only until this returns: copied now.
                guard let u, (try? FileManager.default.copyItem(at: u, to: to)) != nil else { c.resume(returning: nil); return }
                c.resume(returning: to.path)
            }
        }
    }
    #endif

    /// The types rich text arrives in, the one read first first: HTML (a
    /// browser's, a mail's), a web archive (Safari's: its HTML inside),
    /// RTF and flat RTFD (Notes', Pages', TextEdit's).
    static let richTypes = [UTType.html.identifier, webArchive, UTType.rtf.identifier, UTType.flatRTFD.identifier]
    static let webArchive = "Apple Web Archive pasteboard type"

    func clipboardHTML() async throws -> String? {
        #if os(tvOS) || os(watchOS)
        return nil
        #else
        return await MainActor.run {
            #if canImport(UIKit)
            let pb = SystemDevice.board
            func data(_ t: String) -> Data? {
                if let d = pb.data(forPasteboardType: t) { return d }
                return (pb.value(forPasteboardType: t) as? String).map { Data($0.utf8) }
            }
            #else
            let pb = SystemDevice.board
            func data(_ t: String) -> Data? { pb.data(forType: NSPasteboard.PasteboardType(t)) }
            #endif
            for t in Self.richTypes {
                guard let d = data(t), let html = Self.html(d, type: t) else { continue }
                return html
            }
            return nil
        }
        #endif
    }

    #if !os(watchOS)
    /// A rich type's data as HTML: HTML as it is (UTF-8; UTF-16 with its
    /// mark; else Windows' Latin 1), a web archive's main page, RTF and
    /// RTFD converted.
    @MainActor static func html(_ d: Data, type: String) -> String? {
        switch type {
        case UTType.html.identifier:
            if d.starts(with: [0xFF, 0xFE]) || d.starts(with: [0xFE, 0xFF]) { return String(data: d, encoding: .utf16) }
            return String(data: d, encoding: .utf8) ?? String(data: d, encoding: .windowsCP1252)
        case webArchive:
            guard let plist = try? PropertyListSerialization.propertyList(from: d, format: nil) as? [String: Any],
                  let main = plist["WebMainResource"] as? [String: Any],
                  let page = main["WebResourceData"] as? Data else { return nil }
            if let name = main["WebResourceTextEncodingName"] as? String {
                let cf = CFStringConvertIANACharSetNameToEncoding(name as CFString)
                if cf != kCFStringEncodingInvalidId,
                   let s = String(data: page, encoding: String.Encoding(rawValue: CFStringConvertEncodingToNSStringEncoding(cf))) { return s }
            }
            return String(data: page, encoding: .utf8)
        default:
            let kind: NSAttributedString.DocumentType = type == UTType.rtf.identifier ? .rtf : .rtfd
            guard let a = try? NSAttributedString(data: d, options: [.documentType: kind], documentAttributes: nil),
                  let out = try? a.data(from: NSRange(location: 0, length: a.length), documentAttributes: [.documentType: NSAttributedString.DocumentType.html]) else { return nil }
            return String(data: out, encoding: .utf8)
        }
    }
    #endif

    func copyHTML(html: String, text: String) async throws {
        #if os(tvOS) || os(watchOS)
        throw AsukaNativeError.unsupported("device: this system has no clipboard")
        #else
        await MainActor.run {
            #if canImport(UIKit)
            SystemDevice.board.setItems([[UTType.html.identifier: Data(html.utf8), UTType.utf8PlainText.identifier: text]])
            #else
            let pb = SystemDevice.board
            pb.clearContents()
            pb.declareTypes([.html, .string], owner: nil)
            pb.setString(html, forType: .html)
            pb.setString(text, forType: .string)
            #endif
        }
        #endif
    }

    func clipboardImage() async throws -> String? {
        #if os(tvOS) || os(watchOS)
        return nil
        #else
        // The image as it was copied (its bytes, not a re-encoding), in the
        // first image type the clipboard has.
        let found: (Data, String)? = await MainActor.run {
            #if canImport(UIKit)
            let pb = SystemDevice.board
            for t in pb.types.compactMap({ UTType($0) }) where t.conforms(to: .image) {
                if let d = pb.data(forPasteboardType: t.identifier) { return (d, t.preferredFilenameExtension ?? "png") }
            }
            if let img = pb.image, let d = img.pngData() { return (d, "png") }
            return nil
            #else
            let pb = SystemDevice.board
            for t in pb.types ?? [] {
                guard let u = UTType(t.rawValue), u.conforms(to: .image), let d = pb.data(forType: t) else { continue }
                if u == .tiff, let rep = NSBitmapImageRep(data: d), let png = rep.representation(using: .png, properties: [:]) {
                    return (png, "png") // a screenshot's TIFF, as PNG
                }
                return (d, u.preferredFilenameExtension ?? "png")
            }
            return nil
            #endif
        }
        guard let (data, ext) = found else { return nil }
        let f = try Self.cacheDir("asuka-clipboard").appendingPathComponent(UUID().uuidString + "." + ext)
        try data.write(to: f)
        return f.path
        #endif
    }

    func copyImage(path: String) async throws {
        #if os(tvOS) || os(watchOS)
        throw AsukaNativeError.unsupported("device: this system has no clipboard")
        #else
        let url = URL(fileURLWithPath: path)
        guard let data = try? Data(contentsOf: url) else { throw Self.error("device: no file at \(path)") }
        try await MainActor.run {
            #if canImport(UIKit)
            let pb = SystemDevice.board
            if let t = UTType(filenameExtension: url.pathExtension), t.conforms(to: .image) {
                pb.setData(data, forPasteboardType: t.identifier)
            } else if let img = UIImage(data: data) {
                pb.image = img
            } else {
                throw Self.error("device: not an image: \(path)")
            }
            #else
            let pb = SystemDevice.board
            pb.clearContents()
            if let t = UTType(filenameExtension: url.pathExtension), t.conforms(to: .image) {
                pb.setData(data, forType: NSPasteboard.PasteboardType(t.identifier))
                // And as TIFF, which every Mac app pastes.
                if t != .tiff, let img = NSImage(data: data), let tiff = img.tiffRepresentation { pb.setData(tiff, forType: .tiff) }
            } else if let img = NSImage(data: data) {
                pb.writeObjects([img])
            } else {
                throw Self.error("device: not an image: \(path)")
            }
            #endif
        }
        #endif
    }

    func watchClipboard(changed: @escaping (ClipboardKinds) -> Void) async throws {
        #if os(tvOS) || os(watchOS)
        return // nothing changes: no clipboard
        #else
        let w = await MainActor.run { ClipboardWatch(changed) }
        defer { Task { @MainActor in w.stop() } }
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
        #endif
    }

    @MainActor func keepAwake(on: Bool) throws {
        #if os(watchOS)
        // The watch's screen sleeps as the wrist comes down, whatever an app says.
        throw AsukaNativeError.unsupported("device: watchOS keeps no screen on")
        #elseif canImport(UIKit)
        UIApplication.shared.isIdleTimerDisabled = on
        #else
        if on, Self.awake == nil {
            Self.awake = ProcessInfo.processInfo.beginActivity(options: [.idleDisplaySleepDisabled, .userInitiated], reason: "Keeping the screen on")
        } else if !on, let a = Self.awake {
            ProcessInfo.processInfo.endActivity(a)
            Self.awake = nil
        }
        #endif
    }

    @MainActor func openSettings() throws -> Bool {
        #if os(watchOS)
        return false // the watch's Settings have no page for an app
        #elseif canImport(UIKit)
        // The app's own page: its permissions, notifications and language.
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return false }
        UIApplication.shared.open(url)
        return true
        #else
        // A Mac has no page for an app: Privacy & Security, its list of what
        // apps may use.
        guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy") else { return false }
        return NSWorkspace.shared.open(url)
        #endif
    }

    #if os(macOS)
    @MainActor static var awake: NSObjectProtocol?

    /// The open panel, as a sheet on the window (else a panel of its own):
    /// copies of what's chosen.
    static func open(types: [UTType], multiple: Bool, limit: Int) async throws -> [File] {
        let urls: [URL] = await MainActor.run { () -> NSOpenPanel in
            let p = NSOpenPanel()
            p.canChooseFiles = true
            p.canChooseDirectories = false
            p.allowsMultipleSelection = multiple
            if types != [.item] { p.allowedContentTypes = types }
            return p
        }.choose()
        return keep(limit > 0 ? Array(urls.prefix(limit)) : urls, move: false)
    }
    #endif

    /// Keeps the picked files where the app keeps them, each in a folder
    /// of its own (so its name stays its own): moved (the picker's copies)
    /// or copied (the user's own files).
    static func keep(_ urls: [URL], move: Bool) -> [File] {
        let fm = FileManager.default
        guard let dir = try? cacheDir("asuka-files") else { return [] }
        var out: [File] = []
        for u in urls {
            let into = dir.appendingPathComponent(UUID().uuidString, isDirectory: true)
            let to = into.appendingPathComponent(u.lastPathComponent)
            do {
                try fm.createDirectory(at: into, withIntermediateDirectories: true)
                if move { try fm.moveItem(at: u, to: to) } else { try fm.copyItem(at: u, to: to) }
            } catch { continue }
            let size = (try? fm.attributesOfItem(atPath: to.path)[.size] as? NSNumber)?.intValue ?? 0
            let type = UTType(filenameExtension: to.pathExtension)?.preferredMIMEType ?? ""
            out.append(File(path: to.path, name: u.lastPathComponent, type: type, size: size))
        }
        return out
    }

    #if !os(tvOS) && !os(watchOS)
    /// The videos the Mac's picker offers: what's a movie, and the files
    /// of the formats that may not say they are (WebM, Matroska).
    static let videoTypes: [UTType] = [.movie, .mpeg4Movie, .quickTimeMovie, .avi]
        + ["m4v", "webm", "mkv"].compactMap { UTType(filenameExtension: $0) }

    /// A picked photo or video as pickMedia gives it: copied into the
    /// caches' asuka-photos (an image decoded, turned upright, scaled to
    /// maxSize and written as a JPEG of quality when either isn't 0), with
    /// its size in pixels (upright; a video's track's, turned as it plays),
    /// its length and its bytes.
    static func media(_ url: URL, name: String, video: Bool, maxSize: Int, quality: Double) async -> NativeMedia? {
        guard let dir = try? cacheDir("asuka-photos") else { return nil }
        let ext = url.pathExtension.lowercased()
        var m = NativeMedia(name: name, video: video)
        if !video, maxSize > 0 || quality > 0 {
            let to = dir.appendingPathComponent(UUID().uuidString + ".jpg")
            guard let (w, h) = writeJPEG(url, to, maxSize: maxSize, quality: quality) else { return nil }
            m.path = to.path
            m.type = "image/jpeg"
            (m.width, m.height) = (w, h)
            if !name.isEmpty { m.name = (name as NSString).deletingPathExtension + ".jpg" }
        } else {
            let to = dir.appendingPathComponent(UUID().uuidString + "." + (ext.isEmpty ? (video ? "mov" : "jpg") : ext))
            guard (try? FileManager.default.copyItem(at: url, to: to)) != nil else { return nil }
            m.path = to.path
            m.type = UTType(filenameExtension: ext)?.preferredMIMEType ?? (video ? "video/quicktime" : "")
            if video {
                (m.width, m.height, m.durationMs) = await videoInfo(to)
            } else {
                (m.width, m.height) = imageSize(to)
            }
        }
        m.size = (try? FileManager.default.attributesOfItem(atPath: m.path)[.size] as? NSNumber)?.intValue ?? 0
        return m
    }

    /// An image's size in pixels, upright (its EXIF orientation applied).
    static func imageSize(_ url: URL) -> (Int, Int) {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let p = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any] else { return (0, 0) }
        let w = p[kCGImagePropertyPixelWidth] as? Int ?? 0, h = p[kCGImagePropertyPixelHeight] as? Int ?? 0
        let o = p[kCGImagePropertyOrientation] as? Int ?? 1
        return o >= 5 && o <= 8 ? (h, w) : (w, h)
    }

    /// Decodes an image, turned upright and scaled down to maxSize on its
    /// long side (0: as it is; never up), and writes it as a JPEG of
    /// quality (0: 0.9) with its metadata but its orientation: its size.
    static func writeJPEG(_ from: URL, _ to: URL, maxSize: Int, quality: Double) -> (Int, Int)? {
        guard let src = CGImageSourceCreateWithURL(from as CFURL, nil) else { return nil }
        var meta = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any] ?? [:]
        let long = max(meta[kCGImagePropertyPixelWidth] as? Int ?? 0, meta[kCGImagePropertyPixelHeight] as? Int ?? 0)
        guard long > 0, let img = CGImageSourceCreateThumbnailAtIndex(src, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxSize > 0 ? min(maxSize, long) : long,
        ] as CFDictionary) else { return nil }
        for k in [kCGImagePropertyPixelWidth, kCGImagePropertyPixelHeight] { meta[k] = nil }
        meta[kCGImagePropertyOrientation] = 1
        if var tiff = meta[kCGImagePropertyTIFFDictionary] as? [CFString: Any] {
            tiff[kCGImagePropertyTIFFOrientation] = 1
            meta[kCGImagePropertyTIFFDictionary] = tiff
        }
        meta[kCGImageDestinationLossyCompressionQuality] = quality > 0 ? quality : 0.9
        guard let dest = CGImageDestinationCreateWithURL(to as CFURL, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dest, img, meta as CFDictionary)
        return CGImageDestinationFinalize(dest) ? (img.width, img.height) : nil
    }

    /// A video's size as it plays (its track's natural size, turned by its
    /// transform) and its length in milliseconds.
    static func videoInfo(_ url: URL) async -> (Int, Int, Int) {
        let asset = AVURLAsset(url: url)
        var size = CGSize.zero, transform = CGAffineTransform.identity, seconds = 0.0
        if #available(iOS 16.0, macOS 13.0, *) {
            seconds = (try? await asset.load(.duration))?.seconds ?? 0
            if let t = try? await asset.loadTracks(withMediaType: .video).first,
               let (s, tr) = try? await t.load(.naturalSize, .preferredTransform) {
                (size, transform) = (s, tr)
            }
        } else {
            seconds = asset.duration.seconds
            if let t = asset.tracks(withMediaType: .video).first { (size, transform) = (t.naturalSize, t.preferredTransform) }
        }
        let r = CGRect(origin: .zero, size: size).applying(transform)
        let ms = seconds.isFinite ? Int((seconds * 1000).rounded()) : 0
        return (Int(abs(r.width).rounded()), Int(abs(r.height).rounded()), ms)
    }
    #endif

    #if canImport(UIKit) && !os(tvOS) && !os(watchOS)
    /// Copies what was picked to files (the picker's are temporary), in
    /// the order picked: a video's movie, an image in its own type (a HEIC
    /// stays HEIC) unless it's scaled.
    static func copy(_ results: [PHPickerResult], maxSize: Int, quality: Double) async -> [NativeMedia] {
        await withTaskGroup(of: (Int, NativeMedia?).self) { g in
            for (i, r) in results.enumerated() {
                let p = r.itemProvider
                g.addTask { (i, await copy(p, maxSize: maxSize, quality: quality)) }
            }
            var got = [NativeMedia?](repeating: nil, count: results.count)
            for await (i, m) in g { got[i] = m }
            return got.compactMap { $0 }
        }
    }

    static func copy(_ p: NSItemProvider, maxSize: Int, quality: Double) async -> NativeMedia? {
        let video = p.hasItemConformingToTypeIdentifier(UTType.movie.identifier) && !p.hasItemConformingToTypeIdentifier(UTType.image.identifier)
        let family: UTType = video ? .movie : .image
        // Its own type (public.heic, com.apple.quicktime-movie), not one
        // the provider would convert it to.
        let type = p.registeredTypeIdentifiers.first { UTType($0)?.conforms(to: family) ?? false } ?? family.identifier
        let suggested = p.suggestedName
        let got: (URL, String)? = await withCheckedContinuation { c in
            p.loadFileRepresentation(forTypeIdentifier: type) { url, _ in
                // The file goes when this returns: moved aside first.
                guard let url, let dir = try? cacheDir("asuka-picking") else { c.resume(returning: nil); return }
                let tmp = dir.appendingPathComponent(UUID().uuidString + "." + url.pathExtension)
                guard (try? FileManager.default.copyItem(at: url, to: tmp)) != nil else { c.resume(returning: nil); return }
                var name = url.lastPathComponent
                if let s = suggested, !s.isEmpty {
                    name = (s as NSString).pathExtension.isEmpty && !url.pathExtension.isEmpty ? s + "." + url.pathExtension : s
                }
                c.resume(returning: (tmp, name))
            }
        }
        guard let (tmp, name) = got else { return nil }
        defer { try? FileManager.default.removeItem(at: tmp) }
        return await media(tmp, name: name, video: video, maxSize: maxSize, quality: quality)
    }
    #endif

    /// A folder of the app's caches.
    static func cacheDir(_ name: String) throws -> URL {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent(name, isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// A MIME type's UTType; "image/*" and the like are the family's.
    static func utType(_ mime: String) -> UTType? {
        switch mime.trimmingCharacters(in: .whitespaces).lowercased() {
        case "*/*", "*", "": return .item
        case "image/*": return .image
        case "video/*": return .movie
        case "audio/*": return .audio
        case "text/*": return .text
        case "application/*": return .data
        case let m: return UTType(mimeType: m)
        }
    }

    static func error(_ m: String) -> Error { NSError(domain: "device", code: 0, userInfo: [NSLocalizedDescriptionKey: m]) }
}

#if !os(tvOS) && !os(watchOS)
/// Watches the pasteboard: iOS says when it changes (and the app coming
/// back may bring another app's copy); the Mac doesn't, so its change
/// count is read twice a second.
@MainActor final class ClipboardWatch: NSObject {
    private let changed: (ClipboardKinds) -> Void
    private var seen: Int
    private var timer: Timer?

    init(_ changed: @escaping (ClipboardKinds) -> Void) {
        self.changed = changed
        #if canImport(UIKit)
        seen = SystemDevice.board.changeCount
        #else
        seen = SystemDevice.board.changeCount
        #endif
        super.init()
        #if canImport(UIKit)
        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(check), name: UIPasteboard.changedNotification, object: nil)
        nc.addObserver(self, selector: #selector(check), name: UIApplication.didBecomeActiveNotification, object: nil)
        #else
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.check() }
        }
        #endif
    }

    @objc func check() {
        #if canImport(UIKit)
        let n = SystemDevice.board.changeCount
        #else
        let n = SystemDevice.board.changeCount
        #endif
        guard n != seen else { return }
        seen = n
        changed(SystemDevice.kinds())
    }

    func stop() {
        NotificationCenter.default.removeObserver(self)
        timer?.invalidate()
        timer = nil
    }
}
#endif

#if os(tvOS) || os(watchOS)
#elseif canImport(UIKit)
extension UIViewController {
    /// PHPicker over this controller, for up to max (0: any number) of
    /// kind, numbered as they're picked if ordered: what's picked, in the
    /// order picked.
    @MainActor func pickMedia(_ kind: MediaKind, _ max: Int, _ ordered: Bool) async -> [PHPickerResult] {
        await withCheckedContinuation { c in
            var config = PHPickerConfiguration()
            config.filter = kind == .videos ? .videos : kind == .all ? .any(of: [.images, .videos]) : .images
            config.selectionLimit = Swift.max(max, 0)
            // The files as they are (a HEIC, an HEVC movie), not converted.
            config.preferredAssetRepresentationMode = .current
            if ordered, max != 1 { config.selection = .ordered }
            let p = PHPickerViewController(configuration: config)
            let d = PhotoPickerDelegate { c.resume(returning: $0) }
            p.delegate = d
            objc_setAssociatedObject(p, &PhotoPickerDelegate.key, d, .OBJC_ASSOCIATION_RETAIN)
            present(p, animated: true)
        }
    }

    /// The document picker over this controller: copies of what's picked.
    @MainActor func pickDocuments(_ types: [UTType], _ multiple: Bool) async -> [URL] {
        await withCheckedContinuation { c in
            let p = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: true)
            p.allowsMultipleSelection = multiple
            let d = DocumentPickerDelegate { c.resume(returning: $0) }
            p.delegate = d
            objc_setAssociatedObject(p, &DocumentPickerDelegate.key, d, .OBJC_ASSOCIATION_RETAIN)
            present(p, animated: true)
        }
    }
}

final class PhotoPickerDelegate: NSObject, PHPickerViewControllerDelegate {
    nonisolated(unsafe) static var key = 0
    private var done: (([PHPickerResult]) -> Void)?
    init(_ done: @escaping ([PHPickerResult]) -> Void) { self.done = done }

    func picker(_ picker: PHPickerViewController, didFinishPicking results: [PHPickerResult]) {
        picker.dismiss(animated: true)
        done?(results)
        done = nil
    }
}

final class DocumentPickerDelegate: NSObject, UIDocumentPickerDelegate {
    nonisolated(unsafe) static var key = 0
    private var done: (([URL]) -> Void)?
    init(_ done: @escaping ([URL]) -> Void) { self.done = done }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        done?(urls)
        done = nil
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        done?([])
        done = nil
    }
}
#else
extension NSSavePanel {
    /// Shows the panel as a sheet on the app's window (or on its own, in
    /// front, with no window shown): the URLs chosen, none if cancelled.
    @MainActor func choose() async -> [URL] {
        await withCheckedContinuation { c in
            let finish: (NSApplication.ModalResponse) -> Void = { r in
                guard r == .OK else { c.resume(returning: []); return }
                if let open = self as? NSOpenPanel { c.resume(returning: open.urls) } else { c.resume(returning: self.url.map { [$0] } ?? []) }
            }
            var w = AsukaNative.window
            while let s = w?.attachedSheet { w = s }
            if let w, w.isVisible {
                beginSheetModal(for: w, completionHandler: finish)
            } else {
                NSApp.activate(ignoringOtherApps: true)
                begin(completionHandler: finish)
            }
        }
    }
}
#endif

func makePlatform() throws -> any Platform { SystemDevice() }
