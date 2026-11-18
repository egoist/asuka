package check

import (
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// Workers (RFC 0011): worker W(params) { var …; let …; fn … }, a type
// whose instances each run on a thread of their own. Its vars, lets and
// parameters are its own, read and changed by its fns there; its pub fns,
// called from outside, are async (CallWorker): their arguments and
// results cross as values, spawn's rule (sendable). Its body and fns run
// off the main thread, so they read nothing of the main thread's (state,
// stores, a file's let that isn't a value), as spawn's block doesn't.

// workerMembers declares a worker's members: its parameters, vars, lets
// and fns. Anything else in its body is an error.
func (c *Checker) workerMembers(t *TypeName, params *syntax.ParamList, body *syntax.Block) {
	comp := t.Comp
	comp.Members = map[string]*Member{}
	ctx := &FuncCtx{Kind: CtxWorker, Node: t.Decl, Name: t.name, Comp: t, Result: t.Type}
	ctx.scope = NewScope(c.scope)
	comp.Ctx = ctx
	c.info.Ctxs[t.Decl] = ctx
	add := func(id *syntax.Ident, m *Member) {
		m.object = object{id.Name, id.Start}
		m.Owner = t
		c.info.Defs[id] = m
		if comp.Members[id.Name] != nil {
			c.errorf(id, diag.DuplicateDeclaration, "%s is declared twice in %s", id.Name, t.name)
			return
		}
		comp.Members[id.Name] = m
		comp.Order = append(comp.Order, m)
		ctx.scope.Insert(m)
	}
	savedScope := c.scope
	c.scope = ctx.scope
	defer func() { c.scope = savedScope }()
	if params != nil {
		for _, p := range params.List {
			if p.Type == nil {
				c.errorf(p.Name, diag.MissingType, "parameter %s needs a type: %s: Type", p.Name.Name, p.Name.Name)
			}
			m := &Member{Kind: ParamMember, Type: c.typeExpr(p.Type), Default: p.Default, Decl: p}
			if !IsInvalid(m.Type) && !sendable(m.Type) {
				c.errorf(p.Type, diag.NotSendable, "worker %s takes values only: %s is %s, which can't cross to its thread", t.name, p.Name.Name, aType(m.Type))
			}
			add(p.Name, m)
			comp.Params = append(comp.Params, m)
		}
	}
	if body == nil {
		return
	}
	for _, it := range body.Items {
		switch d := it.(type) {
		case *syntax.LetDecl:
			c.noLetElse(d)
			bp, ok := d.Pattern.(*syntax.BindPat)
			if !ok {
				c.errorf(d.Pattern, diag.InvalidDeclaration, "a worker's %s names one value: %s name = …", d.Keyword, d.Keyword)
				continue
			}
			m := &Member{Kind: LetMember, Default: d.Value, Decl: d}
			if d.Keyword == syntax.VAR {
				m.Kind = VarMember
			}
			if d.Type != nil {
				m.Type = c.typeExpr(d.Type)
			}
			if d.Value == nil {
				c.errorf(bp.Name, diag.InvalidDeclaration, "%s needs a value: %s %s = …", bp.Name.Name, d.Keyword, bp.Name.Name)
			}
			add(bp.Name, m)
		case *syntax.FuncDecl:
			f := &Function{object: object{d.Name.Name, d.Name.Start}, Pkg: t.Pkg, Decl: d, Recv: t}
			m := &Member{Kind: FnMember, Decl: d, Fn: f, Pub: d.Pub.IsValid()}
			switch {
			case d.Static.IsValid():
				c.errorf(d.Name, diag.MisplacedDeclaration, "a worker has no static fns: declare %s outside it", d.Name.Name)
			case d.Native.IsValid() || d.Extern.IsValid():
				c.errorf(d.Name, diag.MisplacedDeclaration, "a worker's fns are Asuka's: declare an extern fn outside it, and call it from here")
			case d.Async.IsValid():
				c.errorf(d.Name, diag.InvalidModifier, "a worker's fns don't await: each runs on its thread, one call at a time; drop async (called from outside, a pub fn is async already)")
			case len(d.TypeParams) > 0:
				c.errorf(d.Name, diag.InvalidModifier, "a worker's fns aren't generic: its pub fns' values cross to its thread, and their types are known")
			}
			add(d.Name, m)
			if t.Methods == nil {
				t.Methods = map[string]*Function{}
			}
			t.Methods[f.name] = f
			c.funcSig(f)
			m.Type = f.Sig.Type()
			if m.Pub {
				c.workerSignature(t, d, f.Sig)
			}
		case *syntax.StateDecl:
			c.errorf(d, diag.MisplacedDeclaration, "a worker has no state: views don't read it, and its values are its own: var %s = …", d.Name.Name)
		case *syntax.PropDecl:
			c.errorf(d, diag.MisplacedDeclaration, "a worker has no props: give it parameters, worker %s(name: T)", t.name)
		case *syntax.EventDecl:
			c.errorf(d, diag.MisplacedDeclaration, "a worker has no events: its pub fns return what it has to say")
		case *syntax.UseDecl, *syntax.ProvideDecl:
			c.errorf(d, diag.MisplacedDeclaration, "a worker has no contexts: it runs on a thread of its own; pass it values")
		case *syntax.VariantDecl:
			c.errorf(d, diag.MisplacedDeclaration, "a worker has no variants: a component declares them, for its caller's styles")
		case *syntax.TaskDecl, *syntax.EffectDecl:
			c.errorf(d, diag.MisplacedDeclaration, "a worker has no tasks or effects: its pub fns run when they're called")
		case *syntax.CleanupStmt:
			c.errorf(d, diag.MisplacedDeclaration, "a worker has no cleanups: nothing owns it; its pub fns run when they're called")
		case *syntax.BadItem:
		default:
			c.errorf(it, diag.MisplacedDeclaration, "a worker has no views or statements: it's vars, lets and fns")
		}
	}
}

// workerSignature checks a pub fn's parameters and result: values only,
// which cross to the worker's thread and back.
func (c *Checker) workerSignature(t *TypeName, d *syntax.FuncDecl, sig *Signature) {
	for i, p := range sig.Params {
		if IsInvalid(p.Type) || sendable(p.Type) {
			continue
		}
		var at syntax.Node = d.Name
		if d.Params != nil && i < len(d.Params.List) && d.Params.List[i].Type != nil {
			at = d.Params.List[i].Type
		}
		c.errorf(at, diag.NotSendable, "%s.%s is pub, so it takes values only: %s is %s, which can't cross to the worker's thread", t.name, d.Name.Name, p.Name, aType(p.Type))
	}
	if !IsInvalid(sig.Result) && !sendable(sig.Result) {
		var at syntax.Node = d.Name
		if d.Result != nil {
			at = d.Result
		}
		c.errorf(at, diag.NotSendable, "%s.%s is pub, so it returns values only: %s can't cross back from the worker's thread", t.name, d.Name.Name, aType(sig.Result))
	}
}

// workerBody checks a worker's members' values: its body, which runs on
// its thread as it starts. Its fns are its methods (typeBodies).
func (c *Checker) workerBody(t *TypeName) {
	comp := t.Comp
	for _, m := range comp.Order {
		switch m.Kind {
		case ParamMember:
			if m.Default != nil {
				// Filled in where it's made, on the main thread, as a
				// function's defaults are.
				c.defaultValue(nil, m.Default, m.Type, "parameter "+m.name)
			}
		case LetMember, VarMember:
			if m.Default != nil {
				c.memberValue(m)
			}
		}
	}
}

// workerOf is the worker whose body or fn (or a closure in one) is being
// checked, nil elsewhere.
func (c *Checker) workerOf() *TypeName {
	if cc := c.workerRoot(); cc != nil {
		return cc.Comp
	}
	return nil
}

// workerRoot is the worker's body or fn being checked (the closures in
// it are under it), nil elsewhere.
func (c *Checker) workerRoot() *FuncCtx {
	for cc := c.ctx; cc != nil; cc = cc.Parent {
		if (cc.Kind == CtxWorker || cc.Kind == CtxMethod) && cc.Comp != nil && cc.Comp.Kind == WorkerType {
			return cc
		}
		if cc.Kind == CtxSpawn {
			return nil
		}
	}
	return nil
}

// offThread names what runs the code being checked off the main thread:
// "spawn" for spawn's block, "worker W" for a worker's body and fns; ""
// on the main thread.
func (c *Checker) offThread() string {
	for cc := c.ctx; cc != nil; cc = cc.Parent {
		if cc.Kind == CtxSpawn {
			return "spawn"
		}
		if (cc.Kind == CtxWorker || cc.Kind == CtxMethod) && cc.Comp != nil && cc.Comp.Kind == WorkerType {
			return "worker " + cc.Comp.name
		}
	}
	return ""
}

// workerNew checks W(args): a worker made, its thread started. Its
// arguments are values, copied over.
func (c *Checker) workerNew(e *syntax.CallExpr, tn *TypeName) Type {
	if e.Block != nil {
		c.errorf(e.Block, diag.UnexpectedBlock, "%s is a worker: its parameters go in parentheses", tn.name)
	}
	if what := c.offThread(); what != "" {
		c.errorf(e.Fun, diag.WorkerMisuse, "%s can't make worker %s: a worker is made on the main thread, by what owns it (a component, a store)", what, tn.name)
	}
	var params []*Param
	for _, m := range tn.Comp.Params {
		params = append(params, &Param{Name: m.name, Type: m.Type, Default: m.Default})
	}
	args := c.matchArgs(e, params, tn.name)
	for i, a := range args {
		if a != nil {
			c.assign(a, params[i].Type, argDesc(params[i])+" of "+tn.name)
		}
	}
	c.typeDefaults(args, params)
	c.info.Calls[e] = &Call{Kind: CallWorkerNew, Type: tn.Type.(*Named), Args: args, Params: params, Result: tn.Type}
	return tn.Type
}

// workerMethodCall checks w.f(…) of a worker w: from outside, a pub fn,
// async (its call waits for the worker's thread); inside the worker, any
// of its fns, a plain call.
func (c *Checker) workerMethodCall(e *syntax.CallExpr, fn *syntax.MemberExpr, n *Named, name string, want Type) (Type, bool) {
	m := n.Obj.Comp.Members[name]
	if m == nil {
		return nil, false
	}
	if m.Kind != FnMember {
		c.errorf(fn.Name, diag.WorkerMisuse, "%s is worker %s's own: it's read on its thread, by its fns (return it from a pub fn)", name, n.Obj.name)
		c.checkArgsLoosely(e)
		return Invalid, true
	}
	if c.workerOf() == n.Obj {
		return c.funcCall(e, m.Fn, fn.X, n, want), true
	}
	if !m.Pub {
		c.errorf(fn.Name, diag.NotPub, "%s isn't pub in worker %s: only its pub fns are called from outside it", name, n.Obj.name)
	}
	if what := c.offThread(); what != "" {
		c.errorf(fn, diag.WorkerMisuse, "%s can't call worker %s: its calls are made on the main thread", what, n.Obj.name)
	}
	call := c.funcCallAs(e, m.Fn, fn.X, n, want, true)
	call.Kind = CallWorker
	return call.Result, true
}

// workerCalls reports what a worker's body and fns call that reads the
// main thread's state (as spawn's check does, reactive.go's finalize).
func (c *Checker) workerCalls() {
	for _, p := range c.pkgs {
		for _, t := range p.Types {
			if t.Kind != WorkerType || t.Comp == nil {
				continue
			}
			roots := []*FuncCtx{t.Comp.Ctx}
			for _, name := range sortedMethodNames(t) {
				if f := t.Methods[name]; f.Ctx != nil {
					roots = append(roots, f.Ctx)
				}
			}
			for _, r := range roots {
				if r == nil {
					continue
				}
				for _, f := range r.calls {
					if f.Recv == t || f.Ctx == nil || !f.Ctx.readsState {
						continue
					}
					c.errorf(r.Node, diag.WorkerCapturesState, "worker %s calls %s, which reads state: the worker runs on a thread of its own; read the state on the main thread and pass its value to a pub fn", t.name, f.name)
				}
			}
		}
	}
}
