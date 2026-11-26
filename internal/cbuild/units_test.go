package cbuild

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

// fakeCC is a compiler that only says its target (-dumpmachine).
func fakeCC(t *testing.T, machine string) Compiler {
	t.Helper()
	p := filepath.Join(t.TempDir(), "cc")
	os.WriteFile(p, []byte("#!/bin/sh\necho "+machine+"\n"), 0o755)
	return Compiler{Path: p}
}

// Which compilers link which family's prebuilt libraries: their own
// family's; on Linux, Zig's with any compiler for glibc, not musl's; on
// Windows, Zig's (MinGW's) only with Zig.
func TestPrebuiltLinks(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the fake compilers are shell scripts")
	}
	linux, windows := Target{OS: "linux", Arch: "amd64"}, Target{OS: "windows", Arch: "amd64"}
	gnu, clangGNU, musl := fakeCC(t, "x86_64-linux-gnu"), fakeCC(t, "x86_64-pc-linux-gnu"), fakeCC(t, "x86_64-alpine-linux-musl")
	msvc := fakeCC(t, "x86_64-pc-windows-msvc")
	zig := Compiler{Path: "/opt/zig/zig", Args: []string{"cc"}}
	for _, c := range []struct {
		target Target
		cc     Compiler
		family string
		want   bool
	}{
		{linux, zig, "zig", true},
		{linux, gnu, "zig", true},
		{linux, clangGNU, "zig", true},
		{linux, musl, "zig", false},
		{linux, gnu, "", false},
		{linux, gnu, "ndk", false},
		{windows, zig, "zig", true},
		{windows, msvc, "zig", false},
	} {
		tc := &Toolchain{Target: c.target, CC: c.cc}
		if got := tc.links(c.family); got != c.want {
			t.Errorf("%s with %s, %q's libraries: %v, want %v", c.target, c.cc.Path, c.family, got, c.want)
		}
	}
}

// Zig builds Linux's C against LinuxGlibc's glibc.
func TestZigLinuxGlibc(t *testing.T) {
	if _, err := Zig(); err != nil {
		t.Skip(err)
	}
	t.Setenv("ASUKA_CC", "zig")
	tc, err := ForTarget(Target{OS: "linux", Arch: "arm64"})
	if err != nil {
		t.Fatal(err)
	}
	want := "aarch64-linux-gnu." + LinuxGlibc
	for i, f := range tc.Flags {
		if f == "-target" && i+1 < len(tc.Flags) && tc.Flags[i+1] == want {
			return
		}
	}
	t.Errorf("flags %v: no -target %s", tc.Flags, want)
}

// The core's interpreter is its hot source: the dev configuration
// compiles it apart, optimized as a release's.
func TestCoreHot(t *testing.T) {
	core, err := filepath.Abs("../../core")
	if err != nil {
		t.Fatal(err)
	}
	u := CoreUnit(core)
	cold, hot := splitHot(u.Sources, u.Hot)
	if len(hot) != 1 || filepath.Base(hot[0]) != "interp.c" || filepath.Base(filepath.Dir(hot[0])) != "vm" {
		t.Fatalf("hot: %v", hot)
	}
	if len(cold) != len(u.Sources)-1 {
		t.Fatalf("%d cold of %d", len(cold), len(u.Sources))
	}
}
