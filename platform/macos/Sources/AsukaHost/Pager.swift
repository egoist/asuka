// Paging Scrolls and PageControls.
//
// AppKit has neither: a paging scroll view settles on the nearest page when
// a live scroll (a drag, a swipe and its momentum) ends, and a PageControl
// is drawn, its dots as iOS's, the current one in the accent color.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A dot's diameter, a dot and its gap, the control's height.
private let dotSize: CGFloat = 7
private let dotCell: CGFloat = 14
private let dotsHigh: CGFloat = 24

/// A PageControl's state.
@MainActor final class AsukaPageDots {
    var count = 0
    var page = 0
}

/// A PageControl's view: its dots drawn, a click on one chooses it,
/// VoiceOver adjusts it, as a slider.
final class AsukaPageDotsView: NSView {
    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    @MainActor var rt: AsukaRuntime { AsukaRuntime.shared }

    override func draw(_ dirty: NSRect) {
        if let n = rt.node(of: self), n.dots != nil { rt.drawDots(n) }
    }

    override func mouseDown(with event: NSEvent) {
        guard let n = rt.node(of: self), let d = n.dots, d.count > 0 else { return }
        let p = convert(event.locationInWindow, from: nil)
        rt.chooseDot(n, rt.dotAt(n, p.x))
    }

    override func accessibilityPerformIncrement() -> Bool {
        if let n = rt.node(of: self), let d = n.dots { rt.chooseDot(n, d.page + 1) }
        return true
    }

    override func accessibilityPerformDecrement() -> Bool {
        if let n = rt.node(of: self), let d = n.dots { rt.chooseDot(n, d.page - 1) }
        return true
    }
}

/// The observer of paging scroll views' live scrolls ending.
@MainActor final class AsukaPagingObserver: NSObject {
    static let shared = AsukaPagingObserver()

    @objc func liveScrollEnded(_ note: Notification) {
        guard let sv = note.object as? NSScrollView else { return }
        AsukaRuntime.shared.settlePage(sv)
    }
}

@MainActor extension AsukaRuntime {
    func makePageDots(_ n: AsukaNode) -> NSView {
        n.dots = AsukaPageDots()
        let v = AsukaPageDotsView(frame: .zero)
        v.setAccessibilityElement(true)
        v.setAccessibilityRole(.slider)
        return v
    }

    func setPageDotsProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let d = n.dots else { return }
        switch p {
        case AsukaProp.PageControl.count: d.count = max(Int(v.int), 0)
        case AsukaProp.PageControl.page: d.page = Int(v.int)
        default: break
        }
        if d.count > 0 { n.view?.setAccessibilityValue("\(d.page + 1)/\(d.count)") }
        n.view?.needsDisplay = true
    }

    func measureDots(_ n: AsukaNode) -> CGSize {
        CGSize(width: CGFloat(n.dots?.count ?? 0) * dotCell + dotCell, height: dotsHigh)
    }

    /// The dot under x, in the reading direction.
    func dotAt(_ n: AsukaNode, _ x: CGFloat) -> Int {
        guard let d = n.dots, let v = n.view else { return 0 }
        var i = Int(((x - (v.bounds.width - CGFloat(d.count) * dotCell) / 2) / dotCell).rounded(.down))
        if isRTL(n) { i = d.count - 1 - i }
        return min(max(i, 0), d.count - 1)
    }

    /// The middle of dot i, in the view (the automation clicks it).
    func dotCenter(_ n: AsukaNode, _ i: Int) -> CGPoint {
        guard let d = n.dots, let v = n.view else { return .zero }
        let slot = isRTL(n) ? d.count - 1 - i : i
        let x0 = (v.bounds.width - CGFloat(d.count) * dotCell) / 2
        return CGPoint(x: x0 + CGFloat(slot) * dotCell + dotCell / 2, y: v.bounds.height / 2)
    }

    func chooseDot(_ n: AsukaNode, _ i: Int) {
        guard let d = n.dots, i >= 0, i < d.count, i != d.page else { return }
        d.page = i
        n.view?.needsDisplay = true
        emit(n, .selected) { AsukaEvents.selected($0, Int64(i)) }
    }

    func drawDots(_ n: AsukaNode) {
        guard let d = n.dots, let v = n.view else { return }
        let b = v.bounds
        let x0 = (b.width - CGFloat(d.count) * dotCell) / 2 + (dotCell - dotSize) / 2
        let y = (b.height - dotSize) / 2
        let rtl = isRTL(n)
        for i in 0..<d.count {
            let slot = rtl ? d.count - 1 - i : i
            (i == d.page ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor).set()
            NSBezierPath(ovalIn: CGRect(x: x0 + CGFloat(slot) * dotCell, y: y, width: dotSize, height: dotSize)).fill()
        }
    }

    /// Makes a Scroll settle on whole pages, its scroll bars hidden.
    func setPaging(_ n: AsukaNode, _ on: Bool) {
        guard on != n.paging else { return }
        n.paging = on
        guard let sv = n.view as? NSScrollView else { return }
        sv.hasHorizontalScroller = !on && n.horizontal
        sv.hasVerticalScroller = !on && !n.horizontal
        let c = NotificationCenter.default, o = AsukaPagingObserver.shared
        if on {
            c.addObserver(o, selector: #selector(AsukaPagingObserver.liveScrollEnded(_:)), name: NSScrollView.didEndLiveScrollNotification, object: sv)
        } else {
            c.removeObserver(o, name: NSScrollView.didEndLiveScrollNotification, object: sv)
        }
    }

    /// Scrolls a paging Scroll to the page nearest what it shows, animated,
    /// and reports it.
    func settlePage(_ sv: NSScrollView) {
        guard let n = node(of: sv), n.paging else { return }
        let b = sv.contentView.bounds
        let size = n.horizontal ? b.width : b.height
        let off = n.horizontal ? b.minX : b.minY
        guard size > 0 else { return }
        let page = (off / size).rounded()
        scrollToOffset(n, n.horizontal ? page * size : 0, n.horizontal ? 0 : page * size, true)
        emit(n, .paged) { AsukaEvents.paged($0, Int64(page)) }
    }

    /// A Scroll's (or a List's) scroll view for the automation: its
    /// content's frame, what it shows, its vertical scroller.
    func scrollJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard n.kind == .scroll || n.kind == .list, let sv = n.view as? NSScrollView else { return }
        var j: [String: Any] = ["clip": asukaRect(sv.contentView.bounds), "horizontal": n.horizontal,
                                "hasVerticalScroller": sv.hasVerticalScroller, "hasHorizontalScroller": sv.hasHorizontalScroller]
        if let doc = sv.documentView { j["doc"] = asukaRect(doc.frame) }
        zoomJSON(n, &j)
        if let s = sv.verticalScroller {
            sv.tile()
            j["verticalScroller"] = asukaRect(s.frame)
        }
        d["scroll"] = j
    }

    func dotsJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        if let p = n.dots {
            d["dots"] = ["count": p.count, "page": p.page, "a11yValue": (n.view?.accessibilityValue() as? String) ?? ""]
        }
        if n.paging { d["paging"] = true }
    }
}
