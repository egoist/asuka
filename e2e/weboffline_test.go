//go:build e2e

package e2e

import (
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
)

// TestWebOffline drives a web build (asuka build --platform web) in
// headless Chrome as a server would serve it, its routes' paths the page:
// the page's load installs the app's service worker, which keeps the
// site's files; with the network gone (DevTools' offline, and the server
// answering nothing but 503s) a reload, and a route's path, still start
// the app. Then a new build is deployed: the page's next load is the old
// version's while the browser installs the new worker in the background;
// the load after has the new version take over, and shows it.
func TestWebOffline(t *testing.T) {
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
	// The same app, one version and the next: a build's site each.
	site := func(version int) string {
		dir := filepath.Join(t.TempDir(), "offline")
		os.MkdirAll(filepath.Join(dir, "assets"), 0o755)
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(fmt.Sprintf("name = \"Offline\"\nid = \"dev.asuka.offlinetest\"\nversion = \"1.0.%d\"\nbuild = %d\n", version, version)), 0o644)
		os.WriteFile(filepath.Join(dir, "assets", "note.txt"), []byte(fmt.Sprintf("note %d", version)), 0o644)
		os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(fmt.Sprintf(`component Offline {
  SafeArea {
    Text("version: %d")
  }
}

app { Offline() }
`, version)), 0o644)
		b := exec.Command(cli, "build", "--platform", "web", dir)
		b.Dir = root(t)
		if out, err := b.CombinedOutput(); err != nil {
			t.Fatalf("asuka build --platform web: %v\n%s", err, out)
		}
		return filepath.Join(dir, "dist", "web")
	}
	var current atomic.Pointer[string]
	var down atomic.Bool
	first := site(1)
	current.Store(&first)
	// A static host: the site's files, and its page for any other path
	// (_redirects' rule); the page and the worker never cached.
	srv := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if down.Load() {
			http.Error(w, "down", http.StatusServiceUnavailable)
			return
		}
		dir := *current.Load()
		p := filepath.Join(dir, filepath.FromSlash(strings.TrimPrefix(r.URL.Path, "/")))
		if st, err := os.Stat(p); err != nil || st.IsDir() {
			p = filepath.Join(dir, "index.html")
		}
		switch filepath.Base(p) {
		case "index.html", "sw.js", "version.json", "manifest.webmanifest":
			w.Header().Set("Cache-Control", "no-cache")
		}
		if strings.HasSuffix(p, ".wasm") {
			w.Header().Set("Content-Type", "application/wasm")
		}
		http.ServeFile(w, r, p)
	})}
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(l)
	t.Cleanup(func() { srv.Close() })
	pageURL := "http://" + l.Addr().String() + "/"

	c := launchCDP(t, browser)
	var created struct{ TargetID string }
	if err := c.call("", "Target.createTarget", map[string]any{"url": "about:blank"}, &created, ""); err != nil {
		t.Fatal(err)
	}
	page := c.attach(t, created.TargetID)
	do := func(method string, params map[string]any) {
		t.Helper()
		var res any
		if err := c.call(page, method, params, &res, ""); err != nil {
			t.Fatal(err)
		}
	}
	text := func() string {
		return c.eval(t, page, `document.body ? document.body.innerText : ""`)
	}
	shows := func(want string) {
		t.Helper()
		until(t, func() bool { return strings.Contains(text(), want) }, text)
	}
	do("Page.enable", map[string]any{})
	do("Network.enable", map[string]any{})
	do("Page.navigate", map[string]any{"url": pageURL})
	shows("version: 1")
	// The worker took the page over once it had kept the files.
	until(t, func() bool {
		return c.eval(t, page, `navigator.serviceWorker.ready.then(() => !!navigator.serviceWorker.controller)`) == "true"
	}, func() string { return c.eval(t, page, `String(navigator.serviceWorker.controller)`) })
	// Every file of the site but the server's, version.json, the gzipped
	// copies and the worker.
	kept := c.eval(t, page, `caches.keys().then(async (ks) => { const k = ks.find((k) => k.startsWith("asuka-precache:")); const urls = (await (await caches.open(k)).keys()).map((r) => r.url); return [urls.length, urls.some((u) => u.includes("index.html?__asuka=")), urls.some((u) => /asuka\.[0-9a-f]{12}\.wasm$/.test(u)), urls.some((u) => /\.gz$|^\/(version\.json|sw\.js|_headers|_redirects)$/.test(new URL(u).pathname))].join(" ") })`)
	if n := 0; func() bool { fmt.Sscan(kept, &n); return n < 20 }() || !strings.HasSuffix(kept, " true true false") {
		t.Fatalf("kept: %q", kept)
	}

	// Offline: the browser's network, and the server.
	offline := func(on bool) {
		do("Network.emulateNetworkConditions", map[string]any{"offline": on, "latency": 0, "downloadThroughput": -1, "uploadThroughput": -1})
		down.Store(on)
	}
	offline(true)
	do("Page.reload", map[string]any{"ignoreCache": false})
	shows("version: 1")
	do("Page.navigate", map[string]any{"url": pageURL + "notes/7?from=e2e"})
	shows("version: 1")
	if got := c.eval(t, page, `fetch("assets/note.txt").then((r) => r.text())`); got != "note 1" {
		t.Fatalf("an asset offline: %q", got)
	}
	// What isn't the app's isn't kept: the network's (gone).
	if got := c.eval(t, page, `fetch("version.json?t=1").then((r) => "answered " + r.status, () => "failed")`); got != "failed" {
		t.Fatalf("version.json offline: %q", got)
	}

	// A new build deployed: the next load is still the old version's
	// while the new worker installs; the one after is the new version's.
	offline(false)
	second := site(2)
	current.Store(&second)
	do("Page.navigate", map[string]any{"url": pageURL})
	shows("version: 1")
	until(t, func() bool {
		return c.eval(t, page, `navigator.serviceWorker.getRegistration().then((r) => !!(r && r.waiting))`) == "true"
	}, func() string {
		return c.eval(t, page, `navigator.serviceWorker.getRegistration().then((r) => JSON.stringify({ installing: !!r.installing, waiting: !!r.waiting, active: r.active && r.active.scriptURL }))`)
	})
	do("Page.reload", map[string]any{})
	shows("version: 2")
	if got := c.eval(t, page, `fetch("assets/note.txt").then((r) => r.text())`); got != "note 2" {
		t.Fatalf("the new version's asset: %q", got)
	}
	// The old version's files go once the one before it does: two kept.
	if got := c.eval(t, page, `caches.keys().then((ks) => ks.filter((k) => k.startsWith("asuka-precache:")).length)`); got != "2" {
		t.Fatalf("versions kept: %s", got)
	}
	// And the new version works offline too.
	offline(true)
	do("Page.reload", map[string]any{})
	shows("version: 2")
}
