package main

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
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
// next module on a save, which it loads keeping the count; a restart
// (asuka dev's r) loads it again, the count back to its initial value.
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
	loaded, tapped, reloaded, kept, restarted, last := filepath.Join(dir, "loaded"), filepath.Join(dir, "tapped.png"), filepath.Join(dir, "reloaded"), filepath.Join(dir, "kept.png"), filepath.Join(dir, "restarted"), filepath.Join(dir, "last.png")
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(),
		"ASUKA_DEV=127.0.0.1:"+strconv.Itoa(loop.port()),
		"ASUKA_DATA="+filepath.Join(work, "data"),
		"ASUKA_SCRIPT=waitfor:"+loaded+";tap:Add;snap:"+tapped+";waitfor:"+reloaded+";snap:"+kept+";waitfor:"+restarted,
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
	inspects(t, loop, "macOS", `"text":"Count: 1"`)
	// A save: the next module, which says Total.
	useCounter(t, "Total: ")
	if !loop.compile() {
		t.Fatal("didn't compile")
	}
	if r := nextReload(t, loop); r.first || r.kept < 1 {
		t.Fatalf("the reload: %+v", r)
	}
	os.WriteFile(reloaded, nil, 0o644)
	if b := waitFile(t, kept+".txt"); !strings.Contains(string(b), `"Total: 1"`) {
		t.Fatalf("after the reload, the window shows (the count kept, the new label):\n%s\n%s", b, out.String())
	}
	loop.restart()
	if r := nextReload(t, loop); !r.restarted || r.kept != 0 {
		t.Fatalf("the restart: %+v", r)
	}
	os.WriteFile(restarted, nil, 0o644)
	if err := cmd.Wait(); err != nil {
		t.Fatalf("%v\n%s", err, out.String())
	}
	if b := waitFile(t, last+".txt"); !strings.Contains(string(b), `"Total: 0"`) {
		t.Fatalf("after the restart, the window shows (the count from its initial value):\n%s\n%s", b, out.String())
	}
}

// inspects asks the players for their views, as the inspector's page
// does, and wants one player, called name, whose tree has each of want.
func inspects(t *testing.T, loop *devLoop, name string, want ...string) []inspectorPlayer {
	t.Helper()
	rec := httptest.NewRecorder()
	loop.inspector.ServeHTTP(rec, httptest.NewRequest("GET", "/tree", nil))
	var got struct{ Players []inspectorPlayer }
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatalf("%v: %s", err, rec.Body.String())
	}
	if len(got.Players) != 1 || !strings.HasPrefix(got.Players[0].Name, name) {
		t.Fatalf("the inspector's players: %s", rec.Body.String())
	}
	tree := string(got.Players[0].Tree)
	for _, w := range want {
		if !strings.Contains(tree, w) {
			t.Fatalf("the tree has no %s:\n%s", w, tree)
		}
	}
	rec = httptest.NewRecorder()
	loop.inspector.ServeHTTP(rec, httptest.NewRequest("GET", "/", nil))
	if !strings.Contains(rec.Body.String(), "<title>Asuka Inspector</title>") {
		t.Fatalf("the inspector's page: %s", rec.Body.String())
	}
	return got.Players
}

// treePath is the path of the view in an inspector's tree that shows
// text ("0.1.0"), and its size ("120x20"), as the page sends it.
func treePath(t *testing.T, tree []byte, text string) (string, string) {
	t.Helper()
	type view struct {
		Text  string
		Frame []float64
		Kids  []json.RawMessage
	}
	var find func(raw json.RawMessage, path string) (string, string)
	find = func(raw json.RawMessage, path string) (string, string) {
		var v view
		json.Unmarshal(raw, &v)
		if v.Text == text && len(v.Frame) == 4 {
			return path, fmt.Sprintf("%dx%d", int(math.Round(v.Frame[2])), int(math.Round(v.Frame[3])))
		}
		for i, k := range v.Kids {
			if p, s := find(k, path+"."+strconv.Itoa(i)); p != "" {
				return p, s
			}
		}
		return "", ""
	}
	var roots struct{ Roots []json.RawMessage }
	json.Unmarshal(tree, &roots)
	for i, r := range roots.Roots {
		if p, s := find(r, strconv.Itoa(i)); p != "" {
			return p, s
		}
	}
	t.Fatalf("no view shows %q in %s", text, tree)
	return "", ""
}

// The dev loop on the web: the page runs the player (the core as
// WebAssembly, without an app), loads the Counter's module, counts a
// press, and loads the next module in place on a save, keeping the
// count; a restart loads it again, the count from its initial value.
func TestDevWeb(t *testing.T) {
	useCounter(t, "Count: ")
	loop, automate := webDev(t, counterDir(t))
	if got := automate("tap Add\ndump"); !strings.Contains(got, "Count: 1") {
		t.Fatalf("after tapping Add, the page shows:\n%s", got)
	}
	players := inspects(t, loop, "Web (Chrome)", `"text":"Count: 1"`)
	// The inspector's highlight: over the text it names, in the page.
	path, size := treePath(t, players[0].Tree, "Count: 1")
	rec := httptest.NewRecorder()
	loop.inspector.ServeHTTP(rec, httptest.NewRequest("POST", "/highlight?key="+url.QueryEscape(players[0].Key)+"&path="+path, nil))
	if rec.Code != 200 {
		t.Fatalf("highlight: %d %s", rec.Code, rec.Body.String())
	}
	got := ""
	for i := 0; i < 100 && got != size; i++ {
		got = strings.TrimSpace(automate("highlighted"))
	}
	if got != size {
		t.Fatalf("the highlight is %s, want %s (the text's)", got, size)
	}
	loop.inspector.ServeHTTP(httptest.NewRecorder(), httptest.NewRequest("POST", "/highlight?key="+url.QueryEscape(players[0].Key)+"&path=", nil))
	for i := 0; i < 100 && got != "none"; i++ {
		got = strings.TrimSpace(automate("highlighted"))
	}
	if got != "none" {
		t.Fatalf("after the highlight's cleared, it's %s", got)
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
	// A restart (asuka dev's r): the count from its initial value.
	loop.restart()
	if r := nextReload(t, loop); !r.restarted || r.kept != 0 {
		t.Fatalf("the restart: %+v", r)
	}
	if got := automate("dump"); !strings.Contains(got, "Total: 0") {
		t.Fatalf("after the restart, the page shows (the count from its initial value):\n%s", got)
	}
}

// A reload of an app compiled from its source: a state whose initializer
// was edited starts from the new one, the others keep their values, and
// one whose initializer was only laid out differently keeps its value.
func TestDevWebEditedInitializer(t *testing.T) {
	dir := counterDir(t)
	write := func(count, taps string) {
		src := "component Counter {\n  state count = " + count + "\n  state taps = " + taps + "\n" +
			"  Column {\n    Text(\"Count: {count}, taps: {taps}\")\n" +
			"    Button(\"Add\") { on press { count += 1; taps += 1 } }\n  }\n}\n\napp { Counter() }\n"
		os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(src), 0o644)
	}
	write("0", "0")
	loop, automate := webDev(t, dir)
	if got := automate("tap Add\ndump"); !strings.Contains(got, "Count: 1, taps: 1") {
		t.Fatalf("after tapping Add, the page shows:\n%s", got)
	}
	reload := func(count, taps string, kept uint32, want string) {
		t.Helper()
		write(count, taps)
		if !loop.compile() {
			t.Fatal("didn't compile")
		}
		if r := nextReload(t, loop); r.first || r.restarted || r.kept != kept {
			t.Fatalf("count = %s, taps = %s: the reload %+v, want %d kept", count, taps, r, kept)
		}
		if got := automate("dump"); !strings.Contains(got, want) {
			t.Fatalf("count = %s, taps = %s: the page shows\n%s\nwant %q", count, taps, got, want)
		}
	}
	// count's initializer edited: it starts from it; taps kept.
	reload("5+5", "0", 1, "Count: 10, taps: 1")
	if got := automate("tap Add\ndump"); !strings.Contains(got, "Count: 11, taps: 2") {
		t.Fatalf("after tapping Add, the page shows:\n%s", got)
	}
	// Laid out differently: both kept.
	reload("5 + 5", "0", 2, "Count: 11, taps: 2")
	// Back to the first initializer: from it, not from what count was
	// when it last had it.
	reload("0", "0", 1, "Count: 0, taps: 2")
}

// boomApp is an app whose Boom button throws an error nothing catches.
const boomApp = `component Boom {
  state n = 0

  Column {
    Text("n: {n}")
    Button("Add") { on press { n += 1 } }
    Button("Boom") { on press { try explode(n) } }
  }
}

fn explode(n: int) throws {
  throw Error("boom at {n}")
}

app { Boom() }
`

// shows polls a player's dump until it has each of want (a hang guard,
// not a timing assertion), and returns it.
func shows(t *testing.T, dump func() string, want ...string) string {
	t.Helper()
	var got string
	for i := 0; i < 200; i++ {
		got = dump()
		ok := true
		for _, w := range want {
			ok = ok && strings.Contains(got, w)
		}
		if ok {
			return got
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("the player shows:\n%s\nwant %q", got, want)
	return ""
}

// asuka dev's error overlay on the web: an error the app didn't catch
// shows over it with the lines where it was thrown, and Dismiss goes back
// to the app as it was; a build that fails shows until it builds again;
// Reload starts the app again.
func TestDevWebErrorOverlay(t *testing.T) {
	dir := counterDir(t)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(boomApp), 0o644)
	loop, automate := webDev(t, dir)
	dump := func() string { return automate("dump") }
	automate("tap Add\ntap Boom")
	shows(t, dump, "Uncaught error", "boom at 1", "> 12 |   throw Error(\"boom at {n}\")", "explode (app.asuka:12)", "Dismiss", "Reload")
	if got := automate("tap Dismiss\ndump"); strings.Contains(got, "Uncaught error") || !strings.Contains(got, "n: 1") {
		t.Fatalf("after Dismiss, the page shows:\n%s", got)
	}
	// A save that doesn't build: the overlay says why, over the app.
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(strings.Replace(boomApp, "n += 1", "n += \"x\"", 1)), 0o644)
	if loop.compile() {
		t.Fatal("it compiled")
	}
	shows(t, dump, "Build failed", "app.asuka:6:32: += doesn't take an int and a string", "> 6 |", "^^^")
	// Fixed: the overlay goes, the app reloads.
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(boomApp), 0o644)
	if !loop.compile() {
		t.Fatal("didn't compile")
	}
	nextReload(t, loop)
	if got := dump(); strings.Contains(got, "Build failed") || !strings.Contains(got, "n: 1") {
		t.Fatalf("after the fix, the page shows:\n%s", got)
	}
	// Reload: the app starts again, from its initial state.
	automate("tap Boom")
	shows(t, dump, "Uncaught error", "boom at 1")
	automate("tap Reload")
	if r := nextReload(t, loop); !r.restarted {
		t.Fatalf("Reload: %+v", r)
	}
	if got := dump(); strings.Contains(got, "Uncaught error") || !strings.Contains(got, "n: 0") {
		t.Fatalf("after Reload, the page shows:\n%s", got)
	}
}

// webDev serves the app in dir under asuka dev's web server to headless
// Chrome, and waits for the page's first load: the loop, and the page's
// automation (its commands, a line each; what it answers).
func webDev(t *testing.T, dir string) (*devLoop, func(commands string) string) {
	t.Helper()
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
	app := projectApp(t, dir, nil)
	app.platform = target.Web
	s, loop, err := newWebDev(app, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(loop.close)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: s}
	go srv.Serve(ln)
	t.Cleanup(func() { srv.Close() })
	url := "http://" + ln.Addr().String() + "/"
	cmd := exec.Command(browser, "--headless=new", "--no-first-run", "--no-default-browser-check", "--disable-gpu",
		"--user-data-dir="+t.TempDir(), "--window-size=430,932", url)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cmd.Process.Kill(); cmd.Wait() })
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	return loop, func(commands string) string {
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
}

// chromePath is Chrome's (or Chromium's) executable, "" if there's none.
// asuka dev's error overlay on this Mac: an error the app didn't catch
// shows over the player's window, in AppKit's views, with the lines where
// it was thrown and the stack, and Dismiss goes back to the app as it was;
// a build that fails shows until it builds again; Reload starts the app
// again. ASUKA_DEVERROR_SNAPS=dir keeps the overlay's snapshots there.
func TestDevDesktopErrorOverlay(t *testing.T) {
	needMacBuild(t)
	dir := counterDir(t)
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(boomApp), 0o644)
	app := projectApp(t, dir, nil)
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
	cmds := t.TempDir()
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(),
		"ASUKA_DEV=127.0.0.1:"+strconv.Itoa(loop.port()),
		"ASUKA_DATA="+filepath.Join(work, "data"),
		"ASUKA_COMMANDS="+cmds)
	var out strings.Builder
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait() }()
	if r := nextReload(t, loop); !r.first {
		t.Fatalf("the first report: %+v", r)
	}
	// automate runs the player's automation commands (a line each) and
	// returns its tree's dump, which has the overlay's texts.
	automate := func(commands string) string {
		t.Helper()
		done, tree := filepath.Join(cmds, "asuka-done.txt"), filepath.Join(cmds, "asuka-tree.txt")
		os.Remove(done)
		os.Remove(tree)
		part := filepath.Join(cmds, "commands.part")
		os.WriteFile(part, []byte(commands+"\ntree\n"), 0o644)
		os.Rename(part, filepath.Join(cmds, "asuka-commands.txt"))
		// The player writes done, empty, once it has run them (a hang guard).
		for deadline := time.Now().Add(90 * time.Second); ; time.Sleep(20 * time.Millisecond) {
			if _, err := os.Stat(done); err == nil {
				break
			}
			if time.Now().After(deadline) {
				t.Fatalf("the player didn't run %q:\n%s", commands, out.String())
			}
		}
		b, _ := os.ReadFile(tree)
		return string(b)
	}
	dump := func() string { return automate("") }
	snaps := os.Getenv("ASUKA_DEVERROR_SNAPS")
	snap := func(name string) string {
		if snaps == "" {
			return ""
		}
		return "snap " + filepath.Join(snaps, name) + "\n"
	}
	automate("tap Add\ntap Boom")
	shows(t, dump, "deverror \"Uncaught error\"", "\"boom at 1\"", "> 12 |   throw Error(\"boom at {n}\")", "explode (app.asuka:12)",
		"button \"Dismiss\"", "button \"Reload\" default")
	automate(snap("uncaught.png"))
	if got := automate("tap Dismiss"); strings.Contains(got, "Uncaught error") || !strings.Contains(got, `"n: 1"`) {
		t.Fatalf("after Dismiss, the window shows:\n%s", got)
	}
	// A save that doesn't build: the overlay says why, over the app.
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(strings.Replace(boomApp, "n += 1", "n += \"x\"", 1)), 0o644)
	if loop.compile() {
		t.Fatal("it compiled")
	}
	shows(t, dump, "deverror \"Build failed\" build", "app.asuka:6:32: += doesn't take an int and a string", "> 6 |", "^^^")
	automate(snap("build.png"))
	// Fixed: the overlay goes, the app reloads.
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(boomApp), 0o644)
	if !loop.compile() {
		t.Fatal("didn't compile")
	}
	nextReload(t, loop)
	if got := shows(t, dump, `"n: 1"`); strings.Contains(got, "Build failed") {
		t.Fatalf("after the fix, the window shows:\n%s", got)
	}
	// Reload: the app starts again, from its initial state.
	automate("tap Boom")
	shows(t, dump, "Uncaught error", "boom at 1")
	automate("tap Reload")
	if r := nextReload(t, loop); !r.restarted {
		t.Fatalf("Reload: %+v", r)
	}
	if got := shows(t, dump, `"n: 0"`); strings.Contains(got, "Uncaught error") {
		t.Fatalf("after Reload, the window shows:\n%s\n%s", got, out.String())
	}
}

// A player on this computer is asuka dev's own process, which writes
// what the app prints to asuka dev's terminal itself: asuka dev doesn't
// write it again. A phone's goes to the terminal through asuka dev.
func TestDesktopPlayer(t *testing.T) {
	for hello, want := range map[string]bool{
		`{"platform":"darwin","device":"desktop","os":"15.0"}`: true,
		`{"platform":"windows","device":"desktop"}`:            true,
		`{"platform":"linux","device":"desktop"}`:              true,
		`{"platform":"ios","device":"iPhone 16"}`:              false,
		`{"platform":"android","device":"emulator-5554"}`:      false,
		`{"platform":"web"}`:                                   false,
	} {
		if got := desktopPlayer([]byte(hello)); got != want {
			t.Errorf("%s: %v, want %v", hello, got, want)
		}
	}
}
