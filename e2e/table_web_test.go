//go:build e2e

package e2e

import (
	"fmt"
	"runtime"
	"strings"
	"testing"
	"time"
)

// TestTableWeb drives e2e/testdata/webtable in headless Chrome, with the
// pointer and the keyboard as a user's (DevTools' trusted input events):
//
//   - a Table of 100,000 rows shows a few dozen of them, their text in
//     its cells; a click on a sortable header sorts (aria-sort, the rows'
//     order); a click selects a row, Shift extends, the primary modifier
//     adds; a double click and Enter open;
//   - a Table whose rows are views lays them out at its columns;
//   - an Outline's disclosure expands a folder whose children load then;
//   - a view's Cursor is its CSS cursor;
//   - âZ (Ctrl+Z) outside a field undoes the app's change, â§âZ redoes
//     it, and in a field it's the field's own.
func TestTableWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	url := webServer(t, "e2e/testdata/webtable")
	d := devtoolsPage(t, browser, url, "1")
	var r struct{}
	if err := d.call("Emulation.setDeviceMetricsOverride", map[string]any{"width": 1100, "height": 700, "deviceScaleFactor": 1, "mobile": false}, &r); err != nil {
		t.Fatal(err)
	}
	waitText(t, d, "rows: 100000 selected: 0")
	settle(t, d)

	// Few rows of the 100,000 are in the page, and they show their text.
	var shown int
	if err := pageEval(d, `document.querySelector("table").querySelectorAll("tr.asuka-grid-row").length`, &shown); err != nil {
		t.Fatal(err)
	}
	if shown == 0 || shown > 80 {
		t.Fatalf("%d rows in the page, want a few dozen of 100,000", shown)
	}
	if got := tableEval(t, d, `document.querySelector("table").getAttribute("aria-rowcount")`); got != "100001" {
		t.Errorf("aria-rowcount %s, want 100001", got)
	}
	if got := tableRow(t, d, 0); got != "item 0|0" {
		t.Errorf("first row %q, want item 0|0", got)
	}

	// A click on Size's header sorts by it: what Sort says, and the app's order.
	tableClickText(t, d, "Size", 0)
	tableWait(t, func() bool { return tableRow(t, d, 1) == "item 1000|0" }, "the rows sorted by size")
	if got := tableEval(t, d, `[...document.querySelectorAll("th[aria-sort]")].map(th => th.textContent + " " + th.getAttribute("aria-sort")).join()`); got != "Size ascending" {
		t.Errorf("aria-sort: %q, want Size ascending", got)
	}

	// Selecting: a click, Shift extending, the primary modifier adding.
	primary := 2 // Ctrl
	if runtime.GOOS == "darwin" {
		primary = 4 // â
	}
	tableClickText(t, d, "item 1000", 0)
	waitText(t, d, "selected: 1 ")
	time.Sleep(600 * time.Millisecond) // not a double click
	tableClickText(t, d, "item 3000", 8)
	waitText(t, d, "selected: 3 ")
	time.Sleep(600 * time.Millisecond)
	tableClickText(t, d, "item 6000", primary)
	waitText(t, d, "selected: 4 ")
	if got := tableEval(t, d, `String(document.querySelector("table").querySelectorAll("tr[aria-selected=true]").length)`); got != "4" {
		t.Errorf("%s rows aria-selected, want 4", got)
	}

	// A double click opens; the keyboard moves the selection, and Enter opens.
	time.Sleep(600 * time.Millisecond)
	x, y := tableCenter(t, d, "item 2000")
	tableMouse(t, d, x, y, "left", 0, 1)
	tableMouse(t, d, x, y, "left", 0, 2)
	waitText(t, d, "selected: 1 opened: item 2000")
	tableKey(t, d, "ArrowDown", 40, 0)
	tableKey(t, d, "Enter", 13, 0)
	waitText(t, d, "selected: 1 opened: item 3000")

	// The Table of views: its views at its columns.
	if got := tableEval(t, d, `(() => { const t = document.querySelectorAll("table")[1]; const th = t.querySelectorAll("th")[1].getBoundingClientRect(); const v = [...t.querySelectorAll(".asuka-text")].find(e => e.textContent === "right 1").getBoundingClientRect(); return String(Math.round(v.left - th.left)); })()`); got != "0" {
		t.Errorf("a row's view is %spx from its column", got)
	}

	// The outline: Root's disclosure expands it; its children load and show.
	var at []float64
	pageEval(d, `(() => { const r = [...document.querySelectorAll(".asuka-tree-row")].find(r => r.textContent === "Root").querySelector(".asuka-tree-disclosure").getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`, &at)
	tableMouse(t, d, at[0], at[1], "left", 0, 1)
	waitText(t, d, "expanded: 1")
	waitText(t, d, "Root/a")
	if got := tableEval(t, d, `[...document.querySelectorAll(".asuka-tree-row")].filter(r => r.textContent.startsWith("Root")).map(r => r.textContent + ":" + r.getAttribute("aria-level") + ":" + (r.getAttribute("aria-expanded") || "")).join()`); got != "Root:1:true,Root/a:2:false,Root/b:2:false" {
		t.Errorf("the tree: %s", got)
	}

	// A view's cursor.
	if got := tableEval(t, d, `getComputedStyle([...document.querySelectorAll(".asuka-text")].find(e => e.textContent === "Pointer here")).cursor`); got != "pointer" {
		t.Errorf("the cursor over the view: %q, want pointer", got)
	}

	// Undo: outside a field, the app's; in a field, the field's.
	tableClickText(t, d, "Bump", 0)
	waitText(t, d, "count: 1")
	time.Sleep(600 * time.Millisecond)
	tableClickText(t, d, "Bump", 0)
	waitText(t, d, "count: 2")
	tableKey(t, d, "z", 90, primary)
	waitText(t, d, "count: 1")
	tableKey(t, d, "z", 90, primary|8)
	waitText(t, d, "count: 2")
	x, y = tableFieldCenter(t, d)
	tableMouse(t, d, x, y, "left", 0, 1)
	if err := d.call("Input.insertText", map[string]any{"text": "ab"}, &r); err != nil {
		t.Fatal(err)
	}
	tableWait(t, func() bool { return tableEval(t, d, `document.querySelector("input").value`) == "ab" }, "the field typed in")
	// The editing command a Mac's key bindings give ⌘Z, which the page's
	// keydown leaves to the field (DevTools' key events carry it).
	tableKey(t, d, "z", 90, primary, "undo")
	tableWait(t, func() bool { return tableEval(t, d, `document.querySelector("input").value`) != "ab" }, "the field's own undo")
	time.Sleep(300 * time.Millisecond)
	var body string
	pageEval(d, "document.body.innerText", &body)
	if !strings.Contains(body, "count: 2") {
		t.Errorf("âZ in a field undid the app's change too:\n%s", body)
	}
}

// tableEval is js's value, a string.
func tableEval(t *testing.T, d *devtools, js string) string {
	t.Helper()
	var s string
	if err := pageEval(d, js, &s); err != nil {
		t.Fatalf("%s: %v", js, err)
	}
	return s
}

// tableRow is the big table's row i's cells, joined by |.
func tableRow(t *testing.T, d *devtools, i int) string {
	return tableEval(t, d, fmt.Sprintf(`(() => { const r = document.querySelector("table").querySelector('tr.asuka-grid-row[aria-rowindex="%d"]'); return r ? [...r.querySelectorAll(".asuka-cell-text")].map(e => e.textContent).join("|") : ""; })()`, i+2))
}

func tableCenter(t *testing.T, d *devtools, text string) (float64, float64) {
	t.Helper()
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		if x, y, ok := textCenter(d, text); ok {
			return x, y
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("%q never showed", text)
	return 0, 0
}

func tableFieldCenter(t *testing.T, d *devtools) (float64, float64) {
	var p []float64
	if err := pageEval(d, `(() => { const r = document.querySelector("input").getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`, &p); err != nil || len(p) != 2 {
		t.Fatalf("no field: %v", err)
	}
	return p[0], p[1]
}

// tableClickText clicks the visible text with the modifiers held (CDP's: 2 Ctrl, 4 â, 8 Shift).
func tableClickText(t *testing.T, d *devtools, text string, mods int) {
	t.Helper()
	x, y := tableCenter(t, d, text)
	tableMouse(t, d, x, y, "left", mods, 1)
}

func tableMouse(t *testing.T, d *devtools, x, y float64, button string, mods, count int) {
	t.Helper()
	var r struct{}
	for _, typ := range []string{"mouseMoved", "mousePressed", "mouseReleased"} {
		p := map[string]any{"type": typ, "x": x, "y": y, "modifiers": mods}
		if typ != "mouseMoved" {
			p["button"] = button
			p["clickCount"] = count
		}
		if err := d.call("Input.dispatchMouseEvent", p, &r); err != nil {
			t.Fatal(err)
		}
	}
}

func tableKey(t *testing.T, d *devtools, key string, vk, mods int, commands ...string) {
	t.Helper()
	code := key
	if len(key) == 1 {
		code = "Key" + strings.ToUpper(key)
	}
	var r struct{}
	down := map[string]any{"type": "rawKeyDown", "key": key, "code": code, "windowsVirtualKeyCode": vk, "modifiers": mods}
	if len(commands) > 0 {
		down["commands"] = commands
	}
	if key == "Enter" {
		down["type"], down["text"] = "keyDown", "\r"
	}
	for _, p := range []map[string]any{down, {"type": "keyUp", "key": key, "code": code, "windowsVirtualKeyCode": vk, "modifiers": mods}} {
		if err := d.call("Input.dispatchKeyEvent", p, &r); err != nil {
			t.Fatal(err)
		}
	}
}

// tableWait waits (bounded) until ok, else fails saying what never came.
func tableWait(t *testing.T, ok func() bool, what string) {
	t.Helper()
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		if ok() {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("never: %s", what)
}
