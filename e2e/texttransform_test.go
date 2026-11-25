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

// A text in a view drawn under a transform is laid out at its own size:
// layout doesn't see the transform (e2e/testdata/texttransform). Drawn
// at half its size, it's half as wide and half as high as the same text
// drawn as it is, on one line; and a text that wraps, drawn at half its
// size or turned a quarter, fits its frame.

// wrappedTexts are the app's texts that wrap under a transform.
var wrappedTexts = []string{
	"Wrapped at half its size, it fits its frame",
	"Turned a quarter and wrapped, it fits its frame",
}

// transformedSize is the red text's size and the blue one's, as drawn,
// in pixels; false while either isn't on screen.
func transformedSize(img image.Image) (image.Point, image.Point, bool) {
	red, ok1 := colorBounds(img, func(r, g, b uint32) bool { return r > 200 && g < 60 && b < 60 })
	blue, ok2 := colorBounds(img, func(r, g, b uint32) bool { return r < 60 && g < 60 && b > 200 })
	if !ok1 || !ok2 {
		return image.Point{}, image.Point{}, false
	}
	return red.Size(), blue.Size(), true
}

// checkTextTransform captures the screen with shot until the texts are on
// it (bounded) and checks the red one is drawn at half the blue one's
// size, to a pixel and a half.
func checkTextTransform(t *testing.T, shot func() ([]byte, error)) {
	t.Helper()
	deadline := time.Now().Add(within)
	var red, blue image.Point
	var seen bool
	var last error
	for time.Now().Before(deadline) {
		b, err := shot()
		if err == nil {
			var img image.Image
			if img, err = png.Decode(bytes.NewReader(b)); err == nil {
				if red, blue, seen = transformedSize(img); seen {
					break
				}
				err = fmt.Errorf("the texts aren't on screen")
			}
		}
		last = err
		time.Sleep(300 * time.Millisecond)
	}
	if !seen {
		t.Fatalf("the texts never showed: %v", last)
	}
	t.Logf("drawn at half its size the text is %v pixels, as it is %v", red, blue)
	if math.Abs(float64(red.X)-float64(blue.X)/2) > 1.5 || math.Abs(float64(red.Y)-float64(blue.Y)/2) > 1.5 {
		t.Errorf("drawn at half its size the text is %v pixels, as it is %v: it was measured as drawn, transform and all", red, blue)
	}
}

// checkTextsFit waits (bounded) until the page shows texts and checks
// each fits its frame: its lines, padding included, are no higher or
// wider than the frame the core gave it.
func checkTextsFit(t *testing.T, d *devtools, texts []string) {
	t.Helper()
	js := `(() => { const out = {}; for (const e of document.querySelectorAll(".asuka-text")) out[e.textContent] = [e.clientWidth, e.clientHeight, e.scrollWidth, e.scrollHeight]; return out; })()`
	deadline := time.Now().Add(within)
	var sizes map[string][]float64
	for time.Now().Before(deadline) {
		sizes = nil
		if pageEval(d, js, &sizes) == nil && sizes != nil {
			all := true
			for _, text := range texts {
				if len(sizes[text]) != 4 || sizes[text][1] == 0 {
					all = false
				}
			}
			if all {
				break
			}
		}
		time.Sleep(300 * time.Millisecond)
	}
	for _, text := range texts {
		s := sizes[text]
		if len(s) != 4 {
			t.Fatalf("%q never showed", text)
		}
		t.Logf("%q: frame %vx%v, its lines %vx%v", text, s[0], s[1], s[2], s[3])
		if s[2] > s[0]+1 || s[3] > s[1]+1 {
			t.Errorf("%q is %vx%v in a frame %vx%v: it was measured as drawn", text, s[2], s[3], s[0], s[1])
		}
	}
}

func TestTextTransformWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/texttransform")
	for _, scale := range []string{"1", "2"} {
		t.Run(scale+"x", func(t *testing.T) {
			page := devtoolsPage(t, browser, url, scale)
			checkTextTransform(t, func() ([]byte, error) { return page.screenshot() })
			checkTextsFit(t, page, wrappedTexts)
		})
	}
}
