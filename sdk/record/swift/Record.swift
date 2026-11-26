// platform/record on iOS and macOS: AVAudioRecorder, AAC in MPEG-4. On iOS
// the audio session plays and records while it does (other apps' sound
// stops), and goes back after; an interruption (a call) pauses it, and it
// goes on after if the system says it may. Video is platform/camera's.
import Asuka
import AVFoundation
import Foundation

@MainActor final class AVRecorder: NSObject, Recorder, AVAudioRecorderDelegate {
    static let shared = AVRecorder()
    private var recorder: AVAudioRecorder?
    private var done: CheckedContinuation<String, Error>?
    private var userPaused = false
    #if os(iOS)
    private var category: AVAudioSession.Category?
    private var interruption: NSObjectProtocol?
    #endif

    func record(quality: Quality, maxDuration: TimeInterval, path: String, background: Bool, title: String) async throws -> String {
        guard recorder == nil else { throw RecordError.busy }
        guard await Self.allowed() else { throw RecordError.notAllowed }
        guard recorder == nil else { throw RecordError.busy }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<String, Error>) in
                do {
                    try start(quality == .music, maxDuration, path)
                    done = c
                } catch {
                    c.resume(throwing: error)
                }
            }
        } onCancel: {
            // The task that waits went: the recording ends.
            Task { @MainActor in AVRecorder.shared.recorder?.stop() }
        }
    }

    static func allowed() async -> Bool {
        #if os(iOS)
        return await withCheckedContinuation { c in AVAudioSession.sharedInstance().requestRecordPermission { c.resume(returning: $0) } }
        #else
        return await AVCaptureDevice.requestAccess(for: .audio)
        #endif
    }

    private func start(_ music: Bool, _ max: TimeInterval, _ path: String) throws {
        #if os(iOS)
        let session = AVAudioSession.sharedInstance()
        category = session.category
        try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker, .allowBluetoothHFP])
        try session.setActive(true)
        #endif
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatMPEG4AAC,
            AVSampleRateKey: music ? 44100 : 22050,
            AVNumberOfChannelsKey: music ? 2 : 1,
            AVEncoderBitRateKey: music ? 128_000 : 64_000,
            AVEncoderAudioQualityKey: AVAudioQuality.high.rawValue,
        ]
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("asuka-audio", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = path.isEmpty ? dir.appendingPathComponent(UUID().uuidString + ".m4a") : URL(fileURLWithPath: path)
        do {
            let r = try AVAudioRecorder(url: url, settings: settings)
            r.delegate = self
            r.isMeteringEnabled = true
            let ok = max > 0 ? r.record(forDuration: max) : r.record()
            guard ok else { throw AsukaNativeError.unsupported("couldn't record") }
            recorder = r
            userPaused = false
            observeInterruptions()
        } catch {
            restore()
            throw error
        }
    }

    func stop() throws { recorder?.stop() } // the delegate answers record

    func pause() throws {
        userPaused = true
        recorder?.pause()
    }

    func resume() throws {
        userPaused = false
        #if os(iOS)
        try? AVAudioSession.sharedInstance().setActive(true)
        #endif
        recorder?.record()
    }

    func status() throws -> Status {
        guard let r = recorder else { return Status() }
        var level = 0.0
        if r.isRecording {
            r.updateMeters()
            let db = Double(r.averagePower(forChannel: 0)) // -160...0
            level = db <= -60 ? 0 : min(max(pow(10, db / 20), 0), 1)
        }
        return Status(level: level, recording: r.isRecording, elapsed: max(r.currentTime, 0))
    }

    func access() throws -> String { Self.access() }

    func requestAccess() async throws -> String {
        if AVCaptureDevice.authorizationStatus(for: .audio) == .notDetermined {
            _ = await AVCaptureDevice.requestAccess(for: .audio) // answers off the main thread
        }
        return Self.access()
    }

    /// The microphone's permission as access says it: "notAsked", "denied"
    /// (or restricted) or "allowed". AVCaptureDevice's is the same one
    /// AVAudioSession asks for on iOS.
    static func access() -> String {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .notDetermined: return "notAsked"
        case .authorized: return "allowed"
        default: return "denied"
        }
    }

    private func observeInterruptions() {
        #if os(iOS)
        interruption = NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
            guard let info = n.userInfo, let raw = info[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: raw) == .ended else { return }
            let options = AVAudioSession.InterruptionOptions(rawValue: info[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0)
            MainActor.assumeIsolated {
                guard let self, !self.userPaused, options.contains(.shouldResume) else { return }
                try? AVAudioSession.sharedInstance().setActive(true)
                self.recorder?.record()
            }
        }
        #endif
    }

    private func restore() {
        #if os(iOS)
        if let o = interruption { NotificationCenter.default.removeObserver(o) }
        interruption = nil
        try? AVAudioSession.sharedInstance().setCategory(category ?? .playback)
        category = nil
        #endif
    }

    private func finish(_ result: Result<String, Error>) {
        let d = done
        done = nil
        recorder = nil
        restore()
        d?.resume(with: result)
    }

    nonisolated func audioRecorderDidFinishRecording(_ r: AVAudioRecorder, successfully ok: Bool) {
        let path = r.url.path
        Task { @MainActor in
            AVRecorder.shared.finish(ok ? .success(path) : .failure(AsukaNativeError.unsupported("the recording failed")))
        }
    }

    nonisolated func audioRecorderEncodeErrorDidOccur(_ r: AVAudioRecorder, error: Error?) {
        let e = error
        Task { @MainActor in AVRecorder.shared.finish(.failure(e ?? AsukaNativeError.unsupported("the recording failed"))) }
    }
}

@MainActor func makeRecorder() throws -> any Recorder { AVRecorder.shared }
