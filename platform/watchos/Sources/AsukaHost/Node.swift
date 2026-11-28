// The watch host's node store: one object per view the core made (and per
// list cell the host made), observed by the SwiftUI view that draws it.
//
// SwiftUI is a second reconciler, so the store keeps it from being one: a
// node's props are plain fields SwiftUI doesn't observe, written as the
// core's batch is decoded, and a batch publishes once at its end, a
// node's `rev` bumped once for each node it changed (Runtime.publish).
// Only that node's view runs its body again, with the frame the core
// gave it: SwiftUI never lays out what the core placed.

import CoreText
import Foundation
import Observation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

@MainActor @Observable public final class AsukaNode: Identifiable {
    public let id: UInt32
    /// nil for a list cell the host made.
    public let kind: AsukaKind?

    /// What the view's body reads: bumped once per batch that changed the
    /// node (its props, its frame or its children).
    public internal(set) var rev = 0

    @ObservationIgnored public internal(set) var children: [AsukaNode] = []
    @ObservationIgnored public internal(set) weak var parent: AsukaNode?
    /// Its frame in its parent's coordinates, in points (the core's).
    @ObservationIgnored public internal(set) var frame = CGRect.zero
    /// Whether a frame has come: a view without one isn't drawn yet.
    @ObservationIgnored var placed = false
    @ObservationIgnored var listens = Set<UInt16>()

    // Common style.
    @ObservationIgnored var background: UInt64 = 0
    @ObservationIgnored var cornerRadius: CGFloat = 0
    @ObservationIgnored var borderWidth: CGFloat = 0
    @ObservationIgnored var borderColor: UInt64 = 0
    @ObservationIgnored var opacity: Double = 1
    @ObservationIgnored var hidden = false
    @ObservationIgnored var clip = false
    @ObservationIgnored var disabled = false
    @ObservationIgnored var color: UInt64 = 0
    @ObservationIgnored var textStyle: Int32 = 0
    @ObservationIgnored var fontSize: Float = 0
    @ObservationIgnored var fontWeight: Int32 = 0
    @ObservationIgnored var textAlign: Int32 = 0
    @ObservationIgnored var lineLimit = 0
    /// Style.Transform: translate x, y, scale x, y, rotation in degrees.
    @ObservationIgnored var transform: (tx: CGFloat, ty: CGFloat, sx: CGFloat, sy: CGFloat, deg: Double) = (0, 0, 1, 1, 0)
    @ObservationIgnored var a11yLabel = ""
    @ObservationIgnored var a11yHint = ""
    @ObservationIgnored var a11yHidden = false
    @ObservationIgnored var a11yValue = ""

    // The kinds' own: a text's, a title, a value...
    @ObservationIgnored var text = ""
    @ObservationIgnored var title = ""
    @ObservationIgnored var icon = ""
    @ObservationIgnored var variant: Int32 = 0
    @ObservationIgnored var placeholder = ""
    @ObservationIgnored var secure = false
    @ObservationIgnored var on = false
    @ObservationIgnored var value: Double = 0
    @ObservationIgnored var min: Double = 0
    @ObservationIgnored var max: Double = 1
    @ObservationIgnored var step: Double = 1
    @ObservationIgnored var indeterminate = false
    @ObservationIgnored var items: [String] = []
    @ObservationIgnored var selected = 0
    @ObservationIgnored var source = ""
    @ObservationIgnored var mode: Int32 = 0
    @ObservationIgnored var dateMode: Int32 = 0
    /// A Shape's outline, fill and stroke.
    @ObservationIgnored var path: Path?
    @ObservationIgnored var fill: UInt64 = 0
    @ObservationIgnored var stroke: UInt64 = 0
    @ObservationIgnored var lineWidth: CGFloat = 1
    @ObservationIgnored var horizontal = false
    /// An Image's picture, once loaded.
    @ObservationIgnored var image: CGImage?
    /// A RichText's runs.
    @ObservationIgnored var runs: [AsukaRichRun] = []
    /// A Native's view (a package's).
    @ObservationIgnored var native: AsukaNativeView?

    // Navigation and presentation.
    /// A Screen's bar buttons, an Alert's buttons.
    @ObservationIgnored var actions: [AsukaActionItem] = []
    @ObservationIgnored var message = ""
    @ObservationIgnored var alertStyle: Int32 = 0
    /// A screen popped or a sheet dismissed by the user, which the core
    /// hasn't destroyed yet: it's gone from what's shown.
    @ObservationIgnored var dying = false
    /// The environment last reported for it (a screen's, a sheet's).
    @ObservationIgnored var lastEnv: AsukaEnvironment?
    /// A Sheet's FullScreen.
    @ObservationIgnored var fullScreen = false

    /// A List's state.
    @ObservationIgnored var list: AsukaWatchList?
    /// A list cell's list and the row it shows.
    @ObservationIgnored weak var cellOf: AsukaWatchList?
    @ObservationIgnored var cellIndex = -1

    /// A Scroll's or a List's offset to go to (ScrollToOffset), and a
    /// count of the scrolls asked for, which its view scrolls on as it
    /// changes (the same offset again too: the end of content that grew).
    @ObservationIgnored var scrollTarget = CGPoint.zero
    @ObservationIgnored var scrollRequests = 0
    /// The requests its view has gone to (one asked for as it's made too).
    @ObservationIgnored var scrollsDone = 0
    @ObservationIgnored var scrollAnimated = false

    init(id: UInt32, kind: AsukaKind?) {
        self.id = id
        self.kind = kind
    }

    /// Whether the core listens for event on it.
    func listens(_ e: AsukaEventKind) -> Bool { listens.contains(e.rawValue) }

    func insert(_ child: AsukaNode, at index: Int) {
        child.parent?.remove(child)
        let i = Swift.min(Swift.max(index, 0), children.count)
        children.insert(child, at: i)
        child.parent = self
    }

    func remove(_ child: AsukaNode) {
        if let i = children.firstIndex(where: { $0 === child }) { children.remove(at: i) }
        if child.parent === self { child.parent = nil }
    }

    /// The children a container draws: those placed, not hidden away by
    /// a pop or a dismissal.
    var shownChildren: [AsukaNode] { children.filter { !$0.dying } }
}

/// A run of a RichText: its text in its style's font, color and flags (1
/// italic, 2 monospaced, 4 underline, 8 strikethrough), and its link.
struct AsukaRichRun {
    var text: String
    var font: CTFont
    var color: UInt64
    var flags: UInt8
    var link: String
}
