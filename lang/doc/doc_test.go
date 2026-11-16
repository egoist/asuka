package doc

import (
	"slices"
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
	"github.com/egoist/asuka/std"
)

// The documented packages the compiler provides declare what it does:
// every name, and none it doesn't.
func TestBuiltinMatchesCompiler(t *testing.T) {
	c := check.NewChecker(syntax.NewFileSet())
	paths := BuiltinPaths()
	for _, path := range []string{"std/json", "std/math", "std/embed"} {
		if !slices.Contains(paths, path) {
			t.Errorf("%s isn't documented", path)
		}
	}
	for _, path := range paths {
		if !check.ProvidesStd(path) {
			t.Errorf("%s is documented, but the compiler doesn't provide it", path)
			continue
		}
		p, ok := Builtin(path)
		if !ok {
			t.Fatalf("%s: no documentation", path)
		}
		var documented []string
		for _, d := range p.Decls {
			documented = append(documented, d.Name)
			if d.Doc == "" {
				t.Errorf("%s.%s has no ///", path, d.Name)
			}
		}
		declared := c.Std(path).Scope.Names()
		slices.Sort(documented)
		slices.Sort(declared)
		if !slices.Equal(documented, declared) {
			t.Errorf("%s: documented %v, the compiler declares %v", path, documented, declared)
		}
		if p.Doc == "" {
			t.Errorf("%s has no package comment", path)
		}
	}
}

// A page: the package's comment, its import, then its declarations by
// kind, a type's members after it; examples in comments are Asuka blocks.
func TestMarkdown(t *testing.T) {
	p, err := FromSources("std/clock", map[string][]byte{"clock.asuka": []byte(`// Package std/clock tells the time.
//
//   let t = clock.now()
//
// It's <T>-free.

/// Now.
pub fn now() -> Time = Time(ms: 0)

fn hidden() {}

/// A time.
pub struct Time {
  /// Milliseconds.
  ms: int

  /// Later by d.
  fn add(d: int) -> Time = Time(ms: ms + d)
}

pub component Clock(time: Time) {
  Text("now")
}
`)})
	if err != nil {
		t.Fatal(err)
	}
	got := p.Markdown()
	for _, want := range []string{
		"# std/clock\n\n`std/clock` tells the time.\n\n```asuka\nlet t = clock.now()\n```\n\nIt's \\<T>-free.\n",
		"```asuka\nimport std/clock\n```\n",
		"## Functions\n\n### now\n\n```asuka\nfn now() -> Time\n```\n\nNow.\n",
		"## Components\n\n### Clock\n\n```asuka\ncomponent Clock(time: Time)\n```\n",
		"## Types\n\n### Time\n\n```asuka\nstruct Time {\n  /// Milliseconds.\n  ms: int\n}\n```\n\nA time.\n\n#### Time.add\n\n```asuka\nfn add(d: int) -> Time\n```\n\nLater by d.\n",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("no\n%s\nin\n%s", want, got)
		}
	}
	if strings.Contains(got, "hidden") {
		t.Error("a declaration that isn't pub is documented")
	}
	if d := p.Lookup("Time.add"); d == nil || d.Doc != "Later by d." {
		t.Errorf("Lookup(Time.add): %+v", d)
	}
}

// Every package of the standard library and the platform SDK has its
// reference page: it reads, it has a package comment, and each of its
// pub declarations (and their members) has a /// comment.
func TestStdAndPlatformDocumented(t *testing.T) {
	ps, err := std.Packages()
	if err != nil {
		t.Fatal(err)
	}
	if len(ps) < 50 {
		t.Fatalf("only %d packages", len(ps))
	}
	for path, dir := range ps {
		p, err := Load(dir, path)
		if err != nil {
			t.Errorf("%s: %v", path, err)
			continue
		}
		if p.Doc == "" {
			t.Errorf("%s has no package comment (// Package %s …, above its first declaration)", path, path)
		}
		if !strings.HasPrefix(p.Markdown(), "# "+path+"\n") {
			t.Errorf("%s: its page doesn't start with its path", path)
		}
		for _, d := range p.Decls {
			if d.Doc == "" {
				t.Errorf("%s: %s %s has no ///", path, d.Kind, d.Name)
			}
			for _, m := range d.Members {
				if m.Doc == "" {
					t.Errorf("%s: %s %s has no ///", path, m.Kind, m.Name)
				}
			}
		}
	}
}

// A name each platform's file declares is documented once, saying the
// platforms when not every one has it.
func TestPlatformFiles(t *testing.T) {
	p, err := FromSources("std/tray", map[string][]byte{
		"tray.asuka":         []byte("// Package std/tray shows a tray.\n\n/// Everywhere.\npub fn everywhere() {}\n"),
		"tray_desktop.asuka": []byte("/// Shows the tray.\npub fn show() {}\n\n/// Desktops only.\npub fn desktopOnly() {}\n"),
		"tray_mobile.asuka":  []byte("pub fn show() {}\n"),
		"tray_web.asuka":     []byte("/// Shows the tray.\npub fn show() {}\n"),
	})
	if err != nil {
		t.Fatal(err)
	}
	got := p.Markdown()
	if strings.Count(got, "### show") != 1 || !strings.Contains(got, "### show\n\n```asuka\nfn show()\n```\n\nShows the tray.\n") {
		t.Errorf("show:\n%s", got)
	}
	if !strings.Contains(got, "### desktopOnly\n\n```asuka\nfn desktopOnly()\n```\n\nOnly on macOS, Windows and Linux.\n") {
		t.Errorf("desktopOnly:\n%s", got)
	}
	if strings.Contains(got, "### everywhere\n\n```asuka\nfn everywhere()\n```\n\nOnly") {
		t.Errorf("everywhere:\n%s", got)
	}
}
