// The C core's tests. TestC builds each
// core/test/*_test.c with the whole core and the fake host, under
// AddressSanitizer and UndefinedBehaviorSanitizer, and runs it:
//
//	go test ./core              every C test
//	go test ./core -run Layout  core/test/layout_test.c alone
//	go test ./core -run TestC/Layout -update-known
//	                            its Chrome fixtures' known failures,
//	                            rewritten (core/test/testdata/css)
package core_test

import (
	"flag"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/compile"
)

var updateKnown = flag.Bool("update-known", false, "rewrite core/test/testdata/css/known_failures.txt (TestC/Layout)")

func TestC(t *testing.T) {
	cc, err := cbuild.Find()
	if err != nil {
		t.Skip(err)
	}
	root, _ := filepath.Abs(".")
	srcs, err := cbuild.Sources(root, true)
	if err != nil {
		t.Fatal(err)
	}
	flags := append(cbuild.Includes(root), "-g", "-O1", "-fno-omit-frame-pointer", "-DAK_TEST=1")
	if runtime.GOOS != "windows" {
		flags = append(flags, "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")
	}
	tmp := t.TempDir()
	objs, err := cc.Compile(srcs, filepath.Join(tmp, "core"), flags)
	if err != nil {
		t.Fatal(err)
	}
	modules := compileModules(t, tmp)
	tests, _ := filepath.Glob(filepath.Join(root, "test", "*_test.c"))
	for _, src := range tests {
		name := strings.TrimSuffix(filepath.Base(src), "_test.c")
		t.Run(strings.ToUpper(name[:1])+name[1:], func(t *testing.T) {
			obj, err := cc.Compile([]string{src}, filepath.Join(tmp, name), flags)
			if err != nil {
				t.Fatal(err)
			}
			exe := filepath.Join(tmp, name+"_test")
			link := []string{"-lm"}
			if runtime.GOOS != "windows" {
				link = append(link, "-fsanitize=address,undefined")
			}
			if err := cc.Link(append(obj, objs...), exe, link); err != nil {
				t.Fatal(err)
			}
			// Twice: as a release build's core, and with the run-time
			// diagnostics on, as a development build's (rfcs/0015): the
			// tests, the allocation counts among them, hold for both.
			for _, diag := range []bool{false, true} {
				cmd := exec.Command(exe)
				cmd.Dir = filepath.Join(root, "test")
				cmd.Env = append(os.Environ(), "ASUKA_ROOT="+filepath.Dir(root), "AK_TEST_MODULES="+modules)
				if diag {
					cmd.Env = append(cmd.Env, "AK_TEST_DIAG=1")
				}
				if testing.Verbose() {
					cmd.Env = append(cmd.Env, "T_VERBOSE=1")
				}
				if *updateKnown && !diag {
					cmd.Env = append(cmd.Env, "ASUKA_UPDATE_KNOWN=1")
				}
				out, err := cmd.CombinedOutput()
				if err != nil || testing.Verbose() {
					t.Logf("diagnostics %v:\n%s", diag, out)
				}
				if err != nil {
					t.Fatalf("diagnostics %v: %v", diag, err)
				}
			}
		})
	}
}

// compileModules compiles each core/test/testdata/modules/*.asuka, a
// package of one file, as asuka dev does (not optimized: its locals named
// for the debugger), into dir/modules/<name>.akb: the modules C tests
// load (AK_TEST_MODULES names the directory).
func compileModules(t *testing.T, dir string) string {
	out := filepath.Join(dir, "modules")
	if err := os.MkdirAll(out, 0o755); err != nil {
		t.Fatal(err)
	}
	srcs, _ := filepath.Glob(filepath.Join("test", "testdata", "modules", "*.asuka"))
	for _, p := range srcs {
		b, err := os.ReadFile(p)
		if err != nil {
			t.Fatal(err)
		}
		name := filepath.Base(p)
		res := compile.Files(map[string][]byte{name: b}, &compile.Options{NoOptimize: true, Debug: true, NoCatalogs: true, NoTests: true})
		if err := res.Err(); err != nil {
			t.Fatalf("%s: %v", p, err)
		}
		if err := os.WriteFile(filepath.Join(out, strings.TrimSuffix(name, ".asuka")+".akb"), res.Module.Encode(), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return out
}

// threadTests are the C tests that run threads of their own (the
// allocator's blocks freed across threads, workers, std/http's server,
// std/net, Argon2's lanes), which TestCThreads runs again under
// ThreadSanitizer.
var threadTests = []string{"alloc", "worker", "httpserver", "net", "tls", "password"}

// TestCThreads builds the core and each of threadTests under
// ThreadSanitizer (which ASan excludes) and runs them: the workers'
// threads and the main thread share nothing but what they hand over.
func TestCThreads(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("ThreadSanitizer: not with this toolchain on Windows")
	}
	cc, err := cbuild.Find()
	if err != nil {
		t.Skip(err)
	}
	root, _ := filepath.Abs(".")
	srcs, err := cbuild.Sources(root, true)
	if err != nil {
		t.Fatal(err)
	}
	flags := append(cbuild.Includes(root), "-g", "-O1", "-fno-omit-frame-pointer", "-DAK_TEST=1", "-fsanitize=thread")
	tmp := t.TempDir()
	objs, err := cc.Compile(srcs, filepath.Join(tmp, "core"), flags)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range threadTests {
		t.Run(strings.ToUpper(name[:1])+name[1:], func(t *testing.T) {
			obj, err := cc.Compile([]string{filepath.Join(root, "test", name+"_test.c")}, filepath.Join(tmp, name), flags)
			if err != nil {
				t.Fatal(err)
			}
			exe := filepath.Join(tmp, name+"_test")
			if err := cc.Link(append(obj, objs...), exe, []string{"-lm", "-fsanitize=thread"}); err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command(exe)
			cmd.Dir = filepath.Join(root, "test")
			cmd.Env = append(os.Environ(), "ASUKA_ROOT="+filepath.Dir(root), "TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1")
			if testing.Verbose() {
				cmd.Env = append(cmd.Env, "T_VERBOSE=1")
			}
			out, err := cmd.CombinedOutput()
			if err != nil || testing.Verbose() {
				t.Logf("%s", out)
			}
			if err != nil {
				t.Fatal(err)
			}
		})
	}
}
