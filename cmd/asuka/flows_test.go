package main

import (
	"strings"
	"testing"
	"time"
)

func TestParseFlow(t *testing.T) {
	f, err := parseFlow("flows/a.flow", "# a comment\n\ntap Add\n  see Count: 1  \nsubmit\nback\nwait 200ms\n")
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, s := range f.steps {
		got = append(got, s.verb+"|"+s.arg)
	}
	if strings.Join(got, ",") != "tap|Add,see|Count: 1,submit|,back|,wait|200ms" || f.steps[1].line != 4 {
		t.Errorf("steps: %v (line %d)", got, f.steps[1].line)
	}
	for src, want := range map[string]string{
		"fly away":  `flows/a.flow:1: no step "fly"`,
		"tap":       "flows/a.flow:1: tap what?",
		"back home": "flows/a.flow:1: back takes nothing after it",
		"wait soon": "flows/a.flow:1: wait soon (want a duration",
		"# nothing": "flows/a.flow: no steps",
	} {
		if _, err := parseFlow("flows/a.flow", src); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%q: %v, want %q", src, err, want)
		}
	}
}

// The counter's and the navigation example's flows, on the web: in a
// headless Chrome, each step the core's automation, each flow from a
// restart.
func TestGUIFlowsWeb(t *testing.T) {
	if testing.Short() {
		t.Skip("a browser")
	}
	if chromePath() == "" {
		t.Skip("no Chrome")
	}
	defer func(d time.Duration) { flowTimeout = d }(flowTimeout)
	for _, app := range []string{"counter", "navigation"} {
		if err := testGUI("../../examples/"+app, platformWeb, "", "", "20s"); err != nil {
			t.Errorf("%s: %v", app, err)
		}
	}
}
