package linux_test

import (
	"image"
	"image/png"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// dumped is a view of a dump: its kind, its frame in the window (its
// parents' added) and what it shows.
type dumped struct {
	kind       string
	x, y, w, h float64
	text       string
}

var dumpLine = regexp.MustCompile(`^( *)\d+ (\S+) \[(-?[\d.]+),(-?[\d.]+) ([\d.]+)x([\d.]+)\](.*)$`)

// views reads a dump's views, in order, their frames in the window.
func views(dump string) []dumped {
	var out []dumped
	type at struct{ x, y float64 }
	var stack []at
	for _, l := range strings.Split(dump, "\n") {
		m := dumpLine.FindStringSubmatch(l)
		if m == nil {
			continue
		}
		depth := len(m[1]) / 2
		f := func(i int) float64 { v, _ := strconv.ParseFloat(m[i], 64); return v }
		if depth > len(stack) {
			depth = len(stack)
		}
		stack = stack[:depth]
		var px, py float64
		if depth > 0 {
			px, py = stack[depth-1].x, stack[depth-1].y
		}
		v := dumped{kind: m[2], x: px + f(3), y: py + f(4), w: f(5), h: f(6)}
		if i := strings.Index(m[7], `"`); i >= 0 {
			v.text, _ = strconv.Unquote(m[7][i:])
		}
		out = append(out, v)
		stack = append(stack, at{v.x, v.y})
	}
	return out
}

// The text and look keys (testdata/styles.asuka): a text's letter
// spacing and line height measured with it, shrinking to fit one line,
// and what the view draws: per-corner radii, a dashed border, one side's
// border, a text's shadow, radial and conic gradients.
func TestStyles(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "styles.asuka"), exe)
	d := script(t, dir, exe, "wait:800ms", "snap:styled")
	t.Logf("%s", d["styled"])
	vs := views(d["styled"])
	var texts, boxes []dumped
	for _, v := range vs {
		switch {
		case v.kind == "Text":
			texts = append(texts, v)
		case v.kind == "View" && v.w == 60 && v.h == 60:
			boxes = append(boxes, v)
		}
	}
	if len(texts) != 8 || len(boxes) != 5 {
		t.Fatalf("%d texts and %d boxes, want 8 and 5:\n%s", len(texts), len(boxes), d["styled"])
	}
	plain, spaced, tall, own, shrunk := texts[0], texts[1], texts[2], texts[3], texts[4]
	// Four points between each of its five characters (Pango's: none
	// after the last).
	if dw := spaced.w - plain.w; dw < 14 || dw > 22 {
		t.Errorf("letter spacing 4 widens Hello by %g, want about 16 (%g, %g)", dw, plain.w, spaced.w)
	}
	if tall.h != 80 {
		t.Errorf("two lines 40 points apart are %g tall, want 80", tall.h)
	}
	if own.h >= tall.h || own.h < 30 {
		t.Errorf("two lines of the font's height are %g tall", own.h)
	}
	// Shrunk onto one line: shorter than a line of its size.
	if shrunk.h >= plain.h || shrunk.h < plain.h*0.3 || shrunk.w > 120 {
		t.Errorf("a shrinking text is %gx%g, want one line shorter than %g within 120", shrunk.w, shrunk.h, plain.h)
	}

	f, err := os.Open(filepath.Join(dir, "styled.png"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		t.Fatal(err)
	}
	// Where the window's points are in the capture: the red box's square
	// corners are its frame's.
	red := func(r, g, b uint32) bool { return r > 200 && g < 60 && b < 60 }
	rb, ok := bbox(img, red)
	if !ok {
		t.Fatal("no red box")
	}
	scale := float64(rb.Dx()) / 60
	ox, oy := float64(rb.Min.X)-boxes[0].x*scale, float64(rb.Min.Y)-boxes[0].y*scale
	at := func(v dumped, ux, uy float64) (uint32, uint32, uint32) {
		c := img.At(int(ox+(v.x+ux*v.w)*scale), int(oy+(v.y+uy*v.h)*scale))
		r, g, b, _ := c.RGBA()
		return r >> 8, g >> 8, b >> 8
	}
	// radius: [30, 0, 0, 0]: its top left is cut, its other corners square.
	if r, g, b := at(boxes[0], 0.03, 0.03); red(r, g, b) {
		t.Errorf("the top left corner isn't rounded: %d,%d,%d", r, g, b)
	}
	for _, c := range [][2]float64{{0.97, 0.03}, {0.97, 0.97}, {0.03, 0.97}} {
		if r, g, b := at(boxes[0], c[0], c[1]); !red(r, g, b) {
			t.Errorf("the corner at %v is rounded: %d,%d,%d", c, r, g, b)
		}
	}
	// A dashed border: blue along its top, with gaps.
	runs, blue := 0, false
	for i := 0; i < 60; i++ {
		r, g, b := at(boxes[1], float64(i)/60, 2.0/60)
		on := b > 180 && r < 100 && g < 100
		if on && !blue {
			runs++
		}
		blue = on
	}
	if runs < 2 || runs > 5 {
		t.Errorf("a dashed border's top has %d dashes, want 2 to 5", runs)
	}
	// The bottom border alone.
	if r, g, b := at(boxes[2], 0.5, 0.97); !(g > 200 && r < 60 && b < 60) {
		t.Errorf("no bottom border: %d,%d,%d", r, g, b)
	}
	if r, g, b := at(boxes[2], 0.5, 0.03); g > 200 && r < 60 {
		t.Errorf("a top border: %d,%d,%d", r, g, b)
	}
	// A radial gradient: yellow in its middle, dark at its side.
	if r, g, b := at(boxes[3], 0.5, 0.5); !(r > 200 && g > 200 && b < 60) {
		t.Errorf("the radial gradient's center is %d,%d,%d, want yellow", r, g, b)
	}
	if r, _, _ := at(boxes[3], 0.98, 0.5); r > 60 {
		t.Errorf("the radial gradient's side is %d red, want dark", r)
	}
	// A conic gradient from the top, clockwise: its right more cyan, its
	// left more orange.
	rr, rg, _ := at(boxes[4], 0.9, 0.5)
	lr, lg, _ := at(boxes[4], 0.1, 0.5)
	if !(rg > rr && lr > lg) {
		t.Errorf("the conic gradient's right is %d,%d and its left %d,%d (red, green)", rr, rg, lr, lg)
	}
	// The text's shadow, magenta, 3 points down and right of its glyphs.
	sh := texts[7]
	magenta := 0
	for y := oy + sh.y*scale; y < oy+(sh.y+sh.h)*scale; y++ {
		for x := ox + sh.x*scale; x < ox+(sh.x+sh.w)*scale; x++ {
			if r, g, b, _ := img.At(int(x), int(y)).RGBA(); r>>8 > 200 && g>>8 < 80 && b>>8 > 200 {
				magenta++
			}
		}
	}
	if magenta < 20 {
		t.Errorf("no text shadow: %d magenta pixels", magenta)
	}
}

// bbox is the bounds of img's pixels that are in.
func bbox(img image.Image, in func(r, g, b uint32) bool) (image.Rectangle, bool) {
	b := img.Bounds()
	out := image.Rectangle{Min: b.Max, Max: b.Min}
	found := false
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			r, g, bl, _ := img.At(x, y).RGBA()
			if in(r>>8, g>>8, bl>>8) {
				found = true
				out = out.Union(image.Rect(x, y, x+1, y+1))
			}
		}
	}
	return out, found
}
