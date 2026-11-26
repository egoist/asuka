package windows_test

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The interaction states, keys, gestures and drag and drop, in the app
// testdata/interaction.asuka, driven as a user would (ASUKA_SCRIPT):
//
//   - Hover follows the pointer by the views' rectangles: over a view and
//     those it's in (a view that lets the mouse through too), and off
//     them, innermost first.
//   - A view's keys, with the focus in a field inside it: the nearest
//     view with the key takes it (the field's own Up first); Enter is the
//     view's, not the field's submit; a declared Control is Ctrl; Alt,
//     Shift and function keys; exactly its modifiers.
//   - Ctrl and the wheel (a precision touchpad's pinch) is a pinch: began,
//     a step per notch, ended once the wheel rests.
//   - Another app's drop, through the window's IDropTarget as OLE calls
//     it: taken by the innermost view that takes one of its items, which
//     gets those it takes; what nothing takes, nowhere.
//   - A drag of a view of the app's: its press cancelled, Began, the drop
//     on the app's own target with its source (the core gives the target
//     its items, values and all), then Dropped.
//   - A field's PasteTypes take a pasted file: the app's own copy.
func TestInteraction(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "interaction.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	if err := os.WriteFile(filepath.Join(local, "photo.png"), make([]byte, 1234), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(local, "archive.zip"), []byte("PK"), 0o644); err != nil {
		t.Fatal(err)
	}
	photo, zip := guest+`\photo.png`, guest+`\archive.zip`
	d := script(t, w, local, guest, "app.exe",
		"hover:Inner", "snap:hover1",
		"hover:Outer", "snap:hover2",
		"hover:", "snap:hover3",
		"hover:Leave me", "hover:", "snap:gone",
		"tap:Search",
		"key:up", "key:enter", "key:ctrl+d", "key:ctrl+e", "key:alt+g", "key:shift+f5",
		"key:shift+up", "key:ctrl+shift+d", "key:down",
		"snap:keys",
		"pinch:Pinch here|2", "snap:pinch",
		"drop:Photo|file:"+photo+"|file:"+zip+"|text:hello", "snap:drop1",
		"drop:Photo|text:hello world", "snap:drop2",
		"drop:Photo|file:"+zip, "snap:drop3",
		"drag:Card|Column", "snap:drag",
		"paste:Paste here|file:"+photo, "snap:paste",
	)
	shows(t, d["hover1"], "over the inner view", []string{"Hover:  outer+ inner+"})
	shows(t, d["hover2"], "over the outer view", []string{"Hover:  outer+ inner+ inner-"})
	shows(t, d["hover3"], "off the window", []string{"Hover:  outer+ inner+ inner- outer-"})
	shows(t, d["gone"], "a view gone as the pointer left the one in it", []string{"Gone: true", "Hover:  outer+ inner+ inner- outer- box+"})
	shows(t, d["keys"], "keys", []string{"Log: field-up col-enter col-ctrl-d col-control-e col-alt-g col-shift-f5"})
	shows(t, d["pinch"], "a wheel's pinch", []string{"Pinch: 4 121"})
	shows(t, d["drop1"], "a photo and a text dropped on the photo's target", []string{"Dropped:  photo:1:false:photo.png:image/png:1234:image"})
	if !strings.Contains(d["drop1"], `"Over: "`) {
		t.Errorf("the column saw a drag it wasn't under:\n%s", d["drop1"])
	}
	shows(t, d["drop2"], "a text, over the photo's target", []string{"Dropped:  photo:1:false:photo.png:image/png:1234:image col:1:false:hello world", "Over:  col+ col-"})
	shows(t, d["drop3"], "an archive nothing takes", []string{"Dropped:  photo:1:false:photo.png:image/png:1234:image col:1:false:hello world", "Over:  col+ col-"})
	shows(t, d["drag"], "a drag within the app", []string{"Phases:  began dropped", "Over:  col+ col- col+ col-"})
	if !strings.Contains(d["drag"], "col:1:true:c1Card") {
		t.Errorf("the drag's drop:\n%s", d["drag"])
	}
	shows(t, d["paste"], "a pasted photo", []string{"Pasted:  photo.png:image/png:1234"})
}

// Keys go out from the focus past the views the core leaves without a
// parent (testdata/keys.asuka): a list's row on to its list, then the
// window's keys (WindowKeys); a sheet's field to its own views, then the
// window the sheet is declared in, not the views under the sheet.
func TestKeysOut(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	src, err := os.ReadFile(filepath.Join("testdata", "keys.asuka"))
	if err != nil {
		t.Fatal(err)
	}
	buildApp(t, w, string(src), local)
	d := script(t, w, local, guest, "app.exe",
		"tap:Row 1", "key:ctrl+r", "key:ctrl+j", "key:ctrl+k", "snap:list",
		"tap:Open", "tap:Note", "key:ctrl+s", "key:ctrl+k", "key:ctrl+j", "key:ctrl+r", "snap:sheet",
	)
	shows(t, d["list"], "a row's field", []string{"Log: row1-r list-j window-k"})
	shows(t, d["sheet"], "a sheet's field", []string{"Log: row1-r list-j window-k sheet-s window-k"})
}

// A click-through window's views that still take the mouse (takesClicks):
// where they show, which the window takes the mouse over.
func TestClickRegion(t *testing.T) {
	w := onWindows(t)
	local, guest := w.job(t)
	buildApp(t, w, `
app { Root() }
component Root {
  state open = true
  Column {
    padding: 20
    Text("main")
    Window {
      bind open: open
      title: "HUD"
      clickThrough: true
      width: 200
      height: 100
      Column {
        padding: 10
        Text("CPU 12%")
        View {
          takesClicks: true
          width: 60
          height: 30
          margin: [10, 0, 0, 20]
          Text("Hide")
        }
      }
    }
  }
}
`, local)
	d := script(t, w, local, guest, "app.exe", "wait:1500ms", "snap:hud")
	dump := d["hud"]
	i := strings.Index(dump, "click region ")
	if i < 0 {
		t.Fatalf("no click region:\n%s", dump)
	}
	var x, y, cw, ch float64
	if _, err := fmt.Sscanf(dump[i:], "click region %g %g %g %g", &x, &y, &cw, &ch); err != nil || x != 30 || cw != 60 || ch != 30 || y < 30 {
		t.Fatalf("click region %v %v %v %v (%v): want one at x 30, 60×30, below the text:\n%s", x, y, cw, ch, err, dump)
	}
}
