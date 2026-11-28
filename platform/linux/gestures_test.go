package linux_test

import (
	"fmt"
	"path/filepath"
	"strings"
	"testing"
)

// idShowing is the ID of the view whose line in a dump shows text.
func idShowing(t *testing.T, dump, text string) string {
	t.Helper()
	for _, l := range strings.Split(dump, "\n") {
		if strings.Contains(l, fmt.Sprintf("%q", text)) {
			return strings.Fields(l)[0]
		}
	}
	t.Fatalf("nothing shows %q:\n%s", text, dump)
	return ""
}

// Gestures (testdata/gestures.asuka), with real input through XTest: a
// List's row lifted by a drag, the others making way, put back by Escape;
// one dragged past the others moves (Moved; the host's IDs in the
// new order, the row's views kept: the same text view shows its item),
// one dragged back up, and one moved with Alt+Down; a double click is a
// DoubleTap and no Press, a click a Press once the double-click time has
// passed, the pointer over a view HoverMove; a drag along the other axis
// than PanAxis, or short of PanMinDistance, isn't a pan; Ctrl and the wheel zoom a zoomable Scroll (Zoomed, 10% a
// notch), a double click zooms it back out to MinZoom, another in to
// twice it, and ZoomTo zooms it.
func TestGestures(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "gestures.asuka"), exe)
	d := script(t, dir, exe,
		"snap:start",
		"drag:Apple,0,60,hold", "wait:300ms", "snap:lifted",
		"press:escape", "release", "wait:500ms", "snap:cancelled",
		"drag:Apple,0,1000", "wait:500ms", "snap:dragged",
		"drag:Fig,0,-1000", "wait:500ms", "snap:back",
		"dclick:Tap target", "wait:700ms", "snap:double",
		"click:Tap target", "wait:700ms", "snap:single",
		"drag:Pan me,0,50", "wait:300ms", "snap:across",
		"drag:Pan me,20,0", "wait:300ms", "snap:short",
		"drag:Pan me,60,0", "wait:300ms", "snap:panned",
		"wheel:Zoom me,3,ctrl", "wait:300ms", "snap:wheeled",
		"dclick:Zoom me", "wait:500ms", "snap:out",
		"dclick:Zoom me", "wait:500ms", "snap:in",
		"tap:Zoom 3", "wait:500ms", "snap:zoomto")
	// Lifted, the others making way; Escape puts it back.
	shows(t, d["lifted"], "a row lifted", []string{"Order: Apple Banana Cherry Date Elderberry Fig", "Last: none"})
	if !strings.Contains(d["lifted"], " lifted shifted ") || !strings.Contains(d["lifted"], "row 1 [") || !strings.Contains(d["lifted"], "] shifted -") {
		t.Errorf("no row lifted, the others making way:\n%s", d["lifted"])
	}
	shows(t, d["cancelled"], "Escape", []string{"Order: Apple Banana Cherry Date Elderberry Fig", "Last: none"})
	if strings.Contains(d["cancelled"], " lifted") || strings.Contains(d["cancelled"], " shifted") {
		t.Errorf("Escape didn't put the row back:\n%s", d["cancelled"])
	}
	apple := idShowing(t, d["start"], "Apple")
	shows(t, d["dragged"], "Apple dragged to the end", []string{"Order: Banana Cherry Date Elderberry Fig Apple", "Last: 0 to 5"})
	if got := idShowing(t, d["dragged"], "Apple"); got != apple {
		t.Errorf("Apple's row was bound again: its text was %s, now %s:\n%s", apple, got, d["dragged"])
	}
	shows(t, d["back"], "Fig dragged to the start", []string{"Order: Fig Banana Cherry Date Elderberry Apple", "Last: 4 to 0"})
	shows(t, d["double"], "a double click", []string{"Taps 0 likes 1 at 80,30"})
	shows(t, d["single"], "a click", []string{"Taps 1 likes 1 at 80,30", "Over 80,30"})
	shows(t, d["across"], "a drag across a horizontal pan", []string{"Pans 0"})
	shows(t, d["short"], "a drag short of PanMinDistance", []string{"Pans 0"})
	shows(t, d["panned"], "a horizontal drag", []string{"Pans 1"})
	shows(t, d["wheeled"], "Ctrl and the wheel", []string{"Zoom 133"})
	shows(t, d["out"], "a double click, zoomed in", []string{"Zoom 100"})
	shows(t, d["in"], "a double click", []string{"Zoom 200"})
	shows(t, d["zoomto"], "ZoomTo", []string{"Zoom 300"})
}

// Moving a row from the keyboard: the focused row moves a place with
// Alt+Down (and Alt+Up), as a screen reader's Move down does.
func TestMoveRowKeys(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "gestures.asuka"), exe)
	d := script(t, dir, exe,
		"click:Banana", "press:alt+down", "wait:400ms", "snap:down",
		"press:alt+up", "press:alt+up", "wait:400ms", "snap:up")
	shows(t, d["down"], "Alt+Down", []string{"Order: Apple Cherry Banana Date Elderberry Fig", "Last: 1 to 2"})
	shows(t, d["up"], "Alt+Up twice", []string{"Order: Banana Apple Cherry Date Elderberry Fig", "Last: 1 to 0"})
}
