// platform/widget's Layout: the app's own layout for a widget, a tree the
// app sends with an entry, drawn here with SwiftUI's views as it says:
// columns and rows as stacks, layers as a ZStack, text, symbols, images
// the app kept in the App Group's container, progress bars, gauges,
// timers, spacers and dividers, with their padding, backgrounds, frames,
// fill and links.
import SwiftUI
import UIKit
import WidgetKit

/// A Layout on the wire (platform/widget's Node).
struct AsukaNode: Decodable {
    var t = "", align = "", valign = "", text = "", style = "", weight = "", color = "", textAlign = ""
    var name = "", mode = "", bg = "", url = ""
    var c: [AsukaNode] = []
    var spacing = 0.0, size = 0.0, value = 0.0, untilMs = 0.0
    var pt = 0.0, pb = 0.0, ps = 0.0, pe = 0.0, radius = 0.0, w = 0.0, h = 0.0, opacity = 1.0
    var lines = 0
    var fill = false

    enum CodingKeys: String, CodingKey {
        case t, align, valign, text, style, weight, color, textAlign, name, mode, bg, url, c, spacing, size, value, untilMs
        case pt, pb, ps, pe, radius, w, h, opacity, lines, fill
    }

    init(from d: Decoder) throws {
        let k = try d.container(keyedBy: CodingKeys.self)
        func s(_ key: CodingKeys) -> String { (try? k.decodeIfPresent(String.self, forKey: key)) ?? "" }
        func f(_ key: CodingKeys, _ d: Double = 0) -> Double { (try? k.decodeIfPresent(Double.self, forKey: key)) ?? d }
        t = s(.t); align = s(.align); valign = s(.valign); text = s(.text); style = s(.style); weight = s(.weight)
        color = s(.color); textAlign = s(.textAlign); name = s(.name); mode = s(.mode); bg = s(.bg); url = s(.url)
        c = (try? k.decodeIfPresent([AsukaNode].self, forKey: .c)) ?? []
        spacing = f(.spacing); size = f(.size); value = f(.value); untilMs = f(.untilMs)
        pt = f(.pt); pb = f(.pb); ps = f(.ps); pe = f(.pe); radius = f(.radius); w = f(.w); h = f(.h); opacity = f(.opacity, 1)
        lines = Int(f(.lines))
        fill = (try? k.decodeIfPresent(Bool.self, forKey: .fill)) ?? false
    }
}

/// The name platform/widget's Size has for a family.
extension WidgetFamily {
    var asukaSize: String {
        switch self {
        #if os(watchOS)
        case .accessoryCorner: return "corner"
        #else
        case .systemSmall: return "small"
        case .systemMedium: return "medium"
        case .systemLarge: return "large"
        case .systemExtraLarge: return "extraLarge"
        #endif
        case .accessoryCircular: return "circular"
        case .accessoryRectangular: return "rectangular"
        case .accessoryInline: return "inline"
        default: return "small"
        }
    }
}

enum AsukaLayout {
    /// The widget's own background: the system's (a watch's is black).
    static var background: Color {
        #if os(watchOS)
        return .black
        #else
        return Color(uiColor: .systemBackground)
        #endif
    }

    /// A color on the wire: "#rrggbb(aa)", or a semantic one's name
    /// (".label"), which follows dark mode; nil: none.
    static func color(_ s: String, accent: Color) -> Color? {
        switch s {
        case "": return nil
        case ".label": return .primary
        case ".secondaryLabel": return .secondary
        #if os(watchOS) // the watch is dark, without UIKit's semantic colors
        case ".tertiaryLabel": return .white.opacity(0.3)
        case ".background", ".groupedBackground": return background
        case ".secondaryBackground", ".secondaryGroupedBackground": return Color(white: 0.13)
        case ".separator": return .white.opacity(0.2)
        #else
        case ".tertiaryLabel": return Color(uiColor: .tertiaryLabel)
        case ".background": return Color(uiColor: .systemBackground)
        case ".secondaryBackground": return Color(uiColor: .secondarySystemBackground)
        case ".groupedBackground": return Color(uiColor: .systemGroupedBackground)
        case ".secondaryGroupedBackground": return Color(uiColor: .secondarySystemGroupedBackground)
        case ".separator": return Color(uiColor: .separator)
        #endif
        case ".accent": return accent
        case ".onAccent": return .white
        case ".clear": return .clear
        case ".red": return .red
        case ".orange": return .orange
        case ".yellow": return .yellow
        case ".green": return .green
        case ".blue": return .blue
        case ".purple": return .purple
        case ".pink": return .pink
        case ".gray": return .gray
        default: return Color(asukaHex: s)
        }
    }

    static func weight(_ s: String) -> Font.Weight? {
        switch s {
        case "ultralight": return .ultraLight
        case "thin": return .thin
        case "light": return .light
        case "regular": return .regular
        case "medium": return .medium
        case "semibold": return .semibold
        case "bold": return .bold
        case "heavy": return .heavy
        case "black": return .black
        default: return nil
        }
    }

    static func textStyle(_ s: String) -> Font.TextStyle {
        switch s {
        case "largeTitle": return .largeTitle
        case "title": return .title
        case "title2": return .title2
        case "title3": return .title3
        case "headline": return .headline
        case "subheadline": return .subheadline
        case "callout": return .callout
        case "footnote": return .footnote
        case "caption": return .caption
        case "caption2": return .caption2
        default: return .body
        }
    }

    /// A node's font: its size (fixed), or its text style (the user's text
    /// size), with its weight.
    static func font(_ n: AsukaNode) -> Font {
        let w = weight(n.weight)
        if n.size > 0 { return .system(size: n.size, weight: w ?? .regular) }
        let f = Font.system(textStyle(n.style))
        return w.map { f.weight($0) } ?? f
    }

    static func horizontal(_ s: String) -> HorizontalAlignment {
        s == "end" ? .trailing : s == "center" ? .center : .leading
    }

    static func vertical(_ s: String) -> VerticalAlignment {
        s == "start" ? .top : s == "end" ? .bottom : .center
    }

    /// Where an image setImage kept is: the App Group's container.
    static func image(_ name: String) -> UIImage? {
        guard let group = Bundle.main.object(forInfoDictionaryKey: "AsukaAppGroup") as? String,
              let dir = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group) else { return nil }
        return UIImage(contentsOfFile: dir.appendingPathComponent("AsukaWidgetImages").appendingPathComponent(imageFile(name)).path)
    }

    /// The file an image's name is kept in (the host's AsukaWidgetData too).
    static func imageFile(_ name: String) -> String {
        String(name.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "_" }) + ".img"
    }
}

/// A node, drawn. axis is its parent's (a column's .vertical, a row's
/// .horizontal; nil: layers or the root), which fill grows along; links
/// works where the family takes Links (not a small widget's).
struct AsukaLayoutView: View {
    let node: AsukaNode
    var axis: Axis?
    var links: Bool
    var accent: Color
    var stretch = false  // its column's align is .stretch

    var body: some View {
        let n = node
        let linked = modifiers(content)
        if links, !n.url.isEmpty, let u = URL(string: n.url) {
            Link(destination: u) { linked }
        } else {
            linked
        }
    }

    func modifiers<V: View>(_ v: V) -> some View {
        let n = node
        return v
            .frame(width: n.w > 0 ? n.w : nil, height: n.h > 0 ? n.h : nil)
            .padding(EdgeInsets(top: n.pt, leading: n.ps, bottom: n.pb, trailing: n.pe))
            .background {
                if let bg = AsukaLayout.color(n.bg, accent: accent) {
                    RoundedRectangle(cornerRadius: n.radius, style: .continuous).fill(bg)
                }
            }
            .modifier(AsukaCorners(radius: n.radius))
            .opacity(n.opacity)
            .frame(maxWidth: n.fill && axis != .vertical || stretch ? .infinity : nil,
                   maxHeight: n.fill && axis != .horizontal ? .infinity : nil,
                   alignment: stretch ? Alignment(horizontal: AsukaLayout.horizontal(n.textAlign), vertical: .center) : .center)
    }

    func child(_ c: AsukaNode, _ axis: Axis?, stretch: Bool = false) -> AsukaLayoutView {
        AsukaLayoutView(node: c, axis: axis, links: links, accent: accent, stretch: stretch)
    }

    var tint: Color? { AsukaLayout.color(node.color, accent: accent) }

    @ViewBuilder var content: some View {
        let n = node
        switch n.t {
        case "column":
            VStack(alignment: n.align == "stretch" ? .leading : AsukaLayout.horizontal(n.align), spacing: n.spacing) {
                ForEach(n.c.indices, id: \.self) { i in child(n.c[i], .vertical, stretch: n.align == "stretch") }
            }
        case "row":
            HStack(alignment: AsukaLayout.vertical(n.align), spacing: n.spacing) {
                ForEach(n.c.indices, id: \.self) { i in child(n.c[i], .horizontal) }
            }
        case "layers":
            ZStack(alignment: Alignment(horizontal: AsukaLayout.horizontal(n.align), vertical: AsukaLayout.vertical(n.valign))) {
                ForEach(n.c.indices, id: \.self) { i in child(n.c[i], nil) }
            }
        case "text":
            Text(n.text)
                .font(AsukaLayout.font(n))
                .foregroundStyle(tint ?? .primary)
                .lineLimit(n.lines > 0 ? n.lines : nil)
                .minimumScaleFactor(n.lines == 1 ? 0.5 : 1)
                .multilineTextAlignment(n.textAlign == "end" ? .trailing : n.textAlign == "center" ? .center : .leading)
        case "symbol":
            Image(systemName: n.name)
                .font(n.size > 0 ? .system(size: n.size) : nil)
                .foregroundStyle(tint ?? .primary)
        case "image":
            if let img = AsukaLayout.image(n.name) {
                switch n.mode {
                case "center": Image(uiImage: img)
                case "stretch": Image(uiImage: img).resizable()
                case "fit": Image(uiImage: img).resizable().aspectRatio(contentMode: .fit)
                default: Image(uiImage: img).resizable().aspectRatio(contentMode: .fill).clipped()
                }
            } else {
                Color.clear
            }
        case "progress":
            ProgressView(value: min(max(n.value, 0), 1)).tint(tint ?? accent)
        case "gauge":
            Gauge(value: min(max(n.value, 0), 1)) {
                EmptyView()
            } currentValueLabel: {
                Text(n.text).minimumScaleFactor(0.5)
            }
            .gaugeStyle(.accessoryCircularCapacity)
            .tint(tint ?? accent)
        case "timer":
            Text(Date(timeIntervalSince1970: n.untilMs / 1000), style: .timer)
                .font(AsukaLayout.font(n))
                .monospacedDigit()
                .foregroundStyle(tint ?? .primary)
        case "spacer":
            Spacer(minLength: n.size)
        case "divider":
            Divider()
        default:
            EmptyView()
        }
    }
}

/// Rounds a node's corners, its content's too (an image's), as Android's
/// outline clips.
struct AsukaCorners: ViewModifier {
    let radius: Double

    func body(content: Content) -> some View {
        if radius > 0 {
            content.clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
        } else {
            content
        }
    }
}

/// An entry's own layout for the family: the root fills the widget, its
/// background the container's (to the widget's edges, removable where
/// the system takes it off).
struct AsukaLayoutRoot: View {
    let node: AsukaNode
    let accent: Color
    @Environment(\.widgetFamily) var family

    var body: some View {
        var n = node
        let bg = AsukaLayout.color(n.bg, accent: accent)
        n.bg = ""
        n.fill = true
        let view = AsukaLayoutView(node: n, axis: nil, links: !["small", "circular", "inline", "corner"].contains(family.asukaSize),
                                   accent: accent)
        return view.modifier(AsukaLayoutBackground(color: bg))
    }
}

struct AsukaLayoutBackground: ViewModifier {
    let color: Color?
    @Environment(\.widgetFamily) var family

    func body(content: Content) -> some View {
        if #available(iOS 17.0, *) {
            content.containerBackground(for: .widget) { color ?? AsukaLayout.background }
        } else if family == .accessoryCircular || family == .accessoryRectangular || family == .accessoryInline {
            content
        } else {
            content.padding().background(color ?? AsukaLayout.background)
        }
    }
}
