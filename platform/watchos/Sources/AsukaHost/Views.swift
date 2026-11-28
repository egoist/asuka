// The views: each node drawn by SwiftUI's own view for its kind (Text,
// Button, Toggle, Slider, Stepper, TextField, Picker, ProgressView, Image),
// at the frame the core gave it. A container is a ZStack of its children,
// each one sized (.frame) and moved (.offset) to its frame in the
// container's coordinates: SwiftUI positions nothing itself.

import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A node at its frame.
struct AsukaNodeView: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev
        if node.placed {
            let f = node.frame
            AsukaContent(node: node)
                .frame(width: f.width, height: f.height, alignment: asukaPlacement(node.kind))
                .modifier(AsukaStyle(node: node))
                .offset(x: f.minX, y: f.minY)
        }
    }
}

/// Where a node's view sits in its frame when it's smaller than it. Most
/// views fill their frame (the core measured them) and sit at its top
/// leading corner. A Button is centered: watchOS draws every button style
/// (bordered, prominent, glass, with any label, `buttonSizing` or
/// `controlSize`) at most as wide as the screen less its margins (178pt on
/// a 46mm watch), whatever it's proposed. The host tells the core so as it
/// measures one (Measure.swift's `widest`), and the core narrows the frame
/// of a button it stretched across a wider column to that, centered where
/// it was stretched: the watch's full-width button, with the platform's
/// inset on both sides, as SwiftUI's own frame and VStack place it. A
/// button whose size the core forced on both axes isn't measured, and its
/// frame stays wider: centered in it, it's drawn in the same place.
func asukaPlacement(_ kind: AsukaKind?) -> Alignment {
    kind == .button ? .center : .topLeading
}

/// The children of a container, each at its frame.
struct AsukaChildren: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev // what it reads of the node is the batch's
        ZStack(alignment: .topLeading) {
            ForEach(node.shownChildren) { AsukaNodeView(node: $0) }
        }
    }
}

/// A node's own view, by its kind.
struct AsukaContent: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev // what it reads of the node is the batch's
        switch node.kind {
        case .stack:
            AsukaStackView(node: node)
        case .tabs:
            AsukaTabsView(node: node)
        case .scroll:
            AsukaScrollView(node: node)
        case .list:
            AsukaListView(node: node)
        case .text, .richText, .button, .textField, .toggle, .slider, .stepper, .activity, .progress, .progressRing,
             .symbol, .image, .shape, .segmented, .wheel, .picker, .datePicker, .pageControl, .native:
            AsukaLeaf(node: node)
        case .video, .webView, .canvas, .shader, .colorPicker:
            // What the watch can't show (docs/watch.md): an empty space.
            Color.clear
        default:
            // A View, Row or Column, a cell, an Accessory: its children.
            AsukaChildren(node: node)
        }
    }
}

/// A leaf: what the core measures (Measure.swift draws the same view to
/// measure it, so it's drawn at the size it was measured at).
struct AsukaLeaf: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev // what it reads of the node is the batch's
        let n = node
        let rt = AsukaRuntime.shared
        let tint = asukaColor(n.color)
        switch n.kind {
        case .text:
            // Typeset as it was measured (Typeset.swift): at the scale
            // its frame's width holds it at.
            let scale = AsukaTypeset.scale(n, width: n.frame.width)
            Text(n.text)
                .font(Font(AsukaTypeset.font(n, scale)))
                .kerning(AsukaTypeset.kern(n, scale))
                .foregroundStyle(tint ?? .primary)
                .modifier(AsukaTextLook(node: n, scale: scale))
        case .richText:
            let scale = AsukaTypeset.scale(n, width: n.frame.width)
            richText(n, scale, tint)
                .modifier(AsukaTextLook(node: n, scale: scale))
        case .button:
            AsukaButton(node: n)
        case .textField:
            let text = Binding(get: { n.text }, set: { v in
                n.text = v
                rt.emit(n.id, .textChanged) { AsukaEvents.textChanged(n.id, v) }
            })
            Group {
                if n.secure {
                    SecureField(n.placeholder, text: text)
                } else {
                    TextField(n.placeholder, text: text)
                }
            }
            .onSubmit { rt.emit(n.id, .submit) { AsukaEvents.submit(n.id) } }
            .font(Font(AsukaFonts.font(of: n)))
        case .toggle:
            Toggle("", isOn: Binding(get: { n.on }, set: { v in
                n.on = v
                rt.emit(n.id, .toggled) { AsukaEvents.toggled(n.id, v) }
            }))
            .labelsHidden()
            .tint(tint)
        case .slider:
            Slider(value: Binding(get: { n.value }, set: { v in
                n.value = v
                rt.emit(n.id, .valueChanged) { AsukaEvents.valueChanged(n.id, v) }
            }), in: n.min...Swift.max(n.max, n.min + .ulpOfOne))
            .tint(tint)
        case .stepper:
            Stepper(value: Binding(get: { n.value }, set: { v in
                n.value = v
                rt.emit(n.id, .valueChanged) { AsukaEvents.valueChanged(n.id, v) }
            }), in: n.min...Swift.max(n.max, n.min), step: n.step) {
                Text(n.value.formatted())
            }
        case .activity:
            ProgressView().tint(tint)
        case .progress:
            if n.indeterminate {
                ProgressView().tint(tint)
            } else {
                ProgressView(value: Swift.min(Swift.max(n.value, 0), 1)).tint(tint)
            }
        case .progressRing:
            if n.indeterminate {
                ProgressView().tint(tint)
            } else {
                Gauge(value: Swift.min(Swift.max(n.value, 0), 1)) { EmptyView() }
                    .gaugeStyle(.accessoryCircularCapacity)
                    .tint(tint)
            }
        case .symbol:
            Image(systemName: n.icon)
                .font(Font(AsukaFonts.font(of: n)))
                .foregroundStyle(tint ?? .primary)
        case .image:
            if let img = n.image {
                let mode: ContentMode = AsukaImageMode(rawValue: n.mode) == .fill ? .fill : .fit
                Image(decorative: img, scale: rt.scale)
                    .resizable()
                    .aspectRatio(contentMode: mode)
            } else {
                Color.clear
            }
        case .shape:
            if let p = n.path {
                ZStack {
                    if let fill = asukaColor(n.fill) { p.fill(fill) }
                    if let stroke = asukaColor(n.stroke) { p.stroke(stroke, lineWidth: n.lineWidth) }
                }
            }
        case .segmented, .wheel, .picker:
            // The watch's picker: a wheel the Digital Crown turns.
            Picker(n.title, selection: Binding(get: { n.selected }, set: { v in
                n.selected = v
                rt.emit(n.id, .selected) { AsukaEvents.selected(n.id, Int64(v)) }
            })) {
                ForEach(Array(n.items.enumerated()), id: \.offset) { i, t in Text(t).tag(i) }
            }
            .labelsHidden()
            .asukaWheel()
        case .datePicker:
            DatePicker("", selection: Binding(get: { Date(timeIntervalSince1970: n.value / 1000) }, set: { d in
                let ms = (d.timeIntervalSince1970 * 1000).rounded()
                n.value = ms
                rt.emit(n.id, .valueChanged) { AsukaEvents.valueChanged(n.id, ms) }
            }), displayedComponents: dateComponents(n.dateMode))
            .labelsHidden()
        case .native:
            if let v = n.native?.view { v } else { Color.clear }
        case .pageControl:
            HStack(spacing: 4) {
                ForEach(0..<Swift.max(Int(n.max), 0), id: \.self) { i in
                    Circle().fill(i == n.selected ? Color.primary : Color.secondary.opacity(0.5)).frame(width: 6, height: 6)
                }
            }
        default:
            EmptyView()
        }
    }

    private func richText(_ n: AsukaNode, _ scale: CGFloat, _ tint: Color?) -> Text {
        var t = Text("")
        let kern = AsukaTypeset.kern(n, scale)
        for r in n.runs {
            // Its italic is its font's, as it's measured.
            var piece = Text(r.text).font(Font(AsukaTypeset.sized(r.font(AsukaTypeset.runTraits(n, r)), scale))).kerning(kern)
            if let c = asukaColor(r.color) {
                piece = piece.foregroundColor(c)
            } else if !r.link.isEmpty {
                piece = piece.foregroundColor(.accentColor)
            } else if let tint {
                piece = piece.foregroundColor(tint)
            }
            if r.flags & 4 != 0 { piece = piece.underline() }
            if r.flags & 8 != 0 { piece = piece.strikethrough() }
            t = t + piece
        }
        return t
    }

    private func dateComponents(_ mode: Int32) -> DatePickerComponents {
        switch AsukaDateMode(rawValue: mode) {
        case .time: return .hourAndMinute
        case .dateTime: return [.date, .hourAndMinute]
        default: return .date
        }
    }
}

/// A Button as the watch has them: its default style a bordered capsule
/// (watchOS's own default, an app that sets no variant looks native),
/// prominent when filled, borderless when plain was asked for through a
/// background of its own.
struct AsukaButton: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev // what it reads of the node is the batch's
        let n = node
        let rt = AsukaRuntime.shared
        let tint = asukaColor(n.color)
        let b = Button {
            rt.emit(n.id, .press) { AsukaEvents.press(n.id) }
        } label: {
            if n.icon.isEmpty {
                Text(n.title).font(Font(AsukaFonts.font(of: n)))
            } else if n.title.isEmpty {
                Image(systemName: n.icon).font(Font(AsukaFonts.font(of: n)))
            } else {
                Label(n.title, systemImage: n.icon).font(Font(AsukaFonts.font(of: n)))
            }
        }
        .tint(tint)
        switch AsukaButtonVariant(rawValue: n.variant) {
        case .filled, .glassProminent:
            b.buttonStyle(.borderedProminent)
        case .plain where n.background != 0:
            // A style of the app's own (its background): the label alone.
            b.buttonStyle(.plain)
        default:
            b.buttonStyle(.bordered)
        }
    }
}

/// A Scroll: SwiftUI's ScrollView (the Digital Crown scrolls it), its
/// content at the size the core laid it out at.
struct AsukaScrollView: View {
    let node: AsukaNode

    var body: some View {
        let n = node
        let _ = n.rev
        let rt = AsukaRuntime.shared
        let space = "asuka-scroll-\(n.id)"
        ScrollViewReader { proxy in
            ScrollView(n.horizontal ? .horizontal : .vertical) {
                ZStack(alignment: .topLeading) {
                    Color.clear.frame(width: 0, height: 0).id("top")
                    ForEach(n.shownChildren) { c in
                        AsukaNodeView(node: c)
                            .frame(width: c.frame.maxX, height: c.frame.maxY, alignment: .topLeading)
                    }
                }
                .background(GeometryReader { g in
                    Color.clear.preference(key: AsukaOffsetKey.self, value: g.frame(in: .named(space)).origin)
                })
            }
            .coordinateSpace(name: space)
            .onPreferenceChange(AsukaOffsetKey.self) { o in
                // watchOS 10's offsets (AsukaScrollOffsets has later ones').
                guard !asukaScrollGeometry else { return }
                rt.emit(n.id, .scrolled) { AsukaEvents.scrolled(n.id, x: Double(-o.x), y: Double(-o.y)) }
            }
            .modifier(AsukaScrollOffsets(node: n) { proxy.scrollTo("top", anchor: .topLeading) })
        }
    }
}

/// Whether scroll views report their geometry and go to an offset
/// (watchOS 11).
let asukaScrollGeometry: Bool = {
    if #available(watchOS 11, macOS 15, *) { return true }
    return false
}()

/// A Scroll's or a List's offset: reported as it scrolls (Scrolled), and
/// gone to as ScrollToOffset says, past the end being the end. From
/// watchOS 11 with the scroll view's own geometry and position; watchOS
/// 10's scroll views go to a view, not to an offset, so there it goes to
/// the top only (toTop), and only a Scroll reports its offset (its
/// content's place in it).
struct AsukaScrollOffsets: ViewModifier {
    let node: AsukaNode
    let toTop: () -> Void

    func body(content: Content) -> some View {
        let _ = node.rev // a request comes with a batch
        if #available(watchOS 11, macOS 15, *) {
            content.modifier(AsukaScrollPosition(node: node))
        } else {
            let n = node
            content.onChange(of: n.scrollRequests) {
                n.scrollsDone = n.scrollRequests
                guard n.scrollTarget.x <= 0, n.scrollTarget.y <= 0 else { return }
                if n.scrollAnimated { withAnimation { toTop() } } else { toTop() }
            }
        }
    }
}

@available(watchOS 11, macOS 15, *)
struct AsukaScrollPosition: ViewModifier {
    let node: AsukaNode
    @State private var position = ScrollPosition()

    func body(content: Content) -> some View {
        let n = node
        let _ = n.rev // a request comes with a batch
        let rt = AsukaRuntime.shared
        content
            .scrollPosition($position)
            .onScrollGeometryChange(for: CGPoint.self) { g in
                // From its content's top left: the scroll view starts its
                // content below the navigation bar, inset by as much.
                CGPoint(x: g.contentOffset.x + g.contentInsets.leading, y: g.contentOffset.y + g.contentInsets.top)
            } action: { _, o in
                rt.emit(n.id, .scrolled) { AsukaEvents.scrolled(n.id, x: Double(o.x), y: Double(o.y)) }
            }
            .onChange(of: n.scrollRequests, initial: true) {
                // One asked for as it's made, too: the scroll view goes
                // there once it's laid out. Past the end, it stops at it.
                guard n.scrollRequests != n.scrollsDone else { return }
                n.scrollsDone = n.scrollRequests
                let t = n.scrollTarget
                let go = { if n.horizontal { position.scrollTo(x: Swift.max(t.x, 0)) } else { position.scrollTo(y: Swift.max(t.y, 0)) } }
                if n.scrollAnimated { withAnimation { go() } } else { go() }
            }
    }
}

struct AsukaOffsetKey: PreferenceKey {
    static let defaultValue = CGPoint.zero
    static func reduce(value: inout CGPoint, nextValue: () -> CGPoint) { value = nextValue() }
}

/// The common style props, presses and accessibility.
struct AsukaStyle: ViewModifier {
    let node: AsukaNode

    func body(content: Content) -> some View {
        let _ = node.rev // what it reads of the node is the batch's
        let n = node
        let rt = AsukaRuntime.shared
        let shape = AsukaBoxShape(radii: n.radii)
        let t = n.transform
        content
            .background { if n.background != 0 || n.gradient != nil { AsukaBackground(node: n, shape: shape) } }
            .overlay { if n.hasBorder { AsukaBorder(node: n, shape: shape) } }
            .modifier(AsukaClip(on: n.clip || (n.rounded && n.kind == .image), shape: shape))
            .modifier(AsukaPress(node: n, rt: rt))
            .opacity(n.hidden ? 0 : n.opacity)
            .allowsHitTesting(!n.hidden)
            .disabled(n.disabled)
            .scaleEffect(x: t.sx, y: t.sy)
            .rotationEffect(.degrees(t.deg))
            .offset(x: t.tx, y: t.ty)
            .accessibilityHidden(n.a11yHidden)
            .modifier(AsukaA11y(node: n))
    }
}

/// Style.Clip: its content cut to its shape.
struct AsukaClip<S: Shape>: ViewModifier {
    let on: Bool
    let shape: S

    func body(content: Content) -> some View {
        if on { content.clipShape(shape) } else { content }
    }
}

/// OnPress, OnDoubleTap and OnLongPress on a view that isn't a control.
/// With a double tap, a tap's press waits until a second can't come (the
/// double tap's gesture first); without, it presses at once.
struct AsukaPress: ViewModifier {
    let node: AsukaNode
    let rt: AsukaRuntime

    func body(content: Content) -> some View {
        let _ = node.rev // what it reads of the node is the batch's
        let n = node
        let press = n.kind != .button && n.listens(.press)
        let long = n.listens(.longPress)
        if n.listens(.doubleTap) {
            content
                .contentShape(Rectangle())
                .gesture(SpatialTapGesture(count: 2).onEnded { v in
                    rt.emit(n.id, .doubleTap) { AsukaEvents.doubleTap(n.id, x: Double(v.location.x), y: Double(v.location.y)) }
                })
                .onTapGesture { if press { rt.emit(n.id, .press) { AsukaEvents.press(n.id) } } }
                .onLongPressGesture { if long { rt.emit(n.id, .longPress) { AsukaEvents.longPress(n.id) } } }
                .accessibilityAddTraits(press ? .isButton : [])
        } else if press || long {
            content
                .contentShape(Rectangle())
                .onTapGesture { if press { rt.emit(n.id, .press) { AsukaEvents.press(n.id) } } }
                .onLongPressGesture { if long { rt.emit(n.id, .longPress) { AsukaEvents.longPress(n.id) } } }
                .accessibilityAddTraits(press ? .isButton : [])
        } else {
            content
        }
    }
}

struct AsukaA11y: ViewModifier {
    let node: AsukaNode

    func body(content: Content) -> some View {
        let _ = node.rev // what it reads of the node is the batch's
        if node.a11yLabel.isEmpty && node.a11yHint.isEmpty && node.a11yValue.isEmpty {
            content
        } else {
            content
                .accessibilityElement(children: node.a11yLabel.isEmpty ? .contain : .ignore)
                .accessibilityLabel(node.a11yLabel)
                .accessibilityHint(node.a11yHint)
                .accessibilityValue(node.a11yValue)
        }
    }
}
