// Props into the node store: each SetProp writes a field of its node, which
// its view reads when the batch is published (Runtime.publish).

import Foundation
import ImageIO
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

@MainActor extension AsukaRuntime {
    func setProp(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) {
        if prop < 64 {
            setCommon(n, prop, v)
            return
        }
        switch n.kind {
        case .text:
            if prop == AsukaProp.Text.text { n.text = v.string }
        case .button:
            switch prop {
            case AsukaProp.Button.title: n.title = v.string
            case AsukaProp.Button.variant: n.variant = v.enumValue
            case AsukaProp.Button.icon: n.icon = v.string
            default: break
            }
        case .textField:
            switch prop {
            case AsukaProp.TextField.value: n.text = v.string
            case AsukaProp.TextField.placeholder: n.placeholder = v.string
            case AsukaProp.TextField.secure: n.secure = v.bool
            default: break // keyboards, return keys, autocorrection: the watch's input has none
            }
        case .toggle:
            if prop == AsukaProp.Toggle.on { n.on = v.bool }
        case .slider:
            switch prop {
            case AsukaProp.Slider.value: n.value = v.float
            case AsukaProp.Slider.min: n.min = v.float
            case AsukaProp.Slider.max: n.max = v.float
            default: break
            }
        case .stepper:
            switch prop {
            case AsukaProp.Stepper.value: n.value = v.float
            case AsukaProp.Stepper.min: n.min = v.float
            case AsukaProp.Stepper.max: n.max = v.float
            case AsukaProp.Stepper.step: n.step = v.float > 0 ? v.float : 1
            default: break
            }
        case .progress:
            switch prop {
            case AsukaProp.Progress.value: n.value = v.float
            case AsukaProp.Progress.indeterminate: n.indeterminate = v.bool
            default: break
            }
        case .progressRing:
            switch prop {
            case AsukaProp.ProgressRing.value: n.value = v.float
            case AsukaProp.ProgressRing.indeterminate: n.indeterminate = v.bool
            default: break
            }
        case .list:
            if prop == AsukaProp.List.appearance { n.mode = v.enumValue }
        case .scroll:
            if prop == AsukaProp.Scroll.horizontal { n.horizontal = v.bool }
        case .screen:
            switch prop {
            case AsukaProp.Screen.title: n.title = v.string
            case AsukaProp.Screen.icon: n.icon = v.string
            case AsukaProp.Screen.actions:
                if case .bytes(let b) = v { n.actions = AsukaActions.decode(b) }
            default: break
            }
        case .tabs:
            if prop == AsukaProp.Tabs.selected { n.selected = Int(v.int) }
        case .sheet:
            if prop == AsukaProp.Sheet.fullScreen { n.fullScreen = v.bool }
        case .alert:
            switch prop {
            case AsukaProp.Alert.title: n.title = v.string
            case AsukaProp.Alert.message: n.message = v.string
            case AsukaProp.Alert.style: n.alertStyle = v.enumValue
            case AsukaProp.Alert.buttons:
                if case .bytes(let b) = v { n.actions = AsukaActions.decode(b) }
            default: break
            }
        case .image:
            switch prop {
            case AsukaProp.Image.source:
                if n.source != v.string {
                    n.source = v.string
                    loadImage(n)
                }
            case AsukaProp.Image.mode: n.mode = v.enumValue
            default: break
            }
        case .symbol:
            if prop == AsukaProp.Symbol.name { n.icon = v.string }
        case .segmented:
            switch prop {
            case AsukaProp.Segmented.segments: if case .bytes(let b) = v { n.items = asukaTitles(b) }
            case AsukaProp.Segmented.selected: n.selected = Int(v.int)
            default: break
            }
        case .wheel:
            switch prop {
            case AsukaProp.Wheel.items: if case .bytes(let b) = v { n.items = asukaTitles(b) }
            case AsukaProp.Wheel.selected: n.selected = Int(v.int)
            default: break
            }
        case .picker:
            switch prop {
            case AsukaProp.Picker.items: if case .bytes(let b) = v { n.items = asukaTitles(b) }
            case AsukaProp.Picker.selected: n.selected = Int(v.int)
            case AsukaProp.Picker.label: n.title = v.string
            default: break
            }
        case .datePicker:
            switch prop {
            case AsukaProp.DatePicker.value: n.value = v.float
            case AsukaProp.DatePicker.mode: n.dateMode = v.enumValue
            case AsukaProp.DatePicker.min: n.min = v.float
            case AsukaProp.DatePicker.max: n.max = v.float
            default: break
            }
        case .pageControl:
            switch prop {
            case AsukaProp.PageControl.count: n.max = Double(v.int)
            case AsukaProp.PageControl.page: n.selected = Int(v.int)
            default: break
            }
        case .shape:
            switch prop {
            case AsukaProp.Shape.path: if case .bytes(let b) = v { n.path = asukaPath(b) }
            case AsukaProp.Shape.fill: n.fill = v.color
            case AsukaProp.Shape.stroke: n.stroke = v.color
            case AsukaProp.Shape.lineWidth: n.lineWidth = CGFloat(v.float)
            default: break
            }
        case .native:
            setNativeProp(n, prop, v)
        case .richText:
            // Its runs, styled (fonts, colors, italics, links' color); not
            // its inline components (docs/watch.md).
            if prop == AsukaProp.RichText.content, case .bytes(let b) = v { n.runs = richRuns(b, n) }
        default:
            break
        }
    }

    private func setCommon(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) {
        switch prop {
        case AsukaProp.background: n.background = v.color
        case AsukaProp.cornerRadius: n.cornerRadius = CGFloat(v.float)
        case AsukaProp.borderWidth: n.borderWidth = CGFloat(v.float)
        case AsukaProp.borderColor: n.borderColor = v.color
        case AsukaProp.opacity: n.opacity = v.float
        case AsukaProp.hidden: n.hidden = v.bool
        case AsukaProp.clip: n.clip = v.bool
        case AsukaProp.disabled: n.disabled = v.bool
        case AsukaProp.color: n.color = v.color
        case AsukaProp.textStyle: n.textStyle = v.enumValue
        case AsukaProp.fontSize: n.fontSize = Float(v.float)
        case AsukaProp.fontWeight: n.fontWeight = v.enumValue
        case AsukaProp.textAlign: n.textAlign = v.enumValue
        case AsukaProp.lineLimit: n.lineLimit = Int(v.int)
        case AsukaProp.a11yLabel: n.a11yLabel = v.string
        case AsukaProp.a11yHint: n.a11yHint = v.string
        case AsukaProp.a11yHidden: n.a11yHidden = v.bool
        case AsukaProp.a11yValue: n.a11yValue = v.string
        case AsukaProp.transform:
            guard case .bytes(let b) = v, b.count >= 20 else {
                n.transform = (0, 0, 1, 1, 0)
                return
            }
            var r = AsukaReader(b)
            n.transform = (CGFloat(r.f32()), CGFloat(r.f32()), CGFloat(r.f32()), CGFloat(r.f32()), Double(r.f32()))
        default:
            break // the rest are keyboards', pointers', menus' and desktops': not the watch's
        }
    }

    /// A rich text's paragraphs (RichText.Content): its runs, each with
    /// its style's font, color and flags, paragraphs ending with a newline.
    private func richRuns(_ b: UnsafeRawBufferPointer, _ n: AsukaNode) -> [AsukaRichRun] {
        var r = AsukaReader(b)
        func has(_ k: Int) -> Bool { r.p + k <= r.end }
        struct Style { var font: CTFont; var color: UInt64; var flags: UInt8; var link: String }
        var styles: [Style] = []
        guard has(4) else { return [] }
        for _ in 0..<Int(r.u32()) {
            guard has(23) else { return [] }
            let ts = Int32(r.u8()), size = r.f32(), w = Int32(r.u8()), flags = r.u8()
            let color = r.u64()
            _ = r.u64() // its background: not drawn on the watch
            guard has(4) else { return [] }
            let link = r.string()
            let font = AsukaFonts.font(style: ts != 0 ? ts : n.textStyle, size: size > 0 ? size : n.fontSize, weight: w != 0 ? w : n.fontWeight)
            styles.append(Style(font: font, color: color, flags: flags, link: link))
        }
        var out: [AsukaRichRun] = []
        guard has(4) else { return [] }
        let paragraphs = Int(r.u32())
        for p in 0..<paragraphs {
            guard has(4) else { break }
            for _ in 0..<Int(r.u32()) {
                guard has(8) else { return out }
                let si = Int(r.u32())
                let text = r.string()
                let st = si < styles.count ? styles[si] : Style(font: AsukaFonts.font(of: n), color: 0, flags: 0, link: "")
                if st.flags & 16 != 0 { continue } // an inline component: the watch shows none
                out.append(AsukaRichRun(text: text, font: st.font, color: st.color, flags: st.flags, link: st.link))
            }
            if p + 1 < paragraphs { out.append(AsukaRichRun(text: "\n", font: AsukaFonts.font(of: n), color: 0, flags: 0, link: "")) }
        }
        return out
    }

    /// An image's source: an http(s) URL, downloaded, or a file of the
    /// app's assets. Its natural size goes to the core (Loaded), which
    /// lays it out at it.
    func loadImage(_ n: AsukaNode) {
        n.image = nil
        let src = n.source
        guard !src.isEmpty, let url = AsukaAssets.url(src) else { return }
        let id = n.id
        let done: @MainActor (CGImage?) -> Void = { [weak self] img in
            guard let self, let n = self.nodes[id], n.source == src else { return }
            guard let img else {
                self.send(AsukaEvents.failed(id, "couldn't load \(src)"))
                return
            }
            n.image = img
            n.rev &+= 1
            let s = self.scale
            self.send(AsukaEvents.loaded(id, x: Double(CGFloat(img.width) / s), y: Double(CGFloat(img.height) / s)))
        }
        if url.isFileURL {
            done(decodeImage(try? Data(contentsOf: url)))
            return
        }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            let img = decodeImage(data)
            DispatchQueue.main.async { MainActor.assumeIsolated { done(img) } }
        }.resume()
    }
}

/// An image's bytes decoded (PNG, JPEG, HEIC...).
func decodeImage(_ data: Data?) -> CGImage? {
    guard let data, let src = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
    return CGImageSourceCreateImageAtIndex(src, 0, nil)
}
