// platform/camera on iOS and macOS: AVFoundation. The View is an
// AVCaptureSession's preview filling its box, with a photo output for
// capture, a movie file output in video mode (record), and with scan on a
// metadata output reading codes (macOS 13). The session runs while the
// view is in a window. take is UIImagePickerController's camera on iOS;
// on a Mac, which has no system camera screen, a sheet of the package's
// over the window in use: a live preview with Take Photo and Cancel.
// Photos and videos go to the app's caches (asuka-camera).
import Asuka
import AVFoundation
import Foundation
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

/// Where photos and videos go: the app's caches' asuka-camera.
func cameraFile(_ ext: String) -> URL {
    let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("asuka-camera", isDirectory: true)
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir.appendingPathComponent(UUID().uuidString + "." + ext)
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
        preview.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        layer?.addSublayer(preview)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) isn't used") }

    override func layout() {
        super.layout()
        preview.frame = bounds
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        attached?(window != nil)
    }
}
#endif

@MainActor final class CameraView: NSObject, View, AsukaDestroying, AVCapturePhotoCaptureDelegate, AVCaptureMetadataOutputObjectsDelegate, AVCaptureFileOutputRecordingDelegate {
    /// The View on screen, which capture and record take from.
    static weak var current: CameraView?
    let events: ViewEvents? // none: take's own view, on a Mac
    let preview = CameraPreview()
    var view: AsukaPlatformView { preview }
    private let session = AVCaptureSession()
    private let output = AVCapturePhotoOutput()
    private let metadata = AVCaptureMetadataOutput()
    private let movie = AVCaptureMovieFileOutput()
    private var facing: AVCaptureDevice.Position = .back
    private var scan = false
    private var kinds = CodeKinds([])
    private var seen = CodeSeen()
    private var configuring = false
    private var video = false
    private var capturing: CheckedContinuation<String, Error>?
    private var recording: CheckedContinuation<String, Error>?
    private let queue = DispatchQueue(label: "asuka.camera")

    init(_ events: ViewEvents?) {
        self.events = events
        super.init()
        preview.preview.session = session
        preview.preview.videoGravity = .resizeAspectFill
        preview.attached = { [weak self] on in
            guard let self else { return }
            if on { CameraView.current = self } else if CameraView.current === self { CameraView.current = nil }
            self.run(on)
        }
    }

    func setFacing(_ value: Facing) { facing = value == .front ? .front : .back; changed() }
    func setMode(_ value: Mode) { video = value == .video; changed() }
    func setScan(_ value: Bool) { scan = value; changed() }
    func setCodes(_ value: [String]) { kinds = CodeKinds(value); changed() }

    /// Configures once for the props set together.
    private func changed() {
        guard !configuring else { return }
        configuring = true
        DispatchQueue.main.async { [weak self] in
            self?.configuring = false
            self?.configure()
        }
    }

    static let metadataTypes: [(String, [AVMetadataObject.ObjectType])] = {
        var t: [(String, [AVMetadataObject.ObjectType])] = [
            ("qr", [.qr]), ("aztec", [.aztec]), ("datamatrix", [.dataMatrix]), ("pdf417", [.pdf417]),
            ("ean13", [.ean13]), ("ean8", [.ean8]), ("upca", [.ean13]), ("upce", [.upce]),
            ("code128", [.code128]), ("code39", [.code39]), ("code93", [.code93]), ("itf", [.itf14, .interleaved2of5]),
        ]
        if #available(iOS 15.4, macOS 12.3, *) { t.append(("codabar", [.codabar])) }
        return t
    }()

    private func configure() {
        AVCaptureDevice.requestAccess(for: .video) { ok in
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                guard ok else { self.events?.error(message: "not allowed"); return }
                guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: self.facing) ?? AVCaptureDevice.default(for: .video),
                      let input = try? AVCaptureDeviceInput(device: device) else {
                    self.events?.error(message: "no camera"); return
                }
                self.session.beginConfiguration()
                self.session.sessionPreset = self.video ? .high : .photo
                for i in self.session.inputs where (i as? AVCaptureDeviceInput)?.device.hasMediaType(.video) ?? true {
                    self.session.removeInput(i)
                }
                if self.session.canAddInput(input) { self.session.addInput(input) }
                if self.video {
                    if !self.session.outputs.contains(self.movie), self.session.canAddOutput(self.movie) { self.session.addOutput(self.movie) }
                } else if self.session.outputs.contains(self.movie) {
                    self.session.removeOutput(self.movie)
                }
                if !self.session.outputs.contains(self.output), self.session.canAddOutput(self.output) { self.session.addOutput(self.output) }
                if self.scan {
                    if !self.session.outputs.contains(self.metadata), self.session.canAddOutput(self.metadata) {
                        self.session.addOutput(self.metadata)
                        self.metadata.setMetadataObjectsDelegate(self, queue: .main)
                    }
                    // What it can read is known once it's in the session.
                    let available = self.metadata.availableMetadataObjectTypes
                    var types: [AVMetadataObject.ObjectType] = []
                    for (k, ts) in Self.metadataTypes where self.kinds.wants(k) {
                        for t in ts where available.contains(t) && !types.contains(t) { types.append(t) }
                    }
                    self.metadata.metadataObjectTypes = types
                } else if self.session.outputs.contains(self.metadata) {
                    self.session.removeOutput(self.metadata)
                }
                self.session.commitConfiguration()
                self.run(self.preview.window != nil)
            }
        }
    }

    private func run(_ on: Bool) {
        let s = session
        queue.async { // startRunning blocks: off the main thread
            if on, !s.isRunning, !s.inputs.isEmpty { s.startRunning() } else if !on, s.isRunning { s.stopRunning() }
        }
    }

    func destroy() {
        if CameraView.current === self { CameraView.current = nil }
        run(false)
    }

    func capture() async throws -> String {
        guard session.isRunning else { throw CameraError.noView }
        capturing?.resume(throwing: AsukaNativeError.unsupported("another photo was taken"))
        return try await withCheckedThrowingContinuation { c in
            capturing = c
            output.capturePhoto(with: AVCapturePhotoSettings(), delegate: self)
        }
    }

    /// Records to a file until stopRecording (or max seconds, if not 0),
    /// with the microphone unless mute (or not allowed).
    func record(mute: Bool, max: Double) async throws -> String {
        guard session.isRunning else { throw CameraError.noView }
        guard video, session.outputs.contains(movie) else { throw AsukaNativeError.unsupported("the camera view isn't in video mode") }
        guard !movie.isRecording else { throw AsukaNativeError.unsupported("already recording") }
        var sound = false
        if !mute { sound = await AVCaptureDevice.requestAccess(for: .audio) }
        session.beginConfiguration()
        let mic = session.inputs.first { ($0 as? AVCaptureDeviceInput)?.device.hasMediaType(.audio) ?? false }
        if sound, mic == nil, let d = AVCaptureDevice.default(for: .audio), let i = try? AVCaptureDeviceInput(device: d), session.canAddInput(i) {
            session.addInput(i)
        } else if !sound, let mic {
            session.removeInput(mic)
        }
        session.commitConfiguration()
        movie.maxRecordedDuration = max > 0 ? CMTime(seconds: max, preferredTimescale: 600) : .invalid
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
            let c = self.capturing
            self.capturing = nil
            guard let data, error == nil else { c?.resume(throwing: error ?? AsukaNativeError.unsupported("no photo")); return }
            let f = cameraFile("jpg")
            do {
                try data.write(to: f)
                c?.resume(returning: f.path)
            } catch {
                c?.resume(throwing: error)
            }
        }
    }

    nonisolated func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo url: URL, from c: [AVCaptureConnection], error: Error?) {
        // Stopped by its maximum duration (or the disk filling) the file is
        // still whole: AVErrorRecordingSuccessfullyFinishedKey.
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

    func capture() async throws -> String {
        guard let v = await CameraView.current else { throw CameraError.noView }
        return try await v.capture()
    }

    func record(mute: Bool, maxDuration: TimeInterval) async throws -> String {
        guard let v = await CameraView.current else { throw CameraError.noView }
        return try await v.record(mute: mute, max: maxDuration)
    }

    func stopRecording() throws { MainActor.assumeIsolated { CameraView.current?.stopRecording() } }

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
