//go:build darwin

package mactest

import (
	"os"
	"testing"
)

// shotsDir is where the tests save what they show (ASUKA_SHEET_SHOTS;
// "": nowhere).
func shotsDir() string { return os.Getenv("ASUKA_SHEET_SHOTS") }

// sheetShows waits for the one sheet's window to show as cond has it
// (its content's size cw by ch), and returns its node.
func sheetShows(t *testing.T, a *App, cond func(s *Node, w map[string]any, cw, ch float64) bool) *Node {
	t.Helper()
	var s *Node
	a.Until(func(tr *Tree) bool {
		s = sheetOf(tr)
		if s == nil {
			return false
		}
		w := sheetWindow(s)
		cw, ch := size(w["content"])
		return w["visible"] == true && cond(s, w, cw, ch)
	})
	return s
}


// A sheet with detents rests at the one it's at (its shortest, at
// first), its environment and height (on resize) with it; the app's
// setting it moves the sheet window there, its top edge kept.
func TestNavSheetDetents(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  state open = false
		  state at: Detent = .height(200)
		  state h: float = 0
		  Column {
		    padding: 20
		    Text("open {open}")
		    Button("Show") { on press { open = true } }
		    Sheet {
		      bind open: open
		      detents: [.large, .height(200), .fraction(0.5)]
		      bind detent: at
		      on resize(x) { h = x }
		      Column {
		        padding: 20
		        Text("env {environment().height}")
		        Text("height {h}")
		        Button("Tall") { on press { at = .large } }
		        Button("Half") { on press { at = .fraction(0.5) } }
		      }
		    }
		  }
		}
	`)
	a.UntilText("open false")
	a.Do("tap Show")
	s := sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return w["isSheet"] == true && ch == 200 })
	a.UntilText("env 200")
	a.UntilText("height 200")
	// Where it is once it's come in: its top and middle.
	topY, midY := -1.0, -1.0
	a.Until(func(tr *Tree) bool {
		s = sheetOf(tr)
		if s == nil {
			return false
		}
		f := sheetWindow(s)["frame"].([]any)
		y := f[1].(float64) + f[3].(float64)
		same := y == topY
		topY, midY = y, f[1].(float64)+f[3].(float64)/2
		return same
	})
	_, large := pair(s.Get("sheetLarge"))
	if d := shotsDir(); d != "" {
		a.Snap(d + "/detents-200.png")
	}
	a.Do("tap Tall")
	s = sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return ch == large })
	// It stays where AppKit has it: hanging from its window's title bar
	// (its top kept), or centred on it (its middle kept, macOS 26).
	f := sheetWindow(s)["frame"].([]any)
	if y, m := f[1].(float64)+f[3].(float64), f[1].(float64)+f[3].(float64)/2; !near(y, topY) && !near(m, midY) {
		t.Errorf("it moved from top %v, middle %v to top %v, middle %v", topY, midY, y, m)
	}
	if d := shotsDir(); d != "" {
		a.Snap(d + "/detents-large.png")
	}
	a.UntilText("height " + ftoa(large))
	a.Do("tap Half")
	half := float64(int(large*0.5 + 0.5))
	sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return ch == half })
	a.UntilText("env " + ftoa(half))
}

// A sheet at a detent no taller than its undimmed one is a panel over
// the bottom of its window, which stays usable; moved taller it's a
// sheet of its window, and shorter a panel again; Escape dismisses it.
func TestNavSheetUndimmed(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  state open = false
		  state taps = 0
		  state at: Detent = .height(150)
		  Column {
		    padding: 20
		    Text("taps {taps}")
		    Button("Behind") { on press { taps += 1 } }
		    Button("Show") { on press { open = true } }
		    Text("open {open}")
		    Sheet {
		      bind open: open
		      detents: [.height(150), .large]
		      bind detent: at
		      undimmed: .height(150)
		      cornerRadius: 24
		      dismissible: false
		      Column {
		        padding: 16
		        Button("Tall") { on press { at = .large } }
		        Button("Short") { on press { at = .height(150) } }
		      }
		    }
		  }
		}
	`)
	a.UntilText("taps 0")
	a.Do("tap Show")
	s := sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool {
		return w["panel"] == true && w["isSheet"] == false && ch == 150
	})
	tr := a.Tree()
	if p := sheetWindow(s)["childOf"]; p != tr.Main().Title {
		t.Errorf("a panel of %q, not of the main window %q", p, tr.Main().Title)
	}
	if r, _ := s.Get("cornerRadius").(float64); r != 24 {
		t.Errorf("its corners' radius %v, want 24", r)
	}
	// It's over its window's content's bottom.
	f := sheetWindow(s)["frame"].([]any)
	if mf := tr.Main().Frame; !near(f[1].(float64), mf[1]) {
		t.Errorf("the panel's bottom %v isn't its window's %v", f[1], mf[1])
	}
	// The window behind it takes clicks.
	a.Do("tap Behind")
	a.UntilText("taps 1")
	if d := shotsDir(); d != "" {
		a.Snap(d + "/undimmed-panel.png")
	}
	a.Do("tap Tall")
	sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool {
		return w["isSheet"] == true && w["panel"] == false && ch > 150
	})
	a.Do("tap Short")
	sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return w["panel"] == true && ch == 150 })
	a.Do("tap Tall")
	a.Do("cancel")
	a.UntilText("open false")
	a.Until(func(tr *Tree) bool { return sheetOf(tr) == nil })
}

// A sheet with fit among its detents opens at its content's height (the
// core's measure of it), follows it as it grows, and moves to large and
// back.
func TestNavSheetFitAmongDetents(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  state open = false
		  state more = false
		  state at: Detent = .fit
		  Column {
		    padding: 20
		    Text("open {open}")
		    Button("Show") { on press { open = true } }
		    Sheet {
		      bind open: open
		      detents: [.fit, .large]
		      bind detent: at
		      Column {
		        padding: 20
		        View { height: 100; background: .blue }
		        if more { View { height: 50; background: .green } }
		        Button("More") { on press { more = !more } }
		        Button("Tall") { on press { at = .large } }
		        Button("Fit") { on press { at = .fit } }
		      }
		    }
		  }
		}
	`)
	a.UntilText("open false")
	a.Do("tap Show")
	tr := a.Until(func(tr *Tree) bool { return sheetOf(tr) != nil && tr.Find("Button", "More") != nil })
	bh := tr.Find("Button", "More").Frame[3]
	base := 20 + 100 + 3*bh + 20
	sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return w["isSheet"] == true && near(ch, base) })
	a.Do("tap More")
	sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return near(ch, base+50) })
	a.Do("tap Tall")
	s := sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return ch > base+100 })
	_, large := pair(s.Get("sheetLarge"))
	sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return ch == large })
	a.Do("tap Fit")
	sheetShows(t, a, func(s *Node, w map[string]any, cw, ch float64) bool { return near(ch, base+50) })
}
