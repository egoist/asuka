package ops

import "math"

// A Video's playback, as its host reports it: the VideoState and
// VideoTracks events' payloads.

// VideoStateFlags are a VideoState's bits.
type VideoStateFlags uint8

const (
	VideoReady      VideoStateFlags = 1 << iota // its source loaded and can play
	VideoPlaying                                // it plays, or would but for buffering
	VideoBuffering                              // waiting for data
	VideoEnded                                  // at its end, not looping
	VideoPiP                                    // in picture in picture
	VideoFullScreen                             // in the platform's full-screen player
)

// VideoState is a VideoState event's payload.
type VideoState struct {
	Flags         VideoStateFlags
	Time          float64 // seconds from the start
	Duration      float64 // seconds; NaN: not known yet, +Inf: a live stream
	Rate          float32 // chosen (1 normal), kept while paused
	Width, Height float32 // the natural size; 0: not known, or sound alone
}

// AppendVideoState appends a VideoState event's payload.
func AppendVideoState(b []byte, s VideoState) []byte {
	b = append(b, byte(s.Flags))
	b = le.AppendUint64(b, math.Float64bits(s.Time))
	b = le.AppendUint64(b, math.Float64bits(s.Duration))
	b = le.AppendUint32(b, math.Float32bits(s.Rate))
	b = le.AppendUint32(b, math.Float32bits(s.Width))
	return le.AppendUint32(b, math.Float32bits(s.Height))
}

// DecodeVideoState reads what AppendVideoState writes.
func DecodeVideoState(b []byte) (VideoState, bool) {
	r := &reader{b: b}
	var s VideoState
	s.Flags = VideoStateFlags(r.u8())
	s.Time = r.f64()
	s.Duration = r.f64()
	s.Rate = r.f32()
	s.Width = r.f32()
	s.Height = r.f32()
	return s, r.err == nil
}

// VideoTrackKind is a track's kind in VideoTracks.
type VideoTrackKind uint8

const (
	VideoTrackSubtitle VideoTrackKind = iota
	VideoTrackAudio
)

// VideoTrack is one of a VideoTracks event's tracks.
type VideoTrack struct {
	Kind     VideoTrackKind
	Selected bool
	ID       string // the platform's, stable while the source is
	Label    string
	Language string // BCP 47; empty: not known
}

// AppendVideoTracks appends a VideoTracks event's payload.
func AppendVideoTracks(b []byte, ts []VideoTrack) []byte {
	b = le.AppendUint32(b, uint32(len(ts)))
	for _, t := range ts {
		sel := byte(0)
		if t.Selected {
			sel = 1
		}
		b = append(b, byte(t.Kind), sel)
		b = appendStr(b, t.ID)
		b = appendStr(b, t.Label)
		b = appendStr(b, t.Language)
	}
	return b
}

// DecodeVideoTracks reads what AppendVideoTracks writes.
func DecodeVideoTracks(b []byte) ([]VideoTrack, bool) {
	r := &reader{b: b}
	n := r.u32()
	var ts []VideoTrack
	for i := uint32(0); i < n && r.err == nil; i++ {
		var t VideoTrack
		t.Kind = VideoTrackKind(r.u8())
		t.Selected = r.u8() != 0
		t.ID = string(r.bytes())
		t.Label = string(r.bytes())
		t.Language = string(r.bytes())
		ts = append(ts, t)
	}
	return ts, r.err == nil
}
