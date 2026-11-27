package core_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
)

// TestCWatchOS builds the core, the fake host and every C test for an
// Apple Watch's arm64_32 (ILP32: aarch64's registers with 32-bit
// pointers, the one target where __aarch64__ comes without 64-bit
// pointers) with the core's -Werror, and links each test: the core is
// 32-bit clean there as on wasm32 and armv7. They can't run on this Mac;
// with ASUKA_WATCH_SIM naming a booted watch simulator, the watch
// simulator's build of them (arm64) runs there, on watchOS's libraries.
func TestCWatchOS(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("watchOS builds need Xcode")
	}
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode")
	}
	root, _ := filepath.Abs(".")
	targets := []cbuild.Target{{OS: "watchos", Arch: "arm64_32"}}
	sim := os.Getenv("ASUKA_WATCH_SIM")
	if sim != "" {
		targets = append(targets, cbuild.Target{OS: "watchos-simulator", Arch: "arm64"})
	}
	for _, tg := range targets {
		t.Run(tg.String(), func(t *testing.T) {
			tc, err := cbuild.ForTarget(tg)
			if err != nil {
				t.Skip(err)
			}
			srcs, err := cbuild.Sources(root, true)
			if err != nil {
				t.Fatal(err)
			}
			flags := append(append(append([]string{}, tc.Flags...), cbuild.Includes(root)...), "-O1", "-DAK_TEST=1")
			tmp := t.TempDir()
			objs, err := tc.CC.Compile(srcs, filepath.Join(tmp, "core"), flags)
			if err != nil {
				t.Fatal(err)
			}
			tests, _ := filepath.Glob(filepath.Join(root, "test", "*_test.c"))
			var exes []string
			for _, src := range tests {
				name := strings.TrimSuffix(filepath.Base(src), "_test.c")
				obj, err := tc.CC.Compile([]string{src}, filepath.Join(tmp, name), flags)
				if err != nil && onlyUnavailable(err.Error()) {
					// A test that forks or runs a process: watchOS has
					// neither (the core's own code doesn't call them there).
					t.Logf("%s: uses what watchOS hasn't, left out", name)
					continue
				}
				if err != nil {
					t.Fatalf("%s: %v", name, err)
				}
				exe := filepath.Join(tmp, name+"_test")
				if err := tc.CC.Link(append(obj, objs...), exe, append(append([]string{}, tc.Flags...), "-lm", "-framework", "Foundation", "-framework", "Security", "-framework", "Network")); err != nil {
					t.Fatalf("%s: %v", name, err)
				}
				exes = append(exes, exe)
			}
			if tg.OS != "watchos-simulator" {
				return
			}
			modules := compileModules(t, tmp)
			for _, exe := range exes {
				name := strings.TrimSuffix(filepath.Base(exe), "_test")
				t.Run(name, func(t *testing.T) {
					if why, ok := notOnWatch[name]; ok {
						t.Skip(why)
					}
					// The simulator's processes see this Mac's files; the
					// test runs in core/test, as TestC's do (AK_TEST_DIR: a
					// spawn starts nowhere in particular).
					cmd := exec.Command("xcrun", "simctl", "spawn", sim, exe)
					cmd.Env = append(os.Environ(), "SIMCTL_CHILD_ASUKA_ROOT="+filepath.Dir(root), "SIMCTL_CHILD_AK_TEST_MODULES="+modules,
						"SIMCTL_CHILD_AK_TEST_DIR="+filepath.Join(root, "test"))
					out, err := cmd.CombinedOutput()
					if err != nil {
						t.Fatalf("%v\n%s", err, out)
					}
				})
			}
		})
	}
}

// notOnWatch are the C tests whose expectations are a desktop's, which a
// watch simulator's run (not under ASan) doesn't meet: what they check
// is the same on the watch, tested on this Mac.
var notOnWatch = map[string]string{
	"ota": "frees the core's memory with the C library's free, which only ASan's build (the core on the C library's allocator) allows",
	"std": "expects a desktop's directories (~/Library's, the user's home) and child processes, which iOS and watchOS have not",
}

// onlyUnavailable reports whether a compile's errors are all of calls
// to what watchOS hasn't (fork, system, posix_spawn).
func onlyUnavailable(out string) bool {
	n := 0
	for _, l := range strings.Split(out, "\n") {
		if strings.Contains(l, " error: ") {
			if !strings.Contains(l, "not available on watchOS") {
				return false
			}
			n++
		}
	}
	return n > 0
}
