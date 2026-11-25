//go:build e2e

package e2e

import (
	"fmt"
	"testing"
)

// TestWindowDragWeb drags a Window's panel in headless Chrome
// (e2e/testdata/windowdrag). Its corner, dragged past its minWidth and
// back to where it pressed, leaves it as wide as it was, the corner
// under the pointer: the drag follows where the pointer is, not each
// step it takes. Its title dragged with the right button doesn't move it,
// and a cancelled touch on it ends its drag.
func TestWindowDragWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/windowdrag")
	d := devtoolsPage(t, browser, url, "1")
	waitText(t, d, "Sodium 139 mmol/L")
	settle(t, d)
	// rect is where the panel's part sel is: x, y, width, height.
	rect := func(sel string) [4]float64 {
		t.Helper()
		var r []float64
		js := fmt.Sprintf(`(() => { const e = document.querySelector(".asuka-window %s"); if (!e) return []; const r = e.getBoundingClientRect(); return [r.x, r.y, r.width, r.height]; })()`, sel)
		if err := pageEval(d, js, &r); err != nil || len(r) != 4 {
			t.Fatalf("no %s in the window's panel: %v", sel, err)
		}
		return [4]float64{r[0], r[1], r[2], r[3]}
	}
	mouse := func(typ string, x, y float64, button string) {
		t.Helper()
		buttons := map[string]int{"none": 0, "left": 1, "right": 2}[button]
		if typ == "mouseReleased" {
			buttons = 0
		}
		var r struct{}
		if err := d.call("Input.dispatchMouseEvent", map[string]any{"type": typ, "x": x, "y": y, "button": button, "buttons": buttons, "clickCount": 1}, &r); err != nil {
			t.Fatal(err)
		}
	}
	// drag presses button at x, y, moves by dx, dy in steps (and back to
	// x, y, if back), and lets go.
	drag := func(button string, x, y, dx, dy float64, back bool) {
		t.Helper()
		mouse("mouseMoved", x, y, "none")
		mouse("mousePressed", x, y, button)
		for i := 1; i <= 10; i++ {
			mouse("mouseMoved", x+dx*float64(i)/10, y+dy*float64(i)/10, button)
		}
		ex, ey := x+dx, y+dy
		if back {
			for i := 1; i <= 10; i++ {
				mouse("mouseMoved", x+dx-dx*float64(i)/10, y+dy-dy*float64(i)/10, button)
			}
			ex, ey = x, y
		}
		mouse("mouseReleased", ex, ey, button)
	}

	win := rect("")
	if win[2] != 300 {
		t.Fatalf("the panel is %v wide, want 300", win[2])
	}
	grip := rect(".asuka-window-grip")
	gx, gy := grip[0]+grip[2]/2, grip[1]+grip[3]/2
	drag("left", gx, gy, -200, 0, true)
	if got := rect(""); got[2] != 300 {
		t.Errorf("its corner dragged 200 points in, past its minWidth of 200, and back, the panel is %v wide, want 300", got[2])
	}
	if g := rect(".asuka-window-grip"); g[0]+g[2]/2 != gx {
		t.Errorf("its corner is at x %v, the pointer at %v", g[0]+g[2]/2, gx)
	}

	win = rect("")
	bar := rect(".asuka-window-bar")
	drag("right", bar[0]+40, bar[1]+bar[3]/2, 60, 80, false)
	if got := rect(""); got[0] != win[0] || got[1] != win[1] {
		t.Errorf("its title dragged with the right button moved it from %v, %v to %v, %v", win[0], win[1], got[0], got[1])
	}

	// A touch on its title that the browser cancels ends the drag: the
	// mouse moving over the title after it doesn't move the panel.
	win, bar = rect(""), rect(".asuka-window-bar")
	x, y := bar[0]+40, bar[1]+bar[3]/2
	var r struct{}
	for _, ev := range []map[string]any{
		{"type": "touchStart", "touchPoints": []map[string]any{{"x": x, "y": y}}},
		{"type": "touchCancel", "touchPoints": []map[string]any{}},
	} {
		if err := d.call("Input.dispatchTouchEvent", ev, &r); err != nil {
			t.Fatal(err)
		}
	}
	mouse("mouseMoved", x+30, y, "none")
	mouse("mouseMoved", x+60, y, "none")
	if got := rect(""); got[0] != win[0] || got[1] != win[1] {
		t.Errorf("after a cancelled touch on its title, the mouse moving over it moved it from %v, %v to %v, %v", win[0], win[1], got[0], got[1])
	}
}
