//go:build e2e

package e2e

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"
)

// A one-line field low in a scrolling page, tapped, is scrolled above
// the keyboard that opens for it, as a multiline one keeps its caret in
// sight.
func TestFieldAboveKeyboardAndroid(t *testing.T) {
	s := androidApp(t, "e2e/testdata/field-keyboard", "dev.asuka.fieldkeyboard")
	waitFor(t, s, []string{"ready"})
	left, top, right, bottom := s.fieldBounds()
	if _, err := s.adb("shell", "input", "tap", strconv.Itoa((left+right)/2), strconv.Itoa((top+bottom)/2)); err != nil {
		t.Fatal(err)
	}
	var state string
	until(t, func() bool {
		kb := s.keyboardTop()
		_, top, _, bottom := s.fieldBounds()
		state = fmt.Sprintf("field %d to %d, keyboard from %d", top, bottom, kb)
		return kb > 0 && bottom <= kb && top >= 0
	}, func() string { return state + ", want the field above the keyboard" })
}

// On iOS the automation focuses the field (the keyboard UIKit shows for
// it) and says where it and the keyboard are.
func TestFieldAboveKeyboardIOS(t *testing.T) {
	s := iosApp(t, "e2e/testdata/field-keyboard", "dev.asuka.fieldkeyboard")
	waitFor(t, s, []string{"ready"})
	s.command("type 1")
	var line string
	until(t, func() bool {
		file := filepath.Join(s.tmp, "asuka-focused.txt")
		os.Remove(file)
		s.command("focused")
		line = ""
		for range 50 {
			if b, err := os.ReadFile(file); err == nil {
				line = strings.TrimSpace(string(b))
				break
			}
			time.Sleep(100 * time.Millisecond)
		}
		var top, bottom, kb, window int
		if _, err := fmt.Sscanf(line, "field %d %d keyboard %d window %d", &top, &bottom, &kb, &window); err != nil {
			return false
		}
		// With a hardware keyboard connected, UIKit shows none: nothing to test.
		return kb < window && bottom <= kb && top >= 0
	}, func() string { return line + ", want a keyboard and the field above it" })
}

// accessoriesApp is the field-accessories app with its first field
// focused and the keyboard up. With a hardware keyboard connected UIKit
// shows the field's bar without the keys; if neither shows, there's
// nothing to test.
func accessoriesApp(t *testing.T) *iosScreen {
	s := iosApp(t, "e2e/testdata/field-accessories", "dev.asuka.fieldaccessories")
	waitFor(t, s, []string{"ready", "keyboard down"})
	s.tap("Focus first")
	deadline := time.Now().Add(within)
	for !hasAll(s.texts(), []string{"keyboard up", "Done first"}) {
		if time.Now().After(deadline) {
			t.Skip("neither the keyboard nor the first field's bar showed")
		}
		time.Sleep(200 * time.Millisecond)
	}
	// Wait for the keyboard to finish coming up: a move while it still
	// rises has nothing to drop. This wait can let a bug by, never fail
	// the fix.
	time.Sleep(time.Second)
	return s
}

// shown is the value of the line the app shows with prefix.
func shown(s *iosScreen, prefix string) string {
	for _, line := range s.texts() {
		if v, ok := strings.CutPrefix(line, prefix); ok {
			return v
		}
	}
	return ""
}

// seenHeights are the keyboard heights the app was given since a Focus
// button was pressed.
func seenHeights(t *testing.T, s *iosScreen) []int {
	t.Helper()
	var hs []int
	for _, f := range strings.Fields(shown(s, "seen: ")) {
		hs = append(hs, mustAtoi(t, f))
	}
	if len(hs) == 0 {
		t.Fatalf("no keyboard height seen: %q", s.texts())
	}
	return hs
}

// arrows is hs as "a → b → c".
func arrows(hs []int) string {
	var b strings.Builder
	for i, h := range hs {
		if i > 0 {
			b.WriteString(" → ")
		}
		b.WriteString(strconv.Itoa(h))
	}
	return b.String()
}

// keyboardAfter is the keyboard's height a second after focus moves to a
// field by its button from no field at all, the keyboard down: where the
// keyboard ends with no move between fields to get it wrong.
func keyboardAfter(t *testing.T, s *iosScreen, button string) int {
	t.Helper()
	s.tap("Dismiss")
	waitFor(t, s, []string{"keyboard down"})
	s.tap(button)
	time.Sleep(1500 * time.Millisecond)
	return mustAtoi(t, shown(s, "keyboard: "))
}

// Focus moving from one field to another, each with its own bar on the
// keyboard (an accessory), the second's taller, keeps what sits on the
// keyboard where it is but for the bar's growth. UIKit swaps the bars,
// and the keyboard's layout guide has it shrink by the old one's, then
// grow by the new one's, a few times in a row: followed, what sits on the
// keyboard drops and comes back up. With a hardware keyboard the guide
// stays at 0 after the swap.
func TestFieldAccessoriesIOS(t *testing.T) {
	s := accessoriesApp(t)
	// The second field's bar in place of the first's, then 300 ms more for
	// what UIKit reports as it swaps them (on the simulator, all within
	// about 50 ms of the focus moving). It waits only for the bar, which
	// shows with or without the fix, so the fix can't make it time out; a
	// swap slower than 300 ms could let the bug by.
	s.tap("Focus second")
	waitFor(t, s, []string{"Done second"}, "Done first")
	time.Sleep(300 * time.Millisecond)
	hs := seenHeights(t, s)
	t.Logf("the keyboard's heights: %s", arrows(hs))
	for _, h := range hs[1:] {
		if h < hs[0] {
			t.Fatalf("the keyboard's height went %s as focus moved, want it never to drop", arrows(hs))
		}
	}
	time.Sleep(time.Second)
	moved := mustAtoi(t, shown(s, "keyboard: "))
	if moved <= hs[0] {
		t.Fatalf("the keyboard's height went %s, then %d, want it taller with the second field's taller bar", arrows(hs), moved)
	}
	if direct := keyboardAfter(t, s, "Focus second"); moved != direct {
		t.Fatalf("the keyboard ended %d tall as focus moved to the second field, but it's %d with the focus given to that field alone", moved, direct)
	}
}

// Focus moving from a field with a bar to one without, whose keyboard is
// a number pad, is a real change: the keyboard's height goes down to the
// number pad's, without coming back up, ends where the number pad alone
// puts it, and the app has it with the keyboard's notification, not
// later; after it, the keyboard is followed as before. The bound on that is loose, 200 ms from the press: a loaded
// simulator took up to 81 ms, with the fix and without it alike.
func TestFieldKeyboardChangeIOS(t *testing.T) {
	s := accessoriesApp(t)
	s.tap("Focus third")
	waitFor(t, s, nil, "Done first")
	deadline := time.Now().Add(within)
	for shown(s, "moved after: ") == "-1" {
		if time.Now().After(deadline) {
			t.Fatalf("the keyboard's height never changed: %q", s.texts())
		}
		time.Sleep(100 * time.Millisecond)
	}
	time.Sleep(time.Second)
	hs := seenHeights(t, s)
	after := shown(s, "moved after: ")
	moved := mustAtoi(t, shown(s, "keyboard: "))
	t.Logf("the keyboard's heights: %s, the first change %s ms after the press", arrows(hs), after)
	if len(hs) < 2 || hs[len(hs)-1] >= hs[0] {
		t.Fatalf("the keyboard's height went %s, want it lower for the number pad", arrows(hs))
	}
	for i := 1; i < len(hs); i++ {
		if hs[i] > hs[i-1] {
			t.Fatalf("the keyboard's height went %s, want it only to go down", arrows(hs))
		}
	}
	if ms := mustAtoi(t, after); ms >= 200 {
		t.Fatalf("the keyboard's height changed %d ms after the press, want it with the keyboard's notification (under 200 ms)", ms)
	}
	if direct := keyboardAfter(t, s, "Focus third"); moved != direct {
		t.Fatalf("the keyboard ended %d tall as focus moved to the number pad, but it's %d with the focus given to that field alone", moved, direct)
	}
	// The move held nothing past it: the first field's keyboard comes back
	// as it was at the start.
	if again := keyboardAfter(t, s, "Focus first"); again != hs[0] {
		t.Fatalf("the first field's keyboard is %d tall after the moves, want %d as at the start", again, hs[0])
	}
}

// mustAtoi is s as a number; the test fails if it isn't one.
func mustAtoi(t *testing.T, s string) int {
	t.Helper()
	v, err := strconv.Atoi(s)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

var boundsRE = regexp.MustCompile(`class="android\.widget\.EditText"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"`)

// fieldBounds is the first text field's frame on screen, in pixels, from UiAutomator.
func (s *androidScreen) fieldBounds() (l, t, r, b int) {
	out, _ := s.uiDump()
	m := boundsRE.FindStringSubmatch(out)
	if m == nil {
		s.t.Fatalf("no text field in:\n%s", out)
	}
	n := func(i int) int { v, _ := strconv.Atoi(m[i]); return v }
	return n(1), n(2), n(3), n(4)
}

var imeRE = regexp.MustCompile(`touchable region=SkRegion\(\(\d+,(\d+),\d+,\d+\)\)`)

// keyboardTop is where the keyboard's window starts on screen, in pixels (0: none shown).
func (s *androidScreen) keyboardTop() int {
	if out, _ := s.adb("shell", "dumpsys", "input_method"); !regexp.MustCompile(`mInputShown=true`).MatchString(out) {
		return 0
	}
	out, _ := s.adb("shell", "dumpsys", "window", "InputMethod")
	m := imeRE.FindStringSubmatch(out)
	if m == nil {
		return 0
	}
	v, _ := strconv.Atoi(m[1])
	return v
}
