//go:build e2e

package e2e

import (
	"encoding/json"
	"testing"
)

// TestLaunchColorWeb loads e2e/testdata/launchcolor in headless Chrome
// with the core's wasm held back: while it's held, the page's root and
// its container are the splash's color. Once the app's first frame is
// drawn, the container is asuka-drawn, and both are as without a splash.
func TestLaunchColorWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	pageURL := webServer(t, "e2e/testdata/launchcolor")
	c := launchCDP(t, browser)
	var created struct{ TargetID string }
	if err := c.call("", "Target.createTarget", map[string]any{"url": "about:blank"}, &created, ""); err != nil {
		t.Fatal(err)
	}
	page := c.attach(t, created.TargetID)
	var res json.RawMessage
	if err := c.call(page, "Fetch.enable", map[string]any{"patterns": []map[string]any{{"urlPattern": "*.wasm*"}}}, &res, ""); err != nil {
		t.Fatal(err)
	}
	if err := c.call(page, "Page.navigate", map[string]any{"url": pageURL}, &res, ""); err != nil {
		t.Fatal(err)
	}
	held := func() bool {
		c.mu.Lock()
		defer c.mu.Unlock()
		for _, e := range c.events {
			if e.SessionID == page && e.Method == "Fetch.requestPaused" {
				return true
			}
		}
		return false
	}
	until(t, held, func() string { return "the page never asked for the core's wasm" })
	colors := func() string {
		return c.eval(t, page, `(() => { const p = document.querySelector(".asuka-page"); return p ? getComputedStyle(document.documentElement).backgroundColor + ", " + getComputedStyle(p).backgroundColor : ""; })()`)
	}
	until(t, func() bool { return colors() != "" }, func() string { return "the page has no container" })
	if got, want := colors(), "rgb(74, 99, 181), rgb(74, 99, 181)"; got != want {
		t.Errorf("loading, the page's root and container are %s, want the splash's #4a63b5: %s", got, want)
	}

	// Disabled, the Fetch domain lets what it holds go.
	if err := c.call(page, "Fetch.disable", map[string]any{}, &res, ""); err != nil {
		t.Fatal(err)
	}
	ready := func() bool {
		return c.eval(t, page, `String(document.body.innerText.includes("status: ready"))`) == "true"
	}
	until(t, ready, func() string { return c.eval(t, page, "document.body.innerText") })
	if got := c.eval(t, page, `String(document.querySelector(".asuka-page").classList.contains("asuka-drawn"))`); got != "true" {
		t.Errorf("drawn, the page's container isn't asuka-drawn")
	}
	if got, want := colors(), "rgba(0, 0, 0, 0), rgb(255, 255, 255)"; got != want {
		t.Errorf("drawn, the page's root and container are %s, want %s, as without a splash", got, want)
	}
}
