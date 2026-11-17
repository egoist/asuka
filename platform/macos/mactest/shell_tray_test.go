//go:build darwin

package mactest

import (
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// shellTray is the tray's icons as the automation's JSON has them.
func shellTray(t *testing.T, tr *Tree) []map[string]any {
	var out []map[string]any
	for _, x := range tr.Tray {
		out = append(out, x.(map[string]any))
	}
	return out
}

// shellItems are a JSON menu's items.
func shellItems(v any) []map[string]any {
	var out []map[string]any
	l, _ := v.([]any)
	for _, x := range l {
		out = append(out, x.(map[string]any))
	}
	return out
}

// A tray icon is a status item in the menu bar: its symbol a template
// image, its title, tooltip and menu (with a submenu, a checked, a
// disabled and a destructive item); a click is its on click, a chosen item
// its run; its title follows its binding; gone, it leaves the menu bar.
func TestShellTray(t *testing.T) {
	a := Run(t, `
		import platform/tray
		state clicks = 0
		state chose = ""
		state speed = "1.0 MB/s"
		state shown = true
		app {
		  Column {
		    Text("clicks {clicks} chose {chose}")
		    Button("Faster") { on press { speed = "12.5 MB/s" } }
		    Button("Hide") { on press { shown = false } }
		    if shown {
		      tray.Tray("power") {
		        symbol: "bolt.fill"
		        title: speed
		        tooltip: "Power"
		        menu: [
		          .button(title: "Open", run: () => { chose = "open" }),
		          .submenu(title: "Mode", items: [
		            .button(title: "Fast", checked: true, run: () => { chose = "fast" }),
		            .button(title: "Slow", disabled: true, run: () => {}),
		          ]),
		          .button(title: "Reset", role: .destructive, run: () => {}),
		          .section(title: "", items: [.button(title: "Quit Power", run: () => {})]),
		        ]
		        on click { clicks += 1 }
		      }
		    }
		  }
		}
	`)
	tr := a.Until(func(tr *Tree) bool { return len(tr.Tray) == 1 })
	ic := shellTray(t, tr)[0]
	if ic["image"] != true || ic["template"] != true {
		t.Fatalf("no template image: %v", ic)
	}
	if ic["title"] != "1.0 MB/s" || ic["tooltip"] != "Power" {
		t.Fatalf("title, tooltip: %v", ic)
	}
	// With on click, the menu isn't the item's own (a click is the app's).
	if ic["ownMenu"] != false || ic["clickable"] != true {
		t.Fatalf("the menu opens on a click: %v", ic)
	}
	menu := shellItems(ic["menu"])
	if len(menu) != 5 {
		t.Fatalf("%d items: %v", len(menu), menu)
	}
	mode := shellItems(menu[1]["items"])
	if menu[1]["title"] != "Mode" || len(mode) != 2 || mode[0]["checked"] != true || mode[1]["disabled"] != true {
		t.Fatalf("Mode: %v", menu[1])
	}
	if menu[2]["color"] == nil {
		t.Fatalf("Reset isn't red: %v", menu[2])
	}
	if menu[3]["separator"] != true || menu[4]["title"] != "Quit Power" {
		t.Fatalf("the section: %v", menu[3:])
	}

	a.Do("trayclick", "traymenu Fast")
	a.UntilText("clicks 1 chose fast")
	a.Do("traymenu Open")
	a.UntilText("clicks 1 chose open")
	if e := a.Try("traymenu Slow"); e == "" {
		t.Fatal("a disabled item was chosen")
	}

	a.Do("tap Faster")
	a.Until(func(tr *Tree) bool { return len(tr.Tray) == 1 && shellTray(t, tr)[0]["title"] == "12.5 MB/s" })

	a.Do("tap Hide")
	a.Until(func(tr *Tree) bool { return len(tr.Tray) == 0 })
}

// Without on click, the menu is the status item's own: AppKit opens it on
// any click. Without an image or a symbol, the icon is the app's.
func TestShellTrayMenuOnClick(t *testing.T) {
	a := Run(t, `
		import platform/tray
		app {
		  tray.Tray("star") { symbol: "star"; menu: [.button(title: "Open", run: () => {})] }
		  tray.Tray("plain") { title: "Plain" }
		  Text("x")
		}
	`)
	tr := a.Until(func(tr *Tree) bool { return len(tr.Tray) == 2 })
	icons := shellTray(t, tr)
	if icons[0]["ownMenu"] != true {
		t.Fatalf("no menu of the item's: %v", icons[0])
	}
	if icons[1]["image"] != true || icons[1]["template"] == true || icons[1]["title"] != "Plain" {
		t.Fatalf("an icon without an image: %v", icons[1])
	}
}

// A tray icon's image, a file's: loaded off the main thread, drawn 16
// points tall with its aspect kept, a template only when it says so, the
// image before the title; one that won't load shows the symbol instead.
func TestShellTrayImage(t *testing.T) {
	dir := t.TempDir()
	img := image.NewNRGBA(image.Rect(0, 0, 64, 32))
	for y := range 32 {
		for x := range 64 {
			img.Set(x, y, color.NRGBA{A: 255})
		}
	}
	path := filepath.Join(dir, "wide.png")
	f, _ := os.Create(path)
	png.Encode(f, img)
	f.Close()
	a := Run(t, `
		import platform/tray
		app {
		  tray.Tray("wide") { image: "`+path+`"; title: "Wide" }
		  tray.Tray("mask") { image: "`+path+`"; template: true }
		  tray.Tray("broken") { image: "`+filepath.Join(dir, "none.png")+`"; symbol: "star" }
		  Text("x")
		}
	`)
	tr := a.Until(func(tr *Tree) bool {
		if len(tr.Tray) != 3 {
			return false
		}
		for _, ic := range shellTray(t, tr) {
			if ic["image"] != true {
				return false
			}
		}
		return true
	})
	icons := shellTray(t, tr)
	size := func(ic map[string]any) []float64 {
		var r []float64
		for _, x := range ic["imageSize"].([]any) {
			r = append(r, x.(float64))
		}
		return r
	}
	if s := size(icons[0]); len(s) != 2 || s[0] != 32 || s[1] != 16 {
		t.Errorf("the image's size: %v, want 32×16", s)
	}
	if icons[0]["template"] != false || icons[1]["template"] != true {
		t.Errorf("template: %v, %v", icons[0]["template"], icons[1]["template"])
	}
	// NSImageLeading (7) with a title; NSImageOnly (1) without.
	if icons[0]["imagePosition"] != 7.0 || icons[1]["imagePosition"] != 1.0 {
		t.Errorf("image positions: %v, %v", icons[0]["imagePosition"], icons[1]["imagePosition"])
	}
	if icons[2]["template"] != true {
		t.Errorf("the image that won't load isn't its symbol: %v", icons[2])
	}
}

// The Dock menu: the application delegate's applicationDockMenu gives the
// app's items, a chosen one its run.
func TestShellDockMenu(t *testing.T) {
	a := Run(t, `
		import platform/tray
		state chose = ""
		app {
		  tray.DockMenu {
		    items: [
		      .button(title: "New Window", run: () => { chose = "new" }),
		      .submenu(title: "Status", items: [.button(title: "Away", run: () => { chose = "away" })]),
		    ]
		  }
		  Text("chose {chose}")
		}
	`)
	tr := a.Until(func(tr *Tree) bool { return len(tr.DockMenu) == 2 })
	if s := tr.DockMenu[1]; s.Title != "Status" || len(s.Items) != 1 || s.Items[0].Title != "Away" {
		t.Fatalf("no Away in Status: %+v", s)
	}
	a.Do("dockmenu Away")
	a.UntilText("chose away")
	a.Do("dockmenu New Window")
	a.UntilText("chose new")
}

// The Dock tile's progress is a view over its icon, taken off with none;
// an indeterminate one slides; attention is asked only while the app isn't
// active, and cancelled.
func TestShellDockModule(t *testing.T) {
	a := Run(t, sysCallSrc+`
		state step = 0
		app {
		  Column {
		    Button("Next") { on press { step += 1 } }
		    if step == 1 { Sys("dock", "progress", host.u8(1).concat(host.f64(0.5))) { label: "half" } }
		    if step == 2 { Sys("dock", "progress", host.u8(2).concat(host.f64(0))) { label: "sliding" } }
		    if step == 3 { Sys("dock", "progress", host.u8(4).concat(host.f64(0.3))) { label: "error" } }
		    if step == 4 { Sys("dock", "progress", host.u8(0).concat(host.f64(0))) { label: "none" } }
		    if step == 5 {
		      Sys("dock", "attention", host.u8(0)) { label: "attention" }
		      Sys("dock", "cancelAttention", host.u32(0)) { label: "cancel" }
		    }
		  }
		}
	`)
	tile := func(tr *Tree) map[string]any { m, _ := shellJSON(t, tr)["dockTile"].(map[string]any); return m }
	frame := func(v any) []float64 {
		var r []float64
		for _, x := range v.([]any) {
			r = append(r, x.(float64))
		}
		return r
	}

	a.Do("tap Next")
	sysPrefix(t, a, "half", "ok")
	tr := a.Until(func(tr *Tree) bool { return tile(tr) != nil })
	d := tile(tr)
	track, fill := frame(d["track"]), frame(d["fill"])
	if d["onTile"] != true || math.Abs(fill[2]-track[2]/2) > 1 {
		t.Fatalf("half: %v", d)
	}

	a.Do("tap Next")
	sysPrefix(t, a, "sliding", "ok")
	tr = a.Until(func(tr *Tree) bool { return tile(tr)["sliding"] == true })
	x := frame(tile(tr)["fill"])[0]
	time.Sleep(300 * time.Millisecond)
	a.Until(func(tr *Tree) bool { return frame(tile(tr)["fill"])[0] != x })

	a.Do("tap Next")
	sysPrefix(t, a, "error", "ok")
	tr = a.Until(func(tr *Tree) bool { return tile(tr)["sliding"] == false })
	if d := tile(tr); d["fillColor"] == "" || d["state"].(float64) != 4 {
		t.Fatalf("error: %v", d)
	}

	a.Do("tap Next")
	sysPrefix(t, a, "none", "ok")
	a.Until(func(tr *Tree) bool { return tile(tr) == nil })

	a.Do("tap Next")
	b := sysHex(t, a, "attention")
	if len(b) != 4 {
		t.Fatalf("attention: %x", b)
	}
	if active, _ := shellJSON(t, a.Tree())["app"].(map[string]any)["active"].(bool); active && binary.LittleEndian.Uint32(b) != 0 {
		t.Fatalf("attention asked while active: %x", b)
	}
	sysPrefix(t, a, "cancel", "ok")
}

// An app that keeps running doesn't quit with its last window; with its
// main window closed, its Dock icon brings the window back. Without
// keepRunning it quits with it.
func TestShellKeepRunning(t *testing.T) {
	a := RunWith(t, `app { Text("x") }`, Options{Env: []string{shellConfig(t, `{"keepRunning": true}`)}})
	app := shellJSON(t, a.Tree())["app"].(map[string]any)
	if app["terminatesAfterLastWindow"] != false || app["keepRunning"] != true {
		t.Fatalf("quits with its last window: %v", app)
	}
	a.Do("close")
	a.Until(func(tr *Tree) bool { return tr.Main() != nil && !tr.Main().Visible })
	if a.Exited() {
		t.Fatal("it quit")
	}
	a.Do("reopen")
	a.Until(func(tr *Tree) bool { return tr.Main().Visible })

	b := Run(t, `app { Text("x") }`)
	if app := shellJSON(t, b.Tree())["app"].(map[string]any); app["terminatesAfterLastWindow"] != true {
		t.Fatalf("keeps running without keepRunning: %v", app)
	}
}

// A background app is an accessory (no Dock icon, not in ⌘Tab), and keeps
// running without a window.
func TestShellBackground(t *testing.T) {
	a := RunWith(t, `app { Text("x") }`, Options{Env: []string{shellConfig(t, `{"background": true}`)}})
	app := shellJSON(t, a.Tree())["app"].(map[string]any)
	if app["policy"].(float64) != 1 || app["background"] != true || app["terminatesAfterLastWindow"] != false {
		t.Fatalf("the background app: %v", app)
	}
}
