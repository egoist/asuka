import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// asuka dev's inspector's highlight (DevHighlight): a translucent box over
// the view the inspector selected, in front of everything in its window,
// taking no clicks.

/// A box that lets every click through to what's under it.
private final class AsukaHighlightView: NSView {
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

/// The highlight showing; none when none does.
@MainActor private weak var shownHighlight: NSView?

@MainActor extension AsukaRuntime {
    /// DevHighlight: shows a highlight over a rect of v (from its top
    /// left; a width below 0: all of it), instead of the one showing.
    func devHighlight(_ v: NSView?, _ args: UnsafeRawBufferPointer) {
        devHighlightClear()
        guard args.count >= 16, let v, let content = v.window?.contentView else { return }
        var r = AsukaReader(args)
        var rect = NSRect(x: CGFloat(r.f32()), y: CGFloat(r.f32()), width: CGFloat(r.f32()), height: CGFloat(r.f32()))
        if rect.width < 0 || rect.height < 0 { rect = v.bounds }
        else if !v.isFlipped { rect.origin.y = v.bounds.height - rect.maxY }
        let h = AsukaHighlightView(frame: v.convert(rect, to: content))
        h.wantsLayer = true
        h.layer?.backgroundColor = NSColor.systemBlue.withAlphaComponent(0.18).cgColor
        h.layer?.borderColor = NSColor.systemBlue.cgColor
        h.layer?.borderWidth = 1
        h.setAccessibilityElement(false)
        content.addSubview(h, positioned: .above, relativeTo: nil)
        shownHighlight = h
    }

    /// DevHighlightClear: removes the highlight showing.
    func devHighlightClear() {
        shownHighlight?.removeFromSuperview()
    }
}
