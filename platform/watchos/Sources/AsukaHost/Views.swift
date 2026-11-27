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
                .frame(width: f.width, height: f.height, alignment: .topLeading)
                .modifier(AsukaStyle(node: node))
                .offset(x: f.minX, y: f.minY)
        }
    }
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
            let (ta, a) = textAlignment(n.textAlign)
            Text(n.text)
                .font(Font(AsukaFonts.font(of: n)))
                .foregroundStyle(tint ?? .primary)
                .multilineTextAlignment(ta)
                .lineLimit(n.lineLimit > 0 ? n.lineLimit : nil)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: a)
        case .richText:
            let (ta, a) = textAlignment(n.textAlign)
            richText(n.runs, tint)
                .multilineTextAlignment(ta)
                .lineLimit(n.lineLimit > 0 ? n.lineLimit : nil)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: a)
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

    private func richText(_ runs: [AsukaRichRun], _ tint: Color?) -> Text {
        var t = Text("")
        for r in runs {
            var piece = Text(r.text).font(Font(r.font))
            if let c = asukaColor(r.color) {
                piece = piece.foregroundColor(c)
            } else if !r.link.isEmpty {
                piece = piece.foregroundColor(.accentColor)
            } else if let tint {
                piece = piece.foregroundColor(tint)
            }
            if r.flags & 1 != 0 { piece = piece.italic() }
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
                rt.emit(n.id, .scrolled) { AsukaEvents.scrolled(n.id, x: Double(-o.x), y: Double(-o.y)) }
            }
            .onChange(of: n.rev) {
                guard let t = n.scrollTarget else { return }
                n.scrollTarget = nil
                // To the top: SwiftUI's scroll views on watchOS 10 go to a
                // view, not to an offset.
                if t.y <= 0 && t.x <= 0 {
                    if n.scrollAnimated { withAnimation { proxy.scrollTo("top", anchor: .topLeading) } } else { proxy.scrollTo("top", anchor: .topLeading) }
                }
            }
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
        let shape = RoundedRectangle(cornerRadius: Swift.min(n.cornerRadius, Swift.min(n.frame.width, n.frame.height) / 2), style: .continuous)
        let t = n.transform
        content
            .background { if let bg = asukaColor(n.background) { shape.fill(bg) } }
            .overlay { if n.borderWidth > 0 { shape.strokeBorder(asukaColor(n.borderColor) ?? Color.secondary, lineWidth: n.borderWidth) } }
            .modifier(AsukaClip(on: n.clip || (n.cornerRadius > 0 && n.kind == .image), shape: shape))
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

/// OnPress and OnLongPress on a view that isn't a control.
struct AsukaPress: ViewModifier {
    let node: AsukaNode
    let rt: AsukaRuntime

    func body(content: Content) -> some View {
        let _ = node.rev // what it reads of the node is the batch's
        let n = node
        let press = n.kind != .button && n.listens(.press)
        let long = n.listens(.longPress)
        if press || long {
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
