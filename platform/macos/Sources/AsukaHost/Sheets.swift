// Sheets.swift (nav): a Sheet's detents on a Mac. A Mac's sheet hangs
// from its window's title bar, and the user doesn't drag it between
// heights: it rests at the detent it's at (Detent, else its shortest),
// which the app moves it between, its frame animating from its top edge.
// Its heights: large is the most a sheet may be (sheetLarge), medium
// half of its window, fit its content's (the core's Fit), a height its
// points and a fraction its share of large's.
//
// At a detent no taller than Undimmed's, it's no sheet: a Mac has no
// sheet that leaves its window usable, so it's a panel, a child window
// over the bottom of its window, as wide as a sheet, its top corners
// rounded, which moves with its window and leaves the rest of it to
// the user (Apple Maps' sheet, as a Mac shows such a thing). Moved
// taller, it becomes a sheet of its window again; shorter, a panel.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// One of a Sheet's Detents: a DetentKind and its value (Height's points,
/// Fraction's share of large).
struct AsukaDetent: Equatable {
    var kind: Int32
    var value: Float

    /// Detents' bytes: u32 count, then per detent u8 DetentKind, f32 value.
    static func decode(_ b: [UInt8]) -> [AsukaDetent] {
        let n = Int(b.u32(0))
        guard b.count >= 4 + 5 * n else { return [] }
        return (0..<n).map { i in AsukaDetent(kind: Int32(b[4 + 5 * i]), value: b.f32(5 + 5 * i)) }
    }
}

/// A sheet's window's content view: as it changes size (a detent, the
/// user resizing the window, a Fit), its content is sized with it and its
/// environment and height reported.
final class AsukaSheetContent: AsukaView {
    override func setFrameSize(_ newSize: NSSize) {
        let old = frame.size
        super.setFrameSize(newSize)
        if old != newSize { MainActor.assumeIsolated { AsukaRuntime.shared.sheetContentResized(self) } }
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        MainActor.assumeIsolated { paintPanel() }
    }

    /// A panel's background: the window's (a sheet draws its own).
    @MainActor func paintPanel() {
        guard let l = layer, (window as? AsukaSheetWindow)?.asPanel == true else { return }
        effectiveAppearance.performAsCurrentDrawingAppearance {
            l.backgroundColor = NSColor.windowBackgroundColor.cgColor
            l.borderColor = NSColor.separatorColor.cgColor
        }
        l.borderWidth = 1
        window?.invalidateShadow()
    }
}

/// The corner radius of a panel that says none: a sheet's on this macOS.
private var asukaPanelRadius: CGFloat {
    if #available(macOS 26, *) { return 16 }
    return 10
}

@MainActor extension AsukaRuntime {
    /// The most a sheet on over may be: its window's content less a
    /// margin, at most 640 wide; less 100 high, at least 240.
    func sheetLarge(_ over: AsukaWin) -> CGSize {
        let cs = over.content.bounds.size
        return CGSize(width: min(max(cs.width - asukaSheetMargin, 320), 640), height: max(cs.height - 100, 240))
    }

    /// The heights n's detents rest at on over, in their order (none:
    /// large alone).
    func detentHeights(_ n: AsukaNode, _ over: AsukaWin) -> [CGFloat] {
        guard let nav = n.nav else { return [] }
        let large = sheetLarge(over).height
        let half = max(over.content.bounds.height / 2, 240)
        return nav.detents.map { d in
            var h = large
            switch AsukaDetentKind(rawValue: d.kind) {
            case .medium: h = half
            case .fit: if nav.fitHeight > 0 { h = nav.fitHeight }
            case .height: h = CGFloat(d.value)
            case .fraction: h = (large * CGFloat(d.value)).rounded()
            default: break
            }
            return max(min(h, large), 1)
        }
    }

    /// The index of the detent n is at: Detent's, else its shortest (-1:
    /// it has none).
    func detentIndex(_ n: AsukaNode, _ hs: [CGFloat]) -> Int {
        guard let nav = n.nav, !hs.isEmpty else { return -1 }
        if nav.detentAt >= 1 && nav.detentAt <= hs.count { return nav.detentAt - 1 }
        var at = 0
        for i in hs.indices where hs[i] < hs[at] { at = i }
        return at
    }

    /// The height n rests at on over: its detent's.
    func sheetHeight(_ n: AsukaNode, _ over: AsukaWin) -> CGFloat {
        let hs = detentHeights(n, over)
        let i = detentIndex(n, hs)
        return i >= 0 ? hs[i] : sheetLarge(over).height
    }

    /// Whether n is at its fit detent (one of others: the core measures
    /// its content for it).
    func atFitDetent(_ n: AsukaNode) -> Bool {
        guard let nav = n.nav, let over = onWin(n), !fitsContent(n) else { return false }
        let i = detentIndex(n, detentHeights(n, over))
        return i >= 0 && nav.detents[i].kind == AsukaDetentKind.fit.rawValue
    }

    /// Whether n, at the detent it's at, leaves its window usable: no
    /// taller than Undimmed's.
    func wantsPanel(_ n: AsukaNode, _ over: AsukaWin) -> Bool {
        guard let nav = n.nav, !nav.full, !fitsContent(n), nav.undimmed >= 1 else { return false }
        let hs = detentHeights(n, over)
        guard nav.undimmed <= hs.count else { return false }
        let i = detentIndex(n, hs)
        return i >= 0 && hs[i] <= hs[nav.undimmed - 1] + 0.5
    }

    /// A Sheet's Detents, Detent, Undimmed, CornerRadius, Grabber and
    /// NoDismiss. A Mac's sheet has no grabber, and nothing drags it: its
    /// Grabber and NoDismiss change nothing (Escape still dismisses it).
    func setSheetDetentProp(_ n: AsukaNode, _ nav: AsukaNav, _ p: UInt16, _ v: AsukaValue) {
        switch p {
        case AsukaProp.Sheet.detents: nav.detents = AsukaDetent.decode(v.data)
        case AsukaProp.Sheet.detent: nav.detentAt = Int(v.int)
        case AsukaProp.Sheet.undimmed: nav.undimmed = Int(v.int)
        case AsukaProp.Sheet.cornerRadius: nav.cornerRadius = CGFloat(v.float)
        default: return
        }
        // Shown, it moves to them once the batch is applied (its detents
        // and the one it's at come in one).
        guard nav.window != nil, !nav.sheetQueued else { return }
        nav.sheetQueued = true
        afterCore { [weak self] in
            nav.sheetQueued = false
            guard let self, self.alive(n), !n.dying else { return }
            self.sheetChanged(n, true)
        }
    }

    /// Moves a shown sheet to the detent it's at, a panel or a sheet as
    /// that asks, animated.
    func sheetChanged(_ n: AsukaNode, _ animate: Bool) {
        guard let nav = n.nav, let w = nav.window, !nav.full, !fitsContent(n), let over = onWin(n) else { return }
        if nav.begin != nil { return } // not shown yet: it opens there
        let panel = wantsPanel(n, over)
        if panel != nav.panel {
            if panel {
                if let p = w.sheetParent { p.endSheet(w) }
                w.orderOut(nil)
                showPanel(n, over)
            } else {
                nav.panel = false
                over.window.removeChildWindow(w)
                w.orderOut(nil)
                sheetStyle(w)
                w.setContentSize(sheetRoom(n, over))
                over.window.beginSheet(w, completionHandler: nil)
            }
            return
        }
        if panel {
            placePanel(n, animate)
            if let c = w.contentView as? AsukaSheetContent { roundPanel(n, c) }
            return
        }
        // A sheet: where AppKit has it, its top edge kept where it hangs
        // from its window's title bar, its centre where it's centred on
        // its window (macOS 26's).
        let size = sheetRoom(n, over)
        if w.contentView?.bounds.size == size { return }
        let f = w.frame
        let fr = w.frameRect(forContentRect: CGRect(origin: .zero, size: size))
        let hangs = abs(f.maxY - over.window.convertToScreen(over.window.contentLayoutRect).maxY) < 1
        let y = hangs ? f.maxY - fr.height : (f.midY - fr.height / 2).rounded()
        let to = CGRect(x: f.midX - fr.width / 2, y: y, width: fr.width, height: fr.height)
        setSheetFrame(w, to, animate)
    }

    /// Sets a sheet's or panel's frame, animated unless Reduce Motion is on.
    func setSheetFrame(_ w: NSWindow, _ to: CGRect, _ animate: Bool) {
        if !animate || env.reduceMotion || !w.isVisible {
            w.setFrame(to, display: true)
            return
        }
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.25
            ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            w.animator().setFrame(to, display: true)
        }
    }

    /// The style of a sheet's window: titled, resizable unless its height
    /// is its content's.
    func sheetStyle(_ w: NSWindow) {
        guard let w = w as? AsukaSheetWindow else { return }
        w.asPanel = false
        w.styleMask = [.titled, .resizable]
        w.isOpaque = true
        w.hasShadow = true
        w.backgroundColor = .windowBackgroundColor
        if let l = w.contentView?.layer {
            l.cornerRadius = 0
            l.backgroundColor = nil
            l.borderWidth = 0
            l.masksToBounds = false
        }
    }

    /// Shows n as a panel over the bottom of over's content: a borderless
    /// child window, which moves with it.
    func showPanel(_ n: AsukaNode, _ over: AsukaWin) {
        guard let nav = n.nav, let w = nav.window as? AsukaSheetWindow, let c = w.contentView as? AsukaSheetContent else { return }
        nav.panel = true
        w.asPanel = true
        w.styleMask = [.borderless]
        w.isOpaque = false
        w.backgroundColor = .clear
        w.hasShadow = true
        c.wantsLayer = true
        c.paintPanel()
        roundPanel(n, c)
        placePanel(n, false)
        over.window.addChildWindow(w, ordered: .above)
        w.orderFront(nil)
        w.invalidateShadow()
    }

    /// A panel's top corners: CornerRadius's, else a sheet's.
    func roundPanel(_ n: AsukaNode, _ c: NSView) {
        guard let l = c.layer, let nav = n.nav else { return }
        l.cornerRadius = nav.cornerRadius > 0 ? nav.cornerRadius : asukaPanelRadius
        l.maskedCorners = [.layerMinXMinYCorner, .layerMaxXMinYCorner] // the top's, in a flipped view
        l.masksToBounds = true
    }

    /// Places a panel over the bottom of its window's content, centred, as
    /// wide as a sheet and as tall as its detent.
    func placePanel(_ n: AsukaNode, _ animate: Bool) {
        guard let nav = n.nav, nav.panel, let w = nav.window, let over = onWin(n) else { return }
        let pw = over.window
        let r = pw.convertToScreen(pw.contentLayoutRect)
        let size = sheetRoom(n, over)
        let to = CGRect(x: (r.midX - size.width / 2).rounded(), y: r.minY, width: size.width, height: size.height)
        if w.frame == to { return }
        setSheetFrame(w, to, animate)
    }

    /// The core measured the content of a sheet with fit among its
    /// detents: its fit detent's height, which it moves to if it's there
    /// (opening there, it shows now).
    func sheetFit(_ n: AsukaNode, _ height: CGFloat) {
        guard let nav = n.nav, let w = nav.window, height > 0 else { return }
        let changed = nav.fitHeight != height
        nav.fitHeight = height
        if let begin = nav.begin {
            nav.begin = nil
            if let over = onWin(n) {
                let size = sheetRoom(n, over)
                w.setContentSize(size)
            }
            afterCore(begin)
            return
        }
        if changed { sheetChanged(n, true) }
    }

    /// A sheet's content view changed size: its content goes with it, its
    /// environment (a fit sheet's is its room, laid out by layoutNav) and
    /// height reported.
    func sheetContentResized(_ c: NSView) {
        guard let n = byView[ObjectIdentifier(c)], n.kind == .sheet, !n.dying, let nav = n.nav else { return }
        if !fitsContent(n) && !nav.full {
            let b = c.bounds
            if n.view?.frame != b { n.view?.frame = b }
            reportScreen(n, b.size)
        }
        reportSheetHeight(n)
    }

    /// Reports a sheet's height (SheetHeight) when it changed: its
    /// window's content's, for a sheet that hangs from its window's top
    /// as for a panel over its bottom.
    func reportSheetHeight(_ n: AsukaNode) {
        guard let nav = n.nav, n.listening(.sheetHeight), let c = nav.window?.contentView else { return }
        let h = Float(c.bounds.height)
        if h == nav.heightSent { return }
        nav.heightSent = h
        emit(n, .sheetHeight) { AsukaEvents.sheetHeight($0, Double(h)) }
    }
}
