package appbuild

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/egoist/asuka/lang/cgen"
	"github.com/egoist/asuka/lang/target"
)

// A release's module (and the C it compiles to) is the same wherever the
// app is: its functions' files are the app's own (app.asuka), not this
// computer's paths (reproducible builds, docs/shipping.md). A development
// build's keep their paths: asuka dev's editor links and the debugger
// open them.
func TestReleaseModuleNamesNoPaths(t *testing.T) {
	src := os.DirFS(filepath.Join("..", "..", "examples", "counter"))
	defer func(d bool) { Debug = d }(Debug)
	compileIn := func(debug bool) (string, []byte) {
		dir := filepath.Join(t.TempDir(), "counter")
		if err := os.CopyFS(dir, src); err != nil {
			t.Fatal(err)
		}
		Debug = debug
		p, err := Load(dir)
		if err != nil {
			t.Fatal(err)
		}
		m, diags, err := p.Compile(target.Linux)
		if err != nil || len(diags) > 0 {
			t.Fatal(err, diags)
		}
		c, err := cgen.Generate(m)
		if err != nil {
			t.Fatal(err)
		}
		return dir, c
	}
	dirA, a := compileIn(false)
	dirB, b := compileIn(false)
	if !bytes.Equal(a, b) {
		t.Fatal("the app compiled in two directories made two programs")
	}
	if bytes.Contains(a, []byte(dirA)) || bytes.Contains(b, []byte(dirB)) {
		t.Fatal("a release's C names the directory it was built in")
	}
	if !bytes.Contains(a, []byte(`"app.asuka"`)) {
		t.Fatal("a release's C doesn't name its functions' file as the app's own, app.asuka")
	}
	dirC, c := compileIn(true)
	if !bytes.Contains(c, []byte(filepath.Join(dirC, "app.asuka"))) {
		t.Fatal("a development build's C lost its functions' paths")
	}
}
