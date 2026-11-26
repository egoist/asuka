package check

import (
	"fmt"
	"sync/atomic"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// What asuka dev's debugger evaluates in a paused frame: an expression
// (a breakpoint's condition, a log message's, an editor's watch or
// hover), checked as the frame's code would see it: the names of its
// file and package, and the frame's locals, as parameters of a function
// of its own (lang/ir's LowerEval).

// EvalVar is a local of the frame's, by its name and type, that the
// expression sees. A struct's or an enum's self (named "self") is the
// receiver, its fields and methods then usable by their bare names, as
// in a method. One with Env is a component's or a store's env (its
// type none): its members are what the expression sees by their names,
// as the component's code does.
type EvalVar struct {
	Name string
	Type Type
	Env  *TypeName
}

// Eval is an expression checked to evaluate in a frame: Expr, of type
// Type, in a function whose parameters are Params (EvalVar's, in order).
type Eval struct {
	Expr   syntax.Expr
	Params []*Var
	Ctx    *FuncCtx
	Type   Type
	// File is the file it's checked as written in (the frame's).
	File string
	// EnvParam is the parameter that's Env's env (-1: none).
	Env      *TypeName
	EnvParam int
}

// evals counts the expressions checked, each a file of the fset's own.
var evals atomic.Int64

// CheckEval checks src, an expression, as written in file (the frame's,
// as its functions name it: its imports and its package's names; a file
// the program doesn't have: the root package's), with vars its locals:
// what the debugger evaluates in a paused frame. want is its type: Bool
// (a condition), String (a log message), nil (any value). It may call
// what throws (with try): the error is the evaluation's. It can't await.
// The program isn't changed by it, but for what's recorded of the
// expression's own nodes (Info).
func (p *Program) CheckEval(file string, vars []EvalVar, src string, want Type) (*Eval, []Diagnostic) {
	c := p.checker
	name := fmt.Sprintf("<eval %d>", evals.Add(1))
	e, errs := syntax.ParseExprIn(c.fset, name, src)
	if len(errs) > 0 {
		var ds []Diagnostic
		for _, err := range errs {
			ds = append(ds, FromSyntax(err))
		}
		return nil, ds
	}
	pkg, scope := p.fileScope(file)

	savedPkg, savedScope, savedCtx := c.pkg, c.scope, c.ctx
	savedTry, savedAwait, savedHandled, savedLoops, savedView := c.inTry, c.inAwait, c.handled, c.loops, c.view
	savedPartial := c.partial
	nd, nw := len(c.diags), len(c.warns)
	defer func() {
		c.pkg, c.scope, c.ctx = savedPkg, savedScope, savedCtx
		c.inTry, c.inAwait, c.handled, c.loops, c.view = savedTry, savedAwait, savedHandled, savedLoops, savedView
		c.partial = savedPartial
		c.diags, c.warns = c.diags[:nd], c.warns[:nw]
	}()
	c.pkg, c.ctx = pkg, nil
	c.inTry, c.inAwait, c.handled, c.loops, c.view, c.partial = 0, 0, 0, 0, false, nil
	c.scope = scope

	var self *Var
	var env *TypeName
	for _, v := range vars {
		if v.Env != nil && v.Env.Comp != nil && env == nil {
			// In a component's (a store's) code: its members by their
			// names, as its body's scope has them.
			env = v.Env
			if env.Comp.Ctx != nil && env.Comp.Ctx.scope != nil {
				c.scope = env.Comp.Ctx.scope
			}
			continue
		}
		if v.Name != "self" {
			continue
		}
		if n, ok := v.Type.(*Named); ok && (n.Obj.Kind == StructType || n.Obj.Kind == EnumType) {
			// A method's frame: its fields and methods by their bare
			// names, as funcBody's scope has them.
			c.scope = NewScope(c.scope)
			for _, fl := range n.Obj.Fields {
				c.scope.Insert(&selfMember{object: object{fl.Name, fl.Pos}, field: fl, owner: n.Obj, subst: n.Subst()})
			}
			for _, m := range n.Obj.Methods {
				c.scope.Insert(&selfMember{object: object{m.name, m.pos}, method: m, owner: n.Obj, subst: n.Subst()})
			}
			self = &Var{object: object{"self", e.Range().Start}, Type: v.Type, Kind: SelfVar}
		}
	}
	// The function's own node: the expression's may be a closure's,
	// whose context it keys.
	node := &syntax.Ident{Name: "eval"}
	node.Span = syntax.Span{Start: e.Range().Start, End: e.Range().Start}
	ctx := c.enter(CtxFunc, node, "eval")
	ctx.Throws = true
	ctx.Comp = env
	ev := &Eval{Expr: e, Ctx: ctx, File: file, Env: env, EnvParam: -1}
	for _, v := range vars {
		if v.Env != nil {
			// The env, a parameter of its own (one per local: they're
			// the frame's in order), not a name.
			if v.Env == env && ev.EnvParam < 0 {
				ev.EnvParam = len(ev.Params)
			}
			ev.Params = append(ev.Params, &Var{object: object{"self", node.Start}, Type: v.Env.Type, Ctx: ctx})
			continue
		}
		if v.Name == "self" && self != nil {
			self.Ctx = ctx
			ctx.Self = self
			ev.Params = append(ev.Params, self)
			continue
		}
		id := &syntax.Ident{Name: v.Name}
		id.Span = node.Span
		pv := &Var{object: object{v.Name, id.Start}, Type: v.Type, Ctx: ctx}
		c.info.Defs[id] = pv
		if c.scope.LookupLocal(v.Name) == nil && v.Name != "_" {
			c.scope.Insert(pv)
		}
		ev.Params = append(ev.Params, pv)
	}
	ctx.Params = ev.Params
	var t Type
	if want != nil {
		t = c.assign(e, want, "the expression")
	} else {
		t = c.expr(e, nil)
	}
	ctx.Result = t
	c.leave(nil)
	for len(c.laterChecks) > 0 {
		f := c.laterChecks[0]
		c.laterChecks = c.laterChecks[1:]
		f()
	}
	if len(c.diags) > nd {
		return nil, append([]Diagnostic(nil), c.diags[nd:]...)
	}
	if ctx.Awaits {
		return nil, []Diagnostic{NewDiagnostic(c.fset.Position(e.Range().Start), c.fset.Position(e.Range().End), diag.AwaitOutsideAsync, "an expression the debugger evaluates can't await: evaluate what's awaited once it's done")}
	}
	ev.Type = t
	return ev, nil
}

// fileScope is the package of the file named file and its scope (its
// imports, then the package's names); the root package's when no
// package has it.
func (p *Program) fileScope(file string) (*Package, *Scope) {
	for _, pkg := range p.Packages {
		for _, f := range pkg.Files {
			if f.File != nil && f.File.Name() == file {
				if s := pkg.fileScopes[f]; s != nil {
					return pkg, s
				}
				return pkg, pkg.Scope
			}
		}
	}
	if p.Root != nil {
		return p.Root, p.Root.Scope
	}
	return p.Prelude, p.Prelude.Scope
}
