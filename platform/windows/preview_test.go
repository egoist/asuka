package windows_test

import (
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/lang/compile"
)

const previewApp = `app { Text("The app") }

preview "Plain" (width: 320, height: 200) {
  Column {
    Text("n is 7")
    Button("Tap") { on press { } }
  }
}

preview "Large text" (fontScale: 1.5, width: 320, height: 200) {
  Column {
    Text("n is 7")
    Button("Tap") { on press { } }
  }
}
`

// heightOf is the height of the line of a dump that shows text.
func heightOf(t *testing.T, dump, text string) int {
	t.Helper()
	m := regexp.MustCompile(`\[-?\d+,-?\d+ \d+x(\d+)\][^\n]*` + regexp.QuoteMeta(text)).FindStringSubmatch(dump)
	if m == nil {
		t.Fatalf("no %s in:\n%s", text, dump)
	}
	n, _ := strconv.Atoi(m[1])
	return n
}

// asuka dev's previews on the Windows player: a preview in the app's
// place, in a frame of its size, its text at the preview's size
// (DevFontScale: the host's own text size, as the user's would be).
func TestDevPreview(t *testing.T) {
	w := onWindows(t)
	zig(t)
	local, guest := w.job(t)
	if err := appbuild.WindowsHost(target(w), appbuild.WindowsOptions{Console: true}, filepath.Join(local, "player.exe")); err != nil {
		t.Fatal(err)
	}
	player := guest + `\player.exe`
	plain := devPlayerWith(t, w, player, previewApp, nil, &devHooks{Preview: &compile.PreviewRef{File: "app.asuka", Name: "Plain"}}, "waitfor:loaded", "wait:500ms")
	large := devPlayerWith(t, w, player, previewApp, nil, &devHooks{Preview: &compile.PreviewRef{File: "app.asuka", Name: "Large text"}}, "waitfor:loaded", "wait:500ms")
	shows(t, plain["end"], "the plain preview", []string{"n is 7"}, "The app")
	shows(t, large["end"], "the large text's", []string{"n is 7"}, "The app")
	if !strings.Contains(plain["end"], " 320x200]") {
		t.Errorf("no frame of the preview's size:\n%s", plain["end"])
	}
	tp, tl := heightOf(t, plain["end"], `"n is 7"`), heightOf(t, large["end"], `"n is 7"`)
	bp, bl := heightOf(t, plain["end"], `title="Tap"`), heightOf(t, large["end"], `title="Tap"`)
	if tl*10 < tp*13 || bl <= bp {
		t.Errorf("at 1.5 the text is %d high (%d at 1), the button %d (%d at 1):\n%s\n%s", tl, tp, bl, bp, plain["end"], large["end"])
	}
	t.Logf("plain:\n%s\nlarge:\n%s", plain["end"], large["end"])
}
