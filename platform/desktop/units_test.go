package desktop_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/internal/nativegen"
	"github.com/egoist/asuka/std"
)

// The std and platform packages' C unit tests: each package's
// desktop/*_test.c (a main of its own) built with the package's portable C (its files for
// both platforms; neither platform's own nor the generated glue, which
// calls them), the registry and test/unit.c's host, under ASan and UBSan, and run: it
// passes when it exits 0. What a package's C works out without a
// platform (parsing, decoding, encoding) is tested so on every system.
// A view's Linux test (desktop/*_linux_test.c) is built with the package's
// glue (and the shared C that fetches) too and runs on Linux, with GTK:
// it includes its view's C, makes the view and drives it as the host
// would.
func TestStdUnits(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("built with the POSIX test host's flags")
	}
	core, err := cbuild.CoreDir()
	if err != nil {
		t.Skip(err)
	}
	tests, err := std.Glob(filepath.Join("desktop", "*_test.c"))
	if err != nil {
		t.Fatal(err)
	}
	if len(tests) == 0 {
		t.Fatal("no C tests")
	}
	desk, _ := filepath.Abs(".")
	for _, test := range tests {
		dir := filepath.Dir(test)
		name := filepath.Base(filepath.Dir(dir)) + "/" + strings.TrimSuffix(filepath.Base(test), ".c")
		t.Run(name, func(t *testing.T) {
			// A view's test (_linux_test.c) runs on Linux, with GTK (under
			// Xvfb), its view's C its own (it includes it) and the package's
			// glue; a Windows one (_windows_test.c) is platform/windows'.
			view := strings.HasSuffix(test, "_linux_test.c")
			if strings.HasSuffix(test, "_windows_test.c") || view && runtime.GOOS != "linux" {
				t.Skip("another platform's")
			}
			srcs := []string{test, filepath.Join(desk, "test", "unit.c"), filepath.Join(desk, "src", "native.c"), filepath.Join(desk, "src", "json.c"),
				filepath.Join(desk, "src", "jsontree.c"), filepath.Join(desk, "src", "linux.c"),
				filepath.Join(desk, "src", "linux_syms.c")}
			for _, f := range nativegen.CSources(dir, "unit") {
				if !strings.HasSuffix(f, ".asuka.c") {
					srcs = append(srcs, filepath.Join(dir, f))
				}
			}
			if view {
				// The glue, and what a view's C may call to get its content
				// (asuka_desktop_local_file's downloads: fetch.c, tls.c).
				glue, _ := filepath.Glob(filepath.Join(dir, "*.asuka.c"))
				srcs = append(srcs, glue...)
				srcs = append(srcs, filepath.Join(desk, "src", "fetch.c"), filepath.Join(desk, "src", "tls.c"))
			}
			flags := append(append([]string{}, cbuild.Flags...), "-I"+filepath.Join(desk, "include"), "-I"+dir, "-D_GNU_SOURCE")
			work := t.TempDir()
			exe, err := cbuild.Exe(core, srcs, flags, nil, true, work, "unit")
			if err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command(exe)
			cmd.Dir = dir
			cmd.Env = append(os.Environ(), "ASUKA_DATA="+work, noLeaks)
			out, err := cmd.CombinedOutput()
			t.Logf("%s", out)
			if err != nil {
				t.Fatal(err)
			}
		})
	}
}
