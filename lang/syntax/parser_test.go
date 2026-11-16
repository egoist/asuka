package syntax

import (
	"fmt"
	"strings"
	"testing"
)

// sexpr writes an expression fully parenthesized, to test how it
// parsed: (+ a (* b c)), (call f a b:c), (. x name).
func sexpr(n Node) string {
	switch x := n.(type) {
	case nil:
		return "nil"
	case *Ident:
		return x.Name
	case *BasicLit:
		return x.Value
	case *StringLit:
		var b strings.Builder
		b.WriteString("(str")
		for i, s := range x.Segments {
			fmt.Fprintf(&b, " %q", s)
			if i < len(x.Interps) {
				b.WriteString(" " + sexpr(x.Interps[i]))
			}
		}
		return b.String() + ")"
	case *BoolLit:
		return fmt.Sprint(x.Value)
	case *NoneLit:
		return "none"
	case *SelfExpr:
		return "self"
	case *ImplicitMember:
		return "." + x.Name.Name
	case *MemberExpr:
		op := "."
		if x.Optional {
			op = "?."
		}
		return "(" + op + " " + sexpr(x.X) + " " + x.Name.Name + ")"
	case *CallExpr:
		s := "(call " + sexpr(x.Fun)
		if x.Args != nil {
			for _, a := range x.Args.List {
				s += " "
				if a.Label != nil {
					s += a.Label.Name + ":"
				}
				s += sexpr(a.Value)
			}
		}
		if x.As != nil {
			s += " as " + x.As.Name
		}
		if x.Block != nil {
			s += " " + sexpr(x.Block)
		}
		for _, h := range x.Handlers {
			s += " (on " + h.Name.Name + ")"
		}
		return s + ")"
	case *Block:
		var parts []string
		for _, it := range x.Items {
			parts = append(parts, sexpr(it))
		}
		return "{" + strings.Join(parts, "; ") + "}"
	case *IndexExpr:
		return "([] " + sexpr(x.X) + " " + sexpr(x.Index) + ")"
	case *UnwrapExpr:
		return "(! " + sexpr(x.X) + ")"
	case *UnaryExpr:
		op := x.Op.String()
		if x.Optional {
			op += "?"
		}
		return "(" + op + " " + sexpr(x.X) + ")"
	case *SpawnExpr:
		return "(spawn " + sexpr(x.Body) + ")"
	case *BinaryExpr:
		return "(" + x.Op.String() + " " + sexpr(x.X) + " " + sexpr(x.Y) + ")"
	case *LambdaExpr:
		var ps []string
		for _, p := range x.Params {
			s := p.Name.Name
			if p.Type != nil {
				s += ":" + printed(p.Type)
			}
			ps = append(ps, s)
		}
		arrow := "=>"
		if x.Async.IsValid() {
			arrow = "async=>"
		}
		return "(" + arrow + " (" + strings.Join(ps, " ") + ") " + sexpr(x.Body) + ")"
	case *ParenExpr:
		return "(paren " + sexpr(x.X) + ")"
	case *TupleExpr:
		return "(tuple" + list(exprNodes(x.Elems)) + ")"
	case *ArrayExpr:
		return "(array" + list(exprNodes(x.Elems)) + ")"
	case *MapExpr:
		s := "(map"
		for _, e := range x.Entries {
			s += " " + sexpr(e.Key) + ":" + sexpr(e.Value)
		}
		return s + ")"
	case *IfExpr:
		return "(if " + sexpr(x.Cond) + " " + sexpr(x.Then) + " " + sexpr(x.Else) + ")"
	case *MatchExpr:
		s := "(match " + sexpr(x.Subject)
		for _, a := range x.Arms {
			s += " " + sexpr(a)
		}
		return s + ")"
	case *MatchArm:
		s := "[" + printed(x.Pattern)
		if x.Guard != nil {
			s += " if " + sexpr(x.Guard)
		}
		return s + " => " + sexpr(x.Body) + "]"
	case *LetCond:
		return "(let " + printed(x.Pattern) + " " + sexpr(x.Value) + ")"
	case *CondList:
		return "(cond" + list(exprNodes(x.List)) + ")"
	case *JumpExpr:
		if x.Value == nil {
			return "(" + x.Tok.String() + ")"
		}
		return "(" + x.Tok.String() + " " + sexpr(x.Value) + ")"
	case *CatchExpr:
		s := "(catch " + sexpr(x.X)
		for _, c := range x.Catches {
			s += " [" + strings.TrimSuffix(printed(c), " "+printed(c.Body)) + " " + sexpr(c.Body) + "]"
		}
		return s + ")"
	case *ExprStmt:
		return sexpr(x.X)
	case *BadExpr:
		return "BAD"
	}
	// Items and the rest: their kind and canonical form.
	return fmt.Sprintf("%T<%s>", n, printed(n))
}

func list(ns []Node) string {
	s := ""
	for _, n := range ns {
		s += " " + sexpr(n)
	}
	return s
}

// printed is n in canonical form, on one line where it can be.
func printed(n Node) string {
	var b strings.Builder
	Fprint(&b, nil, n)
	return b.String()
}

func parseOK(t *testing.T, src string) *SourceFile {
	t.Helper()
	f, errs := ParseFile(NewFileSet(), "t.asuka", []byte(src))
	if len(errs) > 0 {
		t.Fatalf("%q: %v", src, errs)
	}
	return f
}

// items parses src as a component's body and returns its items.
func items(t *testing.T, src string) []Item {
	t.Helper()
	f := parseOK(t, "component C {\n"+src+"\n}")
	return f.Decls[0].(*ComponentDecl).Body.Items
}

func TestParseExpr(t *testing.T) {
	tests := []struct{ src, want string }{
		// Precedence and associativity.
		{"a + b * c", "(+ a (* b c))"},
		{"a * b + c", "(+ (* a b) c)"},
		{"a - b - c", "(- (- a b) c)"},
		{"a ?? b ?? c", "(?? a (?? b c))"},
		{"a || b && c", "(|| a (&& b c))"},
		{"a == b || c != d", "(|| (== a b) (!= c d))"},
		{"a < b + 1", "(< a (+ b 1))"},
		{"a ?? b || c", "(?? a (|| b c))"},
		{"0..<n + 1", "(..< 0 (+ n 1))"},
		{"1..10", "(.. 1 10)"},
		{"x % 2 == 0", "(== (% x 2) 0)"},
		{"!a && -b < c", "(&& (! a) (< (- b) c))"},
		{"-x.y", "(- (. x y))"},
		{"!x!", "(! (! x))"},
		{"(a + b) * c", "(* (paren (+ a b)) c)"},
		// Bitwise operators, with Swift's precedence.
		{"a + b << c", "(+ a (<< b c))"},
		{"a * b >> c", "(* a (>> b c))"},
		{"a & b + c", "(+ (& a b) c)"},
		{"a | b & c", "(| a (& b c))"},
		{"a ^ b | c", "(| (^ a b) c)"},
		{"a & b == c", "(== (& a b) c)"},
		{"~a & -b", "(& (~ a) (- b))"},
		{"a >> b > c", "(> (>> a b) c)"},
		{"a > >b", "(> a BAD)"},
		// Unary keywords.
		{"await f()", "(await (call f))"},
		{"try await f()", "(try (await (call f)))"},
		{"try? f()", "(try? (call f))"},
		{"try ?f", "(try BAD)"},
		{"load fetch(id)", "(load (call fetch id))"},
		{"spawn { return 1 }", "(spawn {*syntax.ReturnStmt<return 1>})"},
		{"await all(a(), b())", "(await (call all (call a) (call b)))"},
		// Postfix.
		{"a.b.c", "(. (. a b) c)"},
		{"a?.b?.c", "(?. (?. a b) c)"},
		{"a[0][i + 1]", "([] ([] a 0) (+ i 1))"},
		{"a!.b", "(. (! a) b)"},
		{"f(1, x: 2)(3)", "(call (call f 1 x:2) 3)"},
		{"f(type: .email, none: 1)", "(call f type:.email none:1)"},
		{"x.type.default", "(. (. x type) default)"},
		// Elements: a call with a trailing block, an upper-case name with a block.
		{"Text(\"hi\") { color: .red }", `(call Text (str "hi") {*syntax.PropSet<color: .red>})`},
		{"Row { a }", "(call Row {a})"},
		{"ui.Row { a }", "(call (. ui Row) {a})"},
		{"row { a }", "(call row {a})"},
		{"animate(.spring) { a }", "(call animate .spring {a})"},
		{"View as card { a }", "(call View as card {a})"},
		{"Text(\"hi\") as t { a }", `(call Text (str "hi") as t {a})`},
		{"ui.Row as r", "(call (. ui Row) as r)"},
		{"withTimeout(5s) { go() }", "(call withTimeout 5s {(call go)})"},
		{"Boundary { a } on error(e) { b }", "(call Boundary {a} (on error))"},
		// Primaries.
		{".gray", ".gray"},
		{"self.x", "(. self x)"},
		{"(a, b)", "(tuple a b)"},
		{"()", "(tuple)"},
		{"[]", "(array)"},
		{"[1, 2,]", "(array 1 2)"},
		{"[a]", "(array a)"},
		{"[:]", "(map)"},
		{"[a: b]", "(map a:b)"},
		{`["k": 1, "j": 2]`, `(map (str "k"):1 (str "j"):2)`},
		{"[[1]: [2]]", "(map (array 1):(array 2))"},
		{"#fff", "#fff"},
		{"50%", "50%"},
		{"300ms", "300ms"},
		{"true", "true"},
		{"none", "none"},
		{`"a{b + 1}c{d}"`, `(str "a" (+ b 1) "c" d "")`},
		{`"{f("x")}"`, `(str "" (call f (str "x")) "")`},
		{`r"\{x}"`, `(str "\\{x}")`},
		{"\"\"\"\nhi\n\"\"\"", `(str "\nhi\n")`},
		// Lambdas.
		{"x => x + 1", "(=> (x) (+ x 1))"},
		{"_ => 0", "(=> (_) 0)"},
		{"(a, b) => a", "(=> (a b) a)"},
		{"(a: int) => a", "(=> (a:int) a)"},
		{"() => none", "(=> () none)"},
		{"x => { return x }", "(=> (x) {*syntax.ReturnStmt<return x>})"},
		{"(x) => x", "(=> (x) x)"},
		{"(x)", "(paren x)"},
		{"key => key", "(=> (key) key)"},
		{"list.map(t => t.id)", "(call (. list map) (=> (t) (. t id)))"},
		{"async x => await f(x)", "(async=> (x) (await (call f x)))"},
		{"async () => await f()", "(async=> () (await (call f)))"},
		{"async (a, b: int) => { return a }", "(async=> (a b:int) {*syntax.ReturnStmt<return a>})"},
		{"async (x) => x", "(async=> (x) x)"},
		{"run(async () => go())", "(call run (async=> () (call go)))"},
		// if and match as values.
		{"if a { 1 } else { 2 }", "(if a {1} {2})"},
		{"if a { 1 } else if b { 2 } else { 3 }", "(if a {1} (if b {2} {3}))"},
		{"if let u = user { u } else { none }", "(if (let u user) {u} {none})"},
		{"match x { .a => 1, .b(y) if y > 0 => y, _ => 0 }", "(match x [.a => 1] [.b(y) if (> y 0) => y] [_ => 0])"},
		{"match x {\n0 => \"zero\"\n-1 => \"neg\"\n}", `(match x [0 => (str "zero")] [-1 => (str "neg")])`},
		// Condition chains.
		{"if let a = x, let b = y, a > b { a } else { b }", "(if (cond (let a x) (let b y) (> a b)) {a} {b})"},
		{"if a, b { 1 } else { 2 }", "(if (cond a b) {1} {2})"},
		{"if f(a, b), c { 1 } else { 2 }", "(if (cond (call f a b) c) {1} {2})"},
		// Jumps as values (decision 22).
		{"a ?? throw E.missing(id)", "(?? a (throw (call (. E missing) id)))"},
		{"a ?? throw b ?? c", "(?? a (throw (?? b c)))"},
		{"a ?? return", "(?? a (return))"},
		{"a ?? return 1", "(?? a (return 1))"},
		{"f(a ?? return, b ?? break)", "(call f (?? a (return)) (?? b (break)))"},
		{"[a ?? continue]", "(array (?? a (continue)))"},
		{"x => throw E.bad", "(=> (x) (throw (. E bad)))"},
		{"match x { .a => 1, _ => return }", "(match x [.a => 1] [_ => (return)])"},
		// catch as a value (decision 23).
		{"try f() catch { 0 }", "(catch (try (call f)) [catch {0}])"},
		{"try await f() catch e { log(e); 80 }", "(catch (try (await (call f))) [catch e {(call log e); 80}])"},
		{"a + try f() catch { 0 }", "(catch (+ a (try (call f))) [catch {0}])"},
		{"try f() catch E.a { 1 } catch e: T { 2 }", "(catch (try (call f)) [catch E.a {1}] [catch e: T {2}])"},
		{"try f() catch E.a { 1 }\ncatch { 2 }", "(catch (try (call f)) [catch E.a {1}] [catch {2}])"},
		{"x => try g(x) catch { 0 }", "(=> (x) (catch (try (call g x)) [catch {0}]))"},
		{"try withTimeout(5s) { go() } catch { none }", "(catch (try (call withTimeout 5s {(call go)})) [catch {none}])"},
		// Soft keywords are names in expressions.
		{"app.tap(\"Add\")", `(call (. app tap) (str "Add"))`},
		{"type(\"Hi\", into: \"Title\")", `(call type (str "Hi") into:(str "Title"))`},
		{"test + key", "(+ test key)"},
	}
	for _, tt := range tests {
		x, errs := ParseExpr(tt.src)
		if got := sexpr(x); got != tt.want {
			t.Errorf("%q:\n got %s\nwant %s (errors: %v)", tt.src, got, tt.want, errs)
			continue
		}
		bad := strings.Contains(tt.want, "BAD") || tt.want == "row"
		if !bad && len(errs) > 0 {
			t.Errorf("%q: %v", tt.src, errs)
		}
	}
}

func TestParseItems(t *testing.T) {
	tests := []struct{ src, want string }{
		// Members.
		{"prop compact = false", "*syntax.PropDecl<prop compact = false>"},
		{"prop accessory: View? = none", "*syntax.PropDecl<prop accessory: View? = none>"},
		{"prop title: string", "*syntax.PropDecl<prop title: string>"},
		{"pub prop x = 1", "*syntax.PropDecl<pub prop x = 1>"},
		{"event archive", "*syntax.EventDecl<event archive>"},
		{"event changed(value: int)", "*syntax.EventDecl<event changed(value: int)>"},
		{"state count = 0", "*syntax.StateDecl<state count = 0>"},
		{"state sel: Message? = none", "*syntax.StateDecl<state sel: Message? = none>"},
		{"let (a, b) = pair", "*syntax.LetDecl<let (a, b) = pair>"},
		{"let n: int = 1", "*syntax.LetDecl<let n: int = 1>"},
		{"var n = 1", "*syntax.LetDecl<var n = 1>"},
		{"use mail: Mailbox", "*syntax.UseDecl<use mail: Mailbox>"},
		{"provide Theme(dark: true)", "*syntax.ProvideDecl<provide Theme(dark: true)>"},
		{"fn f(a: int) -> int = a", "*syntax.FuncDecl<fn f(a: int) -> int = a>"},
		{"async fn f() throws { }", "*syntax.FuncDecl<async fn f() throws {}>"},
		{"task { go() }", "*syntax.TaskDecl<task {\n  go()\n}>"},
		{"task(a, b.c) { go() }", "*syntax.TaskDecl<task(a, b.c) {\n  go()\n}>"},
		{"effect { log(x) }", "*syntax.EffectDecl<effect {\n  log(x)\n}>"},
		// View items.
		{"gap: 12", "*syntax.PropSet<gap: 12>"},
		{"type: .email", "*syntax.PropSet<type: .email>"},
		{"key: id", "*syntax.PropSet<key: id>"},
		{"on tap { count += 1 }", "*syntax.OnHandler<on tap {\n  count += 1\n}>"},
		{"on changed(a, b) { }", "*syntax.OnHandler<on changed(a, b) {}>"},
		{"bind text: query", "*syntax.BindItem<bind text: query>"},
		{"bind isOn: todos[i].done", "*syntax.BindItem<bind isOn: todos[i].done>"},
		{"style card", "*syntax.StyleUse<style card>"},
		{"style theme.card", "*syntax.StyleUse<style theme.card>"},
		{"style local { padding: 4 }", "*syntax.StyleDecl<style local {\n  padding: 4\n}>"},
		{"toolbar { Button(\"x\") }", "*syntax.SlotItem<toolbar {\n  Button(\"x\")\n}>"},
		{"Row { a }", "(call Row {a})"},
		{"accessory", "accessory"},
		{"if a { A() }", "*syntax.IfStmt<if a {\n  A()\n}>"},
		{"for t in todos key t.id { Row(t) }", "*syntax.ForStmt<for t in todos key t.id {\n  Row(t)\n}>"},
		{"match m { .a => A(), .b => { B() } }", "*syntax.MatchStmt<match m {\n  .a => A()\n  .b => {\n    B()\n  }\n}>"},
		// Statements.
		{"x = 1", "*syntax.AssignStmt<x = 1>"},
		{"todos[i].done = true", "*syntax.AssignStmt<todos[i].done = true>"},
		{"a ??= b", "*syntax.AssignStmt<a ??= b>"},
		{"n %= 2", "*syntax.AssignStmt<n %= 2>"},
		{"while let x = next() { use(x) }", "*syntax.WhileStmt<while let x = next() {\n  use(x)\n}>"},
		{"while let x = next(), x > 0 { }", "*syntax.WhileStmt<while let x = next(), x > 0 {}>"},
		{"let user = cache[id] else { return }", "*syntax.LetDecl<let user = cache[id] else {\n  return\n}>"},
		{"let .ok(v) = result else { throw E.bad }", "*syntax.LetDecl<let .ok(v) = result else {\n  throw E.bad\n}>"},
		{"let u: User = find(id) else { return }", "*syntax.LetDecl<let u: User = find(id) else {\n  return\n}>"},
		{"try save() catch { failed = true }", "(catch (try (call save)) [catch {*syntax.AssignStmt<failed = true>}])"},
		{"x = a ?? return", "*syntax.AssignStmt<x = a ?? return>"},
		{"for await m in stream { }", "*syntax.ForStmt<for await m in stream {}>"},
		{"do { try a() } catch e: HttpError { } catch { }", "*syntax.DoStmt<do {\n  try a()\n} catch e: HttpError {} catch {}>"},
		{"do { } catch MailError.server(code, _) { }", "*syntax.DoStmt<do {} catch MailError.server(code, _) {}>"},
		{"return", "*syntax.ReturnStmt<return>"},
		{"return a + 1", "*syntax.ReturnStmt<return a + 1>"},
		{"throw MailError.offline", "*syntax.ThrowStmt<throw MailError.offline>"},
		{"break", "*syntax.BranchStmt<break>"},
		{"continue", "*syntax.BranchStmt<continue>"},
		{"defer { close() }", "*syntax.DeferStmt<defer {\n  close()\n}>"},
		{"emit archive", "*syntax.EmitStmt<emit archive>"},
		{"emit changed(value: 2)", "*syntax.EmitStmt<emit changed(value: 2)>"},
		{"print(x)", "(call print x)"},
		// Soft keywords as names.
		{"let app = mount(C())", "*syntax.LetDecl<let app = mount(C())>"},
		{"app.tap(\"Add\")", `(call (. app tap) (str "Add"))`},
		{"test.run()", "(call (. test run))"},
		{"let key = 1", "*syntax.LetDecl<let key = 1>"},
	}
	for _, tt := range tests {
		f, errs := ParseFile(NewFileSet(), "t.asuka", []byte("component C {\n"+tt.src+"\n}"))
		if len(errs) > 0 {
			t.Errorf("%q: %v", tt.src, errs)
			continue
		}
		its := f.Decls[0].(*ComponentDecl).Body.Items
		if len(its) != 1 {
			t.Errorf("%q: %d items", tt.src, len(its))
			continue
		}
		if got := sexpr(its[0]); got != tt.want {
			t.Errorf("%q:\n got %s\nwant %s", tt.src, got, tt.want)
		}
	}
}

func TestParseDecls(t *testing.T) {
	tests := []struct{ src, want string }{
		{"import std/http", "*syntax.ImportDecl"},
		{"import ./components.{Row, Card} as c", "*syntax.ImportDecl"},
		{"component Row<T>(item: T, n: int = 0) { }", "*syntax.ComponentDecl"},
		{"pub screen Inbox(mailbox: Mailbox) { title: \"Inbox\" }", "*syntax.ScreenDecl"},
		{"store Mailbox { state n = 0 }", "*syntax.StoreDecl"},
		{"struct Box<T>: Equatable, Hashable { value: T\n fn get() -> T = value }", "*syntax.StructDecl"},
		{"enum E: Error { a, b(int), c(code: int, String)\n fn f() { } }", "*syntax.EnumDecl"},
		{"interface Named { name: string\n fn describe() -> string }", "*syntax.InterfaceDecl"},
		{"fn id<T: Hashable>(x: T) -> T = x", "*syntax.FuncDecl"},
		{"fn sort<T: Shape & Hashable, U>(x: T) -> T = x", "*syntax.FuncDecl"},
		{"type Pair<A, B> = (A, B)", "*syntax.TypeAliasDecl"},
		{"style card { padding: 16 }", "*syntax.StyleDecl"},
		{"state theme: Theme = .system", "*syntax.StateDecl"},
		{"let total = items.count", "*syntax.LetDecl"},
		{"native interface Haptics { fn impact(style: Impact) }", "*syntax.InterfaceDecl"},
		{"native component MapView(region: Region) { prop pins: [Pin] = []; event moved(r: Region) }", "*syntax.ComponentDecl"},
		{"app { Counter() }", "*syntax.AppDecl"},
		{"test \"counts\" { await idle() }", "*syntax.TestDecl"},
	}
	for _, tt := range tests {
		f := parseOK(t, tt.src)
		var n Node
		if len(f.Imports) > 0 {
			n = f.Imports[0]
		} else {
			n = f.Decls[0]
		}
		if got := fmt.Sprintf("%T", n); got != tt.want {
			t.Errorf("%q: %s, want %s", tt.src, got, tt.want)
		}
	}

	f := parseOK(t, "import ./a/b.{X, Y} as z")
	if d := f.Imports[0]; !d.Relative || d.PathString() != "./a/b" || len(d.Names) != 2 || d.Alias.Name != "z" {
		t.Errorf("import: %+v", d)
	}
	f = parseOK(t, "import @acme/line-charts as lc\nimport @acme/charts.v2\nimport charts2")
	for i, want := range []string{"@acme/line-charts", "@acme/charts.v2", "charts2"} {
		if got := f.Imports[i].PathString(); got != want {
			t.Errorf("import %d: %q, want %q", i, got, want)
		}
	}
	if f.Imports[0].Alias == nil || f.Imports[0].Alias.Name != "lc" {
		t.Errorf("scoped import's alias: %+v", f.Imports[0])
	}
	f = parseOK(t, "enum E: Error { a, b(int), c(code: int, String)\n fn f() { } }")
	if e := f.Decls[0].(*EnumDecl); len(e.Cases) != 3 || len(e.Members) != 1 || e.Cases[2].Params.List[0].Label.Name != "code" || e.Cases[2].Params.List[1].Label != nil {
		t.Errorf("enum: %s", printed(e))
	}
	f = parseOK(t, "struct Font { size: float = 0\n let title = Font(size: 28)\n fn bold() -> Font = self }\nenum Track { auto, fr(float)\n let one = Track.fr(1) }")
	if s := f.Decls[0].(*StructDecl); len(s.Members) != 3 {
		t.Errorf("struct constants: %s", printed(s))
	} else if _, ok := s.Members[1].(*LetDecl); !ok {
		t.Errorf("struct constant: %T", s.Members[1])
	}
	if e := f.Decls[1].(*EnumDecl); len(e.Members) != 1 {
		t.Errorf("enum constants: %s", printed(e))
	}
	f = parseOK(t, "native component MapView(region: Region) { prop pins: [Pin] = []; event moved(r: Region) }")
	if c := f.Decls[0].(*ComponentDecl); !c.Native.IsValid() || len(c.Body.Items) != 2 {
		t.Errorf("native component: %s", printed(c))
	}
	f = parseOK(t, "/// Doc one.\n/// Doc two.\npub fn f() {}")
	if d := f.Decls[0].(*FuncDecl); d.Doc() != "Doc one.\nDoc two." || !d.Pub.IsValid() {
		t.Errorf("doc %q", d.Doc())
	}
}

func TestParseTypes(t *testing.T) {
	tests := []struct{ src, want string }{
		{"int", "int"},
		{"http.Response<T>", "http.Response<T>"},
		{"[Todo]", "[Todo]"},
		{"[string: [int]]", "[string: [int]]"},
		{"(int, string)", "(int, string)"},
		{"()", "()"},
		{"(int)", "int"},
		{"String?", "String?"},
		{"[T?]?", "[T?]?"},
		{"Map<K, Loading<V>>", "Map<K, Loading<V>>"},
		{"fn(int) -> bool", "fn(int) -> bool"},
		{"async fn() throws -> [User]", "async fn() throws -> [User]"},
		{"(fn() -> int)?", "(fn() -> int)?"},
		{"fn() -> int?", "fn() -> int?"},
		{"T??", "T??"},
	}
	for _, tt := range tests {
		f := parseOK(t, "type X = "+tt.src)
		if got := printed(f.Decls[0].(*TypeAliasDecl).Type); got != tt.want {
			t.Errorf("%q: %s, want %s", tt.src, got, tt.want)
		}
	}
}

func TestParsePatterns(t *testing.T) {
	tests := []struct{ src, want string }{
		{"_", "*syntax.WildcardPat"},
		{"x", "*syntax.BindPat"},
		{"0", "*syntax.LitPat"},
		{"-1", "*syntax.LitPat"},
		{`"x"`, "*syntax.LitPat"},
		{"none", "*syntax.LitPat"},
		{".ready(v)", "*syntax.CasePat"},
		{".pending", "*syntax.CasePat"},
		{"Mail.received(m, _)", "*syntax.CasePat"},
		{"HttpError.notFound", "*syntax.CasePat"},
		{"(a, (b, _))", "*syntax.TuplePat"},
		{".a | .b | .c(x)", "*syntax.OrPat"},
		{"Point(x: 0, y)", "*syntax.CasePat"},
		{"Line(from: Point(x: 0), to: p, dashed: true)", "*syntax.CasePat"},
		{"geo.Point(x: _)", "*syntax.CasePat"},
		{".server(code: c, retry)", "*syntax.CasePat"},
		{"Point()", "*syntax.CasePat"},
	}
	for _, tt := range tests {
		f := parseOK(t, "fn f() {\nmatch v {\n"+tt.src+" => 1\n}\n}")
		arm := f.Decls[0].(*FuncDecl).Body.Items[0].(*MatchStmt).Arms[0]
		if got := fmt.Sprintf("%T", arm.Pattern); got != tt.want {
			t.Errorf("%q: %s, want %s", tt.src, got, tt.want)
		}
		if got := printed(arm.Pattern); got != tt.src {
			t.Errorf("%q printed %q", tt.src, got)
		}
	}
}

// TestParseDecisions covers what parser.go's comment decides.
func TestParseDecisions(t *testing.T) {
	// 2: name: value is a prop set, even after an element.
	its := items(t, "Text(\"a\")\ncolor: .red")
	if _, ok := its[1].(*PropSet); !ok {
		t.Errorf("prop set: %T", its[1])
	}
	// 3: a lower-case name and a block is a slot; an upper-case one an element.
	its = items(t, "toolbar { Button(\"x\") }\nToolbar { Button(\"x\") }")
	if _, ok := its[0].(*SlotItem); !ok {
		t.Errorf("slot: %T", its[0])
	}
	if c, ok := its[1].(*ExprStmt).X.(*CallExpr); !ok || c.Block == nil || c.Args != nil {
		t.Errorf("element: %s", sexpr(its[1]))
	}
	// 4: a trailing block binds only on the call's line.
	f, errs := ParseFile(NewFileSet(), "t.asuka", []byte("component C {\nText(\"a\")\n{ }\n}"))
	if c := f.Decls[0].(*ComponentDecl).Body.Items[0].(*ExprStmt).X.(*CallExpr); c.Block != nil {
		t.Errorf("a block on the next line bound to the call")
	}
	if len(errs) == 0 || !strings.Contains(errs[0].Msg, "expected an expression, found '{'") {
		t.Errorf("a block on a line of its own: %v", errs)
	}
	// 4: not in an if's, a for's or a match's head; again inside parentheses.
	its = items(t, "if isEmpty { A() }\nfor x in Items { B() }\nmatch Mode { _ => C() }\nif f(Row { a }) { D() }")
	if c := its[0].(*IfStmt); sexpr(c.Cond) != "isEmpty" || len(c.Then.Items) != 1 {
		t.Errorf("if head: %s", printed(c))
	}
	if c := its[1].(*ForStmt); sexpr(c.Seq) != "Items" {
		t.Errorf("for head: %s", printed(c))
	}
	if c := its[2].(*MatchStmt); sexpr(c.Subject) != "Mode" {
		t.Errorf("match head: %s", printed(c))
	}
	if c := its[3].(*IfStmt); sexpr(c.Cond) != "(call f (call Row {a}))" {
		t.Errorf("parenthesized head: %s", sexpr(c.Cond))
	}
	// 5: handlers after a trailing block belong to the call.
	its = items(t, "Boundary { A() } on error(e) { log(e) }")
	if c := its[0].(*ExprStmt).X.(*CallExpr); len(c.Handlers) != 1 || c.Handlers[0].Params[0].Name != "e" {
		t.Errorf("handlers: %s", sexpr(c))
	}
	// 6: else and catch may start the next line.
	its = items(t, "if a { A() }\nelse { B() }\ndo { x() }\ncatch { y() }")
	if len(its) != 2 || its[0].(*IfStmt).Else == nil || len(its[1].(*DoStmt).Catches) != 1 {
		t.Errorf("else/catch on the next line: %d items", len(its))
	}
	// 9: try? only with the ? right after try.
	if x, _ := ParseExpr("try? f()"); !x.(*UnaryExpr).Optional {
		t.Errorf("try?")
	}
	// 9: x! only right after x; x !y isn't an unwrap.
	its = items(t, "a!\nb")
	if _, ok := its[0].(*ExprStmt).X.(*UnwrapExpr); !ok || len(its) != 2 {
		t.Errorf("unwrap: %v", sexpr(its[0]))
	}
	// 12: an if as a value needs an else; as an item it doesn't.
	if _, errs := ParseExpr("if a { 1 }"); len(errs) == 0 {
		t.Errorf("an if value without else parsed")
	}
	// 14: an enum's cases on lines, or comma-separated, with keyword names.
	f = parseOK(t, "enum E {\na\nb, c\nnone\n}")
	if n := len(f.Decls[0].(*EnumDecl).Cases); n != 4 {
		t.Errorf("%d cases", n)
	}
	// 16: emit's arguments.
	its = items(t, "emit changed(1)\nemit done")
	if its[0].(*EmitStmt).Args == nil || its[1].(*EmitStmt).Args != nil {
		t.Errorf("emit")
	}
	// Match arms: guards, commas, blocks.
	its = items(t, "match x {\n.a(n) if n > 0 => A(), .b => B()\n_ => { C() }\n}")
	m := its[0].(*MatchStmt)
	if len(m.Arms) != 3 || m.Arms[0].Guard == nil {
		t.Errorf("arms: %s", printed(m))
	}
	if _, ok := m.Arms[2].Body.(*Block); !ok {
		t.Errorf("an arm's block: %T", m.Arms[2].Body)
	}
	// A match arm's body may be an element with its block.
	its = items(t, "match x {\n.ready(l) => List {\nfor i in l key i.id { Row(i) }\n}\n}")
	if c, ok := its[0].(*MatchStmt).Arms[0].Body.(*ExprStmt).X.(*CallExpr); !ok || c.Block == nil {
		t.Errorf("an arm's element")
	}
	// Statements may be separated by ;.
	its = items(t, "a = 1; b = 2;c()")
	if len(its) != 3 {
		t.Errorf("%d items", len(its))
	}
}

// TestSpans checks every node of the corpus has a span inside its
// parent's, in order, and that Walk reaches every kind it should.
func TestSpans(t *testing.T) {
	for _, name := range corpus(t) {
		f, src := parseFile(t, name)
		var stack []Node
		Inspect(f, func(n Node) bool {
			if n == nil {
				stack = stack[:len(stack)-1]
				return false
			}
			r := n.Range()
			if !r.Start.IsValid() || r.End < r.Start {
				t.Errorf("%s: %T has span %v", name, n, r)
			}
			if len(stack) > 0 {
				pr := stack[len(stack)-1].Range()
				if r.Start < pr.Start || r.End > pr.End {
					t.Errorf("%s: %T %s outside its parent %T %s", name, n, spanText(f, r), stack[len(stack)-1], spanText(f, pr))
				}
			}
			if id, ok := n.(*Ident); ok {
				off := f.File.Offset(id.Start)
				if got := string(src[off : off+len(id.Name)]); got != id.Name {
					t.Errorf("%s: ident %q spans %q", name, id.Name, got)
				}
			}
			stack = append(stack, n)
			return true
		})
	}
}

func spanText(f *SourceFile, s Span) string {
	return f.File.Position(s.Start).String() + "-" + f.File.Position(s.End).String()
}

// f<T>(…) is a call with type arguments; a < b and a > c stay comparisons.
func TestTypeArguments(t *testing.T) {
	for _, c := range []struct{ src, want string }{
		{"let x = json.decode<[Item]>(s)", "json.decode<[Item]>(s)"},
		{"let x = r.json<[string: Item?]>()", "r.json<[string: Item?]>()"},
		{"let x = pair<int, string>(1, \"a\")", "pair<int, string>(1, \"a\")"},
		{"let x = a < b && c > d", "a < b && c > d"},
		{"let x = a < b", "a < b"},
		{"let x = f(a < b, c > d)", "f(a < b, c > d)"},
		// As C#: <...> right before ( is type arguments.
		{"let x = f(a < b, c > (d))", "f(a<b, c>(d))"},
	} {
		f := parseOK(t, c.src)
		got := printed(f.Decls[0].(*LetDecl).Value)
		if got != c.want {
			t.Errorf("%s: printed %q, want %q", c.src, got, c.want)
		}
	}
}
