package check

import "github.com/egoist/asuka/lang/syntax"

// Info is what the checker found out about the program, by node, for
// the lowering.
type Info struct {
	Types      map[syntax.Expr]Type       // every expression's type
	Uses       map[*syntax.Ident]Object   // what each name used stands for
	Defs       map[*syntax.Ident]Object   // what each name declared is
	Calls      map[*syntax.CallExpr]*Call // what each call calls
	Selections map[syntax.Expr]*Selection // x.name and .name
	StoreSelf  map[syntax.Expr]*TypeName  // self in a store: the store
	// Convs: an expression whose value converts implicitly where it's
	// used (an int literal to a float aside, which is typed float): to
	// an optional, an interface, a Length, Edges.
	Convs map[syntax.Expr]Type
	Ctxs  map[syntax.Node]*FuncCtx // the functions closures make: lambdas, handlers, tasks, effects, spawn, bindings
	Cases map[*syntax.CasePat]*Case
	// FieldPats: a struct pattern's (P(x: 0, y)) fields, per argument
	// (nil where one names none).
	FieldPats map[*syntax.CasePat][]*Field
	// ErrorCases are the case patterns that match an Error by an error
	// enum's case (match e { MailError.offline => … }): that enum.
	ErrorCases map[*syntax.CasePat]*TypeName
	Catches    map[*syntax.CatchClause]*CatchInfo
	Fors       map[*syntax.ForStmt]*ForInfo
	Elements   map[*syntax.CallExpr]*ElementCall
	Matches    map[syntax.Node]*MatchInfo // MatchStmt and MatchExpr
	// ViewConds: a view's if whose condition is a chain (ViewCondChain).
	ViewConds map[*syntax.IfStmt]*ViewCond
	Lets      map[*syntax.LetDecl]Type // a local let's or var's type
	Emits     map[*syntax.EmitStmt]*Member
	Binds     map[*syntax.OnHandler][]*Var // an on handler's parameters
	// Trailing: a call's trailing block, as the closure it passes.
	Trailing map[*syntax.CallExpr]*syntax.LambdaExpr
	// SlotCalls: a slot named after a function, in a statement, read as
	// a call of it with that block (animate { … } in a handler).
	SlotCalls map[*syntax.SlotItem]*syntax.CallExpr
	// BodyCalls: a component's body's (or the app's) registrations,
	// useKeys(…) and onOpenURL(…): statements of its function.
	BodyCalls map[syntax.Item]bool
	// AppHandlers: the app block's events (on openURL(url) { }).
	AppHandlers []*HandlerArg
	// ScreenRoots: a screen's root, its Screen element: the body's prop
	// sets, handlers, bindings and slots in its block, its views its
	// children (made by the checker; no source of its own).
	ScreenRoots map[*TypeName]*syntax.CallExpr
	// Routes: a Route's screen builder (p => View); the routed screens
	// named in a Route's block (a Route of their pattern each); where
	// the Routers route each routed screen; the hrefs made of routed
	// screens' calls (navigate(Thread(id: 7))).
	RouteBuilders map[*ElementCall]*RouteBuilder
	// BuilderLambdas: the lambdas passed to an element's Builder key.
	BuilderLambdas map[*syntax.LambdaExpr]*RouteBuilder
	RoutedScreens  map[syntax.Item]*TypeName
	ScreenRoutes   map[*TypeName][]*ScreenRoute
	Hrefs          map[*syntax.CallExpr]*Href
	// LengthScales: a Length times a number (x * 1%).
	LengthScales map[*syntax.BinaryExpr]bool
	// DurationFloats: a Duration scaled by a float (d * 0.5), or divided
	// by another (a float).
	DurationFloats map[*syntax.BinaryExpr]bool
	// Operators: the binary operators a struct's method does (t + 1h).
	Operators map[*syntax.BinaryExpr]*Operator
	// Embedded: the file embed.text(…), bytes(…) or json(…) reads.
	Embedded map[*syntax.CallExpr]string
	// EmbeddedFiles: the files embed.texts(…) or files(…) names, by their
	// paths in the package, sorted.
	EmbeddedFiles map[*syntax.CallExpr][]EmbeddedFile
	// EmbedPaths are what the program embeds: a file, or a glob that a
	// new file matching it would change; asuka dev compiles again when one
	// changes.
	EmbedPaths map[EmbedPath]bool
	// Shaders: shader("""…""")'s programs, compiled.
	Shaders map[*syntax.CallExpr][]byte

	// For editors (lang/lsp).
	//
	// Imports: the package each import names (absent: it failed).
	Imports map[*syntax.ImportDecl]*Package
	// Expected: the type an implicit member (.name, .name(…)) was
	// checked against, nil where none was known; recorded whether the
	// member was found or not, for completion.
	Expected map[syntax.Expr]Type
}

func newInfo() *Info {
	return &Info{
		Types:          map[syntax.Expr]Type{},
		Uses:           map[*syntax.Ident]Object{},
		Defs:           map[*syntax.Ident]Object{},
		Calls:          map[*syntax.CallExpr]*Call{},
		Selections:     map[syntax.Expr]*Selection{},
		StoreSelf:      map[syntax.Expr]*TypeName{},
		Convs:          map[syntax.Expr]Type{},
		Ctxs:           map[syntax.Node]*FuncCtx{},
		Cases:          map[*syntax.CasePat]*Case{},
		FieldPats:      map[*syntax.CasePat][]*Field{},
		ErrorCases:     map[*syntax.CasePat]*TypeName{},
		Catches:        map[*syntax.CatchClause]*CatchInfo{},
		Fors:           map[*syntax.ForStmt]*ForInfo{},
		Elements:       map[*syntax.CallExpr]*ElementCall{},
		Matches:        map[syntax.Node]*MatchInfo{},
		ViewConds:      map[*syntax.IfStmt]*ViewCond{},
		Lets:           map[*syntax.LetDecl]Type{},
		Emits:          map[*syntax.EmitStmt]*Member{},
		Binds:          map[*syntax.OnHandler][]*Var{},
		Trailing:       map[*syntax.CallExpr]*syntax.LambdaExpr{},
		Shaders:        map[*syntax.CallExpr][]byte{},
		SlotCalls:      map[*syntax.SlotItem]*syntax.CallExpr{},
		BodyCalls:      map[syntax.Item]bool{},
		ScreenRoots:    map[*TypeName]*syntax.CallExpr{},
		RouteBuilders:  map[*ElementCall]*RouteBuilder{},
		BuilderLambdas: map[*syntax.LambdaExpr]*RouteBuilder{},
		RoutedScreens:  map[syntax.Item]*TypeName{},
		ScreenRoutes:   map[*TypeName][]*ScreenRoute{},
		Hrefs:          map[*syntax.CallExpr]*Href{},
		Embedded:       map[*syntax.CallExpr]string{},
		EmbeddedFiles:  map[*syntax.CallExpr][]EmbeddedFile{},
		EmbedPaths:     map[EmbedPath]bool{},
		Operators:      map[*syntax.BinaryExpr]*Operator{},
		LengthScales:   map[*syntax.BinaryExpr]bool{},
		DurationFloats: map[*syntax.BinaryExpr]bool{},
		Imports:        map[*syntax.ImportDecl]*Package{},
		Expected:       map[syntax.Expr]Type{},
	}
}

// CallKind is what a call calls.
type CallKind uint8

const (
	CallFunc          CallKind = iota + 1 // a declared function
	CallMethod                            // a struct's, an enum's, a store's or a component's method
	CallIface                             // an interface's method, by name
	CallNativeNew                         // a native interface made: X()
	CallNative                            // a native interface's method (Index: its number, from 1)
	CallBuiltinMethod                     // a built-in type's method (Method)
	CallBuiltin                           // a function the compiler provides
	CallClosure                           // a function value
	CallStruct                            // making a struct
	CallCase                              // an enum case with a payload
	CallComponent                         // a component, a screen, an element: a View
	CallStore                             // making a store
	CallTr                                // tr.key(…): a message of the app's catalogs (TrKey)
)

// A Call is what a call expression calls, with its arguments matched to
// the parameters.
type Call struct {
	Kind     CallKind
	Func     *Function
	TypeArgs []Type      // a generic function's
	Recv     syntax.Expr // a method's receiver; nil for a method of the enclosing type called by name
	RecvType Type
	Method   *Method // a built-in method
	Builtin  *Builtin
	Type     *Named // the struct, store or component made
	Case     *Case
	// Args holds, per parameter, the argument (nil: the default). For
	// variadic builtins (all, race, expect, min): the arguments as written.
	Args   []syntax.Expr
	Params []*Param
	Result Type
	// Bound are the built-in method's type variables (U in map).
	Bound map[string]Type
	// SelfCall: a method of the enclosing type, called by name (self's,
	// or the env's).
	SelfCall bool
	Optional bool   // x?.f(): none when x is
	Index    int    // CallNative: the method's number, from 1
	Name     string // CallIface, CallNative: the method's name
	// Awaited: animate(…) awaited, which returns once it's complete.
	Awaited bool
	// TrKey is CallTr's message; Args are its arguments, by its Params.
	TrKey *CatalogKey
}

// SelKind is what x.name or .name is.
type SelKind uint8

const (
	SelField         SelKind = iota + 1 // a struct's field
	SelCase                             // an enum's case, as a value
	SelConst                            // a type's constant
	SelMethod                           // a method, as a callee
	SelPkg                              // a package's member
	SelStoreMember                      // a store's state, let or fn
	SelIfaceField                       // an interface's field
	SelErrorMessage                     // an error's message
	SelBuiltinMethod                    // a built-in type's method, as a callee
	SelColor                            // a semantic color: Value
	SelLengthAuto                       // .auto, a Length
	SelLoading                          // a Loading's value, pending, error
	SelTupleElem                        // a tuple's element (t.0 isn't parsed; unused)
	SelBuiltinField                     // a built-in type's property: g.width, card.hovered (Method)
)

// A Selection is what a member expression or an implicit member is.
type Selection struct {
	Kind       SelKind
	Recv       Type // the receiver's type (for an implicit member: the expected one)
	Field      *Field
	Case       *Case
	Global     *Global
	Member     *Member
	Obj        Object
	Value      int64
	Name       string
	Optional   bool    // x?.name
	MemberType Type    // x?.name's: name's own type (an optional one isn't boxed again)
	Method     *Method // SelBuiltinField's
}

// CatchInfo is what a catch clause catches.
type CatchInfo struct {
	All  bool      // catch, catch e, catch e: Error
	Type *TypeName // catch e: T, catch T.case
	Case *Case     // catch T.case(...)
	Var  *Var      // catch e
	Args []syntax.Pattern
}

// ForKind is what a for iterates.
type ForKind uint8

const (
	ForArray      ForKind = iota + 1
	ForRange              // a..b, a..<b
	ForMap                // (key, value) in a map
	ForEnumerated         // (i, x) in xs.enumerated()
	ForStream             // for await x in a stream
	ForSet                // x in a set, in insertion order
)

// ForInfo is a for loop's iteration.
type ForInfo struct {
	Kind  ForKind
	Seq   syntax.Expr // the array, the map, the range's start
	End   syntax.Expr // the range's end
	Incl  bool        // a..b
	Elem  Type
	Item  *Var // the item (the value of a map), or nil for _
	Index *Var // the index (enumerated), the key (a map)
	Pat   syntax.Pattern
	View  bool // in a view: Item and Index are accessors
	Key   syntax.Expr
	Lazy  bool // directly in an element that builds rows lazily (a List)
}

// MatchInfo is a match's subject type and arms' bindings.
type MatchInfo struct {
	Subject Type
	View    bool
	// Exhaustive: the arms cover every value (an exhaustive match of an
	// enum without _); otherwise falling off the end is an error that
	// can't happen for statements, and the checker reported it for
	// expressions.
	Exhaustive bool
}

// ElementCall is a component's, a screen's or an element's use: its
// header arguments and what its block holds.
type ElementCall struct {
	Comp     *TypeName
	Type     *Named
	Args     []syntax.Expr // per header parameter; nil: its default
	Props    []*PropArg    // the block's props (styles expanded), later ones override earlier
	Handlers []*HandlerArg
	Binds    []*BindArg
	Slots    []*SlotArg
	Children []syntax.Item // the view items, for children
	Variants []*VariantArg // a container's: pressed { … }
	Ref      syntax.Expr   // ref: r, a Ref the element's view goes in
	As       *Var          // Element as name: a Ref local of the enclosing view
}

// VariantArg is a container's variant: a state's block of style props.
type VariantArg struct {
	State string // prelude.Variants' name
	Props []*PropArg
	Item  *syntax.SlotItem
}

// PropArg is a prop set: name: value.
type PropArg struct {
	Member *Member
	Value  syntax.Expr
	Style  bool // from a style
}

// HandlerArg is on event { }.
type HandlerArg struct {
	Event   *Member
	Handler *syntax.OnHandler
}

// BindArg is bind name: target.
type BindArg struct {
	Prop   *Member
	Event  *Member // the event the control writes it with
	Target syntax.Expr
	Item   *syntax.BindItem
}

// SlotArg is name { … }.
type SlotArg struct {
	Prop  *Member
	Block *syntax.Block
	Item  *syntax.SlotItem
}

// CtxKind is what a function body is.
type CtxKind uint8

const (
	CtxFunc      CtxKind = iota + 1 // a declared function
	CtxMethod                       // a struct's, an enum's, a store's or a component's method
	CtxLambda                       // x => …
	CtxHandler                      // on event { }
	CtxTask                         // task { }
	CtxEffect                       // effect { }
	CtxTest                         // test "…" { }
	CtxBinding                      // a binding the UI follows: a prop's value, a let, a condition
	CtxBuilder                      // a builder of views: a branch, a row, a slot
	CtxComponent                    // a component's or a screen's body: its function
	CtxStore                        // a store's body: its constructor
	CtxGlobal                       // a package-level initializer
	CtxSpawn                        // spawn { }
	CtxApp                          // app { }
	CtxLocalFn                      // a fn declared in a block (LocalFn)
)

// A FuncCtx is a function body being checked: a declared function, or a
// closure the lowering makes (lambdas, handlers, bindings, ...).
type FuncCtx struct {
	Kind   CtxKind
	Parent *FuncCtx
	Node   syntax.Node
	Func   *Function
	Comp   *TypeName // the component or store whose members it reaches
	Name   string    // for tracebacks

	Async  bool // may await
	Awaits bool // does
	Throws bool // may let errors out
	Result Type

	Captures []*Var // outer locals it uses, in order
	capIndex map[*Var]bool
	UsesEnv  bool // uses its component's or store's members
	Self     *Var // a method's receiver
	Params   []*Var

	// Its scope (the lowering doesn't need it).
	scope *Scope

	// readsState: reads package state or a memo, or calls something that
	// does (for the reactive analysis and spawn).
	readsState bool
	calls      []*Function
	inferRes   bool // a lambda whose result is inferred from its body
	threw      bool // lets an error out
	// The first await (or for await) and the first thing that lets an
	// error out (a try, a throw, an untried call): what made an inferred
	// closure async or throwing, for the errors at its calls.
	awaitAt  syntax.Pos
	threwAt  syntax.Node
	globals  []*Global
	retTypes []Type
}

// awaited notes an await (or a for await) at pos in the body.
func (c *FuncCtx) awaited(pos syntax.Pos) {
	if !c.Awaits {
		c.awaitAt = pos
	}
	c.Awaits = true
}

// IsClosure reports whether the lowering makes the body a closure.
func (c *FuncCtx) IsClosure() bool {
	switch c.Kind {
	case CtxFunc, CtxMethod, CtxComponent, CtxStore, CtxGlobal, CtxTest, CtxApp:
		return false
	}
	return true
}

func (c *FuncCtx) capture(v *Var) {
	if c.capIndex == nil {
		c.capIndex = map[*Var]bool{}
	}
	if !c.capIndex[v] {
		c.capIndex[v] = true
		c.Captures = append(c.Captures, v)
	}
}

// Program is a checked program: its packages, in dependency order (the
// prelude first, the root last), and what the checker found.
type Program struct {
	Fset     *syntax.FileSet
	Packages []*Package
	Prelude  *Package
	Root     *Package
	// Main is the root package's fn main: the program's entry (nil for
	// an app or a library).
	Main  *Function
	Info  *Info
	Diags []Diagnostic
	// Warnings are what's worth saying that doesn't stop the build.
	Warnings []Diagnostic
	// Catalog is the app's catalogs (tr), or nil.
	Catalog *Catalog
	checker *Checker
}
