// platform/widget on the watch: the complications' entries, as iOS's host
// keeps its widgets' (platform/ios WidgetData.swift). The watch's widget
// extension (the CLI builds platform/ios's Widget for watchOS) reads them
// from the App Group (Info.plist's AsukaAppGroup, which the CLI sets when
// the app has complications).

import Foundation
import ImageIO
#if canImport(WidgetKit)
import WidgetKit
#endif

enum AsukaWidgetData {
    static var group: String? { Bundle.main.object(forInfoDictionaryKey: "AsukaAppGroup") as? String }

    /// "set" (JSON: kind, entries) keeps the kind's entries in the App
    /// Group's defaults and reloads its complications; "image" (a name, a 0
    /// byte and the image's data; the name alone: deleted) keeps an image
    /// for their layouts in the App Group's container, no larger than a
    /// complication draws one.
    static func methods() -> [String: AsukaModuleMethod] {
        [
            "set": { args, reply in
                guard let o = (try? JSONSerialization.jsonObject(with: args)) as? [String: Any],
                      let kind = o["kind"] as? String, let entries = o["entries"] else {
                    reply(.failure(AsukaModuleError("bad args"))); return
                }
                guard let g = group, let defaults = UserDefaults(suiteName: g) else {
                    reply(.failure(AsukaModuleError("no complications: give asuka.toml's widgets a watch size (circular, rectangular, inline, corner)"))); return
                }
                defaults.set(try? JSONSerialization.data(withJSONObject: entries), forKey: "asuka.widget." + kind)
                #if canImport(WidgetKit)
                WidgetCenter.shared.reloadTimelines(ofKind: kind)
                #endif
                reply(.success(Data()))
            },
            "image": { args, reply in
                let zero = args.firstIndex(of: 0) ?? args.endIndex
                let name = String(decoding: args[args.startIndex..<zero], as: UTF8.self)
                guard let g = group, let root = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: g) else {
                    reply(.failure(AsukaModuleError("no complications: give asuka.toml's widgets a watch size (circular, rectangular, inline, corner)"))); return
                }
                let dir = root.appendingPathComponent("AsukaWidgetImages")
                let file = dir.appendingPathComponent(imageFile(name))
                if zero == args.endIndex {
                    try? FileManager.default.removeItem(at: file)
                } else {
                    let data = Data(args[args.index(after: zero)...])
                    guard let small = scaled(data) else { reply(.failure(AsukaModuleError("widget: not an image"))); return }
                    do {
                        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                        try small.write(to: file, options: .atomic)
                    } catch {
                        reply(.failure(error)); return
                    }
                }
                #if canImport(WidgetKit)
                WidgetCenter.shared.reloadAllTimelines()
                #endif
                reply(.success(Data()))
            },
        ]
    }

    /// The file an image's name is kept in (the extension's AsukaLayout.imageFile).
    static func imageFile(_ name: String) -> String {
        String(name.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "_" }) + ".img"
    }

    /// The image no larger than a complication draws one (a watch's
    /// screen's width, in pixels: a widget's extension has little memory),
    /// as PNG; as it is when it's smaller; nil: not an image.
    static func scaled(_ data: Data) -> Data? {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil), CGImageSourceGetCount(src) > 0 else { return nil }
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 416,
        ]
        guard let img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary) else { return nil }
        let out = NSMutableData()
        guard let dst = CGImageDestinationCreateWithData(out, "public.png" as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dst, img, nil)
        guard CGImageDestinationFinalize(dst) else { return nil }
        return out as Data
    }
}
