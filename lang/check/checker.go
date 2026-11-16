package check

import (
	"fmt"
	"sort"
	"strings"

	"github.com/egoist/asuka/lang/prelude"
	"github.com/egoist/asuka/lang/syntax"
)

// A Checker checks a program's packages, one at a time, in dependency
// order: the prelude when it's made, then each package with the
// packages it imports already checked.
type Checker struct {
	fset     *syntax.FileSet
	info     *Info
	diags    []Diagnostic
	errLines map[string]bool

	universe *Scope
	catalog  *Catalog // the app's catalogs (tr), or nil
	prelude  *Package
	pkgs     []*Package
	std      map[string]*Package

	// While checking a package.
	pkg        *Package
	fileScopes map[syntax.Node]*Scope // a declaration's file's scope
	styles     map[string]*StyleDef

	// While checking a body.
	ctx     *FuncCtx
	scope   *Scope
	inTry   int           // inside try's operand
	inAwait int           // inside await's operand
	handled int           // inside a do block with a catch-all
	partial *partialCatch // inside a do or a catch that doesn't take every error
	warns   []Diagnostic
	// The local fns' groups, and where their fns are used (by name),
	// for finishLocalFns.
	localGroups []*LocalGroup
	localUses   []localUse
	loops       int
	curGlobal   *Global
	resolving   map[any]bool
	view        bool // checking view items

	chained map[syntax.Expr]bool // the members and calls of a ?. chain
	// The Routes around the views checked: the Router, the path the
	// Routes' paths make (routeKnown: they're constants); and the hrefs
	// made of routed screens' calls, resolved once every Router is.
	router     *ElementCall
	routePath  string
	routeKnown bool
	hrefs      []*Href
	nextTP     int
	loadOK     bool
	inDefer    int
	free       map[*TypeParam]bool
	spawns     []*FuncCtx
	errT       *TypeName // Error
	// Generic calls, for the type parameters read or written as JSON
	// (checked once every body has said which: jsonGenerics).
	genericCalls []genericCall
	laterChecks  []func() // checks for when the package's declarations are whole
	// The built-in methods' and functions' signatures that name the
	// prelude's types, read with this checker's prelude.
	sigs        map[*Method]parsedSig
	fsigs       map[*RuntimeFunc]parsedSig
	jsonTP      map[*TypeParam]bool // type parameters read or written as JSON: each call's argument must be JSON
	bindingArgs bool                // inferArgs checks each argument as a binding (a store's)
}

// NewChecker makes a checker and checks the prelude with it.
func NewChecker(fset *syntax.FileSet) *Checker {
	c := &Checker{fset: fset, info: newInfo(), errLines: map[string]bool{}, std: map[string]*Package{}, resolving: map[any]bool{}, chained: map[syntax.Expr]bool{}, sigs: map[*Method]parsedSig{}, fsigs: map[*RuntimeFunc]parsedSig{}}
	c.universe = NewScope(nil)
	c.initUniverse()
	var files []*syntax.SourceFile
	for _, src := range prelude.Sources() {
		f, errs := syntax.ParseFile(fset, src.Name, src.Text)
		for _, e := range errs {
			c.diags = append(c.diags, Diagnostic{Pos: e.Pos, End: e.End, Msg: e.Msg})
		}
		files = append(files, f)
	}
	c.prelude = &Package{Path: "prelude", Name: "prelude", Prelude: true}
	c.prelude.Scope = NewScope(c.universe)
	c.checkFiles(c.prelude, files, nil)
	return c
}

// Fset is the file set positions are in.
func (c *Checker) Fset() *syntax.FileSet { return c.fset }

// Std returns the standard library's package at path (std/json), one
// the compiler provides, or nil.
func (c *Checker) Std(path string) *Package {
	if p, ok := c.std[path]; ok {
		return p
	}
	p := c.makeStd(path)
	if p != nil {
		c.std[path] = p
	}
	return p
}

// CheckPackage checks a package's files. imports maps each import path
// its files use to the package (checked already).
func (c *Checker) CheckPackage(path, name string, files []*syntax.SourceFile, imports map[string]*Package) *Package {
	return c.CheckPackageIn(path, name, "", files, imports)
}

// CheckPackageIn is CheckPackage of the package in dir, where its
// assets/ are (assets.text).
func (c *Checker) CheckPackageIn(path, name, dir string, files []*syntax.SourceFile, imports map[string]*Package) *Package {
	p := &Package{Path: path, Name: name, Dir: dir, qualify: path != ""}
	p.Scope = NewScope(c.prelude.Scope)
	c.checkFiles(p, files, imports)
	return p
}

// Program is what the checker made of the packages so far.
func (c *Checker) Program(root *Package) *Program {
	c.finishLocalFns()
	c.finalize()
	c.jsonGenerics()
	c.routeHrefs()
	main := c.entry(root)
	pkgs := append([]*Package{c.prelude}, c.pkgs...)
	diags := append([]Diagnostic{}, c.diags...)
	sort.SliceStable(diags, func(i, j int) bool {
		a, b := diags[i].Pos, diags[j].Pos
		if a.Filename != b.Filename {
			return a.Filename < b.Filename
		}
		return a.Offset < b.Offset
	})
	return &Program{Fset: c.fset, Packages: pkgs, Prelude: c.prelude, Root: root, Main: main, Info: c.info, Diags: diags, Warnings: append([]Diagnostic{}, c.warns...), Catalog: c.catalog, checker: c}
}

// entry finds the root package's fn main, which makes it a program (a
// command-line tool, docs/programs.md): main takes no parameters (the
// arguments are std/os's), returns nothing (os.exit sets the exit
// code), and may be async and throw. A program has no app block and
// shows no views. Only the root package's main is an entry: an imported
// package's is a function as any other.
func (c *Checker) entry(root *Package) *Function {
	if root == nil {
		return nil
	}
	var main *Function
	for _, f := range root.Funcs {
		if f.Name() == "main" && f.Recv == nil && f.Static == nil {
			main = f
		}
	}
	if main == nil {
		return nil
	}
	d := main.Decl
	if root.App != nil {
		c.errorf(d.Name, "a package is an app or a program, not both: it has an app block and fn main (rename main to call it from the app)")
	}
	if d.Native.IsValid() || d.Extern.IsValid() {
		c.errorf(d.Name, "fn main is the program's entry: it has a body")
	}
	if len(main.TypeParams) > 0 {
		c.errorf(d.Name, "fn main is the program's entry: it has no type parameters")
	}
	if len(main.Sig.Params) > 0 {
		c.errorf(d.Params, "fn main takes no parameters: the program's arguments are os.args() (import std/os)")
	}
	if r := main.Sig.Result; r != Void && r != Never {
		c.errorf(d.Result, "fn main returns nothing: os.exit(code) ends the program with a code")
	}
	for _, t := range root.Types {
		if root.App == nil && t.Kind == ComponentType && t.Comp != nil && !t.Comp.Store {
			c.errorPos(t.Pos(), "a program shows no views: %s needs an app (an app block, without fn main)", t.Name())
		}
	}
	return main
}

// ---------------------------------------------------------------------
// Diagnostics

func (c *Checker) errorf(n syntax.Node, format string, args ...any) {
	var pos, end syntax.Position
	if n != nil {
		r := n.Range()
		pos = c.fset.Position(r.Start)
		end = c.fset.Position(r.End)
	}
	c.errorAt(pos, end, fmt.Sprintf(format, args...))
}

// warnf reports what's worth saying that doesn't stop the build.
func (c *Checker) warnf(n syntax.Node, format string, args ...any) {
	r := n.Range()
	pos, end := c.fset.Position(r.Start), c.fset.Position(r.End)
	msg := fmt.Sprintf(format, args...)
	key := "warning " + pos.String() + msg
	if c.errLines[key] {
		return
	}
	c.errLines[key] = true
	c.warns = append(c.warns, Diagnostic{Pos: pos, End: end, Msg: msg})
}

func (c *Checker) errorPos(p syntax.Pos, format string, args ...any) {
	pos := c.fset.Position(p)
	c.errorAt(pos, pos, fmt.Sprintf(format, args...))
}

func (c *Checker) errorAt(pos, end syntax.Position, msg string) {
	// One error per position: a mistake reported once.
	key := pos.String() + msg
	if c.errLines[key] {
		return
	}
	c.errLines[key] = true
	c.diags = append(c.diags, Diagnostic{Pos: pos, End: end, Msg: msg})
}

// ---------------------------------------------------------------------
// The universe: the types and functions built into the compiler

func (c *Checker) initUniverse() {
	for _, t := range []*Basic{Int, Float, Bool, String, Bytes, Void, Duration, Color, Length, View, Graphics, Path, Ref, Pointer, TestApp} {
		c.universe.Insert(&TypeName{object: object{name: t.name}, Kind: BasicType, Type: t, resolved: true})
	}
	// Error: the interface every error has.
	c.errT = &TypeName{object: object{name: "Error"}, Kind: InterfaceType, isErrorIface: true, resolved: true}
	c.errT.Iface = &Iface{Fields: []*Field{{Name: "message", Type: String}}}
	c.errT.Type = &Named{Obj: c.errT}
	c.universe.Insert(c.errT)
	loadingT.Cases[2].Params[0] = c.errT.Type
	c.universe.Insert(loadingT)
	for _, b := range []struct {
		name string
		id   BuiltinID
	}{
		{"print", BPrint}, {"expect", BExpect}, {"sleep", BSleep}, {"all", BAll}, {"race", BRace},
		{"mount", BMount}, {"idle", BIdle}, {"push", BPush}, {"pop", BPop}, {"present", BPresent},
		{"dismiss", BDismiss}, {"min", BMin}, {"max", BMax}, {"abs", BAbs}, {"float", BFloat},
		{"int", BInt}, {"panic", BPanic}, {"withTimeout", BWithTimeout}, {"Stream", BStream}, {"Set", BSet},
		{"environment", BEnvironment}, {"live", BLive}, {"animate", BAnimate}, {"shader", BShader},
		{"never", BNever}, {"navigate", BNavigate}, {"replace", BReplace},
	} {
		c.universe.Insert(&Builtin{object: object{name: b.name}, ID: b.id})
	}
	for _, f := range RuntimeFuncs {
		c.universe.Insert(&Builtin{object: object{name: f.Name}, ID: BRuntime, Fn: f})
	}
	// router: the router's builtins, a package of the universe.
	rp := &Package{Path: "router", Name: "router"}
	rp.Scope = NewScope(nil)
	for _, f := range RouterFuncs {
		rp.Scope.Insert(&Builtin{object: object{name: f.Name}, ID: BRuntime, Fn: f})
	}
	c.universe.Insert(&PkgName{object: object{name: "router"}, Pkg: rp})
}

// ErrorType is the Error interface.
func (c *Checker) ErrorType() Type { return c.errT.Type }

// ---------------------------------------------------------------------
// Packages

func (c *Checker) checkFiles(p *Package, files []*syntax.SourceFile, imports map[string]*Package) {
	saved := c.pkg
	c.pkg = p
	c.fileScopes = map[syntax.Node]*Scope{}
	c.styles = map[string]*StyleDef{}
	p.Files = files
	if !p.Prelude {
		c.pkgs = append(c.pkgs, p)
	}

	// Collect every declaration's name first: a package's files see
	// each other's declarations, in any order.
	for _, f := range files {
		fs := NewScope(p.Scope)
		if p.fileScopes == nil {
			p.fileScopes = map[*syntax.SourceFile]*Scope{}
		}
		p.fileScopes[f] = fs
		for _, imp := range f.Imports {
			c.importDecl(fs, imp, imports)
		}
		for _, d := range f.Decls {
			c.fileScopes[d] = fs
			c.collect(p, d)
		}
	}
	// Then the types' insides, the functions' signatures, and the bodies.
	for _, t := range p.Types {
		c.resolveType(t)
	}
	for _, t := range p.Types {
		c.typeMembers(t)
	}
	for _, f := range p.Funcs {
		c.funcSig(f)
	}
	for _, g := range p.Globals {
		c.globalType(g)
	}
	c.checkStyles(p)
	for _, t := range p.Types {
		c.typeBodies(t)
	}
	for _, f := range p.Funcs {
		c.funcBody(f)
	}
	for _, t := range p.Tests {
		c.testBody(t)
	}
	if p.App != nil {
		c.appBody(p)
	}
	for len(c.laterChecks) > 0 {
		f := c.laterChecks[0]
		c.laterChecks = c.laterChecks[1:]
		f()
	}
	c.pkg = saved
}

func (c *Checker) importDecl(fs *Scope, imp *syntax.ImportDecl, imports map[string]*Package) {
	path := imp.PathString()
	ip, found := imports[path]
	if found && ip == nil {
		return // the compiler couldn't load it, and said so
	}
	if ip == nil {
		ip = c.Std(path)
	}
	if ip == nil && path == "std/assets" {
		c.errorf(imp, "std/assets is std/embed now: embed.text(\"assets/reply.md\"), its paths relative to the package's directory")
		return
	}
	if ip == nil {
		c.errorf(imp, "no package %s: %s", path, importHint(imp))
		return
	}
	c.info.Imports[imp] = ip
	if !contains(c.pkg.Imports, ip) {
		c.pkg.Imports = append(c.pkg.Imports, ip)
	}
	if imp.Names != nil {
		for _, n := range imp.Names {
			o := ip.Scope.LookupLocal(n.Name)
			if o == nil {
				c.errorf(n, "package %s has no %s", path, n.Name)
				continue
			}
			if !c.exported(ip, o) {
				c.errorf(n, "%s isn't pub in package %s: it's private to it", n.Name, path)
			}
			c.info.Defs[n] = o
			if old := fs.Insert(o); old != nil && old != o {
				c.errorf(n, "%s is imported twice", n.Name)
			}
		}
		if imp.Alias == nil {
			return
		}
	}
	name := ip.Name
	if imp.Alias != nil {
		name = imp.Alias.Name
	} else if !syntax.IsIdentifier(name) {
		c.errorf(imp, "import %s as a name: %s isn't one", path, name)
		return
	}
	pn := &PkgName{object: object{name, imp.Start}, Pkg: ip}
	if old := fs.Insert(pn); old != nil {
		c.errorf(imp, "%s is imported twice (import it as another name: import %s as other)", name, path)
	}
}

func importHint(imp *syntax.ImportDecl) string {
	if imp.Relative {
		return "a package beside this one is a directory of .asuka files"
	}
	return "the standard library is std/<name> and the platform SDK platform/<name> (docs/stdlib.md); a dependency is added with asuka add"
}

func contains[T comparable](s []T, x T) bool {
	for _, y := range s {
		if y == x {
			return true
		}
	}
	return false
}

// exported reports whether o, declared in p, may be used from another
// package.
func (c *Checker) exported(p *Package, o Object) bool {
	if p.Prelude || p.Path == "" || StdPath(p.Path) && p.Dir == "" {
		return true
	}
	switch x := o.(type) {
	case *TypeName:
		return pubOf(x.Decl)
	case *Function:
		return x.Decl != nil && x.Decl.Pub.IsValid()
	case *Global:
		return x.Pub
	case *Builtin:
		return true
	}
	return true
}

func pubOf(n syntax.Node) bool {
	switch d := n.(type) {
	case *syntax.StructDecl:
		return d.Pub.IsValid()
	case *syntax.EnumDecl:
		return d.Pub.IsValid()
	case *syntax.InterfaceDecl:
		return d.Pub.IsValid()
	case *syntax.ComponentDecl:
		return d.Pub.IsValid()
	case *syntax.ScreenDecl:
		return d.Pub.IsValid()
	case *syntax.StoreDecl:
		return d.Pub.IsValid()
	case *syntax.TypeAliasDecl:
		return d.Pub.IsValid()
	case *syntax.ExternTypeDecl:
		return d.Pub.IsValid()
	}
	return true
}

// collect declares d's name in the package.
func (c *Checker) collect(p *Package, d syntax.Decl) {
	declare := func(id *syntax.Ident, o Object) {
		c.info.Defs[id] = o
		if old := p.Scope.Insert(o); old != nil {
			c.errorf(id, "%s is declared twice in package %s (the other is at %s)", id.Name, pkgLabel(p), c.fset.Position(old.Pos()))
		}
	}
	switch d := d.(type) {
	case *syntax.StructDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: StructType, Decl: d, Extern: d.Extern.IsValid()}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.ExternTypeDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: ExternType, Decl: d}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.EnumDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: EnumType, Decl: d}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.InterfaceDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: InterfaceType, Decl: d, Native: d.Native.IsValid()}
		if t.Native {
			for _, o := range p.Types {
				if o.Native {
					t.NativeIndex++
				}
			}
		}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.TypeAliasDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: AliasType, Decl: d}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.ComponentDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: ComponentType, Decl: d}
		t.Comp = &Component{Native: d.Native.IsValid(), Body: d.Body}
		if p.Prelude {
			t.Comp.Element = prelude.FindElement(d.Name.Name)
			t.Comp.Style = d.Name.Name == "Style"
		}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.ScreenDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: ComponentType, Decl: d}
		t.Comp = &Component{Screen: true, Body: d.Body}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.StoreDecl:
		t := &TypeName{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: StoreType, Decl: d}
		t.Comp = &Component{Store: true, Body: d.Body}
		p.Types = append(p.Types, t)
		declare(d.Name, t)
	case *syntax.FuncDecl:
		f := &Function{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Decl: d}
		p.Funcs = append(p.Funcs, f)
		declare(d.Name, f)
	case *syntax.StateDecl:
		g := &Global{object: object{d.Name.Name, d.Name.Start}, Pkg: p, Kind: StateGlobal, Decl: d, Value: d.Value, Pub: d.Pub.IsValid()}
		p.Globals = append(p.Globals, g)
		declare(d.Name, g)
	case *syntax.LetDecl:
		c.noLetElse(d)
		bp, ok := d.Pattern.(*syntax.BindPat)
		if !ok {
			c.errorf(d.Pattern, "a let at the top of a file names one value: let name = …")
			return
		}
		if d.Keyword == syntax.VAR {
			c.errorf(d, "a file's values don't change: a value that changes is state (state %s = …)", bp.Name.Name)
		}
		g := &Global{object: object{bp.Name.Name, bp.Name.Start}, Pkg: p, Kind: ConstGlobal, Decl: d, Value: d.Value, Pub: d.Pub.IsValid()}
		p.Globals = append(p.Globals, g)
		declare(bp.Name, g)
	case *syntax.StyleDecl:
		c.collectStyle(p, d)
	case *syntax.AppDecl:
		if p.App != nil {
			c.errorf(d, "a package has one app")
		}
		p.App = d
	case *syntax.TestDecl:
		p.Tests = append(p.Tests, &Test{Name: unquoteSimple(d.Name), Decl: d, Pkg: p})
	}
}

func pkgLabel(p *Package) string {
	if p.Path == "" {
		return "main"
	}
	return p.Path
}

// unquoteSimple is a string literal's text, without interpolations.
func unquoteSimple(s *syntax.StringLit) string {
	if s == nil {
		return ""
	}
	t, _ := Unescape(strings.Join(s.Segments, ""), s.Raw)
	return t
}

// ---------------------------------------------------------------------
// Scopes

func (c *Checker) declScope(d syntax.Node) *Scope {
	if s := c.fileScopes[d]; s != nil {
		return s
	}
	return c.pkg.Scope
}

func (c *Checker) openScope() { c.scope = NewScope(c.scope) }
func (c *Checker) closeScope() {
	if c.scope != nil {
		c.scope = c.scope.parent
	}
}

func (c *Checker) declareVar(id *syntax.Ident, t Type, mutable bool) *Var {
	v := &Var{object: object{id.Name, id.Start}, Type: t, Mutable: mutable, Ctx: c.ctx}
	c.info.Defs[id] = v
	if id.Name == "_" {
		return v
	}
	if old := c.scope.LookupLocal(id.Name); old != nil {
		c.errorf(id, "%s is declared twice here: give one another name", id.Name)
	}
	c.scope.Insert(v)
	return v
}

// lookup finds a name from the current scope out.
func (c *Checker) lookup(name string) Object {
	if c.scope != nil {
		return c.scope.Lookup(name)
	}
	return c.pkg.Scope.Lookup(name)
}

// lookupType finds the type named name in scope (a value of the name
// doesn't hide it).
func (c *Checker) lookupType(name string) *TypeName {
	if c.scope != nil {
		return c.scope.LookupType(name)
	}
	return c.pkg.Scope.LookupType(name)
}

// enter starts checking a function body.
func (c *Checker) enter(kind CtxKind, node syntax.Node, name string) *FuncCtx {
	ctx := &FuncCtx{Kind: kind, Parent: c.ctx, Node: node, Name: name}
	if c.ctx != nil {
		ctx.Comp = c.ctx.Comp
		ctx.Self = c.ctx.Self
	}
	if node != nil {
		c.info.Ctxs[node] = ctx
	}
	c.ctx = ctx
	c.openScope()
	ctx.scope = c.scope
	return ctx
}

func (c *Checker) leave(saved *FuncCtx) {
	c.closeScope()
	if c.ctx != nil && c.ctx.Parent != nil && c.ctx.readsState {
		// A closure's reads are its maker's when it runs (bindings,
		// handlers): conservatively, the maker reads state too.
		c.ctx.Parent.readsState = true
	}
	c.ctx = saved
}

// ---------------------------------------------------------------------
// Type expressions

func (c *Checker) typeExpr(t syntax.Type) Type {
	switch t := t.(type) {
	case nil:
		return Invalid
	case *syntax.BadType:
		return Invalid
	case *syntax.ArrayType:
		return &Array{c.typeExpr(t.Elem)}
	case *syntax.MapType:
		k := c.typeExpr(t.Key)
		if !IsInvalid(k) && !IsHashable(k) {
			c.errorf(t.Key, "a map's keys are ints, strings, bools or enums without payloads, not %s", typeString(k))
		}
		return &Map{k, c.typeExpr(t.Value)}
	case *syntax.OptionalType:
		e := c.typeExpr(t.Elem)
		if e == Void {
			c.errorf(t, "void? isn't a type: use bool")
			return Invalid
		}
		return MakeOptional(e)
	case *syntax.TupleType:
		if len(t.Elems) == 0 {
			return Void
		}
		if len(t.Elems) == 1 {
			return c.typeExpr(t.Elems[0])
		}
		var es []Type
		for _, e := range t.Elems {
			es = append(es, c.typeExpr(e))
		}
		return &Tuple{es}
	case *syntax.FuncType:
		ft := &FuncType{Async: t.Async.IsValid(), Throws: t.Throws.IsValid(), Result: Void}
		for _, p := range t.Params {
			ft.Params = append(ft.Params, c.typeExpr(p))
		}
		if t.Result != nil {
			ft.Result = c.typeExpr(t.Result)
		}
		return ft
	case *syntax.NamedType:
		return c.namedType(t)
	}
	return Invalid
}

func (c *Checker) namedType(t *syntax.NamedType) Type {
	var o Object
	name := t.Path[len(t.Path)-1].Name
	if len(t.Path) == 1 {
		switch name {
		case "Stream":
			if o, _ := c.lookup(name).(*Builtin); o != nil && o.ID == BStream {
				if len(t.Args) != 1 {
					c.errorf(t, "Stream takes one type: Stream<T>")
					return Invalid
				}
				return &Stream{c.typeExpr(t.Args[0])}
			}
		case "Set":
			if o, _ := c.lookup(name).(*Builtin); o != nil && o.ID == BSet {
				if len(t.Args) != 1 {
					c.errorf(t, "Set takes one type: Set<T>")
					return Invalid
				}
				return c.setOf(c.typeExpr(t.Args[0]), t.Args[0])
			}
		case "never":
			// What a function that never returns gives (os.exit): the
			// builtin never()'s name, as a type.
			if o, _ := c.lookup(name).(*Builtin); o != nil && o.ID == BNever && len(t.Args) == 0 {
				return Never
			}
		case "Task":
			if c.lookup(name) == nil {
				if len(t.Args) != 1 {
					c.errorf(t, "Task takes one type: Task<T>")
					return Invalid
				}
				return &Task{c.typeExpr(t.Args[0])}
			}
		}
		if tn := c.lookupType(name); tn != nil {
			o = tn
		} else {
			o = c.lookup(name)
		}
		if o == nil {
			if IsCTypeName(name) {
				c.errorf(t, "%s is a C type: it's written in extern declarations, and Asuka sees it as %s", name, map[bool]string{true: "a string", false: map[bool]string{true: "a float", false: "an int"}[name == "Float32"]}[name == "CString"])
				return Invalid
			}
			c.errorf(t, "unknown type %s%s", name, c.suggest(name))
			return Invalid
		}
		c.info.Uses[t.Path[0]] = o
	} else {
		pn, ok := c.lookup(t.Path[0].Name).(*PkgName)
		if !ok {
			// A type's nested name isn't a thing: Foo.Bar.
			c.errorf(t, "unknown type %s (%s isn't an imported package)", joinPath(t.Path), t.Path[0].Name)
			return Invalid
		}
		c.info.Uses[t.Path[0]] = pn
		o = pn.Pkg.Scope.LookupLocal(name)
		if o == nil {
			c.errorf(t, "package %s has no type %s", pn.Pkg.Path, name)
			return Invalid
		}
		if len(t.Path) == 2 {
			c.info.Uses[t.Path[1]] = o
		}
		if !c.exported(pn.Pkg, o) {
			c.errorf(t, "%s isn't pub in package %s", name, pn.Pkg.Path)
		}
	}
	tn, ok := o.(*TypeName)
	if !ok {
		c.errorf(t, "%s isn't a type", name)
		return Invalid
	}
	switch tn.Kind {
	case ComponentType:
		// A component (or the element View) as a type: what it makes.
		return View
	case BasicType, ParamType:
		if len(t.Args) > 0 {
			c.errorf(t, "%s takes no type arguments", name)
		}
		return tn.Type
	case AliasType:
		c.resolveType(tn)
		if len(tn.TypeParams) != len(t.Args) {
			c.errorf(t, "%s takes %s", name, plural(len(tn.TypeParams), "type argument", "type arguments"))
			return Invalid
		}
		s := Subst{}
		for i, a := range t.Args {
			s[tn.TypeParams[i]] = c.typeExpr(a)
		}
		if tn.Type == nil {
			return Invalid
		}
		return s.Apply(tn.Type)
	}
	c.resolveType(tn)
	if len(tn.TypeParams) != len(t.Args) {
		if len(t.Args) == 0 {
			c.errorf(t, "%s is generic: give its types, %s<…>", name, name)
		} else {
			c.errorf(t, "%s takes %s", name, plural(len(tn.TypeParams), "type argument", "type arguments"))
		}
		return Invalid
	}
	n := &Named{Obj: tn}
	for _, a := range t.Args {
		n.Args = append(n.Args, c.typeExpr(a))
	}
	for i, tp := range tn.TypeParams {
		// Once the package's types are whole: a declaration's types are
		// read before the interfaces they name may be.
		at, arg := t.Args[i], n.Args[i]
		c.laterChecks = append(c.laterChecks, func() { c.checkConstraints(at, name, tp, arg) })
	}
	return n
}

func joinPath(p []*syntax.Ident) string {
	var s []string
	for _, x := range p {
		s = append(s, x.Name)
	}
	return strings.Join(s, ".")
}

// suggest names a close match for a misspelled name: " (did you mean
// count?)".
func (c *Checker) suggest(name string) string {
	best, bestD := "", 3
	seen := map[string]bool{}
	// A nearer scope's name wins a tie; in one scope, the first in
	// order does (its names are a map's: the same suggestion each run).
	try := func(s *Scope) {
		bestHere := ""
		for n := range s.names {
			if seen[n] {
				continue
			}
			seen[n] = true
			if d := editDistance(name, n); d < len(name) && (d < bestD || d == bestD && bestHere != "" && n < bestHere) {
				best, bestD, bestHere = n, d, n
			}
		}
	}
	for s := c.scope; s != nil; s = s.parent {
		try(s)
	}
	if c.scope == nil {
		for s := c.pkg.Scope; s != nil; s = s.parent {
			try(s)
		}
	}
	if best != "" {
		return " (did you mean " + best + "?)"
	}
	return ""
}

func editDistance(a, b string) int {
	if a == b {
		return 0
	}
	if len(a) > 32 || len(b) > 32 {
		return 99
	}
	prev := make([]int, len(b)+1)
	cur := make([]int, len(b)+1)
	for j := range prev {
		prev[j] = j
	}
	for i := 1; i <= len(a); i++ {
		cur[0] = i
		for j := 1; j <= len(b); j++ {
			cost := 1
			if strings.EqualFold(a[i-1:i], b[j-1:j]) {
				cost = 0
			}
			cur[j] = min(prev[j]+1, cur[j-1]+1, prev[j-1]+cost)
		}
		prev, cur = cur, prev
	}
	return prev[len(b)]
}

// typeParams declares a declaration's type parameters in the current
// scope.
func (c *Checker) typeParams(list []*syntax.TypeParam) []*TypeParam {
	var out []*TypeParam
	for i, tp := range list {
		p := &TypeParam{Name: tp.Name.Name, Index: i, id: c.nextTP}
		c.nextTP++
		tn := &TypeName{object: object{p.Name, tp.Name.Start}, Kind: ParamType, Type: p, resolved: true}
		c.info.Defs[tp.Name] = tn
		c.scope.Insert(tn)
		out = append(out, p)
	}
	// The constraints once every parameter is declared: one may name
	// another (T: Container<U>).
	for i, tp := range list {
		c.constraints(out[i], tp.Constraints)
	}
	return out
}

// constraints reads a type parameter's constraints: the built-in ones
// (Equatable, Comparable, Hashable) and the program's interfaces.
func (c *Checker) constraints(p *TypeParam, list []syntax.Type) {
	var written []string
	for _, ct := range list {
		if nt, ok := ct.(*syntax.NamedType); ok && len(nt.Path) == 1 && len(nt.Args) == 0 {
			switch n := nt.Path[0].Name; n {
			case "Equatable", "Comparable", "Hashable":
				if !p.Has(n) {
					p.Builtins = append(p.Builtins, n)
				}
				written = append(written, n)
				continue
			}
		}
		t := c.typeExpr(ct)
		n, ok := t.(*Named)
		switch {
		case IsInvalid(t):
			continue
		case !ok || n.Obj.Kind != InterfaceType:
			c.errorf(ct, "a type parameter's constraint is an interface, Equatable, Comparable or Hashable, not %s", typeString(t))
			continue
		case n.Obj.Native:
			c.errorf(ct, "%s is a native interface: its platforms' code implements it, and no type of the program has it", n.Obj.name)
			continue
		}
		p.Ifaces = append(p.Ifaces, n)
		written = append(written, typeString(n))
	}
	p.written = strings.Join(written, " & ")
}
