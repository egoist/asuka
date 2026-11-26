// A click-through window's views that still take the mouse (TakesClicks:
// takesClicks). AppKit's ignoresMouseEvents is the whole window's, so
// while a click-through window has such views, a timer follows the mouse
// (a window that ignores it gets no events of its own to say where it
// is), and the window takes the mouse while it's over one of them: the
// rest of it lets the mouse through. A button held down keeps it as it
// is, so a drag that started on one isn't cut off. Navigation's part.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

@MainActor enum AsukaClickRegion {
    /// The views that take clicks.
    static let takers = NSHashTable<NSView>.weakObjects()
    static var timer: Timer?
}

@MainActor extension AsukaRuntime {
    /// n's TakesClicks prop.
    func setTakesClicks(_ n: AsukaNode, _ on: Bool) {
        guard let v = n.view else { return }
        if on { AsukaClickRegion.takers.add(v) } else { AsukaClickRegion.takers.remove(v) }
        updateClickRegions()
    }

    /// x lets the mouse through, or doesn't (its options, WindowAction).
    func setClickThrough(_ x: AsukaWin, _ on: Bool) {
        x.shell.through = on
        x.window.ignoresMouseEvents = on
        updateClickRegions()
    }

    /// Follows the mouse while a click-through window has views that take
    /// it; otherwise not at all.
    func updateClickRegions() {
        let wanted = allWins.contains { $0.shell.through && hasTakers($0) }
        if wanted, AsukaClickRegion.timer == nil {
            let t = Timer(timeInterval: 1.0 / 60, repeats: true) { _ in MainActor.assumeIsolated { self.followMouse() } }
            RunLoop.main.add(t, forMode: .common)
            AsukaClickRegion.timer = t
        } else if !wanted, let t = AsukaClickRegion.timer {
            t.invalidate()
            AsukaClickRegion.timer = nil
        }
        followMouse()
    }

    private func hasTakers(_ x: AsukaWin) -> Bool {
        AsukaClickRegion.takers.allObjects.contains { $0.window === x.window }
    }

    /// The part of v that shows: what's visible of it (a scroll view's
    /// clip), within its bounds (a view that doesn't clip has a visible
    /// rect past them).
    private func shown(_ v: NSView) -> NSRect { v.visibleRect.intersection(v.bounds) }

    /// Whether the mouse, at p on the screen, is over a view of x's that
    /// takes clicks.
    func takesMouse(_ x: AsukaWin, at p: NSPoint) -> Bool {
        AsukaClickRegion.takers.allObjects.contains { v in
            guard v.window === x.window, !v.isHiddenOrHasHiddenAncestor else { return false }
            let r = x.window.convertToScreen(v.convert(shown(v), to: nil))
            return r.contains(p)
        }
    }

    /// x's views that take clicks, where they show in its content (points,
    /// y down): the automation's.
    func clickRegion(_ x: AsukaWin) -> [CGRect] {
        AsukaClickRegion.takers.allObjects.compactMap { v in
            guard v.window === x.window, !v.isHiddenOrHasHiddenAncestor else { return nil }
            var r = x.content.convert(v.convert(shown(v), to: nil), from: nil)
            if !x.content.isFlipped { r.origin.y = x.content.bounds.height - r.maxY }
            return r
        }
    }

    func followMouse() {
        // Its views gone (destroyed, the window closed): no more following.
        if let t = AsukaClickRegion.timer, !allWins.contains(where: { $0.shell.through && hasTakers($0) }) {
            t.invalidate()
            AsukaClickRegion.timer = nil
        }
        if NSEvent.pressedMouseButtons != 0 { return }
        let p = NSEvent.mouseLocation
        for x in allWins where x.shell.through {
            let ignore = !takesMouse(x, at: p)
            if x.window.ignoresMouseEvents != ignore { x.window.ignoresMouseEvents = ignore }
        }
    }
}
