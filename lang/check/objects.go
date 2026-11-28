package check

import (
	"github.com/egoist/asuka/lang/prelude"
	"github.com/egoist/asuka/lang/syntax"
)

// An Object is what a name stands for.
type Object interface {
	Name() string
	Pos() syntax.Pos
}

type object struct {
	name string
	pos  syntax.Pos
}

func (o *object) Name() string    { return o.name }
func (o *object) Pos() syntax.Pos { return o.pos }

// ---------------------------------------------------------------------
// Packages and scopes

// A Package is a directory of .asuka files, the prelude, or a package of
// the standard library the compiler provides (std/json).
type Package struct {
	Path string // as imported: std/json, ./components; "" for the root, "prelude"
	Name string // its last segment
	Dir  string
	// NativeClass is what its platforms' code registers its native
	// interfaces under (internal/nativegen's Package.Path): asuka.toml's
	// name, else its directory's.
	NativeClass string
	Files       []*syntax.SourceFile
	Scope       *Scope
	Prelude     bool
	qualify     bool // types print as Name.Type
	// Broken: the package couldn't be loaded, which its import reported.
	// It has no members, and what a file uses of it isn't reported again.
	Broken bool

	Globals []*Global   // top-level state and let, in source order
	Funcs   []*Function // top-level functions
	Types   []*TypeName // declared types
	Tests   []*Test
	// Previews are its preview blocks (preview.go).
	Previews []*Preview
	Exts     []*Extension // its extend declarations
	App      *syntax.AppDecl
	Imports  []*Package

	fileScopes map[*syntax.SourceFile]*Scope
	// Styles are its styles, by name: another package applies a pub one
	// as pkg.name.
	Styles     map[string]*StyleDef
	styleOrder []*StyleDef
	// defaults are its default styles, by the element or component.
	defaults map[*TypeName]*StyleDef
}

// FileScope is the scope of f's imports (inside the package's), nil for
// a file that isn't the package's.
func (p *Package) FileScope(f *syntax.SourceFile) *Scope { return p.fileScopes[f] }

// A Test is a test block, or a bench (Bench set, Decl nil).
type Test struct {
	Name string
	Decl *syntax.TestDecl
	// Bench is a bench "name" { … }: a benchmark (testing.go).
	Bench *syntax.BenchDecl
	// Params are a property test's parameters, whose values asuka test
	// generates (testing.go); none for a test without.
	Params []*Var
	// Measure is a bench's measure { … }, the part it times; nil: the
	// whole body.
	Measure *syntax.MeasureStmt
	Ctx     *FuncCtx
	Pkg     *Package
}

// Node is the test's declaration: its test block, or its bench.
func (t *Test) Node() syntax.Decl {
	if t.Bench != nil {
		return t.Bench
	}
	return t.Decl
}

// Body is the test's statements.
func (t *Test) Body() *syntax.Block {
	if t.Bench != nil {
		return t.Bench.Body
	}
	return t.Decl.Body
}

// A Scope maps names to objects.
type Scope struct {
	parent *Scope
	names  map[string]Object
}

// NewScope makes a scope inside parent.
func NewScope(parent *Scope) *Scope { return &Scope{parent: parent} }

// Lookup finds name here or in an enclosing scope.
func (s *Scope) Lookup(name string) Object {
	for ; s != nil; s = s.parent {
		if o, ok := s.names[name]; ok {
			return o
		}
	}
	return nil
}

// LookupType finds the type named name in s or its parents: types and
// values are named apart, so a function or a method named int (asuka/
// random's) doesn't hide the type int.
func (s *Scope) LookupType(name string) *TypeName {
	for ; s != nil; s = s.parent {
		if tn, ok := s.names[name].(*TypeName); ok {
			return tn
		}
	}
	return nil
}

// Parent is the scope s is in: a package's is the prelude's, whose is
// the universe's (nil).
func (s *Scope) Parent() *Scope { return s.parent }

// LookupLocal finds name in s only.
func (s *Scope) LookupLocal(name string) Object { return s.names[name] }

// InsertAs adds o under another name than its own (an alias).
func (s *Scope) InsertAs(name string, o Object) {
	if s.names == nil {
		s.names = map[string]Object{}
	}
	s.names[name] = o
}

// Insert adds o; it returns what was there already under its name.
func (s *Scope) Insert(o Object) Object {
	if s.names == nil {
		s.names = map[string]Object{}
	}
	if old, ok := s.names[o.Name()]; ok {
		return old
	}
	s.names[o.Name()] = o
	return nil
}

// Names lists the scope's own names (for suggestions).
func (s *Scope) Names() []string {
	var out []string
	for n := range s.names {
		out = append(out, n)
	}
	return out
}

// ---------------------------------------------------------------------
// Objects

// VarKind is what a local name holds.
type VarKind uint8

const (
	LocalVar   VarKind = iota // let, var, a parameter, a pattern's binding
	RowItem                   // a for's item or index in a view: an accessor
	ViewMemo                  // a match payload or if let in a view: a memo
	SelfVar                   // a method's receiver
	LocalFnVar                // a fn declared in a block (Var.Fn)
	// StyleLocal is a style's parameter or use: each application of the
	// style gives it a register, or a memo (lang/ir/style.go).
	StyleLocal
	// StateSelf is a state's own name in its initializer (rfcs/0012):
	// its current value, an optional, none the first time.
	StateSelf
)

// A Var is a local: a parameter, let, var, or a pattern's binding.
type Var struct {
	object
	Type    Type
	Kind    VarKind
	Mutable bool
	Bound   bool     // a pattern's or a for's name: a copy of what it matched
	Ctx     *FuncCtx // the function it belongs to

	// Captured: a closure uses it. Assigned: it's written after its
	// declaration; AssignedInClosure: by a closure that captured it.
	Captured, Assigned, AssignedInClosure bool

	// Fn is a local fn's (Kind LocalFnVar).
	Fn *LocalFn
	// RefComp: the Ref of Component as name, the component: its
	// variants read as name.checked.
	RefComp *TypeName
}

// A LocalFn is a fn declared in a block of statements. The fns declared
// one after another in a block are a group: they see each other, so
// they may call each other, and one lowering serves them all.
type LocalFn struct {
	Var   *Var
	Decl  *syntax.FuncDecl
	Sig   *Signature
	Ctx   *FuncCtx // its body's
	Group *LocalGroup
}

// A LocalGroup is the fns declared one after another in a block. They
// share what they capture: each is lowered as a function taking its
// parameters and then the group's captures (and the env, when one uses
// it), so a call of one from another (or itself) passes them on, and no
// closure holds another, nor itself (lang/LOWERING.md).
type LocalGroup struct {
	Fns      []*LocalFn
	Encl     *FuncCtx // where they're declared
	Captures []*Var   // the outer locals they use, in order
	UsesEnv  bool     // they use the component's or store's members
}

// Boxed reports whether the var lives in a box (VBOX) so that the
// closures that use it and the code around them share one variable: a
// var a closure captures, which something changes (the closure, or the
// code around it after making it). spawn takes a copy of the value
// instead (values only).
func (v *Var) Boxed() bool {
	return v.Mutable && (v.AssignedInClosure || v.Captured && v.Assigned)
}

// A Function is a declared function or method.
type Function struct {
	object
	Pkg        *Package
	Decl       *syntax.FuncDecl
	Sig        *Signature
	TypeParams []*TypeParam
	Recv       *TypeName // a method's type (struct, enum, store, component); nil for a function
	Static     *TypeName // a static fn's type (Money.of), which has no receiver
	// Ext: an extension's fn (a method, with its receiver first, or a
	// static fn, Static set too).
	Ext *Extension
	Ctx *FuncCtx // its body's
	// Extern: an extern fn's C side (nil for the rest).
	Extern *Extern

	// What the reactive analysis and spawn's check need.
	readsState bool
	calls      []*Function
	reads      int // 0 unknown, 1 computing, 2 done
	stateful   bool
}

// IsMethod reports whether f has a receiver.
func (f *Function) IsMethod() bool { return f.Recv != nil }

// Signature is a function's parameters and result.
type Signature struct {
	Params []*Param
	Result Type
	Async  bool
	Throws bool
	// Mut: a mut fn, which changes the value it's called on (its
	// receiver is a place: lang/LOWERING.md's "Methods that change").
	Mut bool
}

// applySig is sig with s's type parameters replaced.
func (s Subst) applySig(sig *Signature) *Signature {
	out := &Signature{Result: s.Apply(sig.Result), Async: sig.Async, Throws: sig.Throws, Mut: sig.Mut}
	for _, p := range sig.Params {
		out.Params = append(out.Params, &Param{Name: p.Name, Type: s.Apply(p.Type), Default: p.Default})
	}
	return out
}

// Type is the signature as a function type.
func (s *Signature) Type() *FuncType {
	ps := make([]Type, len(s.Params))
	for i, p := range s.Params {
		ps[i] = p.Type
	}
	return &FuncType{ps, s.Result, s.Async, s.Throws}
}

// A Param is a parameter of a function, a component, an event.
type Param struct {
	Name    string
	Type    Type
	Default syntax.Expr // nil: required
	Var     *Var        // in the function's body
}

// TypeKind is what a TypeName declares.
type TypeKind uint8

const (
	BasicType TypeKind = iota
	StructType
	EnumType
	InterfaceType
	StoreType
	ComponentType
	AliasType
	ParamType  // a type parameter
	ExternType // extern type T: a pointer C gives (RFC 0008)
	WorkerType // worker W { … }: its instances each run on a thread of their own
)

// A TypeName is a declared type: a struct, an enum, an interface, a
// store, a worker, a component (or element), an alias, or a built-in one.
type TypeName struct {
	object
	Pkg        *Package
	Kind       TypeKind
	Decl       syntax.Node
	TypeParams []*TypeParam
	Type       Type // the type it names (for generic ones: with its parameters)

	// Structs.
	Fields     []*Field
	fieldIndex map[string]int
	// Enums.
	Cases     []*Case
	caseIndex map[string]int
	// Structs and enums.
	Methods  map[string]*Function
	Consts   map[string]*Global
	Statics  map[string]*Function // static fn: the type's functions (Money.of(…))
	IsError  bool
	Conforms []*TypeName // interfaces it declares
	// Interfaces.
	Iface        *Iface
	isErrorIface bool
	// Native interfaces: their platforms' code implements them;
	// NativeIndex is their place among the package's.
	Native      bool
	NativeIndex int
	// Extern: an extern struct (its fields' C types are Field.C).
	Extern bool
	// Release: an extern type's with release, nil for none.
	Release *Function
	// Stores, workers and components.
	Comp *Component
	// Own: what a struct or an enum says itself of its ==, hash, text
	// and JSON (own.go); nil when it says nothing. OwnExts: what its
	// package's extensions say, for the types they extend (OwnOf).
	Own     *Own
	OwnExts []*Own

	resolved bool
}

// Field returns the struct's field named name.
func (t *TypeName) Field(name string) *Field {
	if i, ok := t.fieldIndex[name]; ok {
		return t.Fields[i]
	}
	return nil
}

// Case returns the enum's case named name.
func (t *TypeName) Case(name string) *Case {
	if i, ok := t.caseIndex[name]; ok {
		return t.Cases[i]
	}
	return nil
}

// HasPayloads reports whether a case of the enum has a payload.
func (t *TypeName) HasPayloads() bool {
	for _, c := range t.Cases {
		if len(c.Params) > 0 {
			return true
		}
	}
	return false
}

// RefEnum reports whether the enum's values are references (ak_enum):
// it has payloads, it's an error, or it declares an interface. The
// others are ints, their case's index.
func (t *TypeName) RefEnum() bool {
	return t.Kind == EnumType && (t.HasPayloads() || t.IsError || len(t.Conforms) > 0)
}

// A Field is a struct's field.
type Field struct {
	Name    string
	Type    Type
	Default syntax.Expr
	Index   int
	Pos     syntax.Pos
	C       *CType // an extern struct's field: its C type
	// Private: its file's only (private items: [T]); its struct is made
	// elsewhere only without it (it has a default, left out).
	Private bool
}

// A Case is an enum's case.
type Case struct {
	Name   string
	Index  int
	Params []Type
	Labels []string // "" where a payload field has none
	Pos    syntax.Pos
	Owner  *TypeName
}

// An Iface is an interface's methods and fields.
type Iface struct {
	Methods []*IfaceMethod
	Fields  []*Field
	// Self is self's type in its provided methods' bodies: a type
	// parameter of the interface, which each type that has it (a
	// struct, an enum) is as the body is compiled for it.
	Self *TypeParam
}

// IfaceMethod is a method an interface asks for, or provides (with a
// body: a type that has its own of the same signature uses its own).
type IfaceMethod struct {
	Name     string
	Sig      *Signature
	Provided *Function // the interface's own body, nil when it asks for one
}

// A Component is a component's, a screen's, a store's or an element's
// interface and members.
type Component struct {
	Store, Screen, Native bool
	Worker                bool             // a worker's: its vars, lets and fns, on its thread
	Element               *prelude.Element // a built-in element
	Style                 bool             // the prelude's Style pseudo-component
	Params                []*Member        // the header's
	Props                 []*Member
	Events                []*Member
	Members               map[string]*Member
	Order                 []*Member // every member, in source order
	Body                  *syntax.Block
	Ctx                   *FuncCtx // the body's
	Provides              []*syntax.ProvideDecl
	// HoldsHandlers: it declares an async handler of another component's
	// event, which is its task (lang/LOWERING.md's "Components"): its env
	// keeps the owner it was made under.
	HoldsHandlers bool
	// Variants are its variant declarations, in order: states its caller
	// styles (name { … }). Their names are a namespace of their own.
	Variants []*VariantDef
	styled   int8 // TakesStyle: 0 not known yet, 1 it does, -1 it doesn't
}

// A VariantDef is a component's variant name = condition: while the
// condition holds, the caller's name { … } (in its block, or a style's)
// is laid over the element the component shows.
type VariantDef struct {
	object
	Pub   bool // its caller in another package may style it
	Value syntax.Expr
	Decl  *syntax.VariantDecl
	Owner *TypeName
	Index int // its place among Owner's variants
}

// Variant is the variant name the component declares, nil when none.
func (comp *Component) Variant(name string) *VariantDef {
	for _, v := range comp.Variants {
		if v.name == name {
			return v
		}
	}
	return nil
}

// MemberKind is what a component's or store's member is.
type MemberKind uint8

const (
	ParamMember MemberKind = iota // a header parameter: an accessor
	PropMember                    // a prop: an accessor, or a builder for a View
	EventMember                   // an event: its handler, or none
	StateMember                   // a cell
	LetMember                     // a memo, or a constant
	UseMember                     // a provided value
	FnMember                      // a method taking the component's env
	VarMember                     // a worker's var: its own, changed by its fns
)

// A Member is a component's or a store's member.
type Member struct {
	object
	Kind    MemberKind
	Type    Type
	Default syntax.Expr // a prop's default, a state's or a let's value
	Decl    syntax.Node
	Owner   *TypeName
	Index   int // its field in the env
	Fn      *Function
	Params  []*Param // an event's
	// For a let: whether it reads anything reactive (a memo) or not (a
	// value computed once). Path: its value is a path (rfcs/0013: let
	// draft = form.draft), its own accessor of that path rather than a
	// memo: reads through it are paths too.
	Memo bool
	Path bool
	Pub  bool
	// Assigned: a prop its component assigns (open = …). It has its
	// change event (openChange): bound by the caller, an assignment
	// emits it; unbound, the value is the component's own until the
	// caller's changes.
	Assigned bool
	// For a state (rfcs/0012): Follows, its initializer reads what
	// changes, or it says what it follows (state x(a) = …), so it's a
	// follow: the initializer's value until written, and again when
	// what it follows changes. Self is its own name in its initializer,
	// its current value (nil when the initializer doesn't read it).
	Follows bool
	Self    *Var
}

// ChangeEvent is the event a prop's assignments emit: <name>Change, with
// one parameter of the prop's type; nil when its component has none.
func (m *Member) ChangeEvent() *Member {
	if m.Kind != PropMember || m.Owner == nil || m.Owner.Comp == nil {
		return nil
	}
	e := m.Owner.Comp.Members[m.name+"Change"]
	if e == nil || e.Kind != EventMember || len(e.Params) != 1 || m.Type == nil || !Identical(e.Params[0].Type, m.Type) {
		return nil
	}
	return e
}

// IsSlot reports whether the member is a prop that takes views.
func (m *Member) IsSlot() bool {
	return m.Kind == PropMember && Identical(Unopt(m.Type), View)
}

// GlobalKind is what a package-level value is.
type GlobalKind uint8

const (
	StateGlobal GlobalKind = iota // top-level state: a cell
	MemoGlobal                    // a top-level let that reads state
	ConstGlobal                   // a top-level let that doesn't, or a type's let
	VarGlobal                     // a top-level var: a value that changes, not followed (the main thread's)
)

// A Global is a package-level state or let, or a struct's or an enum's
// constant (let in its body).
type Global struct {
	object
	Pkg   *Package
	Kind  GlobalKind
	Type  Type
	Decl  syntax.Node
	Value syntax.Expr
	Owner *TypeName  // a type's constant
	Ext   *Extension // an extension's constant
	Ctx   *FuncCtx   // its initializer's
	deps  []*Global
	Pub   bool
	// A state's, as Member's (rfcs/0012): it follows what its
	// initializer reads; its own name in the initializer.
	Follows bool
	Self    *Var
}

// Deps are the globals g's initializer reads.
func (g *Global) Deps() []*Global { return g.deps }

// A PkgName is an imported package.
type PkgName struct {
	object
	Pkg *Package
}

// BuiltinID names a function the compiler provides.
type BuiltinID uint8

const (
	BPrint BuiltinID = iota + 1
	BExpect
	BSleep
	BAll
	BRace
	BMount
	BIdle
	BPush
	BPop
	BPresent
	BDismiss
	BMin
	BMax
	BAbs
	BFloat
	BInt
	BError
	BPanic
	BJSONDecode
	BJSONEncode
	BMath        // math.<name>: the builtin of the same name
	BRange       // arr.range
	BWithTimeout // withTimeout(d) { … }
	BStream      // Stream<T>(send => { … })
	BSeq         // Seq(first: x, next: f), Seq(state:next:), Seq(repeating:), Seq(from:)
	BSet         // Set(xs), Set<T>(): a set of an array's elements, an empty one
	BEnvironment // environment(): what the platform says about where the app shows (tracked)
	BLive        // live(initial, source): a tracked value a stream keeps current while it's read
	BNever       // never(): waits until the task is cancelled
	BRuntime     // a RuntimeFunc: share, haptic, onFrame, ... (Builtin.Fn)
	BAnimate     // animate(anim) { … }
	BRefNew      // Ref(): a view's handle
	BPathNew     // Path(): an empty outline
	BShader      // a shader declaration's program (shaderlang.go), compiled with the app
	BEmbedText   // embed.text("reply.md"): a file of the package, read as it compiles
	BEmbedBytes  // embed.bytes(…): likewise, its bytes
	BEmbedJSON   // embed.json<T>(…): likewise, read as a T, checked as it compiles
	BEmbedTexts  // embed.texts("posts/*.md"): the files a glob names, [path: text]
	BEmbedFiles  // embed.files(…): likewise, [path: bytes]
	BNavigate    // navigate(href), navigate(X(…)): the Router's
	BReplace     // replace(href): the Router's, replacing where it is
	BPeek        // peek(e): e, read without following it (rfcs/0012)
	BIntParse    // u8.parse(text, radix:): an integer type's from text (the Builtin's name is the type's)
	BColorRGB    // Color.rgb(red, green, blue, alpha:): a color of numbers (a server's, a picker's)
	BColorParse  // Color.parse(text): a color of its hex text (#d73a4a), or none
	BBytesFrom   // bytes(xs): an [int]'s (each modulo 256) or a [u8]'s bytes
	BKeep        // keep(x): x, which the compiler can't leave uncomputed (testing.go)
	BSnapshot    // expectSnapshot(x): x as its stored snapshot says (testing.go)
	BInline      // expectInlineSnapshot(x, "…"): likewise, the snapshot in the call
	BEprint      // eprint(values…): print's, on stderr
	BHashOf      // hashOf(x): a Hashable value's hash, as a map's keys hash
	BChar        // char(n): the char of a code point (an error when it isn't one)
)

// A Builtin is a function the compiler provides (the prelude's print,
// sleep, all, ...; std/json's decode).
type Builtin struct {
	object
	ID BuiltinID
	// For BMath: the runtime builtin's name and signature.
	RT  string
	Sig *FuncType
	// For BRuntime: the function.
	Fn *RuntimeFunc
}

// ---------------------------------------------------------------------
// Diagnostics
