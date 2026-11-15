//go:build e2e

package e2e

import (
	"strings"
	"testing"
)

// A chosen item wider than the picker ends in an ellipsis, as iOS's
// pop-up button truncates its title: it's never cut off in the middle.
func TestPickerLongItemAndroid(t *testing.T) {
	s := androidApp(t, "e2e/testdata/picker-long", "dev.asuka.pickerlong")
	waitFor(t, s, []string{"ready"})
	const long = "Couple pack with two players and twelve rounds, 1280 yen"
	var got string
	until(t, func() bool {
		got = ""
		for _, line := range s.fits() {
			if text, how, ok := strings.Cut(line, "\t"); ok && text == long {
				got = how
			}
		}
		return got == "ellipsized"
	}, func() string { return "the chosen item: " + got + ", want ellipsized" })
}

// fits is the automation's fits: each one-line text shown, a tab, and
// whether it fits, is ellipsized or is cut.
func (s *androidScreen) fits() []string { return s.read("fits", "asuka-fits.txt") }
