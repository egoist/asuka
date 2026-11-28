//go:build e2e

package e2e

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/egoist/asuka/conformance"
)

// The conformance sheet (package conformance) on each platform: the app
// that shows it is launched, and each page of it in turn is shown (its
// button tapped), the screen captured until the page is on it, and the
// page measured.

// conformancePages captures and measures each page of the sheet: tap
// taps a button, shot captures the screen as a PNG.
func conformancePages(t *testing.T, name, host string, tap func(string), shot func() ([]byte, error)) {
	t.Helper()
	for p := range conformance.Pages() {
		show := func() { tap(conformance.PageButton(p)) }
		if p == 0 {
			show = nil // the page the app starts on
		}
		img := conformance.Capture(t, p, within, show, shot)
		conformance.Check(t, name, img, host, p)
	}
}

func TestConformanceIOS(t *testing.T) {
	s := iosApp(t, "conformance/app", "dev.asuka.conformance")
	shot := filepath.Join(t.TempDir(), "screen.png")
	conformancePages(t, "ios", "ios", s.tap, func() ([]byte, error) {
		if out, err := exec.Command("xcrun", "simctl", "io", s.udid, "screenshot", "--type=png", shot).CombinedOutput(); err != nil {
			return nil, fmt.Errorf("%v: %s", err, out)
		}
		return os.ReadFile(shot)
	})
}

func TestConformanceAndroid(t *testing.T) {
	s := androidApp(t, "conformance/app", "dev.asuka.conformance")
	conformancePages(t, "android", "android", s.tap, func() ([]byte, error) {
		return exec.Command(adb(), "-s", s.serial, "exec-out", "screencap", "-p").Output()
	})
}

// TestConformanceWeb measures the sheet in headless Chrome at three
// device pixel ratios: the web host places frames on the device's
// pixels, and sizes shadows and borders in CSS pixels. The page's
// buttons are tapped through the dev server, as webScreen's.
func TestConformanceWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "conformance/app")
	for _, scale := range []string{"1", "1.5", "2"} {
		t.Run(scale+"x", func(t *testing.T) {
			page := devtoolsPage(t, browser, url, scale)
			s := &webScreen{t: t, url: url}
			conformancePages(t, "web@"+scale+"x", "web", s.tap, func() ([]byte, error) { return page.screenshot() })
		})
	}
}
