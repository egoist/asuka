package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/appbuild/apptest"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/target"
	"github.com/egoist/asuka/std"
)

// useCounter makes appbuild.Compile compile any app to the
// hand-assembled Counter whose Text says label and the count (the
// compiler isn't in this build), until the test ends.
func useCounter(t *testing.T, label string) {
	t.Helper()
	old := appbuild.Compile
	appbuild.Compile = func(dir string, deps map[string]string, _ target.Platform) (*bytecode.Module, *appbuild.Uses, []string, error) {
		return apptest.Counter(label), nil, nil, nil
	}
	t.Cleanup(func() { appbuild.Compile = old })
}

// useSQLiteCounter is useCounter for an app that imports std/sqlite
// (sqliteTestApp): the module says so (its C's Std), as the compiler's
// does, so a build links SQLite.
func useSQLiteCounter(t *testing.T, label string) {
	t.Helper()
	dir, ok := std.Package("std/sqlite")
	if !ok {
		t.Fatal("no std/sqlite")
	}
	old := appbuild.Compile
	appbuild.Compile = func(string, map[string]string, target.Platform) (*bytecode.Module, *appbuild.Uses, []string, error) {
		m := apptest.Counter(label)
		m.C = &bytecode.C{Std: []string{dir}}
		return m, nil, nil, nil
	}
	t.Cleanup(func() { appbuild.Compile = old })
}

// projectApp is the app in dir (its asuka.toml and .asuka files), with
// deps (package ID → directory), as loadApp reads one.
func projectApp(t *testing.T, dir string, deps map[string]string) *asukaApp {
	t.Helper()
	app, err := projectAppErr(dir, deps)
	if err != nil {
		t.Fatal(err)
	}
	return app
}

func projectAppErr(dir string, deps map[string]string) (*asukaApp, error) {
	dir, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	cfg, err := loadConfig(dir)
	if err != nil {
		return nil, err
	}
	if deps == nil {
		deps = map[string]string{}
	}
	app := &asukaApp{dir: dir, name: filepath.Base(dir), bundleID: cfg.ID, cfg: cfg, proj: &appbuild.Project{Dir: dir, Deps: deps}}
	app.refresh()
	return app, nil
}

// sqliteDeps are testdata/sqliteapp's dependencies: this checkout's
// std/sqlite.
func sqliteDeps() map[string]string {
	d, _ := filepath.Abs(filepath.Join("..", "..", "std", "sqlite"))
	return map[string]string{"std/sqlite": d}
}

// sqliteTestApp is testdata/sqliteapp, an app that uses std/sqlite.
func sqliteTestApp(t *testing.T) *asukaApp {
	t.Helper()
	return projectApp(t, filepath.Join("testdata", "sqliteapp"), sqliteDeps())
}

// checkSQLiteLinked fails t unless the binary at path has SQLite linked
// in (its databases' header).
func checkSQLiteLinked(t *testing.T, path string) {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(b, []byte("SQLite format 3\x00")) {
		t.Errorf("%s: no SQLite linked in", path)
	}
}

// needDesktopBuild skips a test that builds a desktop app for t when this
// computer can't: the core needs a C compiler for t (Zig to cross-compile),
// the host the go command (the CLI runs from its checkout).
func needDesktopBuild(t *testing.T, target cbuild.Target) {
	t.Helper()
	if testing.Short() {
		t.Skip("builds an app (not -short)")
	}
	if _, err := cbuild.ForTarget(target); err != nil {
		t.Skip(err)
	}
	if _, err := exec.LookPath("go"); err != nil || appbuild.Checkout() == "" {
		t.Skip("the desktop host is built from the checkout with go")
	}
}
