// A TextField's selection, insertions and pastes of content the app takes
// (TextField's Selection, InsertText, PasteTypes).
//
// A one-line field is edited by its window's field editor, an NSTextView;
// the host gives fields a field editor of its own class (AsukaFieldEditor),
// whose paste: hands the app what it takes, and a multiline field's text
// view is of that class too. Offsets are UTF-16, as NSString's are.
import AppKit
import UniformTypeIdentifiers
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A field's text view: a window's field editor, or a multiline field's.
/// Its paste: pastes content of the field's PasteTypes into Pasted.
final class AsukaFieldEditor: NSTextView {
    @MainActor var rt: AsukaRuntime { AsukaRuntime.shared }

    override func paste(_ sender: Any?) {
        if let n = rt.editorNode(self), rt.pasteContent(n) { return }
        super.paste(sender)
    }

    override func validateUserInterfaceItem(_ item: NSValidatedUserInterfaceItem) -> Bool {
        if item.action == #selector(paste(_:)), let n = rt.editorNode(self), !n.pasteTypes.isEmpty, asukaPasteboardHas(n.pasteTypes) {
            return true
        }
        return super.validateUserInterfaceItem(item)
    }
}

/// Each window's field editor.
@MainActor private var fieldEditors: [ObjectIdentifier: AsukaFieldEditor] = [:]

/// windowWillReturnFieldEditor(_:to:): a field of the app's gets the
/// window's AsukaFieldEditor; anything else the window's own.
extension AsukaWin {
    @objc(windowWillReturnFieldEditor:toObject:) func windowWillReturnFieldEditor(_ sender: NSWindow, to client: Any?) -> Any? {
        AsukaRuntime.shared.fieldEditor(sender, client)
    }
}

@MainActor extension AsukaRuntime {
    func fieldEditor(_ w: NSWindow, _ client: Any?) -> Any? {
        guard let v = client as? NSView, let n = node(of: v), n.kind == .textField else { return nil }
        let key = ObjectIdentifier(w)
        if let e = fieldEditors[key] { return e }
        let e = AsukaFieldEditor(frame: .zero)
        e.isFieldEditor = true
        // Its typing undoes with ⌘Z, as the window's own field editor's
        // does (in the window's undo manager: Undo.swift).
        e.allowsUndo = true
        fieldEditors[key] = e
        AsukaTextDelegate.shared.start()
        return e
    }

    /// The field a text view edits: a multiline field's own, or the one-line
    /// field the field editor is editing.
    func editorNode(_ tv: NSTextView) -> AsukaNode? {
        if let n = textViewNode(tv) { return n }
        if tv.isFieldEditor, let f = tv.delegate as? NSView, let n = node(of: f), n.kind == .textField { return n }
        return nil
    }

    /// The text view editing a field now, if one is.
    func editing(_ n: AsukaNode) -> NSTextView? {
        if let tv = n.textView { return tv }
        return (n.view as? NSTextField)?.currentEditor() as? NSTextView
    }

    /// Puts the cursor (or the selection) at UTF-16 offsets of the field's
    /// text, clamped; a one-line field not being edited takes it as it's
    /// edited next.
    func setSelection(_ n: AsukaNode, _ b: [UInt8]) {
        guard b.count >= 8 else { return }
        AsukaTextDelegate.shared.start()
        var s = Int(b.u32(0)), e = Int(b.u32(4))
        guard let tv = editing(n) else {
            n.selection = (s, e)
            n.selectionPending = true
            return
        }
        let length = (tv.string as NSString).length
        s = min(s, length)
        e = min(e, length)
        if e < s { swap(&s, &e) }
        n.selection = (s, e)
        tv.setSelectedRange(NSRange(location: s, length: e - s))
    }

    /// Puts a field's selection where the core set it while it wasn't being
    /// edited, as its editing begins.
    func applyPendingSelection(_ n: AsukaNode) {
        guard n.selectionPending else { return }
        n.selectionPending = false
        var w = AsukaByteWriter()
        w.u32(UInt32(n.selection.0))
        w.u32(UInt32(n.selection.1))
        setSelection(n, w.bytes)
    }

    /// NSTextViewDidChangeSelectionNotification: the field's selection,
    /// reported after what moved it (typing reports its text first), unless
    /// it's where the core put it.
    func selectionChanged(_ tv: NSTextView) {
        guard let n = editorNode(tv) else { return }
        if n.richField != nil && n.textView === tv { reportFormats(n) }
        guard n.listening(.selectionChanged), !n.selectionQueued else { return }
        n.selectionQueued = true
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                n.selectionQueued = false
                guard self.alive(n), let tv = self.editing(n) else { return }
                let r = tv.selectedRange()
                let sel = (r.location, r.location + r.length)
                guard sel != n.selection else { return }
                n.selection = sel
                var w = AsukaByteWriter()
                w.u32(UInt32(sel.0))
                w.u32(UInt32(sel.1))
                let b = w.bytes
                self.emit(n, .selectionChanged) { AsukaEvents.selectionChanged($0, b) }
            }
        }
    }

    /// Types text into a field, as the keyboard would: in place of the
    /// selection, undoably, and reported as typing is. A one-line field not
    /// being edited is edited first, so its field editor does it.
    func insertText(_ n: AsukaNode, _ s: String) {
        var tv = editing(n)
        if tv == nil {
            guard let v = n.view, let w = v.window else { return }
            w.makeFirstResponder(v)
            tv = editing(n)
            guard let t = tv else { return }
            // Where the core last put (or heard of) the selection.
            let length = (t.string as NSString).length
            var a = min(n.selection.0, length), b = min(n.selection.1, length)
            if b < a { swap(&a, &b) }
            n.selectionPending = false
            t.setSelectedRange(NSRange(location: a, length: b - a))
        }
        tv?.insertText(s, replacementRange: NSRange(location: NSNotFound, length: 0))
    }

    /// Takes the pasteboard's items of the field's types, if it has any
    /// (files and image data copied into the caches), and reports them with
    /// Pasted; false: none, the field pastes as it would.
    func pasteContent(_ n: AsukaNode) -> Bool {
        guard !n.pasteTypes.isEmpty, n.listening(.pasted) else { return false }
        var items: [AsukaPasteItem] = []
        for reps in asukaPasteboardReps(.general) {
            guard let r = reps.first(where: { asukaMatchMIME(n.pasteTypes, $0.mime) }) else { continue }
            guard var it = r.read() else { continue }
            if !it.path.isEmpty && !it.copied {
                // Another app's file: the app's own copy, as a paste's are.
                guard let p = asukaCopyToDrops(it.path) else { continue }
                it.path = p
            }
            items.append(it)
        }
        guard !items.isEmpty else { return false }
        var w = AsukaByteWriter()
        w.u32(UInt32(items.count))
        for it in items {
            w.u8(UInt8(it.kind.rawValue))
            w.string(it.text)
            w.string(it.path)
            w.string(it.name)
            w.string(it.mime)
            w.i64(it.size)
        }
        let b = w.bytes
        emit(n, .pasted) { AsukaEvents.pasted($0, b) }
        return true
    }
}

/// An item pasted (ops.Item): a text, a link or a file.
struct AsukaPasteItem {
    var kind = AsukaShareKind.text
    var text = "", path = "", name = "", mime = ""
    var size: Int64 = -1
    /// Its file is the app's own copy.
    var copied = false
}

/// A way a pasteboard item can be read: a file, a file promise, image data,
/// a link, a text.
struct AsukaPasteRep {
    enum Kind { case file, promise, image, link, text }
    var kind: Kind
    var mime: String
    var pbType = ""
    var path = ""
    var item: NSPasteboardItem
    /// A link, read as text.
    var linkText = false

    @MainActor func read() -> AsukaPasteItem? {
        var it = AsukaPasteItem(mime: mime)
        switch kind {
        case .file:
            it.kind = asukaFileKind(mime)
            it.path = path
            it.name = (path as NSString).lastPathComponent
            if let a = try? FileManager.default.attributesOfItem(atPath: path), let s = a[.size] as? NSNumber { it.size = s.int64Value }
        case .promise:
            return nil
        case .image:
            guard let data = item.data(forType: NSPasteboard.PasteboardType(pbType)),
                  let p = asukaWriteDropFile("Image" + asukaUTIExt(pbType), data) else { return nil }
            it.kind = .image
            it.path = p
            it.name = (p as NSString).lastPathComponent
            it.size = Int64(data.count)
            it.copied = true
        case .link:
            it.kind = linkText ? .text : .link
            it.text = item.string(forType: NSPasteboard.PasteboardType(pbType)) ?? ""
        case .text:
            it.kind = .text
            it.text = item.string(forType: NSPasteboard.PasteboardType(pbType)) ?? ""
        }
        return it
    }
}

/// A pasteboard's items, each the ways it can be read, the best first: a
/// file, a file promise, image data, a link, a text.
@MainActor func asukaPasteboardReps(_ pb: NSPasteboard) -> [[AsukaPasteRep]] {
    let promiseTypes = Set(NSFilePromiseReceiver.readableDraggedTypes)
    var out: [[AsukaPasteRep]] = []
    for item in pb.pasteboardItems ?? [] {
        let order = item.types.map { $0.rawValue }
        let types = Set(order)
        var reps: [AsukaPasteRep] = []
        if types.contains("public.file-url") {
            if let s = item.string(forType: .fileURL), let u = URL(string: s), u.isFileURL {
                let path = (u as NSURL).filePathURL?.path ?? u.path
                reps.append(AsukaPasteRep(kind: .file, mime: asukaURLMIME(u, path), path: path, item: item))
            }
            out.append(reps)
            continue
        }
        if order.contains(where: { promiseTypes.contains($0) }) {
            reps.append(AsukaPasteRep(kind: .promise, mime: "application/octet-stream", item: item))
        }
        if let t = order.first(where: asukaIsImageType) {
            reps.append(AsukaPasteRep(kind: .image, mime: asukaUTIMIME(t), pbType: t, item: item))
        }
        if types.contains("public.url") {
            reps.append(AsukaPasteRep(kind: .link, mime: "text/uri-list", pbType: "public.url", item: item))
        }
        if types.contains("public.utf8-plain-text") {
            reps.append(AsukaPasteRep(kind: .text, mime: "text/plain", pbType: "public.utf8-plain-text", item: item))
        } else if types.contains("public.url") {
            reps.append(AsukaPasteRep(kind: .link, mime: "text/plain", pbType: "public.url", item: item, linkText: true))
        }
        if !reps.isEmpty { out.append(reps) }
    }
    return out
}

/// Whether the general pasteboard has content of the types (not text: a
/// field pastes text itself).
@MainActor func asukaPasteboardHas(_ types: [String]) -> Bool {
    for reps in asukaPasteboardReps(.general) {
        for r in reps where r.kind != .text && r.kind != .promise && asukaMatchMIME(types, r.mime) { return true }
    }
    return false
}

/// Whether a MIME type matches one of the patterns (ops.MatchMIME): "*",
/// "*/*", "image/*", or the type itself; parameters aside, any case.
func asukaMatchMIME(_ patterns: [String], _ mime: String) -> Bool {
    func base(_ m: String) -> String {
        (m.split(separator: ";", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init) ?? m).trimmingCharacters(in: .whitespaces)
    }
    let m = base(mime).lowercased()
    for raw in patterns {
        let p = base(raw).lowercased()
        if p == "*" || p == "*/*" { return true }
        if p.hasSuffix("/*") {
            let prefix = String(p.dropLast())
            if m.count > prefix.count && m.hasPrefix(prefix) { return true }
        } else if p == m {
            return true
        }
    }
    return false
}

func asukaUTIMIME(_ uti: String) -> String {
    if #available(macOS 11.0, *), let t = UTType(uti), let m = t.preferredMIMEType { return m }
    return "application/octet-stream"
}

func asukaUTIExt(_ uti: String) -> String {
    if #available(macOS 11.0, *), let t = UTType(uti), let e = t.preferredFilenameExtension { return "." + e }
    return ""
}

func asukaIsImageType(_ uti: String) -> Bool {
    if #available(macOS 11.0, *), let t = UTType(uti) { return t.conforms(to: .image) }
    return false
}

/// A file's MIME type: its content type's, else its extension's (unknown:
/// application/octet-stream).
func asukaURLMIME(_ u: URL, _ path: String) -> String {
    if #available(macOS 11.0, *) {
        if let t = try? u.resourceValues(forKeys: [.contentTypeKey]).contentType, let m = t.preferredMIMEType { return m }
        let ext = (path as NSString).pathExtension
        if !ext.isEmpty, let t = UTType(filenameExtension: ext), let m = t.preferredMIMEType { return m }
    }
    return "application/octet-stream"
}

func asukaFileKind(_ m: String) -> AsukaShareKind {
    if m.hasPrefix("image/") { return .image }
    if m.hasPrefix("video/") { return .video }
    if m.hasPrefix("audio/") { return .audio }
    return .file
}

@MainActor private var dropSeq = 0

/// A new directory in the app's caches for a paste's files.
@MainActor func asukaDropDir() -> String? {
    guard let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first else { return nil }
    dropSeq += 1
    let app = Bundle.main.bundleIdentifier ?? AsukaRuntime.shared.appName
    let dir = base.appendingPathComponent(app).appendingPathComponent("Drops")
        .appendingPathComponent("\(UInt64(Date().timeIntervalSince1970 * 1e9))-\(dropSeq)")
    guard (try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)) != nil else { return nil }
    return dir.path
}

@MainActor func asukaWriteDropFile(_ name: String, _ data: Data) -> String? {
    guard let dir = asukaDropDir() else { return nil }
    let p = (dir as NSString).appendingPathComponent(name)
    return (try? data.write(to: URL(fileURLWithPath: p))) != nil ? p : nil
}

/// Copies a file into a new folder of the app's caches.
@MainActor func asukaCopyToDrops(_ src: String) -> String? {
    guard let dir = asukaDropDir() else { return nil }
    let dst = (dir as NSString).appendingPathComponent((src as NSString).lastPathComponent)
    return (try? FileManager.default.copyItem(atPath: src, toPath: dst)) != nil ? dst : nil
}
