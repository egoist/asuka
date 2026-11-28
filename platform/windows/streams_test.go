package windows_test

import (
	"crypto/sha256"
	"encoding/hex"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/testserver"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/std"
)

// std/http's streamed bodies, std/websocket and their TLS settings on
// Windows (their desktop/'s C: WinHTTP), as platform/desktop's
// TestStdLinux runs them on Linux: its testdata/streams apps against
// internal/testserver (over HTTPS too, a test authority's certificate),
// served from this computer, each step a Text the dump shows. A body that timed
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
		// Not /upload: an upload cancelled, its connection gone from the
		// VM (netstat shows none), is still served here for 15 s or so,
		// the VM's network holding this side of it. TestStdLinux checks
		// that cancelled uploads stop.
		if r.URL.Path == "/stall" {
			stalls.Add(1)
			defer stalls.Add(-1)
		}
		h.ServeHTTP(rw, r)
	})}
	go srv.Serve(ln)
	defer srv.Close()
	ip := hostAddress(t, w)
	host := ip + ":" + itoa(ln.Addr().(*net.TCPAddr).Port)
	// The same paths over HTTPS, a test authority's certificate for the
	// address the VM reaches this computer at.
	pki, err := testserver.NewPKI(ip)
	if err != nil {
		t.Fatal(err)
	}
	tln, err := net.Listen("tcp", ":0")
	if err != nil {
		t.Fatal(err)
	}
	tsrv := &http.Server{Handler: h, TLSConfig: pki.ServerConfig()}
	go tsrv.ServeTLS(tln, "", "")
	defer tsrv.Close()
	tlsBase := "https://" + ip + ":" + itoa(tln.Addr().(*net.TCPAddr).Port)
	for _, c := range []struct {
		name string
		want []string
	}{
		{"http", []string{
			"stream 200 text/plain; charset=utf-8 first one| all one|two|three|",
			"lines <one><two><three>",
			"events message:first: price:1+2:2 message:last:2",
			"posted 200 true true true",
			"long true false",
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
		{"upload", []string{
			"made 268435456",
			"big 200 true true true true true true",
			"memory true",
			"slow 200 true true true true",
			"multipart 200 true true true true true",
			"chunked 200 true true true true true 1000000 1000000",
			"sized 200 true true true true",
			"short true",
			"early 413 refused",
			"early stream 413 refused",
			"early big failed", // WinHTTP sends the body without waiting for the answer to Expect: 100-continue, and fails as the server closes
			"cancelled file",
			"cancelled stream",
			"again 200 true",
			"finished 0",
			"done",
		}},
		{"tls", []string{
			"http refused 403 untrusted 403 403",
			"mutual 200 client: Asuka Test Client",
			"wss refused hello untrusted hello",
			"again refused untrusted refused",
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
				`"DONE_FILE"`, `"`+strings.ReplaceAll(done, `\`, `\\`)+`"`,
				`"TLS_BASE"`, strconv.Quote(tlsBase), `"WSS_BASE"`, strconv.Quote("wss"+strings.TrimPrefix(tlsBase, "https")),
				`"TLS_CA"`, strconv.Quote(pki.CA), `"TLS_OTHER_CA"`, strconv.Quote(pki.OtherCA),
				`"TLS_CLIENT_CERT"`, strconv.Quote(pki.ClientCert), `"TLS_CLIENT_KEY"`, strconv.Quote(pki.ClientKey),
				`"BIG_SHA"`, strconv.Quote(bigUploadSHA())).Replace(string(b))
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

// bigUploadSHA is the SHA-256 of the file testdata/streams/upload.asuka
// makes and uploads: "0123456789abcdef" over 256 MiB.
func bigUploadSHA() string {
	h := sha256.New()
	mib := []byte(strings.Repeat("0123456789abcdef", 65536))
	for i := 0; i < 256; i++ {
		h.Write(mib)
	}
	return hex.EncodeToString(h.Sum(nil))
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
