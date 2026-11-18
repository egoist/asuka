package syntax

import "strings"

// The syntax tree.
//
// Every node embeds a Span, the source it covers, and so has Range. The
// node kinds follow the grammar of RFC 0007:
//
//   - a SourceFile holds Imports and Decls;
//   - a Decl is a top-level declaration: a component, a screen, a store,
//     a struct, an enum, an interface, a function, a type alias, a
//     style, a state, a let, an app or a test (native interfaces and
//     components are InterfaceDecl and ComponentDecl with Native set;
//     extern fns and structs FuncDecl and StructDecl with Extern set,
//     extern types ExternTypeDecl);
//   - a Block's Items are members (prop, event, state, let, use,
//     provide, fn, task, effect), view items (a prop set, on, bind,
//     style, a named slot, if, for, match, an element or any
//     expression) and statements (let, var, assignment, if, for, while,
//     match, do/catch, return, throw, break, continue, defer, emit, an
//     expression). The parser accepts all of them in every block; which
//     a block may hold (no return in a component's body, no prop set in
//     a function) is the checker's to say, with an error that names the
//     item rather than a parse error that loses the rest of the block;
//   - Exprs, Types and Patterns.
//
// Nodes that stand on lines of their own (declarations, imports, items,
// fields, enum cases, match arms, arguments and map entries) embed
// Trivia: the comments above them, one after them on their last line,
// and whether a blank line comes before them.

// A Node is any node of the tree.
type Node interface {
	Range() Span
}

// A Decl is a top-level declaration.
type Decl interface {
	Node
	declNode()
}

// An Item is what a block holds: a member, a view item or a statement.
type Item interface {
	Node
	itemNode()
}

// An Expr is an expression.
type Expr interface {
	Node
	exprNode()
}

// A Type is a type expression.
type Type interface {
	Node
	typeNode()
}

// A Pattern is what a match arm, a let, a for or a catch matches.
type Pattern interface {
	Node
	patternNode()
}

// ---------------------------------------------------------------------
// Comments

// A CommentGroup is a run of comments on consecutive lines.
type CommentGroup struct {
	List []*Comment
	// BlankBefore reports a blank line between the group and what's
	// above it.
	BlankBefore bool
}

// Trivia is what a node on a line of its own keeps of the source around
// it, for the printer.
type Trivia struct {
	Leading     []*CommentGroup // the comments above the node
	Trailing    *Comment        // a comment after the node, on its last line
	BlankBefore bool            // a blank line between the node and what's above it (its comments included)
}

func (t *Trivia) trivia() *Trivia { return t }

// HasComments reports whether t holds a comment.
func (t *Trivia) HasComments() bool { return len(t.Leading) > 0 || t.Trailing != nil }

// Doc is the text of the /// comments right above the node (in the
// group of comments with no blank line between it and the node; a plain
// // comment among them is a note, not part of it), without their
// slashes and one space after them; "" if there are none.
func (t *Trivia) Doc() string {
	var b strings.Builder
	for i, c := range t.DocComments() {
		if i > 0 {
			b.WriteByte('\n')
		}
		line := c.Text[3:]
		line = strings.TrimPrefix(line, " ")
		b.WriteString(line)
	}
	return b.String()
}

// DocComments are the /// comments Doc reads, in order.
func (t *Trivia) DocComments() []*Comment {
	if len(t.Leading) == 0 || t.BlankBefore {
		return nil
	}
	var out []*Comment
	for _, c := range t.Leading[len(t.Leading)-1].List {
		if c.IsDoc() {
			out = append(out, c)
		}
	}
	return out
}

type hasTrivia interface{ trivia() *Trivia }

// TriviaOf is n's Trivia, nil for a node that doesn't stand on a line of
// its own.
func TriviaOf(n Node) *Trivia {
	if t, ok := n.(hasTrivia); ok {
		return t.trivia()
	}
	return nil
}

// ---------------------------------------------------------------------
// Files and imports

// A SourceFile is a parsed .asuka file.
type SourceFile struct {
	Span
	File     *File // the file's positions; nil for a tree made in code
	Imports  []*ImportDecl
	Decls    []Decl
	Dangling []*CommentGroup // the comments after the last declaration
	Comments []*Comment      // every comment of the file, in source order, wherever the tree keeps it
}

// An ImportDecl is import std/http, import ./components .{Row, Card}
// as c.
type ImportDecl struct {
	Span
	Trivia
	Relative bool     // ./path: a directory beside this file's
	Up       int      // ../path: as many directories up first (Relative too)
	Path     []*Ident // the path's segments
	Names    []*Ident // .{A, B}: names imported unqualified; nil for none
	Lbrace   Pos      // the .{ before Names
	Rbrace   Pos      // the } after them
	Alias    *Ident   // as name; nil for none
}

// PathString is the import's path as written: std/http, ./components.
func (d *ImportDecl) PathString() string {
	var b strings.Builder
	if d.Up > 0 {
		b.WriteString(strings.Repeat("../", d.Up))
	} else if d.Relative {
		b.WriteString("./")
	}
	for i, s := range d.Path {
		if i > 0 {
			b.WriteByte('/')
		}
		b.WriteString(s.Name)
	}
	return b.String()
}

// ---------------------------------------------------------------------
// Declarations

// A TypeParam is a generic parameter: T, or T: Hashable.
type TypeParam struct {
	Span
	Name        *Ident
	Constraints []Type // T: A & B; none for none
}

// Angles are the positions of the < and the > around a list of type
// parameters or type arguments; NoPos without one.
type Angles struct{ Lt, Gt Pos }

// A Param is a parameter of a function, a component's header, an event
// or a lambda: name: Type = default. A lambda's may have no Type.
type Param struct {
	Span
	Name    *Ident
	Type    Type // nil: a lambda's, inferred
	Default Expr // nil for none
}

// A ParamList is a parenthesized list of parameters.
type ParamList struct {
	Span // from ( to )
	List []*Param
}

// A ComponentDecl is component Name<T>(params) { … }, or with Native
// set, native component Name(params) { … }.
type ComponentDecl struct {
	Span
	Trivia
	Pub        Pos // the pub keyword; NoPos if not exported
	Native     Pos // the native keyword; NoPos for an Asuka component
	Name       *Ident
	TypeParams []*TypeParam
	TypeAngles Angles     // the <…> around TypeParams
	Params     *ParamList // nil without parentheses
	Body       *Block
}

// A ScreenDecl is screen Name(params) { … }: a component whose root is a
// Screen.
type ScreenDecl struct {
	Span
	Trivia
	Pub    Pos
	Name   *Ident
	Params *ParamList // nil without parentheses
	// At is the route pattern of a routed screen (screen Thread(id: int)
	// at ":id" { … }): relative to the Route it's named in, unless it
	// starts with /. nil for none.
	At   *StringLit
	Body *Block
}

// A StoreDecl is store Name<T>(params) { members }.
type StoreDecl struct {
	Span
	Trivia
	Pub        Pos
	Name       *Ident
	TypeParams []*TypeParam
	TypeAngles Angles     // the <…> around TypeParams
	Params     *ParamList // nil without parentheses
	Body       *Block
}

// A WorkerDecl is worker Name(params) { members }: a type whose
// instances each run on a thread of their own (its vars, lets and fns).
type WorkerDecl struct {
	Span
	Trivia
	Pub    Pos
	Name   *Ident
	Params *ParamList // nil without parentheses
	Body   *Block
}

// A ShaderDecl is shader Name(params) { fn fragment(pos: vec2) -> vec4
// { … } … }: a GPU program that colors each pixel of a view, its
// parameters its uniforms, used as a view (Glow(tint: .accent) { … }).
// Its members are functions (fragment, and helpers) and constants
// (let), in a subset of the language a GPU runs (lang/check's
// shaderlang.go). "shader" is a keyword only here: before a name at a
// declaration's start.
type ShaderDecl struct {
	Span
	Trivia
	Pub      Pos
	Name     *Ident
	Params   *ParamList // nil without parentheses
	Lbrace   Pos
	Members  []Node // *FuncDecl and *LetDecl, in source order
	Dangling []*CommentGroup
	Rbrace   Pos
}

// A StructDecl is struct Name<T>: Error { fields and methods }, or with
// Extern set, extern struct Name { fields }: a C struct, passed by value.
type StructDecl struct {
	Span
	Trivia
	Pub        Pos
	Extern     Pos // the extern keyword; NoPos for an Asuka struct
	Name       *Ident
	TypeParams []*TypeParam
	TypeAngles Angles // the <…> around TypeParams
	Conforms   []Type // after the colon; nil for none
	Lbrace     Pos
	Members    []Node // *Field, *LetDecl (a constant of the type: Font.title) and *FuncDecl, in source order
	Dangling   []*CommentGroup
	Rbrace     Pos
}

// A Field is a struct's or an interface's field: name: Type = default.
type Field struct {
	Span
	Trivia
	Pub     Pos
	Name    *Ident
	Type    Type
	Default Expr // nil for none
}

// An EnumDecl is enum Name<T>: Error { cases, then methods }.
type EnumDecl struct {
	Span
	Trivia
	Pub        Pos
	Name       *Ident
	TypeParams []*TypeParam
	TypeAngles Angles // the <…> around TypeParams
	Conforms   []Type
	Lbrace     Pos
	Cases      []*EnumCase
	Members    []Node // after the cases: *LetDecl (constants of the type) and *FuncDecl (methods), in source order
	Dangling   []*CommentGroup
	Rbrace     Pos
}

// An EnumCase is a case of an enum: name, or name(Type, label: Type).
type EnumCase struct {
	Span
	Trivia
	Name   *Ident
	Params *CaseParams // nil without parentheses
}

// CaseParams are an enum case's payload.
type CaseParams struct {
	Span // from ( to )
	List []*CaseParam
}

// A CaseParam is a payload's element: Type, or label: Type.
type CaseParam struct {
	Span
	Label *Ident // nil for none
	Type  Type
}

// An InterfaceDecl is interface Name { signatures and fields }, or with
// Native set, native interface Name { … }.
type InterfaceDecl struct {
	Span
	Trivia
	Pub        Pos
	Native     Pos
	Name       *Ident
	TypeParams []*TypeParam
	TypeAngles Angles // the <…> around TypeParams
	Lbrace     Pos
	Members    []Node // *FuncDecl (with neither Body nor Value) and *Field
	Dangling   []*CommentGroup
	Rbrace     Pos
}

// An ExtendDecl is extend<T> Type: Interface { members }: fns, mut fns,
// static fns and constants added to a type (RFC 0011), its own or
// another package's, built in or declared.
type ExtendDecl struct {
	Span
	Trivia
	Pub        Pos // an error: pub goes on an extension's members
	TypeParams []*TypeParam
	TypeAngles Angles // the <…> around TypeParams
	Type       Type
	Conforms   []Type
	Lbrace     Pos
	Members    []Node // *FuncDecl and *LetDecl (and, an error, *Field), in source order
	Dangling   []*CommentGroup
	Rbrace     Pos
}

// A FuncDecl is [async] fn name<T>(params) [throws] [-> Result], with a
// Body, = Value, or (in an interface) neither. It's a declaration and,
// in a block, a member.
type FuncDecl struct {
	Span
	Trivia
	Pub        Pos
	Async      Pos // NoPos if not async
	Name       *Ident
	TypeParams []*TypeParam
	TypeAngles Angles // the <…> around TypeParams
	Params     *ParamList
	Throws     Pos  // NoPos if it doesn't throw
	Result     Type // nil for none (void)
	Body       *Block
	Value      Expr // fn f() -> T = expr
	// Native: native fn f(…) -> T, with no body: the runtime implements
	// it (the standard library's).
	Native Pos
	// Extern: extern fn f(…) -> T, with no body: a C function of that
	// name (RFC 0008, docs/c.md).
	Extern Pos
	// Static: static fn f(…) in a struct's, an enum's or a store's body:
	// the type's function (Money.of(dollars: 2)), with no self.
	Static Pos
	// Mut: mut fn, a struct's, an enum's or an extension's method that
	// changes the value it's called on (RFC 0011).
	Mut Pos
}

// An ExternTypeDecl is extern type Name with release fn: a pointer C
// gives, which Asuka holds and frees with Release (nil: never) as the
// last reference goes.
type ExternTypeDecl struct {
	Span
	Trivia
	Pub     Pos
	Extern  Pos
	Name    *Ident
	Release *Ident // nil without with release
}

// A TypeAliasDecl is type Name<T> = Type.
type TypeAliasDecl struct {
	Span
	Trivia
	Pub        Pos
	Name       *Ident
	TypeParams []*TypeParam
	TypeAngles Angles // the <…> around TypeParams
	Type       Type
}

// A StyleDecl is style name { … }, or style name(params) { … }: style
// props, style: and use, and variants, applied with style:.
type StyleDecl struct {
	Span
	Trivia
	Pub    Pos
	Name   *Ident
	Params *ParamList // nil without parentheses
	Body   *Block
}

// A StateDecl is state name: Type = value: a signal. It's a declaration
// (state the app owns) and a member. When value reads what changes, the
// state follows it (it's value until written, and value again when what
// value read changes); state name(deps) = value follows deps only.
type StateDecl struct {
	Span
	Trivia
	Pub    Pos
	Name   *Ident
	Deps   []Expr // state name(a, b) = …: what it follows; nil without parentheses
	Lparen Pos    // NoPos without parentheses
	Rparen Pos
	Type   Type // nil: inferred
	Value  Expr
}

// A LetDecl is let pattern: Type = value, or var …. At the top level and
// in a component's or a store's body a let is a memo; in statements, an
// immutable local, and a var a mutable one. In statements, let pattern
// = value else { … } (Else set) binds what the pattern matches, an
// optional's value for a name, and runs the else block, which must
// leave, when it doesn't match.
type LetDecl struct {
	Span
	Trivia
	Pub     Pos
	Keyword Token // LET or VAR
	Pattern Pattern
	Type    Type   // nil: inferred
	Value   Expr   // nil only after an error
	Else    *Block // let … else { … }; nil for none
}

// An AppDecl is app { … }: the app's root.
type AppDecl struct {
	Span
	Trivia
	Pub  Pos
	Body *Block
}

// A TestDecl is test "name" { statements }.
type TestDecl struct {
	Span
	Trivia
	Name *StringLit
	Body *Block
}

// A BadDecl is a declaration that didn't parse.
type BadDecl struct {
	Span
	Trivia
}

func (*ComponentDecl) declNode()  {}
func (*ScreenDecl) declNode()     {}
func (*StoreDecl) declNode()      {}
func (*WorkerDecl) declNode()     {}
func (*StructDecl) declNode()     {}
func (*ShaderDecl) declNode()     {}
func (*EnumDecl) declNode()       {}
func (*InterfaceDecl) declNode()  {}
func (*ExtendDecl) declNode()     {}
func (*FuncDecl) declNode()       {}
func (*TypeAliasDecl) declNode()  {}
func (*ExternTypeDecl) declNode() {}
func (*StyleDecl) declNode()      {}
func (*StateDecl) declNode()      {}
func (*LetDecl) declNode()        {}
func (*AppDecl) declNode()        {}
func (*TestDecl) declNode()       {}
func (*BadDecl) declNode()        {}

// ---------------------------------------------------------------------
// Blocks and items

// A Block is { items }: a component's, a store's, an element's, a
// handler's or a function's body.
type Block struct {
	Span
	Lbrace   Pos
	Comment  *Comment // a comment after the {, on its line
	Items    []Item
	Dangling []*CommentGroup // comments after the last item
	Rbrace   Pos
}

// A PropDecl is prop name: Type = default.
type PropDecl struct {
	Span
	Trivia
	Pub     Pos
	Name    *Ident
	Type    Type // nil: inferred from Default
	Default Expr // nil: the caller must set it
}

// An EventDecl is event name(params).
type EventDecl struct {
	Span
	Trivia
	Pub    Pos
	Name   *Ident
	Params *ParamList // nil without parentheses
}

// A VariantDecl is variant name = condition: a state of a component's
// that its caller styles (name { … } in its block, or in a style) while
// the condition holds. variant is a contextual keyword: it's an item only
// followed by a name and =.
type VariantDecl struct {
	Span
	Trivia
	Pub   Pos
	Name  *Ident
	Value Expr
}

// A UseDecl is use name: Type: a value provided by an enclosing
// component.
type UseDecl struct {
	Span
	Trivia
	Name *Ident
	Type Type
}

// A ProvideDecl is provide value: makes a value available to the
// components inside.
type ProvideDecl struct {
	Span
	Trivia
	Value Expr
}

// A TaskDecl is task(deps) { … }: restarts when a dependency changes;
// task { … } runs once while mounted.
type TaskDecl struct {
	Span
	Trivia
	Deps   []Expr
	Lparen Pos // NoPos without parentheses
	Rparen Pos
	Body   *Block
}

// An EffectDecl is effect { … }: runs again when what its body reads
// changes; effect(deps) { … } runs again when a dependency changes, its
// body following nothing.
type EffectDecl struct {
	Span
	Trivia
	Deps   []Expr
	Lparen Pos // NoPos without parentheses
	Rparen Pos
	Body   *Block
}

// A PropSet is name: value in a block: sets a prop of the element.
type PropSet struct {
	Span
	Trivia
	Name  *Ident
	Value Expr
}

// An OnHandler is on name(params) { … }: handles an event.
type OnHandler struct {
	Span
	Trivia
	Name   *Ident
	Params []*Ident
	Lparen Pos // NoPos without parentheses
	Rparen Pos
	Body   *Block
}

// A BindItem is bind name: target: a two-way binding.
type BindItem struct {
	Span
	Trivia
	Name   *Ident
	Target Expr
}

// A StyleUse is style name (or style pkg.name): applies a style.
type StyleUse struct {
	Span
	Trivia
	Name Expr // an *Ident or a *MemberExpr
}

// A SlotItem is name { … }: fills a named slot (toolbar { … }).
type SlotItem struct {
	Span
	Trivia
	Name *Ident
	Body *Block
}

// An IfStmt is if cond { … } else …, in a view or in statements.
type IfStmt struct {
	Span
	Trivia
	Cond Expr   // an Expr, a *LetCond, or a *CondList of them
	Then *Block // never nil
	Else Node   // nil, an *IfStmt or a *Block
}

// A ForStmt is for pattern in seq key k { … } (in a view) or for [await]
// pattern in seq { … }.
type ForStmt struct {
	Span
	Trivia
	Await   Pos // NoPos without await
	Pattern Pattern
	Seq     Expr
	Key     Expr   // nil without a key
	Label   *Ident // for … as name: the loop's name, nil without one
	Body    *Block
}

// A WhileStmt is while cond { … }.
type WhileStmt struct {
	Span
	Trivia
	Cond  Expr   // an Expr, a *LetCond, or a *CondList of them
	Label *Ident // while … as name: the loop's name, nil without one
	Body  *Block
}

// A MatchStmt is match subject { arms }, in a view or in statements.
type MatchStmt struct {
	Span
	Trivia
	Subject Expr
	Lbrace  Pos
	Arms    []*MatchArm
	Rbrace  Pos
}

// A MatchArm is pattern if guard => body. In a MatchStmt the body is an
// Item or a *Block; in a MatchExpr an Expr or a *Block.
type MatchArm struct {
	Span
	Trivia
	Pattern Pattern
	Guard   Expr // nil without if
	Body    Node
}

// A DoStmt is do { … } catch pattern { … }….
type DoStmt struct {
	Span
	Trivia
	Body    *Block
	Catches []*CatchClause
}

// A CatchClause is catch { … }, catch e { … }, catch e: Type { … } or
// catch Case(…) { … }.
type CatchClause struct {
	Span
	Pattern Pattern // nil: catch everything, unnamed
	Type    Type    // catch e: Type
	Body    *Block
}

// A ReturnStmt is return [value].
type ReturnStmt struct {
	Span
	Trivia
	Value Expr // nil for none
}

// A ThrowStmt is throw err.
type ThrowStmt struct {
	Span
	Trivia
	Value Expr
}

// A BranchStmt is break or continue.
type BranchStmt struct {
	Span
	Trivia
	Tok   Token  // BREAK or CONTINUE
	Label *Ident // break name: the loop it leaves, nil for the innermost
}

// A DeferStmt is defer { … }.
type DeferStmt struct {
	Span
	Trivia
	Body *Block
}

// A CleanupStmt is cleanup { … }: its block runs when the owner it's
// registered with goes, or before its effect runs again (rfcs/0016).
// cleanup is contextual: a name elsewhere.
type CleanupStmt struct {
	Span
	Trivia
	Body *Block
}

// An EmitStmt is emit name(args).
type EmitStmt struct {
	Span
	Trivia
	Name *Ident
	Args *ArgList // nil without parentheses
}

// An AssignStmt is target op value, op one of = += -= *= /= %= ??=.
type AssignStmt struct {
	Span
	Trivia
	Target Expr
	Op     Token
	OpPos  Pos
	Value  Expr
}

// An ExprStmt is an expression on its own: in a view an element or a
// View-typed value, in statements a call.
type ExprStmt struct {
	Span
	Trivia
	X Expr
}

// A BadItem is an item that didn't parse.
type BadItem struct {
	Span
	Trivia
}

func (*PropDecl) itemNode()    {}
func (*EventDecl) itemNode()   {}
func (*VariantDecl) itemNode() {}
func (*StateDecl) itemNode()   {}
func (*LetDecl) itemNode()     {}
func (*UseDecl) itemNode()     {}
func (*ProvideDecl) itemNode() {}
func (*FuncDecl) itemNode()    {}
func (*TaskDecl) itemNode()    {}
func (*EffectDecl) itemNode()  {}
func (*StyleDecl) itemNode()   {}
func (*PropSet) itemNode()     {}
func (*OnHandler) itemNode()   {}
func (*BindItem) itemNode()    {}
func (*StyleUse) itemNode()    {}
func (*SlotItem) itemNode()    {}
func (*IfStmt) itemNode()      {}
func (*ForStmt) itemNode()     {}
func (*WhileStmt) itemNode()   {}
func (*MatchStmt) itemNode()   {}
func (*DoStmt) itemNode()      {}
func (*ReturnStmt) itemNode()  {}
func (*ThrowStmt) itemNode()   {}
func (*BranchStmt) itemNode()  {}
func (*DeferStmt) itemNode()   {}
func (*CleanupStmt) itemNode() {}
func (*EmitStmt) itemNode()    {}
func (*AssignStmt) itemNode()  {}
func (*ExprStmt) itemNode()    {}
func (*BadItem) itemNode()     {}

// ---------------------------------------------------------------------
// Expressions

// An Ident is a name.
type Ident struct {
	Span
	Name string
}

// A BasicLit is a number, a duration, a length or a color, as written.
type BasicLit struct {
	Span
	Kind  Token // INT, FLOAT, DURATION, LENGTH or COLOR
	Value string
}

// A StringLit is a string: Segments are its text as written (escapes
// and all) around its interpolations, so len(Segments) ==
// len(Interps)+1; a """ string's line breaks are \n, whether the file's
// are \n or \r\n.
type StringLit struct {
	Span
	Raw       bool // r"…": no escapes, no interpolation
	Hashes    int  // r#"…"#: the #s each side of a raw string's quotes
	Multiline bool // """…"""
	Segments  []string
	Interps   []Expr
	// Specs are the interpolations' formats ({x:>8.2}), one per
	// interpolation, nil for one without; nil when none has one.
	Specs []*FormatSpec
}

// A FormatSpec is an interpolation's format: the text after its colon,
// "{price:>8.2}"'s ">8.2".
type FormatSpec struct {
	Span
	Text string
}

// A BoolLit is true or false.
type BoolLit struct {
	Span
	Value bool
}

// A NoneLit is none.
type NoneLit struct {
	Span
}

// A SelfExpr is self.
type SelfExpr struct {
	Span
}

// An AtExpr is a compile-time constant: @platform, the platform the
// program is compiled for. Name is what follows the @ ("platform").
type AtExpr struct {
	Span
	Name string
}

// An ImplicitMember is .name: a member of the type that's expected
// (.gray, .ready).
type ImplicitMember struct {
	Span
	Name *Ident
}

// A MemberExpr is x.name, or with Optional, x?.name.
type MemberExpr struct {
	Span
	X        Expr
	Optional bool
	Dot      Pos // the . or ?.
	Name     *Ident
}

// An ArgList is a call's (args).
type ArgList struct {
	Span // from ( to )
	List []*Arg
}

// An Arg is a call's argument: value, or label: value.
type Arg struct {
	Span
	Trivia
	Label *Ident // nil for a positional argument
	Value Expr
}

// A CallExpr is a call: f(args), an element with a trailing block:
// Text("hi") { color: .red }, Row { … }, and the handlers chained after
// the block: Boundary { … } on error(e) { … }.
type CallExpr struct {
	Span
	Fun      Expr
	TypeArgs []Type   // f<T>(…): explicit type arguments; nil for none
	Angles   Angles   // the <…> around TypeArgs
	Args     *ArgList // nil: no parentheses, Row { … }
	As       *Ident   // View as card { … }: the element's name, a Ref; nil for none
	Block    *Block   // the trailing block; nil for none
	Handlers []*OnHandler
}

// An IndexExpr is x[index].
type IndexExpr struct {
	Span
	X     Expr
	Index Expr
	// Default: m[k, default: v], a map's entry, or v when it has none
	// (RFC 0011); nil for m[k].
	Default Expr
}

// An UnwrapExpr is x!: the value of an optional, which mustn't be none.
type UnwrapExpr struct {
	Span
	X Expr
}

// A UnaryExpr is op x, op one of ! - await load try, and TRY with
// Optional for try?.
type UnaryExpr struct {
	Span
	Op       Token // NOT, SUB, AWAIT, LOAD, RELOAD or TRY
	Optional bool  // try?
	X        Expr
}

// A SpawnExpr is spawn { … }: a block run on a worker thread.
type SpawnExpr struct {
	Span
	Body *Block
}

// A BinaryExpr is x op y.
type BinaryExpr struct {
	Span
	X     Expr
	Op    Token
	OpPos Pos
	Y     Expr
}

// A LambdaExpr is x => body or (a, b: T) => body, maybe after async
// (async () => …); the body is an Expr or a *Block.
type LambdaExpr struct {
	Span
	Async          Pos // NoPos if not marked async (its async-ness is then inferred)
	Params         []*Param
	Parens         bool // the parameters are in parentheses
	Lparen, Rparen Pos  // the parentheses; NoPos without
	Body           Node
}

// A ParenExpr is (x).
type ParenExpr struct {
	Span
	X Expr
}

// A TupleExpr is (a, b), or () with no elements.
type TupleExpr struct {
	Span
	Elems []Expr
}

// An ArrayExpr is [a, b].
type ArrayExpr struct {
	Span
	Elems []Expr
}

// A MapExpr is [k: v, …], or [:] with no entries.
type MapExpr struct {
	Span
	Entries []*MapEntry
}

// A MapEntry is a map literal's key: value.
type MapEntry struct {
	Span
	Trivia
	Key, Value Expr
}

// An IfExpr is if cond { a } else { b } as a value.
type IfExpr struct {
	Span
	Cond Expr // as IfStmt's
	Then *Block
	Else Node // an *IfExpr or a *Block; nil only after an error
}

// A MatchExpr is match subject { arms } as a value.
type MatchExpr struct {
	Span
	Subject Expr
	Lbrace  Pos
	Arms    []*MatchArm
	Rbrace  Pos
}

// A LetCond is let pattern = value as an if's or a while's condition.
type LetCond struct {
	Span
	Pattern Pattern
	Value   Expr
	// Var: if var x = optional, a copy that may change (RFC 0011).
	Var bool
}

// A CondList is a condition of clauses separated by commas, as an if's
// or a while's: if let a = x, let b = y, a > b { … }. Each clause is a
// bool Expr or a *LetCond, in any order; a later one sees what an
// earlier one binds, and the condition holds when every clause does
// (they're tried in order, and stop at the first that doesn't). A
// condition of one clause is that clause, not a CondList.
type CondList struct {
	Span
	List []Expr
}

// A JumpExpr is throw err, return [value], break or continue used as a
// value: x ?? throw E.missing, x ?? return, a match arm's or a lambda's
// body. It never has a value: its type is never, which fits wherever a
// value of any type is expected. On a line of its own, each is a
// statement (ThrowStmt, ReturnStmt, BranchStmt), not a JumpExpr.
type JumpExpr struct {
	Span
	Tok   Token  // THROW, RETURN, BREAK or CONTINUE
	Value Expr   // throw's error, return's value; nil for none
	Label *Ident // break's or continue's loop name, nil for none
}

// A CatchExpr is x catch … { … }…: x's value, or when x throws, the
// value of the first catch clause that takes the error (its block's
// last expression, or the block leaves); an error no clause takes goes
// on. The clauses are DoStmt's: try parsePort(s) catch e { 80 }.
type CatchExpr struct {
	Span
	X       Expr
	Catches []*CatchClause
}

// A BadExpr is an expression that didn't parse.
type BadExpr struct {
	Span
}

func (*Ident) exprNode()          {}
func (*BasicLit) exprNode()       {}
func (*StringLit) exprNode()      {}
func (*BoolLit) exprNode()        {}
func (*NoneLit) exprNode()        {}
func (*SelfExpr) exprNode()       {}
func (*AtExpr) exprNode()         {}
func (*ImplicitMember) exprNode() {}
func (*MemberExpr) exprNode()     {}
func (*CallExpr) exprNode()       {}
func (*IndexExpr) exprNode()      {}
func (*UnwrapExpr) exprNode()     {}
func (*UnaryExpr) exprNode()      {}
func (*SpawnExpr) exprNode()      {}
func (*BinaryExpr) exprNode()     {}
func (*LambdaExpr) exprNode()     {}
func (*ParenExpr) exprNode()      {}
func (*TupleExpr) exprNode()      {}
func (*ArrayExpr) exprNode()      {}
func (*MapExpr) exprNode()        {}
func (*IfExpr) exprNode()         {}
func (*MatchExpr) exprNode()      {}
func (*LetCond) exprNode()        {}
func (*CondList) exprNode()       {}
func (*JumpExpr) exprNode()       {}
func (*CatchExpr) exprNode()      {}
func (*BadExpr) exprNode()        {}

// ---------------------------------------------------------------------
// Types

// A NamedType is a type by name: int, Message, http.Response, Task<T>.
type NamedType struct {
	Span
	Path   []*Ident // the qualified name's parts
	Args   []Type   // <…>; nil for none
	Angles Angles   // the <…> around Args
}

// An ArrayType is [Elem].
type ArrayType struct {
	Span
	Elem Type
}

// A MapType is [Key: Value].
type MapType struct {
	Span
	Key, Value Type
}

// A TupleType is (A, B), or () with no elements.
type TupleType struct {
	Span
	Elems []Type
}

// A FuncType is [async] fn(Params) [throws] [-> Result].
type FuncType struct {
	Span
	Async          Pos
	Lparen, Rparen Pos // around Params
	Params         []Type
	Throws         Pos
	Result         Type // nil for none
}

// An OptionalType is Elem?.
type OptionalType struct {
	Span
	Elem Type
}

// A BadType is a type that didn't parse.
type BadType struct {
	Span
}

func (*NamedType) typeNode()    {}
func (*ArrayType) typeNode()    {}
func (*MapType) typeNode()      {}
func (*TupleType) typeNode()    {}
func (*FuncType) typeNode()     {}
func (*OptionalType) typeNode() {}
func (*BadType) typeNode()      {}

// ---------------------------------------------------------------------
// Patterns

// A WildcardPat is _.
type WildcardPat struct {
	Span
}

// A BindPat is a name: matches anything, and binds it.
type BindPat struct {
	Span
	Name *Ident
}

// A LitPat is a literal to compare with: 0, -1, "x", true, none.
type LitPat struct {
	Span
	Value Expr // a literal, or a UnaryExpr negating a number
}

// A CasePat is an enum case: .ready(value), Mail.received(m),
// HttpError.notFound.
type CasePat struct {
	Span
	Implicit bool     // .name: the case of the type that's expected
	Path     []*Ident // the case's qualified name, or a struct's (P(x: 0, y))
	Args     []Pattern
	// Labels are the Args' labels (x: 0), nil where one has none; nil
	// when none has one.
	Labels []*Ident
	Lparen Pos // NoPos without parentheses
	Rparen Pos
}

// Label is the label of argument i, nil when it has none.
func (c *CasePat) Label(i int) *Ident {
	if i < len(c.Labels) {
		return c.Labels[i]
	}
	return nil
}

// A TypedPat is name: Type, or _: Type: a type test, which matches a
// value of an interface (or of a type parameter) when it's a Type, and
// binds it as one (match shape { c: Circle => … }, if let c: Circle =
// shape).
type TypedPat struct {
	Span
	Pat  Pattern // a *BindPat or a *WildcardPat
	Type Type
}

// A TuplePat is (a, b).
type TuplePat struct {
	Span
	Elems []Pattern
}

// An OrPat is a | b: matches either.
type OrPat struct {
	Span
	Alts []Pattern
}

// A BadPat is a pattern that didn't parse.
type BadPat struct {
	Span
}

func (*WildcardPat) patternNode() {}
func (*BindPat) patternNode()     {}
func (*LitPat) patternNode()      {}
func (*CasePat) patternNode()     {}
func (*TypedPat) patternNode()    {}
func (*TuplePat) patternNode()    {}
func (*OrPat) patternNode()       {}
func (*BadPat) patternNode()      {}
