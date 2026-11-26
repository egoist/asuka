#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// asuka dev's inspector's highlight (DevHighlight): a translucent box over
// the view the inspector selected, in front of everything in its window,
// taking no touches.

/// The highlight showing; none when none does.
@MainActor private weak var shownHighlight: UIView?

extension AsukaRuntime {
    /// DevHighlight: shows a highlight over a rect of v (from its top
    /// left; a width below 0: all of it), instead of the one showing.
    func showDevHighlight(_ v: UIView, _ args: UnsafeRawBufferPointer) {
        clearDevHighlight()
        guard args.count >= 16, let w = v.window else { return }
        var r = AsukaReader(args)
        var rect = CGRect(x: CGFloat(r.f32()), y: CGFloat(r.f32()), width: CGFloat(r.f32()), height: CGFloat(r.f32()))
        if rect.width < 0 || rect.height < 0 { rect = v.bounds }
        let h = UIView(frame: v.convert(rect, to: w))
        h.isUserInteractionEnabled = false
        h.isAccessibilityElement = false
        h.accessibilityElementsHidden = true
        h.backgroundColor = UIColor.systemBlue.withAlphaComponent(0.18)
        h.layer.borderColor = UIColor.systemBlue.cgColor
        h.layer.borderWidth = 1
        w.addSubview(h)
        shownHighlight = h
    }

    /// DevHighlightClear: removes the highlight showing.
    func clearDevHighlight() {
        shownHighlight?.removeFromSuperview()
    }
}
#endif
