package linux_test

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
)

// keepShots copies a script's snapshots (name.png in dir) to
// $ASUKA_LINUX_SHOTS, as prefix-name.png, when it's set: to look at them.
func keepShots(t *testing.T, dir, prefix string, names ...string) {
	t.Helper()
	out := os.Getenv("ASUKA_LINUX_SHOTS")
	if out == "" {
		return
	}
	if err := os.MkdirAll(out, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range names {
		b, err := os.ReadFile(filepath.Join(dir, name+".png"))
		if err != nil {
			t.Errorf("no snapshot %s: %v", name, err)
			continue
		}
		if err := os.WriteFile(filepath.Join(out, prefix+"-"+name+".png"), b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

// line is the first line of dump that has all of want.
func line(dump string, want ...string) string {
	for _, l := range strings.Split(dump, "\n") {
		ok := true
		for _, w := range want {
			ok = ok && strings.Contains(l, w)
		}
		if ok {
			return strings.TrimSpace(l)
		}
	}
	return ""
}

func hasLine(t *testing.T, dump, where string, want ...string) {
	t.Helper()
	if line(dump, want...) == "" {
		t.Errorf("%s: no line with %q:\n%s", where, want, dump)
	}
}

// The new controls (testdata/controls.asuka), driven as a user does:
// filter chips are GtkToggleButtons that toggle (Toggled) with a
// checkmark while selected, an action chip presses, an input chip's
// remove button (named "Remove Ana") and Delete with the focus on it
// remove it; a field's suggestions show under it as it's typed in, the
// core's filtered list, Down and Return choose one (its text in the
// field, Suggestion reported, the list closed), Down opens a combo box's
// whole list and Escape closes it, a click chooses; a DateRangePicker's
// button opens its calendar, two clicks choose a range (marked, reported,
// the popover closed), as on an inline one; a Disclosure is a GtkExpander
// whose toggling shows and hides the core's content under it.
func TestControls(t *testing.T) {
	onLinux(t)
	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, testdata(t, "controls.asuka"), exe)
	shots := []string{"start", "chips", "removed", "deleted", "open", "closed", "typed", "moved", "chosen", "dropdown", "escaped", "clicked", "first", "range", "inline"}
	d := script(t, dir, exe,
		"resize:800,900", "wait:500ms",
		"snap:start",
		"tap:Wi-Fi", "tap:Pool", "tap:Add", "snap:chips",
		"tap:Remove Ana", "snap:removed",
		"click:Bo", "press:delete", "snap:deleted",
		"tap:More", "snap:open",
		"click:More", "snap:closed",
		"tap:City", "type:Pa", "wait:300ms", "snap:typed",
		"press:down", "press:down", "snap:moved",
		"press:enter", "snap:chosen",
		"press:down", "snap:dropdown",
		"press:escape", "snap:escaped",
		"press:ctrl+a", "type:Pr", "wait:300ms", "click:Prague", "snap:clicked",
		"tap:Dates", "wait:300ms", "click:15", "snap:first", "click:19", "wait:300ms", "snap:range",
		"click:20", "click:23", "snap:inline")
	keepShots(t, dir, "controls", shots...)

	// Chips: their lines, their sizes (chip sized: shorter than a button).
	start := d["start"]
	hasLine(t, start, "a filter chip", "chip filter active=0")
	hasLine(t, start, "a selected filter chip's checkmark", "chip filter active=1 icon=object-select-symbolic")
	hasLine(t, start, "an action chip's icon", "chip action icon=list-add-symbolic")
	hasLine(t, start, "an input chip's remove button", `remove="Remove Ana"`)
	for _, n := range parseDump(t, start) {
		if n.kind == "Chip" && (n.h < 24 || n.h > 36 || n.w < 40) {
			t.Errorf("a chip of %vx%v, not chip sized: %s", n.w, n.h, n.line)
		}
	}
	shows(t, start, "at first", []string{"chips false true pressed 0 removed none guests 2", "more false"}, "Inside")
	shows(t, d["chips"], "toggled and pressed", []string{"chips true false pressed 1 removed none guests 2"})
	hasLine(t, d["chips"], "Wi-Fi's checkmark", "chip filter active=1 icon=object-select-symbolic")
	shows(t, d["removed"], "Ana's remove button", []string{"chips true false pressed 1 removed Ana guests 1"}, "Ana")
	shows(t, d["deleted"], "Delete on Bo", []string{"chips true false pressed 1 removed Bo guests 0"}, "Bo")

	// The disclosure: GTK's expander, the core's content under it.
	shows(t, d["open"], "opened", []string{"more true", "Inside"})
	hasLine(t, d["open"], "the expander", "expander expanded=1 a11y-expanded=true")
	shows(t, d["closed"], "closed by a click", []string{"more false"}, "Inside")
	hasLine(t, d["closed"], "the expander", "expander expanded=0 a11y-expanded=false")

	// Suggestions: the core's filtered list, the keys in it, a choice.
	hasLine(t, d["typed"], "typing", `suggestions shown=1 dropdown=1 selected=-1 items="Pamplona","Paris"`)
	hasLine(t, d["moved"], "Down twice", "suggestions shown=1", "selected=1")
	shows(t, d["chosen"], "Return", []string{"chosen Paris text Paris"})
	hasLine(t, d["chosen"], "closed by the choice", "suggestions shown=0")
	hasLine(t, d["dropdown"], "Down: the combo box's list", "suggestions shown=1", `items="Amsterdam","Pamplona","Paris","Prague","Tokyo"`)
	hasLine(t, d["escaped"], "Escape", "suggestions shown=0")
	shows(t, d["clicked"], "a click on Prague", []string{"chosen Prague text Prague"})

	// The date range: two clicks on its popover's calendar.
	hasLine(t, d["first"], "the first day", "daterange button", "first=set marked=15 ")
	shows(t, d["range"], "the range", []string{"range 4"})
	hasLine(t, d["range"], "marked", "daterange button", "first=none marked=15,16,17,18,19 ")
	if l := line(d["range"], "daterange button"); !strings.Contains(l, "\u2013") || strings.Contains(l, "label=\"\"") {
		t.Errorf("the button doesn't show the range: %s", l)
	}
	shows(t, d["inline"], "the inline calendar", []string{"inline 3"})
	hasLine(t, d["inline"], "marked inline", "daterange inline", "marked=20,21,22,23 ")
}

// discPNG is a 32×32 opaque disc of c on transparency, the trip
// example's pin's shape.
func discPNG(t *testing.T, c color.NRGBA) []byte {
	img := image.NewNRGBA(image.Rect(0, 0, 32, 32))
	for y := range 32 {
		for x := range 32 {
			if dx, dy := x-16, y-16; dx*dx+dy*dy < 14*14 {
				img.SetNRGBA(x, y, c)
			}
		}
	}
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

// imageServer serves a PNG (a blue disc): /auth/* only with the header "Authorization:
// Bearer t" (401 otherwise), the rest to anyone; it counts the requests
// for each path.
type imageServer struct {
	mu    sync.Mutex
	count map[string]int
	png   []byte
	base  string
}

func (s *imageServer) ServeHTTP(rw http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	s.count[r.URL.Path]++
	s.mu.Unlock()
	if strings.HasPrefix(r.URL.Path, "/auth/") && r.Header.Get("Authorization") != "Bearer t" {
		http.Error(rw, "no", http.StatusUnauthorized)
		return
	}
	rw.Header().Set("Content-Type", "image/png")
	io.Copy(rw, bytes.NewReader(s.png))
}

func (s *imageServer) requests(path string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.count[path]
}

// The Image's Headers and Cache, the image module, and Color tinting a
// raster image (testdata/images.asuka): an image that needs a header
// loads with it and fails without; Default keeps a fetched image on disk,
// Memory and None don't, None fetches it each time it's shown while
// Memory and Default fetch it once; prefetch fetches before an Image
// shows (which then fetches nothing), clearCache forgets them all (shown
// again, fetched again); a green PNG with a red Color draws red through
// its alpha, no green left.
func TestImageOptions(t *testing.T) {
	onLinux(t)
	srv := &imageServer{count: map[string]int{}, png: discPNG(t, color.NRGBA{0, 0, 255, 255})}
	ln, err := net.Listen("tcp", "0.0.0.0:0")
	if err != nil {
		t.Fatal(err)
	}
	// Not localhost: the disk cache keeps no image of this machine's.
	host, err := os.Hostname()
	if err != nil {
		t.Fatal(err)
	}
	srv.base = fmt.Sprintf("http://%s:%d", host, ln.Addr().(*net.TCPAddr).Port)
	hs := &http.Server{Handler: srv}
	go hs.Serve(ln)
	defer hs.Close()

	dir := t.TempDir()
	exe := filepath.Join(dir, "app")
	buildApp(t, strings.ReplaceAll(testdata(t, "images.asuka"), "IMAGES_BASE", srv.base), exe)
	if err := os.MkdirAll(filepath.Join(dir, "assets"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "assets", "pin.png"), discPNG(t, color.NRGBA{0, 255, 0, 255}), 0o644); err != nil {
		t.Fatal(err)
	}
	d := script(t, dir, exe,
		"wait:1500ms", "snap:loaded",
		"tap:Hide", "tap:Show", "wait:1s", "snap:again",
		"tap:Prefetch", "wait:1s", "tap:Prefetched", "wait:1s", "snap:prefetched",
		"tap:Hide", "tap:Clear", "wait:500ms", "tap:Show", "wait:1s", "snap:cleared")
	keepShots(t, dir, "images", "loaded", "cleared")
	shows(t, d["loaded"], "loaded", []string{"auth loaded 32", "noauth failed", "memory loaded 32", "none loaded 32"})

	shows(t, d["prefetched"], "prefetched", []string{"pre loaded 32"})
	shows(t, d["cleared"], "shown again", []string{"auth loaded 32", "memory loaded 32", "none loaded 32"})

	// What's on disk, in the app's caches' asuka-images (after clearCache
	// and showing them again): Default's image, not Memory's nor None's.
	cached, _ := filepath.Glob(filepath.Join(dir, "home", "cache", "*", "asuka-images", "*"))
	var onDisk []string
	for _, f := range cached {
		b, _ := os.ReadFile(f)
		onDisk = append(onDisk, regexp.MustCompile(`/(auth|memory|none|pre)/[a-z]+\.png`).FindString(string(b)))
	}
	if strings.Join(onDisk, " ") != "/auth/a.png" {
		t.Errorf("on disk: %q, want Default's /auth/a.png alone", onDisk)
	}
	// Shown, hidden and shown again, then the caches cleared and shown
	// again: Default and Memory fetched once each time the caches were
	// empty, None each time it showed; the prefetched image once, by
	// prefetch (its Image showed it from the cache, and kept it).
	if a, m, n, p := srv.requests("/auth/a.png"), srv.requests("/memory/m.png"), srv.requests("/none/n.png"), srv.requests("/pre/p.png"); a != 2 || m != 2 || n != 3 || p != 1 {
		t.Errorf("requests: auth %d, memory %d, none %d, prefetched %d; want 2, 2, 3, 1", a, m, n, p)
	}

	// The tint: red through the green pin's alpha, no green left.
	f, err := os.Open(filepath.Join(dir, "loaded.png"))
	if err != nil {
		t.Fatal(err)
	}
	img, err := png.Decode(f)
	f.Close()
	if err != nil {
		t.Fatal(err)
	}
	red, okRed := bbox(img, func(r, g, b uint32) bool { return r > 200 && g < 60 && b < 60 })
	if _, green := bbox(img, func(r, g, b uint32) bool { return g > 200 && r < 60 && b < 60 }); green {
		t.Errorf("green pixels: the tinted pin isn't a template")
	}
	if !okRed || red.Dx() < 20 || red.Dx() > 40 || red.Dy() < 20 || red.Dy() > 40 {
		t.Errorf("the tinted pin's red is %v, want about 28×28", red)
	}
}
