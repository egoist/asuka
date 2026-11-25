//go:build e2e

package e2e

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// Plain removes the platform's field decoration, but keeps an app's own
// background, border and corners, regardless of the order of its props.
func TestFieldStyleAndroid(t *testing.T) {
	s := androidApp(t, "e2e/testdata/field-style", "dev.asuka.fieldstyle")
	waitFor(t, s, []string{"Field styling"})
	out, err := s.uiDump()
	if err != nil {
		t.Fatalf("field bounds: %v: %s", err, out)
	}
	matches := boundsRE.FindAllStringSubmatch(out, -1)
	if len(matches) != 5 {
		t.Fatalf("got %d fields, want 5: %s", len(matches), out)
	}
	var fields []image.Rectangle
	for _, m := range matches {
		n := func(i int) int { v, _ := strconv.Atoi(m[i]); return v }
		fields = append(fields, image.Rect(n(1), n(2), n(3), n(4)))
	}
	shot := func(name string) image.Image {
		t.Helper()
		b, err := exec.Command(adb(), "-s", s.serial, "exec-out", "screencap", "-p").Output()
		if err != nil {
			t.Fatal(err)
		}
		if dir := os.Getenv("ASUKA_CONFORMANCE_OUT"); dir != "" {
			if err := os.MkdirAll(dir, 0o755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(dir, "android-field-"+name+".png"), b, 0o644); err != nil {
				t.Fatal(err)
			}
		}
		img, err := png.Decode(bytes.NewReader(b))
		if err != nil {
			t.Fatal(err)
		}
		return img
	}
	white := color.NRGBA{255, 255, 255, 255}
	page := color.NRGBA{242, 244, 246, 255}
	border := color.NRGBA{204, 216, 224, 255}
	pixel := func(img image.Image, p image.Point, want color.NRGBA) {
		t.Helper()
		got := color.NRGBAModel.Convert(img.At(p.X, p.Y)).(color.NRGBA)
		if got != want {
			t.Errorf("pixel %v = %v, want %v", p, got, want)
		}
	}
	check := func(img image.Image, r image.Rectangle, line color.NRGBA) {
		t.Helper()
		pixel(img, image.Pt(r.Max.X-r.Dx()/10, (r.Min.Y+r.Max.Y)/2), white)
		pixel(img, image.Pt((r.Min.X+r.Max.X)/2, r.Min.Y+1), line)
		pixel(img, r.Min.Add(image.Pt(1, 1)), page) // outside the rounded corner
	}
	img := shot("initial")
	for i, name := range []string{"style before plain", "plain before inline style", "multiline"} {
		t.Log(name)
		check(img, fields[i], border)
		r := fields[i]
		ink := r.Max.X
		for y := r.Min.Y; y < r.Max.Y; y++ {
			for x := r.Min.X; x < ink; x++ {
				c := color.NRGBAModel.Convert(img.At(x, y)).(color.NRGBA)
				if c.R >= 100 && c.R < 200 && c.G >= 100 && c.G < 200 && c.B >= 100 && c.B < 200 {
					ink = x // the placeholder, not the colored border or page
				}
			}
		}
		inset := float64(ink-r.Min.X) * 240 / float64(r.Dx())
		if ink == r.Max.X || inset < 15 || inset > 22 {
			t.Errorf("placeholder starts %.1f points in, want the 14-point padding plus 1.5-point border", inset)
		}
	}
	// Plain with no style still has no fill or underline. The native
	// field beside it still has its own underline (a positive control).
	r := fields[3]
	pixel(img, image.Pt((r.Min.X+r.Max.X)/2, r.Max.Y-2), page)
	r = fields[4]
	nativeLine := false
	for y := r.Min.Y + r.Dy()*3/4; y < r.Max.Y; y++ {
		if color.NRGBAModel.Convert(img.At((r.Min.X+r.Max.X)/2, y)) != page {
			nativeLine = true
		}
	}
	if !nativeLine {
		t.Error("the unstyled native field lost its underline")
	}
}

// TestFieldStyleIOS is TestFieldStyleAndroid on the simulator. There a
// multiline field is another view, made when multiline comes: in the third
// field after the app's style and plain, which it has to keep. The
// automation's dump has no frames, so the fields are found in the capture
// by their borders.
func TestFieldStyleIOS(t *testing.T) {
	s := iosApp(t, "e2e/testdata/field-style", "dev.asuka.fieldstyle")
	waitFor(t, s, []string{"Field styling"})
	white := color.NRGBA{255, 255, 255, 255}
	page := color.NRGBA{242, 244, 246, 255}
	border := color.NRGBA{204, 216, 224, 255}
	var img image.Image
	is := func(x, y int, want color.NRGBA) bool {
		got := color.NRGBAModel.Convert(img.At(x, y)).(color.NRGBA)
		near := func(a, b uint8) bool { return int(a)-int(b) <= 2 && int(b)-int(a) <= 2 }
		return near(got.R, want.R) && near(got.G, want.G) && near(got.B, want.B)
	}
	pixel := func(p image.Point, want color.NRGBA) {
		t.Helper()
		if !is(p.X, p.Y, want) {
			t.Errorf("pixel %v = %v, want %v", p, img.At(p.X, p.Y), want)
		}
	}
	// Where the border's color starts and ends down the middle of the
	// screen: four times a styled field, its top and bottom borders.
	var edges []int
	file := filepath.Join(t.TempDir(), "screen.png")
	var b []byte
	for deadline := time.Now().Add(10 * time.Second); ; time.Sleep(300 * time.Millisecond) {
		if out, err := exec.Command("xcrun", "simctl", "io", s.udid, "screenshot", "--type=png", file).CombinedOutput(); err != nil {
			t.Fatalf("%v: %s", err, out)
		}
		var err error
		if b, err = os.ReadFile(file); err != nil {
			t.Fatal(err)
		}
		if img, err = png.Decode(bytes.NewReader(b)); err != nil {
			t.Fatal(err)
		}
		edges = edges[:0]
		x := img.Bounds().Dx() / 2
		for y := 1; y < img.Bounds().Dy(); y++ {
			if is(x, y, border) != is(x, y-1, border) {
				edges = append(edges, y)
			}
		}
		if len(edges) == 12 || time.Now().After(deadline) {
			break
		}
	}
	if dir := os.Getenv("ASUKA_CONFORMANCE_OUT"); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "ios-field-initial.png"), b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if len(edges) != 12 {
		t.Fatalf("%d fields in the capture have the app's border, want 3: the multiline one too (the border's color starts and ends at %v)", len(edges)/4, edges)
	}
	// Where a field's placeholder starts: its ink, not the colored border or page.
	ink := func(r image.Rectangle) int {
		ink := r.Max.X
		for y := r.Min.Y; y < r.Max.Y; y++ {
			for x := r.Min.X; x < ink; x++ {
				c := color.NRGBAModel.Convert(img.At(x, y)).(color.NRGBA)
				if c.R >= 100 && c.R < 200 && c.G >= 100 && c.G < 200 && c.B >= 100 && c.B < 200 {
					ink = x
				}
			}
		}
		return ink
	}
	var r image.Rectangle
	for i, name := range []string{"style before plain", "plain before inline style", "multiline"} {
		mid := (edges[4*i] + edges[4*i+3]) / 2
		left, right := img.Bounds().Dx(), 0
		for x := 0; x < img.Bounds().Dx(); x++ {
			if is(x, mid, border) {
				left, right = min(left, x), x+1
			}
		}
		r = image.Rect(left, edges[4*i], right, edges[4*i+3])
		t.Log(name, r)
		if w := float64(r.Dx()) * 56 / float64(r.Dy()); w < 238 || w > 242 {
			t.Errorf("the field is %.1f points wide, want 240", w)
			continue
		}
		pixel(image.Pt(r.Max.X-r.Dx()/10, mid), white)
		pixel(r.Min.Add(image.Pt(1, 1)), page) // outside the rounded corner
		at := ink(r)
		inset := float64(at-r.Min.X) * 240 / float64(r.Dx())
		if at == r.Max.X || inset < 15 || inset > 22 {
			t.Errorf("placeholder starts %.1f points in, want the 14-point padding plus 1.5-point border", inset)
		}
	}
	// Plain with no style has no fill or border, below the last one by the
	// column's gap. The native field below it still has its own fill (a
	// positive control).
	down := image.Pt(0, r.Dy()*(56+16)/56)
	r = r.Add(down)
	if ink(r) == r.Max.X {
		t.Errorf("no placeholder in %v, where the plain field without style should be", r)
	}
	pixel(image.Pt(r.Max.X-r.Dx()/10, (r.Min.Y+r.Max.Y)/2), page)
	pixel(image.Pt((r.Min.X+r.Max.X)/2, r.Max.Y-2), page)
	r = r.Add(down)
	if p := image.Pt(r.Max.X-r.Dx()/10, (r.Min.Y+r.Max.Y)/2); is(p.X, p.Y, page) {
		t.Errorf("the unstyled native field lost its fill: pixel %v is the page's color", p)
	}
}
