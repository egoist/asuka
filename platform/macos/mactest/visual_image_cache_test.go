//go:build darwin

package mactest

import (
	"bytes"
	"fmt"
	"image"
	"image/png"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// imageServer serves a small PNG at any path, counting the requests of
// each and keeping the Authorization header each last came with.
type imageServer struct {
	srv   *httptest.Server
	mu    sync.Mutex
	count map[string]int
	auth  map[string]string
}

func newImageServer(t *testing.T) *imageServer {
	s := &imageServer{count: map[string]int{}, auth: map[string]string{}}
	img := image.NewNRGBA(image.Rect(0, 0, 4, 2))
	for i := 0; i < len(img.Pix); i += 4 {
		img.Pix[i], img.Pix[i+3] = 255, 255
	}
	var b bytes.Buffer
	png.Encode(&b, img)
	s.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.count[r.URL.Path]++
		s.auth[r.URL.Path] = r.Header.Get("Authorization")
		s.mu.Unlock()
		w.Header().Set("Content-Type", "image/png")
		w.Write(b.Bytes())
	}))
	t.Cleanup(s.srv.Close)
	return s
}

func (s *imageServer) requests(path string) (int, string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.count[path], s.auth[path]
}

// waitRequests waits for path to have been requested n times.
func (s *imageServer) waitRequests(t *testing.T, path string, n int) string {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if c, auth := s.requests(path); c == n {
			return auth
		}
		time.Sleep(20 * time.Millisecond)
	}
	c, _ := s.requests(path)
	t.Fatalf("%s: %d requests, want %d", path, c, n)
	return ""
}

// An Image's Headers go with its request; Cache .off loads it each time it
// shows, .memory once; platform/image's prefetch loads a picture (with its
// headers) that an Image then shows without another request, and
// clearCache forgets them.
func TestVisualImageCache(t *testing.T) {
	s := newImageServer(t)
	u := s.srv.URL
	a := Run(t, fmt.Sprintf(`
		import platform/image

		component I {
		  state show = true
		  state prefetched = false
		  Column {
		    padding: 16; gap: 8; align: .start
		    Button("Prefetch") { on press { image.prefetch(["%[1]s/p.png"], headers: ["Authorization": "Bearer p"]) } }
		    Button("Clear") { on press { image.clearCache() } }
		    Button("Toggle") { on press { show = !show } }
		    Button("Show prefetched") { on press { prefetched = true } }
		    if show {
		      Image("%[1]s/off.png") { headers: ["Authorization": "Bearer off"]; cache: .off; width: 20; height: 10 }
		      Image("%[1]s/mem.png") { headers: ["Authorization": "Bearer mem"]; cache: .memory; width: 20; height: 10 }
		    }
		    if prefetched {
		      Image("%[1]s/p.png") { width: 20; height: 10 }
		    }
		    Text("log:{show}")
		  }
		}
		app { I() }
	`, u))
	a.UntilText("log:true")
	if auth := s.waitRequests(t, "/off.png", 1); auth != "Bearer off" {
		t.Errorf("off.png's Authorization %q", auth)
	}
	if auth := s.waitRequests(t, "/mem.png", 1); auth != "Bearer mem" {
		t.Errorf("mem.png's Authorization %q", auth)
	}
	loaded := func(tr *Tree) bool {
		ims := tr.Kind("Image")
		for _, im := range ims {
			if !vbool(vobj(im, "image"), "loaded") {
				return false
			}
		}
		return len(ims) > 0
	}
	a.Until(loaded)
	// Shown again: .off is loaded again, .memory comes from memory.
	a.Do("tap Toggle")
	a.UntilText("log:false")
	a.Do("tap Toggle")
	a.UntilText("log:true")
	a.Until(loaded)
	s.waitRequests(t, "/off.png", 2)
	if c, _ := s.requests("/mem.png"); c != 1 {
		t.Errorf("mem.png requested %d times", c)
	}
	// Prefetched with its headers, then shown without a request.
	a.Do("tap Prefetch")
	if auth := s.waitRequests(t, "/p.png", 1); auth != "Bearer p" {
		t.Errorf("p.png's Authorization %q", auth)
	}
	time.Sleep(200 * time.Millisecond) // decoded into the memory cache
	a.Do("tap Show prefetched")
	a.Until(func(tr *Tree) bool { return len(tr.Kind("Image")) == 3 && loaded(tr) })
	if c, _ := s.requests("/p.png"); c != 1 {
		t.Errorf("p.png requested %d times", c)
	}
	// Cleared: .memory is loaded again.
	a.Do("tap Clear")
	a.Do("tap Toggle")
	a.UntilText("log:false")
	a.Do("tap Toggle")
	a.UntilText("log:true")
	s.waitRequests(t, "/mem.png", 2)
}

// A raster image with a color is the color through its alpha, as a
// template, in each mode (filled: a mask of it over the color).
func TestVisualImageRasterTint(t *testing.T) {
	path := visualPNG(t, "pin.png", 8, 8)
	a := Run(t, fmt.Sprintf(`
		component T {
		  Row {
		    padding: 16; gap: 8
		    Image(%[1]q) { width: 32; height: 32; color: .red }
		    Image(%[1]q) { width: 40; height: 32; color: .red; mode: .fill }
		  }
		}
		app { T() }
	`, path))
	tr := a.Until(func(tr *Tree) bool {
		ims := tr.Kind("Image")
		return len(ims) == 2 && vbool(vobj(ims[0], "image"), "loaded") && vbool(vobj(ims[1], "image"), "loaded")
	})
	fit, fill := vobj(tr.Kind("Image")[0], "image"), vobj(tr.Kind("Image")[1], "image")
	if !vbool(fit, "template") || vstr(fit, "tint") == "" {
		t.Errorf("fit: %v", fit)
	}
	if !vbool(fill, "fillTinted") || !vbool(fill, "layerContents") {
		t.Errorf("fill: %v", fill)
	}
	if strings.Contains(tr.Raw(), "\"failed\"") {
		t.Errorf("a load failed")
	}
}
