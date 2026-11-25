//go:build e2e

package e2e

import (
	"math"
	"testing"
	"time"
)

// A text field's padding is added to it once, and it's measured at its
// own size in a view drawn under a transform (e2e/testdata/fieldmeasure).
// A field with padding of 10 points above and below is as high as the
// plain one less the input's own padding, which an app's replaces, plus
// 20, one line or multiline; drawn at half its size, a field is laid out
// as high as the plain one; and a multiline field with padding at its
// sides is as high as its paragraph's lines at its width.

// fieldHeights is each field's height as laid out, its input's own
// padding above and below, and its input's height at its width with no
// height of its own (its text's lines, padding and border), in points,
// by its placeholder.
type fieldHeights map[string][3]float64

func readFieldHeights(d *devtools) (fieldHeights, bool) {
	js := `(() => { const out = {}; for (const b of document.querySelectorAll(".asuka-field-box")) { const i = b.querySelector(".asuka-field"); const cs = getComputedStyle(i); const saved = i.style.height; i.style.height = "0px"; const lines = i.scrollHeight + parseFloat(cs.borderTopWidth) + parseFloat(cs.borderBottomWidth); i.style.height = saved; out[i.placeholder] = [parseFloat(getComputedStyle(b).height), parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom), lines]; } return out; })()`
	var f fieldHeights
	if pageEval(d, js, &f) != nil {
		return nil, false
	}
	for _, p := range []string{"Plain", "Padded", "Plain lines", "Padded lines", "Half size", "Paragraph"} {
		if f[p][0] == 0 {
			return nil, false
		}
	}
	return f, true
}

// checkFieldMeasure waits (bounded) until the fields are laid out and
// checks their heights, to a point.
func checkFieldMeasure(t *testing.T, d *devtools) {
	t.Helper()
	deadline := time.Now().Add(within)
	var f fieldHeights
	var ok bool
	for time.Now().Before(deadline) {
		if f, ok = readFieldHeights(d); ok {
			break
		}
		time.Sleep(300 * time.Millisecond)
	}
	if !ok {
		t.Fatal("the fields never showed")
	}
	t.Logf("fields (height, own padding, lines): %v", f)
	for _, c := range []struct{ padded, plain string }{{"Padded", "Plain"}, {"Padded lines", "Plain lines"}} {
		want := f[c.plain][0] - f[c.plain][1] + 20
		if got := f[c.padded][0]; math.Abs(got-want) > 1 {
			t.Errorf("%q is %.1f high, want %.1f: its padding is added more than once", c.padded, got, want)
		}
	}
	if got, want := f["Half size"][0], f["Plain"][0]; math.Abs(got-want) > 1 {
		t.Errorf("drawn at half its size, the field is laid out %.1f high, want %.1f: it was measured as drawn", got, want)
	}
	if got, want := f["Paragraph"][0], f["Paragraph"][2]; math.Abs(got-want) > 1 {
		t.Errorf("the paragraph's field is %.1f high, want its lines' %.1f: it was measured narrower by its padding", got, want)
	}
}

func TestFieldMeasureWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/fieldmeasure")
	for _, scale := range []string{"1", "2"} {
		t.Run(scale+"x", func(t *testing.T) {
			checkFieldMeasure(t, devtoolsPage(t, browser, url, scale))
		})
	}
}
