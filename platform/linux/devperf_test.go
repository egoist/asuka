package linux_test

import (
	"bytes"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
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

// perfNumbers is the overlay's text with its numbers in, as the window's
// view tree has it.
var perfNumbers = regexp.MustCompile(`DevPerf UI \d+ fps · dropped \d+ · \d+ flushes/s
last [\d.]+ ms: app [\d.]+ props [\d.]+ layout [\d.]+ frames [\d.]+
worst [\d.]+ ms · \d+ ops · \d+ effects · \d+ measures
memory [\d.]+ MB · [\d.]+k? allocs/s · \d+ views`)

// snapUntil snaps the window until its view tree is what ok wants (a
// hang guard), and returns the snapshot and the tree.
func (p *devPlayer) snapUntil(what string, ok func(dump string) bool) (image.Image, string) {
	p.t.Helper()
	path := filepath.Join(p.dir, "perf.png")
	var d string
	for i := 0; i < 100; i++ {
		p.run("snap " + path)
		b, err := os.ReadFile(path + ".txt")
		if err != nil {
			p.t.Fatal(err)
		}
		if d = string(b); !ok(d) {
			continue
		}
		b, err = os.ReadFile(path)
		if err != nil {
			p.t.Fatal(err)
		}
		img, err := png.Decode(bytes.NewReader(b))
		if err != nil {
			p.t.Fatal(err)
		}
		return img, d
	}
	p.t.Fatalf("waiting for %s, the player shows:\n%s", what, d)
	return nil, ""
}

// boxPixels counts the dark pixels in the top end quarter of a snapshot
// of the window: the overlay's box is there, light text on near black
// (a sheet's dimming over it would leave a part of it showing); the
// window's own (its buttons' symbols) are a few.
func boxPixels(img image.Image) int {
	b := img.Bounds()
	n := 0
	for y := b.Min.Y; y < b.Min.Y+b.Dy()/4; y++ {
		for x := b.Min.X + b.Dx()/2; x < b.Max.X; x++ {
			r, g, bl, _ := img.At(x, y).RGBA()
			if (r>>8)+(g>>8)+(bl>>8) < 3*100 {
				n++
			}
		}
	}
	return n
}

// asuka dev's performance overlay: the core's numbers in a box over the
// top end corner of the window, over a sheet the app presents too (the
// window draws it after its dialogs), gone when it's turned off.
func TestDevPerfOverlay(t *testing.T) {
	onLinux(t)
	p := startDevPlayer(t, perfApp)
	p.srv.SetPerfOverlay(true)
	p.run("tap Add")
	img, d := p.snapUntil("the overlay", perfNumbers.MatchString)
	if !strings.Contains(d, `"n: 1"`) {
		t.Errorf("the app under the overlay:\n%s", d)
	}
	box := boxPixels(img)
	if box < 2000 {
		t.Errorf("the box isn't drawn: %d dark pixels", box)
	}

	p.run("tap Sheet")
	p.until("the sheet", hasAll("A sheet"))
	img, _ = p.snapUntil("the overlay over the sheet", func(d string) bool { return strings.Contains(d, "Sheet") && perfNumbers.MatchString(d) })
	if n := boxPixels(img); n < box*4/5 {
		t.Errorf("the sheet covers the box: %d dark pixels, %d without it", n, box)
	}

	p.srv.SetPerfOverlay(false)
	img, _ = p.snapUntil("the overlay off", func(d string) bool { return !strings.Contains(d, "DevPerf") })
	if n := boxPixels(img); n > 500 {
		t.Errorf("the box stays: %d dark pixels", n)
	}
}
