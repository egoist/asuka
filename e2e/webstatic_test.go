//go:build e2e

package e2e

import (
	"encoding/json"
	"fmt"
	"hash/fnv"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// TestWebStaticServers serves a web build as a plain static server does,
// none of the build's headers (_headers' Cache-Control and the wasm's
// type) and no page for paths with no file, and loads it in headless
// Chrome. Some servers drop requests as a page asks for its files
// together: python3 -m http.server keeps 5 connections waiting and
// closes each after one file, so the dozens of Asuka's files a page
// imports at once see connections reset, which Chrome doesn't ask for
// again on that page. The page then loads again from its service worker
// once that has kept the files (asking for each again if dropped), and
// starts. A build with web.offline off loads again (twice at most); when its
// files never load it says so, on the page and the console, and stops
// there.
func TestWebStaticServers(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	cli := filepath.Join(t.TempDir(), "asuka")
	build := exec.Command("go", "build", "-o", cli, "./cmd/asuka")
	build.Dir = root(t)
	if b, err := build.CombinedOutput(); err != nil {
		t.Fatalf("building the CLI: %v\n%s", err, b)
	}
	site := func(offline bool) string {
		dir := filepath.Join(t.TempDir(), "static")
		os.MkdirAll(dir, 0o755)
		toml := "name = \"Static\"\nid = \"dev.asuka.statictest\"\nversion = \"1.0.0\"\nbuild = 1\n"
		if !offline {
			toml += "\n[web]\noffline = false\n"
		}
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(toml), 0o644)
		os.WriteFile(filepath.Join(dir, "app.asuka"), []byte("component Static {\n  SafeArea {\n    Text(\"static: up\")\n  }\n}\n\napp { Static() }\n"), 0o644)
		b := exec.Command(cli, "build", "--platform", "web", dir)
		b.Dir = root(t)
		if out, err := b.CombinedOutput(); err != nil {
			t.Fatalf("asuka build --platform web: %v\n%s", err, out)
		}
		return filepath.Join(dir, "dist", "web")
	}
	kept, notKept := site(true), site(false)

	c := launchCDP(t, browser)
	// open loads url in a new tab, its console recorded.
	open := func(url string) (page string, logs func() string) {
		t.Helper()
		var created struct{ TargetID string }
		if err := c.call("", "Target.createTarget", map[string]any{"url": "about:blank"}, &created, ""); err != nil {
			t.Fatal(err)
		}
		page = c.attach(t, created.TargetID)
		for _, m := range []string{"Runtime.enable", "Page.enable"} {
			var res any
			if err := c.call(page, m, map[string]any{}, &res, ""); err != nil {
				t.Fatal(err)
			}
		}
		var res any
		if err := c.call(page, "Page.navigate", map[string]any{"url": url}, &res, ""); err != nil {
			t.Fatal(err)
		}
		logs = func() string {
			c.mu.Lock()
			defer c.mu.Unlock()
			var b strings.Builder
			for _, e := range c.events {
				if e.SessionID != page || e.Method != "Runtime.consoleAPICalled" {
					continue
				}
				var p struct {
					Type string
					Args []struct{ Value any }
				}
				json.Unmarshal(e.Params, &p)
				fmt.Fprintf(&b, "%s:", p.Type)
				for _, a := range p.Args {
					fmt.Fprintf(&b, " %v", a.Value)
				}
				b.WriteString("\n")
			}
			return b.String()
		}
		return page, logs
	}
	text := func(page string) string {
		return c.eval(t, page, `document.body ? document.body.innerText : ""`)
	}
	starts := func(name, url string) {
		t.Helper()
		page, logs := open(url)
		until(t, func() bool { return strings.Contains(text(page), "static: up") }, func() string {
			return name + ": " + text(page) + "\n" + logs()
		})
	}

	// Go's file server: every request answered, kept alive.
	starts("a file server", serveStatic(t, kept, nil))

	// One that resets the first connection of a third of Asuka's files.
	var dropped atomic.Int32
	seen := &sync.Map{}
	firstOfSome := func(p string) bool {
		h := fnv.New32a()
		h.Write([]byte(p))
		if !strings.HasPrefix(p, "/asuka/") || h.Sum32()%3 != 0 {
			return false
		}
		_, again := seen.LoadOrStore(p, true)
		if !again {
			dropped.Add(1)
		}
		return !again
	}
	starts("a server that drops connections", serveStatic(t, kept, firstOfSome))
	if dropped.Load() == 0 {
		t.Fatal("the server dropped nothing")
	}

	// python3 -m http.server itself, when there is one.
	if py, err := exec.LookPath("python3"); err == nil {
		l, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		port := l.Addr().(*net.TCPAddr).Port
		l.Close()
		cmd := exec.Command(py, "-m", "http.server", fmt.Sprint(port), "--bind", "127.0.0.1", "--directory", kept)
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { cmd.Process.Kill(); cmd.Wait() })
		addr := fmt.Sprintf("127.0.0.1:%d", port)
		until(t, func() bool {
			conn, err := net.Dial("tcp", addr)
			if err == nil {
				conn.Close()
			}
			return err == nil
		}, func() string { return "python3 -m http.server isn't listening" })
		starts("python3 -m http.server", "http://"+addr+"/")
	} else {
		t.Log("no python3: python3 -m http.server not tried")
	}

	// Offline off: the page loads again, from the network (a file the
	// loader imports dropped once; what that one imports is only asked
	// for as it loads, a load each deeper).
	var once atomic.Bool
	firstHost := func(p string) bool { return strings.HasSuffix(p, "/host.js") && !once.Swap(true) }
	starts("offline off, a server that drops connections", serveStatic(t, notKept, firstHost))

	// A file that never loads: the page says so, after loading twice more.
	var loads atomic.Int32
	never := func(p string) bool {
		if p == "/" || p == "/index.html" {
			loads.Add(1)
		}
		return strings.HasSuffix(p, "/host.js")
	}
	page, logs := open(serveStatic(t, notKept, never))
	until(t, func() bool { return strings.Contains(text(page), "The app couldn't load.") }, func() string { return text(page) + "\n" + logs() })
	if !strings.Contains(logs(), "the app's files didn't load") {
		t.Fatalf("the console:\n%s", logs())
	}
	time.Sleep(time.Second) // no load after it
	if n := loads.Load(); n != 3 {
		t.Fatalf("the page loaded %d times, not 3", n)
	}
}

// serveStatic serves dir as a plain static server: its files, nothing for
// a path without one, no headers of the build's, a connection a request
// (python3 -m http.server's HTTP/1.0); drop, when it says so for a
// request's path, resets its connection with no answer.
func serveStatic(t *testing.T, dir string, drop func(path string) bool) string {
	t.Helper()
	files := http.FileServer(http.Dir(dir))
	srv := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if drop != nil && drop(r.URL.Path) {
			conn, _, err := w.(http.Hijacker).Hijack()
			if err == nil {
				if tc, ok := conn.(*net.TCPConn); ok {
					tc.SetLinger(0)
				}
				conn.Close()
			}
			return
		}
		files.ServeHTTP(w, r)
	})}
	srv.SetKeepAlivesEnabled(false)
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(l)
	t.Cleanup(func() { srv.Close() })
	return "http://" + l.Addr().String() + "/"
}
