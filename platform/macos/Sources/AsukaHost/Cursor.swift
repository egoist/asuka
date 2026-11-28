// The pointer's shape (Cursor): a view that sets one shows it over itself
// and the views inside it that set none, as CSS's cursor is inherited.
//
// It's a cursor rect of the window's (NSView's addCursorRect), the view's
// bounds, registered by a view of the host's at the back of the view's
// subviews: a window's cursor rects are gathered from each view, then its
// subviews in order, the last that holds the pointer winning, so the views
// inside it that set their own (a descendant's Cursor, a text field's
// I-beam) win over it, and a text field that sets one wins over its own
// I-beam. AppKit has no busy cursor of its own (Wait and Progress are the
// arrow), and no move or help cursor: those, and macOS 14's missing
// diagonal resizes and zooms, are drawn from SF Symbols.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// The view that registers a node's cursor rect: it fills the node's
/// view, under its children, and takes no clicks.
final class AsukaCursorView: NSView {
    var cursor = NSCursor.arrow
    var name = "arrow"

    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func isAccessibilityElement() -> Bool { false }
    override func resetCursorRects() { addCursorRect(bounds, cursor: cursor) }
}

@MainActor extension AsukaRuntime {
    /// The Cursor prop: n's cursor rect, or none (Auto: its parent's).
    func setCursor(_ n: AsukaNode, _ v: Int32) {
        n.cursor = v
        guard let host = n.view else { return }
        let c = AsukaCursor(rawValue: v) ?? .auto
        if c == .auto {
            if let cv = n.cursorView {
                cv.removeFromSuperview()
                host.window?.invalidateCursorRects(for: host)
            }
            n.cursorView = nil
            return
        }
        let cv = n.cursorView ?? AsukaCursorView(frame: host.bounds)
        (cv.cursor, cv.name) = asukaCursor(c)
        if cv.superview !== host {
            cv.removeFromSuperview()
            host.addSubview(cv, positioned: .below, relativeTo: nil)
        }
        cv.frame = host.bounds
        cv.autoresizingMask = [.width, .height]
        n.cursorView = cv
        host.window?.invalidateCursorRects(for: cv)
    }

    /// n has a new frame: its cursor rect follows it.
    func cursorPlaced(_ n: AsukaNode) {
        guard let cv = n.cursorView, let host = cv.superview else { return }
        if cv.frame != host.bounds { cv.frame = host.bounds }
    }

    /// The cursor the pointer shows over n, as its cursor rects and its
    /// ancestors' give it: its own, a text field's I-beam, else the nearest
    /// ancestor's (nil: AppKit's own, the arrow).
    func effectiveCursor(_ n: AsukaNode) -> String? {
        var x: AsukaNode? = n
        while let c = x {
            if let cv = c.cursorView { return cv.name }
            if c === n, c.kind == .textField, (c.view as? NSTextField)?.isEditable ?? (c.textView?.isEditable ?? false) { return "iBeam" }
            x = c.parent
        }
        return nil
    }

    func cursorJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        if let cv = n.cursorView {
            d["cursor"] = cv.name
            d["cursorRect"] = asukaRect(cv.frame)
            d["cursorFirst"] = cv.superview?.subviews.first === cv
        }
        if let e = effectiveCursor(n) { d["effectiveCursor"] = e }
    }
}

/// A Cursor as AppKit's cursor, and its name (the automation's).
@MainActor func asukaCursor(_ c: AsukaCursor) -> (NSCursor, String) {
    switch c {
    case .auto, .arrow: return (.arrow, "arrow")
    case .pointer: return (.pointingHand, "pointingHand")
    case .text: return (.iBeam, "iBeam")
    case .crosshair: return (.crosshair, "crosshair")
    case .move: return (asukaSymbolCursor("arrow.up.and.down.and.arrow.left.and.right") ?? .openHand, "move")
    case .grab: return (.openHand, "openHand")
    case .grabbing: return (.closedHand, "closedHand")
    case .notAllowed: return (.operationNotAllowed, "operationNotAllowed")
    case .wait, .progress: return (.arrow, "arrow") // AppKit has no busy cursor for apps
    case .help: return (asukaHelpCursor() ?? .arrow, "help")
    case .contextMenu: return (.contextualMenu, "contextualMenu")
    case .copy: return (.dragCopy, "dragCopy")
    case .alias: return (.dragLink, "dragLink")
    case .resizeHorizontal:
        if #available(macOS 15.0, *) { return (.frameResize(position: .right, directions: .all), "frameResizeHorizontal") }
        return (.resizeLeftRight, "resizeLeftRight")
    case .resizeVertical:
        if #available(macOS 15.0, *) { return (.frameResize(position: .top, directions: .all), "frameResizeVertical") }
        return (.resizeUpDown, "resizeUpDown")
    case .resizeDiagonal:
        if #available(macOS 15.0, *) { return (.frameResize(position: .topLeft, directions: .all), "frameResizeDiagonal") }
        return (asukaSymbolCursor("arrow.up.left.and.arrow.down.right") ?? .crosshair, "resizeDiagonal")
    case .resizeAntiDiagonal:
        if #available(macOS 15.0, *) { return (.frameResize(position: .topRight, directions: .all), "frameResizeAntiDiagonal") }
        return (asukaSymbolCursor("arrow.up.right.and.arrow.down.left") ?? .crosshair, "resizeAntiDiagonal")
    case .resizeColumn:
        if #available(macOS 15.0, *) { return (.columnResize, "columnResize") }
        return (.resizeLeftRight, "resizeLeftRight")
    case .resizeRow:
        if #available(macOS 15.0, *) { return (.rowResize, "rowResize") }
        return (.resizeUpDown, "resizeUpDown")
    case .zoomIn:
        if #available(macOS 15.0, *) { return (.zoomIn, "zoomIn") }
        return (asukaSymbolCursor("plus.magnifyingglass") ?? .crosshair, "zoomIn")
    case .zoomOut:
        if #available(macOS 15.0, *) { return (.zoomOut, "zoomOut") }
        return (asukaSymbolCursor("minus.magnifyingglass") ?? .crosshair, "zoomOut")
    case .hidden: return (asukaHiddenCursor, "hidden")
    }
}

/// The cursors drawn from symbols, by name.
@MainActor private var asukaSymbolCursors: [String: NSCursor] = [:]

/// A cursor of an SF Symbol: black with a white edge, as the system's,
/// its hot spot in the middle.
@MainActor func asukaSymbolCursor(_ name: String) -> NSCursor? {
    if let c = asukaSymbolCursors[name] { return c }
    guard let sym = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
        .withSymbolConfiguration(.init(pointSize: 13, weight: .bold)) else { return nil }
    let size = CGSize(width: 24, height: 24)
    let img = NSImage(size: size, flipped: false) { r in
        let s = sym.size
        let at = CGRect(x: (r.width - s.width) / 2, y: (r.height - s.height) / 2, width: s.width, height: s.height)
        func draw(_ color: NSColor, _ dx: CGFloat, _ dy: CGFloat) {
            let tinted = NSImage(size: s, flipped: false) { tr in
                sym.draw(in: tr)
                color.set()
                tr.fill(using: .sourceAtop)
                return true
            }
            tinted.draw(in: at.offsetBy(dx: dx, dy: dy))
        }
        for (dx, dy) in [(-1.0, 0.0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, 1), (-1, 1), (1, -1)] { draw(.white, dx, dy) }
        draw(.black, 0, 0)
        return true
    }
    let c = NSCursor(image: img, hotSpot: CGPoint(x: size.width / 2, y: size.height / 2))
    asukaSymbolCursors[name] = c
    return c
}

/// The arrow with a question mark beside it.
@MainActor func asukaHelpCursor() -> NSCursor? {
    if let c = asukaSymbolCursors["help"] { return c }
    let arrow = NSCursor.arrow
    let a = arrow.image
    guard let q = NSImage(systemSymbolName: "questionmark.circle.fill", accessibilityDescription: nil)?
        .withSymbolConfiguration(.init(pointSize: 11, weight: .semibold)) else { return nil }
    let size = CGSize(width: a.size.width + 12, height: a.size.height + 6)
    let img = NSImage(size: size, flipped: true) { _ in
        a.draw(in: CGRect(origin: .zero, size: a.size))
        let qs = q.size
        let at = CGRect(x: size.width - qs.width - 1, y: size.height - qs.height - 1, width: qs.width, height: qs.height)
        NSColor.white.setFill()
        NSBezierPath(ovalIn: at.insetBy(dx: -1, dy: -1)).fill()
        let tinted = NSImage(size: qs, flipped: false) { tr in
            q.draw(in: tr)
            NSColor.black.set()
            tr.fill(using: .sourceAtop)
            return true
        }
        tinted.draw(in: at)
        return true
    }
    let c = NSCursor(image: img, hotSpot: arrow.hotSpot)
    asukaSymbolCursors["help"] = c
    return c
}

/// A cursor that draws nothing: the pointer hidden while it's over the view.
@MainActor let asukaHiddenCursor: NSCursor = {
    let img = NSImage(size: CGSize(width: 16, height: 16), flipped: false) { _ in true }
    return NSCursor(image: img, hotSpot: .zero)
}()
