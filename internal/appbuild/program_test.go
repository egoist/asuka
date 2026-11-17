package appbuild

import (
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
)

// A [bin] build's placeholders: Asuka's names, Rust's triple (the
// system's own on it, Zig's cross-built), Go's.
func TestBuildVars(t *testing.T) {
	mac := cbuild.Target{OS: "darwin", Arch: "arm64"}
	linux := cbuild.Target{OS: "linux", Arch: "amd64"}
	windows := cbuild.Target{OS: "windows", Arch: "amd64"}
	for _, c := range []struct {
		t, host cbuild.Target
		want    string // os arch exe target goos goarch
	}{
		{mac, mac, "macos arm64 - aarch64-apple-darwin darwin arm64"},
		{cbuild.Target{OS: "darwin", Arch: "amd64"}, mac, "macos x64 - x86_64-apple-darwin darwin amd64"},
		{linux, linux, "linux x64 - x86_64-unknown-linux-gnu linux amd64"},
		{cbuild.Target{OS: "linux", Arch: "arm64"}, mac, "linux arm64 - aarch64-unknown-linux-musl linux arm64"},
		{windows, windows, "windows x64 .exe x86_64-pc-windows-msvc windows amd64"},
		{cbuild.Target{OS: "windows", Arch: "arm64"}, windows, "windows arm64 .exe aarch64-pc-windows-msvc windows arm64"},
		{windows, mac, "windows x64 .exe x86_64-pc-windows-gnu windows amd64"},
		{cbuild.Target{OS: "windows", Arch: "arm64"}, linux, "windows arm64 .exe aarch64-pc-windows-gnullvm windows arm64"},
	} {
		v := BuildVars(c.t, c.host)
		exe := v["{exe}"]
		if exe == "" {
			exe = "-"
		}
		got := strings.Join([]string{v["{os}"], v["{arch}"], exe, v["{target}"], v["{goos}"], v["{goarch}"]}, " ")
		if got != c.want {
			t.Errorf("%s on %s: %q, want %q", c.t, c.host, got, c.want)
		}
	}
}

// A build's environment has the target for a script, the bin's env over it.
func TestBuildEnv(t *testing.T) {
	b := Bin{Name: "x", Build: []string{"sh", "build.sh", "{out}"}, Env: map[string]string{"ASUKA_OS": "mine", "CC": "{target}-cc"}}
	args, env := b.Command(cbuild.Target{OS: "linux", Arch: "arm64"}, "/o/x")
	if strings.Join(args, " ") != "sh build.sh /o/x" {
		t.Fatalf("args: %q", args)
	}
	got := strings.Join(env, " ")
	for _, w := range []string{"ASUKA_OUT=/o/x", "ASUKA_ARCH=arm64", "ASUKA_GOARCH=arm64", "ASUKA_EXE=", "CC=aarch64-unknown-linux-"} {
		if !strings.Contains(got, w) {
			t.Errorf("env %q: no %s", got, w)
		}
	}
	// exec.Cmd takes a variable's last value: env's.
	if i, j := strings.Index(got, "ASUKA_OS=linux"), strings.Index(got, "ASUKA_OS=mine"); i < 0 || j < i {
		t.Errorf("env %q: ASUKA_OS=mine isn't after linux", got)
	}
}
