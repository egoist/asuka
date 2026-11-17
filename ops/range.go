package ops

import "math"

// Range is the A11yRange prop: a slider's, progress bar's, meter's or
// spin button's range, for screen readers.
type Range struct {
	Min, Max, Now float64
}

// AppendRange appends the A11yRange prop's encoding of r to b: f64 min,
// f64 max, f64 now.
func AppendRange(b []byte, r Range) []byte {
	b = le.AppendUint64(b, math.Float64bits(r.Min))
	b = le.AppendUint64(b, math.Float64bits(r.Max))
	return le.AppendUint64(b, math.Float64bits(r.Now))
}

// DecodeRange reads an A11yRange prop: false when it's empty (none) or
// cut short.
func DecodeRange(b []byte) (Range, bool) {
	if len(b) < 24 {
		return Range{}, false
	}
	return Range{
		Min: math.Float64frombits(le.Uint64(b)),
		Max: math.Float64frombits(le.Uint64(b[8:])),
		Now: math.Float64frombits(le.Uint64(b[16:])),
	}, true
}

// AnchorFrame is the AnchorFrame event's payload: where an Overlay's
// anchor is in the overlay's root, and whether any of it is visible.
type AnchorFrame struct {
	X, Y, Width, Height float32
	Visible             bool
}

// AppendAnchorFrame appends the AnchorFrame event's encoding of f to b:
// f32 x, y, width, height, then u8 visible.
func AppendAnchorFrame(b []byte, f AnchorFrame) []byte {
	for _, v := range []float32{f.X, f.Y, f.Width, f.Height} {
		b = le.AppendUint32(b, math.Float32bits(v))
	}
	if f.Visible {
		return append(b, 1)
	}
	return append(b, 0)
}

// DecodeAnchorFrame reads an AnchorFrame event: false when it's cut
// short.
func DecodeAnchorFrame(b []byte) (AnchorFrame, bool) {
	if len(b) < 17 {
		return AnchorFrame{}, false
	}
	f := func(i int) float32 { return math.Float32frombits(le.Uint32(b[i:])) }
	return AnchorFrame{X: f(0), Y: f(4), Width: f(8), Height: f(12), Visible: b[16] != 0}, true
}
