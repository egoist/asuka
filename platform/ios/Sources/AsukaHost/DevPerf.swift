#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// asuka dev's performance overlay (DevPerf, to the root): the core's
// numbers (frames a second, flushes, the last flush's phases, memory) in a
// small box over the top trailing corner of the app's window, below its
// safe area's top, in front of the app's screens and what they present,
// taking no touches and hidden from VoiceOver. The core sends its text
// twice a second while it's on, and none to remove it.

/// The box showing; none when none does.
@MainActor private var shownPerf: AsukaDevPerfView?

/// The overlay's box: its text, monospaced, on a dark translucent
/// background.
final class AsukaDevPerfView: UIView {
    let label = UILabel()

    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        accessibilityElementsHidden = true
        backgroundColor = UIColor.black.withAlphaComponent(0.72)
        layer.cornerRadius = 6
        layer.cornerCurve = .continuous
        label.numberOfLines = 0
        label.font = .monospacedSystemFont(ofSize: 10, weight: .medium)
        label.textColor = .white
        addSubview(label)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:)") }

    static let pad = UIEdgeInsets(top: 4, left: 6, bottom: 4, right: 6)

    /// Its size for its text, within width.
    func fit(_ width: CGFloat) -> CGSize {
        let p = Self.pad
        let s = label.sizeThatFits(CGSize(width: width - p.left - p.right, height: .greatestFiniteMagnitude))
        label.frame = CGRect(x: p.left, y: p.top, width: ceil(s.width), height: ceil(s.height))
        return CGSize(width: ceil(s.width) + p.left + p.right, height: ceil(s.height) + p.top + p.bottom)
    }
}

extension AsukaRuntime {
    /// DevPerf: shows the text in the box, or removes it (no text).
    func showDevPerf(_ args: UnsafeRawBufferPointer) {
        let text = String(decoding: args, as: UTF8.self)
        guard !text.isEmpty, let w = controller?.view.window ?? nodes[rootID]?.view.window else {
            shownPerf?.removeFromSuperview()
            shownPerf = nil
            return
        }
        let v = shownPerf ?? AsukaDevPerfView()
        shownPerf = v
        if v.label.text != text { v.label.text = text }
        if v.superview !== w { w.addSubview(v) }
        // Over what the app presented since.
        w.bringSubviewToFront(v)
        let inset = w.safeAreaInsets
        let size = v.fit(w.bounds.width - inset.left - inset.right - 16)
        let x = rtl ? inset.left + 8 : w.bounds.width - inset.right - 8 - size.width
        v.frame = CGRect(x: x, y: inset.top + 4, width: size.width, height: size.height)
    }
}
#endif
