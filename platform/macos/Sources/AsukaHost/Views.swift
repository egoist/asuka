// The host's AppKit view classes: a flipped container the core places
// views in, the label, the one-line field and its cell, the image view.
// What a press, a hover or a drag does is Interaction.swift's; these route
// AppKit's calls there.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A container: Asuka places its subviews itself, from the top left.
class AsukaView: NSView, NSDraggingSource {
    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    @MainActor var rt: AsukaRuntime { AsukaRuntime.shared }
    @MainActor var asukaNode: AsukaNode? { rt.node(of: self) }

    // A pressable view is one element for VoiceOver, a button.
    override func accessibilityPerformPress() -> Bool {
        guard let n = asukaNode, n.listening(.press) else { return false }
        rt.emit(n, .press) { AsukaEvents.press($0) }
        return true
    }

    /// The app's label, or a pressable view's content, as VoiceOver reads a
    /// row on iOS: its texts and labeled views, in order.
    override func accessibilityLabel() -> String? {
        guard let n = asukaNode else { return super.accessibilityLabel() }
        if !n.a11yLabel.isEmpty { return n.a11yLabel }
        guard n.listening(.press) else { return super.accessibilityLabel() }
        var parts: [String] = []
        rt.collectLabels(self, &parts)
        return parts.isEmpty ? nil : parts.joined(separator: ", ")
    }

    // OnAdjust: VoiceOver's increment and decrement.
    override func accessibilityPerformIncrement() -> Bool { rt.adjust(self, 1) }
    override func accessibilityPerformDecrement() -> Bool { rt.adjust(self, -1) }

    /// While a modal view shows in its window, only what leads to it
    /// (A11y.swift).
    override func accessibilityChildren() -> [Any]? { rt.a11yChildren(self, super.accessibilityChildren()) }

    // The keyboard's focus (Focus.swift): a Focusable, pressable or
    // adjustable view takes it, and draws AppKit's ring, rounded as it
    // is, while it has it from the keyboard.
    override var acceptsFirstResponder: Bool { rt.acceptsFocus(self) }
    override var canBecomeKeyView: Bool { rt.isKeyView(self) }
    override var focusRingMaskBounds: NSRect { rt.showsRing(self) ? bounds : .zero }
    override func drawFocusRingMask() {
        let r = asukaNode?.cornerOf ?? 0
        NSBezierPath(roundedRect: bounds, xRadius: r, yRadius: r).fill()
    }

    /// Its context menu: its Menu (LongPress: a secondary click), or a
    /// row's swipe actions.
    override func menu(for event: NSEvent) -> NSMenu? {
        guard let n = asukaNode else { return super.menu(for: event) }
        if let c = n.cell, n.menu == nil { return rt.cellMenu(c) }
        guard let m = n.menu, n.menuTrigger == AsukaMenuTrigger.longPress.rawValue else { return nil }
        return m
    }

    // A trackpad's swipes: three fingers (swipe(with:), when the system
    // leaves them to apps), or a two-finger flick (the scroll wheel's).
    override func swipe(with event: NSEvent) {
        if !rt.swipeEvent(self, event) { super.swipe(with: event) }
    }

    override func scrollWheel(with event: NSEvent) {
        if !rt.wheel(self, event) { super.scrollWheel(with: event) }
    }

    /// Escape in a sheet's content dismisses it.
    override func cancelOperation(_ sender: Any?) { rt.cancel(self) }

    // Presses, hover and drag and drop (Interaction.swift).
    override func mouseDown(with event: NSEvent) {
        // A row of a selecting list, a Selectable's selection (Lists.swift,
        // Selectable.swift).
        if rt.partMouseDown(self, event) { return }
        if !rt.mouseDown(self, event) { super.mouseDown(with: event) }
    }
    override func mouseDragged(with event: NSEvent) {
        if !rt.mouseDragged(event) { super.mouseDragged(with: event) }
    }
    override func mouseUp(with event: NSEvent) {
        if !rt.mouseUp(event) { super.mouseUp(with: event) }
    }
    // Under the automation, hover is its steps' (hover, hoverprobe): the
    // pointer of the computer it runs on isn't the test's.
    override func mouseEntered(with event: NSEvent) { if !AsukaAutomation.shared.active { rt.mouseEntered(self) } }
    override func mouseExited(with event: NSEvent) { if !AsukaAutomation.shared.active { rt.mouseExited(self) } }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        rt.watchFocus(window) // its window's first responder (Focus.swift)
        guard let n = asukaNode else { return }
        if window == nil {
            rt.viewLeft(n)
        } else {
            rt.checkHover(n)
            rt.reassertShadow(n)
        }
    }
    override func viewDidHide() {
        super.viewDidHide()
        if let n = asukaNode { rt.viewLeft(n) }
    }
    override func viewDidUnhide() {
        super.viewDidUnhide()
        if let n = asukaNode { rt.checkHover(n) }
    }

    // NSDraggingDestination.
    override func draggingEntered(_ info: NSDraggingInfo) -> NSDragOperation { rt.dragOver(self, info) }
    override func draggingUpdated(_ info: NSDraggingInfo) -> NSDragOperation { rt.dragOver(self, info) }
    override func draggingExited(_ info: NSDraggingInfo?) { rt.dragExited(self, info) }
    override func wantsPeriodicDraggingUpdates() -> Bool { false }
    override func prepareForDragOperation(_ info: NSDraggingInfo) -> Bool { rt.prepareDrop(self, info) }
    override func performDragOperation(_ info: NSDraggingInfo) -> Bool { rt.performDrop(self, info) }
    override func draggingEnded(_ info: NSDraggingInfo) { rt.draggingEnded(self, info) }

    // NSDraggingSource.
    func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
        rt.dragMask(context)
    }
    func draggingSession(_ session: NSDraggingSession, willBeginAt screenPoint: NSPoint) { rt.dragBegan() }
    func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint, operation: NSDragOperation) {
        rt.dragBegan() // ended before it began: it did begin
        rt.endDrag(operation)
    }
}

/// A Text: a label that takes no clicks (they go to the view under it, a
/// pressable row) and draws its text inside its padding (ContentInset).
final class AsukaLabel: NSTextField {
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    override func draw(_ dirty: NSRect) {
        guard let n = AsukaRuntime.shared.node(of: self), let cell else { return super.draw(dirty) }
        let i = n.inset
        var b = bounds
        b.origin.x += i.left
        b.origin.y += isFlipped ? i.top : i.bottom
        b.size.width = max(b.width - i.left - i.right, 0)
        b.size.height = max(b.height - i.top - i.bottom, 0)
        cell.draw(withFrame: b, in: self)
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.reassertShadow(n) }
    }
}

/// A one-line field's cell: its text, placeholder and field editor inside
/// the field's padding (ContentInset), as AppKit lays them out in what this
/// returns.
final class AsukaTextFieldCell: NSTextFieldCell {
    override func drawingRect(forBounds b: NSRect) -> NSRect {
        var r = super.drawingRect(forBounds: b)
        if let v = controlView, let n = MainActor.assumeIsolated({ AsukaRuntime.shared.node(of: v) }) {
            let i = n.inset
            r.origin.x += i.left
            r.origin.y += i.top
            r.size.width = max(r.width - i.left - i.right, 0)
            r.size.height = max(r.height - i.top - i.bottom, 0)
        }
        return r
    }
}

/// A TextField's view: it reports its focus.
final class AsukaTextField: NSTextField {
    override func becomeFirstResponder() -> Bool {
        let ok = super.becomeFirstResponder()
        if ok, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.fieldFocused(n) }
        return ok
    }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.reassertShadow(n) }
    }
}

/// A secure TextField's view.
final class AsukaSecureField: NSSecureTextField {
    override func becomeFirstResponder() -> Bool {
        let ok = super.becomeFirstResponder()
        if ok, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.fieldFocused(n) }
        return ok
    }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.reassertShadow(n) }
    }
}

/// An Image's or a Symbol's view: it takes no clicks.
final class AsukaImageView: NSImageView {
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.reassertShadow(n) }
    }
}

/// A window's content: flipped, so the root sits at the top left, and its
/// subviews aren't resized with it (the core lays them out).
final class AsukaContentView: AsukaView {
    override init(frame: NSRect) {
        super.init(frame: frame)
        autoresizesSubviews = false
    }
    required init?(coder: NSCoder) { fatalError() }
}

@MainActor extension AsukaRuntime {
    /// Gathers the texts under a view, in order.
    func collectLabels(_ v: NSView, _ out: inout [String]) {
        for s in v.subviews where !s.isHidden {
            if let n = node(of: s) {
                if !n.a11yLabel.isEmpty { out.append(n.a11yLabel); continue }
                if n.kind == .text && !n.text.isEmpty { out.append(n.text); continue }
            }
            collectLabels(s, &out)
        }
    }

    func adjust(_ v: NSView, _ by: Int64) -> Bool {
        guard let n = node(of: v), n.adjustable else { return false }
        emit(n, .adjust) { AsukaEvents.adjust($0, by) }
        return true
    }
}
