// Text styled beyond its font (the text part's): FontTraits (italic,
// monospaced digits, small capitals) wherever a font is made, and a Text's
// LetterSpacing, LineHeight, TextShadow, Truncation and MinimumScale. A
// Text that has any of the last draws an attributed string, which it's
// measured with too; one that shrinks finds its scale with one function
// where it's measured and where it's drawn.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// CoreText's feature types and selectors (SFNTLayoutTypes.h).
private let numberSpacingType = 6, monospacedNumbersSelector = 0
private let lowerCaseType = 37, lowerCaseSmallCapsSelector = 1

/// Measures a font's line height as TextKit lays a line of it out.
@MainActor private let lineMeasurer = NSLayoutManager()

@MainActor extension AsukaRuntime {
    /// f with FontTraits' bits: its italic face (slanted, when it has
    /// none), monospaced digits, small capitals.
    func traited(_ f: NSFont, _ traits: Int) -> NSFont {
        guard traits != 0 else { return f }
        var font = f
        if traits & 1 != 0 {
            let it = NSFontManager.shared.convert(font, toHaveTrait: .italicFontMask)
            if it.fontDescriptor.symbolicTraits.contains(.italic) {
                font = it
            } else {
                // No italic face: the regular one, slanted 12°, as CSS's
                // synthesized oblique.
                var m = AffineTransform(scale: font.pointSize)
                m.prepend(AffineTransform(m11: 1, m12: 0, m21: -tan(12 * .pi / 180), m22: 1, tX: 0, tY: 0))
                font = NSFont(descriptor: font.fontDescriptor, textTransform: m) ?? font
            }
        }
        var features: [[NSFontDescriptor.FeatureKey: Int]] = []
        if traits & 2 != 0 { features.append([.typeIdentifier: numberSpacingType, .selectorIdentifier: monospacedNumbersSelector]) }
        if traits & 4 != 0 { features.append([.typeIdentifier: lowerCaseType, .selectorIdentifier: lowerCaseSmallCapsSelector]) }
        if !features.isEmpty {
            let d = font.fontDescriptor.addingAttributes([.featureSettings: features])
            font = NSFont(descriptor: d, size: font.pointSize) ?? font
        }
        return font
    }

    /// A Text's line limit as drawn: its LineLimit, or one while it
    /// shrinks to fit.
    func effectiveLines(_ n: AsukaNode) -> Int {
        n.lineLimit > 0 ? n.lineLimit : shrinks(n) ? 1 : 0
    }

    func shrinks(_ n: AsukaNode) -> Bool { n.minimumScale > 0 && n.minimumScale < 1 }

    func hasTextShadow(_ n: AsukaNode) -> Bool { n.textShadow.count >= 20 && n.textShadow.u64(0) != 0 }

    /// Whether a Text draws an attributed string of its props, rather than
    /// its plain string in its font and color.
    func styledText(_ n: AsukaNode) -> Bool {
        n.letterSpacing != 0 || n.lineHeight > 0 || hasTextShadow(n) || shrinks(n)
    }

    /// How a Text's lines break: one line is cut where its Truncation
    /// says; more wrap, the last cut at its end.
    func lineBreak(_ n: AsukaNode) -> NSLineBreakMode {
        guard effectiveLines(n) == 1 else { return .byWordWrapping }
        switch AsukaTruncation(rawValue: n.truncation) {
        case .head: return .byTruncatingHead
        case .middle: return .byTruncatingMiddle
        case .clip: return .byClipping
        default: return .byTruncatingTail
        }
    }

    /// A Text's attributes at scale s (its font's size, its letter spacing
    /// and line height times s), its lines broken by lineBreak.
    func textAttributes(_ n: AsukaNode, scale s: CGFloat, lineBreak mode: NSLineBreakMode? = nil) -> [NSAttributedString.Key: Any] {
        var f = font(n)
        if s != 1 { f = NSFont(descriptor: f.fontDescriptor, size: f.pointSize * s) ?? f }
        let para = NSMutableParagraphStyle()
        para.alignment = alignment(n)
        para.baseWritingDirection = writingDirection(n)
        para.lineBreakMode = mode ?? lineBreak(n)
        var a: [NSAttributedString.Key: Any] = [.font: f, .foregroundColor: asukaColor(n.color) ?? NSColor.labelColor]
        // Points in the text size it's drawn at (asuka dev's preview's).
        let k = previewFontScale * s
        if n.letterSpacing != 0 { a[.kern] = n.letterSpacing * k }
        if n.lineHeight > 0 {
            let lh = n.lineHeight * k
            para.minimumLineHeight = lh
            para.maximumLineHeight = lh
            // TextKit puts a line's extra height over its glyphs: they're
            // raised by half of it, into its middle, as CSS's half-leading.
            a[.baselineOffset] = (lh - lineMeasurer.defaultLineHeight(for: f)) / 2
        }
        if hasTextShadow(n) {
            let b = n.textShadow
            let sh = NSShadow()
            sh.shadowColor = asukaColor(b.u64(0))
            // A blur radius of 2σ, as a view's shadow's.
            sh.shadowBlurRadius = CGFloat(b.f32(8))
            // Shadows are offset in the window's space, y up.
            sh.shadowOffset = NSSize(width: CGFloat(b.f32(12)), height: -CGFloat(b.f32(16)))
            a[.shadow] = sh
        }
        a[.paragraphStyle] = para
        return a
    }

    /// The scale a shrinking Text fits its width (its content's: the
    /// label's own inset is taken off) in its lines at: the largest from
    /// its MinimumScale to 1, to 1/128; 1 when it doesn't shrink.
    func fitScale(_ n: AsukaNode, _ width: CGFloat) -> CGFloat {
        let m = n.minimumScale
        guard shrinks(n), width < asukaBig, !n.text.isEmpty else { return 1 }
        let lines = CGFloat(max(effectiveLines(n), 1))
        let room = max(width - 4, 1) // the label cell's inset, 2 on each side
        func fits(_ s: CGFloat) -> Bool {
            let a = textAttributes(n, scale: s, lineBreak: .byWordWrapping)
            let str = NSAttributedString(string: n.text, attributes: a)
            if lines == 1 { return str.size().width <= room + 0.25 }
            let h = str.boundingRect(with: CGSize(width: room, height: asukaBig), options: [.usesLineFragmentOrigin, .usesFontLeading]).height
            let f = a[.font] as? NSFont ?? font(n)
            let line = n.lineHeight > 0 ? n.lineHeight * previewFontScale * s : lineMeasurer.defaultLineHeight(for: f)
            return h <= lines * line + 0.5
        }
        if fits(1) { return 1 }
        if !fits(m) { return m }
        var lo = m, hi: CGFloat = 1
        for _ in 0..<7 {
            let mid = (lo + hi) / 2
            if fits(mid) { lo = mid } else { hi = mid }
        }
        return lo
    }

    /// The width a placed Text draws its text in.
    func textWidth(_ n: AsukaNode) -> CGFloat { max(n.frame.width - n.inset.left - n.inset.right, 0) }

    /// Sets a Text's line limit and how its lines break.
    func applyLabelLines(_ n: AsukaNode) {
        guard n.kind == .text, let t = n.view as? NSTextField else { return }
        let lines = effectiveLines(n)
        t.maximumNumberOfLines = lines
        t.cell?.truncatesLastVisibleLine = lines > 0 && AsukaTruncation(rawValue: n.truncation) != .clip
        t.lineBreakMode = lineBreak(n)
    }

    /// A Text's label again, from its props: an attributed string while
    /// it's styled; its plain string, font and color once it no longer is.
    func applyStyledText(_ n: AsukaNode) {
        guard n.kind == .text, let t = n.view as? NSTextField else { return }
        applyLabelLines(n)
        guard styledText(n) else {
            guard n.styledLabel else { return }
            n.styledLabel = false
            n.textScale = 1
            t.font = font(n)
            t.textColor = asukaColor(n.color) ?? .labelColor
            t.alignment = alignment(n)
            t.stringValue = n.text
            return
        }
        n.styledLabel = true
        if n.placed { n.textScale = fitScale(n, textWidth(n)) }
        t.attributedStringValue = NSAttributedString(string: n.text, attributes: textAttributes(n, scale: n.textScale))
    }

    /// A Text has a new frame: a shrinking one fits it.
    func textStylePlaced(_ n: AsukaNode) {
        guard n.kind == .text, shrinks(n) else { return }
        let s = fitScale(n, textWidth(n))
        if s != n.textScale { applyStyledText(n) }
    }

    /// A styled Text's size at width w (its label's cell's, with its
    /// attributed string at the scale it fits w at).
    func measureStyledText(_ n: AsukaNode, _ cell: NSCell, _ w: CGFloat) -> CGSize {
        let s = fitScale(n, w)
        let c: NSCell
        if s == n.textScale && n.styledLabel {
            c = cell
        } else {
            c = cell.copy() as? NSCell ?? cell
            c.attributedStringValue = NSAttributedString(string: n.text, attributes: textAttributes(n, scale: s))
        }
        var size = c.cellSize(forBounds: CGRect(x: 0, y: 0, width: w, height: asukaBig))
        if w < asukaBig { size.width = min(size.width, w) }
        return size
    }

    /// The text props: what's set on n, and its Text's label again.
    func setTextStyleProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        switch p {
        case AsukaProp.letterSpacing: n.letterSpacing = CGFloat(v.float)
        case AsukaProp.lineHeight: n.lineHeight = CGFloat(v.float)
        case AsukaProp.textShadow: n.textShadow = v.data
        case AsukaProp.truncation: n.truncation = v.enumValue
        case AsukaProp.minimumScale: n.minimumScale = CGFloat(v.float)
        case AsukaProp.fontTraits:
            n.fontTraits = Int(v.int)
            applyFont(n)
            return
        default: return
        }
        if n.rich != nil { richInvalidate(n) } else { applyStyledText(n) }
    }

    /// What the tests read of a styled Text: its attributes as drawn.
    func textStyleJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard n.kind == .text, let t = n.view as? NSTextField else { return }
        var j: [String: Any] = ["styled": n.styledLabel, "scale": Double(n.textScale), "lineBreak": t.lineBreakMode.rawValue]
        let a = t.attributedStringValue
        if a.length > 0 {
            if let k = a.attribute(.kern, at: 0, effectiveRange: nil) as? CGFloat { j["kern"] = Double(k) }
            if let p = a.attribute(.paragraphStyle, at: 0, effectiveRange: nil) as? NSParagraphStyle, p.minimumLineHeight > 0 {
                j["lineHeight"] = Double(p.minimumLineHeight)
            }
            if let f = a.attribute(.font, at: 0, effectiveRange: nil) as? NSFont {
                j["fontSize"] = Double(f.pointSize)
                j["italic"] = f.fontDescriptor.symbolicTraits.contains(.italic) || f.fontDescriptor.object(forKey: .matrix) != nil
                if let fs = f.fontDescriptor.object(forKey: .featureSettings) as? [[NSFontDescriptor.FeatureKey: Any]] { j["features"] = fs.count }
            }
            if let s = a.attribute(.shadow, at: 0, effectiveRange: nil) as? NSShadow {
                j["shadow"] = [Double(s.shadowBlurRadius), Double(s.shadowOffset.width), Double(s.shadowOffset.height)]
            }
        }
        d["textStyle"] = j
    }
}
