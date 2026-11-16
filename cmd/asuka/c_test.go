package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
)

// testdata/capp calls its own C and the system's zlib (asuka.toml's
// [c]): asuka test builds them into asuka-run, and its UI test taps
// what calls C, which calls back, from another thread too.
func TestCAppTests(t *testing.T) {
	if _, err := cbuild.Find(); err != nil {
		t.Skip(err)
	}
	if err := cmdTest([]string{filepath.Join("testdata", "capp")}); err != nil {
		t.Fatal(err)
	}
}

// Each target reads its [c.<platform>]: zlib where the platform has it,
// the app's own CRC on the web and Windows; frameworks only on Apple's.
func TestCAppForeign(t *testing.T) {
	p, err := appbuild.Load(filepath.Join("testdata", "capp"))
	if err != nil {
		t.Fatal(err)
	}
	m, diags, err := p.Compile("")
	if err != nil || len(diags) > 0 {
		t.Fatal(err, diags)
	}
	if m.C == nil || !strings.Contains(string(m.C.Glue), `"ffi.main.checksum_lines"`) {
		t.Fatalf("no glue: %+v", m.C)
	}
	for _, c := range []struct {
		t      cbuild.Target
		link   []string
		define string
	}{
		{cbuild.Target{OS: "ios-simulator", Arch: "arm64"}, []string{"-lz"}, "-DCHECKSUM_SEED=7"},
		{cbuild.Target{OS: "android", Arch: "arm64"}, []string{"-lz"}, "-DCHECKSUM_SEED=7"},
		{cbuild.Target{OS: "wasm", Arch: "wasm32"}, nil, "-DCHECKSUM_OWN_CRC"},
		{cbuild.Target{OS: "windows", Arch: "amd64"}, nil, "-DCHECKSUM_OWN_CRC"},
	} {
		f, err := appbuild.Foreign(m.C, c.t)
		if err != nil {
			t.Fatal(err)
		}
		if !slices.Equal(f.Link, c.link) {
			t.Errorf("%s: links %q, want %q", c.t, f.Link, c.link)
		}
		u := f.Units[0]
		if len(u.Sources) != 1 || filepath.Base(u.Sources[0]) != "checksum.c" || !slices.Contains(u.Flags, c.define) {
			t.Errorf("%s: unit %+v", c.t, u)
		}
		if _, err := os.Stat(f.Glue); err != nil {
			t.Error(err)
		}
	}
}

// A vendor's library, prebuilt per target ([c.prebuilt]): the build for
// a target links that target's (a platform's universal one where the
// target has none of its own), asuka test calls it through the app's
// externs, and a target the table doesn't name doesn't build.
func TestCPrebuilt(t *testing.T) {
	cc, err := cbuild.Find()
	if err != nil {
		t.Skip(err)
	}
	ar, err := exec.LookPath("ar")
	if err != nil {
		t.Skip("no ar")
	}
	dir := t.TempDir()
	write := func(p, s string) {
		t.Helper()
		os.MkdirAll(filepath.Dir(filepath.Join(dir, p)), 0o755)
		if err := os.WriteFile(filepath.Join(dir, p), []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// The vendor's library, built here for this computer.
	host := cbuild.Host().CTarget()
	write("vendor/src/answer.c", "int vendor_answer(void) { return 42; }\n")
	obj := filepath.Join(dir, "vendor", "answer.o")
	if out, err := exec.Command(cc.Path, append(append([]string{}, cc.Args...), "-c", filepath.Join(dir, "vendor", "src", "answer.c"), "-o", obj)...).CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	lib := filepath.Join(dir, "vendor", host, "libanswer.a")
	os.MkdirAll(filepath.Dir(lib), 0o755)
	if out, err := exec.Command(ar, "rcs", lib, obj).CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	write("asuka.toml", `name = "vendored"
id = "dev.asuka.vendored"

[c.prebuilt]
`+host+` = "vendor/`+host+`/libanswer.a"
ios-simulator = ["vendor/sim/libanswer.a"]
`)
	write("vendor/sim/libanswer.a", "!<arch>\n")
	write("app.asuka", `extern fn vendor_answer() -> Int32

app { Text("{vendor_answer()}") }

test "the vendor's library answers" {
  expect(vendor_answer() == 42)
}
`)
	if err := cmdTest([]string{dir}); err != nil {
		t.Fatal(err)
	}
	p, err := appbuild.Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	m, diags, err := p.Compile("")
	if err != nil || len(diags) > 0 {
		t.Fatal(err, diags)
	}
	f, err := appbuild.Foreign(m.C, cbuild.Host())
	if err != nil || !slices.Contains(f.Link, lib) {
		t.Errorf("this computer's build links %v (%v), want %s", f.Link, err, lib)
	}
	f, err = appbuild.Foreign(m.C, cbuild.Target{OS: "ios-simulator", Arch: "arm64"})
	if want := filepath.Join(dir, "vendor", "sim", "libanswer.a"); err != nil || !slices.Contains(f.Link, want) {
		t.Errorf("the simulator's build links %v (%v), want the universal %s", f.Link, err, want)
	}
	if _, err := appbuild.Foreign(m.C, cbuild.Target{OS: "android", Arch: "arm64"}); err == nil || !strings.Contains(err.Error(), "[c.prebuilt] has no library for android-arm64") {
		t.Errorf("a target it has no library for: %v", err)
	}
}
