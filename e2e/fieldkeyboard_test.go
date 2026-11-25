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
