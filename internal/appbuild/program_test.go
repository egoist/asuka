package appbuild

import (
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
)

// A [bin] build's placeholders: Asuka's names, Rust's triple, Go's.
func TestBuildVars(t *testing.T) {
	for _, c := range []struct {
		t                                   cbuild.Target
		os, arch, exe, target, goos, goarch string
	}{
		{cbuild.Target{OS: "darwin", Arch: "arm64"}, "macos", "arm64", "", "aarch64-apple-darwin", "darwin", "arm64"},
		{cbuild.Target{OS: "darwin", Arch: "amd64"}, "macos", "x64", "", "x86_64-apple-darwin", "darwin", "amd64"},
		{cbuild.Target{OS: "linux", Arch: "arm64"}, "linux", "arm64", "", "aarch64-unknown-linux-musl", "linux", "arm64"},
		{cbuild.Target{OS: "windows", Arch: "amd64"}, "windows", "x64", ".exe", "x86_64-pc-windows-gnu", "windows", "amd64"},
	} {
		v := BuildVars(c.t)
		got := []string{v["{os}"], v["{arch}"], v["{exe}"], v["{target}"], v["{goos}"], v["{goarch}"]}
		want := []string{c.os, c.arch, c.exe, c.target, c.goos, c.goarch}
		for i := range got {
			if got[i] != want[i] {
				t.Errorf("%s: %q, want %q", c.t, got, want)
				break
			}
		}
	}
}
