package compile

import (
	"bytes"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/cgen"
)

// The conformance suite (testdata/run) on 32-bit pointers: each program's
// tests on the VM and compiled to C (lang/cgen), both in asuka-run built
// for WebAssembly (cbuild's Wasm32, with Zig) and run under Node
// (platform/web/testing/wasi.mjs), as a web build runs them, and the
// watch's arm64_32 would. There an ak_val is 8 bytes and a pointer 4, so
// C that steps through an array's values a pointer at a time (as a
// release's map over an array of structs once did: one element of
// three) prints what the 64-bit runs don't.
//
// What a web page has none of is left out (wasm32Skip): those programs
// and tests are the web's own, not 32-bit pointers'.

// wasm32Skip are the programs, and the tests of others (program/test),
// that need what a web page has none of, and why.
var wasm32Skip = map[string]string{
	"tls":                                 "a web page has no sockets",
	"net":                                 "a web page has no sockets",
	"httpserve":                           "a web page can't listen",
	"sqlite":                              "std/sqlite on the web is the page's SQLite worker (core's TestWebSQLite)",
	"keysessions/sessions resumed":        "a web page has no sockets",
	"ffi/kept callbacks":                  "a thread of the C's: WASI's C library has none",
	"os/stdin":                            "os.lines reads on a thread: the web has none",
	"process/the process and the machine": "a web page is no process: no host name, pid or user",
	"process/home: os.fake's HOME":        "a web page has no user's home",
	"files/files":                         "the web's files have no permissions",
	"fs/the user's directories":           "the web's directories are the page's",
	"fs/the app's directories":            "the web's directories are the page's",
}

// wasm32Drop is out without what the tests skipped of program printed:
// each one's lines, after the line that ended the test before it, to the
// line that ends it ("ok name", "FAIL name: ...").
func wasm32Drop(out, program string) string {
	lines := strings.Split(out, "\n")
	var keep, block []string
	for _, l := range lines {
		block = append(block, l)
		name, ok := strings.CutPrefix(l, "ok ")
		if !ok {
			if name, ok = strings.CutPrefix(l, "FAIL "); ok {
				name, _, _ = strings.Cut(name, ": ")
			}
		}
		if !ok {
			continue
		}
		if _, skip := wasm32Skip[program+"/"+name]; !skip {
			keep = append(keep, block...)
		}
		block = nil
	}
	return strings.Join(append(keep, block...), "\n")
}

var (
	wasmVMOnce sync.Once
	wasmVMExe  string
	wasmVMErr  error
)

// wasm32Tools are what running on wasm32 needs: Node, and Zig (cbuild's
// toolchain for the web); the test is skipped without them.
func wasm32Tools(t *testing.T) (node, script string) {
	t.Helper()
	if testing.Short() {
		t.Skip("WebAssembly under Node (not -short)")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("no Node")
	}
	if _, err := cbuild.ForTarget(cbuild.Target{OS: "wasm", Arch: "wasm32"}); err != nil {
		t.Skip(err)
	}
	script, _ = filepath.Abs(filepath.Join("..", "..", "platform", "web", "testing", "wasi.mjs"))
	return node, script
}

// runWasm32 runs asuka-run.wasm exe with args under Node: what it
// printed (stdout and stderr) and how it ended.
func runWasm32(node, script, exe string, args ...string) (string, error) {
	cmd := exec.Command(node, append([]string{"--no-warnings", "--stack-size=4000", script, exe}, args...)...)
	var out bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &out
	err := cmd.Run()
	return out.String(), err
}

// wasm32VM runs m's tests on the VM in asuka-run.wasm (built once; with
// the C m's externs call, f, built for it).
func wasm32VM(t *testing.T, m *bc.Module, f *cbuild.Foreign) (string, error) {
	t.Helper()
	node, script := wasm32Tools(t)
	wasmVMOnce.Do(func() {
		core, err := FindCore()
		if err != nil {
			wasmVMErr = err
			return
		}
		wasmVMExe, wasmVMErr = cbuild.RunnerFor(core, cbuild.RunnerOptions{Wasm32: true})
	})
	if wasmVMErr != nil {
		t.Fatal(wasmVMErr)
	}
	exe := wasmVMExe
	if f != nil {
		core, err := FindCore()
		if err != nil {
			t.Fatal(err)
		}
		if exe, err = cbuild.RunnerFor(core, cbuild.RunnerOptions{Foreign: f, Dir: t.TempDir(), Wasm32: true}); err != nil {
			t.Fatal(err)
		}
	}
	p := filepath.Join(t.TempDir(), "module.akb")
	if err := os.WriteFile(p, m.Encode(), 0o644); err != nil {
		t.Fatal(err)
	}
	return runWasm32(node, script, exe, "-test", p)
}

// wasm32C runs m's tests compiled to C (lang/cgen), built into
// asuka-run.wasm as a release's app is into the web's core.
func wasm32C(t *testing.T, m *bc.Module, f *cbuild.Foreign) (string, error) {
	t.Helper()
	node, script := wasm32Tools(t)
	core, err := FindCore()
	if err != nil {
		t.Fatal(err)
	}
	src, err := cgen.Generate(m)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	app := filepath.Join(dir, "app.c")
	if err := os.WriteFile(app, src, 0o644); err != nil {
		t.Fatal(err)
	}
	exe, err := cbuild.RunnerFor(core, cbuild.RunnerOptions{Extra: []string{app}, Foreign: f, Dir: dir, Builtins: m.Builtins, Wasm32: true})
	if err != nil {
		t.Fatal("the C the module compiles to doesn't build for wasm32: " + err.Error())
	}
	return runWasm32(node, script, exe, "-test", "-aot")
}

func TestRunWasm32(t *testing.T) {
	files, _ := filepath.Glob("testdata/run/*.asuka")
	if len(files) == 0 {
		t.Fatal("no programs")
	}
	wasm32Tools(t)
	for _, path := range files {
		path := path
		name := strings.TrimSuffix(filepath.Base(path), ".asuka")
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			if why, ok := wasm32Skip[name]; ok {
				t.Skip(why)
			}
			src, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if strings.HasPrefix(string(src), "// skip:") {
				t.Skip(strings.SplitN(string(src), "\n", 2)[0])
			}
			want, _ := wantOutput(string(src))
			want = wasm32Drop(want, name)
			res := Files(map[string][]byte{path: src}, nil)
			if err := res.Err(); err != nil {
				t.Fatalf("compile:\n%v", err)
			}
			var f *cbuild.Foreign
			if c, _ := filepath.Abs(strings.TrimSuffix(path, ".asuka") + ".c"); fileExists(c) {
				if f, err = Foreign(res.Module, []cbuild.CUnit{{Name: name, Sources: []string{c}}}, nil); err != nil {
					t.Fatal(err)
				}
			}
			for _, run := range []struct {
				name string
				run  func() (string, error)
			}{
				{"VM", func() (string, error) { return wasm32VM(t, res.Module, f) }},
				{"C", func() (string, error) { return wasm32C(t, res.Module, f) }},
			} {
				out, err := run.run()
				var exit *exec.ExitError
				if err != nil && !errors.As(err, &exit) {
					t.Fatalf("%s on wasm32: %v\n%s", run.name, err, out)
				}
				if got := wasm32Drop(strings.TrimSpace(out), name); got != want {
					t.Fatalf("%s on wasm32: got:\n%s\nwant:\n%s", run.name, got, want)
				}
				// It fails when a test does, one left out too.
				if failed := regexp.MustCompile(`(?m)^FAIL `).MatchString(out); failed != (err != nil) {
					t.Fatalf("%s on wasm32: the runner's exit: %v, a test failed: %v", run.name, err, failed)
				}
			}
		})
	}
}
