package check

import (
	"strings"

	"github.com/egoist/asuka/lang/diag"
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
		c.loopBody(s, s.Label, func() { c.block(s.Body) })
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
		c.branch(s, s.Tok, s.Label)
	case *syntax.CleanupStmt:
		c.cleanupStmt(s, false)
	case *syntax.DeferStmt:
		if c.inDefer > 0 {
			c.errorf(s, diag.MisplacedStatement, "a defer inside a defer: put the code in the first one")
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
				c.errorf(s, diag.ViewInStatement, "a view here does nothing: views go in a component's body")
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
		c.errorf(it, diag.MisplacedDeclaration, "%s isn't a statement: %s", itemName(it), notStatementHint(it))
	case *syntax.BadItem:
	case *syntax.FuncDecl:
		c.errorf(s, diag.MisplacedDeclaration, "a fn here goes in a block of statements: put it in braces, or use a closure, let %s = (…) => …", s.Name.Name)
	default:
		c.errorf(it, diag.MisplacedDeclaration, "%s isn't a statement: %s", itemName(it), notStatementHint(it))
	}
}

func itemName(it syntax.Item) string {
	switch it := it.(type) {
	case *syntax.PropDecl:
		return "prop"
	case *syntax.EventDecl:
		return "event"
	case *syntax.VariantDecl:
		return "variant"
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
	case *syntax.CleanupStmt:
		return "cleanup"
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
		// The parser said so (expected '=' and a value); the names are
		// still declared, so their uses don't say they're unknown.
		t = Invalid
		if d.Type != nil {
			t = c.typeExpr(d.Type)
		}
		c.bindIrrefutable(d.Pattern, t, d.Keyword == syntax.VAR, true)
		return
	}
	if d.Else != nil {
		c.letElse(d)
		return
	}
	if d.Type != nil {
		want := c.typeExpr(d.Type)
		if tt := testedLet(want); tt != nil {
			// let c: Circle = shape: a Shape that may not be one.
			got := c.expr(d.Value, want)
			if bp, ok := d.Pattern.(*syntax.BindPat); ok && testable(got) && !c.convert(d.Value, got, want) && c.mayBe(got, tt) {
				name := exprName(d.Value)
				if name == "this" {
					name = "…"
				}
				c.errorf(d.Value, diag.MaybeNone, "%s isn't always %s: if let %s: %s = %s, or let … else", aType(Unopt(got)), aType(want), bp.Name.Name, typeString(want), name)
				t = want
			} else {
				t = c.assigned(d.Value, got, want, "the value")
			}
		} else {
			t = c.assign(d.Value, want, "the value")
		}
	} else {
		t = c.valueType(c.expr(d.Value, nil), d.Value)
	}
	c.info.Lets[d] = t
	c.bindIrrefutable(d.Pattern, t, d.Keyword == syntax.VAR, true)
	if fx := c.inferredFx[t]; fx != nil && fx.letName == "" {
		if bp, ok := d.Pattern.(*syntax.BindPat); ok && unparen(d.Value) == syntax.Expr(fx.lambda) {
			fx.letName = d.Keyword.String() + " " + bp.Name.Name
		}
	}
}

// bindIrrefutable binds a let's (let) or a for's pattern: names, _ and
// tuples of them.
func (c *Checker) bindIrrefutable(p syntax.Pattern, t Type, mutable, let bool) {
	switch p := p.(type) {
	case *syntax.BindPat:
		c.declareVar(p.Name, t, mutable).Bound = !let
	case *syntax.WildcardPat:
	case *syntax.TuplePat:
		tt, ok := t.(*Tuple)
		if !ok || len(tt.Elems) != len(p.Elems) {
			if !IsInvalid(t) {
				c.errorf(p, diag.PatternType, "%s", notTuple(t, len(p.Elems)))
			}
			for _, e := range p.Elems {
				c.bindIrrefutable(e, Invalid, mutable, let)
			}
			return
		}
		for i, e := range p.Elems {
			c.bindIrrefutable(e, tt.Elems[i], mutable, let)
		}
	default:
		c.errorf(p, diag.InvalidPattern, "a let binds names (let x = …, let (a, b) = …); to match, use if let or match")
	}
}

// notTuple says why a tuple pattern of n doesn't fit t.
func notTuple(t Type, n int) string {
	if tt, ok := Unopt(t).(*Tuple); ok && IsOptional(t) && len(tt.Elems) == n {
		return aType(t) + " may be none: say what happens then, let (a, b) = x else { … }, or if let (a, b) = x { … }"
	}
	return aType(t) + " isn't a tuple of " + itoa(n)
}

func (c *Checker) assignStmt(s *syntax.AssignStmt) {
	lt := c.placeType(s.Target, c.lvalue(s.Target, "assign to"))
	if s.Op != syntax.ASSIGN {
		// x += y reads x, then writes it.
		if obj, at := c.writeRoot(s.Target); obj != nil {
			c.noteRead(obj, at)
		}
	}
	if ix, ok := unparen(s.Target).(*syntax.IndexExpr); ok && ix.Default == nil {
		if _, isMap := c.info.Types[ix.X].(*Map); isMap {
			// m[k] reads a V?, and stores a V.
			switch s.Op {
			case syntax.ASSIGN, syntax.COALESCE_ASSIGN:
				c.assign(s.Value, Unopt(lt), "the value")
			default:
				c.expr(s.Value, nil)
				c.errorf(s, diag.MaybeNone, "m[k] may be none: write m[k] = (m[k] ?? 0) %s …", strings.TrimSuffix(s.Op.String(), "="))
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
			c.errorf(s.Target, diag.NotOptional, "%s isn't optional: ??= has nothing to do", typeString(lt))
		}
		c.assign(s.Value, lt, "the value")
	default:
		want := numOnly(lt)
		if s.Op.AssignOp().IsBitwise() {
			want = Int
		} else if p := c.operatorParam(s.Op.AssignOp(), lt); p != nil {
			want = p
		}
		vt := c.expr(s.Value, want)
		if IsInvalid(lt) || IsInvalid(vt) {
			return
		}
		if o, _ := c.findOperator(s.Op.AssignOp(), lt, vt, lt); o != nil {
			// x += y is x = x.add(y): the method of x's type.
			c.info.AssignOps[s] = o
			return
		}
		op := strings.TrimSuffix(s.Op.String(), "=")
		if o, res := c.findOperator(s.Op.AssignOp(), lt, vt, nil); o != nil && !o.Compare {
			c.errorf(s, diag.BadOperand, "%s= is x = x %s y, and %s's %s gives %s, not %s", op, op, typeString(lt), o.Func.name, aType(res), aType(lt))
			return
		}
		switch {
		case s.Op.AssignOp().IsBitwise():
			if lt != Int {
				c.errorf(s.Target, diag.BadOperand, "%s= takes ints, not %s", op, aType(lt))
			} else if vt != Int {
				c.errorf(s.Value, diag.BadOperand, "%s= takes ints, not %s", op, aType(vt))
			}
		case IsNumeric(lt) && Identical(lt, vt):
		case lt == String && vt == String && s.Op == syntax.ADD_ASSIGN:
		case lt == Duration && vt == Duration && (s.Op == syntax.ADD_ASSIGN || s.Op == syntax.SUB_ASSIGN):
		case lt == Duration && vt == Int && (s.Op == syntax.MUL_ASSIGN || s.Op == syntax.QUO_ASSIGN):
		default:
			if _, ok := lt.(*Array); ok && Identical(lt, vt) && s.Op == syntax.ADD_ASSIGN {
				return
			}
			c.errorf(s, diag.BadOperand, "%s= doesn't take %s and %s", op, aType(lt), aType(vt))
		}
	}
}

// placeType is the type of the place x (of type t as an expression):
// a ?. chain's member is its own type, not the chain's optional.
func (c *Checker) placeType(x syntax.Expr, t Type) Type {
	switch x := unparen(x).(type) {
	case *syntax.MemberExpr:
		if sel := c.info.Selections[x]; sel != nil && sel.Optional && sel.MemberType != nil {
			return sel.MemberType
		}
	case *syntax.IndexExpr:
		if c.chained[x] {
			switch xt := Unopt(c.info.Types[x.X]).(type) {
			case *Array:
				return xt.Elem
			case *Map:
				if x.Default != nil {
					return xt.Elem
				}
				return MakeOptional(xt.Elem)
			}
		}
	}
	return t
}

// lvalue checks x as something assigned to (or changed in place) and
// returns its type.
func (c *Checker) lvalue(x syntax.Expr, what string) Type {
	t := c.expr(x, nil)
	c.noteWrite(x)
	if c.ctx != nil {
		switch c.ctx.Kind {
		case CtxBinding, CtxGlobal, CtxBuilder, CtxComponent, CtxStore, CtxApp:
			c.errorf(x, diag.WriteInBinding, "can't %s %s here: what the UI follows is computed, not changed; change state in a handler (on press { … }), a fn, a task or an effect", what, exprName(x))
			return t
		}
	}
	// A prop its component assigns: only its whole value, by = (or bind).
	saved := c.lvalueWhole
	c.lvalueWhole = nil
	if what == "assign to" || what == "bind" {
		c.lvalueWhole = unparen(x)
	}
	c.checkLvalue(x, what)
	c.lvalueWhole = saved
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
				hint, short := "it's a let: make it a var", x.Name+" is a let"
				if o.Bound && o.Kind == LocalVar {
					hint = "it's a pattern's name, a copy of what it matched (a for's element, an if let's value): copy it into a var, or change what it came from"
					short = x.Name + " is a copy of what it matched"
				}
				if o.Kind == StateSelf {
					hint = "in its own initializer, a state's name is its current value: the initializer's value is what it becomes"
				} else if o.Kind == RowItem {
					hint, short = "it's a row's item: change the state it comes from", x.Name+" is a row's item"
				} else if o.Kind == SelfVar {
					hint = "self is a value"
				} else if o.Kind == LocalFnVar {
					hint, short = "it's a fn: a var holds a closure that changes", x.Name+" is a fn"
				} else if o.Ctx != nil && o.Ctx.Kind == CtxLambda && contains(o.Ctx.Params, o) || isParam(o) {
					hint, short = "parameters are values: copy it into a var", x.Name+" is a parameter"
				}
				c.notPlace(x, diag.NotAssignable, short, "can't %s %s: %s", what, x.Name, hint)
				return
			}
			o.Assigned = true
			if o.Ctx != c.ctx {
				o.AssignedInClosure = true
				for cc := c.ctx; cc != nil; cc = cc.Parent {
					if cc.Kind == CtxSpawn {
						c.errorf(x, diag.SpawnCapturesState, "spawn can't change %s: it runs on another thread; return a value instead", x.Name)
						break
					}
				}
			}
		case *Member:
			switch o.Kind {
			case StateMember:
			case PropMember:
				switch {
				case o.ChangeEvent() == nil:
					c.notPlace(x, diag.PropNotAssignable, x.Name+" is a prop", "can't %s %s: it's the caller's (a prop); declare event %sChange(%s: %s) to assign it (a caller that binds it gets the change), or keep state of your own", what, x.Name, x.Name, x.Name, typeString(o.Type))
				case c.lvalueWhole != syntax.Expr(x):
					c.notPlace(x, diag.PropNotAssignable, x.Name+" is a prop", "can't %s %s: a prop changes as a whole, which %sChange reports; assign it a new value (%s = …)", what, x.Name, x.Name, x.Name)
				case o.IsSlot():
					c.errorf(x, diag.NotAssignable, "can't %s %s: it's a slot, the caller's views", what, x.Name)
				default:
					o.Assigned = true
				}
			case ParamMember:
				if o.Owner.Comp.Worker {
					c.notPlace(x, diag.NotAssignable, x.Name+" is a parameter", "can't %s %s: it's a parameter; keep a var of the worker's own (var x = %s)", what, x.Name, x.Name)
					break
				}
				c.notPlace(x, diag.NotAssignable, x.Name+" is a parameter", "can't %s %s: it's the caller's (a parameter); keep state of your own, or emit an event", what, x.Name)
			case VarMember:
			case LetMember:
				if o.Owner.Comp.Worker {
					c.notPlace(x, diag.NotAssignable, x.Name+" is a let", "can't %s %s: it's a let; a value the worker changes is a var (var %s = …)", what, x.Name, x.Name)
					break
				}
				c.notPlace(x, diag.NotAssignable, x.Name+" is a let", "can't %s %s: it's derived (a let); change what it reads", what, x.Name)
			default:
				c.notPlace(x, diag.NotAssignable, x.Name+" isn't state", "can't %s %s", what, x.Name)
			}
		case *Global:
			if o.Kind != StateGlobal {
				c.notPlace(x, diag.NotAssignable, x.Name+" is a let", "can't %s %s: it's a let; a value that changes is state", what, x.Name)
			}
		case *selfMember:
			c.selfWrite(x, what, x.Name)
		case nil:
		default:
			c.notPlace(x, diag.NotAssignable, x.Name+" isn't a var", "can't %s %s", what, x.Name)
		}
	case *syntax.MemberExpr:
		sel := c.info.Selections[x]
		if sel == nil {
			return
		}
		if x.Optional {
			// A ?. chain is a place when it starts at one: it changes
			// nothing when it meets none.
			switch unparen(x.X).(type) {
			case *syntax.Ident, *syntax.MemberExpr, *syntax.IndexExpr, *syntax.SelfExpr:
			default:
				c.notPlace(x, diag.NotAssignable, exprName(x.X)+" gives a value, not a place", "can't %s %s: the ?. chain starts at a value, not a place; a chain that's changed starts at a var or state", what, exprName(x))
				return
			}
		}
		switch sel.Kind {
		case SelField, SelTupleElem:
			c.checkLvalue(x.X, what)
		case SelStoreMember:
			if sel.Member.Kind != StateMember {
				c.notPlace(x, diag.NotAssignable, exprName(x)+" isn't state", "can't %s %s: it isn't state", what, x.Name.Name)
			}
		case SelPkg:
			if g, ok := sel.Obj.(*Global); !ok || g.Kind != StateGlobal {
				c.notPlace(x, diag.NotAssignable, exprName(x)+" isn't state", "can't %s %s: only a package's state changes", what, exprName(x))
			}
		default:
			c.notPlace(x, diag.NotAssignable, exprName(x)+" isn't a var", "can't %s %s", what, exprName(x))
		}
	case *syntax.IndexExpr:
		if c.info.Types[x.X] == Bytes {
			c.errorf(x, diag.NotAssignable, "can't %s a byte: bytes don't change in place; make new ones (bytes([…]) of ints, b.slice, b.concat)", what)
			return
		}
		c.checkLvalue(x.X, what)
	case *syntax.SelfExpr:
		c.selfWrite(x, what, "self")
	default:
		short := "this isn't a var"
		if _, ok := unparen(x).(*syntax.CallExpr); ok {
			short = exprName(x) + " gives a value, not a place"
		}
		c.notPlace(x, diag.NotAssignable, short, "can't %s this: only vars, state, and their fields and elements", what)
	}
}

// notPlace reports x, which a statement changes, as what it can't
// change: for a mut fn's call (c.mutName), the call's error, with short
// saying why ("cart is a let"); for the rest, format's.
func (c *Checker) notPlace(x syntax.Node, code *diag.Code, short, format string, args ...any) {
	if c.mutName != "" {
		c.errorf(x, diag.NotAssignable, "%s changes its value: call it on a var or state (%s)", c.mutName, short)
		return
	}
	c.errorf(x, code, format, args...)
}

// selfWrite checks a change of self, or of its field name, in a method:
// a mut fn's body may (its closures may not: they have self's value as
// it was); a fn's may not.
func (c *Checker) selfWrite(x syntax.Node, what, name string) {
	closure := false
	m := c.ctx
	for m != nil && m.Kind != CtxMethod {
		if m.IsClosure() {
			closure = true
		}
		m = m.Parent
	}
	switch {
	case m == nil || m.Func == nil:
		c.errorf(x, diag.NotAssignable, "can't %s %s: it's a value", what, name)
	case !m.Func.Sig.Mut:
		c.errorf(x, diag.NotAssignable, "%s is a fn: declare it mut fn to change the value", m.Func.name)
	case closure:
		c.errorf(x, diag.NotAssignable, "can't %s %s in a closure: it has self's value as it was made; change it in %s's body", what, name, m.Func.name)
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
	if lc.Var && kind != LocalVar {
		c.errorf(lc, diag.InvalidPattern, "a view's if binds what it shows, which changes with what it reads: if let, not if var")
	}
	if bp, ok := lc.Pattern.(*syntax.BindPat); ok {
		if !IsOptional(t) && !IsInvalid(t) {
			kw := "let"
			if lc.Var {
				kw = "var"
			}
			c.errorf(lc.Value, diag.NotOptional, "%s isn't optional: if %s unwraps an optional (use %s)", typeString(t), kw, kw)
		}
		v := c.declareVar(bp.Name, Unopt(t), lc.Var && kind == LocalVar)
		v.Kind = kind
		v.Bound = true
		return
	}
	if lc.Var {
		c.errorf(lc.Pattern, diag.InvalidPattern, "if var binds a name, a copy that may change: if var x = optional")
	}
	c.topPattern(lc.Pattern, lc.Value, t, kind)
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
				c.errorf(s.Seq, diag.InvalidFor, "a for in a view goes over an array: use the map's keys() or values()")
			}
		case *Set:
			fi.Kind, fi.Elem = ForSet, u.Elem
		default:
			if !view && !IsInvalid(t) {
				c.openScope() // closed after the loop
				if c.forIter(s, fi, t) {
					break
				}
				c.closeScope()
			}
			if t == String {
				c.errorf(s.Seq, diag.InvalidFor, "a string isn't a list: go over its characters, s.chars()")
			} else if _, ok := t.(*Stream); ok {
				c.errorf(s.Seq, diag.InvalidFor, "a stream's values come over time: for await x in it (in an async fn, a task or a handler)")
			} else if !IsInvalid(t) {
				c.errorf(s.Seq, diag.InvalidFor, "for goes over an array, a set, a map, a range or an iterator (a type with mut fn next() -> T?, or fn iter() that gives one), not %s", aType(t))
			}
			fi.Kind, fi.Elem = ForArray, Invalid
		}
	}
	if view {
		c.leave(saved)
		if s.Key == nil && (fi.Kind == ForSet || isRange(seq)) {
			fi.KeyItem = true
		}
		if s.Key == nil && !fi.KeyItem {
			c.viewFors = append(c.viewFors, s)
		}
	}
	// The key, in a view: a function of the item.
	if s.Key != nil {
		if !view {
			c.errorf(s.Key, diag.InvalidFor, "key is for a for in a view")
		}
		saved := c.ctx
		kc := c.enter(CtxLambda, s.Key, "key")
		kc.Result = nil
		c.forBindings(fi, s.Pattern, LocalVar, kc)
		kt := c.expr(s.Key, nil)
		c.checkHashable(kt, s.Key, "a row's key", false)
		kc.Result = kt
		c.leave(saved)
	}
	if view {
		if s.Label != nil {
			c.errorf(s.Label, diag.LoopLabel, "a for among views makes rows, and isn't a loop to break: drop as %s", s.Label.Name)
		}
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
	c.loopBody(s, s.Label, func() { c.block(s.Body) })
	c.closeScope()
	if fi.Kind == ForIter {
		c.closeScope() // its hidden vars'
	}
}

// forBindings declares a for's item (and index, or key and value).
func (c *Checker) forBindings(fi *ForInfo, p syntax.Pattern, kind VarKind, ctx *FuncCtx) {
	bind := func(p syntax.Pattern, t Type) *Var {
		switch p := p.(type) {
		case *syntax.BindPat:
			v := c.declareVar(p.Name, t, false)
			v.Kind = kind
			v.Bound = true
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
				c.errorf(p, diag.InvalidPattern, "a row's item is one name: for x in xs")
				return nil
			}
			v := &Var{object: object{"_", p.Start}, Type: t, Ctx: c.ctx}
			c.bindIrrefutable(p, t, false, false)
			return v
		}
		c.errorf(p, diag.InvalidPattern, "for binds a name, _ or a tuple of names")
		return nil
	}
	switch fi.Kind {
	case ForEnumerated, ForMap:
		tp, ok := p.(*syntax.TuplePat)
		if !ok || len(tp.Elems) != 2 {
			if fi.Kind == ForMap {
				c.errorf(p, diag.InvalidPattern, "a map gives (key, value): for (k, v) in m")
			} else {
				c.errorf(p, diag.InvalidPattern, "enumerated gives (index, item): for (i, x) in xs.enumerated()")
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
		c.errorf(s, diag.MisplacedStatement, "return in a defer: a defer runs as its block ends, and can't change how")
		if value != nil {
			c.expr(value, nil) // checked, but not as the result: there's none
		}
		return
	}
	ctx := c.ctx
	if ctx == nil {
		return
	}
	switch ctx.Kind {
	case CtxComponent, CtxStore, CtxBinding, CtxBuilder, CtxGlobal, CtxApp:
		c.errorf(s, diag.MisplacedStatement, "return in a component's body: a body is declarations, not code")
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
		c.errorf(s, diag.ReturnValue, "return needs %s", aType(res))
	case value != nil && res == Void:
		c.expr(value, nil)
		c.errorf(value, diag.ReturnValue, "%s returns nothing: drop the value, or declare a result (-> T)", ctxLabel(ctx))
	case value != nil && ctx.loose != nil:
		ctx.loose.value(c, value, "the result")
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
	case CtxCleanup:
		return "a cleanup"
	case CtxTest:
		return "a test"
	}
	return "this closure"
}

func (c *Checker) emitStmt(s *syntax.EmitStmt) {
	comp := c.ctx.Comp
	if comp == nil || comp.Kind != ComponentType {
		c.errorf(s, diag.MisplacedStatement, "emit raises a component's event, and this isn't in a component")
		c.emitArgs(s, nil)
		return
	}
	m := comp.Comp.Members[s.Name.Name]
	if m == nil || m.Kind != EventMember {
		c.errorf(s.Name, diag.UnknownEvent, "%s has no event %s: declare it, event %s", comp.name, s.Name.Name, s.Name.Name)
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
		c.errorf(s, diag.ArgumentCount, "event %s takes %s", s.Name.Name, plural(len(params), "argument", "arguments"))
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
		c.errorf(s, diag.InvalidCatch, "do needs a catch")
	}
	c.catching(s.Catches, func() { c.block(s.Body) })
	for i, cl := range s.Catches {
		c.openScope()
		c.catchClause(cl)
		c.block(cl.Body)
		c.closeScope()
		if i < len(s.Catches)-1 && c.info.Catches[cl].All {
			c.errorf(s.Catches[i+1], diag.InvalidCatch, "this catch is never reached: the one before catches everything")
		}
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
				c.errorf(cl.Type, diag.NotAnError, "%s isn't an error type: catch e: T takes a type declared : Error", typeString(t))
			}
		} else {
			ci.All = true
		}
		ci.Var = c.declareVar(p.Name, t, false)
	case *syntax.CasePat:
		if p.Implicit || len(p.Path) < 2 {
			c.errorf(p, diag.InvalidCatch, "write the error's type: catch MailError.offline")
			return
		}
		tn := c.casePathType(p.Path[:len(p.Path)-1])
		if tn == nil {
			return
		}
		if !tn.IsError {
			c.errorf(p, diag.NotAnError, "%s isn't an error type (declare it: enum %s: Error)", tn.name, tn.name)
			return
		}
		k := tn.Case(p.Path[len(p.Path)-1].Name)
		if k == nil {
			c.errorf(p, diag.UnknownMember, "%s has no case %s", tn.name, p.Path[len(p.Path)-1].Name)
			return
		}
		ci.Type, ci.Case = tn, k
		c.info.Cases[p] = k
		c.caseArgs(p, k, tn.Type)
	default:
		c.errorf(cl.Pattern, diag.InvalidCatch, "catch takes a name (catch e), a typed one (catch e: T) or a case (catch T.case)")
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
		c.errorf(path[0], diag.PatternType, "%s isn't an enum", joinPath(path))
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
		v.Bound = true
	case *syntax.TypedPat:
		c.typedPat(p, t, kind)
	case *syntax.LitPat:
		// A literal is compared with an optional's value, once it isn't
		// none: it's of the value's type (not converted to the optional).
		et := t
		if _, none := p.Value.(*syntax.NoneLit); !none {
			et = Unopt(t)
		}
		lt := c.expr(p.Value, et)
		if !IsInvalid(lt) && !IsInvalid(t) && !c.convert(p.Value, lt, et) && !(lt == NoneType && IsOptional(t)) {
			c.errorf(p, diag.PatternType, "%s can't be %s", aType(t), aType(lt))
		}
	case *syntax.TuplePat:
		// An optional's value, as a case's or a literal's (none doesn't
		// match).
		tt, ok := Unopt(t).(*Tuple)
		if !ok || len(tt.Elems) != len(p.Elems) {
			if !IsInvalid(t) {
				c.errorf(p, diag.PatternType, "%s isn't a tuple of %d", aType(t), len(p.Elems))
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
		// Each alternative's names in a scope of its own: said once, and
		// the first's kept, so the arm's uses of them aren't unknown.
		saved := c.scope
		binds := false
		for i, a := range p.Alts {
			c.scope = NewScope(saved)
			c.pattern(a, t, kind)
			if len(c.scope.names) > 0 {
				binds = true
				if i == 0 {
					for _, o := range c.scope.names {
						saved.Insert(o)
					}
				}
			}
		}
		c.scope = saved
		if binds {
			c.errorf(p, diag.InvalidPattern, "a | pattern can't bind names: match the cases in arms of their own")
		}
	case *syntax.CasePat:
		if !c.casePattern(p, t) {
			// Its names are bound all the same, so that what reads them
			// isn't reported too.
			for _, a := range p.Args {
				c.pattern(a, Invalid, kind)
			}
		}
	}
}

// casePattern checks a case's (or a struct's) pattern, and reports whether
// it bound its payload's patterns: not when it reported what's wrong
// with it.
func (c *Checker) casePattern(p *syntax.CasePat, t Type) bool {
	if st := c.patternStruct(p); st != nil {
		c.structPattern(p, st, t)
		return true
	}
	et := Unopt(t)
	var tn *TypeName
	if p.Implicit {
		n, ok := et.(*Named)
		if !ok || n.Obj.Kind != EnumType {
			if !IsInvalid(t) {
				c.errorf(p, diag.PatternType, ".%s: %s isn't an enum", p.Path[0].Name, aType(t))
			}
			return false
		}
		tn = n.Obj
	} else if len(p.Path) < 2 {
		name := "case"
		if len(p.Path) == 1 {
			name = p.Path[0].Name
		}
		c.errorf(p, diag.InvalidPattern, "%s(…) isn't a case: write .%s(…), or Type.%s(…)", name, name, name)
		return false
	} else {
		tn = c.casePathType(p.Path[:len(p.Path)-1])
		if tn == nil {
			return false
		}
		if n, ok := et.(*Named); (!ok || n.Obj != tn) && testable(et) && tn.Kind == EnumType {
			// A value of an interface (an Error too) or of a type
			// parameter, matched by an enum's case: a type test,
			// then the case.
			name := p.Path[len(p.Path)-1].Name
			k := tn.Case(name)
			if k == nil {
				c.errorf(p, diag.UnknownMember, "%s has no case %s", tn.name, name)
				return false
			}
			tt := c.typeTest(p, t, c.testedType(p, tn))
			if tt == nil {
				return false
			}
			c.info.Cases[p] = k
			c.caseArgs(p, k, tt)
			return true
		}
		if n, ok := et.(*Named); !ok || n.Obj != tn {
			if !IsInvalid(t) {
				c.errorf(p, diag.PatternType, "%s can't be a %s", aType(t), tn.name)
			}
			return false
		}
	}
	name := p.Path[len(p.Path)-1].Name
	k := tn.Case(name)
	if k == nil {
		c.errorf(p, diag.UnknownMember, "%s has no case %s", tn.name, name)
		return false
	}
	c.info.Cases[p] = k
	c.caseArgs(p, k, et)
	return true
}

// caseArgs checks a case pattern's payload patterns.
func (c *Checker) caseArgs(p *syntax.CasePat, k *Case, t Type) {
	if !p.Lparen.IsValid() {
		return
	}
	kind := LocalVar
	if c.view {
		kind = ViewMemo
	}
	if len(p.Args) != len(k.Params) {
		c.errorf(p, diag.InvalidPattern, "case %s has %s", k.Name, plural(len(k.Params), "payload value", "payload values"))
		for _, a := range p.Args {
			c.pattern(a, Invalid, kind)
		}
		return
	}
	for i := range p.Args {
		// A label names the payload's: positions still say which.
		if l := p.Label(i); l != nil && l.Name != k.Labels[i] {
			if k.Labels[i] == "" {
				c.errorf(l, diag.InvalidPattern, "case %s's payload %d has no label: leave it out", k.Name, i+1)
			} else {
				c.errorf(l, diag.InvalidPattern, "case %s's payload %d is %s, not %s", k.Name, i+1, k.Labels[i], l.Name)
			}
		}
	}
	var s Subst
	if n, ok := t.(*Named); ok {
		s = n.Subst()
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

// notCovered says what a match misses (exhaustive's witnesses), and
// how to cover it.
func notCovered(missing []string, arms []*syntax.MatchArm, rest string) string {
	var msg string
	switch {
	case len(missing) == 1 && missing[0] == "the rest":
		msg = "match doesn't cover every value: end it with " + rest
	case len(missing) == 1:
		msg = "match doesn't cover " + missing[0] + ": add an arm for it, or " + rest
	default:
		msg = "match doesn't cover " + strings.Join(missing, ", ") + ": add arms for them, or " + rest
	}
	for _, a := range arms {
		if a.Guard != nil {
			return msg + " (an arm with an if guard covers nothing: its guard may not hold)"
		}
	}
	return msg
}

func (c *Checker) matchStmt(s *syntax.MatchStmt) {
	t := c.expr(s.Subject, nil)
	mi := &MatchInfo{Subject: t}
	c.info.Matches[s] = mi
	for _, a := range s.Arms {
		c.openScope()
		c.topPattern(a.Pattern, s.Subject, t, LocalVar)
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
		c.errorf(s, diag.NonExhaustiveMatch, "%s", notCovered(missing, s.Arms, "_ => { }"))
	}
}

func (c *Checker) matchExpr(e *syntax.MatchExpr, want Type) Type {
	return c.matchValue(e, e.Subject, e.Arms, want)
}

// matchValue checks a match as a value (a MatchExpr, or a MatchStmt
// that's its block's value): each arm's body a value, an expression or
// a block (a MatchStmt's arm may be a statement that leaves).
func (c *Checker) matchValue(n syntax.Node, subject syntax.Expr, arms []*syntax.MatchArm, want Type) Type {
	t := c.expr(subject, nil)
	mi := &MatchInfo{Subject: t}
	c.info.Matches[n] = mi
	var res Type
	for _, a := range arms {
		c.openScope()
		c.topPattern(a.Pattern, subject, t, LocalVar)
		if a.Guard != nil {
			c.assign(a.Guard, Bool, "the guard")
		}
		var x syntax.Expr
		switch b := a.Body.(type) {
		case *syntax.ExprStmt:
			x = b.X
		case syntax.Expr:
			x = b
		}
		var at Type
		switch b := a.Body.(type) {
		case *syntax.Block:
			at = c.valueBlock(b, want)
		case nil:
		default:
			if x == nil {
				// A statement: it has no value unless it leaves.
				c.stmt(b.(syntax.Item))
				if !c.terminates([]syntax.Item{b.(syntax.Item)}) {
					c.errorf(b, diag.NoValue, "this arm has no value: make it an expression")
					at = Invalid
				} else {
					at = Never
				}
			} else if want != nil {
				at = c.assign(x, want, "the value")
			} else {
				at = c.expr(x, nil)
			}
		}
		c.closeScope()
		if res == nil {
			res = at
		} else if j := c.join(res, at); j != nil {
			res = j
		} else {
			c.errorf(a, diag.TypeMismatch, "this arm is %s, the ones before %s: make them the same type", aType(at), aType(res))
		}
	}
	ok, missing := c.exhaustive(subject, t, arms)
	mi.Exhaustive = ok
	if !ok {
		c.errorf(n, diag.NonExhaustiveMatch, "%s", notCovered(missing, arms, "_ =>"))
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
		c.errorf(s, diag.AwaitOutsideAsync, "a view can't wait: read the stream in a task { }, into state")
	}
	if c.ctx == nil || !c.ctx.Async {
		c.errorf(s, c.awaitCode(), "for await waits, and this can't: %s", c.asyncHint())
	} else {
		c.ctx.awaited(s.Start)
	}
	t := c.expr(s.Seq, nil)
	if st, ok := t.(*Stream); ok {
		fi.Elem = st.Elem
		if msg := c.throwHere(s.Seq); msg != "" {
			c.errorf(s.Seq, diag.UnhandledError, "a stream fails with what its block throws, and %s", msg)
		}
	} else {
		if !IsInvalid(t) {
			c.errorf(s.Seq, diag.InvalidFor, "for await goes over a stream, not %s (for goes over arrays)", aType(t))
		}
		fi.Elem = Invalid
	}
	if s.Key != nil {
		c.errorf(s.Key, diag.InvalidFor, "key is for a for in a view")
	}
	c.openScope()
	c.forBindings(fi, s.Pattern, LocalVar, nil)
	c.loopBody(s, s.Label, func() { c.block(s.Body) })
	c.closeScope()
}

// ---------------------------------------------------------------------
// let … else, jumps as values, catch as a value

// noLetElse reports a let … else where a let isn't a statement.
func (c *Checker) noLetElse(d *syntax.LetDecl) {
	if d.Else != nil {
		c.errorPos(d.Else.Lbrace, diag.MisplacedStatement, "let … else is a statement, in a fn, a handler, a task or a test; a let here is derived: use if let, or ?? for a default")
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
	var tested *Named
	if d.Type != nil {
		want := c.typeExpr(d.Type)
		switch {
		case name && testedLet(want) != nil:
			// let c: Circle = shape else { … }: a type test of an
			// interface's value; otherwise an optional's, unwrapped.
			got := c.expr(d.Value, MakeOptional(want))
			if testable(got) {
				t = got
				if tested = c.typeTest(d, got, testedLet(want)); tested == nil {
					tested = &Named{Obj: c.errT} // said why; the name's Invalid
				}
				break
			}
			t = c.assigned(d.Value, got, MakeOptional(want), "the value")
		case name:
			// let user: User = cache[id] else { … }: the type is the
			// unwrapped value's.
			t = c.assign(d.Value, MakeOptional(want), "the value")
		default:
			t = c.assign(d.Value, want, "the value")
		}
	} else {
		t = c.valueType(c.expr(d.Value, nil), d.Value)
	}
	c.info.Lets[d] = t
	// The else block doesn't see the names.
	c.block(d.Else)
	if !c.terminates(d.Else.Items) {
		c.errorPos(d.Else.Rbrace, diag.LetElse, "the else of a %s goes on with nothing to bind: end it with return, throw, break or continue (or panic)", what)
	}
	if name && tested != nil {
		bound := Type(tested)
		if tested.Obj == c.errT {
			bound = Invalid
		}
		c.declareVar(bp.Name, bound, d.Keyword == syntax.VAR)
		return
	}
	if name {
		if !IsOptional(t) && !IsInvalid(t) {
			c.errorf(d.Value, diag.LetElse, "%s isn't optional: %s unwraps an optional or matches a pattern; this always has a value (drop the else)", typeString(t), what)
		}
		c.declareVar(bp.Name, Unopt(t), d.Keyword == syntax.VAR)
		return
	}
	if isIrrefutablePattern(d.Pattern) && !IsOptional(t) && !IsInvalid(t) {
		c.errorf(d.Pattern, diag.LetElse, "this pattern always matches %s: drop the else", aType(t))
	}
	if d.Keyword == syntax.VAR {
		c.errorf(d, diag.LetElse, "var … else binds a name: var x = optional else { … }; a pattern's names are lets")
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
		c.errorf(value, diag.NotAnError, "throw takes an error, and %s isn't one (declare it: struct %s: Error, or throw Error(\"…\"))", aType(t), typeString(t))
	}
	c.mayThrow(s)
}

// branch checks a break or a continue.
func (c *Checker) branch(s syntax.Node, tok syntax.Token, label *syntax.Ident) {
	if label != nil {
		c.branchLabel(tok, label)
		return
	}
	if c.loops == 0 {
		c.errorf(s, diag.MisplacedStatement, "%s outside a loop", tok)
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
		c.branch(e, e.Tok, e.Label)
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
		c.errorf(e.Catches[0], diag.InvalidCatch, "nothing before this catch throws: it takes the errors of a try (try f() catch { … })")
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
		c.errorf(e.X, diag.CannotInferType, "none says nothing of the type: write it, let x: T? = none")
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
		c.errorf(e.Catches[0], diag.InvalidCatch, "nothing before this catch throws: it takes the errors of a try (try f() catch { … })")
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
			c.errorf(catches[i+1], diag.InvalidCatch, "this catch is never reached: the one before catches everything")
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
		c.errorPos(b.Lbrace, diag.NoValue, "this catch has no value: end it with %s, or leave (return, throw)", aType(t))
		return
	}
	c.openScope()
	defer c.closeScope()
	c.stmts(b.Items[:n-1])
	last := b.Items[n-1]
	if es, ok := last.(*syntax.ExprStmt); ok {
		got := c.expr(es.X, t)
		if got == Void && !IsInvalid(t) {
			c.errorf(es.X, diag.NoValue, "this catch has no value (it's void): end it with %s, or leave (return, throw)", aType(t))
			return
		}
		if !c.convert(es.X, got, t) {
			c.errorf(es.X, diag.TypeMismatch, "the catch's value is %s, and the try's %s: make them the same type%s", aType(got), aType(t), c.convHint(got, t))
		}
		return
	}
	if _, ok := c.valueItem(last, t); ok {
		return
	}
	c.stmt(last)
	if !c.terminates(b.Items) {
		if s, ok := last.(*syntax.IfStmt); ok {
			c.errorf(lastIf(s), diag.NoValue, "an if used as a value needs an else")
			return
		}
		c.errorf(last, diag.NoValue, "this catch has no value: end it with %s, or leave (return, throw)", aType(t))
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
			c.errorf(d, diag.LocalFn, "a fn in a block is its own: pub goes on a file's declarations")
		case d.Native.IsValid() || d.Extern.IsValid():
			c.errorf(d, diag.LocalFn, "native and extern fns are declared at the top of a file")
		case len(d.TypeParams) > 0:
			c.errorf(d.Name, diag.LocalFn, "a fn in a block isn't generic: declare %s at the top of the file", d.Name.Name)
		case d.Body == nil && d.Value == nil:
			c.errorf(d.Name, diag.FnBody, "fn %s has no body", d.Name.Name)
		}
		if d.Mut.IsValid() {
			c.errorf(d.Name, diag.MisplacedDeclaration, "mut fn is a method that changes its value: declare it in a struct, an enum or an extend")
		}
		c.openScope()
		c.invalidTypeParams(d.TypeParams)
		sig := c.signature(d, nil)
		c.closeScope()
		for i, p := range sig.Params {
			if p.Default != nil {
				c.errorf(d.Params.List[i].Default, diag.LocalFn, "a fn in a block takes its arguments in order, without defaults: declare %s at the top of the file for a default", d.Name.Name)
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
	where := c.enterBody()
	ctx := c.enter(CtxLocalFn, d, d.Name.Name)
	lf.Ctx = ctx
	c.invalidTypeParams(d.TypeParams)
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
			c.errorPos(d.Body.Rbrace, diag.MissingReturn, "fn %s must return %s: it can reach its end without a return", d.Name.Name, aType(ctx.Result))
		}
	}
	c.leave(saved)
	c.leaveBody(where)
}

// bodyWhere is where the checker is in the code around a body of its own
// (a local fn's, a cleanup's): in a try, an await, a do's catch, loops, a
// view, a defer, none of which reach into that body.
type bodyWhere struct {
	inTry, inAwait, handled, loops, inDefer int
	view                                    bool
	partial                                 *partialCatch
}

// enterBody starts a body of its own, returning where it was for
// leaveBody.
func (c *Checker) enterBody() bodyWhere {
	w := bodyWhere{c.inTry, c.inAwait, c.handled, c.loops, c.inDefer, c.view, c.partial}
	c.inTry, c.inAwait, c.handled, c.loops, c.inDefer, c.view, c.partial = 0, 0, 0, 0, 0, false, nil
	return w
}

func (c *Checker) leaveBody(w bodyWhere) {
	c.inTry, c.inAwait, c.handled, c.loops, c.inDefer, c.view, c.partial = w.inTry, w.inAwait, w.handled, w.loops, w.inDefer, w.view, w.partial
}

// invalidTypeParams declares the type parameters a local fn can't have
// (said already) as invalid types: its signature and body don't say
// they're unknown.
func (c *Checker) invalidTypeParams(tps []*syntax.TypeParam) {
	for _, p := range tps {
		c.scope.Insert(&TypeName{object: object{p.Name.Name, p.Name.Start}, Kind: ParamType, Type: Invalid, resolved: true})
	}
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
					if (cc.Kind == CtxComponent || cc.Kind == CtxStore || cc.Kind == CtxMethod || cc.Kind == CtxWorker) && cc.Comp == owner {
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
		c.warnf(lam, diag.ClosureKeepsItself, "%s calls itself through the variable it's stored in, which keeps it alive forever: declare it fn %s(…)", v.name, v.name)
	}
}
