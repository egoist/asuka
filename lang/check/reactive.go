package check

import "github.com/egoist/asuka/lang/syntax"

// The reactive analysis: which expressions read what the UI follows
// (state, memos, a component's parameters and props, a row's item), so
// that a binding of one is a thunk the runtime re-runs, and a binding of
// anything else is a value computed once.

// finalize runs once every body is checked: it spreads "reads state"
// through calls, decides which lets are memos, and checks what spawn's
// blocks call.
func (c *Checker) finalize() {
	var ctxs []*FuncCtx
	seen := map[*FuncCtx]bool{}
	addCtx := func(x *FuncCtx) {
		if x != nil && !seen[x] {
			seen[x] = true
			ctxs = append(ctxs, x)
		}
	}
	for _, x := range c.info.Ctxs {
		addCtx(x)
	}
	for _, p := range c.pkgs {
		for _, f := range p.Funcs {
			addCtx(f.Ctx)
		}
		for _, g := range p.Globals {
			addCtx(g.Ctx)
		}
		for _, t := range p.Types {
			for _, f := range t.Methods {
				addCtx(f.Ctx)
			}
		}
		for _, t := range p.Tests {
			addCtx(t.Ctx)
		}
	}
	for changed := true; changed; {
		changed = false
		for _, x := range ctxs {
			if x.readsState {
				continue
			}
			for _, f := range x.calls {
				if f.Ctx != nil && f.Ctx.readsState {
					x.readsState = true
				}
			}
			for _, g := range x.globals {
				if g.Kind == StateGlobal || g.Ctx != nil && g.Ctx.readsState {
					x.readsState = true
				}
			}
			if x.readsState {
				changed = true
			}
		}
	}
	for _, p := range c.pkgs {
		for _, g := range p.Globals {
			if g.Kind == ConstGlobal && g.Owner == nil && g.Ctx != nil && g.Ctx.readsState {
				g.Kind = MemoGlobal
			}
		}
		for _, t := range p.Types {
			if t.Comp == nil {
				continue
			}
			for _, m := range t.Comp.Order {
				if m.Kind == LetMember && m.Default != nil {
					// A generic store's let of its type parameters is a
					// memo: its env holds it as a reference whatever T is.
					m.Memo = c.reactive(m.Default) || len(t.TypeParams) > 0 && HasTypeParams(m.Type)
				}
			}
		}
	}
	for _, s := range c.spawns {
		for _, f := range s.calls {
			if f.Ctx != nil && f.Ctx.readsState {
				c.errorf(s.Node, "spawn calls %s, which reads state: spawn runs on another thread; read the state first and pass its value", f.name)
			}
		}
	}
}

// Reactive reports whether e reads anything reactive.
func (p *Program) Reactive(e syntax.Expr) bool { return p.checker.reactive(e) }

func (c *Checker) reactive(e syntax.Expr) bool {
	if e == nil {
		return false
	}
	found := false
	syntax.Inspect(e, func(n syntax.Node) bool {
		if found {
			return false
		}
		switch n := n.(type) {
		case *syntax.Ident:
			switch o := c.info.Uses[n].(type) {
			case *Member:
				switch o.Kind {
				case StateMember, ParamMember, PropMember:
					found = true
				case LetMember:
					if o.Memo || c.resolving[o] || o.Default != nil && o.Default != e && c.reactiveOnce(o) {
						found = true
					}
				case FnMember:
					if o.Fn.Ctx == nil || o.Fn.Ctx.readsState {
						found = true
					}
				}
			case *Global:
				if o.Kind == StateGlobal || o.Kind == MemoGlobal || o.Ctx != nil && o.Ctx.readsState {
					found = true
				}
			case *Var:
				if o.Kind == RowItem || o.Kind == ViewMemo {
					found = true
				}
			}
		case *syntax.MemberExpr:
			if sel := c.info.Selections[n]; sel != nil {
				switch sel.Kind {
				case SelStoreMember, SelLoading, SelVariant:
					found = true
				case SelBuiltinField:
					if sel.Method.Recv == "ref" {
						found = true // a view's interaction state
					}
				case SelPkg:
					if g, ok := sel.Obj.(*Global); ok && (g.Kind != ConstGlobal || g.Ctx != nil && g.Ctx.readsState) {
						found = true
					}
				}
			}
		case *syntax.CallExpr:
			if call := c.info.Calls[n]; call != nil {
				switch call.Kind {
				case CallFunc, CallMethod:
					if call.Func.Ctx == nil || call.Func.Ctx.readsState || call.Func.Recv != nil && call.Func.Recv.Kind == StoreType {
						found = true
					}
				case CallClosure, CallIface, CallNative, CallTr:
					found = true // a message reads the language shown, a signal
				case CallBuiltinMethod:
					if call.Method.Recv == "loading" {
						found = true
					}
				case CallBuiltin:
					if call.Builtin.ID == BEnvironment {
						found = true // it reads the root's environment, a signal
					}
					if call.Builtin.ID == BRuntime && call.Builtin.Fn.Tracked {
						found = true
					}
					if call.Builtin.ID == BRuntime && call.Builtin.Fn.Once {
						return false // its closure's reads are the closure's
					}
				}
			}
		case *syntax.UnaryExpr:
			if n.Op == syntax.LOAD {
				found = true
			}
		}
		return !found
	})
	return found
}

// reactiveOnce decides a let member's reactivity (before finalize has).
func (c *Checker) reactiveOnce(m *Member) bool {
	if c.resolving[m] {
		return true
	}
	c.resolving[m] = true
	r := c.reactive(m.Default) || len(m.Owner.TypeParams) > 0 && m.Type != nil && HasTypeParams(m.Type)
	delete(c.resolving, m)
	m.Memo = r
	return r
}
