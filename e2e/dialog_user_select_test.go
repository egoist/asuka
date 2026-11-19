//go:build e2e

package e2e

import (
	"runtime"
	"strings"
	"testing"
)

// TestDialogUserSelectWeb opens a sheet in headless Chrome
// (e2e/testdata/dialog-user-select): its own text and controls aren't
// selectable, as the page's aren't. A double click on its title's first
// word selects nothing, its title and button are user-select none, and
// select all (Cmd+A on a Mac, Ctrl+A elsewhere) after a press in its
// selectable text takes in that text alone.
func TestDialogUserSelectWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/dialog-user-select")
	d := devtoolsPage(t, browser, url, "1")
	pressText(t, d, "Open")
	waitText(t, d, "Sheet title")
	settle(t, d)
	selection := func() string {
		var s string
		if err := pageEval(d, `getSelection().toString()`, &s); err != nil {
			t.Fatal(err)
		}
		return s
	}

	// A double click 10 points into the title's text: on its first word.
	var at []float64
	if err := pageEval(d, `(() => { const e = [...document.querySelectorAll("dialog[open] .asuka-text")].find((e) => e.textContent === "Sheet title"); const r = e.getBoundingClientRect(); return [r.left + 10, r.top + r.height / 2]; })()`, &at); err != nil || len(at) != 2 {
		t.Fatalf("no title in the sheet: %v", err)
	}
	for i := 1; i <= 2; i++ {
		for _, typ := range []string{"mousePressed", "mouseReleased"} {
			var r struct{}
			if err := d.call("Input.dispatchMouseEvent", map[string]any{"type": typ, "x": at[0], "y": at[1], "button": "left", "clickCount": i}, &r); err != nil {
				t.Fatal(err)
			}
		}
	}
	if got := selection(); got != "" {
		t.Errorf("a double click on the sheet's title selects %q: a sheet's own text is selectable", got)
	}
	for what, sel := range map[string]string{
		"title":  `[...d.querySelectorAll(".asuka-text")].find((e) => e.textContent === "Sheet title")`,
		"button": `[...d.querySelectorAll("button")].find((e) => e.textContent.trim() === "Close")`,
	} {
		var us string
		if err := pageEval(d, `(() => { const d = document.querySelector("dialog[open]"); const s = getComputedStyle(`+sel+`); return s.userSelect || s.webkitUserSelect; })()`, &us); err != nil {
			t.Fatal(err)
		}
		if us != "none" {
			t.Errorf("the sheet's %s is user-select %q, want none", what, us)
		}
	}

	// Select all after a press in the selectable text takes in that text,
	// and neither the title nor the button. A DevTools key event runs no
	// editing command of its own: the key down names select all, which
	// the shortcut runs in a browser.
	if err := pageEval(d, `(() => { const e = [...document.querySelectorAll("dialog[open] *")].filter((e) => e.textContent.startsWith("First line")).pop(); const r = e.getBoundingClientRect(); return [r.left + 10, r.top + 8]; })()`, &at); err != nil || len(at) != 2 {
		t.Fatalf("no selectable text in the sheet: %v", err)
	}
	if err := pageClick(d, at[0], at[1]); err != nil {
		t.Fatal(err)
	}
	mods := 2 // Ctrl
	if runtime.GOOS == "darwin" {
		mods = 4 // Meta
	}
	for _, typ := range []string{"rawKeyDown", "keyUp"} {
		p := map[string]any{"type": typ, "key": "a", "code": "KeyA", "windowsVirtualKeyCode": 65, "modifiers": mods}
		if typ == "rawKeyDown" {
			p["commands"] = []string{"selectAll"}
		}
		var r struct{}
		if err := d.call("Input.dispatchKeyEvent", p, &r); err != nil {
			t.Fatal(err)
		}
	}
	if got := selection(); !strings.Contains(got, "Second line of the body") || strings.Contains(got, "Sheet title") || strings.Contains(got, "Close") {
		t.Errorf("select all in the sheet selects %q, want its selectable text alone", got)
	}
}
