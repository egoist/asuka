//go:build darwin

package mactest

import (
	"math"
	"strconv"
	"strings"
	"testing"
)

// AppKit's style mask bits.
const (
	styleClosable        = 1 << 1
	styleMiniaturizable  = 1 << 2
	styleNonactivating   = 1 << 7
	styleFullSizeContent = 1 << 15
)

// winNum is a number of a window's JSON.
func winNum(w *Window, key string) float64 { f, _ := w.Props[key].(float64); return f }

// winRect is a [x, y, width, height] of a window's JSON.
func winRect(w *Window, key string) []float64 {
	l, _ := w.Props[key].([]any)
	out := make([]float64, len(l))
	for i, x := range l {
		out[i], _ = x.(float64)
	}
	return out
}

func near(a, b float64) bool { return math.Abs(a-b) <= 1 }

// A Window opens a window of its own with its title and content size, its
// content laid out at its size; the user closing it closes it through the
// core: open false, close raised (TestWindow).
func TestNavWindow(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  state open = false
		  state closed = 0
		  Column {
		    padding: 20
		    Text("open {open} closed {closed}")
		    Button("Open") { on press { open = true } }
		    Window {
		      bind open: open
		      title: "Inspector"
		      width: 300
		      height: 200
		      on close { closed += 1 }
		      Text("width {environment().width}")
		    }
		  }
		}
	`)
	a.UntilText("open false closed 0")
	a.Do("tap Open")
	tr := a.Until(func(tr *Tree) bool { w := tr.Window("Inspector"); return w != nil && w.Visible })
	w := tr.Window("Inspector")
	if w.Content[2] != 300 || w.Content[3] != 200 {
		t.Fatalf("content %v", w.Content)
	}
	if w.Main {
		t.Fatal("the Window is the main window")
	}
	a.UntilText("width 300")
	// The user closes it: the core closes it.
	a.Do("closewindow Inspector")
	a.UntilText("open false closed 1")
	a.Until(func(tr *Tree) bool { return tr.Window("Inspector") == nil })
}

// The app block's Window is the main window: its title, its size (which
// the core lays out at) and its least size (TestWindowOptions).
func TestNavWindowOptions(t *testing.T) {
	a := Run(t, `
		app {
		  Window {
		    title: "Notes"
		    width: 420
		    height: 500
		    minWidth: 400
		    minHeight: 300
		    Text("width {environment().width} height {environment().height}")
		  }
		}
	`)
	a.UntilText("width 420 height 500")
	tr := a.Until(func(tr *Tree) bool { return tr.Main().Title == "Notes" })
	m := tr.Main()
	if s := winRect(m, "minSize"); len(s) != 2 || s[0] != 400 || s[1] != 300 {
		t.Fatalf("least size %v", m.Props["minSize"])
	}
	// Nothing is remembered under automation.
	if n, _ := m.Props["autosaveName"].(string); n != "" {
		t.Fatalf("autosaved as %q", n)
	}
	// The user resizes it: its environment follows.
	a.Do("resize 500,450")
	a.UntilText("width 500 height 450")
}

// A Window's options become its NSWindow's (no resize or minimize button,
// a hidden title bar with its room as SafeTop, floating, out of the Window
// menu, a most size, a background); its handle's actions act on it, and
// the state AppKit reports, the user's moving it too, comes back to the
// handle's reads (TestWindowControl).
func TestNavWindowControl(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  state open = true
		  Column {
		    padding: 20
		    Window as w {
		      bind open: open
		      title: "Tool"
		      width: 300
		      height: 200
		      maxWidth: 600
		      maxHeight: 500
		      hidden: true
		      resizable: false
		      minimizable: false
		      alwaysOnTop: true
		      skipTaskbar: true
		      titleBar: .hidden
		      background: #ff0000
		      Text("top {environment().safeTop}")
		    }
		    Text("visible {w.visible} size {w.windowState.width}x{w.windowState.height} top {w.alwaysOnTop} at {w.windowState.x},{w.windowState.y}")
		    Button("Act") {
		      on press {
		        w.show()
		        w.setSize(320, 240)
		        w.setPosition(100, 120)
		        w.setAlwaysOnTop(false)
		      }
		    }
		    Button("Hide") { on press { w.hide() } }
		  }
		}
	`)
	// Hidden as it opens, its state reported at once.
	a.Until(func(tr *Tree) bool { return textWithPrefix(tr, "visible false size 300x200 top true at ") })
	tr := a.Until(func(tr *Tree) bool { return tr.Window("Tool") != nil })
	w := tr.Window("Tool")
	mask := int(winNum(w, "styleMask"))
	if mask&styleResizable != 0 || mask&styleMiniaturizable != 0 || mask&styleClosable == 0 || mask&styleFullSizeContent == 0 {
		t.Fatalf("style mask %b", mask)
	}
	if w.Visible {
		t.Fatal("a hidden window shows")
	}
	if winNum(w, "level") != 3 || w.Props["excludedFromWindowsMenu"] != true || w.Props["titlebarTransparent"] != true || w.Props["titleHidden"] != true {
		t.Fatalf("level %v, excluded %v, transparent %v", w.Props["level"], w.Props["excludedFromWindowsMenu"], w.Props["titlebarTransparent"])
	}
	if s := winRect(w, "maxSize"); len(s) != 2 || s[0] != 600 || s[1] != 500 {
		t.Fatalf("most size %v", w.Props["maxSize"])
	}
	if w.Content[2] != 300 || w.Content[3] != 200 {
		t.Fatalf("content %v", w.Content)
	}
	if bg, _ := w.Props["background"].(string); bg == "" || bg[:7] != "#ff0000" {
		t.Fatalf("background %v", w.Props["background"])
	}
	// The content goes under the title bar, and knows how far.
	top := winNum(w, "safeTop")
	if top < 20 {
		t.Fatalf("a hidden title bar's room is %v", top)
	}
	a.UntilText("top " + ftoa(top))
	// The handle's actions.
	a.Do("tap Act")
	a.UntilText("visible true size 320x240 top false at 100,120")
	tr = a.Until(func(tr *Tree) bool { w := tr.Window("Tool"); return w != nil && w.Visible && winNum(w, "level") == 0 })
	w = tr.Window("Tool")
	if w.Content[2] != 320 || w.Content[3] != 240 {
		t.Fatalf("content after setSize %v", w.Content)
	}
	if tl := winRect(w, "topLeft"); !near(tl[0], 100) || !near(tl[1], 120) {
		t.Fatalf("top left after setPosition %v", tl)
	}
	// The user moves it: reported.
	a.Do("movewindow Tool 140,160")
	a.UntilText("visible true size 320x240 top false at 140,160")
	a.Do("tap Hide")
	a.Until(func(tr *Tree) bool { return textWithPrefix(tr, "visible false size 320x240") })
	a.Until(func(tr *Tree) bool { w := tr.Window("Tool"); return w != nil && !w.Visible })
}

// textWithPrefix reports whether a node shows text starting with p.
func textWithPrefix(tr *Tree, p string) bool {
	for _, n := range tr.All() {
		if strings.HasPrefix(n.Text, p) {
			return true
		}
	}
	return false
}

// ftoa is a float as Asuka shows it in a string (300, 28.5).
func ftoa(f float64) string { return strconv.FormatFloat(f, 'f', -1, 64) }

// The user closing a window whose app asks about it: it stays, the app is
// asked, and the core closes it once the app agrees; the main window's
// closing asks too (TestWindowCloseRequestDarwin).
func TestNavWindowCloseRequest(t *testing.T) {
	a := Run(t, `
		state asked = 0
		state mainAsked = 0
		app {
		  Window {
		    on closeRequest { mainAsked += 1 }
		    Root()
		  }
		}
		component Root {
		  state open = true
		  state allow = false
		  Column {
		    padding: 20
		    Text("open {open} asked {asked} main {mainAsked}")
		    Button("Allow") { on press { allow = true } }
		    Window {
		      bind open: open
		      title: "W"
		      width: 200
		      height: 100
		      on closeRequest {
		        asked += 1
		        if allow { open = false }
		      }
		      Text("w")
		    }
		  }
		}
	`)
	a.UntilText("open true asked 0 main 0")
	a.Until(func(tr *Tree) bool { w := tr.Window("W"); return w != nil && w.Visible })
	a.Do("closewindow W")
	a.UntilText("open true asked 1 main 0")
	if w := a.Tree().Window("W"); w == nil || !w.Visible {
		t.Fatal("a refused close closed the window")
	}
	a.Do("tap Allow", "closewindow W")
	a.UntilText("open false asked 2 main 0")
	a.Until(func(tr *Tree) bool { return tr.Window("W") == nil })
	// The main window's: asked, kept.
	a.Do("close")
	a.UntilText("open false asked 2 main 1")
	if !a.Tree().Main().Visible || a.Exited() {
		t.Fatal("the main window closed without the app")
	}
}

// A modal Window is a sheet of the window it's declared in, a child one a
// child window of it; the main window's hidden title bar: its room in the
// environment (TestChildWindowsDarwin).
func TestNavChildWindows(t *testing.T) {
	a := Run(t, `
		app {
		  Window {
		    titleBar: .hidden
		    width: 500
		    height: 400
		    Root()
		  }
		}
		component Root {
		  state modal = false
		  state child = false
		  Column {
		    padding: 20
		    Text("top {environment().safeTop}")
		    Button("Modal") { on press { modal = !modal } }
		    Button("Child") { on press { child = !child } }
		    Window {
		      bind open: modal
		      modal: true
		      title: "M"
		      width: 200
		      height: 100
		      Column {
		        Text("m")
		        Button("Done") { on press { modal = false } }
		      }
		    }
		    Window {
		      bind open: child
		      child: true
		      title: "C"
		      Text("c")
		    }
		  }
		}
	`)
	tr := a.Until(func(tr *Tree) bool { return winNum(tr.Main(), "safeTop") >= 20 })
	a.UntilText("top " + ftoa(winNum(tr.Main(), "safeTop")))
	main := tr.Main().Title
	a.Do("tap Modal")
	tr = a.Until(func(tr *Tree) bool { w := tr.Window("M"); return w != nil && w.Props["sheetParent"] == main })
	if s, _ := tr.Main().Props["sheet"].(map[string]any); s == nil || s["title"] != "M" {
		t.Fatalf("the main window's sheet: %v", tr.Main().Props["sheet"])
	}
	a.Do("tap Done")
	a.Until(func(tr *Tree) bool { _, has := tr.Main().Props["sheet"]; return tr.Window("M") == nil && !has })
	a.Do("tap Child")
	a.Until(func(tr *Tree) bool { w := tr.Window("C"); return w != nil && w.Props["parent"] == main && w.Visible })
	a.Do("tap Child")
	a.Until(func(tr *Tree) bool {
		c, _ := tr.Main().Props["childWindows"].([]any)
		return tr.Window("C") == nil && len(c) == 0
	})
}

// The main window's handle (a Ref of a view in it): hide and show again,
// as a tray icon's menu does (TestMainWindowHideFocus). A view's ref's
// focus() is the keyboard's (a Window element's brings it to the front).
func TestNavMainWindowHideFocus(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  Column as c {
		    padding: 20
		    Text("visible {c.visible}")
		    Button("Blink") {
		      on press {
		        c.hide()
		        await sleep(2s)
		        c.show()
		      }
		    }
		  }
		}
	`)
	a.UntilText("visible true")
	a.Do("tap Blink")
	a.Until(func(tr *Tree) bool { return !tr.Main().Visible })
	a.UntilText("visible false")
	a.Until(func(tr *Tree) bool { return tr.Main().Visible })
	a.UntilText("visible true")
}

// Click-through: ignoresMouseEvents, switched from the handle, and
// reported (TestClickThroughDarwin).
func TestNavClickThrough(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  state open = true
		  Column {
		    padding: 20
		    Window as w {
		      bind open: open
		      title: "HUD"
		      clickThrough: true
		      transparent: true
		      alwaysOnTop: true
		      width: 200
		      height: 100
		      Text("HUD")
		    }
		    Text("through {w.clickThrough}")
		    Button("Catch") { on press { w.setClickThrough(false) } }
		  }
		}
	`)
	a.UntilText("through true")
	tr := a.Until(func(tr *Tree) bool { return tr.Window("HUD") != nil })
	if w := tr.Window("HUD"); w.Props["ignoresMouse"] != true || w.Props["opaque"] != false || winNum(w, "level") != 3 {
		t.Fatalf("ignores the mouse %v, opaque %v, level %v", w.Props["ignoresMouse"], w.Props["opaque"], w.Props["level"])
	}
	a.Do("tap Catch")
	a.UntilText("through false")
	a.Until(func(tr *Tree) bool { return tr.Window("HUD").Props["ignoresMouse"] == false })
}

// A click-through window's views that still take the mouse (takesClicks):
// the window lets the mouse through but over them.
func TestNavClickRegion(t *testing.T) {
	a := Run(t, `
		app { Root() }
		component Root {
		  state open = true
		  Column {
		    padding: 20
		    Text("main")
		    Window {
		      bind open: open
		      title: "HUD"
		      clickThrough: true
		      width: 200
		      height: 100
		      Column {
		        padding: 10
		        Text("CPU 12%")
		        View {
		          takesClicks: true
		          width: 60
		          height: 30
		          margin: [10, 0, 0, 20]
		          Text("Hide")
		        }
		      }
		    }
		  }
		}
	`)
	tr := a.Until(func(tr *Tree) bool {
		w := tr.Window("HUD")
		r, _ := w.Props["clickRegion"].([]any)
		return w != nil && len(r) == 1
	})
	w := tr.Window("HUD")
	r := w.Props["clickRegion"].([]any)[0].([]any)
	got := []float64{r[0].(float64), r[2].(float64), r[3].(float64)}
	if w.Props["clickThrough"] != true || got[0] != 30 || got[1] != 60 || got[2] != 30 || r[1].(float64) < 30 {
		t.Fatalf("click-through %v, region %v: want one rect at x 30, 60×30, below the text", w.Props["clickThrough"], r)
	}
}

// A window that shows a document: the dot in its close button while it's
// edited, its file in the title bar (a document Window's edited and file).
func TestNavDocumentWindow(t *testing.T) {
	a := RunWith(t, `
		app { Root() }
		component Root {
		  state edited = false
		  state file = ""
		  state open = true
		  Column {
		    padding: 20
		    Button("Edit") { on press { edited = true } }
		    Button("Save") { on press { edited = false; file = "/tmp/notes.txt" } }
		    Window {
		      bind open: open
		      title: "notes.txt"
		      edited: edited
		      file: file
		      Text("doc")
		    }
		  }
		}
	`, Options{})
	a.Until(func(tr *Tree) bool { w := tr.Window("notes.txt"); return w != nil && w.Visible })
	a.Do("tap Edit")
	a.Until(func(tr *Tree) bool {
		w := tr.Window("notes.txt")
		return w.Props["edited"] == true && w.Props["file"] == nil
	})
	a.Do("tap Save")
	a.Until(func(tr *Tree) bool {
		w := tr.Window("notes.txt")
		f, _ := w.Props["file"].(string)
		return w.Props["edited"] == false && (f == "/tmp/notes.txt" || f == "/private/tmp/notes.txt")
	})
}
