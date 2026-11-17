package linux_test

// RFC 0010's host contract on Linux: the keyboard's focus on any view,
// screen readers' roles and states, and Overlay, in real GTK windows
// driven with real input (X11's XTest under Xvfb: click:, press:), read
// back from the windows' dumps (ASUKA_DUMP: GTK's focus, its accessibles'
// states through its test API, the overlays over each root in z-order).

import (
	"fmt"
	"math"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// dumpNode is a line of a dump: a node, its frame in its root's
// coordinates, and the rest of its line.
type dumpNode struct {
	id         int
	kind       string
	x, y, w, h float64
	line       string
}

// parseDump reads a dump's nodes, each frame made absolute (an overlay is
// at its root's origin).
func parseDump(t *testing.T, dump string) []dumpNode {
	t.Helper()
	type at struct {
		depth int
		x, y  float64
	}
	var out []dumpNode
	var stack []at
	for _, l := range strings.Split(dump, "\n") {
		trimmed := strings.TrimLeft(l, " ")
		if trimmed == "" {
			continue
		}
		depth := (len(l) - len(trimmed)) / 2
		var n dumpNode
		var fx, fy float64
		if _, err := fmt.Sscanf(trimmed, "%d %s [%g,%g %gx%g]", &n.id, &n.kind, &fx, &fy, &n.w, &n.h); err != nil {
			continue
		}
		for len(stack) > 0 && stack[len(stack)-1].depth >= depth {
			stack = stack[:len(stack)-1]
		}
		px, py := 0.0, 0.0
		if len(stack) > 0 {
			px, py = stack[len(stack)-1].x, stack[len(stack)-1].y
		}
		if n.kind == "Overlay" {
			fx, fy = 0, 0
		}
		n.x, n.y = px+fx, py+fy
		n.line = trimmed
		stack = append(stack, at{depth, n.x, n.y})
		out = append(out, n)
	}
	return out
}

// textNode is the node showing text (a text's, a button's title).
func textNode(t *testing.T, nodes []dumpNode, text string) dumpNode {
	t.Helper()
	q := strconv.Quote(text)
	for _, n := range nodes {
		if strings.Contains(n.line, " "+q) || strings.Contains(n.line, "title="+q) {
			return n
		}
	}
	t.Fatalf("no node shows %q", text)
	return dumpNode{}
}

// parentOf is the line before a node's at one depth less: the view
// around it.
func parentOf(t *testing.T, dump, text string) string {
	t.Helper()
	lines := strings.Split(dump, "\n")
	q := strconv.Quote(text)
	for i, l := range lines {
		if !strings.Contains(l, " "+q) {
			continue
		}
		depth := len(l) - len(strings.TrimLeft(l, " "))
		for j := i - 1; j >= 0; j-- {
			if d := len(lines[j]) - len(strings.TrimLeft(lines[j], " ")); d < depth {
				return lines[j]
			}
		}
	}
	t.Fatalf("no view around %q:\n%s", text, dump)
	return ""
}

// The keyboard's focus: a press focuses a focusable view (FocusChange 1:
// focused, not visibly), typing goes to it (keyText), Tab and Shift+Tab
// move it (FocusChange 2: visibly), a focus trap keeps Tab inside it both
// ways, a press reports where it began, and a view that draws its own
// ring has GTK's taken off.
func TestFocus(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "focus.asuka"), exe)
	d := script(t, dir, exe,
		"snap:start",
		"click:B idle", "snap:pressed",
		"press:shift+tab", "snap:visible",
		"press:b", "press:a", "snap:typed",
		"press:tab", "snap:tab",
		"press:tab", "snap:one",
		"press:tab", "press:tab", "snap:wrapped",
		"press:shift+tab", "snap:back",
		"click:Point", "snap:point",
		"click:Keep idle", "snap:kept")
	// A press: focused, not visibly (FocusChange 1).
	shows(t, d["pressed"], "a press", []string{"B focused", "A idle"}, "B visible", "B idle")
	if !strings.Contains(parentOf(t, d["pressed"], "B focused"), " focused") {
		t.Errorf("GTK's focus isn't on B:\n%s", d["pressed"])
	}
	// The keyboard: visibly (FocusChange 2).
	shows(t, d["visible"], "Shift+Tab", []string{"A visible", "B idle"})
	if !strings.Contains(parentOf(t, d["visible"], "A visible"), " focused-visible") {
		t.Errorf("GTK's focus isn't visibly on A:\n%s", d["visible"])
	}
	if !strings.Contains(d["visible"], `a:true"`) {
		t.Errorf("A's focusChange(true) didn't come last:\n%s", d["visible"])
	}
	shows(t, d["typed"], "typed", []string{"Typed:ba"})
	shows(t, d["tab"], "Tab", []string{"B visible", "A idle"})
	if !strings.Contains(d["tab"], `a:true a:false"`) {
		t.Errorf("A's focusChange(false) didn't come:\n%s", d["tab"])
	}
	shows(t, d["one"], "into the trap", []string{"One focused", "Two idle"})
	shows(t, d["wrapped"], "Tab at the trap's end", []string{"One focused", "Two idle", "After idle"}, "After focused")
	shows(t, d["back"], "Shift+Tab at the trap's start", []string{"Two focused", "One idle", "B idle"})
	// Its own ring: B's view has GTK's taken off, A's hasn't.
	if b := parentOf(t, d["pressed"], "B focused"); !strings.Contains(b, " noring") {
		t.Errorf("B's ring isn't off: %s", b)
	}
	if a := parentOf(t, d["pressed"], "A idle"); strings.Contains(a, " noring") {
		t.Errorf("A's ring is off: %s", a)
	}
	// Where the press on Point began: in it.
	nodes := parseDump(t, d["point"])
	p := textNode(t, nodes, "Point")
	var x, y float64
	for _, n := range nodes {
		if strings.Contains(n.line, `"At:`) {
			if _, err := fmt.Sscanf(n.line[strings.Index(n.line, `"At:`)+4:], "%g,%g", &x, &y); err != nil {
				t.Fatalf("no point: %s", n.line)
			}
		}
	}
	if x <= 0 || y <= 0 || x > p.w+8 || y > p.h+8 {
		t.Errorf("the press began at %g,%g, Point is %gx%g", x, y, p.w, p.h)
	}
	if !strings.Contains(parentOf(t, d["point"], "Point"), " focused") {
		t.Errorf("a press on a pressable view didn't focus it:\n%s", d["point"])
	}
	// pressKeepsFocus: the focus stays on Point.
	shows(t, d["kept"], "a press that keeps the focus", []string{"Keep idle"}, "Keep focused")
	if !strings.Contains(parentOf(t, d["kept"], "Point"), " focused") {
		t.Errorf("the focus left Point:\n%s", d["kept"])
	}
}

// Overlay: over its root (here a Scroll), placed under its anchor, and
// placed again as the anchor scrolls (AnchorFrame after a scroll); a
// press outside it closes it and reaches what it lands on; a modal one
// takes every press (nothing behind gets one), the rest of the window
// hidden from screen readers; Escape closes it; the last presented is on
// top.
func TestOverlayLayer(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "layer.asuka"), exe)
	d := script(t, dir, exe,
		"snap:start",
		"click:Choose", "wait:300ms", "snap:open",
		"scroll:60", "wait:300ms", "snap:scrolled",
		"clickat:700,450", "snap:outside",
		"click:Choose", "wait:300ms", "click:Elsewhere", "snap:through",
		"click:Show dialog", "wait:300ms", "snap:dialog",
		"click:Behind", "snap:modal",
		"click:Choose", "wait:300ms", "snap:again",
		"press:escape", "snap:escaped",
		"click:Choose", "wait:300ms", "back", "snap:back")
	shows(t, d["start"], "the start", []string{"Choose", "Closed by none"}, "Apple")

	// Placed under its anchor, 4 points down.
	open := parseDump(t, d["open"])
	shows(t, d["open"], "open", []string{"Apple", "Pear"})
	choose := textNode(t, open, "Choose")
	apple := textNode(t, open, "Apple")
	var anchor dumpNode
	for _, n := range open {
		if n.kind == "View" && n.x <= choose.x && n.y <= choose.y && n.x+n.w >= choose.x+choose.w && n.y+n.h >= choose.y+choose.h && n.h < 60 {
			anchor = n
		}
	}
	if apple.y < anchor.y+anchor.h+4-0.5 {
		t.Errorf("Apple at y %g, its anchor at %g + %g:\n%s", apple.y, anchor.y, anchor.h, d["open"])
	}
	var layer string
	for _, l := range strings.Split(d["open"], "\n") {
		if strings.Contains(l, " Overlay [") && strings.Contains(l, " z=") {
			layer = l
		}
	}
	if layer == "" || strings.Contains(layer, " unmapped") {
		t.Errorf("no overlay shows over the root:\n%s", d["open"])
	}

	// The anchor scrolled up 60: the overlay follows it.
	scrolled := textNode(t, parseDump(t, d["scrolled"]), "Apple")
	if math.Abs(scrolled.y-(apple.y-60)) > 1.5 {
		t.Errorf("after a scroll of 60, Apple is at %g (was %g)", scrolled.y, apple.y)
	}
	shows(t, d["scrolled"], "scrolled", []string{"Apple", "Closed by none"})

	// A press outside closes it (on the root, where nothing is).
	shows(t, d["outside"], "a press outside", []string{"Closed by outsidePress"}, "Apple")
	// And reaches what it lands on.
	shows(t, d["through"], "a press through", []string{"Picked elsewhere"}, "Apple")

	// Modal: the dialog over everything, the rest hidden from screen
	// readers; a press on what's behind it doesn't reach it.
	shows(t, d["dialog"], "the dialog", []string{"Are you sure?"})
	z := layers(d["dialog"])
	var picker, dialog int
	for _, l := range strings.Split(d["dialog"], "\n") {
		if n, ok := layerOf(l); ok {
			if strings.Contains(l, " modal") {
				dialog = n
			} else {
				picker = n
			}
		}
	}
	if dialog == 0 || picker == 0 || z[dialog] != 1 || z[picker] != 0 {
		t.Errorf("the dialog isn't on top of the picker's layer:\n%s", d["dialog"])
	}
	if p := parentOf(t, d["dialog"], "Are you sure?"); !strings.Contains(p, "role=dialog") || !strings.Contains(p, " modal") {
		t.Errorf("the dialog isn't a modal dialog to screen readers: %s", p)
	}
	hidden := false
	for _, n := range parseDump(t, d["dialog"]) {
		if n.kind == "Scroll" && strings.Contains(n.line, "a11y(") && strings.Contains(n.line, " hidden") {
			hidden = true // the root's content, under the dialog
		}
	}
	if !hidden {
		t.Errorf("what's under the modal dialog isn't hidden from screen readers:\n%s", d["dialog"])
	}
	shows(t, d["modal"], "a press behind the dialog", []string{"Behind 0", "Dialog closed by outsidePress"}, "Are you sure?", "Behind 1")
	for _, l := range strings.Split(d["modal"], "\n") {
		if strings.Contains(l, "a11y(") && strings.Contains(l, " hidden") {
			t.Errorf("still hidden from screen readers with the dialog gone: %s", l)
		}
	}

	// Presented again: the picker's layer on top now.
	if z := layers(d["again"]); z[picker] != 1 || z[dialog] != 0 {
		t.Errorf("the picker presented again isn't on top (%d, %d):\n%s", picker, dialog, d["again"])
	}
	shows(t, d["again"], "opened again", []string{"Apple"})
	shows(t, d["escaped"], "Escape", []string{"Closed by escape"}, "Apple")
	// Going back goes to the top overlay first.
	shows(t, d["back"], "back", []string{"Closed by back"}, "Apple")
}

// layerOf is the overlay's ID a dump's line names, if it's an overlay's.
func layerOf(l string) (int, bool) {
	t := strings.TrimLeft(l, " ")
	var id int
	var kind string
	if _, err := fmt.Sscanf(t, "%d %s", &id, &kind); err != nil || kind != "Overlay" || !strings.Contains(t, " z=") {
		return 0, false
	}
	return id, true
}

// layers is each overlay's z (0 the lowest) in a dump.
func layers(dump string) map[int]int {
	out := map[int]int{}
	for _, l := range strings.Split(dump, "\n") {
		if id, ok := layerOf(l); ok {
			var z int
			fmt.Sscanf(l[strings.Index(l, " z=")+3:], "%d", &z)
			out[id] = z
		}
	}
	return out
}

// Screen readers: each role and state reaches GTK's accessible (read
// back through GTK), relations to the views they name, a modal view
// hiding the rest.
func TestA11yStates(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "a11ystates.asuka"), exe)
	d := script(t, dir, exe, "snap:start", "tap:Box", "tap:Combo", "snap:after")
	start := d["start"]
	agree := textNode(t, parseDump(t, start), "Agree")
	want := func(dump, text string, parts ...string) {
		t.Helper()
		p := parentOf(t, dump, text)
		for _, s := range parts {
			if !strings.Contains(p, s) {
				t.Errorf("%s: no %q in %s", text, s, p)
			}
		}
	}
	want(start, "Box", "role=checkbox", "checked=false", fmt.Sprintf("labelledby=%d", agree.id))
	want(start, "Combo", "role=combo-box", "expanded=false")
	hint := textNode(t, parseDump(t, start), "Loudness, in percent")
	want(start, "Volume", "role=slider", `value="40%"`, "range=0/100/40", fmt.Sprintf("describedby=%d", hint.id))
	want(start, "Tab one", "role=tab", "selected=true")
	after := d["after"]
	shows(t, after, "pressed", []string{"On true"})
	var list int
	for _, n := range parseDump(t, after) {
		if strings.Contains(n.line, "role=list-box") {
			list = n.id
		}
	}
	want(after, "Box", "checked=true")
	want(after, "Combo", "expanded=true", fmt.Sprintf("controls=%d", list))
	want(after, "Items", "role=list-box", " modal")
	// The modal list hides its siblings from screen readers.
	want(after, "Volume", " hidden")
	if l := parentOf(t, after, "Items"); strings.Contains(l, " hidden") {
		t.Errorf("the modal view is hidden itself: %s", l)
	}
}
