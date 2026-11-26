//go:build e2e

package e2e

import (
	"testing"
	"time"
)

// A Menu on a view opens on a click on the view, and on a tap, and stays
// open until an action is chosen (e2e/testdata/menuclick). A menu shown
// before the release's click is closed at once by the browser's light
// dismiss, which comes at the release's mouseup (a tap's too).
func TestMenuClickWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/menuclick")
	d := devtoolsPage(t, browser, url, "1")
	waitText(t, d, "Chosen: nothing yet")
	// opened says whether one menu is open after a click or a tap.
	opened := func(how string) bool {
		t.Helper()
		// After the release's events, the light dismiss and the click, and
		// before menuClickMs's fallback.
		time.Sleep(300 * time.Millisecond)
		var n int
		if err := pageEval(d, `document.querySelectorAll(":popover-open").length`, &n); err != nil {
			t.Fatal(err)
		}
		if n != 1 {
			t.Errorf("%d menus open after a %s on the view, want 1", n, how)
		}
		return n == 1
	}

	pressText(t, d, "Metformin 500 mg")
	if opened("click") {
		pressText(t, d, "Hold")
		waitText(t, d, "Chosen: Hold")
	}

	// A tap: DevTools' touch events, which the browser follows with the
	// tap's mouse events and click.
	settle(t, d)
	x, y, ok := textCenter(d, "Metformin 500 mg")
	if !ok {
		t.Fatal("the view isn't on the page")
	}
	var r struct{}
	for _, ev := range []map[string]any{
		{"type": "touchStart", "touchPoints": []map[string]any{{"x": x, "y": y}}},
		{"type": "touchEnd", "touchPoints": []map[string]any{}},
	} {
		if err := d.call("Input.dispatchTouchEvent", ev, &r); err != nil {
			t.Fatal(err)
		}
	}
	if opened("tap") {
		pressText(t, d, "Refill")
		waitText(t, d, "Chosen: Refill")
	}
}
