#if canImport(UIKit) && ASUKA_WIDGET // an app that calls widget.Set (see the CLI's iosFeatures)
import UIKit
import WidgetKit

/// platform/widget's module: "set" (JSON: kind, entries) keeps the kind's
/// entries in the App Group's defaults (AsukaAppGroup), where the widget
/// extension reads them, and reloads its widgets; "image" (a name, a 0
/// byte and the image's data; the name alone: deleted) keeps an image for
/// the widgets' layouts in the App Group's container, no larger than a
/// widget draws one (a widget's extension has little memory).
enum AsukaWidgetData {
    static func methods() -> [String: AsukaModuleMethod] {
        [
            "set": { args, reply in
                guard let o = (try? JSONSerialization.jsonObject(with: args)) as? [String: Any],
                      let kind = o["kind"] as? String, let entries = o["entries"] else {
                    reply(.failure(AsukaModuleError("bad args"))); return
                }
                guard let group = Bundle.main.object(forInfoDictionaryKey: "AsukaAppGroup") as? String,
                      let defaults = UserDefaults(suiteName: group) else {
                    reply(.failure(AsukaModuleError("no App Group: declare the widgets in asuka.json"))); return
                }
                defaults.set(try? JSONSerialization.data(withJSONObject: entries), forKey: "asuka.widget." + kind)
                WidgetCenter.shared.reloadTimelines(ofKind: kind)
                reply(.success(Data()))
            },
            "image": { args, reply in
                let zero = args.firstIndex(of: 0) ?? args.endIndex
                let name = String(decoding: args[args.startIndex..<zero], as: UTF8.self)
                guard let group = Bundle.main.object(forInfoDictionaryKey: "AsukaAppGroup") as? String,
                      let root = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group) else {
                    reply(.failure(AsukaModuleError("no App Group: declare the widgets in asuka.json"))); return
                }
                let dir = root.appendingPathComponent("AsukaWidgetImages")
                let file = dir.appendingPathComponent(imageFile(name))
                if zero == args.endIndex {
                    try? FileManager.default.removeItem(at: file)
                } else {
                    let data = Data(args[args.index(after: zero)...])
                    guard let img = UIImage(data: data) else { reply(.failure(AsukaModuleError("widget: not an image"))); return }
                    do {
                        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                        try (scaled(img, data) ?? data).write(to: file, options: .atomic)
                    } catch {
                        reply(.failure(error)); return
                    }
                }
                WidgetCenter.shared.reloadAllTimelines()
                reply(.success(Data()))
            },
        ]
    }

    /// The file an image's name is kept in (the extension's AsukaLayout.imageFile).
    static func imageFile(_ name: String) -> String {
        String(name.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "_" }) + ".img"
    }

    /// img no larger than a large widget draws (the screen's width, in
    /// pixels), as PNG; nil: it's small enough as it is.
    static func scaled(_ img: UIImage, _ data: Data) -> Data? {
        let px = CGSize(width: img.size.width * img.scale, height: img.size.height * img.scale)
        let most = UIScreen.main.nativeBounds.width
        let k = most / max(px.width, px.height)
        if k >= 1 { return nil }
        let f = UIGraphicsImageRendererFormat()
        f.scale = 1
        let size = CGSize(width: (px.width * k).rounded(), height: (px.height * k).rounded())
        return UIGraphicsImageRenderer(size: size, format: f).pngData { _ in img.draw(in: CGRect(origin: .zero, size: size)) }
    }
}
#endif
