package main

import (
	"encoding/xml"
	"errors"
	"image"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
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
		"fly away":                         `flows/a.flow:1: no step "fly"`,
		"tap":                              "flows/a.flow:1: tap what?",
		"back home":                        "flows/a.flow:1: back takes nothing after it",
		"wait soon":                        "flows/a.flow:1: wait soon (want a duration",
		"# nothing":                        "flows/a.flow: no steps",
		"swipe sideways":                   "swipe sideways: which way?",
		"swipe left of Photo":              "swipe left of Photo: swipe left on <text>",
		"drag Photo":                       "drag Photo: drag <text> to <text>",
		"pinch Photo":                      "pinch Photo: pinch <text> <scale>",
		"pinch Photo big":                  "pinch Photo big: pinch <text> <scale>",
		"assert pretty Photo":              "assert pretty: visible, hidden",
		"assert value Email":               "assert value Email: assert value <field> = <text>",
		"permission camera maybe":          "permission camera maybe: allow, deny or ask",
		"permission teleport allow":        "permission teleport: which?",
		"tap Add\npermission camera allow": "flows/a.flow:2: permission steps come first",
		"screenshot my home":               "screenshot my home: a name",
		"expect Count: 1":                  "expect Count: 1: expect screenshot <name>",
		"expect screenshot home mask":      "expect screenshot home mask: a name",
		"expect screenshot home mask  ":    "a name",
		"expect screenshot a tolerance 5":  "tolerance 5: a share of the pixels",
		"openlink not a url":               "openlink not a url: a URL",
	} {
		if _, err := parseFlow("flows/a.flow", src); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%q: %v, want %q", src, err, want)
		}
	}
}

// Each step, and the core's automation's commands it runs.
func TestFlowStepCommands(t *testing.T) {
	src := `permission camera allow
permission location deny
doubletap Photo
longpress Message 3
type ada@example.com into Email
type hello
hidekeyboard
swipe left on Message 3
swipe up
drag Milk to Eggs
drag Dot to 120, 300
drag Dot by -40,0
pinch Photo 2.5
assert visible Inbox
assert hidden Draft
assert enabled Send
assert disabled Send
assert selected Wi-Fi
assert unselected Bluetooth
assert value Email = ada@*
waitfor Loaded 30s
waitfor Count: 3
openlink notes://add?text=milk
relaunch
background
foreground
screenshot home
expect screenshot home mask Time: * mask Date: * tolerance 0.5%
expect screenshot exact tolerance 0%
`
	f, err := parseFlow("flows/all.flow", src)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, s := range f.steps {
		g := s.verb + ": " + strings.ReplaceAll(strings.Join(s.cmds, " | "), "\t", "→")
		if s.wait > 0 {
			g += " (" + s.wait.String() + ")"
		}
		if s.perm != "" {
			g += s.perm + " " + s.setting
		}
		if s.shot != "" {
			g += s.shot + " [" + strings.Join(s.masks, ",") + "]"
		}
		if s.tolerance != 0 {
			g += " ~" + strconv.FormatFloat(s.tolerance, 'g', -1, 64)
		}
		got = append(got, g)
	}
	want := []string{
		"permission: camera allow",
		"permission: location deny",
		"doubletap: doubletap Photo",
		"longpress: longpress Message 3",
		"type: focus Email | type ada@example.com",
		"type: type hello",
		"hidekeyboard: hidekeyboard",
		"swipe: swipe left Message 3",
		"swipe: swipe up",
		"drag: drag Milk→Eggs",
		"drag: drag Dot→@120,300",
		"drag: drag Dot→+-40,0",
		"pinch: pinch Photo→2.5",
		"assert: see Inbox",
		"assert: notsee Draft",
		"assert: check enabled Send",
		"assert: check disabled Send",
		"assert: check selected Wi-Fi",
		"assert: check unselected Bluetooth",
		"assert: value Email→ada@*",
		"waitfor: see Loaded (30s)",
		"waitfor: see Count: 3",
		"openlink: ",
		"relaunch: ",
		"background: background",
		"foreground: foreground",
		"screenshot: home []",
		"expect: home [Time: *,Date: *] ~0.005",
		"expect: exact [] ~-1",
	}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("steps:\n%s\nwant:\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}
}

func fixture(t *testing.T, name string) string {
	return filepath.Join("testdata", "flows", name)
}

// Screenshots compared as people see them: the same image matches; one
// whose shapes' edges are drawn a fraction of a pixel over (anti-aliasing)
// matches too; one with a button another color doesn't, and its diff
// shows where; another size doesn't compare.
func TestCompareImages(t *testing.T) {
	read := func(name string) *image.NRGBA {
		img, err := readPNG(fixture(t, name))
		if err != nil {
			t.Fatal(err)
		}
		return img
	}
	golden := read("golden.png")
	same, err := compareImages(golden, read("golden.png"), defaultImageCompare)
	if err != nil || same.differ != 0 || same.aliased != 0 {
		t.Errorf("the same image: %d differ, %d aliased, %v", same.differ, same.aliased, err)
	}
	aa, err := compareImages(golden, read("antialiased.png"), defaultImageCompare)
	if err != nil {
		t.Fatal(err)
	}
	if aa.differ != 0 || aa.aliased == 0 {
		t.Errorf("anti-aliasing: %d differ, %d aliased; want none, some", aa.differ, aa.aliased)
	}
	changed, err := compareImages(golden, read("changed.png"), defaultImageCompare)
	if err != nil {
		t.Fatal(err)
	}
	// The bar: 48 by 8 pixels.
	if changed.differ != 48*8 || changed.ratio() <= defaultImageCompare.maxRatio {
		t.Errorf("changed: %d differ (%.4f), want 384", changed.differ, changed.ratio())
	}
	if c := changed.diff.NRGBAAt(20, 40); c.R != 255 || c.G != 0 {
		t.Errorf("the diff at the bar: %v, want red", c)
	}
	if c := changed.diff.NRGBAAt(2, 2); c.R != c.G || c.R < 200 {
		t.Errorf("the diff elsewhere: %v, want the golden faded", c)
	}
	// A loose threshold takes the change.
	if loose, _ := compareImages(golden, read("changed.png"), imageCompare{threshold: 1, maxRatio: 0}); loose.differ != 0 {
		t.Errorf("threshold 1: %d differ", loose.differ)
	}
	if _, err := compareImages(golden, read("bigger.png"), defaultImageCompare); err == nil || !strings.Contains(err.Error(), "the golden is 64x48, the screen 72x48") {
		t.Errorf("another size: %v", err)
	}
	if !samePixels(golden, read("golden.png")) || samePixels(golden, read("changed.png")) {
		t.Error("samePixels")
	}
}

// A flow run's results as JUnit XML, and asuka test's.
func TestJUnit(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "out", "flows.xml")
	start := time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC)
	results := []flowResult{
		{f: flow{file: "flows/count.flow"}, took: 1500 * time.Millisecond},
		{f: flow{file: "flows/note.flow"}, err: errors.New("flows/note.flow:3: see Saved: nothing shows \"Saved\"\n    the app shows:\n      Notes"), took: time.Second},
	}
	if err := writeFlowsJUnit(path, "ios", start, results); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var got junitSuites
	if err := xml.Unmarshal(b, &got); err != nil {
		t.Fatal(err)
	}
	if got.Tests != 2 || got.Failures != 1 || len(got.Suites) != 1 || got.Suites[0].Name != "flows (ios)" || got.Suites[0].Timestamp != "2026-01-02T03:04:05" {
		t.Fatalf("suites: %+v", got)
	}
	cs := got.Suites[0].Cases
	if cs[0].Name != "count" || cs[0].Classname != "flows.ios" || cs[0].Time != 1.5 || cs[0].Failure != nil {
		t.Errorf("the first: %+v", cs[0])
	}
	if cs[1].Failure == nil || cs[1].Failure.Message != `flows/note.flow:3: see Saved: nothing shows "Saved"` || !strings.Contains(cs[1].Failure.Text, "the app shows") {
		t.Errorf("the failure: %+v", cs[1].Failure)
	}
	s := junitOf("examples/counter", start, 2*time.Second, []appbuild.TestResult{
		{Name: "counts", OK: true, Pos: "app.asuka:41"},
		{Name: "resets", OK: false, Message: "the screen doesn't show \"Count: 0\"", Pos: "app.asuka:50"},
	})
	if s.Tests != 2 || s.Failures != 1 || s.Cases[1].File != "app.asuka" || s.Cases[1].Line != 50 || s.Time != 2 {
		t.Errorf("asuka test's: %+v", s)
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
	if _, err := cbuild.ForTarget(cbuild.Target{OS: "wasm", Arch: "wasm32"}); err != nil {
		t.Skip(err) // the web's player is built with Zig
	}
	defer func(d time.Duration, g guiRun) { flowTimeout, guiFlags = d, g }(flowTimeout, guiFlags)
	guiFlags = guiRun{artifacts: t.TempDir()}
	// The counter's count, not its look: its goldens are this Mac's
	// Chrome's, and another system draws text otherwise.
	for app, pattern := range map[string]string{"counter": "count", "navigation": ""} {
		if err := testGUI("../../examples/"+app, platformWeb, "", pattern, "20s"); err != nil {
			t.Errorf("%s: %v", app, err)
		}
	}
}
