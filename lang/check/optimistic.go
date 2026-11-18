package check

import (
	"sort"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// Optimistic changes (rfcs/0017): x.optimistic(f) on a Loading<T> shows
// f of what x shows until x's next answer replaces it, a throw out of
// the task that made it, or that task's end with nothing to confirm it.
// It changes what's shown: a handler's, a fn's or a task's, not a
// binding's. f runs again whenever what's under it changes, so it's a
// value function: what it reads of state may differ by then, and what
// it writes would be written again (optimistic-impure); its type isn't
// async, so it can't wait.

// optimisticCall checks x.optimistic(f): where it's called, and f once
// every body is checked (what f calls reads or writes is known then).
func (c *Checker) optimisticCall(e *syntax.CallExpr, fn *syntax.MemberExpr, args []syntax.Expr) {
	if c.ctx != nil {
		switch c.ctx.Kind {
		case CtxBinding, CtxGlobal, CtxBuilder, CtxComponent, CtxStore, CtxApp:
			c.errorf(fn.Name, diag.WriteInBinding, "can't change what %s shows here: what the UI follows is computed, not changed; make an optimistic change in a handler (on press { … }), a fn or a task", exprName(fn.X))
			return
		}
	}
	if len(args) == 0 || args[0] == nil {
		return
	}
	f := unparen(args[0])
	var ctx *FuncCtx
	switch x := f.(type) {
	case *syntax.LambdaExpr:
		ctx = c.info.Ctxs[x]
	case *syntax.Ident:
		if fo, ok := c.info.Uses[x].(*Function); ok {
			ctx = fo.Ctx
		}
	case *syntax.MemberExpr:
		if sel := c.info.Selections[x]; sel != nil && sel.Kind == SelPkg {
			if fo, ok := sel.Obj.(*Function); ok {
				ctx = fo.Ctx
			}
		}
	}
	if ctx == nil {
		return // a closure's value: what it does is its maker's to say
	}
	name := exprName(fn.X)
	c.laterChecks = append(c.laterChecks, func() { c.optimisticPure(f, ctx, name) })
}

// optimisticPure reports what makes f, x.optimistic's change (its body
// ctx), not a value function: the first state it reads, its await, or
// the first state it writes.
func (c *Checker) optimisticPure(f syntax.Expr, ctx *FuncCtx, name string) {
	const hint = "an optimistic change is a function of what's shown: pass what it needs in"
	// It can't await: its type, fn(T) -> T, isn't async (a closure that
	// awaits is reported where it's passed).
	// What it reads itself first, then what what it reads or calls does.
	for _, u := range append(append([]stateUse{}, ctx.stateReads...), orderedUses(c.readSet(ctx))...) {
		if what := c.stateName(u.obj); what != "" {
			c.errorf(u.at, diag.OptimisticImpure, "the change of %s reads %s, which is state: %s (let v = %s, outside it)", name, what, hint, what)
			return
		}
	}
	for _, u := range orderedUses(c.writeSet(ctx)) {
		if what := c.stateName(u.obj); what != "" {
			c.errorf(u.at, diag.OptimisticImpure, "the change of %s writes %s: it runs again whenever what's under it changes; write in the handler, and give back the changed value", name, what)
			return
		}
	}
}

// stateName names obj when it's what an optimistic change can't read:
// a state, a parameter, a prop, a let that computes, a file's state or
// memo; "" otherwise.
func (c *Checker) stateName(obj Object) string {
	switch o := obj.(type) {
	case *Member:
		switch o.Kind {
		case StateMember, ParamMember, PropMember:
			return o.name
		case LetMember:
			if o.Memo || o.Default != nil && c.reactiveOnce(o) {
				return o.name
			}
		}
	case *Global:
		if o.Kind == StateGlobal || o.Kind == MemoGlobal {
			return o.name
		}
	}
	return ""
}

// orderedUses is s in source order: the first use is what's reported.
func orderedUses(s stateSet) []stateUse {
	out := make([]stateUse, 0, len(s))
	for obj, at := range s {
		out = append(out, stateUse{obj, at})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].at.Range().Start < out[j].at.Range().Start })
	return out
}
