// Measuring for the core: synchronous, on the main thread, as the core asks
// in its layout (asuka.h's measure). The watch has no UIKit views to ask
// sizeThatFits, so:
//
//   - Text and RichText: CoreText (CTFramesetterSuggestFrameSizeWithConstraints)
//     in the CTFont the view draws with (Style.swift's AsukaFonts: the
//     system font at the watch's Dynamic Type size), so what's drawn wraps
//     where it was measured; min-content is the widest word.
//   - Controls (Button, Toggle, Slider, Stepper, TextField, Picker,
//     ProgressView, Symbol...): SwiftUI's own layout of the very view the
//     host draws (AsukaLeaf), through ImageRenderer at the proposed width:
//     the watch's metrics for each control and text size, measured, not
//     tabulated. The core caches measurements by content (its shared
//     measure cache), so a control is measured once per title and size.
//   - Images: their natural size (Loaded), which the core keeps.

import CoreText
import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

@MainActor extension AsukaRuntime {
    func measure(_ id: UInt32, _ w: Float, _ h: Float, _ aw: Float, _ ah: Float) -> CGSize {
        guard let n = nodes[id] else { return .zero }
        var s: CGSize
        switch n.kind {
        case .text:
            s = n.text.isEmpty ? .zero : AsukaMeasure.text(attributed(n.text, AsukaFonts.font(of: n)), w, aw, lines: n.lineLimit)
        case .richText:
            s = n.runs.isEmpty ? .zero : AsukaMeasure.text(attributed(n.runs), w, aw, lines: n.lineLimit)
        case .image:
            s = n.image.map { CGSize(width: CGFloat($0.width) / scale, height: CGFloat($0.height) / scale) } ?? .zero
        case .button, .toggle, .stepper, .symbol, .activity, .progressRing, .pageControl, .datePicker, .native:
            // Their own width (as wide as their content), at most what's
            // available.
            s = AsukaMeasure.control(n, width: w.isNaN ? nil : CGFloat(w))
            if w.isNaN, aw.isFinite, s.width > CGFloat(Swift.max(aw, 0)) {
                s = AsukaMeasure.control(n, width: CGFloat(Swift.max(aw, 0)))
            }
        case .slider, .textField, .progress, .segmented, .wheel, .picker:
            // As wide as they're let be (the watch's are the screen's width).
            let width: CGFloat = !w.isNaN ? CGFloat(w) : aw.isFinite ? CGFloat(Swift.max(aw, 0)) : 120
            s = AsukaMeasure.control(n, width: width)
            s.width = width
            if s.height < 1 {
                // A control an ImageRenderer draws nothing of (a text
                // field: it shows the system's input): a line of its font
                // in the watch's field, as tall as its buttons.
                let f = AsukaFonts.font(of: n)
                let line = CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f)
                s.height = Swift.max(ceil(line + 22), 44)
            }
        default:
            s = .zero
        }
        if !w.isNaN { s.width = CGFloat(w) }
        if !h.isNaN { s.height = CGFloat(h) }
        let k = scale
        return CGSize(width: ceil(s.width * k) / k, height: ceil(s.height * k) / k)
    }

    private func attributed(_ s: String, _ font: CTFont) -> NSAttributedString {
        NSAttributedString(string: s, attributes: [NSAttributedString.Key(kCTFontAttributeName as String): font])
    }

    private func attributed(_ runs: [AsukaRichRun]) -> NSAttributedString {
        let out = NSMutableAttributedString()
        for r in runs { out.append(attributed(r.text, r.font)) }
        return out
    }
}

@MainActor enum AsukaMeasure {
    /// A text's size: at a known width w, or the width available (aw:
    /// +inf max-content, -inf min-content), at most lines lines (0: any).
    static func text(_ s: NSAttributedString, _ w: Float, _ aw: Float, lines: Int) -> CGSize {
        let big = CGFloat.greatestFiniteMagnitude
        let width: CGFloat
        if !w.isNaN {
            width = CGFloat(w)
        } else if aw == .infinity {
            width = big
        } else if aw == -.infinity {
            width = widestWord(s)
        } else {
            width = CGFloat(Swift.max(aw, 0))
        }
        let fs = CTFramesetterCreateWithAttributedString(s as CFAttributedString)
        var size = CTFramesetterSuggestFrameSizeWithConstraints(fs, CFRange(location: 0, length: 0), nil, CGSize(width: width, height: big), nil)
        if lines > 0 {
            // The first lines' height: the frame's lines, laid out.
            let path = CGPath(rect: CGRect(x: 0, y: 0, width: Swift.min(width, 100_000), height: 100_000), transform: nil)
            let frame = CTFramesetterCreateFrame(fs, CFRange(location: 0, length: 0), path, nil)
            let all = CTFrameGetLines(frame) as! [CTLine]
            if all.count > lines {
                var h: CGFloat = 0
                var lw: CGFloat = 0
                for l in all.prefix(lines) {
                    var a: CGFloat = 0, d: CGFloat = 0, lead: CGFloat = 0
                    lw = Swift.max(lw, CGFloat(CTLineGetTypographicBounds(l, &a, &d, &lead)))
                    h += a + d + lead
                }
                size = CGSize(width: Swift.max(size.width, lw), height: h)
            }
        }
        if width < big { size.width = Swift.min(size.width, width) }
        return size
    }

    /// The widest word: a text's min-content width.
    static func widestWord(_ s: NSAttributedString) -> CGFloat {
        var widest: CGFloat = 0
        let str = s.string as NSString
        str.enumerateSubstrings(in: NSRange(location: 0, length: str.length), options: .byWords) { _, range, _, _ in
            let line = CTLineCreateWithAttributedString(s.attributedSubstring(from: range) as CFAttributedString)
            widest = Swift.max(widest, CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil)))
        }
        return ceil(widest)
    }

    /// A control's size as SwiftUI lays it out at a proposed width (nil:
    /// its ideal width): the view the host draws for it, rendered once.
    static func control(_ n: AsukaNode, width: CGFloat?) -> CGSize {
        let r = ImageRenderer(content: AsukaLeaf(node: n).fixedSize(horizontal: width == nil, vertical: true))
        r.proposedSize = ProposedViewSize(width: width, height: nil)
        r.scale = 1
        guard let img = r.cgImage else { return .zero }
        return CGSize(width: CGFloat(img.width), height: CGFloat(img.height))
    }
}
