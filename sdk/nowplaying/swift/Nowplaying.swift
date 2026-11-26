// platform/nowplaying on iOS and macOS: MPNowPlayingInfoCenter, which the
// lock screen, Control Center, the Dynamic Island, CarPlay and the Mac's
// menu bar show, and MPRemoteCommandCenter, whose commands they, AirPods,
// headsets and the keyboard's media keys send. iOS shows the app whose
// audio session plays: the session is put in the playback category (the
// package gives the app the audio background mode, so it plays on with
// the screen locked). Apple TV's Now Playing (Control Center, the Siri
// Remote's buttons) is the same center's.
import Asuka
import AVFoundation
import Foundation
import MediaPlayer
#if canImport(UIKit)
import UIKit
typealias PlatformImage = UIImage
#else
import AppKit
typealias PlatformImage = NSImage
#endif

@MainActor final class MediaCenter: Center {
    static let shared = MediaCenter()

    private var info: Info?
    private var targets = false
    private var artSource = ""
    private var artwork: MPMediaItemArtwork?
    private var watchers: [UUID: (Command, Double) -> Void] = [:]

    private func has(_ i: Info, _ c: Command) -> Bool { i.commands & (1 << c.rawValue) != 0 }

    private func remoteCommands() -> [(MPRemoteCommand, Command)] {
        let c = MPRemoteCommandCenter.shared()
        return [
            (c.playCommand, .play), (c.pauseCommand, .pause), (c.togglePlayPauseCommand, .togglePlayPause),
            (c.stopCommand, .stop), (c.nextTrackCommand, .next), (c.previousTrackCommand, .previous),
            (c.changePlaybackPositionCommand, .seek), (c.skipForwardCommand, .skipForward),
            (c.skipBackwardCommand, .skipBackward),
        ]
    }

    private func command(_ c: Command, _ value: Double) -> MPRemoteCommandHandlerStatus {
        guard let info else { return .noActionableNowPlayingItem }
        guard !watchers.isEmpty, has(info, c) else { return .commandFailed }
        for w in watchers.values { w(c, value) }
        return .success
    }

    func set(info new: Info) throws {
        info = new
        if !targets {
            targets = true
            #if os(iOS) || os(tvOS)
            // Heard with the silent switch on and the screen locked; iOS
            // (and tvOS) shows the Now Playing of the app whose session
            // plays.
            try? AVAudioSession.sharedInstance().setCategory(.playback)
            UIApplication.shared.beginReceivingRemoteControlEvents()
            #endif
            for (cmd, c) in remoteCommands() {
                cmd.addTarget { [weak self] event in
                    var v = 0.0
                    if let e = event as? MPChangePlaybackPositionCommandEvent { v = e.positionTime }
                    if let e = event as? MPSkipIntervalCommandEvent { v = e.interval }
                    // MediaPlayer calls it on the main thread; should it not,
                    // the command goes there.
                    guard Thread.isMainThread else {
                        DispatchQueue.main.async { MainActor.assumeIsolated { _ = self?.command(c, v) } }
                        return .success
                    }
                    return MainActor.assumeIsolated { self?.command(c, v) ?? .commandFailed }
                }
            }
        }
        for (cmd, c) in remoteCommands() {
            cmd.isEnabled = has(new, c)
        }
        if new.skip > 0 {
            let c = MPRemoteCommandCenter.shared()
            c.skipForwardCommand.preferredIntervals = [NSNumber(value: new.skip)]
            c.skipBackwardCommand.preferredIntervals = [NSNumber(value: new.skip)]
        }
        loadArtwork(new.artwork)
        publish(new)
    }

    func clear() throws {
        info = nil
        for (cmd, _) in remoteCommands() { cmd.isEnabled = false }
        artSource = ""
        artwork = nil
        MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
        #if os(macOS)
        MPNowPlayingInfoCenter.default().playbackState = .stopped
        #endif
    }

    nonisolated func watch(command: @escaping (Command, Double) -> Void) async throws {
        let id = UUID()
        await MainActor.run { MediaCenter.shared.watchers[id] = command }
        defer { Task { @MainActor in MediaCenter.shared.watchers[id] = nil } }
        // Until the task is cancelled.
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    private func publish(_ info: Info) {
        var d: [String: Any] = [
            MPNowPlayingInfoPropertyMediaType: MPNowPlayingInfoMediaType.audio.rawValue,
            MPNowPlayingInfoPropertyElapsedPlaybackTime: info.position,
            MPNowPlayingInfoPropertyPlaybackRate: info.state == .playing ? info.rate : 0.0,
            MPNowPlayingInfoPropertyDefaultPlaybackRate: 1.0,
        ]
        if !info.title.isEmpty { d[MPMediaItemPropertyTitle] = info.title }
        if !info.artist.isEmpty { d[MPMediaItemPropertyArtist] = info.artist }
        if !info.album.isEmpty { d[MPMediaItemPropertyAlbumTitle] = info.album }
        if info.duration > 0 { d[MPMediaItemPropertyPlaybackDuration] = info.duration }
        if let artwork { d[MPMediaItemPropertyArtwork] = artwork }
        MPNowPlayingInfoCenter.default().nowPlayingInfo = d
        #if os(macOS)
        // The Mac's Now Playing shows what playbackState says.
        MPNowPlayingInfoCenter.default().playbackState = info.state == .playing ? .playing : info.state == .stopped ? .stopped : .paused
        #endif
    }

    /// Loads the artwork (an asset, a file, an http(s) URL) off the main
    /// thread, and shows it once it's in.
    private func loadArtwork(_ source: String) {
        guard source != artSource else { return }
        artSource = source
        artwork = nil
        guard !source.isEmpty, let url = AsukaAssets.url(source) else { return }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            guard let data else { return }
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    let c = MediaCenter.shared
                    guard c.artSource == source, let img = PlatformImage(data: data) else { return }
                    c.artwork = MPMediaItemArtwork(boundsSize: img.size) { _ in img }
                    if let info = c.info { c.publish(info) }
                }
            }
        }.resume()
    }
}

@MainActor func makeCenter() throws -> any Center { MediaCenter.shared }
