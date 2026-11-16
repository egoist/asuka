package windows_test

import (
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/testserver"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/std"
)

// std/http's streamed bodies and std/websocket on Windows (their
// desktop/'s C: WinHTTP), as platform/desktop's TestStdLinux runs them on
// Linux: its testdata/streams apps against internal/testserver, served
// from this computer, each step a Text the dump shows. A body that timed
// out, or whose read was cancelled, has its request stopped.
func TestStreams(t *testing.T) {
	w := onWindows(t)
	zig(t)
	var stalls atomic.Int32
	h := testserver.Handler()
	ln, err := net.Listen("tcp", ":0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/stall" {
			stalls.Add(1)
			defer stalls.Add(-1)
		}
		h.ServeHTTP(rw, r)
	})}
	go srv.Serve(ln)
	defer srv.Close()
	host := hostAddress(t, w) + ":" + itoa(ln.Addr().(*net.TCPAddr).Port)
	for _, c := range []struct {
		name string
		want []string
	}{
		{"http", []string{
			"stream 200 text/plain; charset=utf-8 first one| all one|two|three|",
			"lines <one><two><three>",
			"events message:first: price:1+2:2 message:last:2",
			"posted 200 true true true",
			"missing 404 false missing",
			"stall timed out true",
			"cancelled 200",
			"again one|two|three|",
			"done",
		}},
		{"websocket", []string{
			"echo chat: header:t1 hello binary bin closed 4000 bye",
			"normal [] ended",
			"client still ended, then refused",
			"refused yes",
			"done",
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			local, guest := w.job(t)
			b, err := os.ReadFile(filepath.Join("..", "desktop", "testdata", "streams", c.name+".asuka"))
			if err != nil {
				t.Fatal(err)
			}
			done := guest + `\done.txt`
			src := strings.NewReplacer(`"BASE"`, `"http://`+host+`"`, `"WSBASE"`, `"ws://`+host+`"`,
				`"DONE_FILE"`, `"`+strings.ReplaceAll(done, `\`, `\\`)+`"`).Replace(string(b))
			buildStdApp(t, w, src, local, "http", "websocket")
			d := script(t, w, local, guest, "app.exe", "waitfor:"+done, "snap:done")
			t.Logf("%s", d["done"])
			want := make([]string, len(c.want))
			for i, s := range c.want {
				want[i] = "#" + s
			}
			shows(t, d["done"], c.name, want)
		})
	}
	for end := time.Now().Add(10 * time.Second); stalls.Load() > 0; time.Sleep(20 * time.Millisecond) {
		if time.Now().After(end) {
			t.Errorf("%d requests to /stall still served after their bodies timed out or were cancelled", stalls.Load())
			break
		}
	}
}

// buildStdApp is buildApp with the native C of the std packages named
// (std/<name>).
func buildStdApp(t *testing.T, w *windows, src, dir string, pkgs ...string) string {
	t.Helper()
	res := compile.Files(map[string][]byte{"app.asuka": []byte(src)}, &compile.Options{Platform: "windows"})
	if err := res.Err(); err != nil {
		t.Fatalf("the app doesn't compile:\n%v", err)
	}
	c, err := appbuild.AppC(appbuild.StripTests(res.Module))
	if err != nil {
		t.Fatal(err)
	}
	dirs, err := std.Glob("desktop")
	if err != nil {
		t.Fatal(err)
	}
	var use []string
	for _, d := range dirs {
		for _, p := range pkgs {
			if filepath.Base(filepath.Dir(d)) == p {
				use = append(use, filepath.Dir(d))
			}
		}
	}
	if len(use) != len(pkgs) {
		t.Fatalf("std packages %v: found %v", pkgs, use)
	}
	natives, err := appbuild.DesktopNatives(target(w), use)
	if err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(dir, "app.exe")
	if err := appbuild.WindowsHost(target(w), appbuild.WindowsOptions{App: []string{c}, Native: natives, Console: true}, exe); err != nil {
		t.Fatal(err)
	}
	return exe
}
