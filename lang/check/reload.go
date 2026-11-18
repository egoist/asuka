package check

import (
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// Reloads (rfcs/0017). reload f(args) and reload f ask again every live
// load in the app whose operand is a call of f: with arguments equal to
// args when they're given, whatever its arguments when they aren't. The
// call is the key: f is a declared function or a store's method (every
// instance's loads of it), checked as a call is, without running it. A
// load of anything else (a closure's call, a method of a value) has its
// x.reload(). reload is a task of nothing: await reload … waits for
// every load it reached to answer or fail.

// A Reload is what a reload asks again: the loads of Func, those whose
// arguments equal Call's when it has any (nil: every load of Func).
type Reload struct {
	Func *Function
	Call *syntax.CallExpr
}

// reloadable is the function a call's loads are keyed by: a declared
// function's, or a store's method's; nil for any other call.
func reloadable(ci *Call) *Function {
	if ci == nil || ci.Func == nil {
		return nil
	}
	switch ci.Kind {
	case CallFunc:
		return ci.Func
	case CallMethod:
		if ci.Func.Recv != nil && ci.Func.Recv.Kind == StoreType {
			return ci.Func
		}
	}
	return nil
}

// ReloadKey is the function load's operand (a call) is keyed by for
// reload, or nil when it isn't one reload reaches.
func (p *Program) ReloadKey(call *syntax.CallExpr) *Function {
	return reloadable(p.Info.Calls[call])
}

// noteLoad records the function a load calls, for reload-not-loaded.
func (c *Checker) noteLoad(call *syntax.CallExpr) {
	if f := reloadable(c.info.Calls[call]); f != nil {
		if c.loaded == nil {
			c.loaded = map[*Function]bool{}
		}
		c.loaded[f] = true
	}
}

func (c *Checker) reload(e *syntax.UnaryExpr) Type {
	if c.ctx != nil {
		switch c.ctx.Kind {
		case CtxBinding, CtxGlobal, CtxBuilder, CtxComponent, CtxStore, CtxApp:
			c.errorf(e, diag.WriteInBinding, "can't reload here: what the UI follows is computed, not changed; reload in a handler (on press { … }), a fn or a task")
		}
	}
	task := &Task{Elem: Void}
	var f *Function
	call, isCall := unparen(e.X).(*syntax.CallExpr)
	if isCall {
		// The call's errors are its arguments' (reload-arguments): it's
		// checked as it's written, not made (no await, no try).
		n := len(c.diags)
		c.inAwait++
		c.inTry++
		c.expr(call, nil)
		c.inTry--
		c.inAwait--
		args := call.Args
		for i := n; i < len(c.diags) && args != nil; i++ {
			d := &c.diags[i]
			if at := c.fset.Position(args.Start); d.Pos.Filename == at.Filename && d.Pos.Offset >= at.Offset && d.Pos.Offset <= c.fset.Position(args.End).Offset {
				d.Code = diag.ReloadArguments.Name
			}
		}
		f = reloadable(c.info.Calls[call])
		if f == nil && c.info.Calls[call] != nil {
			c.errorf(call.Fun, diag.ReloadNotLoaded, "reload reaches the loads of a function or a store's method, by its call: %s isn't one; reload its load with x.reload()", exprName(call.Fun))
			return task
		}
	} else {
		f = c.reloadName(e.X)
	}
	if f == nil {
		return task
	}
	r := &Reload{Func: f}
	if isCall {
		r.Call = call
	}
	c.info.Reloads[e] = r
	c.reloads = append(c.reloads, e)
	return task
}

// reloadName is the function x names, reload f's operand: a function, a
// package's, the store's own method by its name, or a store's method on
// a value of it. Nil (said) when it isn't one.
func (c *Checker) reloadName(x syntax.Expr) *Function {
	notOne := func() {
		c.errorf(x, diag.ReloadNotLoaded, "reload takes what a load calls: a function's call or its name (reload api.notes(id), reload api.notes); %s isn't one", exprName(x))
	}
	switch n := unparen(x).(type) {
	case *syntax.Ident:
		o := c.lookup(n.Name)
		if o == nil {
			c.unknownName(n)
			return nil
		}
		c.info.Uses[n] = o
		switch o := o.(type) {
		case *Function:
			if o.Recv == nil {
				return o
			}
		case *Member:
			if o.Kind == FnMember {
				c.envUse(o.Owner, n, o.name)
				return o.Fn
			}
		}
	case *syntax.MemberExpr:
		if id, ok := n.X.(*syntax.Ident); ok {
			if pn, ok := c.lookup(id.Name).(*PkgName); ok {
				c.info.Uses[id] = pn
				o := pn.Pkg.Scope.LookupLocal(n.Name.Name)
				if o == nil {
					c.errorf(n.Name, diag.UnknownMember, "package %s has no %s", pn.Pkg.Path, n.Name.Name)
					return nil
				}
				if !c.exported(pn.Pkg, o) {
					c.errorf(n.Name, diag.NotPub, "%s isn't pub in package %s", n.Name.Name, pn.Pkg.Path)
				}
				c.info.Uses[n.Name] = o
				c.info.Selections[n] = &Selection{Kind: SelPkg, Obj: o}
				if f, ok := o.(*Function); ok {
					return f
				}
				notOne()
				return nil
			}
		}
		xt := c.expr(n.X, nil)
		if IsInvalid(xt) {
			return nil
		}
		if t, ok := xt.(*Named); ok && t.Obj.Kind == StoreType {
			if m := t.Obj.Comp.Members[n.Name.Name]; m != nil && m.Kind == FnMember {
				if t.Obj.Pkg != c.pkg && !m.Pub {
					c.errorf(n.Name, diag.NotPub, "%s isn't pub in store %s", n.Name.Name, t.Obj.name)
				}
				c.info.Selections[n] = &Selection{Kind: SelMethod, Name: n.Name.Name, Recv: xt}
				return m.Fn
			}
		}
	}
	notOne()
	return nil
}

// reloadsLoaded reports each reload of a function no load in the
// program calls: it would reach nothing (reload-not-loaded).
func (c *Checker) reloadsLoaded() {
	for _, e := range c.reloads {
		r := c.info.Reloads[e]
		if r == nil || c.loaded[r.Func] {
			continue
		}
		what := e.X
		if r.Call != nil {
			what = r.Call.Fun
		}
		c.errorf(what, diag.ReloadNotLoaded, "nothing loads %s: reload what a load calls (let x = load %s(…))", exprName(what), exprName(what))
	}
	c.reloads = nil
}
