//go:build e2e

package e2e

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// Screens already made when the system's text size changes are laid out
// again in the new size, the shown tab's and another tab's alike: their
// labels and button titles are bigger, and their frames hold them.
func TestTextSizeIOS(t *testing.T) {
	s := iosApp(t, "e2e/testdata/text-size", "dev.asuka.textsize")
	size := func(c string) {
		if out, err := exec.Command("xcrun", "simctl", "ui", s.udid, "content_size", c).CombinedOutput(); err != nil {
			t.Fatalf("content_size %s: %v\n%s", c, err, out)
		}
	}
	size("large")
	t.Cleanup(func() { size("large") })
	waitFor(t, s, []string{"ready", "Pill", "Below"})
	var lines string
	// grown waits until every label shown fits, each bigger than in
	// before (nil: any size), and says their sizes.
	grown := func(what string, before map[string]int) map[string]int {
		sizes := map[string]int{}
		until(t, func() bool {
			lines = s.fits()
			if lines == "" || strings.Contains(lines, "\tcut") {
				return false
			}
			clear(sizes)
			for _, l := range strings.Split(lines, "\n") {
				if f := strings.Split(l, "\t"); len(f) == 3 {
					sizes[f[0]], _ = strconv.Atoi(f[1])
				}
			}
			for text, n := range before {
				if sizes[text] <= n {
					return false
				}
			}
			return true
		}, func() string { return what + ":\n" + lines })
		return sizes
	}
	one := grown("the first tab", nil)
	s.command("tab Two")
	waitFor(t, s, []string{"second"}, "ready")
	two := grown("the second tab", nil)
	s.command("tab One")
	waitFor(t, s, []string{"ready"}, "second")
	size("accessibility-extra-extra-extra-large")
	grown("the first tab, at the largest size", one)
	s.command("tab Two")
	waitFor(t, s, []string{"second"}, "ready")
	grown("the second tab, at the largest size", two)
}

// fits is the automation's fits report: a label a line.
func (s *iosScreen) fits() string {
	file := filepath.Join(s.tmp, "asuka-fits.txt")
	os.Remove(file)
	s.command("fits")
	for range 50 {
		if b, err := os.ReadFile(file); err == nil {
			return strings.TrimSpace(string(b))
		}
		time.Sleep(100 * time.Millisecond)
	}
	return ""
}
