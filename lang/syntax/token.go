// Package syntax is the front end of the Asuka language's compiler: its
// tokens and positions, the scanner, the syntax tree, the parser and the
// printer that formats a tree back to canonical source (asuka fmt).
//
// The language is specified in rfcs/0007-c-core-and-asuka-language.md
// ("The language"); GRAMMAR.md is its grammar now, and parser.go writes
// down how this package resolves what the grammar leaves open.
package syntax

import (
	"fmt"
	"sort"
	"strconv"
	"sync"
)

// A Pos is a position in a FileSet: a file's base plus a byte offset in
// it. The zero Pos, NoPos, is no position at all.
type Pos int

// NoPos is the zero Pos: a node made in code, or a part that's absent.
const NoPos Pos = 0

// IsValid reports whether p is a position.
func (p Pos) IsValid() bool { return p != NoPos }

// A Span is the source a node covers: from Start up to, not including,
// End.
type Span struct {
	Start, End Pos
}

// Range returns s: every node embeds a Span, so every node has Range.
func (s Span) Range() Span { return s }

// A Position is a Pos as people read it.
type Position struct {
	Filename string
	Offset   int // in bytes, from 0
	Line     int // from 1
	Column   int // in bytes, from 1
}

// IsValid reports whether the position names a line.
func (p Position) IsValid() bool { return p.Line > 0 }

// String is file:line:col, line:col without a file, or "-" for no
// position.
func (p Position) String() string {
	s := p.Filename
	if p.IsValid() {
		if s != "" {
			s += ":"
		}
		s += strconv.Itoa(p.Line) + ":" + strconv.Itoa(p.Column)
	}
	if s == "" {
		s = "-"
	}
	return s
}

// A File is one source file of a FileSet: its name, where it starts in
// the set, and where its lines start.
type File struct {
	name  string
	base  int
	size  int
	mu    sync.Mutex
	lines []int // the offset of each line's first byte; lines[0] == 0
}

// Name is the file's name, as given to AddFile.
func (f *File) Name() string { return f.name }

// Base is the Pos of the file's first byte.
func (f *File) Base() int { return f.base }

// Size is the file's length in bytes.
func (f *File) Size() int { return f.size }

// LineCount is the number of lines seen so far.
func (f *File) LineCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.lines)
}

// AddLine records that a line starts at offset. Offsets must increase;
// one that doesn't is ignored. The scanner calls it.
func (f *File) AddLine(offset int) {
	f.mu.Lock()
	if n := len(f.lines); (n == 0 || f.lines[n-1] < offset) && offset < f.size {
		f.lines = append(f.lines, offset)
	}
	f.mu.Unlock()
}

// Pos is the position of the byte at offset.
func (f *File) Pos(offset int) Pos {
	if offset < 0 || offset > f.size {
		panic("syntax: offset out of range")
	}
	return Pos(f.base + offset)
}

// Offset is p's offset in the file.
func (f *File) Offset(p Pos) int {
	if int(p) < f.base || int(p) > f.base+f.size {
		panic("syntax: Pos out of range")
	}
	return int(p) - f.base
}

// Line is the line p is on, from 1.
func (f *File) Line(p Pos) int {
	return f.Position(p).Line
}

// Position is p as a file name, a line and a column.
func (f *File) Position(p Pos) Position {
	if !p.IsValid() {
		return Position{}
	}
	offset := f.Offset(p)
	f.mu.Lock()
	i := sort.Search(len(f.lines), func(i int) bool { return f.lines[i] > offset }) - 1
	line, col := i+1, offset-f.lines[i]+1
	f.mu.Unlock()
	return Position{Filename: f.name, Offset: offset, Line: line, Column: col}
}

// A FileSet is a set of files whose positions don't overlap, so a Pos
// alone says which file it's in.
type FileSet struct {
	mu    sync.Mutex
	base  int
	files []*File
}

// NewFileSet returns an empty set.
func NewFileSet() *FileSet { return &FileSet{base: 1} }

// AddFile adds a file of size bytes named name. Its positions follow the
// last file's, with one to spare, so a file's end is a position too.
func (s *FileSet) AddFile(name string, size int) *File {
	s.mu.Lock()
	defer s.mu.Unlock()
	f := &File{name: name, base: s.base, size: size, lines: []int{0}}
	s.base += size + 1
	s.files = append(s.files, f)
	return f
}

// File is the file p is in, nil if none.
func (s *FileSet) File(p Pos) *File {
	if !p.IsValid() {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	i := sort.Search(len(s.files), func(i int) bool { return s.files[i].base > int(p) }) - 1
	if i >= 0 && int(p) <= s.files[i].base+s.files[i].size {
		return s.files[i]
	}
	return nil
}

// Position is p as a file name, a line and a column.
func (s *FileSet) Position(p Pos) Position {
	if f := s.File(p); f != nil {
		return f.Position(p)
	}
	return Position{}
}

// Token is the kind of a lexical token.
type Token uint8

const (
	ILLEGAL Token = iota
	EOF
	NEWLINE // a newline that ends an item, a statement or a member
	COMMENT // the scanner keeps comments aside; Scan never returns one

	literalBeg
	IDENT       // count, Text, _
	INT         // 42, 1_000, 0xff
	FLOAT       // 3.14, 1e-3
	DURATION    // 300ms, 5s, 2min, 1h, 1.5s
	LENGTH      // 50%, 1fr
	COLOR       // #rgb, #rgba, #rrggbb, #rrggbbaa
	STRING      // "…", r"…", """…""": a string with no interpolation
	STRING_HEAD // "…{: a string up to its first interpolation
	STRING_MID  // }…{: between two interpolations
	STRING_TAIL // }…": after the last interpolation
	FORMAT_SPEC // :>10.2 in "{x:>10.2}": an interpolation's format, its text after the colon
	literalEnd

	operatorBeg
	ADD   // +
	SUB   // -
	MUL   // *
	QUO   // /
	REM   // %
	AMP   // &: bitwise and
	CARET // ^: bitwise exclusive or
	TILDE // ~: bitwise not
	SHL   // <<
	SHR   // >>: the parser's, of two adjacent >s (the scanner never makes one, so a type's Box<Box<T>> closes twice)
	WADD  // &+: an int's sum, wrapping
	WSUB  // &-: an int's difference, wrapping
	WMUL  // &*: an int's product, wrapping

	ASSIGN          // =
	ADD_ASSIGN      // +=
	SUB_ASSIGN      // -=
	MUL_ASSIGN      // *=
	QUO_ASSIGN      // /=
	REM_ASSIGN      // %=
	AND_ASSIGN      // &=
	OR_ASSIGN       // |=
	XOR_ASSIGN      // ^=
	SHL_ASSIGN      // <<=
	SHR_ASSIGN      // >>=: the parser's, of > and an adjacent >=
	WADD_ASSIGN     // &+=
	WSUB_ASSIGN     // &-=
	WMUL_ASSIGN     // &*=
	COALESCE_ASSIGN // ??=

	EQL // ==
	NEQ // !=
	LSS // <
	LEQ // <=
	GTR // >
	GEQ // >=

	LAND     // &&
	LOR      // ||
	NOT      // !
	COALESCE // ??
	QUESTION // ?
	OPTDOT   // ?.
	FATARROW // =>
	ARROW    // ->
	RANGE    // ..
	RANGELT  // ..<
	PERIOD   // .
	DOTBRACE // .{
	PIPE     // |
	AT       // @: a scoped package's import path

	COMMA     // ,
	COLON     // :
	SEMICOLON // ;
	LPAREN    // (
	RPAREN    // )
	LBRACK    // [
	RBRACK    // ]
	LBRACE    // {
	RBRACE    // }
	operatorEnd

	keywordBeg
	APP
	AS
	ASYNC
	AWAIT
	BIND
	BREAK
	CATCH
	COMPONENT
	CONTINUE
	DEFER
	DO
	EFFECT
	ELSE
	EMIT
	ENUM
	EVENT
	EXTEND
	EXTERN
	FALSE
	FN
	FOR
	IF
	IMPORT
	IN
	INTERFACE
	KEY
	LET
	LOAD
	MATCH
	MUT
	NATIVE
	NONE
	ON
	PROP
	PROVIDE
	PUB
	RELOAD
	RETURN
	SCREEN
	SELF
	SPAWN
	STATE
	STORE
	STRUCT
	STYLE
	TASK
	TEST
	THROW
	THROWS
	TRUE
	TRY
	TYPE
	USE
	VAR
	WHILE
	WORKER
	keywordEnd
)

var tokens = [...]string{
	ILLEGAL: "ILLEGAL",
	EOF:     "EOF",
	NEWLINE: "newline",
	COMMENT: "comment",

	IDENT:       "identifier",
	INT:         "integer",
	FLOAT:       "float",
	DURATION:    "duration",
	LENGTH:      "length",
	COLOR:       "color",
	STRING:      "string",
	STRING_HEAD: "string",
	STRING_MID:  "string",
	STRING_TAIL: "string",
	FORMAT_SPEC: "format spec",

	ADD:   "+",
	SUB:   "-",
	MUL:   "*",
	QUO:   "/",
	REM:   "%",
	AMP:   "&",
	CARET: "^",
	TILDE: "~",
	SHL:   "<<",
	SHR:   ">>",
	WADD:  "&+",
	WSUB:  "&-",
	WMUL:  "&*",

	ASSIGN:          "=",
	ADD_ASSIGN:      "+=",
	SUB_ASSIGN:      "-=",
	MUL_ASSIGN:      "*=",
	QUO_ASSIGN:      "/=",
	REM_ASSIGN:      "%=",
	AND_ASSIGN:      "&=",
	OR_ASSIGN:       "|=",
	XOR_ASSIGN:      "^=",
	SHL_ASSIGN:      "<<=",
	SHR_ASSIGN:      ">>=",
	WADD_ASSIGN:     "&+=",
	WSUB_ASSIGN:     "&-=",
	WMUL_ASSIGN:     "&*=",
	COALESCE_ASSIGN: "??=",

	EQL: "==",
	NEQ: "!=",
	LSS: "<",
	LEQ: "<=",
	GTR: ">",
	GEQ: ">=",

	LAND:     "&&",
	LOR:      "||",
	NOT:      "!",
	COALESCE: "??",
	QUESTION: "?",
	OPTDOT:   "?.",
	FATARROW: "=>",
	ARROW:    "->",
	RANGE:    "..",
	RANGELT:  "..<",
	PERIOD:   ".",
	DOTBRACE: ".{",
	PIPE:     "|",
	AT:       "@",

	COMMA:     ",",
	COLON:     ":",
	SEMICOLON: ";",
	LPAREN:    "(",
	RPAREN:    ")",
	LBRACK:    "[",
	RBRACK:    "]",
	LBRACE:    "{",
	RBRACE:    "}",

	APP:       "app",
	AS:        "as",
	ASYNC:     "async",
	AWAIT:     "await",
	BIND:      "bind",
	BREAK:     "break",
	CATCH:     "catch",
	COMPONENT: "component",
	CONTINUE:  "continue",
	DEFER:     "defer",
	DO:        "do",
	EFFECT:    "effect",
	ELSE:      "else",
	EMIT:      "emit",
	ENUM:      "enum",
	EVENT:     "event",
	EXTEND:    "extend",
	EXTERN:    "extern",
	FALSE:     "false",
	FN:        "fn",
	FOR:       "for",
	IF:        "if",
	IMPORT:    "import",
	IN:        "in",
	INTERFACE: "interface",
	KEY:       "key",
	LET:       "let",
	LOAD:      "load",
	MATCH:     "match",
	MUT:       "mut",
	NATIVE:    "native",
	NONE:      "none",
	ON:        "on",
	PROP:      "prop",
	PROVIDE:   "provide",
	PUB:       "pub",
	RELOAD:    "reload",
	RETURN:    "return",
	SCREEN:    "screen",
	SELF:      "self",
	SPAWN:     "spawn",
	STATE:     "state",
	STORE:     "store",
	STRUCT:    "struct",
	STYLE:     "style",
	TASK:      "task",
	TEST:      "test",
	THROW:     "throw",
	THROWS:    "throws",
	TRUE:      "true",
	TRY:       "try",
	TYPE:      "type",
	USE:       "use",
	VAR:       "var",
	WHILE:     "while",
	WORKER:    "worker",
}

// String is the token's spelling for operators and keywords, its kind's
// name for the rest.
func (t Token) String() string {
	if int(t) < len(tokens) && tokens[t] != "" {
		return tokens[t]
	}
	return fmt.Sprintf("token(%d)", int(t))
}

// IsLiteral reports whether t is a literal (identifiers included).
func (t Token) IsLiteral() bool { return literalBeg < t && t < literalEnd }

// IsOperator reports whether t is an operator or a delimiter.
func (t Token) IsOperator() bool { return operatorBeg < t && t < operatorEnd }

// IsKeyword reports whether t is a keyword.
func (t Token) IsKeyword() bool { return keywordBeg < t && t < keywordEnd }

// IsAssign reports whether t is = or an assignment operator (+= … ??=).
func (t Token) IsAssign() bool { return ASSIGN <= t && t <= COALESCE_ASSIGN }

// AssignOp is the binary operator of a compound assignment (+= is +),
// ILLEGAL for = and ??=.
func (t Token) AssignOp() Token {
	switch t {
	case ADD_ASSIGN:
		return ADD
	case SUB_ASSIGN:
		return SUB
	case MUL_ASSIGN:
		return MUL
	case QUO_ASSIGN:
		return QUO
	case REM_ASSIGN:
		return REM
	case AND_ASSIGN:
		return AMP
	case OR_ASSIGN:
		return PIPE
	case XOR_ASSIGN:
		return CARET
	case SHL_ASSIGN:
		return SHL
	case SHR_ASSIGN:
		return SHR
	case WADD_ASSIGN:
		return WADD
	case WSUB_ASSIGN:
		return WSUB
	case WMUL_ASSIGN:
		return WMUL
	}
	return ILLEGAL
}

// IsBitwise reports the operators that take ints only: & | ^ << >>,
// and the wrapping &+ &- &*.
func (t Token) IsBitwise() bool {
	switch t {
	case AMP, PIPE, CARET, SHL, SHR, WADD, WSUB, WMUL:
		return true
	}
	return false
}

var keywords map[string]Token

func init() {
	keywords = make(map[string]Token, keywordEnd-keywordBeg)
	for t := keywordBeg + 1; t < keywordEnd; t++ {
		keywords[tokens[t]] = t
	}
}

// Lookup is the keyword spelled ident, or IDENT if it isn't one.
func Lookup(ident string) Token {
	if t, ok := keywords[ident]; ok {
		return t
	}
	return IDENT
}

// Precedence is a binary operator's precedence, from 1 (loosest: ??) to
// 8 (<< >>); 0 for a token that isn't a binary operator. The unary and
// postfix operators bind tighter than any of them. As Swift's: the
// shifts bind tighter than multiplication, & and &* as tightly as *, |,
// ^, &+ and &- as tightly as +.
func (t Token) Precedence() int {
	switch t {
	case COALESCE:
		return 1
	case LOR:
		return 2
	case LAND:
		return 3
	case EQL, NEQ, LSS, LEQ, GTR, GEQ:
		return 4
	case RANGE, RANGELT:
		return 5
	case ADD, SUB, PIPE, CARET, WADD, WSUB:
		return 6
	case MUL, QUO, REM, AMP, WMUL:
		return 7
	case SHL, SHR:
		return 8
	}
	return 0
}

// An Error is a diagnostic: where, and what's wrong.
type Error struct {
	Pos  Position
	End  Position // where the problem ends; may be Pos
	Code string   // its code in lang/diag (syntax-error, unterminated, ...)
	Msg  string
}

func (e *Error) Error() string { return e.Pos.String() + ": " + e.Msg }

// An ErrorList is a file's diagnostics, in source order.
type ErrorList []*Error

func (l ErrorList) Error() string {
	switch len(l) {
	case 0:
		return "no errors"
	case 1:
		return l[0].Error()
	}
	return fmt.Sprintf("%s (and %d more errors)", l[0], len(l)-1)
}

// Err is l as an error: nil when it's empty.
func (l ErrorList) Err() error {
	if len(l) == 0 {
		return nil
	}
	return l
}

// Sort puts l in source order.
func (l ErrorList) Sort() {
	sort.SliceStable(l, func(i, j int) bool {
		a, b := l[i].Pos, l[j].Pos
		if a.Filename != b.Filename {
			return a.Filename < b.Filename
		}
		return a.Offset < b.Offset
	})
}

// IsIdentifier reports whether s is a name a declaration can have: _
// or a letter, then letters, combining marks, digits and _ (IsIdentStart,
// IsIdentContinue), and not a keyword.
func IsIdentifier(s string) bool {
	if s == "" {
		return false
	}
	for i, r := range s {
		if i == 0 && !IsIdentStart(r) || i > 0 && !IsIdentContinue(r) {
			return false
		}
	}
	t, kw := keywords[s]
	return !kw || softKeyword(t)
}
