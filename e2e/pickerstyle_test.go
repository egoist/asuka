//go:build e2e

package e2e

import (
	"bytes"
	"encoding/json"
	"fmt"
	"image"
	"image/png"
	"testing"
	"time"
)

// A picker's background is the app's (e2e/testdata/pickerstyle): a red
// picker 200 by 40 points is red across nearly all of it, its text and
// chevron aside, not the platform's own field over it. It's measured on
// the screen, against a magenta square 48 points high. The box draws the
// chevron, the hairline and the focus ring, so the chevron follows a
// right-to-left layout and the whole box dims when disabled. The ring
// stays with an app's shadow and inside a box that clips, the select
// fills the box, the app's text color reaches the select but not the list
// it opens, an app's border leaves the chevron where it is, and in forced
// colors the chevron is drawn in the text's color.

// redShare is how much of the 200 by 40 point picker is red, from the
// red pixels' count and the magenta square's scale; false while either
// isn't on screen.
func redShare(img image.Image) (float64, bool) {
	b := img.Bounds()
	red, magTop, magBottom := 0, -1, -1
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			r, g, bl, _ := img.At(x, y).RGBA()
			r, g, bl = r>>8, g>>8, bl>>8
			switch {
			case r > 200 && g < 60 && bl < 60:
				red++
			case r > 200 && g < 70 && bl > 200:
				if magTop < 0 {
					magTop = y
				}
				magBottom = y
			}
		}
	}
	if magTop < 0 || red == 0 {
		return 0, false
	}
	k := float64(magBottom-magTop+1) / 48
	return float64(red) / (200 * 40 * k * k), true
}

// checkPickerStyle captures the screen with shot until the picker is on
// it (bounded) and checks it's red across at least 80% of it.
func checkPickerStyle(t *testing.T, shot func() ([]byte, error)) {
	t.Helper()
	deadline := time.Now().Add(within)
	var share float64
	var seen bool
	var last error
	for time.Now().Before(deadline) {
		b, err := shot()
		if err == nil {
			var img image.Image
			if img, err = png.Decode(bytes.NewReader(b)); err == nil {
				if share, seen = redShare(img); seen {
					break
				}
				err = fmt.Errorf("the picker or the square isn't on screen")
			}
		}
		last = err
		time.Sleep(300 * time.Millisecond)
	}
	if !seen {
		t.Fatalf("the picker never showed: %v", last)
	}
	t.Logf("the picker is %.0f%% red", share*100)
	if share < 0.8 {
		t.Errorf("the picker is %.0f%% red, want 80%% or more: the platform's field covers the app's background", share*100)
	}
}

// pickerParts is what the page says of the pickers below the red one.
type pickerParts struct {
	Chevron, Width      float64 // the right-to-left picker's chevron's left edge, and its width
	PadLeft, PadRight   float64 // its select's padding
	Opacity, SelOpacity string  // the disabled picker's box's, and its select's
	Ring                string  // the outline around the shadowed picker's box, its select focused from the keyboard
	Ink                 string  // the color of the select's text in the picker with the app's color
	Edge, EdgeWidth     float64 // the bordered picker's chevron's left edge, and its width
	Line                string  // the bordered picker's box's outline color
	Tall, TallBox       float64 // the red picker's select's height, and its box's
	ClipRing            string  // the ring of the clipping picker, focused from the keyboard: its style and offset
	ListInk             string  // the color of the options in the picker with the app's color
}

// pickerEval runs js in the page until it gives a value (bounded) and
// decodes it into v.
func pickerEval(t *testing.T, page *devtools, js string, v any) {
	t.Helper()
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		var raw json.RawMessage
		if err := pageEval(page, js, &raw); err == nil && string(raw) != "null" && json.Unmarshal(raw, v) == nil {
			return
		}
		time.Sleep(300 * time.Millisecond)
	}
	t.Fatal("the pickers never showed")
}

// pickerBoxJS defines box(v) in the page: the picker box whose select's
// value is v.
const pickerBoxJS = `const box = (v) => [...document.querySelectorAll(".asuka-select-box")].find((b) => b.querySelector("select")?.value === v);`

// checkPickerParts checks a right-to-left picker's chevron is at its
// left, with its text's room beside it; a disabled picker's box is
// dimmed whole, and its text not again; a picker with the app's shadow,
// focused from the keyboard, has its box ringed; the app's text color
// reaches the select, not its list; a picker with the app's border draws
// it with its chevron still at its end; the select fills its box; and a
// clipping picker's ring is drawn inside it.
func checkPickerParts(t *testing.T, page *devtools) {
	t.Helper()
	js := `(() => {
		` + pickerBoxJS + `
		const rtl = box("Right"), off = box("Off"), shadow = box("Shadow"), ink = box("Ink"), edge = box("Border"), red = box("One"), clip = box("Clip");
		if (!rtl || !off || !shadow || !ink || !edge || !red || !clip) return null;
		const edgeAfter = getComputedStyle(edge, "::after");
		const after = getComputedStyle(rtl, "::after"), sel = getComputedStyle(rtl.querySelector("select"));
		clip.querySelector("select").focus({ focusVisible: true });
		const clipRing = getComputedStyle(clip, "::before"), clipRingIs = clipRing.outlineStyle + " " + clipRing.outlineOffset;
		shadow.querySelector("select").focus({ focusVisible: true });
		return {
			Chevron: after.content === "none" ? -1 : parseFloat(after.left), Width: rtl.getBoundingClientRect().width,
			PadLeft: parseFloat(sel.paddingLeft), PadRight: parseFloat(sel.paddingRight),
			Opacity: getComputedStyle(off).opacity, SelOpacity: getComputedStyle(off.querySelector("select")).opacity, Ring: getComputedStyle(shadow, "::before").outlineStyle,
			Ink: getComputedStyle(ink.querySelector("select")).color,
			Edge: edgeAfter.content === "none" ? -1 : parseFloat(edgeAfter.left), EdgeWidth: edge.getBoundingClientRect().width,
			Line: getComputedStyle(edge).outlineColor,
			Tall: red.querySelector("select").getBoundingClientRect().height, TallBox: red.getBoundingClientRect().height,
			ClipRing: clipRingIs,
			ListInk: getComputedStyle(ink.querySelector("option")).color,
		};
	})()`
	var p pickerParts
	pickerEval(t, page, js, &p)
	if p.Chevron < 0 || p.Chevron > p.Width/2 {
		t.Errorf("right to left, the chevron is at %v in a picker %v wide, want it at the left", p.Chevron, p.Width)
	}
	if p.PadLeft <= p.PadRight {
		t.Errorf("right to left, the select's padding is %v left and %v right, want the chevron's room at the left", p.PadLeft, p.PadRight)
	}
	if p.Opacity != "0.5" || p.SelOpacity != "1" {
		t.Errorf("a disabled picker's box has opacity %s and its select %s, want 0.5 and 1: dimmed once, whole", p.Opacity, p.SelOpacity)
	}
	if p.Ring != "solid" {
		t.Errorf("a picker with the app's shadow, focused from the keyboard, has no ring around its box (outline %q)", p.Ring)
	}
	if p.Ink != "rgb(0, 0, 255)" {
		t.Errorf("the app's text color is #0000ff, the select's is %s", p.Ink)
	}
	if p.Edge < p.EdgeWidth/2 || p.Line != "rgb(0, 0, 255)" {
		t.Errorf("with the app's #0000ff border, the chevron is at %v in a picker %v wide and the box's outline is %s, want the chevron at the right and the outline the app's", p.Edge, p.EdgeWidth, p.Line)
	}
	if p.Tall != p.TallBox {
		t.Errorf("a picker %v tall has a select %v tall: a press on the rest of it opens nothing", p.TallBox, p.Tall)
	}
	if p.ClipRing != "solid -2px" {
		t.Errorf("a clipping picker, focused from the keyboard, has its ring %q, want \"solid -2px\": inside the box, not cut off", p.ClipRing)
	}
	if p.ListInk == "rgb(0, 0, 255)" {
		t.Errorf("the options of a picker with the app's text color are in it too, %s, not the page's", p.ListInk)
	}
}

// checkPickerForced checks that in forced colors (as in Windows high
// contrast), emulated, the chevron is drawn in the text's color, not
// the background's.
func checkPickerForced(t *testing.T, page *devtools) {
	t.Helper()
	var r struct{}
	if err := page.call("Emulation.setEmulatedMedia", map[string]any{"features": []map[string]string{{"name": "forced-colors", "value": "active"}}}, &r); err != nil {
		t.Fatal(err)
	}
	js := `(() => {
		if (!matchMedia("(forced-colors: active)").matches) return null;
		` + pickerBoxJS + `
		const plain = box("One");
		if (!plain) return null;
		const probe = (c) => { const d = document.createElement("div"); d.style.color = c; document.body.appendChild(d); const v = getComputedStyle(d).color; d.remove(); return v; };
		return { Chevron: getComputedStyle(plain, "::after").backgroundColor, Text: probe("CanvasText"), Canvas: probe("Canvas") };
	})()`
	var f struct{ Chevron, Text, Canvas string }
	pickerEval(t, page, js, &f)
	if f.Chevron != f.Text || f.Chevron == f.Canvas {
		t.Errorf("in forced colors the chevron is %s, want the text's %s on the background's %s", f.Chevron, f.Text, f.Canvas)
	}
}

func TestPickerStyleWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/pickerstyle")
	for _, scale := range []string{"1", "2"} {
		t.Run(scale+"x", func(t *testing.T) {
			page := devtoolsPage(t, browser, url, scale)
			checkPickerStyle(t, func() ([]byte, error) { return page.screenshot() })
			checkPickerParts(t, page)
			checkPickerForced(t, page)
		})
	}
}
