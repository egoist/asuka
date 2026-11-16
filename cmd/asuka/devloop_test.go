package main

import (
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/target"
)

// counterDir is an app's directory for the dev loop's tests: its
// asuka.toml and a source, whose module is the hand-assembled Counter
// (useCounter).
func counterDir(t *testing.T) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "counter")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte("name = \"Counter\"\nid = \"dev.asuka.devtest\"\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte("app { Text(\"Count\") }\n"), 0o644)
	return dir
}

// nextReload waits for a player's report of a load: a hang guard, not a
// timing assertion.
func nextReload(t *testing.T, loop *devLoop) devReload {
	t.Helper()
	select {
	case r := <-loop.reloads:
		return r
	case <-time.After(90 * time.Second):
		t.Fatal("no player reported a load")
	}
	return devReload{}
}

// waitFile waits for a file the player writes: a hang guard.
func waitFile(t *testing.T, path string) []byte {
	t.Helper()
	for deadline := time.Now().Add(90 * time.Second); time.Now().Before(deadline); time.Sleep(20 * time.Millisecond) {
		if b, err := os.ReadFile(path); err == nil && len(b) > 0 {
			return b
		}
	}
	t.Fatalf("waiting for %s", path)
	return nil
}

// The dev loop on this Mac: the player (the desktop host, the core
// without an app) gets the Counter's module, counts a press, and gets the
// next module on a save, which it loads keeping the count.
func TestDevDesktop(t *testing.T) {
	needMacBuild(t)
	useCounter(t, "Count: ")
	app := projectApp(t, counterDir(t), nil)
	app.platform = target.MacOS
	work := t.TempDir()
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	exe, err := buildDesktopPlayer(app, platformMac, cbuild.Host(), work, false)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	loaded, tapped, reloaded, last := filepath.Join(dir, "loaded"), filepath.Join(dir, "tapped.png"), filepath.Join(dir, "reloaded"), filepath.Join(dir, "last.png")
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(),
		"ASUKA_DEV=127.0.0.1:"+strconv.Itoa(loop.port()),
		"ASUKA_DATA="+filepath.Join(work, "data"),
		"ASUKA_SCRIPT=waitfor:"+loaded+";tap:Add;snap:"+tapped+";waitfor:"+reloaded,
		"ASUKA_SNAPSHOT="+last, "ASUKA_DUMP=1")
	var out strings.Builder
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Process.Kill()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	os.WriteFile(loaded, nil, 0o644)
	if b := waitFile(t, tapped+".txt"); !strings.Contains(string(b), `"Count: 1"`) {
		t.Fatalf("after tapping Add:\n%s", b)
	}
	// A save: the next module, which says Total.
	useCounter(t, "Total: ")
	if !loop.compile() {
		t.Fatal("didn't compile")
	}
	if r := nextReload(t, loop); r.first || r.kept < 1 {
		t.Fatalf("the reload: %+v", r)
	}
	os.WriteFile(reloaded, nil, 0o644)
	if err := cmd.Wait(); err != nil {
		t.Fatalf("%v\n%s", err, out.String())
	}
	if b := waitFile(t, last+".txt"); !strings.Contains(string(b), `"Total: 1"`) {
		t.Fatalf("after the reload, the window shows (the count kept, the new label):\n%s\n%s", b, out.String())
	}
}

// The dev loop on the web: the page runs the player (the core as
// WebAssembly, without an app), loads the Counter's module, counts a
// press, and loads the next module in place on a save, keeping the
// count.
func TestDevWeb(t *testing.T) {
	if testing.Short() {
		t.Skip("headless Chrome (not -short)")
	}
	browser := chromePath()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	if _, err := cbuild.ForTarget(cbuild.Target{OS: "wasm", Arch: "wasm32"}); err != nil {
		t.Skip(err)
	}
	useCounter(t, "Count: ")
	app := projectApp(t, counterDir(t), nil)
	app.platform = target.Web
	s, loop, err := newWebDev(app, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: s}
	go srv.Serve(ln)
	defer srv.Close()
	url := "http://" + ln.Addr().String() + "/"
	cmd := exec.Command(browser, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
		"--user-data-dir="+t.TempDir(), "--window-size=430,932", url)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait() }()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	automate := func(commands string) string {
		t.Helper()
		resp, err := http.Post(url+"__asuka/automation", "text/plain", strings.NewReader(commands))
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		if resp.StatusCode != 200 {
			t.Fatalf("%s: %s", commands, b)
		}
		return string(b)
	}
	if got := automate("tap Add\ndump"); !strings.Contains(got, "Count: 1") {
		t.Fatalf("after tapping Add, the page shows:\n%s", got)
	}
	useCounter(t, "Total: ")
	if !loop.compile() {
		t.Fatal("didn't compile")
	}
	if r := nextReload(t, loop); r.first || r.kept < 1 {
		t.Fatalf("the reload: %+v", r)
	}
	if got := automate("dump"); !strings.Contains(got, "Total: 1") {
		t.Fatalf("after the reload, the page shows (the count kept, the new label):\n%s", got)
	}
}

// chromePath is Chrome's (or Chromium's) executable, "" if there's none.
func chromePath() string {
	if p := os.Getenv("ASUKA_CHROME"); p != "" {
		return p
	}
	for _, p := range []string{"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/Applications/Chromium.app/Contents/MacOS/Chromium"} {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	for _, n := range []string{"google-chrome", "google-chrome-stable", "chromium", "chromium-browser"} {
		if p, err := exec.LookPath(n); err == nil {
			return p
		}
	}
	return ""
}
