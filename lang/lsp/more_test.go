package lsp

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A checker's "did you mean" is a quick fix.
func TestCodeAction(t *testing.T) {
	root := copyTree(t, "testdata/app")
	c := newClient(t, root)
	uri, text := c.open(filepath.Join(root, "main.asuka"))
	c.waitDiags(uri, noDiags)
	broken := strings.Replace(text, "Text(todo.title)", "Text(todo.titel)", 1)
	c.change(uri, 2, broken)
	ds := c.waitDiags(uri, hasDiag("titel"))
	var p CodeActionParams
	p.TextDocument.URI = uri
	p.Range = ds[0].Range
	p.Context.Diagnostics = ds
	var actions []CodeAction
	if err := c.call("textDocument/codeAction", &p, &actions); err != nil {
		t.Fatal(err)
	}
	if len(actions) != 1 || actions[0].Title != "Change to title" {
		t.Fatalf("got %+v", actions)
	}
	if got := applyEdits(broken, actions[0].Edit.Changes[uri]); got != text {
		t.Errorf("fixed:\n%s", got)
	}
}

// Positions count UTF-16 units, as LSP says: text before a name on its
// line that's outside the BMP (an emoji) or multi-byte (é) is counted so.
func TestUTF16AndIncrementalEdits(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	text := "component A {\n  state café = \"🎉\"\n  Text(\"🎉 {café}\") { color: .accent }\n}\n"
	uri := c.openText(filepath.Join(root, "a.asuka"), text)
	c.waitDiags(uri, noDiags)

	var h *Hover
	p := tdp(uri, posAt(t, text, "{caf|é}"))
	if err := c.call("textDocument/hover", &p, &h); err != nil || h == nil || !strings.Contains(h.Contents.Value, "state café: string") {
		t.Fatalf("hover: %v %+v", err, h)
	}
	// The emoji is 2 units: `  Text("🎉 {` is 12 units to café, which
	// is 4 (é is 1).
	if h.Range == nil || h.Range.Start.Character != 12 || h.Range.End.Character != 16 {
		t.Errorf("range %+v", h.Range)
	}

	// An incremental edit after the emoji: café → cafe (a mistake).
	start := posAt(t, text, "{caf|é}")
	c.notify("textDocument/didChange", map[string]any{
		"textDocument":   VersionedTextDocumentIdentifier{URI: uri, Version: 2},
		"contentChanges": []map[string]any{{"range": Range{Start: start, End: Position{Line: start.Line, Character: start.Character + 1}}, "text": "e"}},
	})
	ds := c.waitDiags(uri, hasDiag("unknown name cafe"))
	if ds[0].Range.Start.Character != 12 {
		t.Errorf("diagnostic at %+v", ds[0].Range)
	}
	// And back.
	c.notify("textDocument/didChange", map[string]any{
		"textDocument":   VersionedTextDocumentIdentifier{URI: uri, Version: 3},
		"contentChanges": []map[string]any{{"range": Range{Start: start, End: Position{Line: start.Line, Character: start.Character + 1}}, "text": "é"}},
	})
	c.waitDiags(uri, noDiags)
}

// A prop's value before its . is typed offers the values with it.
func TestCompletionPropValue(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	text := "component A {\n  Button(\"Go\") {\n    variant: \n  }\n}\n"
	uri := c.openText(filepath.Join(root, "a.asuka"), text)
	version := 1
	items := complete(t, c, uri, &version, text, "component A {\n  Button(\"Go\") {\n    variant: |")
	found := false
	for _, it := range items {
		if it.Label == ".filled" {
			found = true
			if it.InsertText != ".filled" || it.FilterText != "filled" {
				t.Errorf("item %+v", it)
			}
		}
	}
	if !found {
		t.Errorf("no .filled in %v", labels(items))
	}
}

// The standard library: hover and definition reach into std's files.
func TestStandardLibrary(t *testing.T) {
	root := t.TempDir()
	if r, err := filepath.EvalSymlinks(root); err == nil {
		root = r
	}
	os.WriteFile(filepath.Join(root, "asuka.toml"), []byte("name = \"app\"\n"), 0o644)
	text := "import std/store\nimport std/math\n\nfn units() -> string = store.read(\"units\", \"metric\")\nfn two() -> float = math.sqrt(4)\n"
	c := newClient(t, root)
	uri := c.openText(filepath.Join(root, "a.asuka"), text)
	c.waitDiags(uri, noDiags)

	var locs []Location
	p := tdp(uri, posAt(t, text, "store.re|ad("))
	if err := c.call("textDocument/definition", &p, &locs); err != nil || len(locs) != 1 || !strings.HasSuffix(locs[0].URI, "/store/store.asuka") {
		t.Fatalf("definition: %v %+v", err, locs)
	}
	var h *Hover
	p = tdp(uri, posAt(t, text, "store.re|ad("))
	if err := c.call("textDocument/hover", &p, &h); err != nil || h == nil || !strings.Contains(h.Contents.Value, "fn read<T>(key: string, def: T) -> T") {
		t.Fatalf("hover: %v %+v", err, h)
	}
	p = tdp(uri, posAt(t, text, "import std/sto|re"))
	if err := c.call("textDocument/hover", &p, &h); err != nil || h == nil || !strings.Contains(h.Contents.Value, "keeps small values") {
		t.Fatalf("package hover: %v %+v", err, h)
	}
	p = tdp(uri, posAt(t, text, "math.sq|rt"))
	if err := c.call("textDocument/hover", &p, &h); err != nil || h == nil || !strings.Contains(h.Contents.Value, "The square root.") {
		t.Fatalf("math hover: %v %+v", err, h)
	}
	version := 1
	text2 := text + "fn x() -> float = math.\n"
	items := complete(t, c, uri, &version, text2, text2[:len(text2)-1]+"|")
	if got := labels(items); !contains(got, "sqrt") || !contains(got, "pi") {
		t.Errorf("math.: %v", got)
	}
}

// The names in an import's braces complete with what the package
// exports, without the ones the braces name already.
func TestCompletionImportNames(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	uri := c.openText(filepath.Join(root, "a.asuka"), "")
	version := 1
	for _, tc := range []struct {
		text      string
		want, not []string
	}{
		{"import std/time.{|}\n", []string{"Time", "now", "Zone"}, []string{"fn", "component"}},
		{"import std/time.{now, |}\n", []string{"Time"}, []string{"now"}},
		{"import std/time.{Ti|, now}\n", []string{"Time"}, []string{"now"}},
	} {
		i := strings.Index(tc.text, "|")
		text := tc.text[:i] + tc.text[i+1:]
		items := complete(t, c, uri, &version, text, text[:i]+"|")
		got := labels(items)
		for _, w := range tc.want {
			if !contains(got, w) {
				t.Errorf("%q: no %s in %v", tc.text, w, got)
			}
		}
		for _, n := range tc.not {
			if contains(got, n) {
				t.Errorf("%q: %s in %v", tc.text, n, got)
			}
		}
		for _, it := range items {
			if it.InsertTextFormat == 2 {
				t.Errorf("%q: %s inserts a snippet %q: the braces list names", tc.text, it.Label, it.InsertText)
			}
		}
	}
}

// A built-in property (g.width, card.hovered) completes as a name, not a
// call, and hovers as a property; a component's variant read through
// its name (box.checked) goes to its declaration.
func TestBuiltinProperties(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	text := `component Check {
  prop checked = false
  prop children: View
  variant checked = checked
  View { children }
}

component Main {
  Column {
    View as card { Text("x") }
    Check as box { Text("y") }
    Text("{card.hovered} {box.checked}")
    Canvas(g => {
      let w = g.width
    })
  }
}
`
	uri := c.openText(filepath.Join(root, "a.asuka"), text)
	c.waitDiags(uri, noDiags)
	for _, tc := range []struct{ at, want string }{
		{"card.hove|red", "Ref.hovered: bool"},
		{"g.wid|th", "Graphics.width: float"},
		{"box.chec|ked", "variant checked = checked"},
	} {
		if got := hoverAt(t, c, uri, text, tc.at); !strings.Contains(got, tc.want) {
			t.Errorf("hover at %s: %q lacks %q", tc.at, got, tc.want)
		}
	}
	var locs []Location
	p := tdp(uri, posAt(t, text, "box.chec|ked"))
	if err := c.call("textDocument/definition", &p, &locs); err != nil {
		t.Fatal(err)
	}
	if want := posAt(t, text, "variant |checked"); len(locs) != 1 || locs[0].Range.Start != want {
		t.Errorf("definition of box.checked: %+v, want %+v", locs, want)
	}
	version := 1
	for _, marked := range []string{"let w = g.wi|", `Text("{card.ho|`} {
		i := strings.Index(marked, "|")
		j := strings.Index(text, marked[:i])
		items := complete(t, c, uri, &version, text, text[:j+i]+"|")
		name := map[bool]string{true: "width", false: "hovered"}[strings.Contains(marked, "g.")]
		found := false
		for _, it := range items {
			if it.Label != name {
				continue
			}
			found = true
			if it.InsertTextFormat == 2 || strings.Contains(it.InsertText, "(") {
				t.Errorf("%s inserts %q: it's read without parentheses", name, it.InsertText)
			}
		}
		if !found {
			t.Errorf("no %s in %v", name, labels(items))
		}
	}
}

// A Loading's properties: hover says what value and loading are, and
// completion offers them beside reload.
func TestLoadingProperties(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	text := "async fn name(id: int) -> string = \"user {id}\"\n\ncomponent A {\n  state id = 1\n  let user = load name(id)\n  Column {\n    Text(user.value ?? \"\")\n    if user.loading { Text(\"…\") }\n    Button(\"x\") { on press { user.optimistic(n => n + \"!\") } }\n  }\n}\n"
	uri := c.openText(filepath.Join(root, "a.asuka"), text)
	c.waitDiags(uri, noDiags)
	for at, want := range map[string]string{
		"user.val|ue":      "value: string?",
		"user.load|ing":    "loading: bool",
		"user.optim|istic": "optimistic(change: fn(string) -> string)",
	} {
		var h *Hover
		p := tdp(uri, posAt(t, text, at))
		if err := c.call("textDocument/hover", &p, &h); err != nil || h == nil || !strings.Contains(h.Contents.Value, want) {
			t.Errorf("hover %s: %v %+v", at, err, h)
		}
	}
	version := 1
	text2 := strings.Replace(text, "if user.loading", "if user.", 1)
	items := complete(t, c, uri, &version, text2, text2[:strings.Index(text2, "if user.")+len("if user.")]+"|")
	if got := labels(items); !contains(got, "loading") || !contains(got, "value") || !contains(got, "reload") || !contains(got, "optimistic") {
		t.Errorf("user.: %v", got)
	}
}

// reload's operand (rfcs/0017) is the function a load calls: hover says
// what it is, definition goes to it, by its name or its call, a store's
// method or a function; after reload, completion offers what loads call.
func TestReloadOperands(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	text := "async fn fetchNotes() -> [string] = []\n\nstore Api {\n  async fn list() -> [string] = []\n}\n\ncomponent A {\n  use api: Api\n  let notes = load fetchNotes()\n  let items = load api.list()\n  Button(\"x\") {\n    on press {\n      await reload fetchNotes()\n      reload api.list\n    }\n  }\n}\n"
	uri := c.openText(filepath.Join(root, "a.asuka"), text)
	c.waitDiags(uri, noDiags)
	for _, tc := range []struct{ at, hover, to string }{
		{"reload fetchNo|tes()", "fn fetchNotes() -> [string]", "fn |fetchNotes"},
		{"reload api.li|st", "async fn Api.list() -> [string]", "fn |list"},
	} {
		if got := hoverAt(t, c, uri, text, tc.at); !strings.Contains(got, tc.hover) {
			t.Errorf("hover at %s: %q lacks %q", tc.at, got, tc.hover)
		}
		var locs []Location
		p := tdp(uri, posAt(t, text, tc.at))
		if err := c.call("textDocument/definition", &p, &locs); err != nil {
			t.Fatal(err)
		}
		if want := posAt(t, text, tc.to); len(locs) != 1 || locs[0].Range.Start != want {
			t.Errorf("definition at %s: %+v, want %+v", tc.at, locs, want)
		}
	}
	version := 1
	at := strings.Index(text, "reload api.list") + len("reload api.")
	if got := labels(complete(t, c, uri, &version, text, text[:at]+"|")); !contains(got, "list") {
		t.Errorf("after reload api.: %v", got)
	}
	at = strings.Index(text, "await reload ") + len("await reload ")
	if got := labels(complete(t, c, uri, &version, text, text[:at]+"|")); !contains(got, "fetchNotes") {
		t.Errorf("after reload: %v", got)
	}
}
