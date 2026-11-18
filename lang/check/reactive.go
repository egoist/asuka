package check

import (
	"strings"

	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

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
			if g.Kind == StateGlobal && g.Value != nil {
				g.Follows = c.follows(g.Decl, g.Value)
			}
		}
		for _, t := range p.Types {
			if t.Comp == nil || t.Comp.Worker {
				// (A worker's lets are values its thread makes.)
				continue
			}
			for _, m := range t.Comp.Order {
				if m.Kind == StateMember && m.Default != nil {
					m.Follows = c.follows(m.Decl, m.Default)
				}
				if m.Kind == LetMember && m.Default != nil {
					// A generic store's let of its type parameters is a
					// memo: its env holds it as a reference whatever T is.
					m.Memo = c.reactive(m.Default) || len(t.TypeParams) > 0 && HasTypeParams(m.Type)
					// One whose value is a path is that path (rfcs/0013):
					// an accessor of it, not a memo.
					m.Path = m.Memo && len(t.TypeParams) == 0 && c.letPath(t, m)
				}
			}
		}
	}
	c.workerCalls()
	c.checkEffectWrites()
	c.checkCleanupReads()
	c.checkKeys()
	touches := c.touchesState(ctxs)
	for _, s := range c.spawns {
		for _, f := range append(s.calls, s.peekCalls...) {
			if f.Ctx != nil && touches[f.Ctx] {
				c.errorf(s.Node, diag.SpawnCapturesState, "spawn calls %s, which reads state: spawn runs on another thread; read the state first and pass its value", f.name)
			}
		}
	}
}

// touchesState is which contexts read state, followed or under peek,
// themselves or through what they call: spawn can't call those.
func (c *Checker) touchesState(ctxs []*FuncCtx) map[*FuncCtx]bool {
	t := map[*FuncCtx]bool{}
	for _, x := range ctxs {
		if x.readsState || x.peeksState {
			t[x] = true
		}
		for _, g := range append(x.globals, x.peekGlobals...) {
			if g.Kind == StateGlobal || g.Kind == MemoGlobal {
				t[x] = true
			}
		}
	}
	for changed := true; changed; {
		changed = false
		for _, x := range ctxs {
			if t[x] {
				continue
			}
			for _, f := range append(x.calls, x.peekCalls...) {
				if f.Ctx != nil && t[f.Ctx] {
					t[x] = true
					changed = true
					break
				}
			}
		}
	}
	return t
}

// follows reports whether a state follows: it says what (state x(a) =
// …), or its initializer reads what changes (state draft = title).
func (c *Checker) follows(decl syntax.Node, init syntax.Expr) bool {
	if d, ok := decl.(*syntax.StateDecl); ok && len(d.Deps) > 0 {
		return true
	}
	return c.reactive(init)
}

// letPath reports whether the let m of t is a path, read through an
// accessor of it rather than a memo: its value is a path (no
// conversion), whose indexes and keys read nothing that changes (the
// accessor keeps their values), from a state or a member declared
// before it (made by then), or a parameter or prop.
func (c *Checker) letPath(t *TypeName, m *Member) bool {
	if _, conv := c.info.Convs[m.Default]; conv {
		return false
	}
	p := c.path(m.Default)
	if p == nil || !c.staticPath(p) {
		return false
	}
	// What the root and the arguments read of t's must be made first.
	before := map[*Member]bool{}
	for _, o := range t.Comp.Order {
		if o == m {
			break
		}
		before[o] = true
	}
	ok := true
	syntax.Inspect(m.Default, func(n syntax.Node) bool {
		if id, isID := n.(*syntax.Ident); isID {
			if o, isM := c.info.Uses[id].(*Member); isM && o.Owner == t && o.Kind != ParamMember && o.Kind != PropMember && !before[o] {
				ok = false
			}
		}
		return ok
	})
	return ok
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
			if c.isPeek(n) {
				return false // what it reads isn't followed
			}
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

// peek checks peek(x): x, read without following it. What x reads isn't
// the context's reads: a binding of it alone is a value computed once,
// and a fn that reads state only under peek doesn't make the binding that
// calls it follow it. It's state all the same (peeksState), which spawn
// can't read.
func (c *Checker) peek(e *syntax.CallExpr, x syntax.Expr, want Type) Type {
	if why := c.notFollowedHere(); why != "" {
		c.errorf(e, diag.PeekUntracked, "%s reads aren't followed: read %s", why, exprText(x))
	}
	ctx := c.ctx
	if ctx == nil {
		return c.expr(x, want)
	}
	reads, calls, globals := ctx.readsState, len(ctx.calls), len(ctx.globals)
	ctx.peeks++
	t := c.expr(x, want)
	ctx.peeks--
	if ctx.readsState && !reads {
		ctx.readsState = false
		ctx.peeksState = true
	}
	ctx.peekCalls = append(ctx.peekCalls, ctx.calls[calls:]...)
	ctx.calls = ctx.calls[:calls]
	ctx.peekGlobals = append(ctx.peekGlobals, ctx.globals[globals:]...)
	ctx.globals = ctx.globals[:globals]
	return t
}

// notFollowedHere says, when what's checked runs untracked (a handler,
// a task, an effect(…)'s body, a test, spawn), whose reads those are: a
// peek there is a no-op, and an error. A closure is its maker's: a lambda
// in a handler runs in it; a fn, a binding, a memo or a builder follows
// what it reads.
func (c *Checker) notFollowedHere() string {
	for cc := c.ctx; cc != nil; cc = cc.Parent {
		switch cc.Kind {
		case CtxLambda:
			continue
		case CtxHandler:
			return "a handler's"
		case CtxTask:
			return "a task's"
		case CtxEffect:
			if cc.Untracked {
				return "an effect(…)'s body's"
			}
		case CtxTest:
			return "a test's"
		case CtxCleanup:
			return "a cleanup's"
		case CtxSpawn:
			return "spawn's"
		}
		return ""
	}
	return ""
}

// exprText is x as it's written, on one line.
func exprText(x syntax.Expr) string {
	var b strings.Builder
	syntax.Fprint(&b, nil, x)
	return b.String()
}

// isPeek reports whether n is a call of peek.
func (c *Checker) isPeek(n syntax.Node) bool {
	ce, ok := n.(*syntax.CallExpr)
	if !ok {
		return false
	}
	call := c.info.Calls[ce]
	return call != nil && call.Kind == CallBuiltin && call.Builtin.ID == BPeek
}

// stateSelf declares, in a state's initializer, the state's own name: its
// current value, as an optional (none the first time; a state whose type
// is optional already is that type), not followed. Without a written
// type it isn't declared: the name is the state, which reads itself, an
// error.
func (c *Checker) stateSelf(name string, pos syntax.Pos, t Type) *Var {
	if t == nil || c.scope == nil {
		return nil
	}
	st := t
	if _, ok := t.(*Optional); !ok && !IsInvalid(t) {
		st = &Optional{Elem: t}
	}
	v := &Var{object: object{name, pos}, Type: st, Kind: StateSelf, Ctx: c.ctx}
	c.scope.Insert(v)
	return v
}

// selfUsed is self when init reads it, else nil.
func (c *Checker) selfUsed(self *Var, init syntax.Expr) *Var {
	if self == nil || init == nil {
		return nil
	}
	used := false
	syntax.Inspect(init, func(n syntax.Node) bool {
		if id, ok := n.(*syntax.Ident); ok && c.info.Uses[id] == self {
			used = true
		}
		return !used
	})
	if used {
		return self
	}
	return nil
}
