#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Scroll: a UIScrollView that takes the rows of its content more than
/// a screen outside what it shows out of the view tree, as a collection
/// view does its cells, and puts them back as they come near. Core
/// Animation draws every layer in the tree that needs it, on screen or
/// not (hidden too): each label is a bitmap of its own, and a long Scroll
/// of text cost as much to update as all its rows, in time and memory.
/// A row out of the tree keeps its frame and props, so it comes back as
/// it was. While VoiceOver runs it takes nothing out: VoiceOver moves
/// through every row, scrolling to it.
///
/// A horizontal one, right to left, starts at the content's trailing end,
/// and keeps its distance from that end as the content's width changes. The
/// content is at the origin and offsets are physical, as ever.
///
/// A zoomable one (MaxZoom more than MinZoom) zooms its content as UIKit's
/// do: a pinch, a double tap (in to twice MinZoom around where it fell, or
/// back out), its zoomScale between the two; zoomed in, it scrolls both
/// ways. The core lays the content out at zoom 1: its frame is kept, and
/// the content is placed at the zoom (placeContent), centered while it's
/// smaller than the scroll view.
final class AsukaScrollView: UIScrollView {
    weak var node: AsukaNode?
    var horizontal = false { didSet { keepEnd() } }
    private var fromEnd: CGFloat = 0 // how far the offset is from the trailing end
    private var keptContent: CGFloat = -1, keptBounds: CGFloat = -1 // the widths it's for
    private var keeping = false // whether it's kept right to left

    override func layoutSubviews() {
        super.layoutSubviews()
        if zooms, let v = zoomView, let f = contentFrame { placeContent(v, f) }
        keepEnd()
        cull()
    }

    // MARK: Zoom

    private(set) var minZoom: CGFloat = 1
    private(set) var maxZoom: CGFloat = 1
    var zoomable: Bool { maxZoom > minZoom }
    /// Whether its content is placed at a zoom (and UIKit's zoom has a
    /// view): it zooms, or did and is going back to 1. Not zoomScale,
    /// which asks the delegate for the view.
    private(set) var zooms = false
    /// The content's frame as the core lays it out, at zoom 1.
    private var contentFrame: CGRect?
    /// The zoom changing isn't the user's (ZoomTo, a clamp): not reported.
    private var quietZoom = false
    private var reportedZoom: CGFloat = 1
    #if !os(tvOS)
    private var zoomTap: UITapGestureRecognizer?
    #endif

    /// The view it zooms: its content.
    var zoomView: UIView? {
        guard let v = node?.children.first?.view, v.superview === self else { return nil }
        return v
    }

    /// MinZoom or MaxZoom changed (nil: kept): the zoom stays within the
    /// two (at once and unreported: it starts at 1, within them).
    func setZoom(min: CGFloat? = nil, max: CGFloat? = nil) {
        if let min { minZoom = min }
        if let max { maxZoom = max }
        let on = zoomable
        if on, let n = node {
            // Its delegate gives UIKit the view to zoom and reports the zoom.
            if n.scrolls == nil { n.scrolls = AsukaScrollReporter(n.id, AsukaRuntime.shared) }
            if delegate !== n.scrolls { delegate = n.scrolls }
        }
        if on { zooms = true }
        minimumZoomScale = on ? minZoom : 1
        maximumZoomScale = on ? maxZoom : 1
        clampZoom()
        if !on, zooms {
            // No longer zoomable: the content back at 1, placed as the core lays it out.
            zooms = false
            if let v = zoomView, let f = contentFrame {
                v.bounds.size = f.size
                v.center = CGPoint(x: f.midX, y: f.midY)
                contentSize = f.size
            }
            contentFrame = nil
        }
        #if !os(tvOS)
        if on, zoomTap == nil {
            let g = UITapGestureRecognizer(target: self, action: #selector(zoomTapped(_:)))
            g.numberOfTapsRequired = 2
            addGestureRecognizer(g)
            zoomTap = g
        }
        zoomTap?.isEnabled = on
        #endif
        setNeedsLayout()
    }

    /// Its zoom within its bounds, unreported.
    private func clampZoom() {
        let z = zoomable ? Swift.min(Swift.max(zoomScale, minZoom), maxZoom) : 1
        guard z != zoomScale, zoomView != nil else { return }
        quietZoom = true
        zoomScale = z
        quietZoom = false
        reportedZoom = zoomScale
    }

    /// Places the content, laid out at f, at the zoom: scaled from its
    /// origin, centered while it's smaller than the scroll view, the
    /// content size its scaled size.
    func placeContent(_ v: UIView, _ f: CGRect) {
        let first = contentFrame == nil
        contentFrame = f
        if first { clampZoom() } // the zoom starts at 1, within its bounds
        let z = zoomScale
        let w = f.width * z, h = f.height * z
        let dx = Swift.max(0, (bounds.width - w) / 2), dy = Swift.max(0, (bounds.height - h) / 2)
        let c = CGPoint(x: f.minX + dx + w / 2, y: f.minY + dy + h / 2)
        if v.center != c { v.center = c }
        let size = CGSize(width: w, height: h)
        if contentSize != size { contentSize = size }
    }

    /// The rect of the content around p that shows at zoom z.
    private func zoomRect(around p: CGPoint, _ z: CGFloat) -> CGRect {
        let w = bounds.width / z, h = bounds.height / z
        return CGRect(x: p.x - w / 2, y: p.y - h / 2, width: w, height: h)
    }

    /// ZoomTo: to z around the middle of what shows, unreported.
    func zoomCommand(_ z: CGFloat, animated: Bool) {
        guard zoomable, let v = zoomView else { return }
        layoutIfNeeded()
        guard abs(z - zoomScale) > 0.0001 else { return }
        let mid = convert(CGPoint(x: bounds.midX, y: bounds.midY), to: v)
        quietZoom = true
        zoom(to: zoomRect(around: mid, z), animated: animated && window != nil)
        if !(animated && window != nil) { quietZoom = false } // an animated one's ends with it (zoomChanged)
    }

    #if !os(tvOS)
    /// A double tap: in to twice MinZoom (MaxZoom at most) around where it
    /// fell, or back out to MinZoom; the user's, reported.
    @objc private func zoomTapped(_ g: UITapGestureRecognizer) {
        guard g.state == .ended, zoomable, let v = zoomView else { return }
        quietZoom = false
        if zoomScale > minZoom + 0.001 {
            setZoomScale(minZoom, animated: true)
        } else {
            zoom(to: zoomRect(around: g.location(in: v), Swift.min(minZoom * 2, maxZoom)), animated: true)
        }
    }
    #endif

    /// A zoom begins: a pinch's is the user's (UIKit says so of an
    /// animated one too).
    func zoomBegan() {
        #if !os(tvOS)
        if let p = pinchGestureRecognizer, p.state == .began || p.state == .changed { quietZoom = false }
        #endif
    }

    /// The zoom changed, as it goes (ended false) and where it settles:
    /// the user's is reported with Zoomed.
    func zoomChanged(ended: Bool) {
        setNeedsLayout() // placed again at it
        let z = zoomScale
        if quietZoom {
            reportedZoom = z
            if ended { quietZoom = false }
            return
        }
        guard z != reportedZoom, let id = node?.id else { return }
        reportedZoom = z
        AsukaRuntime.shared.emit(id, .zoomed) { AsukaEvents.zoomed(id, Double(z)) }
    }

    /// Right to left, a change of width keeps the distance from the trailing
    /// end; else the offset is the user's, and that's the distance now. A
    /// change of direction keeps the distance from the start the content had.
    func keepEnd() {
        let rtl = horizontal && (node?.isRTL ?? false)
        guard rtl || keeping else { return }
        let i = adjustedContentInset
        let last = max(-i.left, contentSize.width - bounds.width + i.right)
        if rtl != keeping {
            keeping = rtl
            if rtl {
                fromEnd = max(contentOffset.x + i.left, 0)
                keptContent = -1
            } else {
                contentOffset.x = min(fromEnd - i.left, last)
            }
        }
        guard rtl, bounds.width > 0 else { return }
        if contentSize.width != keptContent || bounds.width != keptBounds {
            keptContent = contentSize.width
            keptBounds = bounds.width
            let to = max(last - fromEnd, -i.left)
            if contentOffset.x != to { contentOffset.x = to }
        } else {
            fromEnd = max(last - contentOffset.x, 0)
        }
    }

    func cull() {
        guard let content = node?.children.first, content.view.superview === self else { return }
        let all = UIAccessibility.isVoiceOverRunning
        let c = content.container
        // A screen's worth around what shows, in the content's coordinates
        // (unzoomed).
        let around = bounds.insetBy(dx: -bounds.width, dy: -bounds.height)
        let keep = zooms && zoomScale != 1 ? convert(around, to: content.view)
            : around.offsetBy(dx: -content.view.frame.minX, dy: -content.view.frame.minY)
        // From the last: a row comes back below the next one in the tree.
        var next: UIView?
        for row in content.children.reversed() {
            let v = row.view
            let near = all || v.frame.intersects(keep)
            if near, v.superview == nil {
                if let next { c.insertSubview(v, belowSubview: next) } else { c.addSubview(v) }
            } else if !near, v.superview === c {
                v.removeFromSuperview()
            }
            if v.superview === c { next = v }
        }
    }
}

@MainActor extension AsukaRuntime {
    /// VoiceOver started (or stopped): the Scrolls put back (or take out)
    /// their rows out of sight.
    @objc func voiceOverChanged() {
        for n in nodes.values { (n.view as? AsukaScrollView)?.cull() }
    }
}
#endif
