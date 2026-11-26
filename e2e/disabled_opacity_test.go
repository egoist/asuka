//go:build e2e

package e2e

import (
	"testing"
	"time"
)

// A disabled control the web host dims is at one opacity, whatever its
// kind (e2e/testdata/disabled-opacity): a button, a field, a switch, a
// segmented control's choices, a stepper's buttons, a picker, a wheel, a
// date picker and a color picker all at the 0.5 a disabled view has.
func TestDisabledOpacityWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	d := devtoolsPage(t, browser, webServer(t, "e2e/testdata/disabled-opacity"), "1")
	waitText(t, d, "ready")
	// each disabled control's opacity, by what it is; none while one isn't
	// drawn yet
	js := `(() => {
	  const o = (el) => el && el.disabled ? getComputedStyle(el).opacity : null;
	  const buttons = [...document.querySelectorAll(".asuka-button")];
	  const selects = document.querySelectorAll("select.asuka-select");
	  return {
	    "plain button": o(buttons.find((b) => b.textContent === "Plain")),
	    "filled button": o(buttons.find((b) => b.textContent === "Filled")),
	    field: o(document.querySelector("input.asuka-field")),
	    switch: o(document.querySelector(".asuka-switch")),
	    segment: o(document.querySelector(".asuka-segmented button")),
	    stepper: o(document.querySelector(".asuka-stepper button")),
	    picker: o(selects[0]),
	    wheel: o(selects[1]),
	    date: o(document.querySelector(".asuka-date")),
	    color: o(document.querySelector(".asuka-color")),
	  };
	})()`
	var got map[string]any
	deadline := time.Now().Add(within)
	for drawn := false; !drawn; time.Sleep(100 * time.Millisecond) {
		if time.Now().After(deadline) {
			t.Fatalf("not every control is drawn disabled: %v", got)
		}
		got = nil
		if err := pageEval(d, js, &got); err != nil {
			t.Fatal(err)
		}
		drawn = len(got) > 0
		for _, v := range got {
			drawn = drawn && v != nil
		}
	}
	for kind, v := range got {
		if v != "0.5" {
			t.Errorf("a disabled %s is at opacity %v, not 0.5", kind, v)
		}
	}
}
