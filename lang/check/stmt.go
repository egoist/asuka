package check

import (
	"strings"

	"github.com/egoist/asuka/lang/syntax"
)

func (c *Checker) stmts(items []syntax.Item) {
	for i := 0; i < len(items); i++ {
		if _, ok := items[i].(*syntax.FuncDecl); ok {
			// The fns declared one after another: a group.
			j := i + 1
			for j < len(items) {
				if _, ok := items[j].(*syntax.FuncDecl); !ok {
					break
				}
				j++
			}
			c.localFns(items[i:j])
			i = j - 1
			continue
		}
		c.stmt(items[i])
	}
}

// block checks a block of statements in a scope of its own.
func (c *Checker) block(b *syntax.Block) {
	if b == nil {
		return
	}
	c.openScope()
	c.stmts(b.Items)
	c.closeScope()
}

func (c *Checker) stmt(it syntax.Item) {
	switch s := it.(type) {
	case *syntax.LetDecl:
		c.letStmt(s)
	case *syntax.AssignStmt:
		c.assignStmt(s)
	case *syntax.IfStmt:
		c.openScope()
		c.cond(s.Cond)
		c.block(s.Then)
		c.closeScope()
		switch el := s.Else.(type) {
		case *syntax.IfStmt:
			c.stmt(el)
		case *syntax.Block:
			c.block(el)
		}
	case *syntax.ForStmt:
		c.forStmt(s, false)
	case *syntax.WhileStmt:
		c.openScope()
		c.cond(s.Cond)
		c.loops++
		c.block(s.Body)
		c.loops--
		c.closeScope()
	case *syntax.MatchStmt:
		c.matchStmt(s)
	case *syntax.DoStmt:
		c.doStmt(s)
	case *syntax.ReturnStmt:
		c.returnStmt(s)
	case *syntax.ThrowStmt:
		c.throwValue(s, s.Value)
	case *syntax.BranchStmt:
		c.branch(s, s.Tok)
	case *syntax.DeferStmt:
		if c.inDefer > 0 {
			c.errorf(s, "a defer inside a defer: put the code in the first one")
		}
		c.inDefer++
		savedLoops := c.loops
		c.loops = 0
		c.block(s.Body)
		c.loops = savedLoops
		c.inDefer--
	case *syntax.EmitStmt:
		c.emitStmt(s)
	case *syntax.ExprStmt:
		if ce, ok := s.X.(*syntax.CatchExpr); ok {
			c.catchStmt(ce)
			return
		}
		t := c.expr(s.X, nil)
		if t == View {
			if call, ok := s.X.(*syntax.CallExpr); ok && c.info.Elements[call] != nil {
				c.errorf(s, "a view here does nothing: views go in a component's body")
			}
		}
	case *syntax.SlotItem:
		// animate { … }: a function's call with a trailing block.
		if _, ok := c.lookup(s.Name.Name).(*Builtin); ok {
			call := &syntax.CallExpr{Span: s.Span, Fun: s.Name, Block: s.Body}
			c.info.SlotCalls[s] = call
			c.expr(call, nil)
			return
		}
		c.errorf(it, "%s isn't a statement: %s", itemName(it), notStatementHint(it))
	case *syntax.BadItem:
	case *syntax.FuncDecl:
		c.errorf(s, "a fn here goes in a block of statements: put it in braces, or use a closure, let %s = (…) => …", s.Name.Name)
	default:
		c.errorf(it, "%s isn't a statement: %s", itemName(it), notStatementHint(it))
	}
}

func itemName(it syntax.Item) string {
	switch it := it.(type) {
	case *syntax.PropDecl:
		return "prop"
	case *syntax.EventDecl:
		return "event"
	case *syntax.StateDecl:
		return "state"
	case *syntax.UseDecl:
		return "use"
	case *syntax.ProvideDecl:
		return "provide"
	case *syntax.TaskDecl:
		return "task"
	case *syntax.EffectDecl:
		return "effect"
	case *syntax.StyleDecl:
		return "a style"
	case *syntax.PropSet:
		return it.Name.Name + ": …"
	case *syntax.OnHandler:
		return "on " + it.Name.Name
	case *syntax.BindItem:
		return "bind"
	case *syntax.StyleUse:
		return "style"
	case *syntax.SlotItem:
		return it.Name.Name + " { … }"
	}
	return "this"
}

func notStatementHint(it syntax.Item) string {
	switch it.(type) {
	case *syntax.StateDecl:
		return "state belongs to a component, a store or a file; here a value that changes is var"
	case *syntax.PropSet, *syntax.OnHandler, *syntax.BindItem, *syntax.StyleUse, *syntax.SlotItem:
		return "it goes in an element's block"
	}
	return "it goes in a component's or a store's body"
}

func (c *Checker) letStmt(d *syntax.LetDecl) {
	var t Type
	if d.Value == nil {
		c.errorf(d, "a let needs a value")
		return
	}
	if d.Else != nil {
		c.letElse(d)
		return
	}
	if d.Type != nil {
		want := c.typeExpr(d.Type)
		t = c.assign(d.Value, want, "the value")
	} else {
		t = c.valueType(c.expr(d.Value, nil), d.Value)
	}
	c.info.Lets[d] = t
	c.bindIrrefutable(d.Pattern, t, d.Keyword == syntax.VAR)
	if fx := c.inferredFx[t]; fx != nil && fx.letName == "" {
		if bp, ok := d.Pattern.(*syntax.BindPat); ok && unparen(d.Value) == syntax.Expr(fx.lambda) {
			fx.letName = d.Keyword.String() + " " + bp.Name.Name
		}
	}
}

// bindIrrefutable binds a let's or a for's pattern: names, _ and
// tuples of them.
func (c *Checker) bindIrrefutable(p syntax.Pattern, t Type, mutable bool) {
	switch p := p.(type) {
	case *syntax.BindPat:
		c.declareVar(p.Name, t, mutable)
	case *syntax.WildcardPat:
	case *syntax.TuplePat:
		tt, ok := t.(*Tuple)
		if !ok || len(tt.Elems) != len(p.Elems) {
			if !IsInvalid(t) {
				c.errorf(p, "a %s isn't a tuple of %d", typeString(t), len(p.Elems))
			}
			for _, e := range p.Elems {
				c.bindIrrefutable(e, Invalid, mutable)
			}
			return
		}
		for i, e := range p.Elems {
			c.bindIrrefutable(e, tt.Elems[i], mutable)
		}
	default:
		c.errorf(p, "a let binds names (let x = …, let (a, b) = …); to match, use if let or match")
	}
}

func (c *Checker) assignStmt(s *syntax.AssignStmt) {
	lt := c.lvalue(s.Target, "assign to")
	if ix, ok := unparen(s.Target).(*syntax.IndexExpr); ok {
		if _, isMap := c.info.Types[ix.X].(*Map); isMap {
			// m[k] reads a V?, and stores a V.
			switch s.Op {
			case syntax.ASSIGN, syntax.COALESCE_ASSIGN:
				c.assign(s.Value, Unopt(lt), "the value")
			default:
				c.expr(s.Value, nil)
				c.errorf(s, "m[k] may be none: write m[k] = (m[k] ?? 0) %s …", strings.TrimSuffix(s.Op.String(), "="))
			}
			return
		}
	}
	switch s.Op {
	case syntax.ASSIGN:
		c.assign(s.Value, lt, "the value")
		c.selfThroughVar(s)
	case syntax.COALESCE_ASSIGN:
		if !IsOptional(lt) && !IsInvalid(lt) {
			c.errorf(s.Target, "%s isn't optional: ??= has nothing to do", typeString(lt))
		}
		c.assign(s.Value, lt, "the value")
	default:
		want := numOnly(lt)
		if s.Op.AssignOp().IsBitwise() {
			want = Int
		}
		vt := c.expr(s.Value, want)
		if IsInvalid(lt) || IsInvalid(vt) {
			return
		}
		op := strings.TrimSuffix(s.Op.String(), "=")
		switch {
		case s.Op.AssignOp().IsBitwise():
			if lt != Int {
				c.errorf(s.Target, "%s= takes ints, not a %s", op, typeString(lt))
			} else if vt != Int {
				c.errorf(s.Value, "%s= takes ints, not a %s", op, typeString(vt))
			}
		case IsNumeric(lt) && Identical(lt, vt):
		case lt == String && vt == String && s.Op == syntax.ADD_ASSIGN:
		case lt == Duration && vt == Duration && (s.Op == syntax.ADD_ASSIGN || s.Op == syntax.SUB_ASSIGN):
		case lt == Duration && vt == Int && (s.Op == syntax.MUL_ASSIGN || s.Op == syntax.QUO_ASSIGN):
		default:
			if _, ok := lt.(*Array); ok && Identical(lt, vt) && s.Op == syntax.ADD_ASSIGN {
				return
			}
			c.errorf(s, "%s= doesn't take a %s and a %s", op, typeString(lt), typeString(vt))
		}
	}
}

// lvalue checks x as something assigned to (or changed in place) and
// returns its type.
func (c *Checker) lvalue(x syntax.Expr, what string) Type {
	t := c.expr(x, nil)
	if c.ctx != nil {
		switch c.ctx.Kind {
		case CtxBinding, CtxGlobal, CtxBuilder, CtxComponent, CtxStore, CtxApp:
			c.errorf(x, "can't %s %s here: what the UI follows is computed, not changed; change state in a handler (on press { … }), a fn, a task or an effect", what, exprName(x))
			return t
		}
	}
	c.checkLvalue(x, what)
	return t
}

func (c *Checker) checkLvalue(x syntax.Expr, what string) {
	switch x := x.(type) {
	case *syntax.ParenExpr:
		c.checkLvalue(x.X, what)
	case *syntax.Ident:
		switch o := c.info.Uses[x].(type) {
		case *Var:
			if o.Kind != LocalVar || !o.Mutable {
				hint := "it's a let: make it a var"
				if o.Kind == RowItem {
					hint = "it's a row's item: change the state it comes from"
				} else if o.Kind == SelfVar {
					hint = "self is a value"
				} else if o.Kind == LocalFnVar {
					hint = "it's a fn: a var holds a closure that changes"
				} else if o.Ctx != nil && o.Ctx.Kind == CtxLambda && contains(o.Ctx.Params, o) || isParam(o) {
					hint = "parameters are values: copy it into a var"
				}
				c.errorf(x, "can't %s %s: %s", what, x.Name, hint)
				return
			}
			o.Assigned = true
			if o.Ctx != c.ctx {
				o.AssignedInClosure = true
				for cc := c.ctx; cc != nil; cc = cc.Parent {
					if cc.Kind == CtxSpawn {
						c.errorf(x, "spawn can't change %s: it runs on another thread; return a value instead", x.Name)
						break
					}
				}
			}
		case *Member:
			switch o.Kind {
			case StateMember:
			case PropMember, ParamMember:
				c.errorf(x, "can't %s %s: it's the caller's (a prop); keep state of your own, or emit an event", what, x.Name)
			case LetMember:
				c.errorf(x, "can't %s %s: it's derived (a let); change what it reads", what, x.Name)
			default:
				c.errorf(x, "can't %s %s", what, x.Name)
			}
		case *Global:
			if o.Kind != StateGlobal {
				c.errorf(x, "can't %s %s: it's a let; a value that changes is state", what, x.Name)
			}
		case *selfMember:
			c.errorf(x, "can't %s %s: a struct's methods can't change it (it's a value); return a new one", what, x.Name)
		case nil:
		default:
			c.errorf(x, "can't %s %s", what, x.Name)
		}
	case *syntax.MemberExpr:
		sel := c.info.Selections[x]
		if sel == nil {
			return
		}
		if sel.Optional {
			c.errorf(x, "can't %s through ?.: unwrap it first (if let)", what)
			return
		}
		switch sel.Kind {
		case SelField:
			c.checkLvalue(x.X, what)
		case SelStoreMember:
			if sel.Member.Kind != StateMember {
				c.errorf(x, "can't %s %s: it isn't state", what, x.Name.Name)
			}
		case SelPkg:
			if g, ok := sel.Obj.(*Global); !ok || g.Kind != StateGlobal {
				c.errorf(x, "can't %s %s: only a package's state changes", what, exprName(x))
			}
		default:
			c.errorf(x, "can't %s %s", what, exprName(x))
		}
	case *syntax.IndexExpr:
		if c.info.Types[x.X] == Bytes {
			c.errorf(x, "can't %s a byte: bytes don't change in place; make new ones (bytes([…]) of ints, b.slice, b.concat)", what)
			return
		}
		c.checkLvalue(x.X, what)
	case *syntax.SelfExpr:
		c.errorf(x, "can't %s self: a struct's methods can't change it (it's a value); return a new one", what)
	default:
		c.errorf(x, "can't %s this: only vars, state, and their fields and elements", what)
	}
}

func isParam(v *Var) bool {
	if v.Ctx == nil {
		return false
	}
	return contains(v.Ctx.Params, v)
}

// cond checks an if's or a while's condition: a bool, let pattern =
// value, or a chain of them; what a let binds is declared in the current
// scope, for the clauses after it and the body.
func (c *Checker) cond(e syntax.Expr) {
	if l, ok := e.(*syntax.CondList); ok {
		for _, x := range l.List {
			c.cond(x)
		}
		return
	}
	lc, ok := e.(*syntax.LetCond)
	if !ok {
		c.assign(e, Bool, "the condition")
		return
	}
	t := c.expr(lc.Value, nil)
	c.letCondPattern(lc, t, LocalVar)
}

func (c *Checker) letCondPattern(lc *syntax.LetCond, t Type, kind VarKind) {
	if bp, ok := lc.Pattern.(*syntax.BindPat); ok {
		if !IsOptional(t) && !IsInvalid(t) {
			c.errorf(lc.Value, "%s isn't optional: if let unwraps an optional (use let)", typeString(t))
		}
		v := c.declareVar(bp.Name, Unopt(t), false)
		v.Kind = kind
		return
	}
	c.pattern(lc.Pattern, t, kind)
}

func (c *Checker) forStmt(s *syntax.ForStmt, view bool) {
	fi := &ForInfo{Pat: s.Pattern, View: view}
	c.info.Fors[s] = fi
	if s.Await.IsValid() {
		c.forAwait(s, fi, view)
		return
	}
	seq := unparen(s.Seq)
	saved := c.ctx
	if view {
		c.enter(CtxBinding, s.Seq, "items")
	}
	switch {
	case isRange(seq):
		b := seq.(*syntax.BinaryExpr)
		c.assign(b.X, Int, "a range's start")
		c.assign(b.Y, Int, "a range's end")
		c.record(seq, &Array{Int})
		fi.Kind, fi.Seq, fi.End, fi.Incl, fi.Elem = ForRange, b.X, b.Y, b.Op == syntax.RANGE, Int
		if view {
			fi.Kind, fi.Seq = ForArray, seq
		}
	case isEnumerated(seq):
		call := seq.(*syntax.CallExpr)
		c.expr(call, nil)
		fi.Kind, fi.Seq = ForEnumerated, call.Fun.(*syntax.MemberExpr).X
		if a, ok := c.info.Types[fi.Seq].(*Array); ok {
			fi.Elem = a.Elem
		} else {
			fi.Elem = Invalid
		}
	default:
		t := c.expr(s.Seq, nil)
		fi.Seq = s.Seq
		switch u := t.(type) {
		case *Array:
			fi.Kind, fi.Elem = ForArray, u.Elem
		case *Map:
			fi.Kind, fi.Elem = ForMap, &Tuple{[]Type{u.Key, u.Elem}}
			if view {
				c.errorf(s.Seq, "a for in a view goes over an array: use the map's keys() or values()")
			}
		case *Set:
			fi.Kind, fi.Elem = ForSet, u.Elem
		default:
			if t == String {
				c.errorf(s.Seq, "a string isn't a list: go over its characters, s.chars()")
			} else if !IsInvalid(t) {
				c.errorf(s.Seq, "for goes over an array, a set, a map or a range, not a %s", typeString(t))
			}
			fi.Kind, fi.Elem = ForArray, Invalid
		}
	}
	if view {
		c.leave(saved)
	}
	// The key, in a view: a function of the item.
	if s.Key != nil {
		if !view {
			c.errorf(s.Key, "key is for a for in a view")
		}
		saved := c.ctx
		kc := c.enter(CtxLambda, s.Key, "key")
		kc.Result = nil
		c.forBindings(fi, s.Pattern, LocalVar, kc)
		kt := c.expr(s.Key, nil)
		if !IsInvalid(kt) && !IsHashable(kt) {
			c.errorf(s.Key, "a key is an int, a string or an enum without payloads, not a %s", typeString(kt))
		}
		kc.Result = kt
		c.leave(saved)
	}
	if view {
		saved := c.ctx
		row := c.enter(CtxBuilder, s.Body, "row")
		c.forBindings(fi, s.Pattern, RowItem, row)
		savedView := c.view
		c.view = true
		c.viewItems(s.Body.Items, nil)
		c.view = savedView
		c.leave(saved)
		return
	}
	c.openScope()
	c.forBindings(fi, s.Pattern, LocalVar, nil)
	c.loops++
	c.block(s.Body)
	c.loops--
	c.closeScope()
}

// forBindings declares a for's item (and index, or key and value).
func (c *Checker) forBindings(fi *ForInfo, p syntax.Pattern, kind VarKind, ctx *FuncCtx) {
	bind := func(p syntax.Pattern, t Type) *Var {
		switch p := p.(type) {
		case *syntax.BindPat:
			v := c.declareVar(p.Name, t, false)
			v.Kind = kind
			if ctx != nil {
				ctx.Params = append(ctx.Params, v)
			}
			return v
		case *syntax.WildcardPat:
			v := &Var{object: object{"_", p.Start}, Type: t, Kind: kind, Ctx: c.ctx}
			if ctx != nil {
				ctx.Params = append(ctx.Params, v)
			}
			return v
		case *syntax.TuplePat:
			if kind != LocalVar {
				c.errorf(p, "a row's item is one name: for x in xs")
				return nil
			}
			v := &Var{object: object{"_", p.Start}, Type: t, Ctx: c.ctx}
			c.bindIrrefutable(p, t, false)
			return v
		}
		c.errorf(p, "for binds a name, _ or a tuple of names")
		return nil
	}
	switch fi.Kind {
	case ForEnumerated, ForMap:
		tp, ok := p.(*syntax.TuplePat)
		if !ok || len(tp.Elems) != 2 {
			if fi.Kind == ForMap {
				c.errorf(p, "a map gives (key, value): for (k, v) in m")
			} else {
				c.errorf(p, "enumerated gives (index, item): for (i, x) in xs.enumerated()")
			}
			return
		}
		var kt Type = Int
		vt := fi.Elem
		if fi.Kind == ForMap {
			if tt, ok := fi.Elem.(*Tuple); ok {
				kt, vt = tt.Elems[0], tt.Elems[1]
			}
		}
		fi.Index = bind(tp.Elems[0], kt)
		fi.Item = bind(tp.Elems[1], vt)
		if ctx != nil && ctx.Kind == CtxBuilder && fi.Index != nil && fi.Item != nil {
			// The row closure takes (item, index).
			ctx.Params[len(ctx.Params)-2], ctx.Params[len(ctx.Params)-1] = ctx.Params[len(ctx.Params)-1], ctx.Params[len(ctx.Params)-2]
		}
	default:
		fi.Item = bind(p, fi.Elem)
	}
}

func isRange(e syntax.Expr) bool {
	b, ok := e.(*syntax.BinaryExpr)
	return ok && (b.Op == syntax.RANGE || b.Op == syntax.RANGELT)
}

func isEnumerated(e syntax.Expr) bool {
	call, ok := e.(*syntax.CallExpr)
	if !ok || argCount(call) != 0 {
		return false
	}
	m, ok := call.Fun.(*syntax.MemberExpr)
	return ok && m.Name.Name == "enumerated" && !m.Optional
}

func (c *Checker) returnStmt(s *syntax.ReturnStmt) { c.returnValue(s, s.Value) }

// returnValue checks a return (at s) of value (nil for none): a
// statement's or a JumpExpr's.
func (c *Checker) returnValue(s syntax.Node, value syntax.Expr) {
	if c.inDefer > 0 {
		c.errorf(s, "return in a defer: a defer runs as its block ends, and can't change how")
	}
	ctx := c.ctx
	if ctx == nil {
		return
	}
	switch ctx.Kind {
	case CtxComponent, CtxStore, CtxBinding, CtxBuilder, CtxGlobal, CtxApp:
		c.errorf(s, "return in a component's body: a body is declarations, not code")
		return
	}
	if ctx.inferRes {
		if value == nil {
			ctx.retTypes = append(ctx.retTypes, Void)
		} else {
			ctx.retTypes = append(ctx.retTypes, c.valueType(c.expr(value, nil), value))
		}
		return
	}
	res := ctx.Result
	if res == nil {
		res = Void
	}
	switch {
	case value == nil && res != Void:
		c.errorf(s, "return needs a %s", typeString(res))
	case value != nil && res == Void:
		c.expr(value, nil)
		c.errorf(value, "%s returns nothing: drop the value, or declare a result (-> T)", ctxLabel(ctx))
	case value != nil:
		c.assign(value, res, "the result")
	}
}

func ctxLabel(ctx *FuncCtx) string {
	switch ctx.Kind {
	case CtxFunc, CtxMethod, CtxLocalFn:
		return "fn " + ctx.Name
	case CtxHandler:
		return "a handler"
	case CtxTask:
		return "a task"
	case CtxEffect:
		return "an effect"
	case CtxTest:
		return "a test"
	}
	return "this closure"
}

func (c *Checker) emitStmt(s *syntax.EmitStmt) {
	comp := c.ctx.Comp
	if comp == nil || comp.Kind != ComponentType {
		c.errorf(s, "emit raises a component's event, and this isn't in a component")
		c.emitArgs(s, nil)
		return
	}
	m := comp.Comp.Members[s.Name.Name]
	if m == nil || m.Kind != EventMember {
		c.errorf(s.Name, "%s has no event %s: declare it, event %s", comp.name, s.Name.Name, s.Name.Name)
		c.emitArgs(s, nil)
		return
	}
	c.envUse(comp, s, m.name)
	c.info.Emits[s] = m
	c.emitArgs(s, m.Params)
}

func (c *Checker) emitArgs(s *syntax.EmitStmt, params []*Param) {
	var list []*syntax.Arg
	if s.Args != nil {
		list = s.Args.List
	}
	if params != nil && len(list) != len(params) {
		c.errorf(s, "event %s takes %s", s.Name.Name, plural(len(params), "argument", "arguments"))
	}
	for i, a := range list {
		if params != nil && i < len(params) {
			c.assign(a.Value, params[i].Type, "argument "+params[i].Name)
		} else {
			c.expr(a.Value, nil)
		}
	}
}

// ---------------------------------------------------------------------
// do/catch

func (c *Checker) doStmt(s *syntax.DoStmt) {
	if len(s.Catches) == 0 {
		c.errorf(s, "do needs a catch")
	}
	c.catching(s.Catches, func() { c.block(s.Body) })
	for i, cl := range s.Catches {
		if i < len(s.Catches)-1 && c.info.Catches[cl] != nil && c.info.Catches[cl].All {
			c.errorf(s.Catches[i+1], "this catch is never reached: the one before catches everything")
		}
		c.openScope()
		c.catchClause(cl)
		c.block(cl.Body)
		c.closeScope()
	}
}

func (c *Checker) catchAll(cl *syntax.CatchClause) bool {
	if cl.Pattern == nil {
		return true
	}
	if _, ok := cl.Pattern.(*syntax.BindPat); ok {
		if cl.Type == nil {
			return true
		}
		if nt, ok := cl.Type.(*syntax.NamedType); ok && len(nt.Path) == 1 && nt.Path[0].Name == "Error" {
			return true
		}
	}
	if _, ok := cl.Pattern.(*syntax.WildcardPat); ok {
		return true
	}
	return false
}

func (c *Checker) catchClause(cl *syntax.CatchClause) {
	ci := &CatchInfo{}
	c.info.Catches[cl] = ci
	switch p := cl.Pattern.(type) {
	case nil, *syntax.WildcardPat:
		ci.All = true
	case *syntax.BindPat:
		t := c.errT.Type
		if cl.Type != nil {
			t = c.typeExpr(cl.Type)
			if n, ok := t.(*Named); ok && n.Obj == c.errT {
				ci.All = true
			} else if n, ok := t.(*Named); ok && n.Obj.IsError {
				ci.Type = n.Obj
			} else if !IsInvalid(t) {
				c.errorf(cl.Type, "%s isn't an error type: catch e: T takes a type declared : Error", typeString(t))
			}
		} else {
			ci.All = true
		}
		ci.Var = c.declareVar(p.Name, t, false)
	case *syntax.CasePat:
		if p.Implicit || len(p.Path) < 2 {
			c.errorf(p, "write the error's type: catch MailError.offline")
			return
		}
		tn := c.casePathType(p.Path[:len(p.Path)-1])
		if tn == nil {
			return
		}
		if !tn.IsError {
			c.errorf(p, "%s isn't an error type (declare it: enum %s: Error)", tn.name, tn.name)
			return
		}
		k := tn.Case(p.Path[len(p.Path)-1].Name)
		if k == nil {
			c.errorf(p, "%s has no case %s", tn.name, p.Path[len(p.Path)-1].Name)
			return
		}
		ci.Type, ci.Case = tn, k
		c.info.Cases[p] = k
		c.caseArgs(p, k, tn.Type)
	default:
		c.errorf(cl.Pattern, "catch takes a name (catch e), a typed one (catch e: T) or a case (catch T.case)")
	}
}

// casePathType resolves the type part of a qualified case.
func (c *Checker) casePathType(path []*syntax.Ident) *TypeName {
	var o Object
	if len(path) == 1 {
		o = c.lookup(path[0].Name)
	} else if len(path) == 2 {
		if pn, ok := c.lookup(path[0].Name).(*PkgName); ok {
			o = pn.Pkg.Scope.LookupLocal(path[1].Name)
		}
	}
	tn, ok := o.(*TypeName)
	if !ok || tn.Kind != EnumType && tn.Kind != StructType {
		c.errorf(path[0], "%s isn't an enum", joinPath(path))
		return nil
	}
	c.info.Uses[path[len(path)-1]] = tn
	return tn
}

// ---------------------------------------------------------------------
// Patterns

// pattern checks a (refutable) pattern against values of type t,
// declaring its names.
func (c *Checker) pattern(p syntax.Pattern, t Type, kind VarKind) {
	switch p := p.(type) {
	case *syntax.WildcardPat, *syntax.BadPat:
	case *syntax.BindPat:
		v := c.declareVar(p.Name, t, false)
		v.Kind = kind
	case *syntax.LitPat:
		// A literal is compared with an optional's value, once it isn't
		// none: it's of the value's type (not converted to the optional).
		et := t
		if _, none := p.Value.(*syntax.NoneLit); !none {
			et = Unopt(t)
		}
		lt := c.expr(p.Value, et)
		if !IsInvalid(lt) && !IsInvalid(t) && !c.convert(p.Value, lt, et) && !(lt == NoneType && IsOptional(t)) {
			c.errorf(p, "a %s can't be a %s", typeString(t), typeString(lt))
		}
	case *syntax.TuplePat:
		tt, ok := t.(*Tuple)
		if !ok || len(tt.Elems) != len(p.Elems) {
			if !IsInvalid(t) {
				c.errorf(p, "a %s isn't a tuple of %d", typeString(t), len(p.Elems))
			}
			for _, e := range p.Elems {
				c.pattern(e, Invalid, kind)
			}
			return
		}
		for i, e := range p.Elems {
			c.pattern(e, tt.Elems[i], kind)
		}
	case *syntax.OrPat:
		for _, a := range p.Alts {
			before := len(c.scope.names)
			c.pattern(a, t, kind)
			if len(c.scope.names) != before {
				c.errorf(a, "a | pattern can't bind names: match the cases in arms of their own")
			}
		}
	case *syntax.CasePat:
		if st := c.patternStruct(p); st != nil {
			c.structPattern(p, st, t)
			return
		}
		et := Unopt(t)
		var tn *TypeName
		if p.Implicit {
			n, ok := et.(*Named)
			if !ok || n.Obj.Kind != EnumType {
				if !IsInvalid(t) {
					c.errorf(p, ".%s: a %s isn't an enum", p.Path[0].Name, typeString(t))
				}
				return
			}
			tn = n.Obj
		} else if len(p.Path) < 2 {
			name := "case"
			if len(p.Path) == 1 {
				name = p.Path[0].Name
			}
			c.errorf(p, "%s(…) isn't a case: write .%s(…), or Type.%s(…)", name, name, name)
			return
		} else {
			tn = c.casePathType(p.Path[:len(p.Path)-1])
			if tn == nil {
				return
			}
			if n, ok := et.(*Named); ok && n.Obj == c.errT && tn.IsError {
				// An Error, matched by an error enum's case.
				name := p.Path[len(p.Path)-1].Name
				k := tn.Case(name)
				if k == nil {
					c.errorf(p, "%s has no case %s", tn.name, name)
					return
				}
				c.info.Cases[p] = k
				c.info.ErrorCases[p] = tn
				c.caseArgs(p, k, tn.Type)
				return
			}
			if n, ok := et.(*Named); !ok || n.Obj != tn {
				if !IsInvalid(t) {
					c.errorf(p, "a %s can't be a %s", typeString(t), tn.name)
				}
				return
			}
		}
		name := p.Path[len(p.Path)-1].Name
		k := tn.Case(name)
		if k == nil {
			c.errorf(p, "%s has no case %s", tn.name, name)
			return
		}
		c.info.Cases[p] = k
		c.caseArgs(p, k, et)
	}
}

// caseArgs checks a case pattern's payload patterns.
func (c *Checker) caseArgs(p *syntax.CasePat, k *Case, t Type) {
	if !p.Lparen.IsValid() {
		return
	}
	if len(p.Args) != len(k.Params) {
		c.errorf(p, "case %s has %s", k.Name, plural(len(k.Params), "payload value", "payload values"))
		return
	}
	for i := range p.Args {
		// A label names the payload's: positions still say which.
		if l := p.Label(i); l != nil && l.Name != k.Labels[i] {
			if k.Labels[i] == "" {
				c.errorf(l, "case %s's payload %d has no label: leave it out", k.Name, i+1)
			} else {
				c.errorf(l, "case %s's payload %d is %s, not %s", k.Name, i+1, k.Labels[i], l.Name)
			}
		}
	}
	var s Subst
	if n, ok := t.(*Named); ok {
		s = n.Subst()
	}
	kind := LocalVar
	if c.view {
		kind = ViewMemo
	}
	for i, a := range p.Args {
		pt := s.Apply(k.Params[i])
		if pt == nil {
			pt = c.errT.Type
		}
		c.pattern(a, pt, kind)
	}
}

// irrefutable reports a pattern that matches anything of its type.
func irrefutable(p syntax.Pattern) bool {
	switch p := p.(type) {
	case *syntax.WildcardPat, *syntax.BindPat:
		return true
	case *syntax.TuplePat:
		for _, e := range p.Elems {
			if !irrefutable(e) {
				return false
			}
		}
		return true
	}
	return false
}

func (c *Checker) matchStmt(s *syntax.MatchStmt) {
	t := c.expr(s.Subject, nil)
	mi := &MatchInfo{Subject: t}
	c.info.Matches[s] = mi
	for _, a := range s.Arms {
		c.openScope()
		c.pattern(a.Pattern, t, LocalVar)
		if a.Guard != nil {
			c.assign(a.Guard, Bool, "the guard")
		}
		switch b := a.Body.(type) {
		case *syntax.Block:
			c.block(b)
		case syntax.Item:
			c.stmt(b)
		}
		c.closeScope()
	}
	ok, missing := c.exhaustive(s.Subject, t, s.Arms)
	mi.Exhaustive = ok
	if !ok {
		c.errorf(s, "match doesn't cover %s: add arms for them, or _ => { }", strings.Join(missing, ", "))
	}
}

func (c *Checker) matchExpr(e *syntax.MatchExpr, want Type) Type {
	t := c.expr(e.Subject, nil)
	mi := &MatchInfo{Subject: t}
	c.info.Matches[e] = mi
	var res Type
	for _, a := range e.Arms {
		c.openScope()
		c.pattern(a.Pattern, t, LocalVar)
		if a.Guard != nil {
			c.assign(a.Guard, Bool, "the guard")
		}
		var at Type
		switch b := a.Body.(type) {
		case *syntax.Block:
			at = c.valueBlock(b, want)
		case syntax.Expr:
			if want != nil {
				at = c.assign(b, want, "the value")
			} else {
				at = c.expr(b, nil)
			}
		}
		c.closeScope()
		if res == nil {
			res = at
		} else if j := c.join(res, at); j != nil {
			res = j
		} else {
			c.errorf(a, "this arm is a %s, the ones before a %s: make them the same type", typeString(at), typeString(res))
		}
	}
	ok, missing := c.exhaustive(e.Subject, t, e.Arms)
	mi.Exhaustive = ok
	if !ok {
		c.errorf(e, "match doesn't cover %s: add arms for them, or _ =>", strings.Join(missing, ", "))
	}
	if want != nil {
		return want
	}
	if res == nil {
		return Invalid
	}
	return res
}

// terminates reports whether control can't reach the end of items.
func (c *Checker) terminates(items []syntax.Item) bool {
	if len(items) == 0 {
		return false
	}
	switch s := items[len(items)-1].(type) {
	case *syntax.BranchStmt:
		// break and continue leave the block too (a loop's, which the
		// checker made sure is around).
		return true
	case *syntax.ExprStmt:
		// What never has a value: panic(…), x ?? throw E, a catch whose
		// value and fallbacks all leave.
		return c.info.Types[s.X] == Never
	case *syntax.ReturnStmt, *syntax.ThrowStmt:
		return true
	case *syntax.IfStmt:
		if s.Else == nil || !c.terminates(s.Then.Items) {
			return false
		}
		switch el := s.Else.(type) {
		case *syntax.IfStmt:
			return c.terminates([]syntax.Item{el})
		case *syntax.Block:
			return c.terminates(el.Items)
		}
	case *syntax.MatchStmt:
		if mi := c.info.Matches[s]; mi == nil || !mi.Exhaustive {
			return false
		}
		for _, a := range s.Arms {
			switch b := a.Body.(type) {
			case *syntax.Block:
				if !c.terminates(b.Items) {
					return false
				}
			case syntax.Item:
				if !c.terminates([]syntax.Item{b}) {
					return false
				}
			}
		}
		return true
	case *syntax.DoStmt:
		if !c.terminates(s.Body.Items) {
			return false
		}
		for _, cl := range s.Catches {
			if !c.terminates(cl.Body.Items) {
				return false
			}
		}
		return true
	case *syntax.WhileStmt:
		if b, ok := s.Cond.(*syntax.BoolLit); ok && b.Value && !hasBreak(s.Body.Items) {
			return true
		}
	}
	return false
}

func hasBreak(items []syntax.Item) bool {
	found := false
	for _, it := range items {
		syntax.Inspect(it, func(n syntax.Node) bool {
			switch n := n.(type) {
			case *syntax.ForStmt, *syntax.WhileStmt, *syntax.LambdaExpr:
				return false
			case *syntax.BranchStmt:
				if n.Tok == syntax.BREAK {
					found = true
				}
			}
			return !found
		})
	}
	return found
}

// forAwait is for await x in s: each value the stream sends, as it
// comes. It waits, and throws what the stream's producer throws.
func (c *Checker) forAwait(s *syntax.ForStmt, fi *ForInfo, view bool) {
	fi.Kind, fi.Seq = ForStream, s.Seq
	if view {
		c.errorf(s, "a view can't wait: read the stream in a task { }, into state")
	}
	if c.ctx == nil || !c.ctx.Async {
		c.errorf(s, "for await waits, and this can't: %s", c.asyncHint())
	} else {
		c.ctx.awaited(s.Start)
	}
	t := c.expr(s.Seq, nil)
	if st, ok := t.(*Stream); ok {
		fi.Elem = st.Elem
	} else {
		if !IsInvalid(t) {
			c.errorf(s.Seq, "for await goes over a stream, not a %s (for goes over arrays)", typeString(t))
		}
		fi.Elem = Invalid
	}
	if s.Key != nil {
		c.errorf(s.Key, "key is for a for in a view")
	}
	c.mayThrow(s.Seq)
	c.openScope()
	c.forBindings(fi, s.Pattern, LocalVar, nil)
	c.loops++
	c.block(s.Body)
	c.loops--
	c.closeScope()
}

// ---------------------------------------------------------------------
// let … else, jumps as values, catch as a value

// noLetElse reports a let … else where a let isn't a statement.
func (c *Checker) noLetElse(d *syntax.LetDecl) {
	if d.Else != nil {
		c.errorPos(d.Else.Lbrace, "let … else is a statement, in a fn, a handler, a task or a test; a let here is derived: use if let, or ?? for a default")
	}
}

// letElse checks let pattern = value else { … }: the else block, which
// must leave, then the pattern's names, declared for the rest of the
// block. A name unwraps an optional (as if let's); another pattern
// matches as match's.
func (c *Checker) letElse(d *syntax.LetDecl) {
	what := d.Keyword.String() + " … else"
	bp, name := d.Pattern.(*syntax.BindPat)
	var t Type
	if d.Type != nil {
		want := c.typeExpr(d.Type)
		if name {
			// let user: User = cache[id] else { … }: the type is the
			// unwrapped value's.
			t = c.assign(d.Value, MakeOptional(want), "the value")
		} else {
			t = c.assign(d.Value, want, "the value")
		}
	} else {
		t = c.valueType(c.expr(d.Value, nil), d.Value)
	}
	c.info.Lets[d] = t
	// The else block doesn't see the names.
	c.block(d.Else)
	if !c.terminates(d.Else.Items) {
		c.errorPos(d.Else.Rbrace, "the else of a %s goes on with nothing to bind: end it with return, throw, break or continue (or panic)", what)
	}
	if name {
		if !IsOptional(t) && !IsInvalid(t) {
			c.errorf(d.Value, "%s isn't optional: %s unwraps an optional or matches a pattern; this always has a value (drop the else)", typeString(t), what)
		}
		c.declareVar(bp.Name, Unopt(t), d.Keyword == syntax.VAR)
		return
	}
	if isIrrefutablePattern(d.Pattern) && !IsOptional(t) && !IsInvalid(t) {
		c.errorf(d.Pattern, "this pattern always matches a %s: drop the else", typeString(t))
	}
	if d.Keyword == syntax.VAR {
		c.errorf(d, "var … else binds a name: var x = optional else { … }; a pattern's names are lets")
	}
	c.pattern(d.Pattern, t, LocalVar)
}

// isIrrefutablePattern reports a pattern that matches every value of a
// non-optional type: names, _ and tuples of them.
func isIrrefutablePattern(p syntax.Pattern) bool {
	switch p := p.(type) {
	case *syntax.WildcardPat, *syntax.BindPat:
		return true
	case *syntax.TuplePat:
		for _, e := range p.Elems {
			if !isIrrefutablePattern(e) {
				return false
			}
		}
		return true
	}
	return false
}

// throwValue checks a throw (at s) of value.
func (c *Checker) throwValue(s syntax.Node, value syntax.Expr) {
	t := c.expr(value, c.errT.Type)
	if !IsInvalid(t) && !IsErrorType(t) {
		c.errorf(value, "throw takes an error, and a %s isn't one (declare it: struct %s: Error, or throw Error(\"…\"))", typeString(t), typeString(t))
	}
	c.mayThrow(s)
}

// branch checks a break or a continue.
func (c *Checker) branch(s syntax.Node, tok syntax.Token) {
	if c.loops == 0 {
		c.errorf(s, "%s outside a loop", tok)
	}
}

// jumpExpr checks throw, return, break or continue used as a value: it
// never has one.
func (c *Checker) jumpExpr(e *syntax.JumpExpr) Type {
	switch e.Tok {
	case syntax.THROW:
		c.throwValue(e, e.Value)
	case syntax.RETURN:
		c.returnValue(e, e.Value)
	default:
		c.branch(e, e.Tok)
	}
	return Never
}

// A partialCatch is a do or a catch whose clauses don't take every
// error, for mayThrow's message.
type partialCatch struct {
	ctx  *FuncCtx
	what string // the errors they take: MailError.offline, ParseError
	n    int
}

// catching checks body (a do's, a catch expression's value) under
// clauses: what they all take is handled; when they don't take
// everything, what's left goes on, so the function must throw.
func (c *Checker) catching(clauses []*syntax.CatchClause, body func()) {
	all := false
	var took []string
	for _, cl := range clauses {
		if c.catchAll(cl) {
			all = true
		}
		switch p := cl.Pattern.(type) {
		case *syntax.CasePat:
			took = append(took, joinPath(p.Path))
		case *syntax.BindPat:
			if nt, ok := cl.Type.(*syntax.NamedType); ok {
				took = append(took, joinPath(nt.Path))
			}
		}
	}
	if all {
		c.handled++
		body()
		c.handled--
		return
	}
	saved := c.partial
	what := strings.Join(took, ", ")
	if what == "" {
		what = "some errors"
	}
	c.partial = &partialCatch{ctx: c.ctx, what: what, n: len(clauses)}
	body()
	c.partial = saved
}

// cantThrow says that the function around doesn't throw.
func (c *Checker) cantThrow() string {
	switch c.ctx.Kind {
	case CtxFunc, CtxMethod, CtxLocalFn:
		return c.ctx.Name + " doesn't throw"
	case CtxLambda:
		return "this closure doesn't throw where it's passed"
	}
	return "this can't throw here"
}

func (c *Checker) throwsHint() string {
	switch c.ctx.Kind {
	case CtxFunc, CtxMethod, CtxLocalFn:
		return ", or mark " + c.ctx.Name + " throws"
	}
	return ""
}

// catchExpr checks x catch … { … }: x's value, or a clause's fallback
// (its block's last expression, or it leaves) when x throws. With no
// expected type, the fallbacks are x's type; a void x takes blocks of
// statements.
func (c *Checker) catchExpr(e *syntax.CatchExpr, want Type) Type {
	if !throwsIn(e.X) {
		c.errorf(e.Catches[0], "nothing before this catch throws: it takes the errors of a try (try f() catch { … })")
	}
	var xt Type
	c.catching(e.Catches, func() {
		if want != nil {
			xt = c.assign(e.X, want, "the value")
		} else {
			xt = c.expr(e.X, nil)
		}
	})
	target := want
	if target == nil {
		target = xt
	}
	if target == NoneType {
		c.errorf(e.X, "none says nothing of the type: write it, let x: T? = none")
		target = Invalid
	}
	c.catchBlocks(e.Catches, func(cl *syntax.CatchClause) {
		if target == Void || target == Never {
			c.block(cl.Body)
		} else {
			c.fallback(cl, target)
		}
	})
	if want != nil {
		return want
	}
	if xt == nil {
		return Invalid
	}
	return xt
}

// catchStmt checks a catch expression whose value isn't used: try
// save() catch { failed = true }. Its clauses are blocks of statements,
// and its type is void.
func (c *Checker) catchStmt(e *syntax.CatchExpr) {
	if !throwsIn(e.X) {
		c.errorf(e.Catches[0], "nothing before this catch throws: it takes the errors of a try (try f() catch { … })")
	}
	c.catching(e.Catches, func() { c.expr(e.X, nil) })
	c.catchBlocks(e.Catches, func(cl *syntax.CatchClause) { c.block(cl.Body) })
	c.record(e, Void)
}

// catchBlocks checks catch clauses: what each takes, then its block
// (body), in a scope with what the clause binds.
func (c *Checker) catchBlocks(catches []*syntax.CatchClause, body func(*syntax.CatchClause)) {
	for i, cl := range catches {
		if i < len(catches)-1 && c.catchAll(cl) {
			c.errorf(catches[i+1], "this catch is never reached: the one before catches everything")
		}
		c.openScope()
		c.catchClause(cl)
		body(cl)
		c.closeScope()
	}
}

// fallback checks a catch clause's block as a value of type t.
func (c *Checker) fallback(cl *syntax.CatchClause, t Type) {
	b := cl.Body
	n := len(b.Items)
	if n == 0 {
		c.errorPos(b.Lbrace, "this catch has no value: end it with a %s, or leave (return, throw)", typeString(t))
		return
	}
	c.openScope()
	defer c.closeScope()
	c.stmts(b.Items[:n-1])
	last := b.Items[n-1]
	if es, ok := last.(*syntax.ExprStmt); ok {
		got := c.expr(es.X, t)
		if got == Void && !IsInvalid(t) {
			c.errorf(es.X, "this catch has no value (it's void): end it with a %s, or leave (return, throw)", typeString(t))
			return
		}
		if !c.convert(es.X, got, t) {
			c.errorf(es.X, "the catch's value is a %s, and the try's a %s: make them the same type%s", typeString(got), typeString(t), c.convHint(got, t))
		}
		return
	}
	c.stmt(last)
	if !c.terminates(b.Items) {
		c.errorf(last, "this catch has no value: end it with a %s, or leave (return, throw)", typeString(t))
	}
}

// throwsIn reports whether x may throw where it is: a try, or a throw
// as a value, outside the closures in it.
func throwsIn(x syntax.Expr) bool {
	found := false
	syntax.Inspect(x, func(n syntax.Node) bool {
		switch n := n.(type) {
		case *syntax.LambdaExpr, *syntax.SpawnExpr, *syntax.Block:
			return false
		case *syntax.UnaryExpr:
			if n.Op == syntax.TRY && !n.Optional {
				found = true
			}
		case *syntax.JumpExpr:
			if n.Tok == syntax.THROW {
				found = true
			}
		}
		return !found
	})
	return found
}

// ---------------------------------------------------------------------
// Local fns

type localUse struct {
	ctx *FuncCtx // where it's used
	fn  *LocalFn
}

// localFns checks a group of fns declared one after another in a block:
// their names first (so they see each other), then their bodies.
func (c *Checker) localFns(items []syntax.Item) {
	g := &LocalGroup{Encl: c.ctx}
	c.localGroups = append(c.localGroups, g)
	for _, it := range items {
		d := it.(*syntax.FuncDecl)
		switch {
		case d.Pub.IsValid():
			c.errorf(d, "a fn in a block is its own: pub goes on a file's declarations")
		case d.Native.IsValid() || d.Extern.IsValid():
			c.errorf(d, "native and extern fns are declared at the top of a file")
		case len(d.TypeParams) > 0:
			c.errorf(d.Name, "a fn in a block isn't generic: declare %s at the top of the file", d.Name.Name)
		case d.Body == nil && d.Value == nil:
			c.errorf(d.Name, "fn %s has no body", d.Name.Name)
		}
		sig := c.signature(d, nil)
		for i, p := range sig.Params {
			if p.Default != nil {
				c.errorf(d.Params.List[i].Default, "a fn in a block takes its arguments in order, without defaults: declare %s at the top of the file for a default", d.Name.Name)
			}
		}
		ft := &FuncType{Async: sig.Async, Throws: sig.Throws, Result: sig.Result}
		for _, p := range sig.Params {
			ft.Params = append(ft.Params, p.Type)
		}
		v := c.declareVar(d.Name, ft, false)
		v.Kind = LocalFnVar
		lf := &LocalFn{Var: v, Decl: d, Sig: sig, Group: g}
		v.Fn = lf
		g.Fns = append(g.Fns, lf)
	}
	for _, lf := range g.Fns {
		c.localFnBody(lf)
	}
}

// localFnBody checks a local fn's body: a function of its own, inside
// the one it's declared in, whose locals it captures as a closure does.
func (c *Checker) localFnBody(lf *LocalFn) {
	d := lf.Decl
	saved := c.ctx
	savedTry, savedAwait, savedHandled, savedLoops, savedView, savedInDefer := c.inTry, c.inAwait, c.handled, c.loops, c.view, c.inDefer
	c.inTry, c.inAwait, c.handled, c.loops, c.view, c.inDefer = 0, 0, 0, 0, false, 0
	savedPartial := c.partial
	c.partial = nil
	ctx := c.enter(CtxLocalFn, d, d.Name.Name)
	lf.Ctx = ctx
	ctx.Async, ctx.Throws, ctx.Result = lf.Sig.Async, lf.Sig.Throws, lf.Sig.Result
	for i, p := range lf.Sig.Params {
		v := c.declareVar(d.Params.List[i].Name, p.Type, false)
		p.Var = v
		ctx.Params = append(ctx.Params, v)
	}
	switch {
	case d.Value != nil:
		if ctx.Result == Void {
			c.expr(d.Value, nil)
		} else {
			c.assign(d.Value, ctx.Result, "the result of "+d.Name.Name)
		}
	case d.Body != nil:
		c.stmts(d.Body.Items)
		if ctx.Result != Void && !c.terminates(d.Body.Items) {
			c.errorPos(d.Body.Rbrace, "fn %s must return a %s: it can reach its end without a return", d.Name.Name, typeString(ctx.Result))
		}
	}
	c.leave(saved)
	c.inTry, c.inAwait, c.handled, c.loops, c.view, c.inDefer = savedTry, savedAwait, savedHandled, savedLoops, savedView, savedInDefer
	c.partial = savedPartial
}

// finishLocalFns works out what each group of local fns captures: what
// their bodies use of the code around them, and what the local fns they
// use capture (each use of a local fn, a call or a value, needs its
// group's captures where it is). It goes on until nothing changes, as
// fns use each other.
func (c *Checker) finishLocalFns() {
	for changed := true; changed; {
		changed = false
		for _, g := range c.localGroups {
			seen := map[*Var]bool{}
			for _, v := range g.Captures {
				seen[v] = true
			}
			for _, lf := range g.Fns {
				if lf.Ctx == nil {
					continue
				}
				for _, v := range lf.Ctx.Captures {
					if v.Kind == LocalFnVar || seen[v] {
						continue
					}
					seen[v] = true
					g.Captures = append(g.Captures, v)
					changed = true
				}
				if lf.Ctx.UsesEnv && !g.UsesEnv {
					g.UsesEnv = true
					changed = true
				}
			}
		}
		for _, u := range c.localUses {
			g := u.fn.Group
			for _, v := range g.Captures {
				v.Captured = true
				for cc := u.ctx; cc != nil && cc != v.Ctx; cc = cc.Parent {
					if cc.capIndex == nil || !cc.capIndex[v] {
						cc.capture(v)
						changed = true
					}
				}
			}
			if g.UsesEnv {
				// As envUse: up to the component's or the store's
				// function, or its method.
				owner := g.Encl.Comp
				for cc := u.ctx; cc != nil; cc = cc.Parent {
					if (cc.Kind == CtxComponent || cc.Kind == CtxStore || cc.Kind == CtxMethod) && cc.Comp == owner {
						break
					}
					if !cc.UsesEnv {
						cc.UsesEnv = true
						changed = true
					}
				}
			}
		}
	}
}

// selfThroughVar warns of walk = n => { … walk(…) … }: a closure kept in
// a var it captures, so the var's box holds the closure and the closure
// the box, and neither is ever freed. A local fn calls itself without
// either holding the other.
func (c *Checker) selfThroughVar(s *syntax.AssignStmt) {
	id, ok := unparen(s.Target).(*syntax.Ident)
	if !ok {
		return
	}
	v, ok := c.info.Uses[id].(*Var)
	if !ok || !v.Mutable {
		return
	}
	lam, ok := unparen(s.Value).(*syntax.LambdaExpr)
	if !ok {
		return
	}
	if ctx := c.info.Ctxs[lam]; ctx != nil && ctx.capIndex[v] {
		c.warnf(lam, "%s calls itself through the variable it's stored in, which keeps it alive forever: declare it fn %s(…)", v.name, v.name)
	}
}
