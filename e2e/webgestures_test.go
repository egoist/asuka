//go:build e2e

package e2e

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The web host's gestures, in headless Chrome with DevTools' mouse:
// a List's row dragged to another place (on move), the row views kept
// (none bound again); a double click (on doubleTap) that presses nothing
// while a single click presses once the double-click time has passed; a
// zoomable Scroll zoomed with ctrl and the wheel, and by a double click;
// a horizontal pan (panAxis, panMinDistance) that a vertical drag isn't.

const gesturesApp = `app {
  Stack { Home() }
}

struct Item {
  id: int
  name: string
}

screen Home {
  title: "Gestures"
  state page = 0

  Column {
    grow: 1
    Segmented(["Rows", "Taps", "Zoom", "Pan"]) {
      bind selected: page
      margin: [8, 16]
    }
    if page == 0 { Rows() { grow: 1 } }
    if page == 1 { Taps() { grow: 1 } }
    if page == 2 { Zoom() { grow: 1 } }
    if page == 3 { Pan() { grow: 1 } }
  }
}

component Rows {
  state items = [
    Item(id: 1, name: "One"), Item(id: 2, name: "Two"), Item(id: 3, name: "Three"),
    Item(id: 4, name: "Four"), Item(id: 5, name: "Five"), Item(id: 6, name: "Six"),
  ]
  state last = "none"

  Column {
    grow: 1
    Text("order {items.map((f) => f.name).joined(",")}")
    Text("moved {last}")
    List {
      grow: 1
      on move(from, to) {
        items.insert(items.removeAt(from), at: to)
        last = "{from} to {to}"
      }
      for f in items key f.id {
        Text(f.name) { padding: [12, 16] }
      }
    }
  }
}

component Taps {
  state likes = 0
  state presses = 0

  Column {
    padding: 16
    gap: 16
    View {
      height: 160
      background: .secondaryBackground
      label: "Target"
      on press { presses += 1 }
      on doubleTap(x, y) { likes += 1 }
    }
    Text("likes {likes} presses {presses}")
  }
}

component Zoom {
  state zoom = 1.0

  Column {
    grow: 1
    Text("zoom {(zoom * 100).rounded()}")
    Scroll {
      grow: 1
      minZoom: 1
      maxZoom: 4
      bind zoom: zoom
      label: "Zoomable"
      View { width: 100%; height: 300; background: .accent }
    }
  }
}

component Pan {
  state pans = 0

  Column {
    padding: 16
    gap: 16
    View {
      height: 120
      background: .accent
      panAxis: .horizontal
      panMinDistance: 16
      label: "Pannable"
      on pan(g) { if g.state == .ended { pans += 1 } }
    }
    Text("pans {pans}")
  }
}
`

// gestMouse sends one of DevTools' mouse events (the left button).
func gestMouse(t *testing.T, d *devtools, typ string, x, y float64, extra map[string]any) {
	t.Helper()
	p := map[string]any{"type": typ, "x": x, "y": y, "button": "left", "clickCount": 1}
	if typ == "mouseMoved" {
		p["button"] = "none"
	}
	for k, v := range extra {
		p[k] = v
	}
	var r struct{}
	if err := d.call("Input.dispatchMouseEvent", p, &r); err != nil {
		t.Fatal(err)
	}
}

// gestDrag presses at x0, y0, moves to x1, y1 in steps and lets go.
func gestDrag(t *testing.T, d *devtools, x0, y0, x1, y1 float64) {
	t.Helper()
	gestMouse(t, d, "mouseMoved", x0, y0, nil)
	gestMouse(t, d, "mousePressed", x0, y0, map[string]any{"buttons": 1})
	const steps = 12
	for i := 1; i <= steps; i++ {
		f := float64(i) / steps
		gestMouse(t, d, "mouseMoved", x0+(x1-x0)*f, y0+(y1-y0)*f, map[string]any{"button": "left", "buttons": 1})
		time.Sleep(16 * time.Millisecond)
	}
	gestMouse(t, d, "mouseReleased", x1, y1, nil)
}

// gestClick clicks at x, y, the click count the system would give it (2: a double click's second).
func gestClick(t *testing.T, d *devtools, x, y float64, count int) {
	t.Helper()
	gestMouse(t, d, "mousePressed", x, y, map[string]any{"clickCount": count, "buttons": 1})
	gestMouse(t, d, "mouseReleased", x, y, map[string]any{"clickCount": count})
}

// gestAt is the middle of the view labelled l.
func gestAt(t *testing.T, d *devtools, l string) (float64, float64) {
	t.Helper()
	var p []float64
	pageEval(d, `(() => { const e = document.querySelector('[aria-label="`+l+`"]'); if (!e) return []; const r = e.getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`, &p)
	if len(p) != 2 {
		t.Fatalf("no view labelled %q", l)
	}
	return p[0], p[1]
}

func gestText(t *testing.T, d *devtools, text string) (float64, float64) {
	t.Helper()
	for range 50 {
		if x, y, ok := textCenter(d, text); ok {
			return x, y
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("no %q", text)
	return 0, 0
}

func gestBody(d *devtools) string {
	var body string
	pageEval(d, "document.body.innerText", &body)
	return body
}

func TestGesturesWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"Gestures\"\nid = \"dev.asuka.webgestures\"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(gesturesApp), 0o644); err != nil {
		t.Fatal(err)
	}
	d := devtoolsPage(t, browser, webServer(t, dir), "1")
	waitText(t, d, "order One,Two,Three,Four,Five,Six")

	// A row dragged down past two others goes there; each row view keeps its item.
	cells := `[...document.querySelectorAll(".asuka-row:not([hidden])")].map((r) => r.querySelector(".asuka-cell").asukaNode.id + ":" + r.textContent.trim()).sort().join(",")`
	var before, after string
	pageEval(d, cells, &before)
	x, y := gestText(t, d, "One")
	_, y3 := gestText(t, d, "Three")
	gestDrag(t, d, x, y, x, y3+4)
	waitText(t, d, "order Two,Three,One,Four,Five,Six")
	waitText(t, d, "moved 0 to 2")
	time.Sleep(300 * time.Millisecond)
	pageEval(d, cells, &after)
	if before != after {
		t.Errorf("rows bound again after a move:\nbefore %s\nafter  %s", before, after)
	}
	// Alt and the up arrow on a focused row moves it up one.
	pageEval(d, `(() => { const r = [...document.querySelectorAll(".asuka-row:not([hidden])")].find((r) => r.textContent.trim() === "Four"); r.focus(); return true; })()`, new(bool))
	for _, typ := range []string{"rawKeyDown", "keyUp"} {
		var r struct{}
		if err := d.call("Input.dispatchKeyEvent", map[string]any{"type": typ, "key": "ArrowUp", "code": "ArrowUp", "windowsVirtualKeyCode": 38, "modifiers": 1}, &r); err != nil {
			t.Fatal(err)
		}
	}
	waitText(t, d, "order Two,Three,Four,One,Five,Six")

	// A single click presses once a second can't come; a double click likes, and presses nothing.
	x, y = gestText(t, d, "Taps")
	pageClick(d, x, y)
	waitText(t, d, "likes 0 presses 0")
	x, y = gestAt(t, d, "Target")
	gestClick(t, d, x, y, 1)
	time.Sleep(100 * time.Millisecond)
	if b := gestBody(d); !strings.Contains(b, "likes 0 presses 0") {
		t.Errorf("a click with a double tap listened for pressed at once: %q", b)
	}
	waitText(t, d, "likes 0 presses 1")
	gestClick(t, d, x, y, 1)
	time.Sleep(50 * time.Millisecond)
	gestClick(t, d, x, y, 2)
	waitText(t, d, "likes 1 presses 1")
	time.Sleep(700 * time.Millisecond)
	if b := gestBody(d); !strings.Contains(b, "likes 1 presses 1") {
		t.Errorf("a double click pressed too: %q", b)
	}

	// ctrl and the wheel zoom the Scroll in; a double click zooms it back out, and another in to 200%.
	x, y = gestText(t, d, "Zoom")
	pageClick(d, x, y)
	waitText(t, d, "zoom 100")
	x, y = gestAt(t, d, "Zoomable")
	for range 6 {
		gestMouse(t, d, "mouseWheel", x, y, map[string]any{"deltaX": 0, "deltaY": -20, "modifiers": 2})
		time.Sleep(16 * time.Millisecond)
	}
	waitText(t, d, "zoom 331", "zoom 332")
	gestClick(t, d, x, y, 1)
	time.Sleep(50 * time.Millisecond)
	gestClick(t, d, x, y, 2)
	waitText(t, d, "zoom 100")
	time.Sleep(200 * time.Millisecond)
	gestClick(t, d, x, y, 1)
	time.Sleep(50 * time.Millisecond)
	gestClick(t, d, x, y, 2)
	waitText(t, d, "zoom 200")

	// A horizontal drag pans; a vertical one, or one shorter than panMinDistance, doesn't.
	x, y = gestText(t, d, "Pan")
	pageClick(d, x, y)
	waitText(t, d, "pans 0")
	x, y = gestAt(t, d, "Pannable")
	gestDrag(t, d, x-80, y, x+80, y+4)
	waitText(t, d, "pans 1")
	gestDrag(t, d, x, y-40, x+4, y+40)
	gestDrag(t, d, x, y, x+12, y)
	time.Sleep(300 * time.Millisecond)
	if b := gestBody(d); !strings.Contains(b, "pans 1") {
		t.Errorf("a vertical or short drag panned: %q", b)
	}
}
