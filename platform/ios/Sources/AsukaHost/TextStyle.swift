#if canImport(UIKit)
import UIKit
import CoreText
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A font with FontTraits' variants: 1 its italic face (slanted, where
/// it has none), 2 monospaced digits (tnum), 4 small capitals (smcp).
/// Applied before Dynamic Type scales a font, so the scaled one keeps
/// them.
func asukaTraited(_ f: UIFont, _ traits: Int64) -> UIFont {
    guard traits != 0 else { return f }
    var d = f.fontDescriptor
    if traits & 1 != 0 {
        if let i = d.withSymbolicTraits(d.symbolicTraits.union(.traitItalic)), UIFont(descriptor: i, size: f.pointSize).fontDescriptor.symbolicTraits.contains(.traitItalic) {
            d = i
        } else {
            // No italic face: the upright one, slanted as CoreText's
            // synthesized obliques are (about 12°).
            d = d.withMatrix(CGAffineTransform(a: 1, b: 0, c: 0.21, d: 1, tx: 0, ty: 0))
        }
    }
    var features: [[UIFontDescriptor.FeatureKey: Int]] = []
    if traits & 2 != 0 { features.append([.type: kNumberSpacingType, .selector: kMonospacedNumbersSelector]) }
    if traits & 4 != 0 { features.append([.type: kLowerCaseType, .selector: kLowerCaseSmallCapsSelector]) }
    if !features.isEmpty { d = d.addingAttributes([.featureSettings: features]) }
    return UIFont(descriptor: d, size: f.pointSize)
}

/// Dynamic Type's scaling of a text style's points (a letter spacing, a
/// line height), at the app's text size: as its font is scaled.
@MainActor func asukaScaled(_ v: CGFloat, _ textStyle: Int32) -> CGFloat {
    guard v != 0 else { return 0 }
    return UIFontMetrics(forTextStyle: uiTextStyle(textStyle)).scaledValue(for: v, compatibleWith: UITraitCollection(preferredContentSizeCategory: asukaContentSize))
}

/// The baseline offset that sets a line's glyphs in the middle of a line
/// height taller (or shorter) than the font's, as CSS's half-leading
/// does: UIKit puts the extra room above them.
func asukaBaselineOffset(_ lineHeight: CGFloat, _ font: UIFont) -> CGFloat {
    (lineHeight - font.lineHeight) / 2
}

/// The line break mode of Truncation, where a label is cut.
func asukaLineBreak(_ truncation: Int32) -> NSLineBreakMode {
    switch AsukaTruncation(rawValue: truncation) {
    case .head: return .byTruncatingHead
    case .middle: return .byTruncatingMiddle
    case .clip: return .byClipping
    default: return .byTruncatingTail
    }
}

/// A TextShadow (as Shadow: u64 color, f32 blur radius, offset x, y), or
/// none (its color 0).
func asukaTextShadow(_ b: UnsafeRawBufferPointer) -> NSShadow? {
    guard b.count >= 20 else { return nil }
    var r = AsukaReader(b)
    guard let c = asukaColor(r.u64()) else { return nil }
    let s = NSShadow()
    s.shadowColor = c
    // A blur radius as CSS's: twice the Gaussian's σ, which is what
    // Core Graphics' blur takes.
    s.shadowBlurRadius = CGFloat(r.f32())
    s.shadowOffset = CGSize(width: CGFloat(r.f32()), height: CGFloat(r.f32()))
    return s
}

/// A rich text's shadow (its base's TextShadow), as a layout key.
struct AsukaRichShadow: Hashable {
    var color: UIColor
    var blur: CGFloat
    var x: CGFloat, y: CGFloat

    init(_ s: NSShadow) {
        color = (s.shadowColor as? UIColor) ?? .black
        blur = s.shadowBlurRadius
        x = s.shadowOffset.width
        y = s.shadowOffset.height
    }
}

/// MinimumScale's search: the largest scale from min to 1 at which fits
/// holds, to 1/128, or min when none does. fits is monotonic: a smaller
/// text fits where a larger one does.
func asukaFitScale(min lo: CGFloat, _ fits: (CGFloat) -> Bool) -> CGFloat {
    guard lo > 0, lo < 1 else { return 1 }
    if fits(1) { return 1 }
    if !fits(lo) { return lo }
    var a = lo, b: CGFloat = 1 // fits at a, not at b
    while b - a > 1.0 / 128 {
        let m = (a + b) / 2
        if fits(m) { a = m } else { b = m }
    }
    return a
}

/// Measures attributed texts as a label lays them out.
@MainActor private let asukaSizer: UILabel = {
    let l = UILabel()
    l.numberOfLines = 0
    return l
}()

@MainActor extension AsukaNode {
    /// Whether its label's text is attributed: what a label can't draw
    /// from its own properties.
    var labelStyled: Bool {
        letterSpacing != 0 || lineHeight > 0 || textShadow != nil || shrinks
    }

    /// MinimumScale is on: it shrinks its text to fit.
    var shrinks: Bool { minimumScale > 0 && minimumScale < 1 }

    /// The most lines its label shows: its limit, or one while it shrinks
    /// to fit them.
    var labelLines: Int { lineLimit > 0 ? lineLimit : shrinks ? 1 : 0 }

    var labelAlignment: NSTextAlignment {
        switch AsukaTextAlign(rawValue: textAlign) {
        case .center: return .center
        case .end: return isRTL ? .left : .right
        case .justify: return .justified
        default: return AsukaRuntime.shared.startAlignment(isRTL)
        }
    }

    /// Its label's text, at scale: the font, the letter spacing and the
    /// line height all scaled, and the shadow.
    func labelString(_ text: String, scale: CGFloat) -> NSAttributedString {
        let base = font
        let f = scale < 1 ? base.withSize(base.pointSize * scale) : base
        var a: [NSAttributedString.Key: Any] = [.font: f, .foregroundColor: asukaColor(color) ?? UIColor.label]
        let ls = asukaScaled(letterSpacing, textStyle) * scale
        if ls != 0 { a[.kern] = ls }
        let p = NSMutableParagraphStyle()
        p.alignment = labelAlignment
        let lh = asukaScaled(lineHeight, textStyle) * scale
        if lh > 0 {
            p.minimumLineHeight = lh
            p.maximumLineHeight = lh
            a[.baselineOffset] = asukaBaselineOffset(lh, f)
        }
        a[.paragraphStyle] = p
        if let s = textShadow { a[.shadow] = s }
        return NSAttributedString(string: text, attributes: a)
    }

    /// The scale its label's text fits its lines at, at width (its
    /// content's): 1 unless it shrinks. Found once per width while the
    /// text and its props stay.
    func labelScale(_ text: String, width: CGFloat) -> CGFloat {
        guard shrinks, width > 0, width < 1e6, !text.isEmpty else { return 1 }
        if let hit = fitCache.first(where: { $0.width == width }) { return hit.scale }
        let lines = CGFloat(labelLines)
        let s = asukaFitScale(min: minimumScale) { scale in
            let str = labelString(text, scale: scale)
            let sizer = asukaSizer
            sizer.numberOfLines = 0
            sizer.attributedText = str
            let h = sizer.sizeThatFits(CGSize(width: width, height: .greatestFiniteMagnitude)).height
            let f = font
            let lh = lineHeight > 0 ? asukaScaled(lineHeight, textStyle) * scale : f.withSize(f.pointSize * scale).lineHeight
            return h <= lh * lines + 0.5
        }
        if fitCache.count >= 4 { fitCache.removeFirst() }
        fitCache.append((width, s))
        return s
    }

    /// Its label's size at width, as it's drawn there: the size of its
    /// text at the scale it fits at.
    func labelSize(_ l: AsukaLabel, width: CGFloat) -> CGSize {
        let text = l.source
        let scale = labelScale(text, width: width)
        let sizer = asukaSizer
        sizer.numberOfLines = labelLines
        sizer.lineBreakMode = asukaLineBreak(truncation)
        sizer.attributedText = labelString(text, scale: scale)
        return sizer.sizeThatFits(CGSize(width: width, height: .greatestFiniteMagnitude))
    }

    /// Its label's props changed: what it fits at is found again, and
    /// its text made again.
    func restyleLabel() {
        fitCache.removeAll(keepingCapacity: true)
        guard let l = view as? AsukaLabel else { return }
        l.numberOfLines = labelLines
        l.lineBreakMode = asukaLineBreak(truncation)
        guard labelStyled else {
            if l.styled {
                // Back to the label's own properties.
                l.styled = false
                l.shrink = 1
                // A label keeps the attributes of the text it had for the
                // text it's given: the plain ones first.
                l.attributedText = NSAttributedString(string: l.source, attributes: [.font: font, .paragraphStyle: NSParagraphStyle.default])
                l.font = font
                l.adjustsFontForContentSizeCategory = !l.holdsFont
                l.textColor = asukaColor(color) ?? .label
                l.textAlignment = labelAlignment
                l.lineBreakMode = asukaLineBreak(truncation)
            }
            return
        }
        l.styled = true
        l.adjustsFontForContentSizeCategory = false
        let width = l.bounds.width - l.contentInset.left - l.contentInset.right
        l.shrink = labelScale(l.source, width: width)
        l.attributedText = labelString(l.source, scale: l.shrink)
        // After the text: a label's mode cuts its last line, where its
        // text's paragraph style would make it one line.
        l.lineBreakMode = asukaLineBreak(truncation)
    }

    /// Its frame changed: a label that shrinks to fit takes the scale its
    /// new width fits at.
    func fitLabel() {
        guard shrinks, let l = view as? AsukaLabel, l.styled else { return }
        let width = l.bounds.width - l.contentInset.left - l.contentInset.right
        let s = labelScale(l.source, width: width)
        if s != l.shrink {
            l.shrink = s
            l.attributedText = labelString(l.source, scale: s)
            l.lineBreakMode = asukaLineBreak(truncation)
        }
    }

    /// A text field's letter spacing, in its text and its placeholder.
    func applyFieldSpacing() {
        guard let t = view as? UITextField else { return }
        let ls = asukaScaled(letterSpacing, textStyle)
        t.defaultTextAttributes[.kern] = ls != 0 ? ls : nil
    }
}

@MainActor extension AsukaRuntime {
    /// The text props a label can't take from its own properties
    /// (LetterSpacing, LineHeight, FontTraits, TextShadow, Truncation,
    /// MinimumScale): each goes to the label, a rich text's base, a field
    /// or a button as it can take it. Whether prop is one.
    func setTextStyle(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) -> Bool {
        switch prop {
        case AsukaProp.letterSpacing:
            n.letterSpacing = CGFloat(v.float)
            (n.view as? AsukaRichTextView)?.base.letterSpacing = Float(v.float)
            n.applyFieldSpacing()
            if n.view is UIButton { n.applyFont() }
        case AsukaProp.lineHeight:
            n.lineHeight = CGFloat(v.float)
            (n.view as? AsukaRichTextView)?.base.lineHeight = Float(v.float)
        case AsukaProp.fontTraits:
            n.fontTraits = v.int
            (n.view as? AsukaRichTextView)?.base.traits = v.int
            n.applyFont()
        case AsukaProp.textShadow:
            guard case .bytes(let b) = v else { return true }
            n.textShadow = asukaTextShadow(b)
            (n.view as? AsukaRichTextView)?.base.shadow = n.textShadow.map { AsukaRichShadow($0) }
        case AsukaProp.truncation:
            n.truncation = v.enumValue
            (n.view as? AsukaRichTextView)?.base.truncation = v.enumValue
        case AsukaProp.minimumScale:
            n.minimumScale = CGFloat(v.float)
        default:
            return false
        }
        n.restyleLabel()
        return true
    }
}
#endif
