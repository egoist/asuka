//go:build darwin

package mactest

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

// tableOf is the tree's only node of kind's table state (the host's
// "table" JSON).
func tableOf(t *testing.T, tr *Tree, kind string) map[string]any {
	t.Helper()
	ns := tr.Kind(kind)
	if len(ns) == 0 {
		t.Fatalf("no %s:\n%s", kind, tr.Raw())
	}
	m, _ := ns[0].Get("table").(map[string]any)
	if m == nil {
		t.Fatalf("the %s has no table state:\n%s", kind, tr.Raw())
	}
	return m
}

// shownRows are a table's rows on screen as it draws them, by row.
func shownRows(m map[string]any) map[int]map[string]any {
	out := map[int]map[string]any{}
	rows, _ := m["shown"].([]any)
	for _, r := range rows {
		row, _ := r.(map[string]any)
		i, _ := row["row"].(float64)
		out[int(i)] = row
	}
	return out
}

// rowTexts is a shown row's texts, column by column as shown.
func cellTexts(row map[string]any) []string {
	a, _ := row["texts"].([]any)
	out := make([]string, len(a))
	for i, s := range a {
		out[i], _ = s.(string)
	}
	return out
}

func columnsOf(m map[string]any) []map[string]any {
	a, _ := m["columns"].([]any)
	out := make([]map[string]any, len(a))
	for i, c := range a {
		out[i], _ = c.(map[string]any)
	}
	return out
}

func ints(m map[string]any, k string) []int {
	a, _ := m[k].([]any)
	out := make([]int, len(a))
	for i, x := range a {
		f, _ := x.(float64)
		out[i] = int(f)
	}
	return out
}

const tableApp = `
	struct Row {
	  id: int
	  name: string
	  size: int
	}

	component Files {
	  state rows = (0..<10000).map(i => Row(id: i, name: "File {i}", size: (i * 7919) % 1000))
	  state sort: [SortKey] = [SortKey(column: "name")]
	  state selection: [int] = []
	  state opened = "none"
	  state starred = "none"
	  let shown = if sort.isEmpty() || sort[0].column != "size" { rows } else if sort[0].ascending {
	    rows.sorted((a, b) => a.size < b.size || a.size == b.size && a.id < b.id)
	  } else {
	    rows.sorted((a, b) => a.size > b.size || a.size == b.size && a.id < b.id)
	  }
	  Column {
	    grow: 1
	    Text("sort {if sort.isEmpty() { "-" } else { sort[0].column + if sort[0].ascending { " up" } else { " down" } }}")
	    Text("selected {selection.map(i => "{i}").joined(separator: ",")}")
	    Text("opened {opened}")
	    Text("starred {starred}")
	    Button("Select 7") { on press { selection = [7] } }
	    Table {
	      grow: 1
	      columns: [
	        TableColumn(id: "name", title: "Name", width: 200, sortable: true),
	        TableColumn(id: "size", title: "Size", width: 80, sortable: true, align: .end),
	        TableColumn(id: "kind", title: "Kind", width: 90),
	      ]
	      selectionMode: .multiple
	      bind sort: sort
	      bind selection: selection
	      on open(row) { opened = shown[row].name }
	      menu: row => [.button(title: "Star", run: () => { starred = shown[row].name })]
	      for r in shown key r.id {
	        TableRow([r.name, "{r.size}", if r.id % 2 == 0 { "Even" } else { "Odd" }]) {
	          icon: if r.id % 3 == 0 { "folder" } else { "" }
	        }
	      }
	    }
	  }
	}

	app {
	  Window {
	    width: 700
	    height: 500
	    Files()
	  }
	}
`

// A Table of 10,000 rows is an NSTableView of 10,000 rows that binds only
// the rows it shows into cells (a row view each, reused as it scrolls),
// and draws each row's RowData: the cells' text, column by column, a
// symbol before the first.
func TestTableBindsOnlyWhatShows(t *testing.T) {
	a := Run(t, tableApp)
	var m map[string]any
	a.Until(func(tr *Tree) bool {
		m = tableOf(t, tr, "Table")
		r := shownRows(m)[0]
		return r != nil && len(cellTexts(r)) == 3 && cellTexts(r)[0] == "File 0"
	})
	if num(m, "rows") != 10000 || m["class"] != "AsukaTableView" || m["header"] != true {
		t.Fatalf("the table: %v rows, %v, header %v", m["rows"], m["class"], m["header"])
	}
	if c := num(m, "cells"); c == 0 || c > 60 {
		t.Fatalf("%v cells made for 10,000 rows", c)
	}
	r0 := shownRows(m)[0]
	if got := cellTexts(r0); strings.Join(got, "|") != "File 0|0|Even" || r0["symbol"] != true {
		t.Fatalf("row 0 shows %q, symbol %v", got, r0["symbol"])
	}
	if r1 := shownRows(m)[1]; r1["symbol"] != false || cellTexts(r1)[2] != "Odd" {
		t.Fatalf("row 1: %v", r1)
	}
	cols := columnsOf(m)
	if len(cols) != 3 || cols[0]["title"] != "Name" || cols[0]["width"] != 200.0 || cols[1]["headerAlignment"] != "right" ||
		cols[2]["sortable"] != false || cols[0]["sortable"] != true || cols[0]["indicator"] != "ascending" {
		t.Fatalf("the columns: %v", cols)
	}
	// The core heard the columns as shown: their order and widths.
	a.Until(func(tr *Tree) bool { return len(ints(tableOf(t, tr, "Table"), "layout")) == 4+3*8 })

	// Scrolled to the middle: those rows bound, no more cells made.
	a.Do("scroll 120000")
	a.Until(func(tr *Tree) bool {
		m = tableOf(t, tr, "Table")
		vis := ints(m, "visibleRows")
		r := shownRows(m)[vis[0]+1]
		return vis[0] > 4000 && r != nil && cellTexts(r)[0] == fmt.Sprintf("File %d", vis[0]+1)
	})
	if c := num(m, "cells"); c > 60 {
		t.Fatalf("%v cells made after scrolling", c)
	}
	if b := num(m, "bound"); b > 60 {
		t.Fatalf("%v rows bound", b)
	}
}

// A click on a sortable header sends SortBy, and the indicator shows the
// Sort the core sends back: ascending on the first click, descending on
// the second; a header that doesn't sort doesn't.
func TestTableHeaderSorts(t *testing.T) {
	a := Run(t, tableApp)
	a.UntilText("sort name up")
	a.Do("tableheader Size")
	tr := a.UntilText("sort size up")
	m := tableOf(t, tr, "Table")
	cols := columnsOf(m)
	if cols[1]["indicator"] != "ascending" || cols[0]["indicator"] != "" || m["highlighted"] != "size" {
		t.Fatalf("sorted by size: %v, highlighted %v", cols, m["highlighted"])
	}
	// The rows the app sorted.
	a.Until(func(tr *Tree) bool {
		r := shownRows(tableOf(t, tr, "Table"))[0]
		return r != nil && cellTexts(r)[1] == "0"
	})
	a.Do("tableheader Size")
	tr = a.UntilText("sort size down")
	if cols = columnsOf(tableOf(t, tr, "Table")); cols[1]["indicator"] != "descending" {
		t.Fatalf("sorted by size, descending: %v", cols)
	}
	a.Do("tableheader Kind")
	a.Do("tableheader Name")
	tr = a.UntilText("sort name up")
	if cols = columnsOf(tableOf(t, tr, "Table")); cols[0]["indicator"] != "ascending" || cols[1]["indicator"] != "" {
		t.Fatalf("sorted by name: %v", cols)
	}
}

// A click selects a row, ⌘-click and shift-click add to it (RowsSelected),
// the app's selection set from code selects without a report, and a
// double click or Return opens a row; a secondary click's menu runs its
// item for the row it was on.
func TestTableSelectOpenMenu(t *testing.T) {
	a := Run(t, tableApp)
	a.UntilText("selected ")
	a.Do("tablerow 2")
	a.UntilText("selected 2")
	a.Do("tablerow 4,cmd")
	a.UntilText("selected 2,4")
	a.Do("tablerow 6,shift")
	tr := a.UntilText("selected 2,4,5,6")
	if got := ints(tableOf(t, tr, "Table"), "selected"); fmt.Sprint(got) != "[2 4 5 6]" {
		t.Fatalf("the table selects %v", got)
	}
	a.Do("tap Select 7")
	tr = a.Until(func(tr *Tree) bool { return fmt.Sprint(ints(tableOf(t, tr, "Table"), "selected")) == "[7]" })
	if tr.FindText("selected 7") == nil {
		t.Fatalf("the app's selection:\n%s", strings.Join(a.Texts(), "\n"))
	}
	a.Do("tabledouble 3")
	a.UntilText("opened File 3")
	a.Do("tablerow 9", "keyin return")
	a.UntilText("opened File 9")
	// The menu of a row not selected selects it first.
	a.Do("tablemenu 12|Star")
	a.UntilText("starred File 12")
	a.UntilText("selected 12")
}

// An Outline is an NSOutlineView whose items are the rows the core shows:
// a click on a disclosure asks the core (Expand), and the children appear
// once the app has them (loaded lazily), indented a level.
func TestOutlineExpandsLazily(t *testing.T) {
	a := Run(t, `
		component Tree {
		  state loaded: [string: [string]] = [:]
		  state expanded: [string] = []
		  state opened = "none"
		  Column {
		    grow: 1
		    Text("expanded {expanded.joined(separator: ",")}")
		    Text("opened {opened}")
		    Outline(["A", "B"]) {
		      grow: 1
		      childrenOf: id => loaded.get(id) ?? []
		      expandable: id => id.count() == 1
		      text: id => "Item {id}"
		      icon: id => "folder"
		      bind expanded: expanded
		      on expand(id, on) {
		        if on && !loaded.has(id) {
		          loaded[id] = ["{id}1", "{id}2"]
		        }
		      }
		      on open(id) { opened = id }
		    }
		  }
		}
		app { Tree() }
	`)
	var m map[string]any
	a.Until(func(tr *Tree) bool {
		m = tableOf(t, tr, "Outline")
		return len(shownRows(m)) == 2 && cellTexts(shownRows(m)[1])[0] == "Item B"
	})
	r0 := shownRows(m)[0]
	if r0["expandable"] != true || r0["expanded"] != false || r0["symbol"] != true || m["class"] != "AsukaOutlineView" {
		t.Fatalf("row 0: %v (%v)", r0, m["class"])
	}
	a.Do("outlinedisclose 1")
	tr := a.Until(func(tr *Tree) bool {
		m = tableOf(t, tr, "Outline")
		return len(shownRows(m)) == 4
	})
	rows := shownRows(m)
	if rows[1]["expanded"] != true || cellTexts(rows[2])[0] != "Item B1" || rows[2]["level"] != 1.0 || rows[3]["expandable"] != false {
		t.Fatalf("B expanded: %v", rows)
	}
	for i, r := range rows {
		if r["inSync"] != true {
			t.Fatalf("row %d isn't the core's: %v", i, r)
		}
	}
	if tr.FindText("expanded B") == nil {
		t.Fatalf("the app's expanded:\n%s", strings.Join(a.Texts(), "\n"))
	}
	// Collapsed again: as the core says.
	a.Do("outlinedisclose 1")
	a.Until(func(tr *Tree) bool { return len(shownRows(tableOf(t, tr, "Outline"))) == 2 })
	a.UntilText("expanded ")
	// The right arrow on a selected row expands it: the outline's own key,
	// asked of the core.
	a.Do("outlinerow 0", "keyin right")
	a.UntilText("expanded A")
	a.Until(func(tr *Tree) bool { return len(shownRows(tableOf(t, tr, "Outline"))) == 4 })
	a.Do("outlinedouble 0")
	a.UntilText("opened A")
}

// A Table's rows of views: each column's view at its column's cell, as
// the table shows its columns.
func TestTableRowsOfViews(t *testing.T) {
	a := Run(t, `
		app {
		  Table {
		    columns: [TableColumn(id: "a", title: "A", width: 120), TableColumn(id: "b", title: "B", width: 150)]
		    for i in 0..<50 key i {
		      TableRow {
		        Text("a{i}")
		        Button("b{i}") {}
		      }
		    }
		  }
		}
	`)
	a.UntilText("a0")
	a.Until(func(tr *Tree) bool { return len(ints(tableOf(t, tr, "Table"), "layout")) > 0 })
	tr := a.Tree()
	var cell *Node
	for _, n := range tr.Kind("Table")[0].Children {
		if n.Kind == "Cell" && n.Row == 0 {
			cell = n
		}
	}
	if cell == nil || len(cell.Children) != 2 || firstTextIn(cell.Children[0]) != "a0" {
		t.Fatalf("no row of views:\n%s", a.Dump())
	}
	// Each column's view at the table's cell of the column, at its width:
	// the second past the first.
	c0, c1 := cell.Children[0].Frame, cell.Children[1].Frame
	if c0[2] != 120 || c1[2] != 150 || c0[0] < 1 || c1[0] < c0[0]+120 {
		t.Fatalf("the columns' views are at %v and %v\n%s", c0, c1, a.Dump())
	}
	if cs := cellState(cell); cs["contentHidden"] != false {
		t.Fatalf("the row's views are hidden: %v", cs)
	}
}

// The Cursor prop: a view's cursor rect (AppKit's cursor), which the views
// in it that set none show too, a descendant's own winning, and a text
// field keeping its I-beam.
func TestCursor(t *testing.T) {
	a := Run(t, `
		app {
		  Column {
		    cursor: .pointer
		    padding: 20
		    Text("inherits")
		    Text("own") { cursor: .resizeHorizontal }
		    Text("hidden") { cursor: .hidden }
		    Text("grab") { cursor: .grab }
		    TextField { placeholder: "field" }
		  }
		}
	`)
	tr := a.UntilText("own")
	var c *Node
	for _, n := range tr.All() {
		if n.Str("cursor") == "pointingHand" {
			c = n
		}
	}
	if c == nil {
		t.Fatalf("no view has the pointing hand:\n%s", tr.Raw())
	}
	if c.Bool("cursorFirst") != true {
		t.Fatalf("the cursor's view isn't under the children")
	}
	if n := tr.Find("Text", "inherits"); n.Str("cursor") != "" || n.Str("effectiveCursor") != "pointingHand" {
		t.Fatalf("inherits: %v %v", n.Str("cursor"), n.Str("effectiveCursor"))
	}
	own := tr.Find("Text", "own")
	if own.Str("cursor") != "frameResizeHorizontal" && own.Str("cursor") != "resizeLeftRight" {
		t.Fatalf("own: %q", own.Str("cursor"))
	}
	if n := tr.Find("Text", "hidden"); n.Str("cursor") != "hidden" {
		t.Fatalf("hidden: %q", n.Str("cursor"))
	}
	if n := tr.Find("Text", "grab"); n.Str("cursor") != "openHand" {
		t.Fatalf("grab: %q", n.Str("cursor"))
	}
	for _, n := range tr.Kind("TextField") {
		if n.Str("effectiveCursor") != "iBeam" {
			t.Fatalf("the field shows %q", n.Str("effectiveCursor"))
		}
	}
}

// undoState is the tree's undo state (the host's "undo" JSON).
func undoState(t *testing.T, tr *Tree) map[string]any {
	t.Helper()
	var all map[string]any
	if err := json.Unmarshal([]byte(tr.Raw()), &all); err != nil {
		t.Fatal(err)
	}
	u, _ := all["undo"].(map[string]any)
	if u == nil {
		t.Fatalf("no undo state:\n%s", tr.Raw())
	}
	return u
}

// Edit > Undo is the app's undo stack's while no text field is edited
// ("Undo Add", the Undo event), and the field's own while one is: its
// typing undone, the app's stack untouched. The MenuBar's undo and redo
// items are the host's own Edit items: not twice.
func TestUndoThroughEditMenu(t *testing.T) {
	a := Run(t, `
		component Counter {
		  state n = 0
		  fn add() {
		    n += 1
		    undo.register("Add", undo: () => { n -= 1 }, redo: () => { n += 1 })
		  }
		  MenuBar {
		    Menu("Edit") {
		      MenuItem("") { role: .undo }
		      MenuItem("") { role: .redo }
		    }
		  }
		  Column {
		    Text("n {n}")
		    Button("Add") { on press { add() } }
		    TextField { placeholder: "field" }
		  }
		}
		app { Counter() }
	`)
	a.UntilText("n 0")
	a.Do("tap Add", "tap Add")
	tr := a.UntilText("n 2")
	tr = a.Until(func(tr *Tree) bool { return undoState(t, tr)["undoTitle"] == "Undo Add" })
	u := undoState(t, tr)
	if u["canUndo"] != true || u["canRedo"] != false || u["windowManager"] != true {
		t.Fatalf("the undo state: %v", u)
	}
	// The host's Edit menu: Undo and Redo once each.
	edit := tr.Menu("Edit")
	count := 0
	for _, it := range edit.Items {
		if strings.HasPrefix(it.Title, "Undo") || strings.HasPrefix(it.Title, "Redo") {
			count++
		}
	}
	if count != 2 {
		t.Fatalf("the Edit menu: %v", edit.Items)
	}
	a.Do("menu Undo Add")
	a.UntilText("n 1")
	a.Until(func(tr *Tree) bool { return undoState(t, tr)["redoTitle"] == "Redo Add" })
	a.Do("menu Redo Add")
	a.UntilText("n 2")

	// A field being edited undoes its own typing.
	a.Do("tap field", "type hello")
	a.Until(func(tr *Tree) bool {
		f := tr.Kind("TextField")
		return len(f) > 0 && f[0].Str("stringValue") == "hello"
	})
	tr = a.Tree()
	if u := undoState(t, tr); u["undoTitle"] == "Undo Add" || u["canUndo"] != true {
		t.Fatalf("a field being edited, Undo is the app's: %v", u)
	}
	a.Do("keyin cmd+z")
	a.Until(func(tr *Tree) bool {
		f := tr.Kind("TextField")
		return len(f) > 0 && f[0].Str("stringValue") == ""
	})
	if tr := a.Tree(); tr.FindText("n 2") == nil {
		t.Fatalf("the field's undo reached the app:\n%s", strings.Join(a.Texts(), "\n"))
	}
}
