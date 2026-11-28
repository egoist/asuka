package windows_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"
)

// Tables (testdata/tables.asuka), driven with the mouse's own input: a
// Table of 100,000 rows in a ListView with owner data binds only the rows
// it shows, and reads their text back as the ListView asks for it; a
// header's click sorts (the app's SortBy) and the header's arrow follows;
// clicks with Shift select rows, the app hearing of it; a double click
// opens a row; a right click's menu runs its item. An Outline's folder
// loads its children as its disclosure is clicked.
func TestTables(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "tables.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"snap:start",
		"header:Size", "wait:300ms", "snap:sorted",
		"header:Size", "wait:300ms", "snap:desc",
		"row:2", "row:4,shift", "wait:300ms", "snap:selected",
		"rowdbl:3", "wait:300ms", "snap:opened",
		"rowmenu:6", "wait:500ms", "snap:menu", "key:down", "key:down", "key:enter", "wait:300ms", "snap:chose",
		"row:99999", "wait:300ms", "snap:end",
		"chevron:Home", "wait:300ms", "snap:expanded",
		"itemdbl:A", "wait:300ms", "snap:outlineopen",
		"chevron:Home", "wait:300ms", "snap:collapsed")
	for _, k := range []string{"start", "sorted", "selected", "menu", "expanded"} {
		t.Logf("%s:\n%s", k, d[k])
	}
	// Only what shows is bound: a page of rows, out of 100,000.
	rows := regexp.MustCompile(`rows=(100000) cells=(\d+) bound=(\d+)`)
	m := rows.FindStringSubmatch(d["start"])
	if m == nil || m[1] != "100000" {
		t.Fatalf("no table of 100,000 rows:\n%s", d["start"])
	}
	if cells, _ := strconv.Atoi(m[2]); cells == 0 || cells > 200 {
		t.Errorf("%s cells made for 100,000 rows", m[2])
	}
	shows(t, d["start"], "the start", []string{"File 0", "File 1", "Home", "Work"})
	if !strings.Contains(d["start"], `header: "Name":200 "Size":100`) {
		t.Errorf("the header isn't Name, Size, unsorted:\n%s", d["start"])
	}
	// Sorted by size, then the other way: the arrow is the app's sort.
	if !strings.Contains(d["sorted"], `"Size"^:`) || !strings.Contains(d["sorted"], `row 0: "File 0" "0"`) {
		t.Errorf("a click on Size doesn't sort by it:\n%s", d["sorted"])
	}
	if !strings.Contains(d["desc"], `"Size"v:`) || !strings.Contains(d["desc"], `"999"`) {
		t.Errorf("a second click on Size doesn't sort it the other way:\n%s", d["desc"])
	}
	if !strings.Contains(d["selected"], "selected=[2+3]") {
		t.Errorf("a click and a shift click don't select rows 2 to 4:\n%s", d["selected"])
	}
	if !regexp.MustCompile(`"Selected File \d+, File \d+, File \d+"`).MatchString(d["selected"]) {
		t.Errorf("the app doesn't hear of the selection:\n%s", d["selected"])
	}
	if !strings.Contains(d["opened"], `"Opened File `) {
		t.Errorf("a double click doesn't open its row:\n%s", d["opened"])
	}
	if !strings.Contains(d["menu"], "selected=[6+1]") {
		t.Errorf("a right click doesn't select its row first:\n%s", d["menu"])
	}
	if !strings.Contains(d["chose"], `"Chose Delete File `) {
		t.Errorf("the menu's item doesn't run:\n%s", d["chose"])
	}
	if !strings.Contains(d["end"], `row 99999`) {
		t.Errorf("scrolled to the end, the last row isn't shown:\n%s", d["end"])
	}
	if m := rows.FindStringSubmatch(d["end"]); m == nil || m[2] == "0" {
		t.Errorf("no cells at the end:\n%s", d["end"])
	} else if cells, _ := strconv.Atoi(m[2]); cells > 200 {
		t.Errorf("scrolling made %d cells", cells)
	}
	// The outline: Home's children load as it expands.
	shows(t, d["expanded"], "Home expanded", []string{"A", "B", "Expanded Home"})
	if !strings.Contains(d["expanded"], `v "Home"`) {
		t.Errorf("Home isn't shown expanded:\n%s", d["expanded"])
	}
	shows(t, d["outlineopen"], "A opened", []string{"Opened Home/A"})
	shows(t, d["collapsed"], "Home collapsed", []string{"Home"}, "Expanded Home")
	if strings.Contains(d["collapsed"], `"A"`) {
		t.Errorf("Home's children stay after it collapses:\n%s", d["collapsed"])
	}
}

// The pointer's shape (Cursor): a hand over a view that sets it, the
// arrow elsewhere, a drawn open hand over one that grabs, a field's
// I-beam.
func TestCursor(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "tables.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"pointer:Hand here", "wait:300ms", "snap:hand",
		"pointer:Count 0", "wait:300ms", "snap:arrow",
		"pointer:Grab here", "wait:300ms", "snap:grab",
		"pointer:Note", "wait:300ms", "snap:field")
	for name, want := range map[string]string{"hand": "hand", "arrow": "arrow", "grab": "grab", "field": "ibeam"} {
		if !strings.Contains(d[name], "cursor: "+want+"\n") {
			t.Errorf("%s: not the %s cursor:\n%s", name, want, d[name][strings.LastIndex(d[name], "cursor:"):])
		}
	}
}

// Undo: Ctrl+Z reaches the app (the Edit menu's Undo, "Undo Add") where
// no field has the keyboard, and the field being typed in undoes its own
// typing instead.
func TestUndo(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "tables.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"tap:Add", "tap:Add", "snap:added",
		"focus:Add", "key:ctrl+z", "wait:300ms", "snap:undone",
		"key:ctrl+y", "wait:300ms", "snap:redone",
		"tap:Note", "type:hi", "wait:300ms", "key:ctrl+z", "wait:300ms", "snap:field")
	shows(t, d["added"], "added", []string{"Count 2"})
	if !strings.Contains(d["added"], `Undo Add\tCtrl+Z"`) {
		t.Errorf("the Edit menu doesn't say Undo Add:\n%s", d["added"])
	}
	shows(t, d["undone"], "Ctrl+Z", []string{"Count 1"}, "Count 2")
	shows(t, d["redone"], "Ctrl+Y", []string{"Count 2"}, "Count 1")
	// The field's typing undone, the app's count kept.
	shows(t, d["field"], "Ctrl+Z in the field", []string{"Count 2"}, "hi")
}

const undoNoMenu = `
component U {
  state count = 0
  Column {
    gap: 8; padding: 16
    Button("Add") {
      on press {
        count += 1
        undo.register("Add", undo: () => { count -= 1 }, redo: () => { count += 1 })
      }
    }
    Text("Count {count}")
  }
}
app { U() }
`

// Without a menu bar's Undo, Ctrl+Z and Ctrl+Shift+Z are the root's Undo
// event: the app's undo and redo.
func TestUndoKeys(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, undoNoMenu, local)
	d := script(t, w, local, guest, "app.exe",
		"tap:Add", "tap:Add", "focus:Add",
		"key:ctrl+z", "wait:300ms", "snap:undone",
		"key:ctrl+shift+z", "wait:300ms", "snap:redone")
	shows(t, d["undone"], "Ctrl+Z", []string{"Count 1"})
	shows(t, d["redone"], "Ctrl+Shift+Z", []string{"Count 2"})
}

// examples/tables as it ships, light and dark: the table of files sorted,
// several selected, a row's context menu, a hundred thousand of them;
// the outline of folders expanded.
func TestTablesExample(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildExample(t, w, "tables", local)
	for _, scheme := range []string{"light", "dark"} {
		steps := []string{
			"snap:" + scheme + "-start",
			"header:Size", "wait:300ms",
			"row:1", "row:3,shift", "row:6,ctrl", "wait:300ms", "snap:" + scheme + "-selected",
			"rowmenu:3", "wait:500ms", "snap:" + scheme + "-menu", "escape", "wait:300ms",
			"chevron:Documents", "wait:300ms", "item:Folder 2", "wait:300ms", "snap:" + scheme + "-outline",
			"tap:100,000 files", "wait:1s", "row:50000", "wait:300ms", "snap:" + scheme + "-many",
		}
		var s []string
		for _, st := range steps {
			if name, ok := strings.CutPrefix(st, "snap:"); ok {
				st = "snap:" + guest + `\` + name + ".png"
			}
			s = append(s, st)
		}
		out, err := w.run(t, guest, guest+`\tables.exe`, map[string]string{"ASUKA_SCRIPT": strings.Join(s, ";"), "ASUKA_DUMP": "1", "ASUKA_COLOR_SCHEME": scheme, "ASUKA_SNAPSHOT": guest + `\` + scheme + "-end.png"})
		if err != nil {
			t.Fatalf("%v\n%s", err, out)
		}
		read := func(name string) string {
			b, err := os.ReadFile(filepath.Join(local, scheme+"-"+name+".png.txt"))
			if err != nil {
				t.Fatalf("no dump %s: %v\n%s", name, err, out)
			}
			return string(b)
		}
		shows(t, read("start"), scheme, []string{"200 files · 0 selected", "Home", "Documents"})
		if d := read("selected"); !strings.Contains(d, "selected=[1+3 6+1]") || !strings.Contains(d, "200 files · 4 selected") {
			t.Errorf("%s: a click, a shift click and a ctrl click don't select 4 rows:\n%s", scheme, d)
		}
		if _, err := os.Stat(filepath.Join(local, scheme+"-menu.menu.png")); err != nil {
			t.Errorf("%s: no context menu shows", scheme)
		}
		shows(t, read("outline"), scheme, []string{"Folder 1", "Folder 2"})
		if d := read("many"); !strings.Contains(d, "rows=100000") || !strings.Contains(d, "selected=[50000+1]") {
			t.Errorf("%s: no 100,000 files, row 50,000 selected:\n%s", scheme, d)
		}
	}
}

const viewRows = `
component V {
  state pressed = "none"
  Column {
    grow: 1
    Text("Pressed {pressed}")
    Table {
      grow: 1
      columns: [TableColumn(id: "name", title: "Name", width: 160), TableColumn(id: "go", title: "Action", width: 120)]
      for i in 0..<500 key i {
        TableRow {
          Text("Item {i}")
          Button("Go {i}") { on press { pressed = "Go {i}" } }
        }
      }
    }
  }
}
app { V() }
`

// A Table's rows of views: each row's cell a window over its row, its
// views at their columns' places, a button in it pressed; scrolled, the
// cells follow their rows.
func TestTableViewRows(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, viewRows, local)
	d := script(t, w, local, guest, "app.exe",
		"snap:start", "tap:Go 2", "wait:300ms", "snap:pressed",
		"row:300", "wait:300ms", "snap:scrolled", "tap:Go 299", "wait:300ms", "snap:pressed2")
	shows(t, d["start"], "the rows", []string{"Item 0", "Go 0", "Item 3"})
	// Each row's views in their columns' boxes: the button's after the
	// name's 160 points.
	if !regexp.MustCompile(`View \[160,0 120x32\]\n\s+\d+ Button \[0,0 \d+x32\] title="Go 0"`).MatchString(d["start"]) {
		t.Errorf("the buttons aren't in their column:\n%s", d["start"])
	}
	shows(t, d["pressed"], "pressed", []string{"Pressed Go 2"})
	shows(t, d["scrolled"], "scrolled", []string{"Item 300", "Go 299"})
	shows(t, d["pressed2"], "pressed after scrolling", []string{"Pressed Go 299"})
}

const rtlTables = `
component R {
  state sort: [SortKey] = []
  state selection: [int] = []
  state open: [string] = ["Home"]
  Row {
    grow: 1
    layoutDirection: .rightToLeft
    Outline(["Home", "Work"]) {
      width: 200
      childrenOf: id => if id == "Home" { ["Home/A", "Home/B"] } else { [] }
      text: id => id.split("/").last() ?? id
      icon: id => "folder"
      bind expanded: open
    }
    Table {
      grow: 1
      columns: [TableColumn(id: "name", title: "Name", width: 200, sortable: true), TableColumn(id: "size", title: "Size", width: 100, align: .end)]
      selectionMode: .multiple
      bind sort: sort
      bind selection: selection
      for i in 0..<50 key i {
        TableRow(["Row {i}", "{i * 10}"]) { icon: "doc" }
      }
    }
  }
}
app { R() }
`

// Right to left: the table's first column at its right, the outline
// indented from the right, each still selecting and sorting.
func TestTablesRTL(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, rtlTables, local)
	d := script(t, w, local, guest, "app.exe", "row:2", "wait:300ms", "header:Name", "wait:300ms", "snap:rtl")
	if !strings.Contains(d["rtl"], `header: "Name"^:200 "Size":100`) {
		t.Errorf("right to left, the header isn't Name then Size (from the right), sorted:\n%s", d["rtl"])
	}
	shows(t, d["rtl"], "rtl", []string{"Row 0", "A", "B"})
}

// The user moves and resizes a Table's columns with the mouse (the VM's
// own, swiftvm's drag): Size dragged before Name, Name's end dragged
// past its least width, which holds it; the app hears of both
// (ColumnLayout: columnMove, columnResize).
func TestTableColumns(t *testing.T) {
	w := onWindows(t)
	if w.vm == "" {
		t.Skip("the mouse's drags are the VM's (swiftvm drag)")
	}
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "tables.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	done := make(chan map[string]string, 1)
	go func() {
		done <- script(t, w, local, guest, "app.exe",
			"snap:before", "waitfor:"+guest+`\moved.go`, "wait:500ms", "snap:moved",
			"waitfor:"+guest+`\resized.go`, "wait:500ms", "snap:resized")
	}()
	header := func(name string) []int {
		at := regexp.MustCompile(`header at: (\d+)-(\d+) (\d+)-(\d+) y=(\d+)`)
		for end := time.Now().Add(time.Minute); time.Now().Before(end); time.Sleep(200 * time.Millisecond) {
			b, err := os.ReadFile(filepath.Join(local, name+".png.txt"))
			if m := at.FindStringSubmatch(string(b)); err == nil && m != nil {
				var v []int
				for _, s := range m[1:] {
					n, _ := strconv.Atoi(s)
					v = append(v, n)
				}
				return v
			}
		}
		t.Fatalf("no header in %s", name)
		return nil
	}
	drag := func(x0, y0, x1, y1 int, then string) {
		out, err := exec.Command("swiftvm", "drag", w.vm, strconv.Itoa(x0), strconv.Itoa(y0), strconv.Itoa(x1), strconv.Itoa(y1)).CombinedOutput()
		if err != nil {
			t.Errorf("swiftvm drag: %v\n%s", err, out)
		}
		os.WriteFile(filepath.Join(local, then), []byte("x"), 0o644)
	}
	h := header("before") // Name a-b, Size c-d, y
	drag((h[2]+h[3])/2, h[4], h[0]+4, h[4], "moved.go")
	h = header("moved") // Size, then Name
	drag(h[3]-1, h[4], h[3]-1-(h[3]-h[2]), h[4], "resized.go")
	d := <-done
	if !strings.Contains(d["moved"], `header: "Size":100 "Name":200`) || !strings.Contains(d["moved"], `"Columns size at 0"`) {
		t.Errorf("Size dragged before Name isn't first:\n%s", d["moved"])
	}
	if !strings.Contains(d["resized"], `header: "Size":100 "Name":120`) || !strings.Contains(d["resized"], `"Columns name 120"`) {
		t.Errorf("Name dragged narrower isn't at its least, 120 points:\n%s", d["resized"])
	}
}

const outlineRows = `
component O {
  state open: [string] = ["Home"]
  state pressed = "none"
  Column {
    grow: 1
    Text("Pressed {pressed}")
    Outline(["Home", "Work"]) {
      grow: 1
      childrenOf: id => if id == "Home" { ["Home/A", "Home/B"] } else { [] }
      bind expanded: open
      row: id => Row {
        gap: 8
        align: .center
        Text("Item {id}")
        Button("Go {id}") { on press { pressed = id } }
      }
    }
  }
}
app { O() }
`

// An Outline's rows of views (row:): each cell after its disclosure and
// indent, its button pressed.
func TestOutlineViewRows(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, outlineRows, local)
	d := script(t, w, local, guest, "app.exe", "snap:start", "tap:Go Home/B", "wait:300ms", "snap:pressed")
	shows(t, d["start"], "the rows", []string{"Item Home", "Item Home/A", "Item Home/B", "Item Work"})
	shows(t, d["pressed"], "pressed", []string{"Pressed Home/B"})
}
