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
//     A Button is never measured wider than watchOS draws buttons (the
//     screen less its margins), and the host says that width as it
//     measures one (asuka_measure_max, `widest`): a button the core
//     stretches across a wider column, or grows, gets that frame,
//     centered where it was stretched, so its frame is what's drawn.
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
        case .text, .richText:
            s = n.text.isEmpty && n.runs.isEmpty ? .zero : AsukaMeasure.text(n, w, aw)
        case .image:
            s = n.image.map { CGSize(width: CGFloat($0.width) / scale, height: CGFloat($0.height) / scale) } ?? .zero
        case .button, .toggle, .stepper, .symbol, .activity, .progressRing, .pageControl, .datePicker, .native:
            // Their own width (as wide as their content), at most what's
            // available, and a button at most as wide as it's drawn.
            s = AsukaMeasure.control(n, width: w.isNaN ? nil : CGFloat(w))
            var most = aw.isFinite ? CGFloat(Swift.max(aw, 0)) : .infinity
            if let widest = widest(n) { most = Swift.min(most, widest) }
            if w.isNaN, s.width > most {
                s = AsukaMeasure.control(n, width: most)
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

    /// The widest watchOS draws view id, whatever frame it gets, which the
    /// measure callback tells the core (asuka_measure_max): nil for views
    /// that fill their frame.
    func widest(_ id: UInt32) -> CGFloat? {
        nodes[id].flatMap { widest($0) }
    }

    /// A Button's widest: watchOS draws no bordered button (bordered,
    /// prominent, glass) wider than the screen less its margins, 178pt on
    /// a 46mm watch, whatever it's proposed; what it draws at a proposal
    /// wider than any screen is that, measured, not tabulated. A plain
    /// button with a background of its own is its label in the app's
    /// background, which fills its frame: it has none.
    func widest(_ n: AsukaNode) -> CGFloat? {
        guard n.kind == .button, !(AsukaButtonVariant(rawValue: n.variant) == .plain && n.background != 0) else { return nil }
        return AsukaMeasure.widest(n)
    }
}

@MainActor enum AsukaMeasure {
    /// A Text's or RichText's size as it's typeset (Typeset.swift): its
    /// fonts, letter spacing and line height, at the scale MinimumScale
    /// finds at its width (its min-content: the minimum's widest word).
    static func text(_ n: AsukaNode, _ w: Float, _ aw: Float) -> CGSize {
        let room: CGFloat = !w.isNaN ? CGFloat(w) : aw.isFinite ? CGFloat(Swift.max(aw, 0)) : .infinity
        var scale = AsukaTypeset.scale(n, width: room)
        if w.isNaN, aw == -.infinity, AsukaTypeset.shrinks(n) { scale = n.minimumScale }
        let s = AsukaTypeset.attributed(n, scale)
        let lines = AsukaTypeset.lines(n)
        var size = text(s, w, aw, lines: lines)
        let extra = AsukaTypeset.extra(n, scale)
        if extra > 0 {
            // Each line its height: the font's and the extra room
            // (between lines, and half above the first and below the
            // last).
            var count = AsukaTypeset.lineCount(s, width(s, w, aw))
            if lines > 0 { count = Swift.min(count, lines) }
            size.height = CGFloat(count) * (AsukaTypeset.natural(AsukaTypeset.font(n, scale)) + extra)
        }
        return size
    }

    /// The width a text is laid out in: a known width w, or the width
    /// available (aw: +inf max-content, -inf min-content).
    static func width(_ s: NSAttributedString, _ w: Float, _ aw: Float) -> CGFloat {
        if !w.isNaN { return CGFloat(w) }
        if aw == .infinity { return .greatestFiniteMagnitude }
        if aw == -.infinity { return widestWord(s) }
        return CGFloat(Swift.max(aw, 0))
    }

    /// A text's size: at a known width w, or the width available (aw:
    /// +inf max-content, -inf min-content), at most lines lines (0: any).
    static func text(_ s: NSAttributedString, _ w: Float, _ aw: Float, lines: Int) -> CGSize {
        let big = CGFloat.greatestFiniteMagnitude
        let width = width(s, w, aw)
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

    /// What a button looks like to SwiftUI's layout: what its widest
    /// depends on.
    struct ButtonLook: Hashable {
        var title, icon: String
        var variant: Int32
        var font: CGFloat
        var weight: Int32
        var traits: Int
    }

    /// The widest buttons are drawn, by look: measured once each.
    static var widestButtons: [ButtonLook: CGFloat] = [:]

    /// The widest SwiftUI draws a button, whatever it's proposed: its
    /// width proposed more than any screen.
    static func widest(_ n: AsukaNode) -> CGFloat {
        let look = ButtonLook(title: n.title, icon: n.icon, variant: n.variant, font: CTFontGetSize(AsukaFonts.font(of: n)), weight: n.fontWeight, traits: n.fontTraits)
        if let w = widestButtons[look] { return w }
        if widestButtons.count >= 256 { widestButtons.removeAll() }
        let w = control(n, width: 10_000).width
        widestButtons[look] = w
        return w
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
