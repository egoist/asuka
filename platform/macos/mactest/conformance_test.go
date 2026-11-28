//go:build darwin

package mactest

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/egoist/asuka/conformance"
)

// TestConformance draws the conformance sheet in the player's window, a
// page at a time (its app's buttons show each), and measures each page
// (package conformance): the window server's image of the window's
// content, as it shows, in the display's color space, where the window
// server blends (the sheet's swatches say how it shows the cases'
// colors).
func TestConformance(t *testing.T) {
	a := Run(t, conformance.App())
	a.UntilText(conformance.PageButton(0))
	// The window grows to show the tallest page and the buttons above it.
	h := float32(0)
	for p := range conformance.Pages() {
		_, ph := conformance.SheetSize(p)
		h = max(h, ph)
	}
	a.Do(fmt.Sprintf("resize 800,%d", int(h)+120))
	// Images and fonts load; the window settles.
	a.Do("wait 1s")
	shot := filepath.Join(t.TempDir(), "sheet.content.display.png")
	for p := range conformance.Pages() {
		show := func() { a.Do("tap " + conformance.PageButton(p)) }
		if p == 0 {
			show = nil // the page the app starts on
		}
		img := conformance.Capture(t, p, 30*time.Second, show, func() ([]byte, error) {
			a.Snap(shot)
			return os.ReadFile(shot)
		})
		conformance.Check(t, "macos", img, "macos", p)
	}
}
