package linux_test

import (
	"math"
	"path/filepath"
	"regexp"
	"strconv"
	"testing"
)

// sheetLine is what a dump says of a sheet: its view's size, the detent
// it's at, its height on the window, and how dim an undimmed one's scrim
// is (-1: not a panel).
type sheetLine struct {
	w, h, at, height int
	dim              float64
}

var sheetRe = regexp.MustCompile(`Sheet \[[^\]]*\][^\n]* sheet=(\d+)x(\d+) detent=(\d+) height=(\d+)(?: panel dim=([\d.]+))?`)
var rootRe = regexp.MustCompile(`(?m)^\d+ \S+ \[0,0 (\d+)x(\d+)\]`)

func sheetIn(t *testing.T, dump, where string) (sheetLine, bool) {
	t.Helper()
	m := sheetRe.FindStringSubmatch(dump)
	if m == nil {
		return sheetLine{}, false
	}
	n := func(s string) int { v, _ := strconv.Atoi(s); return v }
	s := sheetLine{w: n(m[1]), h: n(m[2]), at: n(m[3]), height: n(m[4]), dim: -1}
	if m[5] != "" {
		s.dim, _ = strconv.ParseFloat(m[5], 64)
	}
	return s, true
}

func rootHeight(t *testing.T, dump string) float64 {
	t.Helper()
	m := rootRe.FindStringSubmatch(dump)
	if m == nil {
		t.Fatalf("no root in the dump:\n%s", dump)
	}
	h, _ := strconv.Atoi(m[2])
	return float64(h)
}

func near(a, b, by float64) bool { return math.Abs(a-b) <= by }

// Sheets (testdata/sheets.asuka): an undimmed sheet is a panel at its
// detent's height over the window, which still takes clicks at its small
// detents and is dimmed and held off above them; its bound detent moves
// it, a drag settles it at the nearest detent and writes the binding,
// its height is reported as it moves, Escape dismisses it. A sheet as tall
// as its content is sized by the core's Fit, and follows its content. A
// modal one rests at its detent, moves to one set, and Escape dismisses it
// though it can't be dragged away.
func TestSheetDetents(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "sheets.asuka"), exe)
	d := script(t, dir, exe,
		"click:Open maps", "wait:600ms", "snap:maps",
		"click:Tap behind", "snap:behind",
		"tap:Go large", "wait:600ms", "snap:large",
		"click:Tap behind", "snap:blocked",
		"tap:Go small", "wait:600ms", "snap:small",
		"drag:Search Maps,0,-110", "wait:600ms", "snap:dragged",
		"escape", "snap:gone",
		"click:Open fit", "snap:fit",
		"tap:More", "snap:fit2",
		"escape",
		"click:Open modal", "snap:modal",
		"tap:Modal large", "snap:modalLarge",
		"escape", "snap:modalGone")
	wh := rootHeight(t, d["maps"])
	large := math.Max(wh*0.9, math.Min(200, wh))
	medium := math.Max(wh*0.5, math.Min(200, wh))

	s, ok := sheetIn(t, d["maps"], "maps")
	if !ok || s.dim < 0 || s.height != 150 || s.at != 0 || s.dim != 0 {
		t.Fatalf("the maps sheet isn't an undimmed panel 150 tall: %+v\n%s", s, d["maps"])
	}
	shows(t, d["maps"], "maps", []string{"Height: 150", "At: small"})
	shows(t, d["behind"], "a click behind it", []string{"Taps: 1"})

	s, _ = sheetIn(t, d["large"], "large")
	if !near(float64(s.height), large, 1) || s.at != 2 || s.dim != 1 {
		t.Errorf("at large: %+v, want %.0f tall and dimmed", s, large)
	}
	shows(t, d["large"], "large", []string{"At: large", "Height: " + strconv.Itoa(int(math.Round(large)))})
	shows(t, d["blocked"], "a click on the scrim", []string{"Taps: 1"}, "Taps: 2")

	s, _ = sheetIn(t, d["small"], "small")
	if s.height != 150 || s.at != 0 || s.dim != 0 {
		t.Errorf("back at small: %+v", s)
	}
	// Dragged up 110 points from 150: medium is nearer than small.
	s, _ = sheetIn(t, d["dragged"], "dragged")
	if !near(float64(s.height), medium, 1) || s.at != 1 {
		t.Errorf("dragged: %+v, want medium (%.0f)", s, medium)
	}
	shows(t, d["dragged"], "dragged", []string{"At: medium"})
	if _, ok := sheetIn(t, d["gone"], "gone"); ok {
		t.Errorf("Escape left the panel:\n%s", d["gone"])
	}

	f, ok := sheetIn(t, d["fit"], "fit")
	if !ok || f.dim >= 0 || f.h < 60 || float64(f.h) > large/2 {
		t.Fatalf("the fit sheet isn't its content's height: %+v\n%s", f, d["fit"])
	}
	f2, _ := sheetIn(t, d["fit2"], "fit2")
	if f2.h <= f.h || f2.h > f.h+40 {
		t.Errorf("the fit sheet didn't follow a line more: %d then %d", f.h, f2.h)
	}
	shows(t, d["fit2"], "a line more", []string{"Line 3"})

	m, ok := sheetIn(t, d["modal"], "modal")
	if !ok || m.dim >= 0 || m.h != 240 || m.at != 0 {
		t.Fatalf("the modal sheet isn't a dialog 240 tall: %+v\n%s", m, d["modal"])
	}
	m, _ = sheetIn(t, d["modalLarge"], "modalLarge")
	if !near(float64(m.h), large, 1) || m.at != 1 {
		t.Errorf("the modal sheet at large: %+v, want %.0f", m, large)
	}
	if _, ok := sheetIn(t, d["modalGone"], "modalGone"); ok {
		t.Errorf("Escape left the modal sheet that can't be dragged away:\n%s", d["modalGone"])
	}
}
