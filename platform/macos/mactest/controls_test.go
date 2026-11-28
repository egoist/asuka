//go:build darwin

package mactest

import (
	"fmt"
	"slices"
	"strings"
	"testing"
	"time"
)

// The controls of their own files on a Mac (Chip.swift, DateRange.swift):
// chips, disclosures and date ranges, driven as the user would.

// Chips are recessed buttons: a filter chip is a push-on-push-off one whose
// click toggles its selection (the checkmark in place of its icon), an
// action chip's click presses it, and a removable chip's remove button (its
// own target, named with Asuka's word and the title), its screen reader
// action and Delete remove it.
func TestControlsChip(t *testing.T) {
	a := Run(t, `
		component C {
		  state wifi = false
		  state presses = 0
		  state guests = ["Ana", "Bo", "Cy"]
		  Column {
		    padding: 16; gap: 8; align: .start
		    Row {
		      gap: 8
		      Chip("Wi-Fi") { icon: "wifi"; bind selected: wifi }
		      Chip("Add") { icon: "plus"; on press { presses += 1 } }
		      for g in guests key g {
		        Chip(g) { icon: "person"; on remove { guests = guests.filter(x => x != g) } }
		      }
		    }
		    Text("log:{wifi} {presses} {guests.count()}")
		  }
		}
		app { C() }
	`)
	tr := a.UntilText("log:false 0 3")
	wifi := tr.Find("Chip", "Wi-Fi")
	if wifi == nil {
		t.Fatalf("no Wi-Fi chip:\n%s", tr.Raw())
	}
	if wifi.Class != "AsukaChipButton" || wifi.Num("bezelStyle") != 13 { // NSButton.BezelStyle.recessed
		t.Errorf("a %s, bezel %v", wifi.Class, wifi.Get("bezelStyle"))
	}
	c := vobj(wifi, "chip")
	if !vbool(c, "filter") || vbool(c, "on") || vstr(c, "icon") != "wifi" {
		t.Errorf("filter chip %v", c)
	}
	if wifi.Frame[2] < 40 || wifi.Frame[3] < 16 {
		t.Errorf("frame %v", wifi.Frame)
	}
	// Its click selects it: the button stays on, and the app hears it.
	a.Do("tap Wi-Fi")
	tr = a.UntilText("log:true 0 3")
	if c := vobj(tr.Find("Chip", "Wi-Fi"), "chip"); !vbool(c, "on") || !vbool(c, "selected") {
		t.Errorf("after a click: %v", c)
	}
	// The app's choice shows too.
	a.Do("tap Wi-Fi")
	tr = a.UntilText("log:false 0 3")
	if c := vobj(tr.Find("Chip", "Wi-Fi"), "chip"); vbool(c, "on") {
		t.Errorf("after a second click: %v", c)
	}
	// An action chip presses and springs back.
	a.Do("tap Add")
	tr = a.UntilText("log:false 1 3")
	if c := vobj(tr.Find("Chip", "Add"), "chip"); vbool(c, "filter") || vbool(c, "on") {
		t.Errorf("action chip %v", c)
	}
	// A removable chip: its remove button at its end.
	ana := tr.Find("Chip", "Ana")
	ac := vobj(ana, "chip")
	if !vbool(ac, "removable") {
		t.Fatalf("Ana isn't removable: %v", ac)
	}
	rf := vnums(ac, "removeFrame")
	if len(rf) != 4 || rf[0] < ana.Frame[2]/2 || rf[0]+rf[2] > ana.Frame[2] {
		t.Errorf("remove button %v in %v", rf, ana.Frame)
	}
	if l := vstr(ac, "removeLabel"); !strings.HasSuffix(l, " Ana") || len(l) <= 4 {
		t.Errorf("remove button's label %q", l)
	}
	if acts := vstrs(ac, "actions"); len(acts) != 1 {
		t.Errorf("actions %q", acts)
	}
	// A click on it: the remove button is its own target.
	a.Do("chipremove " + vid(ana))
	tr = a.UntilText("log:false 1 2")
	if tr.Find("Chip", "Ana") != nil {
		t.Errorf("Ana stays")
	}
	// The screen reader's remove action.
	bo := tr.Find("Chip", "Bo")
	a.Do("a11yaction " + vid(bo) + " " + vstrs(vobj(bo, "chip"), "actions")[0])
	tr = a.UntilText("log:false 1 1")
	// Delete while it has the keyboard's focus.
	cy := tr.Find("Chip", "Cy")
	a.Do("focusview " + vid(cy))
	a.Do("key delete")
	a.UntilText("log:false 1 0")
}

// A disclosure's header is the disclosure triangle and its title: a click
// on the title turns the triangle and shows the content under it (the
// core's views), a second hides it; the app's state turns it too.
func TestControlsDisclosure(t *testing.T) {
	a := Run(t, `
		component D {
		  state open = false
		  Column {
		    padding: 16; gap: 8
		    Disclosure("More options") {
		      bind expanded: open
		      Text("Inside")
		    }
		    Text("log:{open}")
		    Button("Close") { on press { open = false } }
		  }
		}
		app { D() }
	`)
	tr := a.UntilText("log:false")
	d := tr.Find("Disclosure", "More options")
	if d == nil {
		t.Fatalf("no disclosure:\n%s", tr.Raw())
	}
	j := vobj(d, "disclosure")
	if vbool(j, "expanded") || vstr(j, "title") != "More options" || vstr(j, "a11yLabel") != "More options" {
		t.Errorf("disclosure %v", j)
	}
	if r := vstr(j, "a11yRole"); r != "AXDisclosureTriangle" {
		t.Errorf("role %q", r)
	}
	if d.Frame[3] < 16 || d.Frame[2] < 100 {
		t.Errorf("frame %v", d.Frame)
	}
	if n := tr.FindText("Inside"); n != nil && n.Bool("shown") {
		t.Errorf("the content shows closed")
	}
	a.Do("tap More options")
	tr = a.UntilText("log:true")
	if !vbool(vobj(tr.Find("Disclosure", "More options"), "disclosure"), "expanded") {
		t.Errorf("not expanded after a click")
	}
	tr = a.Until(func(tr *Tree) bool { n := tr.FindText("Inside"); return n != nil && n.Bool("shown") })
	in := tr.FindText("Inside")
	if d := tr.Find("Disclosure", "More options"); in.Frame[1] < d.Frame[1]+d.Frame[3] {
		t.Errorf("the content %v isn't under the header %v", in.Frame, d.Frame)
	}
	// A click on the title (not the triangle) turns it too.
	d = tr.Find("Disclosure", "More options")
	a.Do(fmt.Sprintf("mouse down %s 60 6,up %s 60 6", vid(d), vid(d)))
	a.UntilText("log:false")
	a.Do("tap More options")
	a.UntilText("log:true")
	a.Do("tap Close")
	a.Until(func(tr *Tree) bool {
		return !vbool(vobj(tr.Find("Disclosure", "More options"), "disclosure"), "expanded") && tr.FindText("Inside") == nil
	})
}

// A date range is two date pickers with an en dash between: setting
// either reports the range (local midnights), a start past the end takes
// the end with it; inline, it's the graphical calendar in its range mode.
func TestControlsDateRange(t *testing.T) {
	day := func(y int, m time.Month, d int) int64 { return time.Date(y, m, d, 0, 0, 0, 0, time.Local).UnixMilli() }
	a := Run(t, fmt.Sprintf(`
		component R {
		  state start = 0
		  state end = 0
		  state s2 = %d
		  state e2 = %d
		  Column {
		    padding: 16; gap: 8; align: .start
		    DateRangePicker { bind start: start; bind end: end; min: %d }
		    DateRangePicker { bind start: s2; bind end: e2; inline: true }
		    Text("log:{start} {end} {s2} {e2}")
		  }
		}
		app { R() }
	`, day(2030, 5, 2), day(2030, 5, 4), day(2030, 1, 1)))
	tr := a.UntilText(fmt.Sprintf("log:0 0 %d %d", day(2030, 5, 2), day(2030, 5, 4)))
	rs := tr.Kind("DateRangePicker")
	if len(rs) != 2 {
		t.Fatalf("%d date ranges", len(rs))
	}
	compact, inline := vobj(rs[0], "dateRange"), vobj(rs[1], "dateRange")
	if vbool(compact, "inline") || len(compact["pickers"].([]any)) != 2 {
		t.Errorf("compact %v", compact)
	}
	if l := vstrs(compact, "labels"); !slices.Equal(l, []string{"Start date", "End date"}) {
		t.Errorf("the pickers' labels %q", l)
	}
	if vnum(compact, "minDate") != float64(day(2030, 1, 1)) {
		t.Errorf("min %v", compact["minDate"])
	}
	if !vbool(inline, "inline") || vnum(inline, "start") != float64(day(2030, 5, 2)) || vnum(inline, "end") != float64(day(2030, 5, 4)) {
		t.Errorf("inline %v", inline)
	}
	if rs[0].Frame[2] < 150 || rs[1].Frame[3] < 100 {
		t.Errorf("frames %v %v", rs[0].Frame, rs[1].Frame)
	}
	// The log, as it starts (the compact one's) or ends (the calendar's).
	logHas := func(format string, x, y int64) *Tree {
		want := fmt.Sprintf(format, x, y)
		return a.Until(func(tr *Tree) bool {
			l := "log:" + vlogOf(tr)
			return strings.HasPrefix(l, want) || strings.HasSuffix(l, want)
		})
	}
	// The user sets the start (at noon: the day's midnight is reported):
	// none was chosen, so the end shows it too.
	a.Do(fmt.Sprintf("rangeset %s 0 %d", vid(rs[0]), day(2030, 6, 10)+12*3600_000))
	logHas("log:%d %d", day(2030, 6, 10), day(2030, 6, 10))
	a.Do(fmt.Sprintf("rangeset %s 1 %d", vid(rs[0]), day(2030, 6, 14)))
	logHas("log:%d %d", day(2030, 6, 10), day(2030, 6, 14))
	// A start past the end moves the end to it.
	a.Do(fmt.Sprintf("rangeset %s 0 %d", vid(rs[0]), day(2030, 6, 20)))
	logHas("log:%d %d", day(2030, 6, 20), day(2030, 6, 20))
	// The calendar: a new start, then the range's end.
	a.Do(fmt.Sprintf("rangeset %s 0 %d", vid(rs[1]), day(2030, 7, 1)))
	logHas(" %d %d", day(2030, 7, 1), day(2030, 7, 1))
	a.Do(fmt.Sprintf("rangeset %s 1 %d", vid(rs[1]), day(2030, 7, 5)))
	tr = logHas(" %d %d", day(2030, 7, 1), day(2030, 7, 5))
	if in := vobj(tr.Kind("DateRangePicker")[1], "dateRange"); vnum(in, "end") != float64(day(2030, 7, 5)) {
		t.Errorf("the calendar shows %v", in)
	}
}

// The trip example's controls, laid out in a window: none overlaps the
// next.
func TestControlsLayout(t *testing.T) {
	a := Run(t, `
		component L {
		  state sel = true
		  Column {
		    padding: 16; gap: 8; align: .start
		    Row { gap: 8; Chip("One") { bind selected: sel }; Chip("Two") { icon: "star"; on remove {} } }
		    Disclosure("Header") { Text("x") }
		    DateRangePicker {}
		  }
		}
		app { L() }
	`)
	tr := a.Until(func(tr *Tree) bool { return len(tr.Kind("DateRangePicker")) == 1 && tr.Find("Chip", "Two") != nil })
	one, two := tr.Find("Chip", "One"), tr.Find("Chip", "Two")
	if one.InWindow[0]+one.InWindow[2] > two.InWindow[0] {
		t.Errorf("chips overlap: %v %v", one.InWindow, two.InWindow)
	}
	// Two's width has room for its icon, title and remove button.
	if two.Frame[2] < one.Frame[2] {
		t.Errorf("a removable chip with an icon %v is narrower than a selected one %v", two.Frame, one.Frame)
	}
	// In the window's flipped content: down the column.
	d, r := tr.Find("Disclosure", "Header"), tr.Kind("DateRangePicker")[0]
	ys := []float64{one.InWindow[1] + one.InWindow[3], d.InWindow[1], d.InWindow[1] + d.InWindow[3], r.InWindow[1]}
	if !slices.IsSorted(ys) {
		t.Errorf("the disclosure %v is over the chips", ys)
	}
}
