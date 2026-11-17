//go:build darwin

package mactest

import (
	"testing"
)

// overlayOf is an Overlay node's "overlay" JSON (Overlay.swift's).
func overlayOf(n *Node) map[string]any {
	m, _ := n.Get("overlay").(map[string]any)
	return m
}

// openOverlays is the Overlay nodes whose content shows text.
func overlayShowing(tr *Tree, text string) *Node {
	for _, o := range tr.Kind("Overlay") {
		var walk func(n *Node) bool
		walk = func(n *Node) bool {
			if n.Text == text {
				return true
			}
			for _, c := range n.Children {
				if walk(c) {
					return true
				}
			}
			return false
		}
		if walk(o) {
			return o
		}
	}
	return nil
}

const overlayPicker = `
	component Picker {
	  state open = false
	  state reason = "none"
	  state picked = "nothing"
	  let trigger = Ref()

	  Column {
	    padding: 20; gap: 8; align: .start
	    View {
	      width: 120; height: 30
	      ref: trigger
	      on press { open = !open }
	      Text("Choose")
	    }
	    Text("Picked {picked}")
	    Text("Closed by {reason}")
	    View { width: 120; height: 30; on press { picked = "elsewhere" }; Text("Elsewhere") }
	    Overlay {
	      bind open: open
	      anchor: trigger
	      offset: 4
	      on dismiss(r) { reason = "{r}" }
	      Column {
	        View { width: 100; height: 24; on press { picked = "apple"; open = false }; Text("Apple") }
	        View { width: 100; height: 24; on press { picked = "pear"; open = false }; Text("Pear") }
	      }
	    }
	  }
	}
`

// An overlay goes over the main window's content, on top, the size of its
// root (its environment says so); its anchor's frame is reported in the
// root's coordinates and its content is placed under it; a press on its
// content is its own, one outside it is reported (OutsidePress) and still
// reaches what it lands on.
func TestOverlayPlaced(t *testing.T) {
	a := Run(t, overlayPicker+`app { Picker() }`)
	tr := a.UntilText("Choose")
	choose := mustHolder(t, tr, "Choose")
	a.Do("tap Choose")
	tr = a.UntilText("Apple")
	o := overlayShowing(tr, "Apple")
	if o == nil {
		t.Fatalf("no overlay shows Apple:\n%s", tr.Raw())
	}
	tr = a.Until(func(tr *Tree) bool { return len(vnums(overlayOf(overlayShowing(tr, "Apple")), "anchorFrame")) == 5 })
	o = overlayShowing(tr, "Apple")
	j := overlayOf(o)
	if vstr(j, "hostClass") != "AsukaContentView" || vnum(j, "index") != vnum(j, "siblings")-1 {
		t.Errorf("the overlay isn't the main window's top view: %v", j)
	}
	main := tr.Main()
	env := vnums(j, "envSize")
	if len(env) != 2 || main == nil || env[0] != main.Content[2] || env[1] != main.Content[3] || o.Frame[2] != env[0] || o.Frame[3] != env[1] {
		t.Errorf("the overlay's size %v, frame %v; the window's content %v", env, o.Frame, main.Content)
	}
	af := vnums(j, "anchorFrame")
	if af[0] != choose.InWindow[0] || af[1] != choose.InWindow[1] || af[2] != choose.InWindow[2] || af[3] != choose.InWindow[3] || af[4] != 1 {
		t.Errorf("the anchor's frame %v, Choose's %v", af, choose.InWindow)
	}
	apple := mustHolder(t, tr, "Apple")
	if apple.InWindow[1] < choose.InWindow[1]+choose.InWindow[3]+4 || apple.Hidden {
		t.Errorf("Apple at %v, under Choose at %v?", apple.InWindow, choose.InWindow)
	}

	// Its own: a press on its content.
	a.Do("mouse down " + vid(apple) + " 10 10, up " + vid(apple) + " 10 10")
	tr = a.UntilText("Picked apple")
	if tr.FindText("Closed by none") == nil {
		t.Errorf("a press on its content was outside it:\n%s", tr.Raw())
	}

	// Outside: reported, and the press goes on to what's there.
	a.Do("tap Choose")
	a.UntilText("Apple")
	tr = a.Tree()
	else_ := mustHolder(t, tr, "Elsewhere")
	a.Do("mouse down " + vid(else_) + " 10 10, up " + vid(else_) + " 10 10")
	tr = a.UntilText("Closed by outsidePress")
	if tr.FindText("Picked elsewhere") == nil {
		t.Errorf("the press outside didn't reach what it landed on:\n%s", tr.Raw())
	}
	if overlayShowing(tr, "Apple") != nil {
		t.Errorf("the overlay stayed open:\n%s", tr.Raw())
	}
}

// Modal, an overlay takes the presses that land on none of its views (what
// is behind gets none) and reports them as OutsidePress; its view is
// modal to screen readers.
func TestOverlayModal(t *testing.T) {
	a := Run(t, `
		component Dialog {
		  state open = false
		  state reason = "none"
		  state behind = 0
		  Column {
		    padding: 20; gap: 8; align: .start
		    View { width: 100; height: 30; on press { open = true }; Text("Show") }
		    Text("Reason {reason}")
		    Text("Behind {behind}")
		    View { width: 100; height: 30; on press { behind += 1 }; Text("Behind") }
		    Overlay {
		      bind open: open
		      side: .center
		      modal: true
		      on dismiss(r) { reason = "{r}" }
		      Column {
		        padding: 16
		        Text("Are you sure?")
		      }
		    }
		  }
		}
		app { Dialog() }
	`)
	tr := a.UntilText("Show")
	behind := mustHolder(t, tr, "Behind")
	a.Do("tap Show")
	tr = a.UntilText("Are you sure?")
	o := overlayShowing(tr, "Are you sure?")
	if j := overlayOf(o); !vbool(j, "modal") {
		t.Errorf("not modal: %v", j)
	}
	// VoiceOver sees only the overlay's views in the window.
	in := map[float64]bool{}
	var walk func(n *Node)
	walk = func(n *Node) {
		in[float64(n.ID)] = true
		for _, c := range n.Children {
			walk(c)
		}
	}
	walk(o)
	kids := vnums(tr.Main().Props, "a11yChildren")
	if len(kids) == 0 {
		t.Errorf("no accessibility children: %v", tr.Main().Props)
	}
	for _, k := range kids {
		if !in[k] {
			t.Errorf("VoiceOver sees %v beside the modal overlay %d: %v", k, o.ID, kids)
		}
	}
	a.Do("mouse down " + vid(behind) + " 10 10, up " + vid(behind) + " 10 10")
	tr = a.UntilText("Reason outsidePress")
	if kids := vnums(tr.Main().Props, "a11yChildren"); len(kids) < 2 {
		t.Errorf("closed, the overlay still hides the window: %v", kids)
	}
	if tr.FindText("Behind 0") == nil {
		t.Errorf("a press reached what's behind a modal overlay:\n%s", tr.Raw())
	}
}

// The anchor's frame is reported again as a scroll view around it
// scrolls; scrolled out of view, it's hidden (the core closes the overlay:
// anchorHidden).
func TestOverlayAnchorScroll(t *testing.T) {
	a := Run(t, `
		component S {
		  state open = false
		  state reason = "none"
		  let trigger = Ref()
		  Column {
		    Text("Closed by {reason}")
		    Scroll {
		      height: 300
		      Column {
		        padding: 20; gap: 8; align: .start
		        View { width: 100; height: 30; ref: trigger; on press { open = true }; Text("Open") }
		        for i in 0..<40 key i { Text("Row {i}") }
		      }
		    }
		    Overlay {
		      bind open: open
		      anchor: trigger
		      on dismiss(r) { reason = "{r}" }
		      View { width: 80; height: 24; Text("Menu") }
		    }
		  }
		}
		app { S() }
	`)
	a.UntilText("Open")
	a.Do("tap Open")
	tr := a.Until(func(tr *Tree) bool {
		o := overlayShowing(tr, "Menu")
		return o != nil && len(vnums(overlayOf(o), "anchorFrame")) == 5
	})
	before := vnums(overlayOf(overlayShowing(tr, "Menu")), "anchorFrame")
	menuBefore := tr.FindText("Menu").InWindow
	a.Do("scroll 10")
	tr = a.Until(func(tr *Tree) bool {
		o := overlayShowing(tr, "Menu")
		if o == nil {
			return false
		}
		af := vnums(overlayOf(o), "anchorFrame")
		return len(af) == 5 && af[1] == before[1]-10
	})
	// The core places it again once it hears of the anchor.
	a.Until(func(tr *Tree) bool { m := tr.FindText("Menu"); return m != nil && m.InWindow[1] == menuBefore[1]-10 })
	a.Do("scroll 400")
	a.UntilText("Closed by anchorHidden")
}

// The last presented overlay is on top of the others, whichever was made
// first: one opened again goes over the rest.
func TestOverlayOrder(t *testing.T) {
	a := Run(t, `
		component Two {
		  state first = false
		  state second = false
		  Column {
		    padding: 20; gap: 8; align: .start
		    View { width: 100; height: 30; on press { first = !first }; Text("Toggle first") }
		    View { width: 100; height: 30; on press { second = true }; Text("Open second") }
		    Overlay { open: first; side: .center; dismissOn: []; Text("First") }
		    Overlay { open: second; side: .center; dismissOn: []; Text("Second") }
		  }
		}
		app { Two() }
	`)
	a.UntilText("Toggle first")
	a.Do("tap Toggle first")
	a.UntilText("First")
	a.Do("tap Open second")
	tr := a.UntilText("Second")
	f, s := overlayOf(overlayShowing(tr, "First")), overlayOf(overlayShowing(tr, "Second"))
	if vnum(s, "index") != vnum(s, "siblings")-1 || vnum(f, "index") != vnum(s, "index")-1 {
		t.Errorf("second over first: first %v, second %v", f, s)
	}
	a.Do("tap Toggle first")
	a.Until(func(tr *Tree) bool { return overlayShowing(tr, "First") == nil })
	a.Do("tap Toggle first")
	tr = a.UntilText("First")
	f, s = overlayOf(overlayShowing(tr, "First")), overlayOf(overlayShowing(tr, "Second"))
	if vnum(f, "index") != vnum(f, "siblings")-1 || vnum(s, "index") != vnum(f, "index")-1 || vnum(f, "seq") <= vnum(s, "seq") {
		t.Errorf("first opened again isn't on top: first %v, second %v", f, s)
	}
}
