package syntax

import (
	"strings"
	"testing"
)

// load followed by ( calls a function named load, unless the parentheses
// hold one call: then they're the load operator's operand, as before.
func TestParseLoadParen(t *testing.T) {
	tests := []struct{ src, want string }{
		{"load()", "(call load)"},
		{"load(id)", "(call load id)"},
		{"load(id: 1, 2)", "(call load id:1 2)"},
		{"load().value", "(. (call load) value)"},
		{"load(f())", "(load (paren (call f)))"},
		{"load (api.user(id))", "(load (paren (call (. api user) id)))"},
		{"load ((f()))", "(load (paren (paren (call f))))"},
		{"load (f())(x)", "(load (call (paren (call f)) x))"},
		{"load f(x)", "(load (call f x))"},
		{"load (a(), b(x))", "(load (tuple (call a) (call b x)))"},
		{"load (api.user(id), x)", "(load (tuple (call (. api user) id) x))"},
		{"load(a, b)", "(call load a b)"},
		{"load(a: f(), b())", "(call load a:(call f) (call b))"},
	}
	for _, tt := range tests {
		x, errs := ParseExpr(tt.src)
		if len(errs) > 0 {
			t.Errorf("%q: %v", tt.src, errs)
			continue
		}
		if got := sexpr(x); got != tt.want {
			t.Errorf("%q:\n got %s\nwant %s", tt.src, got, tt.want)
		}
	}
	src := "component C {\n  task(id) { await load() }\n  let u = load (api.user(id))\n  let page = load (api.user(id), api.posts(id, page: n))\n}\n"
	if out, err := Format("x.asuka", []byte(src)); err != nil || string(out) != src {
		t.Errorf("formats as %q, %v", out, err)
	}
}

// reload followed by a name is the reload operator's (rfcs/0017): of a
// call, or of a function by its name; followed by ( it calls a function
// named reload, and alone it's that name. x.reload() and .reload are
// members so named.
func TestParseReload(t *testing.T) {
	tests := []struct{ src, want string }{
		{"reload api.todos()", "(reload (call (. api todos)))"},
		{"reload api.note(id)", "(reload (call (. api note) id))"},
		{"reload api.note", "(reload (. api note))"},
		{"reload notes", "(reload notes)"},
		{"await reload api.todos()", "(await (reload (call (. api todos))))"},
		{"reload()", "(call reload)"},
		{"reload(id).x", "(. (call reload id) x)"},
		{"reload", "reload"},
		{"user.reload()", "(call (. user reload))"},
		{".reload", ".reload"},
	}
	for _, tt := range tests {
		x, errs := ParseExpr(tt.src)
		if len(errs) > 0 {
			t.Errorf("%q: %v", tt.src, errs)
			continue
		}
		if got := sexpr(x); got != tt.want {
			t.Errorf("%q:\n got %s\nwant %s", tt.src, got, tt.want)
		}
	}
	src := "component C {\n  on press {\n    await reload api.notes(id)\n    reload api.notes\n    await reload()\n  }\n}\n\nfn reload() {}\n"
	if out, err := Format("x.asuka", []byte(src)); err != nil || string(out) != src {
		t.Errorf("formats as %q, %v", out, err)
	}
	// A name it can't be says to rename it.
	_, errs := ParseFile(NewFileSet(), "x.asuka", []byte("fn f() {\n  let reload = 1\n}\n"))
	if len(errs) == 0 || !strings.Contains(errs[0].Error(), "rename it: reload is a keyword since 0.2.9") {
		t.Errorf("let reload: %v", errs)
	}
}
