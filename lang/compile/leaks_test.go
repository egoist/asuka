package compile

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// testdata/leaks/*.asuka, the conformance programs that recurse through
// local fns, and the one whose values nest a million deep, run with the
// runner's -leaks on the VM and compiled
// to C: a test fails when objects it made outlive it. Each says what it
// prints after "// Output:", the failures it expects included (a
// reference cycle the checker warns of).
func TestLeaks(t *testing.T) {
	files, _ := filepath.Glob("testdata/leaks/*.asuka")
	files = append(files, "testdata/run/localfns.asuka", "testdata/run/closures.asuka", "testdata/run/exits.asuka", "testdata/run/deepvalues.asuka", "testdata/run/iterate.asuka", "testdata/run/extend.asuka", "testdata/run/paths.asuka", "testdata/run/pathfor.asuka", "testdata/run/pathmethods.asuka", "testdata/run/pathinbox.asuka", "testdata/run/optimistic.asuka")
	runner := testRunner(t)
	for _, path := range files {
		path := path
		t.Run(strings.TrimSuffix(filepath.Base(path), ".asuka"), func(t *testing.T) {
			t.Parallel()
			src, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			want, ok := wantOutput(string(src))
			if !ok {
				t.Fatal("no // Output: block")
			}
			res := Files(map[string][]byte{path: src}, nil)
			if err := res.Err(); err != nil {
				t.Fatalf("compile:\n%v", err)
			}
			out, err := Run(runner, res.Module, true, "", "-leaks")
			cout, cerr := RunCWith(res.Module, nil, true, true, "", "-leaks")
			if _, ok := cerr.(*exec.ExitError); cerr != nil && !ok {
				t.Fatalf("the C backend: %v\n%s", cerr, cout)
			}
			if cout != out || (cerr == nil) != (err == nil) {
				t.Fatalf("the VM and the compiled C disagree:\nVM (%v):\n%s\nC (%v):\n%s", err, out, cerr, cout)
			}
			if got := strings.TrimSpace(out); got != want {
				t.Fatalf("got:\n%s\nwant:\n%s", got, want)
			}
		})
	}
}
