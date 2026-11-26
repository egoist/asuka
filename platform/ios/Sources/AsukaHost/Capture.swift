#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A view drawn into an image file (imageedit.capture): the "view"
/// module's "capture". args: u32 the native view, f32 x, y, width,
/// height of what's drawn in it (a width below 0: all of it), u8 format
/// (0 JPEG, 1 PNG), f32 quality, f32 scale (0: the screen's); answers the
/// file's path, in the caches.
enum AsukaCapture {
    @MainActor static func methods(_ rt: AsukaRuntime) -> [String: AsukaModuleMethod] {
        ["capture": { [weak rt] args, reply in
            guard args.count >= 29 else { return reply(.failure(AsukaModuleError("bad arguments"))) }
            let (id, rect, png, quality, scale) = args.withUnsafeBytes { b -> (UInt32, CGRect, Bool, CGFloat, CGFloat) in
                var r = AsukaReader(b)
                let id = r.u32()
                let x = r.f32(), y = r.f32(), w = r.f32(), h = r.f32()
                // A width below 0: all of it (CGRect's width is never below 0).
                let rect = w < 0 || h < 0 ? CGRect.null : CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h))
                let png = r.u8() == 1
                return (id, rect, png, CGFloat(r.f32()), CGFloat(r.f32()))
            }
            guard let rt, let v = rt.nodes[id]?.view else { return reply(.failure(AsukaModuleError("capture: the view isn't showing"))) }
            let area = rect.isNull ? v.bounds : rect
            guard area.width >= 1, area.height >= 1 else { return reply(.failure(AsukaModuleError("capture: the view has no size"))) }
            let format = UIGraphicsImageRendererFormat()
            format.scale = scale > 0 ? scale : (v.window?.screen.scale ?? UIScreen.main.scale)
            format.opaque = !png
            let image = UIGraphicsImageRenderer(bounds: area, format: format).image { ctx in
                if !png {
                    // JPEG has no transparency: what's under the view shows as white.
                    UIColor.white.setFill()
                    ctx.fill(area)
                }
                // Its layers as they show (a video's, a map's too), or as
                // drawn when it isn't in a window.
                if v.window == nil || !v.drawHierarchy(in: v.bounds, afterScreenUpdates: true) {
                    v.layer.render(in: ctx.cgContext)
                }
            }
            guard let data = png ? image.pngData() : image.jpegData(compressionQuality: quality) else {
                return reply(.failure(AsukaModuleError("capture: the image couldn't be written")))
            }
            let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("asuka-capture", isDirectory: true)
            let file = dir.appendingPathComponent(UUID().uuidString + (png ? ".png" : ".jpg"))
            do {
                try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                try data.write(to: file)
            } catch {
                return reply(.failure(error))
            }
            reply(.success(Data(file.path.utf8)))
        }]
    }
}
#endif
