// Images and symbols. An Image is an AsukaImageView (an NSImageView,
// layer-backed when it needs to be): the image at its mode's scaling (Fill,
// which NSImageView can't do, is a sublayer's contents, cropped); a tint is
// the color, the image a template. Its placeholder (the core's decoded hash,
// else the Placeholder image) is a sublayer over the view's content, in its
// own mode's gravity (a tint is its color, masked by it); the image's
// arrival fades in with a CATransition while the placeholder's layer fades
// out (Transition; 0.25s, NSAnimationContext's default, over a
// placeholder). An animated file (GIF, APNG, WebP, HEICS) plays through
// ImageIO's CGAnimateImageDataWithBlock, which times and loops it as the
// file says, while it isn't Paused.
import AppKit
import CryptoKit
import ImageIO
#if canImport(AsukaOps)
import AsukaOps
#endif
#if canImport(CAsuka)
@_implementationOnly import CAsuka
#endif

/// An Image node's: its source and what it shows until and as it arrives.
@MainActor final class AsukaImageState {
    var source = ""
    /// The source has loaded (and shows).
    var loaded = false
    /// Placeholder's source.
    var placeholder = ""
    /// Mode's ImageMode.
    var mode: Int32 = 0
    /// PlaceholderImage's image (the core's decoded hash).
    var hash: NSImage?
    /// The placeholder showing (the hash or the Placeholder image).
    var shown: NSImage?
    /// shown is the Placeholder image, in the image's color.
    var tinted = false
    /// PlaceholderMode; -1: the image's.
    var placeholderMode: Int32 = -1
    /// Transition, in seconds.
    var transition: Double = 0
    var paused = false
    /// Headers sent with the source's request; Cache (AsukaImageCache's).
    var headers: [String: String] = [:]
    var cache: Int32 = 0
    /// The placeholder's sublayer.
    var layer: CALayer?
    /// A filled image's sublayer, under the placeholder's: not the view's
    /// own layer, whose contents NSImageView sets itself as it redraws
    /// (a frame arriving after the load cleared the image).
    var fill: CALayer?
    /// The image shown, and an animated one's file.
    var image: NSImage?
    var anim: Data?
    /// The image's and the placeholder's SVG documents, when they're SVG:
    /// a layer showing one (filled, a placeholder) is drawn at its size.
    var svg: AsukaSVG?
    var placeholderSVG: AsukaSVG?
    /// The frame the animation is at, and its playback's generation: a
    /// newer one stops the older.
    var frame = 0
    var gen = 0
    /// Frames shown since it loaded (the automation's: it plays).
    var framesShown = 0

    /// Stops the animation (it stays at its frame).
    func stopFrames() { gen += 1 }
}

/// A decoded image (one point per pixel), and an animated one's file, or
/// an SVG document's.
struct AsukaCachedImage {
    let image: NSImage
    let anim: Data?
    var svg: AsukaSVG? = nil
}

/// An SVG document the core read (an Image whose source is SVG: read the
/// same on every platform, core/src/ui/svg.c). Its image draws the core's
/// display list with Core Graphics at whatever size it's drawn, so an
/// NSImageView scales vectors, not pixels.
final class AsukaSVG: @unchecked Sendable {
    let doc: [UInt8]
    let size: CGSize
    /// Drawn at the size the view draws it, in the appearance it draws in.
    private(set) var image = NSImage()

    /// The document data holds, if it's SVG (any thread).
    init?(_ data: Data) {
        #if canImport(CAsuka)
        var out: UnsafeMutablePointer<UInt8>? = nil
        let n = data.withUnsafeBytes { b in asuka_svg_parse(b.bindMemory(to: UInt8.self).baseAddress, b.count, &out) }
        guard n > 0, let out else { return nil }
        doc = Array(UnsafeBufferPointer(start: out, count: n))
        asuka_free(out)
        var w: Float = 0, h: Float = 0
        _ = asuka_svg_size(doc, doc.count, &w, &h)
        size = CGSize(width: CGFloat(w), height: CGFloat(h))
        #else
        return nil
        #endif
        let img = NSImage(size: size, flipped: true) { [weak self] rect in
            guard let self, let c = NSGraphicsContext.current?.cgContext, Thread.isMainThread else { return false }
            let cmds = self.commands(rect.size, mode: AsukaImageMode.stretch.rawValue)
            c.saveGState()
            c.translateBy(x: rect.minX, y: rect.minY)
            MainActor.assumeIsolated { AsukaRuntime.shared.replayCanvas(c, cmds) }
            c.restoreGState()
            return true
        }
        img.cacheMode = .never
        image = img
    }

    /// Its display list in a box of size, fitted by mode (an ImageMode).
    func commands(_ size: CGSize, mode: Int32) -> [UInt8] {
        #if canImport(CAsuka)
        var out: UnsafeMutablePointer<UInt8>? = nil
        let n = asuka_svg_draw(doc, doc.count, Float(size.width), Float(size.height), mode, 0, 0, &out)
        guard n > 0, let out else { return [] }
        defer { asuka_free(out) }
        return Array(UnsafeBufferPointer(start: out, count: n))
        #else
        return []
        #endif
    }

    /// Drawn in a box of size points by mode, as a layer's contents
    /// (a filled image's, a placeholder's), at scale pixels a point.
    @MainActor func contents(_ size: CGSize, mode: Int32, scale: CGFloat, _ n: AsukaNode) -> CGImage? {
        let w = Int((size.width * scale).rounded(.up)), h = Int((size.height * scale).rounded(.up))
        guard w > 0, h > 0, let space = CGColorSpace(name: CGColorSpace.sRGB),
              let c = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        c.translateBy(x: 0, y: CGFloat(h))
        c.scaleBy(x: scale, y: -scale)
        let cmds = commands(size, mode: mode)
        let ns = NSGraphicsContext(cgContext: c, flipped: true)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = ns
        AsukaRuntime.shared.withAppearance(n) { AsukaRuntime.shared.replayCanvas(c, cmds) }
        NSGraphicsContext.restoreGraphicsState()
        return c.makeImage()
    }
}

/// Decoded images in memory, the least recently used going first. On the
/// main thread.
@MainActor final class AsukaImageMemory {
    static let shared = AsukaImageMemory()
    static let size = 128
    private var byKey: [String: AsukaCachedImage] = [:]
    private var order: [String] = [] // most recent last

    func get(_ key: String) -> AsukaCachedImage? {
        guard let c = byKey[key] else { return nil }
        touch(key)
        return c
    }

    /// Keeps c for key, and returns what the cache has for key: c, or the
    /// image an earlier load put there (two loads of one source may race:
    /// the cache's is the one).
    func put(_ key: String, _ c: AsukaCachedImage) -> AsukaCachedImage {
        if let old = byKey[key] {
            touch(key)
            return old
        }
        byKey[key] = c
        order.append(key)
        while order.count > Self.size {
            byKey.removeValue(forKey: order.removeFirst())
        }
        return c
    }

    private func touch(_ key: String) {
        if let i = order.lastIndex(of: key) { order.remove(at: i) }
        order.append(key)
    }

    /// Forgets every image (Images showing keep theirs).
    func removeAll() {
        byKey.removeAll()
        order.removeAll()
    }
}

/// The loads under way that keep what they load in memory, by source and
/// whether the disk takes it too: another Image of the source, or a
/// prefetch, waits for the one under way rather than loading it again. On
/// the main thread.
@MainActor final class AsukaImageLoads {
    static let shared = AsukaImageLoads()
    private var waiting: [String: [(Result<AsukaCachedImage, Error>) -> Void]] = [:]

    /// Loads src into the memory cache (and the disk's, if disk), done
    /// called with what the cache has once it's there.
    func load(_ src: String, headers: [String: String], disk: Bool, assets: URL, _ done: @escaping (Result<AsukaCachedImage, Error>) -> Void) {
        let key = (disk ? "d " : "m ") + src
        if waiting[key] != nil {
            waiting[key]?.append(done)
            return
        }
        waiting[key] = [done]
        AsukaImageLoader.load(src, assets: assets, headers: headers, disk: disk) { r in
            MainActor.assumeIsolated {
                var r = r
                if case .success(let c) = r { r = .success(AsukaImageMemory.shared.put(src, c)) }
                let all = AsukaImageLoads.shared.waiting.removeValue(forKey: key) ?? []
                for f in all { f(r) }
            }
        }
    }
}

/// Reads and decodes images off the main thread.
enum AsukaImageLoader {
    static let queue = DispatchQueue(label: "asuka.images", qos: .userInitiated, attributes: .concurrent)
    /// The session of the loads the disk mustn't keep (Cache Memory and
    /// None): no URL cache.
    static let ephemeral = URLSession(configuration: .ephemeral)

    /// Loads src (an http(s) URL, a file:// URL, a path, or an asset's
    /// path), decoded; done is called on the main thread. headers go with
    /// an http(s) request; disk: the disk cache is read first and keeps
    /// what's fetched (unless the response says no-store).
    static func load(_ src: String, assets: URL, headers: [String: String] = [:], disk useDisk: Bool = true,
                     _ done: @escaping @Sendable (Result<AsukaCachedImage, Error>) -> Void) {
        let finish: @Sendable (Result<Data, Error>) -> Void = { r in
            queue.async {
                let out = r.flatMap { decode($0, src) }
                DispatchQueue.main.async { done(out) }
            }
        }
        if src.hasPrefix("http://") || src.hasPrefix("https://") {
            guard let url = URL(string: src) else {
                finish(.failure(AsukaModuleError("can't decode \(src)")))
                return
            }
            // Kept across launches, as iOS's are (AsukaDiskCache).
            let disk = AsukaDiskCache.images
            var req = URLRequest(url: url)
            for (k, v) in headers { req.setValue(v, forHTTPHeaderField: k) }
            if !useDisk { req.cachePolicy = .reloadIgnoringLocalCacheData }
            let session = useDisk ? URLSession.shared : ephemeral
            queue.async {
                if useDisk, let d = disk.read(src), case .success = decode(d, src) {
                    finish(.success(d))
                    return
                }
                session.dataTask(with: req) { data, resp, err in
                    if let err {
                        finish(.failure(err))
                        return
                    }
                    if let h = resp as? HTTPURLResponse, h.statusCode / 100 != 2 {
                        let status = "\(h.statusCode) " + HTTPURLResponse.localizedString(forStatusCode: h.statusCode)
                        finish(.failure(AsukaModuleError("\(src): \(status)")))
                        return
                    }
                    let d = data ?? Data()
                    if useDisk, let h = resp as? HTTPURLResponse, !d.isEmpty, AsukaDiskCache.storable(url, h) { disk.write(src, d) }
                    finish(.success(d))
                }.resume()
            }
            return
        }
        queue.async {
            let path: String
            if src.hasPrefix("file://") {
                path = String(src.dropFirst("file://".count))
            } else if src.hasPrefix("/") {
                path = src
            } else {
                var rel = src
                while rel.hasPrefix("/") { rel.removeFirst() }
                path = assets.appendingPathComponent(rel).path
            }
            do {
                finish(.success(try Data(contentsOf: URL(fileURLWithPath: path))))
            } catch {
                finish(.failure(error))
            }
        }
    }

    /// An NSImage sized one point per pixel (PNG, JPEG, GIF, WebP, HEIC,
    /// AVIF: what NSImage reads), and an animated one's file; SVG the
    /// core's, as every platform reads it (not AppKit's).
    static func decode(_ data: Data, _ src: String) -> Result<AsukaCachedImage, Error> {
        if let svg = AsukaSVG(data) { return .success(AsukaCachedImage(image: svg.image, anim: nil, svg: svg)) }
        guard let img = NSImage(data: data) else { return .failure(AsukaModuleError("can't decode \(src)")) }
        if let rep = NSBitmapImageRep(data: data), rep.pixelsWide > 0, rep.pixelsHigh > 0 {
            img.size = CGSize(width: rep.pixelsWide, height: rep.pixelsHigh)
        }
        return .success(AsukaCachedImage(image: img, anim: frameCount(data) > 1 ? data : nil))
    }

    /// How many frames an image file has (0: ImageIO can't read it).
    static func frameCount(_ data: Data) -> Int {
        guard !data.isEmpty, let s = CGImageSourceCreateWithData(data as CFData, nil) else { return 0 }
        return CGImageSourceGetCount(s)
    }

    /// An image of PlaceholderImage's pixels (u32 width, u32 height, then
    /// RGBA, 8 bits each, not premultiplied, sRGB), one point per pixel.
    static func pixels(_ b: [UInt8]) -> NSImage? {
        guard b.count >= 8 else { return nil }
        let w = Int(b.u32(0)), h = Int(b.u32(4))
        guard w > 0, h > 0, b.count >= 8 + w * h * 4,
              let provider = CGDataProvider(data: Data(b[8..<(8 + w * h * 4)]) as CFData),
              let space = CGColorSpace(name: CGColorSpace.sRGB),
              let cg = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4, space: space,
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
                               decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        else { return nil }
        return NSImage(cgImage: cg, size: CGSize(width: w, height: h))
    }
}

/// A layer's contents gravity for an ImageMode.
func asukaGravity(_ mode: Int32) -> CALayerContentsGravity {
    switch AsukaImageMode(rawValue: mode) {
    case .fill: return .resizeAspectFill
    case .stretch: return .resize
    case .center: return .center
    default: return .resizeAspect
    }
}

@MainActor extension AsukaRuntime {
    // MARK: Views

    func makeImage(_ n: AsukaNode, _ kind: AsukaKind) -> NSView {
        let v = AsukaImageView(frame: .zero)
        v.imageFrameStyle = .none
        if kind == .symbol {
            v.imageScaling = .scaleProportionallyDown
            v.contentTintColor = .labelColor
            return v
        }
        v.imageScaling = .scaleProportionallyUpOrDown
        v.isEditable = false
        v.animates = true
        n.image = AsukaImageState()
        return v
    }

    func setImage(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let x = n.image, let iv = n.view as? NSImageView else { return }
        switch p {
        case AsukaProp.Image.source:
            x.source = v.string
            x.loaded = false
            x.stopFrames()
            x.anim = nil
            x.svg = nil
            x.frame = 0
            x.image = nil
            iv.image = nil
            x.fill?.contents = nil
            x.fill?.mask = nil
            showPlaceholder(n)
            loadImage(n, x.source, placeholder: false)
        case AsukaProp.Image.placeholder:
            x.placeholder = v.string
            x.placeholderSVG = nil
            showPlaceholder(n)
        case AsukaProp.Image.placeholderImage:
            x.hash = AsukaImageLoader.pixels(v.data)
            showPlaceholder(n)
        case AsukaProp.Image.placeholderMode:
            x.placeholderMode = v.enumValue
            if let s = x.shown { setPlaceholderImage(n, s, x.tinted) }
        case AsukaProp.Image.transition:
            x.transition = v.float
        case AsukaProp.Image.headers:
            // As Video's: name, value, ... (sent before Source).
            let s = asukaStrings(v.data)
            x.headers = [:]
            var i = 0
            while i + 1 < s.count {
                x.headers[s[i]] = s[i + 1]
                i += 2
            }
        case AsukaProp.Image.cache:
            x.cache = v.enumValue
        case AsukaProp.Image.paused:
            x.paused = v.bool
            if x.paused { x.stopFrames() } else { playFrames(n) }
        case AsukaProp.Image.mode:
            x.mode = v.enumValue
            showImage(n, x.image)
            if let s = x.shown, x.placeholderMode < 0 { setPlaceholderImage(n, s, x.tinted) }
        default:
            break
        }
    }

    // MARK: Loading

    /// Loads an Image's source (or its placeholder) as its Cache says:
    /// Default from memory, else the disk, else the network, kept in both;
    /// Memory never reading nor writing the disk; None loaded each time.
    /// Its Headers go with the source's request.
    func loadImage(_ n: AsukaNode, _ src: String, placeholder: Bool) {
        guard !src.isEmpty else { return }
        let id = n.id
        let mode = AsukaImageCache(rawValue: placeholder ? 0 : n.image?.cache ?? 0) ?? .default
        let headers = placeholder ? [:] : n.image?.headers ?? [:]
        if mode == .none {
            AsukaImageLoader.load(src, assets: assets, headers: headers, disk: false) { r in
                MainActor.assumeIsolated { AsukaRuntime.shared.imageLoaded(id, src, r, placeholder) }
            }
            return
        }
        if let c = AsukaImageMemory.shared.get(src) {
            imageLoaded(id, src, .success(c), placeholder)
            return
        }
        AsukaImageLoads.shared.load(src, headers: headers, disk: mode == .default, assets: assets) { r in
            AsukaRuntime.shared.imageLoaded(id, src, r, placeholder)
        }
    }

    /// The "image" module: prefetch (u32 n, n u32-length URLs, u32 m, m
    /// u32-length strings: the headers' names and values in turn) loads
    /// each into the caches as an Image would, off the main thread, with
    /// the loads under way; clearCache (u8: 1 the memory cache, 2 the
    /// disk's) forgets them. Images showing keep what they show.
    func registerImageModule() {
        registerModule("image", [
            "prefetch": { [weak self] args, reply in
                guard let self else { return reply(.success(Data())) }
                var r = AsukaByteReader(args)
                var urls: [String] = []
                let n = Int(r.u32())
                while urls.count < n, !r.bad {
                    let u = r.string()
                    if !r.bad { urls.append(u) }
                }
                var headers: [String: String] = [:]
                let m = r.atEnd ? 0 : Int(r.u32())
                var i = 0
                while i + 1 < m, !r.bad {
                    let k = r.string(), v = r.string()
                    if !r.bad { headers[k] = v }
                    i += 2
                }
                for u in urls where !u.isEmpty && AsukaImageMemory.shared.get(u) == nil {
                    AsukaImageLoads.shared.load(u, headers: headers, disk: true, assets: self.assets) { _ in }
                }
                reply(.success(Data()))
            },
            "clearCache": { args, reply in
                let flags = args.first ?? 3
                if flags & 1 != 0 { AsukaImageMemory.shared.removeAll() }
                if flags & 2 != 0 {
                    // The host's files, and what URLSession kept of the
                    // requests (the Default loads' session).
                    DispatchQueue.global(qos: .utility).async { AsukaDiskCache.images.removeAll() }
                    URLCache.shared.removeAllCachedResponses()
                }
                reply(.success(Data()))
            },
        ])
    }

    func imageLoaded(_ id: UInt32, _ src: String, _ r: Result<AsukaCachedImage, Error>, _ placeholder: Bool) {
        guard let n = nodes[id], let x = n.image, let iv = n.view as? NSImageView else { return }
        if placeholder {
            if !x.loaded, x.placeholder == src, x.hash == nil, case .success(let c) = r {
                x.placeholderSVG = c.svg
                setPlaceholderImage(n, c.image, n.color != 0)
            }
            return
        }
        guard x.source == src else { return }
        let c: AsukaCachedImage
        switch r {
        case .failure(let e):
            let msg = asukaErrorText(e)
            emit(n, .failed) { AsukaEvents.failed($0, msg) }
            return
        case .success(let ok):
            c = ok
        }
        x.loaded = true
        x.anim = c.anim
        x.svg = c.svg
        x.frame = 0
        x.framesShown = 0
        // ImageIO plays an animated file (APNG and WebP too), not the view.
        iv.animates = c.anim == nil
        showImage(n, c.image)
        arrive(n)
        let s = c.image.size
        emit(n, .loaded) { AsukaEvents.loaded($0, x: Double(s.width), y: Double(s.height)) }
        if c.anim != nil {
            emit(n, .animated) { AsukaEvents.animated($0) }
            playFrames(n)
        }
    }

    /// Shows img in n's view at its mode: NSImageView can't fill, so a
    /// filled image is a sublayer's contents (x.fill), cropped.
    func showImage(_ n: AsukaNode, _ img: NSImage?) {
        guard let x = n.image, let iv = n.view as? NSImageView else { return }
        x.image = img
        switch AsukaImageMode(rawValue: x.mode) {
        case .fill:
            iv.imageScaling = .scaleProportionallyUpOrDown
            iv.image = nil
            guard let parent = layer(n) else { return }
            CATransaction.begin()
            if !animating { CATransaction.setDisableActions(true) }
            let l: CALayer
            if let f = x.fill {
                l = f
            } else {
                l = CALayer()
                l.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
                parent.insertSublayer(l, at: 0)
                x.fill = l
            }
            l.frame = parent.bounds
            let contents = layerContents(n, img, x.svg, mode: x.mode, size: parent.bounds.size)
            if n.color != 0, contents != nil {
                // A tint: the color through the image's alpha, as a
                // template's (a mask of the image over the color).
                let m = l.mask ?? CALayer()
                m.frame = l.bounds
                m.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
                m.contents = contents
                m.contentsGravity = .resizeAspectFill
                l.mask = m
                l.contents = nil
                withAppearance(n) { l.backgroundColor = asukaColor(n.color)?.cgColor }
            } else {
                l.mask = nil
                l.backgroundColor = nil
                l.contents = contents
            }
            l.contentsGravity = .resizeAspectFill
            l.masksToBounds = true
            parent.masksToBounds = true
            CATransaction.commit()
            return
        case .stretch:
            iv.imageScaling = .scaleAxesIndependently
        case .center:
            iv.imageScaling = .scaleNone
        default:
            iv.imageScaling = .scaleProportionallyUpOrDown
        }
        x.fill?.contents = nil
        x.fill?.mask = nil
        var shown = img
        if let img, n.color != 0, !img.isTemplate {
            // A tint is the color, the image a template: a copy (it shares
            // the image's bitmap), so the cache's image, which other Images
            // of the source show, stays as it is.
            let t = img.copy() as? NSImage ?? img
            t.isTemplate = true
            shown = t
        }
        iv.image = shown
    }

    /// n has a new frame: its placeholder's layer fills it.
    func layoutImage(_ n: AsukaNode) {
        guard let x = n.image else { return }
        // An SVG a layer shows is drawn again at the layer's new size.
        if let f = x.fill, let svg = x.svg, f.contents != nil || f.mask != nil, let parent = f.superlayer {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            f.frame = parent.bounds
            let c = layerContents(n, svg.image, svg, mode: x.mode, size: parent.bounds.size)
            if let m = f.mask { m.frame = f.bounds; m.contents = c } else { f.contents = c }
            CATransaction.commit()
        }
        guard let l = x.layer, let parent = l.superlayer else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        l.frame = parent.bounds
        l.mask?.frame = l.bounds
        if let svg = x.placeholderSVG, let shown = x.shown, shown === svg.image {
            let c = layerContents(n, shown, svg, mode: x.placeholderMode < 0 ? x.mode : x.placeholderMode, size: parent.bounds.size)
            if let m = l.mask { m.contents = c } else { l.contents = c }
        }
        CATransaction.commit()
    }

    /// What a layer shows of img in a box of size: an SVG drawn at that
    /// size by mode (its gravity then changes nothing), else img.
    func layerContents(_ n: AsukaNode, _ img: NSImage?, _ svg: AsukaSVG?, mode: Int32, size: CGSize) -> Any? {
        guard let img, let svg, svg.image === img, size.width > 0, size.height > 0 else { return img }
        let scale = n.view?.window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2
        return svg.contents(size, mode: mode, scale: scale, n) ?? img
    }

    // MARK: Placeholders

    /// Shows img (nil: none) over the image until it arrives; tinted, in
    /// the image's color.
    func setPlaceholderImage(_ n: AsukaNode, _ img: NSImage?, _ tinted: Bool) {
        guard let x = n.image else { return }
        x.shown = img
        x.tinted = tinted
        guard let img else {
            x.layer?.removeFromSuperlayer()
            x.layer = nil
            return
        }
        guard let parent = layer(n) else { return }
        parent.masksToBounds = true
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let l: CALayer
        if let have = x.layer {
            l = have
        } else {
            l = CALayer()
            l.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
            parent.addSublayer(l)
            x.layer = l
        }
        l.removeAllAnimations()
        l.frame = parent.bounds
        l.opacity = 1
        let pm = x.placeholderMode < 0 ? x.mode : x.placeholderMode
        let g = asukaGravity(pm)
        let contents = layerContents(n, img, x.placeholderSVG, mode: pm, size: parent.bounds.size)
        if tinted, n.color != 0 {
            // The color, masked by the image.
            let mask = CALayer()
            mask.frame = l.bounds
            mask.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
            mask.contents = contents
            mask.contentsGravity = g
            l.contents = nil
            l.mask = mask
            withAppearance(n) { l.backgroundColor = asukaColor(n.color)?.cgColor }
        } else {
            l.mask = nil
            l.backgroundColor = nil
            l.contents = contents
            l.contentsGravity = g
        }
        CATransaction.commit()
    }

    /// Fades the image in over its placeholder, or from nothing, for its
    /// Transition; then the placeholder goes.
    func arrive(_ n: AsukaNode) {
        guard let x = n.image else { return }
        let had = x.layer != nil && x.shown != nil
        var d = x.transition
        if d < 0 { d = 0 } else if d == 0 && had { d = 0.25 }
        guard d > 0, n.view?.window != nil, let l = layer(n) else {
            setPlaceholderImage(n, nil, false)
            return
        }
        let t = CATransition()
        t.type = .fade
        t.duration = d
        l.add(t, forKey: "asuka.image")
        guard had, let pl = x.layer else { return }
        CATransaction.begin()
        CATransaction.setAnimationDuration(d)
        pl.opacity = 0
        CATransaction.commit()
        let id = n.id
        DispatchQueue.main.asyncAfter(deadline: .now() + d) {
            MainActor.assumeIsolated {
                let rt = AsukaRuntime.shared
                if let n = rt.nodes[id], let x = n.image, x.layer === pl, x.loaded { rt.setPlaceholderImage(n, nil, false) }
            }
        }
    }

    /// Shows the image's placeholder while it hasn't loaded: the hash's,
    /// else the Placeholder image's.
    func showPlaceholder(_ n: AsukaNode) {
        guard let x = n.image, !x.loaded else { return }
        if let h = x.hash {
            setPlaceholderImage(n, h, false)
            return
        }
        guard !x.placeholder.isEmpty else {
            setPlaceholderImage(n, nil, false)
            return
        }
        loadImage(n, x.placeholder, placeholder: true)
    }

    // MARK: Frames

    /// Plays n's animated image from the frame it's at, if it has one,
    /// has loaded and isn't paused.
    func playFrames(_ n: AsukaNode) {
        guard let x = n.image else { return }
        x.stopFrames()
        guard let data = x.anim, !x.paused, x.loaded else { return }
        guard #available(macOS 10.15, *) else { return }
        let gen = x.gen, id = n.id
        let opts = [kCGImageAnimationStartIndex: x.frame] as CFDictionary
        // ImageIO calls the block on the main queue.
        CGAnimateImageDataWithBlock(data as CFData, opts) { index, cg, stop in
            MainActor.assumeIsolated {
                let rt = AsukaRuntime.shared
                guard let n = rt.nodes[id], let x = n.image, x.gen == gen, n.view != nil else {
                    stop.pointee = true
                    return
                }
                x.frame = index
                x.framesShown += 1
                rt.showImage(n, NSImage(cgImage: cg, size: CGSize(width: cg.width, height: cg.height)))
            }
        }
    }

    /// Lets go of what an Image holds.
    func releaseImage(_ n: AsukaNode) {
        guard let x = n.image else { return }
        x.stopFrames()
        x.hash = nil
        x.layer = nil
        x.shown = nil
    }

    // MARK: Symbols

    /// An SF Symbol's image (nil: none of that name).
    func symbolImage(_ name: String) -> NSImage? {
        guard !name.isEmpty, #available(macOS 11.0, *) else { return nil }
        return NSImage(systemSymbolName: name, accessibilityDescription: nil)
    }

    /// Shows a Symbol at its node's font: the text style's size and the
    /// weight.
    func applySymbol(_ n: AsukaNode) {
        guard let iv = n.view as? NSImageView else { return }
        guard #available(macOS 11.0, *), let img = symbolImage(n.symbol) else {
            iv.image = nil
            return
        }
        let f = font(n)
        var w: CGFloat = 0
        if n.fontWeight > 0 && Int(n.fontWeight) < asukaWeights.count { w = asukaWeights[Int(n.fontWeight)] }
        let conf = NSImage.SymbolConfiguration(pointSize: f.pointSize, weight: NSFont.Weight(w), scale: .medium)
        iv.image = img.withSymbolConfiguration(conf) ?? img
        if n.color == 0 { iv.contentTintColor = .labelColor }
    }

    /// A Symbol's size: its image's (an Image measures itself in the core,
    /// by the size it loaded at).
    func measureImage(_ n: AsukaNode, _ c: AsukaOffer) -> CGSize {
        (n.view as? NSImageView)?.image?.size ?? .zero
    }

    func imageJSON(_ n: AsukaNode, _ x: AsukaImageState, _ d: inout [String: Any]) {
        var j: [String: Any] = ["source": x.source, "loaded": x.loaded, "paused": x.paused, "frame": x.frame,
                                "framesShown": x.framesShown, "animated": x.anim != nil, "placeholderLayer": x.layer != nil,
                                "placeholderShown": x.shown != nil, "hash": x.hash != nil, "tinted": x.tinted]
        if let img = x.image { j["imageSize"] = [Double(img.size.width), Double(img.size.height)] }
        if let l = x.layer {
            j["placeholderOpacity"] = Double(l.opacity)
            j["placeholderGravity"] = l.mask?.contentsGravity.rawValue ?? l.contentsGravity.rawValue
        }
        if let iv = n.view as? NSImageView {
            j["viewImage"] = iv.image != nil
            j["scaling"] = iv.imageScaling.rawValue
            j["layerContents"] = x.fill?.contents != nil || x.fill?.mask?.contents != nil
            j["fillTinted"] = x.fill?.mask != nil
            // Core Animation keeps a transition under kCATransition, whatever
            // the key it was added with.
            if let a = iv.layer?.animation(forKey: kCATransition) { j["arrival"] = a.duration }
            if let c = iv.contentTintColor { j["tint"] = colorHex(c) }
            j["template"] = iv.image?.isTemplate ?? false
        }
        d["image"] = j
    }
}

/// Files kept across launches in the app's caches (ASUKA_CACHE, else
/// Caches/<its ID>), the least recently used going first past a limit:
/// iOS's AsukaDiskCache, for images fetched over http(s).
final class AsukaDiskCache: @unchecked Sendable {
    /// Images fetched over http(s), up to 256 MB.
    static let images = AsukaDiskCache("asuka-images", limit: 256 << 20)

    let dir: URL
    let limit: Int
    private let lock = NSLock()
    private var trimming = false

    init(_ name: String, limit: Int) {
        let env = ProcessInfo.processInfo.environment["ASUKA_CACHE"] ?? ""
        let base: URL
        if !env.isEmpty {
            base = URL(fileURLWithPath: env, isDirectory: true)
        } else {
            var app = Bundle.main.bundleIdentifier ?? ""
            if app.isEmpty { app = "asuka" }
            base = (FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
                ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)).appendingPathComponent(app, isDirectory: true)
        }
        dir = base.appendingPathComponent(name, isDirectory: true)
        self.limit = limit
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    private func file(_ key: String) -> URL {
        dir.appendingPathComponent(SHA256.hash(data: Data(key.utf8)).map { String(format: "%02x", $0) }.joined())
    }

    func read(_ key: String) -> Data? {
        let f = file(key)
        guard let d = try? Data(contentsOf: f) else { return nil }
        try? FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: f.path) // used now
        return d
    }

    func write(_ key: String, _ data: Data) {
        try? data.write(to: file(key), options: .atomic)
        lock.lock()
        let start = !trimming
        trimming = true
        lock.unlock()
        // Once a launch, after it has written: what's past the limit goes.
        if start { DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 5) { self.trim() } }
    }

    /// Forgets every file (any thread).
    func removeAll() {
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        for f in files { try? FileManager.default.removeItem(at: f) }
    }

    func trim() {
        let keys: [URLResourceKey] = [.fileSizeKey, .contentModificationDateKey]
        guard let files = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: keys) else { return }
        var list = files.compactMap { f -> (URL, Int, Date)? in
            guard let v = try? f.resourceValues(forKeys: Set(keys)) else { return nil }
            return (f, v.fileSize ?? 0, v.contentModificationDate ?? .distantPast)
        }
        var total = list.reduce(0) { $0 + $1.1 }
        list.sort { $0.2 < $1.2 }
        for (f, size, _) in list where total > limit {
            try? FileManager.default.removeItem(at: f)
            total -= size
        }
    }

    /// Whether a response may be kept: not one that says not to, nor one
    /// from this machine (asuka dev's assets, which change).
    static func storable(_ url: URL, _ resp: HTTPURLResponse) -> Bool {
        if let cc = resp.value(forHTTPHeaderField: "Cache-Control")?.lowercased(), cc.contains("no-store") { return false }
        let h = url.host ?? ""
        return !(h == "localhost" || h == "127.0.0.1" || h == "::1")
    }
}
