// Zoomable Scrolls (MinZoom, MaxZoom): NSScrollView's magnification.
//
// The core lays the content out at zoom 1 and sets its frame in the clip
// view's (unscaled) coordinates; NSScrollView scales the clip view's
// bounds, so the content's frame and the magnification never meet. A
// trackpad's pinch and its two-finger double tap (smart magnify) are the
// scroll view's own; ⌘ and the wheel zoom around the pointer, and a double
// click zooms in to twice MinZoom around it, or back out. Zoomed reports
// the user's zooming as it goes and where it settles; ZoomTo's isn't
// reported. Offsets (Scrolled, ScrollToOffset) are in the zoomed content's
// points, as UIScrollView's contentOffset.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Scroll's zoom: its bounds as the core sent them, the last zoom
/// reported.
@MainActor final class AsukaZoom {
    var min: CGFloat = 1
    var max: CGFloat = 1
    var reported: CGFloat = 1
    var observed = false

    var zoomable: Bool { max > min }
}

/// The observer of zoomable scroll views' live magnifications ending.
@MainActor final class AsukaZoomObserver: NSObject {
    static let shared = AsukaZoomObserver()

    @objc func magnifyEnded(_ note: Notification) {
        guard let sv = note.object as? NSScrollView else { return }
        AsukaRuntime.shared.userZoomed(sv, settled: true)
    }
}

@MainActor extension AsukaRuntime {
    /// A Scroll's MinZoom or MaxZoom.
    func setZoomProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        let z = n.zoom ?? AsukaZoom()
        n.zoom = z
        let f = CGFloat(v.float)
        if p == AsukaProp.Scroll.minZoom { z.min = f > 0 ? f : 1 } else { z.max = f > 0 ? f : 1 }
        applyZoom(n)
    }

    /// Whether n is a Scroll whose content zooms.
    func zoomable(_ n: AsukaNode) -> Bool { n.kind == .scroll && n.zoom?.zoomable == true }

    /// The scale n's content shows at (1 when it doesn't zoom).
    func zoomScale(_ n: AsukaNode) -> CGFloat {
        guard zoomable(n), let sv = n.view as? NSScrollView else { return 1 }
        return sv.magnification
    }

    /// Gives the scroll view its bounds; its zoom stays (1 at first),
    /// clamped to them, without a report.
    func applyZoom(_ n: AsukaNode) {
        guard let z = n.zoom, let sv = n.view as? NSScrollView else { return }
        if z.zoomable {
            sv.minMagnification = z.min
            sv.maxMagnification = z.max
            sv.allowsMagnification = true
            let m = Swift.min(Swift.max(sv.magnification, z.min), z.max)
            if m != sv.magnification { sv.magnification = m }
            // Zoomed in, it scrolls both ways.
            sv.hasHorizontalScroller = !n.paging
            sv.hasVerticalScroller = !n.paging
            if !z.observed {
                z.observed = true
                NotificationCenter.default.addObserver(AsukaZoomObserver.shared, selector: #selector(AsukaZoomObserver.magnifyEnded(_:)),
                                                       name: NSScrollView.didEndLiveMagnifyNotification, object: sv)
            }
        } else {
            sv.allowsMagnification = false
            if sv.magnification != 1 { sv.magnification = 1 }
            sv.hasHorizontalScroller = n.horizontal && !n.paging
            sv.hasVerticalScroller = !n.horizontal && !n.paging
        }
        z.reported = sv.magnification
    }

    /// Reports the user's zoom, if it changed (settled: where it rests).
    func userZoomed(_ sv: NSScrollView, settled: Bool = false) {
        guard let n = node(of: sv), let z = n.zoom, z.zoomable else { return }
        let m = sv.magnification
        guard m != z.reported else { return }
        z.reported = m
        emit(n, .zoomed) { AsukaEvents.zoomed($0, Double(m)) }
    }

    /// Zooms to m around p (the clip view's coordinates), animated or
    /// not.
    func zoom(_ sv: NSScrollView, to m: CGFloat, at p: CGPoint, animated: Bool) {
        if animated && !env.reduceMotion {
            NSAnimationContext.runAnimationGroup { ctx in
                ctx.duration = 0.25
                ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
                sv.animator().setMagnification(m, centeredAt: p)
            }
        } else {
            sv.setMagnification(m, centeredAt: p)
        }
    }

    /// ZoomTo: to the scale, around the middle of what it shows; not
    /// reported.
    func zoomTo(_ n: AsukaNode, _ args: UnsafeRawBufferPointer) {
        guard args.count >= 5, zoomable(n), let z = n.zoom, let sv = n.view as? NSScrollView else { return }
        let b = Array(args)
        let m = Swift.min(Swift.max(CGFloat(b.f32(0)), z.min), z.max)
        let c = sv.contentView.bounds
        zoom(sv, to: m, at: CGPoint(x: c.midX, y: c.midY), animated: b[4] != 0)
        z.reported = m
    }

    /// A double click on a zoomable Scroll's content: in to twice its
    /// least zoom around the click, or back out to it.
    func doubleClickZoom(_ n: AsukaNode, _ e: NSEvent) {
        guard let z = n.zoom, let sv = n.view as? NSScrollView else { return }
        let m = sv.magnification > z.min + 0.01 ? z.min : Swift.min(z.min * 2, z.max)
        zoom(sv, to: m, at: eventPoint(e, in: sv.contentView), animated: true)
        z.reported = m
        emit(n, .zoomed) { AsukaEvents.zoomed($0, Double(m)) }
    }

    /// A trackpad's two-finger double tap (smart magnify): the double
    /// click's zoom. False: n doesn't zoom (the scroll view's own).
    func smartZoom(_ sv: NSScrollView, _ e: NSEvent) -> Bool {
        guard let n = node(of: sv), zoomable(n) else { return false }
        doubleClickZoom(n, e)
        return true
    }

    /// ⌘ and the wheel (or a trackpad's scroll): zooms around the pointer.
    /// False: n doesn't zoom (the scroll goes on as a scroll).
    func wheelZoom(_ sv: NSScrollView, _ e: NSEvent) -> Bool {
        guard let n = node(of: sv), zoomable(n), let z = n.zoom else { return false }
        // The wheel rolled away from the user (the fingers pushed up) zooms
        // in, whichever way scrolling goes.
        var dy = e.scrollingDeltaY
        if e.isDirectionInvertedFromDevice { dy = -dy }
        guard dy != 0 else { return true }
        let k: CGFloat = e.hasPreciseScrollingDeltas ? 0.005 : 0.05
        let m = Swift.min(Swift.max(sv.magnification * exp(dy * k), z.min), z.max)
        guard m != sv.magnification else { return true }
        sv.setMagnification(m, centeredAt: eventPoint(e, in: sv.contentView))
        userZoomed(sv)
        return true
    }

    /// Where an event fell in v: one without a window (a test's) is
    /// placed on the screen.
    func eventPoint(_ e: NSEvent, in v: NSView) -> CGPoint {
        if e.window != nil || e.windowNumber != 0 { return v.convert(e.locationInWindow, from: nil) }
        guard let w = v.window else { return .zero }
        return v.convert(w.convertPoint(fromScreen: e.locationInWindow), from: nil)
    }

    func zoomJSON(_ n: AsukaNode, _ j: inout [String: Any]) {
        guard let z = n.zoom, let sv = n.view as? NSScrollView else { return }
        j["zoom"] = ["min": Double(z.min), "max": Double(z.max), "magnification": Double(sv.magnification),
                     "allows": sv.allowsMagnification, "minMagnification": Double(sv.minMagnification),
                     "maxMagnification": Double(sv.maxMagnification)]
    }
}
