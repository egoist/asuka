// Colors, fonts and paths as the watch draws them: SwiftUI's colors (the
// watch's palette: light on black), and the system font (SF Compact) at
// the watch's Dynamic Type sizes, as a CTFont that measuring (Measure.swift)
// and drawing (Font(ctFont)) share, so a text is drawn as it was measured.

import CoreText
import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif
#if os(watchOS)
import UIKit // UIFont and UIFontMetrics only: the watch has no UIKit views
#elseif os(macOS)
import AppKit
#endif

/// A wire color (ops.Color): 0 (the default: nil), semantic or RGBA.
func asukaColor(_ v: UInt64) -> Color? {
    switch v >> 32 {
    case 1:
        let a = 255 - Double((v >> 24) & 0xff)
        guard let c = semanticColor(AsukaSemanticColor(rawValue: Int32(truncatingIfNeeded: v & 0xffff))) else { return nil }
        return a == 255 ? c : c.opacity(a / 255)
    case 2:
        let c = UInt32(truncatingIfNeeded: v)
        return Color(.sRGB, red: Double((c >> 24) & 0xff) / 255, green: Double((c >> 16) & 0xff) / 255,
                     blue: Double((c >> 8) & 0xff) / 255, opacity: Double(c & 0xff) / 255)
    default:
        return nil
    }
}

/// The semantic colors as the watch has them: its apps are dark, their
/// text light on black, their rows on a dark gray platter.
func semanticColor(_ c: AsukaSemanticColor?) -> Color? {
    switch c {
    case .label: return .primary
    case .secondaryLabel: return .secondary
    case .tertiaryLabel: return Color.white.opacity(0.4)
    case .background, .groupedBackground: return .black
    case .secondaryBackground, .secondaryGroupedBackground: return Color(white: 0.13)
    case .separator: return Color.white.opacity(0.2)
    case .accent: return .accentColor
    case .red: return .red
    case .orange: return .orange
    case .yellow: return .yellow
    case .green: return .green
    case .blue: return .blue
    case .purple: return .purple
    case .pink: return .pink
    case .gray: return .gray
    case .clear: return .clear
    case .onAccent: return .white
    default: return nil
    }
}

#if os(watchOS)
typealias AsukaFont = UIFont
typealias AsukaFontDescriptor = UIFontDescriptor
#else
typealias AsukaFont = NSFont
typealias AsukaFontDescriptor = NSFontDescriptor
#endif

/// The fonts texts and controls are measured and drawn in.
@MainActor enum AsukaFonts {
    private static var cache: [Int64: CTFont] = [:]

    /// Dynamic Type changed: the fonts are made again at the new size.
    static func reset() { cache.removeAll() }

    static func textStyle(_ v: Int32) -> AsukaFont.TextStyle {
        switch AsukaTextStyle(rawValue: v) {
        case .largeTitle: return .largeTitle
        case .title: return .title1
        case .title2: return .title2
        case .title3: return .title3
        case .headline: return .headline
        case .subheadline: return .subheadline
        case .callout: return .callout
        case .footnote: return .footnote
        case .caption: return .caption1
        case .caption2: return .caption2
        default: return .body
        }
    }

    static func weight(_ v: Int32) -> AsukaFont.Weight? {
        switch AsukaFontWeight(rawValue: v) {
        case .ultralight: return .ultraLight
        case .thin: return .thin
        case .light: return .light
        case .regular: return .regular
        case .medium: return .medium
        case .semibold: return .semibold
        case .bold: return .bold
        case .heavy: return .heavy
        case .black: return .black
        default: return nil
        }
    }

    /// The font of a text style (Body by default) at the user's text
    /// size, or of a size the app gave (scaled as Body is), in a weight.
    static func font(style: Int32, size: Float, weight w: Int32) -> CTFont {
        let key = Int64(style) | Int64(w) << 8 | Int64(size.bitPattern) << 16
        if let f = cache[key] { return f }
        let ts = textStyle(style)
        var f: AsukaFont = AsukaFont.preferredFont(forTextStyle: ts)
        if size > 0 {
            #if os(watchOS)
            let scaled = UIFontMetrics(forTextStyle: .body).scaledValue(for: CGFloat(size))
            #else
            let scaled = CGFloat(size)
            #endif
            f = AsukaFont.systemFont(ofSize: scaled, weight: weight(w) ?? .regular)
        } else if let wt = weight(w) {
            let d = f.fontDescriptor.addingAttributes([.traits: [AsukaFontDescriptor.TraitKey.weight: wt]])
            #if os(watchOS)
            f = UIFont(descriptor: d, size: f.pointSize)
            #else
            f = NSFont(descriptor: d, size: f.pointSize) ?? f
            #endif
        }
        let ct = f as CTFont
        cache[key] = ct
        return ct
    }

    /// A node's font: its TextStyle, FontSize and FontWeight.
    static func font(of n: AsukaNode) -> CTFont { font(style: n.textStyle, size: n.fontSize, weight: n.fontWeight) }

    /// The user's text size against the default's (1 at Large), which
    /// the environment reports as fontScale.
    static var scale: Float {
        #if os(watchOS)
        return Float(UIFontMetrics(forTextStyle: .body).scaledValue(for: 100) / 100)
        #else
        return 1
        #endif
    }
}

/// Text alignment as SwiftUI has it.
func textAlignment(_ v: Int32) -> (TextAlignment, Alignment) {
    switch AsukaTextAlign(rawValue: v) {
    case .center: return (.center, .top)
    case .end: return (.trailing, .topTrailing)
    default: return (.leading, .topLeading)
    }
}

/// A Shape's path from its wire format: u8 PathOp, then its f32 arguments.
func asukaPath(_ b: UnsafeRawBufferPointer) -> Path {
    var p = Path()
    var r = AsukaReader(b)
    func f() -> CGFloat { r.p + 4 <= r.end ? CGFloat(r.f32()) : 0 }
    while r.p < r.end {
        switch AsukaPathOp(rawValue: Int32(r.u8())) {
        case .move: p.move(to: CGPoint(x: f(), y: f()))
        case .line: p.addLine(to: CGPoint(x: f(), y: f()))
        case .quad:
            let c = CGPoint(x: f(), y: f())
            p.addQuadCurve(to: CGPoint(x: f(), y: f()), control: c)
        case .cubic:
            let c1 = CGPoint(x: f(), y: f()), c2 = CGPoint(x: f(), y: f())
            p.addCurve(to: CGPoint(x: f(), y: f()), control1: c1, control2: c2)
        case .close: p.closeSubpath()
        case .rect: p.addRect(CGRect(x: f(), y: f(), width: f(), height: f()))
        case .roundedRect:
            let rect = CGRect(x: f(), y: f(), width: f(), height: f())
            let rad = Swift.min(f(), rect.width / 2, rect.height / 2)
            p.addRoundedRect(in: rect, cornerSize: CGSize(width: rad, height: rad))
        case .ellipse: p.addEllipse(in: CGRect(x: f(), y: f(), width: f(), height: f()))
        case .arc:
            let c = CGPoint(x: f(), y: f())
            let rad = f(), start = f(), end = f(), cw = f() != 0
            // Screen coordinates, y down: SwiftUI's clockwise is the screen's
            // counterclockwise.
            p.addArc(center: c, radius: rad, startAngle: .radians(Double(start)), endAngle: .radians(Double(end)), clockwise: !cw)
        default: return p
        }
    }
    return p
}

// The watch's own styles; the package's tests build the host for this Mac
// too, which has them not.
extension View {
    /// A Picker as a wheel the Digital Crown turns.
    @ViewBuilder func asukaWheel() -> some View {
        #if os(watchOS)
        pickerStyle(.wheel)
        #else
        self
        #endif
    }

    /// A TabView's pages, vertical as watchOS 10's apps page.
    @ViewBuilder func asukaPages() -> some View {
        #if os(watchOS)
        tabViewStyle(.verticalPage)
        #else
        self
        #endif
    }
}

extension ToolbarItemPlacement {
    /// A screen's bar button: the watch's top trailing corner.
    static var asukaTrailing: ToolbarItemPlacement {
        #if os(watchOS)
        return .topBarTrailing
        #else
        return .automatic
        #endif
    }
}

/// A list of titles (Segments, Items): u32 count, then each u32-length.
func asukaTitles(_ b: UnsafeRawBufferPointer) -> [String] {
    var r = AsukaReader(b)
    guard r.p + 4 <= r.end else { return [] }
    let n = Int(r.u32())
    var out: [String] = []
    for _ in 0..<n {
        guard r.p + 4 <= r.end else { break }
        let len = Int(r.u32())
        guard r.p + len <= r.end else { break }
        out.append(String(decoding: UnsafeRawBufferPointer(start: r.p, count: len), as: UTF8.self))
        r.p += len
    }
    return out
}
