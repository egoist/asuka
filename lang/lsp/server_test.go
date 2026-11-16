package lsp

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

func TestMain(m *testing.M) {
	dir, err := os.MkdirTemp("", "asuka-lsp-cache")
	if err != nil {
		panic(err)
	}
	userCacheDir = func() (string, error) { return dir, nil }
	code := m.Run()
	os.RemoveAll(dir)
	os.Exit(code)
}

func noDiags(d []Diagnostic) bool { return len(d) == 0 }

func hasDiag(substr string) func([]Diagnostic) bool {
	return func(ds []Diagnostic) bool {
		for _, d := range ds {
			if strings.Contains(d.Message, substr) {
				return true
			}
		}
		return false
	}
}

func TestDiagnostics(t *testing.T) {
	root := copyTree(t, "testdata/app")
	c := newClient(t, root)
	main := filepath.Join(root, "main.asuka")
	uri, text := c.open(main)
	c.waitDiags(uri, noDiags)

	// A mistake: reported where it is, with what would fix it.
	broken := strings.Replace(text, "Text(todo.title)", "Text(todo.titel)", 1)
	c.change(uri, 2, broken)
	ds := c.waitDiags(uri, hasDiag("titel"))
	if len(ds) != 1 || !strings.Contains(ds[0].Message, "did you mean title?") {
		t.Fatalf("got %+v", ds)
	}
	want := posAt(t, broken, "Text(todo.|titel)")
	if ds[0].Range.Start != want || ds[0].Range.End.Character != want.Character+5 {
		t.Errorf("range %+v, want from %+v, 5 long", ds[0].Range, want)
	}
	// Fixed: cleared.
	c.change(uri, 3, text)
	c.waitDiags(uri, noDiags)

	// A change to an imported package re-checks its importer: the field
	// main uses is gone.
	model := filepath.Join(root, "model", "model.asuka")
	muri, mtext := c.open(model)
	c.waitDiags(muri, noDiags)
	c.change(muri, 2, strings.Replace(mtext, "title: string", "name: string", 1))
	c.waitDiags(uri, hasDiag("has no title"))
	c.change(muri, 3, mtext)
	c.waitDiags(uri, noDiags)

	// A syntax error, and the checker's errors after it.
	c.change(uri, 4, text+"\ncomponent {\n")
	c.waitDiags(uri, func(d []Diagnostic) bool { return len(d) > 0 })

	// Closing the package's files clears its diagnostics.
	c.notify("textDocument/didClose", map[string]any{"textDocument": TextDocumentIdentifier{URI: uri}})
	c.waitDiags(uri, noDiags)
}

func TestHover(t *testing.T) {
	root := copyTree(t, "testdata/app")
	c := newClient(t, root)
	uri, text := c.open(filepath.Join(root, "main.asuka"))
	c.waitDiags(uri, noDiags)
	for _, tc := range []struct {
		at   string
		want []string
	}{
		{"Text(todo.ti|tle)", []string{"title: string", "What to do.", "Field of `Todo`"}},
		{"component TodoRow(to|do: Todo)", []string{"todo: model.Todo", "parameter of `TodoRow`"}},
		{"Text|(todo.title)", []string{"native component Text(text: string)", "a text: it wraps", "**Props:**"}},
		{"color|: .secondaryLabel", []string{"prop color: Color", "text color"}},
		{"color: .second|aryLabel", []string{"Color.secondaryLabel"}},
		{"gap|: 8", []string{"prop gap: float", "style prop"}},
		{"style: ta|g(.red)", []string{"style tag", "A tag's look."}},
		{"on chan|ge(on)", []string{"event change(on: bool)"}},
		{"emit tog|gle(on)", []string{"event toggle(on: bool)", "The toggle was switched."}},
		{"todos.cou|nt(where:", []string{"fn [model.Todo].count(where: fn(model.Todo) -> bool) -> int", "The number of elements the predicate is true of."}},
		{"let remai|ning", []string{"let remaining: int"}},
		{"model.mat|ches(todo", []string{"pub fn matches(t: model.Todo, f: model.Filter) -> bool", "Whether a todo shows under a filter."}},
		{"import ./mo|del\n", []string{"package ./model"}},
		{"if mod|el.matches", []string{"package ./model"}},
		{"Filter.al|l", []string{"case Filter.all"}},
		{"todo.lab|el()", []string{"pub fn Todo.label() -> string"}},
		{"let ne|xt", []string{"let next: int"}},
		{"add(ti|tle: string)", []string{"(parameter) title: string"}},
		{"Todo(i|d: next", []string{"id: int"}},
		{"state todos: [To|do]", []string{"pub struct Todo {", "title: string", "Something to do."}},
		{"pad|ding: 16", []string{"prop padding: Edges"}},
	} {
		t.Run(tc.at, func(t *testing.T) {
			var h *Hover
			p := tdp(uri, posAt(t, text, tc.at))
			if err := c.call("textDocument/hover", &p, &h); err != nil {
				t.Fatal(err)
			}
			if h == nil {
				t.Fatalf("no hover")
			}
			for _, w := range tc.want {
				if !strings.Contains(h.Contents.Value, w) {
					t.Errorf("hover lacks %q:\n%s", w, h.Contents.Value)
				}
			}
		})
	}
}

// complete returns the completion's labels at the | of marked, in text
// sent as the document's.
func complete(t *testing.T, c *client, uri string, version *int, text, marked string) []CompletionItem {
	t.Helper()
	i := strings.IndexByte(marked, '|')
	*version++
	c.change(uri, *version, text)
	var list CompletionList
	p := CompletionParams{TextDocumentPositionParams: tdp(uri, newMapper([]byte(text), false).position(i))}
	if strings.Count(marked, "|") != 1 {
		t.Fatalf("one | in %q", marked)
	}
	_ = text
	if err := c.call("textDocument/completion", &p, &list); err != nil {
		t.Fatal(err)
	}
	return list.Items
}

func labels(items []CompletionItem) []string {
	var out []string
	for _, it := range items {
		out = append(out, it.Label)
	}
	sort.Strings(out)
	return out
}

func TestCompletion(t *testing.T) {
	root := copyTree(t, "testdata/app")
	c := newClient(t, root)
	main := filepath.Join(root, "main.asuka")
	uri, orig := c.open(main)
	c.waitDiags(uri, noDiags)
	version := 1
	for _, tc := range []struct {
		name     string
		replace  string // what in the file to replace
		with     string // with this, | at the cursor
		want     []string
		dontWant []string
	}{
		{"fields and methods", "Text(todo.title)", "Text(todo.|)", []string{"id", "title", "done", "label"}, []string{"push"}},
		{"style list", "style: tag(.red)", "style: [tag(.red), |]", []string{"tag"}, []string{"Column"}},
		{"style", "style: tag(.red)", "style: |", []string{"tag"}, nil},
		{"field prefix", "Text(todo.title)", "Text(todo.ti|)", []string{"title"}, nil},
		{"array methods", "todos.push(", "todos.|push(", []string{"push", "count", "filter", "map", "isEmpty"}, []string{"title"}},
		{"package members", "if model.matches(todo, filter)", "if model.|matches(todo, filter)", []string{"matches", "Todo", "Filter"}, nil},
		{"enum cases", "model.Filter.all", "model.Filter.|", []string{"all", "open", "done"}, nil},
		{"semantic colors", "color: .secondaryLabel", "color: .|", []string{"label", "secondaryLabel", "accent"}, nil},
		{"enum implicit member", "padding: 16", "padding: 16\n    direction: .|", []string{"row", "column"}, nil},
		{"element props", "gap: 8\n", "gap: 8\n    |\n", []string{"wrap", "padding", "background", "on press", "if", "for", "Text"}, []string{"gap", "children"}},
		{"element events", "on change(on) { emit toggle(on) }", "on |", []string{"change"}, []string{"press"}},
		{"bind", "Toggle { on: todo.done;", "Toggle { bind |", []string{"on"}, nil},
		{"component body", "  let remaining", "  |\n  let remaining", []string{"state", "prop", "let", "fn", "event", "Column", "todos"}, nil},
		{"locals", "todos.push(Todo(id: next", "todos.push(Todo(id: ne|", []string{"next", "title", "todos"}, nil},
		{"labels", "Todo(id: next, title: title)", "Todo(id: next, |)", []string{"title:", "done:"}, []string{"id:"}},
		{"top level", "app { TodoApp() }", "|", []string{"component", "struct", "enum", "fn", "import"}, nil},
		{"optional chain", "let next = todos.count() + 1", "let next = todos.first().|", []string{"isNone", "title", "label"}, nil},
		{"string interpolation", "Text(\"{remaining} left\")", "Text(\"{todos.|} left\")", []string{"count"}, nil},
		{"import", "import ./model\n", "import |\n", []string{"./model", "std/json", "std/http"}, nil},
		{"handler params", "on toggle(on) { add(todo.label()) }", "on toggle(on) { add(o|) }", []string{"on", "todo"}, nil},
		{"emit", "{ emit toggle(on) }", "{ emit | }", []string{"toggle"}, nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			i := strings.Index(orig, tc.replace)
			if i < 0 {
				t.Fatalf("%q isn't in the file", tc.replace)
			}
			cur := strings.Index(tc.with, "|")
			text := orig[:i] + strings.Replace(tc.with, "|", "", 1) + orig[i+len(tc.replace):]
			marked := text[:i+cur] + "|"
			items := complete(t, c, uri, &version, text, marked)
			got := labels(items)
			for _, w := range tc.want {
				if !contains(got, w) {
					t.Errorf("no %q in %v", w, got)
				}
			}
			for _, w := range tc.dontWant {
				if contains(got, w) {
					t.Errorf("%q shouldn't be in %v", w, got)
				}
			}
		})
	}
	// The ?. an optional's members need is written for them.
	text := strings.Replace(orig, "let next = todos.count() + 1", "let next = todos.first().", 1)
	i := strings.Index(text, "todos.first().") + len("todos.first().")
	items := complete(t, c, uri, &version, text, text[:i]+"|")
	for _, it := range items {
		if it.Label == "title" {
			if it.TextEdit == nil || it.TextEdit.NewText != "?.title" {
				t.Errorf("title's edit: %+v", it.TextEdit)
			}
		}
	}
}

func TestDefinition(t *testing.T) {
	root := copyTree(t, "testdata/app")
	c := newClient(t, root)
	uri, text := c.open(filepath.Join(root, "main.asuka"))
	c.waitDiags(uri, noDiags)
	model := pathToURI(filepath.Join(root, "model", "model.asuka"))
	mtext, _ := os.ReadFile(filepath.Join(root, "model", "model.asuka"))
	for _, tc := range []struct {
		at      string
		uri     string
		decl    string // the text of the file at the location: | before it
		typeDef bool
	}{
		{"Text(todo.ti|tle)", model, "  |title: string", false},
		{"model.mat|ches(todo", model, "pub fn |matches", false},
		{"TodoRow|(todo) {", uri, "component |TodoRow", false},
		{"emit tog|gle(on)", uri, "event |toggle", false},
		{"on tog|gle(on) { add", uri, "event |toggle", false},
		{"add(todo.lab|el())", model, "pub fn |label", false},
		{"Text(\"{remain|ing} left\")", uri, "let |remaining", false},
		{"todos.push(Todo(id: next, title: tit|le))", uri, "fn add(|title", false},
		{"Todo(i|d: next", model, "  |id: int", false},
		{"Filter.al|l", model, "enum Filter { |all", false},
		{"import ./mo|del\n", model, "|/// Something", false},
		{"let rem|aining", uri, "let |remaining", false},
		{"for todo in todos key tod|o.id", uri, "for |todo in", false},
		{"state todos: [To|do]", model, "pub struct |Todo", false},
		// Type definitions.
		{"TodoRow(to|do) {", model, "pub struct |Todo", true},
		{"state fil|ter", model, "pub enum |Filter", true},
	} {
		t.Run(tc.at, func(t *testing.T) {
			var locs []Location
			method := "textDocument/definition"
			if tc.typeDef {
				method = "textDocument/typeDefinition"
			}
			p := tdp(uri, posAt(t, text, tc.at))
			if err := c.call(method, &p, &locs); err != nil {
				t.Fatal(err)
			}
			if len(locs) != 1 {
				t.Fatalf("got %+v", locs)
			}
			if locs[0].URI != tc.uri {
				t.Fatalf("got %s, want %s", locs[0].URI, tc.uri)
			}
			src := text
			if tc.uri == model {
				src = string(mtext)
			}
			if want := posAt(t, src, tc.decl); locs[0].Range.Start != want {
				t.Errorf("at %+v, want %+v", locs[0].Range.Start, want)
			}
		})
	}
	// An element: its declaration in the prelude, which opens as a file.
	var locs []Location
	p := tdp(uri, posAt(t, text, "Tex|t(todo.title)"))
	c.call("textDocument/definition", &p, &locs)
	if len(locs) != 1 || !strings.HasSuffix(locs[0].URI, "/elements.asuka") {
		t.Fatalf("Text's definition: %+v", locs)
	}
	b, err := os.ReadFile(uriToPath(locs[0].URI))
	if err != nil {
		t.Fatal(err)
	}
	if line := strings.Split(string(b), "\n")[locs[0].Range.Start.Line]; !strings.Contains(line, "native component Text(") {
		t.Errorf("line %q", line)
	}
	// Hover works in the prelude's copy too.
	puri, ptext := c.open(uriToPath(locs[0].URI))
	var h *Hover
	hp := tdp(puri, posAt(t, ptext, "native component Text(text: str|ing)"))
	if err := c.call("textDocument/hover", &hp, &h); err != nil || h == nil || !strings.Contains(h.Contents.Value, "string") {
		t.Errorf("hover in the prelude: %v %+v", err, h)
	}
}

func TestFormatting(t *testing.T) {
	root := t.TempDir()
	c := newClient(t, root)
	path := filepath.Join(root, "a.asuka")
	uri := c.openText(path, "component  A {\n    state x=1\n  Text(\"{x}\")\n}\n")
	var edits []TextEdit
	if err := c.call("textDocument/formatting", &DocumentFormattingParams{TextDocument: TextDocumentIdentifier{URI: uri}}, &edits); err != nil {
		t.Fatal(err)
	}
	got := applyEdits("component  A {\n    state x=1\n  Text(\"{x}\")\n}\n", edits)
	want := "component A {\n  state x = 1\n  Text(\"{x}\")\n}\n"
	if got != want {
		t.Errorf("formatted:\n%s\nwant:\n%s", got, want)
	}
	// Syntax errors: nothing to format, and no error.
	c.change(uri, 2, "component A {\n")
	edits = nil
	if err := c.call("textDocument/formatting", &DocumentFormattingParams{TextDocument: TextDocumentIdentifier{URI: uri}}, &edits); err != nil || len(edits) != 0 {
		t.Errorf("got %v %+v", err, edits)
	}
}

// applyEdits applies non-overlapping edits to text.
func applyEdits(text string, edits []TextEdit) string {
	m := newMapper([]byte(text), false)
	sort.Slice(edits, func(i, j int) bool { return m.offset(edits[i].Range.Start) > m.offset(edits[j].Range.Start) })
	for _, e := range edits {
		a, b := m.offset(e.Range.Start), m.offset(e.Range.End)
		text = text[:a] + e.NewText + text[b:]
	}
	return text
}

func TestReferencesAndRename(t *testing.T) {
	root := copyTree(t, "testdata/app")
	c := newClient(t, root)
	uri, text := c.open(filepath.Join(root, "main.asuka"))
	c.waitDiags(uri, noDiags)
	model := pathToURI(filepath.Join(root, "model", "model.asuka"))

	// title: its declaration, the uses in both packages, Todo(title:).
	var locs []Location
	p := ReferenceParams{TextDocumentPositionParams: tdp(uri, posAt(t, text, "Text(todo.ti|tle)"))}
	p.Context.IncludeDeclaration = true
	if err := c.call("textDocument/references", &p, &locs); err != nil {
		t.Fatal(err)
	}
	byURI := map[string]int{}
	for _, l := range locs {
		byURI[l.URI]++
	}
	// model.asuka: the field, {title} in label. main.asuka: todo.title,
	// title: in Todo(…).
	if byURI[model] != 2 || byURI[uri] != 2 {
		t.Errorf("references: %+v", locs)
	}

	// Renaming the event: its declaration, emit and on.
	var edit WorkspaceEdit
	rp := RenameParams{TextDocumentPositionParams: tdp(uri, posAt(t, text, "emit tog|gle(on)")), NewName: "flip"}
	if err := c.call("textDocument/rename", &rp, &edit); err != nil {
		t.Fatal(err)
	}
	got := applyEdits(text, edit.Changes[uri])
	if strings.Contains(got, "toggle(") || strings.Count(got, "flip") != 3 {
		t.Errorf("renamed:\n%s", got)
	}

	// Renaming a struct across packages.
	edit = WorkspaceEdit{}
	rp = RenameParams{TextDocumentPositionParams: tdp(uri, posAt(t, text, "component TodoRow(todo: To|do)")), NewName: "Task"}
	if err := c.call("textDocument/rename", &rp, &edit); err != nil {
		t.Fatal(err)
	}
	// model.asuka: the struct, matches(t: Todo). main.asuka: the import's
	// .{Todo}, TodoRow(todo: Todo), [Todo], Todo(id: …).
	if len(edit.Changes[model]) != 2 || len(edit.Changes[uri]) != 4 {
		t.Errorf("rename Todo: %+v", edit.Changes)
	}

	// The prelude's names can't be renamed.
	var prep *PrepareRenameResult
	pp := tdp(uri, posAt(t, text, "Tex|t(todo.title)"))
	if err := c.call("textDocument/prepareRename", &pp, &prep); err == nil {
		t.Errorf("prepareRename Text: %+v", prep)
	}
	// Nor to what isn't a name.
	rp = RenameParams{TextDocumentPositionParams: tdp(uri, posAt(t, text, "let rem|aining")), NewName: "fn"}
	if err := c.call("textDocument/rename", &rp, &edit); err == nil {
		t.Error("renamed to a keyword")
	}

	// Highlights in the file.
	var hs []DocumentHighlight
	hp := tdp(uri, posAt(t, text, "let rem|aining"))
	if err := c.call("textDocument/documentHighlight", &hp, &hs); err != nil || len(hs) != 2 {
		t.Errorf("highlights: %v %+v", err, hs)
	}
}

func TestSymbolsAndSignatures(t *testing.T) {
	root := copyTree(t, "testdata/app")
	c := newClient(t, root)
	uri, text := c.open(filepath.Join(root, "main.asuka"))
	c.waitDiags(uri, noDiags)

	var syms []DocumentSymbol
	if err := c.call("textDocument/documentSymbol", &DocumentSymbolParams{TextDocument: TextDocumentIdentifier{URI: uri}}, &syms); err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, s := range syms {
		names = append(names, s.Name)
		for _, ch := range s.Children {
			names = append(names, s.Name+"."+ch.Name)
		}
	}
	for _, w := range []string{"TodoRow", "TodoRow.toggle", "TodoApp", "TodoApp.todos", "TodoApp.add", "app"} {
		if !contains(names, w) {
			t.Errorf("no symbol %s in %v", w, names)
		}
	}

	var ws []SymbolInformation
	if err := c.call("workspace/symbol", &WorkspaceSymbolParams{Query: "match"}, &ws); err != nil || len(ws) == 0 || ws[0].Name != "matches" {
		t.Errorf("workspace symbols: %v %+v", err, ws)
	}

	// Signature help, with the parameter being typed.
	for _, tc := range []struct {
		at     string
		label  string
		active int
	}{
		{"model.matches(todo, |filter)", "fn matches(t: model.Todo, f: model.Filter) -> bool", 1},
		{"Todo(id: next, |title: title)", "Todo(id: int, title: string, done: bool = false)", 1},
		{"todos.count(|where:", "fn [model.Todo].count(where: fn(model.Todo) -> bool) -> int", 0},
		{"add(|todo.label())", "fn TodoApp.add(title: string)", 0},
	} {
		var sh *SignatureHelp
		p := tdp(uri, posAt(t, text, tc.at))
		if err := c.call("textDocument/signatureHelp", &p, &sh); err != nil {
			t.Fatal(err)
		}
		if sh == nil || len(sh.Signatures) != 1 {
			t.Errorf("%s: %+v", tc.at, sh)
			continue
		}
		if got := sh.Signatures[0].Label; !strings.HasSuffix(got, tc.label) {
			t.Errorf("%s: label %q, want %q", tc.at, got, tc.label)
		}
		if sh.ActiveParameter != tc.active {
			t.Errorf("%s: active %d, want %d", tc.at, sh.ActiveParameter, tc.active)
		}
	}

	var toks SemanticTokens
	if err := c.call("textDocument/semanticTokens/full", &SemanticTokensParams{TextDocument: TextDocumentIdentifier{URI: uri}}, &toks); err != nil || len(toks.Data) == 0 || len(toks.Data)%5 != 0 {
		t.Errorf("semantic tokens: %v %d", err, len(toks.Data))
	}
}
