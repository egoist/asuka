// Boxes CALayer's own properties can't draw (the visual part's): corners
// of different radii (CornerRadii), borders of different widths per side
// (BorderWidths), dashed and dotted borders (BorderStyle). Shape layers of
// the box's path draw its background (a container's: a leaf's content is
// its layer's own, which a sublayer would cover, so its background stays
// the layer's, masked to the shape), its border, and mask its gradient
// and, while it clips, its children; its shadow is cast by the path.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A box's layers.
@MainActor final class AsukaBox {
    /// The background, in the box's shape (a container's).
    let fill = CAShapeLayer()
    /// The border: one stroked or filled layer, or a dashed or dotted
    /// one's sides of different widths, a layer each.
    var borders: [CAShapeLayer] = []
    /// The gradient's mask, the box's shape.
    let gradientMask = CAShapeLayer()
    /// Masks the view's content to the shape: a leaf's, or a container's
    /// children while it clips.
    var clipMask: CAShapeLayer?

    init() {
        fill.zPosition = -3 // under the gradient (-2), the border (-1) and the children
    }

    func remove() {
        fill.removeFromSuperlayer()
        for b in borders { b.removeFromSuperlayer() }
        borders = []
    }
}

/// A corner's radii, across and down.
typealias AsukaRadius = (x: CGFloat, y: CGFloat)

/// A rect's path with elliptical corners (top left, top right, bottom
/// right, bottom left), y down.
func asukaBoxPath(_ r: CGRect, _ c: [AsukaRadius]) -> CGPath {
    let p = CGMutablePath()
    let k: CGFloat = 0.5522847498 // a quarter ellipse as a cubic
    let x0 = r.minX, y0 = r.minY, x1 = r.maxX, y1 = r.maxY
    let tl = c[0], tr = c[1], br = c[2], bl = c[3]
    func on(_ q: AsukaRadius) -> Bool { q.x > 0 && q.y > 0 }
    p.move(to: CGPoint(x: x0 + (on(tl) ? tl.x : 0), y: y0))
    p.addLine(to: CGPoint(x: x1 - (on(tr) ? tr.x : 0), y: y0))
    if on(tr) {
        p.addCurve(to: CGPoint(x: x1, y: y0 + tr.y), control1: CGPoint(x: x1 - tr.x + k * tr.x, y: y0), control2: CGPoint(x: x1, y: y0 + tr.y - k * tr.y))
    }
    p.addLine(to: CGPoint(x: x1, y: y1 - (on(br) ? br.y : 0)))
    if on(br) {
        p.addCurve(to: CGPoint(x: x1 - br.x, y: y1), control1: CGPoint(x: x1, y: y1 - br.y + k * br.y), control2: CGPoint(x: x1 - br.x + k * br.x, y: y1))
    }
    p.addLine(to: CGPoint(x: x0 + (on(bl) ? bl.x : 0), y: y1))
    if on(bl) {
        p.addCurve(to: CGPoint(x: x0, y: y1 - bl.y), control1: CGPoint(x: x0 + bl.x - k * bl.x, y: y1), control2: CGPoint(x: x0, y: y1 - bl.y + k * bl.y))
    }
    p.addLine(to: CGPoint(x: x0, y: y0 + (on(tl) ? tl.y : 0)))
    if on(tl) {
        p.addCurve(to: CGPoint(x: x0 + tl.x, y: y0), control1: CGPoint(x: x0, y: y0 + tl.y - k * tl.y), control2: CGPoint(x: x0 + tl.x - k * tl.x, y: y0))
    }
    p.closeSubpath()
    return p
}

@MainActor extension AsukaNode {
    /// Its corners' radii as drawn (top left, top right, bottom right,
    /// bottom left): CornerRadii's, else CornerRadius on each, all scaled
    /// down by one factor when two on a side are more than it, as CSS's.
    var radii: [CGFloat] {
        var r = cornerRadii.contains(where: { $0 != 0 }) ? cornerRadii : [cornerRadius, cornerRadius, cornerRadius, cornerRadius]
        r = r.map { max($0, 0) }
        let w = frame.width, h = frame.height
        guard w > 0, h > 0 else { return r }
        var f: CGFloat = 1
        for (a, b, side) in [(0, 1, w), (3, 2, w), (0, 3, h), (1, 2, h)] where r[a] + r[b] > side {
            f = min(f, side / (r[a] + r[b]))
        }
        return f < 1 ? r.map { $0 * f } : r
    }

    /// Its border's widths (top, right, bottom, left).
    var borderSides: [CGFloat] {
        borderWidths.contains(where: { $0 != 0 }) ? borderWidths : [borderWidth, borderWidth, borderWidth, borderWidth]
    }

    /// Its corners aren't all one radius.
    var unevenCorners: Bool { cornerRadii.contains(where: { $0 != 0 }) }

    /// It needs a box's layers: CALayer draws one radius on every corner
    /// and one solid border.
    var needsBox: Bool {
        unevenCorners || borderWidths.contains(where: { $0 != 0 }) || borderStyle != 0 && borderSides.contains(where: { $0 > 0 })
    }

    /// Its content is its layer's own (a text, a control, an image), not
    /// its children's views.
    var leafBox: Bool { !(view is AsukaView) }
}

@MainActor extension AsukaRuntime {
    /// Draws n's box with its layers while CALayer's own properties can't,
    /// and gives it back to them once they can.
    func updateBox(_ n: AsukaNode) {
        guard let v = n.view else { return }
        if !n.needsBox {
            guard let b = n.box else { return }
            b.remove()
            n.box = nil
            if let l = v.layer {
                l.borderWidth = n.borderWidth
                l.mask = nil
                withAppearance(n) {
                    if n.kind != .button { l.backgroundColor = asukaColor(n.background)?.cgColor }
                    l.borderColor = asukaColor(n.borderColor)?.cgColor
                }
            }
            n.clipView?.layer?.mask = nil
            if let g = n.gradientLayer { layoutGradient(n, g) }
            applyCorner(n)
            updateClip(n)
            return
        }
        guard let l = layer(n) else { return }
        let b: AsukaBox
        if let x = n.box { b = x } else {
            b = AsukaBox()
            n.box = b
        }
        // CALayer's own draw none of it but the radius (which still rounds
        // a clip when it's even), and a leaf's background.
        l.borderWidth = 0
        if !n.leafBox { l.backgroundColor = nil }
        layoutBox(n, b)
        if let g = n.gradientLayer { layoutGradient(n, g) }
        updateShadowPath(n)
        updateClip(n)
    }

    /// Shapes the box's layers to n's size and colors.
    func layoutBox(_ n: AsukaNode, _ b: AsukaBox) {
        guard let l = n.view?.layer else { return }
        CATransaction.begin()
        if !animating { CATransaction.setDisableActions(true) }
        defer { CATransaction.commit() }
        let rect = CGRect(origin: .zero, size: n.frame.size)
        let r = n.radii
        let outer = asukaBoxPath(rect, r.map { ($0, $0) })
        b.gradientMask.frame = rect
        b.gradientMask.path = outer
        withAppearance(n) {
            // The background.
            if n.leafBox || n.kind == .button {
                b.fill.removeFromSuperlayer()
            } else {
                if b.fill.superlayer !== l { l.insertSublayer(b.fill, at: 0) }
                b.fill.frame = rect
                b.fill.path = outer
                b.fill.fillColor = asukaColor(n.background)?.cgColor
            }
            layoutBorder(n, b, l, rect, r)
        }
    }

    /// The border: inside the box, each side its width, in BorderColor
    /// (black when it has none, as CALayer's), solid, dashed or dotted.
    func layoutBorder(_ n: AsukaNode, _ b: AsukaBox, _ l: CALayer, _ rect: CGRect, _ r: [CGFloat]) {
        let w = n.borderSides // top, right, bottom, left
        let color = (asukaColor(n.borderColor) ?? .black).cgColor
        let style = AsukaBorderStyle(rawValue: n.borderStyle) ?? .solid
        let even = w[0] == w[1] && w[1] == w[2] && w[2] == w[3]
        var shapes: [(path: CGPath, width: CGFloat, filled: Bool)] = []
        if w.allSatisfy({ $0 <= 0 }) || rect.width <= 0 || rect.height <= 0 {
            // none
        } else if even {
            // One stroke along the middle of the border: outer radii less
            // half the width.
            let h = w[0] / 2
            shapes.append((asukaBoxPath(rect.insetBy(dx: h, dy: h), r.map { (max($0 - h, 0), max($0 - h, 0)) }), w[0], false))
        } else if style == .solid {
            // The ring between the outer shape and the inner one, whose
            // corners are the outer radii less the sides' widths.
            let inner = CGRect(x: w[3], y: w[0], width: max(rect.width - w[1] - w[3], 0), height: max(rect.height - w[0] - w[2], 0))
            let ir: [AsukaRadius] = [(max(r[0] - w[3], 0), max(r[0] - w[0], 0)), (max(r[1] - w[1], 0), max(r[1] - w[0], 0)),
                                     (max(r[2] - w[1], 0), max(r[2] - w[2], 0)), (max(r[3] - w[3], 0), max(r[3] - w[2], 0))]
            let p = CGMutablePath()
            p.addPath(asukaBoxPath(rect, r.map { ($0, $0) }))
            p.addPath(asukaBoxPath(inner, ir))
            shapes.append((p, 0, true))
        } else {
            // A dashed or dotted border of sides of different widths: each
            // side a line along its middle.
            let W = rect.width, H = rect.height
            let dot = style == .dotted
            func line(_ a: CGPoint, _ z: CGPoint) -> CGPath {
                let p = CGMutablePath()
                p.move(to: a)
                p.addLine(to: z)
                return p
            }
            if w[0] > 0 { let e = dot ? w[0] / 2 : 0; shapes.append((line(CGPoint(x: e, y: w[0] / 2), CGPoint(x: W - e, y: w[0] / 2)), w[0], false)) }
            if w[1] > 0 { let e = dot ? w[1] / 2 : 0; shapes.append((line(CGPoint(x: W - w[1] / 2, y: e), CGPoint(x: W - w[1] / 2, y: H - e)), w[1], false)) }
            if w[2] > 0 { let e = dot ? w[2] / 2 : 0; shapes.append((line(CGPoint(x: W - e, y: H - w[2] / 2), CGPoint(x: e, y: H - w[2] / 2)), w[2], false)) }
            if w[3] > 0 { let e = dot ? w[3] / 2 : 0; shapes.append((line(CGPoint(x: w[3] / 2, y: H - e), CGPoint(x: w[3] / 2, y: e)), w[3], false)) }
        }
        while b.borders.count > shapes.count { b.borders.removeLast().removeFromSuperlayer() }
        while b.borders.count < shapes.count {
            let s = CAShapeLayer()
            s.zPosition = -1
            b.borders.append(s)
        }
        for (i, sh) in shapes.enumerated() {
            let s = b.borders[i]
            if s.superlayer !== l { l.addSublayer(s) }
            s.frame = rect
            s.path = sh.path
            if sh.filled {
                s.fillRule = .evenOdd
                s.fillColor = color
                s.strokeColor = nil
                s.lineDashPattern = nil
            } else {
                s.fillColor = nil
                s.strokeColor = color
                s.lineWidth = sh.width
                switch style {
                case .dashed:
                    s.lineCap = .butt
                    s.lineDashPattern = [NSNumber(value: Double(sh.width * 3)), NSNumber(value: Double(sh.width * 3))]
                case .dotted:
                    // Round dots as wide as the border, a width apart.
                    s.lineCap = .round
                    s.lineDashPattern = [0, NSNumber(value: Double(sh.width * 2))]
                default:
                    s.lineCap = .butt
                    s.lineDashPattern = nil
                }
            }
        }
    }

    /// Corners of different radii mask what CALayer's masksToBounds would
    /// round: a leaf's content (always: its background and content are its
    /// layer's), a container's children while it clips.
    func boxClip(_ n: AsukaNode) {
        guard let b = n.box, let l = n.view?.layer else { return }
        let target: CALayer? = n.leafBox ? l : n.clip ? (n.clipView?.layer ?? l) : nil
        let want = n.unevenCorners && target != nil && !(n.leafBox && n.hasShadow)
        if !want {
            if let m = b.clipMask { m.superlayer?.mask = nil; b.clipMask = nil }
            l.mask = nil
            n.clipView?.layer?.mask = nil
            return
        }
        guard let t = target else { return }
        let m = b.clipMask ?? CAShapeLayer()
        b.clipMask = m
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        m.frame = CGRect(origin: .zero, size: n.frame.size)
        m.path = asukaBoxPath(m.frame, n.radii.map { ($0, $0) })
        CATransaction.commit()
        if t.mask !== m { t.mask = m }
    }

    /// n has a new frame: its box's paths follow it.
    func boxPlaced(_ n: AsukaNode) {
        guard let b = n.box else { return }
        layoutBox(n, b)
        if let g = n.gradientLayer { layoutGradient(n, g) }
        boxClip(n)
    }

    /// What the tests read of n's box.
    func boxJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let b = n.box else { return }
        d["box"] = ["radii": n.radii.map { Double($0) }, "borders": b.borders.count, "fill": b.fill.superlayer != nil,
                    "dashed": b.borders.first?.lineDashPattern?.map { $0.doubleValue } ?? [], "mask": b.clipMask != nil]
    }
}
