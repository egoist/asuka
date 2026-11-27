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

// An app compiles to the same program every time: its packages are
// checked in one order (their imports by path), not a map's, so their
// init, and their functions' and globals' numbers, don't change from one
// build to the next. Two builds of the same app made two executables
// once, and the AppImage's delta from one version to the next was most
// of the AppImage (e2e's TestUpdaterLinuxAppImage).
func TestReleaseModuleSameEachCompile(t *testing.T) {
	dir := t.TempDir()
	// Many imports, each with imports of its own: a map's order would
	// differ from one compile to the next almost always.
	src := ""
	for _, p := range []string{"std/time", "std/fs", "std/url", "std/json", "std/storage", "std/regex", "std/encoding", "std/markdown", "std/html", "std/store", "std/crypto", "platform/update"} {
		src += "import " + p + "\n"
	}
	src += "\napp { Text(\"a\") }\n"
	if err := os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	var first []byte
	for i := 0; i < 10; i++ {
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
		if first == nil {
			first = c
		} else if !bytes.Equal(c, first) {
			t.Fatalf("compile %d made another program than the first", i+1)
		}
	}
}
