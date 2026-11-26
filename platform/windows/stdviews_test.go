package windows_test

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/platform/desktop"
	"github.com/egoist/asuka/std"
)

// TestStdViews runs the std and platform packages' view tests on Windows:
// each package's desktop/*_windows_test.c (a main of its own, which includes
// its view's C, makes the view and drives it as the host would), built
// with the package's Windows C (but that file), its glue, the C the
// desktops share and the unit tests' host (platform/desktop/test/unit.c),
// and run in the signed-in session: it passes when it exits 0.
func TestStdViews(t *testing.T) {
	w := onWindows(t)
	zig(t)
	tests, err := std.Glob(filepath.Join("desktop", "*_windows_test.c"))
	if err != nil {
		t.Fatal(err)
	}
	if len(tests) == 0 {
		t.Skip("no package has Windows view tests ({std,sdk}/*/desktop/*_windows_test.c)")
	}
	desk, err := desktop.Dir()
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range tests {
		dir := filepath.Dir(test)
		pkg := filepath.Dir(dir)
		name := filepath.Base(pkg) + "/" + strings.TrimSuffix(filepath.Base(test), ".c")
		t.Run(name, func(t *testing.T) {
			natives, err := appbuild.DesktopNatives(target(w), []string{pkg})
			if err != nil {
				t.Fatal(err)
			}
			view := filepath.Join(dir, strings.TrimSuffix(filepath.Base(test), "_test.c")+".c") // the test includes it
			natives.Sources = slices.DeleteFunc(natives.Sources, func(s string) bool { return s == view })
			// And from its package's library (an sdk or std package's C is
			// one): the test's copy is the only one.
			for i := range natives.Units {
				natives.Units[i].Sources = slices.DeleteFunc(slices.Clone(natives.Units[i].Sources), func(s string) bool { return s == view })
			}
			host := &appbuild.DesktopC{
				Sources: []string{test, filepath.Join(desk, "test", "unit.c"), filepath.Join(desk, "src", "jsontree.c")},
				Flags:   []string{"-I" + filepath.Join(desk, "include"), "-I" + dir},
			}
			// It runs in a desktop/ of its own, its package's test data (tests/,
			// if it has any) beside it.
			local, guest := w.job(t)
			if err := appbuild.DesktopExe(target(w), nil, host, natives, filepath.Join(local, "desktop", "view_test.exe")); err != nil {
				t.Fatal(err)
			}
			if _, err := os.Stat(filepath.Join(pkg, "tests")); err == nil {
				copyTree(t, filepath.Join(pkg, "tests"), filepath.Join(local, "tests"))
			}
			out, err := w.run(t, guest+`\desktop`, guest+`\desktop\view_test.exe`, nil)
			t.Logf("%s", out)
			if err != nil {
				t.Fatal(err)
			}
		})
	}
}
