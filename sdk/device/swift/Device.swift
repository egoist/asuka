// platform/device on iOS and the Mac: UIDevice and the bundle, the in-app
// browser (SFSafariViewController; the Mac's browser), PHPicker and the
// document picker (NSOpenPanel on the Mac), the pasteboard.
import Foundation
import UniformTypeIdentifiers
import Asuka
#if canImport(UIKit)
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
        #if canImport(UIKit)
        return await MainActor.run {
            NativeInfo(model: UIDevice.current.model, system: UIDevice.current.systemName, systemVersion: UIDevice.current.systemVersion, appVersion: app, build: build,
                       source: source, installedMs: installed, updatedMs: updated)
        }
        #else
        let v = ProcessInfo.processInfo.operatingSystemVersion
        let version = v.patchVersion > 0 ? "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)" : "\(v.majorVersion).\(v.minorVersion)"
        return NativeInfo(model: "Mac", system: "macOS", systemVersion: version, appVersion: app, build: build,
                          source: source, installedMs: installed, updatedMs: updated)
        #endif
    }

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
        #if canImport(UIKit)
        try await MainActor.run {
            guard let top = AsukaNative.presenter else { throw Self.error("device: no screen to show it on") }
            top.present(SFSafariViewController(url: u), animated: true)
        }
        #else
        let ok = await MainActor.run { NSWorkspace.shared.open(u) }
        if !ok { throw Self.error("device: couldn't open \(url)") }
        #endif
    }

    func pickPhotos(max: Int) async throws -> [String] {
        #if canImport(UIKit)
        let results: [PHPickerResult] = try await MainActor.run {
            guard let top = AsukaNative.presenter else { throw Self.error("device: no screen to show it on") }
            return top
        }.pickPhotos(max)
        return await Self.copy(results)
        #else
        return try await Self.open(types: [.image], multiple: max > 1, limit: max).map(\.path)
        #endif
    }

    func pickFiles(types: [String], multiple: Bool) async throws -> [File] {
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
    }

    func clipboardText() async throws -> String? {
        await MainActor.run {
            #if canImport(UIKit)
            UIPasteboard.general.string
            #else
            NSPasteboard.general.string(forType: .string)
            #endif
        }
    }

    func clipboardHas() async throws -> ClipboardKinds { await MainActor.run { Self.kinds() } }

    @MainActor static func kinds() -> ClipboardKinds {
        #if canImport(UIKit)
        let pb = UIPasteboard.general
        return ClipboardKinds(text: pb.hasStrings, image: pb.hasImages, url: pb.hasURLs)
        #else
        let pb = NSPasteboard.general
        let types = pb.types ?? []
        let image = types.contains { UTType($0.rawValue)?.conforms(to: .image) ?? false }
        let url = types.contains(.URL) || types.contains(.fileURL)
        return ClipboardKinds(text: types.contains(.string), image: image, url: url)
        #endif
    }

    func clipboardImage() async throws -> String? {
        // The image as it was copied (its bytes, not a re-encoding), in the
        // first image type the clipboard has.
        let found: (Data, String)? = await MainActor.run {
            #if canImport(UIKit)
            let pb = UIPasteboard.general
            for t in pb.types.compactMap({ UTType($0) }) where t.conforms(to: .image) {
                if let d = pb.data(forPasteboardType: t.identifier) { return (d, t.preferredFilenameExtension ?? "png") }
            }
            if let img = pb.image, let d = img.pngData() { return (d, "png") }
            return nil
            #else
            let pb = NSPasteboard.general
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
    }

    func copyImage(path: String) async throws {
        let url = URL(fileURLWithPath: path)
        guard let data = try? Data(contentsOf: url) else { throw Self.error("device: no file at \(path)") }
        try await MainActor.run {
            #if canImport(UIKit)
            let pb = UIPasteboard.general
            if let t = UTType(filenameExtension: url.pathExtension), t.conforms(to: .image) {
                pb.setData(data, forPasteboardType: t.identifier)
            } else if let img = UIImage(data: data) {
                pb.image = img
            } else {
                throw Self.error("device: not an image: \(path)")
            }
            #else
            let pb = NSPasteboard.general
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
    }

    func watchClipboard(changed: @escaping (ClipboardKinds) -> Void) async throws {
        let w = await MainActor.run { ClipboardWatch(changed) }
        defer { Task { @MainActor in w.stop() } }
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    @MainActor func keepAwake(on: Bool) throws {
        #if canImport(UIKit)
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

    #if canImport(UIKit)
    /// Copies the picked images to files (the picker's are temporary).
    static func copy(_ results: [PHPickerResult]) async -> [String] {
        guard let dir = try? cacheDir("asuka-photos") else { return [] }
        var paths: [String] = []
        for r in results {
            let path: String? = await withCheckedContinuation { c in
                r.itemProvider.loadFileRepresentation(forTypeIdentifier: UTType.image.identifier) { url, _ in
                    guard let url else { c.resume(returning: nil); return }
                    let to = dir.appendingPathComponent(UUID().uuidString + "." + (url.pathExtension.isEmpty ? "jpg" : url.pathExtension))
                    c.resume(returning: (try? FileManager.default.copyItem(at: url, to: to)) != nil ? to.path : nil)
                }
            }
            if let path { paths.append(path) }
        }
        return paths
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
        seen = UIPasteboard.general.changeCount
        #else
        seen = NSPasteboard.general.changeCount
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
        let n = UIPasteboard.general.changeCount
        #else
        let n = NSPasteboard.general.changeCount
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

#if canImport(UIKit)
extension UIViewController {
    /// PHPicker over this controller, for up to max images: what's picked.
    @MainActor func pickPhotos(_ max: Int) async -> [PHPickerResult] {
        await withCheckedContinuation { c in
            var config = PHPickerConfiguration()
            config.filter = .images
            config.selectionLimit = Swift.max(max, 1)
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
