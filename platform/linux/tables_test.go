package linux_test

import (
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// tableLine is the dump's line of a Table's or an Outline's GTK view
// ("table rows=… bound=…", "outline rows=…").
func tableLine(t *testing.T, dump, kind string) string {
	t.Helper()
	for _, l := range strings.Split(dump, "\n") {
		if strings.HasPrefix(strings.TrimSpace(l), kind+" rows=") {
			return strings.TrimSpace(l)
		}
	}
	t.Fatalf("no %s in the dump:\n%s", kind, dump)
	return ""
}

var boundRe = regexp.MustCompile(`rows=(\d+) bound=(\d+)`)

// rowsBound are a table's rows and how many of them are bound.
func rowsBound(t *testing.T, line string) (int, int) {
	t.Helper()
	m := boundRe.FindStringSubmatch(line)
	if m == nil {
		t.Fatalf("no rows and bound in %q", line)
	}
	rows, _ := strconv.Atoi(m[1])
	bound, _ := strconv.Atoi(m[2])
	return rows, bound
}

// A Table (testdata/tables.asuka) as a GtkColumnView, with real input
// through XTest: its rows' RowData in GTK's cells, bound only as GTK
// makes their rows (it keeps about 200 around what shows: 100,000 rows
// bind no more, End moves to the last and lets go of the first); a click on a header
// sends SortBy and the header's arrow shows the app's sort (Name, then
// Size up, then down); a click selects (RowsSelected), a double click
// opens (Open), a right click selects the row and shows its menu, whose
// item runs (Action on the row's cell).
func TestTable(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "tables.asuka"), exe)
	d := script(t, dir, exe,
		"snap:start",
		"click:Size", "wait:300ms", "snap:size",
		"click:Size", "wait:300ms", "snap:down",
		"click:Name", "wait:300ms", "snap:name",
		"click:file 3", "wait:700ms", "snap:selected",
		"press:enter", "wait:300ms", "snap:entered",
		"dclick:file 5", "wait:700ms", "snap:opened",
		"rclick:file 7", "wait:500ms", "snap:menu",
		"click:Remove", "wait:500ms", "snap:removed",
		"tap:Many", "wait:1s", "snap:many",
		"click:file 2", "press:end", "wait:700ms", "snap:end")

	start := tableLine(t, d["start"], "table")
	if !strings.Contains(start, "columns name:") || !strings.Contains(start, " size:") || !strings.Contains(start, "sorted=name:ascending") {
		t.Errorf("the columns and the sort as the app gave them: %s", start)
	}
	if rows, bound := rowsBound(t, start); rows != 200 || bound == 0 || bound > 200 {
		t.Errorf("200 rows, each bound once at most: %s", start)
	}
	shows(t, d["start"], "the rows' text", []string{"file 0", "0 B", "file 1", "919 B", "sort name up"})

	shows(t, d["size"], "a click on Size", []string{"sort size up"})
	if l := tableLine(t, d["size"], "table"); !strings.Contains(l, "sorted=size:ascending") {
		t.Errorf("the header's arrow isn't the app's sort: %s", l)
	}
	// The smallest first: file 0 (0 B), then file 104 (16 B)…
	if !strings.Contains(d["size"], `row 0 "file 0" "0 B"`) {
		t.Errorf("the rows aren't the app's, by size:\n%s", d["size"])
	}
	shows(t, d["down"], "Size again", []string{"sort size down"})
	if l := tableLine(t, d["down"], "table"); !strings.Contains(l, "sorted=size:descending") {
		t.Errorf("the header's arrow isn't the app's sort: %s", l)
	}
	shows(t, d["name"], "a click on Name", []string{"sort name up"})
	if l := tableLine(t, d["name"], "table"); !strings.Contains(l, "sorted=name:ascending") {
		t.Errorf("the header's arrow isn't the app's sort: %s", l)
	}

	shows(t, d["selected"], "a click on a row", []string{"1 selected"})
	if l := tableLine(t, d["selected"], "table"); !strings.Contains(l, "selected=3+1") {
		t.Errorf("row 3 isn't selected: %s", l)
	}
	shows(t, d["entered"], "Enter", []string{"opened file 3"})
	shows(t, d["opened"], "a double click", []string{"opened file 5"})
	shows(t, d["menu"], "a right click", []string{"1 selected"})
	if l := tableLine(t, d["menu"], "table"); !strings.Contains(l, "selected=7+1") {
		t.Errorf("a right click didn't select its row: %s", l)
	}
	if rows, _ := rowsBound(t, tableLine(t, d["removed"], "table")); rows != 199 {
		t.Errorf("the menu's Remove didn't run: %s", tableLine(t, d["removed"], "table"))
	}
	shows(t, d["removed"], "file 7 removed", []string{"file 6", "file 8"}, "file 7")

	many := tableLine(t, d["many"], "table")
	if rows, bound := rowsBound(t, many); rows != 100000 || bound == 0 || bound > 250 {
		t.Errorf("100,000 rows, only those around what shows bound: %s", many)
	}
	shows(t, d["many"], "100,000 rows", []string{"file 0"})
	end := tableLine(t, d["end"], "table")
	if _, bound := rowsBound(t, end); bound > 250 {
		t.Errorf("at the end, rows bound and not let go: %s", end)
	}
	if !strings.Contains(end, "selected=99999+1") {
		t.Errorf("End didn't move to the last row: %s", end)
	}
	shows(t, d["end"], "End", []string{"file 99999"}, "file 0")
}

// An Outline as a GtkListView of a GtkTreeListModel: its roots; a row's
// expander sends Expand, the children the app loads then come with
// Levels (indented a level, under it); collapsed again; a double click
// opens (Open).
func TestOutline(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "tables.asuka"), exe)
	d := script(t, dir, exe,
		"snap:start",
		"tap:folder A", "wait:500ms", "snap:expanded",
		"tap:folder A", "wait:500ms", "snap:collapsed",
		"dclick:folder B", "wait:700ms", "snap:opened")
	if l := tableLine(t, d["start"], "outline"); !strings.HasPrefix(l, "outline rows=2 ") {
		t.Errorf("two roots: %s", l)
	}
	shows(t, d["start"], "the roots", []string{"folder A", "folder B"}, "folder A/x")
	if !strings.Contains(d["start"], `row 0 depth=0 collapsed gtk=collapsed "folder A"`) {
		t.Errorf("A collapsed, expandable:\n%s", d["start"])
	}
	if l := tableLine(t, d["expanded"], "outline"); !strings.HasPrefix(l, "outline rows=4 ") {
		t.Errorf("A expanded, its children loaded: %s\n%s", l, d["expanded"])
	}
	// Levels' and GTK's rows agree (gtk=: the disclosure GTK shows).
	for _, want := range []string{`row 0 depth=0 expanded gtk=expanded "folder A"`, `row 1 depth=1 gtk=leaf "folder A/x"`, `row 2 depth=1 gtk=leaf "folder A/y"`, `row 3 depth=0 collapsed gtk=collapsed "folder B"`} {
		if !strings.Contains(d["expanded"], want) {
			t.Errorf("expanded: no %s:\n%s", want, d["expanded"])
		}
	}
	if l := tableLine(t, d["collapsed"], "outline"); !strings.HasPrefix(l, "outline rows=2 ") {
		t.Errorf("A collapsed again: %s", l)
	}
	shows(t, d["collapsed"], "collapsed", []string{"folder A", "folder B"}, "folder A/x")
	shows(t, d["opened"], "a double click", []string{"opened B"})
}

// Cursor: a view's pointer shape is GTK's cursor of its widget, by its
// CSS name.
func TestCursor(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "tables.asuka"), exe)
	d := script(t, dir, exe, "snap:start")
	for _, l := range strings.Split(d["start"], "\n") {
		if strings.Contains(l, `"Pointer"`) {
			if !strings.Contains(l, " cursor=pointer") {
				t.Errorf("the pointer's shape isn't GTK's pointer: %s", l)
			}
			return
		}
	}
	t.Fatalf("no Pointer:\n%s", d["start"])
}

// Undo: the main menu's Undo and Redo say what (UndoState's name) and
// undo the app's change; Ctrl+Z in a field undoes its typing, not the
// app's; Ctrl+Z and Ctrl+Shift+Z outside it the app's.
func TestUndo(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "tables.asuka"), exe)
	d := script(t, dir, exe,
		"tap:Add", "tap:Add", "tap:Add", "snap:added",
		"menu:Undo Add", "snap:undone",
		"menu:Redo Add", "snap:redone",
		"click:Note", "press:a", "press:b", "press:c", "wait:300ms", "snap:typed",
		"press:ctrl+z", "wait:300ms", "snap:field",
		"click:file 1", "wait:300ms",
		"press:ctrl+z", "wait:300ms", "snap:app",
		"press:ctrl+shift+z", "wait:300ms", "snap:redo")
	shows(t, d["added"], "three adds", []string{"count 3"})
	shows(t, d["undone"], "Undo Add", []string{"count 2"})
	shows(t, d["redone"], "Redo Add", []string{"count 3"})
	shows(t, d["typed"], "typed", []string{"note abc", "count 3"})
	shows(t, d["field"], "Ctrl+Z in the field", []string{"count 3", "note "}, "note abc")
	shows(t, d["app"], "Ctrl+Z on the table", []string{"count 2"})
	shows(t, d["redo"], "Ctrl+Shift+Z", []string{"count 3"})
}

const undoKeys = `
component Counter {
  state count = 0
  Column {
    gap: 8; padding: 16
    Text("count {count}")
    Button("Add") {
      on press {
        count += 1
        undo.register("Add", undo: () => { count -= 1 }, redo: () => { count += 1 })
      }
    }
  }
}
app { Counter() }
`

// Ctrl+Z and Ctrl+Shift+Z with no Undo in the app's menus: Undo on the
// root where no text has the focus.
func TestUndoKeys(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, undoKeys, exe)
	d := script(t, dir, exe,
		"click:Add", "click:Add", "wait:300ms", "snap:added",
		"press:ctrl+z", "wait:300ms", "snap:undone",
		"press:ctrl+shift+z", "wait:300ms", "snap:redone")
	shows(t, d["added"], "two adds", []string{"count 2"})
	shows(t, d["undone"], "Ctrl+Z", []string{"count 1"})
	shows(t, d["redone"], "Ctrl+Shift+Z", []string{"count 2"})
}

const viewRows = `
struct Item {
  id: int
  name: string
}

component Views {
  state items = (0..<50).map(i => Item(id: i, name: "item {i}"))
  Row {
    grow: 1
    Outline(["P", "Q"]) {
      width: 200
      childrenOf: id => []
      row: id => Text("tree {id}") { color: .accent }
    }
    Table {
      grow: 1
      columns: [TableColumn(id: "name", title: "Name", width: 150), TableColumn(id: "tag", title: "Tag", width: 100)]
      for it in items key it.id {
        TableRow {
          Text(it.name)
          Text("tag {it.id}") { color: .secondaryLabel }
        }
      }
    }
  }
}
app { Views() }
`

var frameRe = regexp.MustCompile(`\[(-?\d+),(-?\d+) (\d+)x(\d+)\]`)

// Rows of views: a TableRow's views each in its column's cell, laid out at
// the column's width as GTK shows it (ColumnLayout: the last column takes
// what's left), and an Outline's row builder's view after the disclosure.
func TestTableViewRows(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, viewRows, exe)
	d := script(t, dir, exe, "snap:start")
	shows(t, d["start"], "the rows' views", []string{"item 0", "tag 0", "item 1", "tag 1", "tree P", "tree Q"})
	line := tableLine(t, d["start"], "table")
	m := regexp.MustCompile(`tag:(\d+)`).FindStringSubmatch(line)
	if m == nil {
		t.Fatalf("no Tag column: %s", line)
	}
	// The Tag column's box (the line over its text): at the Name column's
	// end, as wide as GTK shows the Tag column.
	lines := strings.Split(d["start"], "\n")
	for i, l := range lines {
		if !strings.Contains(l, `"tag 0"`) {
			continue
		}
		if strings.Contains(l, " unmapped") {
			t.Errorf("tag 0's view isn't shown: %s", l)
		}
		f := frameRe.FindStringSubmatch(lines[i-1])
		if f == nil || f[1] != "150" || f[3] != m[1] {
			t.Errorf("the Tag column's box isn't at 150, %s wide: %s", m[1], lines[i-1])
		}
		return
	}
	t.Fatalf("no tag 0:\n%s", d["start"])
}
