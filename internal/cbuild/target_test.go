package cbuild

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

func coreRoot(t *testing.T) string {
	p, _ := filepath.Abs(filepath.Join("..", "..", "core"))
	return p
}

func build(t *testing.T, tg Target, kind LibKind, name string) {
	t.Helper()
	tc, err := ForTarget(tg)
	if err != nil {
		t.Skip(err)
	}
	out := filepath.Join(t.TempDir(), name)
	if err := tc.Lib(coreRoot(t), kind, nil, false, out); err != nil {
		t.Fatal(err)
	}
}

func TestLibHost(t *testing.T) {
	name := map[string]string{"darwin": "libasuka.dylib", "windows": "asuka.dll"}[runtime.GOOS]
	if name == "" {
		name = "libasuka.so"
	}
	build(t, Host(), Shared, name)
	build(t, Host(), Static, "libasuka.a")
}

func TestLibIOSSimulator(t *testing.T) {
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode")
	}
	build(t, Target{"ios-simulator", runtime.GOARCH}, Static, "libasuka.a")
	build(t, Target{"ios", "arm64"}, Static, "libasuka.a")
}

func TestLibTVOS(t *testing.T) {
	if _, err := exec.LookPath("xcrun"); err != nil {
		t.Skip("no Xcode")
	}
	build(t, Target{"tvos-simulator", runtime.GOARCH}, Static, "libasuka.a")
	build(t, Target{"tvos", "arm64"}, Static, "libasuka.a")
}

func TestLibAndroid(t *testing.T) {
	build(t, Target{"android", "arm64"}, Shared, "libasuka.so")
}

func TestLibWasm(t *testing.T) {
	if _, err := os.Stat(filepath.Join(coreRoot(t), "src", "abi", "abi.c")); err != nil {
		t.Skip("the C ABI (core/src/abi/abi.c) isn't there yet")
	}
	build(t, Target{"wasm", "wasm32"}, Module, "asuka.wasm")
}

func TestCoreDirExtract(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	t.Setenv("HOME", t.TempDir())
	d, err := extract()
	if err != nil {
		t.Fatal(err)
	}
	if !isCore(d) {
		t.Fatalf("%s isn't the core", d)
	}
	srcs, _ := Sources(d, true)
	if len(srcs) < 10 {
		t.Fatalf("only %d sources in %s", len(srcs), d)
	}
}

// Each target's [c] tables: its platform's, and its key in [c.prebuilt].
func TestCTarget(t *testing.T) {
	for _, c := range []struct {
		t                 Target
		platform, ctarget string
	}{
		{Target{OS: "ios", Arch: "arm64"}, "ios", "ios-arm64"},
		{Target{OS: "ios-simulator", Arch: "amd64"}, "ios", "ios-simulator-x64"},
		{Target{OS: "tvos", Arch: "arm64"}, "ios", "tvos-arm64"},
		{Target{OS: "tvos-simulator", Arch: "arm64"}, "ios", "tvos-simulator-arm64"},
		{Target{OS: "darwin", Arch: "arm64"}, "macos", "macos-arm64"},
		{Target{OS: "android", Arch: "arm"}, "android", "android-arm"},
		{Target{OS: "linux", Arch: "amd64"}, "linux", "linux-x64"},
		{Target{OS: "windows", Arch: "arm64"}, "windows", "windows-arm64"},
		{Target{OS: "wasm", Arch: "wasm32"}, "web", "web"},
	} {
		if c.t.CPlatform() != c.platform || c.t.CTarget() != c.ctarget {
			t.Errorf("%s: %s %s, want %s %s", c.t, c.t.CPlatform(), c.t.CTarget(), c.platform, c.ctarget)
		}
	}
}
