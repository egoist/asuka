// The Mac host's model of what the core made: a node per view the core
// created (and per list cell the host made), holding its AppKit view and
// what the host must remember of its props.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A view the core made, or a list cell the host made.
@MainActor final class AsukaNode {
    let id: UInt32
    /// nil for a list cell (the host's own).
    let kind: AsukaKind?
    /// Its NSView; nil for what has none of its own (an alert, a sheet
    /// before it shows: navigation's nodes that AppKit shows otherwise).
    var view: NSView?
    var children: [AsukaNode] = []
    weak var parent: AsukaNode?
    /// The events the core listens for on it.
    var listens = Set<UInt16>()
    /// Its Keys prop, decoded (Keys.swift).
    var keys: [AsukaKey] = []

    // Text props.
    var textStyle: Int32 = 0
    var fontWeight: Int32 = 0
    var textAlign: Int32 = 0
    var fontSize: Float = 0
    var fontFamily = ""
    var color: UInt64 = 0
    var lineLimit = 0
    /// LetterSpacing and LineHeight, in points (0: the font's).
    var letterSpacing: CGFloat = 0
    var lineHeight: CGFloat = 0
    /// FontTraits' bits: 1 italic, 2 monospaced digits, 4 small capitals.
    var fontTraits = 0
    /// TextShadow's bytes, as Shadow's.
    var textShadow: [UInt8] = []
    /// Truncation (AsukaTruncation), MinimumScale (0: none).
    var truncation: Int32 = 0
    var minimumScale: CGFloat = 0
    /// The scale a shrinking text is drawn at (TextStyle.swift).
    var textScale: CGFloat = 1
    /// Its label draws an attributed string of its props.
    var styledLabel = false

    // Visual props.
    var background: UInt64 = 0
    var borderColor: UInt64 = 0
    var cornerRadius: CGFloat = 0
    var borderWidth: CGFloat = 0
    var opacity: CGFloat = 1
    var shadow: [UInt8] = []
    var gradient: [UInt8] = []
    var gradientLayer: CAGradientLayer?
    /// CornerRadii (top left, top right, bottom right, bottom left) and
    /// BorderWidths (top, right, bottom, left): all 0 when they're the same
    /// (CornerRadius, BorderWidth say).
    var cornerRadii: [CGFloat] = [0, 0, 0, 0]
    var borderWidths: [CGFloat] = [0, 0, 0, 0]
    var borderStyle: Int32 = 0
    /// The layers that draw a box CALayer's own properties can't: per-corner
    /// radii, per-side or dashed borders (Box.swift).
    var box: AsukaBox?
    var transform: [UInt8] = []
    /// The transform its layer's animation holds (Props.swift).
    var held = CATransform3DIdentity
    var clip = false
    /// Its children's view, clipped, while it clips and casts a shadow.
    var clipView: NSView?
    /// An NSVisualEffectView behind its content (Material).
    var effect: NSVisualEffectView?
    /// Its frame, as the core last placed it.
    var frame = CGRect.zero
    /// It has had a frame.
    var placed = false

    // Menus (its Menu prop: a button's pop-up, a view's context menu).
    var menu: NSMenu?
    var menuActions: [AsukaActionItem] = []
    var menuTrigger: Int32 = 0

    // Kind-specific.
    var title = "" // Button, Screen, Window
    var message = "" // Alert, Snackbar
    var variant: Int32 = 0 // Button
    var icon = "" // Button, Screen
    var actions: [AsukaActionItem] = [] // a Screen's bar buttons, an Alert's buttons
    var text = "" // Text; a TextField's last reported value
    var multiline = false
    var plain = false // TextField's Plain
    var secure = false
    var pasteTypes: [String] = []
    /// ContentInset: top, right, bottom, left points.
    var inset: (top: CGFloat, right: CGFloat, bottom: CGFloat, left: CGFloat) = (0, 0, 0, 0)
    var items: [String] = [] // Segmented, Wheel, Picker
    var placeholder = ""
    var symbol = ""
    var selected = 0
    var pickerLabel = ""
    var minValue = 0.0, maxValue = 1.0
    var a11yLabel = ""
    var adjustable = false
    /// A TextField's text view, when it's multiline (its view is then the
    /// scroll view holding it).
    var textView: NSTextView?
    /// Its selection, as last reported or set (UTF-16).
    var selection: (Int, Int) = (0, 0)
    var selectionPending = false
    var selectionQueued = false
    /// Observes its scroll position (Scrolled).
    var observesScroll = false
    var horizontal = false // a horizontal Scroll
    var paging = false // a Scroll that settles on pages
    /// Being popped or dismissed: its views leave with it.
    var dying = false
    /// ui.OnBack: going back asks the core (Back).
    var interceptBack = false
    var presented = false
    /// Style.Transition's bytes, and whether it's coming in with it.
    var transition: [UInt8] = []
    var entering = false
    /// Style.Match's key.
    var match = ""
    /// Its LayoutDirection prop, and the way it reads.
    var direction = AsukaLayoutDirection.inherit
    var rtl = false
    var tooltip = ""

    // What each part of the host keeps of a node (their files'). Made as
    // the part needs it.
    var list: AsukaList? // List.swift
    var cell: AsukaListCell? // List.swift: a list cell's
    var table: AsukaTable? // Tables.swift: a Table's or an Outline's
    var tableCell: AsukaTableCell? // Tables.swift: a Table's or an Outline's row's cell
    var cursor: Int32 = 0 // Cursor.swift: its Cursor (0: Auto)
    var cursorView: AsukaCursorView? // Cursor.swift: what registers its cursor rect
    var refresh: AsukaRefresher? // Refresh.swift
    var nav: AsukaNav? // Navigation.swift: stacks, screens, tabs, sheets, alerts, windows
    var snack: AsukaSnack? // Snackbar.swift
    var popover: AsukaPopover? // Popover.swift
    var split: AsukaSplit? // Split.swift
    var dots: AsukaPageDots? // Pager.swift
    var zoom: AsukaZoom? // Zoom.swift: a zoomable Scroll's
    var rich: AsukaRich? // RichText.swift
    var richField: AsukaRichField? // RichField.swift
    var shape: AsukaShape? // Shape.swift
    var ring: AsukaRing? // ProgressRing.swift
    var canvas: [UInt8] = [] // Canvas.swift: its display list
    var shader: AsukaShader? // Shader.swift
    var web: AsukaWeb? // Web.swift
    var video: AsukaVideo? // Video.swift
    var image: AsukaImageState? // Image.swift
    var native: AsukaNativeHolder? // Native.swift
    var interaction: AsukaInteraction? // Interaction.swift: hover, press, drag and drop, gestures
    var window: AsukaWin? // Windows.swift: a Window's window
    var focus: AsukaFocusProps? // Focus.swift: the keyboard's focus (Focusable, NoFocusOnPress, NoFocusRing, FocusTrap)
    var a11y: AsukaA11y? // A11y.swift: screen readers' roles, states and relations
    var overlay: AsukaOverlay? // Overlay.swift: an Overlay's layer

    init(id: UInt32, kind: AsukaKind?, view: NSView?) {
        self.id = id
        self.kind = kind
        self.view = view
    }

    func listening(_ e: AsukaEventKind) -> Bool { listens.contains(e.rawValue) }

    /// Whether an ancestor is being popped or dismissed.
    var underDying: Bool {
        var p = parent
        while let n = p {
            if n.dying { return true }
            p = n.parent
        }
        return false
    }

    /// The view its children's views go in.
    var container: NSView? {
        if let c = cell { return c.content }
        if kind == .scroll { return nil } // its child is the document view
        if let c = clipView { return c }
        return view
    }

    /// Its corner radius as drawn: no more than half its sides.
    var cornerOf: CGFloat {
        if frame.width == 0 { return cornerRadius }
        return min(cornerRadius, frame.width / 2, frame.height / 2)
    }

    /// It casts a shadow of a color.
    var hasShadow: Bool {
        guard shadow.count >= 20 else { return false }
        return shadow.withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) } != 0
    }
}
