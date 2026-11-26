//go:build e2e

package e2e

import (
	"runtime"
	"strings"
	"testing"
	"time"
)

// A row that hears of a selection beginning (on selectBegin) begins one
// on the web as a desktop list does, with a ⌘-click (Ctrl-click off
// Apple's systems), the row its first; a plain click doesn't
// (e2e/testdata/selectbegin). While selecting, a click selects.

// pageClickMods presses the mouse at x, y with modifiers held (DevTools':
// 2 Ctrl, 4 Meta) and lets it go.
func pageClickMods(d *devtools, x, y float64, mods int) error {
	for _, typ := range []string{"mousePressed", "mouseReleased"} {
		var r struct{}
		if err := d.call("Input.dispatchMouseEvent", map[string]any{"type": typ, "x": x, "y": y, "button": "left", "clickCount": 1, "modifiers": mods}, &r); err != nil {
			return err
		}
	}
	return nil
}

func TestSelectBeginWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/selectbegin")
	d := devtoolsPage(t, browser, url, "1")
	waitText(t, d, "selecting false, chosen []")
	at := func(text string) (float64, float64) {
		t.Helper()
		x, y, ok := textCenter(d, text)
		if !ok {
			t.Fatalf("no %q", text)
		}
		return x, y
	}
	// A plain click begins nothing.
	x, y := at("Message 3")
	if err := pageClick(d, x, y); err != nil {
		t.Fatal(err)
	}
	time.Sleep(300 * time.Millisecond)
	var body string
	pageEval(d, "document.body.innerText", &body)
	if !strings.Contains(body, "selecting false, chosen []") {
		t.Fatalf("a plain click began selecting: %q", body)
	}
	// The primary modifier's click begins it, the row first.
	mods := 2
	if runtime.GOOS == "darwin" {
		mods = 4
	}
	if err := pageClickMods(d, x, y, mods); err != nil {
		t.Fatal(err)
	}
	waitText(t, d, "selecting true, chosen [3]")
	// Selecting, a click selects another.
	x, y = at("Message 5")
	if err := pageClick(d, x, y); err != nil {
		t.Fatal(err)
	}
	waitText(t, d, "selecting true, chosen [3,5]")
}
