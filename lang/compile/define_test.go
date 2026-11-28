package compile

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// defineApp compiles src in a directory whose asuka.toml is manifest
// ("": none), with opts.
func defineApp(t *testing.T, manifest, src string, opts *Options) *Result {
	t.Helper()
	dir := t.TempDir()
	if manifest != "" {
		os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(manifest), 0o644)
	}
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(src), 0o644)
	res, err := Dir(dir, opts)
	if err != nil {
		t.Fatal(err)
	}
	return res
}

const defineManifest = `name = "Notes"
version = "1.0"

[defines]
API_URL = "https://api.example.com"
PORT = 8080
RATIO = 0.25
ANALYTICS = true

[variant.staging]
version = "1.0-staging"

[variant.staging.defines]
API_URL = "https://staging.example.com"
ANALYTICS = false
`

// @define is the build's value of its key: asuka.toml's [defines], a
// variant's over them, --define's over those, of its type; a default
// is the value when there's none, and says its type. The program gets it
// on the VM and compiled to C alike.
func TestDefine(t *testing.T) {
	src := `
let api = @define("API_URL")
let port = @define("PORT")
let ratio: float = @define("RATIO")
let wide: float = @define("PORT")
let missing = @define("MISSING", "fallback")
let neg = @define("NEG", -3)
let flag = @define("FLAG", false)

test "defines" {
  print(api)
  print(port + 1)
  print(ratio * 2.0)
  print(wide + 0.5)
  print(missing)
  print(neg)
  print(flag)
  if @define("ANALYTICS") { print("analytics on") } else { print("analytics off") }
}
`
	run := func(opts *Options) string {
		t.Helper()
		res := defineApp(t, defineManifest, src, opts)
		if err := res.Err(); err != nil {
			t.Fatal(err)
		}
		out, err := runBoth(t, res.Module)
		if err != nil {
			t.Fatalf("%v\n%s", err, out)
		}
		return out
	}
	out := run(nil)
	for _, want := range []string{"https://api.example.com\n", "8081\n", "0.5\n", "8080.5\n", "fallback\n", "-3\n", "false\n", "analytics on\n"} {
		if !strings.Contains(out, want) {
			t.Errorf("base: no %q in\n%s", want, out)
		}
	}
	out = run(&Options{Variant: "staging", Defines: map[string]string{"FLAG": "true", "PORT": "9000", "MISSING": "given"}})
	for _, want := range []string{"https://staging.example.com\n", "9001\n", "given\n", "true\n", "analytics off\n"} {
		if !strings.Contains(out, want) {
			t.Errorf("staging: no %q in\n%s", want, out)
		}
	}
	if strings.Contains(out, "analytics on") {
		t.Errorf("staging: analytics on:\n%s", out)
	}
}

// What a @define can't be: a key the build doesn't give without a
// default, a value of another type than the default's, text that isn't
// one, a key that isn't a literal or a name.
func TestDefineErrors(t *testing.T) {
	for _, c := range []struct {
		src     string
		defines map[string]string
		want    string
	}{
		{`let x = @define("NOPE")`, nil, "the build defines no NOPE"},
		{`let x = @define("API_URL", 3)`, nil, "the build's API_URL is a string, and its default says it's an int"},
		{`let x = @define("PORT", "x")`, nil, "the build's PORT is an int"},
		{`let x = @define("N", 3)`, map[string]string{"N": "three"}, `"three", not an int`},
		{`let x = @define("PORT")`, map[string]string{"PORT": "x"}, `asuka.toml's PORT is an integer, and "x" isn't`},
		{`let k = "API_URL"
let x = @define(k)`, nil, "a string literal"},
		{`let x = @define("API-URL")`, nil, "isn't one"},
		{`let x = @define`, nil, "@define takes the key"},
		{`let y = 2
let x = @define("Q", y)`, nil, "a literal"},
		{`let x = @debug("Q")`, nil, "takes no arguments"},
		{`let x = @defines`, nil, "@define(\"KEY\")"},
	} {
		res := defineApp(t, defineManifest, c.src+"\n", &Options{Defines: c.defines})
		err := res.Err()
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want %q", c.src, err, c.want)
		}
	}
	res := defineApp(t, "[defines]\nLIST = [1]\n", "let x = 1\n", nil)
	if err := res.Err(); err == nil || !strings.Contains(err.Error(), "LIST is an array") {
		t.Errorf("an array define: %v", err)
	}
	res = defineApp(t, defineManifest, "let x = 1\n", &Options{Variant: "prod"})
	if err := res.Err(); err == nil || !strings.Contains(err.Error(), "no [variant.prod]") {
		t.Errorf("an unknown variant: %v", err)
	}
}

// A bool @define folds as @debug does: the branch it doesn't take is no
// part of the module.
func TestDefineFolds(t *testing.T) {
	src := `
fn server() -> string {
  if @define("ANALYTICS") && !@define("OFFLINE", false) {
    return "taken branch"
  }
  return "other branch"
}

test "x" { print(server()) }
`
	m := defineApp(t, defineManifest, src, nil).Module
	if m == nil || !slices.Contains(m.Strings, "taken branch") || slices.Contains(m.Strings, "other branch") {
		t.Fatalf("ANALYTICS on: %v", m.Strings)
	}
	m = defineApp(t, defineManifest, src, &Options{Variant: "staging"}).Module
	if m == nil || slices.Contains(m.Strings, "taken branch") || !slices.Contains(m.Strings, "other branch") {
		t.Fatalf("ANALYTICS off: %v", m.Strings)
	}
}

// A variant's version reaches the module, as its defines do.
func TestVariantVersion(t *testing.T) {
	if v := projectVersion(writeManifest(t, defineManifest), "staging"); v != "1.0-staging" {
		t.Errorf("staging's version: %q", v)
	}
	if v := projectVersion(writeManifest(t, defineManifest), ""); v != "1.0" {
		t.Errorf("the version: %q", v)
	}
	acc, _, _ := appAccent(writeManifest(t, "accent = \"#000000\"\n[variant.b]\naccent = \"#ffffff\"\n"), "b")
	if acc != 2<<32|0xffffffff {
		t.Errorf("b's accent: %x", acc)
	}
}

// In a component's views, a bool @define's if is a build's: only its
// branch's views are in the module.
func TestDefineViews(t *testing.T) {
	src := `
component Home {
  Column {
    if @define("ANALYTICS") {
      Text("Tracking on")
    } else {
      Text("Tracking off")
    }
    Text(@define("API_URL"))
  }
}

app { Home() }
`
	res := defineApp(t, defineManifest, src, &Options{Variant: "staging"})
	m := res.Module
	if m == nil {
		t.Fatal(res.Err())
	}
	if slices.Contains(m.Strings, "Tracking on") || !slices.Contains(m.Strings, "Tracking off") || !slices.Contains(m.Strings, "https://staging.example.com") {
		t.Fatalf("staging: %v", m.Strings)
	}
}

func writeManifest(t *testing.T, s string) string {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "asuka.toml"), []byte(s), 0o644)
	return dir
}
