package windows_test

import (
	"bytes"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/devserver"
)

// perfApp is a counter with a sheet.
const perfApp = `component Counter {
  state n = 0

  Column {
    gap: 8; padding: 16
    Text("n: {n}")
    Button("Add") { on press { n += 1 } }
    Button("Sheet") { on press { present(Note()) } }
  }
}

component Note {
  Screen {
    title: "Note"
    Text("A sheet") { padding: 16 }
  }
}

app { Counter() }
`

// perfLine is the overlay's text as the window's view tree has it: its
// numbers in, and where the box is in the main window's client area.
var perfLine = regexp.MustCompile(`DevPerf \[(-?\d+),(-?\d+) (\d+)x(\d+)\]( above)? UI \d+ fps · dropped \d+ · \d+ flushes/s
last [\d.]+ ms: app [\d.]+ props [\d.]+ layout [\d.]+ frames [\d.]+
worst [\d.]+ ms · \d+ ops · \d+ effects · \d+ measures
memory [\d.]+ MB · [\d.]+k? allocs/s · \d+ views`)

// perfBox is where the dump says the box is: x, y, width, height (in the
// client area's points) and whether it's above the app's other windows.
func perfBox(t *testing.T, where, dump string) (box [4]int, above bool) {
	t.Helper()
	m := perfLine.FindStringSubmatch(dump)
	if m == nil {
		t.Fatalf("%s: no overlay with its numbers:\n%s", where, dump)
	}
	for i := range box {
		box[i] = atoi(m[i+1])
	}
	return box, m[5] != ""
}

func atoi(s string) int {
	n, neg := 0, strings.HasPrefix(s, "-")
	for _, c := range strings.TrimPrefix(s, "-") {
		n = n*10 + int(c-'0')
	}
	if neg {
		return -n
	}
	return n
}

// darkPixels counts the near-black pixels of a snapshot in its top
// quarter's half at the end (right, or left right to left): the box is
// there, light text on a dark translucent box; the app's own (its
// buttons' text, the title bar's) are a few.
func darkPixels(t *testing.T, file string, rtl bool) int {
	t.Helper()
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	img, err := png.Decode(bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}
	r := img.Bounds()
	x0, x1 := r.Min.X+r.Dx()/2, r.Max.X
	if rtl {
		x0, x1 = r.Min.X, r.Min.X+r.Dx()/2
	}
	n := 0
	for y := r.Min.Y; y < r.Min.Y+r.Dy()/4; y++ {
		for x := x0; x < x1; x++ {
			cr, cg, cb, _ := img.At(x, y).RGBA()
			if (cr>>8)+(cg>>8)+(cb>>8) < 3*100 {
				n++
			}
		}
	}
	return n
}

var _ image.Image

// asuka dev's performance overlay: turned on by asuka dev (its p), the
// core's numbers twice a second in a box at the top trailing corner of
// the main window's client area (left, right to left), over a sheet the
// app presents; gone when it's turned off.
func TestDevPerfOverlay(t *testing.T) {
	w := onWindows(t)
	zig(t)
	for _, rtl := range []bool{false, true} {
		name := "ltr"
		if rtl {
			name = "rtl"
		}
		t.Run(name, func(t *testing.T) {
			local, guest := w.job(t)
			if err := appbuild.WindowsHost(target(w), appbuild.WindowsOptions{Console: true}, filepath.Join(local, "player.exe")); err != nil {
				t.Fatal(err)
			}
			hooks := &devHooks{
				Direction: map[bool]string{true: "rtl"}[rtl],
				// asuka dev's p: on once the app's loaded, off once the
				// sheet's snapshot is taken (from a goroutine of its own:
				// these hooks run on the connection's).
				Loaded: func(p *devserver.Player, dir string, flag func(string)) {
					go func() {
						p.SetPerfOverlay(true)
						for i := 0; i < 1200; i++ {
							if _, err := os.Stat(filepath.Join(dir, "sheet.png.txt")); err == nil {
								p.SetPerfOverlay(false)
								flag("off")
								return
							}
							time.Sleep(100 * time.Millisecond)
						}
					}()
				},
			}
			d := devPlayerWith(t, w, guest+`\player.exe`, perfApp, nil, hooks,
				"waitfor:loaded", "tap:Add", "wait:1500ms", "snap:on",
				"tap:Sheet", "wait:1500ms", "snap:sheet",
				"waitfor:off", "wait:800ms", "snap:off")
			if !strings.Contains(d["on"], `"n: 1"`) {
				t.Errorf("the app under the overlay:\n%s", d["on"])
			}
			box, _ := perfBox(t, "on", d["on"])
			// The client area is 800 points wide (the main window's).
			if rtl {
				if box[0] < 0 || box[0] > 24 {
					t.Errorf("right to left, the box is at x %d, not the client area's left (%v)", box[0], box)
				}
			} else if end := box[0] + box[2]; end < 800-24 || end > 800 {
				t.Errorf("the box ends at x %d, not the client area's right, 800 (%v)", end, box)
			}
			if box[1] < 0 || box[1] > 48 || box[2] < 150 || box[3] < 40 {
				t.Errorf("the box isn't a box at the top: %v", box)
			}
			dark := darkPixels(t, filepath.Join(hooks.Dir, "on.png"), rtl)
			if dark < 2000 {
				t.Errorf("the box isn't drawn: %d dark pixels", dark)
			}
			sbox, above := perfBox(t, "sheet", d["sheet"])
			if !above {
				t.Errorf("the box isn't above the sheet's window:\n%s", d["sheet"])
			}
			if sbox != box {
				t.Errorf("the box moved with a sheet open: %v, was %v", sbox, box)
			}
			if strings.Contains(d["off"], "DevPerf") {
				t.Errorf("the overlay stays when it's off:\n%s", d["off"])
			}
			if n := darkPixels(t, filepath.Join(hooks.Dir, "off.png"), rtl); n > dark/4 {
				t.Errorf("the box stays drawn: %d dark pixels, %d with it", n, dark)
			}
		})
	}
}
