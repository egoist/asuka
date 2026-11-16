//go:build darwin

// Package mactest runs Asuka apps on the macOS host (platform/macos) for
// the host's tests: asuka dev's player for the Mac (the Swift host and the
// core without an app), built once per test run by the CLI, runs each
// test's app, compiled from Asuka source and sent as asuka dev sends it.
// The test drives the app through the host's automation (ASUKA_COMMANDS:
// taps, typing, keys, menus) and reads what it shows: the view tree, the
// windows, the menu bar and the tray as JSON (the host's), or the texts.
//
//	a := mactest.Run(t, `
//	  component Counter { state n = 0; Button("Add {n}") { on press { n += 1 } } }
//	  app { Counter() }
//	`)
//	a.Do("tap Add 0")
//	a.Until(func(t *mactest.Tree) bool { return t.Find("Button", "Add 1") != nil })
package mactest

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/cachedir"

	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

var (
	once      sync.Once
	playerExe string
	playerErr error
)

// root is the repository's root.
func root() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Dir(filepath.Dir(filepath.Dir(filepath.Dir(file))))
}

// Player builds the Mac player once per process, with the CLI (asuka dev
// --once on an app of its own, which it then lets go of), and returns its
// executable. It skips the test where there's no Xcode.
func Player(t testing.TB) string {
	t.Helper()
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode")
	}
	once.Do(func() { playerExe, playerErr = buildPlayer() })
	if playerErr != nil {
		t.Fatalf("building the Mac player: %v", playerErr)
	}
	return playerExe
}

// buildPlayer builds the player (one build at a time, across processes:
// a file lock) and copies it for this process, which runs its copy while
// another build may replace the built one.
func buildPlayer() (string, error) {
	if p := os.Getenv("ASUKA_MAC_PLAYER"); p != "" {
		return p, nil
	}
	dir := cachedir.Path("mactest")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	lock, err := os.OpenFile(filepath.Join(dir, ".lock"), os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return "", err
	}
	defer lock.Close()
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX); err != nil {
		return "", err
	}
	exe, err := buildPlayerLocked(dir)
	if err != nil {
		return "", err
	}
	app := filepath.Dir(filepath.Dir(filepath.Dir(exe)))
	own, err := os.MkdirTemp("", "asuka-mactest-")
	if err != nil {
		return "", err
	}
	cp := filepath.Join(own, filepath.Base(app))
	if b, err := exec.Command("ditto", app, cp).CombinedOutput(); err != nil {
		return "", fmt.Errorf("copying the player: %v\n%s", err, b)
	}
	return filepath.Join(cp, "Contents", "MacOS", filepath.Base(exe)), nil
}

func buildPlayerLocked(dir string) (string, error) {
	app := filepath.Join(dir, "mactest") // its work directory: <cache>/asuka/mactest-<hash>/mac
	if err := os.MkdirAll(app, 0o755); err != nil {
		return "", err
	}
	os.WriteFile(filepath.Join(app, "asuka.toml"), []byte("name = \"Mac Test\"\nid = \"dev.asuka.mactest\"\nversion = \"1.0.0\"\n"), 0o644)
	os.WriteFile(filepath.Join(app, "app.asuka"), []byte("app { Text(\"ready\") }\n"), 0o644)
	cli := filepath.Join(dir, "asuka")
	build := exec.Command("go", "build", "-o", cli, "./cmd/asuka")
	build.Dir = root()
	if b, err := build.CombinedOutput(); err != nil {
		return "", fmt.Errorf("building the CLI: %v\n%s", err, b)
	}
	// The player builds, starts, shows the app once, and quits.
	snap := filepath.Join(dir, "ready.png")
	os.Remove(snap)
	started := time.Now()
	c := exec.Command(cli, "dev", "--platform", "mac", "--once", app)
	c.Env = append(os.Environ(), "ASUKA_SNAPSHOT="+snap)
	var out bytes.Buffer
	c.Stdout, c.Stderr = &out, &out
	if err := c.Start(); err != nil {
		return "", err
	}
	done := make(chan error, 1)
	go func() { done <- c.Wait() }()
	exe := ""
	deadline := time.Now().Add(10 * time.Minute)
	for time.Now().Before(deadline) {
		if exe == "" {
			ms, _ := filepath.Glob(filepath.Join(cachedir.Root(), "mactest-*", "mac", "player", "Mac Test.app", "Contents", "MacOS", "Mac Test"))
			for _, m := range ms {
				if st, err := os.Stat(m); err == nil && !st.ModTime().Before(started) {
					exe = m
				}
			}
		}
		select {
		case err := <-done:
			if exe == "" {
				return "", fmt.Errorf("asuka dev: %v\n%s", err, out.String())
			}
			return exe, nil
		case <-time.After(200 * time.Millisecond):
		}
		if exe != "" {
			if _, err := os.Stat(snap); err == nil {
				c.Process.Signal(os.Interrupt)
				<-done
				return exe, nil
			}
		}
	}
	c.Process.Kill()
	return "", fmt.Errorf("asuka dev didn't start the player:\n%s", out.String())
}

var (
	nativeMu      sync.Mutex
	nativePlayers = map[string]string{}
)

// NativePlayer builds, once per process, a player of its own from an app
// (src, its app.asuka) that imports packages with native code: their
// Swift is built into it, which Player's (an app that imports nothing
// native) hasn't. Its bundle ID is dev.asuka.mactest.<key>, its name "Mac
// Test <Key>". It returns this process's copy of its executable, for
// Options.Player.
func NativePlayer(t testing.TB, key, src string) string {
	t.Helper()
	Player(t) // its CLI
	nativeMu.Lock()
	defer nativeMu.Unlock()
	if exe, ok := nativePlayers[key]; ok {
		return exe
	}
	exe, err := buildNativePlayer(key, src)
	if err != nil {
		t.Fatalf("building the %s player: %v", key, err)
	}
	nativePlayers[key] = exe
	return exe
}

func buildNativePlayer(key, src string) (string, error) {
	dir := cachedir.Path("mactest")
	lock, err := os.OpenFile(filepath.Join(dir, ".lock-"+key), os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return "", err
	}
	defer lock.Close()
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX); err != nil {
		return "", err
	}
	name := "Mac Test " + strings.ToUpper(key[:1]) + key[1:]
	app := filepath.Join(dir, "mactest-"+key) // its work directory: <cache>/asuka/mactest-<key>-<hash>/mac
	if err := os.MkdirAll(app, 0o755); err != nil {
		return "", err
	}
	os.WriteFile(filepath.Join(app, "asuka.toml"), []byte(fmt.Sprintf("name = %q\nid = \"dev.asuka.mactest.%s\"\nversion = \"1.0.0\"\n", name, key)), 0o644)
	os.WriteFile(filepath.Join(app, "app.asuka"), []byte(src), 0o644)
	snap := filepath.Join(dir, key+"-ready.png")
	os.Remove(snap)
	started := time.Now()
	c := exec.Command(filepath.Join(dir, "asuka"), "dev", "--platform", "mac", "--once", app)
	c.Env = append(os.Environ(), "ASUKA_SNAPSHOT="+snap)
	var out bytes.Buffer
	c.Stdout, c.Stderr = &out, &out
	if err := c.Start(); err != nil {
		return "", err
	}
	done := make(chan error, 1)
	go func() { done <- c.Wait() }()
	exe := ""
	for deadline := time.Now().Add(15 * time.Minute); time.Now().Before(deadline); {
		if exe == "" {
			ms, _ := filepath.Glob(filepath.Join(cachedir.Root(), "mactest-"+key+"-*", "mac", "player", name+".app", "Contents", "MacOS", name))
			for _, m := range ms {
				if st, err := os.Stat(m); err == nil && !st.ModTime().Before(started) {
					exe = m
				}
			}
		}
		select {
		case err := <-done:
			if exe == "" {
				return "", fmt.Errorf("asuka dev: %v\n%s", err, out.String())
			}
			return ownCopy(exe)
		case <-time.After(200 * time.Millisecond):
		}
		if exe != "" {
			if _, err := os.Stat(snap); err == nil {
				c.Process.Signal(os.Interrupt)
				<-done
				return ownCopy(exe)
			}
		}
	}
	c.Process.Kill()
	return "", fmt.Errorf("asuka dev didn't start the %s player:\n%s", key, out.String())
}

// ownCopy copies exe's app bundle for this process, which runs its copy
// while another process's build may replace the built one.
func ownCopy(exe string) (string, error) {
	app := filepath.Dir(filepath.Dir(filepath.Dir(exe)))
	own, err := os.MkdirTemp("", "asuka-mactest-")
	if err != nil {
		return "", err
	}
	cp := filepath.Join(own, filepath.Base(app))
	if b, err := exec.Command("ditto", app, cp).CombinedOutput(); err != nil {
		return "", fmt.Errorf("copying the player: %v\n%s", err, b)
	}
	return filepath.Join(cp, "Contents", "MacOS", filepath.Base(exe)), nil
}

// App is a test's app, running on the player.
type App struct {
	t    testing.TB
	dir  string // its ASUKA_COMMANDS directory
	cmd  *exec.Cmd
	srv  *devserver.Server
	mu   sync.Mutex
	logs bytes.Buffer
	done chan struct{}
}

// Options say how an app runs.
type Options struct {
	// Env is added to the player's environment.
	Env []string
	// Files are the app's other sources, by name, beside app.asuka.
	Files map[string]string
	// Player is the player's executable to run instead of the one built
	// (a copy of Player's, signed as a test needs).
	Player string
}

// Run runs src (an Asuka file with its app { }) on the Mac player until
// the test ends.
func Run(t testing.TB, src string) *App { return RunWith(t, src, Options{}) }

// RunWith runs src with options.
func RunWith(t testing.TB, src string, o Options) *App {
	t.Helper()
	exe := o.Player
	if exe == "" {
		exe = Player(t)
	}
	files := map[string][]byte{"app.asuka": []byte(Dedent(src))}
	for k, v := range o.Files {
		files[k] = []byte(Dedent(v))
	}
	res := compile.Files(files, &compile.Options{Platform: target.MacOS})
	if err := res.Err(); err != nil {
		t.Fatalf("the app doesn't compile:\n%v", err)
	}
	a := &App{t: t, dir: t.TempDir(), done: make(chan struct{})}
	srv, err := devserver.Listen("127.0.0.1:0", devserver.Events{
		Log: func(p *devserver.Player, stderr bool, text []byte) {
			a.mu.Lock()
			a.logs.Write(text)
			a.mu.Unlock()
		},
		Error: func(p *devserver.Player, text []byte) {
			a.mu.Lock()
			fmt.Fprintf(&a.logs, "error: %s\n", text)
			a.mu.Unlock()
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	a.srv = srv
	srv.Send(target.MacOS, res.Module.Encode())
	data := filepath.Join(a.dir, "data")
	os.MkdirAll(data, 0o755)
	c := exec.Command(exe)
	c.Env = append(os.Environ(),
		fmt.Sprintf("ASUKA_DEV=127.0.0.1:%d", srv.Port()),
		"ASUKA_COMMANDS="+a.dir,
		"ASUKA_DATA="+data,
		"ASUKA_ASSETS="+filepath.Join(a.dir, "assets"),
	)
	c.Env = append(c.Env, o.Env...)
	c.Stdout, c.Stderr = a, a
	if err := c.Start(); err != nil {
		t.Fatal(err)
	}
	a.cmd = c
	go func() { c.Wait(); close(a.done) }()
	t.Cleanup(func() {
		a.Stop()
		srv.Close()
		if t.Failed() {
			t.Logf("the player's output:\n%s", a.Logs())
		}
	})
	// Ready once the app shows something.
	a.Until(func(tr *Tree) bool { return len(tr.Roots) > 0 })
	return a
}

// Write takes the player's output.
func (a *App) Write(p []byte) (int, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.logs.Write(p)
}

// Logs are the player's output and the app's logs.
func (a *App) Logs() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.logs.String()
}

// Stop quits the app (SIGINT, as asuka dev stops it), killed if it hasn't
// quit in a few seconds.
func (a *App) Stop() {
	select {
	case <-a.done:
		return
	default:
	}
	a.cmd.Process.Signal(os.Interrupt)
	select {
	case <-a.done:
	case <-time.After(5 * time.Second):
		a.cmd.Process.Kill()
		<-a.done
	}
}

// Exited reports whether the app's process has ended.
func (a *App) Exited() bool {
	select {
	case <-a.done:
		return true
	default:
		return false
	}
}

// Wait waits up to d for the app to quit, reporting whether it did.
func (a *App) Wait(d time.Duration) bool {
	select {
	case <-a.done:
		return true
	case <-time.After(d):
		return false
	}
}

// Do runs automation commands (the script's steps as "verb arg", a line
// each: tap Add, type hello, key cmd+k, menu Save, back, close, ...) and
// waits for them to be done. Their errors fail the test.
func (a *App) Do(cmds ...string) {
	a.t.Helper()
	if err := a.do(cmds...); err != "" {
		a.t.Fatalf("%s: %s", strings.Join(cmds, "; "), err)
	}
}

// Try runs commands as Do does, and returns their errors ("" for none).
func (a *App) Try(cmds ...string) string { return a.do(cmds...) }

func (a *App) do(cmds ...string) string {
	a.t.Helper()
	file := filepath.Join(a.dir, "asuka-commands.txt")
	doneFile := filepath.Join(a.dir, "asuka-done.txt")
	errFile := filepath.Join(a.dir, "asuka-errors.txt")
	// One the app hasn't read yet isn't written over.
	for i := 0; i < 600; i++ {
		if _, err := os.Stat(file); os.IsNotExist(err) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	os.Remove(doneFile)
	os.Remove(errFile)
	if err := os.WriteFile(file+".part", []byte(strings.Join(cmds, "\n")+"\n"), 0o644); err != nil {
		a.t.Fatal(err)
	}
	if err := os.Rename(file+".part", file); err != nil {
		a.t.Fatal(err)
	}
	deadline := time.Now().Add(time.Minute)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(doneFile); err == nil {
			b, _ := os.ReadFile(errFile)
			return strings.TrimSpace(string(b))
		}
		if a.Exited() {
			return "the app quit"
		}
		time.Sleep(20 * time.Millisecond)
	}
	a.t.Fatalf("the app didn't run %q in a minute", cmds)
	return ""
}

// Texts is what the screen shows: each text, field's text, button's
// title or accessibility label, a line each, in creation order.
func (a *App) Texts() []string {
	a.t.Helper()
	a.Do("dump")
	b, _ := os.ReadFile(filepath.Join(a.dir, "asuka-dump.txt"))
	s := strings.TrimSpace(string(b))
	if s == "" {
		return nil
	}
	return strings.Split(s, "\n")
}

// Dump is the host's view tree, as ASUKA_DUMP writes it.
func (a *App) Dump() string {
	a.t.Helper()
	a.Do("tree")
	b, _ := os.ReadFile(filepath.Join(a.dir, "asuka-tree.txt"))
	return string(b)
}

// Tree is the host's view tree, windows, menu bar and tray.
func (a *App) Tree() *Tree {
	a.t.Helper()
	a.Do("json")
	b, err := os.ReadFile(filepath.Join(a.dir, "asuka-tree.json"))
	if err != nil {
		a.t.Fatal(err)
	}
	var tr Tree
	if err := json.Unmarshal(b, &tr); err != nil {
		a.t.Fatalf("the tree: %v\n%s", err, b)
	}
	tr.raw = b
	return &tr
}

// Until waits (a minute at most) for f to hold of the tree, failing the
// test if it doesn't.
func (a *App) Until(f func(*Tree) bool) *Tree {
	a.t.Helper()
	deadline := time.Now().Add(time.Minute)
	var tr *Tree
	for time.Now().Before(deadline) {
		if a.Exited() {
			a.t.Fatalf("the app quit:\n%s", a.Logs())
		}
		tr = a.Tree()
		if f(tr) {
			return tr
		}
		time.Sleep(50 * time.Millisecond)
	}
	a.t.Fatalf("waited a minute; the tree is:\n%s", tr.raw)
	return nil
}

// UntilText waits for the screen to show text.
func (a *App) UntilText(text string) *Tree {
	a.t.Helper()
	return a.Until(func(t *Tree) bool { return t.FindText(text) != nil })
}

// Snap writes a snapshot of the window in use to path.
func (a *App) Snap(path string) { a.Do("snap " + path) }

// Dir is the app's commands directory (a temporary one of the test's).
func (a *App) Dir() string { return a.dir }

// Tree is what the host's automation writes as JSON.
type Tree struct {
	Roots       []*Node        `json:"roots"`
	Windows     []*Window      `json:"windows"`
	MenuBar     []*MenuItem    `json:"menuBar"`
	Environment map[string]any `json:"environment"`
	KeyWindow   string         `json:"keyWindow"`
	Tray        []any          `json:"tray"`
	DockMenu    []*MenuItem    `json:"dockMenu"`
	Extra       map[string]any `json:"-"`
	raw         []byte
}

// Raw is the tree as the host wrote it.
func (t *Tree) Raw() string { return string(t.raw) }

// Node is a node of the core's (or a list cell's): its kind, its view's
// class and frame, what it shows, and its view's state.
type Node struct {
	ID        uint32         `json:"id"`
	Kind      string         `json:"kind"`
	Class     string         `json:"class"`
	Frame     []float64      `json:"frame"`
	InWindow  []float64      `json:"inWindow"`
	Hidden    bool           `json:"hidden"`
	Shown     bool           `json:"shown"`
	Text      string         `json:"text"`
	Title     string         `json:"title"`
	NodeTitle string         `json:"nodeTitle"`
	Window    string         `json:"window"`
	Children  []*Node        `json:"children"`
	Row       int            `json:"row"`
	Props     map[string]any `json:"-"`
}

// UnmarshalJSON keeps every field in Props too.
func (n *Node) UnmarshalJSON(b []byte) error {
	type plain Node
	if err := json.Unmarshal(b, (*plain)(n)); err != nil {
		return err
	}
	return json.Unmarshal(b, &n.Props)
}

// Get is a field of the node's JSON ("stringValue", "state", "font", ...).
func (n *Node) Get(key string) any { return n.Props[key] }

// Num is a number field of the node's JSON (0 if it has none).
func (n *Node) Num(key string) float64 {
	f, _ := n.Props[key].(float64)
	return f
}

// Str is a string field of the node's JSON.
func (n *Node) Str(key string) string {
	s, _ := n.Props[key].(string)
	return s
}

// Bool is a bool field of the node's JSON.
func (n *Node) Bool(key string) bool {
	b, _ := n.Props[key].(bool)
	return b
}

// Window is one of the app's windows.
type Window struct {
	Title   string           `json:"title"`
	Visible bool             `json:"visible"`
	Key     bool             `json:"key"`
	Main    bool             `json:"main"`
	Frame   []float64        `json:"frame"`
	Content []float64        `json:"content"`
	Toolbar []map[string]any `json:"toolbar"`
	Props   map[string]any   `json:"-"`
}

func (w *Window) UnmarshalJSON(b []byte) error {
	type plain Window
	if err := json.Unmarshal(b, (*plain)(w)); err != nil {
		return err
	}
	return json.Unmarshal(b, &w.Props)
}

// MenuItem is an item of the menu bar, a menu or the Dock's.
type MenuItem struct {
	Title     string      `json:"title"`
	Separator bool        `json:"separator"`
	Key       string      `json:"key"`
	Mods      uint64      `json:"mods"`
	Disabled  bool        `json:"disabled"`
	Checked   bool        `json:"checked"`
	Hidden    bool        `json:"hidden"`
	Color     string      `json:"color"`
	Items     []*MenuItem `json:"items"`
}

// All is every node, depth first.
func (t *Tree) All() []*Node {
	var out []*Node
	var walk func(n *Node)
	walk = func(n *Node) {
		out = append(out, n)
		for _, c := range n.Children {
			walk(c)
		}
	}
	for _, r := range t.Roots {
		walk(r)
	}
	return out
}

// Kind is every node of a kind, depth first.
func (t *Tree) Kind(kind string) []*Node {
	var out []*Node
	for _, n := range t.All() {
		if n.Kind == kind {
			out = append(out, n)
		}
	}
	return out
}

// Find is the first node of a kind that shows text (its text, its title or
// its view's title), nil if none.
func (t *Tree) Find(kind, text string) *Node {
	for _, n := range t.All() {
		if n.Kind == kind && (n.Text == text || n.Title == text || n.NodeTitle == text) {
			return n
		}
	}
	return nil
}

// FindText is the first node that shows text, nil if none.
func (t *Tree) FindText(text string) *Node {
	for _, n := range t.All() {
		if n.Text == text || n.Title == text || n.NodeTitle == text || n.Str("a11yLabel") == text {
			return n
		}
	}
	return nil
}

// Window is the window titled title, nil if none.
func (t *Tree) Window(title string) *Window {
	for _, w := range t.Windows {
		if w.Title == title {
			return w
		}
	}
	return nil
}

// Main is the main window.
func (t *Tree) Main() *Window {
	for _, w := range t.Windows {
		if w.Main {
			return w
		}
	}
	return nil
}

// Menu is the menu bar's menu titled title, nil if none.
func (t *Tree) Menu(title string) *MenuItem {
	for _, m := range t.MenuBar {
		if m.Title == title {
			return m
		}
	}
	return nil
}

// Item is the item of m titled title (in its submenus too), nil if none.
func (m *MenuItem) Item(title string) *MenuItem {
	if m == nil {
		return nil
	}
	for _, it := range m.Items {
		if it.Title == title {
			return it
		}
		if s := it.Item(title); s != nil {
			return s
		}
	}
	return nil
}

// Dedent takes the indentation common to src's lines away, so a test's
// app can be written in a raw string indented with the test.
func Dedent(src string) string {
	lines := strings.Split(src, "\n")
	indent := -1
	for _, l := range lines {
		if strings.TrimSpace(l) == "" {
			continue
		}
		n := len(l) - len(strings.TrimLeft(l, " \t"))
		if indent < 0 || n < indent {
			indent = n
		}
	}
	for i, l := range lines {
		if len(l) >= indent && indent > 0 {
			lines[i] = l[indent:]
		}
	}
	return strings.Join(lines, "\n")
}
