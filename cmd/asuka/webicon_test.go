package main

import (
	"image"
	"image/color"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// writeIcon writes a square PNG of one color, an icon asuka.toml names.
func writeIcon(t *testing.T, c color.NRGBA) string {
	t.Helper()
	img := image.NewNRGBA(image.Rect(0, 0, 1024, 1024))
	for i := 0; i < len(img.Pix); i += 4 {
		img.Pix[i], img.Pix[i+1], img.Pix[i+2], img.Pix[i+3] = c.R, c.G, c.B, c.A
	}
	p := filepath.Join(t.TempDir(), "icon.png")
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if err := png.Encode(f, img); err != nil {
		t.Fatal(err)
	}
	return p
}

// newDevServer is a dev server for an app of cfg, as far as its page
// and icons need.
func newDevServer(cfg *appConfig) *devServer {
	s := &devServer{app: &asukaApp{cfg: cfg}}
	noSQLite := ""
	s.sqlite.Store(&noSQLite)
	var none []webScript
	s.scripts.Store(&none)
	return s
}

// devGet is the dev server's answer to a GET of path, as a browser asks.
func devGet(s *devServer, path string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(http.MethodGet, path, nil)
	if path == "/" {
		r.Header.Set("Accept", "text/html")
	}
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	return w
}

// devIcon is the icon the dev server serves at path: its size and its
// color, or a failure.
func devIcon(t *testing.T, s *devServer, path string) (int, color.NRGBA) {
	t.Helper()
	w := devGet(s, path)
	if w.Code != http.StatusOK || w.Header().Get("Content-Type") != "image/png" {
		t.Fatalf("%s: %d %s", path, w.Code, w.Header().Get("Content-Type"))
	}
	img, err := png.Decode(w.Body)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	b := img.Bounds()
	if b.Dx() != b.Dy() {
		t.Fatalf("%s: %v isn't square", path, b)
	}
	return b.Dx(), color.NRGBAModel.Convert(img.At(b.Dx()/2, b.Dy()/2)).(color.NRGBA)
}

// asuka dev's page links the app's icon as its favicon and iOS's home
// screen icon, which the server makes at a build's sizes, reading the
// icon again only when its file changes; with no icon there are
// neither.
func TestDevWebIcon(t *testing.T) {
	blue := color.NRGBA{R: 0x20, G: 0x60, B: 0xc0, A: 0xff}
	icon := writeIcon(t, blue)
	s := newDevServer(&appConfig{Icon: icon})
	page := devGet(s, "/").Body.String()
	for _, link := range []string{`href="icons/favicon.png"`, `href="icons/apple-touch-icon.png"`} {
		if !strings.Contains(page, link) {
			t.Errorf("the page doesn't link %s:\n%s", link, page)
		}
	}
	for path, want := range map[string]int{"/icons/favicon.png": 32, "/icons/apple-touch-icon.png": 180} {
		if px, c := devIcon(t, s, path); px != want || c != blue {
			t.Errorf("%s: %d pixels of %v, want %d of the app's icon %v", path, px, c, want, blue)
		}
	}

	// The file's bytes changed but not its time: the icon read before.
	fi, err := os.Stat(icon)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(icon, []byte("not a PNG"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(icon, fi.ModTime(), fi.ModTime()); err != nil {
		t.Fatal(err)
	}
	if _, c := devIcon(t, s, "/icons/favicon.png"); c != blue {
		t.Errorf("the icon unchanged, the favicon is %v, want %v as read before", c, blue)
	}
	// A new icon, a new time: read again.
	red := color.NRGBA{R: 0xd0, G: 0x20, B: 0x20, A: 0xff}
	os.Remove(icon)
	if err := os.Rename(writeIcon(t, red), icon); err != nil {
		t.Fatal(err)
	}
	later := fi.ModTime().Add(time.Second)
	if err := os.Chtimes(icon, later, later); err != nil {
		t.Fatal(err)
	}
	if _, c := devIcon(t, s, "/icons/favicon.png"); c != red {
		t.Errorf("the icon changed, the favicon is %v, want the new icon's %v", c, red)
	}

	none := newDevServer(&appConfig{})
	if page := devGet(none, "/").Body.String(); strings.Contains(page, "icons/") {
		t.Errorf("with no icon, the page links one:\n%s", page)
	}
	if w := devGet(none, "/icons/favicon.png"); w.Code != http.StatusNotFound {
		t.Errorf("with no icon, the favicon: %d", w.Code)
	}
}
