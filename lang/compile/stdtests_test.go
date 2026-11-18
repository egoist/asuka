package compile

import (
	"path/filepath"
	"strings"
	"testing"

	"github.com/egoist/asuka/std"
)

// The standard library's packages' own test blocks (a _test.asuka file
// beside the package's code: std/term, std/decimal) pass, on the VM and
// compiled to C, as asuka test std/<name> runs them.
func TestStdPackageTests(t *testing.T) {
	ps, err := std.Packages()
	if err != nil {
		t.Fatal(err)
	}
	for p, dir := range ps {
		if !strings.HasPrefix(p, std.Prefix) {
			continue
		}
		if ms, _ := filepath.Glob(filepath.Join(dir, "*_test.asuka")); len(ms) == 0 {
			continue
		}
		t.Run(strings.TrimPrefix(p, std.Prefix), func(t *testing.T) {
			t.Parallel()
			res, err := Dir(dir, nil)
			if err != nil {
				t.Fatal(err)
			}
			if err := res.Err(); err != nil {
				t.Fatalf("compile:\n%v", err)
			}
			out, err := runBothWith(t, res.Module, nil)
			if err != nil || strings.Contains(out, "FAIL ") {
				t.Fatalf("%v\n%s", err, out)
			}
		})
	}
}
