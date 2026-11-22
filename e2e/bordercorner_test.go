//go:build e2e

package e2e

import (
	"bytes"
	"fmt"
	"image"
	"image/png"
	"math"
	"strconv"
	"testing"
	"time"
)

// A view's border is drawn over its clipped content with the view's own
// corners (e2e/testdata/bordercorner): a card 200 points wide with a
// border 1 point wide, corners of 12 and a green header clipped to them
// shows no green outside its border's line at its top corners. It's
// measured on the screen, the card found by its black border. A scroll
// view's border isn't carried off with its content as it scrolls, and an
// image's border shows over the picture, which fills its frame.

// cornerBleed counts the header's green pixels past the middle of the
// card's border's line at its two top corners; false while the card
// isn't on screen. A pixel the line's edge crosses blends the green with
// the line or the page, and isn't the header's green.
func cornerBleed(img image.Image) (int, bool) {
	b := img.Bounds()
	at := func(x, y int) (uint32, uint32, uint32) {
		r, g, bl, _ := img.At(x, y).RGBA()
		return r >> 8, g >> 8, bl >> 8
	}
	// The card's border box: the black border's bounds.
	x0, y0, x1, y1 := b.Max.X, b.Max.Y, b.Min.X-1, b.Min.Y-1
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			if r, g, bl := at(x, y); r < 40 && g < 40 && bl < 40 {
				x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
			}
		}
	}
	if x1 < x0 {
		return 0, false
	}
	k := float64(x1-x0+1) / 200  // pixels a point
	radius, line := 12*k, 11.5*k // the border's outer curve, and its line's middle
	bleed := 0
	for _, c := range []struct{ cx, left float64 }{ // each corner's curve's center, and its square's left edge
		{float64(x0) + radius, float64(x0)},              // top left
		{float64(x1+1) - radius, float64(x1+1) - radius}, // top right
	} {
		cy := float64(y0) + radius
		for y := y0; float64(y) < cy; y++ {
			for x := int(c.left); float64(x) < c.left+radius; x++ {
				if math.Hypot(float64(x)+0.5-c.cx, float64(y)+0.5-cy) <= line {
					continue
				}
				if r, g, bl := at(x, y); g > 150 && r < 40 && bl < 40 {
					bleed++
				}
			}
		}
	}
	return bleed, true
}

// checkBorderCorner captures the screen with shot until the card is on
// it (bounded) and checks no green is outside its border's line.
func checkBorderCorner(t *testing.T, shot func() ([]byte, error)) {
	t.Helper()
	deadline := time.Now().Add(within)
	var bleed int
	var seen bool
	var last error
	for time.Now().Before(deadline) {
		b, err := shot()
		if err == nil {
			var img image.Image
			if img, err = png.Decode(bytes.NewReader(b)); err == nil {
				if bleed, seen = cornerBleed(img); seen {
					break
				}
				err = fmt.Errorf("the card isn't on screen")
			}
		}
		last = err
		time.Sleep(300 * time.Millisecond)
	}
	if !seen {
		t.Fatalf("the card never showed: %v", last)
	}
	if bleed > 0 {
		t.Errorf("%d green pixels outside the border's line at the card's top corners, want none: the clipped header shows past the border", bleed)
	}
}

// countPixels counts the screen's pixels whose color is like.
func countPixels(t *testing.T, page *devtools, like func(r, g, b uint32) bool) int {
	t.Helper()
	b, err := page.screenshot()
	if err != nil {
		t.Fatal(err)
	}
	img, err := png.Decode(bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for y := img.Bounds().Min.Y; y < img.Bounds().Max.Y; y++ {
		for x := img.Bounds().Min.X; x < img.Bounds().Max.X; x++ {
			if r, g, bl, _ := img.At(x, y).RGBA(); like(r>>8, g>>8, bl>>8) {
				n++
			}
		}
	}
	return n
}

// redPixels counts the scroll view's red border's pixels.
func redPixels(t *testing.T, page *devtools) int {
	return countPixels(t, page, func(r, g, b uint32) bool { return r > 200 && g < 60 && b < 60 })
}

// checkScrollBorder scrolls the scroll view past its height and checks
// its border still shows.
func checkScrollBorder(t *testing.T, page *devtools) {
	t.Helper()
	var top float64
	if err := pageEval(page, `(() => { const s = document.querySelector(".asuka-scroller"); s.scrollTop = 100; return s.scrollTop; })()`, &top); err != nil {
		t.Fatalf("scrolling the scroll view: %v", err)
	}
	if top == 0 {
		t.Fatal("the scroll view didn't scroll")
	}
	time.Sleep(300 * time.Millisecond)
	if redPixels(t, page) == 0 {
		t.Errorf("scrolled %v points, a scroll view's border is gone: it scrolled away with the content", top)
	}
}

// checkImageBorder waits for the image's orange picture (bounded) and
// checks its blue border shows over it: at least half the ring's area,
// a ring 3 points wide outside a circle 54 across, at scale k.
func checkImageBorder(t *testing.T, page *devtools, k float64) {
	t.Helper()
	orange := func(r, g, b uint32) bool { return r > 200 && g > 120 && g < 200 && b < 60 }
	for deadline := time.Now().Add(within); countPixels(t, page, orange) == 0; time.Sleep(300 * time.Millisecond) {
		if time.Now().After(deadline) {
			t.Fatal("the image never showed")
		}
	}
	want := int(math.Pi * (30*30 - 27*27) * k * k / 2)
	if blue := countPixels(t, page, func(r, g, b uint32) bool { return b > 200 && r < 60 && g < 60 }); blue < want {
		t.Errorf("%d pixels of the image's blue border, want at least %d: the picture covers its border", blue, want)
	}
}

func TestBorderCornerWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/bordercorner")
	for _, k := range []int{1, 2} {
		scale := strconv.Itoa(k)
		t.Run(scale+"x", func(t *testing.T) {
			page := devtoolsPage(t, browser, url, scale)
			checkBorderCorner(t, func() ([]byte, error) { return page.screenshot() })
			checkScrollBorder(t, page)
			checkImageBorder(t, page, float64(k))
		})
	}
}
