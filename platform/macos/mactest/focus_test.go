//go:build darwin

package mactest

import (
	"strings"
	"testing"
)

// holder is the node one of whose children shows text (a focusable view
// around its label), nil if none.
func holder(tr *Tree, text string) *Node {
	for _, n := range tr.All() {
		for _, c := range n.Children {
			if c.Text == text {
				return n
			}
		}
	}
	return nil
}

// focusOf is a node's "focus" JSON (Focus.swift's).
func focusOf(n *Node) map[string]any {
	if n == nil {
		return nil
	}
	m, _ := n.Get("focus").(map[string]any)
	return m
}

// mustHolder is holder, failing the test when nothing shows text.
func mustHolder(t *testing.T, tr *Tree, text string) *Node {
	t.Helper()
	n := holder(tr, text)
	if n == nil {
		t.Fatalf("nothing holds %q:\n%s", text, tr.Raw())
	}
	return n
}

// Tab moves the keyboard's focus among Focusable views in the views'
// order (FocusChange 2: focusVisible), drawing AppKit's ring unless
// NoFocusRing; a click focuses a Focusable view (FocusChange 1: focused,
// not visible, no ring) unless NoFocusOnPress; a character no Keys took
// goes to the focused view as KeyText; Enter presses it; a press reports
// where it began (PressPoint).
func TestFocusTab(t *testing.T) {
	a := Run(t, `
		component F {
		  state log = ""
		  state typed = ""
		  state pressed = 0
		  state at = ""
		  Column {
		    padding: 20; gap: 10; align: .start
		    View as a {
		      width: 120; height: 30; radius: 6
		      focusable: true
		      on focusChange(f) { log = "{log} a:{f}" }
		      on keyText(t) { typed = typed + t }
		      on press { pressed += 1 }
		      Text(if a.focusVisible { "A visible" } else if a.focused { "A focused" } else { "A idle" })
		    }
		    View as b {
		      width: 120; height: 30
		      focusable: true
		      hideFocusRing: true
		      Text(if b.focusVisible { "B visible" } else if b.focused { "B focused" } else { "B idle" })
		    }
		    View as c {
		      width: 120; height: 30
		      focusable: true
		      pressKeepsFocus: true
		      Text(if c.focused { "C focused" } else { "C idle" })
		    }
		    View {
		      width: 120; height: 30
		      on pressStart(x, y) { at = "{x},{y}" }
		      on press { }
		      Text("Point")
		    }
		    Text("Log:{log}")
		    Text("Typed:{typed}")
		    Text("Pressed:{pressed}")
		    Text("At:{at}")
		  }
		}
		app { F() }
	`)
	a.UntilText("A idle")

	a.Do("keyin tab")
	tr := a.UntilText("A visible")
	fa := focusOf(mustHolder(t, tr, "A visible"))
	if !vbool(fa, "first") || !vbool(fa, "visible") || !vbool(fa, "ring") {
		t.Errorf("A after Tab: %v", fa)
	}
	a.UntilText("Log: a:true")

	a.Do("keyin x", "keyin y")
	a.UntilText("Typed:xy")
	a.Do("keyin enter")
	a.UntilText("Pressed:1")

	a.Do("keyin tab")
	tr = a.UntilText("B visible")
	if tr.FindText("A idle") == nil {
		t.Errorf("A kept the focus:\n%s", tr.Raw())
	}
	fb := focusOf(mustHolder(t, tr, "B visible"))
	if !vbool(fb, "first") || vbool(fb, "ring") || vnum(fb, "focusRingType") != 1 {
		t.Errorf("B (NoFocusRing) after Tab: %v", fb)
	}
	a.UntilText("Log: a:true a:false")

	a.Do("keyin shift+tab")
	a.UntilText("A visible")

	// A click: focused, not visible; no ring.
	b := mustHolder(t, tr, "B visible")
	a.Do("mouse down " + vid(b) + " 10 10, up " + vid(b) + " 10 10")
	tr = a.UntilText("B focused")
	if fb := focusOf(mustHolder(t, tr, "B focused")); !vbool(fb, "first") || vbool(fb, "visible") {
		t.Errorf("B after a click: %v", fb)
	}
	// NoFocusOnPress: the focus stays where it was.
	c := mustHolder(t, tr, "C idle")
	a.Do("mouse down " + vid(c) + " 10 10, up " + vid(c) + " 10 10")
	tr = a.Tree()
	if tr.FindText("B focused") == nil || tr.FindText("C idle") == nil {
		t.Errorf("a click on a view that keeps the focus moved it:\n%s", tr.Raw())
	}
	// Tab goes on from where the click left it.
	a.Do("keyin tab")
	a.UntilText("C focused")

	p := mustHolder(t, tr, "Point")
	a.Do("mouse down " + vid(p) + " 12 7, up " + vid(p) + " 12 7")
	tr = a.Until(func(tr *Tree) bool {
		for _, n := range tr.All() {
			if strings.HasPrefix(n.Text, "At:12") && strings.Contains(n.Text, ",7") {
				return true
			}
		}
		return false
	})
}

// Tab and Shift+Tab stay within the FocusTrap the focus is in.
func TestFocusTrap(t *testing.T) {
	a := Run(t, `
		component Trapped {
		  Column {
		    padding: 20; gap: 8; align: .start
		    View as x { width: 100; height: 24; focusable: true; Text(if x.focused { "Before*" } else { "Before" }) }
		    View {
		      trapFocus: true
		      View as one { width: 100; height: 24; focusable: true; Text(if one.focused { "One*" } else { "One" }) }
		      View as two { width: 100; height: 24; focusable: true; Text(if two.focused { "Two*" } else { "Two" }) }
		    }
		    View as y { width: 100; height: 24; focusable: true; Text(if y.focused { "After*" } else { "After" }) }
		  }
		}
		app { Trapped() }
	`)
	a.UntilText("Before")
	a.Do("keyin tab")
	a.UntilText("Before*")
	a.Do("keyin tab")
	a.UntilText("One*")
	a.Do("keyin tab")
	a.UntilText("Two*")
	a.Do("keyin tab")
	a.UntilText("One*")
	a.Do("keyin shift+tab")
	a.UntilText("Two*")
	if tr := a.Tree(); tr.FindText("After*") != nil || tr.FindText("Before*") != nil {
		t.Fatalf("the focus left the trap:\n%s", tr.Raw())
	}
}

// Each screen reader's prop reaches the view's NSAccessibility object:
// roles and subroles, AXValue (checked, a range's now), AXMinValue and
// AXMaxValue, AXValueDescription, AXExpanded, AXSelected, AXModal,
// AXTitleUIElement, AXLinkedUIElements, AXHelp.
func TestFocusA11yStates(t *testing.T) {
	a := Run(t, `
		component Box {
		  state on = false
		  state open = false
		  let label = Ref()
		  let list = Ref()
		  let help = Ref()

		  Column {
		    padding: 20; gap: 8; align: .start
		    Text("Agree") { ref: label }
		    Text("You must agree") { ref: help }
		    View {
		      role: .checkbox
		      a11yChecked: if on { .checked } else { .unchecked }
		      a11yLabelledBy: label
		      a11yDescribedBy: help
		      on press { on = !on }
		      Text("Box")
		    }
		    View {
		      role: .comboBox
		      a11yExpanded: if open { .expanded } else { .collapsed }
		      a11yControls: list
		      on press { open = !open }
		      Text("Combo")
		    }
		    if open {
		      View { ref: list; role: .listbox; a11yModal: true; Text("Items") }
		    }
		    View {
		      role: .slider
		      a11yValue: "40%"
		      a11yRange: ValueRange(min: 0, max: 100, now: 40)
		      a11ySelected: true
		      Text("Volume")
		    }
		    View { role: .switch; on press { }; Text("Wifi") }
		    View { role: .dialog; Text("Dlg") }
		  }
		}
		app { Box() }
	`)
	tr := a.UntilText("Box")
	ax := func(tr *Tree, text string) map[string]any {
		t.Helper()
		m, _ := mustHolder(t, tr, text).Get("ax").(map[string]any)
		if m == nil {
			t.Fatalf("%q has no ax:\n%s", text, tr.Raw())
		}
		return m
	}
	box := ax(tr, "Box")
	agree := tr.FindText("Agree")
	if vstr(box, "role") != "AXCheckBox" || vstr(box, "value") != "0" || vnum(box, "titleElement") != float64(agree.ID) || vstr(box, "help") != "You must agree" {
		t.Errorf("the checkbox: %v (Agree is %d)", box, agree.ID)
	}
	combo := ax(tr, "Combo")
	if vstr(combo, "role") != "AXComboBox" || vbool(combo, "expanded") {
		t.Errorf("the combobox: %v", combo)
	}
	slider := ax(tr, "Volume")
	if vstr(slider, "role") != "AXSlider" || vstr(slider, "value") != "40" || vstr(slider, "min") != "0" || vstr(slider, "max") != "100" ||
		vstr(slider, "valueDescription") != "40%" || !vbool(slider, "selected") {
		t.Errorf("the slider: %v", slider)
	}
	if w := ax(tr, "Wifi"); vstr(w, "role") != "AXCheckBox" || vstr(w, "subrole") != "AXSwitch" || vstr(w, "value") != "0" {
		t.Errorf("the switch: %v", w)
	}
	if d := ax(tr, "Dlg"); vstr(d, "role") != "AXGroup" || vstr(d, "subrole") != "AXApplicationDialog" {
		t.Errorf("the dialog: %v", d)
	}

	a.Do("tap Box", "tap Combo")
	tr = a.UntilText("Items")
	if box := ax(tr, "Box"); vstr(box, "value") != "1" {
		t.Errorf("the checkbox, checked: %v", box)
	}
	items := mustHolder(t, tr, "Items")
	combo = ax(tr, "Combo")
	linked := vnums(combo, "linked")
	if !vbool(combo, "expanded") || len(linked) != 1 || linked[0] != float64(items.ID) {
		t.Errorf("the combobox, open: %v (Items is %d)", combo, items.ID)
	}
	if l := ax(tr, "Items"); vstr(l, "role") != "AXList" || !vbool(l, "modal") {
		t.Errorf("the listbox: %v", l)
	}
}
