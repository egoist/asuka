// platform/camera on iOS and macOS: AVFoundation. The View is an
// AVCaptureSession's preview filling its box, with a photo output for
// capture, a movie file output in video mode (record), with scan on a
// metadata output reading codes (macOS 13), and with frameRate on a video
// data output whose frames it writes as JPEGs (frame). The session is
// configured and started on a queue of its own, while the view is in a
// window. take is UIImagePickerController's camera on iOS; on a Mac,
// which has no system camera screen, a sheet of the package's over the
// window in use: a live preview with Take Photo and Cancel. Photos and
// videos go to the app's caches (asuka-camera).
//
// iOS's back camera is the virtual one with every lens the phone has
// (triple, dual wide, dual, else the wide one), as the Camera app's: zoom 1
// is the wide lens, below 1 the ultra wide, and AVFoundation switches
// lenses as it zooms. A pinch zooms, a tap focuses and meters there (and
// the camera's continuous focus comes back when the scene changes, as
// the Camera app's); torch, flash, exposure bias, the session's preset
// (videoQuality) and the photos' quality prioritization are AVFoundation's.
//
// A Mac's cameras zoom, light and bias nothing (macOS has no
// videoZoomFactor, torch for its webcams, nor exposure bias): zoom there
// is digital, the preview scaled and photos and frames cropped to its
// middle, up to 4×, in photo mode; in video mode the movie file output
// can't crop, so maxZoom is 1. A Mac's camera focuses (or meters) at a
// point only where it says it can (a Continuity Camera's iPhone).
import Asuka
import AVFoundation
import CoreImage
import Foundation
import ImageIO
import UniformTypeIdentifiers
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

/// The kinds of code asked for (none: every kind).
struct CodeKinds {
    let wanted: Set<String>
    init(_ kinds: [String]) { wanted = Set(kinds) }
    func wants(_ kind: String) -> Bool { wanted.isEmpty || wanted.contains(kind) }

    /// The code as platform/scan names it. Apple reads a UPC-A as an EAN-13
    /// with a leading 0: it's a UPC-A again if the app asked for those.
    func named(_ kind: String, _ value: String) -> (String, String) {
        if kind == "ean13", value.count == 13, value.hasPrefix("0"), wants("upca") {
            return ("upca", String(value.dropFirst()))
        }
        return (kind, value)
    }
}

/// Repeats of a code in view, dropped: a code is reported as it comes
/// into view, and again once it's been out of it for a second.
struct CodeSeen {
    private var last: [String: TimeInterval] = [:]
    mutating func isNew(_ kind: String, _ value: String, now: TimeInterval) -> Bool {
        let key = kind + "\u{0}" + value
        let new = (last[key].map { now - $0 > 1 }) ?? true
        last[key] = now
        if last.count > 32 { last = last.filter { now - $0.value <= 1 } }
        return new
    }
}

/// The app's caches' asuka-camera, made.
func cameraDir() -> URL {
    let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("asuka-camera", isDirectory: true)
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir
}

/// Where photos and videos go: the app's caches' asuka-camera.
func cameraFile(_ ext: String) -> URL {
    cameraDir().appendingPathComponent(UUID().uuidString + "." + ext)
}

/// How far a Mac zooms digitally (its preview scaled, its photos and
/// frames cropped).
let digitalMaxZoom = 4.0

/// A photo's data as capture gives it: as the camera wrote it (its EXIF
/// orientation kept) when nothing is asked, else decoded, turned upright,
/// cropped to its middle by crop (a Mac's digital zoom), scaled down to
/// maxSize on its long side (never up), and written as a JPEG of quality
/// (0: 0.9), its metadata kept but for the orientation.
func photoJPEG(_ data: Data, maxSize: Int, quality: Double, crop: Double) throws -> Data {
    if maxSize <= 0 && quality <= 0 && crop <= 1.001 { return data }
    guard let src = CGImageSourceCreateWithData(data as CFData, nil),
          let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any] else {
        throw AsukaNativeError.unsupported("camera: the photo couldn't be read")
    }
    let pw = (props[kCGImagePropertyPixelWidth] as? Int) ?? 0, ph = (props[kCGImagePropertyPixelHeight] as? Int) ?? 0
    let long = max(pw, ph, 1)
    var image: CGImage?
    if crop <= 1.001 {
        // Upright and scaled in one: ImageIO's thumbnail of the whole.
        let size = maxSize > 0 ? min(maxSize, long) : long
        image = CGImageSourceCreateThumbnailAtIndex(src, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: size,
        ] as CFDictionary)
    } else if let full = CGImageSourceCreateThumbnailAtIndex(src, 0, [
        kCGImageSourceCreateThumbnailFromImageAlways: true,
        kCGImageSourceCreateThumbnailWithTransform: true,
        kCGImageSourceThumbnailMaxPixelSize: long,
    ] as CFDictionary) {
        let w = Double(full.width) / crop, h = Double(full.height) / crop
        let rect = CGRect(x: (Double(full.width) - w) / 2, y: (Double(full.height) - h) / 2, width: w, height: h).integral
        image = full.cropping(to: rect).flatMap { scaled($0, maxSize) }
    }
    guard let image else { throw AsukaNativeError.unsupported("camera: the photo couldn't be read") }
    var meta = props
    for k in [kCGImagePropertyPixelWidth, kCGImagePropertyPixelHeight, kCGImagePropertyOrientation] { meta[k] = nil }
    meta[kCGImagePropertyOrientation] = 1
    if var tiff = meta[kCGImagePropertyTIFFDictionary] as? [CFString: Any] {
        tiff[kCGImagePropertyTIFFOrientation] = 1
        meta[kCGImagePropertyTIFFDictionary] = tiff
    }
    meta[kCGImageDestinationLossyCompressionQuality] = quality > 0 ? quality : 0.9
    let out = NSMutableData()
    guard let dest = CGImageDestinationCreateWithData(out, UTType.jpeg.identifier as CFString, 1, nil) else {
        throw AsukaNativeError.unsupported("camera: couldn't write the photo")
    }
    CGImageDestinationAddImage(dest, image, meta as CFDictionary)
    guard CGImageDestinationFinalize(dest) else { throw AsukaNativeError.unsupported("camera: couldn't write the photo") }
    return out as Data
}

/// An image scaled down to at most size on its long side (0: as it is).
func scaled(_ img: CGImage, _ size: Int) -> CGImage? {
    let long = max(img.width, img.height)
    guard size > 0, long > size else { return img }
    let f = Double(size) / Double(long)
    let w = max(Int((Double(img.width) * f).rounded()), 1), h = max(Int((Double(img.height) * f).rounded()), 1)
    guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                              space: img.colorSpace ?? CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
    ctx.interpolationQuality = .high
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
    return ctx.makeImage()
}

/// The preview's view: an AVCaptureVideoPreviewLayer filling it, which
/// hears when it's in a window.
#if canImport(UIKit)
final class CameraPreview: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
    var preview: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    var attached: ((Bool) -> Void)?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        attached?(window != nil)
    }
}
#else
final class CameraPreview: NSView {
    let preview = AVCaptureVideoPreviewLayer()
    var attached: ((Bool) -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer = CALayer()
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.masksToBounds = true // a digital zoom's scaled preview, clipped
        layer?.addSublayer(preview)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) isn't used") }

    /// The preview fills the view, scaled about its middle by the digital
    /// zoom (its transform: so its bounds and position, not its frame).
    func place() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        preview.bounds = CGRect(origin: .zero, size: bounds.size)
        preview.position = CGPoint(x: bounds.midX, y: bounds.midY)
        CATransaction.commit()
    }

    override func layout() {
        super.layout()
        place()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        place()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        attached?(window != nil)
    }
}
#endif

/// The frames frameRate asks for: a video data output's, sampled at most
/// rate times a second on its own queue, each written as a JPEG (long side
/// at most size, the middle 1/crop of it with a Mac's digital zoom) to a
/// ring of four files, and handed to the main thread. Frames that come
/// while one is being written are dropped (alwaysDiscardsLateVideoFrames).
final class FrameSampler: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let queue = DispatchQueue(label: "asuka.camera.frames")
    private let lock = NSLock()
    private var rate = 0.0, size = 480, crop = 1.0
    private var last = -1.0
    private var n = 0
    private let id: Int
    private let start: Double
    private lazy var context = CIContext()
    private let space = CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB()
    /// Called on the main thread with each frame.
    var emit: (@MainActor (Frame) -> Void)?

    init(id: Int) {
        self.id = id
        start = CMClockGetTime(CMClockGetHostTimeClock()).seconds
    }

    func set(rate: Double? = nil, size: Int? = nil, crop: Double? = nil) {
        lock.lock()
        if let rate { self.rate = rate }
        if let size { self.size = max(size, 16) }
        if let crop { self.crop = crop }
        lock.unlock()
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sb: CMSampleBuffer, from connection: AVCaptureConnection) {
        lock.lock()
        let rate = self.rate, size = self.size, crop = self.crop
        lock.unlock()
        guard rate > 0, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let now = CMClockGetTime(CMClockGetHostTimeClock()).seconds
        // A little early is on time: frames come at the camera's rate.
        if last >= 0, now - last < 1 / rate - 0.012 { return }
        last = now
        var img = CIImage(cvPixelBuffer: pb)
        if crop > 1.001 {
            let e = img.extent
            let w = e.width / crop, h = e.height / crop
            img = img.cropped(to: CGRect(x: e.midX - w / 2, y: e.midY - h / 2, width: w, height: h).integral)
        }
        let e = img.extent
        let f = min(1, Double(size) / max(e.width, e.height))
        img = img.transformed(by: CGAffineTransform(translationX: -e.minX, y: -e.minY).concatenating(CGAffineTransform(scaleX: f, y: f)))
        let w = Int((e.width * f).rounded()), h = Int((e.height * f).rounded())
        img = img.cropped(to: CGRect(x: 0, y: 0, width: w, height: h))
        let q = CIImageRepresentationOption(rawValue: kCGImageDestinationLossyCompressionQuality as String)
        guard let jpeg = context.jpegRepresentation(of: img, colorSpace: space, options: [q: 0.8]) else { return }
        let url = cameraDir().appendingPathComponent("frame-\(id)-\(n % 4).jpg")
        n += 1
        guard (try? jpeg.write(to: url, options: .atomic)) != nil else { return }
        let frame = Frame(path: url.path, width: w, height: h, time: now - start)
        DispatchQueue.main.async { [weak self] in
            MainActor.assumeIsolated { self?.emit?(frame) }
        }
    }
}

/// The photo capture is taking: who waits, and what it asked.
@MainActor struct Shot {
    let wait: CheckedContinuation<String, Error>
    let maxSize: Int
    let quality: Double
    let crop: Double
}

@MainActor final class CameraView: NSObject, View, AsukaDestroying, AVCapturePhotoCaptureDelegate, AVCaptureMetadataOutputObjectsDelegate, AVCaptureFileOutputRecordingDelegate {
    /// The View on screen, which capture and record take from.
    static weak var current: CameraView?
    private static var made = 0
    let events: ViewEvents? // none: take's own view, on a Mac
    let preview = CameraPreview()
    var view: AsukaPlatformView { preview }
    private let session = AVCaptureSession()
    private let output = AVCapturePhotoOutput()
    private let metadata = AVCaptureMetadataOutput()
    private let movie = AVCaptureMovieFileOutput()
    private let data = AVCaptureVideoDataOutput()
    private let sampler: FrameSampler
    /// The camera in use, and the device's zoom factor of Asuka's 1 (2 on a
    /// virtual camera with an ultra wide lens: its wide one's).
    private var device: AVCaptureDevice?
    private var zoomScale = 1.0
    private var facing: AVCaptureDevice.Position = .back
    private var scan = false
    private var kinds = CodeKinds([])
    private var seen = CodeSeen()
    private var configuring = false
    private var video = false
    private var zoom = 1.0
    private var pinchToZoom = true
    private var torch = false
    private var flash = Flash.off
    private var exposure = 0.0
    private var tapToFocus = true
    private var videoQuality = VideoQuality.fullHD
    private var photoQuality = PhotoQuality.balanced
    private var mirror = false
    private var frameRate = 0.0
    /// What ready last said, and of which camera.
    private var said: Info?
    private var saidOf = ""
    /// A pinch's zoom when it began, and the zooms it raised (the bind's
    /// echoes of which aren't zooms of the app's).
    private var pinchFrom = 1.0
    private var raised: [Double] = []
    private var capturing: Shot?
    private var recording: CheckedContinuation<String, Error>?
    private let queue = DispatchQueue(label: "asuka.camera")
    private var areaWatch: NSObjectProtocol?
    #if canImport(UIKit)
    private var rotation: AnyObject? // AVCaptureDevice.RotationCoordinator, iOS 17
    private var rotationWatch: [NSKeyValueObservation] = []
    #endif

    init(_ events: ViewEvents?) {
        self.events = events
        CameraView.made += 1
        sampler = FrameSampler(id: CameraView.made)
        super.init()
        preview.preview.session = session
        preview.preview.videoGravity = .resizeAspectFill
        preview.attached = { [weak self] on in
            guard let self else { return }
            if on { CameraView.current = self } else if CameraView.current === self { CameraView.current = nil }
            self.run(on)
        }
        data.alwaysDiscardsLateVideoFrames = true
        data.setSampleBufferDelegate(sampler, queue: sampler.queue)
        sampler.emit = { [weak self] f in self?.events?.frame(frame: f) }
        #if canImport(UIKit)
        preview.addGestureRecognizer(UIPinchGestureRecognizer(target: self, action: #selector(pinched(_:))))
        preview.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapped(_:))))
        #else
        preview.addGestureRecognizer(NSMagnificationGestureRecognizer(target: self, action: #selector(pinched(_:))))
        preview.addGestureRecognizer(NSClickGestureRecognizer(target: self, action: #selector(tapped(_:))))
        #endif
    }

    func setFacing(_ value: Facing) {
        let f: AVCaptureDevice.Position = value == .front ? .front : .back
        guard f != facing || device == nil else { return }
        facing = f
        changed()
    }
    func setMode(_ value: Mode) { video = value == .video; changed() }
    func setScan(_ value: Bool) { scan = value; changed() }
    func setCodes(_ value: [String]) { kinds = CodeKinds(value); changed() }

    func setZoom(_ value: Double) {
        // The bind's echo of a pinch's zoom: the pinch has gone on since.
        if let i = raised.firstIndex(of: value) {
            raised.removeFirst(i + 1)
            return
        }
        raised = []
        zoom = value
        applyZoom()
    }

    func setPinchToZoom(_ value: Bool) { pinchToZoom = value }
    func setTorch(_ value: Bool) { torch = value; applyTorch() }
    func setFlash(_ value: Flash) { flash = value }
    func setExposure(_ value: Double) { exposure = value; applyExposure() }
    func setTapToFocus(_ value: Bool) { tapToFocus = value }
    func setVideoQuality(_ value: VideoQuality) {
        guard value != videoQuality else { return }
        videoQuality = value
        if video { changed() }
    }
    func setPhotoQuality(_ value: PhotoQuality) { photoQuality = value }
    func setMirror(_ value: Bool) { mirror = value; connect() }

    func setFrameRate(_ value: Double) {
        let r = min(max(value.isFinite ? value : 0, 0), 15)
        let was = frameRate > 0
        frameRate = r
        sampler.set(rate: r)
        if was != (r > 0) { changed() }
    }

    func setFrameSize(_ value: Int) { sampler.set(size: value) }

    /// Configures once for the props set together.
    private func changed() {
        guard !configuring else { return }
        configuring = true
        DispatchQueue.main.async { [weak self] in
            self?.configuring = false
            self?.configure()
        }
    }

    nonisolated static let metadataTypes: [(String, [AVMetadataObject.ObjectType])] = {
        var t: [(String, [AVMetadataObject.ObjectType])] = [
            ("qr", [.qr]), ("aztec", [.aztec]), ("datamatrix", [.dataMatrix]), ("pdf417", [.pdf417]),
            ("ean13", [.ean13]), ("ean8", [.ean8]), ("upca", [.ean13]), ("upce", [.upce]),
            ("code128", [.code128]), ("code39", [.code39]), ("code93", [.code93]), ("itf", [.itf14, .interleaved2of5]),
        ]
        if #available(iOS 15.4, macOS 12.3, *) { t.append(("codabar", [.codabar])) }
        return t
    }()

    /// The camera facing pos: on iOS's back, the virtual camera with the
    /// most lenses (as the Camera app's), else the wide one.
    static func camera(_ pos: AVCaptureDevice.Position) -> AVCaptureDevice? {
        #if canImport(UIKit)
        let types: [AVCaptureDevice.DeviceType] = pos == .back
            ? [.builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera, .builtInWideAngleCamera]
            : [.builtInWideAngleCamera]
        for t in types {
            if let d = AVCaptureDevice.default(t, for: .video, position: pos) { return d }
        }
        return AVCaptureDevice.default(for: .video)
        #else
        return AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: pos) ?? AVCaptureDevice.default(for: .video)
        #endif
    }

    /// The device's zoom factor of Asuka's 1: a virtual camera's that
    /// starts at its ultra wide lens switches to its wide one at its first
    /// switch-over factor.
    static func scaleOf(_ d: AVCaptureDevice) -> Double {
        #if canImport(UIKit)
        if d.isVirtualDevice, d.constituentDevices.contains(where: { $0.deviceType == .builtInUltraWideCamera }),
           let f = d.virtualDeviceSwitchOverVideoZoomFactors.first {
            return f.doubleValue
        }
        #endif
        return 1
    }

    /// The session's presets for a video quality, the nearest first.
    static func presets(_ q: VideoQuality) -> [AVCaptureSession.Preset] {
        switch q {
        case .sd: return [.vga640x480, .hd1280x720, .hd1920x1080, .high]
        case .hd: return [.hd1280x720, .hd1920x1080, .vga640x480, .high]
        case .uhd: return [.hd4K3840x2160, .hd1920x1080, .hd1280x720, .high]
        default: return [.hd1920x1080, .hd1280x720, .hd4K3840x2160, .high]
        }
    }

    private func configure() {
        // No camera (a simulator, a Mac without one): said without asking.
        guard let device = Self.camera(facing) else { events?.error(message: "no camera"); return }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: break
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { _ in
                DispatchQueue.main.async { [weak self] in self?.configure() }
            }
            return
        default:
            events?.error(message: "not allowed")
            return
        }
        let input: AVCaptureDeviceInput
        if let i = session.inputs.compactMap({ $0 as? AVCaptureDeviceInput }).first(where: { $0.device == device }) {
            input = i
        } else {
            guard let i = try? AVCaptureDeviceInput(device: device) else { events?.error(message: "no camera"); return }
            input = i
        }
        let changedDevice = self.device != device
        self.device = device
        zoomScale = Self.scaleOf(device)
        if changedDevice { watch(device) }
        let video = self.video, scan = self.scan, kinds = self.kinds, frames = frameRate > 0
        let presets = video ? Self.presets(videoQuality) : [.photo, .high]
        let on = preview.window != nil
        let s = session, photo = output, metadata = self.metadata, movie = self.movie, data = self.data
        queue.async { [weak self] in
            s.beginConfiguration()
            let old = s.inputs.compactMap { $0 as? AVCaptureDeviceInput }.filter { $0.device.hasMediaType(.video) && $0 !== input }
            if !old.isEmpty || !s.inputs.contains(input) {
                for i in old { s.removeInput(i) }
                // A preset every camera has while it changes: the new one may
                // not have the old one's.
                if s.canSetSessionPreset(.high) { s.sessionPreset = .high }
                if !s.inputs.contains(input), s.canAddInput(input) { s.addInput(input) }
            }
            if let p = presets.first(where: { s.canSetSessionPreset($0) }), s.sessionPreset != p { s.sessionPreset = p }
            func use(_ o: AVCaptureOutput, _ want: Bool) {
                if want, !s.outputs.contains(o), s.canAddOutput(o) { s.addOutput(o) } else if !want, s.outputs.contains(o) { s.removeOutput(o) }
            }
            use(movie, video)
            use(photo, true)
            if s.outputs.contains(photo), photo.maxPhotoQualityPrioritization != .quality {
                photo.maxPhotoQualityPrioritization = .quality // each photo asks for its own (photoQuality)
            }
            use(metadata, scan)
            if scan, s.outputs.contains(metadata) {
                // What it can read is known once it's in the session.
                let available = metadata.availableMetadataObjectTypes
                var types: [AVMetadataObject.ObjectType] = []
                for (k, ts) in CameraView.metadataTypes where kinds.wants(k) {
                    for t in ts where available.contains(t) && !types.contains(t) { types.append(t) }
                }
                metadata.metadataObjectTypes = types
            }
            // Frames beside a movie file output: where the camera can.
            use(data, frames)
            s.commitConfiguration()
            if on, !s.isRunning, !s.inputs.isEmpty { s.startRunning() }
            let running = s.isRunning
            DispatchQueue.main.async {
                guard let self else { return }
                if scan, s.outputs.contains(metadata) { metadata.setMetadataObjectsDelegate(self, queue: .main) }
                self.connect()
                self.applyDevice()
                if running { self.ready() }
            }
        }
    }

    /// Watches the camera in use: its subject area's changes (iOS: the
    /// focus a tap set gives way to continuous focus again), and on iOS 17
    /// its rotation (the preview's, as the interface is; the photos',
    /// videos' and frames', level with the horizon).
    private func watch(_ d: AVCaptureDevice) {
        if let w = areaWatch { NotificationCenter.default.removeObserver(w) }
        areaWatch = nil
        #if canImport(UIKit)
        areaWatch = NotificationCenter.default.addObserver(forName: AVCaptureDevice.subjectAreaDidChangeNotification, object: d, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.focusDevice(CGPoint(x: 0.5, y: 0.5), continuous: true) }
        }
        rotationWatch = []
        rotation = nil
        if #available(iOS 17.0, *) {
            let rc = AVCaptureDevice.RotationCoordinator(device: d, previewLayer: preview.preview)
            rotation = rc
            rotationWatch = [
                rc.observe(\.videoRotationAngleForHorizonLevelPreview, options: [.initial, .new]) { [weak self] rc, _ in
                    let a = rc.videoRotationAngleForHorizonLevelPreview
                    DispatchQueue.main.async {
                        MainActor.assumeIsolated {
                            guard let c = self?.preview.preview.connection, c.isVideoRotationAngleSupported(a) else { return }
                            c.videoRotationAngle = a
                        }
                    }
                },
                rc.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { [weak self] _, _ in
                    DispatchQueue.main.async { MainActor.assumeIsolated { self?.connect() } }
                },
            ]
        }
        #endif
    }

    /// The outputs' connections: the front camera's photos, videos and
    /// frames mirrored as its preview is when mirror is on (else as others
    /// see the user); the frames upright.
    private func connect() {
        #if canImport(UIKit)
        let mirrored = facing == .front
        #else
        let mirrored = preview.preview.connection?.isVideoMirrored ?? false
        #endif
        let m = mirror && mirrored
        for o in [output as AVCaptureOutput, movie, data] {
            guard session.outputs.contains(o), let c = o.connection(with: .video) else { continue }
            if c.isVideoMirroringSupported {
                c.automaticallyAdjustsVideoMirroring = false
                c.isVideoMirrored = m
            }
        }
        if session.outputs.contains(data), let c = data.connection(with: .video) { orient(c) }
    }

    /// A connection turned as the device is held, level with the horizon
    /// (iOS 17's rotation coordinator; before, as the interface is).
    private func orient(_ c: AVCaptureConnection) {
        #if canImport(UIKit)
        if #available(iOS 17.0, *) {
            guard let rc = rotation as? AVCaptureDevice.RotationCoordinator else { return }
            let a = rc.videoRotationAngleForHorizonLevelCapture
            if c.isVideoRotationAngleSupported(a) { c.videoRotationAngle = a }
        } else if c.isVideoOrientationSupported {
            let o = preview.window?.windowScene?.interfaceOrientation ?? .portrait
            switch o {
            case .landscapeLeft: c.videoOrientation = .landscapeLeft
            case .landscapeRight: c.videoOrientation = .landscapeRight
            case .portraitUpsideDown: c.videoOrientation = .portraitUpsideDown
            default: c.videoOrientation = .portrait
            }
        }
        #endif
    }

    // MARK: The camera's settings

    /// The zooms the camera takes, as Asuka's.
    private var zoomRange: ClosedRange<Double> {
        #if canImport(UIKit)
        guard let d = device else { return 1...1 }
        let lo = Double(d.minAvailableVideoZoomFactor) / zoomScale, hi = Double(d.maxAvailableVideoZoomFactor) / zoomScale
        return lo...max(lo, hi)
        #else
        return 1...(video ? 1 : digitalMaxZoom)
        #endif
    }

    private func clampZoom(_ z: Double) -> Double {
        let r = zoomRange
        return min(max(z.isFinite ? z : 1, r.lowerBound), r.upperBound)
    }

    private func applyZoom() {
        let z = clampZoom(zoom)
        #if canImport(UIKit)
        guard let d = device else { return }
        let f = CGFloat(z * zoomScale)
        queue.async {
            guard (try? d.lockForConfiguration()) != nil else { return }
            d.videoZoomFactor = min(max(f, d.minAvailableVideoZoomFactor), d.maxAvailableVideoZoomFactor)
            d.unlockForConfiguration()
        }
        #else
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        preview.preview.setAffineTransform(CGAffineTransform(scaleX: z, y: z))
        CATransaction.commit()
        sampler.set(crop: z)
        #endif
    }

    private func applyTorch() {
        guard let d = device else { return }
        let mode: AVCaptureDevice.TorchMode = torch ? .on : .off
        queue.async {
            guard d.hasTorch, d.isTorchModeSupported(mode), d.torchMode != mode, (try? d.lockForConfiguration()) != nil else { return }
            d.torchMode = mode
            d.unlockForConfiguration()
        }
    }

    private func applyExposure() {
        #if canImport(UIKit)
        guard let d = device else { return }
        let ev = Float(exposure.isFinite ? exposure : 0)
        queue.async {
            guard (try? d.lockForConfiguration()) != nil else { return }
            d.setExposureTargetBias(min(max(ev, d.minExposureTargetBias), d.maxExposureTargetBias))
            d.unlockForConfiguration()
        }
        #endif
        // A Mac's cameras take no exposure bias.
    }

    /// What the camera keeps across a change of camera or a start: zoom,
    /// torch, exposure.
    private func applyDevice() {
        applyZoom()
        applyTorch()
        applyExposure()
    }

    /// Raises ready with what the camera can do, once for each camera (and
    /// again if that changes: a Mac's video mode zooms no more).
    private func ready() {
        guard let d = device else { return }
        let r = zoomRange
        var info = Info(minZoom: r.lowerBound, maxZoom: r.upperBound, torch: d.hasTorch,
                        flash: output.supportedFlashModes.contains(.on), focus: d.isFocusPointOfInterestSupported)
        #if canImport(UIKit)
        info.minExposure = Double(d.minExposureTargetBias)
        info.maxExposure = Double(d.maxExposureTargetBias)
        #endif
        if let s = said, saidOf == d.uniqueID, s.minZoom == info.minZoom, s.maxZoom == info.maxZoom, s.minExposure == info.minExposure,
           s.maxExposure == info.maxExposure, s.torch == info.torch, s.flash == info.flash, s.focus == info.focus {
            return
        }
        said = info
        saidOf = d.uniqueID
        events?.ready(info: info)
    }

    // MARK: Gestures

    #if canImport(UIKit)
    @objc private func pinched(_ g: UIPinchGestureRecognizer) { pinch(g.state, Double(g.scale)) }
    @objc private func tapped(_ g: UITapGestureRecognizer) {
        guard tapToFocus, g.state == .ended else { return }
        let p = g.location(in: preview)
        if focus(layerPoint: p) {
            events?.focusPoint(point: Point(x: p.x / max(preview.bounds.width, 1), y: p.y / max(preview.bounds.height, 1)))
        }
    }
    #else
    @objc private func pinched(_ g: NSMagnificationGestureRecognizer) { pinch(g.state, 1 + Double(g.magnification)) }
    @objc private func tapped(_ g: NSClickGestureRecognizer) {
        guard tapToFocus, g.state == .ended else { return }
        let p = g.location(in: preview)
        if focus(viewPoint: p) {
            events?.focusPoint(point: Point(x: p.x / max(preview.bounds.width, 1), y: 1 - p.y / max(preview.bounds.height, 1)))
        }
    }
    #endif

    private func pinch(_ state: GestureState, _ scale: Double) {
        guard pinchToZoom, device != nil else { return }
        switch state {
        case .began:
            pinchFrom = clampZoom(zoom)
        case .changed:
            let z = clampZoom(pinchFrom * scale)
            guard abs(z - zoom) > 0.0005 else { return }
            zoom = z
            applyZoom()
            raised.append(z)
            if raised.count > 64 { raised.removeFirst(raised.count - 64) }
            events?.zoomChange(zoom: z)
        default:
            break
        }
    }

    #if canImport(UIKit)
    typealias GestureState = UIGestureRecognizer.State
    #else
    typealias GestureState = NSGestureRecognizer.State
    #endif

    /// Focuses and meters at x, y (fractions of the view, from its top
    /// left), as a tap there does, raising nothing.
    func focus(x: Double, y: Double) {
        let w = preview.bounds.width, h = preview.bounds.height
        #if canImport(UIKit)
        _ = focus(layerPoint: CGPoint(x: x * w, y: y * h))
        #else
        _ = focus(viewPoint: CGPoint(x: x * w, y: (1 - y) * h))
        #endif
    }

    #if canImport(UIKit)
    private func focus(layerPoint p: CGPoint) -> Bool {
        focusDevice(preview.preview.captureDevicePointConverted(fromLayerPoint: p), continuous: false)
    }
    #else
    private func focus(viewPoint p: CGPoint) -> Bool {
        guard let host = preview.layer else { return false }
        let inPreview = preview.preview.convert(p, from: host) // the digital zoom's scale undone
        return focusDevice(preview.preview.captureDevicePointConverted(fromLayerPoint: inPreview), continuous: false)
    }
    #endif

    /// Focuses and meters the camera at a point of its own: once (a tap,
    /// till the scene changes: iOS's subject area), or continuously (the
    /// camera's own, again; a Mac's camera, which has no subject area,
    /// always). Whether the camera could do either.
    @discardableResult private func focusDevice(_ p: CGPoint, continuous: Bool) -> Bool {
        guard let d = device, d.isFocusPointOfInterestSupported || d.isExposurePointOfInterestSupported else { return false }
        #if canImport(UIKit)
        let once = !continuous
        #else
        let once = false
        #endif
        let fm: AVCaptureDevice.FocusMode = once ? .autoFocus : .continuousAutoFocus
        let em: AVCaptureDevice.ExposureMode = once ? .autoExpose : .continuousAutoExposure
        queue.async {
            guard (try? d.lockForConfiguration()) != nil else { return }
            if d.isFocusPointOfInterestSupported, d.isFocusModeSupported(fm) {
                d.focusPointOfInterest = p
                d.focusMode = fm
            }
            if d.isExposurePointOfInterestSupported, d.isExposureModeSupported(em) {
                d.exposurePointOfInterest = p
                d.exposureMode = em
            }
            #if canImport(UIKit)
            d.isSubjectAreaChangeMonitoringEnabled = once
            #endif
            d.unlockForConfiguration()
        }
        return true
    }

    // MARK: Running

    private func run(_ on: Bool) {
        let s = session
        queue.async { [weak self] in // startRunning blocks: off the main thread
            if on, !s.isRunning, !s.inputs.isEmpty {
                s.startRunning()
                guard s.isRunning else { return }
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        self?.applyDevice()
                        self?.ready()
                    }
                }
            } else if !on, s.isRunning {
                s.stopRunning()
            }
        }
    }

    func destroy() {
        if CameraView.current === self { CameraView.current = nil }
        if let w = areaWatch { NotificationCenter.default.removeObserver(w) }
        areaWatch = nil
        sampler.set(rate: 0)
        #if canImport(UIKit)
        rotationWatch = []
        #endif
        run(false)
    }

    /// The digital zoom photos are cropped by (a Mac's; iOS zooms the
    /// camera).
    private var crop: Double {
        #if canImport(UIKit)
        return 1
        #else
        return clampZoom(zoom)
        #endif
    }

    func capture(maxSize: Int = 0, quality: Double = 0) async throws -> String {
        guard session.isRunning, session.outputs.contains(output) else { throw CameraError.noView }
        capturing?.wait.resume(throwing: AsukaNativeError.unsupported("another photo was taken"))
        capturing = nil
        let s = AVCapturePhotoSettings()
        let fm: AVCaptureDevice.FlashMode = flash == .on ? .on : flash == .auto ? .auto : .off
        if output.supportedFlashModes.contains(fm) { s.flashMode = fm }
        let pq: AVCapturePhotoOutput.QualityPrioritization = photoQuality == .speed ? .speed : photoQuality == .quality ? .quality : .balanced
        s.photoQualityPrioritization = pq.rawValue <= output.maxPhotoQualityPrioritization.rawValue ? pq : output.maxPhotoQualityPrioritization
        if let c = output.connection(with: .video) { orient(c) }
        let crop = self.crop
        return try await withCheckedThrowingContinuation { c in
            capturing = Shot(wait: c, maxSize: maxSize, quality: quality, crop: crop)
            output.capturePhoto(with: s, delegate: self)
        }
    }

    /// Records to a file until stopRecording (or max seconds, or maxBytes,
    /// if not 0), with the microphone unless mute (or not allowed).
    func record(mute: Bool, max: Double, maxBytes: Int = 0) async throws -> String {
        guard session.isRunning else { throw CameraError.noView }
        guard video, session.outputs.contains(movie) else { throw AsukaNativeError.unsupported("the camera view isn't in video mode") }
        guard !movie.isRecording else { throw AsukaNativeError.unsupported("already recording") }
        var sound = false
        if !mute { sound = await AVCaptureDevice.requestAccess(for: .audio) }
        let s = session
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            queue.async {
                s.beginConfiguration()
                let mic = s.inputs.first { ($0 as? AVCaptureDeviceInput)?.device.hasMediaType(.audio) ?? false }
                if sound, mic == nil, let d = AVCaptureDevice.default(for: .audio), let i = try? AVCaptureDeviceInput(device: d), s.canAddInput(i) {
                    s.addInput(i)
                } else if !sound, let mic {
                    s.removeInput(mic)
                }
                s.commitConfiguration()
                c.resume()
            }
        }
        movie.maxRecordedDuration = max > 0 ? CMTime(seconds: max, preferredTimescale: 600) : .invalid
        movie.maxRecordedFileSize = maxBytes > 0 ? Int64(maxBytes) : 0
        if let c = movie.connection(with: .video) { orient(c) }
        connect()
        return try await withCheckedThrowingContinuation { c in
            recording = c
            movie.startRecording(to: cameraFile("mov"), recordingDelegate: self)
        }
    }

    func stopRecording() { if movie.isRecording { movie.stopRecording() } }

    nonisolated func metadataOutput(_ o: AVCaptureMetadataOutput, didOutput objects: [AVMetadataObject], from c: AVCaptureConnection) {
        let found = objects.compactMap { o -> (AVMetadataObject.ObjectType, String)? in
            guard let m = o as? AVMetadataMachineReadableCodeObject, let v = m.stringValue else { return nil }
            return (m.type, v)
        }
        MainActor.assumeIsolated { // its queue is the main one
            let now = Date.timeIntervalSinceReferenceDate
            for (t, v) in found {
                guard let k = Self.metadataTypes.first(where: { $0.0 != "upca" && $0.1.contains(t) })?.0 else { continue }
                let (kind, value) = kinds.named(k, v)
                guard kinds.wants(kind), seen.isNew(kind, value, now: now) else { continue }
                events?.code(code: Code(kind: kind, value: value))
            }
        }
    }

    nonisolated func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingPhoto photo: AVCapturePhoto, error: Error?) {
        let data = photo.fileDataRepresentation()
        Task { @MainActor in
            guard let shot = self.capturing else { return }
            self.capturing = nil
            guard let data, error == nil else { shot.wait.resume(throwing: error ?? AsukaNativeError.unsupported("no photo")); return }
            let (maxSize, quality, crop, wait) = (shot.maxSize, shot.quality, shot.crop, shot.wait)
            DispatchQueue.global(qos: .userInitiated).async {
                do {
                    let jpeg = try photoJPEG(data, maxSize: maxSize, quality: quality, crop: crop)
                    let f = cameraFile("jpg")
                    try jpeg.write(to: f)
                    wait.resume(returning: f.path)
                } catch {
                    wait.resume(throwing: error)
                }
            }
        }
    }

    nonisolated func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo url: URL, from c: [AVCaptureConnection], error: Error?) {
        // Stopped by its maximum duration or size (or the disk filling) the
        // file is still whole: AVErrorRecordingSuccessfullyFinishedKey.
        let ok = error == nil || ((error as NSError?)?.userInfo[AVErrorRecordingSuccessfullyFinishedKey] as? Bool ?? false)
        let err = error
        Task { @MainActor in
            let r = self.recording
            self.recording = nil
            if ok { r?.resume(returning: url.path) } else { r?.resume(throwing: err!) }
        }
    }
}

@MainActor func makeView(_ events: ViewEvents) -> any View { CameraView(events) }

#if canImport(UIKit)
/// take's screen: UIImagePickerController's camera.
@MainActor final class SystemCamera: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
    static let shared = SystemCamera()
    private var waiting: CheckedContinuation<String, Error>?

    func take() async throws -> String {
        guard UIImagePickerController.isSourceTypeAvailable(.camera) else { throw CameraError.noCamera }
        guard let top = AsukaPresenting.top else { throw AsukaNativeError.unsupported("no screen to show the camera on") }
        waiting?.resume(returning: "") // one at a time
        return try await withCheckedThrowingContinuation { c in
            waiting = c
            let p = UIImagePickerController()
            p.sourceType = .camera
            p.delegate = self
            top.present(p, animated: true)
        }
    }

    nonisolated func imagePickerController(_ p: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]) {
        let image = info[.originalImage] as? UIImage
        MainActor.assumeIsolated {
            p.dismiss(animated: true)
            let c = self.waiting
            self.waiting = nil
            guard let image else { c?.resume(returning: ""); return }
            DispatchQueue.global(qos: .userInitiated).async {
                let f = cameraFile("jpg")
                let ok = (try? image.jpegData(compressionQuality: 0.9)?.write(to: f)) != nil
                if ok { c?.resume(returning: f.path) } else { c?.resume(throwing: AsukaNativeError.unsupported("couldn't save the photo")) }
            }
        }
    }

    nonisolated func imagePickerControllerDidCancel(_ p: UIImagePickerController) {
        MainActor.assumeIsolated {
            p.dismiss(animated: true)
            waiting?.resume(returning: "")
            waiting = nil
        }
    }
}
#else
/// take's sheet on a Mac: a live preview, Take Photo and Cancel (Return
/// and Escape), over the window in use.
@MainActor final class SystemCamera: NSObject {
    static let shared = SystemCamera()
    private var sheet: NSWindow?
    private var cameraView: CameraView?
    private var waiting: CheckedContinuation<String, Error>?

    func take() async throws -> String {
        guard AVCaptureDevice.default(for: .video) != nil else { throw CameraError.noCamera }
        guard let parent = AsukaPresenting.window else { throw AsukaNativeError.unsupported("no window to show the camera on") }
        waiting?.resume(returning: "")
        return try await withCheckedThrowingContinuation { c in
            waiting = c
            let v = CameraView(nil)
            cameraView = v
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 520), styleMask: [.titled], backing: .buffered, defer: false)
            let content = NSView(frame: w.contentLayoutRect)
            v.preview.frame = NSRect(x: 0, y: 56, width: 640, height: 464)
            v.preview.autoresizingMask = [.width, .height]
            content.addSubview(v.preview)
            let take = NSButton(title: "Take Photo", target: self, action: #selector(shoot))
            take.keyEquivalent = "\r"
            take.frame = NSRect(x: 520, y: 12, width: 108, height: 32)
            let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel))
            cancel.keyEquivalent = "\u{1b}"
            cancel.frame = NSRect(x: 412, y: 12, width: 100, height: 32)
            content.addSubview(take)
            content.addSubview(cancel)
            w.contentView = content
            sheet = w
            v.setMode(.photo)
            parent.beginSheet(w)
        }
    }

    private func finish(_ path: String) {
        let c = waiting
        waiting = nil
        if let s = sheet { s.sheetParent?.endSheet(s) }
        cameraView?.destroy()
        cameraView = nil
        sheet = nil
        c?.resume(returning: path)
    }

    @objc private func shoot() {
        guard let v = cameraView else { return }
        Task { @MainActor in
            let path = (try? await v.capture()) ?? ""
            self.finish(path)
        }
    }

    @objc private func cancel() { finish("") }
}
#endif

final class AVCamera: Camera {
    @MainActor func take() async throws -> String { try await SystemCamera.shared.take() }

    func capture(maxSize: Int, quality: Double) async throws -> String {
        guard let v = await CameraView.current else { throw CameraError.noView }
        return try await v.capture(maxSize: maxSize, quality: quality)
    }

    func record(mute: Bool, maxDuration: TimeInterval, maxBytes: Int) async throws -> String {
        guard let v = await CameraView.current else { throw CameraError.noView }
        return try await v.record(mute: mute, max: maxDuration, maxBytes: maxBytes)
    }

    func stopRecording() throws {
        if Thread.isMainThread {
            MainActor.assumeIsolated { CameraView.current?.stopRecording() }
        } else {
            DispatchQueue.main.async { MainActor.assumeIsolated { CameraView.current?.stopRecording() } }
        }
    }

    @MainActor func focus(x: Double, y: Double) throws { CameraView.current?.focus(x: x, y: y) }

    @MainActor func access() throws -> String { mediaAccess(.video) }

    @MainActor func requestAccess() async throws -> String { await requestMediaAccess(.video) }
}

/// A device's permission as access says it: "notAsked", "denied" (or
/// restricted) or "allowed".
func mediaAccess(_ media: AVMediaType) -> String {
    switch AVCaptureDevice.authorizationStatus(for: media) {
    case .notDetermined: return "notAsked"
    case .authorized: return "allowed"
    default: return "denied"
    }
}

/// Asks for it if it hasn't been asked (the system's prompt, which
/// answers off the main thread), then the permission again.
func requestMediaAccess(_ media: AVMediaType) async -> String {
    if AVCaptureDevice.authorizationStatus(for: media) == .notDetermined {
        _ = await AVCaptureDevice.requestAccess(for: media)
    }
    return mediaAccess(media)
}

func makeCamera() throws -> any Camera { AVCamera() }
