#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// Cursor on iPadOS's pointer (a trackpad's or a mouse's): a view that sets
// one has a UIPointerInteraction whose style is its: Text a vertical beam,
// Hidden none, Pointer the system's effect on the view (a highlight or a
// lift, as on a button). The pointer has no other shapes of its own (no
// crosshair, resize or zoom arrows), so the others are the system's
// pointer. A view inside it that sets none shows its (UIKit asks the
// nearest interaction under the pointer); its own wins. An iPhone has no
// pointer, nor a TV.

/// What iPadOS's pointer does for a Cursor.
enum AsukaPointer: Equatable {
    /// The system's pointer: no interaction.
    case system
    /// A vertical beam, as over text.
    case beam
    /// The system's effect on the view.
    case effect
    case hidden

    init(_ cursor: Int32) {
        switch AsukaCursor(rawValue: cursor) {
        case .text: self = .beam
        case .pointer: self = .effect
        case .hidden: self = .hidden
        default: self = .system
        }
    }
}

#if !os(tvOS)
final class AsukaPointerDelegate: NSObject, UIPointerInteractionDelegate {
    var pointer: AsukaPointer

    init(_ pointer: AsukaPointer) { self.pointer = pointer }

    func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? {
        guard let view = interaction.view else { return nil }
        switch pointer {
        case .beam:
            // As tall as the view's text's line, else body text's.
            let font = (view as? UILabel)?.font ?? UIFont.preferredFont(forTextStyle: .body)
            return UIPointerStyle(shape: .verticalBeam(length: ceil(font.lineHeight)), constrainedAxes: [])
        case .hidden:
            return .hidden()
        case .effect:
            guard view.window != nil else { return nil }
            return UIPointerStyle(effect: .automatic(UITargetedPreview(view: view)))
        case .system:
            return nil
        }
    }
}
#endif

extension AsukaRuntime {
    /// Whether this device has a pointer to style: an iPad's (an iPhone
    /// app on a Mac has the Mac's own cursor).
    var hasPointer: Bool {
        #if os(tvOS)
        return false
        #else
        return UIDevice.current.userInterfaceIdiom == .pad && !ProcessInfo.processInfo.isiOSAppOnMac
        #endif
    }

    /// The Cursor prop.
    func setCursor(_ n: AsukaNode, _ cursor: Int32) {
        #if !os(tvOS)
        let p = AsukaPointer(cursor)
        guard hasPointer || n.pointer != nil else { return }
        if p == .system {
            if let i = n.pointer { n.view.removeInteraction(i) }
            n.pointer = nil
            n.pointerDelegate = nil
            return
        }
        if let d = n.pointerDelegate {
            d.pointer = p
            n.pointer?.invalidate()
            return
        }
        let d = AsukaPointerDelegate(p)
        let i = UIPointerInteraction(delegate: d)
        n.view.addInteraction(i)
        n.pointer = i
        n.pointerDelegate = d
        #endif
    }
}
#endif
