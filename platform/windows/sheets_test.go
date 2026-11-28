package windows_test

import (
	"regexp"
	"strconv"
	"testing"
)

// sheetsApp has a sheet of custom detents, bound, that leaves its window
// usable at its shortest, and a sheet as tall as its content.
const sheetsApp = `
component App {
  state open = false
  state fit = false
  state at: Detent = .height(200)
  state h: float = 0
  state lines = 2
  state taps = 0

  Column {
    padding: 16
    gap: 8
    Text("Height {h.rounded()}")
    Text("Taps {taps}")
    Button("Tap behind") { on press { taps += 1 } }
    Button("Open") { on press { open = true } }
    Button("Fit") { on press { fit = true } }
    Sheet {
      bind open: open
      detents: [.height(200), .medium, .large]
      bind detent: at
      undimmed: .height(200)
      cornerRadius: 4
      on resize(x) { h = x }
      Column {
        padding: 16
        gap: 8
        Text("In the sheet")
        Button("Large") { on press { at = .large } }
        Button("Small") { on press { at = .height(200) } }
        Button("Close") { on press { open = false } }
      }
    }
    Sheet {
      bind open: fit
      detents: [.fit]
      Column {
        padding: 20
        gap: 8
        for i in 0..<lines { Text("Line {i}") }
        Button("More") { on press { lines += 1 } }
        Button("Done") { on press { fit = false } }
      }
    }
  }
}

app { App() }
`

var sheetLine = regexp.MustCompile(`sheet: (\d+)x(\d+) (modal|usable)`)

// sheetSize is the size and modality the dump gives the first sheet
// shown.
func sheetSize(t *testing.T, dump, where string) (w, h float64, mode string) {
	t.Helper()
	m := sheetLine.FindStringSubmatch(dump)
	if m == nil {
		t.Fatalf("%s: no sheet shows:\n%s", where, dump)
	}
	w, _ = strconv.ParseFloat(m[1], 64)
	h, _ = strconv.ParseFloat(m[2], 64)
	return w, h, m[3]
}

// A sheet rests at its detent's height: 200 points, its shortest, where
// its window stays usable (undimmed) and on resize hears its height;
// set to large it grows to most of its window's height and keeps the
// window from the user; set back, it's 200 again. A sheet whose one
// detent is fit is its content's height, and grows as its content does.
func TestSheetDetents(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, sheetsApp, local)
	d := script(t, w, local, guest, "app.exe",
		"tap:Open", "wait:600ms", "snap:small",
		"tap:Tap behind", "wait:300ms", "snap:behind",
		"tap:Large", "wait:600ms", "snap:large",
		"tap:Small", "wait:600ms", "snap:back",
		"tap:Close", "wait:300ms",
		"tap:Fit", "wait:800ms", "snap:fit",
		"tap:More", "wait:800ms", "snap:more")
	_, h, mode := sheetSize(t, d["small"], "small")
	if h != 200 || mode != "usable" {
		t.Errorf("at .height(200): %v points, %s; want 200, usable:\n%s", h, mode, d["small"])
	}
	shows(t, d["small"], "small", []string{"Height 200", "In the sheet"})
	shows(t, d["behind"], "behind", []string{"Taps 1"})
	_, lh, mode := sheetSize(t, d["large"], "large")
	if lh <= 300 || mode != "modal" {
		t.Errorf("at .large: %v points, %s; want most of the window, modal:\n%s", lh, mode, d["large"])
	}
	shows(t, d["large"], "large", []string{"Height " + strconv.Itoa(int(lh))})
	if _, h, mode = sheetSize(t, d["back"], "back"); h != 200 || mode != "usable" {
		t.Errorf("back at .height(200): %v points, %s:\n%s", h, mode, d["back"])
	}
	_, fh, _ := sheetSize(t, d["fit"], "fit")
	_, mh, _ := sheetSize(t, d["more"], "more")
	if fh <= 60 || fh >= lh {
		t.Errorf("a fit sheet is %v points tall, want its content's:\n%s", fh, d["fit"])
	}
	if mh <= fh {
		t.Errorf("a fit sheet didn't grow with its content: %v, then %v:\n%s", fh, mh, d["more"])
	}
	shows(t, d["more"], "more", []string{"Line 2"})
}
