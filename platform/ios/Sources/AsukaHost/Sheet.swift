#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A sheet's content, presented with UISheetPresentationController: its
/// detents (the system's medium and large, custom ones for a height, a
/// share of large and its content's height, which the core measures and
/// sends with WindowAction Fit), the one it's at (Detent, the user's
/// drag reported with Selected), the tallest that doesn't dim what's
/// behind (Undimmed), its corners, grabber and whether a swipe dismisses
/// it (NoDismiss), and its height on the screen while the core listens
/// (SheetHeight). A TV's sheets cover the screen, without detents.
final class AsukaSheetController: AsukaScreenController, UIAdaptivePresentationControllerDelegate {
    /// A detent as the wire has it (SheetProps.Detents).
    struct Detent: Equatable {
        var kind: AsukaDetentKind
        var value: CGFloat
    }

    var detents: [Detent] = []
    /// SheetProps.Detent: 1 + the index it's at (0: the shortest).
    var at = 0
    /// SheetProps.Undimmed: 1 + the index of the tallest detent that
    /// doesn't dim (0: none).
    var undimmed = 0
    var cornerRadius: CGFloat = 0
    var grabber = false
    var noDismiss = false
    /// SheetProps.FullScreen: presented .fullScreen (SwiftUI's
    /// fullScreenCover), over everything, its view the screen's: its
    /// environment is the screen's size and safe area. Nothing but the
    /// core dismisses it.
    var fullScreen = false
    /// Its content's height (WindowAction Fit): its fit detent's.
    var fitHeight: CGFloat = 0
    /// Present waits for its content's height, which a fit detent needs.
    var waitingFit = false
    /// The index the user's drag (or a Detent) left it at, as reported.
    private var reported: Int?
    private var link: CADisplayLink?
    private var lastHeight: CGFloat = -1
    private var still = 0

    var presented: Bool { presentingViewController != nil && !isBeingDismissed }

    // ---- Decoding ----

    static func decodeDetents(_ b: UnsafeRawBufferPointer) -> [Detent] {
        guard b.count >= 4 else { return [] }
        var r = AsukaReader(b)
        let n = Int(r.u32())
        guard b.count == 4 + 5 * n else { return [] }
        var out: [Detent] = []
        out.reserveCapacity(n)
        for _ in 0..<n {
            let k = AsukaDetentKind(rawValue: Int32(r.u8())) ?? .large
            out.append(Detent(kind: k, value: CGFloat(r.f32())))
        }
        return out
    }

    var hasFit: Bool { detents.contains { $0.kind == .fit } }

    // ---- Detents ----

    #if !os(tvOS)
    private func identifier(_ i: Int) -> UISheetPresentationController.Detent.Identifier {
        switch detents[i].kind {
        case .medium: return .medium
        case .large: return .large
        default: return .init("asuka.\(i)")
        }
    }

    private func index(of id: UISheetPresentationController.Detent.Identifier?) -> Int? {
        guard let id else { return nil }
        return detents.indices.first { identifier($0) == id }
    }

    /// The bottom safe area of the window it's on: a custom detent's
    /// height leaves it out, and the core's heights have it in.
    private var bottomInset: CGFloat {
        (view.window ?? AsukaRuntime.shared.topController?.view.window)?.safeAreaInsets.bottom ?? 0
    }

    /// The tallest a sheet may be, as an estimate (UIKit's own is the
    /// window under its top safe area and a gap), to order the detents by.
    private var largeEstimate: CGFloat {
        let w = view.window ?? AsukaRuntime.shared.topController?.view.window
        let h = w?.bounds.height ?? UIScreen.main.bounds.height
        return max(0, h - (w?.safeAreaInsets.top ?? 0) - 10)
    }

    private func estimate(_ d: Detent) -> CGFloat {
        let large = largeEstimate
        switch d.kind {
        case .medium: return large / 2
        case .large: return large
        case .fit: return fitHeight > 0 ? min(fitHeight, large) : large
        case .height: return min(d.value, large)
        case .fraction: return large * d.value
        }
    }

    /// The detents UIKit rests the sheet at, shortest first: medium and
    /// large are the system's; a height, a share of large and the
    /// content's height are custom ones, each at most large.
    private func sheetDetents() -> [UISheetPresentationController.Detent] {
        if detents.isEmpty { return [.large()] }
        let order = detents.indices.sorted { estimate(detents[$0]) < estimate(detents[$1]) }
        var seen = Set<String>()
        var out: [UISheetPresentationController.Detent] = []
        for i in order {
            let id = identifier(i)
            guard seen.insert(id.rawValue).inserted else { continue }
            let d = detents[i]
            switch d.kind {
            case .medium: out.append(.medium())
            case .large: out.append(.large())
            case .fit:
                out.append(.custom(identifier: id) { [weak self] ctx in
                    guard let self, self.fitHeight > 0 else { return ctx.maximumDetentValue }
                    return min(max(self.fitHeight - self.bottomInset, 1), ctx.maximumDetentValue)
                })
            case .height:
                let v = d.value
                out.append(.custom(identifier: id) { [weak self] ctx in
                    min(max(v - (self?.bottomInset ?? 0), 1), ctx.maximumDetentValue)
                })
            case .fraction:
                let f = d.value
                // A share of large's whole height, the bottom safe area in it.
                out.append(.custom(identifier: id) { [weak self] ctx in
                    let inset = self?.bottomInset ?? 0
                    return min(max((ctx.maximumDetentValue + inset) * f - inset, 1), ctx.maximumDetentValue)
                })
            }
        }
        return out
    }

    /// Sets up its presentation controller from its props: before Present
    /// as it is, while it shows animated.
    func configure(animated: Bool) {
        guard !fullScreen, let sheet = sheetPresentationController else { return }
        let apply = { [self] in
            sheet.detents = sheetDetents()
            sheet.selectedDetentIdentifier = at > 0 && at <= detents.count ? identifier(at - 1) : nil
            sheet.largestUndimmedDetentIdentifier = undimmed > 0 && undimmed <= detents.count ? identifier(undimmed - 1) : nil
            sheet.prefersGrabberVisible = grabber
            sheet.preferredCornerRadius = cornerRadius > 0 ? cornerRadius : nil
        }
        if animated { sheet.animateChanges(apply) } else { apply() }
        isModalInPresentation = noDismiss || interceptsBack
        if at > 0 { reported = at - 1 }
        sheet.delegate = self
        startHeights()
    }

    /// A new height of its content (WindowAction Fit): its fit detent's.
    func fit(_ height: CGFloat) {
        guard height > 0, height != fitHeight else { return }
        fitHeight = height
        if waitingFit {
            waitingFit = false
            AsukaRuntime.shared.presentSheet(self)
            return
        }
        guard presented, hasFit, let sheet = sheetPresentationController else { return }
        sheet.animateChanges { sheet.invalidateDetents() }
    }

    /// Before a sheet with a fit detent shows: its environment as it will
    /// be (the window's width, large's height, the home indicator's safe
    /// area), so the core lays its content out and measures it first.
    func reportAhead(in window: UIWindow) {
        let rt = AsukaRuntime.shared
        let w = window.bounds.width
        view.frame = CGRect(x: 0, y: 0, width: w, height: largeEstimate)
        view.layoutIfNeeded()
        var e = rt.environment(of: view)
        let s = window.safeAreaInsets
        e.safeTop = 0
        e.safeBottom = Float(s.bottom)
        e.safeLeft = Float(s.left)
        e.safeRight = Float(s.right)
        lastEnv = e
        rt.send(AsukaEvents.environment(nodeID, e))
    }

    func sheetPresentationControllerDidChangeSelectedDetentIdentifier(_ sheet: UISheetPresentationController) {
        startHeights()
        guard let i = index(of: sheet.selectedDetentIdentifier), i != reported else { return }
        reported = i
        let id = nodeID
        AsukaRuntime.shared.emit(id, .selected) { AsukaEvents.selected(id, Int64(i)) }
    }
    #endif

    // ---- Its height on the screen ----

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        startHeights()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        stopHeights()
    }

    private var listensHeight: Bool {
        AsukaRuntime.shared.nodes[nodeID]?.listens.contains(AsukaEventKind.sheetHeight.rawValue) ?? false
    }

    /// While the core listens: a display link reads where the sheet is
    /// each frame (its presentation layer, so an animation's frames too),
    /// sending its height as it changes, and stops once it's been still a
    /// while; a layout or a detent's change starts it again.
    func startHeights() {
        guard link == nil, listensHeight, presentingViewController != nil else { return }
        still = 0
        let l = CADisplayLink(target: AsukaSheetTicker(self), selector: #selector(AsukaSheetTicker.tick))
        l.add(to: .main, forMode: .common)
        link = l
        tick()
    }

    func stopHeights() {
        link?.invalidate()
        link = nil
    }

    /// The sheet's top edge in its window as it's drawn now. Its views'
    /// frames are where an animation ends, and the layer that moves it is
    /// the system's own (hosted apart from the views, on iOS 26), but the
    /// sheet grows from its bottom edge: it's that edge, as laid out, less
    /// its height as drawn (its presentation layer's), in its scale on the
    /// screen.
    private func drawnTop(in w: UIWindow) -> CGFloat {
        let v = presentationController?.presentedView ?? view!
        let laid = v.convert(v.bounds, to: w)
        let scale = v.bounds.height > 0 ? laid.height / v.bounds.height : 1
        let drawn = v.layer.presentation()?.bounds.height ?? v.bounds.height
        return laid.maxY - drawn * scale
    }

    func tick() {
        guard let w = view.window else {
            stopHeights()
            return
        }
        let h = max(0, w.bounds.height - drawnTop(in: w))
        if abs(h - lastHeight) < 0.25 {
            still += 1
            if still > 30 { stopHeights() }
            return
        }
        still = 0
        lastHeight = h
        let id = nodeID
        AsukaRuntime.shared.emit(id, .sheetHeight) { AsukaEvents.sheetHeight(id, Double(h)) }
    }

    // ---- Dismissal ----

    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
        stopHeights()
        guard let n = AsukaRuntime.shared.nodes[nodeID], !n.dying else { return }
        if listensHeight {
            let id = nodeID
            AsukaRuntime.shared.emit(id, .sheetHeight) { AsukaEvents.sheetHeight(id, 0) }
        }
        AsukaRuntime.shared.emit(nodeID, .dismissed) { AsukaEvents.dismissed(nodeID) }
    }

    /// A swipe down or a tap outside while it can't be (isModalInPresentation):
    /// ui.OnBack decides while it intercepts; NoDismiss alone keeps it.
    func presentationControllerDidAttemptToDismiss(_ presentationController: UIPresentationController) {
        guard interceptsBack, let n = AsukaRuntime.shared.nodes[nodeID], !n.dying else { return }
        let id = nodeID
        AsukaRuntime.shared.emit(id, .back) { AsukaEvents.back(id) }
    }
}

#if !os(tvOS)
extension AsukaSheetController: UISheetPresentationControllerDelegate {}
#endif

/// A display link's target, so the link doesn't keep the sheet alive.
final class AsukaSheetTicker: NSObject {
    weak var sheet: AsukaSheetController?
    init(_ sheet: AsukaSheetController) { self.sheet = sheet }
    @objc func tick(_ link: CADisplayLink) {
        guard let s = sheet else {
            link.invalidate()
            return
        }
        s.tick()
    }
}

extension AsukaRuntime {
    /// Presents a sheet over the top controller (Present), once its
    /// content's height is known when a detent is that height.
    func presentSheet(_ s: AsukaSheetController) {
        guard let n = nodes[s.nodeID], n.vc === s, !n.dying, s.presentingViewController == nil, let top = topController else { return }
        // The style before the presentation controller is first asked
        // for: it's made for the style it has then.
        s.modalPresentationStyle = s.fullScreen ? .fullScreen : .automatic
        #if !os(tvOS) // a TV's sheets cover the screen, without detents
        if !s.fullScreen, s.hasFit, s.fitHeight <= 0, !s.waitingFit, let w = top.view.window {
            // Its content measured first: the core sends Fit as it lays out.
            s.waitingFit = true
            s.reportAhead(in: w)
            if s.fitHeight > 0 { return } // came already, and presented it
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self, weak s] in
                guard let s, s.waitingFit else { return }
                s.waitingFit = false
                self?.presentSheet(s)
            }
            return
        }
        s.configure(animated: false)
        #endif
        s.isModalInPresentation = s.noDismiss || s.interceptsBack
        s.presentationController?.delegate = s
        top.present(s, animated: true) { [weak s] in s?.startHeights() }
    }

    /// A Sheet's prop: kept, and applied at once while it shows.
    func setSheetProp(_ s: AsukaSheetController, _ prop: UInt16, _ v: AsukaValue) {
        switch prop {
        case AsukaProp.Sheet.detents:
            guard case .bytes(let b) = v else { return }
            s.detents = AsukaSheetController.decodeDetents(b)
        case AsukaProp.Sheet.detent: s.at = Int(v.int)
        case AsukaProp.Sheet.undimmed: s.undimmed = Int(v.int)
        case AsukaProp.Sheet.cornerRadius: s.cornerRadius = CGFloat(v.float)
        case AsukaProp.Sheet.grabber: s.grabber = v.bool
        case AsukaProp.Sheet.noDismiss: s.noDismiss = v.bool
        case AsukaProp.Sheet.fullScreen: s.fullScreen = v.bool
        default: return
        }
        #if !os(tvOS)
        if s.presented { s.configure(animated: true) }
        #else
        if s.presented { s.isModalInPresentation = s.noDismiss || s.interceptsBack }
        #endif
    }
}
#endif
