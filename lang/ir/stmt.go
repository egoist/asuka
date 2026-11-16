package ir

import (
	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/syntax"
)

// stmts lowers statements; a defer protects the rest of its block.
func (b *fb) stmts(items []syntax.Item) {
	for i := 0; i < len(items); i++ {
		it := items[i]
		if d, ok := it.(*syntax.DeferStmt); ok {
			b.withDefer(d, items[i+1:])
			return
		}
		if _, ok := it.(*syntax.FuncDecl); ok {
			// The fns declared one after another: a group.
			j := i + 1
			for j < len(items) {
				if _, ok := items[j].(*syntax.FuncDecl); !ok {
					break
				}
				j++
			}
			b.localGroup(items[i:j])
			i = j - 1
			continue
		}
		b.stmt(it)
	}
}

func (b *fb) block(bl *syntax.Block) {
	if bl == nil {
		return
	}
	b.openScope()
	b.stmts(bl.Items)
	b.closeScope()
}

func (b *fb) stmt(it syntax.Item) {
	b.at(it)
	mark := b.mark()
	defer b.release(mark)
	switch s := it.(type) {
	case *syntax.LetDecl:
		t := b.typ(b.l.info.Lets[s])
		v := b.value(s.Value, -1)
		if s.Else != nil {
			b.letElse(s, v, t)
			return
		}
		b.bindLet(s.Pattern, v, t)
	case *syntax.AssignStmt:
		b.assignStmt(s)
	case *syntax.IfStmt:
		b.ifStmt(s)
	case *syntax.ForStmt:
		b.forStmt(s)
	case *syntax.WhileStmt:
		top, end := b.f.Here(), b.f.NewLabel()
		b.openScope()
		b.condJump(s.Cond, end)
		b.loops = append(b.loops, loop{brk: end, cont: top, defers: len(b.defers)})
		b.block(s.Body)
		b.loops = b.loops[:len(b.loops)-1]
		b.closeScope()
		b.jmp(top)
		b.f.Place(end)
	case *syntax.MatchStmt:
		subj := b.value(s.Subject, -1)
		st := b.rawType(s.Subject)
		end := b.f.NewLabel()
		for _, a := range b.l.platformArms(s.Subject, s.Arms) {
			next := b.f.NewLabel()
			b.openScope()
			b.testPattern(a.Pattern, subj, st, next)
			b.bindPattern(a.Pattern, subj, st)
			if a.Guard != nil {
				g := b.value(a.Guard, -1)
				b.emitJump(op("JF"), g, next)
			}
			switch body := a.Body.(type) {
			case *syntax.Block:
				b.block(body)
			case syntax.Item:
				b.stmt(body)
			}
			b.closeScope()
			b.jmp(end)
			b.f.Place(next)
		}
		b.f.Place(end)
	case *syntax.DoStmt:
		b.doStmt(s)
	case *syntax.ReturnStmt:
		b.returnValue(s.Value)
	case *syntax.ThrowStmt:
		b.throw(s.Value)
	case *syntax.BranchStmt:
		b.branch(s.Tok)
	case *syntax.EmitStmt:
		b.emitStmt(s)
	case *syntax.ExprStmt:
		b.exprStmt(s.X)
	case *syntax.SlotItem:
		// animate { … } in a statement: the call.
		if call := b.l.info.SlotCalls[s]; call != nil {
			b.exprStmt(call)
			return
		}
		b.l.errorf(it, "can't lower this statement")
	case *syntax.DeferStmt:
		b.withDefer(s, nil)
	default:
		b.l.errorf(it, "can't lower this statement")
	}
}

// exprStmt lowers an expression whose value isn't used.
func (b *fb) exprStmt(x syntax.Expr) {
	if call, ok := x.(*syntax.CallExpr); ok {
		b.callDiscard(call)
		return
	}
	b.value(x, -1)
}

// bindLet binds a let's pattern to v (a t).
func (b *fb) bindLet(p syntax.Pattern, v int, t check.Type) {
	switch p := p.(type) {
	case *syntax.BindPat:
		vr, _ := b.l.info.Defs[p.Name].(*check.Var)
		b.declare(vr, v)
	case *syntax.TuplePat:
		tt, ok := t.(*check.Tuple)
		if !ok {
			return
		}
		for i, e := range p.Elems {
			et := tt.Elems[i]
			r := b.temp(b.l.kind(et))
			b.emit(op("GETF"), r, v, i)
			b.bindLet(e, r, et)
		}
	}
}

// ---------------------------------------------------------------------
// Conditions

// condJump evaluates an if's or a while's condition and jumps to els
// when it's false (or the let doesn't match), binding the let's names; a
// chain's clauses in order, to the first that fails.
func (b *fb) condJump(c syntax.Expr, els Label) {
	if l, ok := c.(*syntax.CondList); ok {
		for _, x := range l.List {
			b.condJump(x, els)
		}
		return
	}
	lc, ok := c.(*syntax.LetCond)
	if !ok {
		if f := b.l.fold(c); f.known {
			b.effects(f)
			if !f.val {
				b.jmp(els)
			}
			return
		}
		r := b.value(c, -1)
		b.emitJump(op("JF"), r, els)
		return
	}
	v := b.value(lc.Value, -1)
	t := b.rawType(lc.Value)
	if bp, ok := lc.Pattern.(*syntax.BindPat); ok {
		b.emitJump(op("JNONE"), v, els)
		in := b.unbox(v, check.Unopt(t), -1)
		vr, _ := b.l.info.Defs[bp.Name].(*check.Var)
		b.declare(vr, in)
		return
	}
	// if let _ = x: x isn't none, as a view's if let has it.
	if _, ok := lc.Pattern.(*syntax.WildcardPat); ok && check.IsOptional(t) {
		b.emitJump(op("JNONE"), v, els)
		return
	}
	b.testPattern(lc.Pattern, v, t, els)
	b.bindPattern(lc.Pattern, v, t)
}

func (b *fb) ifStmt(s *syntax.IfStmt) {
	if f := b.l.foldCond(s.Cond); f.known {
		// Only the branch @platform takes.
		b.effects(f)
		if f.val {
			b.openScope()
			b.block(s.Then)
			b.closeScope()
			return
		}
		switch el := s.Else.(type) {
		case *syntax.IfStmt:
			b.ifStmt(el)
		case *syntax.Block:
			b.block(el)
		}
		return
	}
	els, end := b.f.NewLabel(), b.f.NewLabel()
	b.openScope()
	b.condJump(s.Cond, els)
	b.block(s.Then)
	b.closeScope()
	if s.Else != nil {
		b.jmp(end)
	}
	b.f.Place(els)
	switch el := s.Else.(type) {
	case *syntax.IfStmt:
		b.ifStmt(el)
	case *syntax.Block:
		b.block(el)
	}
	b.f.Place(end)
}

// ---------------------------------------------------------------------
// Loops

func (b *fb) forStmt(s *syntax.ForStmt) {
	fi := b.l.info.Fors[s]
	if fi.Kind == check.ForStream {
		b.forAwait(s, fi)
		return
	}
	b.openScope()
	defer b.closeScope()
	top, cont, end := b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel()
	i := b.local(bc.KInt)
	var limit, seq, keys, vals int
	cond := b.local(bc.KBool)
	switch fi.Kind {
	case check.ForRange:
		b.value(fi.Seq, i)
		limit = b.local(bc.KInt)
		b.value(fi.End, limit)
	case check.ForSet:
		// The set as it is now (a local holds it, so a change in the
		// body copies it), entry by entry: set.next is the index of the
		// first live entry from i, or -1.
		seq = b.local(bc.KRef)
		b.value(fi.Seq, seq)
		b.loadInt(i, 0)
	case check.ForMap:
		m := b.value(fi.Seq, -1)
		keys = b.local(bc.KRef)
		vals = b.local(bc.KRef)
		b.callBTo("map.keys", keys, m)
		b.callBTo("map.values", vals, m)
		limit = b.local(bc.KInt)
		b.emit(op("LEN"), limit, keys)
		b.loadInt(i, 0)
	default:
		seq = b.local(bc.KRef)
		b.value(fi.Seq, seq)
		limit = b.local(bc.KInt)
		b.emit(op("LEN"), limit, seq)
		b.loadInt(i, 0)
	}
	b.f.Place(top)
	switch {
	case fi.Kind == check.ForSet:
		b.callBTo("set.next", i, seq, i)
		zero := b.konst(0)
		b.emit(op("LEI"), cond, zero, i)
	case fi.Kind == check.ForRange && fi.Incl:
		b.emit(op("LEI"), cond, i, limit)
	default:
		b.emit(op("LTI"), cond, i, limit)
	}
	b.emitJump(op("JF"), cond, end)
	b.openScope()
	switch fi.Kind {
	case check.ForRange:
		b.declare(fi.Item, i)
	case check.ForSet:
		x := b.temp(b.kind(fi.Elem))
		b.callBTo("set.elem", x, seq, i)
		b.declare(fi.Item, x)
	case check.ForMap:
		kt := fi.Elem.(*check.Tuple)
		k := b.temp(b.kind(kt.Elems[0]))
		b.emit(op("IDX"), k, keys, i)
		v := b.temp(b.kind(kt.Elems[1]))
		b.emit(op("IDX"), v, vals, i)
		b.declare(fi.Index, k)
		b.forItem(fi, v, kt.Elems[1])
	case check.ForEnumerated:
		x := b.temp(b.kind(fi.Elem))
		b.emit(op("IDX"), x, seq, i)
		b.declare(fi.Index, i)
		b.forItem(fi, x, fi.Elem)
	default:
		x := b.temp(b.kind(fi.Elem))
		b.emit(op("IDX"), x, seq, i)
		if _, ok := fi.Pat.(*syntax.TuplePat); ok {
			b.bindLet(fi.Pat, x, b.typ(fi.Elem))
		} else {
			b.declare(fi.Item, x)
		}
	}
	b.loops = append(b.loops, loop{brk: end, cont: cont, defers: len(b.defers)})
	b.block(s.Body)
	b.loops = b.loops[:len(b.loops)-1]
	b.closeScope()
	b.f.Place(cont)
	b.emit(op("ADDIK"), i, i, 1)
	b.jmp(top)
	b.f.Place(end)
}

// forItem binds the item of a for over pairs (an index's or a key's
// item, x of type t): its name, or a tuple pattern's names
// (for (i, (x, y)) in points.enumerated()).
func (b *fb) forItem(fi *check.ForInfo, x int, t check.Type) {
	if tp, ok := fi.Pat.(*syntax.TuplePat); ok && len(tp.Elems) == 2 {
		if inner, ok := tp.Elems[1].(*syntax.TuplePat); ok {
			b.bindLet(inner, x, b.typ(t))
			return
		}
	}
	b.declare(fi.Item, x)
}

// ---------------------------------------------------------------------
// Patterns

// testPattern jumps to fail unless v (a t) matches p.
func (b *fb) testPattern(p syntax.Pattern, v int, t check.Type, fail Label) {
	switch p := p.(type) {
	case *syntax.WildcardPat, *syntax.BindPat, nil:
	case *syntax.LitPat:
		if _, ok := p.Value.(*syntax.NoneLit); ok {
			b.emitJump(op("JSOME"), v, fail)
			return
		}
		x := v
		if check.IsOptional(t) {
			b.emitJump(op("JNONE"), v, fail)
			x = b.unbox(v, check.Unopt(t), -1)
		}
		lit := b.value(p.Value, -1)
		et := check.Unopt(t)
		r := b.binop(syntax.EQL, x, lit, et, et, check.Bool, -1)
		b.emitJump(op("JF"), r, fail)
	case *syntax.TuplePat:
		tt, ok := t.(*check.Tuple)
		if !ok {
			return
		}
		for i, e := range p.Elems {
			if isIrrefutable(e) {
				continue
			}
			r := b.temp(b.l.kind(tt.Elems[i]))
			b.emit(op("GETF"), r, v, i)
			b.testPattern(e, r, tt.Elems[i], fail)
		}
	case *syntax.OrPat:
		ok := b.f.NewLabel()
		for i, a := range p.Alts {
			if i == len(p.Alts)-1 {
				b.testPattern(a, v, t, fail)
				break
			}
			next := b.f.NewLabel()
			b.testPattern(a, v, t, next)
			b.jmp(ok)
			b.f.Place(next)
		}
		b.f.Place(ok)
	case *syntax.CasePat:
		if fields, ok := b.l.info.FieldPats[p]; ok {
			// A struct's: each field it names.
			x := v
			et := check.Unopt(t)
			if check.IsOptional(t) {
				b.emitJump(op("JNONE"), v, fail)
				x = b.unbox(v, et, -1)
			}
			n, _ := et.(*check.Named)
			for i, a := range p.Args {
				if fields[i] == nil || n == nil || isIrrefutable(a) {
					continue
				}
				r := b.getField(x, n, fields[i], -1)
				b.testPattern(a, r, n.Subst().Apply(fields[i].Type), fail)
			}
			return
		}
		k := b.l.info.Cases[p]
		if k == nil {
			return
		}
		x := v
		et := check.Unopt(t)
		if check.IsOptional(t) {
			b.emitJump(op("JNONE"), v, fail)
			x = b.unbox(v, et, -1)
		}
		if tn := b.l.info.ErrorCases[p]; tn != nil {
			// An Error matched by an error enum's case: of that enum
			// first (as catch T.case tests it).
			b.errorOf(x, tn, fail)
			et = tn.Type
		}
		n, _ := et.(*check.Named)
		c := b.caseOf(x, n)
		want := b.konst(int64(k.Index + caseBase(n)))
		eq := b.temp(bc.KBool)
		b.emit(op("EQI"), eq, c, want)
		b.emitJump(op("JF"), eq, fail)
		for i, a := range p.Args {
			if isIrrefutable(a) {
				continue
			}
			pt := b.payloadType(n, k, i)
			r := b.payload(x, n, k, i)
			b.testPattern(a, r, pt, fail)
		}
	}
}

// errorOf jumps to fail unless error e is a tn: a value of the app's
// (ECASE), whose type is tn (its __etype).
func (b *fb) errorOf(e int, tn *check.TypeName, fail Label) {
	c := b.temp(bc.KInt)
	b.emit(op("ECASE"), c, e)
	user := b.temp(bc.KBool)
	b.emit(op("NEI"), user, c, b.konst(0))
	b.emitJump(op("JF"), user, fail)
	et := b.temp(bc.KInt)
	blk := b.regs([]bc.Kind{bc.KRef})
	b.move(blk, e)
	b.emit(op("CALLI"), et, b.l.str("__etype"), blk, 1)
	b.releaseRegs(blk, []bc.Kind{bc.KRef})
	id := b.konst(int64(b.l.errIDs[b.l.shape(tn.Type)]))
	eq := b.temp(bc.KBool)
	b.emit(op("EQI"), eq, et, id)
	b.emitJump(op("JF"), eq, fail)
}

// caseOf is the case index of enum value x.
func (b *fb) caseOf(x int, n *check.Named) int {
	if n != nil && check.IsLoading(n) {
		return b.callB(uiLoadState, bc.KInt, x)
	}
	if n == nil || !n.Obj.RefEnum() {
		return x
	}
	c := b.temp(bc.KInt)
	b.emit(op("ECASE"), c, x)
	return c
}

func (b *fb) payloadType(n *check.Named, k *check.Case, i int) check.Type {
	pt := n.Subst().Apply(k.Params[i])
	if pt == nil {
		return b.l.prog.Prelude.Scope.Lookup("Error").(*check.TypeName).Type
	}
	return pt
}

// payload reads payload field i of case k of enum value x.
func (b *fb) payload(x int, n *check.Named, k *check.Case, i int) int {
	pt := b.payloadType(n, k, i)
	if check.IsLoading(n) {
		if k.Name == "failed" {
			return b.callB(uiLoadError, bc.KRef, x)
		}
		v := b.callB(uiLoadValue, bc.KRef, x)
		return b.unbox(v, pt, -1)
	}
	r := b.temp(b.l.kind(pt))
	b.emit(op("GETP"), r, x, i)
	return r
}

func isIrrefutable(p syntax.Pattern) bool {
	switch p := p.(type) {
	case *syntax.WildcardPat, *syntax.BindPat:
		return true
	case *syntax.TuplePat:
		for _, e := range p.Elems {
			if !isIrrefutable(e) {
				return false
			}
		}
		return true
	}
	return false
}

// bindPattern declares p's names from v (which matched it).
func (b *fb) bindPattern(p syntax.Pattern, v int, t check.Type) {
	switch p := p.(type) {
	case *syntax.BindPat:
		vr, _ := b.l.info.Defs[p.Name].(*check.Var)
		b.declare(vr, v)
	case *syntax.TuplePat:
		tt, ok := t.(*check.Tuple)
		if !ok {
			return
		}
		for i, e := range p.Elems {
			if !hasBinding(e) {
				continue
			}
			r := b.temp(b.l.kind(tt.Elems[i]))
			b.emit(op("GETF"), r, v, i)
			b.bindPattern(e, r, tt.Elems[i])
		}
	case *syntax.CasePat:
		x := v
		et := check.Unopt(t)
		if check.IsOptional(t) && hasBinding(p) {
			x = b.unbox(v, et, -1)
		}
		if tn := b.l.info.ErrorCases[p]; tn != nil {
			et = tn.Type
		}
		n, _ := et.(*check.Named)
		if fields, ok := b.l.info.FieldPats[p]; ok {
			for i, a := range p.Args {
				if fields[i] == nil || n == nil || !hasBinding(a) {
					continue
				}
				r := b.getField(x, n, fields[i], -1)
				b.bindPattern(a, r, n.Subst().Apply(fields[i].Type))
			}
			return
		}
		k := b.l.info.Cases[p]
		if k == nil {
			return
		}
		for i, a := range p.Args {
			if !hasBinding(a) {
				continue
			}
			r := b.payload(x, n, k, i)
			b.bindPattern(a, r, b.payloadType(n, k, i))
		}
	}
}

func hasBinding(p syntax.Pattern) bool {
	found := false
	syntax.Inspect(p, func(n syntax.Node) bool {
		if _, ok := n.(*syntax.BindPat); ok {
			found = true
		}
		return !found
	})
	return found
}

// ---------------------------------------------------------------------
// Assignments and changes in place

// An lvalue's path, its indexes and stores computed first.
type lvalue struct {
	x     syntax.Expr
	kind  int // what the root is
	v     *check.Var
	m     *check.Member
	mtype check.Type // m's type where it's reached (a generic store's, instantiated)
	g     *check.Global
	cell  int // a store member's cell
	inner *lvalue
	idx   int // an index's register
	field *check.Field
	named *check.Named
	isMap bool
}

const (
	lvVar = iota
	lvBoxed
	lvCell
	lvField
	lvIndex
)

// prepare computes an lvalue's indexes and store values, before any
// state is taken out.
func (b *fb) prepare(x syntax.Expr) *lvalue {
	switch x := x.(type) {
	case *syntax.ParenExpr:
		return b.prepare(x.X)
	case *syntax.Ident:
		switch o := b.l.info.Uses[x].(type) {
		case *check.Var:
			if o.Boxed() {
				return &lvalue{x: x, kind: lvBoxed, v: o}
			}
			return &lvalue{x: x, kind: lvVar, v: o}
		case *check.Member:
			return &lvalue{x: x, kind: lvCell, cell: b.memberRef(o, x), m: o}
		case *check.Global:
			return &lvalue{x: x, kind: lvCell, cell: b.globalCell(o), g: o}
		}
	case *syntax.MemberExpr:
		sel := b.l.info.Selections[x]
		if sel != nil && sel.Kind == check.SelPkg {
			if g, ok := sel.Obj.(*check.Global); ok {
				return &lvalue{x: x, kind: lvCell, cell: b.globalCell(g), g: g}
			}
		}
		if sel != nil && sel.Kind == check.SelStoreMember {
			s := b.value(x.X, -1)
			return &lvalue{x: x, kind: lvCell, cell: b.storeMemberRef(s, sel.Member), m: sel.Member, mtype: memberTypeIn(b.typ(sel.Recv), sel.Member)}
		}
		if sel != nil && sel.Kind == check.SelField {
			n, _ := b.typ(sel.Recv).(*check.Named)
			return &lvalue{x: x, kind: lvField, inner: b.prepare(x.X), field: sel.Field, named: n}
		}
	case *syntax.IndexExpr:
		inner := b.prepare(x.X)
		_, isMap := b.rawType(x.X).(*check.Map)
		return &lvalue{x: x, kind: lvIndex, inner: inner, idx: b.value(x.Index, -1), isMap: isMap}
	}
	b.l.errorf(x, "can't change this")
	return &lvalue{x: x, kind: lvVar}
}

// update runs fn on a register holding lv's value, which fn may change
// in place, and stores it back.
func (b *fb) update(lv *lvalue, fn func(cur int)) {
	switch lv.kind {
	case lvVar:
		r, ok := b.vars[lv.v]
		if !ok {
			b.l.errorf(lv.x, "internal: no register for %s", lv.v.Name())
			return
		}
		fn(r)
	case lvBoxed:
		box := b.vars[lv.v]
		cur := b.temp(b.kind(lv.v.Type))
		b.emit(op("VGET"), cur, box)
		fn(cur)
		b.emit(op("VSET"), box, cur)
	case lvCell:
		k := b.cellKind(lv)
		cur := b.temp(k)
		b.emit(op("CTAKE"), cur, lv.cell)
		b.protect(func() { fn(cur) }, func(e int) {
			b.emit(op("CPUT"), lv.cell, cur)
		})
		b.emit(op("CPUT"), lv.cell, cur)
	case lvField:
		b.update(lv.inner, func(rx int) {
			t := b.getField(rx, lv.named, lv.field, -1)
			fn(t)
			b.emit(op("SETF"), rx, lv.field.Index, t)
		})
	case lvIndex:
		b.update(lv.inner, func(rx int) {
			t := b.temp(b.kind(b.l.info.Types[lv.x]))
			b.emit(op("IDX"), t, rx, lv.idx)
			fn(t)
			b.emit(op("SETIDX"), rx, lv.idx, t)
		})
	}
}

func (b *fb) cellKind(lv *lvalue) bc.Kind {
	if lv.mtype != nil {
		return b.l.kind(lv.mtype)
	}
	if lv.m != nil {
		return b.kind(lv.m.Type)
	}
	if lv.g != nil {
		return b.kind(lv.g.Type)
	}
	return bc.KRef
}

// set stores val into lv.
func (b *fb) set(lv *lvalue, val int) {
	switch lv.kind {
	case lvVar, lvBoxed:
		b.writeVar(lv.v, val, lv.x)
	case lvCell:
		b.emit(op("CSET"), lv.cell, val)
	case lvField:
		if lv.named != nil && lv.named.Obj.IsError {
			b.l.errorf(lv.x, "an error's fields can't be changed: make a new one")
			return
		}
		b.update(lv.inner, func(rx int) { b.emit(op("SETF"), rx, lv.field.Index, val) })
	case lvIndex:
		if lv.isMap {
			b.update(lv.inner, func(rx int) { b.emit(op("MSET"), rx, lv.idx, val) })
			return
		}
		b.update(lv.inner, func(rx int) { b.emit(op("SETIDX"), rx, lv.idx, val) })
	}
}

// get reads lv's current value.
func (b *fb) get(lv *lvalue) int {
	return b.value(lv.x, -1)
}

func (b *fb) assignStmt(s *syntax.AssignStmt) {
	lt := b.rawType(s.Target)
	switch s.Op {
	case syntax.ASSIGN:
		v := b.value(s.Value, -1)
		b.set(b.prepare(s.Target), v)
	case syntax.COALESCE_ASSIGN:
		lv := b.prepare(s.Target)
		cur := b.get(lv)
		end := b.f.NewLabel()
		b.emitJump(op("JSOME"), cur, end)
		v := b.value(s.Value, -1)
		b.set(lv, v)
		b.f.Place(end)
	default:
		v := b.value(s.Value, -1)
		o := s.Op.AssignOp()
		lv := b.prepare(s.Target)
		vt := b.typeOf(s.Value)
		if lv.kind == lvField || lv.kind == lvIndex && !lv.isMap {
			// Read and write the element where it is.
			b.update(lv.inner, func(rx int) {
				var cur int
				if lv.kind == lvField {
					cur = b.getField(rx, lv.named, lv.field, -1)
				} else {
					cur = b.temp(b.kind(lt))
					b.emit(op("IDX"), cur, rx, lv.idx)
				}
				r := b.binop(o, cur, v, lt, vt, lt, cur)
				if lv.kind == lvField {
					b.emit(op("SETF"), rx, lv.field.Index, r)
				} else {
					b.emit(op("SETIDX"), rx, lv.idx, r)
				}
			})
			return
		}
		cur := b.get(lv)
		r := b.binop(o, cur, v, lt, vt, lt, -1)
		b.set(lv, r)
	}
}

// ---------------------------------------------------------------------
// Errors

// protect runs body with a handler: on an error, onError (with the
// error's register) runs, and the error goes on.
func (b *fb) protect(body func(), onError func(e int)) {
	start, end, target, after := b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel()
	b.f.Place(start)
	body()
	b.f.Place(end)
	b.jmp(after)
	b.f.Place(target)
	e := b.temp(bc.KRef)
	b.emit(op("CATCH"), e)
	onError(e)
	b.emit(op("THROW"), e)
	b.f.Place(after)
	b.f.Handlers = append(b.f.Handlers, Handler{start, end, target})
}

// classify sorts a caught error: a user error (an enum, ECASE > 0), a
// runtime error, or the cancellation, which jumps to cancel.
func (b *fb) classify(e int, cancel Label) (isUser int) {
	c := b.temp(bc.KInt)
	b.emit(op("ECASE"), c, e)
	user := b.temp(bc.KBool)
	z := b.konst(0)
	b.emit(op("NEI"), user, c, z)
	known := b.f.NewLabel()
	b.emitJump(op("JT"), user, known)
	// A runtime error equals the error made from its own message; the
	// cancellation (another shape) doesn't.
	m := b.callB("error.message", bc.KRef, e)
	p := b.callB("error", bc.KRef, m)
	rt := b.temp(bc.KBool)
	b.emit(op("EQ"), rt, e, p, int(bc.KRef))
	b.emitJump(op("JF"), rt, cancel)
	b.f.Place(known)
	return user
}

func (b *fb) doStmt(s *syntax.DoStmt) {
	b.catchRegion(s.Catches, func() { b.block(s.Body) }, func(cl *syntax.CatchClause) { b.block(cl.Body) })
}

// catchRegion lowers body under catch clauses: when it throws, the first
// clause that takes the error runs (clause, its names bound), and an
// error no clause takes (or the cancellation) goes on.
func (b *fb) catchRegion(catches []*syntax.CatchClause, body func(), clause func(*syntax.CatchClause)) {
	start, end, target, after := b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel()
	b.f.Place(start)
	body()
	b.f.Place(end)
	b.jmp(after)
	b.f.Place(target)
	mark := b.mark()
	e := b.newReg(bc.KRef)
	b.emit(op("CATCH"), e)
	rethrow := b.f.NewLabel()
	user := b.classify(e, rethrow)
	for _, cl := range catches {
		ci := b.l.info.Catches[cl]
		next := b.f.NewLabel()
		b.openScope()
		if ci != nil && !ci.All && ci.Type != nil {
			b.emitJump(op("JF"), user, next)
			// Its type: __etype is the shape's id.
			et := b.temp(bc.KInt)
			blk := b.regs([]bc.Kind{bc.KRef})
			b.move(blk, e)
			b.emit(op("CALLI"), et, b.l.str("__etype"), blk, 1)
			b.releaseRegs(blk, []bc.Kind{bc.KRef})
			sh := b.l.shape(ci.Type.Type)
			id := b.konst(int64(b.l.errIDs[sh]))
			eq := b.temp(bc.KBool)
			b.emit(op("EQI"), eq, et, id)
			b.emitJump(op("JF"), eq, next)
			if ci.Case != nil {
				if p, ok := cl.Pattern.(*syntax.CasePat); ok {
					b.testPattern(p, e, ci.Type.Type, next)
					b.bindPattern(p, e, ci.Type.Type)
				}
			}
		}
		if ci != nil && ci.Var != nil {
			b.declare(ci.Var, e)
		}
		clause(cl)
		b.closeScope()
		b.jmp(after)
		b.f.Place(next)
	}
	b.f.Place(rethrow)
	b.emit(op("THROW"), e)
	b.release(mark)
	b.f.Place(after)
	b.f.Handlers = append(b.f.Handlers, Handler{start, end, target})
}

// catchExpr is x catch … { … }: x's value, or the value of the clause
// that takes its error. A void one is a statement's.
func (b *fb) catchExpr(e *syntax.CatchExpr, dst int) int {
	t := b.rawType(e)
	k := b.kind(t)
	if k == bc.KVoid {
		b.catchRegion(e.Catches, func() { b.exprStmt(e.X) }, func(cl *syntax.CatchClause) { b.block(cl.Body) })
		return -1
	}
	r := b.out(dst, k)
	b.catchRegion(e.Catches, func() { b.value(e.X, r) }, func(cl *syntax.CatchClause) { b.valueBlock(cl.Body, r) })
	return r
}

// tryOptional is try? x: none when x throws.
func (b *fb) tryOptional(e *syntax.UnaryExpr, dst int) int {
	t := b.rawType(e)
	inner := b.rawType(e.X)
	var r int = -1
	if t != check.Void {
		r = b.out(dst, bc.KRef)
	}
	start, end, target, after := b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel()
	b.f.Place(start)
	if r >= 0 {
		v := b.value(e.X, -1)
		b.toOptional(v, inner, t, r)
	} else {
		b.exprStmt(e.X)
	}
	b.f.Place(end)
	b.jmp(after)
	b.f.Place(target)
	ev := b.temp(bc.KRef)
	b.emit(op("CATCH"), ev)
	cancel := b.f.NewLabel()
	b.classify(ev, cancel)
	if r >= 0 {
		b.emit(op("KNONE"), r)
	}
	b.jmp(after)
	b.f.Place(cancel)
	b.emit(op("THROW"), ev)
	b.f.Place(after)
	b.f.Handlers = append(b.f.Handlers, Handler{start, end, target})
	return r
}

// withDefer lowers the rest of a block with a defer: its body runs when
// the block ends, by any way.
func (b *fb) withDefer(d *syntax.DeferStmt, rest []syntax.Item) {
	b.defers = append(b.defers, d.Body)
	n := len(b.defers)
	b.protect(func() {
		b.stmts(rest)
	}, func(e int) {
		b.runDefer(d.Body)
	})
	b.defers = b.defers[:n-1]
	b.runDefer(d.Body)
}

// runDefers runs the defers in effect down to depth (for return,
// break and continue).
func (b *fb) runDefers(depth int) {
	saved := b.defers
	for i := len(b.defers) - 1; i >= depth; i-- {
		body := b.defers[i]
		b.defers = saved[:i]
		b.runDefer(body)
	}
	b.defers = saved
}

func (b *fb) runDefer(body *syntax.Block) {
	savedLoops := b.loops
	b.loops = nil
	b.block(body)
	b.loops = savedLoops
}

// emitStmt raises an event: calls its handler, when the caller gave one.
func (b *fb) emitStmt(s *syntax.EmitStmt) {
	m := b.l.info.Emits[s]
	if m == nil {
		return
	}
	h := b.memberRef(m, s)
	skip := b.f.NewLabel()
	b.emitJump(op("JNONE"), h, skip)
	var vals []int
	var ks []bc.Kind
	if s.Args != nil {
		for i, a := range s.Args.List {
			v := b.value(a.Value, -1)
			vals = append(vals, v)
			ks = append(ks, b.kind(m.Params[i].Type))
		}
	}
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, v)
	}
	res := b.temp(bc.KRef)
	b.emit(op("CALLC"), res, h, blk, len(vals))
	b.releaseRegs(blk, ks)
	b.f.Place(skip)
}

// forAwait is for await x in s: await stream.next (whether a value came:
// false at the end; it throws what the producer threw), take it with
// stream.value; leaving the loop closes the stream, which cancels its
// producer.
func (b *fb) forAwait(s *syntax.ForStmt, fi *check.ForInfo) {
	b.openScope()
	defer b.closeScope()
	top, end := b.f.NewLabel(), b.f.NewLabel()
	st := b.local(bc.KRef)
	b.value(fi.Seq, st)
	more := b.local(bc.KBool)
	// An error leaving the loop (the body's, the producer's) closes the
	// stream too, then goes on.
	b.protect(func() {
		b.f.Place(top)
		f := b.callB("stream.next", bc.KRef, st)
		b.emit(op("AWAIT"), more, f)
		b.emitJump(op("JF"), more, end)
		b.openScope()
		x := b.temp(b.kind(fi.Elem))
		b.callBTo("stream.value", x, st)
		if _, ok := fi.Pat.(*syntax.TuplePat); ok {
			b.bindLet(fi.Pat, x, b.typ(fi.Elem))
		} else {
			b.declare(fi.Item, x)
		}
		b.loops = append(b.loops, loop{brk: end, cont: top, defers: len(b.defers)})
		b.block(s.Body)
		b.loops = b.loops[:len(b.loops)-1]
		b.closeScope()
		b.jmp(top)
	}, func(int) {
		b.callBTo("stream.close", bc.None, st)
	})
	b.f.Place(end)
	b.callBTo("stream.close", bc.None, st)
}

// memberTypeIn is m's type in a store of type recv: a generic store's
// with its type arguments.
func memberTypeIn(recv check.Type, m *check.Member) check.Type {
	if n, ok := check.Unopt(recv).(*check.Named); ok && len(n.Args) > 0 {
		return n.Subst().Apply(m.Type)
	}
	return m.Type
}

// ---------------------------------------------------------------------
// let … else and jumps

// letElse binds let pattern = v (a t) else { … }: the else block (which
// leaves) when the pattern doesn't match, or a name's optional is none.
func (b *fb) letElse(s *syntax.LetDecl, v int, t check.Type) {
	els, ok := b.f.NewLabel(), b.f.NewLabel()
	if bp, name := s.Pattern.(*syntax.BindPat); name {
		b.emitJump(op("JNONE"), v, els)
		in := b.unbox(v, check.Unopt(t), -1)
		vr, _ := b.l.info.Defs[bp.Name].(*check.Var)
		b.declare(vr, in)
	} else {
		b.testPattern(s.Pattern, v, t, els)
		b.bindPattern(s.Pattern, v, t)
	}
	b.jmp(ok)
	b.f.Place(els)
	b.block(s.Else)
	b.f.Place(ok)
}

// throw throws value's error.
func (b *fb) throw(value syntax.Expr) {
	v := b.value(value, -1)
	t := b.temp(bc.KRef)
	b.move(t, v)
	b.emit(op("THROW"), t)
}

// returnValue returns value (nil: nothing), running the defers in
// effect.
func (b *fb) returnValue(value syntax.Expr) {
	var r int = -1
	k := b.f.Ret
	if value != nil {
		r = b.value(value, -1)
		if len(b.defers) > 0 && k == bc.KRef {
			// The defers run after the value is computed: keep it.
			t := b.temp(bc.KRef)
			b.move(t, r)
			r = t
		}
	}
	b.runDefers(0)
	b.ret(r, k)
}

// branch is break or continue: the defers of the loop's body, then the
// jump.
func (b *fb) branch(tok syntax.Token) {
	if len(b.loops) == 0 {
		return
	}
	lp := b.loops[len(b.loops)-1]
	b.runDefers(lp.defers)
	if tok == syntax.BREAK {
		b.jmp(lp.brk)
	} else {
		b.jmp(lp.cont)
	}
}

// jumpExpr is throw, return, break or continue as a value: it leaves, so
// what it gives is a register of the kind expected that's never read.
func (b *fb) jumpExpr(e *syntax.JumpExpr, dst int) int {
	switch e.Tok {
	case syntax.THROW:
		b.throw(e.Value)
	case syntax.RETURN:
		b.returnValue(e.Value)
	default:
		b.branch(e.Tok)
	}
	return dst
}

// ---------------------------------------------------------------------
// Local fns

// localGroup lowers a group of local fns (check.LocalGroup): each a
// function taking its parameters, then the group's captures as the code
// around holds them (a var's box, a value), then the env when they use
// it. A call of one, from anywhere it's in scope, itself and its
// siblings included, is a CALL passing them on: no closure is made, and
// none holds another or itself, so recursion makes no cycle. Only a use
// as a value makes a closure (localFnValue).
func (b *fb) localGroup(items []syntax.Item) {
	var fns []*check.LocalFn
	for _, it := range items {
		d := it.(*syntax.FuncDecl)
		if v, ok := b.l.info.Defs[d.Name].(*check.Var); ok && v.Fn != nil {
			fns = append(fns, v.Fn)
		}
	}
	if len(fns) == 0 {
		return
	}
	g := fns[0].Group
	if b.locals == nil {
		b.locals = map[*check.LocalFn]int{}
	}
	ncaps := len(g.Captures)
	if g.UsesEnv {
		ncaps++
	}
	fbs := make([]*fb, len(fns))
	for i, lf := range fns {
		c := b.l.newFunc(b.f.Name+"/"+lf.Decl.Name.Name, b.f.File, len(lf.Sig.Params)+ncaps, b.kind(lf.Sig.Result))
		c.subst = b.subst
		c.comp = b.comp
		c.line = b.line
		c.ctx = lf.Ctx
		c.locals = b.locals
		fbs[i] = c
		b.locals[lf] = c.idx
	}
	for i, lf := range fns {
		c := fbs[i]
		var params []int
		for _, p := range lf.Sig.Params {
			params = append(params, c.newReg(c.kind(p.Type)))
		}
		for _, v := range g.Captures {
			c.vars[v] = c.newReg(c.capKind(v))
		}
		if g.UsesEnv {
			c.envReg = c.newReg(bc.KRef)
		}
		c.openScope()
		for i, p := range lf.Sig.Params {
			if p.Var == nil {
				continue
			}
			if p.Var.Boxed() {
				c.declare(p.Var, params[i])
			} else {
				c.vars[p.Var] = params[i]
			}
		}
		if lf.Sig.Throws {
			c.f.Flags |= bc.FThrows
		}
		d := lf.Decl
		c.at(d)
		switch {
		case d.Value != nil:
			k := c.f.Ret
			if k == bc.KVoid {
				c.exprStmt(d.Value)
				c.emit(op("RET0"))
			} else {
				mark := c.mark()
				c.ret(c.value(d.Value, -1), k)
				c.release(mark)
			}
		case d.Body != nil:
			c.stmts(d.Body.Items)
			c.ret(-1, c.f.Ret)
		}
		c.closeScope()
		if lf.Sig.Async || lf.Ctx.Awaits {
			c.f.Flags |= bc.FAsync
		}
	}
}

// capKind is the kind of the register a local fn's capture of v is
// passed in: as the code around holds it.
func (b *fb) capKind(v *check.Var) bc.Kind {
	if v.Boxed() || v.Kind == check.RowItem || v.Kind == check.ViewMemo {
		return bc.KRef
	}
	return b.kind(v.Type)
}

// localCaps are the registers a call of a local fn of g passes after its
// arguments: the group's captures as this function holds them (a
// spawn's copy of a boxed var boxed again), then the env.
func (b *fb) localCaps(g *check.LocalGroup, n syntax.Node) []int {
	var regs []int
	for _, v := range g.Captures {
		r, ok := b.vars[v]
		if !ok {
			b.l.errorf(n, "internal: %s has no register in %s", v.Name(), b.f.Name)
			r = b.valueReg(-1, b.capKind(v))
		} else if v.Boxed() && b.unboxed[v] {
			box := b.temp(bc.KRef)
			b.emit(op("VBOX"), box, r, int(b.kind(v.Type)))
			r = box
		}
		regs = append(regs, r)
	}
	if g.UsesEnv {
		regs = append(regs, b.env(n))
	}
	return regs
}

// localIndex is the function of local fn lf.
func (b *fb) localIndex(lf *check.LocalFn, n syntax.Node) (int, bool) {
	idx, ok := b.locals[lf]
	if !ok {
		b.l.errorf(n, "internal: local fn %s isn't lowered in %s", lf.Decl.Name.Name, b.f.Name)
	}
	return idx, ok
}

// localCall is a call of a local fn by name: a CALL with its arguments
// and its group's captures.
func (b *fb) localCall(e *syntax.CallExpr, call *check.Call, lf *check.LocalFn, dst int, discard bool) int {
	var vals []int
	for _, a := range call.Args {
		vals = append(vals, b.value(a, -1))
	}
	vals = append(vals, b.localCaps(lf.Group, e)...)
	idx, ok := b.localIndex(lf, e)
	k := b.kind(lf.Sig.Result)
	if !ok {
		return b.valueReg(-1, k)
	}
	return b.invoke(idx, vals, k, dst, discard)
}

// localFnValue is a local fn used as a value: a closure that captures
// its group's captures and calls it with them.
func (b *fb) localFnValue(lf *check.LocalFn, n syntax.Node) int {
	idx, ok := b.localIndex(lf, n)
	if !ok {
		return b.valueReg(-1, bc.KRef)
	}
	g := lf.Group
	var extra []extraCap
	for _, v := range g.Captures {
		r, ok := b.vars[v]
		if !ok {
			b.l.errorf(n, "internal: %s has no register in %s", v.Name(), b.f.Name)
			continue
		}
		if v.Boxed() && b.unboxed[v] {
			box := b.temp(bc.KRef)
			b.emit(op("VBOX"), box, r, int(b.kind(v.Type)))
			r = box
		}
		extra = append(extra, extraCap{v, r})
	}
	var params []*check.Var
	var pkinds []bc.Kind
	for _, p := range lf.Sig.Params {
		params = append(params, nil)
		pkinds = append(pkinds, b.kind(p.Type))
	}
	ret := b.kind(lf.Sig.Result)
	return b.makeClosure(closure{name: lf.Decl.Name.Name, params: params, pkinds: pkinds, extra: extra, env: g.UsesEnv, ret: ret, async: lf.Sig.Async, throws: true, body: func(c *fb) {
		var vals []int
		for i := range lf.Sig.Params {
			vals = append(vals, i)
		}
		for _, v := range g.Captures {
			vals = append(vals, c.vars[v])
		}
		if g.UsesEnv {
			vals = append(vals, c.env(n))
		}
		r := c.invoke(idx, vals, ret, -1, false)
		c.ret(r, ret)
	}})
}
