package windows_test

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"testing"
)

// frameOf is the frame (x, y, w, h, points: pixels at 96 DPI) the dump
// gives a view showing text.
func frameOf(t *testing.T, dump, text string) (x, y, w, h float64) {
	t.Helper()
	re := regexp.MustCompile(`(?m)^\s*\d+ \S+ \[(-?[\d.]+),(-?[\d.]+) ([\d.]+)x([\d.]+)\] ` + regexp.QuoteMeta(fmt.Sprintf("%q", text)))
	m := re.FindStringSubmatch(dump)
	if m == nil {
		t.Fatalf("no view shows %q:\n%s", text, dump)
	}
	f := func(s string) float64 { v, _ := strconv.ParseFloat(s, 64); return v }
	return f(m[1]), f(m[2]), f(m[3]), f(m[4])
}

// The text and look styles the host draws itself (testdata/styles.asuka):
// a text's letter spacing, line height and shrinking measured as they're
// drawn, monospaced digits, a shadow past its text's window, a corner's
// own radius, a side's border, a dashed border, radial and conic
// gradients, a per-corner clip and borders of their own widths.
func TestStyles(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "styles.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe", "wait:800ms", "snap:styled")
	dump := d["styled"]
	if out := os.Getenv("ASUKA_STYLES_OUT"); out != "" {
		b, _ := os.ReadFile(filepath.Join(local, "styled.png"))
		os.WriteFile(out, b, 0o644)
	}
	// Letter spacing: four points after each of six characters.
	_, _, spaced, _ := frameOf(t, dump, "Spaced")
	re := regexp.MustCompile(`(?m)^\s*\d+ \S+ \[[\d.]+,([\d.]+) ([\d.]+)x([\d.]+)\] "Spaced"`)
	all := re.FindAllStringSubmatch(dump, -1)
	if len(all) != 2 {
		t.Fatalf("two Spaced texts, not %d:\n%s", len(all), dump)
	}
	plain, _ := strconv.ParseFloat(all[1][2], 64)
	if diff := spaced - plain; diff < 22 || diff > 26 {
		t.Errorf("letter spacing 4 widens six characters by %g, want 24 (%g, %g)", diff, spaced, plain)
	}
	// Line height: whole lines of 40.
	_, _, _, tall := frameOf(t, dump, "Lines of text at a tall line height")
	if tall < 80 || int(tall)%40 != 0 {
		t.Errorf("lines 40 apart: %g tall", tall)
	}
	// Shrinking: one line, shorter than a line at its size; the same text
	// wraps without it.
	_, _, sw, sh := frameOf(t, dump, "Shrinks to fit a narrow width")
	_, _, _, wh := frameOf(t, dump, "Wraps at a narrow width")
	if sw > 100 || sh >= wh/2+1 || sh < wh/4 {
		t.Errorf("shrunk to one line: %gx%g (a wrapped text is %g tall)", sw, sh, wh)
	}
	// Truncated to one line.
	for _, s := range []string{"Head: a title that doesn't fit", "Middle: a title that doesn't fit"} {
		if _, _, tw, th := frameOf(t, dump, s); tw > 120 || th > wh/2+1 {
			t.Errorf("%s: one line, %gx%g", s, tw, th)
		}
	}
	// Monospaced digits: as wide, whatever the digits.
	_, _, ones, _ := frameOf(t, dump, "1111")
	_, _, zeros, _ := frameOf(t, dump, "0000")
	if ones != zeros {
		t.Errorf("monospaced digits: 1111 is %g wide, 0000 %g", ones, zeros)
	}
	// A selectable text's (a RichText's) line height.
	if _, _, _, ph := frameOf(t, dump, "Picked"); ph != 30 {
		t.Errorf("a selectable text of line height 30 is %g tall", ph)
	}
	img := picture(t, local, "styled")
	type check struct {
		what string
		x, y int
		ok   func(r, g, b int) bool
	}
	red := func(r, g, b int) bool { return r > 180 && g < 90 && b < 90 }
	blue := func(r, g, b int) bool { return b > 180 && r < 90 && g < 90 }
	green := func(r, g, b int) bool { return g > 180 && r < 90 && b < 90 }
	dark := func(r, g, b int) bool { return r < 120 && g < 120 && b < 120 }
	behind := func(r, g, b int) bool { return r > 225 && g > 225 && b > 225 }
	sx, sy, sww, shh := frameOf(t, dump, "Shadow")
	for _, c := range []check{
		{"the rounded top left corner", 303, 19, behind},
		{"the square top right corner", 377, 19, red},
		{"the square bottom left corner", 303, 93, red},
		{"the bottom border", 440, 93, blue},
		{"no top border", 440, 18, behind},
		{"no left border", 401, 56, behind},
		{"the radial gradient's center", 640, 56, red},
		{"the radial gradient's corner", 602, 18, blue},
		{"the conic gradient's start, past the top", 342, 122, red},
		{"the conic gradient's end, before the top", 337, 122, blue},
		{"the clip's square corner", 402, 122, green},
		{"the clip's round corner", 477, 197, behind},
		{"the thick left border", 503, 160, dark},
		{"the thin top border's inside", 540, 124, behind},
	} {
		if r, g, b := img.at(c.x, c.y); !c.ok(r, g, b) {
			t.Errorf("%s at %d, %d: %d %d %d", c.what, c.x, c.y, r, g, b)
		}
	}
	// The text's shadow, three points right of its glyphs: past its
	// window's right edge.
	shadowed := false
	for x := int(sx + sww); x < int(sx+sww)+4; x++ {
		for y := int(sy); y < int(sy+shh); y++ {
			if r, g, b := img.at(x, y); !behind(r, g, b) {
				shadowed = true
			}
		}
	}
	if !shadowed {
		t.Errorf("no shadow past the right of the text at %g,%g %gx%g", sx, sy, sww, shh)
	}
	// The dashed border: dashes and gaps along its top.
	changes, last := 0, false
	for x := 504; x < 576; x++ {
		r, g, b := img.at(x, 17)
		on := dark(r, g, b)
		if x > 504 && on != last {
			changes++
		}
		last = on
	}
	if changes < 6 {
		t.Errorf("a dashed top border changes %d times between dash and gap, want 6 or more", changes)
	}
	if t.Failed() {
		t.Logf("%s", dump)
	}
}
