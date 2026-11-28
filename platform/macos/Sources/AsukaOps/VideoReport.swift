// A Video's reports (ops/video.go): the VideoState and VideoTracks
// events' payloads. Written by hand, beside the generated decoder.

/// A VideoState's bits.
public struct AsukaVideoFlags: OptionSet, Equatable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }
    /// Its source loaded and can play.
    public static let ready = AsukaVideoFlags(rawValue: 1)
    /// It plays, or would but for buffering.
    public static let playing = AsukaVideoFlags(rawValue: 2)
    public static let buffering = AsukaVideoFlags(rawValue: 4)
    /// At its end, not looping.
    public static let ended = AsukaVideoFlags(rawValue: 8)
    public static let pictureInPicture = AsukaVideoFlags(rawValue: 16)
    public static let fullScreen = AsukaVideoFlags(rawValue: 32)
}

/// What a Video's player reports of its playback.
public struct AsukaVideoState: Equatable {
    public var flags: AsukaVideoFlags = []
    /// Seconds from the start.
    public var time: Double = 0
    /// Seconds; NaN: not known yet, +infinity: a live stream.
    public var duration: Double = .nan
    /// The rate chosen (1 normal), kept while paused.
    public var rate: Float = 1
    /// The natural size; 0: not known, or sound alone.
    public var width: Float = 0, height: Float = 0

    public init(flags: AsukaVideoFlags = [], time: Double = 0, duration: Double = .nan, rate: Float = 1, width: Float = 0, height: Float = 0) {
        self.flags = flags
        self.time = time
        self.duration = duration
        self.rate = rate
        self.width = width
        self.height = height
    }

    /// All but the time is the same (a NaN duration is the same as one).
    public func sameBut(time other: AsukaVideoState) -> Bool {
        flags == other.flags && rate == other.rate && width == other.width && height == other.height
            && (duration == other.duration || duration.isNaN && other.duration.isNaN)
    }

    /// VideoState's payload (ops.AppendVideoState).
    public var bytes: [UInt8] {
        var w = AsukaWriter()
        w.u8(flags.rawValue)
        w.f64(time)
        w.f64(duration)
        w.f32(rate)
        w.f32(width)
        w.f32(height)
        return w.bytes
    }
}

/// One of a Video's tracks, as VideoTracks reports it.
public struct AsukaVideoTrack: Equatable {
    /// A subtitle's (false: an audio track's).
    public var subtitle: Bool
    public var selected: Bool
    /// The host's, stable while the source is.
    public var id: String
    public var label: String
    /// BCP 47; empty: not known.
    public var language: String

    public init(subtitle: Bool, selected: Bool, id: String, label: String, language: String) {
        self.subtitle = subtitle
        self.selected = selected
        self.id = id
        self.label = label
        self.language = language
    }

    /// VideoTracks' payload (ops.AppendVideoTracks).
    public static func bytes(_ ts: [AsukaVideoTrack]) -> [UInt8] {
        var w = AsukaWriter()
        w.u32(UInt32(ts.count))
        for t in ts {
            w.u8(t.subtitle ? 0 : 1)
            w.u8(t.selected ? 1 : 0)
            w.string(t.id)
            w.string(t.label)
            w.string(t.language)
        }
        return w.bytes
    }
}
