package compile

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// writeTree writes files (slash paths → contents) into a new directory.
func writeTree(t *testing.T, files map[string]string) string {
	t.Helper()
	dir := t.TempDir()
	for p, s := range files {
		f := filepath.Join(dir, filepath.FromSlash(p))
		os.MkdirAll(filepath.Dir(f), 0o755)
		if err := os.WriteFile(f, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

// std/embed: a package's files, read as it compiles, on the VM and
// compiled to C.
func TestEmbed(t *testing.T) {
	dir := writeTree(t, map[string]string{
		"asuka.toml":         "name = \"embedtest\"\n",
		"README.md":          "# Hello\n",
		"art/logo.bin":       "\x00\x01\x02\xff",
		"config.json":        `{"api": "https://example.com", "retries": 3, "mode": "fast", "extra": [1, {"x": null}]}`,
		"posts/b.md":         "B",
		"posts/a.md":         "A",
		"posts/.draft.md":    "hidden",
		"posts/old/c.md":     "C",
		"icons/x/y.svg":      "<svg/>",
		"icons/z.svg":        "<svg z/>",
		"icons/.cache/q.svg": "hidden",
		"app.asuka": `import std/embed

enum Mode { slow, fast }
struct Config { api: string; retries: int; mode: Mode; timeout: float }

let readme = embed.text("README.md")
let logo = embed.bytes("art/logo.bin")
let config: Config = embed.json("config.json")
let posts = embed.texts("posts/*.md")
let icons = embed.files("icons/**/*.svg")

test "embeds" {
  expect(readme == "# Hello\n")
  expect(logo.count() == 4)
  expect(logo[3] == 255)
  expect(config.api == "https://example.com")
  expect(config.retries == 3)
  expect(config.mode == .fast)
  expect(config.timeout == 0.0)
  expect(embed.json<Config>("config.json").retries == 3)
  expect(posts.keys() == ["posts/a.md", "posts/b.md"])
  expect(posts["posts/a.md"] == "A")
  expect(icons.keys() == ["icons/x/y.svg", "icons/z.svg"])
  expect(icons["icons/z.svg"]?.count() == 8)
}
`,
	})
	res, err := Dir(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	out, err := runBoth(t, res.Module)
	if err != nil || strings.Contains(out, "FAIL") {
		t.Fatalf("%v\n%s", err, out)
	}
}

// What embed says when a path or a file is wrong, as the app compiles.
func TestEmbedErrors(t *testing.T) {
	for _, c := range []struct{ src, want string }{
		{`let a = embed.text("nope.md")`, "there's no nope.md in the package"},
		{`let a = embed.text("../x.md")`, "isn't in the package"},
		{`let a = embed.text("/etc/hosts")`, "isn't in the package"},
		{`let p = "a.md"
let a = embed.text(p)`, "its path is a string literal"},
		{`let a = embed.texts("posts/*.txt")`, "no file in the package matches it"},
		{`struct C { n: int }
let a: C = embed.json("bad.json")`, "embed.json doesn't fit a C: bad.json:1:10: expected an integer"},
		{`struct C { s: S }
struct S { x: int }
let a: C = embed.json("empty.json")`, "a missing field s"},
		{`let a = embed.json("empty.json")`, "embed.json needs to know what it reads"},
		{`let a: fn() -> int = embed.json("empty.json")`, "json can't read"},
		{`let a = embed.text<int>("a.md")`, "takes no type"},
	} {
		dir := writeTree(t, map[string]string{
			"a.md":       "a",
			"bad.json":   `{"n": 1.5}`,
			"empty.json": `{}`,
			"posts/x.md": "x",
			"app.asuka":  "import std/embed\n\n" + c.src + "\n",
		})
		res, err := Dir(dir, nil)
		if err != nil {
			t.Fatal(err)
		}
		if err := res.Err(); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s\nwant %q, got %v", c.src, c.want, err)
		}
	}
	dir := writeTree(t, map[string]string{"app.asuka": "import std/assets\n"})
	res, _ := Dir(dir, nil)
	if err := res.Err(); err == nil || !strings.Contains(err.Error(), "std/assets is std/embed now") {
		t.Errorf("std/assets: %v", err)
	}
}

// embed.json accepts a file exactly when the runtime's json.decode reads
// it: each case compiled (the checker's verdict) and decoded as the
// program runs (the runtime's), which must agree.
func TestEmbedJSONMatchesRuntime(t *testing.T) {
	const types = `enum Hue { red, green }
enum Shape { circle(r: float), rect(w: float, h: float), dot }
struct P { name: string; age: int }
struct Q { p: P; tags: [string] = [] }
struct B { data: bytes }
`
	cases := []struct{ typ, json string }{
		{"P", `{"name": "a", "age": 3}`},
		{"P", `{"name": "a"}`},
		{"P", `{"age": 2.5}`},
		{"P", `{"age": 2.0}`},
		{"P", `{"age": "3"}`},
		{"P", `{"name": "a", "age": 3} x`},
		{"P", `{"name": "a", "extra": {"x": [1, {"y": null}], "z": true}}`},
		{"P", `["a", 3]`},
		{"Q", `{"tags": ["x"]}`},
		{"Q", `{"p": {}}`},
		{"[int]", `[1, 2, 3]`},
		{"[int]", `[1, 2,]`},
		{"[int]", `[-]`},
		{"[string: int]", `{"a": 1, "b": 2}`},
		{"[string: int]", `{"a": 1,}`},
		{"Hue", `"red"`},
		{"Hue", `"blue"`},
		{"Shape", `"dot"`},
		{"Shape", `"circle"`},
		{"Shape", `{"circle": [1]}`},
		{"Shape", `{"circle": 1}`},
		{"Shape", `{"rect": {"w": 1, "h": 2}}`},
		{"Shape", `{"rect": [1]}`},
		{"Shape", `{"square": 1}`},
		{"int?", `null`},
		{"B", `{}`},
		{"B", `{"data": "aGk="}`},
		{"bool", `true`},
		{"bool", `"true"`},
		{"float", `1e3`},
		{"int", `1e3`},
		{"string", `"aé\n"`},
		{"string", `"bad \x"`},
		{"Set<int>", `[1, 1]`},
		{"int", `9223372036854775807`},
		{"int", `9223372036854775808`},
		{"int", `-9.3e18`},
		{"float", `99999999999999999999`},
		{"float", `1e999`},
		{"float", `1-2`},
		{"P", `{"name": "a", "x": ` + strings.Repeat("[", 512) + strings.Repeat("]", 512) + `}`},
		{"P", `{"name": "a", "x": ` + strings.Repeat("[", 511) + strings.Repeat("]", 511) + `}`},
		{"Shape", `{"rect": {"w": 1}}`},
		{"Shape", `{"rect": {"w": 1 "h": 2}}`},
		{"Shape", `{"rect": {"w": 1, "h": 2,}}`},
	}
	files := map[string]string{}
	var main strings.Builder
	main.WriteString("import std/embed\nimport std/json\n\n" + types + "\nfn main() {\n")
	for i, c := range cases {
		files[fmt.Sprintf("c/%d.json", i)] = c.json
		fmt.Fprintf(&main, "  do {\n    let v = try json.decode<%s>(embed.text(\"c/%d.json\"))\n    print(\"%d ok\")\n  } catch {\n    print(\"%d fail\")\n  }\n", c.typ, i, i, i)
	}
	main.WriteString("}\n")
	files["main.asuka"] = main.String()
	dir := writeTree(t, files)
	res, err := Dir(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	run, err := RunMain(testRunner(t), res.Module, MainOptions{})
	if err != nil {
		t.Fatalf("%v\n%+v", err, run)
	}
	runtime := map[int]bool{}
	for _, line := range strings.Split(strings.TrimSpace(run.Stdout), "\n") {
		var i int
		var verdict string
		if _, err := fmt.Sscanf(line, "%d %s", &i, &verdict); err == nil {
			runtime[i] = verdict == "ok"
		}
	}
	t.Logf("the runtime: %v", runtime)
	if len(runtime) != len(cases) {
		t.Fatalf("the program said:\n%s\n%s", run.Stdout, run.Stderr)
	}
	for i, c := range cases {
		d := writeTree(t, map[string]string{
			"c.json":    c.json,
			"app.asuka": "import std/embed\n\n" + types + "\nlet v: " + c.typ + " = embed.json(\"c.json\")\n",
		})
		res, err := Dir(d, nil)
		if err != nil {
			t.Fatal(err)
		}
		checked := res.Err() == nil
		if checked != runtime[i] {
			t.Errorf("%s from %s: the checker says %v, json.decode %v (%v)", c.typ, c.json, checked, runtime[i], res.Err())
		}
	}
}
