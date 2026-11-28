// Measuring a leaf for the core's layout: its AppKit view's own size for
// what the layout offers.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// Wider or taller than anything: an unbounded offer.
let asukaBig: CGFloat = 1e7

/// A size the layout offers: known, or NaN; the space available, a
/// definite size, +inf (max-content) or -inf (min-content).
struct AsukaOffer {
    var width: Float, height: Float, availWidth: Float, availHeight: Float

    var widthKnown: Bool { !width.isNaN }
    var heightKnown: Bool { !height.isNaN }
    var minContent: Bool { availWidth == -.infinity }
    var maxContent: Bool { availWidth == .infinity }

    /// The width a view may take: the known size, the space available,
    /// unbounded for max-content, minContent for min-content.
    func fitWidth(_ minContent: CGFloat) -> CGFloat {
        if widthKnown { return CGFloat(width) }
        if availWidth >= .infinity { return asukaBig }
        if availWidth <= -.infinity { return minContent }
        return CGFloat(max(availWidth, 0))
    }

    /// The available width when it's definite.
    var finiteWidth: CGFloat? { availWidth.isFinite ? CGFloat(max(availWidth, 0)) : nil }
    var finiteHeight: CGFloat? { availHeight.isFinite ? CGFloat(max(availHeight, 0)) : nil }
}

@MainActor extension AsukaRuntime {
    /// The size view id wants within the offer, rounded up to its window's
    /// pixels.
    func measure(_ id: UInt32, _ w: Float, _ h: Float, _ aw: Float, _ ah: Float) -> CGSize {
        guard let n = nodes[id], let v = n.view else { return .zero }
        let c = AsukaOffer(width: w, height: h, availWidth: aw, availHeight: ah)
        var s = measure(n, v, c)
        if c.widthKnown { s.width = CGFloat(w) }
        if c.heightKnown { s.height = CGFloat(h) }
        // Rounded to its own window's pixels: a Window may be on a display
        // of another scale.
        var k = CGFloat(env.scale)
        if let win = v.window { k = win.backingScaleFactor }
        if k <= 0 { k = 2 }
        return CGSize(width: (s.width * k).rounded(.up) / k, height: (s.height * k).rounded(.up) / k)
    }

    func measure(_ n: AsukaNode, _ v: NSView, _ c: AsukaOffer) -> CGSize {
        switch n.kind {
        case .text:
            guard !n.text.isEmpty, let cell = (v as? NSTextField)?.cell else { return .zero }
            var widest: CGFloat = 0
            if c.minContent && !c.widthKnown { widest = widestWord(n) }
            let w = c.fitWidth(widest)
            if n.styledLabel || styledText(n) { return measureStyledText(n, cell, w) } // TextStyle.swift
            var s = cell.cellSize(forBounds: CGRect(x: 0, y: 0, width: w, height: asukaBig))
            if w < asukaBig { s.width = min(s.width, w) }
            return s
        case .textField:
            if n.textView != nil { return measureMultiline(n, c) }
            // A field scrolls its text: it can be narrow.
            var s = v.intrinsicContentSize
            s.width = 200
            if c.minContent { s.width = 80 } else if let w = c.finiteWidth { s.width = min(s.width, w) }
            if s.height < 0 { s.height = 22 }
            return s
        case .slider, .progress:
            var s = v.intrinsicContentSize
            s.width = c.finiteWidth ?? 200
            if s.height <= 0 { s.height = 20 }
            return s
        case .progressRing:
            return measureRing(n, c)
        case .richText:
            return measureRich(n, c)
        case .symbol, .image:
            return measureImage(n, c)
        case .webView, .video, .shape, .canvas, .view, .selectable, .scroll, .list, .shader, .accessory:
            return .zero
        case .pageControl:
            return measureDots(n)
        case .chip:
            // Its title, icon (or checkmark) and remove button's room, in
            // the recessed bezel (Chip.swift).
            return (v as? NSControl)?.cell?.cellSize ?? v.intrinsicContentSize
        case .disclosure:
            // As wide as it's laid out (its title truncates), as tall as its
            // row.
            var s = (v as? AsukaDisclosureView)?.fit() ?? .zero
            if let w = c.finiteWidth, !c.maxContent { s.width = min(s.width, w) }
            return s
        case .dateRangePicker:
            return (v as? AsukaDateRangeView)?.fit() ?? .zero
        case .native:
            var w = c.width, h = c.height
            if w.isNaN, c.availWidth.isFinite { w = c.availWidth }
            if !w.isFinite { w = .nan }
            if !h.isFinite { h = .nan }
            return measureNative(n, w, h)
        default:
            var s = v.intrinsicContentSize
            if s.width < 0 { s.width = c.finiteWidth ?? 0 } // NSView.noIntrinsicMetric
            if s.height < 0 { s.height = 0 }
            return s
        }
    }

    /// A text's min-content width: its widest word.
    func widestWord(_ n: AsukaNode) -> CGFloat {
        var attrs: [NSAttributedString.Key: Any] = [.font: (n.view as? NSTextField)?.font ?? styleFont(0)]
        if styledText(n) { attrs = textAttributes(n, scale: 1) } // its letter spacing; a shrinking one's words at its size
        var widest: CGFloat = 0
        for w in n.text.split(whereSeparator: { $0.isWhitespace }) {
            widest = max(widest, (String(w) as NSString).size(withAttributes: attrs).width)
        }
        // The label's own insets, as cellSize(forBounds:) adds them.
        return widest.rounded(.up) + 4
    }
}
