package syntax

// The printer: a tree back to source, in the one canonical form asuka
// fmt writes.
//
//   - Two spaces of indentation; one member, item, field, case or arm
//     per line; prop sets as name: value; spaces around binary
//     operators, none around .. and ..< (0..<n), none inside brackets.
//   - A block, a struct's or an enum's body, a match's arms stay on one
//     line when they were written on one line and hold one item (an
//     enum: only cases; a match: arms separated by commas) and no
//     comment: on tap { count += 1 }. Otherwise each item goes on a line
//     of its own, and an empty block is {}.
//   - Arguments, parameters, arrays and maps go one per line, each with
//     a trailing comma, when the first was written on the line after
//     the opening bracket or a comment sits between them; otherwise on
//     one line. A binary expression broken after its operator stays
//     broken there, its next lines indented once more.
//   - } else and } catch go on the }'s line. Imports' .{…} follow the
//     path directly: import ./components.{Row, Card}.
//   - Comments are kept: above the node that follows them, after the
//     node on its line, or at the end of a block. A comment inside an
//     expression that can't stay where it was (a // in a binary
//     expression joined on one line) moves above the line it was on.
//     Blank lines between items are kept, at most one in a row; none
//     after an opening brace or before a closing one.
//
// Printing a file the printer wrote gives the same bytes again.

import (
	"bytes"
	"io"
	"sort"
	"strings"
)

// Format parses src as the file filename and returns it in canonical
// form. A file with syntax errors isn't formatted: Format returns its
// ErrorList.
func Format(filename string, src []byte) ([]byte, error) {
	fset := NewFileSet()
	f, errs := ParseFile(fset, filename, src)
	if len(errs) > 0 {
		return nil, errs
	}
	var b bytes.Buffer
	if err := Fprint(&b, f.File, f); err != nil {
		return nil, err
	}
	return b.Bytes(), nil
}

// Fprint writes node to w in canonical form. file has node's positions,
// which say where the source broke lists across lines and left blank
// lines; with a nil file (a tree made in code) lists go on one line.
// A *SourceFile prints with all its comments, any other node with the
// comments its Trivia hold.
func Fprint(w io.Writer, file *File, node Node) error {
	p := &printer{file: file, printed: map[*Comment]bool{}}
	if f, ok := node.(*SourceFile); ok {
		if p.file == nil {
			p.file = f.File
		}
		p.comments = f.Comments
		p.sourceFile(f)
	} else {
		p.node(node)
	}
	_, err := w.Write(p.out)
	return err
}

type printer struct {
	out      []byte
	file     *File
	comments []*Comment // the file's, in source order
	printed  map[*Comment]bool
	indent   int
	bol      bool // at the start of a line, its indentation not written yet
}

// ---------------------------------------------------------------------
// Output

func (p *printer) write(s string) {
	if p.bol {
		for range p.indent {
			p.out = append(p.out, "  "...)
		}
		p.bol = false
	}
	p.out = append(p.out, s...)
}

func (p *printer) newline() {
	p.out = append(p.out, '\n')
	p.bol = true
}

func (p *printer) comment(c *Comment) {
	text := c.Text
	if strings.HasPrefix(text, "//") {
		text = strings.TrimRight(text, " \t\r")
	}
	p.write(text)
	p.printed[c] = true
}

// ---------------------------------------------------------------------
// Positions and comments

func (p *printer) line(pos Pos) int {
	if p.file == nil || !pos.IsValid() || int(pos) < p.file.Base() || int(pos) > p.file.Base()+p.file.Size() {
		return 0
	}
	return p.file.Line(pos)
}

// sameLine reports whether a and b were on one line of the source (no
// when there's no source).
func (p *printer) sameLine(a, b Pos) bool {
	la, lb := p.line(a), p.line(b)
	return la > 0 && la == lb
}

// endLine is the line of a node's last character.
func (p *printer) endLine(n Node) int {
	end := n.Range().End
	if !end.IsValid() {
		return 0
	}
	return p.line(end - 1)
}

// firstComment is the index of the first comment at or after pos.
func (p *printer) firstComment(pos Pos) int {
	return sort.Search(len(p.comments), func(i int) bool { return p.comments[i].Pos >= pos })
}

// commentsIn reports whether a comment, printed or not, is in [lo, hi).
func (p *printer) commentsIn(lo, hi Pos) bool {
	i := p.firstComment(lo)
	return i < len(p.comments) && p.comments[i].Pos < hi
}

// unprinted are the comments in [lo, hi) not printed yet.
func (p *printer) unprinted(lo, hi Pos) []*Comment {
	var list []*Comment
	for i := p.firstComment(lo); i < len(p.comments) && p.comments[i].Pos < hi; i++ {
		if c := p.comments[i]; !p.printed[c] {
			list = append(list, c)
		}
	}
	return list
}

// lineNode prints a node that starts a line with print, then moves any
// comment inside it that print didn't place above it.
func (p *printer) lineNode(n Node, print func()) {
	p.write("")
	mark := len(p.out)
	print()
	r := n.Range()
	left := p.unprinted(r.Start, r.End)
	if len(left) == 0 {
		return
	}
	tail := append([]byte(nil), p.out[mark:]...)
	p.out = p.out[:mark]
	for _, c := range left {
		p.comment(c)
		p.newline()
		p.write("")
	}
	p.out = append(p.out, tail...)
}

// ownLines prints the unprinted comments in [lo, hi), each on its own
// line, before what comes next on the current line.
func (p *printer) ownLines(lo, hi Pos) {
	for _, c := range p.unprinted(lo, hi) {
		p.comment(c)
		p.newline()
	}
}

// leading prints the comments above a node, and the blank lines
// between them.
func (p *printer) leading(t *Trivia) {
	if t == nil {
		return
	}
	for gi, g := range t.Leading {
		if gi > 0 && g.BlankBefore {
			p.newline()
		}
		for _, c := range g.List {
			p.comment(c)
			p.newline()
		}
	}
	if len(t.Leading) > 0 && t.BlankBefore {
		p.newline()
	}
}

// blankBefore reports whether a blank line goes above a node and its
// comments.
func blankBefore(t *Trivia) bool {
	if t == nil {
		return false
	}
	if len(t.Leading) > 0 {
		return t.Leading[0].BlankBefore
	}
	return t.BlankBefore
}

// trailing prints the comment after a node on its line: its Trivia's, or
// one the tree keeps nowhere, before limit.
func (p *printer) trailing(t *Trivia, n Node, limit Pos) {
	if t != nil && t.Trailing != nil {
		if !p.printed[t.Trailing] {
			p.write(" ")
			p.comment(t.Trailing)
		}
		return
	}
	end := n.Range().End
	if !end.IsValid() {
		return
	}
	if !limit.IsValid() {
		limit = Pos(1 << 62)
	}
	l := p.line(end - 1)
	for _, c := range p.unprinted(end, limit) {
		if p.line(c.Pos) != l {
			break
		}
		p.write(" ")
		p.comment(c)
	}
}

// ---------------------------------------------------------------------
// Lists

// lines prints nodes one per line after an opening bracket at open and
// before its closer at close (NoPos when unknown, as for a file), with
// the comments around them; sep follows each node (, in lists). In a
// bracket (inside), the first line starts on a line of its own.
func (p *printer) lines(inside bool, open Pos, nodes []Node, dangling []*CommentGroup, close Pos, sep string) {
	prev := open
	for i, n := range nodes {
		t := TriviaOf(n)
		if i > 0 || inside {
			p.newline()
		}
		if i > 0 && blankBefore(t) {
			p.newline()
		}
		if prev.IsValid() {
			if t != nil {
				for _, g := range t.Leading {
					for _, c := range g.List {
						p.printed[c] = true // printed next, by leading
					}
				}
			}
			p.ownLines(prev, n.Range().Start)
		}
		p.leading(t)
		p.lineNode(n, func() { p.node(n) })
		p.write(sep)
		limit := close
		if i+1 < len(nodes) {
			limit = nodes[i+1].Range().Start
		}
		p.trailing(t, n, limit)
		prev = n.Range().End
	}
	for gi, g := range dangling {
		if len(nodes) > 0 || gi > 0 || inside {
			p.newline()
		}
		if (len(nodes) > 0 || gi > 0) && g.BlankBefore {
			p.newline()
		}
		for ci, c := range g.List {
			if ci > 0 {
				p.newline()
			}
			p.comment(c)
		}
	}
	if prev.IsValid() && close.IsValid() {
		for _, c := range p.unprinted(prev, close) {
			p.newline()
			p.comment(c)
		}
	}
}

// multiline reports whether a bracketed list goes one element per line:
// its first element was on a line after the opening bracket, or a
// comment sits between its elements.
func (p *printer) multiline(open, close Pos, elems []Node) bool {
	if len(elems) == 0 {
		return p.commentsIn(open, close)
	}
	if l := p.line(open); l > 0 && l != p.line(elems[0].Range().Start) {
		return true
	}
	for _, e := range elems {
		if t := TriviaOf(e); t != nil && t.HasComments() {
			return true
		}
	}
	// A comment between the elements, not inside one.
	lo := open
	for _, e := range elems {
		if p.commentsIn(lo, e.Range().Start) {
			return true
		}
		lo = e.Range().End
	}
	return p.commentsIn(lo, close)
}

// list prints elements between the brackets openTok and closeTok,
// separated by commas, on one line or one per line.
func (p *printer) list(openTok, closeTok string, open, close Pos, elems []Node) {
	p.write(openTok)
	if p.multiline(open, close, elems) {
		p.indent++
		p.lines(true, open, elems, nil, close, ",")
		p.indent--
		p.newline()
		p.write(closeTok)
		return
	}
	for i, e := range elems {
		if i > 0 {
			p.write(", ")
		}
		p.node(e)
	}
	p.write(closeTok)
}

// body prints { nodes } as a block's, a struct's or an enum's body.
// oneLine says it may stay on one line, sep how to separate the nodes
// there.
func (p *printer) body(lbrace Pos, comment *Comment, nodes []Node, dangling []*CommentGroup, rbrace Pos, oneLine bool, sep string) {
	if len(nodes) == 0 && len(dangling) == 0 && comment == nil && !p.commentsIn(lbrace, rbrace) {
		p.write("{}")
		return
	}
	if oneLine && len(nodes) > 0 && comment == nil && len(dangling) == 0 &&
		p.sameLine(lbrace, rbrace) && !p.commentsIn(lbrace, rbrace+1) {
		ok := true
		for _, n := range nodes {
			if t := TriviaOf(n); t != nil && t.HasComments() {
				ok = false
			}
		}
		if ok {
			p.write("{ ")
			for i, n := range nodes {
				if i > 0 {
					p.write(sep)
				}
				p.node(n)
			}
			p.write(" }")
			return
		}
	}
	p.write("{")
	if comment != nil {
		p.write(" ")
		p.comment(comment)
	}
	p.indent++
	p.lines(true, lbrace, nodes, dangling, rbrace, "")
	p.indent--
	p.newline()
	p.write("}")
}

func (p *printer) block(b *Block) {
	if b == nil {
		p.write("{}")
		return
	}
	p.body(b.Lbrace, b.Comment, itemNodes(b.Items), b.Dangling, b.Rbrace, len(b.Items) == 1, "")
}

func itemNodes(items []Item) []Node {
	nodes := make([]Node, len(items))
	for i, it := range items {
		nodes[i] = it
	}
	return nodes
}

func exprNodes(xs []Expr) []Node {
	nodes := make([]Node, len(xs))
	for i, x := range xs {
		nodes[i] = x
	}
	return nodes
}

// ---------------------------------------------------------------------
// Files and declarations

func (p *printer) sourceFile(f *SourceFile) {
	var nodes []Node
	for _, d := range f.Imports {
		nodes = append(nodes, d)
	}
	for _, d := range f.Decls {
		nodes = append(nodes, d)
	}
	p.lines(false, NoPos, nodes, f.Dangling, NoPos, "")
	// Anything left (there shouldn't be): at the end, never lost.
	for _, c := range p.comments {
		if !p.printed[c] {
			p.newline()
			p.comment(c)
		}
	}
	if len(p.out) > 0 {
		p.newline()
	}
}

func (p *printer) pub(pos Pos) {
	if pos.IsValid() {
		p.write("pub ")
	}
}

func (p *printer) typeParams(list []*TypeParam) {
	if len(list) == 0 {
		return
	}
	p.write("<")
	for i, tp := range list {
		if i > 0 {
			p.write(", ")
		}
		p.write(tp.Name.Name)
		for j, t := range tp.Constraints {
			if j == 0 {
				p.write(": ")
			} else {
				p.write(" & ")
			}
			p.typ(t)
		}
	}
	p.write(">")
}

func (p *printer) params(l *ParamList) {
	if l == nil {
		return
	}
	nodes := make([]Node, len(l.List))
	for i, x := range l.List {
		nodes[i] = x
	}
	p.list("(", ")", l.Start, l.End-1, nodes)
}

func (p *printer) conforms(list []Type) {
	for i, t := range list {
		if i == 0 {
			p.write(": ")
		} else {
			p.write(", ")
		}
		p.typ(t)
	}
}

func (p *printer) funcDecl(d *FuncDecl) {
	p.pub(d.Pub)
	if d.Static.IsValid() {
		p.write("static ")
	}
	if d.Native.IsValid() {
		p.write("native ")
	}
	if d.Extern.IsValid() {
		p.write("extern ")
	}
	if d.Async.IsValid() {
		p.write("async ")
	}
	p.write("fn " + d.Name.Name)
	p.typeParams(d.TypeParams)
	if d.Params != nil {
		p.params(d.Params)
	} else {
		p.write("()")
	}
	if d.Throws.IsValid() {
		p.write(" throws")
	}
	if d.Result != nil {
		p.write(" -> ")
		p.typ(d.Result)
	}
	switch {
	case d.Body != nil:
		p.write(" ")
		p.block(d.Body)
	case d.Value != nil:
		p.write(" = ")
		p.expr(d.Value)
	}
}

// node prints any node.
func (p *printer) node(n Node) {
	switch n := n.(type) {
	case nil:
	case *SourceFile:
		p.sourceFile(n)
	case *ImportDecl:
		p.write("import ")
		if n.Up > 0 {
			p.write(strings.Repeat("../", n.Up))
		} else if n.Relative {
			p.write("./")
		}
		for i, s := range n.Path {
			if i > 0 {
				p.write("/")
			}
			p.write(s.Name)
		}
		if n.Names != nil {
			p.write(".{")
			for i, id := range n.Names {
				if i > 0 {
					p.write(", ")
				}
				p.write(id.Name)
			}
			p.write("}")
		}
		if n.Alias != nil {
			p.write(" as " + n.Alias.Name)
		}

	// Declarations.
	case *ComponentDecl:
		p.pub(n.Pub)
		if n.Native.IsValid() {
			p.write("native ")
		}
		p.write("component " + n.Name.Name)
		p.typeParams(n.TypeParams)
		p.params(n.Params)
		p.write(" ")
		p.block(n.Body)
	case *ScreenDecl:
		p.pub(n.Pub)
		p.write("screen " + n.Name.Name)
		p.params(n.Params)
		if n.At != nil {
			p.write(" at ")
			p.expr(n.At)
		}
		p.write(" ")
		p.block(n.Body)
	case *StoreDecl:
		p.pub(n.Pub)
		p.write("store " + n.Name.Name)
		p.typeParams(n.TypeParams)
		p.params(n.Params)
		p.write(" ")
		p.block(n.Body)
	case *StructDecl:
		p.pub(n.Pub)
		if n.Extern.IsValid() {
			p.write("extern ")
		}
		p.write("struct " + n.Name.Name)
		p.typeParams(n.TypeParams)
		p.conforms(n.Conforms)
		p.write(" ")
		p.body(n.Lbrace, nil, n.Members, n.Dangling, n.Rbrace, len(n.Members) == 1, "")
	case *Field:
		p.pub(n.Pub)
		p.write(n.Name.Name + ": ")
		p.typ(n.Type)
		if n.Default != nil {
			p.write(" = ")
			p.expr(n.Default)
		}
	case *EnumDecl:
		p.pub(n.Pub)
		p.write("enum " + n.Name.Name)
		p.typeParams(n.TypeParams)
		p.conforms(n.Conforms)
		p.write(" ")
		var nodes []Node
		for _, c := range n.Cases {
			nodes = append(nodes, c)
		}
		nodes = append(nodes, n.Members...)
		p.body(n.Lbrace, nil, nodes, n.Dangling, n.Rbrace, len(n.Members) == 0, ", ")
	case *EnumCase:
		p.write(n.Name.Name)
		if n.Params != nil {
			nodes := make([]Node, len(n.Params.List))
			for i, x := range n.Params.List {
				nodes[i] = x
			}
			p.list("(", ")", n.Params.Start, n.Params.End-1, nodes)
		}
	case *CaseParam:
		if n.Label != nil {
			p.write(n.Label.Name + ": ")
		}
		p.typ(n.Type)
	case *InterfaceDecl:
		p.pub(n.Pub)
		if n.Native.IsValid() {
			p.write("native ")
		}
		p.write("interface " + n.Name.Name)
		p.typeParams(n.TypeParams)
		p.write(" ")
		p.body(n.Lbrace, nil, n.Members, n.Dangling, n.Rbrace, len(n.Members) == 1, "")
	case *FuncDecl:
		p.funcDecl(n)
	case *ExternTypeDecl:
		p.pub(n.Pub)
		p.write("extern type " + n.Name.Name)
		if n.Release != nil {
			p.write(" with release " + n.Release.Name)
		}
	case *TypeAliasDecl:
		p.pub(n.Pub)
		p.write("type " + n.Name.Name)
		p.typeParams(n.TypeParams)
		p.write(" = ")
		p.typ(n.Type)
	case *StyleDecl:
		p.pub(n.Pub)
		p.write("style " + n.Name.Name + " ")
		p.block(n.Body)
	case *StateDecl:
		p.pub(n.Pub)
		p.write("state " + n.Name.Name)
		if n.Type != nil {
			p.write(": ")
			p.typ(n.Type)
		}
		if n.Value != nil {
			p.write(" = ")
			p.expr(n.Value)
		}
	case *LetDecl:
		p.pub(n.Pub)
		p.write(n.Keyword.String() + " ")
		p.pattern(n.Pattern)
		if n.Type != nil {
			p.write(": ")
			p.typ(n.Type)
		}
		if n.Value != nil {
			p.write(" = ")
			p.expr(n.Value)
		}
		if n.Else != nil {
			p.write(" else ")
			p.block(n.Else)
		}
	case *AppDecl:
		p.pub(n.Pub)
		p.write("app ")
		p.block(n.Body)
	case *TestDecl:
		p.write("test ")
		p.expr(n.Name)
		p.write(" ")
		p.block(n.Body)
	case *TypeParam:
		p.typeParams([]*TypeParam{n})
	case *Param:
		p.write(n.Name.Name)
		if n.Type != nil {
			p.write(": ")
			p.typ(n.Type)
		}
		if n.Default != nil {
			p.write(" = ")
			p.expr(n.Default)
		}
	case *ParamList:
		p.params(n)

	// Items.
	case *Block:
		p.block(n)
	case *PropDecl:
		p.pub(n.Pub)
		p.write("prop " + n.Name.Name)
		if n.Type != nil {
			p.write(": ")
			p.typ(n.Type)
		}
		if n.Default != nil {
			p.write(" = ")
			p.expr(n.Default)
		}
	case *EventDecl:
		p.pub(n.Pub)
		p.write("event " + n.Name.Name)
		p.params(n.Params)
	case *UseDecl:
		p.write("use " + n.Name.Name + ": ")
		p.typ(n.Type)
	case *ProvideDecl:
		p.write("provide ")
		p.expr(n.Value)
	case *TaskDecl:
		p.write("task")
		if n.Lparen.IsValid() || len(n.Deps) > 0 {
			p.write("(")
			for i, d := range n.Deps {
				if i > 0 {
					p.write(", ")
				}
				p.expr(d)
			}
			p.write(")")
		}
		p.write(" ")
		p.block(n.Body)
	case *EffectDecl:
		p.write("effect ")
		p.block(n.Body)
	case *PropSet:
		p.write(n.Name.Name + ": ")
		p.expr(n.Value)
	case *OnHandler:
		p.onHandler(n)
	case *BindItem:
		p.write("bind " + n.Name.Name + ": ")
		p.expr(n.Target)
	case *StyleUse:
		p.write("style ")
		p.expr(n.Name)
	case *SlotItem:
		p.write(n.Name.Name + " ")
		p.block(n.Body)
	case *IfStmt:
		p.write("if ")
		p.cond(n.Cond)
		p.write(" ")
		p.block(n.Then)
		if n.Else != nil {
			p.write(" else ")
			p.node(n.Else)
		}
	case *ForStmt:
		p.write("for ")
		if n.Await.IsValid() {
			p.write("await ")
		}
		p.pattern(n.Pattern)
		p.write(" in ")
		p.expr(n.Seq)
		if n.Key != nil {
			p.write(" key ")
			p.expr(n.Key)
		}
		p.write(" ")
		p.block(n.Body)
	case *WhileStmt:
		p.write("while ")
		p.cond(n.Cond)
		p.write(" ")
		p.block(n.Body)
	case *MatchStmt:
		p.write("match ")
		p.expr(n.Subject)
		p.write(" ")
		p.arms(n.Lbrace, n.Arms, n.Rbrace)
	case *MatchArm:
		p.pattern(n.Pattern)
		if n.Guard != nil {
			p.write(" if ")
			p.expr(n.Guard)
		}
		p.write(" => ")
		if x, ok := n.Body.(Expr); ok {
			p.expr(x)
		} else {
			p.node(n.Body)
		}
	case *DoStmt:
		p.write("do ")
		p.block(n.Body)
		for _, c := range n.Catches {
			p.write(" ")
			p.node(c)
		}
	case *CatchClause:
		p.write("catch ")
		if n.Pattern != nil {
			p.pattern(n.Pattern)
			if n.Type != nil {
				p.write(": ")
				p.typ(n.Type)
			}
			p.write(" ")
		}
		p.block(n.Body)
	case *ReturnStmt:
		p.write("return")
		if n.Value != nil {
			p.write(" ")
			p.expr(n.Value)
		}
	case *ThrowStmt:
		p.write("throw ")
		p.expr(n.Value)
	case *BranchStmt:
		p.write(n.Tok.String())
	case *DeferStmt:
		p.write("defer ")
		p.block(n.Body)
	case *EmitStmt:
		p.write("emit " + n.Name.Name)
		if n.Args != nil {
			p.args(n.Args)
		}
	case *AssignStmt:
		p.expr(n.Target)
		p.write(" " + n.Op.String() + " ")
		p.expr(n.Value)
	case *ExprStmt:
		p.expr(n.X)
	case *BadDecl, *BadItem:
		p.write("/* bad syntax */")

	case *Arg:
		if n.Label != nil {
			p.write(n.Label.Name + ": ")
		}
		p.expr(n.Value)
	case *ArgList:
		p.args(n)
	case *MapEntry:
		p.expr(n.Key)
		p.write(": ")
		p.expr(n.Value)
	case *CaseParams:
		nodes := make([]Node, len(n.List))
		for i, x := range n.List {
			nodes[i] = x
		}
		p.list("(", ")", n.Start, n.End-1, nodes)

	case Expr:
		p.expr(n)
	case Type:
		p.typ(n)
	case Pattern:
		p.pattern(n)
	}
}

func (p *printer) onHandler(n *OnHandler) {
	p.write("on " + n.Name.Name)
	if n.Lparen.IsValid() || n.Params != nil {
		p.write("(")
		for i, id := range n.Params {
			if i > 0 {
				p.write(", ")
			}
			p.write(id.Name)
		}
		p.write(")")
	}
	p.write(" ")
	p.block(n.Body)
}

// cond prints an if's or a while's condition.
func (p *printer) cond(x Expr) {
	if l, ok := x.(*CondList); ok {
		for i, c := range l.List {
			if i > 0 {
				p.write(", ")
			}
			p.cond(c)
		}
		return
	}
	if c, ok := x.(*LetCond); ok {
		p.write("let ")
		p.pattern(c.Pattern)
		p.write(" = ")
		p.expr(c.Value)
		return
	}
	p.expr(x)
}

// arms prints a match's { arms }: on one line, separated by commas,
// when they were written so.
func (p *printer) arms(lbrace Pos, arms []*MatchArm, rbrace Pos) {
	nodes := make([]Node, len(arms))
	oneLine := true
	for i, a := range arms {
		nodes[i] = a
		if b, ok := a.Body.(*Block); ok && !p.sameLine(b.Lbrace, b.Rbrace) {
			oneLine = false
		}
	}
	p.body(lbrace, nil, nodes, nil, rbrace, oneLine, ", ")
}

func (p *printer) args(l *ArgList) {
	nodes := make([]Node, len(l.List))
	for i, a := range l.List {
		nodes[i] = a
	}
	p.list("(", ")", l.Start, l.End-1, nodes)
}

// ---------------------------------------------------------------------
// Expressions

// Operands' precedences: what binds tighter than every binary operator.
const (
	precLambda  = 0
	precUnary   = 9
	precPostfix = 10
)

func exprPrec(x Expr) int {
	switch x := x.(type) {
	case *LambdaExpr:
		return precLambda
	case *BinaryExpr:
		return x.Op.Precedence()
	case *UnaryExpr, *SpawnExpr, *JumpExpr:
		// A jump takes the rest of the expression, so the parser makes
		// one an operand only on the right, where it needs no
		// parentheses.
		return precUnary
	case *CatchExpr:
		return precLambda
	}
	return precPostfix
}

func (p *printer) expr(x Expr) { p.exprPrec(x, precLambda) }

// exprPrec prints x where an operand of precedence prec or tighter
// goes, in parentheses when x binds looser.
func (p *printer) exprPrec(x Expr, prec int) {
	if exprPrec(x) < prec {
		p.write("(")
		p.expr1(x)
		p.write(")")
		return
	}
	p.expr1(x)
}

func (p *printer) expr1(x Expr) {
	switch x := x.(type) {
	case nil:
	case *Ident:
		p.write(x.Name)
	case *BasicLit:
		p.write(x.Value)
	case *StringLit:
		p.stringLit(x)
	case *BoolLit:
		if x.Value {
			p.write("true")
		} else {
			p.write("false")
		}
	case *NoneLit:
		p.write("none")
	case *SelfExpr:
		p.write("self")
	case *ImplicitMember:
		p.write("." + x.Name.Name)
	case *MemberExpr:
		p.exprPrec(x.X, precPostfix)
		if x.Optional {
			p.write("?.")
		} else {
			p.write(".")
		}
		p.write(x.Name.Name)
	case *CallExpr:
		p.exprPrec(x.Fun, precPostfix)
		if len(x.TypeArgs) > 0 {
			p.write("<")
			for i, t := range x.TypeArgs {
				if i > 0 {
					p.write(", ")
				}
				p.typ(t)
			}
			p.write(">")
		}
		if x.Args != nil {
			p.args(x.Args)
		}
		if x.As != nil {
			p.write(" as " + x.As.Name)
		}
		if x.Block != nil {
			p.write(" ")
			p.block(x.Block)
		}
		for _, h := range x.Handlers {
			p.write(" ")
			p.onHandler(h)
		}
	case *IndexExpr:
		p.exprPrec(x.X, precPostfix)
		p.write("[")
		p.expr(x.Index)
		p.write("]")
	case *UnwrapExpr:
		p.exprPrec(x.X, precPostfix)
		p.write("!")
	case *UnaryExpr:
		switch x.Op {
		case NOT, SUB, TILDE:
			p.write(x.Op.String())
		case TRY:
			if x.Optional {
				p.write("try? ")
			} else {
				p.write("try ")
			}
		default:
			p.write(x.Op.String() + " ")
		}
		if x.Op == LOAD {
			p.exprPrec(x.X, precPostfix)
		} else {
			p.exprPrec(x.X, precUnary)
		}
	case *SpawnExpr:
		p.write("spawn ")
		p.block(x.Body)
	case *BinaryExpr:
		cont := false
		p.binary(x, &cont)
		if cont {
			p.indent--
		}
	case *LambdaExpr:
		if x.Async.IsValid() {
			p.write("async ")
		}
		if !x.Parens && len(x.Params) == 1 && x.Params[0].Type == nil && x.Params[0].Default == nil {
			p.write(x.Params[0].Name.Name)
		} else {
			p.write("(")
			for i, prm := range x.Params {
				if i > 0 {
					p.write(", ")
				}
				p.node(prm)
			}
			p.write(")")
		}
		p.write(" => ")
		switch b := x.Body.(type) {
		case *Block:
			p.block(b)
		case Expr:
			p.expr(b)
		}
	case *ParenExpr:
		p.write("(")
		p.expr(x.X)
		p.write(")")
	case *TupleExpr:
		p.write("(")
		for i, e := range x.Elems {
			if i > 0 {
				p.write(", ")
			}
			p.expr(e)
		}
		if len(x.Elems) == 1 {
			p.write(",")
		}
		p.write(")")
	case *ArrayExpr:
		p.list("[", "]", x.Start, x.End-1, exprNodes(x.Elems))
	case *MapExpr:
		if len(x.Entries) == 0 && !p.commentsIn(x.Start, x.End) {
			p.write("[:]")
			return
		}
		nodes := make([]Node, len(x.Entries))
		for i, e := range x.Entries {
			nodes[i] = e
		}
		p.list("[", "]", x.Start, x.End-1, nodes)
	case *IfExpr:
		p.write("if ")
		p.cond(x.Cond)
		p.write(" ")
		p.block(x.Then)
		if x.Else != nil {
			p.write(" else ")
			switch e := x.Else.(type) {
			case *Block:
				p.block(e)
			case Expr:
				p.expr(e)
			}
		}
	case *MatchExpr:
		p.write("match ")
		p.expr(x.Subject)
		p.write(" ")
		p.arms(x.Lbrace, x.Arms, x.Rbrace)
	case *LetCond, *CondList:
		p.cond(x)
	case *JumpExpr:
		p.write(x.Tok.String())
		if x.Value != nil {
			p.write(" ")
			p.expr(x.Value)
		}
	case *CatchExpr:
		p.expr(x.X)
		for _, c := range x.Catches {
			p.write(" ")
			p.node(c)
		}
	case *BadExpr:
		p.write("/* bad syntax */")
	}
}

// binary prints x op y, keeping a line break after op where the source
// had one; cont says the chain's next lines are indented already.
func (p *printer) binary(b *BinaryExpr, cont *bool) {
	prec := b.Op.Precedence()
	lp, rp := prec, prec+1
	switch {
	case b.Op == COALESCE: // right-associative
		lp, rp = prec+1, prec
	case prec == 4 || prec == 5: // comparisons and ranges don't chain
		lp = prec + 1
	}
	p.operand(b.X, lp, cont)
	sp := " "
	if b.Op == RANGE || b.Op == RANGELT {
		sp = ""
	}
	p.write(sp + b.Op.String())
	if l := p.endLine(b.X); l > 0 && l < p.line(b.Y.Range().Start) {
		if !*cont {
			*cont = true
			p.indent++
		}
		p.trailing(nil, &BadExpr{Span: Span{b.OpPos, b.OpPos + Pos(len(b.Op.String()))}}, b.Y.Range().Start)
		p.newline()
	} else {
		p.write(sp)
	}
	p.operand(b.Y, rp, cont)
}

func (p *printer) operand(x Expr, prec int, cont *bool) {
	if b, ok := x.(*BinaryExpr); ok && b.Op.Precedence() >= prec {
		p.binary(b, cont)
		return
	}
	p.exprPrec(x, prec)
}

func (p *printer) stringLit(s *StringLit) {
	q := `"`
	if s.Multiline {
		q = `"""`
	}
	if s.Raw {
		p.write("r")
	}
	p.write(q)
	for i, seg := range s.Segments {
		if i > 0 {
			p.write("{")
			p.expr(s.Interps[i-1])
			p.write("}")
		}
		p.out = append(p.out, seg...)
	}
	p.write(q)
}

// ---------------------------------------------------------------------
// Types and patterns

func (p *printer) typ(t Type) {
	switch t := t.(type) {
	case *NamedType:
		for i, id := range t.Path {
			if i > 0 {
				p.write(".")
			}
			p.write(id.Name)
		}
		if len(t.Args) > 0 {
			p.write("<")
			for i, a := range t.Args {
				if i > 0 {
					p.write(", ")
				}
				p.typ(a)
			}
			p.write(">")
		}
	case *ArrayType:
		p.write("[")
		p.typ(t.Elem)
		p.write("]")
	case *MapType:
		p.write("[")
		p.typ(t.Key)
		p.write(": ")
		p.typ(t.Value)
		p.write("]")
	case *TupleType:
		p.write("(")
		for i, e := range t.Elems {
			if i > 0 {
				p.write(", ")
			}
			p.typ(e)
		}
		if len(t.Elems) == 1 {
			p.write(",")
		}
		p.write(")")
	case *FuncType:
		if t.Async.IsValid() {
			p.write("async ")
		}
		p.write("fn(")
		for i, e := range t.Params {
			if i > 0 {
				p.write(", ")
			}
			p.typ(e)
		}
		p.write(")")
		if t.Throws.IsValid() {
			p.write(" throws")
		}
		if t.Result != nil {
			p.write(" -> ")
			p.typ(t.Result)
		}
	case *OptionalType:
		if _, fn := t.Elem.(*FuncType); fn {
			p.write("(")
			p.typ(t.Elem)
			p.write(")")
		} else {
			p.typ(t.Elem)
		}
		p.write("?")
	case *BadType:
		p.write("/* bad syntax */")
	}
}

func (p *printer) pattern(pt Pattern) {
	switch pt := pt.(type) {
	case *WildcardPat:
		p.write("_")
	case *BindPat:
		p.write(pt.Name.Name)
	case *LitPat:
		p.expr(pt.Value)
	case *CasePat:
		if pt.Implicit {
			p.write(".")
		}
		for i, id := range pt.Path {
			if i > 0 {
				p.write(".")
			}
			p.write(id.Name)
		}
		if pt.Lparen.IsValid() || pt.Args != nil {
			p.write("(")
			for i, a := range pt.Args {
				if i > 0 {
					p.write(", ")
				}
				if l := pt.Label(i); l != nil {
					p.write(l.Name + ": ")
				}
				p.pattern(a)
			}
			p.write(")")
		}
	case *TuplePat:
		p.write("(")
		for i, e := range pt.Elems {
			if i > 0 {
				p.write(", ")
			}
			p.pattern(e)
		}
		if len(pt.Elems) == 1 {
			p.write(",")
		}
		p.write(")")
	case *OrPat:
		for i, a := range pt.Alts {
			if i > 0 {
				p.write(" | ")
			}
			p.pattern(a)
		}
	case *BadPat:
		p.write("/* bad syntax */")
	}
}
