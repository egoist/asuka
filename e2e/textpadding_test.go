//go:build e2e

package e2e

import (
	"bytes"
	"fmt"
	"image"
	"image/png"
	"math"
	"testing"
	"time"
)

// A text's padding is added to its text once (e2e/testdata/textpadding):
// the same text with a padding of 10 points above and below and 20 at
// each side is 40 points wider and 20 higher than without one, and given
// a width that holds it and its padding, it's on one line, 20 points
// higher. It's measured on the screen, against a magenta square 48
// points high.

// colorBounds is the bounds of the pixels in img that match, in pixels;
// false while there are none.
func colorBounds(img image.Image, match func(r, g, b uint32) bool) (image.Rectangle, bool) {
	b := img.Bounds()
	out := image.Rectangle{Min: b.Max, Max: b.Min}
	found := false
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			r, g, bl, _ := img.At(x, y).RGBA()
			if match(r>>8, g>>8, bl>>8) {
				found = true
				out = out.Union(image.Rect(x, y, x+1, y+1))
			}
		}
	}
	return out, found
}

// paddingGrowth is how much wider and higher the red text is than the
// blue one, and how much higher the green one is, in points, measured
// against the magenta square; false while any of them isn't on screen.
func paddingGrowth(img image.Image) (dw, dh, gh float64, ok bool) {
	mag, ok1 := colorBounds(img, func(r, g, b uint32) bool { return r > 200 && g < 70 && b > 200 })
	red, ok2 := colorBounds(img, func(r, g, b uint32) bool { return r > 200 && g < 60 && b < 60 })
	blue, ok3 := colorBounds(img, func(r, g, b uint32) bool { return r < 60 && g < 60 && b > 200 })
	green, ok4 := colorBounds(img, func(r, g, b uint32) bool { return r < 60 && g > 200 && b < 60 })
	if !ok1 || !ok2 || !ok3 || !ok4 {
		return 0, 0, 0, false
	}
	k := float64(mag.Dy()) / 48
	return float64(red.Dx()-blue.Dx()) / k, float64(red.Dy()-blue.Dy()) / k, float64(green.Dy()-blue.Dy()) / k, true
}

// checkTextPadding captures the screen with shot until the texts are on
// it (bounded) and checks the padded ones are bigger by their padding, to
// a point.
func checkTextPadding(t *testing.T, shot func() ([]byte, error)) {
	t.Helper()
	deadline := time.Now().Add(within)
	var dw, dh, gh float64
	var seen bool
	var last error
	for time.Now().Before(deadline) {
		b, err := shot()
		if err == nil {
			var img image.Image
			if img, err = png.Decode(bytes.NewReader(b)); err == nil {
				if dw, dh, gh, seen = paddingGrowth(img); seen {
					break
				}
				err = fmt.Errorf("the texts or the square aren't on screen")
			}
		}
		last = err
		time.Sleep(300 * time.Millisecond)
	}
	if !seen {
		t.Fatalf("the texts never showed: %v", last)
	}
	t.Logf("the padded text is %.1f points wider and %.1f higher; given its width, %.1f higher", dw, dh, gh)
	if math.Abs(dw-40) > 1 || math.Abs(dh-20) > 1 {
		t.Errorf("the padded text is %.1f points wider and %.1f higher, want 40 and 20: its padding is added more than once", dw, dh)
	}
	if math.Abs(gh-20) > 1 {
		t.Errorf("given its width, the padded text is %.1f points higher, want 20: it was laid out narrower by its padding", gh)
	}
}

func TestTextPaddingWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/textpadding")
	for _, scale := range []string{"1", "2"} {
		t.Run(scale+"x", func(t *testing.T) {
			page := devtoolsPage(t, browser, url, scale)
			checkTextPadding(t, func() ([]byte, error) { return page.screenshot() })
		})
	}
}
