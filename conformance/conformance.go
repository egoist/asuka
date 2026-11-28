// Package conformance checks that every host draws a Style the same
// way: a background's edges, rounded corners, borders, opacity, clips,
// shadows, gradients and transforms. Tests elsewhere check the ops the
// core sends and the views a host makes; these check the pixels.
//
// Sheet lays the cases out between two registration markers, a page of
// them at a time (PerPage at most, so a page fits a phone's screen); a
// host's runner shows each page, captures the screen as an image, and
// Check finds the page in the capture, measures each of its cases and
// reports what's off. The
// cases assert measured quantities with tolerances (where an edge is to
// the device pixel, a shadow's Gaussian σ, a color's mix), not pixels
// against a reference, so one set of cases holds for every host at any
// scale; the platforms draw differently, and that's allowed.
//
// The runners:
//
//	go test ./platform/macos/mactest -run Conformance   # macOS, on the Mac player
//	go test -tags e2e ./e2e -run Conformance            # iOS, Android and the web (headless Chrome, at 1×, 1.5× and 2×)
//	ASUKA_CONFORMANCE_OUT=/tmp/c go test ...             # also write each capture, as <host>-<page>.png
//
// Its app (conformance/app, App's source, which go generate writes)
// shows it on any platform, a page at a time, a button for each
// (PageButton: what a runner taps):
//
//	go run ./cmd/asuka dev --platform ios conformance/app
package conformance

import (
	"bytes"
	"fmt"
	"image"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

//go:generate go run ./gen

// Feature is something a case relies on that a host may document it
// lacks; its cases are skipped there, with the reason, rather than fail.
type Feature string

const (
	// ShadowBlur: a shadow's blur is Style.Shadow's Radius (2σ, as CSS
	// and SwiftUI mean it) and its offset is Style.Shadow's X and Y.
	ShadowBlur Feature = "shadow blur and offset"
	// Transform: Style.Transform.
	Transform Feature = "transforms"
	// GroupOpacity: Style.Opacity fades a view and its children as one
	// picture, so overlapping children don't show through each other.
	GroupOpacity Feature = "group opacity"
)

// unsupported is what each host documents it lacks (DESIGN.md, Style's
// comments), and why.
var unsupported = map[string]map[Feature]string{
	"android": {
		ShadowBlur: "Android draws its elevation shadow: the radius is the elevation, and the light sets the offset",
	},
}

// skipped is why host skips c: a feature it lacks. Hosts are "ios",
// "android", "macos", "windows" and "web".
func skipped(host string, c Case) string {
	for _, f := range c.Needs {
		if why, ok := unsupported[host][f]; ok {
			return why
		}
	}
	return ""
}

// The sheet's geometry, in points.
const (
	Columns = 3
	Cell    = 100 // a case's square
	marker  = 8   // the registration squares' side
	// PerPage is how many cases a page of the sheet shows at most: five
	// rows, which fit a phone's screen in portrait and the macOS host's
	// default window.
	PerPage = 5 * Columns
)

// Magenta is the registration markers' color, which no case uses.
const Magenta = "#ff00ff"

// swatches are the cases' colors, each a marker-sized square in the
// sheet's top margin, right of the top left marker, a marker apart: what
// a host's capture shows for each, solid. A host blends in its own color
// space (the Mac's window server in the display's, Display P3 on its own
// screen), and a mix is measured between the colors as captured, in that
// space: half-transparent red is half way from the white to the red the
// capture has, which in sRGB it needn't be.
var swatches = []struct {
	hex string
	rgb RGB
}{{cRed, red}, {cBlue, blue}, {cYellow, yellow}}

// swatchX is swatch i's left, in the sheet's points.
func swatchX(i int) float32 { return 2*marker + float32(i)*2*marker }

// pageMarkX is the left of the page's mark j, in the sheet's points:
// page p shows p+1 black squares in the top margin, right of the
// swatches, at their cadence, so a capture says which page it shows.
func pageMarkX(j int) float32 { return swatchX(len(swatches) + j) }

// Pages is how many pages the sheet has.
func Pages() int { return (len(Cases) + PerPage - 1) / PerPage }

// pageCases are page p's cases.
func pageCases(p int) []Case { return Cases[p*PerPage : min((p+1)*PerPage, len(Cases))] }

// PageButton is the title of the app's button that shows page p: what a
// runner taps (App).
func PageButton(p int) string { return fmt.Sprintf("Page %d", p+1) }

// SheetSize is page p's size in points: every page is as wide, as tall
// as its rows.
func SheetSize(p int) (w, h float32) {
	rows := (len(pageCases(p)) + Columns - 1) / Columns
	return 2*marker + Columns*Cell, 2*marker + float32(rows)*Cell
}

// cellOrigin is the top left of a page's case i in the page, in points.
func cellOrigin(i int) (x, y float32) {
	return marker + float32(i%Columns)*Cell, marker + float32(i/Columns)*Cell
}

// Sheet is the sheet as Asuka's ConformanceSheet component, which shows
// one page, its page argument: each of the page's cases in its cell, on
// white, between a magenta square at the top left and one at the bottom
// right, and the swatches and the page's marks in the top margin. Show
// it anywhere on screen, at its own size: Check finds it in the capture.
func Sheet() string {
	var b strings.Builder
	b.WriteString("// The conformance sheet (package conformance's cases), a page at a time.\n")
	b.WriteString("component ConformanceSheet(page: int) {\n")
	for p := range Pages() {
		w, h := SheetSize(p)
		if p == 0 {
			fmt.Fprintf(&b, "  if page == %d {\n", p)
		} else {
			fmt.Fprintf(&b, "  } else if page == %d {\n", p)
		}
		fmt.Fprintf(&b, "    View {\n      width: %g; height: %g; background: #ffffff; alignSelf: .center\n", w, h)
		fmt.Fprintf(&b, "      %s\n", box(0, 0, marker, marker, "background: "+Magenta))
		fmt.Fprintf(&b, "      %s\n", box(w-marker, h-marker, marker, marker, "background: "+Magenta))
		for i, s := range swatches {
			fmt.Fprintf(&b, "      %s\n", box(swatchX(i), 0, marker, marker, "background: "+s.hex))
		}
		for j := range p + 1 {
			fmt.Fprintf(&b, "      %s\n", box(pageMarkX(j), 0, marker, marker, "background: "+cBlack))
		}
		for i, c := range pageCases(p) {
			x, y := cellOrigin(i)
			// A View with only layout props makes no native view: the case's
			// views go straight into the sheet, which its white shows under.
			fmt.Fprintf(&b, "      // %s\n      %s\n", c.Name, box(x, y, Cell, Cell, "", c.Scene))
		}
		b.WriteString("    }\n")
	}
	b.WriteString("  }\n}\n")
	return b.String()
}

// App is an Asuka app that shows the sheet, page 1 first, and a button
// for each page (PageButton) above it: conformance/app's app.asuka.
func App() string {
	var buttons strings.Builder
	for p := range Pages() {
		fmt.Fprintf(&buttons, "      Button(%q) { on press { page = %d } }\n", PageButton(p), p)
	}
	return Sheet() + `
component Conformance {
  state page = 0

  SafeArea {
    justify: .center; align: .center; gap: 8
    Row {
      gap: 8
` + buttons.String() + `    }
    ConformanceSheet(page: page)
  }
}

app { Conformance() }
`
}

// Result is one case's outcome on a capture.
type Result struct {
	Case     string
	Skipped  string   // why, when the host lacks a feature it needs
	Errors   []string // what's off; none: it passed
	Measured []string // what it measured, for the log
}

// Measure finds a page of the sheet in img, a capture of a screen that
// shows it, and runs each of the page's cases on it. It returns which
// page that is.
func Measure(img image.Image, host string) (page int, results []Result, err error) {
	s, err := locate(img)
	if err != nil {
		return 0, nil, err
	}
	var out []Result
	for i, c := range pageCases(s.page) {
		r := Result{Case: c.Name}
		if why := skipped(host, c); why != "" {
			r.Skipped = why
			out = append(out, r)
			continue
		}
		x, y := cellOrigin(i)
		p := &Probe{s: s, x: float64(x), y: float64(y), lacks: unsupported[host]}
		c.Check(p)
		r.Errors, r.Measured = p.errors, p.measured
		out = append(out, r)
	}
	return s.page, out, nil
}

// Capture captures the screen with shot (a PNG) until it shows page p
// of the sheet, for at most d, and returns that capture. While another
// page shows, it calls show (when not nil) now and then: what makes the
// app show page p, tapping PageButton(p), again in case a tap was lost.
func Capture(t *testing.T, p int, d time.Duration, show func(), shot func() ([]byte, error)) image.Image {
	t.Helper()
	deadline := time.Now().Add(d)
	var last error
	shown := time.Time{}
	for time.Now().Before(deadline) {
		if show != nil && time.Since(shown) > 3*time.Second {
			show()
			shown = time.Now()
		}
		b, err := shot()
		if err == nil {
			var img image.Image
			if img, err = png.Decode(bytes.NewReader(b)); err == nil {
				var got int
				if got, _, err = Measure(img, ""); err == nil {
					if got == p {
						return img
					}
					err = fmt.Errorf("page %d shows, not page %d", got+1, p+1)
				}
			}
		}
		last = err
		time.Sleep(300 * time.Millisecond)
	}
	t.Fatalf("page %d of the sheet never showed: %v", p+1, last)
	return nil
}

// Check runs Measure on img, a capture of page p, and reports each of
// its cases as a subtest of t. It writes img to
// $ASUKA_CONFORMANCE_OUT/<name>-<page>.png when that's set, so the
// captures can be looked at side by side.
func Check(t *testing.T, name string, img image.Image, host string, p int) {
	t.Helper()
	if dir := os.Getenv("ASUKA_CONFORMANCE_OUT"); dir != "" {
		if err := writePNG(filepath.Join(dir, fmt.Sprintf("%s-%d.png", name, p+1)), img); err != nil {
			t.Log(err)
		}
	}
	page, results, err := Measure(img, host)
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	if page != p {
		t.Fatalf("%s: the capture shows page %d, not page %d", name, page+1, p+1)
	}
	for _, r := range results {
		t.Run(r.Case, func(t *testing.T) {
			if len(r.Measured) > 0 {
				t.Log(strings.Join(r.Measured, ", "))
			}
			if r.Skipped != "" {
				t.Skip(r.Skipped)
			}
			for _, e := range r.Errors {
				t.Error(e)
			}
		})
	}
}

func writePNG(path string, img image.Image) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// sheet is where a page of the sheet is in a capture: which page, its
// top left, in pixels, and the pixels per point.
type sheet struct {
	img        image.Image
	page       int
	ox, oy     float64
	sx, sy     float64
	maxX, maxY int
	// colors are how the capture shows the cases' colors (white, the
	// swatches'), by their sRGB values.
	colors map[RGB]RGB
}

// locate finds the sheet by its markers: the magenta square nearest the
// capture's top left and the one nearest its bottom right.
func locate(img image.Image) (*sheet, error) {
	b := img.Bounds()
	magenta := func(x, y int) bool {
		r, g, bl, _ := img.At(x, y).RGBA()
		return r>>8 > 200 && g>>8 < 70 && bl>>8 > 200
	}
	// Each marker's pixels: a square's bounding box. The top left one is
	// the magenta pixels nearest the top left, then its box grows.
	box := func(x, y int) image.Rectangle {
		x0, y0, x1, y1 := x, y, x, y
		for x0 > b.Min.X && magenta(x0-1, y) {
			x0--
		}
		for x1+1 < b.Max.X && magenta(x1+1, y) {
			x1++
		}
		for y0 > b.Min.Y && magenta(x, y0-1) {
			y0--
		}
		for y1+1 < b.Max.Y && magenta(x, y1+1) {
			y1++
		}
		return image.Rect(x0, y0, x1+1, y1+1)
	}
	var first, last image.Rectangle
	found := false
	for y := b.Min.Y; y < b.Max.Y && !found; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			if magenta(x, y) {
				// Its row's middle: the square's middle column, so the box
				// isn't cut by an antialiased corner.
				r := box(x, y)
				first, found = box((r.Min.X+r.Max.X)/2, y+(r.Dx())/2), true
				break
			}
		}
	}
	if !found {
		return nil, fmt.Errorf("no registration marker in the %dx%d capture: is the sheet on screen?", b.Dx(), b.Dy())
	}
	found = false
	for y := b.Max.Y - 1; y >= b.Min.Y && !found; y-- {
		for x := b.Max.X - 1; x >= b.Min.X; x-- {
			if magenta(x, y) {
				r := box(x, y)
				last, found = box((r.Min.X+r.Max.X)/2, y-r.Dx()/2), true
				break
			}
		}
	}
	if !found || last == first {
		return nil, fmt.Errorf("one registration marker in the capture: is all of the sheet on screen?")
	}
	// Every page is as wide: the scale across, then the page by its
	// marks (read at that scale down too), then the scale down by the
	// page's height.
	w, _ := SheetSize(0)
	s := &sheet{
		img: img, maxX: b.Max.X - 1, maxY: b.Max.Y - 1,
		ox: float64(first.Min.X), oy: float64(first.Min.Y),
		sx: float64(last.Max.X-first.Min.X) / float64(w),
	}
	s.sy = s.sx
	mark := func(j int) bool {
		c := s.at(float64(pageMarkX(j))+marker/2, marker/2)
		return c.R < 96 && c.G < 96 && c.B < 96
	}
	marks := 0
	for marks < Pages() && mark(marks) {
		marks++
	}
	if marks == 0 {
		return nil, fmt.Errorf("no page mark right of the swatches: is the sheet the cases' own?")
	}
	if mark(marks) {
		return nil, fmt.Errorf("more page marks than the sheet's %d pages: is the sheet the cases' own?", Pages())
	}
	s.page = marks - 1
	_, h := SheetSize(s.page)
	s.sy = float64(last.Max.Y-first.Min.Y) / float64(h)
	// Each marker is marker points square: the scales agree, or what was
	// found isn't the sheet.
	for _, k := range []float64{float64(first.Dx()) / marker, float64(first.Dy()) / marker, float64(last.Dx()) / marker, float64(last.Dy()) / marker} {
		if k < s.sx-1.5/marker*2 || k > s.sx+1.5/marker*2 {
			return nil, fmt.Errorf("the markers (%v, %v) don't match the sheet's scale %.3f", first, last, s.sx)
		}
	}
	if d := s.sx - s.sy; d > 0.02 || d < -0.02 {
		return nil, fmt.Errorf("the sheet's scale differs across (%.3f) and down (%.3f)", s.sx, s.sy)
	}
	// The colors as the capture shows them: the white between the marker
	// and the first swatch, each swatch's middle.
	s.colors = map[RGB]RGB{white: s.at(1.5*marker, marker/2)}
	for i, sw := range swatches {
		got := s.at(float64(swatchX(i))+marker/2, marker/2)
		// A display's space moves a color, never this far (sRGB's red is
		// (234,51,35) in Display P3).
		if d := math.Abs(got.R-sw.rgb.R) + math.Abs(got.G-sw.rgb.G) + math.Abs(got.B-sw.rgb.B); d > 160 {
			return nil, fmt.Errorf("the swatch of %s shows %v: is the sheet the cases' own?", sw.hex, got)
		}
		s.colors[sw.rgb] = got
	}
	return s, nil
}

// pixel is the capture's color at pixel coordinates (clamped to it).
func (s *sheet) pixel(px, py int) RGB {
	px = min(max(px, s.img.Bounds().Min.X), s.maxX)
	py = min(max(py, s.img.Bounds().Min.Y), s.maxY)
	r, g, b, a := s.img.At(px, py).RGBA()
	if a == 0 {
		return RGB{}
	}
	// Straight, not premultiplied: captures are opaque, but be safe.
	k := 255 / float64(a)
	return RGB{float64(r) * k, float64(g) * k, float64(b) * k}
}

// at is the color at the point (x, y) of the sheet.
func (s *sheet) at(x, y float64) RGB {
	return s.pixel(int(math.Floor(s.ox+x*s.sx)), int(math.Floor(s.oy+y*s.sy)))
}
