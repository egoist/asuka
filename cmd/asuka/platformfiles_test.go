package main

import (
	"encoding/binary"
	"io"
	"net"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/devserver"
	"github.com/egoist/asuka/lang/bytecode"
)

// platformApp is an app whose label is its platform's file's: label_ios,
// label_android, label_desktop, label_web.
func platformApp(t *testing.T) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "plat")
	os.MkdirAll(dir, 0o755)
	files := map[string]string{
		"asuka.toml":          "name = \"Plat\"\nid = \"dev.asuka.plat\"\n",
		"app.asuka":           "app { Text(label()) }\n\ntest \"the label\" {\n  expect(label().hasPrefix(\"Linux\"), label())\n  expect(@platform == .linux)\n  mount(Text(label())).expect(label())\n}\n",
		"label_ios.asuka":     "fn label() -> string = \"iOS label\"\n",
		"label_android.asuka": "fn label() -> string = \"Android label\"\n",
		"label_macos.asuka":   "fn label() -> string = \"macOS label\"\n",
		"label_windows.asuka": "fn label() -> string = \"Windows label\"\n",
		"label_linux.asuka":   "fn label() -> string = \"Linux label\"\n",
		"label_web.asuka":     "fn label() -> string = \"Web label\"\n",
	}
	for n, s := range files {
		if err := os.WriteFile(filepath.Join(dir, n), []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

// asuka test --platform linux compiles the tests for Linux (its _linux
// files, @platform) and runs them on a fake host that says Linux; the
// default is iOS's.
func TestCmdTestPlatformFiles(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	dir := platformApp(t)
	if err := cmdTest([]string{"--platform", "linux", dir}); err != nil {
		t.Errorf("--platform linux: %v", err)
	}
	if err := cmdTest([]string{dir}); err == nil || err.Error() != "1 of 1 test failed" && err.Error() != "1 of 1 tests failed" {
		t.Errorf("iOS's: %v", err)
	}
}

// Two players on different platforms connected to one asuka dev each get
// their platform's module, at once and on a save.
func TestDevPlatforms(t *testing.T) {
	dir := platformApp(t)
	app := projectApp(t, dir, nil)
	app.platform = langPlatform(platformIOS)
	loop, err := newDevLoop(app, "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer loop.close()
	loop.out = io.Discard
	player := func(hello string) net.Conn {
		c, err := net.Dial("tcp", "127.0.0.1:"+strconv.Itoa(loop.port()))
		if err != nil {
			t.Fatal(err)
		}
		h := make([]byte, 5, 5+len(hello))
		binary.LittleEndian.PutUint32(h, uint32(len(hello)+1))
		h[4] = devserver.KindHello
		c.Write(append(h, hello...))
		return c
	}
	module := func(c net.Conn) *bytecode.Module {
		t.Helper()
		c.SetReadDeadline(time.Now().Add(60 * time.Second)) // a hang guard
		var h [5]byte
		if _, err := io.ReadFull(c, h[:]); err != nil {
			t.Fatal(err)
		}
		b := make([]byte, binary.LittleEndian.Uint32(h[:4])-1)
		if _, err := io.ReadFull(c, b); err != nil || h[4] != devserver.KindModule {
			t.Fatal(err, h[4])
		}
		m, err := bytecode.Decode(b)
		if err != nil {
			t.Fatal(err)
		}
		return m
	}
	label := func(m *bytecode.Module) string {
		for _, s := range m.Strings {
			if strings.Contains(s, " label") {
				return s
			}
		}
		return ""
	}
	ios := player(`{"platform":"ios","device":"simulator"}`)
	defer ios.Close()
	android := player(`{"platform":"android","device":"Pixel"}`)
	defer android.Close()
	mac := player(`{"platform":"darwin","device":"desktop"}`)
	defer mac.Close()
	for c, want := range map[net.Conn]string{ios: "iOS label", android: "Android label", mac: "macOS label"} {
		if got := label(module(c)); got != want {
			t.Errorf("got %q, want %q", got, want)
		}
	}
	// A save: each its own again.
	os.WriteFile(filepath.Join(dir, "label_android.asuka"), []byte("fn label() -> string = \"Android label 2\"\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "label_ios.asuka"), []byte("fn label() -> string = \"iOS label 2\"\n"), 0o644)
	if !loop.compile() {
		t.Fatal("didn't compile")
	}
	for c, want := range map[net.Conn]string{ios: "iOS label 2", android: "Android label 2", mac: "macOS label"} {
		m := module(c)
		if got := label(m); got != want || slices.Contains(m.Strings, "Web label") {
			t.Errorf("after the save: got %q, want %q", got, want)
		}
	}
}

// asuka check checks every platform's files; --platform one's.
func TestCmdCheckPlatforms(t *testing.T) {
	dir := platformApp(t)
	if err := cmdCheck([]string{dir}); err != nil {
		t.Errorf("every platform: %v", err)
	}
	os.WriteFile(filepath.Join(dir, "label_web.asuka"), []byte("fn label() -> string = 1\n"), 0o644)
	if err := cmdCheck([]string{dir}); err != errReported {
		t.Errorf("an error in the web's file: %v", err)
	}
	if err := cmdCheck([]string{"--platform", "ios", dir}); err != nil {
		t.Errorf("iOS's: %v", err)
	}
	if err := cmdCheck([]string{"-p", "web", dir}); err != errReported {
		t.Errorf("the web's: %v", err)
	}
}
