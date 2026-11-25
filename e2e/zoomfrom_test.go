//go:build e2e

package e2e

import (
	"strings"
	"testing"
	"time"
)

// A screen with zoomFrom grows out of the view on the screen below that
// shares its key, and goes back into it (e2e/testdata/zoomfrom): pushed,
// it's drawn scaled down to the tile's place and clipped to it, the tile
// hidden while the screen stands for it; done, the tile shows again and
// the screen is whole. Popped, it shrinks back into the tile.

// zoomState is what the page shows of a zoom under way: whether a
// screen's animation clips it to an inset, and how many shared views
// are hidden for it.
func zoomState(t *testing.T, d *devtools) (clipping bool, hidden int) {
	t.Helper()
	var s struct {
		Clipping bool
		Hidden   int
	}
	js := `({
		clipping: document.getAnimations().some((a) => (a.effect?.getKeyframes?.() || []).some((k) => /inset\(\d/.test(k.clipPath || "") && k.clipPath !== "inset(0px 0px 0px 0px round 0px)")),
		hidden: [...document.querySelectorAll("[data-asuka-shared]")].filter((e) => e.style.visibility === "hidden").length,
	})`
	if err := pageEval(d, js, &s); err != nil {
		t.Fatal(err)
	}
	return s.Clipping, s.Hidden
}

// untilZoom waits (bounded) for a zoom under way, and checks the tile
// hides for it.
func untilZoom(t *testing.T, d *devtools, what string) {
	t.Helper()
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		if clipping, hidden := zoomState(t, d); clipping {
			if hidden != 1 {
				t.Errorf("%s: %d shared views hidden, want the tile alone", what, hidden)
			}
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("%s: no zoom: the screen slid", what)
}

func TestZoomFromWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/zoomfrom")
	d := devtoolsPage(t, browser, url, "1")
	waitText(t, d, "Tile 3")
	pressText(t, d, "Tile 3")
	untilZoom(t, d, "the push")
	waitText(t, d, "Detail 3")
	settle(t, d)
	if clipping, hidden := zoomState(t, d); clipping || hidden != 0 {
		t.Errorf("after the push: clipping %v, %d shared views hidden", clipping, hidden)
	}
	// The bar's back button: an icon, its label "Back".
	var at []float64
	if err := pageEval(d, `(() => { const b = [...document.querySelectorAll('[aria-label="Back"]')].find((e) => e.getClientRects().length); if (!b) return []; const r = b.getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`, &at); err != nil || len(at) != 2 {
		t.Fatalf("no back button: %v", err)
	}
	if err := pageClick(d, at[0], at[1]); err != nil {
		t.Fatal(err)
	}
	untilZoom(t, d, "the pop")
	settle(t, d)
	var body string
	pageEval(d, "document.body.innerText", &body)
	if strings.Contains(body, "Detail 3") {
		t.Error("the screen stayed after the pop")
	}
	if _, hidden := zoomState(t, d); hidden != 0 {
		t.Errorf("after the pop: %d shared views hidden", hidden)
	}
}
