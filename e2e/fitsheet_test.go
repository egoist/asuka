//go:build e2e

package e2e

import (
	"bytes"
	"fmt"
	"image"
	"image/png"
	"testing"
	"time"
)

// A sheet with the .fit detent is as tall as its content
// (e2e/testdata/fitsheet): its content ends with a red band, and right
// under the band is what's behind the sheet, not more of the sheet.

// fitSheetEnd finds the red band in img and says whether what's just
// under its middle is the sheet's white (the sheet goes on past its
// content); false while the band isn't on screen.
func fitSheetEnd(img image.Image) (white bool, below image.Point, ok bool) {
	b := img.Bounds()
	minX, maxX, bottom := -1, -1, -1
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			r, g, bl, _ := img.At(x, y).RGBA()
			if r>>8 > 200 && g>>8 < 60 && bl>>8 < 60 {
				if minX < 0 || x < minX {
					minX = x
				}
				if x > maxX {
					maxX = x
				}
				bottom = y
			}
		}
	}
	if bottom < 0 {
		return false, image.Point{}, false
	}
	// A few pixels under the band, past any rounding of the sheet's corner.
	p := image.Pt((minX+maxX)/2, bottom+6)
	if p.Y >= b.Max.Y {
		return false, p, true
	}
	r, g, bl, _ := img.At(p.X, p.Y).RGBA()
	return r>>8 > 240 && g>>8 > 240 && bl>>8 > 240, p, true
}

// checkFitSheet captures the screen with shot until the sheet shows
// (bounded) and checks it ends where its content does.
func checkFitSheet(t *testing.T, shot func() ([]byte, error)) {
	t.Helper()
	deadline := time.Now().Add(within)
	var white, seen bool
	var at image.Point
	var last error
	for time.Now().Before(deadline) {
		b, err := shot()
		if err == nil {
			var img image.Image
			if img, err = png.Decode(bytes.NewReader(b)); err == nil {
				if white, at, seen = fitSheetEnd(img); seen {
					// past its opening animation
					time.Sleep(500 * time.Millisecond)
					if b, err = shot(); err == nil {
						if img, err = png.Decode(bytes.NewReader(b)); err == nil {
							white, at, seen = fitSheetEnd(img)
						}
					}
					if seen {
						break
					}
				}
				err = fmt.Errorf("the sheet's content isn't on screen")
			}
		}
		last = err
		time.Sleep(300 * time.Millisecond)
	}
	if !seen {
		t.Fatalf("the sheet never showed: %v", last)
	}
	if white {
		t.Errorf("under the sheet's content, at %v, the sheet goes on: a .fit sheet isn't as tall as its content", at)
	}
}

func TestFitSheetWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/fitsheet")
	for _, scale := range []string{"1", "2"} {
		t.Run(scale+"x", func(t *testing.T) {
			page := devtoolsPage(t, browser, url, scale)
			checkFitSheet(t, func() ([]byte, error) { return page.screenshot() })
		})
	}
}

// A .fit sheet whose content is one column 300 points wide
// (e2e/testdata/fitsheetfixed): on a narrow page (Chrome's window here,
// under 600) a bottom sheet as wide as the page; on a wide one (1000) a
// card as wide as the column.
func TestFitSheetFixedWidthWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/fitsheetfixed")
	page := devtoolsPage(t, browser, url, "1")
	// dialogWidth is the sheet's width once it's shown and laid out at
	// want, or its last width (bounded).
	dialogWidth := func(want float64) float64 {
		var got float64
		for deadline := time.Now().Add(within); time.Now().Before(deadline); time.Sleep(100 * time.Millisecond) {
			var r struct{ Result struct{ Value float64 } }
			if err := page.call("Runtime.evaluate", map[string]any{
				"expression":    `(() => { const d = document.querySelector("dialog[open]"); return d && d.textContent.includes("Stop Lisinopril?") ? d.getBoundingClientRect().width : 0; })()`,
				"returnByValue": true,
			}, &r); err != nil {
				t.Fatal(err)
			}
			if got = r.Result.Value; got == want {
				break
			}
		}
		return got
	}
	var page0 struct{ Result struct{ Value float64 } }
	if err := page.call("Runtime.evaluate", map[string]any{"expression": "document.documentElement.clientWidth", "returnByValue": true}, &page0); err != nil {
		t.Fatal(err)
	}
	if w := page0.Result.Value; w >= 600 {
		t.Fatalf("the page is %v wide, not a narrow one", w)
	} else if got := dialogWidth(w); got != w {
		t.Errorf("on a page %v wide, the bottom sheet is %v wide, want the page's width", w, got)
	}
	if err := page.call("Emulation.setDeviceMetricsOverride", map[string]any{"width": 1000, "height": 700, "deviceScaleFactor": 1, "mobile": false}, &struct{}{}); err != nil {
		t.Fatal(err)
	}
	if got := dialogWidth(300); got != 300 {
		t.Errorf("on a page 1000 wide, the sheet is %v wide, want the column's 300", got)
	}
}
