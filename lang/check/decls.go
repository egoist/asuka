package check

import (
	"strings"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// typeScope is the scope a type's insides are checked in: its file's,
// with its type parameters.
func (c *Checker) typeScope(t *TypeName) *Scope {
	s := NewScope(c.declScope(t.Decl))
	for _, p := range t.TypeParams {
		s.Insert(p.object())
	}
	if d, ok := t.Decl.(*syntax.ComponentDecl); ok && len(t.TypeParams) == 0 {
		// A generic component (reported): its type parameters name no
		// type, rather than being reported unknown at each use.
		for _, p := range d.TypeParams {
			s.Insert(&TypeName{object: object{p.Name.Name, p.Name.Start}, Kind: AliasType, Type: Invalid, resolved: true})
		}
	}
	return s
}

// object is the type name p is declared as: its declaration's (a
// built-in one's made here, without a position).
func (p *TypeParam) object() *TypeName {
	if p.obj == nil {
		p.obj = &TypeName{object: object{p.Name, syntax.NoPos}, Kind: ParamType, Type: p, resolved: true}
	}
	return p.obj
}

// resolveType sets a declared type's parameters and, for an alias, what
// it stands for.
func (c *Checker) resolveType(t *TypeName) {
	if t.resolved {
		if c.resolving[t] {
			// An alias that names itself on the way to what it stands
			// for (type L = [L]): its definition says so (below).
			c.resolving[aliasCycle{t}] = true
		}
		return
	}
	t.resolved = true
	savedScope := c.scope
	c.scope = NewScope(c.declScope(t.Decl))
	defer func() { c.scope = savedScope }()
	var tps []*syntax.TypeParam
	switch d := t.Decl.(type) {
	case *syntax.StructDecl:
		tps = d.TypeParams
	case *syntax.EnumDecl:
		tps = d.TypeParams
	case *syntax.InterfaceDecl:
		tps = d.TypeParams
	case *syntax.TypeAliasDecl:
		tps = d.TypeParams
	case *syntax.StoreDecl:
		tps = d.TypeParams
	case *syntax.ComponentDecl:
		tps = d.TypeParams
		if len(tps) > 0 {
			c.errorf(d.Name, diag.NotSupported, "generic components aren't supported yet: take a View or a closure instead")
			tps = nil
		}
	}
	t.TypeParams = c.typeParams(tps)
	n := &Named{Obj: t}
	for _, p := range t.TypeParams {
		n.Args = append(n.Args, p)
	}
	t.Type = n
	if a, ok := t.Decl.(*syntax.TypeAliasDecl); ok {
		// An alias is another name for a type, so it can't be one of
		// the types it stands for: a name met on the way is invalid.
		t.Type = Invalid
		c.resolving[t] = true
		at := c.typeExpr(a.Type)
		delete(c.resolving, t)
		if c.resolving[aliasCycle{t}] {
			delete(c.resolving, aliasCycle{t})
			c.errorf(a.Name, diag.RecursiveType, "type %s is defined in terms of itself: an alias can't hold itself; a struct or an enum can (struct Node { next: Node? })", t.name)
			return
		}
		t.Type = at
	}
}

// aliasCycle marks, in Checker.resolving, an alias met while its own
// definition is read.
type aliasCycle struct{ t *TypeName }

// typeMembers declares a type's fields, cases, methods, constants and,
// for components and stores, members.
func (c *Checker) typeMembers(t *TypeName) {
	savedScope, savedCtx := c.scope, c.ctx
	c.scope = c.typeScope(t)
	c.ctx = nil
	defer func() { c.scope, c.ctx = savedScope, savedCtx }()
	t.Methods = map[string]*Function{}
	t.Consts = map[string]*Global{}
	switch d := t.Decl.(type) {
	case *syntax.StructDecl:
		t.fieldIndex = map[string]int{}
		if t.Extern {
			c.externStructDecl(t, d)
		} else {
			c.conforms(t, d.Conforms)
		}
		for _, m := range d.Members {
			switch m := m.(type) {
			case *syntax.Field:
				if _, dup := t.fieldIndex[m.Name.Name]; dup {
					c.errorf(m.Name, diag.DuplicateDeclaration, "field %s is declared twice", m.Name.Name)
					continue
				}
				var ft Type
				var fc *CType
				if t.Extern {
					ft, fc = c.cType(m.Type, cField, "field "+m.Name.Name+" of extern struct "+t.name)
				} else {
					ft = c.typeExpr(m.Type)
				}
				f := &Field{Name: m.Name.Name, Type: ft, Default: m.Default, Index: len(t.Fields), Pos: m.Name.Start, C: fc}
				t.fieldIndex[f.Name] = f.Index
				t.Fields = append(t.Fields, f)
			case *syntax.LetDecl:
				c.noLetElse(m)
				c.typeConst(t, m)
			case *syntax.FuncDecl:
				c.method(t, m)
			}
		}
	case *syntax.EnumDecl:
		t.caseIndex = map[string]int{}
		c.conforms(t, d.Conforms)
		for _, cs := range d.Cases {
			if _, dup := t.caseIndex[cs.Name.Name]; dup {
				c.errorf(cs.Name, diag.DuplicateDeclaration, "case %s is declared twice", cs.Name.Name)
				continue
			}
			k := &Case{Name: cs.Name.Name, Index: len(t.Cases), Pos: cs.Name.Start, Owner: t}
			if cs.Params != nil {
				for _, p := range cs.Params.List {
					k.Params = append(k.Params, c.typeExpr(p.Type))
					label := ""
					if p.Label != nil {
						label = p.Label.Name
					}
					k.Labels = append(k.Labels, label)
				}
			}
			t.caseIndex[k.Name] = k.Index
			t.Cases = append(t.Cases, k)
		}
		for _, m := range d.Members {
			switch m := m.(type) {
			case *syntax.LetDecl:
				c.noLetElse(m)
				c.typeConst(t, m)
			case *syntax.FuncDecl:
				c.method(t, m)
			}
		}
	case *syntax.InterfaceDecl:
		t.Iface = &Iface{}
		if self, ok := t.Type.(*Named); ok {
			t.Iface.Self = &TypeParam{Name: t.name, Ifaces: []*Named{self}, written: t.name, id: c.nextTP}
			c.nextTP++
		}
		if t.Native && len(d.TypeParams) > 0 {
			c.errorf(d.Name, diag.InvalidModifier, "a native interface isn't generic: its platforms' code can't be")
		}
		for _, m := range d.Members {
			switch m := m.(type) {
			case *syntax.Field:
				if t.Native {
					c.errorf(m.Name, diag.MisplacedDeclaration, "a native interface has methods only")
				}
				t.Iface.Fields = append(t.Iface.Fields, &Field{Name: m.Name.Name, Type: c.typeExpr(m.Type), Pos: m.Name.Start})
			case *syntax.FuncDecl:
				if c.ifaceMethodDeclared(t, m.Name) {
					continue
				}
				// A method called through the interface can't say its
				// types: its type parameters are declared only so that
				// what names them isn't unknown too.
				var tps []*TypeParam
				saved := c.scope
				if len(m.TypeParams) > 0 {
					c.errorf(m.Name, diag.InvalidModifier, "an interface's methods aren't generic: a call through the interface can't say their types")
					c.scope = NewScope(c.scope)
					tps = c.typeParams(m.TypeParams)
					for _, p := range tps {
						c.scope.Insert(p.object())
					}
				}
				sig := c.signature(m, nil)
				c.scope = saved
				im := &IfaceMethod{Name: m.Name.Name, Sig: sig}
				switch {
				case t.Native && (m.Body != nil || m.Value != nil):
					c.errorf(m.Name, diag.FnBody, "a native interface's methods have no bodies: its platforms' code implements them")
				case m.Body != nil || m.Value != nil:
					// Provided: a method of the interface's own, checked
					// with self as the interface.
					f := &Function{object: object{m.Name.Name, m.Name.Start}, Pkg: t.Pkg, Decl: m, Recv: t, Sig: sig, TypeParams: tps}
					c.info.Defs[m.Name] = f
					t.Methods[f.name] = f
					im.Provided = f
				}
				if t.Native {
					// The platform's side may always fail.
					sig.Throws = true
				}
				t.Iface.Methods = append(t.Iface.Methods, im)
			}
		}
	case *syntax.ComponentDecl:
		c.componentMembers(t, d.Params, d.Body)
	case *syntax.ScreenDecl:
		c.componentMembers(t, d.Params, d.Body)
	case *syntax.StoreDecl:
		c.componentMembers(t, d.Params, d.Body)
	case *syntax.WorkerDecl:
		c.componentMembers(t, d.Params, d.Body)
	}
}

// ifaceMethodDeclared reports (and says) a method or a field of
// interface t already named name.
func (c *Checker) ifaceMethodDeclared(t *TypeName, name *syntax.Ident) bool {
	for _, m := range t.Iface.Methods {
		if m.Name == name.Name {
			c.errorf(name, diag.DuplicateDeclaration, "method %s is declared twice in %s", name.Name, t.name)
			return true
		}
	}
	for _, f := range t.Iface.Fields {
		if f.Name == name.Name {
			c.errorf(name, diag.DuplicateDeclaration, "%s is %s's field and a method", name.Name, t.name)
			return true
		}
	}
	return false
}

func (c *Checker) conforms(t *TypeName, list []syntax.Type) {
	for _, ct := range list {
		nt, ok := ct.(*syntax.NamedType)
		if ok && len(nt.Path) == 1 {
			switch nt.Path[0].Name {
			case "Equatable", "Comparable", "Hashable":
				// Structural: a type has them when its fields do.
				continue
			case "Error":
				t.IsError = true
				continue
			}
		}
		ty := c.typeExpr(ct)
		if n, ok := ty.(*Named); ok && n.Obj.Kind == InterfaceType {
			if n.Obj.Native {
				c.errorf(ct, diag.InvalidConformance, "%s is a native interface: its platforms' code implements it (https://asuka.egoist.dev/docs/native)", n.Obj.name)
				continue
			}
			t.Conforms = append(t.Conforms, n.Obj)
			continue
		}
		if !IsInvalid(ty) {
			c.errorf(ct, diag.InvalidConformance, "%s isn't an interface: a type declares Error, Equatable, Comparable, Hashable or an interface", typeString(ty))
		}
	}
}

func (c *Checker) typeConst(t *TypeName, d *syntax.LetDecl) {
	bp, ok := d.Pattern.(*syntax.BindPat)
	if !ok {
		c.errorf(d.Pattern, diag.InvalidDeclaration, "a type's constant is let name = value")
		return
	}
	if d.Keyword == syntax.VAR {
		c.errorf(d, diag.InvalidDeclaration, "a type's constants don't change: let, not var")
	}
	if _, dup := t.Consts[bp.Name.Name]; dup {
		c.errorf(bp.Name, diag.DuplicateDeclaration, "%s is declared twice in %s", bp.Name.Name, t.name)
		return
	}
	c.inTypeFile(t, d)
	g := &Global{object: object{bp.Name.Name, bp.Name.Start}, Pkg: t.Pkg, Kind: ConstGlobal, Decl: d, Value: d.Value, Owner: t, Pub: true}
	c.info.Defs[bp.Name] = g
	t.Consts[g.name] = g
	t.Pkg.Globals = append(t.Pkg.Globals, g)
}

func (c *Checker) method(t *TypeName, d *syntax.FuncDecl) {
	if d.Static.IsValid() {
		c.staticFunc(t, d)
		return
	}
	if _, dup := t.Methods[d.Name.Name]; dup {
		c.errorf(d.Name, diag.DuplicateDeclaration, "method %s is declared twice in %s", d.Name.Name, t.name)
		return
	}
	f := &Function{object: object{d.Name.Name, d.Name.Start}, Pkg: t.Pkg, Decl: d, Recv: t}
	c.info.Defs[d.Name] = f
	t.Methods[f.name] = f
}

// inTypeFile checks d, a member of t's checked as the package's own (a
// static fn, a constant), in t's file's scope: with its imports.
func (c *Checker) inTypeFile(t *TypeName, d syntax.Node) {
	if s := c.fileScopes[t.Decl]; s != nil && c.fileScopes[d] == nil {
		c.fileScopes[d] = s
	}
}

// staticFunc declares a static fn of t's: a function in t's namespace
// (Money.of(dollars: 2), .of(…) where a Money is expected), checked and
// lowered as any of the package's functions, with no self.
func (c *Checker) staticFunc(t *TypeName, d *syntax.FuncDecl) {
	if len(t.TypeParams) > 0 {
		c.errorf(d.Name, diag.MisplacedDeclaration, "%s is generic: its static functions would need its type parameters; declare a function outside it", t.name)
		return
	}
	if t.Statics == nil {
		t.Statics = map[string]*Function{}
	}
	if _, dup := t.Statics[d.Name.Name]; dup || t.Methods[d.Name.Name] != nil || t.Consts[d.Name.Name] != nil {
		c.errorf(d.Name, diag.DuplicateDeclaration, "%s is declared twice in %s", d.Name.Name, t.name)
		return
	}
	c.inTypeFile(t, d)
	f := &Function{object: object{d.Name.Name, d.Name.Start}, Pkg: t.Pkg, Decl: d, Static: t}
	c.info.Defs[d.Name] = f
	t.Statics[f.name] = f
	t.Pkg.Funcs = append(t.Pkg.Funcs, f)
}

// signature resolves a function's parameters and result in the current
// scope (with the function's type parameters declared).
func (c *Checker) signature(d *syntax.FuncDecl, f *Function) *Signature {
	s := &Signature{Async: d.Async.IsValid(), Throws: d.Throws.IsValid(), Result: Void, Mut: d.Mut.IsValid()}
	if s.Mut && s.Async {
		c.errorf(d.Name, diag.InvalidModifier, "a mut fn can't be async: change a store's state from an async fn instead")
	}
	if d.Params != nil {
		seen := map[string]bool{}
		for _, p := range d.Params.List {
			if seen[p.Name.Name] {
				c.errorf(p.Name, diag.DuplicateDeclaration, "parameter %s is declared twice", p.Name.Name)
			}
			seen[p.Name.Name] = true
			if p.Type == nil {
				c.errorf(p.Name, diag.MissingType, "parameter %s needs a type: %s: Type", p.Name.Name, p.Name.Name)
			}
			s.Params = append(s.Params, &Param{Name: p.Name.Name, Type: c.typeExpr(p.Type), Default: p.Default})
		}
	}
	if d.Result != nil {
		s.Result = c.typeExpr(d.Result)
	}
	return s
}

// funcSig resolves a function's signature (methods' too).
func (c *Checker) funcSig(f *Function) {
	if f.Sig != nil {
		return
	}
	savedScope := c.scope
	switch {
	case f.Recv != nil:
		c.scope = c.typeScope(f.Recv)
	case f.Ext != nil:
		c.scope = c.extScope(f.Ext)
	default:
		c.scope = NewScope(c.declScope(f.Decl))
	}
	f.TypeParams = c.typeParams(f.Decl.TypeParams)
	if f.Decl.Mut.IsValid() {
		c.mutDecl(f)
	}
	if f.Decl.Extern.IsValid() {
		c.externSig(f)
	} else {
		f.Sig = c.signature(f.Decl, f)
	}
	c.scope = savedScope
}

// mutDecl reports a mut fn where nothing is changed through it: only a
// struct's, an enum's or an extension's method changes the value it's
// called on.
func (c *Checker) mutDecl(f *Function) {
	d := f.Decl
	switch {
	case f.Static != nil:
		c.errorf(d.Name, diag.InvalidModifier, "a static fn has no value to change: drop mut")
	case f.Recv != nil && f.Recv.Kind == StoreType:
		c.errorf(d.Name, diag.InvalidModifier, "a store's fns change its state as they are: drop mut")
	case f.Recv != nil && f.Recv.Kind == ComponentType:
		c.errorf(d.Name, diag.InvalidModifier, "a component's fns change its state as they are: drop mut")
	case f.Recv == nil && !f.IsExtMethod():
		c.errorf(d.Name, diag.MisplacedDeclaration, "mut fn is a method that changes its value: declare it in a struct, an enum or an extend")
	case d.Native.IsValid() || d.Extern.IsValid():
		c.errorf(d.Name, diag.InvalidModifier, "a native fn can't be mut")
	}
}

// globalType gives a package-level value, or a type's constant, its type.
func (c *Checker) globalType(g *Global) Type {
	if g.Type != nil {
		return g.Type
	}
	if c.resolving[g] {
		if _, ok := g.Decl.(*syntax.StateDecl); ok {
			c.errorf(g.Decl, diag.MissingType, "%s reads itself (its current value, none the first time), so its type must be written: state %s: T = …", g.name, g.name)
		} else {
			c.errorf(g.Decl, diag.RecursiveDefinition, "%s's value is defined in terms of itself", g.name)
		}
		g.Type = Invalid
		return g.Type
	}
	c.resolving[g] = true
	defer delete(c.resolving, g)

	savedScope, savedCtx, savedG, savedPkg, savedOf := c.scope, c.ctx, c.curGlobal, c.pkg, c.defaultOf
	c.defaultOf = nil
	savedTry, savedAwait, savedHandled, savedView := c.inTry, c.inAwait, c.handled, c.view
	c.inTry, c.inAwait, c.handled, c.view = 0, 0, 0, false
	c.pkg = g.Pkg
	if g.Owner != nil {
		c.scope = c.typeScope(g.Owner)
	} else {
		c.scope = c.declScope(g.Decl)
	}
	c.ctx = nil
	c.curGlobal = g
	ctx := c.enter(CtxGlobal, g.Decl, g.name)
	ctx.Throws = false
	g.Ctx = ctx
	var want Type
	var self *Var
	switch d := g.Decl.(type) {
	case *syntax.StateDecl:
		c.deps(d.Deps)
		if d.Type != nil {
			want = c.typeExpr(d.Type)
		}
		self = c.stateSelf(g.name, g.pos, want)
	case *syntax.LetDecl:
		if d.Type != nil {
			want = c.typeExpr(d.Type)
		}
	}
	var t Type
	if want != nil {
		c.assign(g.Value, want, "the value of "+g.name)
		t = want
	} else {
		t = c.valueType(c.expr(g.Value, nil), g.Value)
	}
	if g.Owner != nil && ctx.readsState {
		c.errorf(g.Value, diag.InvalidDeclaration, "a type's constant can't read state: compute it where it's used")
	}
	g.Self = c.selfUsed(self, g.Value)
	c.leave(nil)
	c.scope, c.ctx, c.curGlobal, c.pkg, c.defaultOf = savedScope, savedCtx, savedG, savedPkg, savedOf
	c.inTry, c.inAwait, c.handled, c.view = savedTry, savedAwait, savedHandled, savedView
	g.Type = t
	return t
}

// valueType is the type a declaration infers from its value: none alone
// says nothing.
func (c *Checker) valueType(t Type, e syntax.Expr) Type {
	if t == NoneType {
		c.errorf(e, diag.CannotInferType, "none says nothing of the type: write it, let x: T? = none")
		return Invalid
	}
	if t == Void {
		c.errorf(e, diag.NoValue, "this has no value (it's void)")
		return Invalid
	}
	if t == Never {
		c.errorf(e, diag.NoValue, "this has no value: it leaves (throw, return, break, continue)")
		return Invalid
	}
	return t
}

// typeBodies checks a type's defaults, constants, methods and, for
// components and stores, its body.
func (c *Checker) typeBodies(t *TypeName) {
	for _, f := range t.Fields {
		if f.Default != nil {
			c.defaultValue(t, f.Default, f.Type, "field "+f.Name)
		}
	}
	for _, g := range t.Consts {
		c.globalType(g)
	}
	for _, name := range sortedMethodNames(t) {
		f := t.Methods[name]
		c.funcSig(f)
		c.funcBody(f)
	}
	if t.Comp != nil && !t.Comp.Native {
		c.componentBody(t)
	}
	if t.Comp != nil && t.Comp.Native && !t.Pkg.Prelude {
		c.nativeComponent(t)
	}
	if t.Kind == StructType || t.Kind == EnumType {
		for _, i := range t.Conforms {
			c.checkImplements(t.Decl, t.Type, i)
		}
	}
	if t.Kind == ExternType {
		c.externType(t)
	}
	if t.Extern {
		c.externStructCycle(t)
	}
}

func sortedMethodNames(t *TypeName) []string {
	var names []string
	for n := range t.Methods {
		names = append(names, n)
	}
	// Source order.
	pos := func(n string) syntax.Pos { return t.Methods[n].pos }
	for i := 1; i < len(names); i++ {
		for j := i; j > 0 && pos(names[j]) < pos(names[j-1]); j-- {
			names[j], names[j-1] = names[j-1], names[j]
		}
	}
	return names
}

// defaultValue checks a default (a field's, a parameter's) where it's
// declared: it may use package-level names only.
func (c *Checker) defaultValue(owner *TypeName, e syntax.Expr, want Type, what string) {
	savedScope, savedCtx := c.scope, c.ctx
	if owner != nil {
		c.scope = c.typeScope(owner)
	} else if c.scope == nil {
		c.scope = c.pkg.Scope
	}
	c.ctx = nil
	savedOf := c.defaultOf
	c.defaultOf = owner
	ctx := c.enter(CtxGlobal, nil, "default")
	c.info.Ctxs[e] = ctx
	c.assign(e, want, "the default of "+what)
	c.leave(nil)
	c.scope, c.ctx, c.defaultOf = savedScope, savedCtx, savedOf
}

// funcBody checks a function's or a method's body.
func (c *Checker) funcBody(f *Function) {
	if f.Ctx != nil || f.Decl == nil {
		return
	}
	c.funcSig(f)
	d := f.Decl
	if d.Native.IsValid() {
		switch {
		case d.Body != nil || d.Value != nil:
			c.errorf(d.Name, diag.FnBody, "a native fn has no body: the runtime implements it")
		case f.Pkg == nil || !StdPath(f.Pkg.Path):
			c.errorf(d.Name, diag.MisplacedDeclaration, "native fn is the standard library's (its runtime implements it); a package's platform code is a native interface (https://asuka.egoist.dev/docs/native)")
		case len(f.TypeParams) > 0 && f.Recv != nil:
			c.errorf(d.Name, diag.InvalidModifier, "a method's native fn isn't generic")
		}
	} else if d.Extern.IsValid() {
		c.externFunc(f)
	} else if d.Body == nil && d.Value == nil {
		c.errorf(d.Name, diag.FnBody, "fn %s has no body", f.name)
		return
	}
	savedScope, savedCtx := c.scope, c.ctx
	savedTry, savedAwait, savedHandled, savedLoops, savedView := c.inTry, c.inAwait, c.handled, c.loops, c.view
	c.inTry, c.inAwait, c.handled, c.loops, c.view = 0, 0, 0, 0, false
	if f.Recv != nil {
		c.scope = c.typeScope(f.Recv)
		if f.Recv.Comp != nil && f.Recv.Comp.Ctx != nil {
			c.scope = f.Recv.Comp.Ctx.scope
		} else {
			// A struct's fields and an enum's or a struct's methods, by
			// their bare names.
			c.scope = NewScope(c.scope)
			for _, fl := range f.Recv.Fields {
				c.scope.Insert(&selfMember{object: object{fl.Name, fl.Pos}, field: fl, owner: f.Recv})
			}
			for _, m := range f.Recv.Methods {
				c.scope.Insert(&selfMember{object: object{m.name, m.pos}, method: m, owner: f.Recv})
			}
			if it := f.Recv.Iface; it != nil {
				// A provided method's: the interface's fields and the
				// methods it asks for.
				for _, fl := range it.Fields {
					c.scope.Insert(&selfMember{object: object{fl.Name, fl.Pos}, field: fl, owner: f.Recv})
				}
				for _, m := range it.Methods {
					if m.Provided == nil {
						c.scope.Insert(&selfMember{object: object{m.Name, f.Recv.pos}, owner: f.Recv})
					}
				}
			}
		}
	} else if f.IsExtMethod() {
		// self's fields by their bare names, as the type's own methods
		// see them; its methods are self's (implicitSelf).
		c.scope = NewScope(c.extScope(f.Ext))
		if n, ok := f.Ext.Target.(*Named); ok && n.Obj.Kind == StructType {
			for _, fl := range n.Obj.Fields {
				c.scope.Insert(&selfMember{object: object{fl.Name, fl.Pos}, field: fl, owner: n.Obj, subst: n.Subst()})
			}
		}
	} else if f.Ext != nil {
		c.scope = c.extScope(f.Ext)
	} else {
		c.scope = c.declScope(d)
	}
	for _, p := range f.TypeParams {
		c.scope = NewScope(c.scope)
		c.scope.Insert(p.object())
	}
	c.ctx = nil
	kind := CtxFunc
	name := f.name
	if f.Recv != nil {
		kind = CtxMethod
		name = f.Recv.name + "." + f.name
	} else if f.IsExtMethod() {
		kind = CtxMethod
		name = f.Ext.Name() + "." + f.name
	}
	ctx := c.enter(kind, d, name)
	ctx.Func = f
	f.Ctx = ctx
	if d.Native.IsValid() && trackedNative(f.Pkg.Path, f.name) {
		ctx.readsState = true
	}
	ctx.Async = f.Sig.Async
	ctx.Throws = f.Sig.Throws
	ctx.Result = f.Sig.Result
	if f.Recv != nil {
		switch f.Recv.Kind {
		case StructType, EnumType:
			self := &Var{object: object{"self", d.Name.Start}, Type: f.Recv.Type, Kind: SelfVar, Ctx: ctx}
			ctx.Self = self
		case InterfaceType:
			// A provided method's self: a value of each type that has
			// the interface, as the body is compiled for it.
			ctx.Self = &Var{object: object{"self", d.Name.Start}, Type: f.Recv.Iface.Self, Kind: SelfVar, Ctx: ctx}
		case StoreType, ComponentType, WorkerType:
			ctx.Comp = f.Recv
		}
	} else if f.IsExtMethod() {
		ctx.Self = &Var{object: object{"self", d.Name.Start}, Type: f.Ext.Target, Kind: SelfVar, Ctx: ctx}
	}
	for i, p := range f.Sig.Params {
		v := c.declareVar(d.Params.List[i].Name, p.Type, false)
		p.Var = v
		ctx.Params = append(ctx.Params, v)
		if p.Default != nil {
			c.defaultValue(nil, p.Default, p.Type, "parameter "+p.Name)
		}
	}
	if d.Native.IsValid() || d.Extern.IsValid() {
		// The runtime's builtin, or the glue's, is the body
		// (lang/LOWERING.md).
	} else if d.Value != nil {
		if ctx.Result == Void {
			c.expr(d.Value, nil)
		} else {
			c.assign(d.Value, ctx.Result, "the result of "+f.name)
		}
	} else {
		c.stmts(d.Body.Items)
		if ctx.Result != Void && !c.terminates(d.Body.Items) {
			if ctx.Result == Never {
				c.errorPos(d.Body.Rbrace, diag.MissingReturn, "fn %s never returns (its result is never): it can reach its end, which it mustn't (end it with panic, a throw or a call of another that never returns)", f.name)
			} else {
				c.errorPos(d.Body.Rbrace, diag.MissingReturn, "fn %s must return %s: it can reach its end without a return", f.name, aType(ctx.Result))
			}
		}
	}
	c.leave(nil)
	c.scope, c.ctx = savedScope, savedCtx
	c.inTry, c.inAwait, c.handled, c.loops, c.view = savedTry, savedAwait, savedHandled, savedLoops, savedView
}

// testBody checks a test block: async, its errors fail it.
func (c *Checker) testBody(t *Test) {
	savedScope, savedCtx := c.scope, c.ctx
	c.scope = c.declScope(t.Decl)
	c.ctx = nil
	ctx := c.enter(CtxTest, t.Decl, "test "+t.Name)
	ctx.Async, ctx.Throws, ctx.Result = true, true, Void
	t.Ctx = ctx
	c.stmts(t.Decl.Body.Items)
	c.leave(nil)
	c.scope, c.ctx = savedScope, savedCtx
}

// appBody checks app { }: the root's view items.
func (c *Checker) appBody(p *Package) {
	savedScope, savedCtx := c.scope, c.ctx
	c.scope = c.declScope(p.App)
	c.ctx = nil
	ctx := c.enter(CtxApp, p.App, "app")
	ctx.Result = View
	var views []syntax.Item
	for _, it := range p.App.Body.Items {
		if d, ok := it.(*syntax.ProvideDecl); ok {
			pt := c.expr(d.Value, nil)
			if n, ok := pt.(*Named); !ok || n.Obj.Kind != StoreType {
				if !IsInvalid(pt) {
					c.errorf(d.Value, diag.TypeMismatch, "provide takes a store, for use in the components below: this is %s", aType(pt))
				}
			}
			continue
		}
		if h, ok := it.(*syntax.OnHandler); ok {
			c.appEvent(h)
			continue
		}
		// The app's tasks and effects: while it runs.
		if d, ok := it.(*syntax.TaskDecl); ok {
			c.taskDecl(d)
			continue
		}
		if d, ok := it.(*syntax.EffectDecl); ok {
			c.effectDecl(d, "app.effect")
			continue
		}
		if d, ok := it.(*syntax.CleanupStmt); ok {
			c.cleanupStmt(d, true)
			continue
		}
		views = append(views, it)
	}
	views = c.bodyCalls(views)
	c.viewBlock(views, nil, "app")
	c.leave(nil)
	c.scope, c.ctx = savedScope, savedCtx
}

// checkImplements reports what t lacks of interface i, when it declared
// it.
func (c *Checker) checkImplements(at syntax.Node, t Type, i *TypeName) {
	if why := c.implements(t, &Named{Obj: i}); why != "" {
		c.errorf(at, diag.InterfaceNotSatisfied, "%s declares %s but %s", typeString(t), i.name, why)
	}
}

// implements reports why t doesn't have interface iface's methods and
// fields, "" when it does.
func (c *Checker) implements(t Type, iface *Named) string {
	if why := c.implementsMembers(t, iface); why != "" {
		return why
	}
	if n := AsNamed(t, EnumType); n != nil && !iface.Obj.isErrorIface && !n.Obj.RefEnum() {
		return "an enum without payloads is stored as an int: declare the interface (enum " + n.Obj.name + ": " + iface.Obj.name + ") to use it as one"
	}
	return ""
}

// implementsMembers reports why a struct or an enum t doesn't have
// interface iface's methods and fields, "" when it does.
// extraParams says why a method that takes what the interface's does
// and more, the rest with defaults (greet(loud: bool, times: int = 1)
// for greet(loud: bool)), isn't the interface's: a call through the
// interface passes its own parameters, and nothing gives the rest. ""
// when that isn't why.
func extraParams(got, want *Signature, name string) string {
	if len(got.Params) <= len(want.Params) || got.Async != want.Async || got.Throws != want.Throws || !Identical(got.Result, want.Result) {
		return ""
	}
	for i, p := range want.Params {
		if !Identical(got.Params[i].Type, p.Type) {
			return ""
		}
	}
	var extra []string
	for _, p := range got.Params[len(want.Params):] {
		if p.Default == nil {
			return ""
		}
		extra = append(extra, p.Name)
	}
	takes := extra[0]
	switch len(extra) {
	case 1:
		takes += ", which the interface's " + name + " doesn't"
	default:
		takes = strings.Join(extra[:len(extra)-1], ", ") + " and " + extra[len(extra)-1] + ", which the interface's " + name + " doesn't"
	}
	return name + " takes " + takes + ": an interface's method is called with its own parameters"
}

func (c *Checker) implementsMembers(t Type, iface *Named) string {
	n, ok := t.(*Named)
	if !ok || (n.Obj.Kind != StructType && n.Obj.Kind != EnumType) {
		return "only structs and enums have interfaces"
	}
	it := iface.Obj
	if it.isErrorIface {
		if !n.Obj.IsError {
			return "it isn't declared an error: struct " + n.Obj.name + ": Error"
		}
		return ""
	}
	if it.Iface == nil {
		return "" // still being declared
	}
	s, is := n.Subst(), iface.Subst()
	for _, m := range it.Iface.Methods {
		if m.Provided != nil {
			// The type's own of its signature, an extension's, or the
			// interface's.
			if n.Obj.Methods[m.Name] == nil {
				if hit, _ := c.extLookup(t, m.Name, "method", nil); hit.f != nil {
					c.extImpl(n, m.Name, hit.f)
				}
			}
			continue
		}
		f, fs := n.Obj.Methods[m.Name], s
		if f == nil {
			// An extension's, where the package sees it.
			hit, _ := c.extLookup(t, m.Name, "method", nil)
			if hit.f == nil {
				return "it has no " + m.Name + "()"
			}
			f, fs = hit.f, hit.s
			c.extImpl(n, m.Name, f)
		}
		c.funcSig(f)
		got, want := fs.Apply(f.Sig.Type()), is.Apply(m.Sig.Type())
		if !Identical(got, want) {
			if why := extraParams(fs.applySig(f.Sig), is.applySig(m.Sig), m.Name); why != "" {
				return why
			}
			return "its " + m.Name + " is " + typeString(got) + ", not " + typeString(want)
		}
		if f.Sig.Mut && !m.Sig.Mut {
			return mutNotAsked(m.Name)
		}
	}
	for _, fl := range it.Iface.Fields {
		want := is.Apply(fl.Type)
		if n.Obj.Kind == StructType {
			f := n.Obj.Field(fl.Name)
			if f == nil {
				return "it has no field " + fl.Name
			}
			if !Identical(s.Apply(f.Type), want) {
				return "its field " + fl.Name + " is " + typeString(s.Apply(f.Type)) + ", not " + typeString(want)
			}
		} else {
			f, fs := n.Obj.Methods[fl.Name], s
			if f == nil {
				if hit, _ := c.extLookup(t, fl.Name, "method", nil); hit.f != nil {
					f, fs = hit.f, hit.s
					c.extImpl(n, fl.Name, f)
				}
			}
			if f == nil {
				return "it has no " + fl.Name + " (an enum has it as a method: fn " + fl.Name + "() -> " + typeString(want) + ")"
			}
			c.funcSig(f)
			if len(f.Sig.Params) != 0 || !Identical(fs.Apply(f.Sig.Result), want) {
				return "its " + fl.Name + " isn't fn " + fl.Name + "() -> " + typeString(want)
			}
		}
	}
	return ""
}

// nativeComponent checks a package's native component: a view its
// platforms' code shows (docs/native.md), declared by its header
// parameters, props and events, whose values cross the native wire.
func (c *Checker) nativeComponent(t *TypeName) {
	comp := t.Comp
	if comp.Body != nil {
		for _, it := range comp.Body.Items {
			switch it.(type) {
			case *syntax.PropDecl, *syntax.EventDecl, *syntax.BadItem:
			default:
				c.errorf(it, diag.MisplacedDeclaration, "a native component declares props and events only: its platforms' code shows it")
			}
		}
	}
	for _, m := range comp.Order {
		switch m.Kind {
		case ParamMember, PropMember:
			if m.Default != nil {
				c.memberValue(m)
			}
			if why := c.nativeValue(m.Type); why != "" {
				c.errorf(m.Decl, diag.NativeType, "%s can't cross to the platform's view: %s", m.name, why)
			}
		case EventMember:
			for _, p := range m.Params {
				if why := c.nativeValue(p.Type); why != "" {
					c.errorf(m.Decl, diag.NativeType, "event %s's %s can't come from the platform's view: %s", m.name, p.Name, why)
				}
			}
		}
	}
}

// nativeValue says why a native view's value can't be t, "" when it can:
// what JSON can (the native codec's values), but no type parameters.
func (c *Checker) nativeValue(t Type) string {
	if IsInvalid(t) {
		return ""
	}
	if hasSet(t, map[*TypeName]bool{}) {
		return "a Set doesn't cross to the platform: pass an array"
	}
	return jsonable1(t, map[*TypeName]bool{}, func(*TypeParam) bool { return false })
}

// hasSet reports a Set in t (its elements, fields, payloads).
func hasSet(t Type, seen map[*TypeName]bool) bool {
	switch x := t.(type) {
	case *Set:
		return true
	case *Array:
		return hasSet(x.Elem, seen)
	case *Map:
		return hasSet(x.Elem, seen)
	case *Optional:
		return hasSet(x.Elem, seen)
	case *Tuple:
		for _, e := range x.Elems {
			if hasSet(e, seen) {
				return true
			}
		}
	case *Named:
		if seen[x.Obj] {
			return false
		}
		seen[x.Obj] = true
		s := x.Subst()
		for _, f := range x.Obj.Fields {
			if hasSet(s.Apply(f.Type), seen) {
				return true
			}
		}
		for _, k := range x.Obj.Cases {
			for _, p := range k.Params {
				if hasSet(s.Apply(p), seen) {
					return true
				}
			}
		}
	}
	return false
}

// structCycles reports the package's structs that hold themselves, in a
// field or a field's field, by value (struct A { b: B }; struct B { a:
// A }): such a value is never whole, so none can be made. An optional,
// an array, a map or an enum between them is a way out (struct Node {
// next: Node? }).
func (c *Checker) structCycles(p *Package) {
	reported := map[*TypeName]bool{}
	for _, t := range p.Types {
		if t.Kind != StructType || t.Extern || reported[t] {
			continue
		}
		var path []string
		onPath := map[*TypeName]bool{}
		var holds func(n *TypeName, s Subst) bool
		var inType func(ft Type) bool
		inType = func(ft Type) bool {
			switch ft := ft.(type) {
			case *Named:
				if ft.Obj.Kind != StructType || ft.Obj.Extern {
					return false
				}
				if ft.Obj == t {
					return true
				}
				if onPath[ft.Obj] {
					return false // another struct's cycle: reported for it
				}
				return holds(ft.Obj, ft.Subst())
			case *Tuple:
				for _, e := range ft.Elems {
					if inType(e) {
						return true
					}
				}
			}
			return false
		}
		holds = func(n *TypeName, s Subst) bool {
			onPath[n] = true
			defer delete(onPath, n)
			for _, f := range n.Fields {
				path = append(path, n.name+"."+f.Name)
				if inType(s.Apply(f.Type)) {
					return true
				}
				path = path[:len(path)-1]
			}
			return false
		}
		if holds(t, nil) {
			for _, o := range p.Types {
				for _, step := range path {
					if strings.HasPrefix(step, o.name+".") {
						reported[o] = true
					}
				}
			}
			f := t.Fields[t.fieldIndex[strings.TrimPrefix(path[0], t.name+".")]]
			c.errorPos(f.Pos, diag.RecursiveType, "struct %s holds itself (%s): a value can't hold a whole copy of itself, so none can be made; make a field on the way optional (%s: %s?), or an array", t.name, strings.Join(path, " → "), f.Name, typeString(f.Type))
		}
	}
}

// mutNotAsked says why a mut fn isn't an interface's method that isn't
// one: the interface's is called on any value, not only on a place.
func mutNotAsked(name string) string {
	return "its " + name + " is a mut fn, and the interface's isn't (an interface's fn is called on any value, a mut fn on a var or state): declare the interface's mut fn " + name
}
