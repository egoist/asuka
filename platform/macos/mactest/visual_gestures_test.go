//go:build darwin

package mactest

import (
	"fmt"
	"slices"
	"strings"
	"testing"
)

// A double click reports where its second click fell; with on press too, a
// click presses once the double-click interval has passed, and a double
// click doesn't press; a view with on press alone presses at once.
// HoverMove reports the pointer's place once it's over the view.
func TestVisualDoubleClick(t *testing.T) {
	a := Run(t, `
		component Taps {
		  state log = ""
		  Column {
		    padding: 20; gap: 20; align: .start
		    View {
		      width: 200; height: 100
		      on press { log = log + "press;" }
		      on doubleTap(x, y) { log = log + "double {x} {y};" }
		      on hover(x, y) { log = log + "at {x} {y};" }
		    }
		    View { width: 201; height: 100; on doubleTap(x, y) { log = log + "only {x} {y};" } }
		    View { width: 202; height: 100; on press { log = log + "now;" } }
		    Text("log:{log}")
		  }
		}
		app { Taps() }
	`)
	tr := a.UntilText("log:")
	both, only, press := vid(viewSized(t, tr, 200, 100)), vid(viewSized(t, tr, 201, 100)), vid(viewSized(t, tr, 202, 100))
	l := &vlogger{a: a}

	a.Do("mouse down " + press + " 10 10, up " + press + " 10 10")
	l.expect(t, "a press alone", "now")

	a.Do("mouse down " + both + " 10 10, up " + both + " 10 10")
	l.until(t, func(s string) bool { return strings.HasSuffix(s, "press;") }) // after the interval

	a.Do("mouse down " + both + " 30 40, up " + both + " 30 40 0.05, down2 " + both + " 31 42 0.1, up2 " + both + " 31 42 0.15")
	a.Do("wait 0.6")
	l.expect(t, "a double click", "double 31 42")

	a.Do("mouse down " + only + " 5 6, up " + only + " 5 6 0.05, down2 " + only + " 7 8 0.1, up2 " + only + " 7 8 0.15")
	l.expect(t, "a double click alone", "only 7 8")

	// A click elsewhere presses the one that waited at once.
	a.Do("mouse down " + both + " 10 10, up " + both + " 10 10, down " + press + " 10 10, up " + press + " 10 10")
	l.expect(t, "a click elsewhere", "press", "now")

	a.Do("hovermove " + both + " 12 34")
	l.expect(t, "hover", "at 12 34")
	ix := vobj(viewSized(t, a.Tree(), 200, 100), "interaction")
	if opts := int(vnum(ix, "hoverOptions")); opts&0x2 == 0 {
		t.Errorf("the tracking area's options %#x: no mouseMoved", opts)
	}
}

// PanAxis: a drag the other way isn't a pan (its press goes on);
// PanMinDistance: the pan begins once the mouse has gone that far.
func TestVisualPanAxis(t *testing.T) {
	a := Run(t, `
		component Pans {
		  state log = ""
		  Column {
		    padding: 20; gap: 20; align: .start
		    View {
		      width: 200; height: 100
		      panAxis: .horizontal
		      panMinDistance: 20
		      panWithScroll: true
		      on pressedChange(p) { log = log + "pressed {p};" }
		      on pan(g) { log = log + "pan {g.state} {g.translationX};" }
		    }
		    Text("log:{log}")
		  }
		}
		app { Pans() }
	`)
	tr := a.UntilText("log:")
	pn := viewSized(t, tr, 200, 100)
	ix := vobj(pn, "interaction")
	if vnum(ix, "panAxis") != 1 || vnum(ix, "panMinDistance") != 20 || !vbool(ix, "panSimultaneous") {
		t.Fatalf("pan config %v", ix)
	}
	p := vid(pn)
	l := &vlogger{a: a}
	drag := func(dx, dy int) {
		steps := []string{fmt.Sprintf("down %s 50 50 0", p)}
		for i := 1; i <= 10; i++ {
			steps = append(steps, fmt.Sprintf("dragged %s %d %d %v", p, 50+dx*i, 50+dy*i, float64(i)*0.01))
		}
		steps = append(steps, fmt.Sprintf("up %s %d %d 0.12", p, 50+dx*10, 50+dy*10))
		a.Do("mouse " + strings.Join(steps, ", "))
	}
	drag(0, 4) // down: not a pan
	if got := l.next(); slices.ContainsFunc(got, func(s string) bool { return strings.HasPrefix(s, "pan") }) {
		t.Errorf("a vertical drag panned: %q", got)
	}
	drag(4, 0) // across, 40 points
	got := l.next()
	var pans []string
	for _, s := range got {
		if strings.HasPrefix(s, "pan") {
			pans = append(pans, s)
		}
	}
	if len(pans) < 2 || !strings.HasPrefix(pans[0], "pan began ") || pans[len(pans)-1] != "pan ended 40" {
		t.Errorf("a horizontal drag: %q", got)
	}
	var began float64
	fmt.Sscanf(strings.TrimPrefix(pans[0], "pan began "), "%g", &began)
	if began < 20 {
		t.Errorf("the pan began at %v, before its 20 points", began)
	}
}

// A zoomable Scroll: NSScrollView's magnification within MinZoom and
// MaxZoom, starting at 1; ⌘ and the wheel zoom around the pointer, a
// double click zooms in to twice MinZoom and back out, a pinch's end is
// reported; ZoomTo zooms, without the host's report.
func TestVisualZoom(t *testing.T) {
	a := Run(t, `
		component Zoom {
		  state log = ""
		  let s = Ref()
		  Column {
		    grow: 1
		    Text("log:{log}")
		    Button("Three") { on press { s.zoomTo(3) } }
		    Scroll {
		      ref: s
		      width: 400; height: 300
		      minZoom: 0.5; maxZoom: 4
		      on zoomChange(v) { log = log + "zoom {(v * 100).rounded()};" }
		      View { width: 400; height: 300; background: #336699 }
		    }
		  }
		}
		app { Zoom() }
	`)
	tr := a.UntilText("log:")
	sc := kindSized(t, tr, "Scroll", 400, 300)
	zoomOf := func(tr *Tree) map[string]any {
		for _, n := range tr.Kind("Scroll") {
			if n.ID == sc.ID {
				return vobj(&Node{Props: vobj(n, "scroll")}, "zoom")
			}
		}
		return nil
	}
	z := zoomOf(tr)
	if z == nil || !vbool(z, "allows") || vnum(z, "minMagnification") != 0.5 || vnum(z, "maxMagnification") != 4 || vnum(z, "magnification") != 1 {
		t.Fatalf("zoom state %v", z)
	}
	content := vid(sc.Children[0])
	l := &vlogger{a: a}

	a.Do("zoomwheel " + vid(sc) + " 5 100 100")
	l.expect(t, "⌘ and the wheel", "zoom 128")

	a.Do("mouse down " + content + " 100 100, up " + content + " 100 100 0.05, down2 " + content + " 100 100 0.1, up2 " + content + " 100 100 0.15")
	l.expect(t, "a double click zoomed in", "zoom 50")
	a.Do("mouse down " + content + " 100 100, up " + content + " 100 100 0.05, down2 " + content + " 100 100 0.1, up2 " + content + " 100 100 0.15")
	l.expect(t, "a double click at the least", "zoom 100")

	a.Do("magnify " + vid(sc) + " 2.5")
	l.expect(t, "a pinch", "zoom 250")

	// ZoomTo: the core's zoomChange hears it (the host doesn't report
	// it).
	a.Do("tap Three")
	l.expect(t, "ZoomTo", "zoom 300")
	if m := vnum(zoomOf(a.Tree()), "magnification"); m != 3 {
		t.Errorf("ZoomTo: %v", m)
	}
}
