package ops

import (
	"fmt"
	"math"
)

// Environment is what a host reports about a root view: its size, safe
// area insets, the keyboard, and display and text settings. Sizes are in
// points.
//
// Wire format: f32 width, height, safe top, right, bottom, left,
// keyboard, scale, font scale; u8 flags (1 dark, 2 reduce motion, bits
// 2-4 the Platform, 32 a screen reader, 64 a TV, 128 a watch); u8 size classes (bits 0-1 the width's, 2-3 the
// height's); string locale; string time zone; string languages; u8
// first weekday; u8 hour cycle; u8 Posture; f32 the fold's x, y, width,
// height; u8 1 if the app can open windows of its own there.
type Environment struct {
	Width, Height                            float32
	SafeTop, SafeRight, SafeBottom, SafeLeft float32
	Keyboard                                 float32 // how much of the root's bottom the keyboard covers
	Scale                                    float32 // pixels per point
	FontScale                                float32 // Dynamic Type / Android font scale; 1 is the default
	Dark                                     bool
	ReduceMotion                             bool // the user asked for less motion (iOS's Reduce Motion; Android's animations off)
	ScreenReader                             bool // a screen reader is on (VoiceOver, TalkBack, Narrator)
	TV                                       bool // a TV (Apple TV, Android TV), driven by a remote's focus rather than touch
	Watch                                    bool // a watch (Apple Watch): a small round-cornered screen, the Digital Crown
	Platform                                 Platform
	// WidthClass and HeightClass are the platform's size classes for
	// the window: UIKit's (compact; regular, reported as SizeClassExpanded)
	// on iOS, Material's window size classes on Android. A host that
	// leaves them unspecified gets Material's, from the size
	// (WidthClassOf, HeightClassOf).
	WidthClass, HeightClass SizeClass
	// Locale is the locale of the user's regional formats, as the system
	// names it: a BCP 47 tag, or a POSIX one with keywords (en_US@rg=gbzzzz).
	Locale   string
	TimeZone string // IANA, e.g. Asia/Shanghai: the core's time.Local
	// Languages are the languages the user reads, most preferred first,
	// BCP 47 tags joined by commas: the app's own language first when it has
	// one (the system's per-app language, or what SaveLocale kept).
	Languages string
	// FirstWeekday is the day the user's weeks start on, 1 Sunday to 7
	// Saturday, and HourCycle 12 or 24; 0 when the system doesn't say.
	FirstWeekday, HourCycle uint8
	// Posture is how a foldable is held (Android's FoldingFeature):
	// flat, or half open as a laptop (tabletop) or a book.
	Posture Posture
	// FoldX, FoldY, FoldWidth and FoldHeight are where a foldable's fold
	// or hinge crosses the window, in points from its top left (a hinge
	// between two screens is as wide as the gap); all 0: none does.
	FoldX, FoldY, FoldWidth, FoldHeight float32
	// MultipleWindows: a Window opens a window of its own here (an
	// iPad's scene; a desktop's window, a panel on the web). A host
	// that doesn't say has it on a desktop and the web (the core fills
	// it in for their platforms), not on a phone.
	MultipleWindows bool
}

// Posture is how a foldable device is held, as Jetpack WindowManager
// tells it: flat (any device without a fold, too), or half open with its
// fold across the window (tabletop: the screen's top half stands up, as
// a laptop's) or down it (book: two pages side by side).
type Posture uint8

const (
	PostureFlat Posture = iota
	PostureTabletop
	PostureBook
)

func (p Posture) String() string {
	switch p {
	case PostureTabletop:
		return "tabletop"
	case PostureBook:
		return "book"
	}
	return "flat"
}

// Platform is the system a host runs on.
type Platform uint8

const (
	PlatformUnknown Platform = iota
	PlatformIOS
	PlatformAndroid
	PlatformMacOS
	PlatformWindows
	PlatformWeb   // a browser (platform/web: the DOM)
	PlatformLinux // GTK 4 and libadwaita (platform/linux)
)

func (p Platform) String() string {
	switch p {
	case PlatformIOS:
		return "iOS"
	case PlatformAndroid:
		return "Android"
	case PlatformMacOS:
		return "macOS"
	case PlatformWindows:
		return "Windows"
	case PlatformWeb:
		return "web"
	case PlatformLinux:
		return "Linux"
	}
	return "unknown"
}

// SizeClass is how much room a window gives an app along one axis, as
// the platform classes it: what to show side by side (a sidebar, a list
// and its detail) rather than one after the other.
type SizeClass uint8

const (
	SizeClassUnspecified SizeClass = iota
	SizeClassCompact               // a phone (in portrait, for the width); a narrow split screen
	SizeClassMedium                // a tablet in portrait, a foldable open (Android; iOS has none)
	SizeClassExpanded              // a tablet in landscape, a desktop window; iOS's regular
)

func (c SizeClass) String() string {
	switch c {
	case SizeClassCompact:
		return "compact"
	case SizeClassMedium:
		return "medium"
	case SizeClassExpanded:
		return "expanded"
	}
	return "unspecified"
}

// WidthClassOf is Material 3's window width class for a width in points
// (dp): compact under 600, medium under 840, else expanded.
func WidthClassOf(w float32) SizeClass {
	switch {
	case w < 600:
		return SizeClassCompact
	case w < 840:
		return SizeClassMedium
	}
	return SizeClassExpanded
}

// HeightClassOf is Material 3's window height class for a height in
// points (dp): compact under 480, medium under 900, else expanded.
func HeightClassOf(h float32) SizeClass {
	switch {
	case h < 480:
		return SizeClassCompact
	case h < 900:
		return SizeClassMedium
	}
	return SizeClassExpanded
}

// Gesture is a continuous gesture's state, in points relative to the
// view (velocity in points per second).
//
// Wire format: u8 state; f32 x, y, translation x, y, velocity x, y,
// scale, rotation.
type Gesture struct {
	State                      GestureState
	X, Y                       float32
	TranslationX, TranslationY float32
	VelocityX, VelocityY       float32
	Scale                      float32
	Rotation                   float32 // a rotation's, in radians clockwise since it began
}

// Event is something a host reports.
type Event struct {
	ID    NodeID
	Kind  EventKind
	Bool  bool
	Int   int64
	Float float64
	X, Y  float64
	Str   string
	Bytes []byte
	Env   Environment
	G     Gesture
}

func (e Event) String() string {
	return fmt.Sprintf("%s %d", e.Kind, e.ID)
}

// AppendEvent appends the encoding of e (hosts do this in their own
// language; Go does it for tests and transports).
func AppendEvent(buf []byte, e *Event) []byte {
	buf = le.AppendUint32(buf, uint32(e.ID))
	buf = le.AppendUint16(buf, uint16(e.Kind))
	switch EventPayload(e.Kind) {
	case PBool:
		b := byte(0)
		if e.Bool {
			b = 1
		}
		buf = append(buf, b)
	case PInt:
		buf = le.AppendUint64(buf, uint64(e.Int))
	case PFloat:
		buf = le.AppendUint64(buf, math.Float64bits(e.Float))
	case PString:
		buf = le.AppendUint32(buf, uint32(len(e.Str)))
		buf = append(buf, e.Str...)
	case PPoint:
		buf = le.AppendUint64(buf, math.Float64bits(e.X))
		buf = le.AppendUint64(buf, math.Float64bits(e.Y))
	case PEnvironment:
		v := &e.Env
		for _, f := range [...]float32{v.Width, v.Height, v.SafeTop, v.SafeRight, v.SafeBottom, v.SafeLeft, v.Keyboard, v.Scale, v.FontScale} {
			buf = le.AppendUint32(buf, math.Float32bits(f))
		}
		d := byte(v.Platform&7) << 2 // flags: 1 dark, 2 reduce motion, bits 2-4 the platform
		if v.Dark {
			d |= 1
		}
		if v.ReduceMotion {
			d |= 2
		}
		if v.ScreenReader {
			d |= 32
		}
		if v.TV {
			d |= 64
		}
		if v.Watch {
			d |= 128
		}
		buf = append(buf, d, byte(v.WidthClass&3)|byte(v.HeightClass&3)<<2)
		buf = le.AppendUint32(buf, uint32(len(v.Locale)))
		buf = append(buf, v.Locale...)
		buf = le.AppendUint32(buf, uint32(len(v.TimeZone)))
		buf = append(buf, v.TimeZone...)
		buf = le.AppendUint32(buf, uint32(len(v.Languages)))
		buf = append(buf, v.Languages...)
		buf = append(buf, v.FirstWeekday, v.HourCycle, byte(v.Posture))
		for _, f := range [...]float32{v.FoldX, v.FoldY, v.FoldWidth, v.FoldHeight} {
			buf = le.AppendUint32(buf, math.Float32bits(f))
		}
		mw := byte(0)
		if v.MultipleWindows {
			mw = 1
		}
		buf = append(buf, mw)
	case PGesture:
		g := &e.G
		buf = append(buf, byte(g.State))
		for _, f := range [...]float32{g.X, g.Y, g.TranslationX, g.TranslationY, g.VelocityX, g.VelocityY, g.Scale, g.Rotation} {
			buf = le.AppendUint32(buf, math.Float32bits(f))
		}
	case PBytes:
		buf = le.AppendUint32(buf, uint32(len(e.Bytes)))
		buf = append(buf, e.Bytes...)
	}
	return buf
}

// DecodeEvent decodes one event.
func DecodeEvent(data []byte) (Event, error) {
	r := &reader{b: data}
	e := Event{ID: NodeID(r.u32()), Kind: EventKind(r.u16())}
	switch EventPayload(e.Kind) {
	case PNone:
	case PBool:
		e.Bool = r.u8() != 0
	case PInt:
		e.Int = int64(r.u64())
	case PFloat:
		e.Float = r.f64()
	case PString:
		e.Str = string(r.bytes())
	case PPoint:
		e.X, e.Y = r.f64(), r.f64()
	case PEnvironment:
		v := &e.Env
		for _, f := range [...]*float32{&v.Width, &v.Height, &v.SafeTop, &v.SafeRight, &v.SafeBottom, &v.SafeLeft, &v.Keyboard, &v.Scale, &v.FontScale} {
			*f = r.f32()
		}
		f := r.u8()
		v.Dark, v.ReduceMotion, v.ScreenReader, v.TV, v.Watch, v.Platform = f&1 != 0, f&2 != 0, f&32 != 0, f&64 != 0, f&128 != 0, Platform(f>>2&7)
		c := r.u8()
		v.WidthClass, v.HeightClass = SizeClass(c&3), SizeClass(c>>2&3)
		v.Locale = string(r.bytes())
		v.TimeZone = string(r.bytes())
		v.Languages = string(r.bytes())
		v.FirstWeekday, v.HourCycle, v.Posture = r.u8(), r.u8(), Posture(r.u8())
		for _, f := range [...]*float32{&v.FoldX, &v.FoldY, &v.FoldWidth, &v.FoldHeight} {
			*f = r.f32()
		}
		v.MultipleWindows = r.u8() != 0
	case PGesture:
		g := &e.G
		g.State = GestureState(r.u8())
		for _, f := range [...]*float32{&g.X, &g.Y, &g.TranslationX, &g.TranslationY, &g.VelocityX, &g.VelocityY, &g.Scale, &g.Rotation} {
			*f = r.f32()
		}
	case PBytes:
		e.Bytes = append([]byte(nil), r.bytes()...)
	default:
		return e, fmt.Errorf("ops: unknown event kind %d", e.Kind)
	}
	if r.err == nil && len(r.b) != 0 {
		return e, fmt.Errorf("ops: %d bytes after event", len(r.b))
	}
	return e, r.err
}
