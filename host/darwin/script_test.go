//go:build darwin && !ios

package darwin

import (
	"testing"

	. "github.com/egoist/asuka/internal/darwin"
	"github.com/egoist/asuka/ops"
	"github.com/egoist/asuka/signal"
	"github.com/egoist/asuka/ui"
)

// ASUKA_SCRIPT's steps press what shows a text, type into the field with
// the focus, press keys and choose the menu bar's items, as the user would.
func TestScriptSteps(t *testing.T) {
	adds, saves, cards := 0, 0, 0
	name := signal.New("")
	seg := signal.New(0)
	h, _ := mount(t, func() ui.Node {
		return ui.Column(ui.Style{Grow: 1},
			ui.MenuBar(signal.Static([]ui.MenuBarMenu{
				{Title: "File", Items: []ui.MenuItem{{Title: "Save", Shortcut: ui.Cmd("s"), OnPress: func() { saves++ }}}},
			})),
			ui.Button(ui.ButtonProps{Title: ui.Val("Add"), OnPress: func() { adds++ }}),
			ui.TextField(ui.TextFieldProps{Value: name, Placeholder: ui.Val("Name")}),
			ui.Segmented(ui.SegmentedProps{Segments: ui.Val([]string{"All", "Problems"}), Selected: seg}),
			ui.View(ui.ViewProps{OnPress: func() { cards++ }}, ui.StaticText("A card")),
		)
	})
	onMain(func() {
		Send(h.window, "orderFront:", 0) // steps act on what shows
		for _, s := range []string{"tap:Add", "tap:A card", "tap:Problems", "menu:Save", "tap:Name", "type:Ada"} {
			if err := h.step(s); err != nil {
				fatal(t, s, err)
			}
		}
		if adds != 1 || cards != 1 || seg.Peek() != 1 || saves != 1 {
			fatalf(t, "adds %d, cards %d, segment %d, saves %d: a step didn't land", adds, cards, seg.Peek(), saves)
		}
		if name.Peek() != "Ada" {
			fatalf(t, "typed %q", name.Peek())
		}
		// A shortcut goes through the menu bar, as a key pressed does.
		if err := h.step("key:cmd+s"); err != nil {
			fatal(t, err)
		}
		drain()
		if saves != 2 {
			fatalf(t, "⌘S saved %d times in all", saves-1)
		}
		for _, s := range []string{"tap:Nothing", "menu:Nothing", "key:hyper+k", "dance"} {
			if h.step(s) == nil {
				fatalf(t, "%s: no error", s)
			}
		}
	})
}

// tap: chooses a SplitView's destination by its row's title, over a text
// that shows the same words and takes no presses.
func TestScriptTapsSidebar(t *testing.T) {
	sel := signal.New(0)
	h, _ := mount(t, func() ui.Node {
		page := func(s string) func() ui.Node { return func() ui.Node { return ui.StaticText(s) } }
		return ui.SplitView(ui.SplitViewProps{Selected: sel, Sections: []ui.SidebarSection{{Destinations: []ui.Destination{
			{Title: ui.Val("Overview"), Content: page("Models")}, // its page repeats the next row's title
			{Title: ui.Val("Models"), Content: page("The models")},
		}}}})
	})
	onMain(func() {
		Send(h.window, "orderFront:", 0)
		if err := h.step("tap:Models"); err != nil {
			fatal(t, err)
		}
		if sel.Peek() != 1 {
			fatalf(t, "selected %d", sel.Peek())
		}
	})
}

// A SplitView's pages follow its content item's size whenever AppKit
// sets it, not only as the window reports a resize: a full-size content
// view's split (a hidden title bar's) sizes its items after that.
func TestSplitDetailFollowsItsFrame(t *testing.T) {
	h, _ := mount(t, func() ui.Node {
		return ui.SplitView(ui.SplitViewProps{Sections: []ui.SidebarSection{{Destinations: []ui.Destination{
			{Title: ui.Val("Home"), Content: func() ui.Node { return ui.StaticText("home") }},
		}}}})
	})
	onMain(func() {
		split := find(h, ops.KindSplit)[0]
		MsgSetSize(split.split.detail, Sel("setFrameSize:"), Size{Width: 640, Height: 480})
		screen := find(h, ops.KindScreen)[0]
		if s := MsgRect(screen.view, Sel("frame")).Size; s.Width != 640 || s.Height != 480 {
			fatalf(t, "the page is %vx%v, its pane 640x480", s.Width, s.Height)
		}
	})
}
