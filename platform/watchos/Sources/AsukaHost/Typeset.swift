// How a text is typeset, which its measure (Measure.swift) and its view
// (Views.swift) share, so it's drawn as it was measured: its font (with
// FontTraits' variants: the CTFont's), letter spacing (CoreText's kern,
// SwiftUI's kerning), line height (its lines' extra room, SwiftUI's
// lineSpacing between them and half of it above the first and below the
// last, as CSS centers a line in its height), and MinimumScale's scale,
// found the same way for both: the largest step of 64 between the
// minimum and 1 at which its lines (LineLimit, or one) hold it at its
// width. A step that fits at the measured width fits at the frame it
// gives (no wider), and the next didn't fit at a wider one, so the view
// finds the measure's.

import CoreText
import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

@MainActor enum AsukaTypeset {
    /// Whether the node shrinks to fit (MinimumScale between 0 and 1).
    static func shrinks(_ n: AsukaNode) -> Bool { n.minimumScale > 0 && n.minimumScale < 1 }

    /// The most lines it shows (0: any): its LineLimit, or one when it
    /// shrinks without one.
    static func lines(_ n: AsukaNode) -> Int { n.lineLimit > 0 ? n.lineLimit : shrinks(n) ? 1 : 0 }

    /// Its font at scale.
    static func font(_ n: AsukaNode, _ scale: CGFloat) -> CTFont { sized(AsukaFonts.font(of: n), scale) }

    static func sized(_ f: CTFont, _ scale: CGFloat) -> CTFont {
        scale == 1 ? f : CTFontCreateCopyWithAttributes(f, CTFontGetSize(f) * scale, nil, nil)
    }

    /// Its letter spacing at the user's text size and scale.
    static func kern(_ n: AsukaNode, _ scale: CGFloat) -> CGFloat {
        n.letterSpacing == 0 ? 0 : AsukaFonts.scaled(n.letterSpacing, n) * scale
    }

    /// A line's own height in f: ascent, descent and leading, as CoreText
    /// and SwiftUI stack lines.
    static func natural(_ f: CTFont) -> CGFloat { CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f) }

    /// The room each line takes beyond its font's (0 without a line
    /// height, or one less than the font's: the watch's lines don't get
    /// tighter than the font sets them).
    static func extra(_ n: AsukaNode, _ scale: CGFloat) -> CGFloat {
        guard n.lineHeight > 0 else { return 0 }
        return Swift.max(AsukaFonts.scaled(n.lineHeight, n) * scale - natural(font(n, scale)), 0)
    }

    /// Its text (a Text's, or a RichText's runs) in its fonts and
    /// spacing at scale.
    static func attributed(_ n: AsukaNode, _ scale: CGFloat) -> NSAttributedString {
        let k = kern(n, scale)
        func attrs(_ f: CTFont) -> [NSAttributedString.Key: Any] {
            var a: [NSAttributedString.Key: Any] = [NSAttributedString.Key(kCTFontAttributeName as String): f]
            if k != 0 { a[NSAttributedString.Key(kCTKernAttributeName as String)] = k }
            return a
        }
        if n.kind == .richText {
            let out = NSMutableAttributedString()
            for r in n.runs { out.append(NSAttributedString(string: r.text, attributes: attrs(sized(r.font(runTraits(n, r)), scale)))) }
            return out
        }
        return NSAttributedString(string: n.text, attributes: attrs(font(n, scale)))
    }

    /// A run's FontTraits: the view's, and its own italic.
    static func runTraits(_ n: AsukaNode, _ r: AsukaRichRun) -> Int { n.fontTraits | (r.flags & 1 != 0 ? 1 : 0) }

    /// Its lines at width, all of them.
    static func lineCount(_ s: NSAttributedString, _ width: CGFloat) -> Int {
        let fs = CTFramesetterCreateWithAttributedString(s as CFAttributedString)
        let path = CGPath(rect: CGRect(x: 0, y: 0, width: Swift.min(width, 100_000), height: 1_000_000), transform: nil)
        let frame = CTFramesetterCreateFrame(fs, CFRange(location: 0, length: 0), path, nil)
        return CFArrayGetCount(CTFrameGetLines(frame))
    }

    static let steps = 64

    /// The scale it's drawn at in width: 1, or with MinimumScale the
    /// largest step whose lines hold it (the minimum when none does).
    static func scale(_ n: AsukaNode, width: CGFloat) -> CGFloat {
        guard shrinks(n), width.isFinite, width > 0, !(n.text.isEmpty && n.runs.isEmpty) else { return 1 }
        let most = lines(n)
        func at(_ k: Int) -> CGFloat { n.minimumScale + (1 - n.minimumScale) * CGFloat(k) / CGFloat(steps) }
        func fits(_ k: Int) -> Bool { lineCount(attributed(n, at(k)), width) <= most }
        if fits(steps) { return 1 }
        // The largest k that fits, between lo (fits, or the minimum) and
        // hi (doesn't).
        var lo = 0, hi = steps
        if !fits(0) { return at(0) }
        while hi - lo > 1 {
            let mid = (lo + hi) / 2
            if fits(mid) { lo = mid } else { hi = mid }
        }
        return at(lo)
    }
}

/// A Shadow on the wire: its color, blur radius (2σ, as CSS's) and
/// offset.
struct AsukaShadow {
    var color: UInt64
    var radius, x, y: CGFloat
}

/// A text's look beyond its font: its letter spacing, line height,
/// truncation and shadow, laid on the Text the view draws.
struct AsukaTextLook: ViewModifier {
    let node: AsukaNode
    let scale: CGFloat

    func body(content: Content) -> some View {
        let _ = node.rev // what it reads of the node is the batch's
        let n = node
        let extra = AsukaTypeset.extra(n, scale)
        let lines = AsukaTypeset.lines(n)
        let clip = AsukaTruncation(rawValue: n.truncation) == .clip
        let (ta, a) = textAlignment(n.textAlign)
        content
            .multilineTextAlignment(ta)
            // Clip: no ellipsis; one line runs on past the frame's edge,
            // more wrap and the frame cuts them after their last.
            .lineLimit(clip ? (lines == 1 ? 1 : nil) : (lines > 0 ? lines : nil))
            .truncationMode(truncationMode(n.truncation))
            .lineSpacing(extra)
            .padding(.vertical, extra / 2)
            .fixedSize(horizontal: clip && lines == 1, vertical: true)
            .modifier(AsukaTextShadow(shadow: n.textShadow))
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: a)
            .modifier(AsukaClip(on: clip, shape: Rectangle()))
    }

    private func truncationMode(_ v: Int32) -> Text.TruncationMode {
        switch AsukaTruncation(rawValue: v) {
        case .head: return .head
        case .middle: return .middle
        default: return .tail
        }
    }
}

/// TextShadow: under the glyphs, a blur of its radius's half (as the
/// other Apple hosts' layers take it).
struct AsukaTextShadow: ViewModifier {
    let shadow: AsukaShadow?

    func body(content: Content) -> some View {
        if let s = shadow, let c = asukaColor(s.color) {
            content.shadow(color: c, radius: s.radius / 2, x: s.x, y: s.y)
        } else {
            content
        }
    }
}
