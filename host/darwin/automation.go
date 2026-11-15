//go:build darwin && !ios

package darwin

import (
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/ebitengine/purego"

	. "github.com/egoist/asuka/internal/darwin"
)

// automation starts the debug automation: ASUKA_SCRIPT's steps
// (script.go), then ASUKA_SNAPSHOT (write the window as a PNG once it has
// shown, ASUKA_DUMP the view tree beside it) and quit.
func (h *Host) automation() {
	if os.Getenv("ASUKA_PRINT_WINDOW") != "" {
		println("asuka: window", SendInt(h.window, "windowNumber"))
	}
	script, path := os.Getenv("ASUKA_SCRIPT"), os.Getenv("ASUKA_SNAPSHOT")
	if script == "" && path == "" {
		return
	}
	go func() {
		time.Sleep(1500 * time.Millisecond) // images and fonts load
		if script != "" {
			h.runScript(script)
		}
		if path == "" {
			return
		}
		h.Post(func() {
			WithPool(func() {
				h.snapshot(path)
				if os.Getenv("ASUKA_DUMP") != "" {
					os.WriteFile(path+".txt", []byte(h.dump()+trayDump), 0o644)
				}
			})
			if os.Getenv("ASUKA_SNAPSHOT_STAY") == "" {
				os.Exit(0)
			}
		})
	}()
}

// snapshot writes the window's content as a PNG, and each tray icon's
// beside it (tray.go).
func (h *Host) snapshot(path string) {
	w := h.window
	if !SendBool(w, "isVisible") {
		w = h.keyWin().window // the main window hidden: the one in use
	}
	v := Send(w, "contentView")
	content := strings.HasSuffix(path, ".content.png")
	if f := Send(Send(w, "contentView"), "superview"); f != 0 && !content {
		v = f // the frame view: the title bar and toolbar too
	}
	// The window server's image, as the window shows: a view can't draw
	// what the system composites (Liquid Glass, a sidebar's or a bar's
	// material, which caching the view's display leaves blank). Drawn by
	// the view when the window server gives none.
	if !writeWindowPNG(w, v, path) {
		writeViewPNG(v, path)
	}
	trayDump = h.traySnapshots(path)
}

// trayDump is what the tray icons show, for the dump.
var trayDump string

var (
	cgWindowListCreateImageFromArray func(r Rect, windows uintptr, opts uint32) uintptr
	cgImageCreateWithInRect          func(img uintptr, r Rect) uintptr
	cgImageRelease                   func(img uintptr)
	cfArrayCreate                    func(alloc uintptr, values *uintptr, n int, callbacks uintptr) uintptr
	cfRelease                        func(obj uintptr)
	cgWindowCaptureAvailable         bool
)

func bindWindowCapture() bool {
	if cgImageRelease != nil {
		return cgWindowCaptureAvailable
	}
	lib := CoreGraphics()
	purego.RegisterLibFunc(&cgImageRelease, lib, "CGImageRelease")
	purego.RegisterLibFunc(&cgImageCreateWithInRect, lib, "CGImageCreateWithImageInRect")
	purego.RegisterLibFunc(&cfArrayCreate, CoreFoundation(), "CFArrayCreate")
	purego.RegisterLibFunc(&cfRelease, CoreFoundation(), "CFRelease")
	loadImageIO() // CGImageGetWidth
	// Gone from the SDK's headers since macOS 15, still in the library.
	if _, err := purego.Dlsym(lib, "CGWindowListCreateImageFromArray"); err == nil {
		purego.RegisterLibFunc(&cgWindowListCreateImageFromArray, lib, "CGWindowListCreateImageFromArray")
		cgWindowCaptureAvailable = true
	}
	return cgWindowCaptureAvailable
}

// windowStack is w and what shows over it, front first: its sheets and
// its child windows, theirs in turn.
func windowStack(w ID) []uintptr {
	var out []uintptr
	if s := Send(w, "attachedSheet"); s != 0 && SendBool(s, "isVisible") {
		out = append(out, windowStack(s)...)
	}
	for _, c := range ArrayItems(Send(w, "childWindows")) {
		if SendBool(c, "isVisible") {
			out = append(windowStack(c), out...)
		}
	}
	return append(out, uintptr(SendInt(w, "windowNumber")))
}

// writeWindowPNG writes window w as the window server composites it, with
// its sheets and child windows over it, cut to view v (the frame view: all
// of it), as a PNG. It reports false when there's no image to be had (the
// call gone, or the window not on screen).
func writeWindowPNG(w, v ID, path string) bool {
	if !bindWindowCapture() || !SendBool(w, "isVisible") {
		return false
	}
	const ignoreFraming, bestResolution = 1 << 0, 1 << 3
	ids := windowStack(w)
	arr := cfArrayCreate(0, &ids[0], len(ids), 0)
	defer cfRelease(arr)
	// The window's own bounds, in the window server's space (from the
	// main display's top left): a sheet or a child reaching past it is cut
	// to it, as the window is what's asked for.
	f := MsgRect(w, Sel("frame"))
	top := MsgRect(Send(Send(Class("NSScreen"), "screens"), "firstObject"), Sel("frame")).Size.Height
	bounds := Rect{Origin: Point{X: f.Origin.X, Y: top - f.Origin.Y - f.Size.Height}, Size: f.Size}
	img := cgWindowListCreateImageFromArray(bounds, arr, ignoreFraming|bestResolution)
	if img == 0 {
		return false
	}
	defer cgImageRelease(img)
	frame := Send(Send(w, "contentView"), "superview")
	if v != frame && frame != 0 {
		// The view's part of it: its rect in the frame view, from the top.
		fb := MsgRect(frame, Sel("bounds"))
		k := float64(cgImageGetWidth(img)) / fb.Size.Width
		r := MsgRectIDToRect(v, Sel("convertRect:toView:"), MsgRect(v, Sel("bounds")), frame)
		if !SendBool(frame, "isFlipped") {
			r.Origin.Y = fb.Size.Height - r.Origin.Y - r.Size.Height
		}
		cut := Rect{Origin: Point{X: r.Origin.X * k, Y: r.Origin.Y * k}, Size: Size{Width: r.Size.Width * k, Height: r.Size.Height * k}}
		part := cgImageCreateWithInRect(img, cut)
		if part == 0 {
			return false
		}
		defer cgImageRelease(part)
		img = part
	}
	rep := Send(Send(Class("NSBitmapImageRep"), "alloc"), "initWithCGImage:", img)
	defer Release(rep)
	data := Send(rep, "representationUsingType:properties:", 4, uintptr(Dict())) // PNG
	return os.WriteFile(path, Bytes(data), 0o644) == nil
}

// writeViewPNG writes what a view draws as a PNG.
func writeViewPNG(v ID, path string) {
	b := MsgRect(v, Sel("bounds"))
	rep := MsgRectToID(v, Sel("bitmapImageRepForCachingDisplayInRect:"), b)
	MsgRectIDVoid(v, Sel("cacheDisplayInRect:toBitmapImageRep:"), b, rep)
	data := Send(rep, "representationUsingType:properties:", 4, uintptr(Dict())) // PNG
	os.WriteFile(path, Bytes(data), 0o644)
}

// dump describes the native view tree: each node's kind, frame and what
// it shows.
func (h *Host) dump() string {
	var b strings.Builder
	var walk func(n *node, depth int)
	walk = func(n *node, depth int) {
		b.WriteString(strings.Repeat("  ", depth))
		fmt.Fprintf(&b, "%d %s", n.id, n.kind)
		if n.view != 0 {
			f := MsgRect(n.view, Sel("frame"))
			fmt.Fprintf(&b, " [%.0f,%.0f %.0fx%.0f]", f.Origin.X, f.Origin.Y, f.Size.Width, f.Size.Height)
			if SendBool(n.view, "isHidden") {
				b.WriteString(" hidden")
			}
			if Send(n.view, "superview") == 0 {
				b.WriteString(" detached")
			}
			if Send(n.view, "window") == 0 {
				b.WriteString(" nowindow")
			}
			if SendInt(n.view, "userInterfaceLayoutDirection") == 1 {
				b.WriteString(" rtl")
			}
		}
		if n.text != "" {
			fmt.Fprintf(&b, " %q", n.text)
		}
		if n.title != "" {
			fmt.Fprintf(&b, " title=%q", n.title)
		}
		b.WriteByte('\n')
		for _, c := range n.children {
			walk(c, depth+1)
		}
		if n.list != nil {
			for r, rv := range n.list.shown {
				for _, c := range rv.cells {
					b.WriteString(strings.Repeat("  ", depth+1))
					fmt.Fprintf(&b, "row %d:\n", r)
					walk(c.n, depth+2)
				}
			}
		}
	}
	if h.root != nil {
		walk(h.root, 0)
	}
	b.WriteString(h.dumpWindows())
	return b.String()
}

// dumpWindows describes the app's windows that show, front to back: their
// titles, the document's dot and file, and the File menu.
func (h *Host) dumpWindows() string {
	var b strings.Builder
	app := Send(Class("NSApplication"), "sharedApplication")
	for _, nw := range ArrayItems(Send(app, "orderedWindows")) {
		if h.winOf(nw) == nil || !SendBool(nw, "isVisible") {
			continue
		}
		fmt.Fprintf(&b, "window %q", GoString(Send(nw, "title")))
		if SendBool(nw, "isDocumentEdited") {
			b.WriteString(" edited")
		}
		if u := Send(nw, "representedURL"); u != 0 {
			fmt.Fprintf(&b, " file=%q", GoString(Send(u, "path")))
		}
		b.WriteByte('\n')
	}
	bar := Send(app, "mainMenu")
	if file := Send(bar, "itemAtIndex:", 1); file != 0 {
		m := Send(file, "submenu")
		b.WriteString("File:")
		for i := range SendInt(m, "numberOfItems") {
			it := Send(m, "itemAtIndex:", uintptr(i))
			if SendBool(it, "isSeparatorItem") {
				b.WriteString(" |")
				continue
			}
			fmt.Fprintf(&b, " %q", GoString(Send(it, "title")))
			if k := GoString(Send(it, "keyEquivalent")); k != "" {
				fmt.Fprintf(&b, "(%s)", k)
			}
		}
		b.WriteByte('\n')
	}
	return b.String()
}
