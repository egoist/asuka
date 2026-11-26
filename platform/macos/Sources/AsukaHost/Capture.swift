import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// A view drawn into an image file (imageedit.capture): the "view"
// module's "capture". args: u32 the native view, f32 x, y, width, height
// of what's drawn in it (a width below 0: all of it; y from its top), u8
// format (0 JPEG, 1 PNG), f32 quality, f32 scale (0: the screen's);
// answers the file's path, in the caches.
@MainActor extension AsukaRuntime {
    func registerCaptureModule() {
        registerModule("view", ["capture": { [weak self] args, reply in
            guard args.count >= 29 else { return reply(.failure(AsukaModuleError("bad arguments"))) }
            let (id, asked, png, quality, scale) = args.withUnsafeBytes { b -> (UInt32, NSRect, Bool, CGFloat, CGFloat) in
                var r = AsukaReader(b)
                let id = r.u32()
                let x = r.f32(), y = r.f32(), w = r.f32(), h = r.f32()
                // A width below 0: all of it (NSRect's width is never below 0).
                let rect = w < 0 || h < 0 ? NSRect.null : NSRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h))
                let png = r.u8() == 1
                return (id, rect, png, CGFloat(r.f32()), CGFloat(r.f32()))
            }
            guard let self, let v = self.nodes[id]?.view else { return reply(.failure(AsukaModuleError("capture: the view isn't showing"))) }
            var rect = asked
            if rect.isNull {
                rect = v.bounds
            } else if !v.isFlipped {
                rect.origin.y = v.bounds.height - rect.maxY
            }
            guard rect.width >= 1, rect.height >= 1 else { return reply(.failure(AsukaModuleError("capture: the view has no size"))) }
            let k = scale > 0 ? scale : (v.window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2)
            guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int((rect.width * k).rounded()), pixelsHigh: Int((rect.height * k).rounded()),
                                             bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                             bytesPerRow: 0, bitsPerPixel: 0) else {
                return reply(.failure(AsukaModuleError("capture: the image couldn't be made")))
            }
            rep.size = rect.size
            if !png, let ctx = NSGraphicsContext(bitmapImageRep: rep) {
                // JPEG has no transparency: what's under the view shows as white.
                NSGraphicsContext.saveGraphicsState()
                NSGraphicsContext.current = ctx
                NSColor.white.setFill()
                NSRect(origin: .zero, size: rect.size).fill()
                NSGraphicsContext.restoreGraphicsState()
            }
            v.cacheDisplay(in: rect, to: rep)
            guard let data = rep.representation(using: png ? .png : .jpeg, properties: png ? [:] : [.compressionFactor: quality]) else {
                return reply(.failure(AsukaModuleError("capture: the image couldn't be written")))
            }
            let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
                .appendingPathComponent(Bundle.main.bundleIdentifier ?? "asuka", isDirectory: true)
                .appendingPathComponent("asuka-capture", isDirectory: true)
            let file = dir.appendingPathComponent(UUID().uuidString + (png ? ".png" : ".jpg"))
            do {
                try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                try data.write(to: file)
            } catch {
                return reply(.failure(error))
            }
            reply(.success(Data(file.path.utf8)))
        }])
    }
}
