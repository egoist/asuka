package syntax

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestFormatCorpus: the corpus is formatted already, so it prints
// unchanged, and parse → print → parse → print is stable.
func TestFormatCorpus(t *testing.T) {
	for _, name := range corpus(t) {
		src, err := os.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		out, err := Format(name, src)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(out, src) {
			t.Errorf("%s isn't printed unchanged; first difference:\n%s", name, firstDiff(string(out), string(src)))
		}
	}
}

// TestFormatGolden formats testdata/format/*.input and compares with
// its .golden (go test -update rewrites them); formatting the result
// again changes nothing, and every comment of the input is kept.
func TestFormatGolden(t *testing.T) {
	inputs, _ := filepath.Glob("testdata/format/*.input")
	if len(inputs) == 0 {
		t.Fatal("no testdata/format/*.input")
	}
	for _, name := range inputs {
		src, err := os.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		out, err := Format(name, src)
		if err != nil {
			t.Fatal(err)
		}
		golden := strings.TrimSuffix(name, ".input") + ".golden"
		if *update {
			if err := os.WriteFile(golden, out, 0o644); err != nil {
				t.Fatal(err)
			}
		} else if want, err := os.ReadFile(golden); err != nil {
			t.Fatalf("%v (go test -update writes it)", err)
		} else if !bytes.Equal(out, want) {
			t.Errorf("%s: the output differs from %s; first difference:\n%s", name, golden, firstDiff(string(out), string(want)))
		}
		again, err := Format(name, out)
		if err != nil {
			t.Fatalf("%s: the output doesn't parse: %v\n%s", name, err, out)
		}
		if !bytes.Equal(again, out) {
			t.Errorf("%s: not idempotent; first difference:\n%s", name, firstDiff(string(again), string(out)))
		}
		f, _ := ParseFile(NewFileSet(), name, src)
		for _, c := range f.Comments {
			if !bytes.Contains(out, []byte(strings.TrimRight(c.Text, " \t"))) {
				t.Errorf("%s: comment %q lost", name, c.Text)
			}
		}
		// The trees match, comments aside (and the form style: replaced
		// rewritten, as the printer does).
		Inspect(f, func(n Node) bool {
			if b, ok := n.(*Block); ok {
				b.Items = styleUses(b.Items)
			}
			return n != nil
		})
		g, _ := ParseFile(NewFileSet(), name, out)
		if a, b := shape(f), shape(g); a != b {
			t.Errorf("%s: formatting changed the tree:\n%s", name, firstDiff(b, a))
		}
	}
}

// shape is a tree's kinds and names, without positions or comments.
func shape(n Node) string {
	var b strings.Builder
	depth := 0
	Inspect(n, func(n Node) bool {
		if n == nil {
			depth--
			return false
		}
		b.WriteString(strings.Repeat(" ", depth))
		switch x := n.(type) {
		case *Ident:
			b.WriteString(x.Name)
		case *BasicLit:
			b.WriteString(x.Value)
		case *BinaryExpr:
			b.WriteString(x.Op.String())
		case *StringLit:
			b.WriteString(strings.Join(x.Segments, "{}"))
		default:
			b.WriteString(strings.TrimPrefix(fmt.Sprintf("%T", n), "*syntax."))
		}
		b.WriteByte('\n')
		depth++
		return true
	})
	return b.String()
}

// TestFormatCases formats small sources and compares with their
// canonical forms.
func TestFormatCases(t *testing.T) {
	tests := []struct{ src, want string }{
		// Spacing.
		{"let x=a+b*c", "let x = a + b * c\n"},
		{"let r=0 ..< n", "let r = 0..<n\n"},
		{"let r = a .. b", "let r = a..b\n"},
		{"let o = a ?? b", "let o = a ?? b\n"},
		{"let f = ( a , b ) => a", "let f = (a, b) => a\n"},
		{"let f = async  ( a , b ) => a", "let f = async (a, b) => a\n"},
		{"let f = async x=>await g(x)", "let f = async x => await g(x)\n"},
		{"let f = async()=>{ await g() }", "let f = async () => { await g() }\n"},
		{"let t = try  await f()", "let t = try await f()\n"},
		{"let n = ! a", "let n = !a\n"},
		{"let d = @platform==.linux||( @platform != .web )", "let d = @platform == .linux || (@platform != .web)\n"},
		{"fn f() { match p { Point( x :0 ,y )=>1 } }", "fn f() { match p { Point(x: 0, y) => 1 } }\n"},
		{"fn f<T:Shape&Hashable,U : ui.View>(x: T) {}", "fn f<T: Shape & Hashable, U: ui.View>(x: T) {}\n"},
		{"let n = ~ a&b|c^d<<1>>2", "let n = ~a & b | c ^ d << 1 >> 2\n"},
		{"let n = (a + b) << (c & d)", "let n = (a + b) << (c & d)\n"},
		{"let n = a + (b << c)", "let n = a + (b << c)\n"},
		{"let n = 0b1010_0101 | 0o17", "let n = 0b1010_0101 | 0o17\n"},
		{"fn f() { x>>=1; x<<=2; x&=3; x|=4; x^=5 }", "fn f() {\n  x >>= 1\n  x <<= 2\n  x &= 3\n  x |= 4\n  x ^= 5\n}\n"},
		{"let m = [ 1 : 2 ]", "let m = [1: 2]\n"},
		{"let m = [ : ]", "let m = [:]\n"},
		{"type F = async fn( int )throws->bool?", "type F = async fn(int) throws -> bool?\n"},
		{"import ./a .{ X,Y }as z", "import ./a.{X, Y} as z\n"},
		{"import @acme/line-charts  as lc", "import @acme/line-charts as lc\n"},
		{"pub  native  async fn  f( a:int )throws->int", "pub native async fn f(a: int) throws -> int\n"},
		{"struct T { native fn  g()->int }", "struct T { native fn g() -> int }\n"},
		// Items, one per line.
		{"component C { prop a = 1; event e; Text(\"x\") }", "component C {\n  prop a = 1\n  event e\n  Text(\"x\")\n}\n"},
		{"component C { Text(\"x\") }", "component C { Text(\"x\") }\n"},
		{"component C {\n}", "component C {}\n"},
		{"component C {\n\n\n  state a = 1\n\n\n\n  state b = 2\n\n}", "component C {\n  state a = 1\n\n  state b = 2\n}\n"},
		{"fn f() {\n  if a { b() }\n  else { c() }\n}", "fn f() {\n  if a { b() } else { c() }\n}\n"},
		{"fn f() {\n  do { a() }\n  catch { b() }\n}", "fn f() {\n  do { a() } catch { b() }\n}\n"},
		// Lists.
		{"let a = f(1,\n  2)", "let a = f(1, 2)\n"},
		{"let a = f(\n  1, 2)", "let a = f(\n  1,\n  2,\n)\n"},
		{"let a = [\n1]", "let a = [\n  1,\n]\n"},
		{"enum E {\n  a, b\n}", "enum E {\n  a\n  b\n}\n"},
		{"enum E { a, b }", "enum E { a, b }\n"},
		{"let x = match v {\n  .a => 1, .b => 2\n}", "let x = match v {\n  .a => 1\n  .b => 2\n}\n"},
		// A binary expression broken after its operator.
		{"let ok = a &&\n        b ||\n  c", "let ok = a &&\n  b ||\n  c\n"},
		// Comments.
		{"// a\n\n\n// b\nlet x = 1 // c\n// d", "// a\n\n// b\nlet x = 1 // c\n// d\n"},
		{"let x = 1 +   // why\n  2", "let x = 1 + // why\n  2\n"},
		{"let x = f(1 /* one */, 2)", "let x = f(\n  1, /* one */\n  2,\n)\n"},
		{"fn f() {\n  let x = 1\n  // last\n}", "fn f() {\n  let x = 1\n  // last\n}\n"},
		{"fn f() { // why\n  let x = 1\n}", "fn f() { // why\n  let x = 1\n}\n"},
		{"/// Doc.\nfn f() {}  ", "/// Doc.\nfn f() {}\n"},
		{"fn f() {\n  let x = [1, // one\n    2]\n}", "fn f() {\n  let x = [\n    1, // one\n    2,\n  ]\n}\n"},
		// Strings print as written.
		{"let s = \"a\\n{b}\\{c\"", "let s = \"a\\n{b}\\{c\"\n"},
		{"let s = r\"\\d+\"", "let s = r\"\\d+\"\n"},
		{"let s = \"\"\"\n    x\n  \"\"\"", "let s = \"\"\"\n    x\n  \"\"\"\n"},
		{"", ""},
		{"// only\n", "// only\n"},
	}
	for _, tt := range tests {
		out, err := Format("t.asuka", []byte(tt.src))
		if err != nil {
			t.Errorf("%q: %v", tt.src, err)
			continue
		}
		if string(out) != tt.want {
			t.Errorf("%q:\n got %q\nwant %q", tt.src, out, tt.want)
			continue
		}
		if again, _ := Format("t.asuka", out); !bytes.Equal(again, out) {
			t.Errorf("%q: not idempotent: %q", tt.src, again)
		}
	}
}

// TestFprintMadeTree prints a tree made in code, with no positions:
// parentheses where precedence needs them, lists on one line.
func TestFprintMadeTree(t *testing.T) {
	id := func(s string) *Ident { return &Ident{Name: s} }
	sum := &BinaryExpr{X: id("a"), Op: ADD, Y: id("b")}
	tree := &SourceFile{Decls: []Decl{
		&LetDecl{Keyword: LET, Pattern: &BindPat{Name: id("x")}, Value: &BinaryExpr{X: sum, Op: MUL, Y: &BasicLit{Kind: INT, Value: "2"}}},
		&LetDecl{Keyword: LET, Pattern: &BindPat{Name: id("y")}, Value: &BinaryExpr{X: id("a"), Op: SUB, Y: &BinaryExpr{X: id("b"), Op: SUB, Y: id("c")}}},
		&LetDecl{Keyword: LET, Pattern: &BindPat{Name: id("z")}, Value: &MemberExpr{X: &UnaryExpr{Op: NOT, X: id("a")}, Name: id("b")}},
		&ComponentDecl{Name: id("C"), Body: &Block{Items: []Item{
			&PropSet{Name: id("gap"), Value: &BasicLit{Kind: INT, Value: "8"}},
			&ExprStmt{X: &CallExpr{Fun: id("Text"), Args: &ArgList{List: []*Arg{{Value: &StringLit{Segments: []string{"hi"}}}}}}},
		}}},
		&TypeAliasDecl{Name: id("F"), Type: &OptionalType{Elem: &FuncType{Result: &NamedType{Path: []*Ident{id("int")}}}}},
	}}
	var b bytes.Buffer
	if err := Fprint(&b, nil, tree); err != nil {
		t.Fatal(err)
	}
	want := "let x = (a + b) * 2\nlet y = a - (b - c)\nlet z = (!a).b\ncomponent C {\n  gap: 8\n  Text(\"hi\")\n}\ntype F = (fn() -> int)?\n"
	if b.String() != want {
		t.Errorf("got\n%s\nwant\n%s", b.String(), want)
	}
	if _, err := Format("made.asuka", b.Bytes()); err != nil {
		t.Errorf("doesn't parse: %v", err)
	}
}

func BenchmarkParse(b *testing.B) {
	src := corpusSource(b)
	b.SetBytes(int64(len(src)))
	b.ReportAllocs()
	for b.Loop() {
		if _, errs := ParseFile(NewFileSet(), "bench.asuka", src); len(errs) > 0 {
			b.Fatal(errs)
		}
	}
}

func BenchmarkScan(b *testing.B) {
	src := corpusSource(b)
	b.SetBytes(int64(len(src)))
	b.ReportAllocs()
	for b.Loop() {
		file := NewFileSet().AddFile("bench.asuka", len(src))
		var s Scanner
		s.Init(file, src, nil)
		for {
			if _, tok, _ := s.Scan(); tok == EOF {
				break
			}
		}
	}
}

func BenchmarkFormat(b *testing.B) {
	src := corpusSource(b)
	b.SetBytes(int64(len(src)))
	b.ReportAllocs()
	for b.Loop() {
		if _, err := Format("bench.asuka", src); err != nil {
			b.Fatal(err)
		}
	}
}

// corpusSource is the corpus as one file: its declarations, its
// imports dropped, eight times over.
func corpusSource(b *testing.B) []byte {
	var src bytes.Buffer
	for range 8 {
		for _, name := range corpus(b) {
			data, err := os.ReadFile(name)
			if err != nil {
				b.Fatal(err)
			}
			for _, line := range strings.SplitAfter(string(data), "\n") {
				if !strings.HasPrefix(line, "import ") {
					src.WriteString(line)
				}
			}
			src.WriteString("\n")
		}
	}
	return src.Bytes()
}
