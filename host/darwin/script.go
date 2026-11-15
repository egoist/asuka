//go:build darwin && !ios

package darwin

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	. "github.com/egoist/asuka/internal/darwin"
	"github.com/egoist/asuka/ops"
)

// ASUKA_SCRIPT's steps, separated by ";", as the Linux and Windows hosts
// run them:
//
//   - tap:<text>: presses what shows the text: a button, a toggle, a
//     segment, a sidebar's row, a tab, a toolbar's item, a view that takes
//     presses (or holds one); a field by its placeholder takes the focus
//   - type:<text>: types into the field with the focus; submit presses
//     Return in it
//   - key:<shortcut>: presses keys in the window in use (cmd+k,
//     shift+tab, escape, f5); ctrl and primary are ⌘, as on other systems
//     they're the shortcut key, and macctrl is ⌃
//   - back pops the screen, escape presses Escape, close closes the
//     window in use, as its close button
//   - scroll:<y> scrolls the innermost scroll view showing to y points
//   - resize:<w>,<h> sizes the main window's content
//   - menu:<title> chooses the menu bar's item, tab:<title> a tab
//   - wait:<duration> waits (500ms), snap:<file.png> writes a snapshot
//
// The script runs once the window has shown, each step after the last
// settles; then ASUKA_SNAPSHOT is written, if it's set.

// runScript runs the steps, from a goroutine: each on the main thread.
func (h *Host) runScript(script string) {
	for _, s := range strings.Split(script, ";") {
		s = strings.TrimSpace(s)
		if s == "" {
			continue
		}
		if d, ok := strings.CutPrefix(s, "wait:"); ok {
			if dur, err := time.ParseDuration(d); err == nil {
				time.Sleep(dur)
			}
			continue
		}
		done := make(chan struct{})
		h.Post(func() {
			defer close(done)
			WithPool(func() {
				if err := h.step(s); err != nil {
					fmt.Fprintln(os.Stderr, "asuka: script:", s+":", err)
				}
			})
		})
		<-done
		time.Sleep(400 * time.Millisecond) // animations settle
	}
}

func (h *Host) step(s string) error {
	verb, arg, _ := strings.Cut(s, ":")
	switch verb {
	case "tap":
		return h.tapText(arg)
	case "type":
		n := h.focusedField()
		if n == nil {
			return fmt.Errorf("no field has the focus")
		}
		h.insertText(n, arg)
	case "submit":
		return h.pressKeys("enter")
	case "key":
		return h.pressKeys(arg)
	case "escape":
		return h.pressKeys("escape")
	case "back":
		h.back(h.keyWin())
	case "close":
		w := h.keyWin()
		if w == nil {
			return fmt.Errorf("no window")
		}
		Send(w.window, "performClose:", 0)
	case "scroll":
		y, err := strconv.ParseFloat(arg, 64)
		if err != nil {
			return err
		}
		var s *node
		for _, n := range h.nodes {
			if (n.kind == ops.KindScroll || n.kind == ops.KindList) && h.showingNode(n) && (s == nil || n.id > s.id) {
				s = n
			}
		}
		if s == nil {
			return fmt.Errorf("no scroll view shows")
		}
		h.scrollToOffset(s, 0, y, false)
	case "resize":
		ws, hs, _ := strings.Cut(arg, ",")
		width, err1 := strconv.ParseFloat(ws, 64)
		height, err2 := strconv.ParseFloat(hs, 64)
		if err1 != nil || err2 != nil {
			return fmt.Errorf("want resize:<width>,<height>")
		}
		MsgSetSize(h.window, Sel("setContentSize:"), Size{Width: width, Height: height})
	case "menu":
		return h.chooseMenu(Send(Send(Class("NSApplication"), "sharedApplication"), "mainMenu"), arg)
	case "tab":
		if !h.tapTab(arg) {
			return fmt.Errorf("no tab %q", arg)
		}
	case "snap":
		h.snapshot(arg)
		if os.Getenv("ASUKA_DUMP") != "" {
			os.WriteFile(arg+".txt", []byte(h.dump()+trayDump), 0o644)
		}
	default:
		return fmt.Errorf("unknown step")
	}
	return nil
}

// showingNode reports whether n's view is in a window that shows, and
// neither it nor what holds it is hidden.
func (h *Host) showingNode(n *node) bool {
	if n.view == 0 || SendBool(n.view, "isHiddenOrHasHiddenAncestor") {
		return false
	}
	w := Send(n.view, "window")
	return w != 0 && SendBool(w, "isVisible")
}

// tapText presses what shows text, as a click on it would.
func (h *Host) tapText(text string) error {
	var found *node
	for _, n := range h.nodes {
		if !h.showingNode(n) {
			continue
		}
		if n.text == text && n.kind != ops.KindTextField || n.title == text && n.kind == ops.KindButton ||
			n.kind == ops.KindTextField && n.placeholder == text || n.a11yLabel == text {
			if found == nil || n.id > found.id {
				found = n
			}
		}
	}
	// AppKit's own: a sidebar's row, a segment, a tab, a toolbar item;
	// over a text that takes no presses (a title the content repeats).
	appKit := func() bool {
		return h.tapSidebar(text) || h.tapSegment(text) || h.tapTab(text) || h.tapToolbar(text)
	}
	if found == nil {
		if appKit() {
			return nil
		}
		return fmt.Errorf("nothing shows %q", text)
	}
	for t := found; t != nil; t = t.parent {
		switch {
		case t.kind == ops.KindTextField:
			Send(Send(t.view, "window"), "makeFirstResponder:", uintptr(t.view))
			return nil
		case t.kind == ops.KindButton || t.kind == ops.KindToggle:
			if t.menu != 0 && t.menuTrigger == int32(ops.MenuTriggerTap) {
				return fmt.Errorf("%q opens a menu, which would hold the script", text)
			}
			Send(t.view, "performClick:", 0)
			return nil
		case t.listening(ops.EventPress) || t.listening(ops.EventPressed):
			h.emit(t, ops.Event{Kind: ops.EventPressed, Bool: true})
			if h.alive(t) {
				h.emit(t, ops.Event{Kind: ops.EventPress})
			}
			if h.alive(t) {
				h.emit(t, ops.Event{Kind: ops.EventPressed, Bool: false})
			}
			return nil
		}
	}
	if appKit() {
		return nil
	}
	return fmt.Errorf("%q takes no presses", text)
}

// tapSidebar chooses a SplitView's destination by its title.
func (h *Host) tapSidebar(text string) bool {
	for table, n := range h.splits {
		if Send(n.view, "window") == 0 {
			continue
		}
		for i, r := range n.split.rows {
			if !r.group && r.title == text {
				set := Send(Class("NSIndexSet"), "indexSetWithIndex:", uintptr(i))
				Send(table, "selectRowIndexes:byExtendingSelection:", uintptr(set), 0)
				h.sidebarChose(table) // if AppKit didn't tell the delegate
				return true
			}
		}
	}
	return false
}

// tapSegment chooses a segmented control's segment by its label.
func (h *Host) tapSegment(text string) bool {
	for _, n := range h.nodes {
		if n.kind != ops.KindSegmented || !h.showingNode(n) {
			continue
		}
		for i := range SendInt(n.view, "segmentCount") {
			if GoString(Send(n.view, "labelForSegment:", uintptr(i))) == text {
				Send(n.view, "setSelectedSegment:", uintptr(i))
				Send(n.view, "sendAction:to:", uintptr(Send(n.view, "action")), uintptr(Send(n.view, "target")))
				return true
			}
		}
	}
	return false
}

// tapTab chooses a Tabs' tab by its title.
func (h *Host) tapTab(text string) bool {
	for _, n := range h.nodes {
		if n.kind != ops.KindTabs || !h.showingNode(n) {
			continue
		}
		for _, c := range n.children {
			if c.title == text && c.nav != nil && c.nav.tabItem != 0 {
				Send(n.view, "selectTabViewItem:", uintptr(c.nav.tabItem))
				return true
			}
		}
	}
	return false
}

// tapToolbar presses the window in use's toolbar item by its label.
func (h *Host) tapToolbar(text string) bool {
	w := h.keyWin()
	if w == nil {
		return false
	}
	tb := Send(w.window, "toolbar")
	if tb == 0 {
		return false
	}
	for _, it := range ArrayItems(Send(tb, "items")) {
		if GoString(Send(it, "label")) == text {
			h.barItem(it)
			return true
		}
	}
	return false
}

// focusedField is the text field a window's first responder edits: the
// front window's that has one (a sheet's, over its window).
func (h *Host) focusedField() *node {
	app := Send(Class("NSApplication"), "sharedApplication")
	wins := append([]ID{Send(app, "keyWindow")}, ArrayItems(Send(app, "orderedWindows"))...)
	for _, w := range wins {
		if w == 0 || !SendBool(w, "isVisible") {
			continue
		}
		r := Send(w, "firstResponder")
		if r != 0 && RespondsTo(r, "isFieldEditor") && SendBool(r, "isFieldEditor") {
			r = Send(r, "delegate") // a one-line field's editor: the field
		}
		for v := r; v != 0 && RespondsTo(v, "superview"); v = Send(v, "superview") {
			if n := h.byView[v]; n != nil && n.kind == ops.KindTextField {
				return n
			}
		}
	}
	return nil
}

// chooseMenu performs the item titled title, in menu or its submenus.
func (h *Host) chooseMenu(menu ID, title string) error {
	if menu == 0 {
		return fmt.Errorf("no menu bar")
	}
	for i, it := range ArrayItems(Send(menu, "itemArray")) {
		if sub := Send(it, "submenu"); sub != 0 {
			Send(sub, "update")
			if h.chooseMenu(sub, title) == nil {
				return nil
			}
			continue
		}
		if GoString(Send(it, "title")) == title {
			if !SendBool(it, "isEnabled") {
				return fmt.Errorf("%q is disabled", title)
			}
			Send(menu, "performActionForItemAtIndex:", uintptr(i))
			return nil
		}
	}
	return fmt.Errorf("no menu item %q", title)
}

// Key codes (a US keyboard's) and the characters they type.
var scriptKeys = map[string]struct {
	code  uint16
	chars string
}{
	"enter": {0x24, "\r"}, "return": {0x24, "\r"}, "escape": {0x35, "\x1b"}, "esc": {0x35, "\x1b"},
	"tab": {0x30, "\t"}, "space": {0x31, " "}, "backspace": {0x33, "\x7f"}, "delete": {0x75, ""},
	"up": {0x7e, ""}, "down": {0x7d, ""}, "left": {0x7b, ""}, "right": {0x7c, ""},
	"home": {0x73, ""}, "end": {0x77, ""}, "pageup": {0x74, ""}, "pagedown": {0x79, ""},
	"f1": {0x7a, ""}, "f2": {0x78, ""}, "f3": {0x63, ""}, "f4": {0x76, ""},
	"f5": {0x60, ""}, "f6": {0x61, ""}, "f7": {0x62, ""}, "f8": {0x64, ""},
	"f9": {0x65, ""}, "f10": {0x6d, ""}, "f11": {0x67, ""}, "f12": {0x6f, ""},
}

// charCodes are the keys of a US keyboard's characters.
var charCodes = map[rune]uint16{
	'a': 0, 's': 1, 'd': 2, 'f': 3, 'h': 4, 'g': 5, 'z': 6, 'x': 7, 'c': 8, 'v': 9, 'b': 11, 'q': 12,
	'w': 13, 'e': 14, 'r': 15, 'y': 16, 't': 17, '1': 18, '2': 19, '3': 20, '4': 21, '6': 22, '5': 23,
	'=': 24, '9': 25, '7': 26, '-': 27, '8': 28, '0': 29, ']': 30, 'o': 31, 'u': 32, '[': 33, 'i': 34,
	'p': 35, 'l': 37, 'j': 38, '\'': 39, 'k': 40, ';': 41, '\\': 42, ',': 43, '/': 44, 'n': 45, 'm': 46,
	'.': 47, '`': 50,
}

// pressKeys presses a shortcut (cmd+shift+k) in the window in use, as
// the window server would send it: the menu bar's key equivalents, the
// key monitor and the first responder see it.
func (h *Host) pressKeys(shortcut string) error {
	parts := strings.Split(strings.ToLower(shortcut), "+")
	var flags uint
	for _, m := range parts[:len(parts)-1] {
		switch m {
		case "cmd", "command", "ctrl", "control", "primary":
			flags |= nsEventModifierCommand
		case "macctrl":
			flags |= nsEventModifierControl
		case "shift":
			flags |= nsEventModifierShift
		case "alt", "option", "opt":
			flags |= nsEventModifierOption
		default:
			return fmt.Errorf("no modifier %q", m)
		}
	}
	key := parts[len(parts)-1]
	code, chars := uint16(0xffff), ""
	if k, ok := scriptKeys[key]; ok {
		code, chars = k.code, k.chars
	} else if r := []rune(key); len(r) == 1 {
		chars = key
		if c, ok := charCodes[r[0]]; ok {
			code = c
		}
	} else {
		return fmt.Errorf("no key %q", key)
	}
	w := h.keyWin()
	if w == nil {
		return fmt.Errorf("no window")
	}
	typed := chars
	if flags&nsEventModifierShift != 0 {
		typed = strings.ToUpper(chars)
	}
	app := Send(Class("NSApplication"), "sharedApplication")
	for _, typ := range []uint{10, 11} { // down, up
		ev := MsgKeyEvent(Class("NSEvent"), Sel("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:charactersIgnoringModifiers:isARepeat:keyCode:"),
			typ, Point{}, flags, MsgFloat(Send(Class("NSProcessInfo"), "processInfo"), Sel("systemUptime")), SendInt(w.window, "windowNumber"), 0,
			String(typed), String(chars), false, code)
		Send(app, "postEvent:atStart:", uintptr(ev), 0)
	}
	return nil
}
