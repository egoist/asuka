package ir

import (
	"slices"

	bc "github.com/egoist/asuka/lang/bytecode"
	"github.com/egoist/asuka/lang/check"
	"github.com/egoist/asuka/lang/diag"
	"github.com/egoist/asuka/lang/syntax"
)

// stmts lowers statements; a defer protects the rest of its block.
func (b *fb) stmts(items []syntax.Item) { b.stmtsThen(items, nil) }

// stmtsThen lowers statements, then what then lowers (a block's value,
// its last expression), under the defers among them: those run after it.
func (b *fb) stmtsThen(items []syntax.Item, then func()) {
	for i := 0; i < len(items); i++ {
		it := items[i]
		if d, ok := it.(*syntax.DeferStmt); ok {
			b.withDefer(d, items[i+1:], then)
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
		if b.l.leaves(it) {
			// Nothing after it runs: it isn't lowered (fold.go).
			return
		}
	}
	if then != nil {
		then()
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
	b.cover(it)
	switch s := it.(type) {
	case *syntax.LetDecl:
		t := b.typ(b.l.info.Lets[s])
		v := b.value(s.Value, -1)
		b.dropTemps(mark, v)
		if s.Else != nil {
			b.letElse(s, v, t)
			return
		}
		b.bindLet(s.Pattern, v, t)
		if b.f.Kinds[v] == bc.KRef && slices.Contains(b.temps[mark:], v) {
			// The value's temp let go of once the names hold it: kept to
			// the statement's end, it would be a second reference until
			// the register's next use, and the local's first change in
			// place would copy it (a [bool] of 50 million, its 50 MB).
			b.emit(op("CLR"), v)
		}
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
		b.loops = append(b.loops, loop{brk: end, cont: top, defers: len(b.defers), node: s})
		b.block(s.Body)
		b.loops = b.loops[:len(b.loops)-1]
		b.closeScope()
		b.jmp(top)
		b.f.Place(end)
	case *syntax.MatchStmt:
		subj := b.value(s.Subject, -1)
		st := b.rawType(s.Subject)
		end := b.f.NewLabel()
		for _, a := range b.matchArms(s.Subject, s.Arms) {
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
	case *syntax.MeasureStmt:
		b.measure(s)
	case *syntax.ReturnStmt:
		b.returnValue(s.Value)
	case *syntax.ThrowStmt:
		b.throw(s.Value)
	case *syntax.BranchStmt:
		b.branch(s.Tok, s.Label)
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
		b.l.errorf(it, diag.NotSupported, "can't lower this statement")
	case *syntax.DeferStmt:
		b.withDefer(s, nil, nil)
	case *syntax.CleanupStmt:
		b.cleanupStmt(s)
	default:
		b.l.errorf(it, diag.NotSupported, "can't lower this statement")
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

// tupleElem is element i (a t) of the tuple v, in a temp; none (-1)
// for a void one, which holds nothing.
func (b *fb) tupleElem(v, i int, t check.Type) int {
	k := b.l.kind(t)
	if k == bc.KVoid {
		return -1
	}
	r := b.temp(k)
	b.emit(op("GETF"), r, v, i)
	return r
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
			b.bindLet(e, b.tupleElem(v, i, et), et)
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
			if b.letNever(x) {
				break // the clauses after it are never reached
			}
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
		mark := b.mark()
		r := b.value(c, -1)
		b.dropTemps(mark) // the body may change what the condition read
		b.emitJump(op("JF"), r, els)
		return
	}
	if bp, ok := lc.Pattern.(*syntax.BindPat); ok {
		if in, ok := b.pathMapRead(lc.Value, b.kind(check.Unopt(b.rawType(lc.Value))), -1, els); ok {
			// if let v = m[k] of a state's map: its key followed.
			vr, _ := b.l.info.Defs[bp.Name].(*check.Var)
			b.declare(vr, in)
			return
		}
		if m, key, ok := b.mapRead(lc.Value); ok {
			// if let v = m[k]: the value read as it is, no optional made.
			in := b.temp(b.kind(check.Unopt(b.rawType(lc.Value))))
			b.emit(op("MGETJ"), in, m, key, int(els))
			vr, _ := b.l.info.Defs[bp.Name].(*check.Var)
			b.declare(vr, in)
			return
		}
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

// letNever reports a condition that's never true as this instantiation
// is compiled: a let clause whose type test never passes (a type
// parameter's value that's another type here), or a chain with one.
func (b *fb) letNever(c syntax.Expr) bool {
	switch x := c.(type) {
	case *syntax.CondList:
		for _, y := range x.List {
			if b.letNever(y) {
				return true
			}
		}
	case *syntax.LetCond:
		return b.patternNever(x.Pattern)
	}
	return false
}

// patternNever reports a pattern that never matches as this
// instantiation is compiled: a type test, in it, of a type parameter's
// value that's another type here.
func (b *fb) patternNever(p syntax.Pattern) bool {
	switch p := p.(type) {
	case *syntax.TypedPat:
		if tt := b.l.info.TypeTests[p]; tt != nil {
			never, _ := b.testedStatically(tt)
			return never
		}
	case *syntax.CasePat:
		if tt := b.l.info.TypeTests[p]; tt != nil {
			if never, _ := b.testedStatically(tt); never {
				return true
			}
		}
		for _, a := range p.Args {
			if b.patternNever(a) {
				return true
			}
		}
	case *syntax.TuplePat:
		for _, e := range p.Elems {
			if b.patternNever(e) {
				return true
			}
		}
	case *syntax.OrPat:
		for _, a := range p.Alts {
			if !b.patternNever(a) {
				return false
			}
		}
		return len(p.Alts) > 0
	}
	return false
}

// patternAlways reports a type test's pattern that always matches as
// this instantiation is compiled: of a type parameter's value that's
// that type here, its parts matching anything.
func (b *fb) patternAlways(p syntax.Pattern) bool {
	var tt *check.TypeTest
	var args []syntax.Pattern
	switch p := p.(type) {
	case *syntax.TypedPat:
		tt = b.l.info.TypeTests[p]
	case *syntax.CasePat:
		if _, ok := b.l.info.FieldPats[p]; ok {
			tt, args = b.l.info.TypeTests[p], p.Args
		}
	}
	if tt == nil {
		return false
	}
	if _, always := b.testedStatically(tt); !always {
		return false
	}
	for _, a := range args {
		switch a.(type) {
		case *syntax.BindPat, *syntax.WildcardPat:
		default:
			return false
		}
	}
	return true
}

// matchArms are the arms of a match that may run as this instantiation
// is compiled: @platform's (platformArms), without those whose type test
// never passes, up to the first whose always does (without a guard).
func (b *fb) matchArms(subject syntax.Expr, arms []*syntax.MatchArm) []*syntax.MatchArm {
	arms = b.l.platformArms(subject, arms)
	var out []*syntax.MatchArm
	for i, a := range arms {
		if b.patternNever(a.Pattern) {
			if out == nil {
				out = append([]*syntax.MatchArm{}, arms[:i]...)
			}
			continue
		}
		if out != nil {
			out = append(out, a)
		}
		if a.Guard == nil && b.patternAlways(a.Pattern) {
			if out == nil {
				return arms[:i+1]
			}
			return out
		}
	}
	if out == nil {
		return arms
	}
	return out
}

func (b *fb) ifStmt(s *syntax.IfStmt) {
	if b.letNever(s.Cond) {
		// A type test this instantiation never passes: the else, after
		// what the condition still runs.
		els := b.f.NewLabel()
		b.openScope()
		b.condJump(s.Cond, els)
		b.closeScope()
		b.f.Place(els)
		switch el := s.Else.(type) {
		case *syntax.IfStmt:
			b.ifStmt(el)
		case *syntax.Block:
			b.block(el)
		}
		return
	}
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
	if fi.Kind == check.ForIter {
		b.forIter(s, fi)
		return
	}
	if fi.Kind == check.ForSeq {
		b.forSeq(s, fi)
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
		if fi.Chars {
			// A string's chars: its code points, an [char].
			b.callBTo("str.codepoints", seq, b.value(fi.Seq, -1))
		} else {
			b.value(fi.Seq, seq)
		}
		limit = b.local(bc.KInt)
		b.emit(op("LEN"), limit, seq)
		b.loadInt(i, 0)
	}
	// a..b is tested once before the first turn, and ends after the turn
	// of b: its counter never goes past b, so b may be Int.max.
	through := fi.Kind == check.ForRange && fi.Incl
	lt, le := "LTI", "LEI"
	if fi.Kind == check.ForRange && b.typ(fi.Elem) == check.U64 {
		lt, le = "LTU", "LEU" // a u64's range, ordered unsigned
	}
	if through {
		b.emit(op(le), cond, i, limit)
		b.emitJump(op("JF"), cond, end)
	}
	b.f.Place(top)
	switch {
	case through:
	case fi.Kind == check.ForSet:
		b.callBTo("set.next", i, seq, i)
		zero := b.konst(0)
		b.emit(op("LEI"), cond, zero, i)
	default:
		b.emit(op(lt), cond, i, limit)
	}
	if !through {
		b.emitJump(op("JF"), cond, end)
	}
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
	b.loops = append(b.loops, loop{brk: end, cont: cont, defers: len(b.defers), node: s})
	b.block(s.Body)
	b.loops = b.loops[:len(b.loops)-1]
	b.closeScope()
	b.f.Place(cont)
	if through {
		b.emit(op("EQI"), cond, i, limit)
		b.emitJump(op("JT"), cond, end)
	}
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
		// An optional's: its value, once it isn't none.
		tt, ok := check.Unopt(t).(*check.Tuple)
		if !ok {
			return
		}
		x := v
		if check.IsOptional(t) {
			b.emitJump(op("JNONE"), v, fail)
			x = b.unbox(v, tt, -1)
		}
		for i, e := range p.Elems {
			if isIrrefutable(e, tt.Elems[i]) {
				continue
			}
			b.testPattern(e, b.tupleElem(x, i, tt.Elems[i]), tt.Elems[i], fail)
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
	case *syntax.TypedPat:
		x := v
		if check.IsOptional(t) {
			b.emitJump(op("JNONE"), v, fail)
			x = b.unbox(v, check.Unopt(t), -1)
		}
		if tt := b.l.info.TypeTests[p]; tt != nil {
			b.testType(x, tt, fail)
		}
	case *syntax.CasePat:
		if fields, ok := b.l.info.FieldPats[p]; ok {
			// A struct's: each field it names.
			x := v
			et := check.Unopt(t)
			if check.IsOptional(t) {
				b.emitJump(op("JNONE"), v, fail)
				x = b.unbox(v, et, -1)
			}
			if tt := b.l.info.TypeTests[p]; tt != nil {
				// Of an interface's value: of this struct first.
				var typ *check.Named
				x, typ = b.testType(x, tt, fail)
				et = typ
			}
			n, _ := et.(*check.Named)
			for i, a := range p.Args {
				if fields[i] == nil || n == nil {
					continue
				}
				ft := n.Subst().Apply(fields[i].Type)
				if isIrrefutable(a, ft) {
					continue
				}
				r := b.getField(x, n, fields[i], -1)
				b.testPattern(a, r, ft, fail)
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
		if tt := b.l.info.TypeTests[p]; tt != nil {
			// Of an interface's value (an Error's: as catch T.case
			// tests it): of this enum first.
			var typ *check.Named
			x, typ = b.testType(x, tt, fail)
			et = typ
		}
		n, _ := et.(*check.Named)
		c := b.caseOf(x, n)
		want := b.konst(int64(k.Index + caseBase(n)))
		eq := b.temp(bc.KBool)
		b.emit(op("EQI"), eq, c, want)
		b.emitJump(op("JF"), eq, fail)
		for i, a := range p.Args {
			pt := b.payloadType(n, k, i)
			if isIrrefutable(a, pt) {
				continue
			}
			r := b.payload(x, n, k, i)
			b.testPattern(a, r, pt, fail)
		}
	}
}

// testType jumps to fail unless x, which type test tt is of (a value of
// an interface, or of a type parameter), is of tt's type, and returns x
// as one, and that type: an Error's by its __etype, another interface's
// by its shape (ISSHAPE); a type parameter's is known in this
// instantiation, so it always is (nothing to test) or never (a jump).
func (b *fb) testType(x int, tt *check.TypeTest, fail Label) (int, *check.Named) {
	typ, _ := b.typ(tt.Type).(*check.Named)
	of := b.typ(tt.Of)
	if n, ok := check.Unopt(of).(*check.Named); ok && n.Obj.Kind == check.InterfaceType {
		if check.IsErrorType(n) {
			if check.IsOptional(of) {
				b.emitJump(op("JNONE"), x, fail)
			}
			b.errorOf(x, typ, fail)
			return x, typ
		}
		// An optional interface's none is NULL, which no shape is.
		r := b.temp(bc.KBool)
		b.emit(op("ISSHAPE"), r, x, b.l.shape(typ))
		b.emitJump(op("JF"), r, fail)
		return x, typ
	}
	if check.Identical(check.Unopt(of), typ) {
		if check.IsOptional(of) {
			b.emitJump(op("JNONE"), x, fail)
		}
		return b.asTested(x, tt), typ
	}
	b.jmp(fail)
	return b.asTested(x, tt), typ
}

// asTested is x, which matched type test tt, as a value of its type: a
// type parameter's optional unwrapped, an interface's value as it is (a
// struct's or an enum's reference).
func (b *fb) asTested(x int, tt *check.TypeTest) int {
	of := b.typ(tt.Of)
	if n, ok := check.Unopt(of).(*check.Named); ok && n.Obj.Kind == check.InterfaceType {
		return x
	}
	if never, _ := b.testedStatically(tt); never {
		// Never reached: what's after the test (its names, its parts)
		// is compiled with a register of the tested type's kind.
		return b.valueReg(-1, b.kind(tt.Type))
	}
	if !check.IsOptional(of) {
		return x
	}
	return b.unbox(x, b.typ(tt.Type), -1)
}

// testedStatically says what type test tt comes to in this
// instantiation, when it's known as the program compiles (of a type
// parameter's value, which is a concrete type here): never matches, or
// always does (an optional's none aside).
func (b *fb) testedStatically(tt *check.TypeTest) (never, always bool) {
	of := check.Unopt(b.typ(tt.Of))
	if n, ok := of.(*check.Named); ok && n.Obj.Kind == check.InterfaceType {
		return false, false
	}
	if check.Identical(of, b.typ(tt.Type)) {
		return false, !check.IsOptional(b.typ(tt.Of))
	}
	return true, false
}

// errorOf jumps to fail unless error e is a tn: a value of the app's
// (ECASE), whose type is tn (its __etype).
func (b *fb) errorOf(e int, t *check.Named, fail Label) {
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
	id := b.konst(int64(b.l.errIDs[b.l.shape(t)]))
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

// isIrrefutable reports a pattern that matches any value of type t (a
// tuple pattern doesn't match an optional's none).
func isIrrefutable(p syntax.Pattern, t check.Type) bool {
	switch p := p.(type) {
	case *syntax.WildcardPat, *syntax.BindPat:
		return true
	case *syntax.TuplePat:
		tt, ok := t.(*check.Tuple)
		if !ok || len(tt.Elems) != len(p.Elems) {
			return false
		}
		for i, e := range p.Elems {
			if !isIrrefutable(e, tt.Elems[i]) {
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
	case *syntax.TypedPat:
		if !hasBinding(p) {
			return
		}
		x := v
		if check.IsOptional(t) {
			x = b.unbox(v, check.Unopt(t), -1)
		}
		if tt := b.l.info.TypeTests[p]; tt != nil {
			x = b.asTested(x, tt)
		}
		b.bindPattern(p.Pat, x, nil)
	case *syntax.TuplePat:
		tt, ok := check.Unopt(t).(*check.Tuple)
		if !ok {
			return
		}
		x := v
		if check.IsOptional(t) && hasBinding(p) {
			x = b.unbox(v, tt, -1)
		}
		for i, e := range p.Elems {
			if !hasBinding(e) {
				continue
			}
			b.bindPattern(e, b.tupleElem(x, i, tt.Elems[i]), tt.Elems[i])
		}
	case *syntax.CasePat:
		x := v
		et := check.Unopt(t)
		if check.IsOptional(t) && hasBinding(p) {
			x = b.unbox(v, et, -1)
		}
		if tt := b.l.info.TypeTests[p]; tt != nil {
			x = b.asTested(x, tt)
			et = b.typ(tt.Type)
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
	kind  int        // what the root is
	v     *check.Var // lvBoxed: nil for a worker's var (m), its box in cell
	m     *check.Member
	mtype check.Type // m's type where it's reached (a generic store's, instantiated)
	g     *check.Global
	cell  int // a store member's cell
	inner *lvalue
	idx   int // an index's register
	field *check.Field
	named *check.Named
	isMap bool
	def   syntax.Expr // m[k, default: v]'s v
	vt    check.Type  // lvSome's: the optional's value type
}

const (
	lvVar = iota
	lvBoxed
	lvCell
	lvField
	lvIndex
	lvModel  // a prop its component assigns: set whole only (the checker's)
	lvSome   // inner's value when it isn't none: x?.f's x (places.go)
	lvGlobal // a file's var: its global's slot in cell
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
			if o.Kind == check.VarMember {
				// A worker's var: its box, in the env.
				return &lvalue{x: x, kind: lvBoxed, cell: b.memberRef(o, x), m: o}
			}
			if o.Kind == check.PropMember {
				// A prop its component assigns: its model.
				return &lvalue{x: x, kind: lvModel, cell: b.memberRef(o, x), m: o}
			}
			return &lvalue{x: x, kind: lvCell, cell: b.memberRef(o, x), m: o}
		case *check.Global:
			if o.Kind == check.VarGlobal {
				return &lvalue{x: x, kind: lvGlobal, cell: b.l.global(o), g: o}
			}
			return &lvalue{x: x, kind: lvCell, cell: b.globalCell(o), g: o}
		}
		if lv := b.selfPlace(x); lv != nil {
			return lv
		}
	case *syntax.SelfExpr:
		if _, store := b.l.info.StoreSelf[x]; !store {
			if lv := b.selfPlace(x); lv != nil {
				return lv
			}
		}
	case *syntax.MemberExpr:
		sel := b.l.info.Selections[x]
		if sel != nil && sel.Kind == check.SelPkg {
			if g, ok := sel.Obj.(*check.Global); ok {
				if g.Kind == check.VarGlobal {
					return &lvalue{x: x, kind: lvGlobal, cell: b.l.global(g), g: g}
				}
				return &lvalue{x: x, kind: lvCell, cell: b.globalCell(g), g: g}
			}
		}
		if sel != nil && sel.Kind == check.SelStoreMember {
			s := b.value(x.X, -1)
			return &lvalue{x: x, kind: lvCell, cell: b.storeMemberRef(s, sel.Member), m: sel.Member, mtype: memberTypeIn(b.typ(sel.Recv), sel.Member)}
		}
		if sel != nil && sel.Kind == check.SelField {
			n, _ := b.typ(sel.Recv).(*check.Named)
			return &lvalue{x: x, kind: lvField, inner: b.prepareRecv(x), field: sel.Field, named: n}
		}
		if sel != nil && sel.Kind == check.SelTupleElem {
			// A tuple's element: its field, by place (named nil).
			return &lvalue{x: x, kind: lvField, inner: b.prepareRecv(x), field: sel.Field}
		}
	case *syntax.IndexExpr:
		inner := b.prepare(x.X)
		_, isMap := check.Unopt(b.rawType(x.X)).(*check.Map)
		return &lvalue{x: x, kind: lvIndex, inner: inner, idx: b.value(x.Index, -1), isMap: isMap, def: x.Default}
	}
	b.l.errorf(x, diag.NotAssignable, "can't change this")
	return &lvalue{x: x, kind: lvVar}
}

// update runs fn on a register holding lv's value, which fn may change
// in place, and stores it back.
func (b *fb) update(lv *lvalue, fn func(cur int)) { b.updateWith(lv, false, fn) }

// updateWith is update; reads says that fn runs code that may read the
// place (a closure a builtin calls): state is then read, not taken out,
// so that code sees its value as it was (the change copies it) rather
// than none.
func (b *fb) updateWith(lv *lvalue, reads bool, fn func(cur int)) {
	b.updatePut(lv, reads, fn, b.putFor(lv, psAll))
}

// updatePut is updateWith, with how the state lv is in is put back (p:
// the place's path and what the change was, rfcs/0013; nil when lv
// isn't in a state).
func (b *fb) updatePut(lv *lvalue, reads bool, fn func(cur int), p *put) {
	switch lv.kind {
	case lvVar:
		r, ok := b.vars[lv.v]
		if !ok {
			b.l.errorf(lv.x, diag.InternalError, "internal: no register for %s", lv.v.Name())
			return
		}
		fn(r)
	case lvBoxed:
		var box int
		var t check.Type
		if lv.v != nil {
			box, t = b.vars[lv.v], lv.v.Type
		} else {
			box, t = lv.cell, lv.m.Type // a worker's var
		}
		cur := b.temp(b.kind(t))
		if reads || b.f.Kinds[cur] != bc.KRef {
			b.emit(op("VGET"), cur, box)
			fn(cur)
			b.emit(op("VSET"), box, cur)
			return
		}
		// Taken out, so that it's unique and changed in place, not
		// copied (as a cell's CTAKE); put back if the change throws.
		b.emit(op("VTAKE"), cur, box)
		b.protect(func() { fn(cur) }, func(int) { b.emit(op("VSET"), box, cur) })
		b.emit(op("VSET"), box, cur)
	case lvGlobal:
		b.updateGlobal(lv, reads, fn)
	case lvCell:
		k := b.cellKind(lv)
		cur := b.temp(k)
		var ok, failed *putPath
		if p != nil {
			ok, failed = p.ok, p.err
		}
		if reads {
			b.emit(op("CPEEK"), cur, lv.cell)
			fn(cur)
			if ok != nil && ok.steps[len(ok.steps)-1] == psAll {
				// What changed isn't known (a mut fn's call), but the old
				// value is in the cell still: compared, path by path.
				b.emit(op("CSET"), lv.cell, cur)
				return
			}
			b.cputp(lv.cell, cur, ok)
			return
		}
		b.emit(op("CTAKE"), cur, lv.cell)
		b.protect(func() { fn(cur) }, func(e int) {
			b.cputp(lv.cell, cur, failed)
		})
		b.cputp(lv.cell, cur, ok)
	case lvField:
		b.updatePut(lv.inner, reads, func(rx int) {
			t := b.getField(rx, lv.named, lv.field, -1)
			b.changePart(t, fn, func(v int) { b.emit(op("SETF"), rx, lv.field.Index, v) })
		}, p)
	case lvIndex:
		if lv.isMap {
			b.updatePut(lv.inner, reads, func(rx int) { b.updateEntry(lv, rx, fn) }, p)
			return
		}
		b.updatePut(lv.inner, reads, func(rx int) {
			t := b.temp(b.kind(b.placeType(lv.x)))
			b.emit(op("IDX"), t, rx, lv.idx)
			b.changePart(t, fn, func(v int) { b.emit(op("SETIDX"), rx, lv.idx, v) })
		}, p)
	case lvSome:
		b.updatePut(lv.inner, reads, func(cur int) { b.updateSome(lv, cur, fn) }, p)
	}
}

// changePart runs fn on t, a field's or an element's value read into
// it, and puts it back (put). A reference is taken out of its place
// first (none put there, as a cell's CTAKE leaves it), so that it's
// unique and fn changes it in place rather than a copy of it; a handler
// puts it back if fn throws, and t lets it go after.
func (b *fb) changePart(t int, fn func(cur int), put func(v int)) {
	if b.f.Kinds[t] != bc.KRef {
		fn(t)
		put(t)
		return
	}
	put(b.null())
	b.protect(func() { fn(t) }, func(int) { put(t) })
	put(t)
	b.emit(op("CLR"), t)
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
		if lv.v == nil {
			b.emit(op("VSET"), lv.cell, val)
			return
		}
		b.writeVar(lv.v, val, lv.x)
	case lvCell:
		b.emit(op("CSET"), lv.cell, val)
	case lvGlobal:
		b.emit(op("GSET"), lv.cell, val, int(b.kind(lv.g.Type)))
	case lvModel:
		// Bound, the change goes to the caller (its bind writes its state,
		// which the prop reads); unbound, the value is the component's own,
		// and the change is reported to a caller that handles it.
		ev := lv.m.ChangeEvent()
		isBound := b.callB(uiModelBound, bc.KBool, lv.cell)
		unbound, end := b.f.NewLabel(), b.f.NewLabel()
		b.emitJump(op("JF"), isBound, unbound)
		b.callEvent(ev, lv.x, func() []int { return []int{val} })
		b.jmp(end)
		b.f.Place(unbound)
		b.callBTo(uiModelSet, bc.None, lv.cell, val)
		b.callEvent(ev, lv.x, func() []int { return []int{val} })
		b.f.Place(end)
	case lvField:
		if lv.named != nil && lv.named.Obj.IsError {
			b.l.errorf(lv.x, diag.NotAssignable, "an error's fields can't be changed: make a new one")
			return
		}
		// In a state: the old value kept, so that the write tells only
		// the readers of what changed (none for an equal one).
		old, p := b.oldFor(lv)
		b.updatePut(lv.inner, false, func(rx int) {
			if old >= 0 {
				b.getField(rx, lv.named, lv.field, old)
			}
			b.emit(op("SETF"), rx, lv.field.Index, val)
		}, p)
	case lvIndex:
		old, p := b.oldFor(lv)
		if lv.isMap {
			b.updatePut(lv.inner, false, func(rx int) {
				if old >= 0 {
					there := p.ok.args[len(p.ok.args)-1]
					skip := b.f.NewLabel()
					b.emit(op("KSMALL"), there, 0)
					b.emit(op("MGETJ"), old, rx, lv.idx, int(skip))
					b.emit(op("KSMALL"), there, 1)
					b.f.Place(skip)
				}
				b.emit(op("MSET"), rx, lv.idx, val)
			}, p)
			return
		}
		b.updatePut(lv.inner, false, func(rx int) {
			if old >= 0 {
				b.emit(op("IDX"), old, rx, lv.idx)
			}
			b.emit(op("SETIDX"), rx, lv.idx, val)
		}, p)
	}
}

// oldFor is, for a write of the place lv (a field, an element, a map's
// entry) in a state, the register its old value goes in and how the
// state is put back: compared with it (and, after a map's key, whether
// it was there). Not in a state: -1, nil.
func (b *fb) oldFor(lv *lvalue) (int, *put) {
	pp, ok := b.placePath(lv)
	if !ok {
		return -1, nil
	}
	if !pp.exact {
		return -1, b.putFor(lv.inner, psAll)
	}
	old := b.temp(b.kind(b.placeType(lv.x)))
	regs := []int{old}
	if pp.keyed {
		regs = append(regs, b.temp(bc.KInt))
	}
	okp, errp := pp.with(psOld, regs...), pp.with(psAll)
	return old, &put{&okp, &errp}
}

// get reads lv's current value.
func (b *fb) get(lv *lvalue) int {
	return b.value(lv.x, -1)
}

func (b *fb) assignStmt(s *syntax.AssignStmt) {
	if optionalPath(s.Target) {
		// A ?. chain: the place reached first, its value only when it's
		// there (places.go).
		lv := b.prepare(s.Target)
		b.optionalChange(lv, func() { b.assign(s, lv, b.value(s.Value, -1)) })
		return
	}
	switch s.Op {
	case syntax.ASSIGN:
		if b.appendStmt(s) {
			return
		}
		mark := b.mark()
		v := b.value(s.Value, -1)
		b.dropTemps(mark, v)
		b.set(b.prepare(s.Target), v)
		if b.f.Kinds[v] == bc.KRef && slices.Contains(b.temps[mark:], v) {
			// The value's temp let go of once the place holds it, as a
			// let's is: kept, it's a second reference until the
			// register's next use (n = n.kids[i] going down a tree: each
			// node held twice), and the optimizer can't make the copy a
			// move (the temp's computation writing the place itself).
			b.emit(op("CLR"), v)
		}
	case syntax.COALESCE_ASSIGN:
		b.assign(s, b.prepare(s.Target), -1)
	default:
		if b.appendStmt(s) {
			return
		}
		mark := b.mark()
		v := b.value(s.Value, -1)
		b.dropTemps(mark, v)
		b.assign(s, b.prepare(s.Target), v)
	}
}

// appendStmt lowers s += x and s = s + x + ..., s a local string var,
// as appends to s's own register (CONCAT s, s, x): in place when the
// string is s's alone (ak_str_append), so building a string in a loop
// copies it O(log n) times, not once a turn. What's appended is all
// computed first, so an error computing it leaves s as it was; the
// parts mustn't read s (s + x + s's last s is s as it was).
func (b *fb) appendStmt(s *syntax.AssignStmt) bool {
	x := s.Target
	for {
		p, ok := x.(*syntax.ParenExpr)
		if !ok {
			break
		}
		x = p.X
	}
	id, ok := x.(*syntax.Ident)
	if !ok {
		return false
	}
	v, ok := b.l.info.Uses[id].(*check.Var)
	if !ok || v.Boxed() || b.pathVars[v] != nil || check.Unopt(v.Type) != v.Type || b.typ(v.Type) != check.String {
		return false
	}
	r, ok := b.vars[v]
	if !ok {
		return false
	}
	var parts []syntax.Expr
	switch s.Op {
	case syntax.ADD_ASSIGN:
		if b.l.info.AssignOps[s] != nil || b.rawType(s.Value) != check.String {
			return false
		}
		parts = []syntax.Expr{s.Value}
		if ps := b.stringChain(s.Value); ps != nil {
			parts = ps
		}
	case syntax.ASSIGN:
		ps := b.stringChain(s.Value)
		if len(ps) < 2 {
			return false
		}
		first := ps[0]
		for {
			p, ok := first.(*syntax.ParenExpr)
			if !ok {
				break
			}
			first = p.X
		}
		if fid, ok := first.(*syntax.Ident); !ok || b.l.info.Uses[fid] != v {
			return false
		}
		parts = ps[1:]
	default:
		return false
	}
	for _, p := range parts {
		if b.mentions(p, v) {
			return false
		}
	}
	mark := b.mark()
	vals := make([]int, len(parts))
	for i, p := range parts {
		vals[i] = b.chainPart(p, -1)
	}
	if len(vals) > 1 {
		vals = []int{b.joinStrings(vals, -1)}
	}
	b.emit(op("CONCAT"), r, r, vals[0])
	b.dropTemps(mark)
	return true
}

// optionalPath reports a ?. in the path of the place x.
func optionalPath(x syntax.Expr) bool {
	for {
		switch e := x.(type) {
		case *syntax.ParenExpr:
			x = e.X
		case *syntax.MemberExpr:
			if e.Optional {
				return true
			}
			x = e.X
		case *syntax.IndexExpr:
			x = e.X
		default:
			return false
		}
	}
}

// assign makes s's change of the place lv with v, its value computed
// (-1 for ??=, whose value is computed only when the place is none).
func (b *fb) assign(s *syntax.AssignStmt, lv *lvalue, v int) {
	lt := b.placeType(s.Target)
	switch s.Op {
	case syntax.ASSIGN:
		b.set(lv, v)
	case syntax.COALESCE_ASSIGN:
		cur := b.get(lv)
		end := b.f.NewLabel()
		b.emitJump(op("JSOME"), cur, end)
		b.set(lv, b.value(s.Value, -1))
		b.f.Place(end)
	default:
		o := s.Op.AssignOp()
		vt := b.typeOf(s.Value)
		apply := func(cur, dst int) int { return b.binop(o, cur, v, lt, vt, lt, dst) }
		reads := b.placeReads(lv)
		if m := b.l.info.AssignOps[s]; m != nil {
			// x += y is x = x.add(y): the method of x's type, which may
			// read the place (a state's value), so it's read, not taken.
			rt := b.typ(m.Recv)
			idx := b.l.funcIndex(m.Func, nil, rt)
			apply = func(cur, dst int) int { return b.invoke(idx, []int{cur, v}, b.kind(lt), dst, false) }
			reads = true
		}
		if lv.kind == lvIndex && lv.isMap {
			// m[k, default: v] += y: the entry changed where it is.
			b.updateWith(lv, reads, func(cur int) { b.move(cur, apply(cur, cur)) })
			return
		}
		if lv.kind == lvField || lv.kind == lvIndex && !lv.isMap {
			// Read and write the element where it is; in a state, its
			// old value kept for the write to compare.
			old, p := b.oldFor(lv)
			if p == nil {
				p = b.putFor(lv.inner, psAll)
			}
			b.updatePut(lv.inner, reads, func(rx int) {
				cur := old
				if lv.kind == lvField {
					cur = b.getField(rx, lv.named, lv.field, old)
				} else {
					if cur < 0 {
						cur = b.temp(b.kind(lt))
					}
					b.emit(op("IDX"), cur, rx, lv.idx)
				}
				dst := cur
				if old >= 0 {
					dst = -1
				}
				r := apply(cur, dst)
				if lv.kind == lvField {
					b.emit(op("SETF"), rx, lv.field.Index, r)
				} else {
					b.emit(op("SETIDX"), rx, lv.idx, r)
				}
			}, p)
			return
		}
		cur := b.get(lv)
		r := apply(cur, -1)
		b.set(lv, r)
	}
}

// ---------------------------------------------------------------------
// Errors

// peek is peek(x): x, read untracked. When x reads nothing reactive,
// it's x.
func (b *fb) peek(x syntax.Expr, dst int) int {
	if !b.l.prog.Reactive(x) {
		return b.value(x, dst)
	}
	b.emit(op("UNTRACK"))
	b.peeks++
	b.untracks++
	r := b.value(x, dst)
	b.peeks--
	b.emit(op("RETRACK"), b.peeks)
	return r
}

// catchAt starts a handler's code, at its target: what a peek in its
// range untracked, an error leaving the peek, tracks again (RETRACK the
// peeks open here), then CATCH takes the error into e. untracks is
// b.untracks where its range began.
func (b *fb) catchAt(e, untracks int) {
	if b.untracks > untracks {
		b.emit(op("RETRACK"), b.peeks)
	}
	b.emit(op("CATCH"), e)
}

// protect runs body with a handler: on an error, onError (with the
// error's register) runs, and the error goes on.
func (b *fb) protect(body func(), onError func(e int)) {
	start, end, target, after := b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel(), b.f.NewLabel()
	u := b.untracks
	b.f.Place(start)
	body()
	b.f.Place(end)
	b.jmp(after)
	b.f.Place(target)
	e := b.temp(bc.KRef)
	b.catchAt(e, u)
	onError(e)
	b.emit(op("THROW"), e)
	b.f.Place(after)
	b.f.Handlers = append(b.f.Handlers, Handler{Start: start, End: end, Target: target, Rethrows: true})
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
	u := b.untracks
	b.f.Place(start)
	body()
	b.f.Place(end)
	b.jmp(after)
	b.f.Place(target)
	mark := b.mark()
	e := b.newReg(bc.KRef)
	b.catchAt(e, u)
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
	b.f.Handlers = append(b.f.Handlers, Handler{Start: start, End: end, Target: target})
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
	u := b.untracks
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
	b.catchAt(ev, u)
	cancel := b.f.NewLabel()
	b.classify(ev, cancel)
	if r >= 0 {
		b.emit(op("KNONE"), r)
	}
	b.jmp(after)
	b.f.Place(cancel)
	b.emit(op("THROW"), ev)
	b.f.Place(after)
	b.f.Handlers = append(b.f.Handlers, Handler{Start: start, End: end, Target: target})
	return r
}

// withDefer lowers the rest of a block with a defer (and then, the
// block's value): its body runs when the block ends, by any way.
func (b *fb) withDefer(d *syntax.DeferStmt, rest []syntax.Item, then func()) {
	b.defers = append(b.defers, d.Body)
	n := len(b.defers)
	b.protect(func() {
		b.stmtsThen(rest, then)
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
	b.callEvent(m, s, func() []int {
		var vals []int
		if s.Args != nil {
			for _, a := range s.Args.List {
				vals = append(vals, b.value(a.Value, -1))
			}
		}
		return vals
	})
}

// callEvent calls event m's handler with the values args makes, when the
// caller gave it one (else args isn't evaluated).
func (b *fb) callEvent(m *check.Member, n syntax.Node, args func() []int) {
	h := b.memberRef(m, n)
	skip := b.f.NewLabel()
	b.emitJump(op("JNONE"), h, skip)
	vals := args()
	var ks []bc.Kind
	for i := range vals {
		ks = append(ks, b.kind(m.Params[i].Type))
	}
	blk := b.regs(ks)
	for i, v := range vals {
		b.move(blk+i, v)
	}
	res := b.temp(bc.KRef)
	b.emit(op("CALLC"), res, h, blk, len(vals))
	b.releaseRegs(blk, ks)
	if b.suspends() {
		// An async function waits for an async handler: its result is
		// the task's future (none: a sync handler's).
		b.emitJump(op("JNONE"), res, skip)
		b.emit(op("AWAIT"), b.temp(bc.KRef), res)
	}
	b.f.Place(skip)
}

// forAwait is for await x in s: stream.ready (a value is queued: most
// turns, a producer sending ahead), else await stream.next (whether a
// value came: false at the end; it throws what the producer threw), take
// it with stream.value; leaving the loop closes the stream, which cancels
// its producer.
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
		got := b.f.NewLabel()
		b.callBTo("stream.ready", more, st)
		b.emitJump(op("JT"), more, got)
		f := b.callB("stream.next", bc.KRef, st)
		b.emit(op("AWAIT"), more, f)
		b.emitJump(op("JF"), more, end)
		b.f.Place(got)
		b.openScope()
		x := b.temp(b.kind(fi.Elem))
		b.callBTo("stream.value", x, st)
		if _, ok := fi.Pat.(*syntax.TuplePat); ok {
			b.bindLet(fi.Pat, x, b.typ(fi.Elem))
		} else {
			b.declare(fi.Item, x)
		}
		b.loops = append(b.loops, loop{brk: end, cont: top, defers: len(b.defers), node: s})
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
	if tt := b.l.info.TypeTests[s]; tt != nil {
		// let c: Circle = shape else { … }: a type test.
		x := v
		if check.IsOptional(t) {
			b.emitJump(op("JNONE"), v, els)
			x = b.unbox(v, check.Unopt(t), -1)
		}
		x, _ = b.testType(x, tt, els)
		vr, _ := b.l.info.Defs[s.Pattern.(*syntax.BindPat).Name].(*check.Var)
		b.declare(vr, x)
	} else if bp, name := s.Pattern.(*syntax.BindPat); name {
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
		if len(b.defers) > 0 && r >= 0 && k != bc.KVoid {
			// The defers run after the value is computed: keep it, as
			// a defer may change the local it's in.
			t := b.temp(k)
			b.move(t, r)
			r = t
		}
	}
	b.runDefers(0)
	b.ret(r, k)
}

// branch is break or continue: the defers of the loop's body, then the
// jump.
func (b *fb) branch(tok syntax.Token, label *syntax.Ident) {
	if len(b.loops) == 0 {
		return
	}
	lp := b.loops[len(b.loops)-1]
	if label != nil {
		// A loop around, by its name: the defers of every loop left run.
		target := b.l.info.Labels[label]
		for i := len(b.loops) - 1; i >= 0; i-- {
			if b.loops[i].node == target {
				lp = b.loops[i]
				break
			}
		}
	}
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
		b.branch(e.Tok, e.Label)
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
			c.setVar(v, c.newReg(c.capKind(v)))
		}
		if g.UsesEnv {
			c.envReg = c.newReg(bc.KRef)
			c.noteEnv()
		}
		c.openScope()
		for i, p := range lf.Sig.Params {
			if p.Var == nil {
				continue
			}
			if p.Var.Boxed() {
				c.declare(p.Var, params[i])
			} else {
				c.setVar(p.Var, params[i])
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
			b.l.errorf(n, diag.InternalError, "internal: %s has no register in %s", v.Name(), b.f.Name)
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
		b.l.errorf(n, diag.InternalError, "internal: local fn %s isn't lowered in %s", lf.Decl.Name.Name, b.f.Name)
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
			b.l.errorf(n, diag.InternalError, "internal: %s has no register in %s", v.Name(), b.f.Name)
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
