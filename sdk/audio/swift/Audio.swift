// platform/audio on iOS and macOS: an AVPlayer a sound. On iOS it plays with
// the silent switch on (the playback category), as a music or game app's
// sound does.
import Asuka
import AVFoundation
import Foundation

@MainActor final class AVSound: Sound {
    private var player: AVPlayer?
    private var end: NSObjectProtocol?
    private var loop = false
    private var once = false
    private var ended = false
    private var rate: Float = 1

    /// The sounds playing once, kept until they end.
    private static var playingOnce: [ObjectIdentifier: AVSound] = [:]

    func open(source: String) throws {
        close()
        guard let url = AsukaAssets.url(source) else { throw AsukaNativeError.unsupported("audio: no file \(source)") }
        let item = AVPlayerItem(url: url)
        item.audioTimePitchAlgorithm = .timeDomain // keeps a voice's pitch at any rate
        let p = AVPlayer(playerItem: item)
        player = p
        end = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reachedEnd() }
        }
    }

    private func reachedEnd() {
        guard let p = player else { return }
        if loop {
            p.seek(to: .zero)
            p.play()
        } else if once {
            close()
            AVSound.playingOnce[ObjectIdentifier(self)] = nil
        } else {
            ended = true
        }
    }

    func play() throws {
        #if os(iOS) || os(tvOS)
        // Heard with the silent switch on, as a music or game app's sound.
        try? AVAudioSession.sharedInstance().setCategory(.playback)
        try? AVAudioSession.sharedInstance().setActive(true)
        #endif
        guard let p = player else { return }
        if ended {
            ended = false
            p.seek(to: .zero, toleranceBefore: .zero, toleranceAfter: .zero)
        }
        p.playImmediately(atRate: rate)
    }

    func playOnce() throws {
        once = true
        AVSound.playingOnce[ObjectIdentifier(self)] = self
        try play()
    }

    func pause() throws { player?.pause() }

    func seek(seconds: Double) throws {
        ended = false
        player?.seek(to: CMTime(seconds: seconds, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
    }

    func setVolume(volume: Double) throws { player?.volume = Float(volume) }

    func setRate(rate: Double) throws {
        self.rate = Float(rate)
        if let p = player, p.rate > 0 { p.rate = self.rate }
    }

    func setLoop(on: Bool) throws { loop = on }

    func status() throws -> Status {
        guard let p = player else { return Status() }
        let position = p.currentTime().seconds
        let duration = p.currentItem?.duration.seconds ?? 0
        return Status(position: position.isFinite ? position : 0, duration: duration.isFinite ? duration : 0,
                      playing: p.timeControlStatus == .playing, ended: ended)
    }

    func close() {
        player?.pause()
        player = nil
        if let e = end { NotificationCenter.default.removeObserver(e) }
        end = nil
        ended = false
    }

    deinit {
        // Let go without close: playing once, it's kept (playingOnce).
        if let e = end { NotificationCenter.default.removeObserver(e) }
    }
}

@MainActor func makeSound() throws -> any Sound { AVSound() }
