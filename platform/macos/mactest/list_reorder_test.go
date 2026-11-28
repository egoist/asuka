//go:build darwin

package mactest

import (
	"fmt"
	"slices"
	"strings"
	"testing"
)

// rowTexts are a list's rows' texts, in order, and their cells' IDs.
func rowTexts(t *testing.T, a *App) ([]string, []uint32) {
	t.Helper()
	l, _ := listOf(t, a.Tree())
	cells := cellsOf(l)
	var texts []string
	var ids []uint32
	for r := 0; r < len(cells); r++ {
		c := cells[r]
		if c == nil {
			break
		}
		texts = append(texts, firstTextIn(c))
		ids = append(ids, c.ID)
	}
	return texts, ids
}

// A List with on move: a drag of a row with the mouse lifts it (it follows
// the mouse, a shadow under it) and drops it where it's dragged to, its
// cell kept; each row offers screen readers its Move up and Move down
// where there's room, which move it by one; a move the app doesn't take
// goes back, the cells still on their items.
func TestListReorder(t *testing.T) {
	a := Run(t, `
		component Moves {
		  state items = ["A", "B", "C", "D", "E", "F"]
		  state take = true
		  state log = ""
		  Column {
		    grow: 1
		    Button("Refuse") { on press { take = false } }
		    Text("log:{log}")
		    List {
		      grow: 1
		      on move(from, to) {
		        if take { items.insert(items.removeAt(from), at: to) }
		        log = log + "move {from} {to};"
		      }
		      for s in items key s {
		        View { height: 40; padding: [0, 16]; justify: .center; Text("Item {s}") }
		      }
		    }
		  }
		}
		app { Moves() }
	`)
	a.UntilText("Item F")
	l := &vlogger{a: a}
	ln, lj := listOf(t, a.Tree())
	if r := vobj(&Node{Props: lj}, "reorder"); r == nil || !vbool(r, "on") || fmt.Sprint(r["names"]) != "[Move up Move down]" {
		t.Fatalf("reorder state %v", lj["reorder"])
	}
	cells := cellsOf(ln)
	acts := func(r int) string { return fmt.Sprint(cellState(cells[r])["moveActions"]) }
	if acts(0) != "[Move down]" || acts(2) != "[Move up Move down]" || acts(5) != "[Move up]" {
		t.Errorf("the rows' actions: %s, %s, %s", acts(0), acts(2), acts(5))
	}
	if !vbool(cellState(cells[0]), "a11yElement") {
		t.Errorf("a row isn't an accessibility element")
	}
	texts, before := rowTexts(t, a)

	// Dragged from A's middle down 115 points: its middle is between C's
	// and D's: it drops after C. Mid-drag it's lifted and follows the
	// mouse; the rows below it make way.
	list := vid(ln)
	steps := []string{"down " + list + " 200 20 0"}
	for i, y := range []int{24, 60, 100, 135} {
		steps = append(steps, fmt.Sprintf("dragged %s 200 %d %v", list, y, float64(i+1)*0.02))
	}
	a.Do("mouse " + strings.Join(steps, ", "))
	tr := a.Tree()
	_, lj = listOf(t, tr)
	mv, _ := vobj(&Node{Props: lj}, "reorder")["move"].(map[string]any)
	if mv == nil || vnum(mv, "from") != 0 || vnum(mv, "to") != 2 || vnum(mv, "y") != 115 {
		t.Errorf("mid-drag: %v", lj["reorder"])
	}
	ln, _ = listOf(t, tr)
	if c := cellsOf(ln)[0]; !vbool(cellState(c), "lifted") || vnums(cellState(c), "rowFrame")[1] != 115 {
		t.Errorf("the lifted row: %v", cellState(c))
	}
	if f := vnums(cellState(cellsOf(ln)[1]), "rowFrame"); f[1] != 0 {
		t.Errorf("B made way to %v", f)
	}
	a.Do("mouse up " + list + " 200 135 0.2")
	l.expect(t, "the drop", "move 0 2")
	got, ids := rowTexts(t, a)
	if want := []string{"Item B", "Item C", "Item A", "Item D", "Item E", "Item F"}; !slices.Equal(got, want) {
		t.Errorf("after the drop: %q, want %q (was %q)", got, want, texts)
	}
	if want := []uint32{before[1], before[2], before[0], before[3], before[4], before[5]}; !slices.Equal(ids, want) {
		t.Errorf("the cells %v, want %v: rows kept their cells", ids, want)
	}
	ln, _ = listOf(t, a.Tree())
	if c := cellsOf(ln)[2]; vbool(cellState(c), "lifted") || vnums(cellState(c), "rowFrame")[1] != 80 {
		t.Errorf("the dropped row: %v", cellState(c))
	}

	// A screen reader's Move up, on A (row 2).
	a.Do(fmt.Sprintf("a11yaction %d Move up", ids[2]))
	l.expect(t, "Move up", "move 2 1")
	if got, _ := rowTexts(t, a); !slices.Equal(got, []string{"Item B", "Item A", "Item C", "Item D", "Item E", "Item F"}) {
		t.Errorf("after Move up: %q", got)
	}
	_, ids = rowTexts(t, a)

	// Refused: the move goes back, each cell still on its item.
	a.Do("tap Refuse", fmt.Sprintf("a11yaction %d Move down", ids[0]))
	l.expect(t, "a refused move", "move 0 1")
	got, back := rowTexts(t, a)
	if !slices.Equal(got, []string{"Item B", "Item A", "Item C", "Item D", "Item E", "Item F"}) || !slices.Equal(back, ids) {
		t.Errorf("a refused move: %q %v, want it as it was (%v)", got, back, ids)
	}
}

// Dragged near a long list's bottom edge, a lifted row scrolls the list,
// and stays under the mouse; dropped, it's at the row it was dragged to.
func TestListReorderScrolls(t *testing.T) {
	a := Run(t, `
		component Moves {
		  state items = (0..<60).lazy().toArray()
		  state log = ""
		  Column {
		    grow: 1
		    Text("log:{log}")
		    List {
		      grow: 1
		      on move(from, to) {
		        items.insert(items.removeAt(from), at: to)
		        log = log + "move {from} {to};"
		      }
		      for i in items key i { View { height: 40; Text("Row {i}") } }
		    }
		  }
		}
		app { Moves() }
	`)
	a.UntilText("Row 0")
	l := &vlogger{a: a}
	ln, lj := listOf(t, a.Tree())
	vb := nums(lj, "visible")
	h := ln.Frame[3]
	list := vid(ln)
	a.Do(fmt.Sprintf("mouse down %s 200 20 0, dragged %s 200 30 0.02, dragged %s 200 %v 0.04", list, list, list, h-5))
	// It scrolls a frame at a time while the mouse is in the edge's band.
	tr := a.Until(func(tr *Tree) bool { _, m := listOf(t, tr); return nums(m, "visible")[1] > vb[1]+100 })
	_, lj = listOf(t, tr)
	y := nums(lj, "visible")[1]
	a.Do(fmt.Sprintf("mouse dragged %s 200 %v 0.06", list, h/2))
	tr = a.Tree()
	_, lj = listOf(t, tr)
	mv, _ := vobj(&Node{Props: lj}, "reorder")["move"].(map[string]any)
	if mv == nil || vbool(mv, "autoscrolling") {
		t.Fatalf("out of the band: %v", lj["reorder"])
	}
	y = nums(lj, "visible")[1]
	to := int(vnum(mv, "to"))
	if want := int((y + h/2) / 40); to < want-1 || to > want {
		t.Errorf("dragged to row %d, scrolled to %v: want about %d", to, y, want)
	}
	a.Do(fmt.Sprintf("mouse up %s 200 %v 0.08", list, h/2))
	l.expect(t, "the drop", fmt.Sprintf("move 0 %d", to))
}
