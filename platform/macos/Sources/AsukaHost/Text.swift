// Text on a Mac: a multiline TextField (an NSTextView in a scroll view that
// grows with its text up to its line limit), the text views' delegate, and
// what the text part adds to the automation. Rich text is
// RichText.swift, a Rich field RichField.swift, selection, insertions and
// pastes TextInput.swift, keys KeyWatcher.swift.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// The delegate of the host's text views (a multiline field's, a rich
/// text's), and the observer of every text view's selection.
@MainActor final class AsukaTextDelegate: NSObject, NSTextViewDelegate {
    static let shared = AsukaTextDelegate()
    var rt: AsukaRuntime { AsukaRuntime.shared }
    private var observing = false

    /// Follows text views' selections, as they move them: a field's
    /// editor's, a multiline field's, a rich text's.
    func start() {
        guard !observing else { return }
        observing = true
        NotificationCenter.default.addObserver(self, selector: #selector(selectionDidChange(_:)), name: NSTextView.didChangeSelectionNotification, object: nil)
    }

    @objc func selectionDidChange(_ note: Notification) {
        guard let tv = note.object as? NSTextView else { return }
        rt.selectionChanged(tv)
    }

    func textDidChange(_ note: Notification) {
        guard let tv = note.object as? NSTextView else { return }
        rt.textViewChanged(tv)
    }

    func textDidBeginEditing(_ note: Notification) {
        guard let tv = note.object as? NSTextView, let n = rt.textViewNode(tv) else { return }
        rt.emit(n, .focus) { AsukaEvents.focus($0) }
    }

    func textDidEndEditing(_ note: Notification) {
        guard let tv = note.object as? NSTextView, let n = rt.textViewNode(tv) else { return }
        rt.emit(n, .blur) { AsukaEvents.blur($0) }
    }

    func textView(_ tv: NSTextView, clickedOnLink link: Any, at charIndex: Int) -> Bool {
        rt.linkClicked(tv, link)
    }
}

@MainActor extension AsukaRuntime {
    /// Makes a TextField a text view in a scroll view (n.textView): Return
    /// is a new line, and it grows with its text up to its line limit.
    func makeMultiline(_ n: AsukaNode) {
        let sv = NSScrollView(frame: n.view?.frame ?? .zero)
        sv.hasVerticalScroller = true
        sv.autohidesScrollers = true
        sv.borderType = .bezelBorder
        // An NSTextView that pastes the content the field takes.
        let tv = AsukaFieldEditor(frame: CGRect(origin: .zero, size: sv.contentSize))
        tv.isRichText = false
        tv.delegate = AsukaTextDelegate.shared
        tv.isVerticallyResizable = true
        tv.isHorizontallyResizable = false
        tv.autoresizingMask = [.width]
        tv.minSize = .zero
        tv.maxSize = CGSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        tv.textContainer?.widthTracksTextView = true
        tv.textContainerInset = CGSize(width: 2, height: 4)
        tv.allowsUndo = true
        tv.isAutomaticSpellingCorrectionEnabled = NSSpellChecker.isAutomaticSpellingCorrectionEnabled
        sv.documentView = tv
        tv.string = n.text
        n.textView = tv
        n.multiline = true
        replaceView(n, sv)
        byView[ObjectIdentifier(tv)] = n
        if n.plain { plainField(n) }
        if !n.placeholder.isEmpty { textViewPlaceholder(n) }
        if n.inset != (0, 0, 0, 0) { applyContentInset(n) }
    }

    /// Sizes a multiline field: as wide as a field would be, as tall as its
    /// text, up to its line limit.
    func measureMultiline(_ n: AsukaNode, _ c: AsukaOffer) -> CGSize {
        var w: CGFloat = 200
        if c.widthKnown { w = CGFloat(c.width) } else if let a = c.finiteWidth { w = a }
        guard let tv = n.textView, let lm = tv.layoutManager, let tc = tv.textContainer else { return CGSize(width: w, height: 22) }
        lm.ensureLayout(for: tc)
        let used = lm.usedRect(for: tc)
        let line = lm.defaultLineHeight(for: tv.font ?? styleFont(0))
        var textH = max(used.height, line)
        if n.lineLimit > 0 { textH = min(textH, line * CGFloat(n.lineLimit)) }
        return CGSize(width: w, height: (textH + tv.textContainerInset.height * 2 + 4).rounded(.up))
    }

    /// Draws a multiline field as Plain says: without its bezel, background
    /// and padding of its own, or with them.
    func plainTextView(_ n: AsukaNode) {
        guard let sv = n.view as? NSScrollView, let tv = n.textView else { return }
        sv.borderType = n.plain ? .noBorder : .bezelBorder
        sv.drawsBackground = !n.plain
        tv.drawsBackground = !n.plain
        var inset = CGSize(width: 2, height: 4)
        if n.plain {
            inset = .zero
            tv.textContainer?.lineFragmentPadding = 0
        }
        tv.textContainerInset = inset
    }

    /// A multiline field's placeholder: NSTextView draws one while it's
    /// empty (its placeholderString, which it has had since macOS 10.10 but
    /// doesn't declare).
    func textViewPlaceholder(_ n: AsukaNode) {
        guard let tv = n.textView else { return }
        let sel = Selector(("setPlaceholderString:"))
        if tv.responds(to: sel) { tv.perform(sel, with: n.placeholder as NSString) }
        tv.needsDisplay = true
    }

    /// The node whose multiline field (or Rich field) tv is.
    func textViewNode(_ tv: NSTextView) -> AsukaNode? {
        guard let n = node(of: tv), n.textView === tv else { return nil }
        return n
    }

    /// A multiline field's text changed: the app hears of it.
    func textViewChanged(_ tv: NSTextView) {
        guard let n = textViewNode(tv) else { return }
        n.text = tv.string
        let s = n.text
        emit(n, .textChanged) { AsukaEvents.textChanged($0, s) }
        richFieldChanged(n)
    }

    /// A command a one-line field's editor runs (not Return): whether it
    /// took it. The field's keys are the key monitor's (KeyWatcher.swift): what
    /// comes here is the field's own.
    func textCommand(_ n: AsukaNode, _ cmd: Selector) -> Bool { false }

    // Hooks (Parts.swift).
    func textDestroyed(_ n: AsukaNode) {}

    func textReleased(_ n: AsukaNode) {
        if let tv = n.textView, tv.delegate === AsukaTextDelegate.shared { tv.delegate = nil }
        if let r = n.rich { r.view.delegate = nil }
        selectableReleased(n)
    }

    func textPlaced(_ n: AsukaNode) {
        if n.rich != nil { placeInlines(n) }
        textStylePlaced(n) // TextStyle.swift: a shrinking text fits its frame
    }

    func textColors(_ n: AsukaNode) {
        if n.kind == .selectable { n.view?.needsDisplay = true }
    }

    func textDirection(_ n: AsukaNode, _ changed: Bool) {
        if changed, n.rich != nil { richDirectionChanged(n) }
    }

    func textJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        if n.kind == .textField {
            var j: [String: Any] = ["multiline": n.multiline, "plain": n.plain, "secure": n.secure]
            if let tv = editing(n) {
                let r = tv.selectedRange()
                j["selection"] = [r.location, r.location + r.length]
                j["editing"] = tv.window?.firstResponder === tv
                j["editorClass"] = String(describing: type(of: tv))
                if tv.hasMarkedText() { j["markedText"] = true }
            }
            j["nodeSelection"] = [n.selection.0, n.selection.1]
            if let tv = n.textView {
                j["string"] = tv.string
                if let sv = n.view as? NSScrollView {
                    j["bordered"] = sv.borderType != .noBorder
                    let i = sv.contentInsets
                    j["contentInsets"] = [Double(i.top), Double(i.right), Double(i.bottom), Double(i.left)]
                }
                j["drawsBackground"] = tv.drawsBackground
                j["autocorrect"] = tv.isAutomaticSpellingCorrectionEnabled
                let sel = Selector(("placeholderString"))
                if tv.responds(to: sel), let p = tv.perform(sel)?.takeUnretainedValue() as? String { j["placeholder"] = p }
                j["alignment"] = asukaAlignName(tv.alignment)
                j["writingDirection"] = tv.baseWritingDirection.rawValue
                if n.richField != nil, let ts = tv.textStorage { j["runs"] = asukaRunsJSON(ts) }
            } else if let f = n.view as? NSTextField, let cell = f.cell {
                j["drawingRect"] = asukaRect(cell.drawingRect(forBounds: f.bounds))
                j["bezeled"] = f.isBezeled
                j["drawsBackground"] = f.drawsBackground
                j["writingDirection"] = f.baseWritingDirection.rawValue
                j["alignment"] = asukaAlignName(f.alignment)
            }
            if !n.pasteTypes.isEmpty { j["pasteTypes"] = n.pasteTypes }
            if let f = n.richField { j["formats"] = Int(f.formats); j["link"] = f.link }
            d["field"] = j
        }
        if n.kind == .text, let t = n.view as? NSTextField {
            d["text"] = n.text
            d["label"] = ["lines": t.maximumNumberOfLines, "writingDirection": t.baseWritingDirection.rawValue, "alignment": asukaAlignName(t.alignment),
                          "inset": [Double(n.inset.top), Double(n.inset.right), Double(n.inset.bottom), Double(n.inset.left)]]
        }
        richJSON(n, &d)
        textStyleJSON(n, &d)
        if n.kind == .selectable { d["selectedText"] = selectableText(n) ?? "" }
        if !n.keys.isEmpty { d["keys"] = n.keys.map { ["name": $0.name, "mods": Int($0.mods)] } }
    }

    func registerTextModules() {}

    /// The automation's steps of lists and text (Automation.swift): nil if
    /// verb isn't one of them, else its error (nil: none).
    func listTextStep(_ verb: String, _ arg: String) -> String?? {
        let a = AsukaAutomation.shared
        switch verb {
        case "select":
            // select <start>,<end>: the focused field's selection, as the
            // user moves it.
            let p = arg.split(separator: ",").compactMap { Int($0.trimmingCharacters(in: .whitespaces)) }
            guard p.count == 2 else { return .some("want select:<start>,<end>") }
            guard let n = a.focusedField() ?? a.firstField(), let tv = editing(n) else { return .some("no field is being edited") }
            let len = (tv.string as NSString).length
            let s = min(p[0], len), e = min(max(p[1], s), len)
            tv.setSelectedRange(NSRange(location: s, length: e - s))
        case "mark", "unmark":
            // An input method's marked text in the focused field, and its
            // commit.
            guard let n = a.focusedField(), let tv = editing(n) else { return .some("no field is being edited") }
            if verb == "unmark" {
                tv.unmarkText()
            } else {
                let ns = arg as NSString
                tv.setMarkedText(arg, selectedRange: NSRange(location: ns.length, length: 0), replacementRange: NSRange(location: NSNotFound, length: 0))
            }
        case "undo", "redo":
            guard let n = a.focusedField(), let tv = editing(n), let u = tv.undoManager else { return .some("no field is being edited") }
            if verb == "undo" { u.undo() } else { u.redo() }
        case "paste":
            // The pasteboard's content pasted in the focused field (paste
            // <text> puts the text on it first).
            if !arg.isEmpty {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(arg, forType: .string)
            }
            guard let n = a.focusedField(), let tv = editing(n) else { return .some("no field is being edited") }
            tv.paste(nil)
        case "pastefile":
            // A file on the pasteboard, then pasted.
            NSPasteboard.general.clearContents()
            NSPasteboard.general.writeObjects([URL(fileURLWithPath: arg) as NSURL])
            guard let n = a.focusedField(), let tv = editing(n) else { return .some("no field is being edited") }
            tv.paste(nil)
        case "link":
            // link <url>: a click on the rich text's link to it.
            for n in nodes.values where n.rich != nil && a.showing(n) {
                guard let r = n.rich, let ts = r.view.textStorage else { continue }
                var found = -1
                ts.enumerateAttribute(.link, in: NSRange(location: 0, length: ts.length)) { v, range, stop in
                    if asukaLinkString(v) == arg { found = range.location; stop.pointee = true }
                }
                if found >= 0 {
                    // As NSTextView's click does: its delegate first, then
                    // the workspace for a link the app doesn't take.
                    if !linkClicked(r.view, arg), let u = URL(string: arg) { NSWorkspace.shared.open(u) }
                    return .some(nil)
                }
            }
            return .some("no link to \(arg)")
        case "click", "cmdclick":
            return .some(click(arg, verb))
        case "rowaction":
            // rowaction <row's text>|<action's title>: the item of the row's
            // context menu (its swipe actions) chosen.
            let p = arg.split(separator: "|", maxSplits: 1).map(String.init)
            guard p.count == 2 else { return .some("want rowaction:<row text>|<title>") }
            return .some(rowAction(p[0], p[1]))
        case "pull":
            guard let pts = Double(arg) else { return .some("want pull:<points>") }
            var s: AsukaNode?
            for n in nodes.values where n.refresh != nil && a.showing(n) && (s == nil || n.id > s!.id) { s = n }
            guard let s else { return .some("nothing refreshable shows") }
            return .some(pull(s, CGFloat(pts)))
        case "endscroll":
            // The end of a live scroll on the innermost scroll view showing:
            // a paging one settles.
            var s: AsukaNode?
            for n in nodes.values where (n.kind == .scroll || n.kind == .list) && a.showing(n) && (s == nil || n.id > s!.id) { s = n }
            guard let sv = s?.view as? NSScrollView else { return .some("no scroll view shows") }
            NotificationCenter.default.post(name: NSScrollView.didEndLiveScrollNotification, object: sv)
        case "scrollx":
            // scrollx <x>: the innermost horizontal scroll view showing.
            guard let x = Double(arg) else { return .some("want scrollx:<x>") }
            var s: AsukaNode?
            for n in nodes.values where n.kind == .scroll && n.horizontal && a.showing(n) && (s == nil || n.id > s!.id) { s = n }
            guard let s else { return .some("no horizontal scroll view shows") }
            scrollToOffset(s, CGFloat(x), 0, false)
        case "dot":
            guard let i = Int(arg) else { return .some("want dot:<index>") }
            for n in nodes.values where n.dots != nil && a.showing(n) {
                guard let v = n.view, let w = v.window else { continue }
                let p = v.convert(dotCenter(n, i), to: nil)
                if let e = NSEvent.mouseEvent(with: .leftMouseDown, location: p, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                              windowNumber: w.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1) {
                    v.mouseDown(with: e)
                }
                return .some(nil)
            }
            return .some("no page control shows")
        case "selectall":
            // In a Selectable showing: all its texts.
            for n in nodes.values where n.kind == .selectable && a.showing(n) {
                selectAllIn(n)
                return .some(nil)
            }
            return .some("no Selectable shows")
        case "copy":
            // ⌘C where the selection is (a Selectable's, a field's, a rich
            // text's): the first responder of the window in use.
            let w = keyTarget()
            if let r = w.firstResponder, r.responds(to: #selector(NSText.copy(_:))) {
                NSApp.sendAction(#selector(NSText.copy(_:)), to: r, from: nil)
            } else {
                NSApp.sendAction(#selector(NSText.copy(_:)), to: nil, from: nil)
            }
        case "selectdrag", "selectwords":
            // selectdrag <text>|<text>: a press at the start of the rich text
            // that says the first, dragged to the end of the one that says the
            // second, let go; selectwords: from a double click (by words).
            let p = arg.split(separator: "|", maxSplits: 1).map(String.init)
            guard p.count == 2 else { return .some("want \(verb) <text>|<text>") }
            return .some(selectDrag(p[0], p[1], clicks: verb == "selectwords" ? 2 : 1))
        case "keyin", "keyrepeat":
            // keyin <shortcut>: a key going down in the window the keyboard's
            // focus is in (a sheet's, a popover's, a Window's), whether or not
            // the app is active; keyrepeat: a held key's repeat.
            return .some(keyIn(arg, repeating: verb == "keyrepeat"))
        case "unfocus":
            // unfocus [title]: the window in use (or the one titled title)
            // itself has the keyboard's focus: nothing in it.
            var w = keyTarget()
            if !arg.isEmpty {
                guard let x = NSApp.windows.first(where: { $0.title == arg && $0.isVisible }) else { return .some("no window \(arg)") }
                w = x
            }
            w.makeFirstResponder(nil)
            asukaKeyTarget = w
        default:
            return nil
        }
        return .some(nil)
    }

    /// The window a test's key goes to: the focused field's, else the one
    /// the focus was last taken from (unfocus), else the key window.
    func keyTarget() -> NSWindow {
        if let w = AsukaAutomation.shared.focusedField()?.view?.window { return w }
        if let w = asukaKeyTarget, w.isVisible { return w }
        return NSApp.keyWindow ?? keyWin.window
    }

    /// A key going down in the window the focus is in, as the application
    /// takes it from its queue: the key monitor, then the menu bar's key
    /// equivalents, then the window's first responder. The app needn't be
    /// active (other apps' windows may be in front while tests run).
    func keyIn(_ shortcut: String, repeating: Bool) -> String? {
        let parts = shortcut.lowercased().split(separator: "+").map(String.init)
        var flags: NSEvent.ModifierFlags = []
        for m in parts.dropLast() {
            switch m {
            case "cmd", "command", "primary": flags.insert(.command)
            case "ctrl", "control": flags.insert(.control)
            case "shift": flags.insert(.shift)
            case "alt", "option", "opt": flags.insert(.option)
            default: return "no modifier \(m)"
            }
        }
        guard let key = parts.last else { return "no key" }
        var code: UInt16 = 0xffff
        var chars = ""
        if let k = asukaScriptKeys[key] {
            code = k.0
            chars = k.1
        } else if key.count == 1 {
            chars = key
            if let c = asukaCharCodes[key.first!] { code = c }
        } else {
            return "no key \(key)"
        }
        let w = keyTarget()
        let typed = flags.contains(.shift) ? chars.uppercased() : chars
        guard let ev = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: ProcessInfo.processInfo.systemUptime,
                                        windowNumber: w.windowNumber, context: nil, characters: typed, charactersIgnoringModifiers: chars,
                                        isARepeat: repeating, keyCode: code) else { return "no event" }
        if keyDown(ev) { return nil }
        if flags.contains(.command), NSApp.mainMenu?.performKeyEquivalent(with: ev) == true { return nil }
        w.sendEvent(ev) // its views' key equivalents, then its first responder
        if let up = NSEvent.keyEvent(with: .keyUp, location: .zero, modifierFlags: flags, timestamp: ProcessInfo.processInfo.systemUptime,
                                     windowNumber: w.windowNumber, context: nil, characters: typed, charactersIgnoringModifiers: chars,
                                     isARepeat: false, keyCode: code) { w.sendEvent(up) }
        return nil
    }

    /// A drag across rich texts, as the window server sends it: the press,
    /// then the drag and the release waiting in the queue for the
    /// selection's tracking loop.
    func selectDrag(_ from: String, _ to: String, clicks: Int) -> String? {
        let a = AsukaAutomation.shared
        func rich(_ s: String) -> NSTextView? {
            for n in nodes.values where a.showing(n) {
                // Its text, as the automation's JSON says it: without the
                // inline components' attachments.
                if let r = n.rich, r.view.textStorage?.string.replacingOccurrences(of: "\u{FFFC}", with: "") == s { return r.view }
            }
            return nil
        }
        guard let v1 = rich(from), let v2 = rich(to), let w = v1.window else { return "no rich text says \"\(from)\" or \"\(to)\"" }
        let start = v1.convert(CGPoint(x: 1, y: v1.bounds.midY), to: nil)
        let end = v2.convert(CGPoint(x: v2.bounds.maxX - 1, y: v2.bounds.midY), to: nil)
        let t = ProcessInfo.processInfo.systemUptime
        func ev(_ type: NSEvent.EventType, _ p: CGPoint) -> NSEvent? {
            NSEvent.mouseEvent(with: type, location: p, modifierFlags: [], timestamp: t, windowNumber: w.windowNumber,
                               context: nil, eventNumber: 0, clickCount: clicks, pressure: 1)
        }
        guard let down = ev(.leftMouseDown, start), let drag = ev(.leftMouseDragged, end), let up = ev(.leftMouseUp, end) else { return "no events" }
        NSApp.postEvent(drag, atStart: false)
        NSApp.postEvent(up, atStart: false)
        w.sendEvent(down)
        return nil
    }

    /// Chooses an item of the context menu of the row that shows text.
    func rowAction(_ text: String, _ title: String) -> String? {
        let a = AsukaAutomation.shared
        for n in nodes.values where n.text == text && a.showing(n) {
            var x: AsukaNode? = n
            while let c = x, c.cell == nil { x = c.parent }
            guard let cn = x, let cell = cn.cell else { continue }
            guard let m = cellMenu(cell) else { return "the row has no context menu" }
            guard let i = m.items.firstIndex(where: { $0.title == title }) else { return "no \"\(title)\" in the row's menu" }
            m.performActionForItem(at: i)
            return nil
        }
        return "no row shows \"\(text)\""
    }

    /// Clicks the middle of what shows text (a node's text, title or
    /// label), as the window server would send it: AppKit hit-tests the
    /// window. cmdclick holds ⌘.
    func click(_ text: String, _ how: String) -> String? {
        let a = AsukaAutomation.shared
        var found: AsukaNode?
        for n in nodes.values where a.showing(n) {
            let match = n.text == text && n.kind != .textField || n.title == text || n.a11yLabel == text
            if match && (found == nil || n.id > found!.id) { found = n }
        }
        guard let n = found, let v = n.view, let w = v.window else { return "nothing shows \"\(text)\"" }
        let p = v.convert(CGPoint(x: v.bounds.midX, y: v.bounds.midY), to: nil)
        let flags: NSEvent.ModifierFlags = how == "cmdclick" ? .command : []
        func ev(_ t: NSEvent.EventType) -> NSEvent? {
            NSEvent.mouseEvent(with: t, location: p, modifierFlags: flags, timestamp: ProcessInfo.processInfo.systemUptime,
                               windowNumber: w.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)
        }
        guard let down = ev(.leftMouseDown), let up = ev(.leftMouseUp) else { return "no mouse events" }
        // The up waits in the queue, for a control's tracking loop to take.
        NSApp.postEvent(up, atStart: false)
        w.sendEvent(down)
        return nil
    }
}

/// An alignment's name, for the automation's JSON (NSTextAlignment's raw
/// values differ between Intel and Apple silicon).
func asukaAlignName(_ a: NSTextAlignment) -> String {
    switch a {
    case .left: return "left"
    case .right: return "right"
    case .center: return "center"
    case .justified: return "justified"
    case .natural: return "natural"
    @unknown default: return "\(a.rawValue)"
    }
}

func asukaRect(_ r: CGRect) -> [Double] { [Double(r.minX), Double(r.minY), Double(r.width), Double(r.height)] }

/// A link attribute's value as a string (a URL's absolute string).
func asukaLinkString(_ v: Any?) -> String {
    switch v {
    case let u as URL: return u.absoluteString
    case let u as NSURL: return u.absoluteString ?? ""
    case let s as String: return s
    case nil: return ""
    default: return String(describing: v!)
    }
}

/// A text's runs for the automation: each run's text, and its styles.
@MainActor func asukaRunsJSON(_ ts: NSAttributedString) -> [[String: Any]] {
    var out: [[String: Any]] = []
    let s = ts.string as NSString
    ts.enumerateAttributes(in: NSRange(location: 0, length: ts.length)) { attrs, range, _ in
        var j: [String: Any] = ["text": s.substring(with: range)]
        if let f = attrs[.font] as? NSFont {
            let t = NSFontManager.shared.traits(of: f)
            j["bold"] = t.contains(.boldFontMask)
            j["italic"] = t.contains(.italicFontMask)
            j["mono"] = f.isFixedPitch
            j["size"] = Double(f.pointSize)
        }
        if let u = attrs[.underlineStyle] as? Int, u != 0 { j["underline"] = true }
        if let u = attrs[.strikethroughStyle] as? Int, u != 0 { j["strike"] = true }
        if let l = attrs[.link] { j["link"] = asukaLinkString(l) }
        if let c = attrs[.foregroundColor] as? NSColor { j["color"] = colorHex(c) }
        if let c = attrs[.backgroundColor] as? NSColor { j["background"] = colorHex(c) }
        if attrs[.attachment] != nil { j["attachment"] = true }
        out.append(j)
    }
    return out
}

/// The window the automation took the focus from last (its unfocus step).
@MainActor var asukaKeyTarget: NSWindow?
