#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// Corner radii, as CSS scales them: when two on a side add up to more
/// than it, all of them by the one factor that fits them (a radius of
/// 999 on every corner: a capsule). Top left, top right, bottom right,
/// bottom left.
func asukaClampRadii(_ r: [CGFloat], _ size: CGSize) -> [CGFloat] {
    guard size.width > 0, size.height > 0 else { return r }
    var f: CGFloat = 1
    func fit(_ side: CGFloat, _ a: CGFloat, _ b: CGFloat) {
        if a + b > side { f = min(f, side / (a + b)) }
    }
    fit(size.width, r[0], r[1])
    fit(size.width, r[3], r[2])
    fit(size.height, r[0], r[3])
    fit(size.height, r[1], r[2])
    return f < 1 ? r.map { $0 * f } : r
}

/// A rectangle's path with each corner's own radii, horizontal and
/// vertical (elliptical: a border's inner corners), clockwise from the
/// top left. Top left, top right, bottom right, bottom left.
func asukaRoundedPath(_ rect: CGRect, _ rx: [CGFloat], _ ry: [CGFloat]) -> CGPath {
    let p = CGMutablePath()
    let k: CGFloat = 0.5523 // a quarter circle's control points, by its radius
    let (x0, y0, x1, y1) = (rect.minX, rect.minY, rect.maxX, rect.maxY)
    p.move(to: CGPoint(x: x0 + rx[0], y: y0))
    p.addLine(to: CGPoint(x: x1 - rx[1], y: y0))
    if rx[1] > 0 || ry[1] > 0 {
        p.addCurve(to: CGPoint(x: x1, y: y0 + ry[1]), control1: CGPoint(x: x1 - rx[1] * (1 - k), y: y0), control2: CGPoint(x: x1, y: y0 + ry[1] * (1 - k)))
    }
    p.addLine(to: CGPoint(x: x1, y: y1 - ry[2]))
    if rx[2] > 0 || ry[2] > 0 {
        p.addCurve(to: CGPoint(x: x1 - rx[2], y: y1), control1: CGPoint(x: x1, y: y1 - ry[2] * (1 - k)), control2: CGPoint(x: x1 - rx[2] * (1 - k), y: y1))
    }
    p.addLine(to: CGPoint(x: x0 + rx[3], y: y1))
    if rx[3] > 0 || ry[3] > 0 {
        p.addCurve(to: CGPoint(x: x0, y: y1 - ry[3]), control1: CGPoint(x: x0 + rx[3] * (1 - k), y: y1), control2: CGPoint(x: x0, y: y1 - ry[3] * (1 - k)))
    }
    p.addLine(to: CGPoint(x: x0, y: y0 + ry[0]))
    if rx[0] > 0 || ry[0] > 0 {
        p.addCurve(to: CGPoint(x: x0 + rx[0], y: y0), control1: CGPoint(x: x0, y: y0 + ry[0] * (1 - k)), control2: CGPoint(x: x0 + rx[0] * (1 - k), y: y0))
    }
    p.closeSubpath()
    return p
}

/// What a view's layer can't draw of its shape and border itself: a
/// fill for corners that differ (its background), the lines of a border
/// per side or dashed or dotted (or around corners that differ), and the
/// masks that hold its gradient, material and children to its shape.
final class AsukaOutline {
    let fill = CAShapeLayer()
    /// The border: one layer for a solid border's sides, filled; or each
    /// side's dashes (or the whole border's, its width the same all round).
    let border = CAShapeLayer()
    var sides: [CAShapeLayer] = []
    let gradientMask = CAShapeLayer()
    let materialMask = CAShapeLayer()
    let clipMask = CAShapeLayer()
    /// Its fill draws the background: the view's corners differ.
    var shaped = false

    init() {
        for l in [fill, border] { l.actions = ["path": NSNull(), "fillColor": NSNull(), "strokeColor": NSNull(), "bounds": NSNull(), "position": NSNull()] }
        // Over the view's children, as a layer's own border is.
        border.zPosition = 1
        border.fillColor = nil
    }

    func remove() {
        fill.removeFromSuperlayer()
        border.removeFromSuperlayer()
    }
}

@MainActor extension AsukaNode {
    /// Its corners' radii as drawn at its size: CornerRadii, or
    /// CornerRadius on all four, scaled as CSS's to fit.
    var drawnRadii: [CGFloat] {
        asukaClampRadii(cornerRadii ?? [cornerRadius, cornerRadius, cornerRadius, cornerRadius], view.bounds.size)
    }

    /// Its radii differ: the layer can't round it, its outline does.
    var cornersDiffer: Bool {
        guard let r = cornerRadii else { return false }
        let on = r.filter { $0 > 0 }
        return on.contains { $0 != on[0] }
    }

    /// Its shape: what its shadow, its outline's fill and masks take.
    var shapePath: CGPath {
        let b = view.bounds
        if cornersDiffer { let r = drawnRadii; return asukaRoundedPath(b, r, r) }
        let r = view.layer.cornerRadius
        if cornerRadii != nil {
            return UIBezierPath(roundedRect: b, byRoundingCorners: rectCorners, cornerRadii: CGSize(width: r, height: r)).cgPath
        }
        return UIBezierPath(roundedRect: b, cornerRadius: r).cgPath
    }

    /// The corners CornerRadii rounds, when the ones it rounds are alike.
    private var rectCorners: UIRectCorner {
        guard let r = cornerRadii else { return .allCorners }
        var c: UIRectCorner = []
        if r[0] > 0 { c.insert(.topLeft) }
        if r[1] > 0 { c.insert(.topRight) }
        if r[2] > 0 { c.insert(.bottomRight) }
        if r[3] > 0 { c.insert(.bottomLeft) }
        return c
    }

    private var maskedCorners: CACornerMask {
        guard let r = cornerRadii else { return [.layerMinXMinYCorner, .layerMaxXMinYCorner, .layerMaxXMaxYCorner, .layerMinXMaxYCorner] }
        var c: CACornerMask = []
        if r[0] > 0 { c.insert(.layerMinXMinYCorner) }
        if r[1] > 0 { c.insert(.layerMaxXMinYCorner) }
        if r[2] > 0 { c.insert(.layerMaxXMaxYCorner) }
        if r[3] > 0 { c.insert(.layerMinXMaxYCorner) }
        return c
    }

    /// The corner radius its layer draws: CornerRadius (clamped), the
    /// radius its rounded corners share, or none while they differ.
    var layerRadius: CGFloat {
        if cornersDiffer { return 0 }
        if cornerRadii != nil { return drawnRadii.max() ?? 0 }
        return clampedRadius
    }

    func setBackground(_ c: UIColor?) {
        background = c
        if let o = outline, o.shaped {
            o.fill.fillColor = c?.resolvedColor(with: view.traitCollection).cgColor
        } else {
            let view = self.view
            AsukaRuntime.shared.animated(view) { view.backgroundColor = c }
        }
    }

    /// Its border is more than its layer draws: widths per side, dashes
    /// or dots, or corners that differ.
    var drawsBorder: Bool {
        borderWidths != nil || (borderWidth > 0 && (borderStyle != 0 || cornersDiffer))
    }

    /// Lays its shape and border out again: its corners (the layer's, or
    /// its outline's), its border, and the masks. Called as a prop of
    /// them changes and as its size does.
    func updateOutline() {
        let layer = view.layer
        let mask = maskedCorners
        if layer.maskedCorners != mask { layer.maskedCorners = mask }
        let shaped = cornersDiffer
        let lines = drawsBorder
        guard shaped || lines else {
            if let o = outline {
                o.remove()
                outline = nil
                if o.shaped {
                    view.backgroundColor = background
                    gradient?.mask = nil
                    material?.layer.mask = nil
                    clipView?.layer.mask = nil
                    layer.mask = nil
                }
                layer.borderWidth = borderWidth
            }
            return
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        let o = outline ?? AsukaOutline()
        outline = o
        let t = view.traitCollection
        let b = view.bounds
        let path = shapePath
        // The background.
        if shaped {
            // Under the gradient (one made after it went in, at 0, too).
            if let g = gradient {
                if o.fill.superlayer !== layer || (layer.sublayers?.firstIndex(of: o.fill) ?? 0) > (layer.sublayers?.firstIndex(of: g) ?? 0) {
                    layer.insertSublayer(o.fill, below: g)
                }
            } else if o.fill.superlayer !== layer {
                layer.insertSublayer(o.fill, at: 0)
            }
            o.shaped = true
            view.backgroundColor = nil
            o.fill.frame = b
            o.fill.path = path
            o.fill.fillColor = background?.resolvedColor(with: t).cgColor
            o.gradientMask.path = path
            gradient?.mask = o.gradientMask
            o.materialMask.path = path
            material?.layer.mask = o.materialMask
            o.clipMask.path = path
            if let c = clipView {
                layer.mask = nil
                c.layer.mask = o.clipMask
            } else {
                layer.mask = clips ? o.clipMask : nil
            }
        } else {
            if o.shaped {
                o.fill.removeFromSuperlayer()
                o.shaped = false
                view.backgroundColor = background
                gradient?.mask = nil
                material?.layer.mask = nil
                clipView?.layer.mask = nil
                layer.mask = nil
            }
        }
        // The border.
        layer.borderWidth = lines ? 0 : borderWidth
        if lines {
            if o.border.superlayer !== layer { layer.addSublayer(o.border) }
            o.border.frame = b
            layoutBorder(o, b)
        } else {
            o.border.removeFromSuperlayer()
        }
        updateOutlineColors()
    }

    /// The border's lines: a solid one's sides filled between its shape
    /// and the shape inside it (each corner's inner radii its outer one
    /// less the widths beside it, as CSS's); a dashed or dotted one
    /// stroked along the middle of its sides, around the corners when it's
    /// the same width all round.
    private func layoutBorder(_ o: AsukaOutline, _ b: CGRect) {
        let w = borderWidths ?? UIEdgeInsets(top: borderWidth, left: borderWidth, bottom: borderWidth, right: borderWidth)
        let r = drawnRadii
        let style = AsukaBorderStyle(rawValue: borderStyle) ?? .solid
        if style == .solid {
            for s in o.sides { s.removeFromSuperlayer() }
            o.sides = []
            let outer = asukaRoundedPath(b, r, r)
            let inner = b.inset(by: w)
            let p = CGMutablePath()
            p.addPath(outer)
            if inner.width > 0, inner.height > 0 {
                let rx = [max(0, r[0] - w.left), max(0, r[1] - w.right), max(0, r[2] - w.right), max(0, r[3] - w.left)]
                let ry = [max(0, r[0] - w.top), max(0, r[1] - w.top), max(0, r[2] - w.bottom), max(0, r[3] - w.bottom)]
                p.addPath(asukaRoundedPath(inner, rx, ry))
            }
            o.border.path = p
            o.border.fillRule = .evenOdd
            o.border.lineWidth = 0
            o.border.lineDashPattern = nil
            return
        }
        o.border.path = nil
        let dash: (CGFloat) -> [NSNumber] = { lw in
            style == .dotted ? [0, NSNumber(value: Double(2 * lw))] : [NSNumber(value: Double(3 * lw)), NSNumber(value: Double(3 * lw))]
        }
        let same = w.top == w.left && w.left == w.bottom && w.bottom == w.right
        var paths: [(CGPath, CGFloat)] = []
        if same {
            let h = w.top / 2
            let rr = r.map { max(0, $0 - h) }
            paths.append((asukaRoundedPath(b.insetBy(dx: h, dy: h), rr, rr), w.top))
        } else {
            func line(_ a: CGPoint, _ z: CGPoint, _ lw: CGFloat) {
                guard lw > 0 else { return }
                let p = CGMutablePath()
                p.move(to: a)
                p.addLine(to: z)
                paths.append((p, lw))
            }
            line(CGPoint(x: b.minX + r[0], y: b.minY + w.top / 2), CGPoint(x: b.maxX - r[1], y: b.minY + w.top / 2), w.top)
            line(CGPoint(x: b.maxX - w.right / 2, y: b.minY + r[1]), CGPoint(x: b.maxX - w.right / 2, y: b.maxY - r[2]), w.right)
            line(CGPoint(x: b.maxX - r[2], y: b.maxY - w.bottom / 2), CGPoint(x: b.minX + r[3], y: b.maxY - w.bottom / 2), w.bottom)
            line(CGPoint(x: b.minX + w.left / 2, y: b.maxY - r[3]), CGPoint(x: b.minX + w.left / 2, y: b.minY + r[0]), w.left)
        }
        while o.sides.count < paths.count {
            let s = CAShapeLayer()
            s.actions = ["path": NSNull(), "strokeColor": NSNull()]
            s.fillColor = nil
            o.border.addSublayer(s)
            o.sides.append(s)
        }
        while o.sides.count > paths.count { o.sides.removeLast().removeFromSuperlayer() }
        for (s, (p, lw)) in zip(o.sides, paths) {
            s.path = p
            s.lineWidth = lw
            s.lineCap = style == .dotted ? .round : .butt
            s.lineDashPattern = dash(lw)
        }
    }

    /// The outline's colors, in the view's appearance (dark mode).
    func updateOutlineColors() {
        guard let o = outline else { return }
        let t = view.traitCollection
        if o.shaped { o.fill.fillColor = background?.resolvedColor(with: t).cgColor }
        let c = (asukaColor(borderColor) ?? .label).resolvedColor(with: t).cgColor
        if o.sides.isEmpty {
            o.border.fillColor = c
        } else {
            o.border.fillColor = nil
            for s in o.sides { s.strokeColor = c }
        }
    }
}

@MainActor extension AsukaRuntime {
    /// The outline's props: CornerRadii, BorderWidths, BorderStyle.
    /// Whether prop is one.
    func setOutline(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) -> Bool {
        switch prop {
        case AsukaProp.cornerRadii:
            guard case .edges(let t, let r, let b, let l) = v else { return true }
            let radii = [CGFloat(t), CGFloat(r), CGFloat(b), CGFloat(l)]
            n.cornerRadii = radii.allSatisfy { $0 == 0 } ? nil : radii
            n.view.layer.cornerCurve = .continuous
            n.view.layer.cornerRadius = n.layerRadius
        case AsukaProp.borderWidths:
            guard case .edges(let t, let r, let b, let l) = v else { return true }
            let w = UIEdgeInsets(top: CGFloat(t), left: CGFloat(l), bottom: CGFloat(b), right: CGFloat(r))
            n.borderWidths = w == .zero ? nil : w
        case AsukaProp.borderStyle:
            n.borderStyle = v.enumValue
        default:
            return false
        }
        n.updateLayers()
        return true
    }
}
#endif
