import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// asuka dev's performance overlay (DevPerf, to the root): the core's
// numbers (frames a second, flushes, the last flush's phases, memory) in a
// small box over the top trailing corner of the main window's content,
// below its title bar, in front of the app's views, letting every click
// through and hidden from VoiceOver. The core sends its text twice a
// second while it's on, and none to remove it.

/// The overlay's box: its text, monospaced, on a dark translucent
/// background; it lets every click through.
final class AsukaDevPerfView: NSView {
    let label = NSTextField(labelWithString: "")

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.withAlphaComponent(0.72).cgColor
        layer?.cornerRadius = 6
        label.font = .monospacedSystemFont(ofSize: 10, weight: .medium)
        label.textColor = .white
        label.maximumNumberOfLines = 0
        label.lineBreakMode = .byWordWrapping
        addSubview(label)
        setAccessibilityElement(false)
        label.setAccessibilityElement(false)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:)") }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override var isFlipped: Bool { true }

    /// Its size for its text, within width.
    func fit(_ width: CGFloat) -> NSSize {
        let s = label.sizeThatFits(NSSize(width: width - 12, height: .greatestFiniteMagnitude))
        label.frame = NSRect(x: 6, y: 4, width: ceil(s.width), height: ceil(s.height))
        return NSSize(width: ceil(s.width) + 12, height: ceil(s.height) + 8)
    }
}

/// The box showing; none when none does.
@MainActor private var shownPerf: AsukaDevPerfView?

@MainActor extension AsukaRuntime {
    /// DevPerf: shows the text in the box, or removes it (no text).
    func devPerf(_ args: UnsafeRawBufferPointer) {
        let text = String(decoding: args, as: UTF8.self)
        guard !text.isEmpty, let content = main?.content else {
            shownPerf?.removeFromSuperview()
            shownPerf = nil
            return
        }
        let v = shownPerf ?? AsukaDevPerfView()
        shownPerf = v
        if v.label.stringValue != text { v.label.stringValue = text }
        // In front of the app's views, those it added since too.
        if v.superview !== content || content.subviews.last !== v {
            content.addSubview(v, positioned: .above, relativeTo: nil)
        }
        let inset = content.safeAreaInsets
        let size = v.fit(content.bounds.width - inset.left - inset.right - 16)
        let x = rtl ? inset.left + 8 : content.bounds.width - inset.right - 8 - size.width
        let top = inset.top + 6
        let y = content.isFlipped ? top : content.bounds.height - top - size.height
        v.frame = NSRect(x: x, y: y, width: size.width, height: size.height)
        // As the window resizes, it stays in its corner.
        v.autoresizingMask = [rtl ? .maxXMargin : .minXMargin, content.isFlipped ? .maxYMargin : .minYMargin]
    }
}
