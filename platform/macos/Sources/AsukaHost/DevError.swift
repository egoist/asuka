// asuka dev's error overlay on a Mac (the DevError command, to the root:
// ops/deverror.go): an error the app didn't catch, a module that didn't
// load, a build that failed. It's AppKit's own views, not the app's (the
// app's tree may be what's broken):
//
//   - A borderless panel over the main window's content (its content
//     layout rect: the title bar and toolbar stay the window's), its
//     bottom corners the window's: a child window of the main window, or
//     of its front sheet (an alert's too) while one shows, as AppKit keeps
//     a window's sheet over its other children; it moves to the front one
//     as sheets begin and end. A child window moves with its window, goes
//     into full screen and out with it, and is ordered above it; it works
//     while a sheet shows (a sheet blocks only its window) and during a
//     modal session. It follows the main window's content layout rect as
//     the window resizes or its toolbar changes. (A popover's window, at
//     its own level, can still be over it.)
//   - In it, scrolling: the title (small, semibold, red; orange for a
//     build), the message (large, semibold, wrapping), the source (the
//     lines where it went wrong: monospaced in a rounded box, not wrapping,
//     scrolling sideways) and the details (the stack, or the build's other
//     errors: monospaced, secondary), all selectable; under them a bar
//     with Dismiss and, when the core offers it, Reload (the default
//     button: Return). Escape dismisses it.
//   - Its buttons send DevErrorAction to the root; the core answers with
//     DevErrorClear (or another DevError), which removes it (or replaces
//     what it shows, in place).
//   - The automation sees it (Automation.swift): its buttons tap, its texts
//     are in the dumps, and the JSON tree has it as devError.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A DevError command's arguments (ops.DecodeDevError).
struct AsukaDevError: Equatable {
    static let reloadFlag: UInt8 = 1 // ops.DevErrorReload: it offers Reload
    static let buildFlag: UInt8 = 2 // ops.DevErrorBuild: a build that failed

    var flags: UInt8 = 0
    var title = ""
    var message = ""
    var source = ""
    var details = ""

    var reload: Bool { flags & Self.reloadFlag != 0 }
    var build: Bool { flags & Self.buildFlag != 0 }

    /// u8 flags, then the title, the message, the source and the details,
    /// u32-length each; nil when it's cut short.
    static func decode(_ b: UnsafeRawBufferPointer) -> AsukaDevError? {
        var i = 0
        func u32() -> Int? {
            guard i + 4 <= b.count else { return nil }
            let v = UInt32(b[i]) | UInt32(b[i + 1]) << 8 | UInt32(b[i + 2]) << 16 | UInt32(b[i + 3]) << 24
            i += 4
            return Int(v)
        }
        func string() -> String? {
            guard let n = u32(), n <= b.count - i else { return nil }
            let s = String(decoding: UnsafeRawBufferPointer(rebasing: b[i..<i + n]), as: UTF8.self)
            i += n
            return s
        }
        guard b.count >= 1 else { return nil }
        var e = AsukaDevError(flags: b[0])
        i = 1
        guard let t = string(), let m = string(), let s = string(), let d = string() else { return nil }
        e.title = t
        e.message = m
        e.source = s
        e.details = d
        return e
    }
}

/// The overlay's window: borderless, it takes the keyboard (Return,
/// Escape, copying a selection) but never becomes the main window, and
/// works while a sheet or a modal session holds the app.
final class AsukaDevErrorPanel: NSPanel {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }

    override func sendEvent(_ event: NSEvent) {
        // Escape dismisses it, whatever has the focus (a text view's
        // selection would take it as complete:).
        if event.type == .keyDown, event.keyCode == 0x35,
           event.modifierFlags.intersection([.command, .option, .control, .shift]).isEmpty {
            MainActor.assumeIsolated { AsukaDevErrors.shared.act(.dismiss) }
            return
        }
        super.sendEvent(event)
    }

    override func cancelOperation(_ sender: Any?) {
        MainActor.assumeIsolated { AsukaDevErrors.shared.act(.dismiss) }
    }
}

/// The overlay's content: the window's background, its scrolling body and
/// its bar, laid out as it's sized.
final class AsukaDevErrorView: NSView {
    override var isFlipped: Bool { true }
    override var isOpaque: Bool { false } // its bottom corners are the window's

    override func draw(_ dirtyRect: NSRect) {
        NSColor.windowBackgroundColor.setFill()
        dirtyRect.fill()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        MainActor.assumeIsolated { AsukaDevErrors.shared.place() }
    }
}

/// The source's box: a fill a little darker (lighter, dark) than the
/// window's, rounded.
final class AsukaDevErrorBox: NSView {
    override var isFlipped: Bool { true }
    override var isOpaque: Bool { false }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.quaternaryLabelColor.withAlphaComponent(0.07).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
    }
}

/// The scrolling body, and the bar: flipped, laid out from the top.
final class AsukaDevErrorDoc: NSView {
    override var isFlipped: Bool { true }
}

/// A scroll view that scrolls sideways only: the scroll wheel's vertical
/// motion goes on to the scroll view it's in.
final class AsukaDevErrorSideScroll: NSScrollView {
    override func scrollWheel(with event: NSEvent) {
        if abs(event.scrollingDeltaY) > abs(event.scrollingDeltaX) {
            nextResponder?.scrollWheel(with: event)
        } else {
            super.scrollWheel(with: event)
        }
    }
}

/// The overlay, while it shows (one: it's over the main window).
@MainActor final class AsukaDevErrors: NSObject {
    static let shared = AsukaDevErrors()

    /// What it shows.
    private(set) var error: AsukaDevError?
    /// The root its actions go to.
    private(set) var root: UInt32 = 0
    private(set) var panel: AsukaDevErrorPanel?
    /// The window it's over.
    private(set) weak var parent: NSWindow?
    private var observation: NSKeyValueObservation?
    private var observers: [Any] = []

    // Its views.
    private var scroll: NSScrollView?
    private var body: AsukaDevErrorDoc?
    private var titleLabel: NSTextField?
    private var messageText: NSTextView?
    private var sourceBox: AsukaDevErrorBox?
    private var sourceScroll: NSScrollView?
    private var sourceText: NSTextView?
    private var detailsText: NSTextView?
    private var bar: NSView?
    private var separator: NSBox?
    private(set) var buttons: [NSButton] = []

    static let pad: CGFloat = 20
    static let barHeight: CGFloat = 52
    static let sourceInset = NSSize(width: 12, height: 10)

    var showing: Bool { panel?.isVisible ?? false }

    // MARK: Showing it

    /// Shows e over window w, its actions to root: in place, if it shows.
    func show(_ e: AsukaDevError, root: UInt32, over w: NSWindow) {
        if panel == nil || parent !== w {
            clear()
            make(over: w)
        }
        self.root = root
        error = e
        fill(e)
        attach()
    }

    /// Removes it, if it shows.
    func clear() {
        observation = nil
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers = []
        guard let p = panel else { return }
        let wasKey = p.isKeyWindow
        p.parent?.removeChildWindow(p)
        p.orderOut(nil)
        p.close()
        panel = nil
        scroll = nil; body = nil; titleLabel = nil; messageText = nil; sourceBox = nil
        sourceScroll = nil; sourceText = nil; detailsText = nil; bar = nil; separator = nil
        buttons = []
        error = nil
        if wasKey, let w = parent, w.isVisible { w.makeKey() }
        parent = nil
    }

    private func make(over w: NSWindow) {
        parent = w
        let p = AsukaDevErrorPanel(contentRect: frame(over: w), styleMask: [.borderless], backing: .buffered, defer: false)
        p.isReleasedWhenClosed = false
        p.hidesOnDeactivate = false
        p.worksWhenModal = true
        p.becomesKeyOnlyIfNeeded = false
        p.isFloatingPanel = false
        p.hasShadow = false
        p.isOpaque = false
        p.backgroundColor = .clear
        p.animationBehavior = .none
        p.collectionBehavior = [.fullScreenAuxiliary]
        p.title = "asuka dev"
        p.setAccessibilityRole(.window)
        p.setAccessibilitySubrole(.dialog)

        let content = AsukaDevErrorView(frame: CGRect(origin: .zero, size: p.frame.size))
        content.wantsLayer = true
        content.layer?.masksToBounds = true
        content.setAccessibilityElement(true)
        content.setAccessibilityRole(.group)
        p.contentView = content

        let sv = NSScrollView()
        sv.drawsBackground = false
        sv.hasVerticalScroller = true
        sv.autohidesScrollers = true
        sv.borderType = .noBorder
        let doc = AsukaDevErrorDoc()
        sv.documentView = doc
        content.addSubview(sv)
        scroll = sv
        body = doc

        let title = NSTextField(labelWithString: "")
        title.font = .systemFont(ofSize: 12, weight: .semibold)
        title.isSelectable = true
        title.lineBreakMode = .byTruncatingTail
        doc.addSubview(title)
        titleLabel = title

        let large = NSFont.preferredFont(forTextStyle: .title2).pointSize
        let message = textView(.systemFont(ofSize: large, weight: .semibold), .labelColor, wraps: true)
        doc.addSubview(message)
        messageText = message

        let box = AsukaDevErrorBox()
        let side = AsukaDevErrorSideScroll()
        side.drawsBackground = false
        side.hasHorizontalScroller = true
        side.hasVerticalScroller = false
        side.autohidesScrollers = true
        side.borderType = .noBorder
        let source = textView(.monospacedSystemFont(ofSize: 12, weight: .regular), .labelColor, wraps: false)
        source.textContainerInset = Self.sourceInset
        side.documentView = source
        box.addSubview(side)
        doc.addSubview(box)
        sourceBox = box
        sourceScroll = side
        sourceText = source

        let details = textView(.monospacedSystemFont(ofSize: 11, weight: .regular), .secondaryLabelColor, wraps: true)
        doc.addSubview(details)
        detailsText = details

        let b = AsukaDevErrorDoc() // flipped: the separator at its top
        let sep = NSBox()
        sep.boxType = .separator
        b.addSubview(sep)
        content.addSubview(b)
        bar = b
        separator = sep
        panel = p

        // It follows the window: its content layout rect (a resize, a
        // toolbar), and full screen's corners.
        observation = w.observe(\.contentLayoutRect, options: []) { _, _ in
            MainActor.assumeIsolated { AsukaDevErrors.shared.follow() }
        }
        for name in [NSWindow.didEnterFullScreenNotification, NSWindow.didExitFullScreenNotification, NSWindow.didResizeNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: w, queue: .main) { _ in
                MainActor.assumeIsolated { AsukaDevErrors.shared.follow() }
            })
        }
        // Its sheets' (and theirs): it goes over the front one.
        for name in [NSWindow.willBeginSheetNotification, NSWindow.didEndSheetNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { note in
                MainActor.assumeIsolated { AsukaDevErrors.shared.sheetsChanged(note) }
            })
        }
    }

    /// A text view that's selectable, not editable, its background the
    /// window's.
    private func textView(_ font: NSFont, _ color: NSColor, wraps: Bool) -> NSTextView {
        let tv = NSTextView(frame: CGRect(x: 0, y: 0, width: 100, height: 20))
        tv.isEditable = false
        tv.isSelectable = true
        tv.isRichText = false
        tv.drawsBackground = false
        tv.font = font
        tv.textColor = color
        tv.textContainerInset = .zero
        tv.textContainer?.lineFragmentPadding = 0
        tv.isVerticallyResizable = false
        tv.isHorizontallyResizable = !wraps
        tv.textContainer?.widthTracksTextView = wraps
        if !wraps { tv.textContainer?.containerSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude) }
        return tv
    }

    /// Sets its texts and buttons to e's.
    private func fill(_ e: AsukaDevError) {
        titleLabel?.stringValue = e.title
        titleLabel?.textColor = e.build ? .systemOrange : .systemRed
        messageText?.string = e.message
        sourceText?.string = e.source
        sourceBox?.isHidden = e.source.isEmpty
        detailsText?.string = e.details
        detailsText?.isHidden = e.details.isEmpty
        panel?.setAccessibilityLabel(e.title)

        for b in buttons { b.removeFromSuperview() }
        buttons = []
        buttons = [button("Dismiss", .dismiss)]
        if e.reload { buttons.append(button("Reload", .reload)) }
        defaultButton()
        for b in buttons { bar?.addSubview(b) }
        place()
        scroll?.contentView.scroll(to: .zero)
        if let s = scroll { s.reflectScrolledClipView(s.contentView) }
    }

    private func button(_ title: String, _ a: AsukaDevErrorAction) -> NSButton {
        let b = NSButton(title: title, target: self, action: #selector(pressed(_:)))
        b.bezelStyle = .rounded
        b.tag = Int(a.rawValue)
        return b
    }

    @objc func pressed(_ b: NSButton) {
        act(AsukaDevErrorAction(rawValue: Int32(b.tag)) ?? .dismiss)
    }

    /// Sends the action to the root; the core says what shows next.
    func act(_ a: AsukaDevErrorAction) {
        guard showing, root != 0 else { return }
        if a == .reload && !(error?.reload ?? false) { return }
        AsukaRuntime.shared.send(AsukaEvents.devErrorAction(root, Int64(a.rawValue)))
    }

    /// Orders it over its window and the window's sheets: a child window
    /// of the front one (AppKit keeps a window's sheet over its other
    /// children), and gives it the keyboard (Return, Escape).
    func attach() {
        guard let p = panel, let w = parent else { return }
        follow()
        var front = w
        while let s = front.attachedSheet { front = s }
        if p.parent !== front {
            p.parent?.removeChildWindow(p)
            front.addChildWindow(p, ordered: .above)
        }
        p.orderFront(nil)
        p.makeKey()
        p.makeFirstResponder(nil)
        defaultButton() // a window ordered in as a child loses it
    }

    /// Gives Return to the default button: Reload, else Dismiss.
    private func defaultButton() {
        for (i, b) in buttons.enumerated() { b.keyEquivalent = i == buttons.count - 1 ? "\r" : "" }
    }

    /// A sheet began or ended on its window (or on a sheet over it): it
    /// goes over the front one, once AppKit has ordered it.
    private func sheetsChanged(_ note: Notification) {
        guard let w = parent, let x = note.object as? NSWindow else { return }
        var on: NSWindow? = x
        while let y = on, y !== w { on = y.sheetParent }
        guard on === w else { return }
        DispatchQueue.main.async { MainActor.assumeIsolated { if AsukaDevErrors.shared.parent === w { AsukaDevErrors.shared.attach() } } }
    }

    /// Covers its window's content layout rect, its bottom corners the
    /// window's.
    func follow() {
        guard let p = panel, let w = parent else { return }
        let f = frame(over: w)
        if p.frame != f { p.setFrame(f, display: true) }
        var radius: CGFloat = 0
        if !w.styleMask.contains(.fullScreen), w.responds(to: Selector(("_cornerRadius"))), let r = w.value(forKey: "_cornerRadius") as? CGFloat {
            radius = r
        }
        if let l = p.contentView?.layer {
            l.cornerRadius = radius
            l.maskedCorners = [.layerMinXMaxYCorner, .layerMaxXMaxYCorner] // the bottom's: the view (its layer) is flipped
        }
        place()
    }

    private func frame(over w: NSWindow) -> CGRect {
        w.convertToScreen(w.contentLayoutRect)
    }

    // MARK: Layout

    /// Lays out its views at its size: the bar at the bottom, the body
    /// scrolling above it.
    func place() {
        guard let content = panel?.contentView, let sv = scroll, let doc = body, let bar else { return }
        let size = content.bounds.size
        let bh = Self.barHeight
        bar.frame = CGRect(x: 0, y: size.height - bh, width: size.width, height: bh)
        separator?.frame = CGRect(x: 0, y: 0, width: size.width, height: 1)
        // The buttons from the trailing edge, the default one last.
        let rtl = AsukaRuntime.shared.rtl
        var x = Self.pad
        for b in buttons.reversed() {
            b.sizeToFit()
            var f = b.frame
            f.size.width = max(f.width, 84)
            f.origin.y = ((bh - f.height) / 2).rounded()
            f.origin.x = rtl ? x : size.width - x - f.width
            b.frame = f
            x += f.width + 8
        }
        sv.frame = CGRect(x: 0, y: 0, width: size.width, height: max(size.height - bh, 0))
        let width = sv.contentSize.width
        let inner = max(width - 2 * Self.pad, 40)
        var y = Self.pad
        if let t = titleLabel {
            let h = t.intrinsicContentSize.height
            t.frame = CGRect(x: Self.pad, y: y, width: inner, height: h)
            y += h + 6
        }
        if let m = messageText {
            let h = height(m, inner)
            m.frame = CGRect(x: Self.pad, y: y, width: inner, height: h)
            y += h + 16
        }
        if let box = sourceBox, let side = sourceScroll, let src = sourceText, !box.isHidden {
            let used = usedSize(src)
            let inset = Self.sourceInset
            let textH = used.height + 2 * inset.height
            let textW = used.width + 2 * inset.width
            var h = textH
            // A scroller that isn't over the content takes room under it.
            if textW > inner, side.scrollerStyle == .legacy {
                h += NSScroller.scrollerWidth(for: .regular, scrollerStyle: .legacy)
            }
            box.frame = CGRect(x: Self.pad, y: y, width: inner, height: h)
            side.frame = box.bounds
            src.frame = CGRect(x: 0, y: 0, width: max(textW, side.contentSize.width), height: textH)
            y += h + 16
        }
        if let d = detailsText, !d.isHidden {
            let h = height(d, inner)
            d.frame = CGRect(x: Self.pad, y: y, width: inner, height: h)
            y += h + 16
        }
        y += Self.pad - 16
        doc.frame = CGRect(x: 0, y: 0, width: width, height: max(y, sv.contentSize.height))
    }

    /// A wrapping text view's height at width.
    private func height(_ tv: NSTextView, _ width: CGFloat) -> CGFloat {
        guard let tc = tv.textContainer, let lm = tv.layoutManager else { return 0 }
        tc.containerSize = NSSize(width: width, height: CGFloat.greatestFiniteMagnitude)
        lm.ensureLayout(for: tc)
        return ceil(lm.usedRect(for: tc).height)
    }

    /// A text view's text's size, not wrapping.
    private func usedSize(_ tv: NSTextView) -> CGSize {
        guard let tc = tv.textContainer, let lm = tv.layoutManager else { return .zero }
        lm.ensureLayout(for: tc)
        let r = lm.usedRect(for: tc)
        return CGSize(width: ceil(r.width), height: ceil(r.height))
    }

    // MARK: Automation

    /// Presses the button titled text, if it shows: nil when none does.
    func tap(_ text: String) -> String? {
        guard showing, let b = buttons.first(where: { $0.title == text }) else { return nil }
        b.performClick(nil)
        return ""
    }

    /// What it shows, for the tree's dump: its title, then each text and
    /// button a line.
    func dump() -> String {
        guard showing, let e = error else { return "" }
        var b = "deverror \"\(e.title)\"" + (e.build ? " build" : "") + "\n"
        b += "  \"\(e.message)\"\n"
        for (name, s) in [("source", e.source), ("details", e.details)] where !s.isEmpty {
            b += "  \(name):\n"
            for l in s.split(separator: "\n", omittingEmptySubsequences: false) { b += "    \(l)\n" }
        }
        for btn in buttons { b += "  button \"\(btn.title)\"" + (btn.keyEquivalent == "\r" ? " default" : "") + "\n" }
        return b
    }

    /// What it shows, a line each, for the screen's texts.
    func texts() -> [String] {
        guard showing, let e = error else { return [] }
        return [e.title, e.message, e.source, e.details].filter { !$0.isEmpty } + buttons.map(\.title)
    }

    /// It in the JSON tree.
    func json() -> [String: Any]? {
        guard showing, let e = error, let p = panel else { return nil }
        var d: [String: Any] = ["title": e.title, "message": e.message, "source": e.source, "details": e.details,
                                "flags": Int(e.flags), "root": root, "frame": [Double(p.frame.minX), Double(p.frame.minY), Double(p.frame.width), Double(p.frame.height)],
                                "key": p.isKeyWindow, "buttons": buttons.map(\.title)]
        if let w = parent {
            d["over"] = w.title
            d["childOf"] = p.parent === w
        }
        if let t = titleLabel?.textColor { d["titleColor"] = colorHex(t) }
        return d
    }
}

@MainActor extension AsukaRuntime {
    /// DevError, to the root: shows (or replaces) the overlay over the main
    /// window.
    func devError(_ n: AsukaNode, _ args: UnsafeRawBufferPointer) {
        guard let e = AsukaDevError.decode(args) else { return }
        let w = winOf(n)?.window ?? main.window
        AsukaDevErrors.shared.show(e, root: n.id, over: w)
    }

    /// DevErrorClear: removes it.
    func devErrorClear() {
        AsukaDevErrors.shared.clear()
    }
}
