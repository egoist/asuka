// Pull to refresh (on refresh) on a Mac.
//
// A trackpad's or a Magic Mouse's scroll pulls a Scroll or List past its
// top, as its elastic overscroll lets it, and let go of beyond 60 points it
// refreshes, as on iOS: a spinner shows above the content, which moves down
// to make room (the scroll view's top content inset), until Refreshing goes
// false. The scroll views' live scroll notifications say how far it went.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// Points past the top that refresh.
let asukaRefreshPull: CGFloat = 60
/// The room the spinner takes.
let asukaRefreshHeight: CGFloat = 40

/// A refreshable Scroll's or List's: how far this live scroll pulled, and
/// its spinner while it refreshes.
@MainActor final class AsukaRefresher: NSObject {
    weak var n: AsukaNode?
    /// The farthest past the top in this live scroll.
    var pulled: CGFloat = 0
    var refreshing = false
    var spinner: NSProgressIndicator?
    /// A pull the automation makes (a clip view constrains its bounds
    /// outside a live scroll's rubber band, so it can't be scrolled there).
    var testPull: CGFloat?

    init(_ n: AsukaNode) {
        self.n = n
        super.init()
    }

    @objc func liveScrolled(_ note: Notification) {
        guard let n, let sv = n.view as? NSScrollView else { return }
        let p = AsukaRuntime.shared.overTop(sv, self)
        if p > pulled { pulled = p }
    }

    @objc func liveScrollEnded(_ note: Notification) {
        guard let n else { return }
        AsukaRuntime.shared.refreshScrollEnded(n)
    }
}

@MainActor extension AsukaRuntime {
    /// Turns pulling to refresh on or off for n's scroll view.
    func setRefreshable(_ n: AsukaNode, _ on: Bool) {
        guard (n.refresh != nil) != on, let sv = n.view as? NSScrollView else { return }
        let c = NotificationCenter.default
        if !on {
            setRefreshing(n, false)
            if let r = n.refresh { c.removeObserver(r) }
            n.refresh = nil
            return
        }
        let r = AsukaRefresher(n)
        n.refresh = r
        sv.automaticallyAdjustsContentInsets = false
        c.addObserver(r, selector: #selector(AsukaRefresher.liveScrolled(_:)), name: NSScrollView.didLiveScrollNotification, object: sv)
        c.addObserver(r, selector: #selector(AsukaRefresher.liveScrollEnded(_:)), name: NSScrollView.didEndLiveScrollNotification, object: sv)
    }

    /// How far past its top a scroll view's content is pulled.
    func overTop(_ sv: NSScrollView, _ r: AsukaRefresher) -> CGFloat {
        if let p = r.testPull { return p }
        let clip = sv.contentView
        let y = clip.bounds.minY
        if !clip.isFlipped {
            // Unflipped, the top is the document's height less the clip's.
            let top = (sv.documentView?.frame.height ?? 0) - clip.bounds.height
            return y - top
        }
        return -(y + (r.refreshing ? asukaRefreshHeight : 0))
    }

    /// Refreshes if the live scroll that ended went far enough past the top.
    func refreshScrollEnded(_ n: AsukaNode) {
        guard let r = n.refresh else { return }
        let pulled = r.pulled
        r.pulled = 0
        r.testPull = nil
        guard pulled >= asukaRefreshPull, !r.refreshing, n.listening(.refresh) else { return }
        setRefreshing(n, true) // the core's handler says when it's done
        emit(n, .refresh) { AsukaEvents.refresh($0) }
    }

    /// Shows the spinner above the content, or takes it away.
    func setRefreshing(_ n: AsukaNode, _ on: Bool) {
        guard let r = n.refresh, r.refreshing != on, let sv = n.view as? NSScrollView else { return }
        r.refreshing = on
        if on {
            let s = r.spinner ?? {
                let p = NSProgressIndicator(frame: .zero)
                p.style = .spinning
                p.isIndeterminate = true
                p.controlSize = .small
                p.isDisplayedWhenStopped = false
                r.spinner = p
                return p
            }()
            placeSpinner(n, r)
            s.autoresizingMask = [.minXMargin, .maxXMargin] // its margins stretch: it stays centered
            sv.addSubview(s)
            s.startAnimation(nil)
            if let a = sv as? AsukaScrollView {
                a.refreshInset = asukaRefreshHeight
                a.applyInsets()
            } else {
                sv.contentInsets = NSEdgeInsets(top: asukaRefreshHeight, left: 0, bottom: 0, right: 0)
            }
            return
        }
        r.spinner?.stopAnimation(nil)
        r.spinner?.removeFromSuperview()
        if let a = sv as? AsukaScrollView {
            a.refreshInset = 0
            a.applyInsets()
        } else {
            sv.contentInsets = NSEdgeInsetsZero
        }
    }

    /// The spinner centered in the room above the content.
    func placeSpinner(_ n: AsukaNode, _ r: AsukaRefresher) {
        guard let s = r.spinner, let sv = n.view else { return }
        let w = sv.bounds.width
        s.frame = CGRect(x: (w - 16) / 2, y: (asukaRefreshHeight - 16) / 2, width: 16, height: 16)
    }

    /// Lets go of what pulling to refresh holds.
    func releaseRefresh(_ n: AsukaNode) {
        guard let r = n.refresh else { return }
        NotificationCenter.default.removeObserver(r)
        r.spinner?.removeFromSuperview()
        n.refresh = nil
    }

    /// The automation's pull: points past the top, let go of.
    func pull(_ n: AsukaNode, _ points: CGFloat) -> String? {
        guard let r = n.refresh, let sv = n.view as? NSScrollView else { return "it doesn't refresh" }
        r.testPull = points
        NotificationCenter.default.post(name: NSScrollView.didLiveScrollNotification, object: sv)
        NotificationCenter.default.post(name: NSScrollView.didEndLiveScrollNotification, object: sv)
        return nil
    }

    func refreshJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let r = n.refresh else { return }
        var j: [String: Any] = ["refreshing": r.refreshing]
        if let sv = n.view as? NSScrollView { j["insetTop"] = Double(sv.contentInsets.top) }
        if let s = r.spinner { j["spinner"] = s.superview != nil && !s.isHidden }
        d["refresh"] = j
    }
}
