// A view's box as the watch draws it: its corners (CornerRadius, or
// CornerRadii's four, scaled down together as CSS's when a side can't
// hold its two), its background and Gradient (linear, radial or conic),
// and its border (BorderWidth, or BorderWidths' four sides, solid, dashed
// or dotted).

import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Gradient on the wire: its stops, its line (unit points), its kind,
/// and a radial's or conic's center, radius (a fraction of the shorter
/// side) and angle (degrees clockwise from the top).
struct AsukaGradient {
    var stops: [Gradient.Stop] = []
    var start = UnitPoint(x: 0, y: 0), end = UnitPoint(x: 0, y: 1)
    var kind: AsukaGradientKind = .linear
    var center = UnitPoint.center
    var radius: CGFloat = 0.5
    var angle: Double = 0

    init?(_ b: UnsafeRawBufferPointer) {
        var r = AsukaReader(b)
        func has(_ k: Int) -> Bool { r.p + k <= r.end }
        guard has(4) else { return nil }
        let n = Int(r.u32())
        guard n > 0, has(n * 12 + 16) else { return nil }
        for _ in 0..<n {
            let c = r.u64()
            stops.append(Gradient.Stop(color: asukaColor(c) ?? .clear, location: CGFloat(r.f32())))
        }
        start = UnitPoint(x: CGFloat(r.f32()), y: CGFloat(r.f32()))
        end = UnitPoint(x: CGFloat(r.f32()), y: CGFloat(r.f32()))
        // Its kind and the rest, when they're there (linear otherwise).
        guard has(17) else { return }
        kind = AsukaGradientKind(rawValue: Int32(r.u8())) ?? .linear
        center = UnitPoint(x: CGFloat(r.f32()), y: CGFloat(r.f32()))
        radius = CGFloat(r.f32())
        angle = Double(r.f32())
    }
}

/// A box's outline with each corner's radius, physical (top left, top
/// right, bottom right, bottom left): the frames are, so it isn't
/// mirrored right to left. One radius on every corner is SwiftUI's
/// continuous rounded rectangle, as RoundedRectangle draws it.
struct AsukaBoxShape: InsettableShape {
    var radii: [CGFloat]
    var inset: CGFloat = 0

    var layoutDirectionBehavior: LayoutDirectionBehavior { .fixed }

    func path(in rect: CGRect) -> Path {
        let r = rect.insetBy(dx: inset, dy: inset)
        guard r.width > 0, r.height > 0 else { return Path() }
        let c = AsukaBoxShape.fit(radii, r.size).map { Swift.max($0 - inset, 0) }
        if c[0] == c[1] && c[1] == c[2] && c[2] == c[3] {
            return Path(roundedRect: r, cornerRadius: c[0], style: .continuous)
        }
        return Path(roundedRect: r, cornerRadii: RectangleCornerRadii(topLeading: c[0], bottomLeading: c[3], bottomTrailing: c[2], topTrailing: c[1]), style: .continuous)
    }

    func inset(by amount: CGFloat) -> AsukaBoxShape { AsukaBoxShape(radii: radii, inset: inset + amount) }

    /// Radii scaled down together, as CSS's, until each side holds its
    /// two corners.
    static func fit(_ c: [CGFloat], _ s: CGSize) -> [CGFloat] {
        var f: CGFloat = 1
        for (sum, side) in [(c[0] + c[1], s.width), (c[3] + c[2], s.width), (c[0] + c[3], s.height), (c[1] + c[2], s.height)] where sum > side {
            f = Swift.min(f, side / sum)
        }
        return c.map { Swift.max($0, 0) * f }
    }
}

extension AsukaNode {
    /// Its corners' radii: CornerRadii's, or CornerRadius on each.
    var radii: [CGFloat] { cornerRadii ?? [cornerRadius, cornerRadius, cornerRadius, cornerRadius] }
    var rounded: Bool { radii.contains { $0 > 0 } }
    var hasBorder: Bool { borderWidths != nil || borderWidth > 0 }
}

/// A view's background: its color, and its gradient over it.
struct AsukaBackground: View {
    let node: AsukaNode
    let shape: AsukaBoxShape

    var body: some View {
        let _ = node.rev // what it reads of the node is the batch's (its frame too)
        let n = node
        ZStack {
            if let bg = asukaColor(n.background) { shape.fill(bg) }
            if let g = n.gradient {
                let stops = Gradient(stops: g.stops)
                switch g.kind {
                case .radial:
                    shape.fill(RadialGradient(gradient: stops, center: g.center, startRadius: 0,
                                              endRadius: g.radius * Swift.min(n.frame.width, n.frame.height)))
                case .conic:
                    // SwiftUI's angles start at the trailing side, clockwise
                    // on the screen: the top is -90°.
                    shape.fill(AngularGradient(gradient: stops, center: g.center, startAngle: .degrees(g.angle - 90), endAngle: .degrees(g.angle + 270)))
                default:
                    shape.fill(LinearGradient(gradient: stops, startPoint: g.start, endPoint: g.end))
                }
            }
        }
    }
}

/// A view's border, inside its box: one width on every side stroked
/// along its outline, solid, dashed (dashes and gaps three widths long)
/// or dotted (round dots a width wide, a width apart); BorderWidths'
/// sides each their own width, solid as the space between its outline
/// and the inner one (its corners' radii less the sides' widths, as
/// CSS's), dashed or dotted along each side.
struct AsukaBorder: View {
    let node: AsukaNode
    let shape: AsukaBoxShape

    var body: some View {
        let _ = node.rev // what it reads of the node is the batch's
        let n = node
        let color = asukaColor(n.borderColor) ?? Color.secondary
        let style = AsukaBorderStyle(rawValue: n.borderStyle) ?? .solid
        if let w = n.borderWidths {
            if style == .solid {
                AsukaSidesShape(radii: n.radii, widths: w).fill(color, style: FillStyle(eoFill: true))
            } else {
                ForEach(0..<4, id: \.self) { i in
                    if w[i] > 0 {
                        AsukaSideLine(side: i, widths: w).stroke(color, style: AsukaBorder.stroke(w[i], style))
                    }
                }
            }
        } else if n.borderWidth > 0 {
            if style == .solid {
                shape.strokeBorder(color, lineWidth: n.borderWidth)
            } else {
                shape.strokeBorder(color, style: AsukaBorder.stroke(n.borderWidth, style))
            }
        }
    }

    static func stroke(_ w: CGFloat, _ style: AsukaBorderStyle) -> StrokeStyle {
        style == .dotted ? StrokeStyle(lineWidth: w, lineCap: .round, dash: [0, 2 * w])
            : StrokeStyle(lineWidth: w, lineCap: .butt, dash: [3 * w, 3 * w])
    }
}

/// The space between a box's outline and its inner one, each side its
/// own width: filled even-odd, a solid border of BorderWidths.
struct AsukaSidesShape: Shape {
    var radii: [CGFloat]
    var widths: [CGFloat] // top, right, bottom, left

    var layoutDirectionBehavior: LayoutDirectionBehavior { .fixed }

    func path(in rect: CGRect) -> Path {
        let c = AsukaBoxShape.fit(radii, rect.size)
        let t = widths[0], r = widths[1], b = widths[2], l = widths[3]
        var p = Path()
        p.addPath(AsukaSidesShape.box(rect, [(c[0], c[0]), (c[1], c[1]), (c[2], c[2]), (c[3], c[3])]))
        let inner = CGRect(x: rect.minX + l, y: rect.minY + t, width: rect.width - l - r, height: rect.height - t - b)
        if inner.width > 0, inner.height > 0 {
            // Each inner corner an ellipse: its radius less the two sides'
            // widths it's between.
            p.addPath(AsukaSidesShape.box(inner, [
                (Swift.max(c[0] - l, 0), Swift.max(c[0] - t, 0)), (Swift.max(c[1] - r, 0), Swift.max(c[1] - t, 0)),
                (Swift.max(c[2] - r, 0), Swift.max(c[2] - b, 0)), (Swift.max(c[3] - l, 0), Swift.max(c[3] - b, 0)),
            ]))
        }
        return p
    }

    /// A rectangle with elliptical corners (rx, ry each: top left, top
    /// right, bottom right, bottom left), clockwise.
    static func box(_ r: CGRect, _ c: [(CGFloat, CGFloat)]) -> Path {
        let k: CGFloat = 0.5523 // a quarter circle's control points
        var p = Path()
        p.move(to: CGPoint(x: r.minX + c[0].0, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX - c[1].0, y: r.minY))
        p.addCurve(to: CGPoint(x: r.maxX, y: r.minY + c[1].1),
                   control1: CGPoint(x: r.maxX - c[1].0 * (1 - k), y: r.minY), control2: CGPoint(x: r.maxX, y: r.minY + c[1].1 * (1 - k)))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - c[2].1))
        p.addCurve(to: CGPoint(x: r.maxX - c[2].0, y: r.maxY),
                   control1: CGPoint(x: r.maxX, y: r.maxY - c[2].1 * (1 - k)), control2: CGPoint(x: r.maxX - c[2].0 * (1 - k), y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX + c[3].0, y: r.maxY))
        p.addCurve(to: CGPoint(x: r.minX, y: r.maxY - c[3].1),
                   control1: CGPoint(x: r.minX + c[3].0 * (1 - k), y: r.maxY), control2: CGPoint(x: r.minX, y: r.maxY - c[3].1 * (1 - k)))
        p.addLine(to: CGPoint(x: r.minX, y: r.minY + c[0].1))
        p.addCurve(to: CGPoint(x: r.minX + c[0].0, y: r.minY),
                   control1: CGPoint(x: r.minX, y: r.minY + c[0].1 * (1 - k)), control2: CGPoint(x: r.minX + c[0].0 * (1 - k), y: r.minY))
        p.closeSubpath()
        return p
    }
}

/// One side of a box (0 top, 1 right, 2 bottom, 3 left), along the
/// middle of its width, for a dashed or dotted BorderWidths.
struct AsukaSideLine: Shape {
    var side: Int
    var widths: [CGFloat]

    var layoutDirectionBehavior: LayoutDirectionBehavior { .fixed }

    func path(in rect: CGRect) -> Path {
        let h = widths[side] / 2
        var p = Path()
        switch side {
        case 0: p.move(to: CGPoint(x: rect.minX, y: rect.minY + h)); p.addLine(to: CGPoint(x: rect.maxX, y: rect.minY + h))
        case 1: p.move(to: CGPoint(x: rect.maxX - h, y: rect.minY)); p.addLine(to: CGPoint(x: rect.maxX - h, y: rect.maxY))
        case 2: p.move(to: CGPoint(x: rect.maxX, y: rect.maxY - h)); p.addLine(to: CGPoint(x: rect.minX, y: rect.maxY - h))
        default: p.move(to: CGPoint(x: rect.minX + h, y: rect.maxY)); p.addLine(to: CGPoint(x: rect.minX + h, y: rect.minY))
        }
        return p
    }
}
