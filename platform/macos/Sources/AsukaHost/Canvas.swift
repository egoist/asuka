// Canvas: an AsukaCanvasView whose draw(_:) replays the canvas's display
// list (ops.CanvasOp) with Core Graphics, in the view's appearance
// (semantic colors follow dark mode), its coordinates flipped as the
// core's are.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Canvas's view: it draws its node's display list and takes no clicks.
final class AsukaCanvasView: NSView {
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.reassertShadow(n) }
    }

    override func draw(_ dirty: NSRect) {
        let rt = AsukaRuntime.shared
        guard let n = rt.node(of: self), !n.canvas.isEmpty, let c = NSGraphicsContext.current?.cgContext else { return }
        let b = n.canvas
        if #available(macOS 11.0, *) {
            effectiveAppearance.performAsCurrentDrawingAppearance { rt.replayCanvas(c, b) }
        } else {
            rt.replayCanvas(c, b)
        }
    }
}

@MainActor extension AsukaRuntime {
    func makeCanvas(_ n: AsukaNode) -> NSView { AsukaCanvasView(frame: .zero) }

    /// Replays a display list into c. Cut short, it ends there.
    func replayCanvas(_ c: CGContext, _ b: [UInt8]) {
        var i = 0
        var evenOdd = false // FillRule's, kept with each Save
        var rules: [Bool] = []
        func ok(_ n: Int) -> Bool { i + n <= b.count }
        func f() -> CGFloat {
            guard ok(4) else { i = b.count; return 0 }
            defer { i += 4 }
            return CGFloat(b.f32(i))
        }
        func u32() -> Int {
            guard ok(4) else { i = b.count; return 0 }
            defer { i += 4 }
            return Int(b.u32(i))
        }
        func u8() -> UInt8? {
            guard ok(1) else { i = b.count; return nil }
            defer { i += 1 }
            return b[i]
        }
        func color() -> NSColor? {
            guard ok(8) else { i = b.count; return nil }
            defer { i += 8 }
            return asukaColor(b.u64(i))
        }
        func cg(_ col: NSColor?) -> CGColor { (col ?? .clear).cgColor }
        func path() -> CGPath {
            let n = min(u32(), b.count - i)
            let p = asukaPath(Array(b[i..<i + n]))
            i += n
            return p
        }
        func clip(_ p: CGPath) {
            c.addPath(p)
            c.clip(using: evenOdd ? .evenOdd : .winding)
        }
        func gradient() -> CGGradient? {
            let n = u32()
            var cols: [CGColor] = [], locs: [CGFloat] = []
            for _ in 0..<n where i < b.count {
                cols.append(cg(color()))
                locs.append(f())
            }
            guard !cols.isEmpty, let space = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
            return CGGradient(colorsSpace: space, colors: cols as CFArray, locations: locs)
        }
        func capOf(_ v: UInt8) -> CGLineCap { v == 1 ? .round : v == 2 ? .square : .butt }
        func joinOf(_ v: UInt8) -> CGLineJoin { v == 1 ? .round : v == 2 ? .bevel : .miter }
        /// A stroke's width, cap, join and dashes, set on the context.
        func strokeStyle() -> Bool {
            let w = f()
            guard ok(2) else { return false }
            let cap = b[i], join = b[i + 1]
            i += 2
            let nd = u32()
            let phase = f()
            var dash: [CGFloat] = []
            for _ in 0..<nd where i < b.count { dash.append(f()) }
            c.setLineWidth(w)
            c.setLineCap(capOf(cap))
            c.setLineJoin(joinOf(join))
            if !dash.isEmpty { c.setLineDash(phase: phase, lengths: dash) }
            return true
        }
        while i < b.count {
            let op = AsukaCanvasOp(rawValue: Int32(b[i]))
            i += 1
            switch op {
            case .save:
                c.saveGState()
                rules.append(evenOdd)
            case .restore:
                c.restoreGState()
                if let r = rules.popLast() { evenOdd = r }
            case .translate:
                let x = f(), y = f()
                c.translateBy(x: x, y: y)
            case .scale:
                let x = f(), y = f()
                c.scaleBy(x: x, y: y)
            case .rotate:
                c.rotate(by: f() * .pi / 180) // flipped: clockwise on screen
            case .alpha:
                c.setAlpha(f())
            case .clip:
                clip(path())
            case .fill:
                let col = color()
                let p = path()
                c.setFillColor(cg(col))
                c.addPath(p)
                c.fillPath(using: evenOdd ? .evenOdd : .winding)
            case .transform:
                let a = f(), bb = f(), cc = f(), d = f(), tx = f(), ty = f()
                c.concatenate(CGAffineTransform(a: a, b: bb, c: cc, d: d, tx: tx, ty: ty))
            case .fillRule:
                if let v = u8() { evenOdd = v == 1 }
            case .radialGradient:
                let g = gradient()
                let center = CGPoint(x: f(), y: f()), r = f()
                let focus = CGPoint(x: f(), y: f())
                let p = path()
                if let g {
                    c.saveGState()
                    clip(p)
                    c.drawRadialGradient(g, startCenter: focus, startRadius: 0, endCenter: center, endRadius: r, options: [.drawsAfterEndLocation])
                    c.restoreGState()
                }
            case .strokeGradient:
                c.saveGState()
                guard strokeStyle(), let kind = u8() else {
                    c.restoreGState()
                    return
                }
                let radial = kind == 1
                let g = gradient()
                var s = CGPoint.zero, e = CGPoint.zero
                var r: CGFloat = 0
                if radial {
                    e = CGPoint(x: f(), y: f())
                    r = f()
                    s = CGPoint(x: f(), y: f())
                } else {
                    s = CGPoint(x: f(), y: f())
                    e = CGPoint(x: f(), y: f())
                }
                c.addPath(path())
                c.replacePathWithStrokedPath()
                c.clip()
                if let g {
                    if radial {
                        c.drawRadialGradient(g, startCenter: s, startRadius: 0, endCenter: e, endRadius: r, options: [.drawsAfterEndLocation])
                    } else {
                        c.drawLinearGradient(g, start: s, end: e, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
                    }
                }
                c.restoreGState()
            case .stroke:
                let col = color()
                c.saveGState()
                guard strokeStyle() else {
                    c.restoreGState()
                    return
                }
                let p = path()
                c.setStrokeColor(cg(col))
                c.addPath(p)
                c.strokePath()
                c.restoreGState()
            case .gradient:
                let g = gradient()
                let s = CGPoint(x: f(), y: f())
                let e = CGPoint(x: f(), y: f())
                let p = path()
                if let g {
                    c.saveGState()
                    clip(p)
                    c.drawLinearGradient(g, start: s, end: e, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
                    c.restoreGState()
                }
            case .text:
                let col = color()
                let size = f()
                guard ok(2) else { return }
                let weight = Int32(b[i]), align = AsukaTextAlign(rawValue: Int32(b[i + 1]))
                i += 2
                var x = f()
                let y = f()
                let n = min(u32(), b.count - i)
                let s = String(decoding: b[i..<i + n], as: UTF8.self)
                i += n
                let fnt = font(style: 0, weight: max(weight, AsukaFontWeight.regular.rawValue), size: Float(size), family: "", scaled: false)
                let str = NSAttributedString(string: s, attributes: [.font: fnt, .foregroundColor: col ?? NSColor.labelColor])
                let w = str.size().width
                switch align {
                case .center: x -= w / 2
                case .end: x -= w
                default: break
                }
                // In the current NSGraphicsContext: the view's, c, flipped.
                str.draw(at: CGPoint(x: x, y: y - fnt.ascender))
            default:
                return
            }
        }
    }
}
