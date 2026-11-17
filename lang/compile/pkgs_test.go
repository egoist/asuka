package compile

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPackages(t *testing.T) {
	res, err := Dir("testdata/pkgs/app", nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	out, err := runBoth(t, res.Module)
	want := "#1 a\nHI!\n1\nok packages"
	if strings.TrimSpace(out) != want || err != nil {
		t.Fatalf("got (%v):\n%s\nwant:\n%s", err, out, want)
	}
}

func TestPackageErrors(t *testing.T) {
	res := Files(map[string][]byte{"a.asuka": []byte(`
import ./missing
import std/nope
`)}, nil)
	got := res.Err()
	if got == nil || !strings.Contains(got.Error(), "can't read package ./missing") || !strings.Contains(got.Error(), "no package std/nope") {
		t.Fatalf("got %v", got)
	}
	res2, err := Dir("testdata/pkgs/app", nil)
	if err != nil {
		t.Fatal(err)
	}
	_ = res2
}

// A project's imports: the standard library's packages (std/), and a
// dependency asuka.toml names, resolved and locked on the first build.
func TestProjectImports(t *testing.T) {
	root := t.TempDir()
	write := func(p, s string) {
		p = filepath.Join(root, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("charts/asuka.toml", "name = \"@acme/line-charts\"\nversion = \"1.0.0\"\n")
	write("charts/charts.asuka", "pub fn twice(n: int) -> int = n * 2\n")
	write("app/asuka.toml", "name = \"app\"\n\n[dependencies]\n\"@acme/line-charts\" = \"file:../charts\"\n")
	write("app/app.asuka", `import std/store
import @acme/line-charts as charts

test "imports" {
  print(store.read("units", "metric"))
  print(charts.twice(21))
}
`)
	res, err := Dir(filepath.Join(root, "app"), nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	if lock, err := os.ReadFile(filepath.Join(root, "app", "asuka.lock")); err != nil {
		t.Errorf("no asuka.lock: %v", err)
	} else if !strings.Contains(string(lock), `"file:../charts"`) {
		t.Errorf("asuka.lock should record the package from the project:\n%s", lock)
	}
	out, err := runBoth(t, res.Module)
	// No platform here: the store keeps nothing, and reads the default.
	if want := "metric\n42\nok imports"; strings.TrimSpace(out) != want || err != nil {
		t.Fatalf("got (%v):\n%s\nwant:\n%s", err, out, want)
	}

	res = Files(map[string][]byte{"a.asuka": []byte("import @acme/charts\n")}, nil)
	if got := res.Err(); got == nil || !strings.Contains(got.Error(), "can't import @acme/charts: a dependency needs a project") {
		t.Errorf("a dependency outside a project: %v", got)
	}
}

// The asuka/ prefix is retired: an import of it says what the package is
// now, the standard library's or the platform SDK's.
func TestRetiredPrefix(t *testing.T) {
	for imp, want := range map[string]string{
		"asuka/json":     "asuka/json is std/json now",
		"asuka/http":     "asuka/http is std/http now",
		"asuka/camera":   "asuka/camera is platform/camera now",
		"asuka/platform": "asuka/platform is platform/host now",
		"asuka/nope":     "no package asuka/nope: the asuka/ prefix is retired",
	} {
		res := Files(map[string][]byte{"a.asuka": []byte("import " + imp + "\n")}, nil)
		if got := res.Err(); got == nil || !strings.Contains(got.Error(), want) {
			t.Errorf("import %s: %v, want %q", imp, got, want)
		}
	}
	res := Files(map[string][]byte{"a.asuka": []byte("import std/json\nimport platform/device\n\nfn f() -> string = json.encode(1)\n")}, nil)
	if err := res.Err(); err != nil {
		t.Errorf("std/json and platform/device: %v", err)
	}
}

// A standard library package's native fn is the runtime's builtin of
// its package and name (here str.len, time.sleep); others can't have one.
func TestNativeFn(t *testing.T) {
	res := Files(map[string][]byte{"a.asuka": []byte(`import std/str
import std/time

test "native fns" {
  print(str.len("héllo"))
  await time.sleep(5ms)
  let f = str.len
  print(f("ab"))
}
`)}, &Options{Packages: map[string]map[string][]byte{
		"std/str":  {"str.asuka": []byte("pub native fn len(s: string) -> int\n")},
		"std/time": {"time.asuka": []byte("pub native async fn sleep(d: Duration)\n")},
	}})
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	out, err := runBoth(t, res.Module)
	if want := "5\n2\nok native fns"; strings.TrimSpace(out) != want || err != nil {
		t.Fatalf("got (%v):\n%s\nwant:\n%s", err, out, want)
	}

	res = Files(map[string][]byte{"a.asuka": []byte("native fn f() -> int\n")}, nil)
	if got := res.Err(); got == nil || !strings.Contains(got.Error(), "native fn is the standard library's") {
		t.Errorf("a native fn outside the standard library: %v", got)
	}
}

// A package's native component is one native view of the kind its
// platforms register ("<class>#<Name>", the class asuka.toml's name):
// its handlers first, then its props by their exported names (the
// caller's, else their defaults), its style on the view; its events'
// values come decoded.
func TestNativeComponents(t *testing.T) {
	root := t.TempDir()
	write := func(p, s string) {
		p = filepath.Join(root, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("app/gauge/asuka.toml", "name = \"@acme/gauge\"\nversion = \"1.0.0\"\n")
	write("app/gauge/gauge.asuka", `/// A gauge, the platform's.
pub native component Dial(items: [string]) {
  prop value: float = 0.5
  prop label: string = "Level"
  event changed(text: string)
}
`)
	write("app/app.asuka", `import ./gauge

component Panel {
  state level = 0.25
  state said = ""
  let dial = Ref()

  Column {
    gauge.Dial(["a", "b"]) {
      ref: dial
      value: level
      height: 40
      on changed(t) { said = t }
    }
    Text("Said: {said}")
    Button("More") { on press { level = 0.75 } }
  }
}

app { Panel() }

test "a native component" {
  let app = mount(Panel())
  let first = app.ops()
  expect(first.contains("Create 2 Native") && first.contains("SetProp 2 Type=\"@acme/gauge#Dial\""), first)
  // Items, Value and Label (by their exported names, hex), Value's 0.25.
  expect(first.contains("Listen 2 Native") && first.contains("Prop=050000004974656d73") && first.contains("Prop=0500000056616c75650508000000000000000000d03f") && first.contains("Prop=050000004c6162656c"), first)
  expect(first.contains("SetFrame 2 0,0 390x40"), first)
  app.emit("@acme/gauge#Dial", "Changed", "hi")
  app.expect("Said: hi")
  app.tap("More")
  let next = app.ops()
  expect(next.contains("SetProp 2 Prop=0500000056616c75650508000000000000000000e83f") && !next.contains("4c6162656c"), next)
}
`)
	res, err := Dir(filepath.Join(root, "app"), nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	out, err := runBoth(t, res.Module)
	if err != nil || !strings.Contains(out, "ok a native component") {
		t.Fatalf("got (%v):\n%s", err, out)
	}
}

// A package's pub style is another's as style pkg.name: its props, set
// where it's applied (later props win), its values read in its own
// package (a private constant of it, here); a style that isn't pub
// isn't.
func TestPackageStyles(t *testing.T) {
	theme := map[string][]byte{"theme.asuka": []byte(`let pad = 12

pub style card {
  padding: pad; radius: 8
}

style secret { opacity: 0.5 }

pub style tinted(tint: Color, on: bool) {
  style: [card, if on { glow(tint) }]
}

style glow(tint: Color) { borderColor: tint; border: 2 }
`)}
	res := Files(map[string][]byte{"a.asuka": []byte(`import ./theme

component Card {
  state lit = true
  View {
    style: [theme.card, theme.tinted(#00ff00, on: lit)]
    radius: 4
    Text("in a card")
  }
}

app { Card() }

test "a package's style" {
  let app = mount(Card())
  app.expect("in a card")
  print(app.ops())
}
`)}, &Options{Packages: map[string]map[string][]byte{"./theme": theme}})
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	out, err := runBoth(t, res.Module)
	if err != nil || !strings.Contains(out, "ok a package's style") {
		t.Fatalf("got (%v):\n%s", err, out)
	}
	// The padding is the layout's: the text sits inside it (and the
	// border's 2). A style with parameters from the package, whose own
	// condition applies another of its styles (not pub) with its
	// argument.
	for _, want := range []string{"SetFrame 3 14,14 ", "CornerRadius=4", "BorderColor=#200ff00ff", "BorderWidth=2"} {
		if !strings.Contains(out, want) {
			t.Errorf("the ops lack %q:\n%s", want, out)
		}
	}
	if strings.Contains(out, "CornerRadius=8") {
		t.Errorf("the style's radius won over the view's own:\n%s", out)
	}

	res = Files(map[string][]byte{"a.asuka": []byte("import ./theme\n\ncomponent C { View { style: theme.secret } }\n\napp { C() }\n")},
		&Options{Packages: map[string]map[string][]byte{"./theme": theme}})
	if got := res.Err(); got == nil || !strings.Contains(got.Error(), "style secret isn't pub in ./theme") {
		t.Errorf("a style that isn't pub: %v", got)
	}
}

// A package's variant that isn't pub is its own: another package's block
// of it is an error, and a style names only pub ones.
func TestPrivateVariants(t *testing.T) {
	kit := map[string][]byte{"kit.asuka": []byte(`pub component Box {
  state n = 0
  variant busy = n > 0
  pub variant shown = true
  View { on press { n += 1 } }
}
`)}
	res := Files(map[string][]byte{"a.asuka": []byte(`import ./kit

style s { busy { opacity: 0.5 } }

app {
  kit.Box() {
    busy { opacity: 0.5 }
    shown { opacity: 0.25 }
  }
}
`)}, &Options{Packages: map[string]map[string][]byte{"./kit": kit}})
	got := res.Err()
	if got == nil {
		t.Fatal("no error")
	}
	for _, want := range []string{"no component this package sees declares variant busy", "Box's variant busy isn't pub"} {
		if !strings.Contains(got.Error(), want) {
			t.Errorf("want %q in:\n%v", want, got)
		}
	}
	if strings.Contains(got.Error(), "shown") {
		t.Errorf("a pub variant is the caller's to style:\n%v", got)
	}
}
