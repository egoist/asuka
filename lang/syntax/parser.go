package syntax

// The parser: hand-written recursive descent over the scanner's tokens,
// with precedence climbing for binary operators.
//
// It never panics on bad input and never stops at the first error: it
// reports what it expected, skips to the end of the item or declaration
// (a newline, a ; or a } at the depth it started), and carries on, so a
// file's diagnostics come out together and the tree covers everything
// that did parse (what didn't is a Bad node). One error per line: a
// mistake often confuses the rest of its line. A missing closer names
// its opener: "expected '}' to close the block opened at 12:5". A
// declaration that can only be a file's (component, struct, enum,
// import, …) in column 1 inside a block ends the block, so a missing }
// costs one error, not the rest of the file.
//
// Where the RFC's grammar is silent or ambiguous, the parser decides as
// follows.
//
//  1. Blocks. Every { … } body is parsed by one item grammar: the union
//     of members (prop, event, state, let, use, provide, fn, task,
//     effect), view items (name: value, on, bind, style, a named slot,
//     if, for, match, an expression) and statements (var, assignment,
//     while, do/catch, return, throw, break, continue, defer, emit). The
//     grammar's IfView and the if statement, ForView and the for
//     statement, MatchView and the match statement are the same node.
//     Which items a block may hold is the checker's to say, so "return
//     in a component's body" is one precise error, not a parse error
//     that loses the rest of the block.
//  2. Prop sets. An item that starts with a name and a colon is a prop
//     set (PropSet), and the name may be a keyword (type: .email, key:
//     id): no other item starts that way.
//  3. Elements and slots. A lower-case name followed by { is a named
//     slot (toolbar { … }). An upper-case name (or a qualified one whose
//     last part is upper-case, ui.Row) followed by { is an element: a
//     call with no parentheses and a trailing block (Row { … }); after
//     the name or the arguments, as name names it (View as card { … }).
//     Any call f(…) followed by { takes it as its trailing block too:
//     Text("hi") { color: .red }, withTimeout(5s) { … }. A lower-case
//     name and { elsewhere than at the start of an item is a call of it
//     with that trailing block (await animate { … }); at the start of an
//     item, the checker reads a slot named after a function in a
//     statement as that call (animate { … } in a handler).
//  4. Trailing blocks bind only on the call's line: a { on the next line
//     starts a new item (and a block on a line of its own isn't one, an
//     error). They never bind in the head of an if, a while, a for (its
//     sequence and its key), a match or a catch (its pattern and type),
//     where the { starts the body, as Go does with composite literals:
//     if isEmpty { … }, for x in Items { … }. Inside parentheses or
//     brackets in such a head they bind again: if f(Row { a }) { … }.
//  5. Handlers after a trailing block, on its line, belong to the call:
//     Boundary { … } on error(e) { … } (CallExpr.Handlers).
//  6. else and catch may start the line after a }: } else { and }
//     newline else { parse the same; the printer writes the first.
//  7. Lambdas. x => … and _ => … are lambdas; so is a parenthesized list
//     of names, each maybe with a type, whose ) is followed by =>:
//     (a, b) => …, (a: int) => …, () => …, found by looking ahead to the
//     matching parenthesis. Either may follow async (async x => …,
//     async () => …), which marks the lambda async. Anything else in parentheses is a grouping
//     ((x) without =>) or a tuple ((a, b)). A lambda's body is an
//     expression, or after => a { always starts a block of statements.
//  8. Maps and arrays. [:] is an empty map, [] an empty array; after [
//     and an expression, a : makes a map ([a: b]), a , or ] an array
//     ([a]). Types likewise: [T], [K: V].
//  9. try? is try followed directly by ? (no space between: try ? x is
//     try and an error). A postfix ! (force unwrap) must follow its
//     operand directly: x!. Comparisons and ranges don't chain: a < b <
//     c is an error that says what to write.
//  10. Keywords as names. After . and ?. a keyword is a member's name
//      (x.type, .default). The keywords that only start a declaration or
//      an item (app bind component effect enum event interface key
//      native on prop provide screen state store struct style task test
//      throws type use) are names anywhere else: as an expression's
//      start, a binding, a parameter, a lambda's parameter: let app =
//      mount(…); app.tap("Add"); type("Hi", into: "Title"); for (key,
//      value) in m. Each starts its declaration or item only when what
//      follows fits (prop and a name, app and {, test and a string, …).
//      Argument labels, prop sets, fields, enum cases and import path
//      segments may be any keyword (none:, in:).
//  11. Match arms are separated by newlines, commas or semicolons; an
//      arm's body is a block, or one item (in a view or statements) or
//      expression (in a match expression); a guard is if and an
//      expression before the =>, whose { never starts a block.
//  12. An if used as a value (an IfExpr) needs an else; in a block, if,
//      for and match are items, and an if's else is optional.
//  13. Types. ( T ) groups: (fn() -> int)? is an optional function.
//      Generic arguments follow a qualified name's last part:
//      http.Response<T>. A type parameter may name constraints, joined
//      by &: <T: Hashable>, <T: Shape & Hashable>. T?? is an optional
//      optional.
//  14. Enums. Cases are separated by commas or newlines and come before
//      the methods; a case's name may be a keyword (none).
//  15. Imports come before declarations. pub may precede any
//      declaration or member; the checker says where it means
//      something. A var at the top level is an error: a file's mutable
//      values are state.
//  16. return's value is what follows on its line. emit's arguments
//      are a ( on its line after the event's name.
//  17. A style declared inside a block, style name { … }, is parsed (a
//      local style) and left to the checker; style name without a block
//      applies one. A component, a struct or another top-level
//      declaration inside a block is an error, parsed to recover.
//  18. Interpolation. Any expression goes between { and } in a string;
//      \{ is a brace. In a """ string an interpolation may span lines;
//      in a one-line string it ends with its line, and the string isn't
//      terminated.
//  19. Comments. Those above a node on a line of its own, and one after
//      it on its last line, are its Trivia; /// ones are its Doc. A
//      comment inside an expression (between arguments, in an array)
//      stays in SourceFile.Comments only, and the printer places it.
//  20. let … else. An else on the line of a let's value starts its else
//      block: let user = cache[id] else { return }. Any let parses so;
//      only a statement's may have one, which the checker says.
//  21. Condition chains. An if's or a while's condition is clauses
//      separated by commas, each an expression or a let: if let a = x,
//      a > 0 { … } (a CondList; a single clause stays itself).
//  22. Jumps as values. throw, return, break and continue where an
//      operand goes (after an operator, as an argument, a lambda's or a
//      match arm's body) are a JumpExpr: x ?? throw E.missing, x ??
//      return. throw takes the rest of the expression; return a value
//      only when one starts before the line's end or what closes the
//      expression (x ?? return, f(x ?? return)). At the start of an
//      item they stay statements.
//  23. catch as a value. A catch after an expression, on its line,
//      takes the whole expression before it, the loosest of all:
//      try parsePort(s) catch e { 80 } (a CatchExpr). Its clauses are
//      do's, and the later ones may start the line after a }. Nothing
//      continues the expression after its last clause.
//  24. Shifts. << is a token; >> is two > tokens side by side (and >>=
//      a > and a >= side by side), so a type's Kept<Box<int>> needs no
//      space: a > b > c isn't a shift (and comparisons don't chain).
//  25. Struct patterns. A pattern of a name and parentheses, P(…), is a
//      case pattern whose path is P: the checker says it's a struct's
//      when P names one. Its arguments may be labeled (P(x: 0, y)), as
//      a case's payload's may (.server(code: c)).
//  26. Lines that start with .name. Such a line continues the expression
//      on the line before, as a member of it (users ⏎ .filter(…) ⏎
//      .map(…)), except where it starts something of its own: at a match
//      arm's level it's the next arm's pattern (.a => x ⏎ .b => y), and
//      alone on the last line of a block whose last item is its value
//      (an if or a match used as a value, a closure's, a catch's or a
//      spawn's block, and an if, a match or a do inside one) it's that
//      value: if c { log() ⏎ .red }. A chain of two .name lines or more
//      continues there too (.a() alone would be no value). The scanner
//      continues a line that starts with a binary operator.
//  27. load and (. load(…) is a call of a function named load, unless
//      the parentheses hold one call: load (api.user(id)) is the load
//      operator's (load f(…) is how it's written).
//
// The scanner documents how newlines end items (Scanner) and how a %
// after a number reads (50% is a length, 10%3 a remainder).

import (
	"slices"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/egoist/asuka/lang/diag"
)

type tokRec struct {
	pos Pos
	tok Token
	lit string
}

type parser struct {
	file     *File
	errors   ErrorList
	errLines map[int]bool

	toks []tokRec
	i    int
	tok  Token
	pos  Pos
	lit  string

	prevEnd Pos // the end of the last token consumed, newlines and ;s aside
	exprLev int // < 0: in a control clause's head, where { starts the body

	// Lines that start with .name (decision 26). armLev > 0: at a match
	// arm's level, where such a line starts the next arm. valueBlock: the
	// block being parsed gives its last item as its value (wantValue: the
	// next one parsed does). dotLine: the item being parsed has gone on
	// past a line already.
	armLev     int
	valueBlock bool
	wantValue  bool
	dotLine    bool
	// styleValue > 0: in a style: prop's value, where an if may have no
	// else (an entry that adds nothing when it's false).
	styleValue int
	// wantCall: the next block is a call's trailing block; inCall: the
	// items being parsed are one's, where cleanup { … } is a slot named
	// cleanup (or a variant's block), not a cleanup (rfcs/0016).
	wantCall, inCall bool
	inBench          bool // in a bench's body: measure { … } is its statement
	// letPat: parsing a let's pattern, whose name: is the let's type,
	// not a type test's (TypedPat).
	letPat bool

	comments []*Comment
	ci       int // the next comment not yet attached

	angles map[int]int // each <'s matching >, by token index (angleEnd)

	depth int
	bail  bool // nesting too deep: give up on the rest of the file
}

const maxDepth = 1000

// ParseFile parses the source of one file, adds it to fset under
// filename and returns its tree with the errors found. The tree is
// complete even when there are errors: what didn't parse is a Bad node.
func ParseFile(fset *FileSet, filename string, src []byte) (*SourceFile, ErrorList) {
	var p parser
	p.init(fset, filename, src)
	f := p.parseFile()
	p.errors.Sort()
	return f, p.errors
}

// ParseExpr parses a single expression, for tools and tests.
func ParseExpr(src string) (Expr, ErrorList) { return ParseExprIn(NewFileSet(), "", src) }

// ParseExprIn parses a single expression, its positions a file of fset's
// named filename: what the debugger evaluates, checked with a program
// whose positions are fset's.
func ParseExprIn(fset *FileSet, filename, src string) (Expr, ErrorList) {
	var p parser
	p.init(fset, filename, []byte(src))
	x := p.parseExpr()
	for p.tok == NEWLINE || p.tok == SEMICOLON {
		p.next()
	}
	if p.tok != EOF {
		p.errorExpected("the end of the expression")
	}
	p.errors.Sort()
	return x, p.errors
}

func (p *parser) init(fset *FileSet, filename string, src []byte) {
	p.file = fset.AddFile(filename, len(src))
	var s Scanner
	s.Init(p.file, src, func(pos Pos, code *diag.Code, msg string) {
		p.addError(pos, code, msg)
	})
	p.toks = make([]tokRec, 0, len(src)/4+1)
	for {
		pos, tok, lit := s.Scan()
		p.toks = append(p.toks, tokRec{pos, tok, lit})
		if tok == EOF {
			break
		}
	}
	p.comments = s.Comments()
	t := p.toks[0]
	p.tok, p.pos, p.lit = t.tok, t.pos, t.lit
}

// ---------------------------------------------------------------------
// Errors

func (p *parser) addError(pos Pos, code *diag.Code, msg string) {
	position := p.file.Position(pos)
	if p.errLines == nil {
		p.errLines = map[int]bool{}
	}
	p.errLines[position.Line] = true
	p.errors = append(p.errors, &Error{Pos: position, End: position, Code: code.Name, Msg: msg})
}

// error reports msg at pos, unless the line already has an error: one
// mistake often confuses the rest of its line.
func (p *parser) error(pos Pos, code *diag.Code, msg string) {
	if p.bail {
		return
	}
	if p.errLines[p.line(pos)] {
		return
	}
	p.addError(pos, code, msg)
}

func (p *parser) errorExpected(what string) {
	p.error(p.pos, diag.SyntaxError, "expected "+what+", found "+p.describe()+p.newKeyword())
}

// errorExpectedName is errorExpected where a name could go.
func (p *parser) errorExpectedName(what string) { p.errorExpected(what) }

// newKeywords are the keywords a release made of what were names, by
// the release: a file that names something so says to rename it.
var newKeywords = map[Token]string{
	MUT:    "0.2.7",
	EXTEND: "0.2.7",
	WORKER: "0.2.7",
	RELOAD: "0.2.9",
}

// newKeyword says to rename a name that's a keyword since a release ("
// (rename it: mut is a keyword since 0.2.7)"), "" for other tokens.
func (p *parser) newKeyword() string {
	if v, ok := newKeywords[p.tok]; ok {
		return " (rename it: " + p.tok.String() + " is a keyword since " + v + ")"
	}
	return ""
}

// funcAhead reports whether [async] [mut] fn starts here.
func (p *parser) funcAhead() bool {
	i := 0
	if p.peek(i) == ASYNC {
		i++
	}
	if p.peek(i) == MUT {
		i++
	}
	return p.peek(i) == FN
}

// describe names the current token for a message.
func (p *parser) describe() string {
	switch p.tok {
	case EOF:
		return "the end of the file"
	case NEWLINE:
		return "a newline"
	case IDENT:
		return "'" + p.lit + "'"
	case INT, FLOAT, DURATION, LENGTH, COLOR, CHAR:
		return "'" + p.lit + "'"
	case STRING, STRING_HEAD:
		return "a string"
	case STRING_MID, STRING_TAIL:
		return "'}'"
	}
	if p.tok.IsKeyword() {
		return "keyword '" + p.tok.String() + "'"
	}
	return "'" + p.tok.String() + "'"
}

func (p *parser) at(pos Pos) string {
	ps := p.file.Position(pos)
	return strconv.Itoa(ps.Line) + ":" + strconv.Itoa(ps.Column)
}

// ---------------------------------------------------------------------
// Tokens

func (p *parser) next() {
	// A ; ends items as a newline does: a comment above a stray one is
	// the next item's, not inside the last.
	if p.tok != NEWLINE && p.tok != SEMICOLON {
		p.prevEnd = p.pos + Pos(len(p.lit))
	}
	if p.i < len(p.toks)-1 {
		p.i++
	}
	t := p.toks[p.i]
	p.tok, p.pos, p.lit = t.tok, t.pos, t.lit
}

func (p *parser) peek(n int) Token {
	if j := p.i + n; j < len(p.toks) {
		return p.toks[j].tok
	}
	return EOF
}

// isIdentTok reports whether t can be a name where a declaration names
// something: an identifier, or a keyword that only starts a declaration
// or an item (decision 10).
func isIdentTok(t Token) bool { return t == IDENT || softKeyword(t) }

func (p *parser) got(tok Token) bool {
	if p.tok == tok {
		p.next()
		return true
	}
	return false
}

// expect consumes tok, or reports it missing; it returns tok's position.
func (p *parser) expect(tok Token) Pos {
	pos := p.pos
	if p.tok != tok {
		p.errorExpected("'" + tok.String() + "'")
		return pos
	}
	p.next()
	return pos
}

// expectClose consumes the closer of the bracket opened at open, or
// reports it missing.
func (p *parser) expectClose(tok Token, open Pos, what string) Pos {
	pos := p.pos
	if p.tok != tok {
		p.error(p.pos, diag.SyntaxError, "expected '"+tok.String()+"' to close the "+what+" opened at "+p.at(open)+", found "+p.describe())
		return pos
	}
	p.next()
	return pos
}

func (p *parser) span(start Pos) Span {
	end := p.prevEnd
	if end < start {
		end = start
	}
	return Span{start, end}
}

// line is pos's line, 0 for a position outside the file.
func (p *parser) line(pos Pos) int {
	if int(pos) < p.file.Base() || int(pos) > p.file.Base()+p.file.Size() {
		return 0
	}
	return p.file.Line(pos)
}

func (p *parser) atTerminator() bool {
	switch p.tok {
	case NEWLINE, SEMICOLON, RBRACE, EOF:
		return true
	}
	return false
}

func (p *parser) skipTerminators() {
	for p.tok == NEWLINE || p.tok == SEMICOLON {
		p.next()
	}
}

// isName reports whether the current token can be a name where any
// name goes (a label, a prop set's, a member's): an identifier or a
// keyword.
func (p *parser) isName() bool { return p.tok == IDENT || p.tok.IsKeyword() }

// ident is the name here. Its span is its source's; its Name, NFC
// normalized, may be shorter (see NormalizeIdent).
func (p *parser) ident() *Ident {
	if isIdentTok(p.tok) {
		id := &Ident{Span: Span{p.pos, p.pos + Pos(len(p.lit))}, Name: NormalizeIdent(p.lit)}
		p.next()
		return id
	}
	p.errorExpectedName("a name")
	return &Ident{Span: Span{p.pos, p.pos + 1}, Name: "_"}
}

// name is an identifier or a keyword used as a name.
func (p *parser) name() *Ident {
	if p.tok.IsKeyword() {
		id := &Ident{Span: Span{p.pos, p.pos + Pos(len(p.lit))}, Name: p.lit}
		p.next()
		return id
	}
	return p.ident()
}

// syncTo skips to the end of what's being parsed: a newline or a ; at
// the depth it started, a } that closes an enclosing block (not
// consumed), or the end of the file.
func (p *parser) sync() {
	depth := 0
	for p.tok != EOF {
		switch p.tok {
		case LBRACE, LPAREN, LBRACK, DOTBRACE:
			depth++
		case RPAREN, RBRACK:
			if depth > 0 {
				depth--
			}
		case RBRACE:
			if depth == 0 {
				return
			}
			depth--
		case NEWLINE, SEMICOLON:
			if depth == 0 {
				return
			}
		}
		p.next()
	}
}

func (p *parser) enter() bool {
	p.depth++
	if p.depth > maxDepth && !p.bail {
		p.error(p.pos, diag.SyntaxError, "too deeply nested")
		p.bail = true
	}
	if p.bail {
		p.i = len(p.toks) - 1
		t := p.toks[p.i]
		p.tok, p.pos, p.lit = t.tok, t.pos, t.lit
		return false
	}
	return true
}

func (p *parser) leave() { p.depth-- }

// ---------------------------------------------------------------------
// Comments

// leading takes the comments before start, for the node that starts
// there.
func (p *parser) leading(start Pos) Trivia {
	var t Trivia
	last := 0 // the line of what's above
	if p.prevEnd.IsValid() {
		last = p.line(p.prevEnd - 1)
	}
	if p.ci > 0 {
		// The comment the node above took after it, which may end on a
		// later line (/* … ⏎ … */).
		if c := p.comments[p.ci-1]; c.Pos >= p.prevEnd && c.End() <= start {
			last = max(last, p.line(c.End()-1))
		}
	}
	var g *CommentGroup
	for p.ci < len(p.comments) && p.comments[p.ci].Pos < start {
		c := p.comments[p.ci]
		p.ci++
		if c.Pos < p.prevEnd {
			// Inside what was parsed last (an argument list, an
			// array): not the next node's. The printer keeps it where
			// it is, or above the line it's on.
			continue
		}
		l := p.line(c.Pos)
		if g == nil || l > last+1 {
			g = &CommentGroup{BlankBefore: last > 0 && l > last+1}
			t.Leading = append(t.Leading, g)
		}
		g.List = append(g.List, c)
		last = p.line(c.End() - 1)
	}
	t.BlankBefore = last > 0 && p.line(start) > last+1
	return t
}

// dangling takes the comments before pos (a closing brace or the end of
// the file) that no node took.
func (p *parser) dangling(pos Pos) []*CommentGroup {
	t := p.leading(pos)
	if len(t.Leading) == 0 {
		return nil
	}
	return t.Leading
}

// trailing takes a comment after end on end's line, unless another item
// starts between them.
func (p *parser) trailing(t *Trivia, end Pos) {
	if t == nil || !end.IsValid() {
		return
	}
	p.skipComments(end)
	if p.ci >= len(p.comments) {
		return
	}
	c := p.comments[p.ci]
	if c.Pos < end || p.line(c.Pos) != p.line(end-1) {
		return
	}
	j := p.i
	for p.toks[j].tok == SEMICOLON && j < len(p.toks)-1 {
		j++
	}
	if nt := p.toks[j]; nt.tok != NEWLINE && nt.tok != EOF && nt.pos < c.Pos {
		return
	}
	t.Trailing = c
	p.ci++
}

// skipComments passes the comments before pos that no node took: they
// are inside a node (its arguments, an array) and stay with the file's
// Comments for the printer.
func (p *parser) skipComments(pos Pos) {
	for p.ci < len(p.comments) && p.comments[p.ci].Pos < pos {
		p.ci++
	}
}

// endItem ends a node on a line of its own: a comment after it, then a
// newline, a ;, or the } or the end of the file that ends the list.
func (p *parser) endItem(n Node, what string) {
	p.trailing(TriviaOf(n), n.Range().End)
	switch p.tok {
	case NEWLINE, SEMICOLON:
		p.next()
	case RBRACE, EOF:
	default:
		if p.line(p.pos) > p.line(p.prevEnd-1) {
			// On a later line with no newline token between: after an
			// unclosed ( or [, already reported. Go on from here.
			return
		}
		p.error(p.pos, diag.SyntaxError, "expected a newline or ';' after the "+what+", found "+p.describe())
		p.sync()
	}
}

// benchAhead reports whether bench "name" starts here: "bench" is a
// keyword only before a string, at a file's top level.
func (p *parser) benchAhead() bool {
	if p.tok != IDENT || p.lit != "bench" {
		return false
	}
	next := p.peek(1)
	return next == STRING || next == STRING_HEAD
}

// previewAhead reports whether preview "name" starts here: "preview" is
// a keyword only before a string, at a file's top level.
func (p *parser) previewAhead() bool {
	if p.tok != IDENT || p.lit != "preview" {
		return false
	}
	next := p.peek(1)
	return next == STRING || next == STRING_HEAD
}

// declAhead reports whether a declaration that can't be in a block
// starts here, at the start of a line: a component, a struct, … in
// column 1 inside a block means the block's } is missing, and the
// block ends there so the declaration parses as the file's.
func (p *parser) declAhead() bool {
	if p.file.Position(p.pos).Column != 1 {
		return false
	}
	tok, next := p.tok, p.peek(1)
	if tok == PUB {
		tok, next = next, p.peek(2)
	}
	switch tok {
	case COMPONENT, SCREEN, STORE, STRUCT, ENUM, INTERFACE, WORKER:
		return isIdentTok(next)
	case EXTEND:
		return true
	case NATIVE:
		return next == INTERFACE || next == COMPONENT || next == FN || next == ASYNC
	case EXTERN:
		return next == FN || next == TYPE || next == STRUCT
	case IMPORT:
		return true
	case APP:
		return next == LBRACE
	case TEST:
		return next == STRING || next == STRING_HEAD
	case TYPE:
		return isIdentTok(next)
	case IDENT:
		return tok == p.tok && (p.benchAhead() || p.previewAhead())
	}
	return false
}

// braceOnNextLine reports a { written on the line after what it opens
// (if a ⏎ {) and moves to it, so the block parses as if it were on the
// line: one error, not one for the head and more for the block's items.
func (p *parser) braceOnNextLine() {
	if p.tok == NEWLINE && p.peek(1) == LBRACE {
		p.next()
		p.error(p.pos, diag.BracePlacement, "a '{' goes at the end of the line before, not on a line of its own")
	}
}

// ---------------------------------------------------------------------
// Files and declarations

func (p *parser) parseFile() *SourceFile {
	f := &SourceFile{File: p.file}
	p.skipTerminators()
	for p.tok != EOF {
		start, i := p.pos, p.i
		t := p.leading(start)
		if p.tok == IMPORT {
			d := p.parseImport()
			d.Trivia = t
			if len(f.Decls) > 0 {
				p.error(start, diag.MisplacedDeclaration, "an import goes before the file's declarations")
			}
			f.Imports = append(f.Imports, d)
			p.endItem(d, "import")
		} else {
			d := p.parseDecl()
			*TriviaOf(d) = t
			f.Decls = append(f.Decls, d)
			p.endItem(d, describeNode(d))
		}
		if p.tok == RBRACE {
			p.error(p.pos, diag.SyntaxError, "unexpected '}'")
			p.next()
		}
		if p.i == i {
			p.next() // never loop without consuming
		}
		p.skipTerminators()
	}
	f.Dangling = p.dangling(p.pos)
	f.Comments = p.comments
	f.Span = Span{p.file.Pos(0), p.file.Pos(p.file.Size())}
	return f
}

// describeNode names a node's kind for a message.
func describeNode(n Node) string {
	switch n := n.(type) {
	case *ComponentDecl:
		if n.Native.IsValid() {
			return "native component"
		}
		return "component"
	case *ScreenDecl:
		return "screen"
	case *StoreDecl:
		return "store"
	case *WorkerDecl:
		return "worker"
	case *StructDecl:
		return "struct"
	case *EnumDecl:
		return "enum"
	case *InterfaceDecl:
		return "interface"
	case *ExtendDecl:
		return "extension"
	case *FuncDecl:
		return "function"
	case *TypeAliasDecl:
		return "type alias"
	case *StyleDecl:
		return "style"
	case *StateDecl:
		return "state"
	case *LetDecl:
		return n.Keyword.String()
	case *AppDecl:
		return "app"
	case *TestDecl:
		return "test"
	case *BenchDecl:
		return "bench"
	case *PreviewDecl:
		return "preview"
	case *MeasureStmt:
		return "measure"
	case *Field:
		return "field"
	case *EnumCase:
		return "case"
	case *PropDecl:
		return "prop"
	case *EventDecl:
		return "event"
	case *VariantDecl:
		return "variant"
	case *UseDecl:
		return "use"
	case *ProvideDecl:
		return "provide"
	case *TaskDecl:
		return "task"
	case *EffectDecl:
		return "effect"
	case *PropSet:
		return "prop"
	case *OnHandler:
		return "handler"
	case *BindItem:
		return "binding"
	case *StyleUse:
		return "style"
	case *SlotItem:
		return "slot"
	case *IfStmt:
		return "if"
	case *ForStmt:
		return "for"
	case *WhileStmt:
		return "while"
	case *MatchStmt:
		return "match"
	case *DoStmt:
		return "do"
	case *ReturnStmt:
		return "return"
	case *ThrowStmt:
		return "throw"
	case *BranchStmt:
		return n.Tok.String()
	case *DeferStmt:
		return "defer"
	case *CleanupStmt:
		return "cleanup"
	case *EmitStmt:
		return "emit"
	case *AssignStmt:
		return "assignment"
	case *ExprStmt:
		if c, ok := n.X.(*CallExpr); ok && c.Block != nil {
			return "element"
		}
		return "expression"
	case *MatchArm:
		return "match arm"
	}
	return "declaration"
}

func (p *parser) parseImport() *ImportDecl {
	d := &ImportDecl{}
	start := p.expect(IMPORT)
	if p.tok == PERIOD && p.peek(1) == QUO {
		d.Relative = true
		p.next()
		p.next()
	} else {
		// ../path: a directory up (a project's program importing the
		// project's packages).
		for p.tok == RANGE && p.peek(1) == QUO {
			d.Relative = true
			d.Up++
			p.next()
			p.next()
		}
	}
	for {
		seg := p.importSegment(!d.Relative && len(d.Path) == 0)
		if seg == nil {
			p.errorExpected("an import path")
			break
		}
		d.Path = append(d.Path, seg)
		if p.tok != QUO {
			break
		}
		p.next()
	}
	if p.tok == DOTBRACE {
		d.Lbrace = p.pos
		p.next()
		d.Names = []*Ident{}
		for p.tok != RBRACE && p.tok != EOF {
			d.Names = append(d.Names, p.name())
			if !p.got(COMMA) {
				break
			}
		}
		d.Rbrace = p.expectClose(RBRACE, d.Lbrace, "import list")
	}
	if p.got(AS) {
		d.Alias = p.ident()
	}
	d.Span = p.span(start)
	return d
}

// importSegment is one segment of an import path: a name, or a
// package's name on the registry, which may have hyphens and dots
// (line-charts) and, first, a scope (@acme). Its parts are adjacent.
func (p *parser) importSegment(first bool) *Ident {
	start := p.pos
	var b strings.Builder
	if first && p.tok == AT {
		b.WriteByte('@')
		p.next()
		if p.pos != p.prevEnd {
			return nil
		}
	}
	if !p.isName() {
		return nil
	}
	b.WriteString(p.lit)
	p.next()
	for (p.tok == SUB || p.tok == PERIOD) && p.pos == p.prevEnd {
		if t := p.peek(1); t != IDENT && t != INT && !t.IsKeyword() || p.toks[p.i+1].pos != p.pos+Pos(len(p.lit)) {
			break
		}
		b.WriteString(p.lit)
		p.next()
		b.WriteString(p.lit)
		p.next()
	}
	return &Ident{Span: Span{start, p.prevEnd}, Name: b.String()}
}

func (p *parser) parseDecl() Decl {
	start := p.pos
	var pub Pos
	if p.tok == PUB {
		pub = p.pos
		p.next()
	}
	priv := p.parsePrivate(pub)
	var d Decl
	if p.benchAhead() {
		// bench "name" { … }: "bench" is a keyword only here.
		if pub.IsValid() {
			p.error(pub, diag.InvalidModifier, "a bench can't be pub")
		}
		p.next()
		b := &BenchDecl{Name: p.parseString()}
		in := p.inBench
		p.inBench = true
		b.Body = p.parseBlock()
		p.inBench = in
		setSpan(b, p.span(start))
		return b
	}
	if p.previewAhead() {
		// preview "name" (settings) { … }: "preview" is a keyword only here.
		if pub.IsValid() {
			p.error(pub, diag.InvalidModifier, "a preview can't be pub: asuka dev shows it, nothing imports it")
		}
		p.next()
		d := &PreviewDecl{Name: p.parseString()}
		if p.tok == LPAREN {
			d.Settings = p.parseArgs()
		}
		d.Body = p.parseBlock()
		setSpan(d, p.span(start))
		return d
	}
	if p.tok == IDENT && p.lit == "shader" && p.peek(1) == IDENT {
		// shader Name { … }: "shader" is a keyword only here.
		d = p.parseShader(start)
		setPub(d, pub)
		setSpan(d, p.span(start))
		return d
	}
	switch p.tok {
	case COMPONENT:
		d = p.parseComponent(start, NoPos)
	case SCREEN:
		d = p.parseScreen(start)
	case STORE:
		d = p.parseStore(start)
	case WORKER:
		d = p.parseWorker(start)
	case STRUCT:
		d = p.parseStruct(start)
	case ENUM:
		d = p.parseEnum(start)
	case INTERFACE:
		d = p.parseInterface(start, NoPos)
	case EXTEND:
		d = p.parseExtend(start)
	case FN, ASYNC, MUT:
		d = p.parseFunc(start)
	case TYPE:
		d = p.parseTypeAlias(start)
	case STYLE:
		d = p.parseStyleDecl(start)
	case STATE:
		d = p.parseState(start)
	case LET, VAR:
		d = p.parseLet(start)
	case NATIVE:
		native := p.pos
		p.next()
		switch p.tok {
		case INTERFACE:
			d = p.parseInterface(start, native)
		case COMPONENT:
			d = p.parseComponent(start, native)
		case FN, ASYNC:
			f := p.parseFunc(start)
			f.Native = native
			d = f
		case LPAREN:
			// native(prefix) fn: the builtin prefix.f, a table of its own.
			p.next()
			prefix := p.ident()
			p.expect(RPAREN)
			if p.tok != FN && p.tok != ASYNC {
				p.errorExpected("'fn' after 'native(" + prefix.Name + ")'")
				p.sync()
				return &BadDecl{Span: p.span(start)}
			}
			f := p.parseFunc(start)
			f.Native, f.NativePrefix = native, prefix
			d = f
		default:
			p.errorExpected("'interface', 'component' or 'fn' after 'native'")
			p.sync()
			return &BadDecl{Span: p.span(start)}
		}
	case EXTERN:
		ext := p.pos
		p.next()
		switch p.tok {
		case FN, ASYNC:
			f := p.parseFunc(start)
			f.Extern = ext
			d = f
		case STRUCT:
			s := p.parseStruct(start)
			s.Extern = ext
			d = s
		case TYPE:
			d = p.parseExternType(start, ext)
		default:
			p.errorExpected("'fn', 'type' or 'struct' after 'extern'")
			p.sync()
			return &BadDecl{Span: p.span(start)}
		}
	case APP:
		p.next()
		d = &AppDecl{Body: p.parseBlock()}
	case TEST:
		if pub.IsValid() {
			p.error(pub, diag.InvalidModifier, "a test can't be pub")
		}
		p.next()
		t := &TestDecl{}
		if p.tok == STRING || p.tok == STRING_HEAD {
			t.Name = p.parseString()
		} else {
			p.errorExpected("the test's name, a string")
			t.Name = &StringLit{Span: Span{p.pos, p.pos}, Segments: []string{""}}
		}
		if p.tok == LPAREN {
			// A property test's parameters: test "name" (a: int) { … }.
			t.Params = p.parseParams(false)
		}
		t.Body = p.parseBlock()
		d = t
	default:
		if pub.IsValid() {
			p.errorExpected("a declaration after 'pub'")
		} else {
			p.errorExpected("a declaration (component, screen, store, struct, enum, fn, …)")
		}
		p.sync()
		return &BadDecl{Span: p.span(start)}
	}
	setPub(d, pub)
	p.setPrivate(d, priv)
	setSpan(d, p.span(start))
	return d
}

// privateAhead reports whether a private modifier is here: "private" is
// a keyword only before a declaration (fn, let, var, static fn, struct,
// enum, interface, type) or a field (private items: [T]), so a field or
// a value named private is one still.
func (p *parser) privateAhead() bool {
	if p.tok != IDENT || p.lit != "private" {
		return false
	}
	switch p.peek(1) {
	case FN, ASYNC, MUT, LET, VAR, STRUCT, ENUM, INTERFACE, TYPE, NATIVE:
		return true
	case IDENT:
		next := p.toks[p.i+1].lit
		return next == "static" || p.peek(2) == COLON
	}
	return false
}

// parsePrivate parses a private modifier, if there's one, after pub's
// (which it can't go with).
func (p *parser) parsePrivate(pub Pos) Pos {
	if !p.privateAhead() {
		return NoPos
	}
	priv := p.pos
	if pub.IsValid() {
		p.error(priv, diag.InvalidModifier, "pub and private: a declaration is one or the other")
	}
	p.next()
	return priv
}

// setPrivate records a private modifier on what it makes its file's,
// and says where it can't go.
func (p *parser) setPrivate(n Node, priv Pos) {
	if !priv.IsValid() {
		return
	}
	switch n := n.(type) {
	case *StructDecl:
		n.Private = priv
	case *EnumDecl:
		n.Private = priv
	case *InterfaceDecl:
		n.Private = priv
	case *TypeAliasDecl:
		n.Private = priv
	case *FuncDecl:
		n.Private = priv
	case *LetDecl:
		n.Private = priv
	case *Field:
		n.Private = priv
	default:
		p.error(priv, diag.InvalidModifier, "private goes on a struct's, an enum's or a file's fn, let, var, field or type")
	}
}

// setPub records a pub keyword on the declaration or member it exports.
func setPub(n Node, pub Pos) bool {
	if !pub.IsValid() {
		return true
	}
	switch n := n.(type) {
	case *ShaderDecl:
		n.Pub = pub
	case *ComponentDecl:
		n.Pub = pub
	case *ScreenDecl:
		n.Pub = pub
	case *StoreDecl:
		n.Pub = pub
	case *WorkerDecl:
		n.Pub = pub
	case *StructDecl:
		n.Pub = pub
	case *EnumDecl:
		n.Pub = pub
	case *InterfaceDecl:
		n.Pub = pub
	case *ExtendDecl:
		n.Pub = pub
	case *FuncDecl:
		n.Pub = pub
	case *TypeAliasDecl:
		n.Pub = pub
	case *ExternTypeDecl:
		n.Pub = pub
	case *StyleDecl:
		n.Pub = pub
	case *StateDecl:
		n.Pub = pub
	case *LetDecl:
		n.Pub = pub
	case *AppDecl:
		n.Pub = pub
	case *PropDecl:
		n.Pub = pub
	case *EventDecl:
		n.Pub = pub
	case *VariantDecl:
		n.Pub = pub
	case *Field:
		n.Pub = pub
	default:
		return false
	}
	return true
}

// setSpan sets the span of a node whose parse function started after
// its pub or native keyword.
func setSpan(n Node, s Span) {
	switch n := n.(type) {
	case *ShaderDecl:
		n.Span = s
	case *ComponentDecl:
		n.Span = s
	case *ScreenDecl:
		n.Span = s
	case *StoreDecl:
		n.Span = s
	case *WorkerDecl:
		n.Span = s
	case *StructDecl:
		n.Span = s
	case *EnumDecl:
		n.Span = s
	case *InterfaceDecl:
		n.Span = s
	case *ExtendDecl:
		n.Span = s
	case *FuncDecl:
		n.Span = s
	case *TypeAliasDecl:
		n.Span = s
	case *ExternTypeDecl:
		n.Span = s
	case *StyleDecl:
		n.Span = s
	case *StateDecl:
		n.Span = s
	case *LetDecl:
		n.Span = s
	case *AppDecl:
		n.Span = s
	case *TestDecl:
		n.Span = s
	case *BenchDecl:
		n.Span = s
	case *PreviewDecl:
		n.Span = s
	case *PropDecl:
		n.Span = s
	case *EventDecl:
		n.Span = s
	case *VariantDecl:
		n.Span = s
	case *Field:
		n.Span = s
	}
}

func (p *parser) parseTypeParams() ([]*TypeParam, Angles) {
	if p.tok != LSS {
		return nil, Angles{}
	}
	open := p.pos
	p.next()
	var list []*TypeParam
	for p.tok != GTR && p.tok != EOF {
		start := p.pos
		tp := &TypeParam{Name: p.ident()}
		if p.got(COLON) {
			tp.Constraints = append(tp.Constraints, p.parseType())
			for p.got(AMP) {
				tp.Constraints = append(tp.Constraints, p.parseType())
			}
		}
		tp.Span = p.span(start)
		list = append(list, tp)
		if !p.got(COMMA) {
			break
		}
	}
	if len(list) == 0 {
		p.error(open, diag.SyntaxError, "expected a type parameter")
	}
	p.lineBeforeAngle()
	return list, Angles{open, p.expectClose(GTR, open, "type parameters")}
}

// lineBeforeAngle skips the end of a line before a list's closing >: a
// < isn't a bracket to the scanner, so a line ending in the list's last
// element, without a comma, counts.
func (p *parser) lineBeforeAngle() {
	if p.tok == NEWLINE && p.peek(1) == GTR {
		p.next()
	}
}

// parseParams parses (name: Type = default, …). In a lambda, types are
// optional.
func (p *parser) parseParams(lambda bool) *ParamList {
	l := &ParamList{}
	open := p.expect(LPAREN)
	p.exprLev++
	for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
		start := p.pos
		param := &Param{}
		if p.tok == IDENT {
			param.Name = p.ident()
		} else {
			param.Name = p.name()
		}
		if p.got(COLON) {
			param.Type = p.parseType()
		} else if !lambda {
			p.errorExpected("':' and the parameter's type")
		}
		if p.got(ASSIGN) {
			param.Default = p.parseExpr()
		}
		param.Span = p.span(start)
		l.List = append(l.List, param)
		if !p.got(COMMA) {
			break
		}
	}
	p.exprLev--
	p.expectClose(RPAREN, open, "parameter list")
	l.Span = p.span(open)
	return l
}

func (p *parser) optParams() *ParamList {
	if p.tok == LPAREN {
		return p.parseParams(false)
	}
	return nil
}

func (p *parser) parseComponent(start, native Pos) *ComponentDecl {
	p.expect(COMPONENT)
	d := &ComponentDecl{Native: native}
	d.Name = p.ident()
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	d.Params = p.optParams()
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

func (p *parser) parseShader(start Pos) *ShaderDecl {
	p.next() // shader
	d := &ShaderDecl{}
	d.Name = p.ident()
	d.Params = p.optParams()
	d.Lbrace, d.Members, d.Dangling, d.Rbrace = p.parseBody("shader", func() Node {
		s := p.pos
		var n Node
		switch p.tok {
		case FN:
			n = p.parseFunc(s)
		case LET:
			n = p.parseLet(s)
		default:
			p.errorExpected("a function (fn fragment(pos: vec2) -> vec4 { … }) or a constant (let)")
			p.sync()
			return nil
		}
		setSpan(n, p.span(s))
		return n
	})
	d.Span = p.span(start)
	return d
}

func (p *parser) parseScreen(start Pos) *ScreenDecl {
	p.expect(SCREEN)
	d := &ScreenDecl{}
	d.Name = p.ident()
	d.Params = p.optParams()
	if p.tok == IDENT && p.lit == "at" {
		// at "pattern": a routed screen.
		p.next()
		if p.tok == STRING || p.tok == STRING_HEAD {
			d.At = p.parseString()
		} else {
			p.errorExpected("the screen's route pattern, a string: at \":id\"")
		}
	}
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

func (p *parser) parseStore(start Pos) *StoreDecl {
	p.expect(STORE)
	d := &StoreDecl{}
	d.Name = p.ident()
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	d.Params = p.optParams()
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

// parseWorker parses worker Name(params) { members }.
func (p *parser) parseWorker(start Pos) *WorkerDecl {
	p.expect(WORKER)
	d := &WorkerDecl{}
	d.Name = p.ident()
	d.Params = p.optParams()
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

func (p *parser) parseConforms() []Type {
	if !p.got(COLON) {
		return nil
	}
	list := []Type{p.parseType()}
	for p.got(COMMA) {
		list = append(list, p.parseType())
	}
	return list
}

// parseBody parses the members of a struct's, an enum's or an
// interface's { … }, one per line, each by member.
func (p *parser) parseBody(what string, member func() Node) (lbrace Pos, members []Node, dangling []*CommentGroup, rbrace Pos) {
	p.braceOnNextLine()
	lbrace = p.pos
	if p.tok != LBRACE {
		p.errorExpected("'{' to start the " + what + "'s body")
		return lbrace, nil, nil, lbrace
	}
	p.next()
	p.skipTerminators()
	for p.tok != RBRACE && p.tok != EOF {
		if p.declAhead() {
			break
		}
		i := p.i
		t := p.leading(p.pos)
		m := member()
		if m != nil {
			if tr := TriviaOf(m); tr != nil {
				*tr = t
			}
			members = append(members, m)
			p.endItem(m, describeNode(m))
		}
		if p.i == i {
			p.next()
		}
		p.skipTerminators()
	}
	dangling = p.dangling(p.pos)
	rbrace = p.expectClose(RBRACE, lbrace, what+"'s body")
	return
}

func (p *parser) parseStruct(start Pos) *StructDecl {
	p.expect(STRUCT)
	d := &StructDecl{}
	d.Name = p.ident()
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	d.Conforms = p.parseConforms()
	d.Lbrace, d.Members, d.Dangling, d.Rbrace = p.parseBody("struct", func() Node {
		return p.parseFieldOrMethod("a field (name: Type), a constant (let) or a method", true)
	})
	d.Span = p.span(start)
	return d
}

// parseFieldOrMethod parses a struct's or an interface's member.
func (p *parser) parseFieldOrMethod(what string, constants bool) Node {
	start := p.pos
	var pub Pos
	if p.tok == PUB {
		pub = p.pos
		p.next()
	}
	priv := p.parsePrivate(pub)
	var n Node
	switch {
	case p.funcAhead():
		n = p.parseFunc(start)
	case constants && p.tok == IDENT && p.lit == "static" && (p.peek(1) == FN || p.peek(1) == ASYNC || p.peek(1) == MUT):
		// A function of the type's (Money.of(dollars: 2)).
		static := p.pos
		p.next()
		f := p.parseFunc(start)
		f.Static = static
		n = f
	case p.tok == NATIVE && (p.peek(1) == FN || p.peek(1) == ASYNC):
		native := p.pos
		p.next()
		f := p.parseFunc(start)
		f.Native = native
		n = f
	case (p.tok == LET || p.tok == VAR) && constants:
		// A constant of the type (Font.title), in a struct's or an
		// enum's body.
		n = p.parseLet(start)
	case p.isName() && p.peek(1) == COLON:
		f := &Field{Name: p.name()}
		p.next()
		f.Type = p.parseType()
		if p.got(ASSIGN) {
			f.Default = p.parseExpr()
		}
		n = f
	default:
		p.errorExpected(what)
		p.sync()
		return nil
	}
	setPub(n, pub)
	p.setPrivate(n, priv)
	setSpan(n, p.span(start))
	return n
}

func (p *parser) parseEnum(start Pos) *EnumDecl {
	p.expect(ENUM)
	d := &EnumDecl{}
	d.Name = p.ident()
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	d.Conforms = p.parseConforms()
	p.braceOnNextLine()
	d.Lbrace = p.pos
	if !p.got(LBRACE) {
		p.errorExpected("'{' to start the enum's body")
		d.Span = p.span(start)
		return d
	}
	p.skipTerminators()
	for p.tok != RBRACE && p.tok != EOF {
		if p.declAhead() {
			break
		}
		i := p.i
		t := p.leading(p.pos)
		static := p.tok == IDENT && p.lit == "static" && (p.peek(1) == FN || p.peek(1) == ASYNC || p.peek(1) == MUT)
		if p.funcAhead() || p.tok == PUB || p.tok == LET || static || p.privateAhead() {
			m := p.parseFieldOrMethod("a method or a constant", true)
			switch f := m.(type) {
			case *FuncDecl:
				f.Trivia = t
				d.Members = append(d.Members, f)
				p.endItem(f, "method")
			case *LetDecl:
				f.Trivia = t
				d.Members = append(d.Members, f)
				p.endItem(f, "constant")
			default:
				if m != nil {
					p.error(m.Range().Start, diag.MisplacedDeclaration, "an enum has cases, constants and methods, not fields")
				}
			}
		} else if p.isName() {
			c := p.parseEnumCase()
			c.Trivia = t
			if len(d.Members) > 0 {
				p.error(c.Pos(), diag.MisplacedDeclaration, "an enum's cases come before its constants and methods")
			}
			d.Cases = append(d.Cases, c)
			if p.tok == COMMA {
				p.next()
				p.trailing(&c.Trivia, c.End)
				if p.tok == NEWLINE || p.tok == SEMICOLON {
					p.next()
				}
			} else {
				p.endItem(c, "case")
			}
		} else {
			p.errorExpected("an enum case or a method")
			p.sync()
		}
		if p.i == i {
			p.next()
		}
		p.skipTerminators()
	}
	d.Dangling = p.dangling(p.pos)
	d.Rbrace = p.expectClose(RBRACE, d.Lbrace, "enum's body")
	d.Span = p.span(start)
	return d
}

// Pos is the case's start.
func (c *EnumCase) Pos() Pos { return c.Start }

func (p *parser) parseEnumCase() *EnumCase {
	start := p.pos
	c := &EnumCase{Name: p.name()}
	if p.tok == LPAREN {
		open := p.pos
		p.next()
		c.Params = &CaseParams{}
		for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
			s := p.pos
			cp := &CaseParam{}
			if p.isName() && p.peek(1) == COLON {
				cp.Label = p.name()
				p.next()
			}
			cp.Type = p.parseType()
			cp.Span = p.span(s)
			c.Params.List = append(c.Params.List, cp)
			if !p.got(COMMA) {
				break
			}
		}
		p.expectClose(RPAREN, open, "case's payload")
		c.Params.Span = p.span(open)
	}
	c.Span = p.span(start)
	return c
}

// parseExtend parses extend<T> Type: Interface { members }.
func (p *parser) parseExtend(start Pos) *ExtendDecl {
	p.expect(EXTEND)
	d := &ExtendDecl{}
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	d.Type = p.parseType()
	d.Conforms = p.parseConforms()
	d.Lbrace, d.Members, d.Dangling, d.Rbrace = p.parseBody("extension", func() Node {
		return p.parseFieldOrMethod("a method (fn), a static fn or a constant (let)", true)
	})
	d.Span = p.span(start)
	return d
}

func (p *parser) parseInterface(start, native Pos) *InterfaceDecl {
	p.expect(INTERFACE)
	d := &InterfaceDecl{Native: native}
	d.Name = p.ident()
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	d.Lbrace, d.Members, d.Dangling, d.Rbrace = p.parseBody("interface", func() Node {
		return p.parseFieldOrMethod("a function's signature (fn name(…)) or a field (name: Type)", false)
	})
	d.Span = p.span(start)
	return d
}

// parseFunc parses [async] [mut] fn name<T>(params) [throws] [-> T] and a
// body, = value, or (a signature) neither.
func (p *parser) parseFunc(start Pos) *FuncDecl {
	d := &FuncDecl{}
	if p.tok == ASYNC {
		d.Async = p.pos
		p.next()
	}
	if p.tok == MUT {
		d.Mut = p.pos
		p.next()
	}
	p.expect(FN)
	if p.tok.IsKeyword() && (p.peek(1) == LPAREN || p.peek(1) == LSS) {
		// A method named as a keyword (r.defer()), called through its
		// value as a field of that name is.
		d.Name = p.name()
	} else {
		d.Name = p.ident()
	}
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	if p.tok == LPAREN {
		d.Params = p.parseParams(false)
	} else {
		p.errorExpected("'(' and the function's parameters")
		d.Params = &ParamList{Span: Span{p.pos, p.pos}}
	}
	if p.tok == THROWS {
		d.Throws = p.pos
		p.next()
	}
	if p.got(ARROW) {
		d.Result = p.parseType()
	}
	p.braceOnNextLine() // a signature has no body: { can't start the next line
	switch p.tok {
	case LBRACE:
		d.Body = p.parseBlock()
	case ASSIGN:
		p.next()
		d.Value = p.parseExpr()
	}
	d.Span = p.span(start)
	return d
}

func (p *parser) parseTypeAlias(start Pos) *TypeAliasDecl {
	p.expect(TYPE)
	d := &TypeAliasDecl{}
	d.Name = p.ident()
	d.TypeParams, d.TypeAngles = p.parseTypeParams()
	p.expect(ASSIGN)
	d.Type = p.parseType()
	d.Span = p.span(start)
	return d
}

// parseExternType parses extern type Name [with release fn], after the
// extern keyword (at ext).
func (p *parser) parseExternType(start, ext Pos) *ExternTypeDecl {
	p.expect(TYPE)
	d := &ExternTypeDecl{Extern: ext, Name: p.ident()}
	if p.tok == IDENT && p.lit == "with" {
		p.next()
		if p.tok == IDENT && p.lit == "release" {
			p.next()
			d.Release = p.ident()
		} else {
			p.errorExpected("'release' and the function that frees it (with release close)")
		}
	}
	d.Span = p.span(start)
	return d
}

func (p *parser) parseStyleDecl(start Pos) *StyleDecl {
	p.expect(STYLE)
	d := &StyleDecl{Name: p.ident()}
	if p.tok == LPAREN {
		d.Params = p.parseParams(false)
	}
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

func (p *parser) parseState(start Pos) *StateDecl {
	p.expect(STATE)
	d := &StateDecl{Name: p.ident()}
	d.Deps, d.Lparen, d.Rparen = p.parseDeps("state's dependencies")
	if d.Lparen.IsValid() && len(d.Deps) == 0 {
		p.error(d.Lparen, diag.FollowsNothing, "state "+d.Name.Name+"() follows nothing: a value read once is peek(…), state "+d.Name.Name+" = peek(…)")
	}
	if p.got(COLON) {
		d.Type = p.parseType()
	}
	if p.got(ASSIGN) {
		d.Value = p.parseExpr()
	} else {
		p.errorExpected("'=' and the state's first value")
	}
	d.Span = p.span(start)
	return d
}

func (p *parser) parseLet(start Pos) *LetDecl {
	d := &LetDecl{Keyword: p.tok}
	p.next()
	saved := p.letPat
	p.letPat = true
	d.Pattern = p.parsePattern()
	p.letPat = saved
	if p.got(COLON) {
		d.Type = p.parseType()
	}
	if p.got(ASSIGN) {
		d.Value = p.parseExpr()
	} else {
		p.errorExpected("'=' and a value")
	}
	if p.tok == ELSE {
		// let … else { … } (decision 20).
		p.next()
		d.Else = p.parseBlock()
	}
	d.Span = p.span(start)
	return d
}

// ---------------------------------------------------------------------
// Blocks and items

// parseValueBlock parses a block whose last item may be its value (an if
// used as a value, a closure's), when value says so: there, a last line
// that starts with .name is that value (decision 26).
func (p *parser) parseValueBlock(value bool) *Block {
	p.wantValue = value
	return p.parseBlock()
}

func (p *parser) parseBlock() *Block {
	value := p.wantValue
	p.wantValue = false
	call := p.wantCall
	p.wantCall = false
	p.braceOnNextLine()
	b := &Block{Lbrace: p.pos}
	if p.tok != LBRACE {
		p.errorExpected("'{'")
		b.Rbrace = p.pos
		b.Span = Span{p.pos, p.pos}
		return b
	}
	if !p.enter() {
		b.Span = Span{b.Lbrace, b.Lbrace}
		return b
	}
	defer p.leave()
	p.next()
	p.skipComments(b.Lbrace)
	// A comment after the {, on its line, with the items below.
	if p.ci < len(p.comments) {
		if c := p.comments[p.ci]; c.Pos < p.pos && p.line(c.Pos) == p.line(b.Lbrace) && p.line(c.End()-1) < p.line(p.pos) {
			b.Comment = c
			p.ci++
		}
	}
	lev, arm, vb, dl, ic := p.exprLev, p.armLev, p.valueBlock, p.dotLine, p.inCall
	p.exprLev, p.armLev, p.valueBlock, p.inCall = 0, 0, value, call
	defer func() { p.armLev, p.valueBlock, p.dotLine, p.inCall = arm, vb, dl, ic }()
	p.skipTerminators()
	for p.tok != RBRACE && p.tok != EOF {
		if p.declAhead() {
			break
		}
		i := p.i
		p.dotLine = false
		it := p.parseItem()
		b.Items = append(b.Items, it)
		p.endItem(it, describeNode(it))
		if p.i == i {
			p.next()
		}
		p.skipTerminators()
	}
	p.exprLev = lev
	b.Dangling = p.dangling(p.pos)
	b.Rbrace = p.expectClose(RBRACE, b.Lbrace, "block")
	b.Span = p.span(b.Lbrace)
	return b
}

func (p *parser) parseItem() Item {
	t := p.leading(p.pos)
	it := p.item()
	*TriviaOf(it) = t
	return it
}

func isLower(name string) bool {
	r, _ := utf8.DecodeRuneInString(name)
	return r == '_' || unicode.IsLower(r)
}

func isUpper(name string) bool {
	r, _ := utf8.DecodeRuneInString(name)
	return unicode.IsUpper(r)
}

// item parses one item of a block (see decision 1).
func (p *parser) item() Item {
	start := p.pos
	if p.isName() && p.peek(1) == COLON {
		return p.parsePropSet()
	}
	var pub Pos
	if p.tok == PUB {
		pub = p.pos
		p.next()
	}
	it := p.itemAfterPub(start)
	if !setPub(it, pub) {
		p.error(pub, diag.SyntaxError, "pub goes before a declaration")
	}
	setSpan(it, p.span(start))
	return it
}

func (p *parser) itemAfterPub(start Pos) Item {
	next := p.peek(1)
	switch p.tok {
	case PROP:
		if isIdentTok(next) {
			return p.parseProp(start)
		}
	case EVENT:
		if isIdentTok(next) {
			p.next()
			d := &EventDecl{Name: p.ident()}
			d.Params = p.optParams()
			d.Span = p.span(start)
			return d
		}
	case STATE:
		if isIdentTok(next) {
			return p.parseState(start)
		}
	case LET, VAR:
		return p.parseLet(start)
	case USE:
		if isIdentTok(next) {
			p.next()
			d := &UseDecl{Name: p.ident()}
			p.expect(COLON)
			d.Type = p.parseType()
			d.Span = p.span(start)
			return d
		}
	case PROVIDE:
		p.next()
		d := &ProvideDecl{Value: p.parseExpr()}
		d.Span = p.span(start)
		return d
	case FN, ASYNC, MUT:
		if p.funcAhead() {
			return p.parseFunc(start)
		}
	case TASK:
		if next == LBRACE || next == LPAREN {
			return p.parseTask(start)
		}
	case EFFECT:
		if next == LBRACE || next == LPAREN {
			p.next()
			d := &EffectDecl{}
			d.Deps, d.Lparen, d.Rparen = p.parseDeps("effect's dependencies")
			d.Body = p.parseBlock()
			d.Span = p.span(start)
			return d
		}
	case ON:
		if next == IDENT || next.IsKeyword() {
			return p.parseOn()
		}
	case BIND:
		if next == IDENT || next.IsKeyword() {
			p.next()
			d := &BindItem{Name: p.name()}
			p.expect(COLON)
			d.Target = p.parseExpr()
			d.Span = p.span(start)
			return d
		}
	case STYLE:
		if isIdentTok(next) {
			if p.peek(2) == LBRACE || p.peek(2) == LPAREN {
				return p.parseStyleDecl(start)
			}
			p.next()
			var x Expr = p.ident()
			for p.tok == PERIOD {
				dot := p.pos
				p.next()
				x = &MemberExpr{X: x, Dot: dot, Name: p.ident(), Span: p.span(start)}
			}
			d := &StyleUse{Name: x}
			d.Span = p.span(start)
			return d
		}
	case LBRACE:
		// A block on a line of its own isn't one (decision 4): Row ⏎ {
		// meant Row {.
		p.error(p.pos, diag.BracePlacement, "a block on a line of its own isn't one: its '{' goes at the end of the line before (Row {)")
		p.sync()
		return &BadItem{Span: p.span(start)}
	case IF:
		return p.parseIfStmt()
	case FOR:
		return p.parseFor()
	case WHILE:
		p.next()
		d := &WhileStmt{Cond: p.parseCond()}
		d.Label = p.loopLabel()
		d.Body = p.parseBlock()
		d.Span = p.span(start)
		return d
	case MATCH:
		return p.parseMatchStmt()
	case DO:
		return p.parseDo()
	case RETURN:
		p.next()
		d := &ReturnStmt{}
		if !p.atTerminator() {
			d.Value = p.parseExpr()
		}
		d.Span = p.span(start)
		return d
	case THROW:
		p.next()
		d := &ThrowStmt{Value: p.parseExpr()}
		d.Span = p.span(start)
		return d
	case BREAK, CONTINUE:
		d := &BranchStmt{Tok: p.tok}
		p.next()
		d.Label = p.branchLabel()
		d.Span = p.span(start)
		return d
	case DEFER:
		p.next()
		d := &DeferStmt{Body: p.parseBlock()}
		d.Span = p.span(start)
		return d
	case EMIT:
		p.next()
		d := &EmitStmt{Name: p.name()}
		if p.tok == LPAREN {
			d.Args = p.parseArgs()
		}
		d.Span = p.span(start)
		return d
	case EXTEND:
		p.error(p.pos, diag.MisplacedDeclaration, "an extension is declared at the top level of a file, not in a block")
		p.parseDecl()
		return &BadItem{Span: p.span(start)}
	case COMPONENT, SCREEN, STORE, STRUCT, ENUM, INTERFACE, NATIVE, EXTERN, APP, TEST, IMPORT, WORKER:
		// Only where a declaration follows: elsewhere these are names
		// (decision 10), as in let app = mount(…); app.tap("Add").
		switch {
		case p.tok == IMPORT:
		case p.tok == APP && next == LBRACE:
		case p.tok == TEST && (next == STRING || next == STRING_HEAD):
		case p.tok == NATIVE && (next == INTERFACE || next == COMPONENT):
		case p.tok == EXTERN && (next == FN || next == TYPE || next == STRUCT):
		case p.tok != APP && p.tok != TEST && p.tok != NATIVE && p.tok != EXTERN && isIdentTok(next):
		default:
			goto expr
		}
		what := "a " + p.tok.String()
		switch p.tok {
		case APP, ENUM, INTERFACE, IMPORT:
			what = "an " + p.tok.String()
		case NATIVE:
			what = "a native " + p.peek(1).String()
		case EXTERN:
			what = "an extern " + p.peek(1).String()
		}
		p.error(p.pos, diag.MisplacedDeclaration, what+" is declared at the top level of a file, not in a block")
		if p.tok == IMPORT {
			p.parseImport()
		} else {
			p.parseDecl()
		}
		return &BadItem{Span: p.span(start)}
	case TYPE:
		if isIdentTok(next) {
			p.error(p.pos, diag.MisplacedDeclaration, "a type alias is declared at the top level of a file, not in a block")
			p.parseDecl()
			return &BadItem{Span: p.span(start)}
		}
	case IDENT:
		if p.inBench && p.lit == "measure" && next == LBRACE {
			// measure { … }: the part of a bench it times; contextual, in
			// a bench's body.
			p.next()
			d := &MeasureStmt{Body: p.parseBlock()}
			d.Span = p.span(start)
			return d
		}
		// variant name = condition: contextual, since variant is also a
		// name (a Button's variant: prop).
		if p.lit == "variant" && isIdentTok(next) && p.peek(2) == ASSIGN {
			p.next()
			d := &VariantDecl{Name: p.ident()}
			p.expect(ASSIGN)
			d.Value = p.parseExpr()
			d.Span = p.span(start)
			return d
		}
		// cleanup { … }: contextual, as variant is (rfcs/0016); in a
		// call's block, a slot or a variant of that name, as before.
		if p.lit == "cleanup" && next == LBRACE && !p.inCall {
			p.next()
			d := &CleanupStmt{Body: p.parseBlock()}
			d.Span = p.span(start)
			return d
		}
		if next == LBRACE && isLower(p.lit) {
			d := &SlotItem{Name: p.ident()}
			d.Body = p.parseBlock()
			d.Span = p.span(start)
			return d
		}
	}

expr:
	x := p.parseExpr()
	if op := p.assignOp(); op != ILLEGAL {
		d := &AssignStmt{Target: x, Op: op, OpPos: p.pos}
		if op == SHR_ASSIGN {
			p.next() // the >
		}
		p.next()
		d.Value = p.parseExpr()
		d.Span = p.span(start)
		return d
	}
	if _, bad := x.(*BadExpr); bad {
		p.sync()
		return &BadItem{Span: p.span(start)}
	}
	d := &ExprStmt{X: x}
	d.Span = p.span(start)
	return d
}

func (p *parser) parsePropSet() *PropSet {
	start := p.pos
	d := &PropSet{Name: p.name()}
	p.expect(COLON)
	if d.Name.Name == "style" {
		// A style list's entries may be ifs without an else (the
		// checker allows them there only).
		p.styleValue++
		d.Value = p.parseExpr()
		p.styleValue--
	} else {
		d.Value = p.parseExpr()
	}
	d.Span = p.span(start)
	return d
}

func (p *parser) parseProp(start Pos) *PropDecl {
	p.expect(PROP)
	d := &PropDecl{Name: p.ident()}
	if p.got(COLON) {
		d.Type = p.parseType()
	}
	if p.got(ASSIGN) {
		d.Default = p.parseExpr()
	}
	d.Span = p.span(start)
	return d
}

func (p *parser) parseTask(start Pos) *TaskDecl {
	p.expect(TASK)
	d := &TaskDecl{}
	d.Deps, d.Lparen, d.Rparen = p.parseDeps("task's dependencies")
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

// parseDeps parses the dependencies of a task, an effect or a state,
// (a, b), when there are parentheses.
func (p *parser) parseDeps(what string) (deps []Expr, lparen, rparen Pos) {
	if p.tok != LPAREN {
		return nil, NoPos, NoPos
	}
	lparen = p.pos
	p.next()
	p.exprLev++
	for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
		deps = append(deps, p.parseExpr())
		if !p.got(COMMA) {
			break
		}
	}
	p.exprLev--
	rparen = p.expectClose(RPAREN, lparen, what)
	return deps, lparen, rparen
}

// parseOn parses on name(params) { … }.
func (p *parser) parseOn() *OnHandler {
	start := p.expect(ON)
	d := &OnHandler{Name: p.name()}
	if p.tok == LPAREN {
		d.Lparen = p.pos
		p.next()
		d.Params = []*Ident{}
		for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
			d.Params = append(d.Params, p.ident())
			if !p.got(COMMA) {
				break
			}
		}
		d.Rparen = p.expectClose(RPAREN, d.Lparen, "handler's parameters")
	}
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

// parseCond parses an if's or a while's condition: clauses separated by
// commas, each an expression or let pattern = value (decision 21). A {
// in it starts the body (decision 4).
func (p *parser) parseCond() Expr {
	lev := p.exprLev
	p.exprLev = -1
	defer func() { p.exprLev = lev }()
	start := p.pos
	x := p.condClause()
	if p.tok != COMMA {
		return x
	}
	l := &CondList{List: []Expr{x}}
	for p.got(COMMA) {
		l.List = append(l.List, p.condClause())
	}
	l.Span = p.span(start)
	return l
}

// condClause parses a clause of a condition: an expression, or let
// pattern = value.
func (p *parser) condClause() Expr {
	if p.tok == LET || p.tok == VAR {
		start := p.pos
		isVar := p.tok == VAR
		p.next()
		c := &LetCond{Pattern: p.parsePattern(), Var: isVar}
		p.expect(ASSIGN)
		c.Value = p.parseExpr()
		c.Span = p.span(start)
		return c
	}
	return p.parseExpr()
}

// headExpr parses an expression in a control clause's head.
func (p *parser) headExpr() Expr {
	lev := p.exprLev
	p.exprLev = -1
	x := p.parseExpr()
	p.exprLev = lev
	return x
}

// elseOrCatch reports whether tok follows, maybe on the next line, and
// moves to it.
func (p *parser) continues(tok Token) bool {
	if p.tok == NEWLINE && p.peek(1) == tok {
		p.next()
	}
	return p.tok == tok
}

func (p *parser) parseIfStmt() *IfStmt {
	start := p.expect(IF)
	d := &IfStmt{Cond: p.parseCond()}
	d.Then = p.parseValueBlock(p.valueBlock)
	if p.continues(ELSE) {
		p.next()
		if p.tok == IF {
			d.Else = p.parseIfStmt()
		} else {
			d.Else = p.parseValueBlock(p.valueBlock)
		}
	}
	d.Span = p.span(start)
	return d
}

func (p *parser) parseFor() *ForStmt {
	start := p.expect(FOR)
	d := &ForStmt{}
	if p.tok == AWAIT {
		d.Await = p.pos
		p.next()
	}
	d.Pattern = p.parsePattern()
	p.expect(IN)
	d.Seq = p.headExpr()
	if p.got(KEY) {
		d.Key = p.headExpr()
	}
	d.Label = p.loopLabel()
	d.Body = p.parseBlock()
	d.Span = p.span(start)
	return d
}

// loopLabel parses a loop's as name, after its head (RFC 0011): nil
// without one.
func (p *parser) loopLabel() *Ident {
	if p.tok != AS {
		return nil
	}
	p.next()
	return p.ident()
}

// branchLabel parses break's or continue's loop name, on its line: nil
// without one.
func (p *parser) branchLabel() *Ident {
	if isIdentTok(p.tok) && p.line(p.pos) == p.line(p.prevEnd-1) {
		return p.ident()
	}
	return nil
}

func (p *parser) parseDo() *DoStmt {
	start := p.expect(DO)
	d := &DoStmt{Body: p.parseValueBlock(p.valueBlock)}
	for p.continues(CATCH) {
		d.Catches = append(d.Catches, p.parseCatch(p.valueBlock))
	}
	if len(d.Catches) == 0 {
		p.errorExpected("'catch' after do { … }")
	}
	d.Span = p.span(start)
	return d
}

// parseCatch parses a catch clause, at its catch: catch { … }, catch e
// { … }, catch e: Type { … }, catch Type.case(…) { … }. value says its
// block's last item may be its value.
func (p *parser) parseCatch(value bool) *CatchClause {
	cs := p.expect(CATCH)
	c := &CatchClause{}
	if p.tok != LBRACE {
		if isIdentTok(p.tok) && p.peek(1) == COLON {
			c.Pattern = &BindPat{Name: p.ident()}
			setPatSpan(c.Pattern)
			p.next()
			c.Type = p.parseType()
		} else {
			lev := p.exprLev
			p.exprLev = -1
			c.Pattern = p.parsePattern()
			p.exprLev = lev
		}
	}
	c.Body = p.parseValueBlock(value)
	c.Span = p.span(cs)
	return c
}

// parseCatchExpr parses x catch … { … }…, at the first catch (decision
// 23): later catches may start the line after a }, as do's.
func (p *parser) parseCatchExpr(x Expr) *CatchExpr {
	start := x.Range().Start
	c := &CatchExpr{X: x}
	c.Catches = append(c.Catches, p.parseCatch(true))
	for p.continues(CATCH) {
		c.Catches = append(c.Catches, p.parseCatch(true))
	}
	c.Span = p.span(start)
	return c
}

func setPatSpan(pt Pattern) {
	if b, ok := pt.(*BindPat); ok {
		b.Span = b.Name.Span
	}
}

func (p *parser) parseMatchStmt() *MatchStmt {
	start := p.expect(MATCH)
	d := &MatchStmt{Subject: p.headExpr()}
	d.Lbrace, d.Arms, d.Rbrace = p.parseArms(false)
	d.Span = p.span(start)
	return d
}

func (p *parser) parseMatchExpr() *MatchExpr {
	start := p.expect(MATCH)
	d := &MatchExpr{Subject: p.headExpr()}
	d.Lbrace, d.Arms, d.Rbrace = p.parseArms(true)
	d.Span = p.span(start)
	return d
}

// parseArms parses a match's { arms }; expr says the match is a value.
func (p *parser) parseArms(expr bool) (lbrace Pos, arms []*MatchArm, rbrace Pos) {
	p.braceOnNextLine()
	lbrace = p.pos
	if !p.got(LBRACE) {
		p.errorExpected("'{' to start the match's arms")
		return lbrace, nil, lbrace
	}
	lev := p.exprLev
	p.exprLev = 0
	p.skipTerminators()
	for p.tok != RBRACE && p.tok != EOF {
		if p.declAhead() {
			break
		}
		i := p.i
		start := p.pos
		a := &MatchArm{Trivia: p.leading(start)}
		p.armLev++
		a.Pattern = p.parsePattern()
		if p.got(IF) {
			a.Guard = p.headExpr()
		}
		p.expect(FATARROW)
		switch {
		case p.tok == LBRACE:
			a.Body = p.parseValueBlock(expr || p.valueBlock)
		case expr:
			a.Body = p.parseExpr()
		default:
			a.Body = p.item()
		}
		p.armLev--
		a.Span = p.span(start)
		arms = append(arms, a)
		if p.tok == COMMA {
			p.next()
			p.trailing(&a.Trivia, a.End)
			if p.tok == NEWLINE || p.tok == SEMICOLON {
				p.next()
			}
		} else {
			p.endItem(a, "match arm")
		}
		if p.i == i {
			p.next()
		}
		p.skipTerminators()
	}
	p.exprLev = lev
	rbrace = p.expectClose(RBRACE, lbrace, "match")
	return
}

// ---------------------------------------------------------------------
// Expressions

func (p *parser) parseExpr() Expr {
	if !p.enter() {
		return &BadExpr{Span: Span{p.pos, p.pos}}
	}
	defer p.leave()
	if p.lambdaAhead() {
		return p.parseLambda()
	}
	x := p.parseBinary(1)
	if p.tok == CATCH {
		return p.parseCatchExpr(x)
	}
	return x
}

// lambdaAhead reports whether a lambda starts here (decision 7), maybe
// after async.
func (p *parser) lambdaAhead() bool {
	k := 0
	if p.tok == ASYNC {
		k = 1
	}
	switch t := p.peek(k); {
	case isIdentTok(t):
		return p.peek(k+1) == FATARROW
	case t == LPAREN:
		if p.peek(k+1) == RPAREN {
			return p.peek(k+2) == FATARROW
		}
		if !isIdentTok(p.peek(k + 1)) {
			return false
		}
		switch p.peek(k + 2) {
		case RPAREN:
			return p.peek(k+3) == FATARROW
		case COMMA, COLON:
		default:
			return false
		}
		// Find the matching parenthesis.
		depth := 0
		for j := p.i + k; j < len(p.toks); j++ {
			switch p.toks[j].tok {
			case LPAREN, LBRACK, LBRACE, DOTBRACE:
				depth++
			case RPAREN, RBRACK, RBRACE:
				depth--
				if depth == 0 {
					return j+1 < len(p.toks) && p.toks[j+1].tok == FATARROW
				}
			case EOF:
				return false
			}
		}
	}
	return false
}

func (p *parser) parseLambda() *LambdaExpr {
	start := p.pos
	l := &LambdaExpr{}
	if p.tok == ASYNC {
		l.Async = p.pos
		p.next()
	}
	if isIdentTok(p.tok) {
		id := p.ident()
		l.Params = []*Param{{Span: id.Span, Name: id}}
	} else {
		l.Parens = true
		pl := p.parseParams(true)
		l.Params, l.Lparen, l.Rparen = pl.List, pl.Start, pl.End-1
	}
	p.expect(FATARROW)
	if p.tok == LBRACE {
		l.Body = p.parseValueBlock(true)
	} else {
		l.Body = p.parseExpr()
	}
	l.Span = p.span(start)
	return l
}

// shiftRight reports >> (SHR) or >>= (SHR_ASSIGN) here: a > and, right
// after it, another > or a >=. The scanner makes each > a token of its
// own, so a type's Box<Box<int>> closes twice (decision 24).
func (p *parser) shiftRight() Token {
	if p.tok != GTR || p.i+1 >= len(p.toks) {
		return ILLEGAL
	}
	n := p.toks[p.i+1]
	if n.pos != p.pos+1 {
		return ILLEGAL
	}
	switch n.tok {
	case GTR:
		return SHR
	case GEQ:
		return SHR_ASSIGN
	}
	return ILLEGAL
}

// binaryOp is the binary operator here: the token, or >> of two.
func (p *parser) binaryOp() Token {
	if t := p.shiftRight(); t != ILLEGAL {
		return t
	}
	return p.tok
}

// assignOp is the assignment operator here (ILLEGAL when there's none):
// the token, or >>= of a > and a >=.
func (p *parser) assignOp() Token {
	if t := p.shiftRight(); t == SHR_ASSIGN {
		return t
	}
	if p.tok.IsAssign() {
		return p.tok
	}
	return ILLEGAL
}

func (p *parser) parseBinary(minPrec int) Expr {
	start := p.pos
	x := p.parseUnary()
	for {
		op, opPos := p.binaryOp(), p.pos
		if p.incDec() {
			// x++, x--: C's, not ours.
			p.error(opPos, diag.NoSuchOperator, "there's no "+op.String()+op.String()+" operator: write x "+op.String()+"= 1")
			p.next()
			p.next()
			return x
		}
		if op == QUESTION {
			p.error(opPos, diag.NoSuchOperator, "there's no ?: operator: write if a { b } else { c }")
			return x
		}
		prec := op.Precedence()
		if prec == 0 || prec < minPrec {
			return x
		}
		if op == SHR {
			p.next() // the first >
		}
		p.next()
		var y Expr
		if op == COALESCE || op == POW {
			y = p.parseBinary(prec) // right-associative
		} else {
			y = p.parseBinary(prec + 1)
		}
		x = &BinaryExpr{X: x, Op: op, OpPos: opPos, Y: y, Span: p.span(start)}
		if (prec == 4 || prec == 5) && p.binaryOp().Precedence() == prec {
			if prec == 4 {
				p.error(p.pos, diag.NoSuchOperator, "comparisons don't chain: write a < b && b < c")
			} else {
				p.error(p.pos, diag.NoSuchOperator, "ranges don't chain")
			}
		}
	}
}

// incDec reports ++ or -- here: two + or two - side by side, the second
// the end of the expression or its line (a--b is a - -b).
func (p *parser) incDec() bool {
	if p.tok != ADD && p.tok != SUB || p.i+2 >= len(p.toks) {
		return false
	}
	n := p.toks[p.i+1]
	if n.tok != p.tok || n.pos != p.pos+1 {
		return false
	}
	if p.tok == ADD {
		return true // there's no unary +
	}
	switch t := p.toks[p.i+2]; t.tok {
	case NEWLINE, SEMICOLON, RBRACE, RPAREN, RBRACK, COMMA, EOF:
		return true
	default:
		// A newline after an operator doesn't end the line: y-- ⏎ z.
		return p.line(t.pos) > p.line(p.pos)
	}
}

func (p *parser) parseUnary() Expr {
	start := p.pos
	switch p.tok {
	case NOT, SUB, TILDE, AWAIT:
		// The operand takes a ** after it: -2 ** 2 is -(2 ** 2).
		op := p.tok
		p.next()
		if !p.enter() {
			return &BadExpr{Span: p.span(start)}
		}
		x := p.parseBinary(PrecPow)
		p.leave()
		return &UnaryExpr{Op: op, X: x, Span: p.span(start)}
	case TRY:
		p.next()
		u := &UnaryExpr{Op: TRY}
		if p.tok == QUESTION && p.pos == p.prevEnd {
			u.Optional = true
			p.next()
		}
		if !p.enter() {
			return &BadExpr{Span: p.span(start)}
		}
		u.X = p.parseBinary(PrecPow)
		p.leave()
		u.Span = p.span(start)
		return u
	case LOAD:
		if p.peek(1) == LPAREN {
			return p.loadParen()
		}
		p.next()
		x := p.parsePostfix()
		return &UnaryExpr{Op: LOAD, X: x, Span: p.span(start)}
	case RELOAD:
		// reload f(…), reload f (rfcs/0017): what loads ask again. Followed
		// by ( it calls a function named reload, as load(…) does one named
		// load; alone, it's that name.
		if next := p.peek(1); next != IDENT && !softKeyword(next) {
			id := &Ident{Span: Span{start, start + Pos(len(p.lit))}, Name: p.lit}
			p.next()
			return p.postfixFrom(start, id)
		}
		p.next()
		x := p.parsePostfix()
		return &UnaryExpr{Op: RELOAD, X: x, Span: p.span(start)}
	case THROW, RETURN, BREAK, CONTINUE:
		// A jump used as a value (decision 22): x ?? throw E.missing.
		j := &JumpExpr{Tok: p.tok}
		p.next()
		if j.Tok == THROW || j.Tok == RETURN && p.valueFollows() {
			if !p.enter() {
				return &BadExpr{Span: p.span(start)}
			}
			j.Value = p.parseExpr()
			p.leave()
		}
		if j.Tok == BREAK || j.Tok == CONTINUE {
			j.Label = p.branchLabel()
		}
		j.Span = p.span(start)
		return j
	case SPAWN:
		p.next()
		b := p.parseValueBlock(true)
		return &SpawnExpr{Body: b, Span: p.span(start)}
	}
	return p.parsePostfix()
}

// valueFollows reports whether an expression starts here, after a
// return used as a value: not at the end of its line, nor at what closes
// or separates the expression it's in.
func (p *parser) valueFollows() bool {
	if p.atTerminator() {
		return false
	}
	switch p.tok {
	case RPAREN, RBRACK, COMMA, COLON, LBRACE, CATCH, ELSE, FATARROW, STRING_MID, STRING_TAIL:
		return false
	}
	return true
}

// isElement reports whether x names an element, which takes a trailing
// block without parentheses: Row, ui.Row.
func isElement(x Expr) bool {
	switch x := x.(type) {
	case *Ident:
		return isUpper(x.Name)
	case *MemberExpr:
		return isUpper(x.Name.Name)
	}
	return false
}

// loadParen parses load followed by a ( (decision 27): a call of a
// function named load, load(…), unless the parentheses hold one call,
// the load operator's operand: load (api.user(id)).
func (p *parser) loadParen() Expr {
	start := p.pos
	id := &Ident{Span: Span{start, start + Pos(len(p.lit))}, Name: p.lit}
	p.next()
	args := p.parseArgs()
	if l := args.List; len(l) == 1 && l[0].Label == nil && isCall(l[0].Value) {
		x := p.postfixFrom(args.Start, &ParenExpr{X: l[0].Value, Span: args.Span})
		return &UnaryExpr{Op: LOAD, X: x, Span: p.span(start)}
	}
	if l := args.List; len(l) > 1 && groupsCalls(l) {
		// load (a(), b()) (rfcs/0017): one load of several calls, which
		// the checker says are each a call (group-not-calls).
		elems := make([]Expr, len(l))
		for i, a := range l {
			elems[i] = a.Value
		}
		return &UnaryExpr{Op: LOAD, X: &TupleExpr{Elems: elems, Span: args.Span}, Span: p.span(start)}
	}
	c := &CallExpr{Fun: id, Args: args}
	p.trailingBlock(c)
	c.Span = p.span(start)
	return p.postfixFrom(start, c)
}

// groupsCalls reports whether load's parentheses hold a group: their
// elements unlabeled, one of them a call at least.
func groupsCalls(l []*Arg) bool {
	call := false
	for _, a := range l {
		if a.Label != nil {
			return false
		}
		call = call || isCall(a.Value)
	}
	return call
}

// isCall reports whether x is a call, maybe in parentheses.
func isCall(x Expr) bool {
	for {
		switch e := x.(type) {
		case *ParenExpr:
			x = e.X
		case *CallExpr:
			return true
		default:
			return false
		}
	}
}

func (p *parser) parsePostfix() Expr {
	start := p.pos
	return p.postfixFrom(start, p.parsePrimary())
}

// postfixFrom parses what follows x, which started at start: members,
// calls, indexes, unwraps.
func (p *parser) postfixFrom(start Pos, x Expr) Expr {
	for {
		switch p.tok {
		case PERIOD, OPTDOT:
			opt, dot := p.tok == OPTDOT, p.pos
			p.next()
			var name *Ident
			if p.tok == INT {
				// A tuple's element: t.0, its index the member's name.
				name = &Ident{Span: Span{p.pos, p.pos + Pos(len(p.lit))}, Name: p.lit}
				p.next()
			} else if p.tok == RELOAD {
				// x.reload(): a Loading's, a method so named.
				name = &Ident{Span: Span{p.pos, p.pos + Pos(len(p.lit))}, Name: p.lit}
				p.next()
			} else {
				name = p.ident()
			}
			x = &MemberExpr{X: x, Optional: opt, Dot: dot, Name: name, Span: Span{}}
			x.(*MemberExpr).Span = p.span(start)
		case LPAREN:
			c := &CallExpr{Fun: x, Args: p.parseArgs()}
			if isElement(x) {
				p.elementName(c)
			}
			p.trailingBlock(c)
			c.Span = p.span(start)
			x = c
		case AS:
			// View as card { … }: an element named.
			if !isElement(x) || p.peek(1) != IDENT || p.exprLev < 0 {
				return x
			}
			c := &CallExpr{Fun: x}
			p.elementName(c)
			p.trailingBlock(c)
			c.Span = p.span(start)
			x = c
		case LSS:
			// f<T>(…): explicit type arguments, when what follows the < is
			// types up to a > right before a (; else it's a comparison.
			// Box<int>.empty(): a generic type's, before a static member.
			if targs, angles, ok := p.tryTypeApp(x); ok {
				x = &TypeApp{X: x, TypeArgs: targs, Angles: angles, Span: p.span(start)}
				continue
			}
			targs, angles, ok := p.tryTypeArgs(x)
			if !ok {
				return x
			}
			c := &CallExpr{Fun: x, TypeArgs: targs, Angles: angles, Args: p.parseArgs()}
			p.trailingBlock(c)
			c.Span = p.span(start)
			x = c
		case LBRACE:
			// An element's block, or a lower-case function's trailing
			// block: await animate { … } (decision 3).
			if _, name := x.(*Ident); p.exprLev < 0 || !isElement(x) && !name {
				return x
			}
			c := &CallExpr{Fun: x}
			p.trailingBlock(c)
			c.Span = p.span(start)
			x = c
		case LBRACK:
			open := p.pos
			p.next()
			p.exprLev++
			idx := p.parseExpr()
			var def Expr
			if p.tok == COMMA && p.peek(1) == IDENT && p.toks[p.i+1].lit == "default" && p.peek(2) == COLON {
				// m[k, default: v]: "default" is a label only here.
				p.next()
				p.next()
				p.next()
				def = p.parseExpr()
			}
			p.exprLev--
			p.expectClose(RBRACK, open, "index")
			x = &IndexExpr{X: x, Index: idx, Default: def, Span: p.span(start)}
		case NOT:
			if p.pos != p.prevEnd {
				return x
			}
			p.next()
			x = &UnwrapExpr{X: x, Span: p.span(start)}
		case NEWLINE:
			if !p.dotContinues() {
				return x
			}
			p.next() // the line's end: the next line's .name goes on
			p.dotLine = true
		default:
			return x
		}
	}
}

// dotContinues reports, at a newline, whether the next line starts
// with .name that continues the expression before it (decision 26): not
// at a match arm's level, where it starts the next arm, nor where it's a
// block's value, alone on the block's last line.
func (p *parser) dotContinues() bool {
	if p.peek(1) != PERIOD || p.peek(2) != IDENT || p.armLev > 0 {
		return false
	}
	return !p.valueBlock || p.dotLine || !p.lastLine(p.i+1)
}

// lastLine reports whether the line that starts at token j is its
// block's last: the block's } ends it.
func (p *parser) lastLine(j int) bool {
	depth := 0
	for ; j < len(p.toks); j++ {
		switch p.toks[j].tok {
		case LPAREN, LBRACK, LBRACE, DOTBRACE:
			depth++
		case RPAREN, RBRACK:
			depth--
		case RBRACE:
			if depth == 0 {
				return true
			}
			depth--
		case NEWLINE, SEMICOLON:
			if depth == 0 {
				return j+1 < len(p.toks) && p.toks[j+1].tok == RBRACE
			}
		case EOF:
			return true
		}
	}
	return true
}

// tryTypeArgs parses <T, U> after a name, when it's followed at once by
// a (: a call with explicit type arguments (json.decode<[Item]>(s)).
// Anything else (a < b, a < b > c's error) it leaves untouched.
func (p *parser) tryTypeArgs(x Expr) ([]Type, Angles, bool) {
	switch x.(type) {
	case *Ident, *MemberExpr:
	default:
		return nil, Angles{}, false
	}
	// Types are what's between: names, brackets, ?, commas, colons, fn,
	// arrows. A quick look rejects most comparisons without parsing.
	end := p.angleEnd()
	if end < 0 || end+1 >= len(p.toks) || p.toks[end+1].tok != LPAREN {
		return nil, Angles{}, false
	}
	return p.typeArgsTo(end)
}

// typeArgsTo parses the types between the < here and its > (at the
// token end), or leaves the < untouched when they aren't types.
func (p *parser) typeArgsTo(end int) ([]Type, Angles, bool) {
	_ = end
	saved, savedEnd := p.i, p.prevEnd
	nerr := len(p.errors)
	lt := p.pos
	p.next() // <
	var ts []Type
	for {
		ts = append(ts, p.parseType())
		if !p.got(COMMA) || p.tok == GTR {
			break
		}
	}
	if p.tok != GTR || len(p.errors) != nerr {
		// Not types after all: back to the <, and to the lines with
		// errors then, so the errors of what it is are reported.
		p.errors = p.errors[:nerr]
		clear(p.errLines)
		for _, e := range p.errors {
			p.errLines[e.Pos.Line] = true
		}
		p.i, p.prevEnd = saved, savedEnd
		t := p.toks[p.i]
		p.tok, p.pos, p.lit = t.tok, t.pos, t.lit
		return nil, Angles{}, false
	}
	gt := p.pos
	p.next() // >
	return ts, Angles{lt, gt}, true
}

// tryTypeApp parses <T, U> after a type's name (capitalized: Box,
// pkg.Box) when a .name follows the >: Box<int>.empty(). Anything else
// it leaves untouched.
func (p *parser) tryTypeApp(x Expr) ([]Type, Angles, bool) {
	var name string
	switch x := x.(type) {
	case *Ident:
		name = x.Name
	case *MemberExpr:
		if _, ok := x.X.(*Ident); ok && !x.Optional {
			name = x.Name.Name
		}
	}
	if name == "" || name[0] < 'A' || name[0] > 'Z' {
		return nil, Angles{}, false
	}
	end := p.angleEnd()
	if end < 0 || end+2 >= len(p.toks) || p.toks[end+1].tok != PERIOD || p.toks[end+2].tok != IDENT {
		return nil, Angles{}, false
	}
	return p.typeArgsTo(end)
}

// angleEnd is the index of the > that closes the < here, with only
// what a type has between them (names, brackets, ?, commas, colons, fn,
// arrows, nested <…>), or -1. The file's < are matched once, in one pass,
// so a list of comparisons (a < b, c < d, …) costs no rescan for each.
func (p *parser) angleEnd() int {
	if p.angles == nil {
		p.angles = make(map[int]int)
		var open []int
		for j, t := range p.toks {
			switch t.tok {
			case IDENT, PERIOD, COMMA, COLON, LBRACK, RBRACK, QUESTION, LPAREN, RPAREN, FN, ARROW, ASYNC, THROWS:
			case LSS:
				open = append(open, j)
			case GTR:
				if n := len(open); n > 0 {
					p.angles[open[n-1]] = j
					open = open[:n-1]
				}
			default:
				open = open[:0] // a < still open here closes nowhere
			}
		}
	}
	if end, ok := p.angles[p.i]; ok {
		return end
	}
	return -1
}

// elementName parses an element's as name, when it has one.
func (p *parser) elementName(c *CallExpr) {
	// In a control clause's head, as names the loop (for x in xs as
	// rows {): no element is named there.
	if p.tok == AS && p.peek(1) == IDENT && p.exprLev >= 0 {
		p.next()
		c.As = p.ident()
	}
}

// trailingBlock parses a call's trailing block, if one may follow
// (decision 4), and the handlers after it (decision 5).
func (p *parser) trailingBlock(c *CallExpr) {
	if p.tok != LBRACE || p.exprLev < 0 {
		return
	}
	p.wantCall = true
	c.Block = p.parseBlock()
	for p.tok == ON && (p.peek(1) == IDENT || p.peek(1).IsKeyword()) {
		c.Handlers = append(c.Handlers, p.parseOn())
	}
}

func (p *parser) parseArgs() *ArgList {
	l := &ArgList{}
	open := p.expect(LPAREN)
	p.exprLev++
	for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
		start := p.pos
		a := &Arg{Trivia: p.leading(start)}
		if p.isName() && p.peek(1) == COLON {
			a.Label = p.name()
			p.next()
		}
		a.Value = p.parseExpr()
		a.Span = p.span(start)
		l.List = append(l.List, a)
		if !p.got(COMMA) {
			p.trailing(&a.Trivia, a.End)
			break
		}
		p.trailing(&a.Trivia, a.End)
	}
	p.exprLev--
	p.expectClose(RPAREN, open, "argument list")
	l.Span = p.span(open)
	return l
}

// softKeyword reports whether a keyword is an identifier where an
// expression starts (decision 10).
func softKeyword(t Token) bool {
	switch t {
	case APP, BIND, COMPONENT, EFFECT, ENUM, EVENT, EXTERN, INTERFACE, KEY, NATIVE, ON,
		PROP, PROVIDE, SCREEN, STATE, STORE, STRUCT, STYLE, TASK, TEST, TYPE, USE, THROWS:
		return true
	}
	return false
}

func (p *parser) parsePrimary() Expr {
	start := p.pos
	switch p.tok {
	case IDENT:
		return p.ident()
	case INT, FLOAT, DURATION, LENGTH, COLOR, CHAR:
		x := &BasicLit{Kind: p.tok, Value: p.lit}
		p.next()
		x.Span = p.span(start)
		return x
	case STRING, STRING_HEAD:
		return p.parseString()
	case TRUE, FALSE:
		x := &BoolLit{Value: p.tok == TRUE}
		p.next()
		x.Span = p.span(start)
		return x
	case NONE:
		p.next()
		return &NoneLit{Span: p.span(start)}
	case SELF:
		p.next()
		return &SelfExpr{Span: p.span(start)}
	case AT:
		// @platform: a compile-time constant, its name right after the @.
		p.next()
		if p.pos != p.prevEnd || !isIdentTok(p.tok) && !p.tok.IsKeyword() {
			p.errorExpected("a compile-time constant's name right after @ (@platform)")
			return &BadExpr{Span: p.span(start)}
		}
		name := p.lit
		p.next()
		x := &AtExpr{Name: name}
		// @define("KEY"): its arguments, right after its name.
		if p.tok == LPAREN && p.pos == p.prevEnd {
			x.Args = p.parseArgs()
		}
		x.Span = p.span(start)
		return x
	case PERIOD:
		p.next()
		if (p.tok == INT || p.tok == FLOAT || p.tok == DURATION || p.tok == LENGTH) && p.pos == p.prevEnd {
			p.error(start, diag.InvalidLiteral, "a number starts with a digit: 0"+p.toks[p.i-1].lit+p.lit)
		}
		if p.tok == RELOAD {
			id := &Ident{Span: Span{p.pos, p.pos + Pos(len(p.lit))}, Name: p.lit}
			p.next()
			return &ImplicitMember{Name: id, Span: p.span(start)}
		}
		return &ImplicitMember{Name: p.ident(), Span: p.span(start)}
	case LPAREN:
		return p.parseParenOrTuple()
	case LBRACK:
		return p.parseArrayOrMap()
	case IF:
		return p.parseIfExpr()
	case MATCH:
		return p.parseMatchExpr()
	}
	if softKeyword(p.tok) {
		return p.name()
	}
	if p.tok == ADD && p.peek(1) == ADD && p.toks[p.i+1].pos == p.pos+1 {
		p.error(p.pos, diag.NoSuchOperator, "there's no ++ operator: write x += 1")
	}
	p.errorExpectedName("an expression")
	switch p.tok {
	case NEWLINE, SEMICOLON, RBRACE, RPAREN, RBRACK, COMMA, EOF, STRING_MID, STRING_TAIL:
	default:
		// Skip the token, unless it starts what comes next: a keyword,
		// or a token on a line of its own after an unclosed bracket.
		if !p.tok.IsKeyword() && p.line(p.pos) == p.line(p.prevEnd-1) {
			p.next()
		}
	}
	return &BadExpr{Span: p.span(start)}
}

func (p *parser) parseParenOrTuple() Expr {
	open := p.expect(LPAREN)
	p.exprLev++
	defer func() { p.exprLev-- }()
	if p.tok == RPAREN {
		p.next()
		return &TupleExpr{Span: p.span(open)}
	}
	x := p.parseExpr()
	if p.tok != COMMA {
		p.expectClose(RPAREN, open, "parenthesis")
		return &ParenExpr{X: x, Span: p.span(open)}
	}
	elems := []Expr{x}
	for p.got(COMMA) && p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
		elems = append(elems, p.parseExpr())
	}
	p.expectClose(RPAREN, open, "tuple")
	return &TupleExpr{Elems: elems, Span: p.span(open)}
}

func (p *parser) parseArrayOrMap() Expr {
	open := p.expect(LBRACK)
	p.exprLev++
	defer func() { p.exprLev-- }()
	switch p.tok {
	case RBRACK:
		p.next()
		return &ArrayExpr{Span: p.span(open)}
	case COLON:
		p.next()
		p.expectClose(RBRACK, open, "map")
		return &MapExpr{Span: p.span(open)}
	}
	t := p.leading(p.pos)
	first := p.parseExpr()
	if p.tok != COLON {
		elems := []Expr{first}
		for p.got(COMMA) && p.tok != RBRACK && p.tok != RBRACE && p.tok != EOF {
			elems = append(elems, p.parseExpr())
		}
		p.expectClose(RBRACK, open, "array")
		return &ArrayExpr{Elems: elems, Span: p.span(open)}
	}
	m := &MapExpr{}
	key := first
	for {
		e := &MapEntry{Trivia: t, Key: key}
		p.expect(COLON)
		e.Value = p.parseExpr()
		e.Span = p.span(key.Range().Start)
		m.Entries = append(m.Entries, e)
		more := p.got(COMMA)
		p.trailing(&e.Trivia, e.End)
		if !more || p.tok == RBRACK || p.tok == EOF {
			break
		}
		t = p.leading(p.pos)
		key = p.parseExpr()
	}
	p.expectClose(RBRACK, open, "map")
	m.Span = p.span(open)
	return m
}

func (p *parser) parseIfExpr() *IfExpr {
	start := p.expect(IF)
	x := &IfExpr{Cond: p.parseCond()}
	x.Then = p.parseValueBlock(true)
	if p.continues(ELSE) {
		p.next()
		if p.tok == IF {
			x.Else = p.parseIfExpr()
		} else {
			x.Else = p.parseValueBlock(true)
		}
	} else if p.styleValue == 0 {
		p.error(p.pos, diag.NoValue, "an if used as a value needs an else")
	}
	x.Span = p.span(start)
	return x
}

// parseString parses a string literal and its interpolations.
func (p *parser) parseString() *StringLit {
	start := p.pos
	s := &StringLit{}
	lit := p.lit
	if p.tok == STRING {
		var body string
		s.Raw, s.Hashes, s.Multiline, body = splitString(lit)
		s.Segments = []string{body}
		p.next()
		s.Span = p.span(start)
		lineBreaks(s)
		return s
	}
	// STRING_HEAD: "…{
	s.Multiline = strings.HasPrefix(lit, `"""`)
	open := 1
	if s.Multiline {
		open = 3
	}
	s.Segments = append(s.Segments, lit[open:len(lit)-1])
	p.next()
	for {
		lev := p.exprLev
		p.exprLev = 0
		var x Expr
		if p.tok == STRING_MID || p.tok == STRING_TAIL {
			p.error(p.pos, diag.SyntaxError, "expected an expression between '{' and '}' (a brace in a string is \\{)")
			x = &BadExpr{Span: Span{p.pos, p.pos}}
		} else {
			x = p.parseExpr()
		}
		p.exprLev = lev
		s.Interps = append(s.Interps, x)
		var spec *FormatSpec
		if p.tok == FORMAT_SPEC {
			spec = &FormatSpec{Span: Span{p.pos, p.pos + Pos(len(p.lit)+1)}, Text: p.lit}
			p.next()
		}
		s.Specs = append(s.Specs, spec)
		switch p.tok {
		case STRING_MID:
			s.Segments = append(s.Segments, p.lit[1:len(p.lit)-1])
			p.next()
			continue
		case STRING_TAIL:
			s.Segments = append(s.Segments, stripClose(p.lit[1:], s.Multiline, 0))
			p.next()
		default:
			p.errorExpected("'}' to end the interpolation")
			for p.tok != STRING_TAIL && p.tok != EOF && p.tok != NEWLINE {
				p.next()
			}
			if p.tok == STRING_TAIL {
				s.Segments = append(s.Segments, stripClose(p.lit[1:], s.Multiline, 0))
				p.next()
			} else {
				s.Segments = append(s.Segments, "")
			}
		}
		break
	}
	if !slices.ContainsFunc(s.Specs, func(f *FormatSpec) bool { return f != nil }) {
		s.Specs = nil // none has a format
	}
	s.Span = p.span(start)
	lineBreaks(s)
	return s
}

// lineBreaks makes a """ string's line breaks \n: one written in a file
// with Windows's line endings (\r\n) means what it does in any other.
func lineBreaks(s *StringLit) {
	if !s.Multiline {
		return
	}
	for i, seg := range s.Segments {
		if strings.Contains(seg, "\r\n") {
			s.Segments[i] = strings.ReplaceAll(seg, "\r\n", "\n")
		}
	}
}

// splitString splits a string token into its kind and its text.
func splitString(lit string) (raw bool, hashes int, multiline bool, body string) {
	if strings.HasPrefix(lit, "r") {
		raw = true
		lit = lit[1:]
		for strings.HasPrefix(lit, "#") {
			hashes++
			lit = lit[1:]
		}
	}
	if strings.HasPrefix(lit, `"""`) {
		return raw, hashes, true, stripClose(lit[3:], true, hashes)
	}
	return raw, hashes, false, stripClose(lit[1:], false, hashes)
}

// stripClose takes a string's closing quotes (and a raw string's #s)
// off its text, if it has them (it hasn't when it's not terminated).
func stripClose(s string, multiline bool, hashes int) string {
	q := `"`
	if multiline {
		q = `"""`
	}
	q += strings.Repeat("#", hashes)
	if strings.HasSuffix(s, q) {
		return s[:len(s)-len(q)]
	}
	return s
}

// ---------------------------------------------------------------------
// Types

func (p *parser) parseType() Type {
	if !p.enter() {
		return &BadType{Span: Span{p.pos, p.pos}}
	}
	defer p.leave()
	start := p.pos
	t := p.parseNonOptionalType()
	for {
		switch p.tok {
		case QUESTION:
			p.next()
			t = &OptionalType{Elem: t, Span: p.span(start)}
			continue
		case COALESCE: // T??
			p.next()
			t = &OptionalType{Elem: t, Span: Span{start, p.prevEnd - 1}}
			t = &OptionalType{Elem: t, Span: p.span(start)}
			continue
		}
		return t
	}
}

func (p *parser) parseNonOptionalType() Type {
	start := p.pos
	switch p.tok {
	case IDENT:
		t := &NamedType{Path: []*Ident{p.ident()}}
		for p.tok == PERIOD {
			p.next()
			t.Path = append(t.Path, p.ident())
		}
		if p.tok == LSS {
			open := p.pos
			p.next()
			for p.tok != GTR && p.tok != EOF {
				t.Args = append(t.Args, p.parseType())
				if !p.got(COMMA) {
					break
				}
			}
			p.lineBeforeAngle()
			t.Angles = Angles{open, p.expectClose(GTR, open, "type arguments")}
		}
		t.Span = p.span(start)
		return t
	case LBRACK:
		open := p.pos
		p.next()
		elem := p.parseType()
		if p.got(COLON) {
			val := p.parseType()
			p.expectClose(RBRACK, open, "map type")
			return &MapType{Key: elem, Value: val, Span: p.span(start)}
		}
		p.expectClose(RBRACK, open, "array type")
		return &ArrayType{Elem: elem, Span: p.span(start)}
	case LPAREN:
		open := p.pos
		p.next()
		var elems []Type
		comma := false
		for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
			elems = append(elems, p.parseType())
			if !p.got(COMMA) {
				break
			}
			comma = true
		}
		p.expectClose(RPAREN, open, "tuple type")
		if len(elems) == 1 && !comma {
			return elems[0] // (T) groups (decision 13)
		}
		return &TupleType{Elems: elems, Span: p.span(start)}
	case FN, ASYNC:
		t := &FuncType{}
		if p.tok == ASYNC {
			t.Async = p.pos
			p.next()
		}
		p.expect(FN)
		t.Lparen = p.expect(LPAREN)
		for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
			t.Params = append(t.Params, p.parseType())
			if !p.got(COMMA) {
				break
			}
		}
		t.Rparen = p.expectClose(RPAREN, t.Lparen, "parameter types")
		if p.tok == THROWS {
			t.Throws = p.pos
			p.next()
		}
		if p.got(ARROW) {
			t.Result = p.parseType()
		}
		t.Span = p.span(start)
		return t
	}
	p.errorExpected("a type")
	return &BadType{Span: Span{start, start}}
}

// ---------------------------------------------------------------------
// Patterns

func (p *parser) parsePattern() Pattern {
	if !p.enter() {
		return &BadPat{Span: Span{p.pos, p.pos}}
	}
	defer p.leave()
	start := p.pos
	x := p.parsePrimaryPattern()
	if p.tok != PIPE {
		return x
	}
	or := &OrPat{Alts: []Pattern{x}}
	for p.got(PIPE) {
		or.Alts = append(or.Alts, p.parsePrimaryPattern())
	}
	or.Span = p.span(start)
	return or
}

func (p *parser) parsePrimaryPattern() Pattern {
	start := p.pos
	switch p.tok {
	case IDENT, APP, BIND, COMPONENT, EFFECT, ENUM, EVENT, EXTERN, INTERFACE, KEY, NATIVE, ON, PROP, PROVIDE, SCREEN, STATE, STORE, STRUCT, STYLE, TASK, TEST, TYPE, USE, THROWS:
		if p.lit == "_" {
			p.next()
			return p.typedPat(&WildcardPat{Span: p.span(start)})
		}
		if p.peek(1) != PERIOD && p.peek(1) != LPAREN {
			id := p.ident()
			return p.typedPat(&BindPat{Name: id, Span: id.Span})
		}
		c := &CasePat{Path: []*Ident{p.ident()}}
		for p.got(PERIOD) {
			c.Path = append(c.Path, p.ident())
		}
		p.caseArgs(c)
		c.Span = p.span(start)
		return c
	case PERIOD:
		p.next()
		c := &CasePat{Implicit: true, Path: []*Ident{p.ident()}}
		p.caseArgs(c)
		c.Span = p.span(start)
		return c
	case INT, FLOAT, DURATION, LENGTH, COLOR, CHAR, STRING, STRING_HEAD, TRUE, FALSE, NONE:
		return &LitPat{Value: p.parsePrimary(), Span: p.span(start)}
	case SUB:
		if t := p.peek(1); t == INT || t == FLOAT || t == DURATION || t == LENGTH {
			p.next()
			x := p.parsePrimary()
			return &LitPat{Value: &UnaryExpr{Op: SUB, X: x, Span: p.span(start)}, Span: p.span(start)}
		}
	case LPAREN:
		open := p.pos
		p.next()
		var elems []Pattern
		comma := false
		for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
			elems = append(elems, p.parsePattern())
			if !p.got(COMMA) {
				break
			}
			comma = true
		}
		p.expectClose(RPAREN, open, "tuple pattern")
		if len(elems) == 1 && !comma {
			return elems[0]
		}
		return &TuplePat{Elems: elems, Span: p.span(start)}
	}
	p.errorExpectedName("a pattern")
	switch p.tok {
	case NEWLINE, SEMICOLON, RBRACE, RPAREN, FATARROW, ASSIGN, EOF, LBRACE, IN, COMMA:
	default:
		p.next()
	}
	return &BadPat{Span: p.span(start)}
}

// typedPat parses name: Type after a name or _ (x), a type test; x
// alone without the colon, or in a let's pattern.
func (p *parser) typedPat(x Pattern) Pattern {
	if p.tok != COLON || p.letPat {
		return x
	}
	p.next()
	t := &TypedPat{Pat: x, Type: p.parseType()}
	t.Span = Span{x.Range().Start, t.Type.Range().End}
	return t
}

func (p *parser) caseArgs(c *CasePat) {
	if p.tok != LPAREN {
		return
	}
	c.Lparen = p.pos
	p.next()
	c.Args = []Pattern{}
	for p.tok != RPAREN && p.tok != RBRACE && p.tok != EOF {
		if p.isName() && p.peek(1) == COLON {
			// A label: a struct's field (P(x: 0, y)), a case's payload's.
			for len(c.Labels) < len(c.Args) {
				c.Labels = append(c.Labels, nil)
			}
			c.Labels = append(c.Labels, p.name())
			p.next()
		}
		c.Args = append(c.Args, p.parsePattern())
		if len(c.Labels) > 0 {
			for len(c.Labels) < len(c.Args) {
				c.Labels = append(c.Labels, nil)
			}
		}
		if !p.got(COMMA) {
			break
		}
	}
	c.Rparen = p.expectClose(RPAREN, c.Lparen, "case's pattern")
}
