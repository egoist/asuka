package syntax

import (
	"bytes"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
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
		if msg := roundTrip(name, src, out); msg != "" {
			t.Error(msg)
		}
	}
}

// TestFormatRepo formats every .asuka file in the repository that
// parses (the checker's error cases have syntax errors of their own):
// formatting keeps its meaning and its comments, and is stable. A file
// outside testdata (whose inputs may be unformatted on purpose) is
// formatted already: asuka fmt -w formats it.
func TestFormatRepo(t *testing.T) {
	n := 0
	for _, path := range repoFiles(t) {
		src, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		out, err := Format(path, src)
		if err != nil {
			if !inTestdata(path) {
				t.Errorf("%s: %v", path, err)
			}
			continue
		}
		n++
		if msg := roundTrip(path, src, out); msg != "" {
			t.Error(msg)
		}
		if !inTestdata(path) && !bytes.Equal(out, src) {
			t.Errorf("%s isn't formatted (asuka fmt -w); first difference:\n%s", path, firstDiff(string(out), string(src)))
		}
	}
	if n == 0 {
		t.Fatal("no .asuka files")
	}
}

func inTestdata(path string) bool {
	return slices.Contains(strings.Split(filepath.ToSlash(path), "/"), "testdata")
}

// repoFiles are the repository's .asuka files.
func repoFiles(t testing.TB) []string {
	root := filepath.Join("..", "..")
	var files []string
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			if path != root && (strings.HasPrefix(d.Name(), ".") || d.Name() == "node_modules" || d.Name() == "dist") {
				return filepath.SkipDir
			}
			return nil
		}
		if strings.HasSuffix(path, ".asuka") {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return files
}

// roundTrip checks out, src formatted: it formats to itself again, has
// every comment of src, and parses to src's tree (comments aside, and
// the form style: replaced rewritten, as the printer does). It returns
// what's wrong, or "".
func roundTrip(name string, src, out []byte) string {
	again, err := Format(name, out)
	if err != nil {
		return fmt.Sprintf("%s: the output doesn't parse: %v\n%s", name, err, out)
	}
	if !bytes.Equal(again, out) {
		return fmt.Sprintf("%s: not idempotent on %q; first difference:\n%s", name, src, firstDiff(string(again), string(out)))
	}
	f, _ := ParseFile(NewFileSet(), name, src)
	for _, c := range f.Comments {
		text := strings.ReplaceAll(strings.TrimRight(c.Text, " \t\r"), "\r\n", "\n")
		if !bytes.Contains(out, []byte(text)) {
			return fmt.Sprintf("%s: comment %q lost from %q", name, c.Text, src)
		}
	}
	Inspect(f, func(n Node) bool {
		if b, ok := n.(*Block); ok {
			b.Items = styleUses(b.Items)
		}
		return n != nil
	})
	g, _ := ParseFile(NewFileSet(), name, out)
	if a, b := shape(f), shape(g); a != b {
		return fmt.Sprintf("%s: formatting %q changed the tree:\n%s", name, src, firstDiff(b, a))
	}
	return ""
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
		{`let s = "{ price :>8.2f}|{n:#x}|{ name }"`, "let s = \"{price:>8.2f}|{n:#x}|{name}\"\n"},
		{"let r=0 ..< n", "let r = 0..<n\n"},
		{"let r = a .. b", "let r = a..b\n"},
		{"let r = a .. .b.c(1) + 1", "let r = a .. .b.c(1) + 1\n"},
		{"let r = a..<.b", "let r = a ..< .b\n"},
		{"let o = a ?? b", "let o = a ?? b\n"},
		{"let f = ( a , b ) => a", "let f = (a, b) => a\n"},
		{"let f = async  ( a , b ) => a", "let f = async (a, b) => a\n"},
		{"let f = async x=>await g(x)", "let f = async x => await g(x)\n"},
		{"let f = async()=>{ await g() }", "let f = async () => { await g() }\n"},
		{"let t = try  await f()", "let t = try await f()\n"},
		{"let n = ! a", "let n = !a\n"},
		{"let n = - -a + -(-b) - - c", "let n = - -a + -(-b) - -c\n"},
		// A % length before (, [ or . keeps its space: 0%() is 0 % ().
		{"let n = 0% () + 5% [1] + 5% .a + 5%?.a + 5%!", "let n = 0% () + 5% [1] + 5% .a + 5%?.a + 5%!\n"},
		{"let d = @platform==.linux||( @platform != .web )", "let d = @platform == .linux || (@platform != .web)\n"},
		{"let u = @define( \"API_URL\" ,\"x\" )", "let u = @define(\"API_URL\", \"x\")\n"},
		{"fn f() { match p { Point( x :0 ,y )=>1 } }", "fn f() { match p { Point(x: 0, y) => 1 } }\n"},
		{"fn f() { match s { c :Circle=>1; _ :  ui.Square => 2 } }", "fn f() { match s { c: Circle => 1, _: ui.Square => 2 } }\n"},
		{"fn f() { if let c :Circle=s, let x : int? = c.r { } }", "fn f() { if let c: Circle = s, let x: int? = c.r {} }\n"},
		{"fn f() { let c :Circle=s else { return } }", "fn f() { let c: Circle = s else { return } }\n"},
		{"fn f<T:Shape&Hashable,U : ui.View>(x: T) {}", "fn f<T: Shape & Hashable, U: ui.View>(x: T) {}\n"},
		{"let n = ~ a&b|c^d<<1>>2", "let n = ~a & b | c ^ d << 1 >> 2\n"},
		{"let n = (a + b) << (c & d)", "let n = (a + b) << (c & d)\n"},
		{"let n = a + (b << c)", "let n = a + (b << c)\n"},
		{"let n = 0b1010_0101 | 0o17", "let n = 0b1010_0101 | 0o17\n"},
		{"let h = (h^b)&*p&+1 &- -c", "let h = (h ^ b) &* p &+ 1 &- -c\n"},
		{"fn f() {\n  h&+=1; h&-=2; h&*=3\n}", "fn f() {\n  h &+= 1; h &-= 2; h &*= 3\n}\n"},
		{"let n = -a**2 + (-a)**2", "let n = -a ** 2 + (-a) ** 2\n"},
		{"let n = (a**b)**c * a**(b**c) * 2**-1", "let n = (a ** b) ** c * a ** (b ** c) * 2 ** -1\n"},
		{"let n = (a*b)**2 + -(a**2)", "let n = (a * b) ** 2 + -(a ** 2)\n"},
		{"let n = try f()**2 + a ** b**c", "let n = try f() ** 2 + a ** b ** c\n"},
		{"fn f() {\n  x**=2\n}", "fn f() {\n  x **= 2\n}\n"},
		{"fn f() {\n  x>>=1; x<<=2; x&=3; x|=4; x^=5\n}", "fn f() {\n  x >>= 1; x <<= 2; x &= 3; x |= 4; x ^= 5\n}\n"},
		// A break after a dot stays after it: a line that ends in . goes on
		// where one that starts with .0 is a number, and .name alone on a
		// value block's last line is its value.
		{"test \"\" {\n  0.\n  0\n}", "test \"\" {\n  0.\n    0\n}\n"},
		{"fn f() -> string = if c {\n  x.\n  name\n} else { \"\" }", "fn f() -> string = if c {\n  x.\n    name\n} else { \"\" }\n"},
		{"let n = items?.\n  first", "let n = items?.\n  first\n"},
		{"let m = [ 1 : 2 ]", "let m = [1: 2]\n"},
		{"let m = [ : ]", "let m = [:]\n"},
		{"type F = async fn( int )throws->bool?", "type F = async fn(int) throws -> bool?\n"},
		{"import ./a .{ X,Y }as z", "import ./a.{X, Y} as z\n"},
		{"import @acme/line-charts  as lc", "import @acme/line-charts as lc\n"},
		{"pub  native  async fn  f( a:int )throws->int", "pub native async fn f(a: int) throws -> int\n"},
		{"struct T { native fn  g()->int }", "struct T { native fn g() -> int }\n"},
		{"pub native ( crypto_pk )fn  f( a:int )->int", "pub native(crypto_pk) fn f(a: int) -> int\n"},
		// Items: a line each, but those the source had on one line stay
		// so, and a block written on one line stays on one.
		{"component C {\n  prop a = 1\n  event e; Text(\"x\")\n}", "component C {\n  prop a = 1\n  event e; Text(\"x\")\n}\n"},
		{"component C { prop a = 1; event e; Text(\"x\") }", "component C { prop a = 1; event e; Text(\"x\") }\n"},
		{"component C { Text(\"x\") }", "component C { Text(\"x\") }\n"},
		{"component C {\n  prop on=false; pub  variant  on=on&&!x\n  View { variant: 1; gap: 2 }\n}", "component C {\n  prop on = false; pub variant on = on && !x\n  View { variant: 1; gap: 2 }\n}\n"},
		{"component C {\n  a(); Row {\n    b()\n  }\n}", "component C {\n  a()\n  Row {\n    b()\n  }\n}\n"},
		{"struct P { x: int; y: int }", "struct P { x: int; y: int }\n"},
		{"component C { variant(1) }", "component C { variant(1) }\n"},
		{"component C {\n}", "component C {}\n"},
		{"component C {\n\n\n  state a = 1\n\n\n\n  state b = 2\n\n}", "component C {\n  state a = 1\n\n  state b = 2\n}\n"},
		{"fn f() {\n  if a { b() }\n  else { c() }\n}", "fn f() {\n  if a { b() } else { c() }\n}\n"},
		{"fn f() {\n  do { a() }\n  catch { b() }\n}", "fn f() {\n  do { a() } catch { b() }\n}\n"},
		// A one-line block whose item spans lines breaks too.
		{"component C { Button(\"x\") { on press { a(1 /* x */, 2) } } }", "component C {\n  Button(\"x\") {\n    on press {\n      a(\n        1, /* x */\n        2,\n      )\n    }\n  }\n}\n"},
		{"let x = match v { .a => () => { a(1 /* x */, 2) } }", "let x = match v {\n  .a => () => {\n    a(\n      1, /* x */\n      2,\n    )\n  }\n}\n"},
		// Lists.
		// A line break after a comma stays, as gofmt keeps it; elements
		// on one line of a list a line each stay on it.
		{"let a = f(1,\n  2)", "let a = f(1,\n  2)\n"},
		{"let a = f(1, 2,\n         3, g(4,\n    5), 6)", "let a = f(1, 2,\n  3, g(4,\n    5), 6)\n"},
		{"let a = f(\n  1, 2)", "let a = f(\n  1, 2,\n)\n"},
		{"let m = [\n  1, 0,\n  0, 1]", "let m = [\n  1, 0,\n  0, 1,\n]\n"},
		{"let a = [\n1]", "let a = [\n  1,\n]\n"},
		{"let a = [\n  1,\n\n\n  2]", "let a = [\n  1,\n\n  2,\n]\n"},
		{"fn f(\n  a: int,\n\n  b: int) {}", "fn f(\n  a: int,\n\n  b: int,\n) {}\n"},
		{"let t = (\n  1, 2)", "let t = (\n  1, 2,\n)\n"},
		{"let t = (1,\n  2)", "let t = (1,\n  2)\n"},
		{"let t = (1, // one\n  2)", "let t = (\n  1, // one\n  2,\n)\n"},
		{"let t = (\n  1,\n)", "let t = (\n  1,\n)\n"},
		{"let t = ( 1 , )", "let t = (1,)\n"},
		{"import ./a.{\n  b, // first\n  c}", "import ./a.{\n  b, // first\n  c,\n}\n"},
		// Type parameters and arguments, case patterns' arguments, a
		// handler's parameters, a task's dependencies, tuple and function
		// types, tuple patterns, a closure's parameters: as arguments.
		{"struct Page<\n  T, U> { items: [T] }", "struct Page<\n  T, U,\n> { items: [T] }\n"},
		{"struct Page<T, // the item\n  U: Hashable> {}", "struct Page<\n  T, // the item\n  U: Hashable,\n> {}\n"},
		{"fn f<T,\n  U>(x: T) {}", "fn f<T,\n  U>(x: T) {}\n"},
		{"fn f<\n  T\n>(x: T) {}", "fn f<\n  T,\n>(x: T) {}\n"},
		{"type M = Map<string, /* values */ int>", "type M = Map<\n  string, /* values */\n  int,\n>\n"},
		{"let x = decode<\n  [Item]>(s)", "let x = decode<\n  [Item],\n>(s)\n"},
		{"fn f() { match p { Point(x: 0, // origin\n  y) => 1 } }", "fn f() {\n  match p {\n    Point(\n      x: 0, // origin\n      y,\n    ) => 1\n  }\n}\n"},
		{"fn f() { match p { .some(\n  v) => 1 } }", "fn f() {\n  match p {\n    .some(\n      v,\n    ) => 1\n  }\n}\n"},
		{"fn f() { let (a, // first\n  b) = t }", "fn f() {\n  let (\n    a, // first\n    b,\n  ) = t\n}\n"},
		{"component C { Toggle { on change(\n  on) { x = on } } }", "component C {\n  Toggle {\n    on change(\n      on,\n    ) { x = on }\n  }\n}\n"},
		{"component C { task(a, /* and */ b) { n += 1 } }", "component C {\n  task(\n    a, /* and */\n    b,\n  ) { n += 1 }\n}\n"},
		{"type P = (int, // x\n  int)", "type P = (\n  int, // x\n  int,\n)\n"},
		{"type F = fn(\n  int) -> bool", "type F = fn(\n  int,\n) -> bool\n"},
		{"type F = fn(int, /* b */ int)", "type F = fn(\n  int, /* b */\n  int,\n)\n"},
		{"let f = (a, // first\n  b) => a", "let f = (\n  a, // first\n  b,\n) => a\n"},
		{"import ./a.{b,\n  c} as x", "import ./a.{b,\n  c} as x\n"},
		{"import ./a.{ /* none */ }", "import ./a.{\n  /* none */\n}\n"},
		{"enum E {\n  a, b\n}", "enum E {\n  a, b\n}\n"},
		{"enum E {\n  a\n  b; fn f() {}\n}", "enum E {\n  a\n  b\n  fn f() {}\n}\n"},
		{"enum E { a, b }", "enum E { a, b }\n"},
		{"let x = match v {\n  .a => 1, .b => 2\n}", "let x = match v {\n  .a => 1, .b => 2\n}\n"},
		// A line break after =, : or => stays.
		{"fn f() -> int =\n      g()", "fn f() -> int =\n  g()\n"},
		{"fn f() {\n  let x: int = // why\n    g()\n  x +=\n    1\n}", "fn f() {\n  let x: int = // why\n    g()\n  x +=\n    1\n}\n"},
		{"component C {\n  title:\n    \"Inbox\"\n}", "component C {\n  title:\n    \"Inbox\"\n}\n"},
		{"let f = x =>\n  x + 1", "let f = x =>\n  x + 1\n"},
		// Comments after the code of consecutive lines line up.
		{"struct S {\n  a: int = 0 // one\n  bcd: string = \"\"   // two\n\n  e: int // three\n  f: int\n  g: int // four\n}", "struct S {\n  a: int = 0       // one\n  bcd: string = \"\" // two\n\n  e: int // three\n  f: int\n  g: int // four\n}\n"},
		{"enum E {\n  a // one\n  bb // two\n  c(int) // three\n}", "enum E {\n  a      // one\n  bb     // two\n  c(int) // three\n}\n"},
		{"let xs = [\n  1, // one\n  22, // two\n]", "let xs = [\n  1,  // one\n  22, // two\n]\n"},
		{"fn f() {\n  a() // x\n  // own line\n  bb() // y\n}", "fn f() {\n  a() // x\n  // own line\n  bb() // y\n}\n"},
		{"fn f() {\n  let é = 1 // x\n  let ab = 2 // y\n}", "fn f() {\n  let é = 1  // x\n  let ab = 2 // y\n}\n"},
		// A binary expression broken after its operator.
		{"let ok = a &&\n        b ||\n  c", "let ok = a &&\n  b ||\n  c\n"},
		// Comments.
		{"// a\n\n\n// b\nlet x = 1 // c\n// d", "// a\n\n// b\nlet x = 1 // c\n// d\n"},
		{"let x = 1 +   // why\n  2", "let x = 1 + // why\n  2\n"},
		{"let x = f(1 /* one */, 2)", "let x = f(\n  1, /* one */\n  2,\n)\n"},
		{"fn f() {\n  let x = 1\n  // last\n}", "fn f() {\n  let x = 1\n  // last\n}\n"},
		{"fn f() { // why\n  let x = 1\n}", "fn f() { // why\n  let x = 1\n}\n"},
		{"/// Doc.\nfn f() {}  ", "/// Doc.\nfn f() {}\n"},
		// A ; on a line of its own is a blank line: the comment above
		// stays above what follows.
		{"// a\n;\nlet x = 1", "// a\n\nlet x = 1\n"},
		{"//\n;", "//\n"},
		// A comment after a node that ends on a later line is no blank
		// line before the next.
		{"let a = 1 /* multi\nline */\nlet b = 2", "let a = 1 /* multi\nline */\nlet b = 2\n"},
		{"fn f() {\n  a() /* x\n  */\n\n  b()\n}", "fn f() {\n  a() /* x\n  */\n\n  b()\n}\n"},
		// An empty map's comments go above its line.
		{"let m = [/* e */ :]", "/* e */\nlet m = [:]\n"},
		{"fn f() {\n  g([ // none\n  :])\n}", "fn f() {\n  // none\n  g([:])\n}\n"},
		{"fn f() {\n  let x = [1, // one\n    2]\n}", "fn f() {\n  let x = [\n    1, // one\n    2,\n  ]\n}\n"},
		// A file with Windows's line endings prints with \n, in its
		// block comments and """ strings too.
		{"fn f() {\r\n  /* a\r\n  b */\r\n  let s = \"\"\"\r\n    x {y}\r\n    \"\"\" // t\r\n}\r\n", "fn f() {\n  /* a\n  b */\n  let s = \"\"\"\n    x {y}\n    \"\"\" // t\n}\n"},
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

// TestFormatNestedOneLine formats blocks written on one line, nested
// 100 deep, the innermost too long for one: each is tried on one line
// once, so it ends (trying each again when the one around it breaks
// was 2^100 tries).
func TestFormatNestedOneLine(t *testing.T) {
	const depth = 100
	src := "component C { " + strings.Repeat("Row { ", depth) + "a(1 /* one */, 2)" + strings.Repeat(" }", depth) + " }"
	out, err := Format("t.asuka", []byte(src))
	if err != nil {
		t.Fatal(err)
	}
	if n := strings.Count(string(out), "\n"); n != 2*depth+6 {
		t.Errorf("%d lines, want %d:\n%s", n, 2*depth+6, out)
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
