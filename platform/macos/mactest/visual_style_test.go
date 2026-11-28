//go:build darwin

package mactest

import (
	"testing"
)

// The text and box styling CALayer's and NSTextField's own properties
// don't draw (TextStyle.swift, Box.swift): what a Text's label draws with,
// and a box's layers.

// A Text with letter spacing, a line height, a text shadow or a minimum
// scale draws an attributed string of them, and is measured with it: its
// frame grows with its spacing and its lines; a shrinking one is drawn at
// the scale it fits its width at; its font's variants are its label's.
func TestTextStyle(t *testing.T) {
	a := Run(t, `
		component Texts {
		  state spacing = 2.0
		  Column {
		    padding: 20; gap: 10; align: .start
		    Text("Spaced") { letterSpacing: spacing }
		    Text("Plain")
		    Text("Tall lines that wrap onto more than one line of text here") { lineHeight: 30; width: 160 }
		    Text("A long title that does not fit in its width at all") { minimumScale: 0.5; width: 160 }
		    Text("Shadow") { textShadow: Shadow(color: #000000, radius: 4, x: 1, y: 2) }
		    Text("12:34") { font: Font(size: 17, monospacedDigits: true).italic() }
		    Text("Cut in the middle of the title") { lines: 1; truncation: .middle; width: 100 }
		    Button("Wider") { on press { spacing = 6 } }
		  }
		}
		app { Texts() }
	`)
	tr := a.UntilText("Wider")
	style := func(n *Node) map[string]any { return vobj(n, "textStyle") }
	spaced := tr.Find("Text", "Spaced")
	if spaced == nil {
		t.Fatalf("no text:\n%s", tr.Raw())
	}
	s := style(spaced)
	if !vbool(s, "styled") || vnum(s, "kern") != 2 {
		t.Errorf("the spaced text's label: %v", s)
	}
	plain := tr.Find("Text", "Plain")
	if vbool(style(plain), "styled") {
		t.Errorf("a plain text draws an attributed string: %v", style(plain))
	}
	w := spaced.Frame[2]
	a.Do("tap Wider")
	tr = a.Until(func(tr *Tree) bool {
		n := tr.Find("Text", "Spaced")
		return n != nil && vnum(style(n), "kern") == 6
	})
	// Six characters, 4 more points after each.
	if got := tr.Find("Text", "Spaced").Frame[2]; got < w+20 || got > w+28 {
		t.Errorf("wider spacing: %v wide, was %v", got, w)
	}
	tall := tr.FindText("Tall lines that wrap onto more than one line of text here")
	if ts := style(tall); vnum(ts, "lineHeight") != 30 {
		t.Errorf("the tall text's line height: %v", ts)
	}
	if h := tall.Frame[3]; int(h)%30 != 0 || h < 60 {
		t.Errorf("the tall text is %v high, want lines of 30", h)
	}
	shrunk := style(tr.FindText("A long title that does not fit in its width at all"))
	if sc := vnum(shrunk, "scale"); sc >= 1 || sc < 0.5 {
		t.Errorf("the shrinking text's scale: %v", shrunk)
	}
	if sh := vnums(style(tr.FindText("Shadow")), "shadow"); len(sh) != 3 || sh[0] != 4 || sh[1] != 1 || sh[2] != -2 {
		t.Errorf("the text's shadow: %v", sh)
	}
	// Italic, with monospaced digits: the label's font has both.
	if ds := style(tr.FindText("12:34")); !vbool(ds, "italic") || vnum(ds, "features") != 1 {
		t.Errorf("the timer's font: %v", ds)
	}
	cut := style(tr.FindText("Cut in the middle of the title"))
	if vnum(cut, "lineBreak") != 5 { // NSLineBreakMode.byTruncatingMiddle
		t.Errorf("the middle-cut text's line break: %v", cut)
	}
}

// Corners of different radii, per-side and dashed borders draw with a
// box's layers; once they no longer need them, CALayer's own again.
func TestBoxStyle(t *testing.T) {
	a := Run(t, `
		component Boxes {
		  state uneven = true
		  Column {
		    padding: 20; gap: 10; align: .start
		    View { width: 60; height: 40; background: #ff0000; radius: if uneven { Corners(topLeft: 20) } else { 8 } }
		    View { width: 70; height: 40; border: Edges(bottom: 2); borderColor: #0000ff }
		    View { width: 80; height: 40; border: 2; borderStyle: .dashed }
		    Text("Box") { width: 90; height: 40; background: #00ff00; radius: [12, 0, 12, 0] }
		    Button("Even") { on press { uneven = false } }
		  }
		}
		app { Boxes() }
	`)
	tr := a.UntilText("Even")
	box := func(n *Node) map[string]any { return vobj(n, "box") }
	b := box(viewSized(t, tr, 60, 40))
	if r := vnums(b, "radii"); len(r) != 4 || r[0] != 20 || r[1] != 0 || !vbool(b, "fill") {
		t.Errorf("the uneven corners' box: %v", b)
	}
	if b := box(viewSized(t, tr, 70, 40)); vnum(b, "borders") != 1 {
		t.Errorf("the bottom border's box: %v", b)
	}
	if d := vnums(box(viewSized(t, tr, 80, 40)), "dashed"); len(d) != 2 || d[0] != 6 || d[1] != 6 {
		t.Errorf("the dashed border's pattern: %v", d)
	}
	if b := box(kindSized(t, tr, "Text", 90, 40)); !vbool(b, "mask") || vbool(b, "fill") {
		t.Errorf("a text's uneven corners: %v", b)
	}
	a.Do("tap Even")
	tr = a.Until(func(tr *Tree) bool { return box(viewSized(t, tr, 60, 40)) == nil })
	if n := viewSized(t, tr, 60, 40); n.Num("layerCornerRadius") != 8 {
		t.Errorf("even corners: the layer's radius is %v", n.Props["layerCornerRadius"])
	}
}
