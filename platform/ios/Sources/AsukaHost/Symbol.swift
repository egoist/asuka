#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A Symbol: an SF Symbol at its node's font, made with the app's text
/// size (as rich text measures) so that what's measured is what's drawn.
final class AsukaSymbolView: UIImageView {
    var name = "" { didSet { if name != oldValue { refresh() } } }
    weak var node: AsukaNode?

    override init(frame: CGRect) {
        super.init(frame: frame)
        contentMode = .center
        tintColor = .label
        isAccessibilityElement = false // decoration, unless the app labels it
        if #available(iOS 17, *) {
            registerForTraitChanges([UITraitPreferredContentSizeCategory.self]) { (v: AsukaSymbolView, _: UITraitCollection) in v.refresh() }
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    func refresh() {
        guard !name.isEmpty else { image = nil; return }
        let traits = UITraitCollection(traitsFrom: [traitCollection, UITraitCollection(preferredContentSizeCategory: asukaContentSize)])
        var font = UIFont.preferredFont(forTextStyle: .body)
        traits.performAsCurrent { font = node?.font ?? .preferredFont(forTextStyle: .body, compatibleWith: traits) }
        image = UIImage(systemName: name, withConfiguration: UIImage.SymbolConfiguration(font: font, scale: .medium))
    }
}
#endif

#if canImport(UIKit)
/// Holds a compact UIDatePicker, which computes NaN frames when UIKit
/// resizes it from a zero size: it's laid out here, only once there's room.
final class AsukaDatePickerView: UIView {
    let picker = UIDatePicker()

    override init(frame: CGRect) {
        super.init(frame: frame)
        autoresizesSubviews = false
        picker.preferredDatePickerStyle = .compact
        picker.datePickerMode = .date
        picker.autoresizingMask = []
        addSubview(picker)
    }
    required init?(coder: NSCoder) { fatalError() }

    /// Its own size: a compact picker asked to fit a size takes all of it.
    override func sizeThatFits(_ size: CGSize) -> CGSize {
        picker.systemLayoutSizeFitting(UIView.layoutFittingCompressedSize)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard bounds.width > 0, bounds.height > 0 else { return }
        // At its own size, at the end (as a trailing control sits).
        let s = sizeThatFits(bounds.size)
        let w = min(s.width, bounds.width)
        let x = effectiveUserInterfaceLayoutDirection == .rightToLeft ? 0 : bounds.width - w
        picker.frame = CGRect(x: x, y: (bounds.height - s.height) / 2, width: w, height: s.height)
    }

    override var tintColor: UIColor! {
        get { super.tintColor }
        set { super.tintColor = newValue; picker.tintColor = newValue }
    }
}
#endif

#if canImport(UIKit)
@MainActor extension AsukaNode {
    /// A background gradient (Style.Gradient): a layer under the subviews.
    func setGradient(_ b: UnsafeRawBufferPointer) {
        var r = AsukaReader(b)
        let n = b.count >= 4 ? Int(r.u32()) : 0
        guard n > 0 else {
            gradient?.removeFromSuperlayer()
            gradient = nil
            return
        }
        var colors: [UIColor?] = []
        var locations: [NSNumber] = []
        for _ in 0..<n {
            colors.append(asukaColor(r.u64()))
            locations.append(NSNumber(value: r.f32()))
        }
        let g = gradient ?? CAGradientLayer()
        if gradient == nil {
            view.layer.insertSublayer(g, at: 0)
            gradient = g
        }
        gradientColors = colors
        g.locations = locations
        g.startPoint = CGPoint(x: CGFloat(r.f32()), y: CGFloat(r.f32()))
        g.endPoint = CGPoint(x: CGFloat(r.f32()), y: CGFloat(r.f32()))
        // Its kind, center, radius and angle (linear, without them).
        gradientKind = 0
        if r.end - r.p >= 17 {
            gradientKind = Int32(r.u8())
            gradientCenter = CGPoint(x: CGFloat(r.f32()), y: CGFloat(r.f32()))
            gradientRadius = CGFloat(r.f32())
            gradientAngle = CGFloat(r.f32())
        }
        switch AsukaGradientKind(rawValue: gradientKind) {
        case .radial: g.type = .radial
        case .conic: g.type = .conic
        default: g.type = .axial
        }
        resolveLayerColors()
        updateLayers()
    }

    /// A radial or conic gradient's points at the view's size: a radial
    /// one's a circle (CAGradientLayer's radial is an ellipse through its
    /// end point's offsets, in unit points), a conic one's from its angle,
    /// clockwise from the top.
    func placeGradient(_ g: CAGradientLayer, _ b: CGRect) {
        guard gradientKind != 0, b.width > 0, b.height > 0 else { return }
        let c = gradientCenter
        g.startPoint = c
        if gradientKind == AsukaGradientKind.radial.rawValue {
            let r = gradientRadius * min(b.width, b.height)
            g.endPoint = CGPoint(x: c.x + r / b.width, y: c.y + r / b.height)
        } else {
            let a = gradientAngle * .pi / 180, k = min(b.width, b.height) / 2
            g.endPoint = CGPoint(x: c.x + sin(a) * k / b.width, y: c.y - cos(a) * k / b.height)
        }
    }

    /// Resolves the layers' dynamic colors with the view's traits (dark mode).
    func resolveLayerColors() {
        let t = view.traitCollection
        if borderColor != 0 { view.layer.borderColor = asukaColor(borderColor)?.resolvedColor(with: t).cgColor }
        gradient?.colors = gradientColors.map { ($0 ?? .clear).resolvedColor(with: t).cgColor }
        if let c = shadowColor { view.layer.shadowColor = c.resolvedColor(with: t).cgColor }
        updateOutlineColors()
    }

    /// Clips the children to the view's bounds, rounded, while Style.Clip
    /// is set. A layer that clips to its bounds clips its own shadow too,
    /// so a view that casts one clips its children in a view of their own
    /// that fills it, and keeps its shadow (SwiftUI's
    /// .clipShape().shadow(), CSS's overflow: hidden with a box-shadow).
    func updateClip() {
        let split = clips && shadowColor != nil && kind == .view && cell == nil
        if split, clipView == nil {
            let c = UIView(frame: view.bounds)
            c.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            c.clipsToBounds = true
            c.layer.cornerCurve = view.layer.cornerCurve
            // Above its material and gradient, its children in it in order.
            view.addSubview(c)
            for child in children where child.view.superview === view { c.addSubview(child.view) }
            if let o = menuOverlay { view.bringSubviewToFront(o) }
            clipView = c
        } else if !split, let c = clipView {
            for child in children where child.view.superview === c { view.insertSubview(child.view, belowSubview: c) }
            c.removeFromSuperview()
            clipView = nil
        }
        view.clipsToBounds = clips && clipView == nil
        clipView?.layer.cornerRadius = view.layer.cornerRadius
        clipView?.layer.maskedCorners = view.layer.maskedCorners
        if outline != nil { updateOutline() } // its children's mask
    }

    /// Fits the shadow's path and the gradient to the view's bounds.
    /// The corner radius drawn: no more than half the shorter side.
    var clampedRadius: CGFloat {
        let b = view.bounds
        return b.width > 0 && b.height > 0 ? min(cornerRadius, min(b.width, b.height) / 2) : cornerRadius
    }

    func updateLayers() {
        let b = view.bounds
        // A new size may change how far a large radius is clamped.
        if cornerRadius > 0 || cornerRadii != nil, view.layer.cornerRadius != layerRadius { view.layer.cornerRadius = layerRadius }
        let corners = view.layer.maskedCorners
        if let g = gradient {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            g.frame = b
            g.cornerRadius = view.layer.cornerRadius
            g.cornerCurve = view.layer.cornerCurve
            g.maskedCorners = corners
            placeGradient(g, b)
            CATransaction.commit()
        }
        if let m = material {
            m.layer.cornerRadius = view.layer.cornerRadius
            m.layer.maskedCorners = corners
            m.superview?.sendSubviewToBack(m)
        }
        clipView?.layer.cornerRadius = view.layer.cornerRadius
        clipView?.layer.maskedCorners = corners
        updateOutline()
        if view.layer.shadowOpacity > 0 {
            view.layer.shadowPath = shapePath
        }
        fitLabel()
    }
}

@MainActor extension AsukaRuntime {
    /// Layer colors (borders, shadows, gradients) don't follow dark mode
    /// as views' do: they're resolved again when it changes.
    func resolveLayerColors() {
        for n in nodes.values { n.resolveLayerColors() }
    }
}

@MainActor extension AsukaNode {
    /// Style.Material: a system material's blur, under the subviews.
    func setMaterial(_ m: AsukaMaterial) {
        let style: UIBlurEffect.Style?
        switch m {
        #if os(tvOS)
        // tvOS's blurs, which follow light and dark: regular, and a
        // denser prominent.
        case .ultraThin, .thin, .regular: style = .regular
        case .thick, .bar: style = .prominent
        #else
        case .ultraThin: style = .systemUltraThinMaterial
        case .thin: style = .systemThinMaterial
        case .regular: style = .systemMaterial
        case .thick: style = .systemThickMaterial
        case .bar: style = .systemChromeMaterial
        #endif
        default: style = nil
        }
        guard let style else {
            material?.removeFromSuperview()
            material = nil
            return
        }
        let v = material ?? UIVisualEffectView()
        v.effect = UIBlurEffect(style: style)
        if material == nil {
            v.frame = view.bounds
            v.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            v.isUserInteractionEnabled = false
            v.clipsToBounds = true
            view.insertSubview(v, at: 0)
            material = v
        }
        v.layer.cornerRadius = view.layer.cornerRadius
        v.layer.cornerCurve = .continuous
    }
}
#endif
