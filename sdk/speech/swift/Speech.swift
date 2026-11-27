// platform/speech on iOS and macOS: AVSpeechSynthesizer. On iOS (and Apple
// TV and Apple Watch), while it
// speaks the audio session plays (with the silent switch on) and ducks
// other apps, unless the app set it up to play itself; after, it goes
// back.
import AVFoundation
import Foundation

@MainActor final class SpeechSynth: NSObject, AVSpeechSynthesizerDelegate {
    static let shared = SpeechSynth()
    private let synth = AVSpeechSynthesizer()
    private var waiting: [ObjectIdentifier: CheckedContinuation<Void, Error>] = [:]
    #if !os(macOS)
    private var session: (AVAudioSession.Category, AVAudioSession.CategoryOptions)? // what it was, if this set it
    #endif

    override init() {
        super.init()
        synth.delegate = self
    }

    func speak(_ text: String, _ o: Options) async throws {
        let u = AVSpeechUtterance(string: text)
        if !o.voice.isEmpty, let v = AVSpeechSynthesisVoice(identifier: o.voice) {
            u.voice = v
        } else if !o.language.isEmpty {
            u.voice = AVSpeechSynthesisVoice(language: o.language)
        }
        u.pitchMultiplier = Float(min(max(o.pitch > 0 ? o.pitch : 1, 0.5), 2))
        let rate = Float(o.rate > 0 ? o.rate : 1) * AVSpeechUtteranceDefaultSpeechRate
        u.rate = min(max(rate, AVSpeechUtteranceMinimumSpeechRate), AVSpeechUtteranceMaximumSpeechRate)
        u.volume = Float(min(max(o.volume, 0), 1))
        let id = ObjectIdentifier(u)
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
                claimSession()
                waiting[id] = c
                synth.speak(u)
            }
        } onCancel: {
            // The task that waits went: it stops speaking.
            Task { @MainActor in SpeechSynth.shared.stop() }
        }
    }

    func stop() {
        // Each queued one ends stopped here: after a pause, the
        // synthesizer reports a stopped utterance as finished, and its
        // delegate's calls for them find nothing after.
        let ws = waiting
        waiting = [:]
        synth.stopSpeaking(at: .immediate)
        for c in ws.values { c.resume(throwing: SpeechError.stopped) }
        releaseSession()
    }

    func pause() { synth.pauseSpeaking(at: .word) }
    func resume() { synth.continueSpeaking() }
    var speaking: Bool { synth.isSpeaking || synth.isPaused || !waiting.isEmpty }

    #if !os(macOS)
    /// Plays through the silent switch, ducking others, while it speaks,
    /// unless the app's session already plays (platform/audio's).
    private func claimSession() {
        let s = AVAudioSession.sharedInstance()
        guard session == nil, s.category != .playback, s.category != .playAndRecord else { return }
        session = (s.category, s.categoryOptions)
        try? s.setCategory(.playback, mode: .spokenAudio, options: [.duckOthers, .interruptSpokenAudioAndMixWithOthers])
        try? s.setActive(true)
    }

    private func releaseSession() {
        guard let (category, options) = session, waiting.isEmpty else { return }
        session = nil
        let s = AVAudioSession.sharedInstance()
        try? s.setActive(false, options: .notifyOthersOnDeactivation)
        try? s.setCategory(category, options: options)
    }
    #else
    private func claimSession() {}
    private func releaseSession() {}
    #endif

    nonisolated func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) {
        let id = ObjectIdentifier(u)
        Task { @MainActor in SpeechSynth.shared.finish(id, nil) }
    }

    nonisolated func speechSynthesizer(_ s: AVSpeechSynthesizer, didCancel u: AVSpeechUtterance) {
        let id = ObjectIdentifier(u)
        Task { @MainActor in SpeechSynth.shared.finish(id, SpeechError.stopped) }
    }

    private func finish(_ id: ObjectIdentifier, _ err: Error?) {
        if let c = waiting.removeValue(forKey: id) {
            if let err { c.resume(throwing: err) } else { c.resume() }
        }
        releaseSession()
    }

    nonisolated static func voices() -> [Voice] {
        AVSpeechSynthesisVoice.speechVoices().map { v in
            var q = Quality.standard
            switch v.quality {
            case .enhanced: q = .enhanced
            case .premium: q = .premium
            default: break
            }
            return Voice(id: v.identifier, name: v.name, language: v.language, quality: q, network: false)
        }
    }
}

/// The package's Synthesizer: its calls come on the main thread (the
/// async ones on a task of their own), and go to the main actor's.
final class AVSynthesizer: Synthesizer {
    func speak(text: String, options: Options) async throws {
        try await SpeechSynth.shared.speak(text, options)
    }

    func stop() throws { MainActor.assumeIsolated { SpeechSynth.shared.stop() } }
    func pause() throws { MainActor.assumeIsolated { SpeechSynth.shared.pause() } }
    func resume() throws { MainActor.assumeIsolated { SpeechSynth.shared.resume() } }
    func speaking() throws -> Bool { MainActor.assumeIsolated { SpeechSynth.shared.speaking } }
    func voices() async throws -> [Voice] { SpeechSynth.voices() }
}

func makeSynthesizer() throws -> any Synthesizer { AVSynthesizer() }
