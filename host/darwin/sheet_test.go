//go:build darwin && !ios

package darwin

import (
	"testing"

	. "github.com/egoist/asuka/internal/darwin"
	"github.com/egoist/asuka/ops"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// A full-screen sheet is a sheet of its window as big as all of that
// window's content under its title bar and toolbar, which the user
// doesn't resize; it follows the window as it resizes, its environment
// with it; Escape dismisses it as it does a sheet.
func TestFullScreenSheetDarwin(t *testing.T) {
	open := signal.New(false)
	var env ui.Environment
	h, _ := mount(t, func() ui.Node {
		return ui.Sheet(ui.SheetProps{Open: open, FullScreen: true, Content: func() ui.Node {
			e := ui.UseEnvironment()
			signal.Effect(func() { env = e.Get() })
			return ui.TextField(ui.TextFieldProps{Value: signal.New("")})
		}})
	})
	onMain(func() {
		Send(h.window, "makeKeyAndOrderFront:", 0)
		post(h, func() { open.Set(true) })
		sheets := find(h, ops.KindSheet)
		if len(sheets) != 1 || sheets[0].nav.window == 0 {
			fatal(t, "no sheet window")
		}
		s := sheets[0]
		w := s.nav.window
		if Send(w, "sheetParent") != h.window {
			fatal(t, "not a sheet of the main window")
		}
		if Send(w, "styleMask")&styleResizable != 0 {
			fatal(t, "a full-screen sheet the user resizes")
		}
		covers := func(what string) {
			t.Helper()
			want := MsgRect(h.window, Sel("contentLayoutRect")).Size
			got := MsgRect(Send(w, "contentView"), Sel("bounds")).Size
			if got != want {
				fatalf(t, "%s: the sheet is %v, the window's content %v", what, got, want)
			}
			if env.Width != float32(want.Width) || env.Height != float32(want.Height) {
				fatalf(t, "%s: its environment is %v×%v, the window's content %v", what, env.Width, env.Height, want)
			}
			if b := MsgRect(s.view, Sel("frame")).Size; b != want {
				fatalf(t, "%s: its content is %v", what, b)
			}
		}
		covers("opened")

		// The window resizes: the sheet follows.
		r := MsgRect(h.content, Sel("bounds")).Size
		MsgSetSize(h.window, Sel("setContentSize:"), Size{Width: r.Width + 120, Height: r.Height + 80})
		drain()
		covers("the window grown")
		MsgSetSize(h.window, Sel("setContentSize:"), Size{Width: r.Width - 60, Height: r.Height - 40})
		drain()
		covers("the window shrunk")

		// Escape in its content (cancelOperation:) dismisses it.
		h.enter(func() { Send(s.view, "cancelOperation:", 0) })
		h.runPosted()
		if open.Peek() {
			fatal(t, "Escape left it open")
		}
	})
}

// A sheet as tall as its content (DetentFit) opens at its content's
// height, at a sheet's width, follows its content as it grows and
// shrinks, and goes no taller than a sheet may, beyond which its content
// scrolls; the user doesn't resize it.
func TestFitSheetDarwin(t *testing.T) {
	open, more, lots := signal.New(false), signal.New(false), signal.New(false)
	h, _ := mount(t, func() ui.Node {
		return ui.Sheet(ui.SheetProps{Open: open, Detents: ui.DetentFit, Content: func() ui.Node {
			return ui.Column(ui.Style{Padding: ui.All(20), Gap: 10},
				ui.View(ui.ViewProps{Style: ui.Style{Height: ui.Pt(100)}}),
				ui.Show(more, func() ui.Node { return ui.View(ui.ViewProps{Style: ui.Style{Height: ui.Pt(50)}}) }),
				ui.Show(lots, func() ui.Node {
					return ui.Scroll(ui.ScrollProps{Style: ui.Style{Shrink: 1}}, ui.View(ui.ViewProps{Style: ui.Style{Height: ui.Pt(5000)}}))
				}),
			)
		}})
	})
	onMain(func() {
		Send(h.window, "makeKeyAndOrderFront:", 0)
		post(h, func() { open.Set(true) })
		sheets := find(h, ops.KindSheet)
		if len(sheets) != 1 || sheets[0].nav.window == 0 {
			fatal(t, "no sheet window")
		}
		s := sheets[0]
		w := s.nav.window
		if Send(w, "sheetParent") != h.window {
			fatal(t, "not a sheet of the main window")
		}
		if Send(w, "styleMask")&styleResizable != 0 {
			fatal(t, "a sheet as tall as its content the user resizes")
		}
		room := sheetRoom(s, h.main)
		is := func(what string, height float64) {
			t.Helper()
			got := MsgRect(Send(w, "contentView"), Sel("bounds")).Size
			if got.Width != room.Width || got.Height != height {
				fatalf(t, "%s: the sheet is %v, want %vx%v", what, got, room.Width, height)
			}
			if b := MsgRect(s.view, Sel("frame")).Size; b != got {
				fatalf(t, "%s: its content is %v in a sheet of %v", what, b, got)
			}
		}
		is("opened", 140)
		post(h, func() { more.Set(true) })
		is("grown", 200)
		post(h, func() { more.Set(false) })
		is("shrunk", 140)
		post(h, func() { lots.Set(true) })
		is("too tall for a sheet", room.Height)
	})
}
