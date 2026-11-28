package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"testing/fstest"
)

// The app's service worker is its settings, then the host's shared
// notification code, its offline files' code, then the worker.
func TestWebWorker(t *testing.T) {
	src := fstest.MapFS{"notify-core.js": {Data: []byte("CORE")}, "offline.js": {Data: []byte("OFFLINE")}, "sw.js": {Data: []byte("WORKER")}}
	b, err := webWorker(src, map[string]any{"name": "com.example.app", "icon": "icons/icon-192.png"})
	if err != nil {
		t.Fatal(err)
	}
	s := string(b)
	i, j, o, k := strings.Index(s, `globalThis.asukaApp = {"icon":"icons/icon-192.png","name":"com.example.app"};`), strings.Index(s, "CORE"), strings.Index(s, "OFFLINE"), strings.Index(s, "WORKER")
	if i < 0 || j < i || o < j || k < o {
		t.Fatalf("worker:\n%s", s)
	}
	if _, err := webWorker(fstest.MapFS{}, nil); err == nil {
		t.Fatal("no host files: no error")
	}
}

// asuka.toml's web.vapidPublicKey is checked: a P-256 point, in base64url.
func TestWebVAPIDKey(t *testing.T) {
	const key = "BKpYsAT0ADj2pHQQZeNsXmA3fgij4OZ_7ZqkS63fvpscgjHLZys0UDBluROJdtDaGlE3WW1_IlFyzVzC2LlHyO0"
	app := &asukaApp{cfg: &appConfig{}}
	if k, err := webVAPIDKey(app); k != "" || err != nil {
		t.Fatalf("none: %q %v", k, err)
	}
	app.cfg.Web.VapidPublicKey = " " + key + "= "
	if k, err := webVAPIDKey(app); k != key || err != nil {
		t.Fatalf("%q %v", k, err)
	}
	for _, bad := range []string{"AAAA", key[:len(key)-4] + "AAAA", "not base64!"} {
		app.cfg.Web.VapidPublicKey = bad
		if _, err := webVAPIDKey(app); err == nil {
			t.Errorf("%q: no error", bad)
		}
	}
}

// A web build's service worker keeps the site's files offline: each with
// its revision (none for one named by its digest), but what a server
// reads, what changes without a build, the gzipped copies and the worker;
// their version changes with any file, and only then.
func TestWebPrecache(t *testing.T) {
	site := t.TempDir()
	write := func(p, s string) {
		f := filepath.Join(site, filepath.FromSlash(p))
		os.MkdirAll(filepath.Dir(f), 0o755)
		if err := os.WriteFile(f, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for p, s := range map[string]string{
		"index.html": "<!doctype html>", "manifest.webmanifest": "{}", "icons/icon-192.png": "PNG", "assets/fonts/a.ttf": "FONT",
		"asuka/0123456789ab/asuka.js": "JS", "asuka/0123456789ab/asuka.js.gz": "GZ", "asuka/0123456789ab/packages/x/x.js": "PKG",
		"asuka.abcdef012345.wasm": "WASM", "asuka.abcdef012345.wasm.gz": "GZ", "_headers": "H", "_redirects": "R", "version.json": "{}",
		"sw.js": "SW", ".asuka.wasm": "TMP", ".cache/x": "TMP",
	} {
		write(p, s)
	}
	pc, err := webPrecache(site, "asuka/0123456789ab", "asuka.abcdef012345.wasm")
	if err != nil {
		t.Fatal(err)
	}
	var got [][2]string
	for _, f := range pc["files"].([]webPrecacheFile) {
		got = append(got, f)
	}
	rev := func(content string) string {
		sum := sha256.Sum256([]byte(content))
		return hex.EncodeToString(sum[:])[:16]
	}
	want := [][2]string{
		{"assets/fonts/a.ttf", rev("FONT")},
		{"asuka.abcdef012345.wasm", ""},
		{"asuka/0123456789ab/asuka.js", ""},
		{"asuka/0123456789ab/packages/x/x.js", ""},
		{"icons/icon-192.png", rev("PNG")},
		{"index.html", rev("<!doctype html>")},
		{"manifest.webmanifest", rev("{}")},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("files:\n%v\nwant\n%v", got, want)
	}
	// As the worker's settings say it.
	b, _ := json.Marshal(pc)
	if !strings.Contains(string(b), `["index.html","`) || !strings.Contains(string(b), `["asuka.abcdef012345.wasm",""]`) {
		t.Fatalf("settings: %s", b)
	}
	v := pc["version"].(string)
	if len(v) != 16 {
		t.Fatalf("version %q", v)
	}
	again, _ := webPrecache(site, "asuka/0123456789ab", "asuka.abcdef012345.wasm")
	if again["version"] != v {
		t.Fatalf("the same files, another version: %v %v", again["version"], v)
	}
	write("version.json", `{"version":"2"}`)
	write("sw.js", "SW2")
	if pc2, _ := webPrecache(site, "asuka/0123456789ab", "asuka.abcdef012345.wasm"); pc2["version"] != v {
		t.Fatal("version.json or the worker changed the version")
	}
	write("index.html", "<!doctype html><title>2</title>")
	pc3, _ := webPrecache(site, "asuka/0123456789ab", "asuka.abcdef012345.wasm")
	if pc3["version"] == v {
		t.Fatal("a new index.html, the same version")
	}
	if pc3["files"].([]webPrecacheFile)[5][1] == got[5][1] {
		t.Fatal("a new index.html, the same revision")
	}
}

// asuka.toml's web.offline is on unless it says false.
func TestWebOffline(t *testing.T) {
	c := &appConfig{}
	if !c.webOffline() {
		t.Fatal("default: off")
	}
	off := false
	c.Web.Offline = &off
	if c.webOffline() {
		t.Fatal("false: on")
	}
}
