//go:build e2e

package e2e

import (
	"bytes"
	"fmt"
	"image"
	"image/png"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

// A button's Padding replaces the platform's own, as CSS's does
// (e2e/testdata/buttonpadding): with a MinHeight of 48 and 10 points
// above and below a 16 point title, the button is 48 points high, on
// every platform. It's measured on the screen, against a magenta square
// 48 points high.

// buttonHeight is the red button's height in points in img, measured
// against the magenta square; false while either isn't on screen.
func buttonHeight(img image.Image) (float64, bool) {
	b := img.Bounds()
	redTop, redBottom, magTop, magBottom := -1, -1, -1, -1
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			r, g, bl, _ := img.At(x, y).RGBA()
			r, g, bl = r>>8, g>>8, bl>>8
			switch {
			case r > 200 && g < 60 && bl < 60:
				if redTop < 0 {
					redTop = y
				}
				redBottom = y
			case r > 200 && g < 70 && bl > 200:
				if magTop < 0 {
					magTop = y
				}
				magBottom = y
			}
		}
	}
	if redTop < 0 || magTop < 0 {
		return 0, false
	}
	k := float64(magBottom-magTop+1) / 48
	return float64(redBottom-redTop+1) / k, true
}

// checkButtonHeight captures the screen with shot until the button is on
// it (bounded) and checks it's 48 points high, to a point.
func checkButtonHeight(t *testing.T, shot func() ([]byte, error)) {
	t.Helper()
	deadline := time.Now().Add(within)
	var h float64
	var last error
	for time.Now().Before(deadline) {
		b, err := shot()
		if err == nil {
			var img image.Image
			if img, err = png.Decode(bytes.NewReader(b)); err == nil {
				var ok bool
				if h, ok = buttonHeight(img); ok {
					break
				}
				err = fmt.Errorf("the button or the square isn't on screen")
			}
		}
		last = err
		time.Sleep(300 * time.Millisecond)
	}
	if h == 0 {
		t.Fatalf("the button never showed: %v", last)
	}
	t.Logf("the button is %.1f points high", h)
	if math.Abs(h-48) > 1 {
		t.Errorf("the button is %.1f points high, want 48: its Padding is added to the platform's own", h)
	}
}

func TestButtonPaddingIOS(t *testing.T) {
	s := iosApp(t, "e2e/testdata/buttonpadding", "dev.asuka.buttonpadding")
	shot := filepath.Join(t.TempDir(), "screen.png")
	checkButtonHeight(t, func() ([]byte, error) {
		if out, err := exec.Command("xcrun", "simctl", "io", s.udid, "screenshot", "--type=png", shot).CombinedOutput(); err != nil {
			return nil, fmt.Errorf("%v: %s", err, out)
		}
		return os.ReadFile(shot)
	})
}

func TestButtonPaddingAndroid(t *testing.T) {
	s := androidApp(t, "e2e/testdata/buttonpadding", "dev.asuka.buttonpadding")
	checkButtonHeight(t, func() ([]byte, error) {
		return exec.Command(adb(), "-s", s.serial, "exec-out", "screencap", "-p").Output()
	})
}

func TestButtonPaddingWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/buttonpadding")
	for _, scale := range []string{"1", "2"} {
		t.Run(scale+"x", func(t *testing.T) {
			page := devtoolsPage(t, browser, url, scale)
			checkButtonHeight(t, func() ([]byte, error) { return page.screenshot() })
		})
	}
}
